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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace starrocks {

class StringSearcher {
private:
    /// string to be searched for
    const uint8_t* const needle;
    size_t needle_size;
    /// first, last, and discriminating interior anchor in `needle`
    uint8_t first{};
    uint8_t last{};
    uint8_t mid{};
    size_t mid_off{};
    size_t alt_off{};

    static constexpr size_t kMaxAnchorScan = 64;

    inline bool _verify_interior(const uint8_t* pos) const {
        if (needle_size <= 2) {
            return true;
        }
        if (pos[mid_off] != mid || pos[alt_off] != needle[alt_off]) {
            return false;
        }
        return std::memcmp(pos + 1, needle + 1, needle_size - 2) == 0;
    }

    inline const uint8_t* _search_memmem_from(const uint8_t* from, const uint8_t* haystack_end) const {
        if (from >= haystack_end || static_cast<size_t>(haystack_end - from) < needle_size) {
            return haystack_end;
        }
        const void* res = memmem(from, static_cast<size_t>(haystack_end - from), needle, needle_size);
        return res != nullptr ? static_cast<const uint8_t*>(res) : haystack_end;
    }

    // Crochemore-Perrin Two-Way algorithm run on the virtual reversed sequence to find the
    // rightmost match starting strictly before `cand` in O(N + M) worst-case time and O(1) space.
    const uint8_t* _rsearch_two_way_upto(const uint8_t* haystack, const uint8_t* cand,
                                         const uint8_t* haystack_end) const {
        if (cand <= haystack) {
            return haystack_end;
        }
        const size_t M = needle_size;
        const uint8_t* const rev_end = cand - 1 + M;
        const size_t N = static_cast<size_t>(rev_end - haystack);
        auto n_at = [&](size_t i) -> uint8_t { return needle[M - 1 - i]; };
        auto h_at = [&](size_t idx) -> uint8_t { return *(rev_end - 1 - idx); };

        auto max_suffix_rev = [&](bool less_order, size_t* period_out) -> size_t {
            size_t ms = std::numeric_limits<size_t>::max();
            size_t j = 0;
            size_t k = 1;
            size_t p = 1;
            while (j + k < M) {
                const uint8_t a = n_at(j + k);
                const uint8_t b = n_at(static_cast<size_t>(ms + k));
                if (less_order ? (a < b) : (a > b)) {
                    j += k;
                    k = 1;
                    p = j - ms;
                } else if (a == b) {
                    if (k != p) {
                        ++k;
                    } else {
                        j += p;
                        k = 1;
                    }
                } else {
                    ms = j;
                    j = ms + 1;
                    k = 1;
                    p = 1;
                }
            }
            *period_out = p;
            return ms + 1;
        };

        size_t p1 = 1;
        size_t p2 = 1;
        const size_t ms1 = max_suffix_rev(true, &p1);
        const size_t ms2 = max_suffix_rev(false, &p2);
        const size_t crit_pos = (ms1 > ms2) ? ms1 : ms2;
        size_t period = (ms1 > ms2) ? p1 : p2;

        const bool periodic =
                (crit_pos == 0) || (std::memcmp(needle + M - crit_pos, needle + M - period - crit_pos, crit_pos) == 0);
        if (!periodic) {
            period = std::max(crit_pos, M - crit_pos) + 1;
        }

        size_t j = 0;
        size_t memory = 0;
        while (j <= N - M) {
            size_t i = std::max(crit_pos, memory);
            while (i < M && n_at(i) == h_at(j + i)) {
                ++i;
            }
            if (i < M) {
                j += i - crit_pos + 1;
                memory = 0;
                continue;
            }
            i = crit_pos;
            while (i > memory && n_at(i - 1) == h_at(j + i - 1)) {
                --i;
            }
            if (i <= memory) {
                return (cand - 1) - j;
            }
            j += period;
            memory = periodic ? (M - period) : 0;
        }
        return haystack_end;
    }

#if defined(__x86_64__)
    __attribute__((target("sse4.1"))) const uint8_t* _search_sse41_from(const uint8_t* p,
                                                                        const uint8_t* haystack_end) const {
        const size_t last_off = needle_size - 1;
        const __m128i v_first = _mm_set1_epi8(static_cast<char>(first));
        const __m128i v_last = _mm_set1_epi8(static_cast<char>(last));
        const __m128i v_mid = _mm_set1_epi8(static_cast<char>(mid));

        while (static_cast<size_t>(haystack_end - p) >= last_off + 16) {
            const __m128i f0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i l0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + last_off));
            __m128i eq0 = _mm_and_si128(_mm_cmpeq_epi8(f0, v_first), _mm_cmpeq_epi8(l0, v_last));
            uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0));
            if (mask != 0 && needle_size > 2) {
                const __m128i m0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + mid_off));
                eq0 = _mm_and_si128(eq0, _mm_cmpeq_epi8(m0, v_mid));
                mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0));
            }
            while (mask != 0) {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
                if (_verify_interior(p + bit)) {
                    return p + bit;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _search_memmem_from(p + bit + 1, haystack_end);
                }
                mask &= (mask - 1);
            }
            p += 16;
        }

        if (static_cast<size_t>(haystack_end - p) >= needle_size) {
            const uint8_t* tail_p = haystack_end - (last_off + 16);
            const size_t already_checked = static_cast<size_t>(p - tail_p);
            const __m128i f0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tail_p));
            const __m128i l0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tail_p + last_off));
            __m128i eq0 = _mm_and_si128(_mm_cmpeq_epi8(f0, v_first), _mm_cmpeq_epi8(l0, v_last));
            uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0)) & (~0U << already_checked);
            if (mask != 0 && needle_size > 2) {
                const __m128i m0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tail_p + mid_off));
                eq0 = _mm_and_si128(eq0, _mm_cmpeq_epi8(m0, v_mid));
                mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0)) & (~0U << already_checked);
            }
            while (mask != 0) {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
                if (_verify_interior(tail_p + bit)) {
                    return tail_p + bit;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _search_memmem_from(tail_p + bit + 1, haystack_end);
                }
                mask &= (mask - 1);
            }
        }

        return haystack_end;
    }

    __attribute__((target("sse4.1"))) const uint8_t* _rsearch_sse41_upto(const uint8_t* haystack, const uint8_t* p,
                                                                         const uint8_t* haystack_end) const {
        const size_t last_off = needle_size - 1;
        const __m128i v_first = _mm_set1_epi8(static_cast<char>(first));
        const __m128i v_last = _mm_set1_epi8(static_cast<char>(last));
        const __m128i v_mid = _mm_set1_epi8(static_cast<char>(mid));

        while (static_cast<size_t>(p - haystack) >= 16) {
            p -= 16;
            const __m128i f0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i l0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + last_off));
            __m128i eq0 = _mm_and_si128(_mm_cmpeq_epi8(f0, v_first), _mm_cmpeq_epi8(l0, v_last));
            uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0));
            if (mask != 0 && needle_size > 2) {
                const __m128i m0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + mid_off));
                eq0 = _mm_and_si128(eq0, _mm_cmpeq_epi8(m0, v_mid));
                mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0));
            }
            while (mask != 0) {
                const uint32_t bit = 31U - static_cast<uint32_t>(__builtin_clz(mask));
                if (_verify_interior(p + bit)) {
                    return p + bit;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _rsearch_two_way_upto(haystack, p + bit, haystack_end);
                }
                mask ^= (1U << bit);
            }
        }

        if (p > haystack) {
            const size_t rem = static_cast<size_t>(p - haystack);
            const uint32_t head_mask = (1U << rem) - 1U;
            const __m128i f0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(haystack));
            const __m128i l0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(haystack + last_off));
            __m128i eq0 = _mm_and_si128(_mm_cmpeq_epi8(f0, v_first), _mm_cmpeq_epi8(l0, v_last));
            uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0)) & head_mask;
            if (mask != 0 && needle_size > 2) {
                const __m128i m0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(haystack + mid_off));
                eq0 = _mm_and_si128(eq0, _mm_cmpeq_epi8(m0, v_mid));
                mask = static_cast<uint32_t>(_mm_movemask_epi8(eq0)) & head_mask;
            }
            while (mask != 0) {
                const uint32_t bit = 31U - static_cast<uint32_t>(__builtin_clz(mask));
                if (_verify_interior(haystack + bit)) {
                    return haystack + bit;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _rsearch_two_way_upto(haystack, haystack + bit, haystack_end);
                }
                mask ^= (1U << bit);
            }
        }

        return haystack_end;
    }
#endif

public:
    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    StringSearcher(const CharT* needle_, const size_t needle_size_)
            : needle{reinterpret_cast<const uint8_t*>(needle_)}, needle_size(needle_size_) {
        if (0 == needle_size) {
            return;
        }

        first = *needle;
        last = needle[needle_size - 1];
        if (needle_size > 2) {
            const size_t max_interior = std::min<size_t>(needle_size - 2, kMaxAnchorScan);
            const size_t center = (1 + max_interior) / 2;
            size_t chosen_mid = center;
            bool found_mid = false;
            for (size_t delta = 0; delta <= max_interior; ++delta) {
                const size_t right = center + delta;
                if (right <= max_interior && needle[right] != first && needle[right] != last) {
                    chosen_mid = right;
                    found_mid = true;
                    break;
                }
                if (delta <= center - 1) {
                    const size_t left = center - delta;
                    if (left >= 1 && needle[left] != first && needle[left] != last) {
                        chosen_mid = left;
                        found_mid = true;
                        break;
                    }
                }
            }
            // If the head window is homogeneous, also inspect up to kMaxAnchorScan bytes at the tail
            // (which resides in the same trailing page already touched by `last = needle[needle_size - 1]`).
            const size_t tail_min = (needle_size - 2 > kMaxAnchorScan)
                                            ? std::max<size_t>(max_interior + 1, (needle_size - 1) - kMaxAnchorScan)
                                            : max_interior + 1;
            if (!found_mid && needle_size - 2 >= tail_min) {
                for (size_t i = needle_size - 2; i >= tail_min; --i) {
                    if (needle[i] != first && needle[i] != last) {
                        chosen_mid = i;
                        found_mid = true;
                        break;
                    }
                }
            }
            mid_off = chosen_mid;
            mid = needle[mid_off];

            size_t chosen_alt = (chosen_mid != 1) ? 1 : max_interior;
            bool found_alt = false;
            for (size_t i = max_interior; i >= 1; --i) {
                if (i != chosen_mid && needle[i] != first && needle[i] != last && needle[i] != mid) {
                    chosen_alt = i;
                    found_alt = true;
                    break;
                }
            }
            if (!found_alt && needle_size - 2 >= tail_min) {
                for (size_t i = needle_size - 2; i >= tail_min; --i) {
                    if (i != chosen_mid && needle[i] != first && needle[i] != last && needle[i] != mid) {
                        chosen_alt = i;
                        break;
                    }
                }
            }
            alt_off = chosen_alt;
        }
    }

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    inline bool compare(const CharT* haystack, const CharT* haystack_end, const CharT* pos) const {
        if (needle_size == 0) {
            return true;
        }
        if (haystack == nullptr || haystack_end == nullptr || pos == nullptr || pos < haystack || pos > haystack_end ||
            static_cast<size_t>(haystack_end - pos) < needle_size) {
            return false;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(pos);
        if (p[0] != first || p[needle_size - 1] != last) {
            return false;
        }
        return _verify_interior(p);
    }

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    inline bool compare_lt_sse4_1_width(const CharT* haystack, const CharT* haystack_end, const CharT* pos) const {
        return compare(haystack, haystack_end, pos);
    }

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    inline bool compare_ge_sse4_1_width(const CharT* haystack, const CharT* haystack_end, const CharT* pos) const {
        return compare(haystack, haystack_end, pos);
    }

    const uint8_t* search_scalar(const uint8_t* haystack, const uint8_t* haystack_end) const {
        if (needle_size == 0) {
            return haystack;
        }
        if (haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < needle_size) {
            return haystack_end;
        }
        if (needle_size == 1) {
            const void* res = std::memchr(haystack, first, static_cast<size_t>(haystack_end - haystack));
            return res != nullptr ? static_cast<const uint8_t*>(res) : haystack_end;
        }

        const size_t last_off = needle_size - 1;
        const uint8_t* p = haystack;
        while (static_cast<size_t>(haystack_end - p) >= needle_size) {
            if (p[0] == first && p[last_off] == last) {
                if (_verify_interior(p)) {
                    return p;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _search_memmem_from(p + 1, haystack_end);
                }
            }
            ++p;
        }
        return haystack_end;
    }

#if defined(__x86_64__)
    __attribute__((target("sse4.1"))) const uint8_t* search_sse41(const uint8_t* haystack,
                                                                  const uint8_t* haystack_end) const {
        if (needle_size <= 1 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < (needle_size - 1) + 16) {
            return search_scalar(haystack, haystack_end);
        }
        return _search_sse41_from(haystack, haystack_end);
    }

    __attribute__((target("avx2"))) const uint8_t* search_avx2(const uint8_t* haystack,
                                                               const uint8_t* haystack_end) const {
        if (needle_size <= 1 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < (needle_size - 1) + 16) {
            return search_scalar(haystack, haystack_end);
        }

        const size_t last_off = needle_size - 1;
        const __m256i v_first256 = _mm256_set1_epi8(static_cast<char>(first));
        const __m256i v_last256 = _mm256_set1_epi8(static_cast<char>(last));
        const __m256i v_mid256 = _mm256_set1_epi8(static_cast<char>(mid));
        const uint8_t* p = haystack;

        while (static_cast<size_t>(haystack_end - p) >= last_off + 32) {
            const __m256i f = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            const __m256i l = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + last_off));
            __m256i eq = _mm256_and_si256(_mm256_cmpeq_epi8(f, v_first256), _mm256_cmpeq_epi8(l, v_last256));
            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
            if (mask != 0 && needle_size > 2) {
                const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + mid_off));
                eq = _mm256_and_si256(eq, _mm256_cmpeq_epi8(m, v_mid256));
                mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
            }
            while (mask != 0) {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
                if (_verify_interior(p + bit)) {
                    return p + bit;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _search_memmem_from(p + bit + 1, haystack_end);
                }
                mask &= (mask - 1);
            }
            p += 32;
        }

        return _search_sse41_from(p, haystack_end);
    }
#elif defined(__ARM_NEON)
    const uint8_t* search_neon(const uint8_t* haystack, const uint8_t* haystack_end) const {
        if (needle_size <= 1 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < (needle_size - 1) + 16) {
            return search_scalar(haystack, haystack_end);
        }

        const size_t last_off = needle_size - 1;
        const uint8x16_t v_first = vdupq_n_u8(first);
        const uint8x16_t v_last = vdupq_n_u8(last);
        const uint8x16_t v_mid = vdupq_n_u8(mid);
        const uint8_t* p = haystack;

        while (static_cast<size_t>(haystack_end - p) >= last_off + 32) {
            const uint8x16_t f0 = vld1q_u8(p);
            const uint8x16_t l0 = vld1q_u8(p + last_off);
            const uint8x16_t f1 = vld1q_u8(p + 16);
            const uint8x16_t l1 = vld1q_u8(p + 16 + last_off);

            uint8x16_t eq0 = vandq_u8(vceqq_u8(f0, v_first), vceqq_u8(l0, v_last));
            uint8x16_t eq1 = vandq_u8(vceqq_u8(f1, v_first), vceqq_u8(l1, v_last));

            if (vmaxvq_u8(vorrq_u8(eq0, eq1)) != 0) {
                if (needle_size > 2) {
                    const uint8x16_t m0 = vld1q_u8(p + mid_off);
                    const uint8x16_t m1 = vld1q_u8(p + 16 + mid_off);
                    eq0 = vandq_u8(eq0, vceqq_u8(m0, v_mid));
                    eq1 = vandq_u8(eq1, vceqq_u8(m1, v_mid));
                }
                uint64_t mask0 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq0), 4)), 0);
                while (mask0 != 0) {
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask0) >> 2);
                    if (_verify_interior(p + bit)) {
                        return p + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _search_memmem_from(p + bit + 1, haystack_end);
                    }
                    mask0 &= ~(0xFULL << (bit << 2));
                }
                uint64_t mask1 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq1), 4)), 0);
                while (mask1 != 0) {
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask1) >> 2);
                    if (_verify_interior(p + 16 + bit)) {
                        return p + 16 + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _search_memmem_from(p + 16 + bit + 1, haystack_end);
                    }
                    mask1 &= ~(0xFULL << (bit << 2));
                }
            }
            p += 32;
        }

        while (static_cast<size_t>(haystack_end - p) >= last_off + 16) {
            const uint8x16_t f0 = vld1q_u8(p);
            const uint8x16_t l0 = vld1q_u8(p + last_off);
            uint8x16_t eq0 = vandq_u8(vceqq_u8(f0, v_first), vceqq_u8(l0, v_last));
            if (vmaxvq_u8(eq0) != 0) {
                if (needle_size > 2) {
                    const uint8x16_t m0 = vld1q_u8(p + mid_off);
                    eq0 = vandq_u8(eq0, vceqq_u8(m0, v_mid));
                }
                uint64_t mask0 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq0), 4)), 0);
                while (mask0 != 0) {
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask0) >> 2);
                    if (_verify_interior(p + bit)) {
                        return p + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _search_memmem_from(p + bit + 1, haystack_end);
                    }
                    mask0 &= ~(0xFULL << (bit << 2));
                }
            }
            p += 16;
        }

        if (static_cast<size_t>(haystack_end - p) >= needle_size) {
            const uint8_t* tail_p = haystack_end - (last_off + 16);
            const size_t already_checked = static_cast<size_t>(p - tail_p);
            const uint8x16_t f0 = vld1q_u8(tail_p);
            const uint8x16_t l0 = vld1q_u8(tail_p + last_off);
            uint8x16_t eq0 = vandq_u8(vceqq_u8(f0, v_first), vceqq_u8(l0, v_last));
            if (vmaxvq_u8(eq0) != 0) {
                if (needle_size > 2) {
                    const uint8x16_t m0 = vld1q_u8(tail_p + mid_off);
                    eq0 = vandq_u8(eq0, vceqq_u8(m0, v_mid));
                }
                uint64_t mask64 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq0), 4)), 0);
                mask64 &= (~0ULL << (already_checked << 2));
                while (mask64 != 0) {
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask64) >> 2);
                    if (_verify_interior(tail_p + bit)) {
                        return tail_p + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _search_memmem_from(tail_p + bit + 1, haystack_end);
                    }
                    mask64 &= ~(0xFULL << (bit << 2));
                }
            }
        }

        return haystack_end;
    }
#endif

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    const CharT* search(const CharT* haystack, const CharT* const haystack_end) const {
        const auto* h = reinterpret_cast<const uint8_t*>(haystack);
        const auto* h_end = reinterpret_cast<const uint8_t*>(haystack_end);
#if defined(__AVX2__)
        return reinterpret_cast<const CharT*>(search_avx2(h, h_end));
#elif defined(__SSE4_1__)
        return reinterpret_cast<const CharT*>(search_sse41(h, h_end));
#elif defined(__ARM_NEON)
        return reinterpret_cast<const CharT*>(search_neon(h, h_end));
#else
        return reinterpret_cast<const CharT*>(search_scalar(h, h_end));
#endif
    }

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    const CharT* search(const CharT* haystack, const size_t haystack_size) const {
        return search(haystack, haystack + haystack_size);
    }

    const uint8_t* rsearch_scalar(const uint8_t* haystack, const uint8_t* haystack_end) const {
        if (needle_size == 0 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < needle_size) {
            return haystack_end;
        }
        if (needle_size == 1) {
#if defined(__linux__) || defined(__GLIBC__)
            const void* res = memrchr(haystack, first, static_cast<size_t>(haystack_end - haystack));
            return res != nullptr ? static_cast<const uint8_t*>(res) : haystack_end;
#else
            const uint8_t* p = haystack_end;
            while (p > haystack) {
                --p;
                if (*p == first) {
                    return p;
                }
            }
            return haystack_end;
#endif
        }

        const size_t last_off = needle_size - 1;
        const uint8_t* p = haystack_end - needle_size;
        while (true) {
            if (p[0] == first && p[last_off] == last) {
                if (_verify_interior(p)) {
                    return p;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _rsearch_two_way_upto(haystack, p, haystack_end);
                }
            }
            if (p == haystack) {
                break;
            }
            --p;
        }
        return haystack_end;
    }

#if defined(__x86_64__)
    __attribute__((target("sse4.1"))) const uint8_t* rsearch_sse41(const uint8_t* haystack,
                                                                   const uint8_t* haystack_end) const {
        if (needle_size <= 1 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < (needle_size - 1) + 16) {
            return rsearch_scalar(haystack, haystack_end);
        }
        const size_t last_off = needle_size - 1;
        return _rsearch_sse41_upto(haystack, haystack_end - last_off, haystack_end);
    }

    __attribute__((target("avx2"))) const uint8_t* rsearch_avx2(const uint8_t* haystack,
                                                                const uint8_t* haystack_end) const {
        if (needle_size <= 1 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < (needle_size - 1) + 16) {
            return rsearch_scalar(haystack, haystack_end);
        }

        const size_t last_off = needle_size - 1;
        const __m256i v_first256 = _mm256_set1_epi8(static_cast<char>(first));
        const __m256i v_last256 = _mm256_set1_epi8(static_cast<char>(last));
        const __m256i v_mid256 = _mm256_set1_epi8(static_cast<char>(mid));
        const uint8_t* p = haystack_end - last_off;

        while (static_cast<size_t>(p - haystack) >= 32) {
            p -= 32;
            const __m256i f = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            const __m256i l = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + last_off));
            __m256i eq = _mm256_and_si256(_mm256_cmpeq_epi8(f, v_first256), _mm256_cmpeq_epi8(l, v_last256));
            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
            if (mask != 0 && needle_size > 2) {
                const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + mid_off));
                eq = _mm256_and_si256(eq, _mm256_cmpeq_epi8(m, v_mid256));
                mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
            }
            while (mask != 0) {
                const uint32_t bit = 31U - static_cast<uint32_t>(__builtin_clz(mask));
                if (_verify_interior(p + bit)) {
                    return p + bit;
                }
                if (needle_size > kMaxAnchorScan) {
                    return _rsearch_two_way_upto(haystack, p + bit, haystack_end);
                }
                mask ^= (1U << bit);
            }
        }

        return _rsearch_sse41_upto(haystack, p, haystack_end);
    }
#elif defined(__ARM_NEON)
    const uint8_t* rsearch_neon(const uint8_t* haystack, const uint8_t* haystack_end) const {
        if (needle_size <= 1 || haystack == nullptr || haystack_end == nullptr || haystack >= haystack_end ||
            static_cast<size_t>(haystack_end - haystack) < (needle_size - 1) + 16) {
            return rsearch_scalar(haystack, haystack_end);
        }

        const size_t last_off = needle_size - 1;
        const uint8x16_t v_first = vdupq_n_u8(first);
        const uint8x16_t v_last = vdupq_n_u8(last);
        const uint8x16_t v_mid = vdupq_n_u8(mid);
        const uint8_t* p = haystack_end - last_off;

        while (static_cast<size_t>(p - haystack) >= 32) {
            p -= 32;
            const uint8x16_t f0 = vld1q_u8(p);
            const uint8x16_t l0 = vld1q_u8(p + last_off);
            const uint8x16_t f1 = vld1q_u8(p + 16);
            const uint8x16_t l1 = vld1q_u8(p + 16 + last_off);

            uint8x16_t eq0 = vandq_u8(vceqq_u8(f0, v_first), vceqq_u8(l0, v_last));
            uint8x16_t eq1 = vandq_u8(vceqq_u8(f1, v_first), vceqq_u8(l1, v_last));

            if (vmaxvq_u8(vorrq_u8(eq0, eq1)) != 0) {
                if (needle_size > 2) {
                    const uint8x16_t m0 = vld1q_u8(p + mid_off);
                    const uint8x16_t m1 = vld1q_u8(p + 16 + mid_off);
                    eq0 = vandq_u8(eq0, vceqq_u8(m0, v_mid));
                    eq1 = vandq_u8(eq1, vceqq_u8(m1, v_mid));
                }
                uint64_t mask1 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq1), 4)), 0);
                while (mask1 != 0) {
                    const uint32_t bit = static_cast<uint32_t>((63 - __builtin_clzll(mask1)) >> 2);
                    if (_verify_interior(p + 16 + bit)) {
                        return p + 16 + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _rsearch_two_way_upto(haystack, p + 16 + bit, haystack_end);
                    }
                    mask1 &= ~(0xFULL << (bit << 2));
                }
                uint64_t mask0 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq0), 4)), 0);
                while (mask0 != 0) {
                    const uint32_t bit = static_cast<uint32_t>((63 - __builtin_clzll(mask0)) >> 2);
                    if (_verify_interior(p + bit)) {
                        return p + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _rsearch_two_way_upto(haystack, p + bit, haystack_end);
                    }
                    mask0 &= ~(0xFULL << (bit << 2));
                }
            }
        }

        while (static_cast<size_t>(p - haystack) >= 16) {
            p -= 16;
            const uint8x16_t f0 = vld1q_u8(p);
            const uint8x16_t l0 = vld1q_u8(p + last_off);
            uint8x16_t eq0 = vandq_u8(vceqq_u8(f0, v_first), vceqq_u8(l0, v_last));
            if (vmaxvq_u8(eq0) != 0) {
                if (needle_size > 2) {
                    const uint8x16_t m0 = vld1q_u8(p + mid_off);
                    eq0 = vandq_u8(eq0, vceqq_u8(m0, v_mid));
                }
                uint64_t mask0 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq0), 4)), 0);
                while (mask0 != 0) {
                    const uint32_t bit = static_cast<uint32_t>((63 - __builtin_clzll(mask0)) >> 2);
                    if (_verify_interior(p + bit)) {
                        return p + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _rsearch_two_way_upto(haystack, p + bit, haystack_end);
                    }
                    mask0 &= ~(0xFULL << (bit << 2));
                }
            }
        }

        if (p > haystack) {
            const size_t rem = static_cast<size_t>(p - haystack);
            const uint8x16_t f0 = vld1q_u8(haystack);
            const uint8x16_t l0 = vld1q_u8(haystack + last_off);
            uint8x16_t eq0 = vandq_u8(vceqq_u8(f0, v_first), vceqq_u8(l0, v_last));
            if (vmaxvq_u8(eq0) != 0) {
                if (needle_size > 2) {
                    const uint8x16_t m0 = vld1q_u8(haystack + mid_off);
                    eq0 = vandq_u8(eq0, vceqq_u8(m0, v_mid));
                }
                uint64_t mask64 = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq0), 4)), 0);
                mask64 &= (1ULL << (rem << 2)) - 1ULL;
                while (mask64 != 0) {
                    const uint32_t bit = static_cast<uint32_t>((63 - __builtin_clzll(mask64)) >> 2);
                    if (_verify_interior(haystack + bit)) {
                        return haystack + bit;
                    }
                    if (needle_size > kMaxAnchorScan) {
                        return _rsearch_two_way_upto(haystack, haystack + bit, haystack_end);
                    }
                    mask64 &= ~(0xFULL << (bit << 2));
                }
            }
        }

        return haystack_end;
    }
#endif

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    const CharT* rsearch(const CharT* haystack, const CharT* const haystack_end) const {
        const auto* h = reinterpret_cast<const uint8_t*>(haystack);
        const auto* h_end = reinterpret_cast<const uint8_t*>(haystack_end);
#if defined(__AVX2__)
        return reinterpret_cast<const CharT*>(rsearch_avx2(h, h_end));
#elif defined(__SSE4_1__)
        return reinterpret_cast<const CharT*>(rsearch_sse41(h, h_end));
#elif defined(__ARM_NEON)
        return reinterpret_cast<const CharT*>(rsearch_neon(h, h_end));
#else
        return reinterpret_cast<const CharT*>(rsearch_scalar(h, h_end));
#endif
    }

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    const CharT* rsearch(const CharT* haystack, const size_t haystack_size) const {
        return rsearch(haystack, haystack + haystack_size);
    }
};

/** Uses functions from libc.
  * It makes sense to use only with short haystacks when cheap initialization is required.
  */

struct LibcASCIICaseSensitiveStringSearcher {
    const char* const needle;
    const size_t needle_size;

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    LibcASCIICaseSensitiveStringSearcher(const CharT* const needle_, const size_t needle_size_)
            : needle(reinterpret_cast<const char*>(needle_)), needle_size(needle_size_) {}

    template <typename CharT, typename = std::enable_if_t<sizeof(CharT) == 1>>
    const CharT* search(const CharT* haystack, size_t haystack_len) const {
        return reinterpret_cast<const CharT*>(memmem(haystack, haystack_len, needle, needle_size));
    }
};

} // namespace starrocks
