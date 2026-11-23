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

#include "base/string/byte_set.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace starrocks {

TEST(ByteSetTest, EmptyAndSingleByte) {
    const ByteSet empty = ByteSet::from_bytes("");
    EXPECT_TRUE(empty.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        EXPECT_FALSE(empty.contains(byte)) << "b=" << b;
        EXPECT_FALSE(empty.nibble_matches(byte)) << "b=" << b;
    }

    const ByteSet space = ByteSet::from_bytes(" ");
    EXPECT_TRUE(space.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        const bool expected = (byte == 0x20);
        EXPECT_EQ(expected, space.contains(byte)) << "b=" << b;
        EXPECT_EQ(expected, space.nibble_matches(byte)) << "b=" << b;
    }

    const ByteSet nul = ByteSet::from_bytes(std::string_view("\0", 1));
    EXPECT_TRUE(nul.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        const bool expected = (byte == 0x00);
        EXPECT_EQ(expected, nul.contains(byte)) << "b=" << b;
        EXPECT_EQ(expected, nul.nibble_matches(byte)) << "b=" << b;
    }

    ByteSet cleared = space;
    cleared.clear();
    EXPECT_TRUE(cleared.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        EXPECT_FALSE(cleared.contains(byte)) << "b=" << b;
        EXPECT_FALSE(cleared.nibble_matches(byte)) << "b=" << b;
    }

    // Mutating a ByteSet via add() must invalidate exact_nibble_lut until finalize_lut() is called.
    ByteSet mutated = ByteSet::from_bytes(" ");
    mutated.add(static_cast<uint8_t>('\t'));
    EXPECT_FALSE(mutated.exact_nibble_lut);
    EXPECT_TRUE(mutated.contains(static_cast<uint8_t>(' ')));
    EXPECT_TRUE(mutated.contains(static_cast<uint8_t>('\t')));
    EXPECT_FALSE(mutated.nibble_matches(static_cast<uint8_t>('\t')));

    mutated.finalize_lut();
    EXPECT_TRUE(mutated.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        const bool expected = (byte == static_cast<uint8_t>(' ') || byte == static_cast<uint8_t>('\t'));
        EXPECT_EQ(expected, mutated.contains(byte)) << "b=" << b;
        EXPECT_EQ(expected, mutated.nibble_matches(byte)) << "b=" << b;
    }
}

TEST(ByteSetTest, AsciiSubsetsHaveZeroFalsePositives) {
    std::string all_ascii(128, '\0');
    for (int i = 0; i < 128; ++i) {
        all_ascii[i] = static_cast<char>(i);
    }

    const std::string_view cases[] = {
            " \t\r\n\v\f",
            "0123456789abcdefABCDEF",
            all_ascii,
    };

    for (std::string_view chars : cases) {
        const ByteSet set = ByteSet::from_bytes(chars);
        EXPECT_TRUE(set.exact_nibble_lut);
        for (int h = 8; h < 16; ++h) {
            EXPECT_EQ(0u, set.high_nibble_lut[h]) << "h=" << h;
        }
        for (int b = 0; b < 256; ++b) {
            const auto byte = static_cast<uint8_t>(b);
            const bool in_chars = (chars.find(static_cast<char>(byte)) != std::string_view::npos);
            EXPECT_EQ(in_chars, set.contains(byte)) << "b=" << b;
            EXPECT_EQ(set.contains(byte), set.nibble_matches(byte)) << "b=" << b;
        }
    }
}

TEST(ByteSetTest, NonAsciiEquivalenceClassesAndOverflow) {
    // 8 distinct non-empty high-nibble rows across 0x00..0xFF with distinct low nibbles.
    std::string eight_classes;
    for (int k = 0; k < 8; ++k) {
        const auto h = static_cast<uint8_t>(k * 2); // 0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE
        const auto l = static_cast<uint8_t>(k);
        eight_classes.push_back(static_cast<char>((h << 4) | l));
    }
    const ByteSet set8 = ByteSet::from_bytes(eight_classes);
    EXPECT_TRUE(set8.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        const bool expected = (eight_classes.find(static_cast<char>(byte)) != std::string::npos);
        EXPECT_EQ(expected, set8.contains(byte)) << "b=" << b;
        EXPECT_EQ(set8.contains(byte), set8.nibble_matches(byte)) << "b=" << b;
    }

    // 10 non-empty high-nibble rows that collapse into 8 distinct equivalence classes.
    std::string shared_classes = eight_classes;
    shared_classes.push_back(static_cast<char>((0x1 << 4) | 0x0)); // same row pattern as h=0x0
    shared_classes.push_back(static_cast<char>((0xF << 4) | 0x7)); // same row pattern as h=0xE
    const ByteSet set_shared = ByteSet::from_bytes(shared_classes);
    EXPECT_TRUE(set_shared.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        const bool expected = (shared_classes.find(static_cast<char>(byte)) != std::string::npos);
        EXPECT_EQ(expected, set_shared.contains(byte)) << "b=" << b;
        EXPECT_EQ(set_shared.contains(byte), set_shared.nibble_matches(byte)) << "b=" << b;
    }

    // 9 distinct non-empty high-nibble rows (16 * h + h for h in 0..8).
    std::string nine_classes;
    for (int h = 0; h < 9; ++h) {
        nine_classes.push_back(static_cast<char>(16 * h + h));
    }
    const ByteSet set9 = ByteSet::from_bytes(nine_classes);
    EXPECT_FALSE(set9.exact_nibble_lut);
    for (int b = 0; b < 256; ++b) {
        const auto byte = static_cast<uint8_t>(b);
        const bool expected = (nine_classes.find(static_cast<char>(byte)) != std::string::npos);
        EXPECT_EQ(expected, set9.contains(byte)) << "b=" << b;
        if (set9.contains(byte)) {
            EXPECT_TRUE(set9.nibble_matches(byte)) << "b=" << b;
        }
    }
}

TEST(ByteSetTest, ConcurrentReadSafety) {
    const std::string_view chars = " \t\r\n\v\f0123456789";
    const ByteSet set = ByteSet::from_bytes(chars);
    ASSERT_TRUE(set.exact_nibble_lut);

    constexpr int kNumThreads = 4;
    constexpr int kIterations = 1000;
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&set, chars]() {
            for (int iter = 0; iter < kIterations; ++iter) {
                for (int b = 0; b < 256; ++b) {
                    const auto byte = static_cast<uint8_t>(b);
                    const bool expected = (chars.find(static_cast<char>(byte)) != std::string_view::npos);
                    EXPECT_EQ(expected, set.contains(byte));
                    EXPECT_EQ(expected, set.nibble_matches(byte));
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
}

} // namespace starrocks
