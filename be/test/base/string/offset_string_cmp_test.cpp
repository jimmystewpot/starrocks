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

#include "base/string/offset_string_cmp.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace starrocks {

namespace {

template <typename OffsetT>
void build_offset_buffer(const std::vector<std::string>& rows, OffsetT base_offset, std::vector<OffsetT>* offsets,
                         std::vector<uint8_t>* bytes) {
    offsets->clear();
    offsets->reserve(rows.size() + 1);
    size_t total = static_cast<size_t>(base_offset);
    for (const auto& r : rows) {
        total += r.size();
    }
    bytes->assign(total, 0x7F);
    OffsetT cur = base_offset;
    offsets->push_back(cur);
    for (const auto& r : rows) {
        if (!r.empty()) {
            std::memcpy(bytes->data() + cur, r.data(), r.size());
        }
        cur += static_cast<OffsetT>(r.size());
        offsets->push_back(cur);
    }
}

template <typename OffsetT>
void run_all_modes_size_classes_and_regimes(OffsetT base_offset) {
    const std::vector<size_t> pat_lengths = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  12, 15,
                                             16, 17, 20, 24, 25, 31, 32, 33, 47, 48, 49, 64};
    const std::vector<size_t> row_counts = {0, 1, 3, 4, 7, 8, 9, 15, 16, 17, 63, 64, 65};

    for (size_t pat_len : pat_lengths) {
        std::string pat_str(pat_len, '\0');
        for (size_t i = 0; i < pat_len; ++i) {
            pat_str[i] = (i == 2 && pat_len >= 4) ? '\0' : static_cast<char>('a' + (i % 23));
        }
        Slice pat_slice(pat_str);

        for (size_t n_rows : row_counts) {
            std::vector<std::string> rows;
            rows.reserve(n_rows);
            std::vector<uint8_t> exp_eq(n_rows, 0);
            std::vector<uint8_t> exp_ne(n_rows, 0);
            std::vector<uint8_t> exp_starts(n_rows, 0);
            std::vector<uint8_t> exp_ends(n_rows, 0);

            for (size_t r = 0; r < n_rows; ++r) {
                std::string val;
                if (pat_len == 0) {
                    val = (r % 3 == 0) ? "" : "non_empty";
                } else if (r < 16 && n_rows >= 32) {
                    // Regime 1: 16 consecutive length-rejected rows (mask == 0)
                    val = pat_str.substr(0, pat_len - 1);
                } else if (r >= 16 && r < 32 && n_rows >= 48) {
                    // Regime 2: 16 consecutive length-equal rows (mask == kStringLenSimdMask)
                    val = pat_str;
                    if (r % 2 == 1) {
                        val[pat_len / 2] ^= 0x01;
                    }
                } else {
                    // Regime 3: partial mask + tail
                    switch (r % 7) {
                    case 0:
                        val = pat_str;
                        break;
                    case 1:
                        val = pat_str + "tail_extra";
                        break;
                    case 2:
                        val = std::string("head_extra") + pat_str;
                        break;
                    case 3:
                        val = pat_str.substr(0, pat_len / 2);
                        break;
                    case 4: {
                        val = pat_str;
                        val.front() ^= 0x01;
                        break;
                    }
                    case 5: {
                        val = pat_str;
                        val.back() ^= 0x01;
                        break;
                    }
                    case 6: {
                        val = pat_str;
                        val[pat_len / 2] ^= 0x01;
                        val = "x" + val + "y";
                        break;
                    }
                    }
                }
                rows.push_back(val);
                const bool is_eq = (val.size() == pat_len && std::memcmp(val.data(), pat_str.data(), pat_len) == 0);
                const bool is_starts = (val.size() >= pat_len && std::memcmp(val.data(), pat_str.data(), pat_len) == 0);
                const bool is_ends = (val.size() >= pat_len &&
                                      std::memcmp(val.data() + val.size() - pat_len, pat_str.data(), pat_len) == 0);
                exp_eq[r] = static_cast<uint8_t>(is_eq);
                exp_ne[r] = static_cast<uint8_t>(!is_eq);
                exp_starts[r] = static_cast<uint8_t>(is_starts);
                exp_ends[r] = static_cast<uint8_t>(is_ends);
            }

            std::vector<OffsetT> offsets;
            std::vector<uint8_t> bytes;
            build_offset_buffer(rows, base_offset, &offsets, &bytes);
            const size_t payload_bytes = bytes.size() - static_cast<size_t>(base_offset);

            std::vector<uint8_t> dst_eq(n_rows, 0xAA);
            std::vector<uint8_t> dst_ne(n_rows, 0xAA);
            std::vector<uint8_t> dst_starts(n_rows, 0xAA);
            std::vector<uint8_t> dst_ends(n_rows, 0xAA);

            eval_offset_string_const<OffsetStringMatchMode::EQUALS>(offsets.data(), bytes.data(), payload_bytes,
                                                                    pat_slice, n_rows, dst_eq.data());
            eval_offset_string_const<OffsetStringMatchMode::NOT_EQUALS>(offsets.data(), bytes.data(), payload_bytes,
                                                                        pat_slice, n_rows, dst_ne.data());
            eval_offset_string_const<OffsetStringMatchMode::STARTS_WITH>(offsets.data(), bytes.data(), payload_bytes,
                                                                         pat_slice, n_rows, dst_starts.data());
            eval_offset_string_const<OffsetStringMatchMode::ENDS_WITH>(offsets.data(), bytes.data(), payload_bytes,
                                                                       pat_slice, n_rows, dst_ends.data());

            for (size_t r = 0; r < n_rows; ++r) {
                ASSERT_EQ(exp_eq[r], dst_eq[r]) << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
                ASSERT_EQ(exp_ne[r], dst_ne[r]) << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
                ASSERT_EQ(exp_starts[r], dst_starts[r]) << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
                ASSERT_EQ(exp_ends[r], dst_ends[r]) << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
            }
        }
    }
}

} // namespace

TEST(OffsetStringCmpTest, Const32BitOffsetsAllModesAndRegimes) {
    run_all_modes_size_classes_and_regimes<uint32_t>(0);
    run_all_modes_size_classes_and_regimes<uint32_t>(7);
}

TEST(OffsetStringCmpTest, Const64BitOffsetsAllModesAndRegimes) {
    run_all_modes_size_classes_and_regimes<uint64_t>(0);
    run_all_modes_size_classes_and_regimes<uint64_t>(11);
}

TEST(OffsetStringCmpTest, WholeColumnEarlyExitWhenTotalBytesLessThanPattern) {
    std::vector<std::string> rows = {"a", "bb", "ccc", ""};
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> bytes;
    build_offset_buffer<uint32_t>(rows, 0, &offsets, &bytes);

    Slice long_pattern("pattern_longer_than_total_bytes");
    ASSERT_LT(bytes.size(), long_pattern.size);

    std::vector<uint8_t> dst_eq(rows.size(), 0xFF);
    std::vector<uint8_t> dst_ne(rows.size(), 0x00);
    std::vector<uint8_t> dst_starts(rows.size(), 0xFF);
    std::vector<uint8_t> dst_ends(rows.size(), 0xFF);

    eval_offset_string_const<OffsetStringMatchMode::EQUALS>(offsets.data(), bytes.data(), bytes.size(), long_pattern,
                                                            rows.size(), dst_eq.data());
    eval_offset_string_const<OffsetStringMatchMode::NOT_EQUALS>(offsets.data(), bytes.data(), bytes.size(),
                                                                long_pattern, rows.size(), dst_ne.data());
    eval_offset_string_const<OffsetStringMatchMode::STARTS_WITH>(offsets.data(), bytes.data(), bytes.size(),
                                                                 long_pattern, rows.size(), dst_starts.data());
    eval_offset_string_const<OffsetStringMatchMode::ENDS_WITH>(offsets.data(), bytes.data(), bytes.size(), long_pattern,
                                                               rows.size(), dst_ends.data());

    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(0, dst_eq[i]);
        EXPECT_EQ(1, dst_ne[i]);
        EXPECT_EQ(0, dst_starts[i]);
        EXPECT_EQ(0, dst_ends[i]);
    }
}

TEST(OffsetStringCmpTest, VectorVectorEqAndNeMixedOffsetWidths) {
    std::vector<std::string> lhs_rows = {
            "",      "a", std::string("ab\0cd", 5), std::string("ab\0cd", 5), "0123456789abcdef0123456789abcdef_long",
            "short",
    };
    std::vector<std::string> rhs_rows = {
            "",
            "b",
            std::string("ab\0cd", 5),
            std::string("ab\0cx", 5),
            "0123456789abcdef0123456789abcdef_long",
            "short_extra",
    };
    const std::vector<uint8_t> exp_eq = {1, 0, 1, 0, 1, 0};

    std::vector<uint32_t> off32_lhs;
    std::vector<uint8_t> bytes_lhs;
    build_offset_buffer<uint32_t>(lhs_rows, 3, &off32_lhs, &bytes_lhs);

    std::vector<uint64_t> off64_rhs;
    std::vector<uint8_t> bytes_rhs;
    build_offset_buffer<uint64_t>(rhs_rows, 5, &off64_rhs, &bytes_rhs);

    const size_t n = lhs_rows.size();
    std::vector<uint8_t> dst_eq(n, 0xFF);
    std::vector<uint8_t> dst_ne(n, 0xFF);

    eval_offset_string_vec<false>(off32_lhs.data(), bytes_lhs.data(), off64_rhs.data(), bytes_rhs.data(), n,
                                  dst_eq.data());
    eval_offset_string_vec<true>(off32_lhs.data(), bytes_lhs.data(), off64_rhs.data(), bytes_rhs.data(), n,
                                 dst_ne.data());

    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(exp_eq[i], dst_eq[i]) << "row=" << i;
        EXPECT_EQ(static_cast<uint8_t>(exp_eq[i] ^ 1), dst_ne[i]) << "row=" << i;
    }
}

} // namespace starrocks
