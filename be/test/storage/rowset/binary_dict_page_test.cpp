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
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/segment_v2/binary_dict_page_test.cpp

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

#include "storage/rowset/binary_dict_page.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>

#include "column/chunk_factory.h"
#include "column/column.h"
#include "column/column_helper.h"
#include "common/logging.h"
#include "common/util/debug_util.h"
#include "gen_cpp/segment.pb.h"
#include "runtime/mem_pool.h"
#include "storage/chunk_helper.h"
#include "storage/olap_common.h"
#include "storage/rowset/binary_plain_page.h"
#include "storage/rowset/page_decoder.h"
#include "storage/rowset/storage_page_decoder.h"
#include "storage/types.h"
#include "storage_primitive/column_predicate_factory.h"

namespace starrocks {

class BinaryDictPageTest : public testing::Test {
public:
    void test_by_small_data_size(const std::vector<Slice>& slices) {
        // encode
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        options.dict_page_size = 256 * 1024;
        BinaryDictPageBuilder page_builder(options);
        size_t count = slices.size();
        Status status;

        const Slice* ptr = &slices[0];
        count = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), count);

        auto s = page_builder.finish()->build();
        ASSERT_EQ(slices.size(), page_builder.count());
        ASSERT_FALSE(page_builder.is_page_full());

        //check first value and last value
        Slice first_value;
        page_builder.get_first_value(&first_value);
        ASSERT_EQ(slices[0], first_value);
        Slice last_value;
        page_builder.get_last_value(&last_value);
        ASSERT_EQ(slices[count - 1], last_value);

        // construct dict page
        OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
        auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
        status = dict_page_decoder->init();
        ASSERT_TRUE(status.ok());
        // because every slice is unique
        ASSERT_EQ(slices.size(), dict_page_decoder->count());

        // decode
        Slice encoded_data = s.slice();
        PageFooterPB footer;
        footer.set_type(DATA_PAGE);
        DataPageFooterPB* data_page_footer = footer.mutable_data_page_footer();
        data_page_footer->set_nullmap_size(0);
        std::unique_ptr<std::vector<uint8_t>> page = nullptr;

        Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
        ASSERT_TRUE(st.ok());

        BinaryDictPageDecoder<TYPE_VARCHAR> page_decoder(encoded_data);
        page_decoder.set_dict_decoder(dict_page_decoder.get());

        status = page_decoder.init();
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(slices.size(), page_decoder.count());

        //check values

        auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);
        size_t size = slices.size();
        status = page_decoder.next_batch(&size, column.get());
        auto values = GetStorageContainer<TYPE_VARCHAR>::get_data(column);
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(slices.size(), size);
        ASSERT_EQ("Individual", values[0].to_string());
        ASSERT_EQ("Lifetime", values[1].to_string());
        ASSERT_EQ("Objective", values[2].to_string());
        ASSERT_EQ("Value", values[3].to_string());
        ASSERT_EQ("Evolution", values[4].to_string());
        ASSERT_EQ("Nature", values[5].to_string());
        ASSERT_EQ("Captain", values[6].to_string());
        ASSERT_EQ("Xmas", values[7].to_string());

        column->resize(0);
        status = page_decoder.seek_to_position_in_page(5);
        ASSERT_TRUE(status.ok()) << status.to_string();
        status = page_decoder.next_batch(&size, column.get());
        ASSERT_TRUE(status.ok()) << status.to_string();
        // read 3 items
        ASSERT_EQ(3, size);
        values = GetStorageContainer<TYPE_VARCHAR>::get_data(column);
        ASSERT_EQ("Nature", values[0].to_string());
        ASSERT_EQ("Captain", values[1].to_string());
        ASSERT_EQ("Xmas", values[2].to_string());

        ASSERT_TRUE(page_decoder.seek_to_position_in_page(0).ok());
        ASSERT_EQ(0, page_decoder.current_index());
        column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);
        SparseRange<> read_range;
        read_range.add(Range<>(0, 2));
        read_range.add(Range<>(4, 7));
        status = page_decoder.next_batch(read_range, column.get());
        ASSERT_TRUE(status.ok());
        ASSERT_EQ("['Individual', 'Lifetime', 'Evolution', 'Nature', 'Captain']", column->debug_string());
    }

    void test_with_large_data_size(const std::vector<Slice>& contents) {
        Status status;
        // encode
        PageBuilderOptions options;
        // page size: 128K
        options.data_page_size = 128 * 1024;
        options.dict_page_size = 128 * 1024;
        BinaryDictPageBuilder page_builder(options);
        size_t count = contents.size();
        std::vector<OwnedSlice> results;
        std::vector<size_t> page_start_ids;
        size_t total_size = 0;
        page_start_ids.push_back(0);
        for (int i = 0; i < count;) {
            size_t add_num = 1;
            const Slice* ptr = &contents[i];
            add_num = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), add_num);
            if (page_builder.is_page_full()) {
                OwnedSlice s = page_builder.finish()->build();
                total_size += s.slice().size;
                results.emplace_back(std::move(s));
                page_builder.reset();
                page_start_ids.push_back(i + 1);
            }
            i += add_num;
        }
        OwnedSlice s = page_builder.finish()->build();
        total_size += s.slice().size;
        results.emplace_back(std::move(s));

        page_start_ids.push_back(count);

        OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
        size_t data_size = total_size;
        total_size += dict_slice.slice().size;
        ASSERT_TRUE(status.ok());
        LOG(INFO) << "total size:" << total_size << ", data size:" << data_size
                  << ", dict size:" << dict_slice.slice().size << " result page size:" << results.size();

        // validate
        // random 20 times to validate
        srand(time(nullptr));
        for (int i = 0; i < 20; ++i) {
            int slice_index = random() % results.size();
            //int slice_index = 1;
            auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
            status = dict_page_decoder->init();
            ASSERT_TRUE(status.ok());

            // decode
            Slice encoded_data = results[slice_index].slice();
            PageFooterPB footer;
            footer.set_type(DATA_PAGE);
            DataPageFooterPB* data_page_footer = footer.mutable_data_page_footer();
            data_page_footer->set_nullmap_size(0);
            std::unique_ptr<std::vector<uint8_t>> page = nullptr;

            Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
            ASSERT_TRUE(st.ok());

            BinaryDictPageDecoder<TYPE_VARCHAR> page_decoder(encoded_data);
            status = page_decoder.init();
            page_decoder.set_dict_decoder(dict_page_decoder.get());
            ASSERT_TRUE(status.ok());

            //check values
            auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);

            size_t num = 1;
            size_t pos = random() % (page_start_ids[slice_index + 1] - page_start_ids[slice_index]);
            //size_t pos = 613631;
            status = page_decoder.seek_to_position_in_page(pos);
            ASSERT_TRUE(status.ok());
            status = page_decoder.next_batch(&num, column.get());
            ASSERT_TRUE(status.ok());
            const auto values = GetStorageContainer<TYPE_VARCHAR>::get_data(column);
            std::string expect = contents[page_start_ids[slice_index] + pos].to_string();
            std::string actual = values[0].to_string();
            ASSERT_EQ(expect, actual) << "slice index:" << slice_index << ", pos:" << pos
                                      << ", expect:" << hexdump((char*)expect.data(), expect.size())
                                      << ", actual:" << hexdump((char*)actual.data(), actual.size())
                                      << ", line number:" << page_start_ids[slice_index] + pos + 1;

            status = page_decoder.seek_to_position_in_page(0);
            ASSERT_TRUE(status.ok());
            size_t slice_num = page_start_ids[slice_index + 1] - page_start_ids[slice_index];
            auto dst = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);
            SparseRange<> read_range;
            read_range.add(Range<>(0, slice_num / 3));
            read_range.add(Range<>(slice_num / 2, (slice_num * 2 / 3)));
            read_range.add(Range<>((slice_num * 3 / 4), slice_num));
            size_t read_num = read_range.span_size();

            status = page_decoder.next_batch(read_range, dst.get());
            ASSERT_TRUE(status.ok());
            ASSERT_EQ(read_num, dst->size());

            size_t offset = 0;
            SparseRangeIterator<> read_iter = read_range.new_iterator();
            while (read_iter.has_more()) {
                Range<> r = read_iter.next(read_num);
                for (int i = 0; i < r.span_size(); ++i) {
                    std::string expect = contents[page_start_ids[slice_index] + r.begin() + i].to_string();
                    std::string actual = dst->get(i + offset).get_slice().to_string();
                    ASSERT_EQ(expect, actual) << "slice index:" << slice_index << ", pos:" << offset + i
                                              << ", expect:" << hexdump((char*)expect.data(), expect.size())
                                              << ", actual:" << hexdump((char*)actual.data(), actual.size())
                                              << ", line number:" << page_start_ids[slice_index] + offset + i + 1;
                }
                offset += r.span_size();
            }
        }
    }
};

// NOLINTNEXTLINE
TEST_F(BinaryDictPageTest, TestBySmallDataSize) {
    std::vector<Slice> slices;
    slices.emplace_back("Individual");
    slices.emplace_back("Lifetime");
    slices.emplace_back("Objective");
    slices.emplace_back("Value");
    slices.emplace_back("Evolution");
    slices.emplace_back("Nature");
    slices.emplace_back("Captain");
    slices.emplace_back("Xmas");
    test_by_small_data_size(slices);
}

// NOLINTNEXTLINE
TEST_F(BinaryDictPageTest, TestEncodingRatio) {
    std::vector<Slice> slices;
    std::vector<std::string> src_strings;
    std::string file = "./be/test/storage/test_data/dict_encoding_data.dat";
    std::string line;
    std::ifstream infile(file.c_str());
    while (getline(infile, line)) {
        src_strings.emplace_back(line);
    }
    for (int i = 0; i < 1000; ++i) {
        for (const auto& src_string : src_strings) {
            slices.emplace_back(src_string);
        }
    }

    LOG(INFO) << "source line number:" << slices.size();
    test_with_large_data_size(slices);
}

TEST_F(BinaryDictPageTest, TestNextBatchWithFilter) {
    std::vector<Slice> slices;
    slices.emplace_back("a_100");
    slices.emplace_back("b_200");
    slices.emplace_back("c_300");
    slices.emplace_back("d_400");
    slices.emplace_back("e_500");

    // encode
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    options.dict_page_size = 256 * 1024;
    BinaryDictPageBuilder page_builder(options);
    size_t count = slices.size();

    const Slice* ptr = &slices[0];
    count = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), count);
    auto s = page_builder.finish()->build();

    // construct dict page
    OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
    auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
    ASSERT_TRUE(dict_page_decoder->init().ok());

    // decode
    Slice encoded_data = s.slice();
    PageFooterPB footer;
    footer.set_type(DATA_PAGE);
    DataPageFooterPB* data_page_footer = footer.mutable_data_page_footer();
    data_page_footer->set_nullmap_size(0);
    std::unique_ptr<std::vector<uint8_t>> page = nullptr;

    Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
    ASSERT_TRUE(st.ok());

    BinaryDictPageDecoder<TYPE_VARCHAR> page_decoder(encoded_data);
    page_decoder.set_dict_decoder(dict_page_decoder.get());
    ASSERT_TRUE(page_decoder.init().ok());

    // Case 1: Without NULLs (nullptr passed for null_data)
    {
        auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);

        // Prepare filter: >= "c_300"
        std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "c_300"));
        std::vector<const ColumnPredicate*> predicates;
        predicates.push_back(predicate.get());

        SparseRange<> range(0, 5);
        std::vector<uint8_t> selection(5);
        std::vector<uint16_t> selected_idx(5);

        // reset selection
        for (int i = 0; i < 5; ++i) selection[i] = 1;

        st = page_decoder.next_batch_with_filter(column.get(), range, predicates, nullptr, selection.data(),
                                                 selected_idx.data());
        ASSERT_TRUE(st.ok()) << st.to_string();

        // Check selection
        ASSERT_EQ(0, selection[0]);
        ASSERT_EQ(0, selection[1]);
        ASSERT_EQ(1, selection[2]);
        ASSERT_EQ(1, selection[3]);
        ASSERT_EQ(1, selection[4]);

        ASSERT_EQ("['c_300', 'd_400', 'e_500']", column->debug_string());
    }

    // Reset decoder state
    ASSERT_TRUE(page_decoder.seek_to_position_in_page(0).ok());

    // Case 2: With NULLs (null_data passed)
    {
        // Use NullableColumn
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

        st = page_decoder.next_batch_with_filter(column.get(), range, predicates, null_data, selection.data(),
                                                 selected_idx.data());
        ASSERT_TRUE(st.ok()) << st.to_string();

        // Check selection
        // 0: "a_100" < "c_300" -> 0
        // 1: "b_200" < "c_300" -> 0
        // 2: NULL < "c_300" (unknown/false) -> 0
        // 3: "d_400" >= "c_300" -> 1
        // 4: NULL < "c_300" -> 0

        ASSERT_EQ(0, selection[0]);
        ASSERT_EQ(0, selection[1]);
        ASSERT_EQ(0, selection[2]);
        ASSERT_EQ(1, selection[3]);
        ASSERT_EQ(0, selection[4]);

        // Check column data
        // Should contain 1 row: "d_400"
        ASSERT_EQ(1, column->size());
        ASSERT_EQ("d_400", column->get(0).get_slice().to_string());
    }
}

// Same scenario as BinaryPlainPageTest.TestNextBatchWithFilterSparseRangeWithNulls for the dictionary decoder: the null
// flags are page-level and indexed by ordinal, a sparse range must not consume them linearly.
TEST_F(BinaryDictPageTest, TestNextBatchWithFilterSparseRangeWithNulls) {
    std::vector<Slice> slices{"a_100", "b_200", "c_300", "d_400", "e_500"};

    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    options.dict_page_size = 256 * 1024;
    BinaryDictPageBuilder page_builder(options);
    size_t count = page_builder.add(reinterpret_cast<const uint8_t*>(slices.data()), slices.size());
    ASSERT_EQ(slices.size(), count);
    auto s = page_builder.finish()->build();

    OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
    auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
    ASSERT_TRUE(dict_page_decoder->init().ok());

    Slice encoded_data = s.slice();
    PageFooterPB footer;
    footer.set_type(DATA_PAGE);
    footer.mutable_data_page_footer()->set_nullmap_size(0);
    std::unique_ptr<std::vector<uint8_t>> page = nullptr;
    Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
    ASSERT_TRUE(st.ok());

    BinaryDictPageDecoder<TYPE_VARCHAR> page_decoder(encoded_data);
    page_decoder.set_dict_decoder(dict_page_decoder.get());
    ASSERT_TRUE(page_decoder.init().ok());

    auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
    std::unique_ptr<ColumnPredicate> predicate(new_column_ge_predicate(get_type_info(TYPE_VARCHAR), 0, "c_300"));
    std::vector<const ColumnPredicate*> predicates{predicate.get()};

    // read ordinal 0 and ordinals 3..4, skip 1..2
    SparseRange<> range;
    range.add(Range<>(0, 1));
    range.add(Range<>(3, 5));
    std::vector<uint8_t> selection(3, 1);
    std::vector<uint16_t> selected_idx(3);

    // ordinals 1, 2 and 4 are null; a linear walk would hand {0, 1, 1} to the 3 rows read and drop "d_400"
    uint8_t null_data[] = {0, 1, 1, 0, 1};

    st = page_decoder.next_batch_with_filter(column.get(), range, predicates, null_data, selection.data(),
                                             selected_idx.data());
    ASSERT_TRUE(st.ok()) << st.to_string();

    ASSERT_EQ(0, selection[0]);
    ASSERT_EQ(1, selection[1]);
    ASSERT_EQ(0, selection[2]);

    ASSERT_EQ(1, column->size());
    ASSERT_FALSE(down_cast<NullableColumn*>(column.get())->has_null());
    ASSERT_EQ("d_400", column->get(0).get_slice().to_string());
}

TEST_F(BinaryDictPageTest, TestReadByRowids) {
    std::vector<std::string> strings;
    std::vector<Slice> slices;
    strings.reserve(10);
    for (int i = 0; i < 10; ++i) {
        strings.emplace_back("val_" + std::to_string(i));
        slices.emplace_back(strings.back());
    }

    // encode
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    options.dict_page_size = 256 * 1024;
    BinaryDictPageBuilder page_builder(options);
    size_t count = slices.size();

    const Slice* ptr = &slices[0];
    page_builder.add(reinterpret_cast<const uint8_t*>(ptr), count);
    auto s = page_builder.finish()->build();

    // construct dict page
    OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
    auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
    ASSERT_TRUE(dict_page_decoder->init().ok());

    // decode
    Slice encoded_data = s.slice();
    PageFooterPB footer;
    footer.set_type(DATA_PAGE);
    DataPageFooterPB* data_page_footer = footer.mutable_data_page_footer();
    data_page_footer->set_nullmap_size(0);
    std::unique_ptr<std::vector<uint8_t>> page = nullptr;

    Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
    ASSERT_TRUE(st.ok());

    BinaryDictPageDecoder<TYPE_VARCHAR> page_decoder(encoded_data);
    page_decoder.set_dict_decoder(dict_page_decoder.get());
    ASSERT_TRUE(page_decoder.init().ok());

    auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);

    rowid_t rowids[] = {1, 3, 5, 8};
    size_t num_read = 4;

    st = page_decoder.read_by_rowids(0, rowids, &num_read, column.get());
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(4, num_read);
    ASSERT_EQ("['val_1', 'val_3', 'val_5', 'val_8']", column->debug_string());
}

// A predicate that rejects every dictionary entry used to make next_batch_with_filter return before consuming the
// codes of `range`, leaving the data page decoder behind the page cursor. The next range read from the same page then
// trips `DCHECK_EQ(_offset_in_page, _data_decoder->current_index())` in ParsedPageV2::read_with_filter
// (parsed_page.cpp), which is how the fuzzer killed an ASan BE: a 100000-row VARCHAR column whose dictionary
// overflowed mid-segment, filtered by a value absent from the dictionary, read chunk by chunk from the same page.
// The decoder must end every call positioned at range.end(), whether or not any row survived the filter.
TEST_F(BinaryDictPageTest, TestNextBatchWithFilterAllDictRejectedAdvancesCursor) {
    std::vector<std::string> values;
    std::vector<Slice> slices;
    for (int i = 0; i < 8; ++i) {
        values.emplace_back("v_" + std::to_string(i));
    }
    for (const auto& v : values) {
        slices.emplace_back(v);
    }

    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    options.dict_page_size = 256 * 1024;
    BinaryDictPageBuilder page_builder(options);
    size_t count = slices.size();
    const Slice* ptr = &slices[0];
    count = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), count);
    ASSERT_EQ(8, count);
    auto s = page_builder.finish()->build();

    OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
    auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
    ASSERT_TRUE(dict_page_decoder->init().ok());

    Slice encoded_data = s.slice();
    PageFooterPB footer;
    footer.set_type(DATA_PAGE);
    footer.mutable_data_page_footer()->set_nullmap_size(0);
    std::unique_ptr<std::vector<uint8_t>> page = nullptr;
    ASSERT_TRUE(StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data).ok());

    BinaryDictPageDecoder<TYPE_VARCHAR> page_decoder(encoded_data);
    page_decoder.set_dict_decoder(dict_page_decoder.get());
    ASSERT_TRUE(page_decoder.init().ok());
    ASSERT_EQ(DICT_ENCODING, page_decoder.encoding_type());

    // "zzz" is in no dictionary entry, so the dictionary-level selection is empty for every range of this page.
    std::unique_ptr<ColumnPredicate> predicate(new_column_eq_predicate(get_type_info(TYPE_VARCHAR), 0, "zzz"));
    std::vector<const ColumnPredicate*> predicates{predicate.get()};

    auto column = ChunkFactory::column_from_field_type(TYPE_VARCHAR, false);
    std::vector<uint8_t> selection(8, 1);
    std::vector<uint16_t> selected_idx(8);

    // First half of the page: nothing selected, and the decoder has to stand at row 4 afterwards.
    Status st = page_decoder.next_batch_with_filter(column.get(), SparseRange<>(0, 4), predicates, nullptr,
                                                    selection.data(), selected_idx.data());
    ASSERT_TRUE(st.ok()) << st.to_string();
    ASSERT_EQ(0, column->size());
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(0, selection[i]) << i;
    }
    ASSERT_EQ(4, page_decoder.current_index());

    // Second half: exactly what ParsedPageV2::read_with_filter issues for the next chunk of the same page.
    st = page_decoder.next_batch_with_filter(column.get(), SparseRange<>(4, 8), predicates, nullptr,
                                             selection.data() + 4, selected_idx.data() + 4);
    ASSERT_TRUE(st.ok()) << st.to_string();
    ASSERT_EQ(0, column->size());
    ASSERT_EQ(8, page_decoder.current_index());
}

TEST_F(BinaryDictPageTest, TestOffsetOnlyNullableDecodeAndPredicates) {
    auto compare_nullable_offset_only_vs_eager = [](const Column* offset_col, const Column* eager_col) {
        offset_col->check_or_die();
        eager_col->check_or_die();

        const auto* offset_null = down_cast<const NullableColumn*>(offset_col);
        const auto* eager_null = down_cast<const NullableColumn*>(eager_col);
        ASSERT_EQ(eager_null->size(), offset_null->size());
        ASSERT_EQ(eager_null->has_null(), offset_null->has_null());

        const auto& offset_null_flags = offset_null->immutable_null_column_data();
        const auto& eager_null_flags = eager_null->immutable_null_column_data();
        ASSERT_EQ(eager_null_flags.size(), offset_null_flags.size());

        const auto* offset_bin = down_cast<const BinaryColumn*>(offset_null->data_column().get());
        const auto* eager_bin = down_cast<const BinaryColumn*>(eager_null->data_column().get());
        ASSERT_TRUE(offset_bin->is_offset_only());
        ASSERT_FALSE(eager_bin->is_offset_only());
        ASSERT_EQ(0u, offset_bin->get_immutable_bytes().size());

        const auto offset_offsets = offset_bin->get_offset();
        const auto eager_offsets = eager_bin->get_offset();
        ASSERT_EQ(offset_null->size() + 1, offset_offsets.size());
        ASSERT_EQ(eager_null->size() + 1, eager_offsets.size());

        for (size_t i = 0; i < offset_null->size(); ++i) {
            ASSERT_EQ(eager_null_flags[i], offset_null_flags[i]) << "null flag mismatch at row " << i;
            if (offset_null_flags[i] == 0) {
                size_t offset_len = offset_offsets[i + 1] - offset_offsets[i];
                size_t eager_len = eager_offsets[i + 1] - eager_offsets[i];
                ASSERT_EQ(eager_len, offset_len) << "non-null length mismatch at row " << i;
                ASSERT_EQ(eager_bin->get_slice(i).size, offset_len) << "slice size mismatch at row " << i;
            }
        }
    };

    // 16 rows mixing NULLs (encoded as "" in data page with null_data[i] == 1), empty strings "", and non-empty strings.
    const std::vector<std::string> page0_strings = {
            "",                // 0: NULL
            "",                // 1: empty
            "alpha",           // 2: non-empty (5)
            "beta_longer",     // 3: non-empty (11)
            "",                // 4: NULL
            "gamma_val_12345", // 5: non-empty (15)
            "",                // 6: empty
            "",                // 7: NULL
            "alpha",           // 8: non-empty (5)
            "",                // 9: empty
            "beta_longer",     // 10: non-empty (11)
            "",                // 11: NULL
            "alpha",           // 12: non-empty (5)
            "",                // 13: empty
            "",                // 14: NULL
            "z",               // 15: non-empty (1) - pushes dictionary past dict_page_size
    };
    const std::vector<uint8_t> page0_nulls = {
            1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0,
    };
    ASSERT_EQ(page0_strings.size(), page0_nulls.size());

    std::vector<Slice> page0_slices;
    page0_slices.reserve(page0_strings.size());
    for (const auto& s : page0_strings) {
        page0_slices.emplace_back(s);
    }

    // Small dict_page_size so page 0 uses DICT_ENCODING and overflows the dictionary on its last row,
    // causing page 1 after reset() to fall back to PLAIN_ENCODING.
    PageBuilderOptions options;
    options.data_page_size = 256 * 1024;
    options.dict_page_size = 52;
    BinaryDictPageBuilder page_builder(options);

    size_t count0 = page0_slices.size();
    const Slice* ptr0 = page0_slices.data();
    ASSERT_EQ(count0, page_builder.add(reinterpret_cast<const uint8_t*>(ptr0), count0));
    ASSERT_TRUE(page_builder.is_page_full());
    ASSERT_TRUE(page_builder.all_dict_encoded());
    OwnedSlice page0_owned = page_builder.finish()->build();

    // Resetting after dictionary full transitions builder to PLAIN_ENCODING for the fallback page.
    page_builder.reset();

    const std::vector<std::string> page1_strings = {
            "",                        // 0: empty
            "",                        // 1: NULL
            "plain_fallback_short",    // 2: non-empty (20)
            "",                        // 3: NULL
            "",                        // 4: empty
            "plain_fallback_longer_x", // 5: non-empty (23)
            "k",                       // 6: non-empty (1)
            "",                        // 7: NULL
            "",                        // 8: empty
            "plain_fallback_short",    // 9: non-empty (20)
            "",                        // 10: NULL
            "tail_value_99",           // 11: non-empty (13)
    };
    const std::vector<uint8_t> page1_nulls = {
            0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0,
    };
    ASSERT_EQ(page1_strings.size(), page1_nulls.size());

    std::vector<Slice> page1_slices;
    page1_slices.reserve(page1_strings.size());
    for (const auto& s : page1_strings) {
        page1_slices.emplace_back(s);
    }
    size_t count1 = page1_slices.size();
    const Slice* ptr1 = page1_slices.data();
    ASSERT_EQ(count1, page_builder.add(reinterpret_cast<const uint8_t*>(ptr1), count1));
    OwnedSlice page1_owned = page_builder.finish()->build();
    ASSERT_FALSE(page_builder.all_dict_encoded());

    OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
    auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<TYPE_VARCHAR>>(dict_slice.slice());
    ASSERT_TRUE(dict_page_decoder->init().ok());

    auto decode_raw_page = [](const OwnedSlice& owned, std::unique_ptr<std::vector<uint8_t>>* page_buf,
                              Slice* out_slice) {
        *out_slice = owned.slice();
        PageFooterPB footer;
        footer.set_type(DATA_PAGE);
        footer.mutable_data_page_footer()->set_nullmap_size(0);
        ASSERT_TRUE(StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, page_buf, out_slice).ok());
    };

    std::unique_ptr<std::vector<uint8_t>> page0_buf;
    Slice page0_encoded;
    decode_raw_page(page0_owned, &page0_buf, &page0_encoded);

    std::unique_ptr<std::vector<uint8_t>> page1_buf;
    Slice page1_encoded;
    decode_raw_page(page1_owned, &page1_buf, &page1_encoded);

    auto make_decoder = [&](const Slice& encoded_data, EncodingTypePB expected_encoding, bool offset_only,
                            bool set_before_init) {
        auto decoder = std::make_unique<BinaryDictPageDecoder<TYPE_VARCHAR>>(encoded_data);
        decoder->set_dict_decoder(dict_page_decoder.get());
        if (offset_only && set_before_init) {
            decoder->set_offset_only(true);
        }
        Status st = decoder->init();
        EXPECT_TRUE(st.ok()) << st.to_string();
        EXPECT_EQ(expected_encoding, decoder->encoding_type());
        if (offset_only && !set_before_init) {
            decoder->set_offset_only(true);
        }
        return decoder;
    };

    // Predicates compatible with offset-only columns: col <> '', col = '', col IS NOT NULL, col IS NULL.
    std::vector<std::unique_ptr<ColumnPredicate>> pred_holders;
    pred_holders.emplace_back(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
    pred_holders.emplace_back(new_column_eq_predicate(get_type_info(TYPE_VARCHAR), 0, ""));
    pred_holders.emplace_back(new_column_null_predicate(get_type_info(TYPE_VARCHAR), 0, false));
    pred_holders.emplace_back(new_column_null_predicate(get_type_info(TYPE_VARCHAR), 0, true));

    auto verify_page_decodes = [&](const Slice& encoded_data, EncodingTypePB expected_encoding,
                                   const std::vector<uint8_t>& page_nulls, const SparseRange<>& multi_range,
                                   const SparseRange<>& single_range, const std::vector<rowid_t>& rowids) {
        for (bool set_before_init : {true, false}) {
            // Subcase A & C: Sparse filtered reads (next_batch_with_filter) across multi-range with gaps
            // and single-range, both with null_data != nullptr and null_data == nullptr.
            for (const SparseRange<>& range : {multi_range, single_range}) {
                const size_t num_rows = range.span_size();
                for (const auto& pred_ptr : pred_holders) {
                    std::vector<const ColumnPredicate*> preds{pred_ptr.get()};

                    for (const uint8_t* null_data_ptr : {page_nulls.data(), static_cast<const uint8_t*>(nullptr)}) {
                        auto eager_dec = make_decoder(encoded_data, expected_encoding, false, set_before_init);
                        auto offset_dec = make_decoder(encoded_data, expected_encoding, true, set_before_init);

                        auto eager_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
                        auto offset_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);

                        std::vector<uint8_t> eager_sel(num_rows, 0);
                        std::vector<uint8_t> offset_sel(num_rows, 0);
                        std::vector<uint16_t> eager_idx(num_rows, 0);
                        std::vector<uint16_t> offset_idx(num_rows, 0);

                        Status st1 = eager_dec->next_batch_with_filter(eager_col.get(), range, preds, null_data_ptr,
                                                                       eager_sel.data(), eager_idx.data());
                        Status st2 = offset_dec->next_batch_with_filter(offset_col.get(), range, preds, null_data_ptr,
                                                                        offset_sel.data(), offset_idx.data());
                        ASSERT_TRUE(st1.ok()) << st1.to_string();
                        ASSERT_TRUE(st2.ok()) << st2.to_string();
                        ASSERT_EQ(eager_sel, offset_sel);
                        ASSERT_EQ(eager_dec->current_index(), offset_dec->current_index());
                        compare_nullable_offset_only_vs_eager(offset_col.get(), eager_col.get());
                    }
                }

                // Also verify unfiltered next_batch(SparseRange<>, Column*) into NullableColumn with null flags.
                auto populate_null_flags_for_range = [&](NullableColumn* nullable_col) {
                    auto* null_col = nullable_col->null_column_raw_ptr();
                    for (size_t i = 0; i < range.size(); ++i) {
                        Range<> r = range[i];
                        null_col->append_numbers(page_nulls.data() + r.begin(), r.span_size() * sizeof(uint8_t));
                    }
                    nullable_col->update_has_null();
                };

                auto eager_dec = make_decoder(encoded_data, expected_encoding, false, set_before_init);
                auto offset_dec = make_decoder(encoded_data, expected_encoding, true, set_before_init);

                auto eager_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
                auto offset_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
                auto* eager_nullable = down_cast<NullableColumn*>(eager_col.get());
                auto* offset_nullable = down_cast<NullableColumn*>(offset_col.get());

                ASSERT_TRUE(eager_dec->next_batch(range, eager_nullable->data_column_raw_ptr()).ok());
                ASSERT_TRUE(offset_dec->next_batch(range, offset_nullable->data_column_raw_ptr()).ok());
                populate_null_flags_for_range(eager_nullable);
                populate_null_flags_for_range(offset_nullable);
                compare_nullable_offset_only_vs_eager(offset_col.get(), eager_col.get());
            }

            // Subcase B & C: Row-ID reads (read_by_rowids) into NullableColumn (both direct and with page null map).
            {
                auto eager_dec = make_decoder(encoded_data, expected_encoding, false, set_before_init);
                auto offset_dec = make_decoder(encoded_data, expected_encoding, true, set_before_init);

                auto eager_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
                auto offset_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);

                size_t eager_read = rowids.size();
                size_t offset_read = rowids.size();
                ASSERT_TRUE(eager_dec->read_by_rowids(0, rowids.data(), &eager_read, eager_col.get()).ok());
                ASSERT_TRUE(offset_dec->read_by_rowids(0, rowids.data(), &offset_read, offset_col.get()).ok());
                ASSERT_EQ(rowids.size(), eager_read);
                ASSERT_EQ(rowids.size(), offset_read);
                compare_nullable_offset_only_vs_eager(offset_col.get(), eager_col.get());

                // Apply the page's null flags for the requested rowids (matching ParsedPageV2::read_by_rowids)
                // and verify null flags and non-null lengths match eager decoding.
                auto* eager_nullable = down_cast<NullableColumn*>(eager_col.get());
                auto* offset_nullable = down_cast<NullableColumn*>(offset_col.get());
                for (size_t i = 0; i < rowids.size(); ++i) {
                    eager_nullable->null_column_data()[i] = page_nulls[rowids[i]];
                    offset_nullable->null_column_data()[i] = page_nulls[rowids[i]];
                }
                eager_nullable->update_has_null();
                offset_nullable->update_has_null();
                compare_nullable_offset_only_vs_eager(offset_col.get(), eager_col.get());
            }
        }
    };

    // Page 0 (DICT_ENCODING): multi-range with gaps [0, 3), [5, 9), [11, 16), single-range [2, 14), and rowids.
    SparseRange<> page0_multi_range;
    page0_multi_range.add(Range<>(0, 3));
    page0_multi_range.add(Range<>(5, 9));
    page0_multi_range.add(Range<>(11, 16));
    SparseRange<> page0_single_range(2, 14);
    std::vector<rowid_t> page0_rowids = {0, 1, 2, 4, 5, 6, 7, 11, 12, 13, 15};
    verify_page_decodes(page0_encoded, DICT_ENCODING, page0_nulls, page0_multi_range, page0_single_range, page0_rowids);

    // Page 1 (PLAIN_ENCODING fallback): multi-range with gaps [0, 4), [5, 8), [9, 12), single-range [1, 11), and rowids.
    SparseRange<> page1_multi_range;
    page1_multi_range.add(Range<>(0, 4));
    page1_multi_range.add(Range<>(5, 8));
    page1_multi_range.add(Range<>(9, 12));
    SparseRange<> page1_single_range(1, 11);
    std::vector<rowid_t> page1_rowids = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    verify_page_decodes(page1_encoded, PLAIN_ENCODING, page1_nulls, page1_multi_range, page1_single_range,
                        page1_rowids);
}

} // namespace starrocks
