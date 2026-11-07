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

#include "base/string/trim.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace starrocks {

TEST(TrimSpacesTest, trim_spaces) {
    EXPECT_EQ("abc", trim_spaces("abc"));
    EXPECT_EQ("abc", trim_spaces("   abc"));
    EXPECT_EQ("abc", trim_spaces("abc   "));
    EXPECT_EQ("abc", trim_spaces("   abc   "));

    // Only the outermost spaces are removed.
    EXPECT_EQ("a b  c", trim_spaces("  a b  c  "));

    EXPECT_EQ("", trim_spaces(""));
    EXPECT_EQ("", trim_spaces(" "));
    EXPECT_EQ("", trim_spaces("      "));
}

TEST(TrimSpacesTest, keeps_the_other_whitespace) {
    // A tab, a newline or a carriage return is data for the CSV `trim_space` option and for URL
    // parsing, so it must survive.
    EXPECT_EQ("\tabc\t", trim_spaces("\tabc\t"));
    EXPECT_EQ("abc\n", trim_spaces(" abc\n "));
    EXPECT_EQ("abc\r", trim_spaces("abc\r"));
    EXPECT_EQ("\vabc\f", trim_spaces("  \vabc\f  "));
}

TEST(TrimSpacesTest, points_into_the_input_buffer) {
    const std::string input = "  abc  ";
    std::string_view trimmed = trim_spaces(input);
    EXPECT_EQ(input.data() + 2, trimmed.data());
    EXPECT_EQ(3, trimmed.size());

    // An all-space input still yields a view into the input rather than a null one.
    const std::string spaces = "   ";
    trimmed = trim_spaces(spaces);
    EXPECT_TRUE(trimmed.empty());
    EXPECT_NE(nullptr, trimmed.data());
}

TEST(TrimSpacesTest, handles_embedded_nul) {
    // The input is a view, not a C string, so a NUL is just another byte.
    const std::string input(" a\0b ", 5);
    EXPECT_EQ(std::string("a\0b", 3), std::string(trim_spaces(input)));
}

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
        const uint8_t h = static_cast<uint8_t>(k * 2); // 0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE
        const uint8_t l = static_cast<uint8_t>(k);
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

} // namespace starrocks
