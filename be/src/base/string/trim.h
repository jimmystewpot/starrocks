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

    constexpr void add(uint8_t b) noexcept { bitmap[b >> 6] |= (1ULL << (b & 63)); }

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

// Removes the leading and trailing ASCII spaces (0x20) of `str` and returns the remaining
// substring, which still points into the buffer owned by the caller.
//
// Only the space character is removed. Callers such as the CSV `trim_space` option and URL
// parsing treat a tab, a newline or a carriage return as data, so widening this to the whole
// whitespace set would silently change what they parse. Use StripWhiteSpace() from
// gutil/strings/strip.h when the full whitespace set should be removed instead.
inline std::string_view trim_spaces(std::string_view str) {
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

} // namespace starrocks
