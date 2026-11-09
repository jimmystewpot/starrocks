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
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/segment_v2/segment_test.cpp

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

#include "storage/rowset/segment.h"

#include <gtest/gtest.h>

#include <functional>
#include <iostream>

#include "base/failpoint/fail_point.h"
#include "base/testutil/assert.h"
#include "base/utility/defer_op.h"
#include "column/chunk_factory.h"
#include "column/datum_tuple.h"
#include "common/config_exec_fwd.h"
#include "common/config_rowset_fwd.h"
#include "common/logging.h"
#include "fs/fs_memory.h"
#include "gutil/strings/substitute.h"
#include "platform/key_cache.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/chunk_helper.h"
#include "storage/olap_common.h"
#include "storage/rowset/column_iterator.h"
#include "storage/rowset/column_reader.h"
#include "storage/rowset/segment_options.h"
#include "storage/rowset/segment_writer.h"
#include "storage/tablet_schema.h"
#include "storage/tablet_schema_helper.h"
#include "storage_primitive/chunk_iterator.h"
#include "storage_primitive/column_predicate.h"

namespace starrocks {

using std::string;
using std::shared_ptr;

using std::vector;

using ValueGenerator = std::function<Datum(size_t rid, int cid, int block_id)>;

// 0,  1,  2,  3
// 10, 11, 12, 13
// 20, 21, 22, 23
static Datum DefaultIntGenerator(size_t rid, int cid, int block_id) {
    return {static_cast<int32_t>(rid * 10 + cid)};
}

class SegmentReaderWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        _fs = std::make_shared<MemoryFileSystem>();
        ASSERT_TRUE(_fs->create_dir(kSegmentDir).ok());
    }

    void TearDown() override {}

    void build_segment(const SegmentWriterOptions& opts, const TabletSchemaCSPtr& build_schema,
                       const TabletSchemaCSPtr& query_schema, size_t nrows, const ValueGenerator& generator,
                       shared_ptr<Segment>* res) {
        static int seg_id = 0;
        // must use unique filename for each segment, otherwise page cache kicks in and produces
        // the wrong answer (it use (filename,offset) as cache key)
        std::string filename = strings::Substitute("$0/seg_$1.dat", kSegmentDir, seg_id++);
        ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
        SegmentWriter writer(std::move(wfile), 0, build_schema, opts);
        ASSERT_OK(writer.init());

        auto schema = ChunkHelper::convert_schema(build_schema);
        auto chunk = ChunkFactory::new_chunk(schema, nrows);
        for (size_t rid = 0; rid < nrows; ++rid) {
            auto cols = chunk->columns();
            for (int cid = 0; cid < build_schema->num_columns(); ++cid) {
                int row_block_id = rid / opts.num_rows_per_block;
                cols[cid]->as_mutable_ptr()->append_datum(generator(rid, cid, row_block_id));
            }
        }
        ASSERT_OK(writer.append_chunk(*chunk));

        uint64_t file_size, index_size, footer_position;
        ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));

        *res = *Segment::open(_fs, FileInfo{filename}, 0, query_schema);
        ASSERT_EQ(nrows, (*res)->num_rows());
    }

    const std::string kSegmentDir = "/segment_test";

    std::shared_ptr<MemoryFileSystem> _fs = nullptr;
    std::unique_ptr<MemTracker> _page_cache_mem_tracker = nullptr;
};

// The per-column page size has to survive the whole way from the table property to the
// column writer: the page is what a point lookup decompresses, and a schema that loses
// the size on the way silently writes the column at the BE default instead. Assert on
// what the segment actually contains rather than on the schema that produced it.
TEST_F(SegmentReaderWriterTest, per_column_zstd_page_size_changes_the_pages_written) {
    constexpr size_t kRows = 2000;
    constexpr size_t kValueBytes = 1024;

    auto make_schema = [](uint32_t page_size) {
        TabletSchemaPB pb;
        pb.set_keys_type(DUP_KEYS);
        pb.set_num_short_key_columns(1);
        auto* k = pb.add_column();
        k->set_unique_id(1);
        k->set_name("k");
        k->set_type("INT");
        k->set_is_key(true);
        k->set_is_nullable(false);
        k->set_length(4);
        k->set_index_length(4);
        k->set_aggregation("NONE");
        auto* v = pb.add_column();
        v->set_unique_id(2);
        v->set_name("v");
        v->set_type("VARCHAR");
        v->set_is_key(false);
        v->set_is_nullable(false);
        v->set_length(65535);
        v->set_aggregation("NONE");
        v->set_use_zstd_compression(true);
        v->set_zstd_compression_page_size(page_size);
        return TabletSchema::create(pb);
    };

    // values are unique and incompressible enough not to be dictionary-encoded, so the
    // page size is what decides how many pages the column takes.
    auto write = [&](const TabletSchemaCSPtr& schema, int32_t* value_pages) {
        static int seg_id = 0;
        std::string filename = strings::Substitute("$0/page_size_$1.dat", kSegmentDir, seg_id++);
        ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
        SegmentWriterOptions opts;
        SegmentWriter writer(std::move(wfile), 0, schema, opts);
        ASSERT_OK(writer.init());

        auto chunk_schema = ChunkHelper::convert_schema(schema);
        auto chunk = ChunkFactory::new_chunk(chunk_schema, kRows);
        for (size_t rid = 0; rid < kRows; ++rid) {
            // every value must be distinct, or the writer speculates DICT encoding and the
            // column collapses into a single data page whatever the page size is.
            std::string value(kValueBytes, ' ');
            uint64_t state = rid * 6364136223846793005ULL + 1442695040888963407ULL;
            for (size_t i = 0; i < kValueBytes; ++i) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                value[i] = static_cast<char>('a' + ((state >> 33) % 26));
            }
            auto cols = chunk->columns();
            cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(rid)));
            cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(value)));
        }
        ASSERT_OK(writer.append_chunk(*chunk));
        uint64_t file_size, index_size, footer_position;
        ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));

        ASSIGN_OR_ABORT(auto segment, Segment::open(_fs, FileInfo{filename}, 0, schema));
        ASSERT_EQ(kRows, segment->num_rows());
        ASSIGN_OR_ABORT(auto iter, segment->new_column_iterator(schema->column(1), nullptr));
        ColumnIteratorOptions iter_opts;
        ASSIGN_OR_ABORT(auto rfile, _fs->new_random_access_file(filename));
        OlapReaderStatistics stats;
        iter_opts.stats = &stats;
        iter_opts.read_file = rfile.get();
        ASSERT_OK(iter->init(iter_opts));
        *value_pages = segment->column(1)->num_data_pages();
    };

    int32_t default_pages = 0;
    int32_t big_page_pages = 0;
    write(make_schema(0), &default_pages);
    write(make_schema(1024 * 1024), &big_page_pages);

    // ~2 MB of values: roughly 32 pages at the 64 KB default, a couple at 1 MB.
    ASSERT_GT(default_pages, 8);
    ASSERT_LT(big_page_pages, default_pages / 4);
}

TEST_F(SegmentReaderWriterTest, estimate_segment_size) {
    size_t num_rows_per_block = 10;

    std::shared_ptr<TabletSchema> tablet_schema = TabletSchemaHelper::create_tablet_schema(
            {create_int_key_pb(1), create_int_key_pb(2), create_int_key_pb(3), create_int_value_pb(4)}, 2);
    tablet_schema->_num_rows_per_row_block = 2;

    // segment write
    std::string dname = "/segment_write_size";
    ASSERT_OK(_fs->create_dir(dname));

    SegmentWriterOptions opts;
    opts.num_rows_per_block = num_rows_per_block;

    std::string fname = dname + "/int_case";
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(fname));
    SegmentWriter writer(std::move(wfile), 0, tablet_schema, opts);
    ASSERT_OK(writer.init());

    // 0, 1, 2, 3
    // 10, 11, 12, 13
    // 20, 21, 22, 23
    size_t nrows = 1048576;
    auto schema = ChunkHelper::convert_schema(tablet_schema);
    auto chunk = ChunkFactory::new_chunk(schema, nrows);
    for (size_t rid = 0; rid < nrows; ++rid) {
        auto cols = chunk->columns();
        for (int cid = 0; cid < tablet_schema->num_columns(); ++cid) {
            cols[cid]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(rid * 10 + cid)));
        }
    }
    ASSERT_OK(writer.append_chunk(*chunk));

    uint32_t segment_size = writer.estimate_segment_size();
    LOG(INFO) << "estimated segment sizes=" << segment_size;

    uint64_t file_size = 0;
    uint64_t index_size;
    uint64_t footer_position;
    ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));

    file_size = _fs->get_file_size(fname).value();
    LOG(INFO) << "segment file size=" << file_size;

    ASSERT_NE(segment_size, 0);
}

TEST_F(SegmentReaderWriterTest, TestBloomFilterIndexUniqueModel) {
    std::shared_ptr<TabletSchema> schema =
            TabletSchemaHelper::create_tablet_schema({create_int_key_pb(1), create_int_key_pb(2), create_int_key_pb(3),
                                                      create_int_value_pb(4, "REPLACE", true, "", true)});

    // for not base segment
    SegmentWriterOptions opts1;
    shared_ptr<Segment> seg1;
    build_segment(opts1, schema, schema, 100, DefaultIntGenerator, &seg1);
    ASSERT_TRUE(seg1->column(3)->has_bloom_filter_index());

    // for base segment
    SegmentWriterOptions opts2;
    shared_ptr<Segment> seg2;
    build_segment(opts2, schema, schema, 100, DefaultIntGenerator, &seg2);
    ASSERT_TRUE(seg2->column(3)->has_bloom_filter_index());
}

TEST_F(SegmentReaderWriterTest, TestHorizontalWrite) {
    std::shared_ptr<TabletSchema> tablet_schema = TabletSchemaHelper::create_tablet_schema(
            {create_int_key_pb(1), create_int_key_pb(2), create_int_value_pb(3), create_int_value_pb(4)});

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;

    std::string file_name = kSegmentDir + "/horizontal_write_case";
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(file_name));

    SegmentWriter writer(std::move(wfile), 0, tablet_schema, opts);
    ASSERT_OK(writer.init());

    int32_t chunk_size = config::vector_chunk_size;
    size_t num_rows = 10000;
    auto schema = ChunkHelper::convert_schema(tablet_schema);
    auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
    for (auto i = 0; i < num_rows % chunk_size; ++i) {
        chunk->reset();
        auto cols = chunk->columns();
        for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
            cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j)));
            cols[1]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            cols[2]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 2)));
            cols[3]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 3)));
        }
        ASSERT_OK(writer.append_chunk(*chunk));
    }

    uint64_t file_size = 0;
    uint64_t index_size;
    uint64_t footer_position;
    ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));

    auto segment = *Segment::open(_fs, FileInfo{file_name}, 0, tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    SegmentReadOptions seg_options;
    seg_options.fs = _fs;
    OlapReaderStatistics stats;
    seg_options.stats = &stats;
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    const auto& seg_iterator = res.value();

    size_t count = 0;
    while (true) {
        chunk->reset();
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(count + 2, chunk->get(i)[2].get_int32());
            EXPECT_EQ(count + 3, chunk->get(i)[3].get_int32());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);

    // Test new_column_iterator
    {
        TabletColumn column;
        column.set_unique_id(5);
        column.set_type(LogicalType::TYPE_BIGINT);
        column.set_is_nullable(false);
        auto r = segment->new_column_iterator(column, nullptr);
        ASSERT_FALSE(r.ok());
        ASSERT_TRUE(r.status().is_not_found()) << r.status();
    }
    // Test new_column_iterator_or_default
    {
        TabletColumn column;
        column.set_unique_id(5);
        column.set_type(LogicalType::TYPE_BIGINT);
        column.set_is_nullable(false);

        auto r = segment->new_column_iterator_or_default(column, nullptr);
        ASSERT_FALSE(r.ok());

        column.set_is_nullable(true);
        r = segment->new_column_iterator_or_default(column, nullptr);
        ASSERT_TRUE(r.ok()) << r.status();

        column.set_is_nullable(false);
        column.set_default_value("10");
        r = segment->new_column_iterator_or_default(column, nullptr);
        ASSERT_TRUE(r.ok()) << r.status();
    }
    // test new_dcg_segment
    {
        auto ep = KeyCache::instance().create_plain_random_encryption_meta_pair().value();
        DeltaColumnGroup dcg;
        dcg.init(1, {{1}}, {"abc0.cols"}, {ep.encryption_meta});
        auto r = segment->new_dcg_segment(dcg, 0, nullptr);
        ASSERT_FALSE(r.ok());
    }
}

// NOLINTNEXTLINE
TEST_F(SegmentReaderWriterTest, TestVerticalWrite) {
    std::shared_ptr<TabletSchema> tablet_schema = TabletSchemaHelper::create_tablet_schema(
            {create_int_key_pb(1), create_int_key_pb(2), create_int_value_pb(3), create_int_value_pb(4)});

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;

    std::string file_name = kSegmentDir + "/vertical_write_case";
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(file_name));

    SegmentWriter writer(std::move(wfile), 0, tablet_schema, opts);

    int32_t chunk_size = config::vector_chunk_size;
    size_t num_rows = 10000;
    uint64_t file_size = 0;
    uint64_t index_size = 0;

    {
        // col1 col2
        std::vector<uint32_t> column_indexes{0, 1};
        ASSERT_OK(writer.init(column_indexes, true));
        auto schema = ChunkHelper::convert_schema(tablet_schema, column_indexes);
        auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j)));
                cols[1]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    {
        // col3
        std::vector<uint32_t> column_indexes{2};
        ASSERT_OK(writer.init(column_indexes, false));
        auto schema = ChunkHelper::convert_schema(tablet_schema, column_indexes);
        auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 2)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    {
        // col4
        std::vector<uint32_t> column_indexes{3};
        ASSERT_OK(writer.init(column_indexes, false));
        auto schema = ChunkHelper::convert_schema(tablet_schema, column_indexes);
        auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 3)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    ASSERT_OK(writer.finalize_footer(&file_size));

    auto segment = *Segment::open(_fs, FileInfo{file_name}, 0, tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    SegmentReadOptions seg_options;
    seg_options.fs = _fs;
    OlapReaderStatistics stats;
    seg_options.stats = &stats;
    auto schema = ChunkHelper::convert_schema(tablet_schema);
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    const auto& seg_iterator = res.value();

    size_t count = 0;
    auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
    while (true) {
        chunk->reset();
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(count + 2, chunk->get(i)[2].get_int32());
            EXPECT_EQ(count + 3, chunk->get(i)[3].get_int32());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);
}

TEST_F(SegmentReaderWriterTest, TestReadMultipleTypesColumn) {
    std::string s1("abcdefghijklmnopqrstuvwxyz");
    std::string s2("bbcdefghijklmnopqrstuvwxyz");
    std::string s3("cbcdefghijklmnopqrstuvwxyz");
    std::string s4("dbcdefghijklmnopqrstuvwxyz");
    std::string s5("ebcdefghijklmnopqrstuvwxyz");
    std::string s6("fbcdefghijklmnopqrstuvwxyz");
    std::string s7("gbcdefghijklmnopqrstuvwxyz");
    std::string s8("hbcdefghijklmnopqrstuvwxyz");
    std::vector<Slice> data_strs{s1, s2, s3, s4, s5, s6, s7, s8};

    ColumnPB c1 = create_int_key_pb(1);
    ColumnPB c2 = create_int_key_pb(2);
    ColumnPB c3 = create_with_default_value_pb("VARCHAR", "");
    c3.set_length(65535);

    std::shared_ptr<TabletSchema> tablet_schema = TabletSchemaHelper::create_tablet_schema({c1, c2, c3});

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;

    std::string file_name = kSegmentDir + "/read_multiple_types_column";
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(file_name));

    SegmentWriter writer(std::move(wfile), 0, tablet_schema, opts);

    int32_t chunk_size = config::vector_chunk_size;
    size_t num_rows = 10000;
    uint64_t file_size = 0;
    uint64_t index_size = 0;

    {
        // col1 col2
        std::vector<uint32_t> column_indexes{0, 1};
        ASSERT_OK(writer.init(column_indexes, true));
        auto schema = ChunkHelper::convert_schema(tablet_schema, column_indexes);
        auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j)));
                cols[1]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    {
        // col3
        std::vector<uint32_t> column_indexes{2};
        ASSERT_OK(writer.init(column_indexes, false));
        auto schema = ChunkHelper::convert_schema(tablet_schema, column_indexes);
        auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->as_mutable_ptr()->append_datum(Datum(data_strs[j % 8]));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    ASSERT_OK(writer.finalize_footer(&file_size));
    auto segment = *Segment::open(_fs, FileInfo{file_name}, 0, tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    SegmentReadOptions seg_options;
    seg_options.fs = _fs;
    OlapReaderStatistics stats;
    seg_options.stats = &stats;
    auto schema = ChunkHelper::convert_schema(tablet_schema);
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    const auto& seg_iterator = res.value();

    size_t count = 0;
    auto chunk = ChunkFactory::new_chunk(schema, chunk_size);
    while (true) {
        chunk->reset();
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(data_strs[i % 8].to_string(), chunk->get(i)[2].get_slice().to_string());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);
}

TEST_F(SegmentReaderWriterTest, TestTypeConversion) {
    auto tablet_schema = std::shared_ptr<TabletSchema>{
            TabletSchemaHelper::create_tablet_schema({create_int_key_pb(0), create_int_value_pb(1)})};
    auto opts = SegmentWriterOptions{};
    opts.num_rows_per_block = 10;
    auto file_name = kSegmentDir + "/type_conversion_cast";
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(file_name));

    SegmentWriter writer(std::move(wfile), 0, tablet_schema, opts);
    ASSERT_OK(writer.init());

    auto chunk_size = std::max<int32_t>(10, config::vector_chunk_size);
    auto num_rows = chunk_size * 2;
    auto write_schema = ChunkHelper::convert_schema(tablet_schema);
    auto chunk = ChunkFactory::new_chunk(write_schema, chunk_size);
    for (auto i = 0; i < num_rows / chunk_size; ++i) {
        chunk->reset();
        auto cols = chunk->columns();
        for (auto j = 0; j < chunk_size; ++j) {
            cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j)));
            cols[1]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
        }
        ASSERT_OK(writer.append_chunk(*chunk));
    }

    auto file_size = uint64_t{0};
    auto index_size = uint64_t{0};
    auto footer_position = uint64_t{0};
    ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));

    auto segment = *Segment::open(_fs, FileInfo{file_name}, 0, tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    auto tablet_schema_for_read = std::shared_ptr<TabletSchema>{
            TabletSchemaHelper::create_tablet_schema({create_int_key_pb(0), create_bigint_value_pb(1)})};
    auto read_schema = ChunkHelper::convert_schema(tablet_schema_for_read);
    // full scan
    {
        auto count = 0;
        auto c1_type_info = get_type_info(LogicalType::TYPE_BIGINT);
        auto stats = OlapReaderStatistics{};
        auto seg_options = SegmentReadOptions{};
        auto read_chunk = ChunkFactory::new_chunk(read_schema, chunk_size);
        seg_options.fs = _fs;
        seg_options.stats = &stats;
        seg_options.tablet_schema = tablet_schema_for_read;
        ASSIGN_OR_ABORT(auto seg_iter, segment->new_iterator(read_schema, seg_options));
        while (true) {
            read_chunk->reset();
            auto st = seg_iter->get_next(read_chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            ASSERT_OK(st);
            for (auto i = 0; i < read_chunk->num_rows(); ++i) {
                EXPECT_EQ(count, read_chunk->get(i)[0].get_int32());
                EXPECT_EQ(count + 1, read_chunk->get(i)[1].get_int64());
                ++count;
            }
        }
        EXPECT_EQ(count, num_rows);
    }
    // With predicate
    {
        auto c1_type_info = get_type_info(LogicalType::TYPE_BIGINT);
        auto predicate = new_column_eq_predicate(c1_type_info, 1, "10");
        auto guard = std::unique_ptr<ColumnPredicate>{predicate};
        auto pred_root = PredicateAndNode{};
        pred_root.add_child(PredicateColumnNode{predicate});
        auto stats = OlapReaderStatistics{};
        auto seg_options = SegmentReadOptions{};
        auto read_chunk = ChunkFactory::new_chunk(read_schema, chunk_size);
        seg_options.fs = _fs;
        seg_options.stats = &stats;
        seg_options.tablet_schema = tablet_schema_for_read;
        seg_options.pred_tree = PredicateTree::create(std::move(pred_root));
        seg_options.pred_tree_for_zone_map = seg_options.pred_tree;
        ASSIGN_OR_ABORT(auto seg_iter, segment->new_iterator(read_schema, seg_options));
        ASSERT_OK(seg_iter->get_next(read_chunk.get()));
        EXPECT_EQ(1, read_chunk->num_rows());
        EXPECT_EQ(9, read_chunk->get(0)[0].get_int32());
        EXPECT_EQ(10, read_chunk->get(0)[1].get_int64());
    }
}

// A Segment builds its column readers from the schema it was constructed with, so a column that the
// construction schema omits is unreachable even when the footer carries it: the read falls back to
// the column's default value with no error. This is why any cache shared between readers must only
// ever hold Segments built from the full tablet schema -- see the `allow_segment_cache` argument in
// lake::UpdateManager::get_column_values.
TEST_F(SegmentReaderWriterTest, TestNarrowConstructionSchemaHidesColumnPresentInFooter) {
    auto full_schema = std::shared_ptr<TabletSchema>{TabletSchemaHelper::create_tablet_schema(
            {create_int_key_pb(0), create_int_value_pb(1),
             create_int_value_pb(2, "SUM", /*is_nullable=*/true, /*default_value=*/"77")})};
    // Same file, opened through a schema that stops at column 1.
    auto narrow_schema = std::shared_ptr<TabletSchema>{
            TabletSchemaHelper::create_tablet_schema({create_int_key_pb(0), create_int_value_pb(1)})};

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;
    constexpr size_t kNumRows = 20;

    // DefaultIntGenerator writes column c of row r as r * 10 + c, so column 2 holds 2, 12, 22 -- well
    // away from the default value the read must not fall back to.
    std::shared_ptr<Segment> narrow_segment;
    build_segment(opts, full_schema, narrow_schema, kNumRows, DefaultIntGenerator, &narrow_segment);

    ColumnIteratorOptions iter_opts;
    OlapReaderStatistics stats;
    iter_opts.stats = &stats;
    ASSIGN_OR_ABORT(auto read_file, _fs->new_random_access_file(narrow_segment->file_name()));
    iter_opts.read_file = read_file.get();
    const std::vector<rowid_t> rowids{0, 1, 2};

    auto read_column_2 = [&](Segment* segment) {
        auto read_schema = ChunkHelper::convert_schema(full_schema, std::vector<ColumnId>{2});
        auto values = ChunkFactory::column_from_field(*read_schema.field(0).get())->clone_empty();
        auto iter = segment->new_column_iterator_or_default(full_schema->column(2), nullptr);
        CHECK(iter.ok()) << iter.status();
        CHECK(iter.value()->init(iter_opts).ok());
        CHECK(iter.value()->fetch_values_by_rowid(rowids.data(), rowids.size(), values.get()).ok());
        std::vector<int32_t> out;
        for (size_t i = 0; i < values->size(); ++i) {
            out.push_back(values->get(i).get_int32());
        }
        return out;
    };

    // Column 1 is in the construction schema, so it reads through to the stored data.
    {
        auto read_schema = ChunkHelper::convert_schema(full_schema, std::vector<ColumnId>{1});
        auto values = ChunkFactory::column_from_field(*read_schema.field(0).get())->clone_empty();
        ASSIGN_OR_ABORT(auto iter, narrow_segment->new_column_iterator_or_default(full_schema->column(1), nullptr));
        ASSERT_OK(iter->init(iter_opts));
        ASSERT_OK(iter->fetch_values_by_rowid(rowids.data(), rowids.size(), values.get()));
        ASSERT_EQ(rowids.size(), values->size());
        for (size_t i = 0; i < rowids.size(); ++i) {
            EXPECT_EQ(static_cast<int32_t>(rowids[i] * 10 + 1), values->get(i).get_int32());
        }
    }

    // Column 2 is not, so the same file silently yields its default value instead of 2, 12, 22.
    EXPECT_EQ(std::vector<int32_t>({77, 77, 77}), read_column_2(narrow_segment.get()));

    // Opened through the full schema the same file returns the stored values.
    ASSIGN_OR_ABORT(auto full_segment, Segment::open(_fs, FileInfo{narrow_segment->file_name()}, 0, full_schema));
    EXPECT_EQ(std::vector<int32_t>({2, 12, 22}), read_column_2(full_segment.get()));
}

TEST_F(SegmentReaderWriterTest, TestCheckColumnUniqueIdUniqueness) {
    std::shared_ptr<TabletSchema> tablet_schema = TabletSchemaHelper::create_tablet_schema();

    SegmentWriterOptions opts;
    shared_ptr<Segment> res;

    PFailPointTriggerMode trigger_mode;
    trigger_mode.set_mode(FailPointTriggerModeType::ENABLE);
    auto fp = starrocks::failpoint::FailPointRegistry::GetInstance()->get("ingest_duplicate_column_unique_id");
    fp->setMode(trigger_mode);
    // The failpoint is process-global, so turn it back off on every exit path: left enabled, it makes
    // every later Segment::open() in the test binary fail with "Duplicate column id found in tablet
    // schema".
    DeferOp disable_fp([&]() {
        PFailPointTriggerMode disable_mode;
        disable_mode.set_mode(FailPointTriggerModeType::DISABLE);
        fp->setMode(disable_mode);
    });

    static int seg_id = 0;
    std::string filename = strings::Substitute("$0/seg_$1.dat", kSegmentDir, seg_id++);
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
    SegmentWriter writer(std::move(wfile), 0, tablet_schema, opts);
    ASSERT_OK(writer.init());

    auto schema = ChunkHelper::convert_schema(tablet_schema);
    auto chunk = ChunkFactory::new_chunk(schema, 100);
    for (size_t rid = 0; rid < 100; ++rid) {
        auto cols = chunk->columns();
        for (int cid = 0; cid < tablet_schema->num_columns(); ++cid) {
            int row_block_id = rid / opts.num_rows_per_block;
            cols[cid]->as_mutable_ptr()->append_datum(DefaultIntGenerator(rid, cid, row_block_id));
        }
    }
    ASSERT_OK(writer.append_chunk(*chunk));

    uint64_t file_size, index_size, footer_position;
    ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));
    auto result = Segment::open(_fs, FileInfo{filename}, 0, tablet_schema);
    ASSERT_TRUE(!result.ok());

    const auto& st = result.status();
    EXPECT_EQ(st.code(), TStatusCode::INTERNAL_ERROR)
            << "Expected InternalError, got: " << st.code() << ", message: " << st.message();
    EXPECT_EQ(st.message(), "Duplicate column id found in tablet schema")
            << "Error message should indicate duplicate column id in tablet schema.";
}

TEST_F(SegmentReaderWriterTest, TestOffsetOnlyVarcharScan) {
    constexpr size_t kNumRows = 2048;

    TabletSchemaPB pb;
    pb.set_keys_type(DUP_KEYS);
    pb.set_num_short_key_columns(1);
    auto* c0 = pb.add_column();
    c0->set_unique_id(1);
    c0->set_name("counter_id");
    c0->set_type("INT");
    c0->set_is_key(true);
    c0->set_is_nullable(false);
    c0->set_length(4);
    c0->set_index_length(4);
    c0->set_aggregation("NONE");

    auto* c1 = pb.add_column();
    c1->set_unique_id(2);
    c1->set_name("url");
    c1->set_type("VARCHAR");
    c1->set_is_key(false);
    c1->set_is_nullable(false);
    c1->set_length(65535);
    c1->set_aggregation("NONE");

    auto tablet_schema = TabletSchema::create(pb);
    auto schema = ChunkHelper::convert_schema(tablet_schema);

    static int seg_id = 0;
    std::string filename = strings::Substitute("$0/offset_only_$1.dat", kSegmentDir, seg_id++);
    ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
    SegmentWriterOptions writer_opts;
    SegmentWriter writer(std::move(wfile), 0, tablet_schema, writer_opts);
    ASSERT_OK(writer.init());

    std::vector<std::string> urls(kNumRows);
    std::vector<int32_t> expected_counters;
    std::vector<uint32_t> expected_lengths;
    auto write_chunk = ChunkFactory::new_chunk(schema, kNumRows);
    for (size_t rid = 0; rid < kNumRows; ++rid) {
        int32_t counter_id = static_cast<int32_t>(rid);
        if (rid % 10 == 0) {
            urls[rid] = "";
        } else {
            urls[rid] = "https://starrocks.io/clickbench/q27/row_" + std::to_string(rid) + "_" +
                        std::string(16 + (rid % 48), 'u');
            expected_counters.push_back(counter_id);
            expected_lengths.push_back(static_cast<uint32_t>(urls[rid].size()));
        }
        auto cols = write_chunk->columns();
        cols[0]->as_mutable_ptr()->append_datum(Datum(counter_id));
        cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(urls[rid])));
    }
    ASSERT_OK(writer.append_chunk(*write_chunk));
    uint64_t file_size = 0, index_size = 0, footer_position = 0;
    ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));

    ASSIGN_OR_ABORT(auto segment, Segment::open(_fs, FileInfo{filename}, 0, tablet_schema));
    ASSERT_EQ(kNumRows, segment->num_rows());

    // Case 1: URL is projected as an offset-only column (e.g. SELECT CounterID, AVG(length(URL)) WHERE URL <> '')
    {
        std::unique_ptr<ColumnPredicate> ne_empty_pred(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 1, ""));
        PredicateAndNode root;
        root.add_child(PredicateColumnNode{ne_empty_pred.get()});

        SegmentReadOptions seg_options;
        seg_options.fs = _fs;
        OlapReaderStatistics stats;
        seg_options.stats = &stats;
        seg_options.pred_tree_for_zone_map = PredicateTree::create(std::move(root));
        PredicateAndNode eval_root;
        eval_root.add_child(PredicateColumnNode{ne_empty_pred.get()});
        seg_options.pred_tree = PredicateTree::create(std::move(eval_root));

        std::unordered_set<uint32_t> offset_only_cids = {1};
        seg_options.offset_only_column_ids = &offset_only_cids;

        ASSIGN_OR_ABORT(auto seg_iter, segment->new_iterator(schema, seg_options));
        auto read_chunk = ChunkFactory::new_chunk(schema, 512);

        std::vector<int32_t> actual_counters;
        std::vector<uint32_t> actual_lengths;
        while (true) {
            read_chunk->reset();
            Status st = seg_iter->get_next(read_chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            ASSERT_OK(st);
            auto* counter_col = down_cast<const Int32Column*>(read_chunk->get_column_by_index(0).get());
            auto* url_col = ColumnHelper::get_binary_column(read_chunk->get_column_raw_ptr_by_index(1));
            ASSERT_TRUE(url_col->is_offset_only());
            ASSERT_EQ(0u, url_col->get_immutable_bytes().size());

            const auto& offsets = url_col->get_offset();
            for (size_t i = 0; i < read_chunk->num_rows(); ++i) {
                actual_counters.push_back(counter_col->get_data()[i]);
                actual_lengths.push_back(offsets[i + 1] - offsets[i]);
            }
        }
        seg_iter->close();

        ASSERT_EQ(expected_counters, actual_counters);
        ASSERT_EQ(expected_lengths, actual_lengths);
    }

    // Case 2: URL is in unused_output_column_ids (e.g. SELECT CounterID, COUNT(*) WHERE URL <> '')
    {
        std::unique_ptr<ColumnPredicate> ne_empty_pred(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 1, ""));
        PredicateAndNode eval_root;
        eval_root.add_child(PredicateColumnNode{ne_empty_pred.get()});

        SegmentReadOptions seg_options;
        seg_options.fs = _fs;
        OlapReaderStatistics stats;
        seg_options.stats = &stats;
        seg_options.pred_tree = PredicateTree::create(std::move(eval_root));

        std::unordered_set<uint32_t> unused_cids = {1};
        seg_options.unused_output_column_ids = &unused_cids;

        ASSIGN_OR_ABORT(auto seg_iter, segment->new_iterator(schema, seg_options));
        auto read_chunk = ChunkFactory::new_chunk(schema, 512);

        std::vector<int32_t> actual_counters;
        while (true) {
            read_chunk->reset();
            Status st = seg_iter->get_next(read_chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            ASSERT_OK(st);
            auto* counter_col = down_cast<const Int32Column*>(read_chunk->get_column_by_index(0).get());
            for (size_t i = 0; i < read_chunk->num_rows(); ++i) {
                actual_counters.push_back(counter_col->get_data()[i]);
            }
        }
        seg_iter->close();

        ASSERT_EQ(expected_counters, actual_counters);
    }
}

TEST_F(SegmentReaderWriterTest, TestOffsetOnlyRecycledChunkAcrossSegments) {
    constexpr size_t kNumRows = 1200;

    TabletSchemaPB pb;
    pb.set_keys_type(DUP_KEYS);
    pb.set_num_short_key_columns(1);
    auto* c0 = pb.add_column();
    c0->set_unique_id(1);
    c0->set_name("counter_id");
    c0->set_type("INT");
    c0->set_is_key(true);
    c0->set_is_nullable(false);
    c0->set_length(4);
    c0->set_index_length(4);
    c0->set_aggregation("NONE");

    auto* c1 = pb.add_column();
    c1->set_unique_id(2);
    c1->set_name("url");
    c1->set_type("VARCHAR");
    c1->set_is_key(false);
    c1->set_is_nullable(false);
    c1->set_length(65535);
    c1->set_aggregation("NONE");

    auto tablet_schema = TabletSchema::create(pb);
    auto schema = ChunkHelper::convert_schema(tablet_schema);

    auto write_segment = [&](int seg_idx, std::vector<std::string>* urls_out) -> std::shared_ptr<Segment> {
        static int seg_id = 0;
        std::string filename = strings::Substitute("$0/offset_only_recycled_$1_$2.dat", kSegmentDir, seg_idx, seg_id++);
        ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
        SegmentWriterOptions writer_opts;
        SegmentWriter writer(std::move(wfile), seg_idx, tablet_schema, writer_opts);
        CHECK_OK(writer.init());

        urls_out->resize(kNumRows);
        auto write_chunk = ChunkFactory::new_chunk(schema, kNumRows);
        for (size_t rid = 0; rid < kNumRows; ++rid) {
            int32_t counter_id = static_cast<int32_t>(rid);
            (*urls_out)[rid] = "https://starrocks.io/seg_" + std::to_string(seg_idx) + "/row_" + std::to_string(rid) +
                               "_" + std::string(16 + (rid % 32), 'x');
            auto cols = write_chunk->columns();
            cols[0]->as_mutable_ptr()->append_datum(Datum(counter_id));
            cols[1]->as_mutable_ptr()->append_datum(Datum(Slice((*urls_out)[rid])));
        }
        CHECK_OK(writer.append_chunk(*write_chunk));
        uint64_t file_size = 0, index_size = 0, footer_position = 0;
        CHECK_OK(writer.finalize(&file_size, &index_size, &footer_position));
        ASSIGN_OR_ABORT(auto segment, Segment::open(_fs, FileInfo{filename}, seg_idx, tablet_schema));
        return segment;
    };

    std::vector<std::string> urls0;
    std::vector<std::string> urls1;
    auto seg0 = write_segment(0, &urls0);
    auto seg1 = write_segment(1, &urls1);

    SegmentReadOptions seg_options0;
    seg_options0.fs = _fs;
    OlapReaderStatistics stats0;
    seg_options0.stats = &stats0;
    seg_options0.chunk_size = 512;
    std::unordered_set<uint32_t> offset_only_cids = {1};
    seg_options0.offset_only_column_ids = &offset_only_cids;
    ASSIGN_OR_ABORT(auto seg_iter0, seg0->new_iterator(schema, seg_options0));

    SegmentReadOptions seg_options1;
    seg_options1.fs = _fs;
    OlapReaderStatistics stats1;
    seg_options1.stats = &stats1;
    seg_options1.chunk_size = 512;
    seg_options1.offset_only_column_ids = nullptr;
    ASSIGN_OR_ABORT(auto seg_iter1, seg1->new_iterator(schema, seg_options1));

    auto read_chunk = ChunkFactory::new_chunk(schema, 512);

    size_t rows_seg0 = 0;
    while (true) {
        read_chunk->reset();
        Status st = seg_iter0->get_next(read_chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_OK(st);
        auto* url_col = ColumnHelper::get_binary_column(read_chunk->get_column_raw_ptr_by_index(1));
        ASSERT_TRUE(url_col->is_offset_only());
        rows_seg0 += read_chunk->num_rows();
    }
    seg_iter0->close();
    ASSERT_EQ(kNumRows, rows_seg0);

    // At the end of seg_iter0, read_chunk holds a BinaryColumn with is_offset_only() == true.
    ASSERT_TRUE(ColumnHelper::get_binary_column(read_chunk->get_column_raw_ptr_by_index(1))->is_offset_only());

    size_t rows_seg1 = 0;
    while (true) {
        read_chunk->reset();
        Status st = seg_iter1->get_next(read_chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_OK(st);
        auto* url_col = ColumnHelper::get_binary_column(read_chunk->get_column_raw_ptr_by_index(1));
        ASSERT_FALSE(url_col->is_offset_only());
        for (size_t i = 0; i < read_chunk->num_rows(); ++i) {
            ASSERT_EQ(urls1[rows_seg1 + i], url_col->get_slice(i).to_string());
        }
        rows_seg1 += read_chunk->num_rows();
    }
    seg_iter1->close();
    ASSERT_EQ(kNumRows, rows_seg1);
}

TEST_F(SegmentReaderWriterTest, TestOffsetOnlyNullableDictAndFallbackScan) {
    TabletSchemaPB pb;
    pb.set_keys_type(DUP_KEYS);
    pb.set_num_short_key_columns(1);
    auto* c0 = pb.add_column();
    c0->set_unique_id(1);
    c0->set_name("counter_id");
    c0->set_type("INT");
    c0->set_is_key(true);
    c0->set_is_nullable(false);
    c0->set_length(4);
    c0->set_index_length(4);
    c0->set_aggregation("NONE");

    auto* c1 = pb.add_column();
    c1->set_unique_id(2);
    c1->set_name("url");
    c1->set_type("VARCHAR");
    c1->set_is_key(false);
    c1->set_is_nullable(true);
    c1->set_length(65535);
    c1->set_aggregation("NONE");

    auto tablet_schema = TabletSchema::create(pb);
    auto schema = ChunkHelper::convert_schema(tablet_schema);

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

        for (size_t i = 0; i < offset_null->size(); ++i) {
            ASSERT_EQ(eager_null_flags[i], offset_null_flags[i]) << "null flag mismatch at row " << i;
            if (offset_null_flags[i] == 0) {
                uint32_t offset_len = offset_offsets[i + 1] - offset_offsets[i];
                uint32_t eager_len = eager_offsets[i + 1] - eager_offsets[i];
                ASSERT_EQ(eager_len, offset_len) << "non-null length mismatch at row " << i;
                ASSERT_EQ(eager_bin->get_slice(i).size, offset_len) << "slice size mismatch at row " << i;
            }
        }
    };

    const auto orig_speculate_size = config::dictionary_speculate_min_chunk_size;
    const auto orig_dict_page_size = config::dictionary_page_size;
    DeferOp restore_configs([&]() {
        config::dictionary_speculate_min_chunk_size = orig_speculate_size;
        config::dictionary_page_size = orig_dict_page_size;
    });

    // 1. Build a pure dictionary-encoded nullable segment (all_page_dict_encoded() == true).
    std::shared_ptr<Segment> dict_only_seg;
    constexpr size_t kDictSegRows = 256;
    {
        config::dictionary_speculate_min_chunk_size = 64;
        config::dictionary_page_size = 256 * 1024;

        static int seg_id = 0;
        std::string filename = strings::Substitute("$0/offset_only_nullable_dict_$1.dat", kSegmentDir, seg_id++);
        ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
        SegmentWriterOptions writer_opts;
        SegmentWriter writer(std::move(wfile), 0, tablet_schema, writer_opts);
        ASSERT_OK(writer.init());

        const std::vector<std::string> dict_pool = {
                "https://starrocks.io/a",
                "https://starrocks.io/beta_longer_path",
                "x",
                "https://starrocks.io/gamma/1234567890_abcdef",
        };
        auto chunk = ChunkFactory::new_chunk(schema, kDictSegRows);
        for (size_t rid = 0; rid < kDictSegRows; ++rid) {
            auto cols = chunk->columns();
            cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(rid)));
            if (rid % 7 == 0) {
                cols[1]->as_mutable_ptr()->append_datum(Datum()); // NULL
            } else if (rid % 5 == 0) {
                cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(""))); // empty
            } else {
                const std::string& val = dict_pool[rid % dict_pool.size()];
                cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(val)));
            }
        }
        ASSERT_OK(writer.append_chunk(*chunk));
        uint64_t file_size = 0, index_size = 0, footer_position = 0;
        ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));
        ASSIGN_OR_ABORT(dict_only_seg, Segment::open(_fs, FileInfo{filename}, 0, tablet_schema));
        ASSERT_EQ(kDictSegRows, dict_only_seg->num_rows());
    }

    // 2. Build a dictionary-to-plain fallback nullable segment (all_page_dict_encoded() == false).
    // Chunk 0 (64 rows) has low cardinality so StringColumnWriter selects DICT_ENCODING, and its dictionary entries
    // exceed the 64-byte dict_page_size so page 0 finishes and transitions BinaryDictPageBuilder to PLAIN_ENCODING.
    // Chunk 1 (128 rows) is then encoded in PLAIN_ENCODING on page 1.
    std::shared_ptr<Segment> fallback_seg;
    constexpr size_t kFallbackChunk0Rows = 64;
    constexpr size_t kFallbackChunk1Rows = 128;
    constexpr size_t kFallbackSegRows = kFallbackChunk0Rows + kFallbackChunk1Rows;
    {
        config::dictionary_speculate_min_chunk_size = 64;
        config::dictionary_page_size = 64;

        static int seg_id = 0;
        std::string filename = strings::Substitute("$0/offset_only_nullable_fallback_$1.dat", kSegmentDir, seg_id++);
        ASSIGN_OR_ABORT(auto wfile, _fs->new_writable_file(filename));
        SegmentWriterOptions writer_opts;
        SegmentWriter writer(std::move(wfile), 1, tablet_schema, writer_opts);
        ASSERT_OK(writer.init());

        const std::vector<std::string> page0_dict_entries = {
                "https://starrocks.io/dict_page0_entry_one_11111",
                "https://starrocks.io/dict_page0_entry_two_22222",
                "https://starrocks.io/dict_page0_entry_three_33333",
        };
        auto chunk0 = ChunkFactory::new_chunk(schema, kFallbackChunk0Rows);
        for (size_t rid = 0; rid < kFallbackChunk0Rows; ++rid) {
            auto cols = chunk0->columns();
            cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(rid)));
            if (rid % 6 == 0) {
                cols[1]->as_mutable_ptr()->append_datum(Datum()); // NULL
            } else if (rid % 5 == 0) {
                cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(""))); // empty
            } else {
                const std::string& val = page0_dict_entries[rid % page0_dict_entries.size()];
                cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(val)));
            }
        }
        ASSERT_OK(writer.append_chunk(*chunk0));

        std::vector<std::string> page1_strings(kFallbackChunk1Rows);
        auto chunk1 = ChunkFactory::new_chunk(schema, kFallbackChunk1Rows);
        for (size_t i = 0; i < kFallbackChunk1Rows; ++i) {
            size_t rid = kFallbackChunk0Rows + i;
            auto cols = chunk1->columns();
            cols[0]->as_mutable_ptr()->append_datum(Datum(static_cast<int32_t>(rid)));
            if (rid % 7 == 0) {
                cols[1]->as_mutable_ptr()->append_datum(Datum()); // NULL
            } else if (rid % 4 == 0) {
                cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(""))); // empty
            } else {
                page1_strings[i] = "https://starrocks.io/plain_fallback_row_" + std::to_string(rid) + "_" +
                                   std::string(1 + (rid % 23), 'p');
                cols[1]->as_mutable_ptr()->append_datum(Datum(Slice(page1_strings[i])));
            }
        }
        ASSERT_OK(writer.append_chunk(*chunk1));

        uint64_t file_size = 0, index_size = 0, footer_position = 0;
        ASSERT_OK(writer.finalize(&file_size, &index_size, &footer_position));
        ASSIGN_OR_ABORT(fallback_seg, Segment::open(_fs, FileInfo{filename}, 1, tablet_schema));
        ASSERT_EQ(kFallbackSegRows, fallback_seg->num_rows());
    }

    auto verify_segment = [&](const std::shared_ptr<Segment>& seg, bool expect_all_dict_encoded) {
        const size_t total_rows = seg->num_rows();

        // Check ColumnIterator encoding state and exercise direct ColumnIterator SparseRange & row-ID reads.
        {
            ColumnIteratorOptions iter_opts;
            OlapReaderStatistics stats;
            iter_opts.stats = &stats;
            iter_opts.check_dict_encoding = true;
            ASSIGN_OR_ABORT(auto read_file, _fs->new_random_access_file(seg->file_name()));
            iter_opts.read_file = read_file.get();

            ASSIGN_OR_ABORT(auto eager_col_iter, seg->new_column_iterator(tablet_schema->column(1), nullptr));
            ASSIGN_OR_ABORT(auto offset_col_iter, seg->new_column_iterator(tablet_schema->column(1), nullptr));
            ASSERT_OK(eager_col_iter->init(iter_opts));
            offset_col_iter->set_offset_only(true);
            ASSERT_OK(offset_col_iter->init(iter_opts));

            ASSERT_EQ(expect_all_dict_encoded, eager_col_iter->all_page_dict_encoded());
            ASSERT_EQ(expect_all_dict_encoded, offset_col_iter->all_page_dict_encoded());

            // Multi-range SparseRange<> with gaps crossing page boundaries.
            SparseRange<> sparse_range;
            sparse_range.add(Range<>(0, 19));
            sparse_range.add(Range<>(27, 73));
            sparse_range.add(Range<>(95, static_cast<rowid_t>(total_rows - 5)));

            auto eager_sparse_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
            auto offset_sparse_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
            ASSERT_OK(eager_col_iter->seek_to_first());
            ASSERT_OK(offset_col_iter->seek_to_first());
            ASSERT_OK(eager_col_iter->next_batch(sparse_range, eager_sparse_col.get()));
            ASSERT_OK(offset_col_iter->next_batch(sparse_range, offset_sparse_col.get()));
            compare_nullable_offset_only_vs_eager(offset_sparse_col.get(), eager_sparse_col.get());

            // Row-ID reads (fetch_values_by_rowid) spanning NULL, empty, and non-empty rows across pages.
            std::vector<rowid_t> rowids;
            for (rowid_t r = 0; r < static_cast<rowid_t>(total_rows); r += 3) {
                rowids.push_back(r);
            }
            auto eager_rowid_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
            auto offset_rowid_col = ChunkFactory::column_from_field_type(TYPE_VARCHAR, true);
            ASSERT_OK(eager_col_iter->fetch_values_by_rowid(rowids.data(), rowids.size(), eager_rowid_col.get()));
            ASSERT_OK(offset_col_iter->fetch_values_by_rowid(rowids.data(), rowids.size(), offset_rowid_col.get()));
            compare_nullable_offset_only_vs_eager(offset_rowid_col.get(), eager_rowid_col.get());
        }

        // Compare full segment scans (unfiltered and predicate-filtered) between offset-only and eager decoding.
        std::vector<std::unique_ptr<ColumnPredicate>> preds;
        preds.emplace_back(nullptr); // unfiltered
        preds.emplace_back(new_column_ne_predicate(get_type_info(TYPE_VARCHAR), 1, ""));
        preds.emplace_back(new_column_eq_predicate(get_type_info(TYPE_VARCHAR), 1, ""));
        preds.emplace_back(new_column_null_predicate(get_type_info(TYPE_VARCHAR), 1, false));
        preds.emplace_back(new_column_null_predicate(get_type_info(TYPE_VARCHAR), 1, true));
        // Predicate on counter_id (col 0) to trigger late materialization of nullable url (col 1).
        preds.emplace_back(new_column_ge_predicate(get_type_info(TYPE_INT), 0, "15"));

        struct RowRecord {
            int32_t counter_id;
            uint8_t is_null;
            uint32_t length;
            bool operator==(const RowRecord& other) const {
                return counter_id == other.counter_id && is_null == other.is_null && length == other.length;
            }
        };

        auto run_scan = [&](const ColumnPredicate* pred, bool offset_only, bool url_unused,
                            std::vector<RowRecord>* out_rows) {
            SegmentReadOptions seg_options;
            seg_options.fs = _fs;
            OlapReaderStatistics stats;
            seg_options.stats = &stats;
            seg_options.chunk_size = 37; // Force multiple chunks straddling page boundaries.

            if (pred != nullptr) {
                PredicateAndNode zone_root;
                zone_root.add_child(PredicateColumnNode{pred});
                seg_options.pred_tree_for_zone_map = PredicateTree::create(std::move(zone_root));

                PredicateAndNode eval_root;
                eval_root.add_child(PredicateColumnNode{pred});
                seg_options.pred_tree = PredicateTree::create(std::move(eval_root));
            }

            std::unordered_set<uint32_t> offset_only_cids = {1};
            if (offset_only) {
                seg_options.offset_only_column_ids = &offset_only_cids;
            }
            std::unordered_set<uint32_t> unused_cids = {1};
            if (url_unused) {
                seg_options.unused_output_column_ids = &unused_cids;
            }

            ASSIGN_OR_ABORT(auto seg_iter, seg->new_iterator(schema, seg_options));
            auto read_chunk = ChunkFactory::new_chunk(schema, 37);

            while (true) {
                read_chunk->reset();
                Status st = seg_iter->get_next(read_chunk.get());
                if (st.is_end_of_file()) {
                    break;
                }
                ASSERT_OK(st);
                read_chunk->check_or_die();

                const auto* counter_col = down_cast<const Int32Column*>(read_chunk->get_column_by_index(0).get());
                if (url_unused) {
                    for (size_t i = 0; i < read_chunk->num_rows(); ++i) {
                        out_rows->push_back(RowRecord{counter_col->get_data()[i], 0, 0});
                    }
                    continue;
                }

                const auto* url_nullable = down_cast<const NullableColumn*>(read_chunk->get_column_by_index(1).get());
                const auto* url_bin = down_cast<const BinaryColumn*>(url_nullable->data_column().get());
                const auto& null_flags = url_nullable->immutable_null_column_data();
                const auto offsets = url_bin->get_offset();

                if (offset_only) {
                    ASSERT_TRUE(url_bin->is_offset_only());
                    ASSERT_EQ(0u, url_bin->get_immutable_bytes().size());
                } else {
                    ASSERT_FALSE(url_bin->is_offset_only());
                }

                for (size_t i = 0; i < read_chunk->num_rows(); ++i) {
                    uint8_t is_null = null_flags[i];
                    uint32_t len = 0;
                    if (is_null == 0) {
                        len = offsets[i + 1] - offsets[i];
                        if (!offset_only) {
                            ASSERT_EQ(url_bin->get_slice(i).size, len);
                        }
                    }
                    out_rows->push_back(RowRecord{counter_col->get_data()[i], is_null, len});
                }
            }
            seg_iter->close();
        };

        for (const auto& pred_ptr : preds) {
            std::vector<RowRecord> eager_rows;
            std::vector<RowRecord> offset_rows;
            run_scan(pred_ptr.get(), false, false, &eager_rows);
            run_scan(pred_ptr.get(), true, false, &offset_rows);
            ASSERT_FALSE(eager_rows.empty());
            ASSERT_EQ(eager_rows.size(), offset_rows.size());
            for (size_t i = 0; i < eager_rows.size(); ++i) {
                ASSERT_EQ(eager_rows[i].counter_id, offset_rows[i].counter_id) << "row " << i;
                ASSERT_EQ(eager_rows[i].is_null, offset_rows[i].is_null) << "row " << i;
                ASSERT_EQ(eager_rows[i].length, offset_rows[i].length) << "row " << i;
            }

            if (pred_ptr != nullptr && pred_ptr->column_id() == 1) {
                std::vector<RowRecord> unused_rows;
                run_scan(pred_ptr.get(), false, true, &unused_rows);
                ASSERT_EQ(eager_rows.size(), unused_rows.size());
                for (size_t i = 0; i < eager_rows.size(); ++i) {
                    ASSERT_EQ(eager_rows[i].counter_id, unused_rows[i].counter_id) << "row " << i;
                }
            }
        }
    };

    verify_segment(dict_only_seg, true);
    verify_segment(fallback_seg, false);
}

} // namespace starrocks
