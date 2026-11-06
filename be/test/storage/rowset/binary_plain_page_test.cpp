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

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/segment_v2/binary_plain_page_test.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "storage/rowset/binary_plain_page.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include "base/testutil/assert.h"
#include "column/binary_column.h"
#include "column/chunk_factory.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "gutil/casts.h"
#include "runtime/mem_pool.h"
#include "storage/chunk_helper.h"
#include "storage/olap_common.h"
#include "storage/types.h"
#include "storage_primitive/column_predicate_factory.h"
#include "storage_primitive/range.h"

namespace starrocks {

class BinaryPlainPageTest : public testing::Test {
public:
    BinaryPlainPageTest() = default;

    ~BinaryPlainPageTest() override = default;

    template <class PageBuilderType, class PageDecoderType, bool vectorize = false>
    void TestBinarySeekByValueSmallPage() {
        std::vector<Slice> slices;
        slices.emplace_back("Hello");
        slices.emplace_back(",");
        slices.emplace_back("StarRocks");

        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        PageBuilderType page_builder(options);
        size_t count = slices.size();

        Slice* ptr = &slices[0];
        count = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), count);

        OwnedSlice owned_slice = page_builder.finish()->build();

        //check first value and last value
        Slice first_value;
        page_builder.get_first_value(&first_value);
        ASSERT_EQ(slices[0], first_value);
        Slice last_value;
        page_builder.get_last_value(&last_value);
        ASSERT_EQ(slices[count - 1], last_value);

        PageDecoderType page_decoder(owned_slice.slice());
        Status status = page_decoder.init();
        ASSERT_TRUE(status.ok());

        //test1

        auto column = BinaryColumn::create();
        column->reserve(1024);
        size_t size = 1024;
        status = page_decoder.next_batch(&size, column.get());
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(3U, size);
        ASSERT_EQ("['Hello', ',', 'StarRocks']", column->debug_string());

        size = 1024;
        auto column1 = BinaryColumn::create();
        ASSERT_OK(page_decoder.seek_to_position_in_page(2));
        status = page_decoder.next_batch(&size, column1.get());
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(1, size);
        ASSERT_EQ("StarRocks", column1->immutable_data()[0]);

        auto column2 = BinaryColumn::create();
        ASSERT_OK(page_decoder.seek_to_position_in_page(0));
        SparseRange<> read_range;
        read_range.add(Range<>(0, 1));
        read_range.add(Range<>(2, 3));
        status = page_decoder.next_batch(read_range, column2.get());
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(2, column2->size());
        ASSERT_EQ("['Hello', 'StarRocks']", column2->debug_string());
    }
};

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, test_seek_by_value) {
    TestBinarySeekByValueSmallPage<BinaryPlainPageBuilder, BinaryPlainPageDecoder<TYPE_VARCHAR>, false>();
    TestBinarySeekByValueSmallPage<BinaryPlainPageBuilder, BinaryPlainPageDecoder<TYPE_VARCHAR>, true>();
}

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, test_reserve_head) {
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    BinaryPlainPageBuilder builder(options);
    builder.reserve_head(4);
    Slice slices[] = {
            "first value", "second value", "third value", "fourth value", "fifth value",
    };

    ASSERT_EQ(5, builder.add((const uint8_t*)slices, 5));
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(slices[i], builder.get_value(i));
    }

    OwnedSlice data_with_head = builder.finish()->build();

    Slice slice;
    EXPECT_EQ(5, builder.count());

    ASSERT_TRUE(builder.get_first_value(&slice).ok());
    EXPECT_EQ(slices[0], slice);

    ASSERT_TRUE(builder.get_last_value(&slice).ok());
    EXPECT_EQ(slices[4], slice);

    Slice data_without_head = data_with_head.slice();
    data_without_head.remove_prefix(4);

    BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(data_without_head);
    ASSERT_TRUE(decoder.init().ok());
    ASSERT_EQ(5, decoder.count());
    for (uint32_t i = 0; i < 5; i++) {
        EXPECT_EQ(slices[i], decoder.string_at_index(i));
    }

    auto column = BinaryColumn::create();
    SparseRange<> range(0, 5);
    ASSERT_OK(decoder.next_batch(range, column.get()));
    ASSERT_EQ(column->debug_string(), "['first value', 'second value', 'third value', 'fourth value', 'fifth value']");

    ASSERT_OK(decoder.seek_to_position_in_page(0));
    ASSERT_OK(decoder.next_batch(SparseRange<>(0, 1), column.get()));
    ASSERT_EQ(column->debug_string(),
              "['first value', 'second value', 'third value', 'fourth value', 'fifth value', 'first value']");
}

TEST_F(BinaryPlainPageTest, TestGetOffsetsForZeroCopy) {
    {
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        BinaryPlainPageBuilder builder(options);

        OwnedSlice page = builder.finish()->build();
        BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(page.slice());
        ASSERT_OK(decoder.init());

        BinaryColumn::Offsets offsets;
        decoder.get_offsets_for_zero_copy(offsets);

        ASSERT_EQ(1, offsets.size());
        EXPECT_EQ(0, offsets[0]);
        EXPECT_FALSE(offsets.is_large());
    }

    {
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        BinaryPlainPageBuilder builder(options);
        Slice slices[] = {
                "alpha",
                "",
                "bb",
                "ccc",
        };

        ASSERT_EQ(4, builder.add((const uint8_t*)slices, 4));
        OwnedSlice page = builder.finish()->build();
        BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(page.slice());
        ASSERT_OK(decoder.init());

        BinaryColumn::Offsets offsets;
        decoder.get_offsets_for_zero_copy(offsets);

        ASSERT_EQ(5, offsets.size());
        EXPECT_FALSE(offsets.is_large());
        EXPECT_EQ(0, offsets[0]);
        EXPECT_EQ(5, offsets[1]);
        EXPECT_EQ(5, offsets[2]);
        EXPECT_EQ(7, offsets[3]);
        EXPECT_EQ(10, offsets[4]);
    }
}

TEST_F(BinaryPlainPageTest, TestNextBatchWithFilter) {
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    BinaryPlainPageBuilder builder(options);
    Slice slices[] = {
            "a_100", "b_200", "c_300", "d_400", "e_500",
    };

    ASSERT_EQ(5, builder.add((const uint8_t*)slices, 5));
    OwnedSlice data_with_head = builder.finish()->build();
    BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(data_with_head.slice());
    ASSERT_TRUE(decoder.init().ok());

    // Case 1: Without NULLs
    {
        auto column = BinaryColumn::create();

        // Prepare filter: >= "c_300"
        std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "c_300"));
        std::vector<const ColumnPredicate*> predicates;
        predicates.push_back(predicate.get());

        SparseRange<> range(0, 5);
        std::vector<uint8_t> selection(5);
        std::vector<uint16_t> selected_idx(5);

        // reset selection
        for (int i = 0; i < 5; ++i) selection[i] = 1;

        Status st = decoder.next_batch_with_filter(column.get(), range, predicates, nullptr, selection.data(),
                                                   selected_idx.data());
        ASSERT_TRUE(st.ok()) << st.to_string();

        // Check selection
        ASSERT_EQ(0, selection[0]);
        ASSERT_EQ(0, selection[1]);
        ASSERT_EQ(1, selection[2]);
        ASSERT_EQ(1, selection[3]);
        ASSERT_EQ(1, selection[4]);

        ASSERT_EQ(3, column->size());
        ASSERT_EQ("['c_300', 'd_400', 'e_500']", column->debug_string());
    }

    // Reset decoder
    ASSERT_TRUE(decoder.seek_to_position_in_page(0).ok());

    // Case 2: Nullable destination column but the page has no NULL flags (null_data is nullptr).
    {
        auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);

        std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "c_300"));
        std::vector<const ColumnPredicate*> predicates;
        predicates.push_back(predicate.get());

        SparseRange<> range(0, 5);
        std::vector<uint8_t> selection(5);
        std::vector<uint16_t> selected_idx(5);

        for (int i = 0; i < 5; ++i) selection[i] = 1;

        Status st = decoder.next_batch_with_filter(column.get(), range, predicates, nullptr, selection.data(),
                                                   selected_idx.data());
        ASSERT_TRUE(st.ok()) << st.to_string();

        ASSERT_EQ(0, selection[0]);
        ASSERT_EQ(0, selection[1]);
        ASSERT_EQ(1, selection[2]);
        ASSERT_EQ(1, selection[3]);
        ASSERT_EQ(1, selection[4]);

        ASSERT_EQ(3, column->size());
        auto nullable_col = down_cast<NullableColumn*>(column.get());
        auto binary_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());

        ASSERT_EQ("['c_300', 'd_400', 'e_500']", binary_col->debug_string());

        ASSERT_EQ(3, nullable_col->null_column_raw_ptr()->size());
        ASSERT_EQ(0, nullable_col->null_column_data()[0]);
        ASSERT_EQ(0, nullable_col->null_column_data()[1]);
        ASSERT_EQ(0, nullable_col->null_column_data()[2]);
        ASSERT_FALSE(nullable_col->has_null());
    }

    // Reset decoder
    ASSERT_TRUE(decoder.seek_to_position_in_page(0).ok());

    // Case 3: With NULLs
    {
        // Use NullableColumn with ByteColumn as data
        auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);

        // Prepare filter: >= "c_300"
        std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "c_300"));
        std::vector<const ColumnPredicate*> predicates;
        predicates.push_back(predicate.get());

        SparseRange<> range(0, 5);
        std::vector<uint8_t> selection(5);
        std::vector<uint16_t> selected_idx(5);

        // reset selection
        for (int i = 0; i < 5; ++i) selection[i] = 1;

        // null_data: 0->not null, 1->null
        // Mark "c_300" (idx 2) as NULL. "e_500" (idx 4) as NULL.
        // "b_200" (idx 1) is not null but filtered out (< "c_300")
        // "a_100" (idx 0) is not null but filtered out
        // "d_400" (idx 3) is not null and kept (>= "c_300")
        uint8_t null_data[] = {0, 0, 1, 0, 1};

        Status st = decoder.next_batch_with_filter(column.get(), range, predicates, null_data, selection.data(),
                                                   selected_idx.data());
        ASSERT_TRUE(st.ok()) << st.to_string();

        // Check selection
        ASSERT_EQ(0, selection[0]);
        ASSERT_EQ(0, selection[1]);
        ASSERT_EQ(0, selection[2]);
        ASSERT_EQ(1, selection[3]);
        ASSERT_EQ(0, selection[4]);

        ASSERT_EQ(1, column->size());
        // For NullableColumn, we need to check the data values.
        auto nullable_col = down_cast<NullableColumn*>(column.get());
        auto binary_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());

        ASSERT_EQ("d_400", binary_col->immutable_data()[0]);
    }
}

// A predicate pushed down to a page may come with a sparse range (several non-adjacent sub-ranges of the same page,
// merged by ScalarColumnIterator). The null flags belong to the page and are indexed by the in-page ordinal, so the
// rows after a gap must take their own flags, not the flags of the rows that were skipped.
TEST_F(BinaryPlainPageTest, TestNextBatchWithFilterSparseRangeWithNulls) {
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    BinaryPlainPageBuilder builder(options);
    Slice slices[] = {
            "a_100", "b_200", "c_300", "d_400", "e_500",
    };
    ASSERT_EQ(5, builder.add((const uint8_t*)slices, 5));
    OwnedSlice data_with_head = builder.finish()->build();
    BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(data_with_head.slice());
    ASSERT_TRUE(decoder.init().ok());

    auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
    std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "c_300"));
    std::vector<const ColumnPredicate*> predicates{predicate.get()};

    // read ordinal 0 and ordinals 3..4, skip 1..2
    SparseRange<> range;
    range.add(Range<>(0, 1));
    range.add(Range<>(3, 5));
    ASSERT_EQ(3, range.span_size());
    std::vector<uint8_t> selection(3, 1);
    std::vector<uint16_t> selected_idx(3);

    // page-level null flags: ordinals 1, 2 and 4 are null. Walking them linearly over the 3 rows read would take
    // {0, 1, 1} and wrongly mark "d_400" (ordinal 3) as null.
    uint8_t null_data[] = {0, 1, 1, 0, 1};

    Status st = decoder.next_batch_with_filter(column.get(), range, predicates, null_data, selection.data(),
                                               selected_idx.data());
    ASSERT_TRUE(st.ok()) << st.to_string();

    // ordinal 0 "a_100" < "c_300" -> 0; ordinal 3 "d_400" >= "c_300" and not null -> 1; ordinal 4 is null -> 0
    ASSERT_EQ(0, selection[0]);
    ASSERT_EQ(1, selection[1]);
    ASSERT_EQ(0, selection[2]);

    ASSERT_EQ(1, column->size());
    auto nullable_col = down_cast<NullableColumn*>(column.get());
    ASSERT_FALSE(nullable_col->has_null());
    auto binary_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());
    ASSERT_EQ("d_400", binary_col->immutable_data()[0]);
}

TEST_F(BinaryPlainPageTest, TestReadByRowids) {
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    BinaryPlainPageBuilder builder(options);

    std::vector<std::string> strings;
    strings.reserve(10);
    std::vector<Slice> slices;
    slices.reserve(10);
    for (int i = 0; i < 10; ++i) {
        strings.emplace_back("val_" + std::to_string(i));
        slices.emplace_back(strings.back());
    }

    ASSERT_EQ(10, builder.add((const uint8_t*)slices.data(), 10));
    OwnedSlice data_with_head = builder.finish()->build();
    BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(data_with_head.slice());
    ASSERT_TRUE(decoder.init().ok());

    auto column = BinaryColumn::create();

    rowid_t rowids[] = {1, 3, 5, 8};
    size_t num_read = 4;

    Status st = decoder.read_by_rowids(0, rowids, &num_read, column.get());
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(4, num_read);
    ASSERT_EQ("['val_1', 'val_3', 'val_5', 'val_8']", column->debug_string());
}

TEST_F(BinaryPlainPageTest, TestDictFilterSelectionLargeDictSize) {
    // Ensure dictionary-page predicate evaluation remains correct when dict_size > 65535.
    // This is required by predicate-late-materialization which evaluates predicates on DICTIONARY_PAGE.
    constexpr uint32_t kNumDictValues = 70000;

    PageBuilderOptions options;
    options.data_page_size = 2 * 1024 * 1024; // big enough for 70k small strings + offsets
    BinaryPlainPageBuilder builder(options);

    char buf[16];
    for (uint32_t i = 0; i < kNumDictValues; ++i) {
        // Fixed-width numeric part to keep lexicographic order aligned with numeric order.
        snprintf(buf, sizeof(buf), "k%06u", i);
        ASSERT_TRUE(builder.add_slice(Slice(buf)));
    }

    OwnedSlice dict_page = builder.finish()->build();
    BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(dict_page.slice());
    ASSERT_TRUE(decoder.init().ok());
    ASSERT_EQ(kNumDictValues, decoder.count());

    std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "k065536"));
    std::vector<const ColumnPredicate*> predicates;
    predicates.push_back(predicate.get());

    const uint8_t* selection = nullptr;
    uint32_t dict_size = 0;
    uint32_t selected_count = 0;
    ASSERT_OK(decoder.get_dict_filter_selection(predicates, &selection, &dict_size, &selected_count));

    ASSERT_EQ(kNumDictValues, dict_size);
    ASSERT_NE(nullptr, selection);
    ASSERT_EQ(kNumDictValues - 65536, selected_count);
    ASSERT_EQ(0, selection[65535]);
    ASSERT_EQ(1, selection[65536]);
    ASSERT_EQ(1, selection[kNumDictValues - 1]);
}

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, test_delta_offset_roundtrip) {
    std::vector<Slice> slices;
    slices.emplace_back(""); // zero-length value -> delta 0
    slices.emplace_back("a");
    slices.emplace_back("persona,1");
    slices.emplace_back("persona,1234567");
    slices.emplace_back("bb");
    std::string longstr(300, 'z');
    slices.emplace_back(longstr);
    slices.emplace_back("tail");
    const size_t n = slices.size();

    // delta == true exercises the PLAIN_ENCODING_DELTA_OFFSET path; false is the legacy
    // absolute-offset path. Both must decode to identical values. The format is passed via the
    // builder/decoder ctor flag (the on-disk count field stays a plain element count).
    auto build_and_check = [&](bool delta) {
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        BinaryPlainPageBuilder builder(options, delta);
        size_t added = builder.add(reinterpret_cast<const uint8_t*>(slices.data()), n);
        ASSERT_EQ(n, added);
        OwnedSlice owned = builder.finish()->build();
        Slice page = owned.slice();

        // The trailer's count field is a plain element count regardless of delta encoding.
        uint32_t raw = decode_fixed32_le((const uint8_t*)page.data + page.size - sizeof(uint32_t));
        ASSERT_EQ(n, raw);

        BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(page, delta);
        ASSERT_OK(decoder.init());
        ASSERT_EQ(n, decoder.count());

        // full scan round-trip: every value must come back byte-identical
        auto col = BinaryColumn::create();
        size_t size = 1024;
        ASSERT_OK(decoder.next_batch(&size, col.get()));
        ASSERT_EQ(n, size);
        for (size_t i = 0; i < n; i++) {
            ASSERT_EQ(slices[i], col->immutable_data()[i]);
        }

        // point seek
        auto col2 = BinaryColumn::create();
        ASSERT_OK(decoder.seek_to_position_in_page(3));
        size = 1;
        ASSERT_OK(decoder.next_batch(&size, col2.get()));
        ASSERT_EQ(1, size);
        ASSERT_EQ(slices[3], col2->immutable_data()[0]);

        // sparse range read crossing non-adjacent values
        auto col3 = BinaryColumn::create();
        ASSERT_OK(decoder.seek_to_position_in_page(0));
        SparseRange<> r;
        r.add(Range<>(0, 2));
        r.add(Range<>(5, 7));
        ASSERT_OK(decoder.next_batch(r, col3.get()));
        ASSERT_EQ(4, col3->size());
        ASSERT_EQ(slices[0], col3->immutable_data()[0]);
        ASSERT_EQ(slices[1], col3->immutable_data()[1]);
        ASSERT_EQ(slices[5], col3->immutable_data()[2]);
        ASSERT_EQ(slices[6], col3->immutable_data()[3]);
    };

    build_and_check(false); // legacy absolute offsets
    build_and_check(true);  // delta offsets must decode identically
}

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, test_max_value_length_is_cached_and_correct) {
    auto build_decoder = [](const std::vector<Slice>& slices, OwnedSlice* keep_alive) {
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        BinaryPlainPageBuilder builder(options);
        size_t count = slices.size();
        builder.add(reinterpret_cast<const uint8_t*>(slices.data()), count);
        *keep_alive = builder.finish()->build();
        return BinaryPlainPageDecoder<TYPE_VARCHAR>(keep_alive->slice());
    };

    // Longest value is "StarRocks" (9 bytes); repeated calls must be stable and equal to the
    // freshly-recomputed value (memoization must not change the answer).
    {
        std::vector<Slice> slices{"Hello", ",", "StarRocks", "ab"};
        OwnedSlice owned;
        auto decoder = build_decoder(slices, &owned);
        ASSERT_OK(decoder.init());
        EXPECT_EQ(9U, decoder.max_value_length());
        EXPECT_EQ(9U, decoder.max_value_length());
        EXPECT_EQ(9U, decoder.max_value_length());
    }

    // All-empty dictionary: max length is the legitimate value 0; the -1 "not computed" sentinel
    // must not be confused with a cached 0.
    {
        std::vector<Slice> slices{"", "", ""};
        OwnedSlice owned;
        auto decoder = build_decoder(slices, &owned);
        ASSERT_OK(decoder.init());
        EXPECT_EQ(0U, decoder.max_value_length());
        EXPECT_EQ(0U, decoder.max_value_length());
    }
}

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, TestNextBatchWithFilterVectorisedOffsets) {
    constexpr size_t kTotalRows = 1050;
    std::vector<std::string> raw_strings(kTotalRows);
    std::vector<Slice> slices(kTotalRows);

    for (size_t i = 0; i < kTotalRows; ++i) {
        if (i % 37 == 0) {
            raw_strings[i] = ""; // empty string
        } else if (i % 13 == 0) {
            raw_strings[i] = std::string(128 + (i % 64), static_cast<char>('A' + (i % 26))); // long string
        } else if (i % 3 == 0) {
            raw_strings[i] = std::string(1 + (i % 5), static_cast<char>('a' + (i % 26))); // short string
        } else {
            raw_strings[i] = "str_" + std::to_string(i) + "_" + std::string(i % 17, 'x'); // medium string
        }
        slices[i] = Slice(raw_strings[i]);
    }

    auto run_tests = [&](bool delta) {
        PageBuilderOptions options;
        options.data_page_size = 2 * 1024 * 1024;
        BinaryPlainPageBuilder builder(options, delta);
        size_t added = builder.add(reinterpret_cast<const uint8_t*>(slices.data()), kTotalRows);
        ASSERT_EQ(kTotalRows, added);

        OwnedSlice owned = builder.finish()->build();
        BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(owned.slice(), delta);
        ASSERT_OK(decoder.init());
        ASSERT_EQ(kTotalRows, decoder.count());

        auto test_range = [&](uint32_t idx, uint32_t end) {
            ASSERT_OK(decoder.seek_to_position_in_page(idx));
            auto column = BinaryColumn::create();
            SparseRange<> range(idx, end);
            size_t count = range.span_size();
            std::vector<uint8_t> selection(count, 1);
            std::vector<uint16_t> selected_idx(count);

            std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
            std::vector<const ColumnPredicate*> predicates{predicate.get()};

            Status st = decoder.next_batch_with_filter(column.get(), range, predicates, nullptr, selection.data(),
                                                       selected_idx.data());
            ASSERT_TRUE(st.ok()) << st.to_string();
            ASSERT_EQ(count, column->size());

            const auto& offsets = column->get_offset();
            ASSERT_EQ(count + 1, offsets.size());
            EXPECT_FALSE(offsets.is_large());
            EXPECT_EQ(0, offsets[0]);

            for (size_t i = 0; i < count; ++i) {
                uint32_t expected_len = raw_strings[idx + i].size();
                EXPECT_EQ(expected_len, offsets[i + 1] - offsets[i]);
                EXPECT_GE(offsets[i + 1], offsets[i]);
                if (expected_len > 0) {
                    EXPECT_GT(offsets[i + 1], offsets[i]);
                } else {
                    EXPECT_EQ(offsets[i + 1], offsets[i]);
                }
                EXPECT_EQ(slices[idx + i], column->get_slice(i));
            }
        };

        // Full scan
        test_range(0, 1024);

        // Sub-ranges
        test_range(17, 533);
        test_range(100, 101);
        test_range(500, 516);

        // Sub-range where all strings are non-empty: offsets must be strictly monotonic
        {
            ASSERT_OK(decoder.seek_to_position_in_page(500));
            auto column = BinaryColumn::create();
            SparseRange<> range(500, 516);
            size_t count = range.span_size();
            std::vector<uint8_t> selection(count, 1);
            std::vector<uint16_t> selected_idx(count);
            std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
            std::vector<const ColumnPredicate*> predicates{predicate.get()};
            ASSERT_OK(decoder.next_batch_with_filter(column.get(), range, predicates, nullptr, selection.data(),
                                                     selected_idx.data()));
            const auto& offsets = column->get_offset();
            EXPECT_EQ(0, offsets[0]);
            for (size_t i = 0; i < count; ++i) {
                EXPECT_GT(offsets[i + 1], offsets[i]);
            }
        }
    };

    run_tests(false);
    run_tests(true);
}

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, TestOffsetOnlyDecodeAndPredicates) {
    constexpr size_t kTotalRows = 1024;
    std::vector<std::string> raw_strings(kTotalRows);
    std::vector<Slice> slices(kTotalRows);

    for (size_t i = 0; i < kTotalRows; ++i) {
        if (i % 10 == 0) {
            raw_strings[i] = ""; // empty URL
        } else {
            raw_strings[i] =
                    "https://example.com/clickbench/q27/path/" + std::to_string(i) + "/" + std::string(i % 64, 'q');
        }
        slices[i] = Slice(raw_strings[i]);
    }

    for (bool delta : {false, true}) {
        PageBuilderOptions options;
        options.data_page_size = 2 * 1024 * 1024;
        BinaryPlainPageBuilder builder(options, delta);
        ASSERT_EQ(kTotalRows, builder.add(reinterpret_cast<const uint8_t*>(slices.data()), kTotalRows));
        OwnedSlice owned = builder.finish()->build();

        // 1. Offset-only next_batch over SparseRange (simulating multi-range page reads)
        {
            BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(owned.slice(), delta);
            ASSERT_OK(decoder.init());
            decoder.set_offset_only(true);
            ASSERT_TRUE(decoder.is_offset_only());

            auto col = BinaryColumn::create();
            SparseRange<> range;
            range.add(Range<>(0, 128));
            range.add(Range<>(256, 512));
            range.add(Range<>(700, 1024));
            ASSERT_OK(decoder.next_batch(range, col.get()));
            ASSERT_TRUE(col->is_offset_only());
            ASSERT_EQ(0u, col->get_immutable_bytes().size());
            ASSERT_EQ(range.span_size(), col->size());
            col->check_or_die();

            const auto& offsets = col->get_offset();
            size_t out_idx = 0;
            for (size_t r = 0; r < range.size(); ++r) {
                for (uint32_t row = range[r].begin(); row < range[r].end(); ++row, ++out_idx) {
                    ASSERT_EQ(raw_strings[row].size(), offsets[out_idx + 1] - offsets[out_idx]) << "row=" << row;
                }
            }

            // Evaluate BinaryColumnNePredicate(URL <> '') and BinaryColumnEqPredicate(URL = '') directly on col
            std::unique_ptr<ColumnPredicate> ne_empty_pred(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
            std::unique_ptr<ColumnPredicate> eq_empty_pred(new_column_eq_predicate(get_type_info(TYPE_VARCHAR), 0, ""));

            std::vector<uint8_t> ne_sel(col->size(), 0);
            std::vector<uint8_t> eq_sel(col->size(), 0);
            ASSERT_OK(ne_empty_pred->evaluate(col.get(), ne_sel.data(), 0, col->size()));
            ASSERT_OK(eq_empty_pred->evaluate(col.get(), eq_sel.data(), 0, col->size()));
            for (size_t i = 0; i < col->size(); ++i) {
                const bool is_non_empty = (offsets[i + 1] > offsets[i]);
                ASSERT_EQ(is_non_empty ? 1 : 0, ne_sel[i]) << "i=" << i;
                ASSERT_EQ(is_non_empty ? 0 : 1, eq_sel[i]) << "i=" << i;
            }
        }

        // 2. Offset-only next_batch_with_filter with URL <> '' predicate
        {
            BinaryPlainPageDecoder<TYPE_VARCHAR> eager_decoder(owned.slice(), delta);
            ASSERT_OK(eager_decoder.init());
            BinaryPlainPageDecoder<TYPE_VARCHAR> offset_decoder(owned.slice(), delta);
            ASSERT_OK(offset_decoder.init());
            offset_decoder.set_offset_only(true);

            std::unique_ptr<ColumnPredicate> ne_empty_pred(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
            std::vector<const ColumnPredicate*> preds = {ne_empty_pred.get()};

            SparseRange<> range(0, kTotalRows);
            std::vector<uint8_t> eager_sel(kTotalRows, 1);
            std::vector<uint16_t> eager_idx(kTotalRows);
            auto eager_col = BinaryColumn::create();
            ASSERT_OK(eager_decoder.next_batch_with_filter(eager_col.get(), range, preds, nullptr, eager_sel.data(),
                                                           eager_idx.data()));

            std::vector<uint8_t> offset_sel(kTotalRows, 1);
            std::vector<uint16_t> offset_idx(kTotalRows);
            auto offset_col = BinaryColumn::create();
            ASSERT_OK(offset_decoder.next_batch_with_filter(offset_col.get(), range, preds, nullptr, offset_sel.data(),
                                                            offset_idx.data()));

            ASSERT_TRUE(offset_col->is_offset_only());
            ASSERT_EQ(0u, offset_col->get_immutable_bytes().size());
            ASSERT_GT(eager_col->get_immutable_bytes().size(), 0u);
            ASSERT_EQ(eager_col->size(), offset_col->size());
            ASSERT_EQ(eager_sel, offset_sel);
            for (size_t i = 0; i < offset_col->size(); ++i) {
                ASSERT_EQ(eager_col->get_offset()[i + 1] - eager_col->get_offset()[i],
                          offset_col->get_offset()[i + 1] - offset_col->get_offset()[i]);
            }
        }

        // 3. Offset-only read_by_rowids (late materialization path)
        {
            BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(owned.slice(), delta);
            ASSERT_OK(decoder.init());
            decoder.set_offset_only(true);

            auto col = BinaryColumn::create();
            std::vector<rowid_t> rowids = {0, 3, 10, 11, 255, 512, 1023};
            size_t num_read = rowids.size();
            ASSERT_OK(decoder.read_by_rowids(0, rowids.data(), &num_read, col.get()));
            ASSERT_EQ(rowids.size(), num_read);
            ASSERT_TRUE(col->is_offset_only());
            ASSERT_EQ(0u, col->get_immutable_bytes().size());
            col->check_or_die();
            for (size_t i = 0; i < rowids.size(); ++i) {
                ASSERT_EQ(raw_strings[rowids[i]].size(), col->get_offset()[i + 1] - col->get_offset()[i]);
            }
        }
    }
}

// NOLINTNEXTLINE
TEST_F(BinaryPlainPageTest, TestOffsetOnlyColdScanBenchmark) {
    // Simulate ClickBench Q27 cold storage scan:
    // SELECT CounterID, AVG(length(URL)), COUNT(*) FROM hits WHERE URL <> '' GROUP BY CounterID;
    constexpr size_t kRowsPerPage = 4096;
    constexpr size_t kNumPages = 32; // 131,072 rows per pass (~14 MB URL payload per pass)
    constexpr size_t kPasses = 8;    // ~1.05M rows, ~112 MB URL payload scanned

    std::vector<std::string> page_strings(kRowsPerPage);
    std::vector<Slice> page_slices(kRowsPerPage);
    for (size_t i = 0; i < kRowsPerPage; ++i) {
        if (i % 10 == 0) {
            page_strings[i] = "";
        } else {
            page_strings[i] =
                    "https://yandex.ru/clck/jsredir?from=yandex.ru%3Bsearch%3Bweb%3B%3B&text=clickbench_q27_" +
                    std::to_string(i) + "_" + std::string(24 + (i % 48), 'a');
        }
        page_slices[i] = Slice(page_strings[i]);
    }

    std::vector<OwnedSlice> pages(kNumPages);
    for (size_t p = 0; p < kNumPages; ++p) {
        PageBuilderOptions options;
        options.data_page_size = 2 * 1024 * 1024;
        BinaryPlainPageBuilder builder(options);
        ASSERT_EQ(kRowsPerPage, builder.add(reinterpret_cast<const uint8_t*>(page_slices.data()), kRowsPerPage));
        pages[p] = builder.finish()->build();
    }

    std::unique_ptr<ColumnPredicate> ne_empty_pred(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
    std::vector<const ColumnPredicate*> preds = {ne_empty_pred.get()};

    auto run_scan = [&](bool offset_only, uint64_t* out_rows, uint64_t* out_sum_len, uint64_t* out_bytes_copied,
                        double* out_ms) {
        uint64_t total_rows = 0;
        uint64_t sum_len = 0;
        uint64_t bytes_copied = 0;
        std::vector<uint8_t> selection(kRowsPerPage);
        std::vector<uint16_t> selected_idx(kRowsPerPage);
        auto col = BinaryColumn::create();
        col->set_offset_only(offset_only);

        auto t0 = std::chrono::steady_clock::now();
        for (size_t pass = 0; pass < kPasses; ++pass) {
            for (size_t p = 0; p < kNumPages; ++p) {
                BinaryPlainPageDecoder<TYPE_VARCHAR> decoder(pages[p].slice());
                ASSERT_OK(decoder.init());
                decoder.set_offset_only(offset_only);

                col->reset_column();
                col->set_offset_only(offset_only);
                std::fill(selection.begin(), selection.end(), 1);
                SparseRange<> range(0, kRowsPerPage);
                ASSERT_OK(decoder.next_batch_with_filter(col.get(), range, preds, nullptr, selection.data(),
                                                         selected_idx.data()));
                bytes_copied += col->get_immutable_bytes().size();

                const size_t nrows = col->size();
                total_rows += nrows;
                const auto* off_ptr = col->get_offset().small_storage().data();
                for (size_t i = 0; i < nrows; ++i) {
                    sum_len += (off_ptr[i + 1] - off_ptr[i]);
                }
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        *out_rows = total_rows;
        *out_sum_len = sum_len;
        *out_bytes_copied = bytes_copied;
        *out_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    uint64_t eager_rows = 0, eager_sum_len = 0, eager_bytes = 0;
    double eager_ms = 0.0;
    run_scan(false, &eager_rows, &eager_sum_len, &eager_bytes, &eager_ms);

    uint64_t offset_rows = 0, offset_sum_len = 0, offset_bytes = 0;
    double offset_ms = 0.0;
    run_scan(true, &offset_rows, &offset_sum_len, &offset_bytes, &offset_ms);

    ASSERT_EQ(eager_rows, offset_rows);
    ASSERT_EQ(eager_sum_len, offset_sum_len);
    ASSERT_GT(eager_bytes, 100ULL * 1024 * 1024);
    ASSERT_EQ(0u, offset_bytes);

    fprintf(stderr,
            "[Track 10A Benchmark] Eager: %.2f ms (payload copied: %.2f MB) | Offset-Only: %.2f ms (payload copied: "
            "%.2f MB) | Speedup: %.2fx\n",
            eager_ms, static_cast<double>(eager_bytes) / (1024.0 * 1024.0), offset_ms,
            static_cast<double>(offset_bytes) / (1024.0 * 1024.0), offset_ms > 0.0 ? (eager_ms / offset_ms) : 0.0);
}

} // namespace starrocks
