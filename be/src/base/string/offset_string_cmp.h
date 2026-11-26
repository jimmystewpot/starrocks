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

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "base/hash/unaligned_access.h"
#include "base/simd/string_length_filter.h"
#include "base/string/memcmp.h"
#include "base/string/slice.h"

namespace starrocks {

enum class OffsetStringMatchMode {
    EQUALS,
    NOT_EQUALS,
    STARTS_WITH,
    ENDS_WITH,
};

namespace detail {

inline bool compare_interior_gt32_simd(const char* cand, const char* pat_data, size_t pat_len) {
    [[maybe_unused]] size_t off = 8;
#if defined(__SSE2__)
    while (off + 16 <= pat_len - 8) {
        __m128i vc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cand + off));
        __m128i vp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pat_data + off));
        __m128i diff = _mm_cmpeq_epi8(vc, vp);
        if (_mm_movemask_epi8(diff) != 0xffff) {
            return false;
        }
        off += 16;
    }
    if (off < pat_len - 8) {
        __m128i vc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cand + pat_len - 24));
        __m128i vp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pat_data + pat_len - 24));
        __m128i diff = _mm_cmpeq_epi8(vc, vp);
        if (_mm_movemask_epi8(diff) != 0xffff) {
            return false;
        }
    }
    return true;
#elif defined(__ARM_NEON) && defined(__aarch64__)
    while (off + 16 <= pat_len - 8) {
        uint8x16_t vc = vld1q_u8(reinterpret_cast<const uint8_t*>(cand + off));
        uint8x16_t vp = vld1q_u8(reinterpret_cast<const uint8_t*>(pat_data + off));
        uint8x16_t diff = veorq_u8(vc, vp);
        if (vmaxvq_u8(diff) != 0) {
            return false;
        }
        off += 16;
    }
    if (off < pat_len - 8) {
        uint8x16_t vc = vld1q_u8(reinterpret_cast<const uint8_t*>(cand + pat_len - 24));
        uint8x16_t vp = vld1q_u8(reinterpret_cast<const uint8_t*>(pat_data + pat_len - 24));
        uint8x16_t diff = veorq_u8(vc, vp);
        if (vmaxvq_u8(diff) != 0) {
            return false;
        }
    }
    return true;
#else
    return std::memcmp(cand + 8, pat_data + 8, pat_len - 16) == 0;
#endif
}

template <OffsetStringMatchMode Mode, typename OffsetT, typename ResultT, typename MatchFn>
inline void eval_with_comparator(const OffsetT* __restrict offset_data, const char* __restrict base_ptr, size_t pat_len,
                                 size_t num_rows, ResultT* __restrict dst, MatchFn&& match_fn) {
    static_assert(sizeof(ResultT) == 1);
    constexpr bool kIsNe = (Mode == OffsetStringMatchMode::NOT_EQUALS);

    size_t i = 0;
    if constexpr (sizeof(OffsetT) == sizeof(uint32_t) && SIMD::kStringLenSimdWidth > 1) {
        constexpr size_t kSimdWidth = SIMD::kStringLenSimdWidth;
        const auto* offsets32 = reinterpret_cast<const uint32_t*>(offset_data);
        const auto pat_len32 = static_cast<uint32_t>(pat_len);
        for (; i + kSimdWidth <= num_rows; i += kSimdWidth) {
            uint32_t mask = 0;
            if constexpr (Mode == OffsetStringMatchMode::EQUALS || Mode == OffsetStringMatchMode::NOT_EQUALS) {
                mask = SIMD::length_eq_mask(offsets32, i, pat_len32);
            } else {
                mask = SIMD::length_in_range_mask(offsets32, i, pat_len32, std::numeric_limits<uint32_t>::max());
            }

            if (mask == 0) {
                std::memset(dst + i, kIsNe ? 1 : 0, kSimdWidth);
                continue;
            }

            if (mask == SIMD::kStringLenSimdMask) {
                for (size_t k = 0; k < kSimdWidth; ++k) {
                    const size_t row = i + k;
                    const char* cand = (Mode == OffsetStringMatchMode::ENDS_WITH)
                                               ? (base_ptr + offset_data[row + 1] - pat_len)
                                               : (base_ptr + offset_data[row]);
                    dst[row] = static_cast<ResultT>(match_fn(cand) ^ kIsNe);
                }
                continue;
            }

            std::memset(dst + i, kIsNe ? 1 : 0, kSimdWidth);
            do {
                const auto bit = static_cast<uint32_t>(__builtin_ctz(mask));
                const size_t row = i + bit;
                const char* cand = (Mode == OffsetStringMatchMode::ENDS_WITH)
                                           ? (base_ptr + offset_data[row + 1] - pat_len)
                                           : (base_ptr + offset_data[row]);
                dst[row] = static_cast<ResultT>(match_fn(cand) ^ kIsNe);
                mask &= (mask - 1);
            } while (mask != 0);
        }
    }

    for (; i < num_rows; ++i) {
        const OffsetT off0 = offset_data[i];
        const OffsetT off1 = offset_data[i + 1];
        const auto row_len = static_cast<size_t>(off1 - off0);
        bool len_ok = false;
        if constexpr (Mode == OffsetStringMatchMode::EQUALS || Mode == OffsetStringMatchMode::NOT_EQUALS) {
            len_ok = (row_len == pat_len);
        } else {
            len_ok = (row_len >= pat_len);
        }
        if (len_ok) {
            const char* cand =
                    (Mode == OffsetStringMatchMode::ENDS_WITH) ? (base_ptr + off1 - pat_len) : (base_ptr + off0);
            dst[i] = static_cast<ResultT>(match_fn(cand) ^ kIsNe);
        } else {
            dst[i] = static_cast<ResultT>(kIsNe);
        }
    }
}

} // namespace detail

template <OffsetStringMatchMode Mode, typename OffsetT, typename ResultT>
inline void eval_offset_string_const(const OffsetT* __restrict offsets, const uint8_t* __restrict base_ptr,
                                     size_t total_bytes, const Slice& pattern, size_t num_rows,
                                     ResultT* __restrict dst) {
    static_assert(sizeof(ResultT) == 1);
    constexpr bool kIsNe = (Mode == OffsetStringMatchMode::NOT_EQUALS);

    if (num_rows == 0) {
        return;
    }

    const size_t pat_size = pattern.size;
    if (pat_size == 0) {
        if constexpr (Mode == OffsetStringMatchMode::STARTS_WITH || Mode == OffsetStringMatchMode::ENDS_WITH) {
            std::memset(dst, 1, num_rows);
        } else {
            for (size_t i = 0; i < num_rows; ++i) {
                dst[i] = static_cast<ResultT>((offsets[i + 1] == offsets[i]) ^ kIsNe);
            }
        }
        return;
    }

    if (total_bytes < pat_size ||
        (sizeof(OffsetT) == sizeof(uint32_t) && pat_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))) {
        std::memset(dst, kIsNe ? 1 : 0, num_rows);
        return;
    }

    const char* const pat_data = pattern.data;
    const char* const base_char = reinterpret_cast<const char*>(base_ptr);

    switch (pat_size) {
    case 1: {
        const char c0 = pat_data[0];
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(offsets, base_char, pat_size, num_rows, dst,
                                                             [c0](const char* cand) { return cand[0] == c0; });
        break;
    }
    case 2: {
        const uint16_t w16 = unaligned_load<uint16_t>(pat_data);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst,
                [w16](const char* cand) { return unaligned_load<uint16_t>(cand) == w16; });
        break;
    }
    case 3: {
        const uint16_t head16 = unaligned_load<uint16_t>(pat_data);
        const char tail8 = pat_data[2];
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst, [head16, tail8](const char* cand) {
                    return (unaligned_load<uint16_t>(cand) == head16) && (cand[2] == tail8);
                });
        break;
    }
    case 4: {
        const uint32_t w32 = unaligned_load<uint32_t>(pat_data);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst,
                [w32](const char* cand) { return unaligned_load<uint32_t>(cand) == w32; });
        break;
    }
    case 5:
    case 6:
    case 7: {
        const uint32_t head32 = unaligned_load<uint32_t>(pat_data);
        const uint32_t tail32 = unaligned_load<uint32_t>(pat_data + pat_size - 4);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst, [head32, tail32, pat_size](const char* cand) {
                    return ((unaligned_load<uint32_t>(cand) ^ head32) |
                            (unaligned_load<uint32_t>(cand + pat_size - 4) ^ tail32)) == 0;
                });
        break;
    }
    case 8: {
        const uint64_t w64 = unaligned_load<uint64_t>(pat_data);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst,
                [w64](const char* cand) { return unaligned_load<uint64_t>(cand) == w64; });
        break;
    }
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16: {
        const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
        const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_size - 8);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst, [head64, tail64, pat_size](const char* cand) {
                    return ((unaligned_load<uint64_t>(cand) ^ head64) |
                            (unaligned_load<uint64_t>(cand + pat_size - 8) ^ tail64)) == 0;
                });
        break;
    }
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24: {
        const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
        const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_size - 8);
        const uint64_t mid64 = unaligned_load<uint64_t>(pat_data + 8);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst, [head64, tail64, mid64, pat_size](const char* cand) {
                    return (((unaligned_load<uint64_t>(cand) ^ head64) |
                             (unaligned_load<uint64_t>(cand + pat_size - 8) ^ tail64)) == 0) &&
                           (unaligned_load<uint64_t>(cand + 8) == mid64);
                });
        break;
    }
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32: {
        const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
        const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_size - 8);
        const uint64_t mid_head64 = unaligned_load<uint64_t>(pat_data + 8);
        const uint64_t mid_tail64 = unaligned_load<uint64_t>(pat_data + pat_size - 16);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst,
                [head64, tail64, mid_head64, mid_tail64, pat_size](const char* cand) {
                    return (((unaligned_load<uint64_t>(cand) ^ head64) |
                             (unaligned_load<uint64_t>(cand + pat_size - 8) ^ tail64)) == 0) &&
                           (((unaligned_load<uint64_t>(cand + 8) ^ mid_head64) |
                             (unaligned_load<uint64_t>(cand + pat_size - 16) ^ mid_tail64)) == 0);
                });
        break;
    }
    default: {
        const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
        const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_size - 8);
        detail::eval_with_comparator<Mode, OffsetT, ResultT>(
                offsets, base_char, pat_size, num_rows, dst, [head64, tail64, pat_data, pat_size](const char* cand) {
                    if (((unaligned_load<uint64_t>(cand) ^ head64) |
                         (unaligned_load<uint64_t>(cand + pat_size - 8) ^ tail64)) != 0) {
                        return false;
                    }
                    return detail::compare_interior_gt32_simd(cand, pat_data, pat_size);
                });
        break;
    }
    }
}

template <bool IsNe, typename OffsetT1, typename OffsetT2, typename ResultT>
inline void eval_offset_string_vec(const OffsetT1* __restrict off1, const uint8_t* __restrict base1,
                                   const OffsetT2* __restrict off2, const uint8_t* __restrict base2, size_t num_rows,
                                   ResultT* __restrict dst) {
    static_assert(sizeof(ResultT) == 1);
    const char* const b1 = reinterpret_cast<const char*>(base1);
    const char* const b2 = reinterpret_cast<const char*>(base2);
    for (size_t i = 0; i < num_rows; ++i) {
        const OffsetT1 o1 = off1[i];
        const OffsetT2 o2 = off2[i];
        const auto len1 = static_cast<size_t>(off1[i + 1] - o1);
        const auto len2 = static_cast<size_t>(off2[i + 1] - o2);
        const bool eq = (len1 == len2) && memequal(b1 + o1, len1, b2 + o2, len1);
        dst[i] = static_cast<ResultT>(eq ^ IsNe);
    }
}

} // namespace starrocks
