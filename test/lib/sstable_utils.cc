/*
 * Copyright (C) 2017-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "test/lib/sstable_utils.hh"

#include "replica/database.hh"
#include "replica/memtable-sstable.hh"
#include "dht/i_partitioner.hh"
#include "dht/murmur3_partitioner.hh"
#include <boost/range/irange.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include "sstables/version.hh"
#include "test/lib/flat_mutation_reader_assertions.hh"
#include "test/lib/reader_concurrency_semaphore.hh"
#include "test/boost/sstable_test.hh"
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/coroutine.hh>

using namespace sstables;
using namespace std::chrono_literals;

lw_shared_ptr<replica::memtable> make_memtable(schema_ptr s, const std::vector<mutation>& muts) {
    auto mt = make_lw_shared<replica::memtable>(s);

    std::size_t i{0};
    for (auto&& m : muts) {
        mt->apply(m);
        // Give the reactor some time to breathe
        if (++i == 10) {
            seastar::thread::yield();
            i = 0;
        }
    }

    return mt;
}

sstables::shared_sstable make_sstable_containing(std::function<sstables::shared_sstable()> sst_factory, lw_shared_ptr<replica::memtable> mt) {
    return make_sstable_containing(sst_factory(), std::move(mt));
}

sstables::shared_sstable make_sstable_containing(sstables::shared_sstable sst, lw_shared_ptr<replica::memtable> mt) {
    write_memtable_to_sstable_for_test(*mt, sst).get();
    sstable_open_config cfg { .load_first_and_last_position_metadata = true };
    sst->open_data(cfg).get();
    return sst;
}

sstables::shared_sstable make_sstable_containing(std::function<sstables::shared_sstable()> sst_factory, std::vector<mutation> muts) {
    return make_sstable_containing(sst_factory(), std::move(muts));
}

sstables::shared_sstable make_sstable_containing(sstables::shared_sstable sst, std::vector<mutation> muts) {
    tests::reader_concurrency_semaphore_wrapper semaphore;

    schema_ptr s = muts[0].schema();
    make_sstable_containing(sst, make_memtable(s, muts));

    std::set<mutation, mutation_decorated_key_less_comparator> merged;
    for (auto&& m : muts) {
        auto it = merged.find(m);
        if (it == merged.end()) {
            merged.insert(std::move(m));
        } else {
            auto old = merged.extract(it);
            old.value().apply(std::move(m));
            merged.insert(std::move(old));
        }
    }

    // validate the sstable
    auto rd = assert_that(sst->as_mutation_source().make_reader_v2(s, semaphore.make_permit()));
    for (auto&& m : merged) {
        rd.produces(m);
    }
    rd.produces_end_of_stream();

    return sst;
}

shared_sstable make_sstable(sstables::test_env& env, schema_ptr s, sstring dir, std::vector<mutation> mutations,
        sstable_writer_config cfg, sstables::sstable::version_types version, gc_clock::time_point query_time) {
    fs::path dir_path(dir);
    auto mt = make_memtable(s, mutations);
    auto sst = env.make_sstable(s, dir_path.string(), env.new_generation(), version, sstable_format_types::big, default_sstable_buffer_size, query_time);
    auto mr = mt->make_flat_reader(s, env.make_reader_permit());
    sst->write_components(std::move(mr), mutations.size(), s, cfg, mt->get_encoding_stats()).get();
    sst->load(s->get_sharder()).get();
    return sst;
}

shared_sstable make_sstable_easy(test_env& env, flat_mutation_reader_v2 rd, sstable_writer_config cfg,
        sstables::generation_type gen, const sstables::sstable::version_types version, int expected_partition, gc_clock::time_point query_time) {
    auto s = rd.schema();
    auto sst = env.make_sstable(s, gen, version, sstable_format_types::big, default_sstable_buffer_size, query_time);
    sst->write_components(std::move(rd), expected_partition, s, cfg, encoding_stats{}).get();
    sst->load(s->get_sharder()).get();
    return sst;
}

shared_sstable make_sstable_easy(test_env& env, lw_shared_ptr<replica::memtable> mt, sstable_writer_config cfg,
        sstables::generation_type gen, const sstable::version_types v, int estimated_partitions, gc_clock::time_point query_time) {
    return make_sstable_easy(env, mt->make_flat_reader(mt->schema(), env.make_reader_permit()), std::move(cfg), gen, v, estimated_partitions, query_time);
}

future<compaction_result> compact_sstables(compaction_manager& cm, sstables::compaction_descriptor descriptor, table_state& table_s, std::function<shared_sstable()> creator, compaction_sstable_replacer_fn replacer,
                                           can_purge_tombstones can_purge) {
    descriptor.creator = [creator = std::move(creator)] (shard_id dummy) mutable {
        return creator();
    };
    descriptor.replacer = std::move(replacer);
    if (can_purge) {
        descriptor.enable_garbage_collection(table_s.main_sstable_set());
    }
    auto cmt = compaction_manager_test(cm);
    sstables::compaction_result ret;
    co_await cmt.run(descriptor.run_identifier, table_s, [&] (sstables::compaction_data& cdata) {
        return sstables::compact_sstables(std::move(descriptor), cdata, table_s).then([&] (sstables::compaction_result res) {
            ret = std::move(res);
        });
    });
    co_return ret;
}

static sstring toc_filename(const sstring& dir, schema_ptr schema, sstables::generation_type generation, sstable_version_types v) {
    return sstable::filename(dir, schema->ks_name(), schema->cf_name(), v, generation,
                             sstable_format_types::big, component_type::TOC);
}

future<shared_sstable> test_env::reusable_sst(schema_ptr schema, sstring dir, sstables::generation_type generation) {
    for (auto v : boost::adaptors::reverse(all_sstable_versions)) {
        if (co_await file_exists(toc_filename(dir, schema, generation, v))) {
            co_return co_await reusable_sst(schema, dir, generation, v);
        }
    }
    throw sst_not_found(dir, generation);
}

compaction_manager_for_testing::wrapped_compaction_manager::wrapped_compaction_manager(bool enabled)
        : cm(tm, compaction_manager::for_testing_tag{})
{
    if (enabled) {
        cm.enable();
    }
}

// Must run in a seastar thread
compaction_manager_for_testing::wrapped_compaction_manager::~wrapped_compaction_manager() {
    if (!tm.abort_source().abort_requested()) {
        tm.abort_source().request_abort();
    }
    cm.stop().get();
}

class compaction_manager_test_task : public compaction::compaction_task_executor {
    sstables::run_id _run_id;
    noncopyable_function<future<> (sstables::compaction_data&)> _job;

public:
    compaction_manager_test_task(compaction_manager& cm, table_state& table_s, sstables::run_id run_id, noncopyable_function<future<> (sstables::compaction_data&)> job)
        : compaction::compaction_task_executor(cm, &table_s, sstables::compaction_type::Compaction, "Test compaction")
        , _run_id(run_id)
        , _job(std::move(job))
    { }

protected:
    virtual future<compaction_manager::compaction_stats_opt> do_run() override {
        setup_new_compaction(_run_id);
        return _job(_compaction_data).then([] {
            return make_ready_future<compaction_manager::compaction_stats_opt>(std::nullopt);
        });
    }

    friend class compaction_manager_test;
};

future<> compaction_manager_test::run(sstables::run_id output_run_id, table_state& table_s, noncopyable_function<future<> (sstables::compaction_data&)> job) {
    auto task = make_shared<compaction_manager_test_task>(_cm, table_s, output_run_id, std::move(job));
    gate::holder gate_holder = task->_compaction_state.gate.hold();
    auto& cdata = register_compaction(task);
    co_await task->run_compaction().discard_result().finally([this, &cdata] {
        deregister_compaction(cdata);
    });
}

sstables::compaction_data& compaction_manager_test::register_compaction(shared_ptr<compaction::compaction_task_executor> task) {
    testlog.debug("compaction_manager_test: register_compaction uuid={}: {}", task->compaction_data().compaction_uuid, *task);
    _cm._tasks.push_back(task);
    return task->compaction_data();
}

void compaction_manager_test::deregister_compaction(const sstables::compaction_data& c) {
    auto it = boost::find_if(_cm._tasks, [&c] (auto& task) { return task->compaction_data().compaction_uuid == c.compaction_uuid; });
    if (it != _cm._tasks.end()) {
        auto task = *it;
        testlog.debug("compaction_manager_test: deregister_compaction uuid={}: {}", c.compaction_uuid, *task);
        _cm._tasks.erase(it);
    } else {
        testlog.error("compaction_manager_test: deregister_compaction uuid={}: task not found", c.compaction_uuid);
    }
}

shared_sstable verify_mutation(test_env& env, shared_sstable sst, lw_shared_ptr<replica::memtable> mt, bytes key, std::function<void(mutation_opt&)> verify) {
    auto sstp = make_sstable_containing(std::move(sst), mt);
    return verify_mutation(env, std::move(sstp), std::move(key), std::move(verify));
}

shared_sstable verify_mutation(test_env& env, shared_sstable sstp, bytes key, std::function<void(mutation_opt&)> verify) {
    auto s = sstp->get_schema();
    auto pr = dht::partition_range::make_singular(make_dkey(s, key));
    auto rd = sstp->make_reader(s, env.make_reader_permit(), pr, s->full_slice());
    auto close_rd = deferred_close(rd);
    auto mopt = read_mutation_from_flat_mutation_reader(rd).get();
    verify(mopt);
    return sstp;
}

shared_sstable verify_mutation(test_env& env, shared_sstable sst, lw_shared_ptr<replica::memtable> mt, dht::partition_range pr, std::function<stop_iteration(mutation_opt&)> verify) {
    auto sstp = make_sstable_containing(std::move(sst), mt);
    return verify_mutation(env, std::move(sstp), std::move(pr), std::move(verify));
}

shared_sstable verify_mutation(test_env& env, shared_sstable sstp, dht::partition_range pr, std::function<stop_iteration(mutation_opt&)> verify) {
    auto s = sstp->get_schema();
    auto rd = sstp->make_reader(s, env.make_reader_permit(), std::move(pr), s->full_slice());
    auto close_rd = deferred_close(rd);
    while (auto mopt = read_mutation_from_flat_mutation_reader(rd).get()) {
        if (verify(mopt) == stop_iteration::yes) {
            break;
        }
    }
    return sstp;
}
