#
# Copyright (C) 2022-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#
"""Scylla clusters for testing.
   Provides helpers to setup and manage clusters of Scylla servers for testing.
"""
import asyncio
from asyncio.subprocess import Process
from contextlib import asynccontextmanager
from collections import ChainMap
import itertools
import logging
import os
import pathlib
import shutil
import tempfile
import time
import traceback
from typing import Any, Optional, Dict, List, Set, Tuple, Callable, AsyncIterator, NamedTuple, Union
import uuid
from enum import Enum
from io import BufferedWriter
from test.pylib.host_registry import Host, HostRegistry
from test.pylib.pool import Pool
from test.pylib.rest_client import ScyllaRESTAPIClient, HTTPError
from test.pylib.util import LogPrefixAdapter, read_last_line
from test.pylib.internal_types import ServerNum, IPAddress, HostID, ServerInfo
import aiohttp
import aiohttp.web
import yaml
import signal

from cassandra import InvalidRequest                    # type: ignore
from cassandra import OperationTimedOut                 # type: ignore
from cassandra.auth import PlainTextAuthProvider        # type: ignore
from cassandra.cluster import Cluster           # type: ignore # pylint: disable=no-name-in-module
from cassandra.cluster import NoHostAvailable   # type: ignore # pylint: disable=no-name-in-module
from cassandra.cluster import Session           # pylint: disable=no-name-in-module
from cassandra.cluster import ExecutionProfile  # pylint: disable=no-name-in-module
from cassandra.cluster import EXEC_PROFILE_DEFAULT  # pylint: disable=no-name-in-module
from cassandra.policies import WhiteListRoundRobinPolicy  # type: ignore


class ReplaceConfig(NamedTuple):
    replaced_id: ServerNum
    reuse_ip_addr: bool
    use_host_id: bool
    ignore_dead_nodes: list[IPAddress | HostID] = []


def make_scylla_conf(workdir: pathlib.Path, host_addr: str, seed_addrs: List[str], cluster_name: str) -> dict[str, object]:
    return {
        'cluster_name': cluster_name,
        'workdir': str(workdir.resolve()),
        'listen_address': host_addr,
        'rpc_address': host_addr,
        'api_address': host_addr,
        'prometheus_address': host_addr,
        'alternator_address': host_addr,
        'seed_provider': [{
            'class_name': 'org.apache.cassandra.locator.SimpleSeedProvider',
            'parameters': [{
                'seeds': '{}'.format(','.join(seed_addrs))
                }]
            }],

        'developer_mode': True,

        # Allow testing experimental features. Following issue #9467, we need
        # to add here specific experimental features as they are introduced.
        'enable_user_defined_functions': True,
        'experimental': True,
        'experimental_features': ['udf', 'consistent-topology-changes'],

        'consistent_cluster_management': True,

        'skip_wait_for_gossip_to_settle': 0,
        'ring_delay_ms': 0,
        'num_tokens': 16,
        'flush_schema_tables_after_modification': False,
        'auto_snapshot': False,

        # Significantly increase default timeouts to allow running tests
        # on a very slow setup (but without network losses). Note that these
        # are server-side timeouts: The client should also avoid timing out
        # its own requests - for this reason we increase the CQL driver's
        # client-side timeout in conftest.py.

        'range_request_timeout_in_ms': 300000,
        'read_request_timeout_in_ms': 300000,
        'counter_write_request_timeout_in_ms': 300000,
        'cas_contention_timeout_in_ms': 300000,
        'truncate_request_timeout_in_ms': 300000,
        'write_request_timeout_in_ms': 300000,
        'request_timeout_in_ms': 300000,

        'strict_allow_filtering': True,
        'strict_is_not_null_in_views': True,

        'permissions_update_interval_in_ms': 100,
        'permissions_validity_in_ms': 100,

        'reader_concurrency_semaphore_serialize_limit_multiplier': 0,
        'reader_concurrency_semaphore_kill_limit_multiplier': 0,

        'force_schema_commit_log': True,
    }

# Seastar options can not be passed through scylla.yaml, use command line
# for them. Keep everything else in the configuration file to make
# it easier to restart. Sic: if you make a typo on the command line,
# Scylla refuses to boot.
SCYLLA_CMDLINE_OPTIONS = [
    '--smp', '2',
    '-m', '1G',
    '--collectd', '0',
    '--overprovisioned',
    '--max-networking-io-control-blocks', '1000',
    '--unsafe-bypass-fsync', '1',
    '--kernel-page-cache', '1',
    '--commitlog-use-o-dsync', '0',
    '--abort-on-lsa-bad-alloc', '1',
    '--abort-on-seastar-bad-alloc',
    '--abort-on-internal-error', '1',
    '--abort-on-ebadf', '1'
]

# [--smp, 1], [--smp, 2] -> [--smp, 2]
# [--smp, 1], [--smp] -> [--smp]
# [--smp, 1], [--smp, __missing__] -> [--smp]
# [--smp, 1], [--smp, __remove__] -> []
# [--smp=1], [--smp=2] -> [--smp, 2]
# [--smp=1], [--smp=__remove__] -> []
# [--overprovisioned, --smp=1, --abort-on-ebadf], [--smp=2] -> [--overprovisioned, --smp=2, --abort-on-ebadf]
# [], [--experimental-features, raft, --experimental-features, broadcast-tables] ->
# [--experimental-features, raft, --experimental-features, broadcast-tables]
def merge_cmdline_options(base: List[str], override: List[str]) -> List[str]:
    if len(override) == 0:
        return base

    def to_dict(args: List[str]) -> Dict[str, List[Optional[str]]]:
        result: Dict[str, List[Optional[str]]] = {}
        i = 0
        while i < len(args):
            name = args[i]
            if not name.startswith('-'):
                raise ValueError(f'invalid argument name {name}, all args {args}')
            if '=' in name:
                name, _, value = name.partition('=')
                i += 1
            elif i < len(args) - 1 and not args[i + 1].startswith('-'):
                value = args[i + 1]
                i += 2
            else:
                value = None
                i += 1
            result.setdefault(name, []).append(value)
        return result

    def run() -> List[str]:
        merged: Dict[str, List[Optional[str]]] = to_dict(base)
        for name, values in to_dict(override).items():
            merged_values = None
            for v in values:
                if v != '__remove__':
                    if merged_values is None:
                        merged_values = merged.setdefault(name, [])
                        merged_values.clear()
                    merged_values.append(v if v != '__missing__' else None)
                elif name in merged:
                    del merged[name]
                    merged_values = None

        result: List[str] = []
        for name, values in merged.items():
            for v in values:
                result.append(name)
                if v is not None:
                    result.append(v)
        return result

    return run()

class CqlUpState(Enum):
    NOT_CONNECTED = 1,
    CONNECTED = 2,
    QUERIED = 3

class ScyllaServer:
    """Starts and handles a single Scylla server, managing logs, checking if responsive,
       and cleanup when finished."""
    # pylint: disable=too-many-instance-attributes

    # in seconds, used for topology operations such as bootstrap or decommission
    TOPOLOGY_TIMEOUT = 1000
    start_time: float
    sleep_interval: float
    log_file: BufferedWriter
    host_id: HostID                             # Host id (UUID)
    newid = itertools.count(start=1).__next__   # Sequential unique id

    def __init__(self, exe: str, vardir: str,
                 logger: Union[logging.Logger, logging.LoggerAdapter],
                 cluster_name: str, ip_addr: str, seeds: List[str],
                 cmdline_options: List[str],
                 config_options: Dict[str, Any]) -> None:
        # pylint: disable=too-many-arguments
        self.server_id = ServerNum(ScyllaServer.newid())
        self.exe = pathlib.Path(exe).resolve()
        self.vardir = pathlib.Path(vardir)
        self.logger = logger
        self.cmdline_options = merge_cmdline_options(SCYLLA_CMDLINE_OPTIONS, cmdline_options)
        self.cluster_name = cluster_name
        self.ip_addr = IPAddress(ip_addr)
        self.seeds = seeds
        self.cmd: Optional[Process] = None
        self.log_savepoint = 0
        self.control_cluster: Optional[Cluster] = None
        self.control_connection: Optional[Session] = None
        shortname = f"scylla-{self.server_id}"
        self.workdir = self.vardir / shortname
        self.log_filename = (self.vardir / shortname).with_suffix(".log")
        self.config_filename = self.workdir / "conf/scylla.yaml"
        # Sum of basic server configuration and the user-provided config options.
        self.config = make_scylla_conf(
                workdir = self.workdir,
                host_addr = self.ip_addr,
                seed_addrs = self.seeds,
                cluster_name = self.cluster_name) \
            | config_options

    def change_ip(self, ip_addr: IPAddress) -> None:
        """Change IP address of the current server. Pre: the server is
        stopped"""
        if self.is_running:
            raise RuntimeError(f"Can't change IP of a running server {self.ip_addr}.")
        self.ip_addr = ip_addr
        self.config["listen_address"] = ip_addr
        self.config["rpc_address"] = ip_addr
        self.config["api_address"] = ip_addr
        self.config["prometheus_address"] = ip_addr
        self.config["alternator_address"] = ip_addr
        self._write_config_file()


    async def install_and_start(self, api: ScyllaRESTAPIClient) -> None:
        """Setup and start this server"""
        await self.install()

        self.logger.info("starting server at host %s in %s...", self.ip_addr, self.workdir.name)

        try:
            await self.start(api)
        except:
            await self.stop()
            raise

        if self.cmd:
            self.logger.info("started server at host %s in %s, pid %d", self.ip_addr,
                         self.workdir.name, self.cmd.pid)

    @property
    def is_running(self) -> bool:
        """Check the server subprocess is up"""
        return self.cmd is not None

    def check_scylla_executable(self) -> None:
        """Check if executable exists and can be run"""
        if not os.access(self.exe, os.X_OK):
            raise RuntimeError(f"{self.exe} is not executable")

    async def install(self) -> None:
        """Create a working directory with all subdirectories, initialize
        a configuration file."""

        self.check_scylla_executable()

        self.logger.info("installing Scylla server in %s...", self.workdir)

        # Cleanup any remains of the previously running server in this path
        shutil.rmtree(self.workdir, ignore_errors=True)

        try:
            self.workdir.mkdir(parents=True, exist_ok=True)
            self.config_filename.parent.mkdir(parents=True, exist_ok=True)
            self._write_config_file()

            self.log_file = self.log_filename.open("wb")
        except:
            try:
                shutil.rmtree(self.workdir)
            except FileNotFoundError:
                pass
            self.log_filename.unlink(missing_ok=True)
            raise

    def get_config(self) -> dict[str, object]:
        """Return the contents of conf/scylla.yaml as a dict."""
        return self.config

    def update_config(self, key: str, value: object) -> None:
        """Update conf/scylla.yaml by setting `value` under `key`.
           If we're running, reload the config with a SIGHUP."""
        self.config[key] = value
        self._write_config_file()
        if self.cmd:
            self.cmd.send_signal(signal.SIGHUP)

    def take_log_savepoint(self) -> None:
        """Save the server current log size when a test starts so that if
        the test fails, we can only capture the relevant lines of the log"""
        self.log_savepoint = self.log_file.tell()

    def read_log(self) -> str:
        """ Return first 3 lines of the log + everything that happened
        since the last savepoint. Used to diagnose CI failures, so
        avoid a nessted exception."""
        try:
            with self.log_filename.open("r") as log:
                # Read the first 5 lines of the start log
                lines: List[str] = []
                for _ in range(3):
                    lines.append(log.readline())
                # Read the lines since the last savepoint
                if self.log_savepoint and self.log_savepoint > log.tell():
                    log.seek(self.log_savepoint)
                return "".join(lines + log.readlines())
        except Exception as exc:    # pylint: disable=broad-except
            return f"Exception when reading server log {self.log_filename}: {exc}"

    async def cql_is_up(self) -> CqlUpState:
        """Test that CQL is serving (a check we use at start up)."""
        caslog = logging.getLogger('cassandra')
        oldlevel = caslog.getEffectiveLevel()
        # Be quiet about connection failures.
        caslog.setLevel('CRITICAL')
        auth = PlainTextAuthProvider(username='cassandra', password='cassandra')
        # auth::standard_role_manager creates "cassandra" role in an
        # async loop auth::do_after_system_ready(), which retries
        # role creation with an exponential back-off. In other
        # words, even after CQL port is up, Scylla may still be
        # initializing. When the role is ready, queries begin to
        # work, so rely on this "side effect".
        profile = ExecutionProfile(load_balancing_policy=WhiteListRoundRobinPolicy([self.ip_addr]),
                                   request_timeout=self.TOPOLOGY_TIMEOUT)
        connected = False
        try:
            # In a cluster setup, it's possible that the CQL
            # here is directed to a node different from the initial contact
            # point, so make sure we execute the checks strictly via
            # this connection
            with Cluster(execution_profiles={EXEC_PROFILE_DEFAULT: profile},
                         contact_points=[self.ip_addr],
                         # This is the latest version Scylla supports
                         protocol_version=4,
                         auth_provider=auth) as cluster:
                with cluster.connect() as session:
                    connected = True
                    # See the comment above about `auth::standard_role_manager`. We execute
                    # a 'real' query to ensure that the auth service has finished initializing.
                    session.execute("SELECT key FROM system.local where key = 'local'")
                    self.control_cluster = Cluster(execution_profiles=
                                                        {EXEC_PROFILE_DEFAULT: profile},
                                                   contact_points=[self.ip_addr],
                                                   auth_provider=auth)
                    self.control_connection = self.control_cluster.connect()
                    return CqlUpState.QUERIED
        except (NoHostAvailable, InvalidRequest, OperationTimedOut) as exc:
            self.logger.debug("Exception when checking if CQL is up: %s", exc)
            return CqlUpState.CONNECTED if connected else CqlUpState.NOT_CONNECTED
        finally:
            caslog.setLevel(oldlevel)
        # Any other exception may indicate a problem, and is passed to the caller.

    async def get_host_id(self, api: ScyllaRESTAPIClient) -> bool:
        """Try to get the host id (also tests Scylla REST API is serving)"""
        try:
            self.host_id = await api.get_host_id(self.ip_addr)
            return True
        except (aiohttp.ClientConnectionError, HTTPError) as exc:
            if isinstance(exc, HTTPError) and exc.code >= 500:
                raise exc
            return False
        # Any other exception may indicate a problem, and is passed to the caller.

    async def start(self, api: ScyllaRESTAPIClient, expected_error: Optional[str] = None) -> None:
        """Start an installed server. May be used for restarts."""

        env = os.environ.copy()
        env.clear()     # pass empty env to make user user's SCYLLA_HOME has no impact
        self.cmd = await asyncio.create_subprocess_exec(
            self.exe,
            *self.cmdline_options,
            cwd=self.workdir,
            stderr=self.log_file,
            stdout=self.log_file,
            env=env,
            preexec_fn=os.setsid,
        )

        self.start_time = time.time()
        sleep_interval = 0.1
        cql_up_state = CqlUpState.NOT_CONNECTED

        def report_error(message: str):
            message += f", server_id {self.server_id}, IP {self.ip_addr}, workdir {self.workdir.name}"
            message += f", host_id {self.host_id if hasattr(self, 'host_id') else '<missing>'}"
            message += f", cql [{'connected' if cql_up_state == CqlUpState.CONNECTED else 'not connected'}]"
            if expected_error is not None:
                message += f", the node log was expected to contain the string [{expected_error}]"
            self.logger.error(message)
            self.logger.error("last line of %s:\n%s", self.log_filename, read_last_line(self.log_filename))
            log_handler = logging.getLogger().handlers[0]
            if hasattr(log_handler, 'baseFilename'):
                logpath = log_handler.baseFilename   # type: ignore
            else:
                logpath = "?"
            raise RuntimeError(message + "\nCheck the log files:\n"
                                         f"{logpath}\n"
                                         f"{self.log_filename}")

        while time.time() < self.start_time + self.TOPOLOGY_TIMEOUT:
            if self.cmd.returncode:
                self.cmd = None
                if expected_error is not None:
                    with self.log_filename.open('r') as log_file:
                        for line in log_file:
                            if expected_error in line:
                                return
                        report_error("the node startup failed, but the log file doesn't contain the expected error")
                report_error("failed to start the node")

            if hasattr(self, "host_id") or await self.get_host_id(api):
                cql_up_state = await self.cql_is_up()
                if cql_up_state == CqlUpState.QUERIED:
                    if expected_error is not None:
                        report_error("the node started, but was expected to fail with the expected error")
                    return

            # Sleep and retry
            await asyncio.sleep(sleep_interval)

        report_error('failed to start the node, timeout reached')

    async def force_schema_migration(self) -> None:
        """This is a hack to change schema hash on an existing cluster node
        which triggers a gossip round and propagation of entire application
        state. Helps quickly propagate tokens and speed up node boot if the
        previous state propagation was missed."""
        auth = PlainTextAuthProvider(username='cassandra', password='cassandra')
        profile = ExecutionProfile(load_balancing_policy=WhiteListRoundRobinPolicy(self.seeds),
                                   request_timeout=self.TOPOLOGY_TIMEOUT)
        with Cluster(execution_profiles={EXEC_PROFILE_DEFAULT: profile},
                     contact_points=self.seeds,
                     auth_provider=auth,
                     # This is the latest version Scylla supports
                     protocol_version=4,
                     ) as cluster:
            with cluster.connect() as session:
                session.execute("CREATE KEYSPACE IF NOT EXISTS k WITH REPLICATION = {" +
                                "'class' : 'NetworkTopologyStrategy', 'replication_factor' : 1 }")
                session.execute("DROP KEYSPACE k")

    async def shutdown_control_connection(self) -> None:
        """Shut down driver connection"""
        if self.control_connection is not None:
            self.control_connection.shutdown()
            self.control_connection = None
        if self.control_cluster is not None:
            self.control_cluster.shutdown()
            self.control_cluster = None

    async def stop(self) -> None:
        """Stop a running server. No-op if not running. Uses SIGKILL to
        stop, so is not graceful. Waits for the process to exit before return."""
        self.logger.info("stopping %s in %s", self, self.workdir.name)
        if not self.cmd:
            return

        await self.shutdown_control_connection()
        try:
            self.cmd.kill()
        except ProcessLookupError:
            pass
        else:
            await self.cmd.wait()
        finally:
            if self.cmd:
                self.logger.info("stopped %s in %s", self, self.workdir.name)
            self.cmd = None

    async def stop_gracefully(self) -> None:
        """Stop a running server. No-op if not running. Uses SIGTERM to
        stop, so it is graceful. Waits for the process to exit before return."""
        self.logger.info("gracefully stopping %s", self)
        if not self.cmd:
            return

        await self.shutdown_control_connection()
        try:
            self.cmd.terminate()
        except ProcessLookupError:
            pass
        else:
            STOP_TIMEOUT_SECONDS = 60
            wait_task = self.cmd.wait()
            try:
                await asyncio.wait_for(wait_task, timeout=STOP_TIMEOUT_SECONDS)
            except asyncio.TimeoutError:
                self.cmd.kill()
                await self.cmd.wait()
                raise RuntimeError(
                    f"Stopping server {self} gracefully took longer than {STOP_TIMEOUT_SECONDS}s")
        finally:
            if self.cmd:
                self.logger.info("gracefully stopped %s", self)
            self.cmd = None

    def pause(self) -> None:
        """Pause a running server."""
        if self.cmd:
            self.cmd.send_signal(signal.SIGSTOP)

    def unpause(self) -> None:
        """Unpause a paused server."""
        if self.cmd:
            self.cmd.send_signal(signal.SIGCONT)

    async def uninstall(self) -> None:
        """Clear all files left from a stopped server, including the
        data files and log files."""

        self.logger.info("Uninstalling server at %s", self.workdir)

        try:
            shutil.rmtree(self.workdir)
        except FileNotFoundError:
            pass
        self.log_filename.unlink(missing_ok=True)

    def write_log_marker(self, msg) -> None:
        """Write a message to the server's log file (e.g. separator/marker)"""
        self.log_file.seek(0, 2)  # seek to file end
        self.log_file.write(msg.encode())
        self.log_file.flush()

    def setLogger(self, logger: logging.LoggerAdapter):
        """Change the logger used by the server.
           Called when a cluster is reused between tests so that logs during the new test
           are prefixed appropriately with the corresponding test's name.
        """
        self.logger = logger

    def __str__(self):
        host_id = getattr(self, 'host_id', 'undefined id')
        return f"ScyllaServer({self.server_id}, {self.ip_addr}, {host_id})"

    def _write_config_file(self) -> None:
        with self.config_filename.open('w') as config_file:
            yaml.dump(self.config, config_file)


class ScyllaCluster:
    """A cluster of Scylla servers providing an API for changes"""
    # pylint: disable=too-many-instance-attributes

    class ActionReturn(NamedTuple):
        """Return status, message, and data (where applicable, otherwise empty) for API requests."""
        success: bool
        msg: str = ""
        data: dict = {}

    class CreateServerParams(NamedTuple):
        logger: Union[logging.Logger, logging.LoggerAdapter]
        cluster_name: str
        ip_addr: IPAddress
        seeds: List[str]
        config_from_test: dict[str, Any]
        cmdline_from_test: List[str]

    def __init__(self, logger: Union[logging.Logger, logging.LoggerAdapter],
                 host_registry: HostRegistry, replicas: int,
                 create_server: Callable[[CreateServerParams], ScyllaServer]) -> None:
        self.logger = logger
        self.host_registry = host_registry
        self.leased_ips = set[IPAddress]()
        self.name = str(uuid.uuid1())
        self.replicas = replicas
        self.create_server = create_server
        # Every ScyllaServer is in one of self.running, self.stopped.
        # These dicts are disjoint.
        # A server ID present in self.removed may be either in self.running or in self.stopped.
        self.running: Dict[ServerNum, ScyllaServer] = {}        # started servers
        self.stopped: Dict[ServerNum, ScyllaServer] = {}        # servers no longer running but present
        self.servers = ChainMap(self.running, self.stopped)
        self.removed: Set[ServerNum] = set()                    # removed servers (might be running)
        # cluster is started (but it might not have running servers)
        self.is_running: bool = False
        # cluster was modified in a way it should not be used in subsequent tests
        self.is_dirty: bool = False
        self.start_exception: Optional[Exception] = None
        self.keyspace_count = 0
        self.api = ScyllaRESTAPIClient()
        self.logger.info("Created new cluster %s", self.name)

    async def install_and_start(self) -> None:
        """Setup initial servers and start them.
           Catch and save any startup exception"""
        try:
            for _ in range(self.replicas):
                await self.add_server()
            if self.replicas > 0:
                self.keyspace_count = self._get_keyspace_count()
        except Exception as exc:
            # If start fails, swallow the error to throw later,
            # at test time.
            self.start_exception = exc
        self.is_running = True
        self.logger.info("Created cluster %s", self)
        self.is_dirty = False

    async def uninstall(self) -> None:
        """Stop running servers and uninstall all servers"""
        self.is_dirty = True
        self.logger.info("Uninstalling cluster %s", self)
        await self.stop()
        await asyncio.gather(*(srv.uninstall() for srv in self.stopped.values()))
        await asyncio.gather(*(self.host_registry.release_host(Host(ip))
                               for ip in self.leased_ips))

    async def release_ips(self) -> None:
        """Release all IPs leased from the host registry by this cluster.
        Call this function only if the cluster is stopped and will not be started again."""
        assert not self.running
        self.logger.info("Cluster %s releases ips %s", self, self.leased_ips)
        while self.leased_ips:
            ip = self.leased_ips.pop()
            await self.host_registry.release_host(Host(ip))

    async def stop(self) -> None:
        """Stop all running servers ASAP"""
        if self.is_running:
            self.logger.info("Cluster %s stopping", self)
            self.is_dirty = True
            # If self.running is empty, no-op
            await asyncio.gather(*(server.stop() for server in self.running.values()))
            self.stopped.update(self.running)
            self.running.clear()
            self.is_running = False

    async def stop_gracefully(self) -> None:
        """Stop all running servers in a clean way"""
        if self.is_running:
            self.logger.info("Cluster %s stopping gracefully", self)
            self.is_dirty = True
            # If self.running is empty, no-op
            await asyncio.gather(*(server.stop_gracefully() for server in self.running.values()))
            self.stopped.update(self.running)
            self.running.clear()
            self.is_running = False

    def _seeds(self) -> List[str]:
        return [server.ip_addr for server in self.running.values()]

    async def add_server(self, replace_cfg: Optional[ReplaceConfig] = None, cmdline: Optional[List[str]] = None, config: Optional[dict[str, Any]] = None, start: bool = True) -> ServerInfo:
        """Add a new server to the cluster"""
        self.is_dirty = True

        extra_config: dict[str, Any] = config.copy() if config else {}
        if replace_cfg:
            replaced_id = replace_cfg.replaced_id
            assert replaced_id in self.servers, \
                f"add_server: replaced id {replaced_id} not found in existing servers"

            replaced_srv = self.servers[replaced_id]
            if replace_cfg.use_host_id:
                extra_config['replace_node_first_boot'] = replaced_srv.host_id
            else:
                extra_config['replace_address_first_boot'] = replaced_srv.ip_addr

            if replace_cfg.ignore_dead_nodes:
                extra_config['ignore_dead_nodes_for_replace'] = ','.join(replace_cfg.ignore_dead_nodes)

            assert replaced_id not in self.removed, \
                f"add_server: cannot replace removed server {replaced_srv}"
            assert replaced_id in self.stopped, \
                f"add_server: cannot replace running server {replaced_srv}"

        if replace_cfg and replace_cfg.reuse_ip_addr:
            ip_addr = replaced_srv.ip_addr
        else:
            self.logger.info("Cluster %s waiting for new IP...", self.name)
            ip_addr = IPAddress(await self.host_registry.lease_host())
            self.logger.info("Cluster %s obtained new IP: %s", self.name, ip_addr)
            self.leased_ips.add(ip_addr)

        seeds = self._seeds()
        if not seeds:
            seeds = [ip_addr]

        params = ScyllaCluster.CreateServerParams(
            logger = self.logger,
            cluster_name = self.name,
            ip_addr = ip_addr,
            seeds = seeds,
            config_from_test = extra_config,
            cmdline_from_test = cmdline or []
        )

        try:
            server = self.create_server(params)
            self.logger.info("Cluster %s adding server...", self)
            if start:
                await server.install_and_start(self.api)
            else:
                await server.install()
        except Exception as exc:
            self.logger.error("Failed to start Scylla server at host %s in %s: %s",
                          ip_addr, server.workdir.name, str(exc))
            if not replace_cfg or not replace_cfg.reuse_ip_addr:
                self.leased_ips.remove(ip_addr)
                await self.host_registry.release_host(Host(ip_addr))
            raise
        if start:
            self.running[server.server_id] = server
        else:
            self.stopped[server.server_id] = server
        self.logger.info("Cluster %s added %s", self, server)
        return ServerInfo(server.server_id, server.ip_addr)

    def endpoint(self) -> str:
        """Get a server id (IP) from running servers"""
        return next(server.ip_addr for server in self.running.values())

    def take_log_savepoint(self) -> None:
        """Save the log size on all running servers"""
        for server in self.running.values():
            server.take_log_savepoint()

    def read_server_log(self) -> str:
        """Read log data of failed server"""
        # FIXME: pick failed server
        if self.running:
            return next(iter(self.running.values())).read_log()
        else:
            return ""

    def server_log_filename(self) -> Optional[pathlib.Path]:
        """The log file name of the failed server"""
        # FIXME: pick failed server
        if self.running:
            return next(server for server in self.running.values()).log_filename
        else:
            return None

    def __str__(self):
        running = ", ".join(str(server) for server in self.running.values())
        stopped = ", ".join(str(server) for server in self.stopped.values())
        return f"ScyllaCluster(name: {self.name}, running: {running}, stopped: {stopped})"

    def running_servers(self) -> List[Tuple[ServerNum, IPAddress]]:
        """Get a list of tuples of server id and IP address of running servers (and not removed)"""
        return [(server.server_id, server.ip_addr) for server in self.running.values()
                if server.server_id not in self.removed]

    def _get_keyspace_count(self) -> int:
        """Get the current keyspace count"""
        assert self.start_exception is None
        assert self.running, "No active nodes left"
        server = next(iter(self.running.values()))
        self.logger.debug("_get_keyspace_count() using server %s", server)
        assert server.control_connection is not None
        rows = server.control_connection.execute(
               "select count(*) as c from system_schema.keyspaces")
        keyspace_count = int(rows.one()[0])
        return keyspace_count

    def before_test(self, name) -> None:
        """Check that  the cluster is ready for a test. If
        there was a start error, throw it here - the server is
        running when it's added to the pool, which can't be attributed
        to any specific test, throwing it here would stop a specific
        test."""
        if self.start_exception:
            # Mark as dirty so further test cases don't try to reuse this cluster.
            self.is_dirty = True
            raise Exception(f'Exception when starting cluster {self}:\n{self.start_exception}')

        for server in self.running.values():
            server.write_log_marker(f"------ Starting test {name} ------\n")

    def after_test(self, name: str, success: bool) -> None:
        """Mark the cluster as dirty after a failed test.
        If the cluster is not dirty, check that it's still alive and the test
        hasn't left any garbage."""
        assert self.start_exception is None
        if not success:
            self.logger.debug(f"Test failed using cluster {self.name}, marking the cluster as dirty")
            self.is_dirty = True
        if self.is_dirty:
            self.logger.info(f"The cluster {self.name} is dirty, not checking"
                             f" keyspace count post-condition")
        else:
            if self.running and self._get_keyspace_count() != self.keyspace_count:
                raise RuntimeError(f"Test post-condition on cluster {self.name} failed, "
                                   f"the test must drop all keyspaces it creates.")
        for server in itertools.chain(self.running.values(), self.stopped.values()):
            server.write_log_marker(f"------ Ending test {name} ------\n")

    async def server_stop(self, server_id: ServerNum, gracefully: bool) -> ActionReturn:
        """Stop a server. No-op if already stopped."""
        self.logger.info("Cluster %s stopping server %s", self, server_id)
        if server_id in self.stopped:
            return ScyllaCluster.ActionReturn(success=True,
                                              msg=f"Server {server_id} already stopped")
        if server_id not in self.running:
            return ScyllaCluster.ActionReturn(success=False, msg=f"Server {server_id} unknown")
        self.is_dirty = True
        server = self.running[server_id]
        # Remove the server from `running` only after we successfully stop it.
        # Stopping may fail and if we removed it from `running` now it might leak.
        if gracefully:
            await server.stop_gracefully()
        else:
            await server.stop()
        self.running.pop(server_id)
        self.stopped[server_id] = server
        return ScyllaCluster.ActionReturn(success=True, msg=f"{server} stopped")

    def server_mark_removed(self, server_id: ServerNum) -> None:
        """Mark server as removed."""
        self.logger.debug("Cluster %s marking server %s as removed", self, server_id)
        self.removed.add(server_id)

    async def server_start(self, server_id: ServerNum, expected_error: Optional[str] = None) -> ActionReturn:
        """Start a stopped server"""
        if server_id in self.running:
            return ScyllaCluster.ActionReturn(success=True,
                                              msg=f"{self.running[server_id]} already running")
        if server_id not in self.stopped:
            return ScyllaCluster.ActionReturn(success=False, msg=f"Server {server_id} unknown")
        self.is_dirty = True
        server = self.stopped.pop(server_id)
        self.logger.info("Cluster %s starting server %s ip %s", self,
                         server_id, server.ip_addr)
        server.seeds = self._seeds()
        # Put the server in `running` before starting it.
        # Starting may fail and if we didn't add it now it might leak.
        self.running[server_id] = server
        await server.start(self.api, expected_error)
        if expected_error is not None:
            self.running.pop(server_id)
            self.stopped[server_id] = server
        return ScyllaCluster.ActionReturn(success=True, msg=f"{server} started")

    async def server_restart(self, server_id: ServerNum) -> ActionReturn:
        """Restart a running server"""
        self.logger.info("Cluster %s restarting server %s", self, server_id)
        ret = await self.server_stop(server_id, gracefully=True)
        if not ret.success:
            self.logger.error("Cluster %s failed to stop server %s", self, server_id)
            return ret
        return await self.server_start(server_id)

    def server_pause(self, server_id: ServerNum) -> None:
        """Pause a running server process."""
        self.logger.info("Cluster %s pausing server %s", self.name, server_id)
        assert server_id in self.running
        self.is_dirty = True
        server = self.running[server_id]
        server.pause()

    def server_unpause(self, server_id: ServerNum) -> None:
        """Unpause a paused server process."""
        self.logger.info("Cluster %s unpausing server %s", self.name, server_id)
        assert server_id in self.running
        server = self.running[server_id]
        server.unpause()

    def get_config(self, server_id: ServerNum) -> ActionReturn:
        """Get conf/scylla.yaml of the given server as a dictionary.
           Fails if the server cannot be found."""
        if not server_id in self.servers:
            return ScyllaCluster.ActionReturn(success=False, msg=f"Server {server_id} unknown")
        return ScyllaCluster.ActionReturn(success=True, data=self.servers[server_id].get_config())

    def update_config(self, server_id: ServerNum, key: str, value: object) -> ActionReturn:
        """Update conf/scylla.yaml of the given server by setting `value` under `key`.
           If the server is running, reload the config with a SIGHUP.
           Marks the cluster as dirty.
           Fails if the server cannot be found."""
        if not server_id in self.servers:
            return ScyllaCluster.ActionReturn(success=False, msg=f"Server {server_id} unknown")
        self.is_dirty = True
        self.servers[server_id].update_config(key, value)
        return ScyllaCluster.ActionReturn(success=True)

    def setLogger(self, logger: logging.LoggerAdapter):
        """Change the logger used by the cluster.
           Called when a cluster is reused between tests so that logs during the new test
           are prefixed appropriately with the corresponding test's name.
        """
        self.logger = logger
        for srv in self.servers.values():
            srv.setLogger(self.logger)

    async def change_ip(self, server_id: ServerNum) -> IPAddress:
        """Lease a new IP address and update conf/scylla.yaml with it. The
        original IP is released at the end of the test to avoid an
        immediate recycle within the same cluster. The server must be
        stopped before its ip is changed."""
        if server_id not in self.servers:
            raise RuntimeError(f"Server {server_id} unknown")
        server = self.servers[server_id]
        if server.is_running:
            raise RuntimeError(f"Server {server_id} is running: stop it first and then change its ip")
        self.is_dirty = True
        ip_addr = IPAddress(await self.host_registry.lease_host())
        self.leased_ips.add(ip_addr)
        logging.info("Cluster %s changed server %s IP from %s to %s", self.name,
                     server_id, server.ip_addr, ip_addr)
        server.change_ip(ip_addr)
        return ip_addr


class ScyllaClusterManager:
    """Manages a Scylla cluster for running test cases
       Provides an async API for tests to request changes in the Cluster.
       Parallel requests are not supported.
    """
    # pylint: disable=too-many-instance-attributes
    cluster: ScyllaCluster
    site: aiohttp.web.UnixSite
    is_after_test_ok: bool

    def __init__(self, test_uname: str, clusters: Pool[ScyllaCluster], base_dir: str) -> None:
        self.test_uname: str = test_uname
        logger = logging.getLogger(self.test_uname)
        self.logger = LogPrefixAdapter(logger, {'prefix': self.test_uname})
        # The currently running test case with self.test_uname prepended, e.g.
        # test_topology.1::test_add_server_add_column
        self.current_test_case_full_name: str = ''
        self.clusters: Pool[ScyllaCluster] = clusters
        self.is_running: bool = False
        self.is_before_test_ok: bool = False
        self.is_after_test_ok: bool = False
        # API
        # NOTE: need to make a safe temp dir as tempfile can't make a safe temp sock name
        # Put the socket in /tmp, not base_dir, to avoid going over the length
        # limit of UNIX-domain socket addresses (issue #12622).
        self.manager_dir: str = tempfile.mkdtemp(prefix="manager-", dir="/tmp")
        self.sock_path: str = f"{self.manager_dir}/api"
        app = aiohttp.web.Application()
        self._setup_routes(app)
        self.runner = aiohttp.web.AppRunner(app)

    async def start(self) -> None:
        """Get first cluster, setup API"""
        if self.is_running:
            self.logger.warning("ScyllaClusterManager already running")
            return
        self.cluster = await self.clusters.get(self.logger)
        self.logger.info("First Scylla cluster: %s", self.cluster)
        self.cluster.setLogger(self.logger)
        await self.runner.setup()
        self.site = aiohttp.web.UnixSite(self.runner, path=self.sock_path)
        await self.site.start()
        self.is_running = True

    async def _before_test(self, test_case_name: str) -> str:
        self.current_test_case_full_name = f'{self.test_uname}::{test_case_name}'
        self.logger.info("Setting up %s", self.current_test_case_full_name)
        if self.cluster.is_dirty:
            self.logger.info(f"Current cluster %s is dirty after test %s, replacing with a new one...",
                             self.cluster.name, self.current_test_case_full_name)
            self.cluster = await self.clusters.replace_dirty(self.cluster, self.logger)
            self.logger.info("Got new Scylla cluster: %s", self.cluster.name)
        self.cluster.setLogger(self.logger)
        self.logger.info("Leasing Scylla cluster %s for test %s", self.cluster, self.current_test_case_full_name)
        self.cluster.before_test(self.current_test_case_full_name)
        self.is_before_test_ok = True
        self.cluster.take_log_savepoint()
        return str(self.cluster)

    async def stop(self) -> None:
        """Stop, cycle last cluster if not dirty and present"""
        self.logger.info("ScyllaManager stopping for test %s", self.test_uname)
        if hasattr(self, "site"):
            await self.site.stop()
            del self.site
        if not self.cluster.is_dirty:
            self.logger.info("Returning Scylla cluster %s for test %s", self.cluster, self.test_uname)
            await self.clusters.put(self.cluster, is_dirty=False)
        else:
            self.logger.info("ScyllaManager: Scylla cluster %s is dirty after %s, stopping it",
                            self.cluster, self.test_uname)
            await self.clusters.put(self.cluster, is_dirty=True)
        del self.cluster
        if os.path.exists(self.manager_dir):
            shutil.rmtree(self.manager_dir)
        self.is_running = False

    def _setup_routes(self, app: aiohttp.web.Application) -> None:
        def make_catching_handler(handler: Callable) -> Callable:
            async def catching_handler(request) -> aiohttp.web.Response:
                """Catch all exceptions and return them to the client.
                   Without this, the client would get an 'Internal server error' message
                   without any details. Thanks to this the test log shows the actual error.
                """
                try:
                    return await handler(request)
                except Exception as e:
                    tb = traceback.format_exc()
                    self.logger.error(f'Exception when executing {handler.__name__}: {e}\n{tb}')
                    return aiohttp.web.Response(status=500, text=str(e))
            return catching_handler

        def add_get(route: str, handler: Callable):
            app.router.add_get(route, make_catching_handler(handler))

        def add_put(route: str, handler: Callable):
            app.router.add_put(route, make_catching_handler(handler))

        add_get('/up', self._manager_up)
        add_get('/cluster/up', self._cluster_up)
        add_get('/cluster/is-dirty', self._is_dirty)
        add_get('/cluster/replicas', self._cluster_replicas)
        add_get('/cluster/running-servers', self._cluster_running_servers)
        add_get('/cluster/host-ip/{server_id}', self._cluster_server_ip_addr)
        add_get('/cluster/host-id/{server_id}', self._cluster_host_id)
        add_get('/cluster/before-test/{test_case_name}', self._before_test_req)
        add_get('/cluster/after-test/{success}', self._after_test)
        add_get('/cluster/mark-dirty', self._mark_dirty)
        add_get('/cluster/server/{server_id}/stop', self._cluster_server_stop)
        add_get('/cluster/server/{server_id}/stop_gracefully', self._cluster_server_stop_gracefully)
        add_get('/cluster/server/{server_id}/start', self._cluster_server_start)
        add_get('/cluster/server/{server_id}/restart', self._cluster_server_restart)
        add_get('/cluster/server/{server_id}/pause', self._cluster_server_pause)
        add_get('/cluster/server/{server_id}/unpause', self._cluster_server_unpause)
        add_put('/cluster/addserver', self._cluster_server_add)
        add_put('/cluster/remove-node/{initiator}', self._cluster_remove_node)
        add_get('/cluster/decommission-node/{server_id}', self._cluster_decommission_node)
        add_get('/cluster/server/{server_id}/get_config', self._server_get_config)
        add_put('/cluster/server/{server_id}/update_config', self._server_update_config)
        add_put('/cluster/server/{server_id}/change_ip', self._server_change_ip)

    async def _manager_up(self, _request) -> aiohttp.web.Response:
        return aiohttp.web.Response(text=f"{self.is_running}")

    async def _cluster_up(self, _request) -> aiohttp.web.Response:
        """Is cluster running"""
        return aiohttp.web.Response(text=f"{self.cluster is not None and self.cluster.is_running}")

    async def _is_dirty(self, _request) -> aiohttp.web.Response:
        """Report if current cluster is dirty"""
        if self.cluster is None:
            return aiohttp.web.Response(status=500, text="No cluster active")
        return aiohttp.web.Response(text=f"{self.cluster.is_dirty}")

    async def _cluster_replicas(self, _request) -> aiohttp.web.Response:
        """Return cluster's configured number of replicas (replication factor)"""
        if self.cluster is None:
            return aiohttp.web.Response(status=500, text="No cluster active")
        return aiohttp.web.Response(text=f"{self.cluster.replicas}")

    async def _cluster_running_servers(self, _request) -> aiohttp.web.Response:
        """Return a dict of running server ids to IPs"""
        return aiohttp.web.json_response(self.cluster.running_servers())

    async def _cluster_server_ip_addr(self, request) -> aiohttp.web.Response:
        """IP address of a server"""
        server_id = ServerNum(int(request.match_info["server_id"]))
        return aiohttp.web.Response(text=f"{self.cluster.servers[server_id].ip_addr}")

    async def _cluster_host_id(self, request) -> aiohttp.web.Response:
        """IP address of a server"""
        server_id = ServerNum(int(request.match_info["server_id"]))
        return aiohttp.web.Response(text=f"{self.cluster.servers[server_id].host_id}")

    async def _before_test_req(self, request) -> aiohttp.web.Response:
        cluster_str = await self._before_test(request.match_info['test_case_name'])
        return aiohttp.web.Response(text=cluster_str)

    async def _after_test(self, _request) -> aiohttp.web.Response:
        assert self.cluster is not None
        assert self.current_test_case_full_name
        success = _request.match_info["success"] == "True"
        self.logger.info("Test %s %s, cluster: %s", self.current_test_case_full_name,
                         "SUCCEEDED" if success else "FAILED", self.cluster)
        try:
            self.cluster.after_test(self.current_test_case_full_name, success)
        finally:
            self.current_test_case_full_name = ''
        self.is_after_test_ok = True
        cluster_str = str(self.cluster)
        return aiohttp.web.Response(text=cluster_str)

    async def _mark_dirty(self, _request) -> aiohttp.web.Response:
        """Mark current cluster dirty"""
        assert self.cluster
        self.cluster.is_dirty = True
        return aiohttp.web.Response(text="OK")

    async def _server_stop(self, request: aiohttp.web.Request, gracefully: bool) \
                        -> aiohttp.web.Response:
        """Stop a server. No-op if already stopped."""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        ret = await self.cluster.server_stop(server_id, gracefully)
        return aiohttp.web.Response(status=200 if ret[0] else 500, text=ret[1])

    async def _cluster_server_stop(self, request) -> aiohttp.web.Response:
        """Stop a specified server"""
        assert self.cluster
        return await self._server_stop(request, gracefully = False)

    async def _cluster_server_stop_gracefully(self, request) -> aiohttp.web.Response:
        """Stop a specified server gracefully"""
        assert self.cluster
        return await self._server_stop(request, gracefully = True)

    async def _cluster_server_start(self, request) -> aiohttp.web.Response:
        """Start a specified server (must be stopped)"""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        expected_error = request.query.get("expected_error")
        ret = await self.cluster.server_start(server_id, expected_error)
        return aiohttp.web.Response(status=200 if ret[0] else 500, text=ret[1])

    async def _cluster_server_restart(self, request) -> aiohttp.web.Response:
        """Restart a specified server (must be already started)"""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        ret = await self.cluster.server_restart(server_id)
        return aiohttp.web.Response(status=200 if ret[0] else 500, text=ret[1])

    async def _cluster_server_pause(self, request) -> aiohttp.web.Response:
        """Pause the specified server."""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        self.cluster.server_pause(server_id)
        return aiohttp.web.Response(status=200)

    async def _cluster_server_unpause(self, request) -> aiohttp.web.Response:
        """Pause the specified server."""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        self.cluster.server_unpause(server_id)
        return aiohttp.web.Response(status=200)

    async def _cluster_server_add(self, request) -> aiohttp.web.Response:
        """Add a new server"""
        assert self.cluster
        data = await request.json()
        replace_cfg = ReplaceConfig(**data["replace_cfg"]) if "replace_cfg" in data else None
        s_info = await self.cluster.add_server(replace_cfg, data.get('cmdline'), data.get('config'), data.get('start', True))
        return aiohttp.web.json_response({"server_id" : s_info.server_id,
                                          "ip_addr": s_info.ip_addr})

    async def _cluster_remove_node(self, request: aiohttp.web.Request) -> aiohttp.web.Response:
        """Run remove node on Scylla REST API for a specified server"""
        assert self.cluster
        data = await request.json()
        initiator_id = ServerNum(int(request.match_info["initiator"]))
        server_id = ServerNum(int(data["server_id"]))
        assert isinstance(data["ignore_dead"], list), "Invalid list of dead IP addresses"
        ignore_dead = [IPAddress(ip_addr) for ip_addr in data["ignore_dead"]]
        if not initiator_id in self.cluster.running:
            self.logger.error("_cluster_remove_node initiator %s is not a running server",
                          initiator_id)
            return aiohttp.web.Response(status=500, text=f"Error removing {server_id}")
        if server_id in self.cluster.running:
            self.logger.warning("_cluster_remove_node %s is a running node", server_id)
        else:
            assert server_id in self.cluster.stopped, f"_cluster_remove_node: {server_id} unknown"
        to_remove = self.cluster.servers[server_id]
        initiator = self.cluster.servers[initiator_id]
        self.logger.info("_cluster_remove_node %s with initiator %s", to_remove, initiator)

        # initate remove
        try:
            await self.cluster.api.remove_node(initiator.ip_addr, to_remove.host_id, ignore_dead,
                                               timeout=ScyllaServer.TOPOLOGY_TIMEOUT)
        except RuntimeError as exc:
            self.logger.error("_cluster_remove_node failed initiator %s server %s ignore_dead %s, check log at %s",
                          initiator, to_remove, ignore_dead, initiator.log_filename)
            return aiohttp.web.Response(status=500,
                                        text=f"Error removing {to_remove}: {exc}")
        self.cluster.server_mark_removed(server_id)
        return aiohttp.web.Response(text="OK")

    async def _cluster_decommission_node(self, request) -> aiohttp.web.Response:
        """Run remove node on Scylla REST API for a specified server"""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        self.logger.info("_cluster_decommission_node %s", server_id)
        assert server_id in self.cluster.running, "Can't decommission not running node"
        if len(self.cluster.running) == 1:
            self.logger.warning("_cluster_decommission_node %s is only running node left", server_id)
        server = self.cluster.running[server_id]
        try:
            await self.cluster.api.decommission_node(server.ip_addr, timeout=ScyllaServer.TOPOLOGY_TIMEOUT)
        except RuntimeError as exc:
            self.logger.error("_cluster_decommission_node %s, check log at %s", server,
                          server.log_filename)
            return aiohttp.web.Response(status=500,
                                        text=f"Error decommissioning {server}: {exc}")
        await self.cluster.server_stop(server_id, gracefully=True)
        return aiohttp.web.Response(text="OK")

    async def _server_get_config(self, request: aiohttp.web.Request) -> aiohttp.web.Response:
        """Get conf/scylla.yaml of the given server as a dictionary."""
        assert self.cluster
        ret = self.cluster.get_config(ServerNum(int(request.match_info["server_id"])))
        if not ret.success:
            return aiohttp.web.Response(status=404, text=ret.msg)
        return aiohttp.web.json_response(ret.data)

    async def _server_update_config(self, request: aiohttp.web.Request) -> aiohttp.web.Response:
        """Update conf/scylla.yaml of the given server by setting `value` under `key`.
           If the server is running, reload the config with a SIGHUP.
           Marks the cluster as dirty."""
        assert self.cluster
        data = await request.json()
        ret = self.cluster.update_config(ServerNum(int(request.match_info["server_id"])),
                                         data['key'], data['value'])
        if not ret.success:
            return aiohttp.web.Response(status=404, text=ret.msg)
        return aiohttp.web.Response()

    async def _server_change_ip(self, request: aiohttp.web.Request) -> aiohttp.web.Response:
        """Pass change_ip command for the given server to the cluster"""
        assert self.cluster
        server_id = ServerNum(int(request.match_info["server_id"]))
        ip_addr = await self.cluster.change_ip(server_id)
        return aiohttp.web.json_response({"ip_addr": ip_addr})


@asynccontextmanager
async def get_cluster_manager(test_uname: str, clusters: Pool[ScyllaCluster], test_path: str) \
        -> AsyncIterator[ScyllaClusterManager]:
    """Create a temporary manager for the active cluster used in a test
       and provide the cluster to the caller."""
    manager = ScyllaClusterManager(test_uname, clusters, test_path)
    try:
        yield manager
    finally:
        await manager.stop()
