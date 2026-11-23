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

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include "base/string/byte_set.h"
#include "base/string/trim.h"

namespace starrocks {

static constexpr size_t kBatch = 4096;

// =====================================================================
// String TRIM / LTRIM / RTRIM and trim_spaces microbenchmarks
// =====================================================================
//
// Parameterized by (str_len, pad_len) over a 4,096-slice batch:
//   * (24, 0)   : short strings, 0 leading/trailing trim chars (fast-reject path)
//   * (64, 2)   : medium strings, 2 leading + 2 trailing trim chars
//   * (64, 24)  : medium strings, 24 leading + 24 trailing trim chars
//   * (256, 96) : long strings, 96 leading + 96 trailing trim chars

static inline std::string_view legacy_trim_spaces(std::string_view str) {
    size_t begin = 0;
    size_t end = str.size();
    while (begin < end && str[begin] == ' ') {
        ++begin;
    }
    while (end > begin && str[end - 1] == ' ') {
        --end;
    }
    return str.substr(begin, end - begin);
}

static inline const char* legacy_skip_leading_single(const char* begin, const char* end, char c) {
    auto p = begin;
#if defined(__SSE2__)
    const auto size = end - begin;
    const auto SSE2_BYTES = sizeof(__m128i);
    const auto sse2_end = begin + (size & ~(SSE2_BYTES - 1));
    const auto spaces = _mm_set1_epi8(c);
    for (; p < sse2_end; p += SSE2_BYTES) {
        uint32_t masks =
                _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), spaces));
        int pos = __builtin_ctz((1u << SSE2_BYTES) | ~masks);
        if (pos < static_cast<int>(SSE2_BYTES)) {
            return p + pos;
        }
    }
#endif
    if (c == ' ') {
        for (; p < end && *p == ' '; ++p) {
        }
    } else {
        for (; p < end && *p == c; ++p) {
        }
    }
    return p;
}

static inline const char* legacy_skip_trailing_single(const char* begin, const char* end, char c) {
    if (begin == nullptr || begin == end) {
        return end;
    }
    auto p = end;
#if defined(__SSE2__)
    const auto size = end - begin;
    const auto SSE2_BYTES = sizeof(__m128i);
    const auto sse2_begin = end - (size & ~(SSE2_BYTES - 1));
    const auto spaces = _mm_set1_epi8(c);
    for (p = end - SSE2_BYTES; p >= sse2_begin; p -= SSE2_BYTES) {
        uint32_t masks =
                _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), spaces));
        int pos = __builtin_clz(~(masks << SSE2_BYTES));
        if (pos < static_cast<int>(SSE2_BYTES)) {
            return p + SSE2_BYTES - pos;
        }
    }
    p += SSE2_BYTES;
#endif
    if (c == ' ') {
        for (--p; p >= begin && *p == ' '; --p) {
        }
    } else {
        for (--p; p >= begin && *p == c; --p) {
        }
    }
    return p + 1;
}

static inline const char* legacy_skip_leading_multi(const char* begin, const char* end, std::string_view remove) {
    auto p = begin;
    for (; p < end && remove.find(*p) != std::string_view::npos; ++p) {
    }
    return p;
}

static inline const char* legacy_skip_trailing_multi(const char* begin, const char* end, std::string_view remove) {
    if (begin == nullptr || begin == end) {
        return end;
    }
    auto p = end;
    for (--p; p >= begin && remove.find(*p) != std::string_view::npos; --p) {
    }
    return p + 1;
}

struct TrimBenchBatch {
    std::string arena;
    std::vector<std::string_view> slices;
    std::vector<uint32_t> offsets;
};

static TrimBenchBatch BuildTrimBenchBatch(size_t str_len, size_t lead_pad, size_t trail_pad,
                                          std::string_view pad_alphabet) {
    TrimBenchBatch batch;
    batch.arena.resize(kBatch * str_len);
    batch.slices.resize(kBatch);
    batch.offsets.resize(kBatch + 1, 0);
    const size_t total_pad = lead_pad + trail_pad;
    const size_t body_len = (total_pad <= str_len) ? (str_len - total_pad) : 0;
    for (size_t i = 0; i < kBatch; ++i) {
        char* dst = batch.arena.data() + i * str_len;
        for (size_t j = 0; j < lead_pad; ++j) {
            dst[j] = pad_alphabet[(i + j) % pad_alphabet.size()];
        }
        for (size_t j = 0; j < body_len; ++j) {
            dst[lead_pad + j] = static_cast<char>('a' + ((i + j) % 26));
        }
        for (size_t j = 0; j < trail_pad; ++j) {
            dst[lead_pad + body_len + j] = pad_alphabet[(i + j + 3) % pad_alphabet.size()];
        }
        batch.slices[i] = std::string_view(dst, str_len);
        batch.offsets[i + 1] = static_cast<uint32_t>((i + 1) * str_len);
    }
    return batch;
}

static TrimBenchBatch BuildTrimBenchBatch(size_t str_len, size_t pad_len, std::string_view pad_alphabet) {
    return BuildTrimBenchBatch(str_len, pad_len, pad_len, pad_alphabet);
}

static void GenerateTrimBenchArgs(benchmark::Benchmark* b) {
    // a) Zero whitespace across short (<16B), medium (16-64B), and long (>128B)
    b->Args({12, 0});
    b->Args({24, 0});
    b->Args({48, 0});
    b->Args({192, 0});
    // b) 1 leading + 1 trailing space across short (<16B), medium (16-64B), and long (>128B)
    b->Args({12, 1});
    b->Args({48, 1});
    b->Args({192, 1});
    // c) Multi-space padding (2 spaces, 24 spaces, 96 spaces)
    b->Args({64, 2});
    b->Args({64, 24});
    b->Args({256, 96});
}

static void GenerateTrimAsymmetricBenchArgs(benchmark::Benchmark* b) {
    // (str_len, lead_pad, trail_pad): 1 leading space only vs 1 trailing space only across short/medium/long
    b->Args({12, 1, 0});
    b->Args({12, 0, 1});
    b->Args({48, 1, 0});
    b->Args({48, 0, 1});
    b->Args({192, 1, 0});
    b->Args({192, 0, 1});
}

static void BM_TrimSpaces_LegacyScalar(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            std::string_view res = legacy_trim_spaces(batch.slices[i]);
            total_remaining += res.size();
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimSpaces_Current(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            std::string_view res = trim_spaces(batch.slices[i]);
            total_remaining += res.size();
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimSingleChar_Scalar(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = skip_leading_char<false>(b, e, ' ');
            e = skip_trailing_char<false>(b, e, ' ');
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimSingleChar_LegacySSE2(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = legacy_skip_leading_single(b, e, ' ');
            e = legacy_skip_trailing_single(b, e, ' ');
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimSingleChar_Current(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = skip_leading_char<true>(b, e, ' ');
            e = skip_trailing_char<true>(b, e, ' ');
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimAsymmetric_Scalar(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t lead_pad = static_cast<size_t>(state.range(1));
    const size_t trail_pad = static_cast<size_t>(state.range(2));
    const auto batch = BuildTrimBenchBatch(str_len, lead_pad, trail_pad, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = skip_leading_char<false>(b, e, ' ');
            e = skip_trailing_char<false>(b, e, ' ');
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimAsymmetric_Current(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t lead_pad = static_cast<size_t>(state.range(1));
    const size_t trail_pad = static_cast<size_t>(state.range(2));
    const auto batch = BuildTrimBenchBatch(str_len, lead_pad, trail_pad, " ");
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = skip_leading_char<true>(b, e, ' ');
            e = skip_trailing_char<true>(b, e, ' ');
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimMultiChar_LegacyFind(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    constexpr std::string_view kRemove = " \t\r\n\v\f";
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, kRemove);
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = legacy_skip_leading_multi(b, e, kRemove);
            e = legacy_skip_trailing_multi(b, e, kRemove);
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimMultiChar_ByteSetScalar(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    constexpr std::string_view kRemove = " \t\r\n\v\f";
    const ByteSet set = ByteSet::from_bytes(kRemove);
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, kRemove);
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = skip_leading_byteset<false>(b, e, set);
            e = skip_trailing_byteset<false>(b, e, set);
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

static void BM_TrimMultiChar_Current(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    constexpr std::string_view kRemove = " \t\r\n\v\f";
    const ByteSet set = ByteSet::from_bytes(kRemove);
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, kRemove);
    for (auto _ : state) {
        size_t total_remaining = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* b = batch.slices[i].data();
            const char* e = b + batch.slices[i].size();
            b = skip_leading_byteset<true>(b, e, set);
            e = skip_trailing_byteset<true>(b, e, set);
            total_remaining += static_cast<size_t>(e - b);
        }
        benchmark::DoNotOptimize(total_remaining);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

// Simulates the legacy AdaptiveTrimFunction::evaluate loop (unconditional per-row vector::insert + offset store).
static void BM_ColumnTrim_Legacy(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    const char* const src_base = batch.arena.data();
    const uint32_t* const src_offsets = batch.offsets.data();
    const size_t src_bytes_size = batch.arena.size();
    const bool use_simd = pad_len > 8;
    for (auto _ : state) {
        std::vector<uint32_t> dst_offsets(kBatch + 1);
        std::vector<uint8_t> dst_bytes;
        dst_bytes.reserve(src_bytes_size);
        dst_offsets[0] = 0;
        for (size_t i = 0; i < kBatch; ++i) {
            const char* from_ptr = src_base + src_offsets[i];
            const char* to_ptr = src_base + src_offsets[i + 1];
            if (use_simd && i >= 100) {
                from_ptr = legacy_skip_leading_single(from_ptr, to_ptr, ' ');
                to_ptr = legacy_skip_trailing_single(from_ptr, to_ptr, ' ');
            } else {
                for (; from_ptr < to_ptr && *from_ptr == ' '; ++from_ptr) {
                }
                for (; to_ptr > from_ptr && *(to_ptr - 1) == ' '; --to_ptr) {
                }
            }
            dst_bytes.insert(dst_bytes.end(), reinterpret_cast<const uint8_t*>(from_ptr),
                             reinterpret_cast<const uint8_t*>(to_ptr));
            dst_offsets[i + 1] = static_cast<uint32_t>(dst_bytes.size());
        }
        benchmark::DoNotOptimize(dst_offsets.data());
        benchmark::DoNotOptimize(dst_bytes.data());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

// Simulates the optimized AdaptiveTrimFunction::evaluate loop (clean-column fast-path + pre-sized memcpy).
static void BM_ColumnTrim_Current(benchmark::State& state) {
    const size_t str_len = static_cast<size_t>(state.range(0));
    const size_t pad_len = static_cast<size_t>(state.range(1));
    const auto batch = BuildTrimBenchBatch(str_len, pad_len, " ");
    const char* const src_base = batch.arena.data();
    const uint32_t* const src_offsets = batch.offsets.data();
    const size_t src_bytes_size = batch.arena.size();
    const char remove_char = ' ';
    std::vector<uint32_t> dst_offsets;
    std::vector<uint8_t> dst_bytes;
    for (auto _ : state) {
        size_t first_trimmed_row = kBatch;
        for (size_t r = 0; r < kBatch; ++r) {
            const uint32_t start_off = src_offsets[r];
            const uint32_t end_off = src_offsets[r + 1];
            if (start_off < end_off) {
                const char* b = src_base + start_off;
                const char* e = src_base + end_off;
                if (*b == remove_char || *(e - 1) == remove_char) {
                    first_trimmed_row = r;
                    break;
                }
            }
        }
        if (first_trimmed_row == kBatch) {
            uint64_t unmodified_rows = first_trimmed_row;
            benchmark::DoNotOptimize(unmodified_rows);
            continue;
        }
        dst_offsets.resize(kBatch + 1);
        dst_bytes.resize(src_bytes_size);
        uint32_t* const dst_off_ptr = dst_offsets.data();
        uint8_t* const dst_byte_ptr = dst_bytes.data();
        const size_t prefix_bytes = src_offsets[first_trimmed_row];
        if (first_trimmed_row > 0) {
            std::memcpy(dst_off_ptr, src_offsets, (first_trimmed_row + 1) * sizeof(uint32_t));
            if (prefix_bytes > 0) {
                std::memcpy(dst_byte_ptr, src_base, prefix_bytes);
            }
        } else {
            dst_off_ptr[0] = 0;
        }
        size_t dst_byte_offset = prefix_bytes;
        size_t leading_spaces_num = 0;
        size_t trailing_spaces_num = 0;
        const size_t sample_end = std::min(kBatch, first_trimmed_row + 100ul);
        for (size_t i = first_trimmed_row; i < sample_end; ++i) {
            const char* const begin = src_base + src_offsets[i];
            const char* const end = src_base + src_offsets[i + 1];
            const char* from_ptr = skip_leading_char<false>(begin, end, remove_char);
            leading_spaces_num += static_cast<size_t>(from_ptr - begin);
            const char* to_ptr = skip_trailing_char<false>(from_ptr, end, remove_char);
            trailing_spaces_num += static_cast<size_t>(end - to_ptr);
            const size_t kept_len = static_cast<size_t>(to_ptr - from_ptr);
            if (kept_len > 0) {
                std::memcpy(dst_byte_ptr + dst_byte_offset, from_ptr, kept_len);
                dst_byte_offset += kept_len;
            }
            dst_off_ptr[i + 1] = static_cast<uint32_t>(dst_byte_offset);
        }
        if (sample_end < kBatch) {
            const size_t sample_count = sample_end - first_trimmed_row;
            const bool leading_simd = leading_spaces_num > 8 * sample_count;
            const bool trailing_simd = trailing_spaces_num > 8 * sample_count;
            auto run_rest = [&]<bool l_simd, bool r_simd>() {
                for (size_t i = sample_end; i < kBatch; ++i) {
                    const char* from_ptr = src_base + src_offsets[i];
                    const char* to_ptr = src_base + src_offsets[i + 1];
                    from_ptr = skip_leading_char<l_simd>(from_ptr, to_ptr, remove_char);
                    to_ptr = skip_trailing_char<r_simd>(from_ptr, to_ptr, remove_char);
                    const size_t kept_len = static_cast<size_t>(to_ptr - from_ptr);
                    if (kept_len > 0) {
                        std::memcpy(dst_byte_ptr + dst_byte_offset, from_ptr, kept_len);
                        dst_byte_offset += kept_len;
                    }
                    dst_off_ptr[i + 1] = static_cast<uint32_t>(dst_byte_offset);
                }
            };
            if (leading_simd && trailing_simd) {
                run_rest.template operator()<true, true>();
            } else if (leading_simd && !trailing_simd) {
                run_rest.template operator()<true, false>();
            } else if (!leading_simd && trailing_simd) {
                run_rest.template operator()<false, true>();
            } else {
                run_rest.template operator()<false, false>();
            }
        }
        dst_bytes.resize(dst_byte_offset);
        benchmark::DoNotOptimize(dst_offsets.data());
        benchmark::DoNotOptimize(dst_bytes.data());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch * str_len));
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch));
}

BENCHMARK(BM_TrimSpaces_LegacyScalar)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimSpaces_Current)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimSingleChar_Scalar)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimSingleChar_LegacySSE2)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimSingleChar_Current)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimAsymmetric_Scalar)->Apply(GenerateTrimAsymmetricBenchArgs);
BENCHMARK(BM_TrimAsymmetric_Current)->Apply(GenerateTrimAsymmetricBenchArgs);
BENCHMARK(BM_TrimMultiChar_LegacyFind)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimMultiChar_ByteSetScalar)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_TrimMultiChar_Current)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_ColumnTrim_Legacy)->Apply(GenerateTrimBenchArgs);
BENCHMARK(BM_ColumnTrim_Current)->Apply(GenerateTrimBenchArgs);

} // namespace starrocks

BENCHMARK_MAIN();

// =====================================================================
// Measured String TRIM / LTRIM / RTRIM & trim_spaces Before vs. After Results
// (x86-64 24 x 5.0 GHz, GCC 14.3.0 -O3 -mavx2 -mbmi2 -msse4.2, 4,096 slices/batch)
// =====================================================================
//
// | Family | (str_len, pad_len) | Legacy Baseline | Scalar (<false>) | Before Fast-Reject (<true>) | After Fast-Reject (<true>) | Speedup vs Legacy / Before |
// |---|---:|---:|---:|---:|---:|---:|
// | trim_spaces | 12/0 | 2,718 ns (16.84 Gi/s) | - | 1,680 ns (27.26 Gi/s) | 1,491 ns (30.70 Gi/s) | 1.82x / 1.13x |
// | trim_spaces | 24/0 | 2,761 ns (33.16 Gi/s) | - | 1,665 ns (54.99 Gi/s) | 1,512 ns (60.54 Gi/s) | 1.83x / 1.10x |
// | trim_spaces | 48/0 | 2,732 ns (67.01 Gi/s) | - | 1,738 ns (105.33 Gi/s) | 1,706 ns (107.35 Gi/s) | 1.60x / 1.02x |
// | trim_spaces | 192/0 | 3,265 ns (224.31 Gi/s) | - | 2,366 ns (309.59 Gi/s) | 2,309 ns (317.26 Gi/s) | 1.41x / 1.02x |
// | trim_spaces | 12/1 | 4,173 ns (10.97 Gi/s) | - | 5,093 ns (8.99 Gi/s) | 2,861 ns (16.00 Gi/s) | 1.46x / 1.78x |
// | trim_spaces | 48/1 | 4,194 ns (43.66 Gi/s) | - | 5,096 ns (35.93 Gi/s) | 2,920 ns (62.72 Gi/s) | 1.44x / 1.75x |
// | trim_spaces | 192/1 | 4,332 ns (169.08 Gi/s) | - | 5,069 ns (144.49 Gi/s) | 3,813 ns (192.09 Gi/s) | 1.14x / 1.33x |
// | trim_spaces | 64/24 | 33,475 ns (7.29 Gi/s) | - | 6,419 ns (38.03 Gi/s) | 6,337 ns (38.53 Gi/s) | 5.28x / 1.01x |
// | trim_spaces | 256/96 | 157,268 ns (6.21 Gi/s) | - | 11,488 ns (85.01 Gi/s) | 11,451 ns (85.28 Gi/s) | 13.73x / 1.00x |
// | TrimSingleChar | 12/0 | 4,737 ns (9.66 Gi/s) | 1,716 ns (26.68 Gi/s) | 5,120 ns (8.94 Gi/s) | 1,699 ns (26.95 Gi/s) | 2.79x / 3.01x |
// | TrimSingleChar | 24/0 | 6,649 ns (13.77 Gi/s) | 1,724 ns (53.11 Gi/s) | 5,195 ns (17.62 Gi/s) | 1,707 ns (53.62 Gi/s) | 3.90x / 3.04x |
// | TrimSingleChar | 48/0 | 6,614 ns (27.68 Gi/s) | 1,830 ns (100.05 Gi/s) | 4,441 ns (41.24 Gi/s) | 1,851 ns (98.92 Gi/s) | 3.57x / 2.40x |
// | TrimSingleChar | 192/0 | 6,602 ns (110.95 Gi/s) | 2,514 ns (291.35 Gi/s) | 4,426 ns (165.49 Gi/s) | 2,513 ns (291.44 Gi/s) | 2.63x / 1.76x |
// | TrimSingleChar | 64/24 | 10,291 ns (23.72 Gi/s) | 31,295 ns (7.80 Gi/s) | 4,565 ns (53.48 Gi/s) | 4,706 ns (51.88 Gi/s) | 2.19x / 0.97x |
// | TrimSingleChar | 256/96 | 25,524 ns (38.26 Gi/s) | 141,582 ns (6.90 Gi/s) | 10,956 ns (89.13 Gi/s) | 11,225 ns (87.00 Gi/s) | 2.27x / 0.98x |
// | TrimAsymmetric | 48/1/0 | - | 2,609 ns (70.18 Gi/s) | 4,449 ns (41.15 Gi/s) | 3,524 ns (51.97 Gi/s) | - / 1.26x |
// | TrimAsymmetric | 48/0/1 | - | 2,711 ns (67.55 Gi/s) | 4,446 ns (41.19 Gi/s) | 3,370 ns (54.33 Gi/s) | - / 1.32x |
// | TrimMultiChar | 12/0 | 10,111 ns (4.53 Gi/s) | 3,550 ns (12.90 Gi/s) | 4,020 ns (11.39 Gi/s) | 3,333 ns (13.74 Gi/s) | 3.03x / 1.21x |
// | TrimMultiChar | 24/0 | 10,064 ns (9.10 Gi/s) | 3,548 ns (25.80 Gi/s) | 6,053 ns (15.12 Gi/s) | 3,377 ns (27.11 Gi/s) | 2.98x / 1.79x |
// | TrimMultiChar | 48/0 | 10,075 ns (18.17 Gi/s) | 3,529 ns (51.88 Gi/s) | 5,766 ns (31.76 Gi/s) | 3,345 ns (54.73 Gi/s) | 3.01x / 1.72x |
// | TrimMultiChar | 192/0 | 10,713 ns (68.37 Gi/s) | 3,795 ns (193.00 Gi/s) | 5,687 ns (128.80 Gi/s) | 3,890 ns (188.30 Gi/s) | 2.75x / 1.46x |
// | TrimMultiChar | 64/24 | 268,938 ns (930 Mi/s) | 64,824 ns (3.77 Gi/s) | 5,863 ns (41.64 Gi/s) | 7,336 ns (33.28 Gi/s) | 36.66x / 0.80x |
// | TrimMultiChar | 256/96 | 1,133,772 ns (882 Mi/s) | 245,218 ns (3.98 Gi/s) | 18,009 ns (54.23 Gi/s) | 18,811 ns (51.91 Gi/s) | 60.27x / 0.96x |
// | ColumnTrim | 12/0 | 9,402 ns (4.87 Gi/s) | - | 9,263 ns (4.94 Gi/s) | 1,204 ns (38.03 Gi/s) | 7.81x / 7.69x |
// | ColumnTrim | 24/0 | 9,186 ns (9.97 Gi/s) | - | 8,834 ns (10.36 Gi/s) | 1,265 ns (72.39 Gi/s) | 7.26x / 6.98x |
// | ColumnTrim | 48/0 | 9,499 ns (19.28 Gi/s) | - | 8,871 ns (20.64 Gi/s) | 1,485 ns (123.33 Gi/s) | 6.40x / 5.97x |
// | ColumnTrim | 192/0 | 15,236 ns (48.07 Gi/s) | - | 14,767 ns (49.60 Gi/s) | 1,763 ns (415.44 Gi/s) | 8.64x / 8.38x |
// | ColumnTrim | 12/1 | 12,721 ns (3.60 Gi/s) | - | 12,965 ns (3.53 Gi/s) | 7,938 ns (5.77 Gi/s) | 1.60x / 1.63x |
// | ColumnTrim | 48/1 | 10,483 ns (17.47 Gi/s) | - | 10,275 ns (17.82 Gi/s) | 8,031 ns (22.80 Gi/s) | 1.31x / 1.28x |
// | ColumnTrim | 192/1 | 15,248 ns (48.03 Gi/s) | - | 15,075 ns (48.59 Gi/s) | 14,631 ns (50.06 Gi/s) | 1.04x / 1.03x |
// | ColumnTrim | 64/2 | 13,015 ns (18.76 Gi/s) | - | 12,869 ns (18.97 Gi/s) | 8,907 ns (27.41 Gi/s) | 1.46x / 1.44x |
// | ColumnTrim | 64/24 | 15,881 ns (15.37 Gi/s) | - | 15,801 ns (15.45 Gi/s) | 10,502 ns (23.25 Gi/s) | 1.51x / 1.50x |
// | ColumnTrim | 256/96 | 35,062 ns (27.85 Gi/s) | - | 34,930 ns (27.96 Gi/s) | 25,394 ns (38.46 Gi/s) | 1.38x / 1.38x |
// =====================================================================
