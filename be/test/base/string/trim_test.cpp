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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
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

    // Mutating a ByteSet via add() must invalidate exact_nibble_lut until finalize_lut() is called,
    // so SIMD and scalar paths remain consistent even if a caller mutates after construction.
    ByteSet mutated = ByteSet::from_bytes(" ");
    mutated.add(static_cast<uint8_t>('\t'));
    EXPECT_FALSE(mutated.exact_nibble_lut);
    const std::string tabs(32, '\t');
    EXPECT_EQ(tabs.data() + tabs.size(), skip_leading_byteset<true>(tabs.data(), tabs.data() + tabs.size(), mutated));
    EXPECT_EQ(tabs.data(), skip_trailing_byteset<true>(tabs.data(), tabs.data() + tabs.size(), mutated));
    mutated.finalize_lut();
    EXPECT_TRUE(mutated.exact_nibble_lut);
    EXPECT_EQ(tabs.data() + tabs.size(), skip_leading_byteset<true>(tabs.data(), tabs.data() + tabs.size(), mutated));
    EXPECT_EQ(tabs.data(), skip_trailing_byteset<true>(tabs.data(), tabs.data() + tabs.size(), mutated));
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

TEST(SkipCharSimdTest, NullAndEmptySlices) {
    EXPECT_EQ(nullptr, skip_leading_char<true>(nullptr, nullptr, ' '));
    EXPECT_EQ(nullptr, skip_trailing_char<true>(nullptr, nullptr, ' '));
    EXPECT_EQ(nullptr, skip_leading_char<false>(nullptr, nullptr, ' '));
    EXPECT_EQ(nullptr, skip_trailing_char<false>(nullptr, nullptr, ' '));

    const char buf[4] = {' ', ' ', ' ', ' '};
    for (int i = 0; i <= 4; ++i) {
        const char* p = buf + i;
        EXPECT_EQ(p, skip_leading_char<true>(p, p, ' '));
        EXPECT_EQ(p, skip_trailing_char<true>(p, p, ' '));
        EXPECT_EQ(p, skip_leading_char<false>(p, p, ' '));
        EXPECT_EQ(p, skip_trailing_char<false>(p, p, ' '));
    }

    // Inverted range [begin, end) where begin > end returns begin for leading and end for trailing.
    EXPECT_EQ(buf + 2, skip_leading_char<true>(buf + 2, buf + 1, ' '));
    EXPECT_EQ(buf + 1, skip_trailing_char<true>(buf + 2, buf + 1, ' '));
}

TEST(SkipCharSimdTest, BoundaryLengthsAndAlignments) {
    alignas(64) char buf[256];
    const size_t lengths[] = {0, 1, 2, 7, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65, 96, 128};

    for (size_t align = 0; align < 32; ++align) {
        for (size_t len : lengths) {
            char* b = buf + align;
            char* e = b + len;
            for (size_t k = 0; k <= len; ++k) {
                // Leading run of length k
                std::memset(buf, '#', sizeof(buf));
                std::memset(b, ' ', k);
                if (k < len) {
                    b[k] = (k & 1) ? '\0' : 'A';
                }
                const char* lead_simd = skip_leading_char<true>(b, e, ' ');
                const char* lead_scalar = skip_leading_char<false>(b, e, ' ');
                ASSERT_EQ(b + k, lead_scalar);
                ASSERT_EQ(lead_scalar, lead_simd) << "align=" << align << " len=" << len << " k=" << k;

                // Trailing run of length k
                std::memset(buf, '#', sizeof(buf));
                std::memset(e - k, ' ', k);
                if (k < len) {
                    *(e - k - 1) = (k & 1) ? '\0' : 'Z';
                }
                const char* trail_simd = skip_trailing_char<true>(b, e, ' ');
                const char* trail_scalar = skip_trailing_char<false>(b, e, ' ');
                ASSERT_EQ(e - k, trail_scalar);
                ASSERT_EQ(trail_scalar, trail_simd) << "align=" << align << " len=" << len << " k=" << k;
            }
        }
    }

    const size_t pad_lengths[] = {0, 1, 15, 16, 17, 31, 32, 33, 64};
    const std::string_view payload("a\tb\0c", 5);
    for (size_t lead : pad_lengths) {
        for (size_t trail : pad_lengths) {
            std::string s(lead, ' ');
            s.append(payload.data(), payload.size());
            s.append(trail, ' ');
            std::string_view res = trim_spaces(s);
            ASSERT_EQ(payload, res) << "lead=" << lead << " trail=" << trail;
            ASSERT_EQ(s.data() + lead, res.data());
        }
    }

    for (size_t len = 0; len <= 96; ++len) {
        auto all_spaces = std::make_unique<char[]>(len);
        if (len > 0) {
            std::memset(all_spaces.get(), ' ', len);
        }
        std::string_view res = trim_spaces(std::string_view(all_spaces.get(), len));
        ASSERT_TRUE(res.empty()) << "len=" << len;
        ASSERT_NE(nullptr, res.data()) << "len=" << len;
    }
}

TEST(SkipByteSetSimdTest, BoundaryLengthsAlignmentsAndUtf8Interior) {
    const ByteSet ws_set = ByteSet::from_bytes(" \t\r\n\v\f");
    const ByteSet nul_xyz_set = ByteSet::from_bytes(std::string_view("\0xyz", 4));
    EXPECT_EQ(nullptr, skip_leading_byteset<true>(nullptr, nullptr, ws_set));
    EXPECT_EQ(nullptr, skip_trailing_byteset<true>(nullptr, nullptr, ws_set));

    alignas(64) char buf[256];
    const size_t lengths[] = {0, 1, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65, 96};

    struct SetCase {
        ByteSet set;
        std::string_view trim_chars;
        char stop_char;
    };
    const SetCase cases[] = {
            {ws_set, " \t\r\n\v\f", 'A'},
            {nul_xyz_set, std::string_view("\0xyz", 4), 'B'},
    };

    for (const auto& tc : cases) {
        for (size_t align = 0; align < 32; ++align) {
            for (size_t len : lengths) {
                char* b = buf + align;
                char* e = b + len;
                for (size_t k = 0; k <= len; ++k) {
                    // Leading run of length k
                    std::memset(buf, tc.stop_char, sizeof(buf));
                    for (size_t i = 0; i < k; ++i) {
                        b[i] = tc.trim_chars[i % tc.trim_chars.size()];
                    }
                    const char* lead_simd = skip_leading_byteset<true>(b, e, tc.set);
                    const char* lead_scalar = skip_leading_byteset<false>(b, e, tc.set);
                    ASSERT_EQ(b + k, lead_scalar);
                    ASSERT_EQ(lead_scalar, lead_simd) << "align=" << align << " len=" << len << " k=" << k;

                    // Trailing run of length k
                    std::memset(buf, tc.stop_char, sizeof(buf));
                    for (size_t i = 0; i < k; ++i) {
                        e[-(static_cast<ptrdiff_t>(i) + 1)] = tc.trim_chars[i % tc.trim_chars.size()];
                    }
                    const char* trail_simd = skip_trailing_byteset<true>(b, e, tc.set);
                    const char* trail_scalar = skip_trailing_byteset<false>(b, e, tc.set);
                    ASSERT_EQ(e - k, trail_scalar);
                    ASSERT_EQ(trail_scalar, trail_simd) << "align=" << align << " len=" << len << " k=" << k;
                }
            }
        }
    }

    // UTF-8 multi-byte payload preservation with ASCII ByteSet
    const ByteSet ascii_ws = ByteSet::from_bytes(" \t\r\n");
    const std::string_view utf8_payload = "你好世界🙂🐶";
    const std::string_view fixed_utf8 = "   \t\r\n你好世界🙂🐶\r\n\t   ";
    {
        const char* b = fixed_utf8.data();
        const char* e = b + fixed_utf8.size();
        const char* nb = skip_leading_byteset<true>(b, e, ascii_ws);
        const char* ne = skip_trailing_byteset<true>(nb, e, ascii_ws);
        EXPECT_EQ(utf8_payload, std::string_view(nb, static_cast<size_t>(ne - nb)));
    }
    const std::string_view ws_chars = " \t\r\n";
    for (size_t pad = 0; pad <= 64; ++pad) {
        std::string s;
        for (size_t i = 0; i < pad; ++i) {
            s.push_back(ws_chars[i % ws_chars.size()]);
        }
        s.append(utf8_payload);
        for (size_t i = 0; i < pad; ++i) {
            s.push_back(ws_chars[(i + 1) % ws_chars.size()]);
        }
        const char* b = s.data();
        const char* e = b + s.size();
        const char* nb = skip_leading_byteset<true>(b, e, ascii_ws);
        const char* ne = skip_trailing_byteset<true>(nb, e, ascii_ws);
        ASSERT_EQ(utf8_payload, std::string_view(nb, static_cast<size_t>(ne - nb))) << "pad=" << pad;
    }

    // 9-high-nibble-class ByteSet (exact_nibble_lut == false): verify zero false positives
    std::string nine_classes;
    for (int h = 0; h < 9; ++h) {
        nine_classes.push_back(static_cast<char>(16 * h + h));
    }
    const ByteSet set9 = ByteSet::from_bytes(nine_classes);
    ASSERT_FALSE(set9.exact_nibble_lut);
    // Byte 0x78 (h=7, l=8) shares bit 7 with h=8 (0x88) in the folded nibble LUT,
    // so set9.nibble_matches(0x78) is true while set9.contains(0x78) is false.
    const auto fp_stop_byte = static_cast<char>(0x78);
    ASSERT_FALSE(set9.contains(static_cast<uint8_t>(fp_stop_byte)));
    ASSERT_TRUE(set9.nibble_matches(static_cast<uint8_t>(fp_stop_byte)));

    for (size_t len = 0; len <= 96; ++len) {
        for (size_t k = 0; k <= len; ++k) {
            std::string s(len, fp_stop_byte);
            for (size_t i = 0; i < k; ++i) {
                s[i] = nine_classes[i % nine_classes.size()];
            }
            const char* b = s.data();
            const char* e = b + s.size();
            ASSERT_EQ(skip_leading_byteset<false>(b, e, set9), skip_leading_byteset<true>(b, e, set9))
                    << "len=" << len << " k=" << k;
            ASSERT_EQ(b + k, skip_leading_byteset<true>(b, e, set9));

            std::string st(len, fp_stop_byte);
            for (size_t i = 0; i < k; ++i) {
                st[len - 1 - i] = nine_classes[i % nine_classes.size()];
            }
            const char* bt = st.data();
            const char* et = bt + st.size();
            ASSERT_EQ(skip_trailing_byteset<false>(bt, et, set9), skip_trailing_byteset<true>(bt, et, set9))
                    << "len=" << len << " k=" << k;
            ASSERT_EQ(et - k, skip_trailing_byteset<true>(bt, et, set9));
        }
    }

    // Tight heap allocations (exact size 0..96) so ASAN detects any 1-byte OOB vector read
    for (size_t len = 0; len <= 96; ++len) {
        auto tight_all = std::make_unique<char[]>(len);
        if (len > 0) {
            std::memset(tight_all.get(), ' ', len);
        }
        const char* b = tight_all.get();
        const char* e = b + len;
        ASSERT_EQ(e, skip_leading_char<true>(b, e, ' '));
        ASSERT_EQ(b, skip_trailing_char<true>(b, e, ' '));
        ASSERT_EQ(e, skip_leading_byteset<true>(b, e, ws_set));
        ASSERT_EQ(b, skip_trailing_byteset<true>(b, e, ws_set));

        if (len > 0) {
            auto tight_mid = std::make_unique<char[]>(len);
            std::memset(tight_mid.get(), ' ', len);
            tight_mid[len / 2] = 'x';
            const char* bm = tight_mid.get();
            const char* em = bm + len;
            ASSERT_EQ(bm + (len / 2), skip_leading_char<true>(bm, em, ' '));
            ASSERT_EQ(bm + (len / 2) + 1, skip_trailing_char<true>(bm, em, ' '));
            ASSERT_EQ(bm + (len / 2), skip_leading_byteset<true>(bm, em, ws_set));
            ASSERT_EQ(bm + (len / 2) + 1, skip_trailing_byteset<true>(bm, em, ws_set));
        }
    }
}

TEST(SkipCharAndByteSetFastPathTest, CleanOneSpaceAndInternalSpacesBoundaries) {
    const ByteSet ws_set = ByteSet::from_bytes(" \t\r\n\v\f");

    // Single-char and two-char boundary cases
    const std::string_view single_clean = "x";
    EXPECT_EQ(single_clean.data(),
              skip_leading_char<true>(single_clean.data(), single_clean.data() + single_clean.size(), ' '));
    EXPECT_EQ(single_clean.data() + 1,
              skip_trailing_char<true>(single_clean.data(), single_clean.data() + single_clean.size(), ' '));
    EXPECT_EQ(single_clean.data(),
              skip_leading_byteset<true>(single_clean.data(), single_clean.data() + single_clean.size(), ws_set));
    EXPECT_EQ(single_clean.data() + 1,
              skip_trailing_byteset<true>(single_clean.data(), single_clean.data() + single_clean.size(), ws_set));

    const std::string_view single_space = " ";
    EXPECT_EQ(single_space.data() + 1,
              skip_leading_char<true>(single_space.data(), single_space.data() + single_space.size(), ' '));
    EXPECT_EQ(single_space.data(),
              skip_trailing_char<true>(single_space.data(), single_space.data() + single_space.size(), ' '));
    EXPECT_EQ(single_space.data() + 1,
              skip_leading_byteset<true>(single_space.data(), single_space.data() + single_space.size(), ws_set));
    EXPECT_EQ(single_space.data(),
              skip_trailing_byteset<true>(single_space.data(), single_space.data() + single_space.size(), ws_set));

    // Strings with only internal spaces across short (<16B), medium (16-64B), and long (>128B)
    const std::string internal_cases[] = {
            "a b",
            "hello   world",
            "ClickBench   Title   With   Many   Internal   Spaces   Only",
            std::string(64, 'a') + "     " + std::string(96, 'b'),
    };
    for (const std::string& s : internal_cases) {
        const char* b = s.data();
        const char* e = b + s.size();
        EXPECT_EQ(b, skip_leading_char<true>(b, e, ' '));
        EXPECT_EQ(e, skip_trailing_char<true>(b, e, ' '));
        EXPECT_EQ(b, skip_leading_byteset<true>(b, e, ws_set));
        EXPECT_EQ(e, skip_trailing_byteset<true>(b, e, ws_set));
        EXPECT_EQ(std::string_view(s), trim_spaces(s));

        // 1 leading space only
        const std::string lead1 = " " + s;
        EXPECT_EQ(lead1.data() + 1, skip_leading_char<true>(lead1.data(), lead1.data() + lead1.size(), ' '));
        EXPECT_EQ(lead1.data() + lead1.size(),
                  skip_trailing_char<true>(lead1.data(), lead1.data() + lead1.size(), ' '));
        EXPECT_EQ(lead1.data() + 1, skip_leading_byteset<true>(lead1.data(), lead1.data() + lead1.size(), ws_set));
        EXPECT_EQ(lead1.data() + lead1.size(),
                  skip_trailing_byteset<true>(lead1.data(), lead1.data() + lead1.size(), ws_set));
        EXPECT_EQ(std::string_view(s), trim_spaces(lead1));

        // 1 trailing space only
        const std::string trail1 = s + " ";
        EXPECT_EQ(trail1.data(), skip_leading_char<true>(trail1.data(), trail1.data() + trail1.size(), ' '));
        EXPECT_EQ(trail1.data() + trail1.size() - 1,
                  skip_trailing_char<true>(trail1.data(), trail1.data() + trail1.size(), ' '));
        EXPECT_EQ(trail1.data(), skip_leading_byteset<true>(trail1.data(), trail1.data() + trail1.size(), ws_set));
        EXPECT_EQ(trail1.data() + trail1.size() - 1,
                  skip_trailing_byteset<true>(trail1.data(), trail1.data() + trail1.size(), ws_set));
        EXPECT_EQ(std::string_view(s), trim_spaces(trail1));

        // 2 leading + 2 trailing spaces
        const std::string both2 = "  " + s + "  ";
        EXPECT_EQ(both2.data() + 2, skip_leading_char<true>(both2.data(), both2.data() + both2.size(), ' '));
        EXPECT_EQ(both2.data() + both2.size() - 2,
                  skip_trailing_char<true>(both2.data(), both2.data() + both2.size(), ' '));
        EXPECT_EQ(both2.data() + 2, skip_leading_byteset<true>(both2.data(), both2.data() + both2.size(), ws_set));
        EXPECT_EQ(both2.data() + both2.size() - 2,
                  skip_trailing_byteset<true>(both2.data(), both2.data() + both2.size(), ws_set));
        EXPECT_EQ(std::string_view(s), trim_spaces(both2));
    }
}

} // namespace starrocks
