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

// Microbenchmarks for the column-ops SIMD paths shipping in PR #73289.
//
// Each pair benches the inline pre-PR scalar form against the post-PR SIMD
// form on identical inputs.  Run twice (USE_AVX512=OFF / ON) and report
// both columns: a keepable optimization must beat or match scalar in both.
//
// Covers (one or two pairs per file touched):
//
//   * column.cpp                     -- empty_null_in_complex_column
//                                       (BreakEarly + FullSweep scenarios)
//   * chunk.cpp                      -- Chunk::filter all-ones short-circuit
//   * fixed_length_column_base.cpp   -- replicate() (const + variable fill)
//                                       and fill_default pointer-hoist
//   * append_with_mask.cpp           -- append_with_mask (FixedLength & Binary,
//                                       alternating & clustered patterns)
//   * simd.h                         -- count_zero on selection vectors
//
// Out of scope here:
//   * column_hash.cpp boundary detection -- regression confirmed, dropping.
//   * column_helper.cpp bytewise AND/OR/ANDN -- already scalar in prod.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#ifdef __AVX2__
#include <immintrin.h>
#endif

#include "base/simd/filter.h"
#include "base/simd/simd.h"
#include "base/simd/simd_utils.h"
#include "column/append_with_mask.h"
#include "column/binary_column.h"
#include "column/fixed_length_column.h"
#include "column/simd_mulselector.h"

namespace starrocks {

constexpr size_t kChunk = 4096;

static void fill_byte_mask(uint8_t* data, size_t n, int one_ratio_percent, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> d(0, 99);
    for (size_t i = 0; i < n; ++i) {
        data[i] = (d(rng) < one_ratio_percent) ? 1 : 0;
    }
}

// =====================================================================
// column.cpp :: empty_null_in_complex_column
// =====================================================================
//
// Pre-PR:  byte-by-byte `if (null_data[i] && offsets[i+1] != offsets[i])`
//          with early-exit on first hit.
// Post-PR (AVX2): pack 32 null bytes via cmpgt+movemask, walk only set
//          bits via ctz, identical early-exit semantics.
//
// Two scenarios drive the realistic spread:
//   * BreakEarly -- every row non-empty (offsets[i+1] > offsets[i]).
//                   First null triggers break.  Measures "wasted SIMD
//                   load on small input" worst case.
//   * FullSweep  -- null rows have empty offsets (no offset violation),
//                   so break never triggers.  Real hot path for
//                   `std::optional<varchar>` columns where nulls are
//                   stored as zero-length slices.

static void scan_nulls_scalar(const uint8_t* null_data, const uint32_t* offsets, size_t size, bool& need_empty) {
    for (size_t i = 0; i < size && !need_empty; ++i) {
        if (null_data[i] && offsets[i + 1] != offsets[i]) {
            need_empty = true;
        }
    }
}

#if defined(__AVX2__)
static void scan_nulls_avx2(const uint8_t* null_data, const uint32_t* offsets, size_t size, bool& need_empty) {
    const __m256i zero = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 32 <= size && !need_empty; i += 32) {
        __m256i v_null = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(null_data + i));
        uint32_t null_mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpgt_epi8(v_null, zero)));
        while (null_mask && !need_empty) {
            uint32_t bit_pos = __builtin_ctz(null_mask);
            size_t idx = i + bit_pos;
            if (offsets[idx + 1] != offsets[idx]) {
                need_empty = true;
            }
            null_mask &= null_mask - 1;
        }
    }
    for (; i < size && !need_empty; ++i) {
        if (null_data[i] && offsets[i + 1] != offsets[i]) {
            need_empty = true;
        }
    }
}
#endif

enum NullScanScenario { kBreakEarly = 0, kFullSweep = 1 };

template <bool simd, NullScanScenario scenario>
static void do_NullScan(benchmark::State& state) {
    const int null_pct = state.range(0);
    std::vector<uint8_t> null_data(kChunk);
    std::vector<uint32_t> offsets(kChunk + 1);
    fill_byte_mask(null_data.data(), kChunk, null_pct, 0xBEEF);
    offsets[0] = 0;
    for (size_t i = 0; i < kChunk; ++i) {
        // BreakEarly: every row non-empty -> first null breaks loop.
        // FullSweep:  null rows empty, non-null rows non-empty -> break
        //             never triggers, full array is scanned.
        const bool empty = (scenario == kFullSweep) && (null_data[i] != 0);
        offsets[i + 1] = offsets[i] + (empty ? 0 : 4);
    }
    for (auto _ : state) {
        bool need_empty = false;
        if constexpr (simd) {
#if defined(__AVX2__)
            scan_nulls_avx2(null_data.data(), offsets.data(), kChunk, need_empty);
#else
            scan_nulls_scalar(null_data.data(), offsets.data(), kChunk, need_empty);
#endif
        } else {
            scan_nulls_scalar(null_data.data(), offsets.data(), kChunk, need_empty);
        }
        benchmark::DoNotOptimize(need_empty);
    }
}

BENCHMARK_TEMPLATE(do_NullScan, false, kBreakEarly)
        ->Name("BM_NullScan_BreakEarly_Scalar")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);
BENCHMARK_TEMPLATE(do_NullScan, true, kBreakEarly)
        ->Name("BM_NullScan_BreakEarly_SIMD")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);
BENCHMARK_TEMPLATE(do_NullScan, false, kFullSweep)
        ->Name("BM_NullScan_FullSweep_Scalar")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);
BENCHMARK_TEMPLATE(do_NullScan, true, kFullSweep)
        ->Name("BM_NullScan_FullSweep_SIMD")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);

// =====================================================================
// chunk.cpp :: Chunk::filter all-ones short-circuit
// =====================================================================
//
// Pre-PR:  `SIMD::count_zero(selection) == 0` -- always full scan.
// Post-PR: `SIMD::all_ones(selection)` -- memchr early-exits at first 0.

static void BM_AllOnes_CountZero(benchmark::State& state) {
    const int zero_pct = state.range(0);
    std::vector<uint8_t> sel(kChunk);
    fill_byte_mask(sel.data(), kChunk, 100 - zero_pct, 0xCAFE);
    for (auto _ : state) {
        bool all_ones = SIMD::count_zero(sel) == 0;
        benchmark::DoNotOptimize(all_ones);
    }
}

static void BM_AllOnes_Memchr(benchmark::State& state) {
    const int zero_pct = state.range(0);
    std::vector<uint8_t> sel(kChunk);
    fill_byte_mask(sel.data(), kChunk, 100 - zero_pct, 0xCAFE);
    for (auto _ : state) {
        bool all_ones = SIMD::all_ones(sel);
        benchmark::DoNotOptimize(all_ones);
    }
}

BENCHMARK(BM_AllOnes_CountZero)->Arg(0)->Arg(1)->Arg(10)->Arg(50);
BENCHMARK(BM_AllOnes_Memchr)->Arg(0)->Arg(1)->Arg(10)->Arg(50);

// =====================================================================
// fixed_length_column_base.cpp :: replicate() inner fill
// =====================================================================
//
// Pre-PR:  nested loop `dest[j] = src[i]` for j in [offsets[i], offsets[i+1]).
// Post-PR: `SIMDUtils::simd_fill<T>` (== `std::fill_n`; compiler emits
//          broadcast-store, see PR #73287 commit body).
//
// Two scenarios:
//   * Const     -- constant fill_count across all source rows.  Synthetic,
//                  but isolates "long fill loop unrolling" cleanly.
//   * Variable  -- fill_count uniform in [0, max].  Mirrors map/array
//                  replicate where source rows have varying group sizes.

template <typename T>
static void replicate_scalar(T* dest, const T* src, const uint32_t* offsets, size_t orig_size) {
    for (size_t i = 0; i < orig_size; ++i) {
        for (uint32_t j = offsets[i]; j < offsets[i + 1]; ++j) {
            dest[j] = src[i];
        }
    }
}

template <typename T>
static void replicate_simd_fill(T* dest, const T* src, const uint32_t* offsets, size_t orig_size) {
    for (size_t i = 0; i < orig_size; ++i) {
        size_t fill_count = offsets[i + 1] - offsets[i];
        SIMDUtils::simd_fill(dest + offsets[i], src[i], fill_count);
    }
}

constexpr size_t kSrcRows = 256;

template <typename T, void (*Impl)(T*, const T*, const uint32_t*, size_t)>
static void do_Replicate_Const(benchmark::State& state) {
    const uint32_t fill = static_cast<uint32_t>(state.range(0));
    std::vector<T> src(kSrcRows, T{42});
    std::vector<uint32_t> offsets(kSrcRows + 1);
    for (size_t i = 0; i <= kSrcRows; ++i) offsets[i] = static_cast<uint32_t>(i) * fill;
    std::vector<T> dest(static_cast<size_t>(kSrcRows) * fill + 1);
    for (auto _ : state) {
        Impl(dest.data(), src.data(), offsets.data(), kSrcRows);
        benchmark::DoNotOptimize(dest.data());
    }
}

template <typename T, void (*Impl)(T*, const T*, const uint32_t*, size_t)>
static void do_Replicate_Variable(benchmark::State& state) {
    const uint32_t max_fill = static_cast<uint32_t>(state.range(0));
    std::vector<T> src(kSrcRows, T{42});
    std::vector<uint32_t> offsets(kSrcRows + 1);
    std::mt19937_64 rng(0xFAB1E);
    std::uniform_int_distribution<uint32_t> d(0, max_fill);
    offsets[0] = 0;
    for (size_t i = 0; i < kSrcRows; ++i) offsets[i + 1] = offsets[i] + d(rng);
    std::vector<T> dest(offsets.back() + 1);
    for (auto _ : state) {
        Impl(dest.data(), src.data(), offsets.data(), kSrcRows);
        benchmark::DoNotOptimize(dest.data());
    }
}

BENCHMARK_TEMPLATE(do_Replicate_Const, int32_t, replicate_scalar<int32_t>)
        ->Name("BM_Replicate_Const_Int32_Scalar")
        ->Arg(1)
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);
BENCHMARK_TEMPLATE(do_Replicate_Const, int32_t, replicate_simd_fill<int32_t>)
        ->Name("BM_Replicate_Const_Int32_SIMD")
        ->Arg(1)
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);
BENCHMARK_TEMPLATE(do_Replicate_Const, int64_t, replicate_scalar<int64_t>)
        ->Name("BM_Replicate_Const_Int64_Scalar")
        ->Arg(1)
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);
BENCHMARK_TEMPLATE(do_Replicate_Const, int64_t, replicate_simd_fill<int64_t>)
        ->Name("BM_Replicate_Const_Int64_SIMD")
        ->Arg(1)
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);

BENCHMARK_TEMPLATE(do_Replicate_Variable, int32_t, replicate_scalar<int32_t>)
        ->Name("BM_Replicate_Var_Int32_Scalar")
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);
BENCHMARK_TEMPLATE(do_Replicate_Variable, int32_t, replicate_simd_fill<int32_t>)
        ->Name("BM_Replicate_Var_Int32_SIMD")
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);
BENCHMARK_TEMPLATE(do_Replicate_Variable, int64_t, replicate_scalar<int64_t>)
        ->Name("BM_Replicate_Var_Int64_Scalar")
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);
BENCHMARK_TEMPLATE(do_Replicate_Variable, int64_t, replicate_simd_fill<int64_t>)
        ->Name("BM_Replicate_Var_Int64_SIMD")
        ->Arg(4)
        ->Arg(16)
        ->Arg(64);

// =====================================================================
// fixed_length_column_base.cpp :: fill_default pointer-hoist
// =====================================================================
//
// Pre-PR:  for (i; i < filter.size(); i++) if (filter[i]==1) datas[i] = val;
// Post-PR: cache size/data/filter pointers in locals, otherwise identical.
//
// The "post-PR" form is a plain pointer hoist, not SIMD.  This bench
// shows whether gcc already hoists the loop-invariant `data()` calls
// (it should -- both forms should be identical at -O3) so we can decide
// whether the change belongs in a SIMD PR at all.

template <typename T>
static void fill_default_no_hoist(std::vector<T>& datas, const std::vector<uint8_t>& filter, T val) {
    for (size_t i = 0; i < filter.size(); i++) {
        if (filter[i] == 1) {
            datas[i] = val;
        }
    }
}

template <typename T>
static void fill_default_hoist(std::vector<T>& datas, const std::vector<uint8_t>& filter, T val) {
    const size_t size = filter.size();
    const uint8_t* f = filter.data();
    T* data = datas.data();
    for (size_t i = 0; i < size; i++) {
        if (f[i] == 1) data[i] = val;
    }
}

template <typename T, void (*Impl)(std::vector<T>&, const std::vector<uint8_t>&, T)>
static void do_FillDefault(benchmark::State& state) {
    const int filter_pct = state.range(0);
    std::vector<T> datas(kChunk, T{0});
    std::vector<uint8_t> filter(kChunk);
    fill_byte_mask(filter.data(), kChunk, filter_pct, 0xF111);
    for (auto _ : state) {
        Impl(datas, filter, T{42});
        benchmark::DoNotOptimize(datas.data());
    }
}

BENCHMARK_TEMPLATE(do_FillDefault, int32_t, fill_default_no_hoist<int32_t>)
        ->Name("BM_FillDefault_Int32_NoHoist")
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);
BENCHMARK_TEMPLATE(do_FillDefault, int32_t, fill_default_hoist<int32_t>)
        ->Name("BM_FillDefault_Int32_Hoist")
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);
BENCHMARK_TEMPLATE(do_FillDefault, int64_t, fill_default_no_hoist<int64_t>)
        ->Name("BM_FillDefault_Int64_NoHoist")
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);
BENCHMARK_TEMPLATE(do_FillDefault, int64_t, fill_default_hoist<int64_t>)
        ->Name("BM_FillDefault_Int64_Hoist")
        ->Arg(10)
        ->Arg(50)
        ->Arg(90);

// ---- SIMD::Filter::filter_range stream compaction ---------------------------------------
// Pre-generate a ring of 16 distinct 4096-row selector arrays (64 KB total = 65,536 rows)
// and rotate across iterations so the CPU branch predictor cannot memorize a single 4096-row pattern.
constexpr size_t kSelectorRingSize = 16;

static std::vector<std::vector<uint8_t>> make_filter_selector_ring(size_t n, int keep_pct, int cluster, uint64_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 99);
    std::vector<std::vector<uint8_t>> ring(kSelectorRingSize, std::vector<uint8_t>(n));
    const size_t run = cluster > 0 ? static_cast<size_t>(cluster) : 1;
    for (size_t r = 0; r < kSelectorRingSize; ++r) {
        for (size_t i = 0; i < n; i += run) {
            const uint8_t keep = dist(rng) < keep_pct ? 1 : 0;
            for (size_t j = i; j < std::min(n, i + run); ++j) ring[r][j] = keep;
        }
    }
    return ring;
}

template <typename T, bool InPlace>
static void run_filter_range_bench(benchmark::State& state) {
    const int keep_pct = static_cast<int>(state.range(0));
    const int cluster = static_cast<int>(state.range(1));
    std::vector<T> src(kChunk);
    for (size_t i = 0; i < kChunk; ++i) src[i] = static_cast<T>(i * 2654435761ull + 1);
    std::vector<T> dst;
    if constexpr (!InPlace) {
        dst.resize(kChunk);
    }
    const auto sel_ring = make_filter_selector_ring(kChunk, keep_pct, cluster, 42);
    size_t idx = 0;
    for (auto _ : state) {
        const auto& sel = sel_ring[(idx++) & (kSelectorRingSize - 1)];
        T* dst_ptr = InPlace ? src.data() : dst.data();
        size_t n = SIMD::Filter::filter_range(dst_ptr, src.data(), sel.data(), 0, kChunk);
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(dst_ptr);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * kChunk);
    state.SetBytesProcessed(state.iterations() * kChunk * sizeof(T));
}

template <typename T>
static void BM_SimdFilterRange(benchmark::State& state) {
    run_filter_range_bench<T, false>(state);
}

template <typename T>
static void BM_SimdFilterRange_InPlace(benchmark::State& state) {
    run_filter_range_bench<T, true>(state);
}

BENCHMARK_TEMPLATE(BM_SimdFilterRange, uint32_t)->ArgsProduct({{0, 1, 5, 10, 25, 50, 75, 90, 99, 100}, {0, 16, 32}});
BENCHMARK_TEMPLATE(BM_SimdFilterRange, uint64_t)->ArgsProduct({{0, 1, 5, 10, 25, 50, 75, 90, 99, 100}, {0, 16, 32}});
BENCHMARK_TEMPLATE(BM_SimdFilterRange_InPlace, uint32_t)->ArgsProduct({{0, 1, 5, 10, 50, 90, 100}, {0, 16}});
BENCHMARK_TEMPLATE(BM_SimdFilterRange_InPlace, uint64_t)->ArgsProduct({{0, 1, 5, 10, 50, 90, 100}, {0, 16}});

#if defined(__ARM_NEON) && defined(__aarch64__)
template <typename T, size_t (*Kernel)(T*, const T*, const uint8_t*, size_t, size_t)>
static void BM_SimdFilterKernel(benchmark::State& state) {
    const int keep_pct = static_cast<int>(state.range(0));
    const int cluster = static_cast<int>(state.range(1));
    std::vector<T> src(kChunk), dst(kChunk);
    for (size_t i = 0; i < kChunk; ++i) src[i] = static_cast<T>(i * 2654435761ull + 1);
    const auto sel_ring = make_filter_selector_ring(kChunk, keep_pct, cluster, 42);
    size_t idx = 0;
    for (auto _ : state) {
        const auto& sel = sel_ring[(idx++) & (kSelectorRingSize - 1)];
        size_t n = Kernel(dst.data(), src.data(), sel.data(), 0, kChunk);
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * kChunk);
    state.SetBytesProcessed(state.iterations() * kChunk * sizeof(T));
}

BENCHMARK_TEMPLATE(BM_SimdFilterKernel, uint32_t, SIMD::Filter::detail::compress_neon_w4)
        ->ArgsProduct({{0, 1, 5, 10, 25, 50, 75, 90, 99, 100}, {0, 16, 32}});
BENCHMARK_TEMPLATE(BM_SimdFilterKernel, uint64_t, SIMD::Filter::detail::compress_neon_w8)
        ->ArgsProduct({{0, 1, 5, 10, 25, 50, 75, 90, 99, 100}, {0, 16, 32}});
#endif

// =====================================================================
// binary_column.cpp :: BinaryColumn::filter_range
// =====================================================================
static void fill_clustered_mask(uint8_t* data, size_t n, size_t cluster_len, int one_ratio_percent, uint64_t seed) {
    if (cluster_len <= 1) {
        fill_byte_mask(data, n, one_ratio_percent, seed);
        return;
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> d(0, 99);
    for (size_t i = 0; i < n; i += cluster_len) {
        uint8_t val = (d(rng) < one_ratio_percent) ? 1 : 0;
        size_t end = std::min(n, i + cluster_len);
        std::memset(data + i, val, end - i);
    }
}

static void BM_BinaryColumn_filter_range(benchmark::State& state) {
    const size_t cluster_len = static_cast<size_t>(state.range(0));
    const int sel_pct = static_cast<int>(state.range(1));

    std::mt19937_64 rng(0x12345678ULL);
    std::uniform_int_distribution<int> len_dist(16, 32);
    BinaryColumn::MutablePtr template_col = BinaryColumn::create();
    std::string buf(32, 'x');
    for (size_t i = 0; i < kChunk; ++i) {
        int len = len_dist(rng);
        for (int k = 0; k < len; ++k) {
            buf[k] = static_cast<char>('a' + ((i + k) % 26));
        }
        template_col->append(Slice(buf.data(), len));
    }
    const auto& src_bytes = template_col->get_bytes();
    const auto& src_offsets = template_col->get_offset().small_storage();

    Filter filter(kChunk);
    fill_clustered_mask(filter.data(), kChunk, cluster_len, sel_pct, 0xABCD0000ULL + sel_pct);

    BinaryColumn::MutablePtr work_col = BinaryColumn::create();
    work_col->get_bytes().reserve(src_bytes.size());
    work_col->get_offset().small_storage().reserve(src_offsets.size());

    for (auto _ : state) {
        state.PauseTiming();
        auto& wb = work_col->get_bytes();
        auto& wo = work_col->get_offset().small_storage();
        wb.resize(src_bytes.size());
        wo.resize(src_offsets.size());
        std::memcpy(wb.data(), src_bytes.data(), src_bytes.size());
        std::memcpy(wo.data(), src_offsets.data(), src_offsets.size() * sizeof(src_offsets[0]));
        state.ResumeTiming();

        size_t kept = work_col->filter_range(filter, 0, kChunk);
        benchmark::DoNotOptimize(kept);
        benchmark::DoNotOptimize(work_col->get_bytes().data());
    }
}

static void BinaryFilterRangeArgs(benchmark::Benchmark* b) {
    for (int pct : {0, 1, 10, 50, 90, 99, 100}) {
        b->Args({0, pct});
    }
    for (int cluster : {16, 32}) {
        for (int pct : {10, 50, 90}) {
            b->Args({cluster, pct});
        }
    }
}

BENCHMARK(BM_BinaryColumn_filter_range)->Apply(BinaryFilterRangeArgs);

// =====================================================================
// simd_mulselector.h :: SIMD_muti_selector::multi_select_if
// =====================================================================
enum MulSelectorScenario {
    kVarRandom50 = 0,
    kConstRandom50 = 1,
    kVarFirstTrue100 = 2,
    kVarAllFalse = 3,
    kVarClustered16 = 4,
};

template <LogicalType LT>
static void do_SIMD_mulselector(benchmark::State& state) {
    using CppType = RunTimeCppType<LT>;
    using Container = typename RunTimeColumnType<LT>::Container;

    const int arity = static_cast<int>(state.range(0));
    const int scenario = static_cast<int>(state.range(1));
    const int list_size = arity + 1;

    std::vector<Filter> filters(arity, Filter(kChunk, 0));
    std::vector<uint8_t*> select_vec(arity);
    for (int i = 0; i < arity; ++i) {
        if (scenario == kVarFirstTrue100) {
            std::memset(filters[i].data(), (i == 0) ? 1 : 0, kChunk);
        } else if (scenario == kVarAllFalse) {
            std::memset(filters[i].data(), 0, kChunk);
        } else if (scenario == kVarClustered16) {
            fill_clustered_mask(filters[i].data(), kChunk, 16, 50, 0x7000ULL + i);
        } else {
            fill_byte_mask(filters[i].data(), kChunk, 50, 0x5000ULL + i);
        }
        select_vec[i] = filters[i].data();
    }

    const bool use_const = (scenario == kConstRandom50);
    std::vector<Container> branches(list_size);
    std::vector<const Container*> select_list(list_size);
    std::vector<bool> is_const(list_size, use_const);
    for (int i = 0; i < list_size; ++i) {
        branches[i].resize(use_const ? 1 : kChunk);
        for (size_t r = 0; r < branches[i].size(); ++r) {
            branches[i][r] = static_cast<CppType>((i + 1) * 100000 + r);
        }
        select_list[i] = &branches[i];
    }

    Container dst(kChunk);
    for (auto _ : state) {
        SIMD_muti_selector<LT>::multi_select_if(select_vec.data(), arity, dst, select_list.data(), list_size, is_const,
                                                static_cast<int>(kChunk));
        benchmark::DoNotOptimize(dst.data());
    }
}

static void MulSelectorArgs(benchmark::Benchmark* b) {
    for (int arity : {2, 4, 8}) {
        for (int scenario : {kVarRandom50, kConstRandom50, kVarFirstTrue100, kVarAllFalse, kVarClustered16}) {
            b->Args({arity, scenario});
        }
    }
}

// =====================================================================
// append_with_mask.cpp :: AppendWithMaskVisitor
// =====================================================================
//
// Evaluates AppendWithMask on FixedLengthColumnBase<int64_t> and BinaryColumn
// across alternating (101010) and clustered (111...000...) patterns at
// selectivities: 0%, 1%, 10%, 50%, 90%, 100%.

enum MaskPattern { kAlternating = 0, kClustered = 1 };

static void generate_pattern_mask(uint8_t* mask, size_t n, int selectivity_pct, MaskPattern pattern) {
    if (selectivity_pct <= 0) {
        std::memset(mask, 0, n);
        return;
    }
    if (selectivity_pct >= 100) {
        std::memset(mask, 1, n);
        return;
    }
    size_t selected_count = (n * selectivity_pct) / 100;
    if (pattern == kClustered) {
        std::memset(mask, 1, selected_count);
        std::memset(mask + selected_count, 0, n - selected_count);
    } else {
        std::memset(mask, 0, n);
        if (selectivity_pct == 50) {
            for (size_t i = 0; i < n; i += 2) {
                mask[i] = 1;
            }
        } else {
            double step = static_cast<double>(n) / static_cast<double>(selected_count);
            for (size_t i = 0; i < selected_count; ++i) {
                size_t idx = static_cast<size_t>(i * step);
                if (idx < n) {
                    mask[idx] = 1;
                }
            }
        }
    }
}

template <MaskPattern Pattern, bool PositiveSelect = true>
static void BM_AppendWithMask_Int64(benchmark::State& state) {
    const int selectivity = static_cast<int>(state.range(0));
    const size_t n = kChunk;
    auto src = Int64Column::create();
    for (size_t i = 0; i < n; ++i) {
        src->append(static_cast<int64_t>(i));
    }
    std::vector<uint8_t> mask(n);
    generate_pattern_mask(mask.data(), n, selectivity, Pattern);

    auto dst = Int64Column::create();
    for (auto _ : state) {
        dst->resize(0);
        Status st = append_with_mask<PositiveSelect>(dst.get(), *src, mask.data(), n);
        benchmark::DoNotOptimize(dst->size());
    }
    state.SetItemsProcessed(state.iterations() * n);
}

template <MaskPattern Pattern, bool PositiveSelect = true>
static void BM_AppendWithMask_Binary(benchmark::State& state) {
    const int selectivity = static_cast<int>(state.range(0));
    const size_t n = kChunk;
    auto src = BinaryColumn::create();
    for (size_t i = 0; i < n; ++i) {
        std::string s = "starrocks_str_record_" + std::to_string(i);
        src->append(Slice(s));
    }
    std::vector<uint8_t> mask(n);
    generate_pattern_mask(mask.data(), n, selectivity, Pattern);

    auto dst = BinaryColumn::create();
    for (auto _ : state) {
        dst->get_offset().resize(1);
        dst->get_offset().set(0, 0);
        dst->get_bytes().resize(0);
        Status st = append_with_mask<PositiveSelect>(dst.get(), *src, mask.data(), n);
        benchmark::DoNotOptimize(dst->size());
    }
    state.SetItemsProcessed(state.iterations() * n);
}

BENCHMARK_TEMPLATE(BM_AppendWithMask_Int64, kAlternating)
        ->Name("BM_AppendWithMask_Int64_Alternating")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);
BENCHMARK_TEMPLATE(BM_AppendWithMask_Int64, kClustered)
        ->Name("BM_AppendWithMask_Int64_Clustered")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);
BENCHMARK_TEMPLATE(BM_AppendWithMask_Binary, kAlternating)
        ->Name("BM_AppendWithMask_Binary_Alternating")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);
BENCHMARK_TEMPLATE(BM_AppendWithMask_Binary, kClustered)
        ->Name("BM_AppendWithMask_Binary_Clustered")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);

BENCHMARK_TEMPLATE(BM_AppendWithMask_Int64, kAlternating, false)
        ->Name("BM_AppendWithMask_Neg_Int64_Alternating")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);
BENCHMARK_TEMPLATE(BM_AppendWithMask_Int64, kClustered, false)
        ->Name("BM_AppendWithMask_Neg_Int64_Clustered")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);
BENCHMARK_TEMPLATE(BM_AppendWithMask_Binary, kAlternating, false)
        ->Name("BM_AppendWithMask_Neg_Binary_Alternating")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);
BENCHMARK_TEMPLATE(BM_AppendWithMask_Binary, kClustered, false)
        ->Name("BM_AppendWithMask_Neg_Binary_Clustered")
        ->Arg(0)
        ->Arg(1)
        ->Arg(10)
        ->Arg(50)
        ->Arg(90)
        ->Arg(100);

// =====================================================================
// SIMD::count_zero on selection and index vectors
// =====================================================================

static void GenerateCountZeroArgs(benchmark::Benchmark* b) {
    for (int64_t bytes : {64, 256, 4096, 65536}) {
        for (int64_t zero_pct : {0, 1, 10, 50, 90, 100}) {
            b->Args({bytes, zero_pct});
        }
    }
}

BENCHMARK_TEMPLATE(do_SIMD_mulselector, TYPE_BIGINT)->Name("BM_SIMD_mulselector_8byte")->Apply(MulSelectorArgs);
BENCHMARK_TEMPLATE(do_SIMD_mulselector, TYPE_INT)->Name("BM_SIMD_mulselector_4byte")->Apply(MulSelectorArgs);

static void BM_CountZero_8Bit(benchmark::State& state) {
    const size_t bytes = static_cast<size_t>(state.range(0));
    const int zero_pct = static_cast<int>(state.range(1));
    const size_t n = bytes / sizeof(uint8_t);
    std::vector<uint8_t> data(n);
    std::mt19937_64 rng(0xC0072E80);
    std::uniform_int_distribution<int> d(0, 99);
    for (size_t i = 0; i < n; ++i) {
        data[i] = (d(rng) < zero_pct) ? 0 : 1;
    }
    for (auto _ : state) {
        size_t count = SIMD::count_zero(data.data(), n);
        benchmark::DoNotOptimize(count);
    }
    state.SetBytesProcessed(state.iterations() * bytes);
}

static void BM_CountZero_32Bit(benchmark::State& state) {
    const size_t bytes = static_cast<size_t>(state.range(0));
    const int zero_pct = static_cast<int>(state.range(1));
    const size_t n = bytes / sizeof(uint32_t);
    std::vector<uint32_t> data(n);
    std::mt19937_64 rng(0xC0072E80);
    std::uniform_int_distribution<int> d(0, 99);
    for (size_t i = 0; i < n; ++i) {
        data[i] = (d(rng) < zero_pct) ? 0 : static_cast<uint32_t>(i + 1);
    }
    for (auto _ : state) {
        size_t count = SIMD::count_zero(data.data(), n);
        benchmark::DoNotOptimize(count);
    }
    state.SetBytesProcessed(state.iterations() * bytes);
}

BENCHMARK(BM_CountZero_8Bit)->Apply(GenerateCountZeroArgs);
BENCHMARK(BM_CountZero_32Bit)->Apply(GenerateCountZeroArgs);


#if defined(__x86_64__)

// =====================================================================
// SIMD filter_range stream compaction microbenchmarks (AVX2)
// =====================================================================

template <typename T, size_t (*Kernel)(T*, const T*, const uint8_t*, size_t, size_t)>
static void BM_SimdFilterKernel(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx2")) {
        state.SkipWithError("AVX2 is not supported on this CPU");
        return;
    }
    const int selectivity = state.range(0);
    const int cluster = state.range(1);

    auto ring = make_filter_selector_ring(kChunk, selectivity, cluster, 0x12345678ULL);

    std::vector<T> src(kChunk);
    for (size_t i = 0; i < kChunk; ++i) {
        src[i] = static_cast<T>(i * 31 + 7);
    }
    std::vector<T> dst(kChunk, 0);

    size_t ring_idx = 0;
    size_t total_retained = 0;
    for (auto _ : state) {
        const uint8_t* selector = ring[ring_idx].data();
        size_t retained = Kernel(dst.data(), src.data(), selector, 0, kChunk);
        total_retained += retained;
        benchmark::DoNotOptimize(dst.data());
        ring_idx = (ring_idx + 1) & 15;
    }
    benchmark::DoNotOptimize(total_retained);
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(kChunk * sizeof(T)));
    state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(kChunk));
}

static void SimdFilterKernelArgs(benchmark::Benchmark* b) {
    for (int cluster : {0, 7, 13, 16}) {
        for (int sel : {10, 20, 30, 40, 50, 60, 70, 80, 90}) {
            b->Args({sel, cluster});
        }
    }
}

BENCHMARK_TEMPLATE(BM_SimdFilterKernel, uint32_t, SIMD::Filter::detail::scan_avx2_baseline_w4)
        ->Name("BM_SimdFilterKernel_scan_avx2_baseline_w4")
        ->Apply(SimdFilterKernelArgs);

BENCHMARK_TEMPLATE(BM_SimdFilterKernel, uint32_t, SIMD::Filter::detail::compress_avx2_w4)
        ->Name("BM_SimdFilterKernel_compress_avx2_w4")
        ->Apply(SimdFilterKernelArgs);

BENCHMARK_TEMPLATE(BM_SimdFilterKernel, uint64_t, SIMD::Filter::detail::scan_avx2_baseline_w8)
        ->Name("BM_SimdFilterKernel_scan_avx2_baseline_w8")
        ->Apply(SimdFilterKernelArgs);

BENCHMARK_TEMPLATE(BM_SimdFilterKernel, uint64_t, SIMD::Filter::detail::compress_avx2_w8)
        ->Name("BM_SimdFilterKernel_compress_avx2_w8")
        ->Apply(SimdFilterKernelArgs);

template <typename T>
static void BM_SimdFilterRangeAvx2(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx2")) {
        state.SkipWithError("AVX2 is not supported on this CPU");
        return;
    }
    const int selectivity = state.range(0);
    const int cluster = state.range(1);

    auto ring = make_filter_selector_ring(kChunk, selectivity, cluster, 0x12345678ULL);

    std::vector<T> src(kChunk);
    for (size_t i = 0; i < kChunk; ++i) {
        src[i] = static_cast<T>(i * 31 + 7);
    }
    std::vector<T> dst(kChunk, 0);

    size_t ring_idx = 0;
    size_t total_retained = 0;
    for (auto _ : state) {
        const uint8_t* selector = ring[ring_idx].data();
        size_t retained =
                SIMD::Filter::detail::filter_range_avx2(dst.data(), src.data(), sizeof(T), selector, 0, kChunk);
        total_retained += retained;
        benchmark::DoNotOptimize(dst.data());
        ring_idx = (ring_idx + 1) & 15;
    }
    benchmark::DoNotOptimize(total_retained);
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(kChunk * sizeof(T)));
    state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(kChunk));
}

BENCHMARK_TEMPLATE(BM_SimdFilterRangeAvx2, uint32_t)->Name("BM_SimdFilterRangeAvx2_w4")->Apply(SimdFilterKernelArgs);

BENCHMARK_TEMPLATE(BM_SimdFilterRangeAvx2, uint64_t)->Name("BM_SimdFilterRangeAvx2_w8")->Apply(SimdFilterKernelArgs);

#endif // __x86_64__

} // namespace starrocks

BENCHMARK_MAIN();
