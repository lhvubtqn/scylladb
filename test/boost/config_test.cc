/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <boost/test/unit_test.hpp>
#include <stdlib.h>
#include <iostream>

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/future-util.hh>
#include "db/config.hh"
#include "utils/updateable_value.hh"
#include <algorithm>

using namespace db;

SEASTAR_THREAD_TEST_CASE(test_updateable_value_basics) {
    using namespace utils;
    updateable_value_source<int> source;
    source.set(3);
    updateable_value<int> u1(source);
    updateable_value<int> u2(u1);
    updateable_value<int> u3(std::move(u2));
    u2 = u3;
    BOOST_REQUIRE_EQUAL(u1.get(), 3);
    BOOST_REQUIRE_EQUAL(u2.get(), 3);
    BOOST_REQUIRE_EQUAL(u3.get(), 3);
    unsigned called = 0;
    auto u3observer = u3.observe([&] (int v) {
        ++called;
        BOOST_REQUIRE_EQUAL(v, 4);
    });
    source.set(4);
    BOOST_REQUIRE_EQUAL(u1.get(), 4);
    BOOST_REQUIRE_EQUAL(u2.get(), 4);
    BOOST_REQUIRE_EQUAL(u3.get(), 4);
    BOOST_REQUIRE_EQUAL(called, 1);
}

// stock, default cassandra.yaml
const char* cassandra_conf = R"apa(
# Cassandra storage config YAML 

# NOTE:
#   See http://wiki.apache.org/cassandra/StorageConfiguration for
#   full explanations of configuration directives
# /NOTE

# The name of the cluster. This is mainly used to prevent machines in
# one logical cluster from joining another.
cluster_name: 'Test Cluster'

# This defines the number of tokens randomly assigned to this node on the ring
# The more tokens, relative to other nodes, the larger the proportion of data
# that this node will store. You probably want all nodes to have the same number
# of tokens assuming they have equal hardware capability.
#
# If you leave this unspecified, Cassandra will use the default of 1 token for legacy compatibility,
# and will use the initial_token as described below.
#
# Specifying initial_token will override this setting on the node's initial start,
# on subsequent starts, this setting will apply even if initial token is set.
#
# If you already have a cluster with 1 token per node, and wish to migrate to 
# multiple tokens per node, see http://wiki.apache.org/cassandra/Operations
num_tokens: 256

# initial_token allows you to specify tokens manually.  While you can use # it with
# vnodes (num_tokens > 1, above) -- in which case you should provide a 
# comma-separated list -- it's primarily used when adding nodes # to legacy clusters 
# that do not have vnodes enabled.
# initial_token:

# See http://wiki.apache.org/cassandra/HintedHandoff
# May either be "true" or "false" to enable globally, or contain a list
# of data centers to enable per-datacenter.
# hinted_handoff_enabled: DC1,DC2
hinted_handoff_enabled: true
# this defines the maximum amount of time a dead host will have hints
# generated.  After it has been dead this long, new hints for it will not be
# created until it has been seen alive and gone down again.
max_hint_window_in_ms: 10800000 # 3 hours
# Maximum throttle in KBs per second, per delivery thread.  This will be
# reduced proportionally to the number of nodes in the cluster.  (If there
# are two nodes in the cluster, each delivery thread will use the maximum
# rate; if there are three, each will throttle to half of the maximum,
# since we expect two nodes to be delivering hints simultaneously.)
hinted_handoff_throttle_in_kb: 1024
# Number of threads with which to deliver hints;
# Consider increasing this number when you have multi-dc deployments, since
# cross-dc handoff tends to be slower
max_hints_delivery_threads: 2

# Maximum throttle in KBs per second, total. This will be
# reduced proportionally to the number of nodes in the cluster.
batchlog_replay_throttle_in_kb: 1024

# Authentication backend, implementing IAuthenticator; used to identify users
# Out of the box, Cassandra provides org.apache.cassandra.auth.{AllowAllAuthenticator,
# PasswordAuthenticator}.
#
# - AllowAllAuthenticator performs no checks - set it to disable authentication.
# - PasswordAuthenticator relies on username/password pairs to authenticate
#   users. It keeps usernames and hashed passwords in system_auth.credentials table.
#   Please increase system_auth keyspace replication factor if you use this authenticator.
authenticator: AllowAllAuthenticator

# Authorization backend, implementing IAuthorizer; used to limit access/provide permissions
# Out of the box, Cassandra provides org.apache.cassandra.auth.{AllowAllAuthorizer,
# CassandraAuthorizer}.
#
# - AllowAllAuthorizer allows any action to any user - set it to disable authorization.
# - CassandraAuthorizer stores permissions in system_auth.permissions table. Please
#   increase system_auth keyspace replication factor if you use this authorizer.
authorizer: AllowAllAuthorizer

# Validity period for permissions cache (fetching permissions can be an
# expensive operation depending on the authorizer, CassandraAuthorizer is
# one example). Defaults to 2000, set to 0 to disable.
# Will be disabled automatically for AllowAllAuthorizer.
permissions_validity_in_ms: 2000

# Refresh interval for permissions cache (if enabled).
# After this interval, cache entries become eligible for refresh. Upon next
# access, an async reload is scheduled and the old value returned until it
# completes. If permissions_validity_in_ms is non-zero, then this must be
# also.
# Defaults to the same value as permissions_validity_in_ms.
# permissions_update_interval_in_ms: 1000

# The partitioner is responsible for distributing groups of rows (by
# partition key) across nodes in the cluster.  You should leave this
# alone for new clusters.  The partitioner can NOT be changed without
# reloading all data, so when upgrading you should set this to the
# same partitioner you were already using.
#
# Murmur3Partitioner is currently the only supported partitioner,
#
partitioner: org.apache.cassandra.dht.Murmur3Partitioner

# Directories where Cassandra should store data on disk.  Cassandra
# will spread data evenly across them, subject to the granularity of
# the configured compaction strategy.
# If not set, the default directory is $CASSANDRA_HOME/data/data.
# data_file_directories:
#     - /var/lib/cassandra/data

# commit log.  when running on magnetic HDD, this should be a
# separate spindle than the data directories.
# If not set, the default directory is $CASSANDRA_HOME/data/commitlog.
# commitlog_directory: /var/lib/cassandra/commitlog

# policy for data disk failures:
# die: shut down gossip and Thrift and kill the JVM for any fs errors or
#      single-sstable errors, so the node can be replaced.
# stop_paranoid: shut down gossip and Thrift even for single-sstable errors.
# stop: shut down gossip and Thrift, leaving the node effectively dead, but
#       can still be inspected via JMX.
# best_effort: stop using the failed disk and respond to requests based on
#              remaining available sstables.  This means you WILL see obsolete
#              data at CL.ONE!
# ignore: ignore fatal errors and let requests fail, as in pre-1.2 Cassandra
disk_failure_policy: stop

# policy for commit disk failures:
# die: shut down gossip and Thrift and kill the JVM, so the node can be replaced.
# stop: shut down gossip and Thrift, leaving the node effectively dead, but
#       can still be inspected via JMX.
# stop_commit: shutdown the commit log, letting writes collect but
#              continuing to service reads, as in pre-2.0.5 Cassandra
# ignore: ignore fatal errors and let the batches fail
commit_failure_policy: stop

# Maximum size of the key cache in memory.
#
# Each key cache hit saves 1 seek and each row cache hit saves 2 seeks at the
# minimum, sometimes more. The key cache is fairly tiny for the amount of
# time it saves, so it's worthwhile to use it at large numbers.
# The row cache saves even more time, but must contain the entire row,
# so it is extremely space-intensive. It's best to only use the
# row cache if you have hot rows or static rows.
#
# NOTE: if you reduce the size, you may not get you hottest keys loaded on startup.
#
# Default value is empty to make it "auto" (min(5% of Heap (in MB), 100MB)). Set to 0 to disable key cache.
key_cache_size_in_mb:

# Duration in seconds after which Cassandra should
# save the key cache. Caches are saved to saved_caches_directory as
# specified in this configuration file.
#
# Saved caches greatly improve cold-start speeds, and is relatively cheap in
# terms of I/O for the key cache. Row cache saving is much more expensive and
# has limited use.
#
# Default is 14400 or 4 hours.
key_cache_save_period: 14400

# Number of keys from the key cache to save
# Disabled by default, meaning all keys are going to be saved
# key_cache_keys_to_save: 100

# Maximum size of the row cache in memory.
# NOTE: if you reduce the size, you may not get you hottest keys loaded on startup.
#
# Default value is 0, to disable row caching.
row_cache_size_in_mb: 0

# Duration in seconds after which Cassandra should
# save the row cache. Caches are saved to saved_caches_directory as specified
# in this configuration file.
#
# Saved caches greatly improve cold-start speeds, and is relatively cheap in
# terms of I/O for the key cache. Row cache saving is much more expensive and
# has limited use.
#
# Default is 0 to disable saving the row cache.
row_cache_save_period: 0

# Number of keys from the row cache to save
# Disabled by default, meaning all keys are going to be saved
# row_cache_keys_to_save: 100

# Maximum size of the counter cache in memory.
#
# Counter cache helps to reduce counter locks' contention for hot counter cells.
# In case of RF = 1 a counter cache hit will cause Cassandra to skip the read before
# write entirely. With RF > 1 a counter cache hit will still help to reduce the duration
# of the lock hold, helping with hot counter cell updates, but will not allow skipping
# the read entirely. Only the local (clock, count) tuple of a counter cell is kept
# in memory, not the whole counter, so it's relatively cheap.
#
# NOTE: if you reduce the size, you may not get you hottest keys loaded on startup.
#
# Default value is empty to make it "auto" (min(2.5% of Heap (in MB), 50MB)). Set to 0 to disable counter cache.
# NOTE: if you perform counter deletes and rely on low gcgs, you should disable the counter cache.
counter_cache_size_in_mb:

# Duration in seconds after which Cassandra should
# save the counter cache (keys only). Caches are saved to saved_caches_directory as
# specified in this configuration file.
#
# Default is 7200 or 2 hours.
counter_cache_save_period: 7200

# Number of keys from the counter cache to save
# Disabled by default, meaning all keys are going to be saved
# counter_cache_keys_to_save: 100

# The off-heap memory allocator.  Affects storage engine metadata as
# well as caches.  Experiments show that JEMAlloc saves some memory
# than the native GCC allocator (i.e., JEMalloc is more
# fragmentation-resistant).
# 
# Supported values are: NativeAllocator, JEMallocAllocator
#
# If you intend to use JEMallocAllocator you have to install JEMalloc as library and
# modify cassandra-env.sh as directed in the file.
#
# Defaults to NativeAllocator
# memory_allocator: NativeAllocator

# saved caches
# If not set, the default directory is $CASSANDRA_HOME/data/saved_caches.
# saved_caches_directory: /var/lib/cassandra/saved_caches

# commitlog_sync may be either "periodic" or "batch." 
# When in batch mode, Cassandra won't ack writes until the commit log
# has been fsynced to disk.  It will wait up to
# commitlog_sync_batch_window_in_ms milliseconds for other writes, before
# performing the sync.
#
# commitlog_sync: batch
# commitlog_sync_batch_window_in_ms: 50
#
# the other option is "periodic" where writes may be acked immediately
# and the CommitLog is simply synced every commitlog_sync_period_in_ms
# milliseconds. 
commitlog_sync: periodic
commitlog_sync_period_in_ms: 10000

# The size of the individual commitlog file segments.  A commitlog
# segment may be archived, deleted, or recycled once all the data
# in it (potentially from each columnfamily in the system) has been
# flushed to sstables.  
#
# The default size is 32, which is almost always fine, but if you are
# archiving commitlog segments (see commitlog_archiving.properties),
# then you probably want a finer granularity of archiving; 8 or 16 MB
# is reasonable.
commitlog_segment_size_in_mb: 32

# any class that implements the SeedProvider interface and has a
# constructor that takes a Map<String, String> of parameters will do.
seed_provider:
    # Addresses of hosts that are deemed contact points. 
    # Cassandra nodes use this list of hosts to find each other and learn
    # the topology of the ring.  You must change this if you are running
    # multiple nodes!
    - class_name: org.apache.cassandra.locator.SimpleSeedProvider
      parameters:
          # seeds is actually a comma-delimited list of addresses.
          # Ex: "<ip1>,<ip2>,<ip3>"
          - seeds: "127.0.0.1"

# For workloads with more data than can fit in memory, Cassandra's
# bottleneck will be reads that need to fetch data from
# disk. "concurrent_reads" should be set to (16 * number_of_drives) in
# order to allow the operations to enqueue low enough in the stack
# that the OS and drives can reorder them. Same applies to
# "concurrent_counter_writes", since counter writes read the current
# values before incrementing and writing them back.
#
# On the other hand, since writes are almost never IO bound, the ideal
# number of "concurrent_writes" is dependent on the number of cores in
# your system; (8 * number_of_cores) is a good rule of thumb.
concurrent_reads: 32
concurrent_writes: 32
concurrent_counter_writes: 32

# Total memory to use for sstable-reading buffers.  Defaults to
# the smaller of 1/4 of heap or 512MB.
# file_cache_size_in_mb: 512

# Total permitted memory to use for memtables. Cassandra will stop 
# accepting writes when the limit is exceeded until a flush completes,
# and will trigger a flush based on memtable_cleanup_threshold
# If omitted, Cassandra will set both to 1/4 the size of the heap.
# memtable_heap_space_in_mb: 2048
# memtable_offheap_space_in_mb: 2048

# Ratio of occupied non-flushing memtable size to total permitted size
# that will trigger a flush of the largest memtable.  Lager mct will
# mean larger flushes and hence less compaction, but also less concurrent
# flush activity which can make it difficult to keep your disks fed
# under heavy write load.
#
# memtable_cleanup_threshold defaults to 1 / (memtable_flush_writers + 1)
# memtable_cleanup_threshold: 0.11

# Specify the way Cassandra allocates and manages memtable memory.
# Options are:
#   heap_buffers:    on heap nio buffers
#   offheap_buffers: off heap (direct) nio buffers
#   offheap_objects: native memory, eliminating nio buffer heap overhead
memtable_allocation_type: heap_buffers

# Total space to use for commitlogs.  Since commitlog segments are
# mmapped, and hence use up address space, the default size is 32
# on 32-bit JVMs, and 8192 on 64-bit JVMs.
#
# If space gets above this value (it will round up to the next nearest
# segment multiple), Cassandra will flush every dirty CF in the oldest
# segment and remove it.  So a small total commitlog space will tend
# to cause more flush activity on less-active columnfamilies.
# commitlog_total_space_in_mb: 8192

# This sets the amount of memtable flush writer threads.  These will
# be blocked by disk io, and each one will hold a memtable in memory
# while blocked. 
#
# memtable_flush_writers defaults to the smaller of (number of disks,
# number of cores), with a minimum of 2 and a maximum of 8.
# 
# If your data directories are backed by SSD, you should increase this
# to the number of cores.
#memtable_flush_writers: 8

# A fixed memory pool size in MB for for SSTable index summaries. If left
# empty, this will default to 5% of the heap size. If the memory usage of
# all index summaries exceeds this limit, SSTables with low read rates will
# shrink their index summaries in order to meet this limit.  However, this
# is a best-effort process. In extreme conditions Cassandra may need to use
# more than this amount of memory.
index_summary_capacity_in_mb:

# How frequently index summaries should be resampled.  This is done
# periodically to redistribute memory from the fixed-size pool to sstables
# proportional their recent read rates.  Setting to -1 will disable this
# process, leaving existing index summaries at their current sampling level.
index_summary_resize_interval_in_minutes: 60

# Whether to, when doing sequential writing, fsync() at intervals in
# order to force the operating system to flush the dirty
# buffers. Enable this to avoid sudden dirty buffer flushing from
# impacting read latencies. Almost always a good idea on SSDs; not
# necessarily on platters.
trickle_fsync: false
trickle_fsync_interval_in_kb: 10240

# TCP port, for commands and data
storage_port: 7000

# SSL port, for encrypted communication.  Unused unless enabled in
# encryption_options
ssl_storage_port: 7001

# Address or interface to bind to and tell other Cassandra nodes to connect to.
# You _must_ change this if you want multiple nodes to be able to communicate!
#
# Set listen_address OR listen_interface, not both. Interfaces must correspond
# to a single address, IP aliasing is not supported.
#
# Leaving it blank leaves it up to InetAddress.getLocalHost(). This
# will always do the Right Thing _if_ the node is properly configured
# (hostname, name resolution, etc), and the Right Thing is to use the
# address associated with the hostname (it might not be).
#
# Setting listen_address to 0.0.0.0 is always wrong.
listen_address: localhost
# listen_interface: eth0

# Address to broadcast to other Cassandra nodes
# Leaving this blank will set it to the same value as listen_address
# broadcast_address: 1.2.3.4

# Internode authentication backend, implementing IInternodeAuthenticator;
# used to allow/disallow connections from peer nodes.
# internode_authenticator: org.apache.cassandra.auth.AllowAllInternodeAuthenticator

# Whether to start the native transport server.
# Please note that the address on which the native transport is bound is the
# same as the rpc_address. The port however is different and specified below.
start_native_transport: true
# port for the CQL native transport to listen for clients on
native_transport_port: 9042
# The maximum threads for handling requests when the native transport is used.
# This is similar to rpc_max_threads though the default differs slightly (and
# there is no native_transport_min_threads, idle threads will always be stopped
# after 30 seconds).
# native_transport_max_threads: 128
#
# The maximum size of allowed frame. Frame (requests) larger than this will
# be rejected as invalid. The default is 256MB.
# native_transport_max_frame_size_in_mb: 256

# Whether to start the thrift rpc server.
start_rpc: true

# The address or interface to bind the Thrift RPC service and native transport
# server to.
#
# Set rpc_address OR rpc_interface, not both. Interfaces must correspond
# to a single address, IP aliasing is not supported.
#
# Leaving rpc_address blank has the same effect as on listen_address
# (i.e. it will be based on the configured hostname of the node).
#
# Note that unlike listen_address, you can specify 0.0.0.0, but you must also
# set broadcast_rpc_address to a value other than 0.0.0.0.
rpc_address: localhost
# rpc_interface: eth1

# port for Thrift to listen for clients on
rpc_port: 9160

# RPC address to broadcast to drivers and other Cassandra nodes. This cannot
# be set to 0.0.0.0. If left blank, this will be set to the value of
# rpc_address. If rpc_address is set to 0.0.0.0, broadcast_rpc_address must
# be set.
# broadcast_rpc_address: 1.2.3.4

# enable or disable keepalive on rpc/native connections
rpc_keepalive: true

# Cassandra provides two out-of-the-box options for the RPC Server:
#
# sync  -> One thread per thrift connection. For a very large number of clients, memory
#          will be your limiting factor. On a 64 bit JVM, 180KB is the minimum stack size
#          per thread, and that will correspond to your use of virtual memory (but physical memory
#          may be limited depending on use of stack space).
#
# hsha  -> Stands for "half synchronous, half asynchronous." All thrift clients are handled
#          asynchronously using a small number of threads that does not vary with the amount
#          of thrift clients (and thus scales well to many clients). The rpc requests are still
#          synchronous (one thread per active request). If hsha is selected then it is essential
#          that rpc_max_threads is changed from the default value of unlimited.
#
# The default is sync because on Windows hsha is about 30% slower.  On Linux,
# sync/hsha performance is about the same, with hsha of course using less memory.
#
# Alternatively,  can provide your own RPC server by providing the fully-qualified class name
# of an o.a.c.t.TServerFactory that can create an instance of it.
rpc_server_type: sync

# Uncomment rpc_min|max_thread to set request pool size limits.
#
# Regardless of your choice of RPC server (see above), the number of maximum requests in the
# RPC thread pool dictates how many concurrent requests are possible (but if you are using the sync
# RPC server, it also dictates the number of clients that can be connected at all).
#
# The default is unlimited and thus provides no protection against clients overwhelming the server. You are
# encouraged to set a maximum that makes sense for you in production, but do keep in mind that
# rpc_max_threads represents the maximum number of client requests this server may execute concurrently.
#
# rpc_min_threads: 16
# rpc_max_threads: 2048

# uncomment to set socket buffer sizes on rpc connections
# rpc_send_buff_size_in_bytes:
# rpc_recv_buff_size_in_bytes:

# Uncomment to set socket buffer size for internode communication
# Note that when setting this, the buffer size is limited by net.core.wmem_max
# and when not setting it it is defined by net.ipv4.tcp_wmem
# See:
# /proc/sys/net/core/wmem_max
# /proc/sys/net/core/rmem_max
# /proc/sys/net/ipv4/tcp_wmem
# /proc/sys/net/ipv4/tcp_wmem
# and: man tcp
# internode_send_buff_size_in_bytes:
# internode_recv_buff_size_in_bytes:

# Frame size for thrift (maximum message length).
thrift_framed_transport_size_in_mb: 15

# Set to true to have Cassandra create a hard link to each sstable
# flushed or streamed locally in a backups/ subdirectory of the
# keyspace data.  Removing these links is the operator's
# responsibility.
incremental_backups: false

# Whether or not to take a snapshot before each compaction.  Be
# careful using this option, since Cassandra won't clean up the
# snapshots for you.  Mostly useful if you're paranoid when there
# is a data format change.
snapshot_before_compaction: false

# Whether or not a snapshot is taken of the data before keyspace truncation
# or dropping of column families. The STRONGLY advised default of true 
# should be used to provide data safety. If you set this flag to false, you will
# lose data on truncation or drop.
auto_snapshot: true

# When executing a scan, within or across a partition, we need to keep the
# tombstones seen in memory so we can return them to the coordinator, which
# will use them to make sure other replicas also know about the deleted rows.
# With workloads that generate a lot of tombstones, this can cause performance
# problems and even exaust the server heap.
# (http://www.datastax.com/dev/blog/cassandra-anti-patterns-queues-and-queue-like-datasets)
# Adjust the thresholds here if you understand the dangers and want to
# scan more tombstones anyway.  These thresholds may also be adjusted at runtime
# using the StorageService mbean.
tombstone_warn_threshold: 1000
tombstone_failure_threshold: 100000

# Granularity of the collation index of rows within a partition.
# Increase if your rows are large, or if you have a very large
# number of rows per partition.  The competing goals are these:
#   1) a smaller granularity means more index entries are generated
#      and looking up rows withing the partition by collation column
#      is faster
#   2) but, Cassandra will keep the collation index in memory for hot
#      rows (as part of the key cache), so a larger granularity means
#      you can cache more hot rows
column_index_size_in_kb: 64


# Log WARN on any batch size exceeding this value. 5kb per batch by default.
# Caution should be taken on increasing the size of this threshold as it can lead to node instability.
batch_size_warn_threshold_in_kb: 5

# Number of simultaneous compactions to allow, NOT including
# validation "compactions" for anti-entropy repair.  Simultaneous
# compactions can help preserve read performance in a mixed read/write
# workload, by mitigating the tendency of small sstables to accumulate
# during a single long running compactions. The default is usually
# fine and if you experience problems with compaction running too
# slowly or too fast, you should look at
# compaction_throughput_mb_per_sec first.
#
# concurrent_compactors defaults to the smaller of (number of disks,
# number of cores), with a minimum of 2 and a maximum of 8.
# 
# If your data directories are backed by SSD, you should increase this
# to the number of cores.
#concurrent_compactors: 1

# Throttles compaction to the given total throughput across the entire
# system. The faster you insert data, the faster you need to compact in
# order to keep the sstable count down, but in general, setting this to
# 16 to 32 times the rate you are inserting data is more than sufficient.
# Setting this to 0 disables throttling. Note that this account for all types
# of compaction, including validation compaction.
compaction_throughput_mb_per_sec: 16

# When compacting, the replacement sstable(s) can be opened before they
# are completely written, and used in place of the prior sstables for
# any range that has been written. This helps to smoothly transfer reads 
# between the sstables, reducing page cache churn and keeping hot rows hot
sstable_preemptive_open_interval_in_mb: 50

# Throttles all outbound streaming file transfers on this node to the
# given total throughput in Mbps. This is necessary because Cassandra does
# mostly sequential IO when streaming data during bootstrap or repair, which
# can lead to saturating the network connection and degrading rpc performance.
# When unset, the default is 200 Mbps or 25 MB/s.
# stream_throughput_outbound_megabits_per_sec: 200

# Throttles all streaming file transfer between the datacenters,
# this setting allows users to throttle inter dc stream throughput in addition
# to throttling all network stream traffic as configured with
# stream_throughput_outbound_megabits_per_sec
# inter_dc_stream_throughput_outbound_megabits_per_sec:

# How long the coordinator should wait for read operations to complete
read_request_timeout_in_ms: 5000
# How long the coordinator should wait for seq or index scans to complete
range_request_timeout_in_ms: 10000
# How long the coordinator should wait for writes to complete
write_request_timeout_in_ms: 2000
# How long the coordinator should wait for counter writes to complete
counter_write_request_timeout_in_ms: 5000
# How long a coordinator should continue to retry a CAS operation
# that contends with other proposals for the same row
cas_contention_timeout_in_ms: 1000
# How long the coordinator should wait for truncates to complete
# (This can be much longer, because unless auto_snapshot is disabled
# we need to flush first so we can snapshot before removing the data.)
truncate_request_timeout_in_ms: 60000
# The default timeout for other, miscellaneous operations
request_timeout_in_ms: 10000

# Enable operation timeout information exchange between nodes to accurately
# measure request timeouts.  If disabled, replicas will assume that requests
# were forwarded to them instantly by the coordinator, which means that
# under overload conditions we will waste that much extra time processing 
# already-timed-out requests.
#
# Warning: before enabling this property make sure to ntp is installed
# and the times are synchronized between the nodes.
cross_node_timeout: false

# Enable socket timeout for streaming operation.
# When a timeout occurs during streaming, streaming is retried from the start
# of the current file. This _can_ involve re-streaming an important amount of
# data, so you should avoid setting the value too low.
# Default value is 0, which never timeout streams.
# streaming_socket_timeout_in_ms: 0

# phi value that must be reached for a host to be marked down.
# most users should never need to adjust this.
# phi_convict_threshold: 8

# endpoint_snitch -- Set this to a class that implements
# IEndpointSnitch.  The snitch has two functions:
# - it teaches Cassandra enough about your network topology to route
#   requests efficiently
# - it allows Cassandra to spread replicas around your cluster to avoid
#   correlated failures. It does this by grouping machines into
#   "datacenters" and "racks."  Cassandra will do its best not to have
#   more than one replica on the same "rack" (which may not actually
#   be a physical location)
#
# IF YOU CHANGE THE SNITCH AFTER DATA IS INSERTED INTO THE CLUSTER,
# YOU MUST RUN A FULL REPAIR, SINCE THE SNITCH AFFECTS WHERE REPLICAS
# ARE PLACED.
#
# Out of the box, Cassandra provides
#  - SimpleSnitch:
#    Treats Strategy order as proximity. This can improve cache
#    locality when disabling read repair.  Only appropriate for
#    single-datacenter deployments.
#  - GossipingPropertyFileSnitch
#    This should be your go-to snitch for production use.  The rack
#    and datacenter for the local node are defined in
#    cassandra-rackdc.properties and propagated to other nodes via
#    gossip.  If cassandra-topology.properties exists, it is used as a
#    fallback, allowing migration from the PropertyFileSnitch.
#  - PropertyFileSnitch:
#    Proximity is determined by rack and data center, which are
#    explicitly configured in cassandra-topology.properties.
#  - Ec2Snitch:
#    Appropriate for EC2 deployments in a single Region. Loads Region
#    and Availability Zone information from the EC2 API. The Region is
#    treated as the datacenter, and the Availability Zone as the rack.
#    Only private IPs are used, so this will not work across multiple
#    Regions.
#  - Ec2MultiRegionSnitch:
#    Uses public IPs as broadcast_address to allow cross-region
#    connectivity.  (Thus, you should set seed addresses to the public
#    IP as well.) You will need to open the storage_port or
#    ssl_storage_port on the public IP firewall.  (For intra-Region
#    traffic, Cassandra will switch to the private IP after
#    establishing a connection.)
#  - RackInferringSnitch:
#    Proximity is determined by rack and data center, which are
#    assumed to correspond to the 3rd and 2nd octet of each node's IP
#    address, respectively.  Unless this happens to match your
#    deployment conventions, this is best used as an example of
#    writing a custom Snitch class and is provided in that spirit.
#
# You can use a custom Snitch by setting this to the full class name
# of the snitch, which will be assumed to be on your classpath.
endpoint_snitch: SimpleSnitch

# controls how often to perform the more expensive part of host score
# calculation
dynamic_snitch_update_interval_in_ms: 100 
# controls how often to reset all host scores, allowing a bad host to
# possibly recover
dynamic_snitch_reset_interval_in_ms: 600000
# if set greater than zero and read_repair_chance is < 1.0, this will allow
# 'pinning' of replicas to hosts in order to increase cache capacity.
# The badness threshold will control how much worse the pinned host has to be
# before the dynamic snitch will prefer other replicas over it.  This is
# expressed as a double which represents a percentage.  Thus, a value of
# 0.2 means Cassandra would continue to prefer the static snitch values
# until the pinned host was 20% worse than the fastest.
dynamic_snitch_badness_threshold: 0.1

# request_scheduler -- Set this to a class that implements
# RequestScheduler, which will schedule incoming client requests
# according to the specific policy. This is useful for multi-tenancy
# with a single Cassandra cluster.
# NOTE: This is specifically for requests from the client and does
# not affect inter node communication.
# org.apache.cassandra.scheduler.NoScheduler - No scheduling takes place
# org.apache.cassandra.scheduler.RoundRobinScheduler - Round robin of
# client requests to a node with a separate queue for each
# request_scheduler_id. The scheduler is further customized by
# request_scheduler_options as described below.
request_scheduler: org.apache.cassandra.scheduler.NoScheduler

# Scheduler Options vary based on the type of scheduler
# NoScheduler - Has no options
# RoundRobin
#  - throttle_limit -- The throttle_limit is the number of in-flight
#                      requests per client.  Requests beyond 
#                      that limit are queued up until
#                      running requests can complete.
#                      The value of 80 here is twice the number of
#                      concurrent_reads + concurrent_writes.
#  - default_weight -- default_weight is optional and allows for
#                      overriding the default which is 1.
#  - weights -- Weights are optional and will default to 1 or the
#               overridden default_weight. The weight translates into how
#               many requests are handled during each turn of the
#               RoundRobin, based on the scheduler id.
#
# request_scheduler_options:
#    throttle_limit: 80
#    default_weight: 5
#    weights:
#      Keyspace1: 1
#      Keyspace2: 5

# request_scheduler_id -- An identifier based on which to perform
# the request scheduling. Currently the only valid option is keyspace.
# request_scheduler_id: keyspace

# Enable or disable inter-node encryption
# Default settings are TLS v1, RSA 1024-bit keys (it is imperative that
# users generate their own keys) TLS_RSA_WITH_AES_128_CBC_SHA as the cipher
# suite for authentication, key exchange and encryption of the actual data transfers.
# Use the DHE/ECDHE ciphers if running in FIPS 140 compliant mode.
# NOTE: No custom encryption options are enabled at the moment
# The available internode options are : all, none, dc, rack
#
# If set to dc cassandra will encrypt the traffic between the DCs
# If set to rack cassandra will encrypt the traffic between the racks
#
# The passwords used in these options must match the passwords used when generating
# the keystore and truststore.  For instructions on generating these files, see:
# http://download.oracle.com/javase/6/docs/technotes/guides/security/jsse/JSSERefGuide.html#CreateKeystore
#
server_encryption_options:
    internode_encryption: none
    keystore: conf/.keystore
    keystore_password: cassandra
    truststore: conf/.truststore
    truststore_password: cassandra
    # More advanced defaults below:
    # protocol: TLS
    # algorithm: SunX509
    # store_type: JKS
    # cipher_suites: [TLS_RSA_WITH_AES_128_CBC_SHA,TLS_RSA_WITH_AES_256_CBC_SHA,TLS_DHE_RSA_WITH_AES_128_CBC_SHA,TLS_DHE_RSA_WITH_AES_256_CBC_SHA,TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA]
    # require_client_auth: false

# enable or disable client/server encryption.
client_encryption_options:
    enabled: false
    keystore: conf/.keystore
    keystore_password: cassandra
    # require_client_auth: false
    # Set trustore and truststore_password if require_client_auth is true
    # truststore: conf/.truststore
    # truststore_password: cassandra
    # More advanced defaults below:
    # protocol: TLS
    # algorithm: SunX509
    # store_type: JKS
    # cipher_suites: [TLS_RSA_WITH_AES_128_CBC_SHA,TLS_RSA_WITH_AES_256_CBC_SHA,TLS_DHE_RSA_WITH_AES_128_CBC_SHA,TLS_DHE_RSA_WITH_AES_256_CBC_SHA,TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA]

# internode_compression controls whether traffic between nodes is
# compressed.
# can be:  all  - all traffic is compressed
#          dc   - traffic between different datacenters is compressed
#          none - nothing is compressed.
internode_compression: all

# Enable or disable tcp_nodelay for inter-dc communication.
# Disabling it will result in larger (but fewer) network packets being sent,
# reducing overhead from the TCP protocol itself, at the cost of increasing
# latency if you block for cross-datacenter responses.
inter_dc_tcp_nodelay: false
    
)apa";

namespace utils {
template<typename... Args>
inline std::basic_ostream<Args...> & operator<<(std::basic_ostream<Args...> & os, const utils::config_file::config_source & v) {
    typedef std::underlying_type<utils::config_file::config_source>::type type;
    return os << type(v);
}
}

namespace db {
template<typename... T, typename... Args>
inline std::basic_ostream<Args...> & operator<<(std::basic_ostream<Args...> & os, const db::config::named_value<T...> & v) {
    return os << v();
}
}

namespace {

void throw_on_error(const sstring& opt, const sstring& msg, std::optional<utils::config_file::value_status> status) {
    if (status != config::value_status::Invalid) {
        throw std::invalid_argument(msg + " : " + opt);
    }
}

} // anonymous namespace

SEASTAR_TEST_CASE(test_parse_yaml) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;

    cfg.read_from_yaml(cassandra_conf, throw_on_error);

    BOOST_CHECK_EQUAL(cfg.cluster_name(), "Test Cluster");
    BOOST_CHECK_EQUAL(cfg.cluster_name.is_set(), true);
    BOOST_CHECK_EQUAL(cfg.cluster_name.source(), utils::config_file::config_source::SettingsFile);


    BOOST_CHECK_EQUAL(cfg.compaction_throughput_mb_per_sec(), 16);
    BOOST_CHECK_EQUAL(cfg.compaction_throughput_mb_per_sec.is_set(), true);
    BOOST_CHECK_EQUAL(cfg.compaction_throughput_mb_per_sec.source(), utils::config_file::config_source::SettingsFile);

    /*
     * Horrible, unused, maybe invalid. Let's test it.
     *
    seed_provider:
        # Addresses of hosts that are deemed contact points.
        # Cassandra nodes use this list of hosts to find each other and learn
        # the topology of the ring.  You must change this if you are running
        # multiple nodes!
        - class_name: org.apache.cassandra.locator.SimpleSeedProvider
          parameters:
              # seeds is actually a comma-delimited list of addresses.
              # Ex: "<ip1>,<ip2>,<ip3>"
              - seeds: "127.0.0.1"
    */
    BOOST_CHECK_EQUAL(cfg.seed_provider.is_set(), true);
    BOOST_CHECK_EQUAL(cfg.seed_provider.source(), utils::config_file::config_source::SettingsFile);
    BOOST_CHECK_EQUAL(cfg.seed_provider().class_name, "org.apache.cassandra.locator.SimpleSeedProvider");

    const auto expected_seed_provider_params = std::unordered_map<sstring, sstring>({{"seeds", "127.0.0.1"}});
    BOOST_CHECK_EQUAL(cfg.seed_provider().parameters, expected_seed_provider_params);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_parse_broken) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;

    bool ok = false;

    // this should become an "unknown option" kind of error.
    cfg.read_from_yaml(R"foo(bork_bnork:
    apa
    ko
)foo", [&](auto& opt, auto& msg, auto status) {
        if (!status) { // unknown
            ok = true;
        }
    });

    BOOST_REQUIRE(ok);

    ok = false;

    // this should be a value parsing error.
    cfg.read_from_yaml(R"foo(commitlog_segment_size_in_mb: flaska
)foo", [&](auto& opt, auto& msg, auto status) {
        if (status && status != config::value_status::Invalid) { // option exists and is valid.
            ok = true;
        }
    });

    BOOST_REQUIRE(ok);

    return make_ready_future<>();
}

using ef = experimental_features_t;
using features = std::vector<enum_option<ef>>;

SEASTAR_TEST_CASE(test_parse_experimental_features_cdc) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental_features:\n    - cdc\n", throw_on_error);
    BOOST_CHECK_EQUAL(cfg.experimental_features(), features{ef::UNUSED_CDC});
    BOOST_CHECK(cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(!cfg.check_experimental(ef::UDF));
    BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(!cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_features_unused) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental_features:\n    - lwt\n", throw_on_error);
    BOOST_CHECK_EQUAL(cfg.experimental_features(), features{ef::UNUSED});
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(!cfg.check_experimental(ef::UDF));
    BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(!cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_features_udf) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental_features:\n    - udf\n", throw_on_error);
    BOOST_CHECK_EQUAL(cfg.experimental_features(), features{ef::UDF});
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(cfg.check_experimental(ef::UDF));
    BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(!cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_features_alternator_streams) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental_features:\n    - alternator-streams\n", throw_on_error);
    BOOST_CHECK_EQUAL(cfg.experimental_features(), features{ef::ALTERNATOR_STREAMS});
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(!cfg.check_experimental(ef::UDF));
    BOOST_CHECK(cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(!cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_features_raft) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental_features:\n    - raft\n", throw_on_error);
    BOOST_CHECK_EQUAL(cfg.experimental_features(), features{ef::RAFT});
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(!cfg.check_experimental(ef::UDF));
    BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_features_multiple) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental_features:\n    - cdc\n    - lwt\n    - cdc\n", throw_on_error);
    BOOST_CHECK_EQUAL(cfg.experimental_features(), (features{ef::UNUSED_CDC, ef::UNUSED, ef::UNUSED_CDC}));
    BOOST_CHECK(cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(!cfg.check_experimental(ef::UDF));
    BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_features_invalid) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    using value_status = utils::config_file::value_status;
    cfg.read_from_yaml("experimental_features:\n    - invalidoptiontvaluedonotuse\n",
                       [&cfg] (const sstring& opt, const sstring& msg, std::optional<value_status> status) {
                           BOOST_REQUIRE_EQUAL(opt, "experimental_features");
                           BOOST_REQUIRE_NE(msg.find("line 2, column 7"), msg.npos);
                           BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
                           BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
                           BOOST_CHECK(!cfg.check_experimental(ef::UDF));
                           BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
                       });
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_true) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental: true", throw_on_error);
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(cfg.check_experimental(ef::UDF));
    BOOST_CHECK(cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(!cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_parse_experimental_false) {
    auto cfg_ptr = std::make_unique<config>();
    config& cfg = *cfg_ptr;
    cfg.read_from_yaml("experimental: false", throw_on_error);
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED_CDC));
    BOOST_CHECK(!cfg.check_experimental(ef::UNUSED));
    BOOST_CHECK(!cfg.check_experimental(ef::UDF));
    BOOST_CHECK(!cfg.check_experimental(ef::ALTERNATOR_STREAMS));
    BOOST_CHECK(!cfg.check_experimental(ef::RAFT));
    return make_ready_future();
}
