/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compaction/task_manager_module.hh"
#include "compaction/compaction_manager.hh"
#include "replica/database.hh"
#include "sstables/sstables.hh"
#include "sstables/sstable_directory.hh"
#include "utils/pretty_printers.hh"

namespace compaction {

struct table_tasks_info {
    tasks::task_manager::task_ptr task;
    table_info ti;

    table_tasks_info(tasks::task_manager::task_ptr t, table_info info)
        : task(t)
        , ti(info)
    {}
};

future<> run_on_table(sstring op, replica::database& db, std::string keyspace, table_info ti, std::function<future<> (replica::table&)> func) {
    std::exception_ptr ex;
    tasks::tmlogger.debug("Starting {} on {}.{}", op, keyspace, ti.name);
    try {
        co_await func(db.find_column_family(ti.id));
    } catch (const replica::no_such_column_family& e) {
        tasks::tmlogger.warn("Skipping {} of {}.{}: {}", op, keyspace, ti.name, e.what());
    } catch (...) {
        ex = std::current_exception();
        tasks::tmlogger.error("Failed {} of {}.{}: {}", op, keyspace, ti.name, ex);
    }
    if (ex) {
        co_await coroutine::return_exception_ptr(std::move(ex));
    }
}

// Run on all tables, skipping dropped tables
future<> run_on_existing_tables(sstring op, replica::database& db, std::string keyspace, const std::vector<table_info> local_tables, std::function<future<> (replica::table&)> func) {
    for (const auto& ti : local_tables) {
        co_await run_on_table(op, db, keyspace, ti, func);
    }
}

future<> wait_for_your_turn(seastar::condition_variable& cv, tasks::task_manager::task_ptr& current_task, tasks::task_id id) {
    co_await cv.wait([&] {
        return current_task && current_task->id() == id;
    });
}

future<> run_table_tasks(replica::database& db, std::vector<table_tasks_info> table_tasks, seastar::condition_variable& cv, tasks::task_manager::task_ptr& current_task, bool sort) {
    std::exception_ptr ex;

    // While compaction is run on one table, the size of tables may significantly change.
    // Thus, they are sorted before each invidual compaction and the smallest table is chosen.
    while (!table_tasks.empty()) {
        try {
            if (sort) {
                // Major compact smaller tables first, to increase chances of success if low on space.
                // Tables will be kept in descending order.
                std::ranges::sort(table_tasks, std::greater<>(), [&] (const table_tasks_info& tti) {
                    try {
                        return db.find_column_family(tti.ti.id).get_stats().live_disk_space_used;
                    } catch (const replica::no_such_column_family& e) {
                        return int64_t(-1);
                    }
                });
            }
            // Task responsible for the smallest table.
            current_task = table_tasks.back().task;
            table_tasks.pop_back();
            cv.broadcast();
            co_await current_task->done();
        } catch (...) {
            ex = std::current_exception();
            current_task = nullptr;
            cv.broken(ex);
            break;
        }
    }

    if (ex) {
        // Wait for all tasks even on failure.
        for (auto& tti: table_tasks) {
            co_await tti.task->done();
        }
        co_await coroutine::return_exception_ptr(std::move(ex));
    }
}

future<> major_keyspace_compaction_task_impl::run() {
    co_await _db.invoke_on_all([&] (replica::database& db) -> future<> {
        tasks::task_info parent_info{_status.id, _status.shard};
        auto& module = db.get_compaction_manager().get_task_manager_module();
        auto task = co_await module.make_and_start_task<shard_major_keyspace_compaction_task_impl>(parent_info, _status.keyspace, _status.id, db, _table_infos);
        co_await task->done();
    });
}

tasks::is_internal shard_major_keyspace_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> shard_major_keyspace_compaction_task_impl::run() {
    seastar::condition_variable cv;
    tasks::task_manager::task_ptr current_task;
    tasks::task_info parent_info{_status.id, _status.shard};
    std::vector<table_tasks_info> table_tasks;
    for (auto& ti : _local_tables) {
        table_tasks.emplace_back(co_await _module->make_and_start_task<table_major_keyspace_compaction_task_impl>(parent_info, _status.keyspace, ti.name, _status.id, _db, ti, cv, current_task), ti);
    }

    co_await run_table_tasks(_db, std::move(table_tasks), cv, current_task, true);
}

tasks::is_internal table_major_keyspace_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> table_major_keyspace_compaction_task_impl::run() {
    co_await wait_for_your_turn(_cv, _current_task, _status.id);
    tasks::task_info info{_status.id, _status.shard};
    co_await run_on_table("force_keyspace_compaction", _db, _status.keyspace, _ti, [info] (replica::table& t) {
        return t.compact_all_sstables(info);
    });
}

future<> cleanup_keyspace_compaction_task_impl::run() {
    co_await _db.invoke_on_all([&] (replica::database& db) -> future<> {
        auto& module = db.get_compaction_manager().get_task_manager_module();
        auto task = co_await module.make_and_start_task<shard_cleanup_keyspace_compaction_task_impl>({_status.id, _status.shard}, _status.keyspace, _status.id, db, _table_infos);
        co_await task->done();
    });
}

tasks::is_internal shard_cleanup_keyspace_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> shard_cleanup_keyspace_compaction_task_impl::run() {
    seastar::condition_variable cv;
    tasks::task_manager::task_ptr current_task;
    tasks::task_info parent_info{_status.id, _status.shard};
    std::vector<table_tasks_info> table_tasks;
    for (auto& ti : _local_tables) {
        table_tasks.emplace_back(co_await _module->make_and_start_task<table_cleanup_keyspace_compaction_task_impl>(parent_info, _status.keyspace, ti.name, _status.id, _db, ti, cv, current_task), ti);
    }

    co_await run_table_tasks(_db, std::move(table_tasks), cv, current_task, true);
}

tasks::is_internal table_cleanup_keyspace_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> table_cleanup_keyspace_compaction_task_impl::run() {
    co_await wait_for_your_turn(_cv, _current_task, _status.id);
    auto owned_ranges_ptr = compaction::make_owned_ranges_ptr(_db.get_keyspace_local_ranges(_status.keyspace));
    co_await run_on_table("force_keyspace_cleanup", _db, _status.keyspace, _ti, [&] (replica::table& t) {
        return t.perform_cleanup_compaction(owned_ranges_ptr);
    });
}

future<> offstrategy_keyspace_compaction_task_impl::run() {
    _needed = co_await _db.map_reduce0([&] (replica::database& db) -> future<bool> {
        bool needed = false;
        tasks::task_info parent_info{_status.id, _status.shard};
        auto& module = db.get_compaction_manager().get_task_manager_module();
        auto task = co_await module.make_and_start_task<shard_offstrategy_keyspace_compaction_task_impl>(parent_info, _status.keyspace, _status.id, db, _table_infos, needed);
        co_await task->done();
        co_return needed;
    }, false, std::plus<bool>());
}

tasks::is_internal shard_offstrategy_keyspace_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> shard_offstrategy_keyspace_compaction_task_impl::run() {
    seastar::condition_variable cv;
    tasks::task_manager::task_ptr current_task;
    tasks::task_info parent_info{_status.id, _status.shard};
    std::vector<table_tasks_info> table_tasks;
    for (auto& ti : _table_infos) {
        table_tasks.emplace_back(co_await _module->make_and_start_task<table_offstrategy_keyspace_compaction_task_impl>(parent_info, _status.keyspace, ti.name, _status.id, _db, ti, cv, current_task, _needed), ti);
    }

    co_await run_table_tasks(_db, std::move(table_tasks), cv, current_task, false);
}

tasks::is_internal table_offstrategy_keyspace_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> table_offstrategy_keyspace_compaction_task_impl::run() {
    co_await wait_for_your_turn(_cv, _current_task, _status.id);
    co_await run_on_table("perform_keyspace_offstrategy_compaction", _db, _status.keyspace, _ti, [this] (replica::table& t) -> future<> {
        _needed |= co_await t.perform_offstrategy_compaction();
    });
}

future<> upgrade_sstables_compaction_task_impl::run() {
    co_await _db.invoke_on_all([&] (replica::database& db) -> future<> {
        tasks::task_info parent_info{_status.id, _status.shard};
        auto& compaction_module = db.get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<shard_upgrade_sstables_compaction_task_impl>(parent_info, _status.keyspace, _status.id, db, _table_infos, _exclude_current_version);
        co_await task->done();
    });
}

tasks::is_internal shard_upgrade_sstables_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> shard_upgrade_sstables_compaction_task_impl::run() {
    seastar::condition_variable cv;
    tasks::task_manager::task_ptr current_task;
    tasks::task_info parent_info{_status.id, _status.shard};
    std::vector<table_tasks_info> table_tasks;
    for (auto& ti : _table_infos) {
        table_tasks.emplace_back(co_await _module->make_and_start_task<table_upgrade_sstables_compaction_task_impl>(parent_info, _status.keyspace, ti.name, _status.id, _db, ti, cv, current_task, _exclude_current_version), ti);
    }

    co_await run_table_tasks(_db, std::move(table_tasks), cv, current_task, false);
}

tasks::is_internal table_upgrade_sstables_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> table_upgrade_sstables_compaction_task_impl::run() {
    co_await wait_for_your_turn(_cv, _current_task, _status.id);
    auto owned_ranges_ptr = compaction::make_owned_ranges_ptr(_db.get_keyspace_local_ranges(_status.keyspace));
    co_await run_on_table("upgrade_sstables", _db, _status.keyspace, _ti, [&] (replica::table& t) -> future<> {
        return t.parallel_foreach_table_state([&] (compaction::table_state& ts) -> future<> {
            return t.get_compaction_manager().perform_sstable_upgrade(owned_ranges_ptr, ts, _exclude_current_version);
        });
    });
}

future<> scrub_sstables_compaction_task_impl::run() {
    _stats = co_await _db.map_reduce0([&] (replica::database& db) -> future<sstables::compaction_stats> {
        sstables::compaction_stats stats;
        tasks::task_info parent_info{_status.id, _status.shard};
        auto& compaction_module = db.get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<shard_scrub_sstables_compaction_task_impl>(parent_info, _status.keyspace, _status.id, db, _column_families, _opts, stats);
        co_await task->done();
        co_return stats;
    }, sstables::compaction_stats{}, std::plus<sstables::compaction_stats>());
}

tasks::is_internal shard_scrub_sstables_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> shard_scrub_sstables_compaction_task_impl::run() {
    _stats = co_await map_reduce(_column_families, [&] (sstring cfname) -> future<sstables::compaction_stats> {
        sstables::compaction_stats stats{};
        tasks::task_info parent_info{_status.id, _status.shard};
        auto& compaction_module = _db.get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<table_scrub_sstables_compaction_task_impl>(parent_info, _status.keyspace, cfname, _status.id, _db, _opts, stats);
        co_await task->done();
        co_return stats;
    }, sstables::compaction_stats{}, std::plus<sstables::compaction_stats>());
}

tasks::is_internal table_scrub_sstables_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> table_scrub_sstables_compaction_task_impl::run() {
    auto& cm = _db.get_compaction_manager();
    auto& cf = _db.find_column_family(_status.keyspace, _status.table);
    co_await cf.parallel_foreach_table_state([&] (compaction::table_state& ts) mutable -> future<> {
        auto r = co_await cm.perform_sstable_scrub(ts, _opts);
        _stats += r.value_or(sstables::compaction_stats{});
    });
}

future<> table_reshaping_compaction_task_impl::run() {
    auto start = std::chrono::steady_clock::now();
    auto total_size = co_await _dir.map_reduce0([&] (sstables::sstable_directory& d) -> future<uint64_t> {
        uint64_t total_shard_size;
        tasks::task_info parent_info{_status.id, _status.shard};
        auto& compaction_module = _db.local().get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<shard_reshaping_compaction_task_impl>(parent_info, _status.keyspace, _status.table, _status.id, d, _db, _mode, _creator, _filter, total_shard_size);
        co_await task->done();
        co_return total_shard_size;
    }, uint64_t(0), std::plus<uint64_t>());

    if (total_size > 0) {
        auto duration = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - start);
        dblog.info("Reshaped {} in {:.2f} seconds, {}", utils::pretty_printed_data_size(total_size), duration.count(), utils::pretty_printed_throughput(total_size, duration));
    }
}

tasks::is_internal shard_reshaping_compaction_task_impl::is_internal() const noexcept {
    return tasks::is_internal::yes;
}

future<> shard_reshaping_compaction_task_impl::run() {
    auto& table = _db.local().find_column_family(_status.keyspace, _status.table);
    uint64_t reshaped_size = 0;

    while (true) {
        auto reshape_candidates = boost::copy_range<std::vector<sstables::shared_sstable>>(_dir.get_unshared_local_sstables()
                | boost::adaptors::filtered([&filter = _filter] (const auto& sst) {
            return filter(sst);
        }));
        auto desc = table.get_compaction_strategy().get_reshaping_job(std::move(reshape_candidates), table.schema(), _mode);
        if (desc.sstables.empty()) {
            break;
        }

        if (!reshaped_size) {
            dblog.info("Table {}.{} with compaction strategy {} found SSTables that need reshape. Starting reshape process", table.schema()->ks_name(), table.schema()->cf_name(), table.get_compaction_strategy().name());
        }

        std::vector<sstables::shared_sstable> sstlist;
        for (auto& sst : desc.sstables) {
            reshaped_size += sst->data_size();
            sstlist.push_back(sst);
        }

        desc.creator = _creator;

        std::exception_ptr ex;
        try {
            co_await table.get_compaction_manager().run_custom_job(table.as_table_state(), sstables::compaction_type::Reshape, "Reshape compaction", [&dir = _dir, &table, sstlist = std::move(sstlist), desc = std::move(desc)] (sstables::compaction_data& info) mutable -> future<> {
                sstables::compaction_result result = co_await sstables::compact_sstables(std::move(desc), info, table.as_table_state());
                co_await dir.remove_unshared_sstables(std::move(sstlist));
                co_await dir.collect_output_unshared_sstables(std::move(result.new_sstables), sstables::sstable_directory::can_be_remote::no);
            });
        } catch (...) {
            ex = std::current_exception();
        }

        if (ex != nullptr) {
              try {
                std::rethrow_exception(std::move(ex));
              } catch (sstables::compaction_stopped_exception& e) {
                  dblog.info("Table {}.{} with compaction strategy {} had reshape successfully aborted.", table.schema()->ks_name(), table.schema()->cf_name(), table.get_compaction_strategy().name());
                  break;
              } catch (...) {
                  dblog.info("Reshape failed for Table {}.{} with compaction strategy {} due to {}", table.schema()->ks_name(), table.schema()->cf_name(), table.get_compaction_strategy().name(), std::current_exception());
                  break;
              }
        }

        co_await coroutine::maybe_yield();
    }

    _total_shard_size = reshaped_size;
}

}
