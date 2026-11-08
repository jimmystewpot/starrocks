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

// Microbenchmarks for the SIMD primitives introduced by PR #73287
// ([Enhancement] Add AVX2/NEON SIMD primitives for analytic workloads).
//
// These primitives are foundation helpers: they have no in-tree call sites
// yet; the consumers land in stacked PRs (parquet, storage, aggregate).
// The microbenchmarks below exercise each helper against the scalar baseline
// it replaces, so reviewers can confirm the helper is worth taking on its
// own merits.
//
// Functions covered:
//   * simd.h          : all_zeros / all_ones (8-bit) vs scalar early-exit
//   * rle_simd.h      : simd_fill_int32 / simd_fill_int16 vs scalar fill
//                       simd_dict_gather_int32 vs scalar dict[indices[i]]
//                       simd_minmax_int32       vs scalar min/max loop
//                       simd_widen_int8_to_int32 vs scalar cast loop
//   * string_length_filter.h : length_eq_mask  vs scalar batched compute

#include <benchmark/benchmark.h>
#include <re2/stringpiece.h>

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "base/simd/rle_simd.h"
#include "base/simd/simd.h"
#include "base/simd/string_length_filter.h"
#include "base/string/slice.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "exprs/binary_function.h"
#include "exprs/function_context.h"
#include "exprs/like_predicate.h"

namespace starrocks {

constexpr size_t kBatch = 4096;

// =====================================================================
// all_zeros / all_ones
// =====================================================================
//
// Pre-PR equivalent:
//   bool all_zero = true; for (i) if (data[i]) { all_zero = false; break; }
// Post-PR:
//   SIMD::all_zeros(data, n)  -> wraps contains_nonzero_bit
//   SIMD::all_ones(data,  n)  -> wraps std::memchr (libc SIMD)
//
// state.range(0) is the position of the disagreeing byte (0..kBatch), or
// kBatch to mean "all bytes agree" (the worst case where no early exit).

static bool scalar_all_zeros(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

static bool scalar_all_ones(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (data[i] == 0) return false;
    }
    return true;
}

static void BM_AllZeros_Scalar(benchmark::State& state) {
    std::vector<uint8_t> data(kBatch, 0);
    size_t pos = static_cast<size_t>(state.range(0));
    if (pos < kBatch) data[pos] = 1;
    for (auto _ : state) {
        bool r = scalar_all_zeros(data.data(), data.size());
        benchmark::DoNotOptimize(r);
    }
}

static void BM_AllZeros_SIMD(benchmark::State& state) {
    std::vector<uint8_t> data(kBatch, 0);
    size_t pos = static_cast<size_t>(state.range(0));
    if (pos < kBatch) data[pos] = 1;
    for (auto _ : state) {
        bool r = SIMD::all_zeros(data.data(), data.size());
        benchmark::DoNotOptimize(r);
    }
}

static void BM_AllOnes_Scalar(benchmark::State& state) {
    std::vector<uint8_t> data(kBatch, 1);
    size_t pos = static_cast<size_t>(state.range(0));
    if (pos < kBatch) data[pos] = 0;
    for (auto _ : state) {
        bool r = scalar_all_ones(data.data(), data.size());
        benchmark::DoNotOptimize(r);
    }
}

static void BM_AllOnes_SIMD(benchmark::State& state) {
    std::vector<uint8_t> data(kBatch, 1);
    size_t pos = static_cast<size_t>(state.range(0));
    if (pos < kBatch) data[pos] = 0;
    for (auto _ : state) {
        bool r = SIMD::all_ones(data.data(), data.size());
        benchmark::DoNotOptimize(r);
    }
}

BENCHMARK(BM_AllZeros_Scalar)->Arg(0)->Arg(kBatch / 2)->Arg(kBatch - 1)->Arg(kBatch);
BENCHMARK(BM_AllZeros_SIMD)->Arg(0)->Arg(kBatch / 2)->Arg(kBatch - 1)->Arg(kBatch);
BENCHMARK(BM_AllOnes_Scalar)->Arg(0)->Arg(kBatch / 2)->Arg(kBatch - 1)->Arg(kBatch);
BENCHMARK(BM_AllOnes_SIMD)->Arg(0)->Arg(kBatch / 2)->Arg(kBatch - 1)->Arg(kBatch);

// =====================================================================
// simd_widen_int8_to_int32 (parquet INT8 -> INT32 promotion)
// =====================================================================

static void BM_WidenInt8_Scalar(benchmark::State& state) {
    size_t n = static_cast<size_t>(state.range(0));
    std::vector<int8_t> src(n);
    std::vector<int32_t> dest(n);
    std::mt19937_64 rng(0xC0DECAFE);
    std::uniform_int_distribution<int> d(-128, 127);
    for (auto& v : src) v = static_cast<int8_t>(d(rng));
    for (auto _ : state) {
        for (size_t i = 0; i < n; ++i) dest[i] = static_cast<int32_t>(src[i]);
        benchmark::DoNotOptimize(dest.data());
    }
}

static void BM_WidenInt8_SIMD(benchmark::State& state) {
    size_t n = static_cast<size_t>(state.range(0));
    std::vector<int8_t> src(n);
    std::vector<int32_t> dest(n);
    std::mt19937_64 rng(0xC0DECAFE);
    std::uniform_int_distribution<int> d(-128, 127);
    for (auto& v : src) v = static_cast<int8_t>(d(rng));
    for (auto _ : state) {
        simd_widen_int8_to_int32(dest.data(), src.data(), static_cast<int32_t>(n));
        benchmark::DoNotOptimize(dest.data());
    }
}

BENCHMARK(BM_WidenInt8_Scalar)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);
BENCHMARK(BM_WidenInt8_SIMD)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// =====================================================================
// simd_dict_gather_int32: scalar (autovec) vs hand AVX2 i32gather vs AVX-512
// =====================================================================
//
// The hand-AVX2 `_mm256_i32gather_epi32` path was removed from rle_simd.h
// because microbench on AMD m6a / Aliyun Intel (GDS microcode applied)
// showed it was ~25% slower than the scalar default. This bench provides
// the data needed to revisit that decision on Intel hosts where the GDS
// mitigation is not applied (per stdpain's review on PR #73287).
//
// Setup: a 256-entry dictionary (fits comfortably in L1d), random indices
// drawn uniformly. count = 256 / 1024 / 4096 covers the chunk sizes the
// Parquet dictionary decoder typically calls with.

static constexpr int32_t kDictSize = 256;

static void prepare_dict_gather(std::vector<int32_t>& dict, std::vector<uint32_t>& indices, int32_t count) {
    dict.resize(kDictSize);
    indices.resize(count);
    std::mt19937_64 rng(0x6A746F72);
    std::uniform_int_distribution<int32_t> dv(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
    for (auto& v : dict) v = dv(rng);
    std::uniform_int_distribution<uint32_t> id(0, kDictSize - 1);
    for (auto& v : indices) v = id(rng);
}

// Plain scalar gather. Compiler is free to auto-vectorise this any way it
// likes (AVX2 vpgatherdd on -mavx2, AVX-512 vpgatherdd on -mavx512f, or
// scalar loads if it prefers).
static void scalar_dict_gather_int32(int32_t* __restrict dest, const int32_t* __restrict dict,
                                     const uint32_t* __restrict indices, int32_t count) {
    for (int32_t i = 0; i < count; ++i) dest[i] = dict[indices[i]];
}

#if defined(__AVX2__)
// Hand AVX2 vpgatherdd, mirrors the path that was removed from rle_simd.h.
static void avx2_dict_gather_int32(int32_t* __restrict dest, const int32_t* __restrict dict,
                                   const uint32_t* __restrict indices, int32_t count) {
    int32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i v_idx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(indices + i));
        __m256i v_gat = _mm256_i32gather_epi32(dict, v_idx, sizeof(int32_t));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest + i), v_gat);
    }
    for (; i < count; ++i) dest[i] = dict[indices[i]];
}
#endif

#if defined(__AVX512F__)
// Hand AVX-512 vpgatherdd zmm, same as simd_dict_gather_int32_avx512 in
// rle_simd.h but called directly so the bench numbers line up against the
// AVX2 path on the same hardware in the same binary.
static void avx512_dict_gather_int32(int32_t* __restrict dest, const int32_t* __restrict dict,
                                     const uint32_t* __restrict indices, int32_t count) {
    int32_t i = 0;
    for (; i + 16 <= count; i += 16) {
        __m512i v_idx = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(indices + i));
        __m512i v_gat = _mm512_i32gather_epi32(v_idx, dict, sizeof(int32_t));
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(dest + i), v_gat);
    }
    for (; i < count; ++i) dest[i] = dict[indices[i]];
}
#endif

static void BM_DictGather_Scalar(benchmark::State& state) {
    int32_t n = static_cast<int32_t>(state.range(0));
    std::vector<int32_t> dict;
    std::vector<uint32_t> indices;
    prepare_dict_gather(dict, indices, n);
    std::vector<int32_t> dest(n);
    for (auto _ : state) {
        scalar_dict_gather_int32(dest.data(), dict.data(), indices.data(), n);
        benchmark::DoNotOptimize(dest.data());
    }
}

#if defined(__AVX2__)
static void BM_DictGather_AVX2(benchmark::State& state) {
    int32_t n = static_cast<int32_t>(state.range(0));
    std::vector<int32_t> dict;
    std::vector<uint32_t> indices;
    prepare_dict_gather(dict, indices, n);
    std::vector<int32_t> dest(n);
    for (auto _ : state) {
        avx2_dict_gather_int32(dest.data(), dict.data(), indices.data(), n);
        benchmark::DoNotOptimize(dest.data());
    }
}
#endif

#if defined(__AVX512F__)
static void BM_DictGather_AVX512(benchmark::State& state) {
    int32_t n = static_cast<int32_t>(state.range(0));
    std::vector<int32_t> dict;
    std::vector<uint32_t> indices;
    prepare_dict_gather(dict, indices, n);
    std::vector<int32_t> dest(n);
    for (auto _ : state) {
        avx512_dict_gather_int32(dest.data(), dict.data(), indices.data(), n);
        benchmark::DoNotOptimize(dest.data());
    }
}
#endif

BENCHMARK(BM_DictGather_Scalar)->Arg(256)->Arg(1024)->Arg(4096);
#if defined(__AVX2__)
BENCHMARK(BM_DictGather_AVX2)->Arg(256)->Arg(1024)->Arg(4096);
#endif
#if defined(__AVX512F__)
BENCHMARK(BM_DictGather_AVX512)->Arg(256)->Arg(1024)->Arg(4096);
#endif

// =====================================================================
// length_eq_mask (parquet PLAIN binary length filter, 8 strings per call)
// =====================================================================
//
// Pre-PR equivalent: scalar 8-element batched compute.
// Post-PR: AVX2 (8 lanes) or NEON (4 lanes). The NEON bench loops twice
// per AVX2 call so total work matches.

static uint32_t scalar_length_eq_mask_avx_width(const uint32_t* offsets, size_t base, uint32_t target_len) {
    uint32_t mask = 0;
    for (int i = 0; i < 8; ++i) {
        if (offsets[base + i + 1] - offsets[base + i] == target_len) mask |= (1u << i);
    }
    return mask;
}

static void BM_LengthEqMask_Scalar(benchmark::State& state) {
    size_t n_strings = static_cast<size_t>(state.range(0));
    std::vector<uint32_t> offsets(n_strings + 1);
    std::mt19937_64 rng(0xFADEDDEE);
    std::uniform_int_distribution<int> d(1, 32);
    uint32_t cur = 0;
    offsets[0] = 0;
    for (size_t i = 0; i < n_strings; ++i) {
        cur += d(rng);
        offsets[i + 1] = cur;
    }
    for (auto _ : state) {
        uint32_t agg = 0;
        for (size_t base = 0; base + 8 <= n_strings; base += 8) {
            agg ^= scalar_length_eq_mask_avx_width(offsets.data(), base, 5);
        }
        benchmark::DoNotOptimize(agg);
    }
}

static void BM_LengthEqMask_SIMD(benchmark::State& state) {
    size_t n_strings = static_cast<size_t>(state.range(0));
    std::vector<uint32_t> offsets(n_strings + 1);
    std::mt19937_64 rng(0xFADEDDEE);
    std::uniform_int_distribution<int> d(1, 32);
    uint32_t cur = 0;
    offsets[0] = 0;
    for (size_t i = 0; i < n_strings; ++i) {
        cur += d(rng);
        offsets[i + 1] = cur;
    }
    for (auto _ : state) {
        uint32_t agg = 0;
        for (size_t base = 0; base + SIMD::kStringLenSimdWidth <= n_strings; base += SIMD::kStringLenSimdWidth) {
            agg ^= SIMD::length_eq_mask(offsets.data(), base, 5);
        }
        benchmark::DoNotOptimize(agg);
    }
}

BENCHMARK(BM_LengthEqMask_Scalar)->Arg(256)->Arg(1024)->Arg(4096);
BENCHMARK(BM_LengthEqMask_SIMD)->Arg(256)->Arg(1024)->Arg(4096);

// =====================================================================
// Constant starts_with / ends_with / equals (Legacy Slice vs SIMD Raw)
// =====================================================================

namespace {

struct LegacyStartsImpl {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const LType& s, const RType& p) {
        return (s.size >= p.size) && (Slice(s.data, p.size) == p);
    }
};

struct LegacyEndsImpl {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const LType& s, const RType& p) {
        return (s.size >= p.size) && (Slice(s.data + s.size - p.size, p.size) == p);
    }
};

struct LegacyEqualsImpl {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const LType& s, const RType& p) {
        return s == p;
    }
};

struct LegacyRe2StartsImpl {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const LType& s, const RType& p) {
        re2::StringPiece str_sp(s.data, s.size);
        re2::StringPiece prefix_sp(p.data, p.size);
        return str_sp.starts_with(prefix_sp);
    }
};

// profile:
//   0 = length-rejected (all rows shorter than pattern for starts/ends, or len != pat_len for equals)
//   1 = length-pass, 0% match (mismatch on last byte of pattern)
//   2 = 50% match
//   3 = 100% match
static std::pair<ColumnPtr, std::string> make_like_bench_column(size_t num_rows, size_t pat_len, int profile,
                                                                bool is_ends, bool is_equals = false) {
    std::string pattern(pat_len, 'a');
    for (size_t i = 0; i < pat_len; ++i) {
        pattern[i] = static_cast<char>('a' + (i % 26));
    }
    auto col = BinaryColumn::create();
    col->reserve(num_rows, num_rows * (pat_len + 16));
    for (size_t r = 0; r < num_rows; ++r) {
        if (profile == 0) {
            size_t short_len = pat_len > 0 ? (r % pat_len) : 0;
            col->append(Slice(pattern.data(), short_len));
        } else {
            bool match = (profile == 3) || (profile == 2 && (r & 1) == 0);
            std::string row_pat = pattern;
            if (!match && !row_pat.empty()) {
                row_pat.back() ^= 0x01;
            }
            std::string val;
            if (is_equals) {
                val = row_pat;
            } else {
                std::string extra("0123456789abcdef", (r % 8) + 4);
                val = is_ends ? (extra + row_pat) : (row_pat + extra);
            }
            col->append(Slice(val));
        }
    }
    return {std::move(col), std::move(pattern)};
}

} // namespace

static void BM_ConstStartsWith_Legacy(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, false);
    ColumnPtr pat_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(pat_str), 1);
    for (auto _ : state) {
        ColumnPtr res =
                VectorizedStrictBinaryFunction<LegacyStartsImpl>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, pat_col);
        benchmark::DoNotOptimize(res);
    }
}

static void BM_ConstStartsWith_Re2Legacy(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, false);
    ColumnPtr pat_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(pat_str), kBatch);
    for (auto _ : state) {
        ColumnPtr res =
                VectorizedStrictBinaryFunction<LegacyRe2StartsImpl>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, pat_col);
        benchmark::DoNotOptimize(res);
    }
}

static void BM_ConstEndsWith_Legacy(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, true);
    ColumnPtr pat_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(pat_str), 1);
    for (auto _ : state) {
        ColumnPtr res =
                VectorizedStrictBinaryFunction<LegacyEndsImpl>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, pat_col);
        benchmark::DoNotOptimize(res);
    }
}

static void BM_ConstEquals_Legacy(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, false, true);
    ColumnPtr pat_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(pat_str), 1);
    for (auto _ : state) {
        ColumnPtr res =
                VectorizedStrictBinaryFunction<LegacyEqualsImpl>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, pat_col);
        benchmark::DoNotOptimize(res);
    }
}

static void BM_ConstStartsWith_SIMD(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, false);
    Slice pat(pat_str);
    for (auto _ : state) {
        ColumnPtr res = LikePredicate::match_constant_starts_with(col, pat).value();
        benchmark::DoNotOptimize(res);
    }
}

static void BM_ConstEndsWith_SIMD(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, true);
    Slice pat(pat_str);
    for (auto _ : state) {
        ColumnPtr res = LikePredicate::match_constant_ends_with(col, pat).value();
        benchmark::DoNotOptimize(res);
    }
}

static void BM_ConstEquals_SIMD(benchmark::State& state) {
    const size_t pat_len = static_cast<size_t>(state.range(0));
    const int profile = static_cast<int>(state.range(1));
    auto [col, pat_str] = make_like_bench_column(kBatch, pat_len, profile, false, true);
    Slice pat(pat_str);
    for (auto _ : state) {
        ColumnPtr res = LikePredicate::match_constant_equals(col, pat).value();
        benchmark::DoNotOptimize(res);
    }
}

BENCHMARK(BM_ConstStartsWith_Legacy)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});
BENCHMARK(BM_ConstStartsWith_Re2Legacy)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});
BENCHMARK(BM_ConstEndsWith_Legacy)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});
BENCHMARK(BM_ConstEquals_Legacy)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});
BENCHMARK(BM_ConstStartsWith_SIMD)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});
BENCHMARK(BM_ConstEndsWith_SIMD)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});
BENCHMARK(BM_ConstEquals_SIMD)->ArgsProduct({{4, 7, 13, 24, 48}, {0, 1, 2, 3}});

} // namespace starrocks

BENCHMARK_MAIN();

// =====================================================================
// Paste results below after `./build_Release/src/bench/output/simd_common_bench`.
// =====================================================================
