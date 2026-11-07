// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Midi-bench comparing phmap::flat_hash_map<int16_t, AggDataPtr> vs
// SmallFixedSizeHashMap<int16_t, AggDataPtr> (a direct 65 537-cell array
// indexed by static_cast<uint16_t>(key)) as the backing of the SMALLINT
// GROUP BY wrapper AggHashMapWithOneNumberKey<TYPE_SMALLINT, ...>.
//
// Driver: production callsite build_hash_map with a production-shaped
// HashTableKeyAllocator (sizeof(int64_t) state matching COUNT(*)).  No FE,
// no scan I/O, no pipeline operator overhead.  Working-set sized so the
// Int16 column (~200 MiB at 100M rows) does not fit in L3.
//
// What is timed in each section:
//   A) build_hash_map only.  Wrapper construction is paused; the array
//      path's 524 KiB upfront zero-fill is NOT in the timed number here.
//      This isolates steady-state hash-insert cost.
//   B) Both BuildOnly and BuildWithCtor companions on a rows sweep.
//      BuildWithCtor includes wrapper construction inside the timed
//      region, so the per-iteration timing carries the 524 KiB upfront
//      cost paid at every Aggregator instantiation.  Comparing the two
//      sections shows the row count beyond which construction stops
//      dominating -- the construction-vs-amortization break-even.
//   C) Construction-only cost (regression guard, microsecond unit).
//   D) Nullable wrapper at null fractions {0%, 10%, 50%}.
//
// density=1 is included as a boundary case (single distinct value after
// partition pruning); it is the array path's best case but NOT the
// headline density we expect upstream readers to focus on.  Mid-density
// (24 hour-of-day, 366 day-of-year, 1000 region-code) is where production
// SMALLINT GROUP BY queries cluster.

#include <benchmark/benchmark.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "base/phmap/phmap.h"
#include "base/utility/defer_op.h"
#include "column/column_helper.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "common/config_exec_flow_fwd.h"
#include "common/config_exec_fwd.h"
#include "common/runtime_profile.h"
#include "exec/aggregate/agg_hash_map.h"
#include "exec/aggregate/agg_profile.h"
#include "exec/aggregator.h"
#include "runtime/mem_pool.h"
#include "runtime/runtime_state.h"
#include "types/logical_type.h"

namespace starrocks {

inline constexpr int kBenchChunkSize = 4096;

// PhmapInt16 mirrors the original phmap<int16_t, AggDataPtr> using-declaration
// verbatim so we measure the actual baseline, not a default-constructed phmap
// with different hash/seed parameters.
template <PhmapSeed seed>
using PhmapInt16 = phmap::flat_hash_map<int16_t, AggDataPtr, StdHashWithSeed<int16_t, seed>>;

using PhmapInt16Wrapper = AggHashMapWithOneNumberKey<TYPE_SMALLINT, PhmapInt16<PhmapSeed1>>;
using ArrayInt16Wrapper =
        AggHashMapWithOneNumberKey<TYPE_SMALLINT, SmallFixedSizeHashMap<int16_t, AggDataPtr, PhmapSeed1>>;

using PhmapInt16NullableWrapper = AggHashMapWithOneNullableNumberKey<TYPE_SMALLINT, PhmapInt16<PhmapSeed1>>;
using ArrayInt16NullableWrapper =
        AggHashMapWithOneNullableNumberKey<TYPE_SMALLINT, SmallFixedSizeHashMap<int16_t, AggDataPtr, PhmapSeed1>>;

enum class Distribution : int {
    Random = 0,
    Sorted = 1,
    Clustered64 = 2, // run-length 64: each value repeats 64 times before the next
    Zipf = 3,        // skewed: ~90% of rows hit ~10% of values (s=1.5)
};

// Production-shaped allocator.  Aggregator uses HashTableKeyAllocator with
// aggregate_key_size set to the COUNT aggregate state size (sizeof(int64_t));
// matching this layout keeps allocation cost realistic across the two
// hash-map backings.
struct BenchAllocateState {
    HashTableKeyAllocator* allocator;
    AggDataPtr operator()(std::nullptr_t) { return allocator->allocate_null_key_data(); }
    template <typename K>
    AggDataPtr operator()(K /*key*/) {
        return allocator->allocate();
    }
};

// Build (num_rows / kBenchChunkSize) Int16 chunks of kBenchChunkSize rows
// each.  Each chunk is a fresh Int16Column owning its own buffer, so the
// hot loop walks across distinct cache lines and the prefetcher behavior
// matches a streaming-scan scenario rather than an L1-resident replay.
class Int16ChunkStream {
public:
    Int16ChunkStream(int64_t num_rows, int distinct_count, Distribution dist) {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<int> uni(0, distinct_count > 0 ? distinct_count - 1 : 0);
        const int64_t num_chunks = (num_rows + kBenchChunkSize - 1) / kBenchChunkSize;
        _chunks.reserve(num_chunks);
        const int range_start = -distinct_count / 2; // straddle signed range to exercise unsigned cast

        // Pre-build a Zipf code table once if needed.
        std::vector<int> zipf_codes;
        if (dist == Distribution::Zipf) {
            zipf_codes = build_zipf_codes(distinct_count);
        }
        std::uniform_int_distribution<int> zipf_pick(0, zipf_codes.empty() ? 0 : zipf_codes.size() - 1);

        for (int64_t c = 0; c < num_chunks; ++c) {
            auto col = Int16Column::create();
            auto& data = col->get_data();
            data.resize(kBenchChunkSize);
            switch (dist) {
            case Distribution::Random:
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    data[i] = static_cast<int16_t>(range_start + uni(rng));
                }
                break;
            case Distribution::Sorted: {
                std::vector<int> vals(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) vals[i] = uni(rng);
                std::sort(vals.begin(), vals.end());
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    data[i] = static_cast<int16_t>(range_start + vals[i]);
                }
                break;
            }
            case Distribution::Clustered64: {
                int current = uni(rng);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    if (i > 0 && i % 64 == 0) current = uni(rng);
                    data[i] = static_cast<int16_t>(range_start + current);
                }
                break;
            }
            case Distribution::Zipf: {
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    data[i] = static_cast<int16_t>(range_start + zipf_codes[zipf_pick(rng)]);
                }
                break;
            }
            }
            _chunks.emplace_back(std::move(col));
        }
    }

    const std::vector<ColumnPtr>& chunks() const { return _chunks; }

private:
    // Build a 4096-entry sample table over [0, distinct_count) with
    // Zipf(s=1.5) frequencies; sample uniformly from this table to emit
    // skewed codes.  Top ~10% of distinct values get ~80-90% of mass for
    // typical OLAP dimension columns (status, country, segment).
    static std::vector<int> build_zipf_codes(int distinct_count) {
        constexpr int table_size = 4096;
        constexpr double s = 1.5;
        if (distinct_count <= 0) return {};
        // Cumulative weights w_k = 1 / k^s.
        std::vector<double> cum(distinct_count);
        double total = 0.0;
        for (int k = 0; k < distinct_count; ++k) {
            total += 1.0 / std::pow(k + 1, s);
            cum[k] = total;
        }
        std::vector<int> codes(table_size);
        std::mt19937 rng(0xBEEFCAFE);
        std::uniform_real_distribution<double> uni(0.0, total);
        for (int i = 0; i < table_size; ++i) {
            double r = uni(rng);
            int lo = 0, hi = distinct_count - 1;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (cum[mid] < r)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            codes[i] = lo;
        }
        return codes;
    }

    std::vector<ColumnPtr> _chunks;
};

// Same as Int16ChunkStream but each chunk is wrapped in a NullableColumn.
// null_fraction = 0 produces nullable-column-with-has_null=false (the
// AggHashMapWithOneNullableNumberKey fast path that delegates to the
// non-nullable code), 0.1 / 0.5 produce realistic mixed-null inputs that
// route through compute_agg_through_null_data.
class NullableInt16ChunkStream {
public:
    NullableInt16ChunkStream(int64_t num_rows, int distinct_count, double null_fraction) {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<int> uni(0, distinct_count > 0 ? distinct_count - 1 : 0);
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        const int64_t num_chunks = (num_rows + kBenchChunkSize - 1) / kBenchChunkSize;
        _chunks.reserve(num_chunks);
        const int range_start = -distinct_count / 2;
        for (int64_t c = 0; c < num_chunks; ++c) {
            auto data_col = Int16Column::create();
            auto null_col = NullColumn::create();
            auto& data = data_col->get_data();
            auto& nulls = null_col->get_data();
            data.resize(kBenchChunkSize);
            nulls.resize(kBenchChunkSize);
            bool any_null = false;
            for (int i = 0; i < kBenchChunkSize; ++i) {
                if (null_fraction > 0 && coin(rng) < null_fraction) {
                    nulls[i] = 1;
                    data[i] = 0;
                    any_null = true;
                } else {
                    nulls[i] = 0;
                    data[i] = static_cast<int16_t>(range_start + uni(rng));
                }
            }
            auto nullable = NullableColumn::create(std::move(data_col), std::move(null_col));
            nullable->set_has_null(any_null);
            _chunks.emplace_back(std::move(nullable));
        }
    }

    const std::vector<ColumnPtr>& chunks() const { return _chunks; }

private:
    std::vector<ColumnPtr> _chunks;
};

class BenchSuite {
public:
    void SetUp() {
        CpuInfo::init();
        config::vector_chunk_size = kBenchChunkSize;
        TUniqueId fragment_id;
        TQueryOptions query_options;
        query_options.batch_size = kBenchChunkSize;
        TQueryGlobals query_globals;
        _runtime_state = std::make_shared<RuntimeState>(fragment_id, query_options, query_globals, nullptr);
        _runtime_state->init_instance_mem_tracker();
        _mem_pool = std::make_unique<MemPool>();
        _runtime_profile = std::make_unique<RuntimeProfile>("agg_hash_map_bench");
        _agg_stat = std::make_unique<AggStatistics>(_runtime_profile.get());
    }

    void TearDown() {
        _agg_stat.reset();
        _runtime_profile.reset();
        _mem_pool.reset();
        _runtime_state.reset();
    }

    std::shared_ptr<RuntimeState> _runtime_state;
    std::unique_ptr<MemPool> _mem_pool;
    std::unique_ptr<RuntimeProfile> _runtime_profile;
    std::unique_ptr<AggStatistics> _agg_stat;
};

// Memory snapshot helper: hash-table byte footprint via dump_bound()
// (uniform across phmap and SmallFixedSizeHashMap) plus mem-pool
// allocated bytes.  Reported as benchmark counters so reviewers can see
// the time/memory trade-off without re-running with a profiler.
template <typename Wrapper>
inline size_t hash_table_bytes(Wrapper* w) {
    return w->hash_map.dump_bound();
}

// ============================================================================
// Section A -- steady-state hash-insert win (build_hash_map only)
// ============================================================================
template <typename Wrapper>
static void BM_Int16BuildOnly(benchmark::State& state) {
    const int64_t num_rows = state.range(0);
    const int distinct = static_cast<int>(state.range(1));
    const Distribution dist = static_cast<Distribution>(state.range(2));

    BenchSuite suite;
    suite.SetUp();
    Int16ChunkStream stream(num_rows, distinct, dist);

    int64_t total_rows = 0;
    size_t final_groups = 0;
    size_t final_ht_bytes = 0;
    size_t final_pool_bytes = 0;
    int64_t cum_checksum = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Heap-allocated to match production (Aggregator uses std::make_unique
        // for variant pointers).  Stack alloc would hide the 524 KiB upfront
        // cost the array path actually pays in production.
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        HashTableKeyAllocator allocator;
        allocator.aggregate_key_size = sizeof(int64_t);
        allocator.pool = suite._mem_pool.get();
        BenchAllocateState alloc{&allocator};
        Buffer<AggDataPtr> agg_states(kBenchChunkSize);
        state.ResumeTiming();

        for (const auto& chunk_col : stream.chunks()) {
            Columns key_columns;
            key_columns.emplace_back(chunk_col);
            wrapper->build_hash_map(kBenchChunkSize, key_columns, suite._mem_pool.get(), alloc, &agg_states);
            total_rows += kBenchChunkSize;
        }
        benchmark::DoNotOptimize(agg_states.data());
        benchmark::ClobberMemory();

        // End the iteration paused so wrapper / pool destruction does not
        // bleed into the next iter's measurement window.
        state.PauseTiming();
        final_groups = wrapper->hash_map.size();
        final_ht_bytes = hash_table_bytes(wrapper.get());
        final_pool_bytes = suite._mem_pool->total_allocated_bytes();
        // Observe the last-chunk AggDataPtr targets so build_hash_map
        // results survive DCE.
        int64_t cs = 0;
        for (int i = 0; i < kBenchChunkSize; ++i) cs += reinterpret_cast<intptr_t>(agg_states[i]);
        cum_checksum += cs;
        wrapper.reset();
        suite._mem_pool->clear();
    }
    benchmark::DoNotOptimize(cum_checksum);
    state.SetItemsProcessed(total_rows);
    state.counters["distinct"] = distinct;
    state.counters["dist"] = static_cast<int>(dist);
    state.counters["rows_per_iter"] = num_rows;
    state.counters["final_groups"] = final_groups;
    state.counters["hash_table_bytes"] = final_ht_bytes;
    state.counters["pool_bytes"] = final_pool_bytes;

    suite.TearDown();
}

// ============================================================================
// Section A (continued) -- build_hash_map + COUNT(*) update
// Same shape as BM_Int16BuildOnly but with a simulated COUNT(*) increment
// per resolved AggDataPtr.  Exposes the total per-row cost (hash-insert
// cost + agg-state-update cost) so the reader can see how the kernel-level
// delta translates to total agg-phase delta.
// ============================================================================
template <typename Wrapper>
static void BM_Int16BuildAndCount(benchmark::State& state) {
    const int64_t num_rows = state.range(0);
    const int distinct = static_cast<int>(state.range(1));
    const Distribution dist = static_cast<Distribution>(state.range(2));

    BenchSuite suite;
    suite.SetUp();
    Int16ChunkStream stream(num_rows, distinct, dist);

    int64_t total_rows = 0;
    size_t final_groups = 0;
    size_t final_ht_bytes = 0;
    size_t final_pool_bytes = 0;
    int64_t cum_checksum = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        HashTableKeyAllocator allocator;
        allocator.aggregate_key_size = sizeof(int64_t);
        allocator.pool = suite._mem_pool.get();
        BenchAllocateState alloc{&allocator};
        Buffer<AggDataPtr> agg_states(kBenchChunkSize);
        state.ResumeTiming();

        for (const auto& chunk_col : stream.chunks()) {
            Columns key_columns;
            key_columns.emplace_back(chunk_col);
            wrapper->build_hash_map(kBenchChunkSize, key_columns, suite._mem_pool.get(), alloc, &agg_states);
            // Simulated CountAggregateFunction::update per row: state is
            // sizeof(int64_t) counter, increment unconditionally.
            for (int i = 0; i < kBenchChunkSize; ++i) {
                ++(*reinterpret_cast<int64_t*>(agg_states[i]));
            }
            total_rows += kBenchChunkSize;
        }
        benchmark::DoNotOptimize(agg_states.data());
        benchmark::ClobberMemory();

        state.PauseTiming();
        final_groups = wrapper->hash_map.size();
        final_ht_bytes = hash_table_bytes(wrapper.get());
        final_pool_bytes = suite._mem_pool->total_allocated_bytes();
        int64_t cs = 0;
        for (int i = 0; i < kBenchChunkSize; ++i) cs += *reinterpret_cast<int64_t*>(agg_states[i]);
        cum_checksum += cs;
        wrapper.reset();
        suite._mem_pool->clear();
    }
    benchmark::DoNotOptimize(cum_checksum);
    state.SetItemsProcessed(total_rows);
    state.counters["distinct"] = distinct;
    state.counters["dist"] = static_cast<int>(dist);
    state.counters["rows_per_iter"] = num_rows;
    state.counters["final_groups"] = final_groups;
    state.counters["hash_table_bytes"] = final_ht_bytes;
    state.counters["pool_bytes"] = final_pool_bytes;

    suite.TearDown();
}

// ============================================================================
// Section B -- amortization sweep, ctor-included companion
// Same as BM_Int16BuildOnly but wrapper construction (and the 524 KiB
// zero-fill for the array path) happens INSIDE the timed region.  This
// is the row count at which the upfront cost stops dominating.
// ============================================================================
template <typename Wrapper>
static void BM_Int16BuildWithCtor(benchmark::State& state) {
    const int64_t num_rows = state.range(0);
    const int distinct = static_cast<int>(state.range(1));
    const Distribution dist = static_cast<Distribution>(state.range(2));

    BenchSuite suite;
    suite.SetUp();
    Int16ChunkStream stream(num_rows, distinct, dist);

    int64_t total_rows = 0;
    size_t final_ht_bytes = 0;
    int64_t cum_checksum = 0;
    for (auto _ : state) {
        // No PauseTiming before construction -- the ctor's 524 KiB
        // zero-fill (array path) or default-capacity table (phmap) is
        // included in the per-iter timing.
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        HashTableKeyAllocator allocator;
        allocator.aggregate_key_size = sizeof(int64_t);
        allocator.pool = suite._mem_pool.get();
        BenchAllocateState alloc{&allocator};
        Buffer<AggDataPtr> agg_states(kBenchChunkSize);

        for (const auto& chunk_col : stream.chunks()) {
            Columns key_columns;
            key_columns.emplace_back(chunk_col);
            wrapper->build_hash_map(kBenchChunkSize, key_columns, suite._mem_pool.get(), alloc, &agg_states);
            total_rows += kBenchChunkSize;
        }
        benchmark::DoNotOptimize(agg_states.data());
        benchmark::ClobberMemory();

        state.PauseTiming();
        final_ht_bytes = hash_table_bytes(wrapper.get());
        int64_t cs = 0;
        for (int i = 0; i < kBenchChunkSize; ++i) cs += reinterpret_cast<intptr_t>(agg_states[i]);
        cum_checksum += cs;
        wrapper.reset();
        suite._mem_pool->clear();
        state.ResumeTiming();
    }
    benchmark::DoNotOptimize(cum_checksum);
    state.SetItemsProcessed(total_rows);
    state.counters["distinct"] = distinct;
    state.counters["dist"] = static_cast<int>(dist);
    state.counters["rows_per_iter"] = num_rows;
    state.counters["hash_table_bytes"] = final_ht_bytes;

    suite.TearDown();
}

// ============================================================================
// Section C -- construction-only cost
// SmallFixedSizeHashMap<int16_t> ctor zero-fills the 524 KiB pointer table
// while phmap starts at near-zero capacity; this exposes the upfront cost
// independent of any workload.
// ============================================================================
template <typename Wrapper>
static void BM_Int16Construct(benchmark::State& state) {
    BenchSuite suite;
    suite.SetUp();
    for (auto _ : state) {
        // make_unique allocates on heap matching production -- this exposes
        // the malloc + 524 KiB memset cost the array wrapper pays at every
        // Aggregator instantiation.
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        benchmark::DoNotOptimize(wrapper.get());
        state.PauseTiming();
        wrapper.reset();
        state.ResumeTiming();
    }
    suite.TearDown();
}

// ============================================================================
// Section D -- nullable coverage
// Three null fractions exercise the three branches of compute_agg_states_nullable:
//   null_fraction == 0  -> NullableColumn with has_null=false; fast-path
//                          falls through to compute_agg_states_non_nullable.
//   null_fraction == 0.1 / 0.5 -> NullableColumn with has_null=true;
//                          routes through compute_agg_through_null_data
//                          (the per-row null-bit-aware loop).
// ============================================================================
template <typename Wrapper>
static void BM_Int16NullableBuildOnly(benchmark::State& state) {
    const int64_t num_rows = state.range(0);
    const int distinct = static_cast<int>(state.range(1));
    // null_fraction encoded as state.range(2) * 0.01 (so 0, 10, 50 -> 0%, 10%, 50%)
    const double null_fraction = state.range(2) * 0.01;

    BenchSuite suite;
    suite.SetUp();
    NullableInt16ChunkStream stream(num_rows, distinct, null_fraction);

    int64_t total_rows = 0;
    int64_t cum_checksum = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        HashTableKeyAllocator allocator;
        allocator.aggregate_key_size = sizeof(int64_t);
        allocator.pool = suite._mem_pool.get();
        BenchAllocateState alloc{&allocator};
        Buffer<AggDataPtr> agg_states(kBenchChunkSize);
        state.ResumeTiming();

        for (const auto& chunk_col : stream.chunks()) {
            Columns key_columns;
            key_columns.emplace_back(chunk_col);
            wrapper->build_hash_map(kBenchChunkSize, key_columns, suite._mem_pool.get(), alloc, &agg_states);
            total_rows += kBenchChunkSize;
        }
        benchmark::DoNotOptimize(agg_states.data());
        benchmark::ClobberMemory();

        state.PauseTiming();
        int64_t cs = 0;
        for (int i = 0; i < kBenchChunkSize; ++i) cs += reinterpret_cast<intptr_t>(agg_states[i]);
        cum_checksum += cs;
        wrapper.reset();
        suite._mem_pool->clear();
    }
    benchmark::DoNotOptimize(cum_checksum);
    state.SetItemsProcessed(total_rows);
    state.counters["distinct"] = distinct;
    state.counters["null_pct"] = state.range(2);

    suite.TearDown();
}

// ============================================================================
// Section E -- cx1 (compressed-key int8) vs direct array (int16) for
// SMALLINT GROUP BY when the FE-supplied value range fits in 8 bits.
//
// Today our slice_cx1 guard on TINYINT/BOOL/SMALLINT (aggregator.cpp) means
// we always pick the 65 536-cell int16 direct array, even when the range
// stats say only 8 bits are used.  stdpain raised on #73558 that for int16
// with an 8-bit range, slice_cx1 may actually win: footprint drops from
// 524 KiB to 2 KiB (L1-resident) at the cost of a bitcompress_serialize
// step per row.  This section answers that head-on.
//
// SliceCx1Wrapper is the production wrapper used when slice_cx1 routing
// fires: AggHashMapWithCompressedKeyFixedSize over Int8AggHashMap
// (SmallFixedSizeHashMap<int8_t>).  The wrapper requires bases/offsets/
// used_bits set up before build_hash_map, normally done by Aggregator's
// _build_hash_variant; we mirror that setup explicitly.
// ============================================================================
using SliceCx1Wrapper = AggHashMapWithCompressedKeyFixedSize<Int8AggHashMap<PhmapSeed1>>;

template <typename Wrapper>
static void BM_Int16Cx1BuildOnly(benchmark::State& state) {
    const int64_t num_rows = state.range(0);
    const int distinct = static_cast<int>(state.range(1));
    const Distribution dist = static_cast<Distribution>(state.range(2));

    BenchSuite suite;
    suite.SetUp();
    Int16ChunkStream stream(num_rows, distinct, dist);

    // Same range Int16ChunkStream uses: keys live in
    // [-distinct/2, distinct/2).  delta = distinct - 1, used_bits is the
    // number of bits Aggregator would have computed via get_used_bits.
    const int16_t range_start = static_cast<int16_t>(-distinct / 2);
    const int16_t range_end = static_cast<int16_t>(range_start + distinct - 1);
    const uint16_t delta = static_cast<uint16_t>(static_cast<uint16_t>(range_end) - static_cast<uint16_t>(range_start));
    const int used_bits = delta == 0 ? 0 : (32 - __builtin_clz(static_cast<uint32_t>(delta)));

    int64_t total_rows = 0;
    size_t final_groups = 0;
    size_t final_ht_bytes = 0;
    int64_t cum_checksum = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        // Mirror Aggregator::_build_hash_variant's CompressKeyContext setup
        // for a single int16 column: base = min value, offset = 0,
        // used_bits = ceil_log2(range + 1).
        wrapper->bases = std::vector<std::any>{range_start};
        wrapper->offsets = std::vector<int>{0};
        wrapper->used_bits = std::vector<int>{used_bits};
        HashTableKeyAllocator allocator;
        allocator.aggregate_key_size = sizeof(int64_t);
        allocator.pool = suite._mem_pool.get();
        BenchAllocateState alloc{&allocator};
        Buffer<AggDataPtr> agg_states(kBenchChunkSize);
        state.ResumeTiming();

        for (const auto& chunk_col : stream.chunks()) {
            Columns key_columns;
            key_columns.emplace_back(chunk_col);
            wrapper->build_hash_map(kBenchChunkSize, key_columns, suite._mem_pool.get(), alloc, &agg_states);
            total_rows += kBenchChunkSize;
        }
        benchmark::DoNotOptimize(agg_states.data());
        benchmark::ClobberMemory();

        state.PauseTiming();
        final_groups = wrapper->hash_map.size();
        final_ht_bytes = wrapper->hash_map.dump_bound();
        int64_t cs = 0;
        for (int i = 0; i < kBenchChunkSize; ++i) cs += reinterpret_cast<intptr_t>(agg_states[i]);
        cum_checksum += cs;
        wrapper.reset();
        suite._mem_pool->clear();
    }
    benchmark::DoNotOptimize(cum_checksum);
    state.SetItemsProcessed(total_rows);
    state.counters["distinct"] = distinct;
    state.counters["dist"] = static_cast<int>(dist);
    state.counters["rows_per_iter"] = num_rows;
    state.counters["final_groups"] = final_groups;
    state.counters["hash_table_bytes"] = final_ht_bytes;
    state.counters["used_bits"] = used_bits;

    suite.TearDown();
}

// ============================================================================
// Args registration
// ============================================================================

// Section A: density x distribution sweep at 100M rows.
// Densities chosen for realistic SMALLINT GROUP BY shapes:
//   1     -> boundary "single distinct after pruning"; array's pathological
//            best case.  Not the headline density.
//   4     -> year bucket
//   24    -> hour-of-day (headline mid density)
//   366   -> day-of-year
//   1000  -> region-code-ish
//   10000 -> high-cardinality dimension (merchant/sku-ish)
//   65536 -> array's worst-case memory locality (every slot used)
static void RegisterArgs_SectionA(benchmark::internal::Benchmark* b) {
    b->ArgNames({"rows", "distinct", "dist"});
    constexpr int64_t kRows = 100'000'000;
    for (int distinct : {1, 4, 24, 366, 1000, 10000, 65536}) {
        for (int d = 0; d <= 3; ++d) {
            b->Args({kRows, distinct, d});
        }
    }
    b->Unit(benchmark::kMillisecond);
    b->Iterations(3);
}

// Section B: amortization sweep at density=24, sorted.  Shows where the
// 524 KiB upfront alloc stops dominating.  Same args used by both
// BM_Int16BuildOnly (ctor paused) and BM_Int16BuildWithCtor (ctor timed)
// so reviewers can read the two side by side.
static void RegisterArgs_SectionB(benchmark::internal::Benchmark* b) {
    b->ArgNames({"rows", "distinct", "dist"});
    for (int64_t rows : {4'096LL, 100'000LL, 1'000'000LL, 10'000'000LL, 100'000'000LL}) {
        b->Args({rows, 24, /*Sorted*/ 1});
    }
    b->Unit(benchmark::kMillisecond);
    b->Iterations(3);
}

// Section D: nullable, density=24 hour-of-day shape, sorted, three null
// fractions {0%, 10%, 50%}, smaller row count because nullable codepath is
// inherently slower and we just want to verify no regression vs baseline.
static void RegisterArgs_SectionD(benchmark::internal::Benchmark* b) {
    b->ArgNames({"rows", "distinct", "null_pct"});
    constexpr int64_t kRows = 50'000'000;
    for (int null_pct : {0, 10, 50}) {
        b->Args({kRows, 24, null_pct});
    }
    b->Unit(benchmark::kMillisecond);
    b->Iterations(3);
}

// Section E: cx1 vs direct for SMALLINT with 8-bit value range.  All
// distincts <= 256 so slice_cx1 is a legal routing (8 used_bits).
// Random and Sorted cover the prefetch-vs-no-prefetch axis; cx1 has
// prefetch-on under flat_hash_map but no_prefetch under
// SmallFixedSizeHashMap<int8>, so Sorted is the cleanest read.
static void RegisterArgs_SectionE(benchmark::internal::Benchmark* b) {
    b->ArgNames({"rows", "distinct", "dist"});
    constexpr int64_t kRows = 100'000'000;
    for (int distinct : {16, 64, 128, 200, 256}) {
        b->Args({kRows, distinct, /*Sorted*/ 1});
        b->Args({kRows, distinct, /*Random*/ 0});
    }
    b->Unit(benchmark::kMillisecond);
    b->Iterations(3);
}

BENCHMARK_TEMPLATE(BM_Int16BuildOnly, PhmapInt16Wrapper)->Apply(RegisterArgs_SectionA);
BENCHMARK_TEMPLATE(BM_Int16BuildOnly, ArrayInt16Wrapper)->Apply(RegisterArgs_SectionA);

BENCHMARK_TEMPLATE(BM_Int16BuildAndCount, PhmapInt16Wrapper)->Apply(RegisterArgs_SectionA);
BENCHMARK_TEMPLATE(BM_Int16BuildAndCount, ArrayInt16Wrapper)->Apply(RegisterArgs_SectionA);

BENCHMARK_TEMPLATE(BM_Int16BuildOnly, PhmapInt16Wrapper)->Apply(RegisterArgs_SectionB);
BENCHMARK_TEMPLATE(BM_Int16BuildOnly, ArrayInt16Wrapper)->Apply(RegisterArgs_SectionB);

BENCHMARK_TEMPLATE(BM_Int16BuildWithCtor, PhmapInt16Wrapper)->Apply(RegisterArgs_SectionB);
BENCHMARK_TEMPLATE(BM_Int16BuildWithCtor, ArrayInt16Wrapper)->Apply(RegisterArgs_SectionB);

BENCHMARK_TEMPLATE(BM_Int16Construct, PhmapInt16Wrapper)->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_Int16Construct, ArrayInt16Wrapper)->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_Int16NullableBuildOnly, PhmapInt16NullableWrapper)->Apply(RegisterArgs_SectionD);
BENCHMARK_TEMPLATE(BM_Int16NullableBuildOnly, ArrayInt16NullableWrapper)->Apply(RegisterArgs_SectionD);

BENCHMARK_TEMPLATE(BM_Int16BuildOnly, ArrayInt16Wrapper)->Apply(RegisterArgs_SectionE);
BENCHMARK_TEMPLATE(BM_Int16Cx1BuildOnly, SliceCx1Wrapper)->Apply(RegisterArgs_SectionE);

// ============================================================================
// Section F -- Fixed-Size Serialized Key (SliceKey4 / SliceKey8 / SliceKey16 /
// SliceKey32): upstream/main Mixed-Stride vs AoS CacheEntry (32B / 24B / 16B /
// 40B) vs Proposed SoA (Buffer<FixedSizeSliceKey> keys + Buffer<size_t> hashes)
// ============================================================================

// 1. AoS CacheEntry wrapper: both noprefetch and prefetch serialize into
// std::vector<CacheEntry> at stride max_fixed_size = sizeof(CacheEntry)
// (16B for SliceKey4/SliceKey8, 32B for SliceKey16, 40B for SliceKey32).
template <typename HashMap>
struct AoSCacheEntryFixedSizeWrapper : public AggHashMapWithKey<HashMap, AoSCacheEntryFixedSizeWrapper<HashMap>> {
    using Self = AoSCacheEntryFixedSizeWrapper<HashMap>;
    using Base = AggHashMapWithKey<HashMap, AoSCacheEntryFixedSizeWrapper<HashMap>>;
    using KeyType = typename HashMap::key_type;
    using Iterator = typename HashMap::iterator;
    using FixedSizeSliceKey = typename HashMap::key_type;
    using ResultVector = typename std::vector<FixedSizeSliceKey>;

    bool has_null_column = false;
    int fixed_byte_size = -1;
    struct CacheEntry {
        FixedSizeSliceKey key;
        size_t hashval;
    };

    static constexpr size_t max_fixed_size = sizeof(CacheEntry);

    std::vector<CacheEntry> caches;

    template <class... Args>
    AoSCacheEntryFixedSizeWrapper(int chunk_size, Args&&... args)
            : Base(chunk_size, std::forward<Args>(args)...), _chunk_size(chunk_size) {
        caches.resize(chunk_size);
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        memset(buffer, 0x0, max_fixed_size * _chunk_size);
    }

    AggDataPtr get_null_key_data() { return nullptr; }
    void set_null_key_data(AggDataPtr /*data*/) {}

    template <AllocFunc<Self> Func, typename HTBuildOp>
    ALWAYS_NOINLINE void compute_agg_prefetch(size_t chunk_size, const Columns& key_columns,
                                              Buffer<AggDataPtr>* agg_states, Func&& allocate_func,
                                              ExtraAggParam* extra) {
        [[maybe_unused]] size_t hash_table_size = this->hash_map.size();
        auto* __restrict not_founds = extra->not_founds;
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        for (const auto& key_column : key_columns) {
            key_column->serialize_batch(buffer, slice_sizes, chunk_size, max_fixed_size);
        }
        if (has_null_column) {
            for (size_t i = 0; i < chunk_size; ++i) {
                caches[i].key.u.size = slice_sizes[i];
            }
        }
        for (size_t i = 0; i < chunk_size; i++) {
            caches[i].hashval = this->hash_map.hash_function()(caches[i].key);
        }

        const size_t __prefetch_dist = agg_hash_map_default_prefetch_dist();
        size_t __prefetch_index = __prefetch_dist;

        for (size_t i = 0; i < chunk_size; ++i) {
            if (__prefetch_dist != 0 && __prefetch_index < chunk_size) {
                this->hash_map.prefetch_hash(caches[__prefetch_index++].hashval);
            }
            FixedSizeSliceKey& key = caches[i].key;
            if constexpr (HTBuildOp::process_limit) {
                if (hash_table_size < extra->limits) {
                    _emplace_key_with_hash(key, caches[i].hashval, (*agg_states)[i], allocate_func,
                                           [&]() { hash_table_size++; });
                } else {
                    _find_key((*agg_states)[i], (*not_founds)[i], key, caches[i].hashval);
                }
            } else if constexpr (HTBuildOp::allocate) {
                _emplace_key_with_hash(key, caches[i].hashval, (*agg_states)[i], allocate_func,
                                       FillNotFounds<HTBuildOp::fill_not_found>(not_founds, i));
            } else if constexpr (HTBuildOp::fill_not_found) {
                _find_key((*agg_states)[i], (*not_founds)[i], key, caches[i].hashval);
            }
        }
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    ALWAYS_NOINLINE void compute_agg_noprefetch(size_t chunk_size, const Columns& key_columns,
                                                Buffer<AggDataPtr>* agg_states, Func&& allocate_func,
                                                ExtraAggParam* extra) {
        [[maybe_unused]] size_t hash_table_size = this->hash_map.size();
        auto* __restrict not_founds = extra->not_founds;
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        for (const auto& key_column : key_columns) {
            key_column->serialize_batch(buffer, slice_sizes, chunk_size, max_fixed_size);
        }
        if (has_null_column) {
            for (size_t i = 0; i < chunk_size; ++i) {
                caches[i].key.u.size = slice_sizes[i];
            }
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            FixedSizeSliceKey& key = caches[i].key;
            if constexpr (HTBuildOp::process_limit) {
                if (hash_table_size < extra->limits) {
                    _emplace_key(key, (*agg_states)[i], allocate_func, [&] { hash_table_size++; });
                } else {
                    _find_key((*agg_states)[i], (*not_founds)[i], key);
                }
            } else if constexpr (HTBuildOp::allocate) {
                _emplace_key(key, (*agg_states)[i], allocate_func,
                             FillNotFounds<HTBuildOp::fill_not_found>(not_founds, i));
            } else if constexpr (HTBuildOp::fill_not_found) {
                _find_key((*agg_states)[i], (*not_founds)[i], key);
            }
        }
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    void compute_agg_states(size_t chunk_size, const Columns& key_columns, MemPool* /*pool*/, Func&& allocate_func,
                            Buffer<AggDataPtr>* agg_states, ExtraAggParam* extra) {
        DCHECK(has_null_column || fixed_byte_size != -1);
        DCHECK_LE(chunk_size, caches.size());
        slice_sizes.assign(chunk_size, 0);

        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        if (has_null_column) {
            memset(buffer, 0x0, max_fixed_size * chunk_size);
        }

        if (!agg_should_prefetch_table(this->hash_map)) {
            this->template compute_agg_noprefetch<Func, HTBuildOp>(chunk_size, key_columns, agg_states,
                                                                   std::forward<Func>(allocate_func), extra);
        } else {
            this->template compute_agg_prefetch<Func, HTBuildOp>(chunk_size, key_columns, agg_states,
                                                                 std::forward<Func>(allocate_func), extra);
        }
    }

    template <AllocFunc<Self> Func, typename EmplaceCallBack>
    ALWAYS_INLINE void _emplace_key(KeyType key, AggDataPtr& target_state, Func&& allocate_func,
                                    EmplaceCallBack&& callback) {
        auto iter = this->hash_map.lazy_emplace(key, [&](const auto& ctor) {
            callback();
            AggDataPtr pv = allocate_func(key);
            ctor(key, pv);
        });
        target_state = iter->second;
    }

    template <AllocFunc<Self> Func, typename EmplaceCallBack>
    ALWAYS_INLINE void _emplace_key_with_hash(KeyType key, size_t hash, AggDataPtr& target_state, Func&& allocate_func,
                                              EmplaceCallBack&& callback) {
        auto iter = this->hash_map.lazy_emplace_with_hash(key, hash, [&](const auto& ctor) {
            callback();
            AggDataPtr pv = allocate_func(key);
            ctor(key, pv);
        });
        target_state = iter->second;
    }

    template <typename... Args>
    ALWAYS_INLINE void _find_key(AggDataPtr& target_state, uint8_t& not_found, Args&&... args) {
        if (auto iter = this->hash_map.find(std::forward<Args>(args)...); iter != this->hash_map.end()) {
            target_state = iter->second;
        } else {
            not_found = 1;
        }
    }

    static constexpr bool has_single_null_key = false;

    Buffer<uint32_t> slice_sizes;
    ResultVector results;
    Buffer<Slice> tmp_slices;
    int32_t _chunk_size;
};

// 2. AoS 24B packed/unaligned CacheEntry24 wrapper for SliceKey16:
// sizeof(CacheEntry24) == 24 (alignas(8)), so every odd 16B key is at byte
// offset 24 * (2k + 1) (8-byte misaligned for 16B loads) and every 3rd entry
// crosses a 64B cache-line boundary.
template <typename HashMap>
struct AoS24BCacheEntryFixedSizeWrapper : public AggHashMapWithKey<HashMap, AoS24BCacheEntryFixedSizeWrapper<HashMap>> {
    using Self = AoS24BCacheEntryFixedSizeWrapper<HashMap>;
    using Base = AggHashMapWithKey<HashMap, AoS24BCacheEntryFixedSizeWrapper<HashMap>>;
    using KeyType = typename HashMap::key_type;
    using Iterator = typename HashMap::iterator;
    using FixedSizeSliceKey = typename HashMap::key_type;
    using ResultVector = typename std::vector<FixedSizeSliceKey>;

    static_assert(sizeof(FixedSizeSliceKey) == 16);

    bool has_null_column = false;
    int fixed_byte_size = -1;
    // Note: SliceKey16 has a user-defined copy constructor and int128_t member
    // (alignof == 16), so GCC ignores __attribute__((packed)) on a struct
    // containing a SliceKey16 field directly and pads it to 32B. Using an
    // alignas(8) 16B key_bytes + 8B hashval struct models the exact 24B AoS
    // stride where every odd 16B key is at offset 24 * (2k + 1) (8B misaligned).
    struct alignas(8) CacheEntry24 {
        uint8_t key_bytes[sizeof(FixedSizeSliceKey)];
        size_t hashval;
    };
    static_assert(sizeof(CacheEntry24) == 24);
    static_assert(alignof(CacheEntry24) == 8);

    static constexpr size_t max_fixed_size = sizeof(CacheEntry24);

    std::vector<CacheEntry24> caches;

    template <class... Args>
    AoS24BCacheEntryFixedSizeWrapper(int chunk_size, Args&&... args)
            : Base(chunk_size, std::forward<Args>(args)...), _chunk_size(chunk_size) {
        caches.resize(chunk_size);
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        memset(buffer, 0x0, max_fixed_size * _chunk_size);
    }

    AggDataPtr get_null_key_data() { return nullptr; }
    void set_null_key_data(AggDataPtr /*data*/) {}

    ALWAYS_INLINE FixedSizeSliceKey _load_unaligned_key(size_t i) const {
        FixedSizeSliceKey k;
        std::memcpy(&k.u, caches[i].key_bytes, sizeof(FixedSizeSliceKey));
        return k;
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    ALWAYS_NOINLINE void compute_agg_prefetch(size_t chunk_size, const Columns& key_columns,
                                              Buffer<AggDataPtr>* agg_states, Func&& allocate_func,
                                              ExtraAggParam* extra) {
        [[maybe_unused]] size_t hash_table_size = this->hash_map.size();
        auto* __restrict not_founds = extra->not_founds;
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        for (const auto& key_column : key_columns) {
            key_column->serialize_batch(buffer, slice_sizes, chunk_size, max_fixed_size);
        }
        if (has_null_column) {
            for (size_t i = 0; i < chunk_size; ++i) {
                buffer[i * max_fixed_size + sizeof(FixedSizeSliceKey) - 1] = static_cast<uint8_t>(slice_sizes[i]);
            }
        }
        for (size_t i = 0; i < chunk_size; i++) {
            caches[i].hashval = this->hash_map.hash_function()(_load_unaligned_key(i));
        }

        const size_t __prefetch_dist = agg_hash_map_default_prefetch_dist();
        size_t __prefetch_index = __prefetch_dist;

        for (size_t i = 0; i < chunk_size; ++i) {
            if (__prefetch_dist != 0 && __prefetch_index < chunk_size) {
                this->hash_map.prefetch_hash(caches[__prefetch_index++].hashval);
            }
            const FixedSizeSliceKey key = _load_unaligned_key(i);
            const size_t hashval = caches[i].hashval;
            if constexpr (HTBuildOp::process_limit) {
                if (hash_table_size < extra->limits) {
                    _emplace_key_with_hash(key, hashval, (*agg_states)[i], allocate_func, [&]() { hash_table_size++; });
                } else {
                    _find_key((*agg_states)[i], (*not_founds)[i], key, hashval);
                }
            } else if constexpr (HTBuildOp::allocate) {
                _emplace_key_with_hash(key, hashval, (*agg_states)[i], allocate_func,
                                       FillNotFounds<HTBuildOp::fill_not_found>(not_founds, i));
            } else if constexpr (HTBuildOp::fill_not_found) {
                _find_key((*agg_states)[i], (*not_founds)[i], key, hashval);
            }
        }
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    ALWAYS_NOINLINE void compute_agg_noprefetch(size_t chunk_size, const Columns& key_columns,
                                                Buffer<AggDataPtr>* agg_states, Func&& allocate_func,
                                                ExtraAggParam* extra) {
        [[maybe_unused]] size_t hash_table_size = this->hash_map.size();
        auto* __restrict not_founds = extra->not_founds;
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        for (const auto& key_column : key_columns) {
            key_column->serialize_batch(buffer, slice_sizes, chunk_size, max_fixed_size);
        }
        if (has_null_column) {
            for (size_t i = 0; i < chunk_size; ++i) {
                buffer[i * max_fixed_size + sizeof(FixedSizeSliceKey) - 1] = static_cast<uint8_t>(slice_sizes[i]);
            }
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            const FixedSizeSliceKey key = _load_unaligned_key(i);
            if constexpr (HTBuildOp::process_limit) {
                if (hash_table_size < extra->limits) {
                    _emplace_key(key, (*agg_states)[i], allocate_func, [&] { hash_table_size++; });
                } else {
                    _find_key((*agg_states)[i], (*not_founds)[i], key);
                }
            } else if constexpr (HTBuildOp::allocate) {
                _emplace_key(key, (*agg_states)[i], allocate_func,
                             FillNotFounds<HTBuildOp::fill_not_found>(not_founds, i));
            } else if constexpr (HTBuildOp::fill_not_found) {
                _find_key((*agg_states)[i], (*not_founds)[i], key);
            }
        }
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    void compute_agg_states(size_t chunk_size, const Columns& key_columns, MemPool* /*pool*/, Func&& allocate_func,
                            Buffer<AggDataPtr>* agg_states, ExtraAggParam* extra) {
        DCHECK(has_null_column || fixed_byte_size != -1);
        DCHECK_LE(chunk_size, caches.size());
        slice_sizes.assign(chunk_size, 0);

        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        if (has_null_column) {
            memset(buffer, 0x0, max_fixed_size * chunk_size);
        }

        if (!agg_should_prefetch_table(this->hash_map)) {
            this->template compute_agg_noprefetch<Func, HTBuildOp>(chunk_size, key_columns, agg_states,
                                                                   std::forward<Func>(allocate_func), extra);
        } else {
            this->template compute_agg_prefetch<Func, HTBuildOp>(chunk_size, key_columns, agg_states,
                                                                 std::forward<Func>(allocate_func), extra);
        }
    }

    template <AllocFunc<Self> Func, typename EmplaceCallBack>
    ALWAYS_INLINE void _emplace_key(KeyType key, AggDataPtr& target_state, Func&& allocate_func,
                                    EmplaceCallBack&& callback) {
        auto iter = this->hash_map.lazy_emplace(key, [&](const auto& ctor) {
            callback();
            AggDataPtr pv = allocate_func(key);
            ctor(key, pv);
        });
        target_state = iter->second;
    }

    template <AllocFunc<Self> Func, typename EmplaceCallBack>
    ALWAYS_INLINE void _emplace_key_with_hash(KeyType key, size_t hash, AggDataPtr& target_state, Func&& allocate_func,
                                              EmplaceCallBack&& callback) {
        auto iter = this->hash_map.lazy_emplace_with_hash(key, hash, [&](const auto& ctor) {
            callback();
            AggDataPtr pv = allocate_func(key);
            ctor(key, pv);
        });
        target_state = iter->second;
    }

    template <typename... Args>
    ALWAYS_INLINE void _find_key(AggDataPtr& target_state, uint8_t& not_found, Args&&... args) {
        if (auto iter = this->hash_map.find(std::forward<Args>(args)...); iter != this->hash_map.end()) {
            target_state = iter->second;
        } else {
            not_found = 1;
        }
    }

    static constexpr bool has_single_null_key = false;

    Buffer<uint32_t> slice_sizes;
    ResultVector results;
    Buffer<Slice> tmp_slices;
    int32_t _chunk_size;
};

// 3. upstream/main Mixed-Stride wrapper:
// compute_agg_noprefetch serializes at key_size = sizeof(FixedSizeSliceKey) into
// caches.data(), while compute_agg_prefetch serializes at max_fixed_size =
// sizeof(CacheEntry) into caches.data().
template <typename HashMap>
struct MainMixedStrideFixedSizeWrapper : public AggHashMapWithKey<HashMap, MainMixedStrideFixedSizeWrapper<HashMap>> {
    using Self = MainMixedStrideFixedSizeWrapper<HashMap>;
    using Base = AggHashMapWithKey<HashMap, MainMixedStrideFixedSizeWrapper<HashMap>>;
    using KeyType = typename HashMap::key_type;
    using Iterator = typename HashMap::iterator;
    using FixedSizeSliceKey = typename HashMap::key_type;
    using ResultVector = typename std::vector<FixedSizeSliceKey>;

    bool has_null_column = false;
    int fixed_byte_size = -1;
    struct CacheEntry {
        FixedSizeSliceKey key;
        size_t hashval;
    };

    static constexpr size_t max_fixed_size = sizeof(CacheEntry);

    std::vector<CacheEntry> caches;

    template <class... Args>
    MainMixedStrideFixedSizeWrapper(int chunk_size, Args&&... args)
            : Base(chunk_size, std::forward<Args>(args)...), _chunk_size(chunk_size) {
        caches.resize(chunk_size);
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        memset(buffer, 0x0, max_fixed_size * _chunk_size);
    }

    AggDataPtr get_null_key_data() { return nullptr; }
    void set_null_key_data(AggDataPtr /*data*/) {}

    template <AllocFunc<Self> Func, typename HTBuildOp>
    ALWAYS_NOINLINE void compute_agg_prefetch(size_t chunk_size, const Columns& key_columns,
                                              Buffer<AggDataPtr>* agg_states, Func&& allocate_func,
                                              ExtraAggParam* extra) {
        [[maybe_unused]] size_t hash_table_size = this->hash_map.size();
        auto* __restrict not_founds = extra->not_founds;
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        for (const auto& key_column : key_columns) {
            key_column->serialize_batch(buffer, slice_sizes, chunk_size, max_fixed_size);
        }
        if (has_null_column) {
            for (size_t i = 0; i < chunk_size; ++i) {
                caches[i].key.u.size = slice_sizes[i];
            }
        }
        for (size_t i = 0; i < chunk_size; i++) {
            caches[i].hashval = this->hash_map.hash_function()(caches[i].key);
        }

        const size_t __prefetch_dist = agg_hash_map_default_prefetch_dist();
        size_t __prefetch_index = __prefetch_dist;

        for (size_t i = 0; i < chunk_size; ++i) {
            if (__prefetch_dist != 0 && __prefetch_index < chunk_size) {
                this->hash_map.prefetch_hash(caches[__prefetch_index++].hashval);
            }
            FixedSizeSliceKey& key = caches[i].key;
            if constexpr (HTBuildOp::process_limit) {
                if (hash_table_size < extra->limits) {
                    _emplace_key_with_hash(key, caches[i].hashval, (*agg_states)[i], allocate_func,
                                           [&]() { hash_table_size++; });
                } else {
                    _find_key((*agg_states)[i], (*not_founds)[i], key, caches[i].hashval);
                }
            } else if constexpr (HTBuildOp::allocate) {
                _emplace_key_with_hash(key, caches[i].hashval, (*agg_states)[i], allocate_func,
                                       FillNotFounds<HTBuildOp::fill_not_found>(not_founds, i));
            } else if constexpr (HTBuildOp::fill_not_found) {
                _find_key((*agg_states)[i], (*not_founds)[i], key, caches[i].hashval);
            }
        }
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    ALWAYS_NOINLINE void compute_agg_noprefetch(size_t chunk_size, const Columns& key_columns,
                                                Buffer<AggDataPtr>* agg_states, Func&& allocate_func,
                                                ExtraAggParam* extra) {
        [[maybe_unused]] size_t hash_table_size = this->hash_map.size();
        auto* __restrict not_founds = extra->not_founds;
        constexpr int key_size = sizeof(FixedSizeSliceKey);
        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        for (const auto& key_column : key_columns) {
            key_column->serialize_batch(buffer, slice_sizes, chunk_size, key_size);
        }
        auto* key = reinterpret_cast<FixedSizeSliceKey*>(caches.data());
        if (has_null_column) {
            for (size_t i = 0; i < chunk_size; ++i) {
                key[i].u.size = slice_sizes[i];
            }
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            if constexpr (HTBuildOp::process_limit) {
                if (hash_table_size < extra->limits) {
                    _emplace_key(key[i], (*agg_states)[i], allocate_func, [&] { hash_table_size++; });
                } else {
                    _find_key((*agg_states)[i], (*not_founds)[i], key[i]);
                }
            } else if constexpr (HTBuildOp::allocate) {
                _emplace_key(key[i], (*agg_states)[i], allocate_func,
                             FillNotFounds<HTBuildOp::fill_not_found>(not_founds, i));
            } else if constexpr (HTBuildOp::fill_not_found) {
                _find_key((*agg_states)[i], (*not_founds)[i], key[i]);
            }
        }
    }

    template <AllocFunc<Self> Func, typename HTBuildOp>
    void compute_agg_states(size_t chunk_size, const Columns& key_columns, MemPool* /*pool*/, Func&& allocate_func,
                            Buffer<AggDataPtr>* agg_states, ExtraAggParam* extra) {
        DCHECK(has_null_column || fixed_byte_size != -1);
        DCHECK_LE(chunk_size, caches.size());
        slice_sizes.assign(chunk_size, 0);

        auto* buffer = reinterpret_cast<uint8_t*>(caches.data());
        if (has_null_column) {
            memset(buffer, 0x0, max_fixed_size * chunk_size);
        }

        if (!agg_should_prefetch_table(this->hash_map)) {
            this->template compute_agg_noprefetch<Func, HTBuildOp>(chunk_size, key_columns, agg_states,
                                                                   std::forward<Func>(allocate_func), extra);
        } else {
            this->template compute_agg_prefetch<Func, HTBuildOp>(chunk_size, key_columns, agg_states,
                                                                 std::forward<Func>(allocate_func), extra);
        }
    }

    template <AllocFunc<Self> Func, typename EmplaceCallBack>
    ALWAYS_INLINE void _emplace_key(KeyType key, AggDataPtr& target_state, Func&& allocate_func,
                                    EmplaceCallBack&& callback) {
        auto iter = this->hash_map.lazy_emplace(key, [&](const auto& ctor) {
            callback();
            AggDataPtr pv = allocate_func(key);
            ctor(key, pv);
        });
        target_state = iter->second;
    }

    template <AllocFunc<Self> Func, typename EmplaceCallBack>
    ALWAYS_INLINE void _emplace_key_with_hash(KeyType key, size_t hash, AggDataPtr& target_state, Func&& allocate_func,
                                              EmplaceCallBack&& callback) {
        auto iter = this->hash_map.lazy_emplace_with_hash(key, hash, [&](const auto& ctor) {
            callback();
            AggDataPtr pv = allocate_func(key);
            ctor(key, pv);
        });
        target_state = iter->second;
    }

    template <typename... Args>
    ALWAYS_INLINE void _find_key(AggDataPtr& target_state, uint8_t& not_found, Args&&... args) {
        if (auto iter = this->hash_map.find(std::forward<Args>(args)...); iter != this->hash_map.end()) {
            target_state = iter->second;
        } else {
            not_found = 1;
        }
    }

    static constexpr bool has_single_null_key = false;

    Buffer<uint32_t> slice_sizes;
    ResultVector results;
    Buffer<Slice> tmp_slices;
    int32_t _chunk_size;
};

using FixedSize16SoAWrapper = AggHashMapWithSerializedKeyFixedSize<FixedSize16SliceAggHashMap<PhmapSeed1>>;
using FixedSize16AoS32BWrapper = AoSCacheEntryFixedSizeWrapper<FixedSize16SliceAggHashMap<PhmapSeed1>>;
using FixedSize16AoS24BWrapper = AoS24BCacheEntryFixedSizeWrapper<FixedSize16SliceAggHashMap<PhmapSeed1>>;
using FixedSize16MainMixedStrideWrapper = MainMixedStrideFixedSizeWrapper<FixedSize16SliceAggHashMap<PhmapSeed1>>;

using FixedSize8SoAWrapper = AggHashMapWithSerializedKeyFixedSize<FixedSize8SliceAggHashMap<PhmapSeed1>>;
using FixedSize8AoS16BWrapper = AoSCacheEntryFixedSizeWrapper<FixedSize8SliceAggHashMap<PhmapSeed1>>;
using FixedSize8MainMixedStrideWrapper = MainMixedStrideFixedSizeWrapper<FixedSize8SliceAggHashMap<PhmapSeed1>>;

using FixedSize4SoAWrapper = AggHashMapWithSerializedKeyFixedSize<FixedSize4SliceAggHashMap<PhmapSeed1>>;
using FixedSize4AoS16BWrapper = AoSCacheEntryFixedSizeWrapper<FixedSize4SliceAggHashMap<PhmapSeed1>>;
using FixedSize4MainMixedStrideWrapper = MainMixedStrideFixedSizeWrapper<FixedSize4SliceAggHashMap<PhmapSeed1>>;

using FixedSize32SoAWrapper = AggHashMapWithSerializedKeyFixedSize<FixedSize32SliceAggHashMap<PhmapSeed1>>;
using FixedSize32AoS40BWrapper = AoSCacheEntryFixedSizeWrapper<FixedSize32SliceAggHashMap<PhmapSeed1>>;
using FixedSize32MainMixedStrideWrapper = MainMixedStrideFixedSizeWrapper<FixedSize32SliceAggHashMap<PhmapSeed1>>;

static_assert(FixedSize4SoAWrapper::max_fixed_size == 4);
static_assert(FixedSize8SoAWrapper::max_fixed_size == 8);
static_assert(FixedSize16SoAWrapper::max_fixed_size == 16);
static_assert(FixedSize32SoAWrapper::max_fixed_size == 32);

static_assert(FixedSize4AoS16BWrapper::max_fixed_size == 16);
static_assert(FixedSize8AoS16BWrapper::max_fixed_size == 16);
static_assert(FixedSize16AoS32BWrapper::max_fixed_size == 32);
static_assert(FixedSize16AoS24BWrapper::max_fixed_size == 24);
static_assert(FixedSize32AoS40BWrapper::max_fixed_size == 40);

inline constexpr int64_t kFixedSizeBenchRows = 2'048'000; // 500 chunks of 4096 rows

enum class FixedKeyBenchShape {
    Q35_4xInt32_16B,
    Q8_Int64Int32_12B,
    Nullable_Int64Int32_16B,
    SubWidth_Int32Int16_6B,
    SubWidth_Int16Int8_3B,
    FullWidth_4xInt64_32B,
};

class FixedSizeKeyChunkStream {
public:
    FixedSizeKeyChunkStream(int64_t num_rows, int distinct_count, FixedKeyBenchShape shape) {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<int> uni(0, distinct_count > 0 ? distinct_count - 1 : 0);
        const int64_t num_chunks = (num_rows + kBenchChunkSize - 1) / kBenchChunkSize;
        _chunks.reserve(num_chunks);

        for (int64_t c = 0; c < num_chunks; ++c) {
            Columns cols;
            switch (shape) {
            case FixedKeyBenchShape::Q35_4xInt32_16B: {
                auto c0 = Int32Column::create();
                auto c1 = Int32Column::create();
                auto c2 = Int32Column::create();
                auto c3 = Int32Column::create();
                c0->get_data().resize(kBenchChunkSize);
                c1->get_data().resize(kBenchChunkSize);
                c2->get_data().resize(kBenchChunkSize);
                c3->get_data().resize(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    const uint32_t g = static_cast<uint32_t>(uni(rng));
                    c0->get_data()[i] = static_cast<int32_t>(g);
                    c1->get_data()[i] = static_cast<int32_t>(g * 0x9E3779B1U + 0x11223344U);
                    c2->get_data()[i] = static_cast<int32_t>(g * 0x85EBCA6BU + 0x55667788U);
                    c3->get_data()[i] = static_cast<int32_t>(g * 0xC2B2AE35U + 0x99AABBCCU);
                }
                cols.emplace_back(std::move(c0));
                cols.emplace_back(std::move(c1));
                cols.emplace_back(std::move(c2));
                cols.emplace_back(std::move(c3));
                break;
            }
            case FixedKeyBenchShape::Q8_Int64Int32_12B: {
                auto c0 = Int64Column::create();
                auto c1 = Int32Column::create();
                c0->get_data().resize(kBenchChunkSize);
                c1->get_data().resize(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    const uint64_t g = static_cast<uint64_t>(uni(rng));
                    c0->get_data()[i] = static_cast<int64_t>(g * 0x9E3779B97F4A7C15ULL + 1ULL);
                    c1->get_data()[i] = static_cast<int32_t>(static_cast<uint32_t>(g) * 0x85EBCA6BU + 2U);
                }
                cols.emplace_back(std::move(c0));
                cols.emplace_back(std::move(c1));
                break;
            }
            case FixedKeyBenchShape::Nullable_Int64Int32_16B: {
                auto d0 = Int64Column::create();
                auto n0 = NullColumn::create();
                auto d1 = Int32Column::create();
                auto n1 = NullColumn::create();
                d0->get_data().resize(kBenchChunkSize);
                n0->get_data().resize(kBenchChunkSize);
                d1->get_data().resize(kBenchChunkSize);
                n1->get_data().resize(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    const uint64_t g = static_cast<uint64_t>(uni(rng));
                    const bool null0 = (g % 20 == 0);
                    const bool null1 = (g % 25 == 0);
                    n0->get_data()[i] = null0 ? 1 : 0;
                    d0->get_data()[i] = null0 ? 0 : static_cast<int64_t>(g * 0x9E3779B97F4A7C15ULL + 1ULL);
                    n1->get_data()[i] = null1 ? 1 : 0;
                    d1->get_data()[i] = null1 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(g) * 0x85EBCA6BU + 2U);
                }
                auto col0 = NullableColumn::create(std::move(d0), std::move(n0));
                auto col1 = NullableColumn::create(std::move(d1), std::move(n1));
                col0->update_has_null();
                col1->update_has_null();
                cols.emplace_back(std::move(col0));
                cols.emplace_back(std::move(col1));
                break;
            }
            case FixedKeyBenchShape::SubWidth_Int32Int16_6B: {
                auto c0 = Int32Column::create();
                auto c1 = Int16Column::create();
                c0->get_data().resize(kBenchChunkSize);
                c1->get_data().resize(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    const uint32_t g = static_cast<uint32_t>(uni(rng));
                    c0->get_data()[i] = static_cast<int32_t>(g * 0x9E3779B1U + 1U);
                    c1->get_data()[i] = static_cast<int16_t>((g * 0x85EBU) & 0xFFFFU);
                }
                cols.emplace_back(std::move(c0));
                cols.emplace_back(std::move(c1));
                break;
            }
            case FixedKeyBenchShape::SubWidth_Int16Int8_3B: {
                auto c0 = Int16Column::create();
                auto c1 = Int8Column::create();
                c0->get_data().resize(kBenchChunkSize);
                c1->get_data().resize(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    const uint32_t g = static_cast<uint32_t>(uni(rng));
                    c0->get_data()[i] = static_cast<int16_t>(g & 0xFFFFU);
                    c1->get_data()[i] = static_cast<int8_t>((g >> 8) ^ (g & 0xFFU));
                }
                cols.emplace_back(std::move(c0));
                cols.emplace_back(std::move(c1));
                break;
            }
            case FixedKeyBenchShape::FullWidth_4xInt64_32B: {
                auto c0 = Int64Column::create();
                auto c1 = Int64Column::create();
                auto c2 = Int64Column::create();
                auto c3 = Int64Column::create();
                c0->get_data().resize(kBenchChunkSize);
                c1->get_data().resize(kBenchChunkSize);
                c2->get_data().resize(kBenchChunkSize);
                c3->get_data().resize(kBenchChunkSize);
                for (int i = 0; i < kBenchChunkSize; ++i) {
                    const uint64_t g = static_cast<uint64_t>(uni(rng));
                    c0->get_data()[i] = static_cast<int64_t>(g);
                    c1->get_data()[i] = static_cast<int64_t>(g * 0x9E3779B97F4A7C15ULL + 1ULL);
                    c2->get_data()[i] = static_cast<int64_t>(g * 0xBF58476D1CE4E5B9ULL + 2ULL);
                    c3->get_data()[i] = static_cast<int64_t>(g * 0x94D049BB133111EBULL + 3ULL);
                }
                cols.emplace_back(std::move(c0));
                cols.emplace_back(std::move(c1));
                cols.emplace_back(std::move(c2));
                cols.emplace_back(std::move(c3));
                break;
            }
            }
            _chunks.emplace_back(std::move(cols));
        }
    }

    const std::vector<Columns>& chunks() const { return _chunks; }

private:
    std::vector<Columns> _chunks;
};

template <typename Wrapper, FixedKeyBenchShape Shape, bool HasNull, int FixedByteSize>
static void RunFixedSizeBench(benchmark::State& state) {
    const int distinct = static_cast<int>(state.range(0));
    const double saved_ratio = config::agg_prefetch_l2_ratio;
    DeferOp restore_ratio([&]() { config::agg_prefetch_l2_ratio = saved_ratio; });
    // Keep distinct <= 16384 strictly in noprefetch and distinct >= 65536 in prefetch.
    config::agg_prefetch_l2_ratio = (distinct <= 16384) ? 2.0 : 1.0;

    BenchSuite suite;
    suite.SetUp();
    FixedSizeKeyChunkStream stream(kFixedSizeBenchRows, distinct, Shape);

    int64_t total_rows = 0;
    size_t final_groups = 0;
    size_t final_ht_bytes = 0;
    int prefetch_active = 0;
    int64_t cum_checksum = 0;

    for (auto _ : state) {
        state.PauseTiming();
        auto wrapper = std::make_unique<Wrapper>(kBenchChunkSize, suite._agg_stat.get());
        wrapper->has_null_column = HasNull;
        wrapper->fixed_byte_size = FixedByteSize;
        HashTableKeyAllocator allocator;
        allocator.aggregate_key_size = sizeof(int64_t);
        allocator.pool = suite._mem_pool.get();
        BenchAllocateState alloc{&allocator};
        Buffer<AggDataPtr> agg_states(kBenchChunkSize);
        state.ResumeTiming();

        for (const auto& key_columns : stream.chunks()) {
            wrapper->build_hash_map(kBenchChunkSize, key_columns, suite._mem_pool.get(), alloc, &agg_states);
            total_rows += kBenchChunkSize;
        }
        benchmark::DoNotOptimize(agg_states.data());
        benchmark::ClobberMemory();

        state.PauseTiming();
        final_groups = wrapper->hash_map.size();
        final_ht_bytes = hash_table_bytes(wrapper.get());
        prefetch_active = agg_should_prefetch_table(wrapper->hash_map) ? 1 : 0;
        int64_t cs = 0;
        for (int i = 0; i < kBenchChunkSize; ++i) {
            cs += reinterpret_cast<intptr_t>(agg_states[i]);
        }
        cum_checksum += cs;
        wrapper.reset();
        suite._mem_pool->clear();
    }
    benchmark::DoNotOptimize(cum_checksum);
    state.SetItemsProcessed(total_rows);
    state.counters["distinct"] = distinct;
    state.counters["stride_B"] = Wrapper::max_fixed_size;
    state.counters["prefetch"] = prefetch_active;
    state.counters["final_groups"] = final_groups;
    state.counters["hash_table_bytes"] = final_ht_bytes;

    suite.TearDown();
}

template <typename Wrapper>
static void BM_FixedSize16_Q35_4xInt32_NoNull(benchmark::State& state) {
    RunFixedSizeBench<Wrapper, FixedKeyBenchShape::Q35_4xInt32_16B, false, 16>(state);
}

template <typename Wrapper>
static void BM_FixedSize16_Q8_Int64Int32_NoNull(benchmark::State& state) {
    RunFixedSizeBench<Wrapper, FixedKeyBenchShape::Q8_Int64Int32_12B, false, 12>(state);
}

template <typename Wrapper>
static void BM_FixedSize16_Nullable(benchmark::State& state) {
    RunFixedSizeBench<Wrapper, FixedKeyBenchShape::Nullable_Int64Int32_16B, true, -1>(state);
}

template <typename Wrapper>
static void BM_FixedSize8_SubWidth_NoNull(benchmark::State& state) {
    RunFixedSizeBench<Wrapper, FixedKeyBenchShape::SubWidth_Int32Int16_6B, false, 6>(state);
}

template <typename Wrapper>
static void BM_FixedSize4_SubWidth_NoNull(benchmark::State& state) {
    RunFixedSizeBench<Wrapper, FixedKeyBenchShape::SubWidth_Int16Int8_3B, false, 3>(state);
}

template <typename Wrapper>
static void BM_FixedSize32_4xInt64_NoNull(benchmark::State& state) {
    RunFixedSizeBench<Wrapper, FixedKeyBenchShape::FullWidth_4xInt64_32B, false, 32>(state);
}

static void RegisterArgs_FixedSize16_4Card(::benchmark::Benchmark* b) {
    b->ArgNames({"distinct"});
    for (int distinct : {1024, 16384, 65536, 500000}) {
        b->Args({distinct});
    }
    b->Unit(benchmark::kMillisecond);
    b->Iterations(3);
}

static void RegisterArgs_FixedSize_2Card(::benchmark::Benchmark* b) {
    b->ArgNames({"distinct"});
    for (int distinct : {1024, 65536}) {
        b->Args({distinct});
    }
    b->Unit(benchmark::kMillisecond);
    b->Iterations(3);
}

// 1. BM_FixedSize16_Q35_4xInt32_NoNull (16B full-width key, noprefetch & prefetch)
BENCHMARK_TEMPLATE(BM_FixedSize16_Q35_4xInt32_NoNull, FixedSize16SoAWrapper)->Apply(RegisterArgs_FixedSize16_4Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Q35_4xInt32_NoNull, FixedSize16AoS32BWrapper)->Apply(RegisterArgs_FixedSize16_4Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Q35_4xInt32_NoNull, FixedSize16AoS24BWrapper)->Apply(RegisterArgs_FixedSize16_4Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Q35_4xInt32_NoNull, FixedSize16MainMixedStrideWrapper)
        ->Apply(RegisterArgs_FixedSize16_4Card);

// 2. BM_FixedSize16_Q8_Int64Int32_NoNull (12B sub-width key, noprefetch & prefetch)
BENCHMARK_TEMPLATE(BM_FixedSize16_Q8_Int64Int32_NoNull, FixedSize16SoAWrapper)->Apply(RegisterArgs_FixedSize16_4Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Q8_Int64Int32_NoNull, FixedSize16AoS32BWrapper)
        ->Apply(RegisterArgs_FixedSize16_4Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Q8_Int64Int32_NoNull, FixedSize16AoS24BWrapper)
        ->Apply(RegisterArgs_FixedSize16_4Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Q8_Int64Int32_NoNull, FixedSize16MainMixedStrideWrapper)
        ->Apply(RegisterArgs_FixedSize16_4Card);

// 3. BM_FixedSize16_Nullable (has_null_column = true, per-chunk memset + serialize + probe)
BENCHMARK_TEMPLATE(BM_FixedSize16_Nullable, FixedSize16SoAWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Nullable, FixedSize16AoS32BWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Nullable, FixedSize16AoS24BWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize16_Nullable, FixedSize16MainMixedStrideWrapper)->Apply(RegisterArgs_FixedSize_2Card);

// 4. BM_FixedSize8_SubWidth_NoNull (6B key in SliceKey8: 8B SoA stride vs 16B AoS stride)
BENCHMARK_TEMPLATE(BM_FixedSize8_SubWidth_NoNull, FixedSize8SoAWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize8_SubWidth_NoNull, FixedSize8AoS16BWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize8_SubWidth_NoNull, FixedSize8MainMixedStrideWrapper)
        ->Apply(RegisterArgs_FixedSize_2Card);

// 5. BM_FixedSize4_SubWidth_NoNull (3B key in SliceKey4: 4B SoA stride vs 16B AoS stride)
BENCHMARK_TEMPLATE(BM_FixedSize4_SubWidth_NoNull, FixedSize4SoAWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize4_SubWidth_NoNull, FixedSize4AoS16BWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize4_SubWidth_NoNull, FixedSize4MainMixedStrideWrapper)
        ->Apply(RegisterArgs_FixedSize_2Card);

// 6. BM_FixedSize32_4xInt64_NoNull (32B key in SliceKey32: 32B SoA stride vs 40B AoS stride)
BENCHMARK_TEMPLATE(BM_FixedSize32_4xInt64_NoNull, FixedSize32SoAWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize32_4xInt64_NoNull, FixedSize32AoS40BWrapper)->Apply(RegisterArgs_FixedSize_2Card);
BENCHMARK_TEMPLATE(BM_FixedSize32_4xInt64_NoNull, FixedSize32MainMixedStrideWrapper)
        ->Apply(RegisterArgs_FixedSize_2Card);

} // namespace starrocks

BENCHMARK_MAIN();
