/*
 * Copyright (C) 2018-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <iterator>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/util/closeable.hh>
#include "distributed_loader.hh"
#include "replica/database.hh"
#include "replica/global_table_ptr.hh"
#include "db/config.hh"
#include "db/extensions.hh"
#include "db/system_keyspace.hh"
#include "db/system_distributed_keyspace.hh"
#include "db/schema_tables.hh"
#include "utils/lister.hh"
#include "compaction/compaction.hh"
#include "compaction/compaction_manager.hh"
#include "sstables/sstables.hh"
#include "sstables/sstables_manager.hh"
#include "sstables/sstable_directory.hh"
#include "auth/common.hh"
#include "tracing/trace_keyspace_helper.hh"
#include "db/view/view_update_checks.hh"
#include <unordered_map>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/min_element.hpp>
#include "db/view/view_update_generator.hh"
#include "utils/directories.hh"

extern logging::logger dblog;

static const std::unordered_set<std::string_view> system_keyspaces = {
                db::system_keyspace::NAME, db::schema_tables::NAME
};

// Not super nice. Adding statefulness to the file. 
static std::unordered_set<sstring> load_prio_keyspaces;
static bool population_started = false;

void replica::distributed_loader::mark_keyspace_as_load_prio(const sstring& ks) {
    assert(!population_started);
    load_prio_keyspaces.insert(ks);
}

bool is_system_keyspace(std::string_view name) {
    return system_keyspaces.contains(name);
}

bool is_load_prio_keyspace(std::string_view name) {
    return load_prio_keyspaces.contains(sstring(name));
}

static const std::unordered_set<std::string_view> internal_keyspaces = {
        db::system_distributed_keyspace::NAME,
        db::system_distributed_keyspace::NAME_EVERYWHERE,
        db::system_keyspace::NAME,
        db::schema_tables::NAME,
        auth::meta::AUTH_KS,
        tracing::trace_keyspace_helper::KEYSPACE_NAME
};

bool is_internal_keyspace(std::string_view name) {
    return internal_keyspaces.contains(name);
}

namespace replica {

static io_error_handler error_handler_for_upload_dir() {
    return [] (std::exception_ptr eptr) {
        // do nothing about sstable exception and caller will just rethrow it.
    };
}

io_error_handler error_handler_gen_for_upload_dir(disk_error_signal_type& dummy) {
    return error_handler_for_upload_dir();
}

future<>
distributed_loader::process_sstable_dir(sharded<sstables::sstable_directory>& dir, sstables::sstable_directory::process_flags flags) {
    co_await dir.invoke_on(0, [] (const sstables::sstable_directory& d) {
        return utils::directories::verify_owner_and_mode(d.sstable_dir());
    });

    if (flags.garbage_collect) {
        co_await dir.invoke_on(0, [] (sstables::sstable_directory& d) {
            return d.garbage_collect();
        });
    }

    co_await dir.invoke_on_all([&dir, flags] (sstables::sstable_directory& d) -> future<> {
        // Supposed to be called with the node either down or on behalf of maintenance tasks
        // like nodetool refresh
        co_await d.process_sstable_dir(flags);
        co_await d.move_foreign_sstables(dir);
    });

    co_await dir.invoke_on_all(&sstables::sstable_directory::commit_directory_changes);
}

future<>
distributed_loader::lock_table(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring cf_name) {
    return dir.invoke_on_all([&db, ks_name, cf_name] (sstables::sstable_directory& d) {
        auto& table = db.local().find_column_family(ks_name, cf_name);
        d.store_phaser(table.write_in_progress());
        return make_ready_future<>();
    });
}

// Helper structure for resharding.
//
// Describes the sstables (represented by their foreign_sstable_open_info) that are shared and
// need to be resharded. Each shard will keep one such descriptor, that contains the list of
// SSTables assigned to it, and their total size. The total size is used to make sure we are
// fairly balancing SSTables among shards.
struct reshard_shard_descriptor {
    sstables::sstable_directory::sstable_open_info_vector info_vec;
    uint64_t uncompressed_data_size = 0;

    bool total_size_smaller(const reshard_shard_descriptor& rhs) const {
        return uncompressed_data_size < rhs.uncompressed_data_size;
    }

    uint64_t size() const {
        return uncompressed_data_size;
    }
};

// Collects shared SSTables from all shards and sstables that require cleanup and returns a vector containing them all.
// This function assumes that the list of SSTables can be fairly big so it is careful to
// manipulate it in a do_for_each loop (which yields) instead of using standard accumulators.
future<sstables::sstable_directory::sstable_open_info_vector>
collect_all_shared_sstables(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring table_name, compaction::owned_ranges_ptr owned_ranges_ptr) {
    auto info_vec = sstables::sstable_directory::sstable_open_info_vector();

    // We want to make sure that each distributed object reshards about the same amount of data.
    // Each sharded object has its own shared SSTables. We can use a clever algorithm in which they
    // all distributely figure out which SSTables to exchange, but we'll keep it simple and move all
    // their foreign_sstable_open_info to a coordinator (the shard who called this function). We can
    // move in bulk and that's efficient. That shard can then distribute the work among all the
    // others who will reshard.
    auto coordinator = this_shard_id();
    // We will first move all of the foreign open info to temporary storage so that we can sort
    // them. We want to distribute bigger sstables first.
    const auto* sorted_owned_ranges_ptr = owned_ranges_ptr.get();
    co_await dir.invoke_on_all([&] (sstables::sstable_directory& d) -> future<> {
        auto shared_sstables = d.retrieve_shared_sstables();
        sstables::sstable_directory::sstable_open_info_vector need_cleanup;
        if (sorted_owned_ranges_ptr) {
            co_await d.filter_sstables([&] (sstables::shared_sstable sst) -> future<bool> {
                if (needs_cleanup(sst, *sorted_owned_ranges_ptr)) {
                    need_cleanup.push_back(co_await sst->get_open_info());
                    co_return false;
                }
                co_return true;
            });
        }
        if (shared_sstables.empty() && need_cleanup.empty()) {
            co_return;
        }
        co_await smp::submit_to(coordinator, [&] () -> future<> {
            info_vec.reserve(info_vec.size() + shared_sstables.size() + need_cleanup.size());
            for (auto& info : shared_sstables) {
                info_vec.emplace_back(std::move(info));
                co_await coroutine::maybe_yield();
            }
            for (auto& info : need_cleanup) {
                info_vec.emplace_back(std::move(info));
                co_await coroutine::maybe_yield();
            }
        });
    });

    co_return info_vec;
}

// Given a vector of shared sstables to be resharded, distribute it among all shards.
// The vector is first sorted to make sure that we are moving the biggest SSTables first.
//
// Returns a reshard_shard_descriptor per shard indicating the work that each shard has to do.
future<std::vector<reshard_shard_descriptor>>
distribute_reshard_jobs(sstables::sstable_directory::sstable_open_info_vector source) {
    auto destinations = std::vector<reshard_shard_descriptor>(smp::count);

    std::sort(source.begin(), source.end(), [] (const sstables::foreign_sstable_open_info& a, const sstables::foreign_sstable_open_info& b) {
        // Sort on descending SSTable sizes.
        return a.uncompressed_data_size > b.uncompressed_data_size;
    });

    for (auto& info : source) {
        // Choose the stable shard owner with the smallest amount of accumulated work.
        // Note that for sstables that need cleanup via resharding, owners may contain
        // a single shard.
        auto shard_it = boost::min_element(info.owners, [&] (const shard_id& lhs, const shard_id& rhs) {
            return destinations[lhs].total_size_smaller(destinations[rhs]);
        });
        auto& dest = destinations[*shard_it];
        dest.uncompressed_data_size += info.uncompressed_data_size;
        dest.info_vec.push_back(std::move(info));
        co_await coroutine::maybe_yield();
    }

    co_return destinations;
}

// reshards a collection of SSTables.
//
// A reference to the compaction manager must be passed so we can register with it. Knowing
// which table is being processed is a requirement of the compaction manager, so this must be
// passed too.
//
// We will reshard max_sstables_per_job at once.
//
// A creator function must be passed that will create an SSTable object in the correct shard,
// and an I/O priority must be specified.
future<> reshard(sstables::sstable_directory& dir, sstables::sstable_directory::sstable_open_info_vector shared_info, replica::table& table,
                           sstables::compaction_sstable_creator_fn creator, compaction::owned_ranges_ptr owned_ranges_ptr)
{
    // Resharding doesn't like empty sstable sets, so bail early. There is nothing
    // to reshard in this shard.
    if (shared_info.empty()) {
        co_return;
    }

    // We want to reshard many SSTables at a time for efficiency. However if we have too many we may
    // be risking OOM.
    auto max_sstables_per_job = table.schema()->max_compaction_threshold();
    auto num_jobs = (shared_info.size() + max_sstables_per_job - 1) / max_sstables_per_job;
    auto sstables_per_job = shared_info.size() / num_jobs;

    std::vector<std::vector<sstables::shared_sstable>> buckets;
    buckets.reserve(num_jobs);
    buckets.emplace_back();
    co_await coroutine::parallel_for_each(shared_info, [&] (sstables::foreign_sstable_open_info& info) -> future<> {
        auto sst = co_await dir.load_foreign_sstable(info);
        // Last bucket gets leftover SSTables
        if ((buckets.back().size() >= sstables_per_job) && (buckets.size() < num_jobs)) {
            buckets.emplace_back();
        }
        buckets.back().push_back(std::move(sst));
    });
    // There is a semaphore inside the compaction manager in run_resharding_jobs. So we
    // parallel_for_each so the statistics about pending jobs are updated to reflect all
    // jobs. But only one will run in parallel at a time
    auto& t = table.as_table_state();
    co_await coroutine::parallel_for_each(buckets, [&] (std::vector<sstables::shared_sstable>& sstlist) mutable {
        return table.get_compaction_manager().run_custom_job(table.as_table_state(), sstables::compaction_type::Reshard, "Reshard compaction", [&] (sstables::compaction_data& info) -> future<> {
            auto erm = table.get_effective_replication_map(); // keep alive around compaction.

            sstables::compaction_descriptor desc(sstlist);
            desc.options = sstables::compaction_type_options::make_reshard();
            desc.creator = creator;
            desc.sharder = &erm->get_sharder(*table.schema());
            desc.owned_ranges = owned_ranges_ptr;

            auto result = co_await sstables::compact_sstables(std::move(desc), info, t);
            // input sstables are moved, to guarantee their resources are released once we're done
            // resharding them.
            co_await when_all_succeed(dir.collect_output_unshared_sstables(std::move(result.new_sstables), sstables::sstable_directory::can_be_remote::yes), dir.remove_sstables(std::move(sstlist))).discard_result();
        });
    });
}

future<> run_resharding_jobs(sharded<sstables::sstable_directory>& dir, std::vector<reshard_shard_descriptor> reshard_jobs,
                             sharded<replica::database>& db, sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator,
                             compaction::owned_ranges_ptr owned_ranges_ptr) {

    uint64_t total_size = boost::accumulate(reshard_jobs | boost::adaptors::transformed(std::mem_fn(&reshard_shard_descriptor::size)), uint64_t(0));
    if (total_size == 0) {
        co_return;
    }

    auto start = std::chrono::steady_clock::now();
    dblog.info("Resharding {} for {}.{}", utils::pretty_printed_data_size(total_size), ks_name, table_name);

    co_await dir.invoke_on_all(coroutine::lambda([&] (sstables::sstable_directory& d) -> future<> {
        auto& table = db.local().find_column_family(ks_name, table_name);
        auto info_vec = std::move(reshard_jobs[this_shard_id()].info_vec);
        // make shard-local copy of owned_ranges
        compaction::owned_ranges_ptr local_owned_ranges_ptr;
        if (owned_ranges_ptr) {
            local_owned_ranges_ptr = make_lw_shared<const dht::token_range_vector>(*owned_ranges_ptr);
        }
        co_await ::replica::reshard(d, std::move(info_vec), table, creator, std::move(local_owned_ranges_ptr));
        co_await d.move_foreign_sstables(dir);
    }));

    auto duration = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - start);
    dblog.info("Resharded {} for {}.{} in {:.2f} seconds, {}", utils::pretty_printed_data_size(total_size), ks_name, table_name, duration.count(), utils::pretty_printed_throughput(total_size, duration));
}

// Global resharding function. Done in two parts:
//  - The first part spreads the foreign_sstable_open_info across shards so that all of them are
//    resharding about the same amount of data
//  - The second part calls each shard's distributed object to reshard the SSTables they were
//    assigned.
future<>
distributed_loader::reshard(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator, compaction::owned_ranges_ptr owned_ranges_ptr) {
    auto all_jobs = co_await collect_all_shared_sstables(dir, db, ks_name, table_name, owned_ranges_ptr);
    auto destinations = co_await distribute_reshard_jobs(std::move(all_jobs));
    co_await run_resharding_jobs(dir, std::move(destinations), db, ks_name, table_name, std::move(creator), std::move(owned_ranges_ptr));
}

future<sstables::sstable::version_types>
highest_version_seen(sharded<sstables::sstable_directory>& dir, sstables::sstable_version_types system_version) {
    using version = sstables::sstable_version_types;
    return dir.map_reduce0(std::mem_fn(&sstables::sstable_directory::highest_version_seen), system_version, [] (version a, version b) {
        return std::max(a, b);
    });
}

future<>
distributed_loader::reshape(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstables::reshape_mode mode,
        sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator,
        std::function<bool (const sstables::shared_sstable&)> filter) {
    auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
    auto task = co_await compaction_module.make_and_start_task<table_reshaping_compaction_task_impl>({}, std::move(ks_name), std::move(table_name), dir, db, mode, std::move(creator), std::move(filter));
    co_await task->done();
}

// Loads SSTables into the main directory (or staging) and returns how many were loaded
future<size_t>
distributed_loader::make_sstables_available(sstables::sstable_directory& dir, sharded<replica::database>& db,
        sharded<db::view::view_update_generator>& view_update_generator, bool needs_view_update, sstring ks, sstring cf) {

    auto& table = db.local().find_column_family(ks, cf);
    auto new_sstables = std::vector<sstables::shared_sstable>();

    co_await dir.do_for_each_sstable([&table, needs_view_update, &new_sstables] (sstables::shared_sstable sst) -> future<> {
        auto gen = table.calculate_generation_for_new_table();
        dblog.trace("Loading {} into {}, new generation {}", sst->get_filename(), needs_view_update ? "staging" : "base", gen);
        co_await sst->pick_up_from_upload(!needs_view_update ? sstables::normal_dir : sstables::staging_dir, gen);
            // When loading an imported sst, set level to 0 because it may overlap with existing ssts on higher levels.
            sst->set_sstable_level(0);
            new_sstables.push_back(std::move(sst));
    });

    // nothing loaded
    if (new_sstables.empty()) {
        co_return 0;
    }

    co_await table.add_sstables_and_update_cache(new_sstables).handle_exception([&table] (std::exception_ptr ep) {
        dblog.error("Failed to load SSTables for {}.{}: {}. Aborting.", table.schema()->ks_name(), table.schema()->cf_name(), ep);
        abort();
    });

    co_await coroutine::parallel_for_each(new_sstables, [&view_update_generator, &table] (sstables::shared_sstable sst) -> future<> {
        if (sst->requires_view_building()) {
            co_await view_update_generator.local().register_staging_sstable(sst, table.shared_from_this());
        }
    });

    co_return new_sstables.size();
}

future<>
distributed_loader::process_upload_dir(distributed<replica::database>& db, distributed<db::system_distributed_keyspace>& sys_dist_ks,
        distributed<db::view::view_update_generator>& view_update_generator, sstring ks, sstring cf) {
    seastar::thread_attributes attr;
    attr.sched_group = db.local().get_streaming_scheduling_group();

    return seastar::async(std::move(attr), [&db, &view_update_generator, &sys_dist_ks, ks = std::move(ks), cf = std::move(cf)] {
        auto global_table = get_table_on_all_shards(db, ks, cf).get0();

        sharded<locator::effective_replication_map_ptr> erms;
        erms.start(sharded_parameter([&global_table] {
            return global_table->get_effective_replication_map();
        })).get();
        auto stop_erms = deferred_stop(erms);

        sharded<sstables::sstable_directory> directory;
        auto upload = fs::path(global_table->dir()) / sstables::upload_dir;
        directory.start(
            sharded_parameter([&global_table] { return std::ref(global_table->get_sstables_manager()); }),
            sharded_parameter([&global_table] { return global_table->schema(); }),
            sharded_parameter([&global_table, &erms] { return std::ref(erms.local()->get_sharder(*global_table->schema())); }),
            sharded_parameter([&global_table] { return global_table->get_storage_options_ptr(); }),
            upload, &error_handler_gen_for_upload_dir
        ).get();

        auto stop_directory = deferred_stop(directory);

        lock_table(directory, db, ks, cf).get();
        sstables::sstable_directory::process_flags flags {
            .need_mutate_level = true,
            .enable_dangerous_direct_import_of_cassandra_counters = db.local().get_config().enable_dangerous_direct_import_of_cassandra_counters(),
            .allow_loading_materialized_view = false,
        };
        process_sstable_dir(directory, flags).get();

        sharded<sstables::sstable_generation_generator> sharded_gen;
        auto highest_generation = highest_generation_seen(directory).get0();
        sharded_gen.start(highest_generation ? highest_generation.as_int() : 0).get();
        auto stop_generator = deferred_stop(sharded_gen);

        auto make_sstable = [&] (shard_id shard) {
            auto& sstm = global_table->get_sstables_manager();
            bool uuid_sstable_identifiers = sstm.uuid_sstable_identifiers();
            auto generation = sharded_gen.invoke_on(shard, [uuid_sstable_identifiers] (auto& gen) {
                return gen(sstables::uuid_identifiers{uuid_sstable_identifiers});
            }).get();
            return sstm.make_sstable(global_table->schema(), global_table->get_storage_options(),
                                     upload.native(), generation, sstm.get_highest_supported_format(),
                                     sstables::sstable_format_types::big, gc_clock::now(), &error_handler_gen_for_upload_dir);
        };
        // Pass owned_ranges_ptr to reshard to piggy-back cleanup on the resharding compaction.
        // Note that needs_cleanup() is inaccurate and may return false positives,
        // maybe triggerring resharding+cleanup unnecessarily for some sstables.
        // But this is resharding on refresh (sstable loading via upload dir),
        // which will usually require resharding anyway.
        //
        // FIXME: take multiple compaction groups into account
        // - segregate resharded tables into compaction groups
        // - split the keyspace local ranges per compaction_group as done in table::perform_cleanup_compaction
        //   so that cleanup can be considered per compaction group
        auto owned_ranges_ptr = compaction::make_owned_ranges_ptr(db.local().get_keyspace_local_ranges(ks));
        reshard(directory, db, ks, cf, make_sstable, owned_ranges_ptr).get();
        reshape(directory, db, sstables::reshape_mode::strict, ks, cf, make_sstable,
                [] (const sstables::shared_sstable&) { return true; }).get();

        // Move to staging directory to avoid clashes with future uploads. Unique generation number ensures no collisions.
        const bool use_view_update_path = db::view::check_needs_view_update_path(sys_dist_ks.local(), db.local().get_token_metadata(), *global_table, streaming::stream_reason::repair).get0();

        size_t loaded = directory.map_reduce0([&db, ks, cf, use_view_update_path, &view_update_generator] (sstables::sstable_directory& dir) {
            return make_sstables_available(dir, db, view_update_generator, use_view_update_path, ks, cf);
        }, size_t(0), std::plus<size_t>()).get0();

        dblog.info("Loaded {} SSTables", loaded);
    });
}

future<std::tuple<table_id, std::vector<std::vector<sstables::shared_sstable>>>>
distributed_loader::get_sstables_from_upload_dir(distributed<replica::database>& db, sstring ks, sstring cf, sstables::sstable_open_config cfg) {
    return seastar::async([&db, ks = std::move(ks), cf = std::move(cf), cfg] {
        auto global_table = get_table_on_all_shards(db, ks, cf).get0();
        sharded<sstables::sstable_directory> directory;
        auto table_id = global_table->schema()->id();
        auto upload = fs::path(global_table->dir()) / sstables::upload_dir;

        sharded<locator::effective_replication_map_ptr> erms;
        erms.start(sharded_parameter([&global_table] {
            return global_table->get_effective_replication_map();
        })).get();
        auto stop_erms = deferred_stop(erms);

        directory.start(
            sharded_parameter([&global_table] { return std::ref(global_table->get_sstables_manager()); }),
            sharded_parameter([&global_table] { return global_table->schema(); }),
            sharded_parameter([&global_table, &erms] { return std::ref(erms.local()->get_sharder(*global_table->schema())); }),
            sharded_parameter([&global_table] { return global_table->get_storage_options_ptr(); }),
            upload, &error_handler_gen_for_upload_dir
        ).get();

        auto stop = deferred_stop(directory);

        std::vector<std::vector<sstables::shared_sstable>> sstables_on_shards(smp::count);
        lock_table(directory, db, ks, cf).get();
        sstables::sstable_directory::process_flags flags {
            .need_mutate_level = true,
            .enable_dangerous_direct_import_of_cassandra_counters = db.local().get_config().enable_dangerous_direct_import_of_cassandra_counters(),
            .allow_loading_materialized_view = false,
            .sort_sstables_according_to_owner = false,
            .sstable_open_config = cfg,
        };
        process_sstable_dir(directory, flags).get();
        directory.invoke_on_all([&sstables_on_shards] (sstables::sstable_directory& d) mutable {
            sstables_on_shards[this_shard_id()] = d.get_unsorted_sstables();
        }).get();

        return std::make_tuple(table_id, std::move(sstables_on_shards));
    });
}

class table_populator {
    distributed<replica::database>& _db;
    sstring _ks;
    sstring _cf;
    global_table_ptr _global_table;
    fs::path _base_path;
    std::unordered_map<sstring, lw_shared_ptr<sharded<sstables::sstable_directory>>> _sstable_directories;
    sstables::sstable_version_types _highest_version = sstables::oldest_writable_sstable_format;
    sstables::generation_type _highest_generation;
    sharded<locator::effective_replication_map_ptr> _erms;

public:
    table_populator(global_table_ptr ptr, distributed<replica::database>& db, sstring ks, sstring cf)
        : _db(db)
        , _ks(std::move(ks))
        , _cf(std::move(cf))
        , _global_table(std::move(ptr))
        , _base_path(_global_table->dir())
    {}

    ~table_populator() {
        // All directories must have been stopped
        // using table_populator::stop()
        assert(_sstable_directories.empty());
    }

    future<> start() {
        assert(this_shard_id() == 0);

        co_await _erms.start(sharded_parameter([this] {
            return _global_table->get_effective_replication_map();
        }));

        for (auto subdir : { "", sstables::staging_dir, sstables::quarantine_dir }) {
            co_await start_subdir(subdir);
        }

        co_await smp::invoke_on_all([this] {
            _global_table->update_sstables_known_generation(_highest_generation);
            return _global_table->disable_auto_compaction();
        });

        co_await populate_subdir(sstables::staging_dir, allow_offstrategy_compaction::no);
        co_await populate_subdir(sstables::quarantine_dir, allow_offstrategy_compaction::no, must_exist::no);
        co_await populate_subdir("", allow_offstrategy_compaction::yes);
    }

    future<> stop() {
        co_await _erms.stop();
        for (auto it = _sstable_directories.begin(); it != _sstable_directories.end(); it = _sstable_directories.erase(it)) {
            co_await it->second->stop();
        }
    }

private:
    fs::path get_path(std::string_view subdir) {
        return subdir.empty() ? _base_path : _base_path / subdir;
    }

    using allow_offstrategy_compaction = bool_class<struct allow_offstrategy_compaction_tag>;
    using must_exist = bool_class<struct must_exist_tag>;
    future<> populate_subdir(sstring subdir, allow_offstrategy_compaction, must_exist = must_exist::yes);

    future<> start_subdir(sstring subdir);
};

future<> table_populator::start_subdir(sstring subdir) {
    sstring sstdir = get_path(subdir).native();
    if (!co_await file_exists(sstdir)) {
        co_return;
    }

    auto dptr = make_lw_shared<sharded<sstables::sstable_directory>>();
    auto& directory = *dptr;
    auto& global_table = _global_table;
    auto& db = _db;
    co_await directory.start(
        sharded_parameter([&global_table] { return std::ref(global_table->get_sstables_manager()); }),
        sharded_parameter([&global_table] { return global_table->schema(); }),
        sharded_parameter([this] { return std::ref(_erms.local()->get_sharder(*_global_table->schema())); }),
        sharded_parameter([&global_table] { return global_table->get_storage_options_ptr(); }),
        fs::path(sstdir),
        default_io_error_handler_gen()
    );

    // directory must be stopped using table_populator::stop below
    _sstable_directories[subdir] = dptr;

    co_await distributed_loader::lock_table(directory, _db, _ks, _cf);

    sstables::sstable_directory::process_flags flags {
        .throw_on_missing_toc = true,
        .enable_dangerous_direct_import_of_cassandra_counters = db.local().get_config().enable_dangerous_direct_import_of_cassandra_counters(),
        .allow_loading_materialized_view = true,
        .garbage_collect = true,
    };
    co_await distributed_loader::process_sstable_dir(directory, flags);

    // If we are resharding system tables before we can read them, we will not
    // know which is the highest format we support: this information is itself stored
    // in the system tables. In that case we'll rely on what we find on disk: we'll
    // at least not downgrade any files. If we already know that we support a higher
    // format than the one we see then we use that.
    auto sys_format = global_table->get_sstables_manager().get_highest_supported_format();
    auto sst_version = co_await highest_version_seen(directory, sys_format);
    auto generation = co_await highest_generation_seen(directory);

    _highest_version = std::max(sst_version, _highest_version);
    _highest_generation = std::max(generation, _highest_generation);
}

sstables::shared_sstable make_sstable(replica::table& table, fs::path dir, sstables::generation_type generation, sstables::sstable_version_types v) {
    return table.get_sstables_manager().make_sstable(table.schema(), table.get_storage_options(), dir.native(), generation, v, sstables::sstable_format_types::big);
}

future<> table_populator::populate_subdir(sstring subdir, allow_offstrategy_compaction do_allow_offstrategy_compaction, must_exist dir_must_exist) {
    auto sstdir = get_path(subdir);
    dblog.debug("Populating {}/{}/{} allow_offstrategy_compaction={} must_exist={}", _ks, _cf, sstdir, do_allow_offstrategy_compaction, dir_must_exist);

    if (!_sstable_directories.contains(subdir)) {
        if (dir_must_exist) {
            throw std::runtime_error(format("Populating {}/{} failed: {} does not exist", _ks, _cf, sstdir));
        }
        co_return;
    }

    auto& directory = *_sstable_directories.at(subdir);

    co_await distributed_loader::reshard(directory, _db, _ks, _cf, [this, sstdir] (shard_id shard) mutable {
        auto gen = smp::submit_to(shard, [this] () {
            return _global_table->calculate_generation_for_new_table();
        }).get0();

        return make_sstable(*_global_table, sstdir, gen, _highest_version);
    });

    // The node is offline at this point so we are very lenient with what we consider
    // offstrategy.
    // SSTables created by repair may not conform to compaction strategy layout goal
    // because data segregation is only performed by compaction
    // Instead of reshaping them on boot, let's add them to maintenance set and allow
    // off-strategy compaction to reshape them. This will allow node to become online
    // ASAP. Given that SSTables with repair origin are disjoint, they can be efficiently
    // read from.
    auto eligible_for_reshape_on_boot = [] (const sstables::shared_sstable& sst) {
        return sst->get_origin() != sstables::repair_origin;
    };

    co_await distributed_loader::reshape(directory, _db, sstables::reshape_mode::relaxed, _ks, _cf, [this, sstdir] (shard_id shard) {
        auto gen = _global_table->calculate_generation_for_new_table();
        return make_sstable(*_global_table, sstdir, gen, _highest_version);
    }, eligible_for_reshape_on_boot);

    co_await directory.invoke_on_all([this, &eligible_for_reshape_on_boot, do_allow_offstrategy_compaction] (sstables::sstable_directory& dir) -> future<> {
        co_await dir.do_for_each_sstable([this, &eligible_for_reshape_on_boot, do_allow_offstrategy_compaction] (sstables::shared_sstable sst) {
            auto requires_offstrategy = sstables::offstrategy(do_allow_offstrategy_compaction && !eligible_for_reshape_on_boot(sst));
            return _global_table->add_sstable_and_update_cache(sst, requires_offstrategy);
        });
        if (do_allow_offstrategy_compaction) {
            _global_table->trigger_offstrategy_compaction();
        }
    });
}

future<> distributed_loader::populate_keyspace(distributed<replica::database>& db, sstring datadir, sstring ks_name) {
    auto ksdir = datadir + "/" + ks_name;
    auto& keyspaces = db.local().get_keyspaces();
    auto i = keyspaces.find(ks_name);
    if (i == keyspaces.end()) {
        dblog.warn("Skipping undefined keyspace: {}", ks_name);
        co_return;
    }

    dblog.info("Populating Keyspace {}", ks_name);
    auto& ks = i->second;
    auto& column_families = db.local().get_column_families();

    co_await coroutine::parallel_for_each(ks.metadata()->cf_meta_data() | boost::adaptors::map_values, [&] (schema_ptr s) -> future<> {
        auto uuid = s->id();
        lw_shared_ptr<replica::column_family> cf = column_families[uuid];

        // System tables (from system and system_schema keyspaces) are loaded in two phases.
        // The populate_keyspace function can be called in the second phase for tables that
        // were already populated in the first phase.
        // This check protects from double-populating them, since every populated cf
        // is marked as ready_for_writes.
        if (cf->is_ready_for_writes()) {
            co_return;
        }

        sstring cfname = cf->schema()->cf_name();
        dblog.info("Keyspace {}: Reading CF {} id={} version={} storage={}", ks_name, cfname, uuid, s->version(), cf->get_storage_options().type_string());

        auto gtable = co_await get_table_on_all_shards(db, ks_name, cfname);
        auto metadata = table_populator(std::move(gtable), db, ks_name, cfname);
        std::exception_ptr ex;

        try {
            co_await cf->init_storage();

            co_await metadata.start();
        } catch (...) {
            std::exception_ptr eptr = std::current_exception();
            std::string msg =
                format("Exception while populating keyspace '{}' with column family '{}' from file '{}': {}",
                        ks_name, cfname, cf->dir(), eptr);
            dblog.error("Exception while populating keyspace '{}' with column family '{}' from file '{}': {}",
                        ks_name, cfname, cf->dir(), eptr);
            try {
                std::rethrow_exception(eptr);
            } catch (sstables::compaction_stopped_exception& e) {
                // swallow compaction stopped exception, to allow clean shutdown.
            } catch (...) {
                ex = std::make_exception_ptr(std::runtime_error(msg.c_str()));
            }
        }

        co_await metadata.stop();
        if (ex) {
            co_await coroutine::return_exception_ptr(std::move(ex));
        }
    });
}

future<> distributed_loader::init_system_keyspace(sharded<db::system_keyspace>& sys_ks, distributed<locator::effective_replication_map_factory>& erm_factory, distributed<replica::database>& db, db::config& cfg, system_table_load_phase phase) {
    population_started = true;

    return seastar::async([&sys_ks, &erm_factory, &db, &cfg, phase] {
        sys_ks.invoke_on_all([&erm_factory, &db, &cfg, phase] (auto& sys_ks) {
            return sys_ks.make(erm_factory.local(), db.local(), cfg, phase);
        }).get();

        const auto& cfg = db.local().get_config();
        for (auto& data_dir : cfg.data_file_directories()) {
            for (auto ksname : system_keyspaces) {
                if (db.local().has_keyspace(ksname)) {
                    distributed_loader::populate_keyspace(db, data_dir, sstring(ksname)).get();
                }
            }
        }

        db.invoke_on_all([] (replica::database& db) {
            for (auto ksname : system_keyspaces) {
                if (!db.has_keyspace(ksname)) {
                    continue;
                }
                auto& ks = db.find_keyspace(ksname);
                for (auto& pair : ks.metadata()->cf_meta_data()) {
                    auto cfm = pair.second;
                    auto& cf = db.find_column_family(cfm);
                    // During phase2 some tables may have already been
                    // marked as 'ready for writes' at phase1.
                    if (!cf.is_ready_for_writes()) {
                        cf.mark_ready_for_writes();
                    }
                }
            }
            return make_ready_future<>();
        }).get();
    });
}

future<> distributed_loader::init_non_system_keyspaces(distributed<replica::database>& db,
        distributed<service::storage_proxy>& proxy, sharded<db::system_keyspace>& sys_ks) {
    return seastar::async([&db, &proxy, &sys_ks] {
        db.invoke_on_all([&proxy, &sys_ks] (replica::database& db) {
            return db.parse_system_tables(proxy, sys_ks);
        }).get();

        const auto& cfg = db.local().get_config();
        using ks_dirs = std::unordered_multimap<sstring, sstring>;

        ks_dirs dirs;

        parallel_for_each(cfg.data_file_directories(), [&dirs] (sstring directory) {
            // we want to collect the directories first, so we can get a full set of potential dirs
            return lister::scan_dir(directory, lister::dir_entry_types::of<directory_entry_type::directory>(), [&dirs] (fs::path datadir, directory_entry de) {
                if (!is_system_keyspace(de.name)) {
                    dirs.emplace(de.name, datadir.native());
                }
                return make_ready_future<>();
            });
        }).get();


        for (bool prio_only : { true, false}) {
            std::vector<future<>> futures;

            // treat "dirs" as immutable to avoid modifying it while still in
            // a range-iteration. Also to simplify the "finally"
            for (auto i = dirs.begin(); i != dirs.end();) {
                auto& ks_name = i->first;
                auto j = i++;

                /**
                 * Must process in two phases: Prio and non-prio.
                 * This looks like it is not needed. And it is not
                 * in open-source version. But essential for enterprise.
                 * Do _not_ remove or refactor away.
                 */
                if (prio_only != cfg.extensions().is_extension_internal_keyspace(ks_name)) {
                    continue;
                }

                auto e = dirs.equal_range(ks_name).second;
                // might have more than one dir for a keyspace iff data_file_directories is > 1 and
                // somehow someone placed sstables in more than one of them for a given ks. (import?)
                futures.emplace_back(parallel_for_each(j, e, [&](const std::pair<sstring, sstring>& p) {
                    auto& datadir = p.second;
                    return distributed_loader::populate_keyspace(db, datadir, ks_name);
                }));
            }

            when_all_succeed(futures.begin(), futures.end()).discard_result().get();
        }

        db.invoke_on_all([] (replica::database& db) {
            return parallel_for_each(db.get_non_system_column_families(), [] (lw_shared_ptr<replica::table> table) {
                // Make sure this is called even if the table is empty
                table->mark_ready_for_writes();
                return make_ready_future<>();
            });
        }).get();
    });
}

}
