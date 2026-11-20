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

#include "base/simd/expand.h"

#include <array>
#include <cstring>

#include "base/simd/multi_version.h"
#include "base/simd/simd.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace SIMD::Expand {

#if defined(__x86_64__)
// Lookup table for AVX2 32-bit expand: for each 8-bit mask, stores the
// permutation indices needed to scatter `popcount(mask)` valid elements into
// their final positions.
constexpr auto make_expand_permute_lut32() {
    std::array<std::array<uint32_t, 8>, 256> table{};
    for (uint32_t mask = 0; mask < 256; ++mask) {
        uint32_t src_idx = 0;
        for (uint32_t dst_idx = 0; dst_idx < 8; ++dst_idx) {
            if (mask & (1u << dst_idx)) {
                table[mask][dst_idx] = src_idx++;
            } else {
                table[mask][dst_idx] = 0;
            }
        }
    }
    return table;
}

alignas(64) inline constexpr auto kExpandPermuteLUT = make_expand_permute_lut32();

// 4-bit mask -> 8 x int32 permutation indices that treat the 256-bit register
// as four 64-bit lanes. The permute is done via _mm256_permutevar8x32_epi32
// because there is no native epi64 permute on AVX2.
constexpr auto make_expand_permute_lut64() {
    std::array<std::array<int32_t, 8>, 16> table{};
    for (uint32_t mask = 0; mask < 16; ++mask) {
        int32_t src_idx = 0;
        for (uint32_t dst_idx = 0; dst_idx < 4; ++dst_idx) {
            int32_t base = 0;
            if (mask & (1u << dst_idx)) {
                base = src_idx * 2;
                ++src_idx;
            }
            table[mask][dst_idx * 2] = base;
            table[mask][dst_idx * 2 + 1] = base + 1;
        }
    }
    return table;
}

alignas(64) inline constexpr auto kPerm64LUT = make_expand_permute_lut64();

// popcount(mask) for each 8-bit mask, used to advance the source cursor.
inline constexpr std::array<uint8_t, 256> kExpandCountLUT = {
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2,
        3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3,
        3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5,
        6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4,
        3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4,
        5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6,
        6, 7, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

// popcount for each 4-bit mask, used by the 64-bit AVX2 path.
inline constexpr std::array<uint8_t, 16> kExpandCountLUT64 = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
#endif // __x86_64__

#if defined(__ARM_NEON) && defined(__aarch64__)
constexpr auto make_neon_expand_permute_lut32() {
    std::array<std::array<uint8_t, 16>, 16> table{};
    for (uint32_t mask = 0; mask < 16; ++mask) {
        uint8_t src_idx = 0;
        for (uint32_t dst_idx = 0; dst_idx < 4; ++dst_idx) {
            uint8_t base = 0;
            if (mask & (1u << dst_idx)) {
                base = static_cast<uint8_t>(src_idx * 4);
                ++src_idx;
            }
            for (uint8_t b = 0; b < 4; ++b) {
                table[mask][dst_idx * 4 + b] = static_cast<uint8_t>(base + b);
            }
        }
    }
    return table;
}

alignas(16) constexpr std::array<std::array<uint8_t, 16>, 16> kNeonExpandPermuteLUT32 =
        make_neon_expand_permute_lut32();

constexpr auto make_neon_expand_permute_lut64() {
    std::array<std::array<uint8_t, 16>, 4> table{};
    for (uint32_t mask = 0; mask < 4; ++mask) {
        uint8_t src_idx = 0;
        for (uint32_t dst_idx = 0; dst_idx < 2; ++dst_idx) {
            uint8_t base = 0;
            if (mask & (1u << dst_idx)) {
                base = static_cast<uint8_t>(src_idx * 8);
                ++src_idx;
            }
            for (uint8_t b = 0; b < 8; ++b) {
                table[mask][dst_idx * 8 + b] = static_cast<uint8_t>(base + b);
            }
        }
    }
    return table;
}

alignas(16) constexpr std::array<std::array<uint8_t, 16>, 4> kNeonExpandPermuteLUT64 = make_neon_expand_permute_lut64();

inline constexpr std::array<uint8_t, 16> kNeonCountLUT32 = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
inline constexpr std::array<uint8_t, 4> kNeonCountLUT64 = {0, 1, 1, 2};
inline constexpr uint64_t kNeonAllValidNibbles = 0xffff'ffff'ffff'ffffull;

// Extract 4-bit mask for sub-group k in {0..3} (4 lanes of 4B each) from 64-bit nibble_mask.
constexpr uint32_t extract_neon_mask4(uint64_t nibble_mask, int k) noexcept {
    uint32_t c = static_cast<uint32_t>(nibble_mask >> (k * 16 + 3)) & 0x1111u;
    return ((c * 0x1248u) >> 12) & 0xFu;
}

// Extract 2-bit mask for sub-group k in {0..7} (2 lanes of 8B each) from 64-bit nibble_mask.
constexpr uint32_t extract_neon_mask2(uint64_t nibble_mask, int k) noexcept {
    uint32_t pair_bits = static_cast<uint32_t>(nibble_mask >> (k * 8 + 3)) & 0x11u;
    return (pair_bits | (pair_bits >> 3)) & 0x3u;
}
#endif // __ARM_NEON && __aarch64__

MFV_DEFAULT(void expand_load_selection_i32(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls,
                                           size_t count, size_t total_valid) {
    expand_load_branchless(dst_data, src_data, nulls, count, total_valid);
});

MFV_AVX2(void expand_load_selection_i32(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls, size_t count,
                                        size_t total_valid) {
    size_t cnt = 0;
    size_t i = 0;
    const __m128i zero16 = _mm_setzero_si128();
    // Process 8 elements per iteration using AVX2 permute + LUT.
    // Only enter SIMD path while we have >= 8 valid elements remaining so the
    // wide load never reads past the source buffer.
    for (; i + 8 <= count && cnt + 8 <= total_valid; i += 8) {
        __m128i null_vec = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&nulls[i]));
        __m128i cmp = _mm_cmpeq_epi8(null_vec, zero16);
        uint32_t mask8 = static_cast<uint32_t>(_mm_movemask_epi8(cmp)) & 0xFFu;
        if (mask8 == 0) {
            continue;
        }
        if (mask8 == 0xFFu) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_data + i),
                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_data + cnt)));
            cnt += 8;
            continue;
        }

        __m256i src_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_data + cnt));
        __m256i perm = _mm256_load_si256(reinterpret_cast<const __m256i*>(kExpandPermuteLUT[mask8].data()));
        __m256i dst_vec = _mm256_permutevar8x32_epi32(src_vec, perm);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_data + i), dst_vec);

        cnt += kExpandCountLUT[mask8];
    }
    // Scalar tail
    for (; i < count && cnt < total_valid; ++i) {
        dst_data[i] = starrocks::unaligned_load<int32_t>(src_data + cnt);
        cnt += !nulls[i];
    }
});

MFV_AVX512VLBW(void expand_load_selection_i32(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls,
                                              size_t count, size_t total_valid) {
    size_t cnt = 0;
    size_t i = 0;
    for (; i + 16 <= count && cnt < total_valid; i += 16) {
        __m128i mask = _mm_loadu_epi8(&nulls[i]);
        __mmask16 null_mask = _mm_cmp_epi8_mask(mask, _mm_setzero_si128(), _MM_CMPINT_EQ);
        if (null_mask == 0) {
            continue;
        }
        __m512i loaded = _mm512_maskz_expandloadu_epi32(null_mask, &src_data[cnt]);
        cnt += _mm_popcnt_u32(null_mask);
        _mm512_storeu_epi32(&dst_data[i], loaded);
    }
    for (; i < count && cnt < total_valid; ++i) {
        dst_data[i] = starrocks::unaligned_load<int32_t>(src_data + cnt);
        cnt += !nulls[i];
    }
});

MFV_DEFAULT(void expand_load_selection_i64(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls,
                                           size_t count, size_t total_valid) {
    expand_load_branchless(dst_data, src_data, nulls, count, total_valid);
});

MFV_AVX2(void expand_load_selection_i64(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls, size_t count,
                                        size_t total_valid) {
    size_t cnt = 0;
    size_t i = 0;
    const __m128i zero16 = _mm_setzero_si128();
    // Process 4 x int64 (256 bits) per iteration with the same safe-load guard
    // as the 32-bit path.
    for (; i + 4 <= count && cnt + 4 <= total_valid; i += 4) {
        // Bit-cast 4 null bytes into an int via memcpy to avoid a strict-aliasing
        // violation from reinterpret_cast<const uint32_t*>(uint8_t*).
        uint32_t nulls_packed;
        std::memcpy(&nulls_packed, &nulls[i], sizeof(nulls_packed));
        __m128i null_vec = _mm_cvtsi32_si128(static_cast<int>(nulls_packed));
        __m128i cmp = _mm_cmpeq_epi8(null_vec, zero16);
        uint32_t mask4 = static_cast<uint32_t>(_mm_movemask_epi8(cmp)) & 0x0Fu;
        if (mask4 == 0) {
            continue;
        }
        if (mask4 == 0x0Fu) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_data + i),
                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_data + cnt)));
            cnt += 4;
            continue;
        }

        __m256i src_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_data + cnt));
        __m256i perm = _mm256_load_si256(reinterpret_cast<const __m256i*>(kPerm64LUT[mask4].data()));
        __m256i dst_vec = _mm256_permutevar8x32_epi32(src_vec, perm);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_data + i), dst_vec);

        cnt += kExpandCountLUT64[mask4];
    }
    for (; i < count && cnt < total_valid; ++i) {
        dst_data[i] = starrocks::unaligned_load<int64_t>(src_data + cnt);
        cnt += !nulls[i];
    }
});

MFV_AVX512VLBW(void expand_load_selection_i64(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls,
                                              size_t count, size_t total_valid) {
    size_t cnt = 0;
    size_t i = 0;
    for (; i + 16 <= count && cnt < total_valid; i += 16) {
        __m128i mask = _mm_loadu_epi8(&nulls[i]);
        __mmask16 null_mask = _mm_cmp_epi8_mask(mask, _mm_setzero_si128(), _MM_CMPINT_EQ);
        if (null_mask == 0) {
            continue;
        }

        __m512i loaded;

        // process low mask
        __mmask8 mask_lo = null_mask;
        if (mask_lo != 0) {
            loaded = _mm512_maskz_expandloadu_epi64(mask_lo, &src_data[cnt]);
            _mm512_storeu_epi64(&dst_data[i], loaded);
            cnt += _mm_popcnt_u32(mask_lo);
        }

        __mmask8 mask_hi = null_mask >> 8;
        if (mask_hi != 0) {
            loaded = _mm512_maskz_expandloadu_epi64(mask_hi, &src_data[cnt]);
            _mm512_storeu_epi64(&dst_data[i + 8], loaded);
            cnt += _mm_popcnt_u32(mask_hi);
        }
    }
    for (; i < count && cnt < total_valid; ++i) {
        dst_data[i] = starrocks::unaligned_load<int64_t>(src_data + cnt);
        cnt += !nulls[i];
    }
})

void expand_load_simd(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls, size_t count,
                      size_t total_valid) {
    if (total_valid == 0) {
        return;
    }
    if (total_valid >= count) {
        std::memcpy(dst_data, src_data, count * sizeof(int32_t));
        return;
    }
#if defined(__ARM_NEON) && defined(__aarch64__)
    size_t cnt = 0;
    size_t i = 0;
    const uint8x16_t zero16 = vdupq_n_u8(0);
    for (; i + 16 <= count && cnt < total_valid; i += 16) {
        uint64_t valid_nibble = SIMD::get_nibble_mask(vceqq_u8(vld1q_u8(nulls + i), zero16));
        if (valid_nibble == 0) {
            continue;
        }
        if (valid_nibble == kNeonAllValidNibbles) {
            std::memcpy(dst_data + i, src_data + cnt, 16 * sizeof(int32_t));
            cnt += 16;
            continue;
        }
        for (int k = 0; k < 4; ++k) {
            uint32_t mask4 = extract_neon_mask4(valid_nibble, k);
            if (mask4 == 0) {
                continue;
            }
            size_t sub_i = i + static_cast<size_t>(k) * 4;
            if (mask4 == 0xFu) {
                std::memcpy(dst_data + sub_i, src_data + cnt, 4 * sizeof(int32_t));
                cnt += 4;
            } else if (cnt + 4 <= total_valid) {
                uint8x16_t src_bytes = vld1q_u8(reinterpret_cast<const uint8_t*>(src_data + cnt));
                uint8x16_t idx = vld1q_u8(kNeonExpandPermuteLUT32[mask4].data());
                uint8x16_t dst_bytes = vqtbl1q_u8(src_bytes, idx);
                vst1q_u8(reinterpret_cast<uint8_t*>(dst_data + sub_i), dst_bytes);
                cnt += kNeonCountLUT32[mask4];
            } else {
                for (size_t j = 0; j < 4 && cnt < total_valid; ++j) {
                    dst_data[sub_i + j] = starrocks::unaligned_load<int32_t>(src_data + cnt);
                    cnt += (mask4 >> j) & 1u;
                }
            }
        }
    }
    for (; i < count && cnt < total_valid; ++i) {
        dst_data[i] = starrocks::unaligned_load<int32_t>(src_data + cnt);
        cnt += !nulls[i];
    }
#else
    expand_load_selection_i32(dst_data, src_data, nulls, count, total_valid);
#endif
}

void expand_load_simd(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls, size_t count) {
    expand_load_simd(dst_data, src_data, nulls, count, SIMD::count_zero(nulls, count));
}

void expand_load_simd(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls, size_t count,
                      size_t total_valid) {
    if (total_valid == 0) {
        return;
    }
    if (total_valid >= count) {
        std::memcpy(dst_data, src_data, count * sizeof(int64_t));
        return;
    }
#if defined(__ARM_NEON) && defined(__aarch64__)
    size_t cnt = 0;
    size_t i = 0;
    const uint8x16_t zero16 = vdupq_n_u8(0);
    for (; i + 16 <= count && cnt < total_valid; i += 16) {
        uint64_t valid_nibble = SIMD::get_nibble_mask(vceqq_u8(vld1q_u8(nulls + i), zero16));
        if (valid_nibble == 0) {
            continue;
        }
        if (valid_nibble == kNeonAllValidNibbles) {
            std::memcpy(dst_data + i, src_data + cnt, 16 * sizeof(int64_t));
            cnt += 16;
            continue;
        }
        for (int k = 0; k < 8; ++k) {
            uint32_t mask2 = extract_neon_mask2(valid_nibble, k);
            if (mask2 == 0) {
                continue;
            }
            size_t sub_i = i + static_cast<size_t>(k) * 2;
            if (mask2 == 0x3u) {
                std::memcpy(dst_data + sub_i, src_data + cnt, 2 * sizeof(int64_t));
                cnt += 2;
            } else if (cnt + 2 <= total_valid) {
                uint8x16_t src_bytes = vld1q_u8(reinterpret_cast<const uint8_t*>(src_data + cnt));
                uint8x16_t idx = vld1q_u8(kNeonExpandPermuteLUT64[mask2].data());
                uint8x16_t dst_bytes = vqtbl1q_u8(src_bytes, idx);
                vst1q_u8(reinterpret_cast<uint8_t*>(dst_data + sub_i), dst_bytes);
                cnt += kNeonCountLUT64[mask2];
            } else {
                for (size_t j = 0; j < 2 && cnt < total_valid; ++j) {
                    dst_data[sub_i + j] = starrocks::unaligned_load<int64_t>(src_data + cnt);
                    cnt += (mask2 >> j) & 1u;
                }
            }
        }
    }
    for (; i < count && cnt < total_valid; ++i) {
        dst_data[i] = starrocks::unaligned_load<int64_t>(src_data + cnt);
        cnt += !nulls[i];
    }
#else
    expand_load_selection_i64(dst_data, src_data, nulls, count, total_valid);
#endif
}

void expand_load_simd(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls, size_t count) {
    expand_load_simd(dst_data, src_data, nulls, count, SIMD::count_zero(nulls, count));
}

} // namespace SIMD::Expand
