/*
 * Copyright (C) 2016-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

/*
 */

#include <vector>
#include <chrono>
#include <seastar/core/shared_ptr.hh>
#include "sstables/sstables.hh"
#include "compaction.hh"
#include "compaction_strategy.hh"
#include "compaction_strategy_impl.hh"
#include "compaction_strategy_state.hh"
#include "schema/schema.hh"
#include "sstables/sstable_set.hh"
#include <boost/range/algorithm/find.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>
#include "size_tiered_compaction_strategy.hh"
#include "leveled_compaction_strategy.hh"
#include "time_window_compaction_strategy.hh"
#include "backlog_controller.hh"
#include "compaction_backlog_manager.hh"
#include "size_tiered_backlog_tracker.hh"
#include "leveled_manifest.hh"

logging::logger leveled_manifest::logger("LeveledManifest");

namespace sstables {

compaction_descriptor compaction_strategy_impl::make_major_compaction_job(std::vector<sstables::shared_sstable> candidates, int level, uint64_t max_sstable_bytes) {
    // run major compaction in maintenance priority
    return compaction_descriptor(std::move(candidates), level, max_sstable_bytes);
}

std::vector<compaction_descriptor> compaction_strategy_impl::get_cleanup_compaction_jobs(table_state& table_s, std::vector<shared_sstable> candidates) const {
    // The default implementation is suboptimal and causes the writeamp problem described issue in #10097.
    // The compaction strategy relying on it should strive to implement its own method, to make cleanup bucket aware.
    return boost::copy_range<std::vector<compaction_descriptor>>(candidates | boost::adaptors::transformed([] (const shared_sstable& sst) {
        return compaction_descriptor({ sst },
            sst->get_sstable_level(), sstables::compaction_descriptor::default_max_sstable_bytes, sst->run_identifier());
    }));
}

bool compaction_strategy_impl::worth_dropping_tombstones(const shared_sstable& sst, gc_clock::time_point compaction_time, const tombstone_gc_state& gc_state) {
    if (_disable_tombstone_compaction) {
        return false;
    }
    // ignore sstables that were created just recently because there's a chance
    // that expired tombstones still cover old data and thus cannot be removed.
    // We want to avoid a compaction loop here on the same data by considering
    // only old enough sstables.
    if (db_clock::now()-_tombstone_compaction_interval < sst->data_file_write_time()) {
        return false;
    }
    auto gc_before = sst->get_gc_before_for_drop_estimation(compaction_time, gc_state);
    return sst->estimate_droppable_tombstone_ratio(gc_before) >= _tombstone_threshold;
}

uint64_t compaction_strategy_impl::adjust_partition_estimate(const mutation_source_metadata& ms_meta, uint64_t partition_estimate) const {
    return partition_estimate;
}

reader_consumer_v2 compaction_strategy_impl::make_interposer_consumer(const mutation_source_metadata& ms_meta, reader_consumer_v2 end_consumer) const {
    return end_consumer;
}

compaction_descriptor
compaction_strategy_impl::get_reshaping_job(std::vector<shared_sstable> input, schema_ptr schema, reshape_mode mode) const {
    return compaction_descriptor();
}

std::optional<sstring> compaction_strategy_impl::get_value(const std::map<sstring, sstring>& options, const sstring& name) {
    auto it = options.find(name);
    if (it == options.end()) {
        return std::nullopt;
    }
    return it->second;
}

compaction_strategy_impl::compaction_strategy_impl(const std::map<sstring, sstring>& options) {
    using namespace cql3::statements;

    auto tmp_value = get_value(options, TOMBSTONE_THRESHOLD_OPTION);
    _tombstone_threshold = property_definitions::to_double(TOMBSTONE_THRESHOLD_OPTION, tmp_value, DEFAULT_TOMBSTONE_THRESHOLD);

    tmp_value = get_value(options, TOMBSTONE_COMPACTION_INTERVAL_OPTION);
    auto interval = property_definitions::to_long(TOMBSTONE_COMPACTION_INTERVAL_OPTION, tmp_value, DEFAULT_TOMBSTONE_COMPACTION_INTERVAL().count());
    _tombstone_compaction_interval = db_clock::duration(std::chrono::seconds(interval));

    // FIXME: validate options.
}

} // namespace sstables

size_tiered_backlog_tracker::inflight_component
size_tiered_backlog_tracker::compacted_backlog(const compaction_backlog_tracker::ongoing_compactions& ongoing_compactions) const {
    inflight_component in;
    for (auto const& crp : ongoing_compactions) {
        // A SSTable being compacted may not contribute to backlog if compaction strategy decided
        // to perform a low-efficiency compaction when system is under little load, or when user
        // performs major even though strategy is completely satisfied
        if (!_sstables_contributing_backlog.contains(crp.first)) {
            continue;
        }
        auto compacted = crp.second->compacted();
        in.total_bytes += compacted;
        in.contribution += compacted * log4(crp.first->data_size());
    }
    return in;
}

void size_tiered_backlog_tracker::refresh_sstables_backlog_contribution() {
    _sstables_backlog_contribution = 0.0f;
    _sstables_contributing_backlog = {};
    if (_all.empty()) {
        return;
    }
    using namespace sstables;

    // Deduce threshold from the last SSTable added to the set
    // Low-efficiency jobs, which fan-in is smaller than min-threshold, will not have backlog accounted.
    // That's because they can only run when system is under little load, and accounting them would result
    // in efficient jobs acting more aggressive than they really have to.
    // TODO: potentially switch to compaction manager's fan-in threshold, so to account for the dynamic
    //  fan-in threshold behavior.
    const auto& newest_sst = std::ranges::max(_all, std::less<generation_type>(), std::mem_fn(&sstable::generation));
    auto threshold = newest_sst->get_schema()->min_compaction_threshold();

    for (auto& bucket : size_tiered_compaction_strategy::get_buckets(boost::copy_range<std::vector<shared_sstable>>(_all), _stcs_options)) {
        if (!size_tiered_compaction_strategy::is_bucket_interesting(bucket, threshold)) {
            continue;
        }
        _sstables_backlog_contribution += boost::accumulate(bucket | boost::adaptors::transformed([this] (const shared_sstable& sst) -> double {
            return sst->data_size() * log4(sst->data_size());
        }), double(0.0f));
        // Controller is disabled if exception is caught during add / remove calls, so not making any effort to make this exception safe
        _sstables_contributing_backlog.insert(bucket.begin(), bucket.end());
    }
}

double size_tiered_backlog_tracker::backlog(const compaction_backlog_tracker::ongoing_writes& ow, const compaction_backlog_tracker::ongoing_compactions& oc) const {
    inflight_component compacted = compacted_backlog(oc);

    auto total_backlog_bytes = boost::accumulate(_sstables_contributing_backlog | boost::adaptors::transformed(std::mem_fn(&sstables::sstable::data_size)), uint64_t(0));

    // Bail out if effective backlog is zero, which happens in a small window where ongoing compaction exhausted
    // input files but is still sealing output files or doing managerial stuff like updating history table
    if (total_backlog_bytes <= compacted.total_bytes) {
        return 0;
    }

    // Formula for each SSTable is (Si - Ci) * log(T / Si)
    // Which can be rewritten as: ((Si - Ci) * log(T)) - ((Si - Ci) * log(Si))
    //
    // For the meaning of each variable, please refer to the doc in size_tiered_backlog_tracker.hh

    // Sum of (Si - Ci) for all SSTables contributing backlog
    auto effective_backlog_bytes = total_backlog_bytes - compacted.total_bytes;

    // Sum of (Si - Ci) * log (Si) for all SSTables contributing backlog
    auto sstables_contribution = _sstables_backlog_contribution - compacted.contribution;
    // This is subtracting ((Si - Ci) * log (Si)) from ((Si - Ci) * log(T)), yielding the final backlog
    auto b = (effective_backlog_bytes * log4(_total_bytes)) - sstables_contribution;
    return b > 0 ? b : 0;
}

void size_tiered_backlog_tracker::replace_sstables(std::vector<sstables::shared_sstable> old_ssts, std::vector<sstables::shared_sstable> new_ssts) {
    for (auto& sst : old_ssts) {
        if (sst->data_size() > 0) {
            _total_bytes -= sst->data_size();
            _all.erase(sst);
        }
    }
    for (auto& sst : new_ssts) {
        if (sst->data_size() > 0) {
            _total_bytes += sst->data_size();
            _all.insert(std::move(sst));
        }
    }
    refresh_sstables_backlog_contribution();
}

namespace sstables {

extern logging::logger clogger;

// The backlog for TWCS is just the sum of the individual backlogs in each time window.
// We'll keep various SizeTiered backlog tracker objects-- one per window for the static SSTables.
// We then scan the current compacting and in-progress writes and matching them to existing time
// windows.
//
// With the above we have everything we need to just calculate the backlogs individually and sum
// them. Just need to be careful that for the current in progress backlog we may have to create
// a new object for the partial write at this time.
class time_window_backlog_tracker final : public compaction_backlog_tracker::impl {
    time_window_compaction_strategy_options _twcs_options;
    size_tiered_compaction_strategy_options _stcs_options;
    std::unordered_map<api::timestamp_type, size_tiered_backlog_tracker> _windows;

    api::timestamp_type lower_bound_of(api::timestamp_type timestamp) const {
        timestamp_type ts = time_window_compaction_strategy::to_timestamp_type(_twcs_options.timestamp_resolution, timestamp);
        return time_window_compaction_strategy::get_window_lower_bound(_twcs_options.sstable_window_size, ts);
    }
public:
    time_window_backlog_tracker(time_window_compaction_strategy_options twcs_options, size_tiered_compaction_strategy_options stcs_options)
        : _twcs_options(twcs_options)
        , _stcs_options(stcs_options)
    {}

    virtual double backlog(const compaction_backlog_tracker::ongoing_writes& ow, const compaction_backlog_tracker::ongoing_compactions& oc) const override {
        std::unordered_map<api::timestamp_type, compaction_backlog_tracker::ongoing_writes> writes_per_window;
        std::unordered_map<api::timestamp_type, compaction_backlog_tracker::ongoing_compactions> compactions_per_window;
        double b = 0;

        for (auto& wp : ow) {
            auto bound = lower_bound_of(wp.second->maximum_timestamp());
            writes_per_window[bound].insert(wp);
        }

        for (auto& cp : oc) {
            auto bound = lower_bound_of(cp.first->get_stats_metadata().max_timestamp);
            compactions_per_window[bound].insert(cp);
        }

        auto no_ow = compaction_backlog_tracker::ongoing_writes();
        auto no_oc = compaction_backlog_tracker::ongoing_compactions();
        // Match the in-progress backlogs to existing windows. Compactions should always match an
        // existing windows. Writes in progress can fall into an non-existent window.
        for (auto& windows : _windows) {
            auto bound = windows.first;
            auto* ow_this_window = &no_ow;
            auto itw = writes_per_window.find(bound);
            if (itw != writes_per_window.end()) {
                ow_this_window = &itw->second;
            }
            auto* oc_this_window = &no_oc;
            auto itc = compactions_per_window.find(bound);
            if (itc != compactions_per_window.end()) {
                oc_this_window = &itc->second;
            }
            b += windows.second.backlog(*ow_this_window, *oc_this_window);
            if (itw != writes_per_window.end()) {
                // We will erase here so we can keep track of which
                // writes belong to existing windows. Writes that don't belong to any window
                // are writes in progress to new windows and will be accounted in the final
                // loop before we return
                writes_per_window.erase(itw);
            }
        }

        // Partial writes that don't belong to any window are accounted here.
        for (auto& current : writes_per_window) {
            b += size_tiered_backlog_tracker(_stcs_options).backlog(current.second, no_oc);
        }
        return b;
    }

    virtual void replace_sstables(std::vector<sstables::shared_sstable> old_ssts, std::vector<sstables::shared_sstable> new_ssts) override {
        struct replacement {
            std::vector<sstables::shared_sstable> old_ssts;
            std::vector<sstables::shared_sstable> new_ssts;
        };
        std::unordered_map<api::timestamp_type, replacement> per_window_replacement;

        for (auto& sst : new_ssts) {
            auto bound = lower_bound_of(sst->get_stats_metadata().max_timestamp);
            if (!_windows.contains(bound)) {
                _windows.emplace(bound, size_tiered_backlog_tracker(_stcs_options));
            }
            per_window_replacement[bound].new_ssts.push_back(std::move(sst));
        }
        for (auto& sst : old_ssts) {
            auto bound = lower_bound_of(sst->get_stats_metadata().max_timestamp);
            if (_windows.contains(bound)) {
                per_window_replacement[bound].old_ssts.push_back(std::move(sst));
            }
        }

        for (auto& [bound, r] : per_window_replacement) {
            // All windows must exist here, as windows are created for new files and will
            // remain alive as long as there's a single file in them
            auto& w = _windows.at(bound);
            w.replace_sstables(std::move(r.old_ssts), std::move(r.new_ssts));
            if (w.total_bytes() <= 0) {
                _windows.erase(bound);
            }
        }
    }
};

class leveled_compaction_backlog_tracker final : public compaction_backlog_tracker::impl {
    // Because we can do SCTS in L0, we will account for that in the backlog.
    // Whatever backlog we accumulate here will be added to the main backlog.
    size_tiered_backlog_tracker _l0_scts;
    std::vector<uint64_t> _size_per_level;
    uint64_t _max_sstable_size;
public:
    leveled_compaction_backlog_tracker(int32_t max_sstable_size_in_mb, size_tiered_compaction_strategy_options stcs_options)
        : _l0_scts(stcs_options)
        , _size_per_level(leveled_manifest::MAX_LEVELS, uint64_t(0))
        , _max_sstable_size(max_sstable_size_in_mb * 1024 * 1024)
    {}

    virtual double backlog(const compaction_backlog_tracker::ongoing_writes& ow, const compaction_backlog_tracker::ongoing_compactions& oc) const override {
        std::vector<uint64_t> effective_size_per_level = _size_per_level;
        compaction_backlog_tracker::ongoing_writes l0_partial_writes;
        compaction_backlog_tracker::ongoing_compactions l0_compacted;

        for (auto& op : ow) {
            auto level = op.second->level();
            if (level == 0) {
                l0_partial_writes.insert(op);
            }
            effective_size_per_level[level] += op.second->written();
        }

        for (auto& cp : oc) {
            auto level = cp.first->get_sstable_level();
            if (level == 0) {
                l0_compacted.insert(cp);
            }
            effective_size_per_level[level] -= cp.second->compacted();
        }

        double b = _l0_scts.backlog(l0_partial_writes, l0_compacted);

        size_t max_populated_level = [&effective_size_per_level] () -> size_t {
            auto it = std::find_if(effective_size_per_level.rbegin(), effective_size_per_level.rend(), [] (uint64_t s) {
                return s != 0;
            });
            if (it == effective_size_per_level.rend()) {
                return 0;
            }
            return std::distance(it, effective_size_per_level.rend()) - 1;
        }();

        // The LCS goal is to achieve a layout where for every level L, sizeof(L+1) >= (sizeof(L) * fan_out)
        // If table size is S, which is the sum of size of all levels, the target size of the highest level
        // is S % 1.111, where 1.111 refers to strategy's space amplification goal.
        // As level L is fan_out times smaller than L+1, level L-1 is fan_out^2 times smaller than L+1,
        // and so on, the target size of any level can be easily calculated.

        static constexpr auto fan_out = leveled_manifest::leveled_fan_out;
        static constexpr double space_amplification_goal = 1.111;
        uint64_t total_size = std::accumulate(effective_size_per_level.begin(), effective_size_per_level.end(), uint64_t(0));
        uint64_t target_max_level_size = std::ceil(total_size / space_amplification_goal);

        auto target_level_size = [&] (size_t level) {
            auto r = std::ceil(target_max_level_size / std::pow(fan_out, max_populated_level - level));
            return std::max(uint64_t(r), _max_sstable_size);
        };

        // The backlog for a level L is the amount of bytes to be compacted, such that:
        // sizeof(L) <= sizeof(L+1) * fan_out
        // If we start from L0, then L0 backlog is (sizeof(L0) - target_sizeof(L0)) * fan_out, where
        // (sizeof(L0) - target_sizeof(L0)) is the amount of data to be promoted into next level
        // By summing the backlog for each level, we get the total amount of work for all levels to
        // reach their target size.
        for (size_t level = 0; level < max_populated_level; ++level) {
            auto lsize = effective_size_per_level[level];
            auto target_lsize = target_level_size(level);

            // Current level satisfies the goal, skip to the next one.
            if (lsize <= target_lsize) {
                continue;
            }
            auto next_level = level + 1;
            auto bytes_for_next_level =  lsize - target_lsize;

            // The fan_out is usually 10. But if the level above us is not fully populated -- which
            // can happen when a level is still being born, we don't want that to jump abruptly.
            // So what we will do instead is to define the fan out as the minimum between 10
            // and the number of sstables that are estimated to be there.
            unsigned estimated_next_level_ssts = (effective_size_per_level[next_level] + _max_sstable_size - 1) / _max_sstable_size;
            auto estimated_fan_out = std::min(fan_out, estimated_next_level_ssts);

            b += bytes_for_next_level * estimated_fan_out;

            // Update size of next level, as data from current level can be promoted as many times
            // as needed, and therefore needs to be included in backlog calculation for the next
            // level, if needed.
            effective_size_per_level[next_level] += bytes_for_next_level;
        }
        return b;
    }

    virtual void replace_sstables(std::vector<sstables::shared_sstable> old_ssts, std::vector<sstables::shared_sstable> new_ssts) override {
        std::vector<sstables::shared_sstable> l0_old_ssts, l0_new_ssts;
        for (auto& sst : new_ssts) {
            auto level = sst->get_sstable_level();
            _size_per_level[level] += sst->data_size();
            if (level == 0) {
                l0_new_ssts.push_back(std::move(sst));
            }
        }
        for (auto& sst : old_ssts) {
            auto level = sst->get_sstable_level();
            _size_per_level[level] -= sst->data_size();
            if (level == 0) {
                l0_old_ssts.push_back(std::move(sst));
            }
        }
        if (l0_old_ssts.size() || l0_new_ssts.size()) {
            _l0_scts.replace_sstables(std::move(l0_old_ssts), std::move(l0_new_ssts));
        }
    }
};

struct unimplemented_backlog_tracker final : public compaction_backlog_tracker::impl {
    virtual double backlog(const compaction_backlog_tracker::ongoing_writes& ow, const compaction_backlog_tracker::ongoing_compactions& oc) const override {
        return compaction_controller::disable_backlog;
    }
    virtual void replace_sstables(std::vector<sstables::shared_sstable> old_ssts, std::vector<sstables::shared_sstable> new_ssts) override {}
};

struct null_backlog_tracker final : public compaction_backlog_tracker::impl {
    virtual double backlog(const compaction_backlog_tracker::ongoing_writes& ow, const compaction_backlog_tracker::ongoing_compactions& oc) const override {
        return 0;
    }
    virtual void replace_sstables(std::vector<sstables::shared_sstable> old_ssts, std::vector<sstables::shared_sstable> new_ssts) override {}
};

//
// Null compaction strategy is the default compaction strategy.
// As the name implies, it does nothing.
//
class null_compaction_strategy : public compaction_strategy_impl {
public:
    virtual compaction_descriptor get_sstables_for_compaction(table_state& table_s, strategy_control& control, std::vector<sstables::shared_sstable> candidates) override {
        return sstables::compaction_descriptor();
    }

    virtual int64_t estimated_pending_compactions(table_state& table_s) const override {
        return 0;
    }

    virtual compaction_strategy_type type() const override {
        return compaction_strategy_type::null;
    }

    virtual std::unique_ptr<compaction_backlog_tracker::impl> make_backlog_tracker() const override {
        return std::make_unique<null_backlog_tracker>();
    }
};

leveled_compaction_strategy::leveled_compaction_strategy(const std::map<sstring, sstring>& options)
        : compaction_strategy_impl(options)
        , _max_sstable_size_in_mb(calculate_max_sstable_size_in_mb(compaction_strategy_impl::get_value(options, SSTABLE_SIZE_OPTION)))
        , _stcs_options(options)
{
}

std::unique_ptr<compaction_backlog_tracker::impl> leveled_compaction_strategy::make_backlog_tracker() const {
    return std::make_unique<leveled_compaction_backlog_tracker>(_max_sstable_size_in_mb, _stcs_options);
}

int32_t
leveled_compaction_strategy::calculate_max_sstable_size_in_mb(std::optional<sstring> option_value) const {
    using namespace cql3::statements;
    auto max_size = property_definitions::to_int(SSTABLE_SIZE_OPTION, option_value, DEFAULT_MAX_SSTABLE_SIZE_IN_MB);

    if (max_size >= 1000) {
        leveled_manifest::logger.warn("Max sstable size of {}MB is configured; having a unit of compaction this large is probably a bad idea",
            max_size);
    } else if (max_size < 50) {
        leveled_manifest::logger.warn("Max sstable size of {}MB is configured. Testing done for CASSANDRA-5727 indicates that performance" \
            "improves up to 160MB", max_size);
    }
    return max_size;
}

time_window_compaction_strategy::time_window_compaction_strategy(const std::map<sstring, sstring>& options)
    : compaction_strategy_impl(options)
    , _options(options)
    , _stcs_options(options)
{
    if (!options.contains(TOMBSTONE_COMPACTION_INTERVAL_OPTION) && !options.contains(TOMBSTONE_THRESHOLD_OPTION)) {
        _disable_tombstone_compaction = true;
        clogger.debug("Disabling tombstone compactions for TWCS");
    } else {
        clogger.debug("Enabling tombstone compactions for TWCS");
    }
    _use_clustering_key_filter = true;
}

std::unique_ptr<compaction_backlog_tracker::impl> time_window_compaction_strategy::make_backlog_tracker() const {
    return std::make_unique<time_window_backlog_tracker>(_options, _stcs_options);
}

} // namespace sstables

namespace sstables {

size_tiered_compaction_strategy::size_tiered_compaction_strategy(const std::map<sstring, sstring>& options)
    : compaction_strategy_impl(options)
    , _options(options)
{}

size_tiered_compaction_strategy::size_tiered_compaction_strategy(const size_tiered_compaction_strategy_options& options)
    : _options(options)
{}

std::unique_ptr<compaction_backlog_tracker::impl> size_tiered_compaction_strategy::make_backlog_tracker() const {
    return std::make_unique<size_tiered_backlog_tracker>(_options);
}

compaction_strategy::compaction_strategy(::shared_ptr<compaction_strategy_impl> impl)
    : _compaction_strategy_impl(std::move(impl)) {}
compaction_strategy::compaction_strategy() = default;
compaction_strategy::~compaction_strategy() = default;
compaction_strategy::compaction_strategy(const compaction_strategy&) = default;
compaction_strategy::compaction_strategy(compaction_strategy&&) = default;
compaction_strategy& compaction_strategy::operator=(compaction_strategy&&) = default;

compaction_strategy_type compaction_strategy::type() const {
    return _compaction_strategy_impl->type();
}

compaction_descriptor compaction_strategy::get_sstables_for_compaction(table_state& table_s, strategy_control& control, std::vector<sstables::shared_sstable> candidates) {
    return _compaction_strategy_impl->get_sstables_for_compaction(table_s, control, std::move(candidates));
}

compaction_descriptor compaction_strategy::get_major_compaction_job(table_state& table_s, std::vector<sstables::shared_sstable> candidates) {
    return _compaction_strategy_impl->get_major_compaction_job(table_s, std::move(candidates));
}

std::vector<compaction_descriptor> compaction_strategy::get_cleanup_compaction_jobs(table_state& table_s, std::vector<shared_sstable> candidates) const {
    return _compaction_strategy_impl->get_cleanup_compaction_jobs(table_s, std::move(candidates));
}

void compaction_strategy::notify_completion(table_state& table_s, const std::vector<shared_sstable>& removed, const std::vector<shared_sstable>& added) {
    _compaction_strategy_impl->notify_completion(table_s, removed, added);
}

bool compaction_strategy::parallel_compaction() const {
    return _compaction_strategy_impl->parallel_compaction();
}

int64_t compaction_strategy::estimated_pending_compactions(table_state& table_s) const {
    return _compaction_strategy_impl->estimated_pending_compactions(table_s);
}

bool compaction_strategy::use_clustering_key_filter() const {
    return _compaction_strategy_impl->use_clustering_key_filter();
}

compaction_backlog_tracker compaction_strategy::make_backlog_tracker() const {
    return compaction_backlog_tracker(_compaction_strategy_impl->make_backlog_tracker());
}

sstables::compaction_descriptor
compaction_strategy::get_reshaping_job(std::vector<shared_sstable> input, schema_ptr schema, reshape_mode mode) const {
    return _compaction_strategy_impl->get_reshaping_job(std::move(input), schema, mode);
}

uint64_t compaction_strategy::adjust_partition_estimate(const mutation_source_metadata& ms_meta, uint64_t partition_estimate) const {
    return _compaction_strategy_impl->adjust_partition_estimate(ms_meta, partition_estimate);
}

reader_consumer_v2 compaction_strategy::make_interposer_consumer(const mutation_source_metadata& ms_meta, reader_consumer_v2 end_consumer) const {
    return _compaction_strategy_impl->make_interposer_consumer(ms_meta, std::move(end_consumer));
}

bool compaction_strategy::use_interposer_consumer() const {
    return _compaction_strategy_impl->use_interposer_consumer();
}

compaction_strategy make_compaction_strategy(compaction_strategy_type strategy, const std::map<sstring, sstring>& options) {
    ::shared_ptr<compaction_strategy_impl> impl;

    switch (strategy) {
    case compaction_strategy_type::null:
        impl = ::make_shared<null_compaction_strategy>();
        break;
    case compaction_strategy_type::size_tiered:
        impl = ::make_shared<size_tiered_compaction_strategy>(options);
        break;
    case compaction_strategy_type::leveled:
        impl = ::make_shared<leveled_compaction_strategy>(options);
        break;
    case compaction_strategy_type::time_window:
        impl = ::make_shared<time_window_compaction_strategy>(options);
        break;
    default:
        throw std::runtime_error("strategy not supported");
    }

    return compaction_strategy(std::move(impl));
}

}

namespace compaction {

compaction_strategy_state compaction_strategy_state::make(const compaction_strategy& cs) {
    switch (cs.type()) {
        case compaction_strategy_type::null:
        case compaction_strategy_type::size_tiered:
            return compaction_strategy_state(default_empty_state{});
        case compaction_strategy_type::leveled:
            return compaction_strategy_state(leveled_compaction_strategy_state{});
        case compaction_strategy_type::time_window:
            return compaction_strategy_state(time_window_compaction_strategy_state{});
        default:
            throw std::runtime_error("strategy not supported");
    }
}

}
