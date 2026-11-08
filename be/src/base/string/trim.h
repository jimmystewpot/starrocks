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
#include <string_view>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "base/simd/simd.h"

namespace starrocks {

struct ByteSet {
    // 256-bit membership bitset (StringZilla sz_byteset_t layout):
    // byte `b` is present iff (bitmap[b >> 6] >> (b & 63)) & 1ULL.
    uint64_t bitmap[4] = {0, 0, 0, 0};

    // 16-byte SIMD nibble lookup tables for pshufb / vqtbl1q_u8.
    // A byte `b` with low nibble `L = b & 0x0F` and high nibble `H = b >> 4`
    // matches the SIMD filter iff (low_nibble_lut[L] & high_nibble_lut[H]) != 0.
    alignas(16) uint8_t low_nibble_lut[16] = {0};
    alignas(16) uint8_t high_nibble_lut[16] = {0};

    // True when (low_nibble_lut[L] & high_nibble_lut[H]) != 0 is 100% exact
    // with zero false positives across all 256 byte values.
    // Guaranteed to be `true` for ANY subset of ASCII (0x00..0x7F) of any size,
    // and for any byte set (0x00..0xFF) with <= 8 distinct non-empty high-nibble rows.
    bool exact_nibble_lut = true;

    constexpr void clear() noexcept {
        for (uint64_t& w : bitmap) {
            w = 0;
        }
        for (uint8_t& v : low_nibble_lut) {
            v = 0;
        }
        for (uint8_t& v : high_nibble_lut) {
            v = 0;
        }
        exact_nibble_lut = true;
    }

    constexpr void add(uint8_t b) noexcept {
        bitmap[b >> 6] |= (1ULL << (b & 63));
        exact_nibble_lut = false;
    }

    constexpr bool contains(uint8_t b) const noexcept { return ((bitmap[b >> 6] >> (b & 63)) & 1ULL) != 0; }

    constexpr bool nibble_matches(uint8_t b) const noexcept {
        return (low_nibble_lut[b & 0x0F] & high_nibble_lut[b >> 4]) != 0;
    }

    void finalize_lut() noexcept {
        std::memset(low_nibble_lut, 0, sizeof(low_nibble_lut));
        std::memset(high_nibble_lut, 0, sizeof(high_nibble_lut));
        exact_nibble_lut = true;

        uint16_t class_patterns[16] = {0};
        int num_classes = 0;

        for (int h = 0; h < 16; ++h) {
            const uint64_t word = bitmap[h >> 2];
            const int shift = (h & 3) * 16;
            const auto row = static_cast<uint16_t>((word >> shift) & 0xFFFFu);
            if (row == 0) {
                high_nibble_lut[h] = 0;
                continue;
            }
            int cls = -1;
            for (int k = 0; k < num_classes; ++k) {
                if (class_patterns[k] == row) {
                    cls = k;
                    break;
                }
            }
            if (cls < 0) {
                cls = num_classes++;
                class_patterns[cls] = row;
            }
            const int bit_index = (cls < 8) ? cls : 7;
            if (cls >= 8) {
                exact_nibble_lut = false;
            }
            const auto bit_mask = static_cast<uint8_t>(1u << bit_index);
            high_nibble_lut[h] = bit_mask;
            for (int l = 0; l < 16; ++l) {
                if ((row >> l) & 1u) {
                    low_nibble_lut[l] |= bit_mask;
                }
            }
        }
    }

    static ByteSet from_bytes(std::string_view bytes) noexcept {
        ByteSet set;
        for (char c : bytes) {
            set.add(static_cast<uint8_t>(c));
        }
        set.finalize_lut();
        return set;
    }
};

template <bool Simd = true>
inline const char* skip_leading_char(const char* begin, const char* end, char c) noexcept {
    if (begin == nullptr || begin >= end) {
        return begin;
    }
    if (*begin != c) {
        return begin;
    }
    const char* p = begin;
    if constexpr (Simd) {
#if defined(__AVX2__)
        if (static_cast<size_t>(end - p) >= 32) {
            const __m256i target256 = _mm256_set1_epi8(c);
            do {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
                const uint32_t eq_mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, target256)));
                const uint32_t keep_mask = ~eq_mask;
                if (keep_mask != 0) {
                    return p + __builtin_ctz(keep_mask);
                }
                p += 32;
            } while (static_cast<size_t>(end - p) >= 32);
        }
        if (static_cast<size_t>(end - p) >= 16) {
            const __m128i target128 = _mm_set1_epi8(c);
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const uint32_t eq_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, target128)));
            const uint32_t keep_mask = (~eq_mask) & 0xFFFFu;
            if (keep_mask != 0) {
                return p + __builtin_ctz(keep_mask);
            }
            p += 16;
        }
#elif defined(__SSE2__)
        if (static_cast<size_t>(end - p) >= 16) {
            const __m128i target128 = _mm_set1_epi8(c);
            do {
                const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                const uint32_t eq_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, target128)));
                const uint32_t keep_mask = (~eq_mask) & 0xFFFFu;
                if (keep_mask != 0) {
                    return p + __builtin_ctz(keep_mask);
                }
                p += 16;
            } while (static_cast<size_t>(end - p) >= 16);
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        if (static_cast<size_t>(end - p) >= 16) {
            const uint8x16_t target_vec = vdupq_n_u8(static_cast<uint8_t>(c));
            do {
                const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
                const uint64_t keep_mask = ~SIMD::get_nibble_mask(vceqq_u8(v, target_vec));
                if (keep_mask != 0) {
                    return p + (__builtin_ctzll(keep_mask) >> 2);
                }
                p += 16;
            } while (static_cast<size_t>(end - p) >= 16);
        }
#endif
    }
    for (; p < end && *p == c; ++p) {
    }
    return p;
}

template <bool Simd = true>
inline const char* skip_trailing_char(const char* begin, const char* end, char c) noexcept {
    if (begin == nullptr || begin >= end) {
        return end;
    }
    if (*(end - 1) != c) {
        return end;
    }
    const char* p = end;
    if constexpr (Simd) {
#if defined(__AVX2__)
        if (static_cast<size_t>(p - begin) >= 32) {
            const __m256i target256 = _mm256_set1_epi8(c);
            do {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p - 32));
                const uint32_t eq_mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, target256)));
                const uint32_t keep_mask = ~eq_mask;
                if (keep_mask != 0) {
                    return p - __builtin_clz(keep_mask);
                }
                p -= 32;
            } while (static_cast<size_t>(p - begin) >= 32);
        }
        if (static_cast<size_t>(p - begin) >= 16) {
            const __m128i target128 = _mm_set1_epi8(c);
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p - 16));
            const uint32_t eq_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, target128)));
            const uint32_t keep_mask = (~eq_mask) & 0xFFFFu;
            if (keep_mask != 0) {
                return (p - 16) + (32 - __builtin_clz(keep_mask));
            }
            p -= 16;
        }
#elif defined(__SSE2__)
        if (static_cast<size_t>(p - begin) >= 16) {
            const __m128i target128 = _mm_set1_epi8(c);
            do {
                const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p - 16));
                const uint32_t eq_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, target128)));
                const uint32_t keep_mask = (~eq_mask) & 0xFFFFu;
                if (keep_mask != 0) {
                    return (p - 16) + (32 - __builtin_clz(keep_mask));
                }
                p -= 16;
            } while (static_cast<size_t>(p - begin) >= 16);
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        if (static_cast<size_t>(p - begin) >= 16) {
            const uint8x16_t target_vec = vdupq_n_u8(static_cast<uint8_t>(c));
            do {
                const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p - 16));
                const uint64_t keep_mask = ~SIMD::get_nibble_mask(vceqq_u8(v, target_vec));
                if (keep_mask != 0) {
                    return p - (__builtin_clzll(keep_mask) >> 2);
                }
                p -= 16;
            } while (static_cast<size_t>(p - begin) >= 16);
        }
#endif
    }
    for (; p > begin && *(p - 1) == c; --p) {
    }
    return p;
}

template <bool Simd = true>
inline const char* skip_leading_byteset(const char* begin, const char* end, const ByteSet& set) noexcept {
    if (begin == nullptr || begin >= end) {
        return begin;
    }
    if (!set.contains(static_cast<uint8_t>(*begin))) {
        return begin;
    }
    const char* p = begin;
    if constexpr (Simd) {
        if (set.exact_nibble_lut) {
#if defined(__AVX2__)
            if (static_cast<size_t>(end - p) >= 16) {
                const __m128i low128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.low_nibble_lut));
                const __m128i high128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.high_nibble_lut));
                if (static_cast<size_t>(end - p) >= 32) {
                    const __m256i low_lut = _mm256_broadcastsi128_si256(low128);
                    const __m256i high_lut = _mm256_broadcastsi128_si256(high128);
                    const __m256i mask_0f = _mm256_set1_epi8(0x0F);
                    const __m256i zero = _mm256_setzero_si256();
                    do {
                        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
                        const __m256i lo = _mm256_and_si256(v, mask_0f);
                        const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), mask_0f);
                        const __m256i match =
                                _mm256_and_si256(_mm256_shuffle_epi8(low_lut, lo), _mm256_shuffle_epi8(high_lut, hi));
                        const uint32_t keep_mask =
                                static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(match, zero)));
                        if (keep_mask != 0) {
                            return p + __builtin_ctz(keep_mask);
                        }
                        p += 32;
                    } while (static_cast<size_t>(end - p) >= 32);
                }
                if (static_cast<size_t>(end - p) >= 16) {
                    const __m128i mask_0f = _mm_set1_epi8(0x0F);
                    const __m128i zero128 = _mm_setzero_si128();
                    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                    const __m128i lo = _mm_and_si128(v, mask_0f);
                    const __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), mask_0f);
                    const __m128i match = _mm_and_si128(_mm_shuffle_epi8(low128, lo), _mm_shuffle_epi8(high128, hi));
                    const uint32_t keep_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, zero128)));
                    if (keep_mask != 0) {
                        return p + __builtin_ctz(keep_mask);
                    }
                    p += 16;
                }
            }
#elif defined(__SSSE3__)
            if (static_cast<size_t>(end - p) >= 16) {
                const __m128i low128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.low_nibble_lut));
                const __m128i high128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.high_nibble_lut));
                const __m128i mask_0f = _mm_set1_epi8(0x0F);
                const __m128i zero128 = _mm_setzero_si128();
                do {
                    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                    const __m128i lo = _mm_and_si128(v, mask_0f);
                    const __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), mask_0f);
                    const __m128i match = _mm_and_si128(_mm_shuffle_epi8(low128, lo), _mm_shuffle_epi8(high128, hi));
                    const uint32_t keep_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, zero128)));
                    if (keep_mask != 0) {
                        return p + __builtin_ctz(keep_mask);
                    }
                    p += 16;
                } while (static_cast<size_t>(end - p) >= 16);
            }
#elif defined(__ARM_NEON) && defined(__aarch64__)
            if (static_cast<size_t>(end - p) >= 16) {
                const uint8x16_t low_lut = vld1q_u8(set.low_nibble_lut);
                const uint8x16_t high_lut = vld1q_u8(set.high_nibble_lut);
                const uint8x16_t mask_0f = vdupq_n_u8(0x0F);
                do {
                    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
                    const uint8x16_t lo = vandq_u8(v, mask_0f);
                    const uint8x16_t hi = vshrq_n_u8(v, 4);
                    const uint8x16_t in_set = vtstq_u8(vqtbl1q_u8(low_lut, lo), vqtbl1q_u8(high_lut, hi));
                    const uint64_t keep_mask = ~SIMD::get_nibble_mask(in_set);
                    if (keep_mask != 0) {
                        return p + (__builtin_ctzll(keep_mask) >> 2);
                    }
                    p += 16;
                } while (static_cast<size_t>(end - p) >= 16);
            }
#endif
        }
    }
    for (; p < end && set.contains(static_cast<uint8_t>(*p)); ++p) {
    }
    return p;
}

template <bool Simd = true>
inline const char* skip_trailing_byteset(const char* begin, const char* end, const ByteSet& set) noexcept {
    if (begin == nullptr || begin >= end) {
        return end;
    }
    if (!set.contains(static_cast<uint8_t>(*(end - 1)))) {
        return end;
    }
    const char* p = end;
    if constexpr (Simd) {
        if (set.exact_nibble_lut) {
#if defined(__AVX2__)
            if (static_cast<size_t>(p - begin) >= 16) {
                const __m128i low128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.low_nibble_lut));
                const __m128i high128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.high_nibble_lut));
                if (static_cast<size_t>(p - begin) >= 32) {
                    const __m256i low_lut = _mm256_broadcastsi128_si256(low128);
                    const __m256i high_lut = _mm256_broadcastsi128_si256(high128);
                    const __m256i mask_0f = _mm256_set1_epi8(0x0F);
                    const __m256i zero = _mm256_setzero_si256();
                    do {
                        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p - 32));
                        const __m256i lo = _mm256_and_si256(v, mask_0f);
                        const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), mask_0f);
                        const __m256i match =
                                _mm256_and_si256(_mm256_shuffle_epi8(low_lut, lo), _mm256_shuffle_epi8(high_lut, hi));
                        const uint32_t keep_mask =
                                static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(match, zero)));
                        if (keep_mask != 0) {
                            return p - __builtin_clz(keep_mask);
                        }
                        p -= 32;
                    } while (static_cast<size_t>(p - begin) >= 32);
                }
                if (static_cast<size_t>(p - begin) >= 16) {
                    const __m128i mask_0f = _mm_set1_epi8(0x0F);
                    const __m128i zero128 = _mm_setzero_si128();
                    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p - 16));
                    const __m128i lo = _mm_and_si128(v, mask_0f);
                    const __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), mask_0f);
                    const __m128i match = _mm_and_si128(_mm_shuffle_epi8(low128, lo), _mm_shuffle_epi8(high128, hi));
                    const uint32_t keep_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, zero128)));
                    if (keep_mask != 0) {
                        return (p - 16) + (32 - __builtin_clz(keep_mask));
                    }
                    p -= 16;
                }
            }
#elif defined(__SSSE3__)
            if (static_cast<size_t>(p - begin) >= 16) {
                const __m128i low128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.low_nibble_lut));
                const __m128i high128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(set.high_nibble_lut));
                const __m128i mask_0f = _mm_set1_epi8(0x0F);
                const __m128i zero128 = _mm_setzero_si128();
                do {
                    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p - 16));
                    const __m128i lo = _mm_and_si128(v, mask_0f);
                    const __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), mask_0f);
                    const __m128i match = _mm_and_si128(_mm_shuffle_epi8(low128, lo), _mm_shuffle_epi8(high128, hi));
                    const uint32_t keep_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, zero128)));
                    if (keep_mask != 0) {
                        return (p - 16) + (32 - __builtin_clz(keep_mask));
                    }
                    p -= 16;
                } while (static_cast<size_t>(p - begin) >= 16);
            }
#elif defined(__ARM_NEON) && defined(__aarch64__)
            if (static_cast<size_t>(p - begin) >= 16) {
                const uint8x16_t low_lut = vld1q_u8(set.low_nibble_lut);
                const uint8x16_t high_lut = vld1q_u8(set.high_nibble_lut);
                const uint8x16_t mask_0f = vdupq_n_u8(0x0F);
                do {
                    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p - 16));
                    const uint8x16_t lo = vandq_u8(v, mask_0f);
                    const uint8x16_t hi = vshrq_n_u8(v, 4);
                    const uint8x16_t in_set = vtstq_u8(vqtbl1q_u8(low_lut, lo), vqtbl1q_u8(high_lut, hi));
                    const uint64_t keep_mask = ~SIMD::get_nibble_mask(in_set);
                    if (keep_mask != 0) {
                        return p - (__builtin_clzll(keep_mask) >> 2);
                    }
                    p -= 16;
                } while (static_cast<size_t>(p - begin) >= 16);
            }
#endif
        }
    }
    for (; p > begin && set.contains(static_cast<uint8_t>(*(p - 1))); --p) {
    }
    return p;
}

// Removes the leading and trailing ASCII spaces (0x20) of `str` and returns the remaining
// substring, which still points into the buffer owned by the caller.
//
// Only the space character is removed. Callers such as the CSV `trim_space` option and URL
// parsing treat a tab, a newline or a carriage return as data, so widening this to the whole
// whitespace set would silently change what they parse. Use StripWhiteSpace() from
// gutil/strings/strip.h when the full whitespace set should be removed instead.
inline std::string_view trim_spaces(std::string_view str) {
    const char* begin = str.data();
    const char* end = begin + str.size();
    if (begin == end) {
        return str;
    }
    if (*begin == ' ') {
        begin = skip_leading_char<true>(begin + 1, end, ' ');
        if (begin == end) {
            return {begin, 0};
        }
    }
    if (*(end - 1) == ' ') {
        end = skip_trailing_char<true>(begin, end - 1, ' ');
    }
    return {begin, static_cast<size_t>(end - begin)};
}

} // namespace starrocks
