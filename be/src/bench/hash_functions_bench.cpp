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

#include <base/testutil/assert.h>
#include <benchmark/benchmark.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "base/hash/hash.h"
#include "base/phmap/phmap.h"
#include "base/string/memcmp.h"
#include "bench/bench_util.h"
#include "exec/aggregate/agg_hash_map.h"
#include "exec/aggregate/agg_hash_set.h"
#include "exprs/function_context.h"
#include "exprs/hash_functions.h"
#include "exprs/time_functions.h"
#include "runtime/mem_pool.h"
#include "runtime/runtime_state.h"

namespace starrocks {

class HashFunctionsBench {
public:
    void SetUp();
    void TearDown() {}

    HashFunctionsBench(size_t num_column, size_t num_rows) : _num_column(num_column), _num_rows(num_rows) {}

    void do_bench(benchmark::State& state, size_t num_column, bool test_default_hash);

private:
    size_t _num_column = 0;
    size_t _num_rows = 0;
    Columns _columns{};
};

void HashFunctionsBench::SetUp() {
    for (int i = 0; i < _num_column; i++) {
        auto columnPtr = BenchUtil::create_random_string_column(_num_rows, 32);
        _columns.push_back(std::move(columnPtr));
    }
}

void HashFunctionsBench::do_bench(benchmark::State& state, size_t num_rows, bool test_default_hash) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    if (test_default_hash) {
        ColumnPtr result = HashFunctions::murmur_hash3_32(ctx.get(), _columns).value();
        auto column = ColumnHelper::cast_to<TYPE_INT>(result);
    } else {
        ColumnPtr result = HashFunctions::xx_hash3_64(ctx.get(), _columns).value();
        auto column = ColumnHelper::cast_to<TYPE_BIGINT>(result);
    }
}

static void BM_HashFunctions_Eval_Arg(benchmark::internal::Benchmark* b) {
    b->Args({10, true});
    b->Args({10, false});
    b->Args({100, true});
    b->Args({100, false});
    b->Args({10000, true});
    b->Args({10000, false});
    b->Args({1000000, true});
    b->Args({1000000, false});
    b->Iterations(10000);
}

static void BM_HashFunctions_Eval(benchmark::State& state) {
    size_t num_rows = state.range(0);
    bool test_default_hash = state.range(1);

    HashFunctionsBench hashFunctionsBench(1, num_rows);
    hashFunctionsBench.SetUp();

    for (auto _ : state) {
        hashFunctionsBench.do_bench(state, num_rows, test_default_hash);
    }
}

BENCHMARK(BM_HashFunctions_Eval)->Apply(BM_HashFunctions_Eval_Arg);

const static size_t kCRC32HashSeed = 0x811C9DC5;
// Custom hash functor using crc_hash_64
struct CrcHash64String {
    size_t operator()(const std::string& s) const {
        return starrocks::crc_hash_64(s.data(), static_cast<int32_t>(s.size()), kCRC32HashSeed);
    }
};

struct CrcHash64StringUnmixed {
    size_t operator()(const std::string& s) const {
        return starrocks::crc_hash_64_unmixed(s.data(), static_cast<int32_t>(s.size()), kCRC32HashSeed);
    }
};

// Generate a dataset to simulate HashSet fingerprint collisions, which can degrade HashSet performance
static std::vector<std::string> gen_32bytes_string(size_t num_elems) {
    std::vector<std::string> data;
    data.reserve(num_elems);
    for (size_t i = 0; i < num_elems; ++i) {
        char buf[33];
        snprintf(buf, sizeof(buf), "%032zu", i);
        // construct lowest 7bit conflicts
        size_t hash = starrocks::crc_hash_64_unmixed(buf, sizeof(buf), kCRC32HashSeed);
        while ((hash & 0xF) != 0) {
            buf[32]++;
            hash = starrocks::crc_hash_64_unmixed(buf, sizeof(buf), kCRC32HashSeed);
        }
        data.emplace_back(buf);
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(data.begin(), data.end(), g);

    return data;
}

template <class HashFunction>
static void BM_PhmapFlatHashSet_CrcHash64_Insert_Impl(benchmark::State& state) {
    size_t num_elems = state.range(0);
    auto data = gen_32bytes_string(num_elems);
    for (auto _ : state) {
        phmap::flat_hash_set<std::string, HashFunction> set;
        for (const auto& s : data) {
            set.insert(s);
        }
        benchmark::DoNotOptimize(set);
    }
}

static void BM_PhmapFlatHashSet_CrcHash64_Insert(benchmark::State& state) {
    BM_PhmapFlatHashSet_CrcHash64_Insert_Impl<CrcHash64String>(state);
}

static void BM_PhmapFlatHashSet_CrcHash64_Insert_Unmixed(benchmark::State& state) {
    BM_PhmapFlatHashSet_CrcHash64_Insert_Impl<CrcHash64StringUnmixed>(state);
}

template <class HashFunction>
static void BM_PhmapFlatHashSet_CrcHash64_Lookup_Impl(benchmark::State& state) {
    size_t num_elems = state.range(0);
    auto data = gen_32bytes_string(num_elems);
    // Half success, half failure
    auto lookup_data = gen_32bytes_string(num_elems * 2);

    phmap::flat_hash_set<std::string, HashFunction> set;
    for (const auto& s : data) {
        set.insert(s);
    }

    for (auto _ : state) {
        size_t found = 0;
        for (const auto& s : lookup_data) {
            found += set.count(s);
        }
        benchmark::DoNotOptimize(found);
    }
}

static void BM_PhmapFlatHashSet_CrcHash64_Lookup(benchmark::State& state) {
    BM_PhmapFlatHashSet_CrcHash64_Lookup_Impl<CrcHash64String>(state);
}

static void BM_PhmapFlatHashSet_CrcHash64_Lookup_Unmixed(benchmark::State& state) {
    BM_PhmapFlatHashSet_CrcHash64_Lookup_Impl<CrcHash64StringUnmixed>(state);
}

// 2025-06-19T15:48:27+08:00
// Running ./hash_functions_bench
// Run on (32 X 2500 MHz CPU s)
// CPU Caches:
//   L1 Data 32 KiB (x16)
//   L1 Instruction 32 KiB (x16)
//   L2 Unified 1024 KiB (x16)
//   L3 Unified 36608 KiB (x1)
// Load Average: 0.12, 0.09, 0.25
// -----------------------------------------------------------------------------------------------
// Benchmark                                                     Time             CPU   Iterations
// -----------------------------------------------------------------------------------------------
// BM_PhmapFlatHashSet_CrcHash64_Insert/10000              1098692 ns      1098642 ns          642
// BM_PhmapFlatHashSet_CrcHash64_Insert/100000            13848007 ns     13846984 ns           48
// BM_PhmapFlatHashSet_CrcHash64_Insert/1000000          552114016 ns    552078649 ns            1
// BM_PhmapFlatHashSet_CrcHash64_Insert_Unmixed/10000      1290259 ns      1290163 ns          541
// BM_PhmapFlatHashSet_CrcHash64_Insert_Unmixed/100000    18784369 ns     18783587 ns           38
// BM_PhmapFlatHashSet_CrcHash64_Insert_Unmixed/1000000  626093501 ns    626056004 ns            1
// BM_PhmapFlatHashSet_CrcHash64_Lookup/10000               605826 ns       605798 ns         1161
// BM_PhmapFlatHashSet_CrcHash64_Lookup/100000            11105350 ns     11104843 ns           63
// BM_PhmapFlatHashSet_CrcHash64_Lookup/1000000          254053820 ns    254043024 ns            3
// BM_PhmapFlatHashSet_CrcHash64_Lookup_Unmixed/10000       709504 ns       709473 ns          987
// BM_PhmapFlatHashSet_CrcHash64_Lookup_Unmixed/100000    10992399 ns     10991650 ns           63
// BM_PhmapFlatHashSet_CrcHash64_Lookup_Unmixed/1000000  254696960 ns    254686158 ns            3
BENCHMARK(BM_PhmapFlatHashSet_CrcHash64_Insert)->Arg(10 * 1000)->Arg(100 * 1000)->Arg(1000 * 1000);
BENCHMARK(BM_PhmapFlatHashSet_CrcHash64_Insert_Unmixed)->Arg(10 * 1000)->Arg(100 * 1000)->Arg(1000 * 1000);
BENCHMARK(BM_PhmapFlatHashSet_CrcHash64_Lookup)->Arg(10 * 1000)->Arg(100 * 1000)->Arg(1000 * 1000);
BENCHMARK(BM_PhmapFlatHashSet_CrcHash64_Lookup_Unmixed)->Arg(10 * 1000)->Arg(100 * 1000)->Arg(1000 * 1000);

class HourFromUnixtimeBench {
public:
    HourFromUnixtimeBench(int N = 4096)
            : N(N), min_ts(946684800), max_ts(1893456000), rng(12345), dist(min_ts, max_ts) {
        // Generate N random unix timestamps
        timestamps.reserve(N);
        for (int i = 0; i < N; ++i) {
            timestamps.push_back(dist(rng));
        }
        // Construct Column
        col = Int64Column::create();
        for (int i = 0; i < N; ++i) {
            col->append(timestamps[i]);
        }
        columns.emplace_back(col);
        // Construct context
        globals.__set_now_string("2020-01-01 00:00:00");
        globals.__set_timestamp_ms(1577836800000);
        globals.__set_time_zone("UTC");
        state = std::make_unique<starrocks::RuntimeState>(globals);
        mem_pool = std::make_unique<starrocks::MemPool>();
        fn_ctx.reset(starrocks::FunctionContext::create_context(
                state.get(), mem_pool.get(), FunctionContext::TypeDesc{}, std::vector<FunctionContext::TypeDesc>{}));
    }

    void bench_hour_from_unixtime(benchmark::State& state_bench) {
        for (auto _ : state_bench) {
            auto result = starrocks::TimeFunctions::hour_from_unixtime(fn_ctx.get(), columns).value();
            benchmark::DoNotOptimize(result);
        }
    }

    void bench_from_unixtime_extract_hour(benchmark::State& state_bench) {
        for (auto _ : state_bench) {
            auto dt_col_ptr = starrocks::TimeFunctions::from_unix_to_datetime_64(fn_ctx.get(), columns).value();
            auto dt_col = starrocks::ColumnHelper::cast_to<starrocks::TYPE_DATETIME>(dt_col_ptr);
            int64_t sum = 0;

            const auto* timestamp_col = down_cast<const TimestampColumn*>(dt_col.get());

            // Read-only benchmark access over TimestampColumn data
            for (int i = 0; i < N; ++i) {
                int year, month, day, hour, minute, second, usec;
                timestamp_col->get_data()[i].to_timestamp(&year, &month, &day, &hour, &minute, &second, &usec);
                sum += hour;
            }
            benchmark::DoNotOptimize(sum);
        }
    }

private:
    int N;
    int64_t min_ts, max_ts;
    std::mt19937_64 rng;
    std::uniform_int_distribution<int64_t> dist;
    std::vector<int64_t> timestamps;
    starrocks::Int64Column::MutablePtr col;
    starrocks::Columns columns;
    starrocks::TQueryGlobals globals;
    std::unique_ptr<starrocks::RuntimeState> state;
    std::unique_ptr<starrocks::MemPool> mem_pool;
    std::unique_ptr<starrocks::FunctionContext> fn_ctx;
};

static void BM_HourFromUnixtime(benchmark::State& state) {
    static HourFromUnixtimeBench suite;
    suite.bench_hour_from_unixtime(state);
}

static void BM_FromUnixtime_HourExtract(benchmark::State& state) {
    static HourFromUnixtimeBench suite;
    suite.bench_from_unixtime_extract_hour(state);
}

// Run on (104 X 3200 MHz CPU s)
// CPU Caches:
//   L1 Data 32 KiB (x52)
//   L1 Instruction 32 KiB (x52)
//   L2 Unified 1024 KiB (x52)
//   L3 Unified 36608 KiB (x2)
// Load Average: 36.03, 17.40, 21.87
// ----------------------------------------------------------------------
// Benchmark                            Time             CPU   Iterations
// ----------------------------------------------------------------------
// BM_HourFromUnixtime             147895 ns       147841 ns         4147
// BM_FromUnixtime_HourExtract     656650 ns       651969 ns         1445
BENCHMARK(BM_HourFromUnixtime);
BENCHMARK(BM_FromUnixtime_HourExtract);

static void BM_CrcHash64_StringLengths(benchmark::State& state) {
    const int len = static_cast<int>(state.range(0));
    std::vector<std::string> strs(1024);
    for (int i = 0; i < 1024; ++i) {
        strs[i] = std::string(len + (i & 3), static_cast<char>(i));
    }
    for (auto _ : state) {
        uint64_t acc = 0;
        for (const auto& s : strs) {
            acc ^= crc_hash_64(s.data(), static_cast<int32_t>(s.size()), kCRC32HashSeed);
        }
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_CrcHash64_StringLengths)->Arg(3)->Arg(8)->Arg(16)->Arg(24)->Arg(32)->Arg(64);

static void BM_Memequal_StringLengths(benchmark::State& state) {
    const int len = static_cast<int>(state.range(0));
    std::vector<std::string> a(1024), b(1024);
    for (int i = 0; i < 1024; ++i) {
        a[i] = std::string(len + (i & 3), static_cast<char>(i + 1));
        b[i] = a[i];
    }
    for (auto _ : state) {
        int eq = 0;
        for (int i = 0; i < 1024; ++i) {
            eq += memequal(a[i].data(), a[i].size(), b[i].data(), b[i].size());
        }
        benchmark::DoNotOptimize(eq);
    }
}
BENCHMARK(BM_Memequal_StringLengths)->Arg(3)->Arg(7)->Arg(12)->Arg(20)->Arg(32)->Arg(64);

static void BM_Memcompare_StringLengths(benchmark::State& state) {
    const int len = static_cast<int>(state.range(0));
    std::vector<std::string> a(1024), b(1024);
    for (int i = 0; i < 1024; ++i) {
        const int l = len + (i & 1);
        a[i] = std::string(l, 'a');
        b[i] = a[i];
        b[i][l - 1] = 'b';
    }
    for (auto _ : state) {
        int acc = 0;
        for (int i = 0; i < 1024; ++i) {
            acc += memcompare(a[i].data(), a[i].size(), b[i].data(), b[i].size());
        }
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_Memcompare_StringLengths)->Arg(4)->Arg(12)->Arg(24)->Arg(64);

static void BM_SliceAggHashSet_Insert(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    std::vector<std::string> pool(n);
    for (int i = 0; i < n; ++i) {
        pool[i] = "str_key_value_" + std::to_string(i);
    }
    MemPool scratch;
    for (auto _ : state) {
        SliceAggHashSet<PhmapSeed1> set;
        for (int i = 0; i < n; ++i) {
            Slice s(pool[i].data(), pool[i].size());
            set.insert(make_agg_slice_key<SliceWithHash32<PhmapSeed1>>(s, &scratch));
        }
        benchmark::DoNotOptimize(set);
    }
}
// ----------------------------------------------------------------------------
// Benchmark                                  Time             CPU   Iterations
// ----------------------------------------------------------------------------
// BM_CrcHash64_StringLengths/3            1658 ns         1653 ns        37070
// BM_CrcHash64_StringLengths/8             934 ns          930 ns        80444
// BM_CrcHash64_StringLengths/16           1107 ns         1102 ns        55404
// BM_CrcHash64_StringLengths/24           1297 ns         1286 ns        54481
// BM_CrcHash64_StringLengths/32           1665 ns         1663 ns        47354
// BM_CrcHash64_StringLengths/64           2140 ns         2139 ns        32986
// BM_Memequal_StringLengths/3              795 ns          795 ns        87123
// BM_Memequal_StringLengths/7              722 ns          722 ns        98285
// BM_Memequal_StringLengths/12             705 ns          705 ns        96428
// BM_Memequal_StringLengths/20             940 ns          940 ns        76030
// BM_Memequal_StringLengths/32            1137 ns         1137 ns        61690
// BM_Memequal_StringLengths/64            2073 ns         2068 ns        40246
// BM_Memcompare_StringLengths/4            956 ns          956 ns        69325
// BM_Memcompare_StringLengths/12          1010 ns         1007 ns        70625
// BM_Memcompare_StringLengths/24          1875 ns         1875 ns        34972
// BM_Memcompare_StringLengths/64          2339 ns         2338 ns        29856
// BM_SliceAggHashSet_Insert/1000         20874 ns        20868 ns         3416
// BM_SliceAggHashSet_Insert/100000     2056348 ns      2055800 ns           34
// BM_SliceAggHashSet_Insert/1000000   74175602 ns     74170807 ns            1
BENCHMARK(BM_SliceAggHashSet_Insert)->Arg(1000)->Arg(100000)->Arg(1000000);

template <typename KeyMaker>
static auto gen_agg_bench_keys(size_t distinct_keys, bool is_zipf, KeyMaker&& make_key) {
    using Key = decltype(make_key(size_t{0}));
    std::vector<Key> keys;
    keys.reserve(distinct_keys);
    uint64_t rng_state = 0x123456789ABCDEF0ULL;
    auto next_u64 = [&]() -> uint64_t {
        uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };

    if (!is_zipf) {
        for (size_t i = 0; i < distinct_keys; ++i) {
            keys.push_back(make_key(i));
        }
    } else {
        constexpr size_t kTableSize = 4096;
        std::vector<double> inv_cdf(kTableSize + 1);
        const double log_n = std::log(static_cast<double>(distinct_keys + 1));
        for (size_t j = 0; j <= kTableSize; ++j) {
            double u = static_cast<double>(j) / static_cast<double>(kTableSize);
            inv_cdf[j] = std::exp(u * log_n) - 1.0;
        }
        for (size_t i = 0; i < distinct_keys; ++i) {
            uint64_t r = next_u64();
            size_t idx = r & (kTableSize - 1);
            double frac = static_cast<double>((r >> 12) & 0xFFFFFULL) * (1.0 / 1048576.0);
            auto rank = static_cast<size_t>(inv_cdf[idx] + frac * (inv_cdf[idx + 1] - inv_cdf[idx]));
            if (rank >= distinct_keys) {
                rank = distinct_keys - 1;
            }
            keys.push_back(make_key(rank));
        }
    }
    return keys;
}

template <typename HashMap, typename KeyMaker>
static void run_agg_single_level_baseline(benchmark::State& state, KeyMaker&& make_key) {
    const auto distinct_keys = static_cast<size_t>(state.range(0));
    const bool is_zipf = state.range(1) != 0;
    auto keys = gen_agg_bench_keys(distinct_keys, is_zipf, std::forward<KeyMaker>(make_key));

    size_t table_bytes = 0;
    for (auto _ : state) {
        HashMap map;
        AggDataPtr dummy_state = nullptr;
        for (const auto& k : keys) {
            map.lazy_emplace(k, [&](const auto& ctor) { ctor(k, dummy_state); });
        }
        table_bytes = map.dump_bound();
        benchmark::DoNotOptimize(map);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(keys.size()));
    state.counters["table_bytes"] = static_cast<double>(table_bytes);
}

static void BM_AggSingleLevel_Baseline_Args(benchmark::Benchmark* b) {
    for (int64_t distinct_keys : {10000, 100000, 1000000, 10000000}) {
        for (int64_t is_zipf : {0, 1}) {
            b->Args({distinct_keys, is_zipf});
        }
    }
    b->MinTime(0.05);
}

static void BM_AggSingleLevel_Int32_Baseline(benchmark::State& state) {
    run_agg_single_level_baseline<Int32AggHashMap<PhmapSeed1>>(
            state, [](size_t i) -> int32_t { return static_cast<int32_t>(i); });
}

static void BM_AggSingleLevel_Int64_Baseline(benchmark::State& state) {
    run_agg_single_level_baseline<Int64AggHashMap<PhmapSeed1>>(
            state, [](size_t i) -> int64_t { return static_cast<int64_t>(i); });
}

static void BM_AggSingleLevel_FixedSize8_Baseline(benchmark::State& state) {
    run_agg_single_level_baseline<FixedSize8SliceAggHashMap<PhmapSeed1>>(state, [](size_t i) -> SliceKey8 {
        SliceKey8 k{};
        k.u.value = static_cast<int64_t>(i);
        return k;
    });
}

static void BM_AggSingleLevel_FixedSize16_Baseline(benchmark::State& state) {
    run_agg_single_level_baseline<FixedSize16SliceAggHashMap<PhmapSeed1>>(state, [](size_t i) -> SliceKey16 {
        SliceKey16 k{};
        k.u.ui64[0] = static_cast<uint64_t>(i);
        k.u.ui64[1] = static_cast<uint64_t>(i) ^ 0x9E3779B97F4A7C15ULL;
        return k;
    });
}

BENCHMARK(BM_AggSingleLevel_Int32_Baseline)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggSingleLevel_Int64_Baseline)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggSingleLevel_FixedSize8_Baseline)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggSingleLevel_FixedSize16_Baseline)->Apply(BM_AggSingleLevel_Baseline_Args);

static void BM_AggSlice17To32B_Baseline_Args(benchmark::Benchmark* b) {
    for (int64_t key_bytes : {20, 32}) {
        for (int64_t distinct_keys : {10000, 100000, 1000000}) {
            b->Args({key_bytes, distinct_keys});
        }
    }
    b->MinTime(0.05);
}

static void BM_AggSlice17To32B_Baseline(benchmark::State& state) {
    const auto key_bytes = static_cast<size_t>(state.range(0));
    const auto distinct_keys = static_cast<size_t>(state.range(1));

    std::vector<char> raw_keys(distinct_keys * key_bytes + SLICE_MEMEQUAL_OVERFLOW_PADDING, 0);
    for (size_t i = 0; i < distinct_keys; ++i) {
        char* dst = raw_keys.data() + i * key_bytes;
        uint64_t lo = static_cast<uint64_t>(i);
        uint64_t hi = static_cast<uint64_t>(i) ^ 0x9E3779B97F4A7C15ULL;
        memcpy(dst, &lo, sizeof(lo));
        memcpy(dst + 8, &hi, sizeof(hi));
        uint64_t tail = static_cast<uint64_t>(i) * 0xBF58476D1CE4E5B9ULL;
        memcpy(dst + 16, &tail, std::min<size_t>(sizeof(tail), key_bytes - 16));
    }

    size_t pool_bytes = 0;
    size_t table_bytes = 0;
    for (auto _ : state) {
        MemPool pool;
        MemPool escape_pool;
        SliceAggHashMap<PhmapSeed1> map;
        AggDataPtr dummy_state = nullptr;
        for (size_t i = 0; i < distinct_keys; ++i) {
            Slice s(raw_keys.data() + i * key_bytes, key_bytes);
            const auto key = make_agg_slice_key<SliceWithHash32<PhmapSeed1>>(s, &escape_pool);
            map.lazy_emplace(key, [&](const auto& ctor) {
                auto pk = persist_agg_slice_key(key, &pool);
                ctor(pk, dummy_state);
            });
        }
        pool_bytes = static_cast<size_t>(pool.total_allocated_bytes());
        table_bytes = map.dump_bound();
        benchmark::DoNotOptimize(map);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(distinct_keys));
    state.counters["pool_bytes"] = static_cast<double>(pool_bytes);
    state.counters["table_bytes"] = static_cast<double>(table_bytes);
}

BENCHMARK(BM_AggSlice17To32B_Baseline)->Apply(BM_AggSlice17To32B_Baseline_Args);

template <typename SingleHashMap, typename TwoLevelHashMap, typename KeyMaker>
static void run_agg_two_level(benchmark::State& state, KeyMaker&& make_key) {
    const auto distinct_keys = static_cast<size_t>(state.range(0));
    const bool is_zipf = state.range(1) != 0;
    auto keys = gen_agg_bench_keys(distinct_keys, is_zipf, std::forward<KeyMaker>(make_key));

    constexpr size_t kChunkSize = 4096;
    constexpr size_t kTwoLevelByteThreshold = 4 * 1024 * 1024;
    constexpr size_t kTwoLevelSizeThreshold = 65536;

    size_t table_bytes = 0;
    size_t converted_count = 0;
    for (auto _ : state) {
        SingleHashMap single_map;
        TwoLevelHashMap two_level_map;
        bool is_two_level = false;
        AggDataPtr dummy_state = nullptr;

        for (size_t offset = 0; offset < keys.size(); offset += kChunkSize) {
            const size_t end = std::min(offset + kChunkSize, keys.size());
            if (!is_two_level) {
                for (size_t i = offset; i < end; ++i) {
                    const auto& k = keys[i];
                    single_map.lazy_emplace(k, [&](const auto& ctor) { ctor(k, dummy_state); });
                }
                if (single_map.size() >= kTwoLevelSizeThreshold || single_map.dump_bound() > kTwoLevelByteThreshold) {
                    two_level_map.reserve(single_map.capacity());
                    two_level_map.insert(single_map.begin(), single_map.end());
                    single_map = SingleHashMap();
                    is_two_level = true;
                }
            } else {
                for (size_t i = offset; i < end; ++i) {
                    const auto& k = keys[i];
                    two_level_map.lazy_emplace(k, [&](const auto& ctor) { ctor(k, dummy_state); });
                }
            }
        }
        table_bytes = is_two_level ? two_level_map.dump_bound() : single_map.dump_bound();
        converted_count = is_two_level ? 1 : 0;
        benchmark::DoNotOptimize(single_map);
        benchmark::DoNotOptimize(two_level_map);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(keys.size()));
    state.counters["table_bytes"] = static_cast<double>(table_bytes);
    state.counters["is_two_level"] = static_cast<double>(converted_count);
}

static void BM_AggTwoLevel_Int32(benchmark::State& state) {
    run_agg_two_level<Int32AggHashMap<PhmapSeed1>, Int32AggTwoLevelHashMap<PhmapSeed1>>(
            state, [](size_t i) -> int32_t { return static_cast<int32_t>(i); });
}

static void BM_AggTwoLevel_Int64(benchmark::State& state) {
    run_agg_two_level<Int64AggHashMap<PhmapSeed1>, Int64AggTwoLevelHashMap<PhmapSeed1>>(
            state, [](size_t i) -> int64_t { return static_cast<int64_t>(i); });
}

static void BM_AggTwoLevel_FixedSize8(benchmark::State& state) {
    run_agg_two_level<FixedSize8SliceAggHashMap<PhmapSeed1>, FixedSize8SliceAggTwoLevelHashMap<PhmapSeed1>>(
            state, [](size_t i) -> SliceKey8 {
                SliceKey8 k{};
                k.u.value = static_cast<int64_t>(i);
                return k;
            });
}

static void BM_AggTwoLevel_FixedSize16(benchmark::State& state) {
    run_agg_two_level<FixedSize16SliceAggHashMap<PhmapSeed1>, FixedSize16SliceAggTwoLevelHashMap<PhmapSeed1>>(
            state, [](size_t i) -> SliceKey16 {
                SliceKey16 k{};
                k.u.ui64[0] = static_cast<uint64_t>(i);
                k.u.ui64[1] = static_cast<uint64_t>(i) ^ 0x9E3779B97F4A7C15ULL;
                return k;
            });
}

static void BM_AggTwoLevel_FixedSize32(benchmark::State& state) {
    run_agg_two_level<FixedSize32SliceAggHashMap<PhmapSeed1>, FixedSize32SliceAggTwoLevelHashMap<PhmapSeed1>>(
            state, [](size_t i) -> SliceKey32 {
                SliceKey32 k{};
                k.u.ui64[0] = static_cast<uint64_t>(i);
                k.u.ui64[1] = static_cast<uint64_t>(i) ^ 0x9E3779B97F4A7C15ULL;
                k.u.ui64[2] = static_cast<uint64_t>(i) * 0xBF58476D1CE4E5B9ULL;
                k.u.ui64[3] = static_cast<uint64_t>(i) ^ 0x94D049BB133111EBULL;
                return k;
            });
}

BENCHMARK(BM_AggTwoLevel_Int32)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggTwoLevel_Int64)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggTwoLevel_FixedSize8)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggTwoLevel_FixedSize16)->Apply(BM_AggSingleLevel_Baseline_Args);
BENCHMARK(BM_AggTwoLevel_FixedSize32)->Apply(BM_AggSingleLevel_Baseline_Args);

struct SliceKey32WordMixHash {
    std::size_t operator()(const SliceKey32& s) const {
        int256_t val;
        memcpy(static_cast<void*>(&val), &s, sizeof(SliceKey32));
        return Hash256WithSeed<PhmapSeed1>()(val);
    }
};

static void BM_SliceKey32_CRC32C_vs_WordMix_Args(benchmark::Benchmark* b) {
    for (int64_t use_word_mix : {0, 1}) {
        for (int64_t num_keys : {10000, 100000, 1000000}) {
            b->Args({use_word_mix, num_keys});
        }
    }
    b->MinTime(0.05);
}

static void BM_SliceKey32_CRC32C_vs_WordMix(benchmark::State& state) {
    const bool use_word_mix = state.range(0) != 0;
    const auto num_keys = static_cast<size_t>(state.range(1));
    auto keys = gen_agg_bench_keys(num_keys, false, [](size_t i) -> SliceKey32 {
        SliceKey32 k{};
        k.u.ui64[0] = static_cast<uint64_t>(i);
        k.u.ui64[1] = static_cast<uint64_t>(i) ^ 0x9E3779B97F4A7C15ULL;
        k.u.ui64[2] = static_cast<uint64_t>(i) * 0xBF58476D1CE4E5B9ULL;
        k.u.ui64[3] = static_cast<uint64_t>(i) ^ 0x94D049BB133111EBULL;
        return k;
    });

    if (!use_word_mix) {
        FixedSizeSliceKeyHash<SliceKey32, PhmapSeed1> hasher;
        for (auto _ : state) {
            size_t acc = 0;
            for (const auto& k : keys) {
                acc ^= hasher(k);
            }
            benchmark::DoNotOptimize(acc);
        }
    } else {
        SliceKey32WordMixHash hasher;
        for (auto _ : state) {
            size_t acc = 0;
            for (const auto& k : keys) {
                acc ^= hasher(k);
            }
            benchmark::DoNotOptimize(acc);
        }
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(keys.size()));
}

BENCHMARK(BM_SliceKey32_CRC32C_vs_WordMix)->Apply(BM_SliceKey32_CRC32C_vs_WordMix_Args);

static void BM_FixedSize32_vs_GenericSlice_Q35_Args(benchmark::Benchmark* b) {
    for (int64_t use_fixed_size32 : {0, 1}) {
        for (int64_t key_bytes : {20, 32}) {
            for (int64_t distinct_keys : {10000, 100000, 1000000}) {
                b->Args({use_fixed_size32, key_bytes, distinct_keys});
            }
        }
    }
    b->MinTime(0.05);
}

static void BM_FixedSize32_vs_GenericSlice_Q35(benchmark::State& state) {
    const bool use_fixed_size32 = state.range(0) != 0;
    const auto key_bytes = static_cast<size_t>(state.range(1));
    const auto distinct_keys = static_cast<size_t>(state.range(2));

    std::vector<char> raw_keys(distinct_keys * key_bytes + SLICE_MEMEQUAL_OVERFLOW_PADDING, 0);
    for (size_t i = 0; i < distinct_keys; ++i) {
        char* dst = raw_keys.data() + i * key_bytes;
        uint64_t lo = static_cast<uint64_t>(i);
        uint64_t hi = static_cast<uint64_t>(i) ^ 0x9E3779B97F4A7C15ULL;
        memcpy(dst, &lo, sizeof(lo));
        memcpy(dst + 8, &hi, sizeof(hi));
        uint64_t tail = static_cast<uint64_t>(i) * 0xBF58476D1CE4E5B9ULL;
        memcpy(dst + 16, &tail, std::min<size_t>(sizeof(tail), key_bytes - 16));
    }

    size_t pool_bytes = 0;
    size_t table_bytes = 0;
    if (!use_fixed_size32) {
        for (auto _ : state) {
            MemPool pool;
            MemPool escape_pool;
            SliceAggHashMap<PhmapSeed1> map;
            AggDataPtr dummy_state = nullptr;
            for (size_t i = 0; i < distinct_keys; ++i) {
                Slice s(raw_keys.data() + i * key_bytes, key_bytes);
                const auto key = make_agg_slice_key<SliceWithHash32<PhmapSeed1>>(s, &escape_pool);
                map.lazy_emplace(key, [&](const auto& ctor) {
                    auto pk = persist_agg_slice_key(key, &pool);
                    ctor(pk, dummy_state);
                });
            }
            pool_bytes = static_cast<size_t>(pool.total_allocated_bytes());
            table_bytes = map.dump_bound();
            benchmark::DoNotOptimize(map);
        }
    } else {
        for (auto _ : state) {
            FixedSize32SliceAggHashMap<PhmapSeed1> map;
            AggDataPtr dummy_state = nullptr;
            for (size_t i = 0; i < distinct_keys; ++i) {
                SliceKey32 key{};
                memcpy(key.u.data, raw_keys.data() + i * key_bytes, key_bytes);
                if (key_bytes < sizeof(SliceKey32)) {
                    key.u.size = static_cast<uint8_t>(key_bytes);
                }
                map.lazy_emplace(key, [&](const auto& ctor) { ctor(key, dummy_state); });
            }
            pool_bytes = 0;
            table_bytes = map.dump_bound();
            benchmark::DoNotOptimize(map);
        }
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(distinct_keys));
    state.counters["pool_bytes"] = static_cast<double>(pool_bytes);
    state.counters["table_bytes"] = static_cast<double>(table_bytes);
}

BENCHMARK(BM_FixedSize32_vs_GenericSlice_Q35)->Apply(BM_FixedSize32_vs_GenericSlice_Q35_Args);

} // namespace starrocks

BENCHMARK_MAIN();
