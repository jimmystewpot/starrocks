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

#include "runtime/chunk_accumulator.h"

#include <memory>
#include <vector>

#include "base/testutil/assert.h"
#include "column/binary_column.h"
#include "column/chunk.h"
#include "column/column_helper.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "gtest/gtest.h"

namespace starrocks {

namespace {

ChunkPtr make_chunk(size_t num_rows) {
    auto chunk = std::make_shared<Chunk>();
    auto column = Int32Column::create();
    column->append_default(num_rows);
    chunk->append_column(std::move(column), 0);
    return chunk;
}

ChunkPtr make_binary_chunk(size_t num_rows, Slice val, bool offset_only, bool nullable = false) {
    auto chunk = std::make_shared<Chunk>();
    auto col = BinaryColumn::create();
    col->set_offset_only(offset_only);
    for (size_t i = 0; i < num_rows; ++i) {
        col->append(val);
    }
    if (nullable) {
        auto null_col = NullColumn::create(num_rows, 0);
        chunk->append_column(NullableColumn::create(std::move(col), std::move(null_col)), 0);
    } else {
        chunk->append_column(std::move(col), 0);
    }
    return chunk;
}

} // namespace

TEST(ChunkAccumulatorTest, Accumulator) {
    constexpr size_t kDesiredSize = 4096;
    ChunkAccumulator accumulator(kDesiredSize);
    size_t input_rows = 0;
    size_t output_rows = 0;

    // push small chunks
    for (int i = 0; i < 10; i++) {
        auto chunk = make_chunk(1025);
        input_rows += 1025;

        static_cast<void>(accumulator.push(std::move(chunk)));
        if (ChunkPtr output = accumulator.pull()) {
            output_rows += output->num_rows();
            EXPECT_EQ(kDesiredSize, output->num_rows());
        }
    }

    // push large chunks
    for (int i = 0; i < 10; i++) {
        auto chunk = make_chunk(8888);
        input_rows += 8888;
        static_cast<void>(accumulator.push(std::move(chunk)));
    }

    accumulator.finalize();
    while (ChunkPtr output = accumulator.pull()) {
        EXPECT_LE(output->num_rows(), kDesiredSize);
        output_rows += output->num_rows();
    }
    EXPECT_EQ(input_rows, output_rows);

    // push empty chunks
    for (int i = 0; i < ChunkAccumulator::kAccumulateLimit; i++) {
        auto chunk = make_chunk(0);
        static_cast<void>(accumulator.push(std::move(chunk)));
    }
    EXPECT_TRUE(accumulator.reach_limit());
    auto output = accumulator.pull();
    EXPECT_EQ(nullptr, output);
    EXPECT_TRUE(accumulator.reach_limit());
}

TEST(ChunkAccumulatorTest, MixedOffsetOnlyAndMaterializedBinaryChunks) {
    constexpr size_t kDesiredSize = 4096;

    for (bool nullable : {false, true}) {
        ChunkAccumulator accumulator(kDesiredSize);
        ASSERT_OK(accumulator.push(make_binary_chunk(10, Slice("hello"), /*offset_only=*/false, nullable)));
        ASSERT_OK(accumulator.push(make_binary_chunk(15, Slice("world"), /*offset_only=*/true, nullable)));
        ASSERT_OK(accumulator.push(make_binary_chunk(20, Slice("world"), /*offset_only=*/false, nullable)));
        accumulator.finalize();

        std::vector<ChunkPtr> outputs;
        size_t total_rows = 0;
        while (ChunkPtr out = accumulator.pull()) {
            out->check_or_die();
            total_rows += out->num_rows();
            outputs.push_back(std::move(out));
        }

        ASSERT_EQ(3u, outputs.size());
        EXPECT_EQ(45u, total_rows);

        const auto* col0 = ColumnHelper::get_data_column(outputs[0]->get_column_by_index(0).get());
        const auto* col1 = ColumnHelper::get_data_column(outputs[1]->get_column_by_index(0).get());
        const auto* col2 = ColumnHelper::get_data_column(outputs[2]->get_column_by_index(0).get());
        EXPECT_EQ(10u, outputs[0]->num_rows());
        EXPECT_FALSE(down_cast<const BinaryColumn*>(col0)->is_offset_only());
        EXPECT_EQ(15u, outputs[1]->num_rows());
        EXPECT_TRUE(down_cast<const BinaryColumn*>(col1)->is_offset_only());
        EXPECT_EQ(20u, outputs[2]->num_rows());
        EXPECT_FALSE(down_cast<const BinaryColumn*>(col2)->is_offset_only());
    }
}

} // namespace starrocks
