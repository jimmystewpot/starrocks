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

#include <gtest/gtest.h>
#include <streamvbyte.h>
#include <streamvbytedelta.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>

#include "base/coding.h"
#include "base/failpoint/fail_point.h"
#include "base/hash/hash_std.hpp"
#include "base/testutil/assert.h"
#include "base/testutil/parallel_test.h"
#include "base/utility/defer_op.h"
#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/chunk.h"
#include "column/chunk_extra_data.h"
#include "column/column_helper.h"
#include "column/const_column.h"
#include "column/field.h"
#include "column/fixed_length_column.h"
#include "column/json_column.h"
#include "column/map_column.h"
#include "column/nullable_column.h"
#include "column/schema.h"
#include "column/serde/column_array_serde.h"
#include "column/serde/encode_level.h"
#include "column/variant_column.h"
#include "common/config_local_io_fwd.h"
#include "common/statusor.h"
#include "gutil/strings/substitute.h"
#include "runtime/memory/mem_chunk_allocator.h"
#include "runtime/serde/chunk_encode_context.h"
#include "runtime/serde/protobuf_chunk_serde.h"
#include "types/hll.h"
#include "types/json_value.h"
#include "types/type_descriptor.h"
#include "types/variant.h"

#ifdef FIU_ENABLE
namespace starrocks {
DECLARE_FAIL_POINT(mem_chunk_allocator_allocate_fail);
}
#endif

namespace starrocks::serde {

static BinaryColumn::MutablePtr make_unrepresentable_binary_column() {
    const bool old_zero_copy = config::enable_zero_copy_from_page_cache;
    config::enable_zero_copy_from_page_cache = true;
    DeferOp restore_zero_copy([old_zero_copy] { config::enable_zero_copy_from_page_cache = old_zero_copy; });

    auto owner = std::make_shared<std::string>("x");
    ContainerResource resource(owner, owner->data(), Column::MAX_CAPACITY_LIMIT);
    BinaryColumn::Offsets offsets;
    offsets.emplace_back(0);
    offsets.emplace_back(Column::MAX_CAPACITY_LIMIT);
    return BinaryColumn::create(std::move(resource), std::move(offsets));
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, json_column) {
    auto c1 = JsonColumn::create();

    ASSERT_EQ(8, ColumnArraySerde::max_serialized_size(*c1));

    for (int i = 0; i < 10; i++) {
        JsonValue json;
        std::string json_str = strings::Substitute("{\"a\": $0}", i);
        ASSERT_TRUE(JsonValue::parse(json_str, &json).ok());
        c1->append(&json);
    }

    ASSERT_EQ(148, ColumnArraySerde::max_serialized_size(*c1));

    auto c2 = JsonColumn::create();

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_EQ(10, c2->size());
    for (size_t i = 0; i < c1->size(); i++) {
        const JsonValue* datum1 = c1->get(i).get_json();
        const JsonValue* datum2 = c2->get(i).get_json();
        std::string str1 = datum1->to_string().value();
        std::string str2 = datum2->to_string().value();
        ASSERT_EQ(str1, str2);
        ASSERT_EQ(0, datum1->compare(*datum2));
    }

    // no effect
    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1), level);
        const auto* end = buffer.data() + buffer.size();
        ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        ASSERT_EQ(buffer.data() + buffer.size(), p1);
        ASSERT_EQ(buffer.data() + buffer.size(), p2);

        ASSERT_EQ(10, c2->size());
        for (size_t i = 0; i < c1->size(); i++) {
            const JsonValue* datum1 = c1->get(i).get_json();
            const JsonValue* datum2 = c2->get(i).get_json();
            std::string str1 = datum1->to_string().value();
            std::string str2 = datum2->to_string().value();
            ASSERT_EQ(str1, str2);
            ASSERT_EQ(0, datum1->compare(*datum2));
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, variant_column) {
    auto c1 = VariantColumn::create();

    auto primitive_header = [](VariantType type) { return (static_cast<uint8_t>(type) << 2); };

    // Prepare 5 int8 variant values
    const uint8_t int8_values[][2] = {
            {primitive_header(VariantType::INT8), 0x01}, // 1
            {primitive_header(VariantType::INT8), 0x02}, // 2
            {primitive_header(VariantType::INT8), 0x03}, // 3
            {primitive_header(VariantType::INT8), 0x04}, // 4
            {primitive_header(VariantType::INT8), 0x05}, // 5
    };
    for (size_t i = 0; i < std::size(int8_values); ++i) {
        std::string_view value(reinterpret_cast<const char*>(int8_values[i]), sizeof(int8_values[i]));
        VariantRowValue variant(VariantMetadata::kEmptyMetadata, value);
        c1->append(&variant);
    }

    auto c2 = VariantColumn::create();
    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_EQ(5, c2->size());
    for (size_t i = 0; i < c1->size(); i++) {
        VariantRowValue row1;
        VariantRowValue row2;
        const VariantRowValue* datum1 = c1->get_row_value(i, &row1);
        const VariantRowValue* datum2 = c2->get_row_value(i, &row2);
        ASSERT_NE(nullptr, datum1);
        ASSERT_NE(nullptr, datum2);
        ASSERT_EQ(datum1->serialize_size(), datum2->serialize_size());
        ASSERT_EQ(datum1->get_metadata(), datum2->get_metadata());
        ASSERT_EQ(datum1->get_value(), datum2->get_value());
        EXPECT_EQ(datum1->to_string(), datum2->to_string());
    }

    // no effect
    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1), level);
        const auto* end = buffer.data() + buffer.size();
        ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        ASSERT_EQ(buffer.data() + buffer.size(), p1);
        ASSERT_EQ(buffer.data() + buffer.size(), p2);

        ASSERT_EQ(5, c2->size());
        for (size_t i = 0; i < c1->size(); i++) {
            VariantRowValue row1;
            VariantRowValue row2;
            const VariantRowValue* datum1 = c1->get_row_value(i, &row1);
            const VariantRowValue* datum2 = c2->get_row_value(i, &row2);
            ASSERT_NE(nullptr, datum1);
            ASSERT_NE(nullptr, datum2);
            ASSERT_EQ(datum1->serialize_size(), datum2->serialize_size());
            ASSERT_EQ(datum1->get_metadata(), datum2->get_metadata());
            ASSERT_EQ(datum1->get_value(), datum2->get_value());
            EXPECT_EQ(datum1->to_string(), datum2->to_string());
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_shredded_complex_type_descriptor_roundtrip) {
    auto c1 = VariantColumn::create();
    TypeDescriptor struct_type = TypeDescriptor::create_struct_type({"k"}, {TYPE_INT_DESC});
    struct_type.field_ids = {7};
    struct_type.field_physical_names = {"k_phys"};

    MutableColumns typed;
    typed.emplace_back(ColumnHelper::create_column(struct_type, true));
    c1->set_shredded_columns({"obj"}, {struct_type}, std::move(typed), nullptr, nullptr);
    ASSERT_EQ(0, c1->size());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    auto c2 = VariantColumn::create();
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), buffer.data() + buffer.size(), c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_EQ(1u, c2->shredded_paths().size());
    ASSERT_EQ("obj", c2->shredded_paths()[0]);
    ASSERT_EQ(1u, c2->shredded_types().size());
    const TypeDescriptor& out = c2->shredded_types()[0];
    ASSERT_EQ(TYPE_STRUCT, out.type);
    ASSERT_EQ(1u, out.children.size());
    ASSERT_EQ(TYPE_INT, out.children[0].type);
    ASSERT_EQ(1u, out.field_names.size());
    ASSERT_EQ("k", out.field_names[0]);
    ASSERT_EQ(1u, out.field_ids.size());
    ASSERT_EQ(7, out.field_ids[0]);
    ASSERT_EQ(1u, out.field_physical_names.size());
    ASSERT_EQ("k_phys", out.field_physical_names[0]);
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_shredded_base_only_without_typed_columns) {
    auto c1 = VariantColumn::create();
    auto metadata = BinaryColumn::create();
    auto remain = BinaryColumn::create();

    metadata->append(Slice("m0", 2));
    metadata->append(Slice("m1", 2));
    remain->append(Slice("r0", 2));
    remain->append(Slice("r1", 2));
    c1->set_shredded_columns({}, {}, {}, std::move(metadata), std::move(remain));
    ASSERT_EQ(2, c1->size());
    ASSERT_TRUE(c1->typed_columns().empty());
    ASSERT_TRUE(c1->has_metadata_column());
    ASSERT_TRUE(c1->has_remain_value());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    auto c2 = VariantColumn::create();
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), buffer.data() + buffer.size(), c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_EQ(2, c2->size());
    ASSERT_TRUE(c2->typed_columns().empty());
    ASSERT_TRUE(c2->has_metadata_column());
    ASSERT_TRUE(c2->has_remain_value());
    ASSERT_TRUE(c2->shredded_paths().empty());
    ASSERT_TRUE(c2->shredded_types().empty());
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(c1->metadata_column()->get(i).get_slice(), c2->metadata_column()->get(i).get_slice());
        ASSERT_EQ(c1->remain_value_column()->get(i).get_slice(), c2->remain_value_column()->get(i).get_slice());
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_shredded_count_mismatch_corruption) {
    auto c1 = VariantColumn::create();
    MutableColumns typed;
    typed.emplace_back(ColumnHelper::create_column(TYPE_BIGINT_DESC, true));
    c1->set_shredded_columns({"a"}, {TYPE_BIGINT_DESC}, std::move(typed), nullptr, nullptr);

    auto write_u32 = [](uint8_t* p, uint32_t v) { encode_fixed32_le(p, v); };
    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    // Layout for one short path "a":
    // [num_paths:4][path_len:4][path:1][num_types:4][type_desc:32][num_typed_cols:4]...
    constexpr size_t kNumTypesOffset = 4 + 4 + 1;
    constexpr size_t kNumTypedColsOffset = kNumTypesOffset + 4 + 32;

    // num_types mismatch
    {
        auto corrupted = buffer;
        write_u32(corrupted.data() + kNumTypesOffset, 0);
        auto c2 = VariantColumn::create();
        auto st = ColumnArraySerde::deserialize(corrupted.data(), corrupted.data() + corrupted.size(), c2.get());
        ASSERT_FALSE(st.ok());
        ASSERT_TRUE(st.status().is_corruption());
    }

    // num_typed_cols mismatch
    {
        auto corrupted = buffer;
        write_u32(corrupted.data() + kNumTypedColsOffset, 0);
        auto c2 = VariantColumn::create();
        auto st = ColumnArraySerde::deserialize(corrupted.data(), corrupted.data() + corrupted.size(), c2.get());
        ASSERT_FALSE(st.ok());
        ASSERT_TRUE(st.status().is_corruption());
    }
}

#if !DCHECK_IS_ON()
// We have DCHECK inside VariantColumn deserialize to check version,
// so this test case is only enabled when DCHECK is off.

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_failed_deserialize) {
    auto c1 = VariantColumn::create();

    // Prepare a variant value with an unsupported version
    constexpr uint8_t v2_metadata_charts[] = {0x02, 0x00, 0x00};
    const std::string_view v2_metadata(reinterpret_cast<const char*>(v2_metadata_charts), sizeof(v2_metadata_charts));
    const VariantRowValue variant(v2_metadata, "");
    c1->append(&variant);
    ASSERT_EQ(1, c1->size());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data()));

    auto c2 = VariantColumn::create();
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    ASSERT_EQ(1, c2->size());
    ASSERT_TRUE(c2->has_metadata_column());
    ASSERT_TRUE(c2->has_remain_value());
    Slice meta_slice = c2->metadata_column()->get(0).get_slice();
    Slice remain_slice = c2->remain_value_column()->get(0).get_slice();
    ASSERT_EQ(Slice(v2_metadata.data(), v2_metadata.size()), meta_slice);
    ASSERT_EQ(0, remain_slice.size);
}
#endif

#ifdef FIU_ENABLE

// NOLINTNEXTLINE
// Shredded variant: typed-only (no metadata/remain columns)
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_shredded_typed_only) {
    auto c1 = VariantColumn::create();
    {
        auto data = Int64Column::create();
        auto nulls = NullColumn::create();
        for (auto [v, n] : std::initializer_list<std::pair<int64_t, uint8_t>>{{10, 0}, {20, 1}, {30, 0}}) {
            data->append(v);
            nulls->append(n);
        }
        MutableColumns typed;
        typed.emplace_back(NullableColumn::create(std::move(data), std::move(nulls)));
        c1->set_shredded_columns({"a"}, {TYPE_BIGINT_DESC}, std::move(typed), nullptr, nullptr);
    }
    ASSERT_EQ(3, c1->size());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    auto c2 = VariantColumn::create();
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), buffer.data() + buffer.size(), c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_FALSE(c2->has_metadata_column());
    ASSERT_FALSE(c2->has_remain_value());
    ASSERT_EQ(3, c2->size());
    ASSERT_EQ(1u, c2->shredded_paths().size());
    ASSERT_EQ("a", c2->shredded_paths()[0]);
    ASSERT_EQ(TYPE_BIGINT, c2->shredded_types()[0].type);
    ASSERT_EQ(0, c2->find_shredded_path("a"));
    ASSERT_EQ(-1, c2->find_shredded_path("not_exists"));

    const auto& tc = c2->typed_columns()[0];
    ASSERT_EQ(3, tc->size());
    // row 0: value=10, non-null
    ASSERT_FALSE(tc->is_null(0));
    ASSERT_EQ(10, tc->get(0).get_int64());
    // row 1: null
    ASSERT_TRUE(tc->is_null(1));
    // row 2: value=30, non-null
    ASSERT_FALSE(tc->is_null(2));
    ASSERT_EQ(30, tc->get(2).get_int64());
}

// NOLINTNEXTLINE
// Shredded variant: base_shredded (typed + metadata + remain)
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_shredded_base_shredded) {
    auto c1 = VariantColumn::create();
    {
        auto metadata = BinaryColumn::create();
        auto remain = BinaryColumn::create();
        // 3 rows of simple binary payloads
        for (int i = 0; i < 3; ++i) {
            std::string m(1, static_cast<char>('m' + i));
            std::string r(1, static_cast<char>('r' + i));
            metadata->append(Slice(m.data(), m.size()));
            remain->append(Slice(r.data(), r.size()));
        }

        auto data = Int64Column::create();
        auto nulls = NullColumn::create();
        data->append(100);
        nulls->append(0);
        data->append(200);
        nulls->append(0);
        data->append(300);
        nulls->append(0);

        MutableColumns typed;
        typed.emplace_back(NullableColumn::create(std::move(data), std::move(nulls)));
        c1->set_shredded_columns({"a"}, {TYPE_BIGINT_DESC}, std::move(typed), std::move(metadata), std::move(remain));
    }
    ASSERT_TRUE(c1->has_metadata_column());
    ASSERT_EQ(3, c1->size());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    auto c2 = VariantColumn::create();
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), buffer.data() + buffer.size(), c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_TRUE(c2->has_metadata_column());
    ASSERT_TRUE(c2->has_remain_value());
    ASSERT_EQ(3, c2->size());
    ASSERT_EQ("a", c2->shredded_paths()[0]);
    ASSERT_EQ(TYPE_BIGINT, c2->shredded_types()[0].type);
    ASSERT_EQ(3, c2->metadata_column()->size());
    ASSERT_EQ(3, c2->remain_value_column()->size());

    const auto& tc = c2->typed_columns()[0];
    ASSERT_EQ("[100, 200, 300]", tc->debug_string());

    // Verify metadata/remain content round-trips correctly
    for (int i = 0; i < 3; ++i) {
        Slice m_slice = c1->metadata_column()->get(i).get_slice();
        Slice r_slice = c1->remain_value_column()->get(i).get_slice();
        Slice m2_slice = c2->metadata_column()->get(i).get_slice();
        Slice r2_slice = c2->remain_value_column()->get(i).get_slice();
        ASSERT_EQ(m_slice, m2_slice) << "metadata mismatch at row " << i;
        ASSERT_EQ(r_slice, r2_slice) << "remain mismatch at row " << i;
    }
}

// NOLINTNEXTLINE
// Shredded variant: multiple paths with different types
PARALLEL_TEST(ColumnArraySerdeTest, variant_column_shredded_multiple_paths) {
    auto c1 = VariantColumn::create();
    {
        auto metadata = BinaryColumn::create();
        auto remain = BinaryColumn::create();
        metadata->append(Slice("meta", 4));
        metadata->append(Slice("meta", 4));
        remain->append(Slice("rval", 4));
        remain->append(Slice("rval", 4));

        // path "a": nullable BIGINT
        auto int_data = Int64Column::create();
        auto int_nulls = NullColumn::create();
        int_data->append(42);
        int_nulls->append(0);
        int_data->append(0);
        int_nulls->append(1); // null
        MutableColumnPtr int_col = NullableColumn::create(std::move(int_data), std::move(int_nulls));

        // path "b": nullable VARCHAR
        auto str_data = BinaryColumn::create();
        auto str_nulls = NullColumn::create();
        str_data->append("hello");
        str_nulls->append(0);
        str_data->append("world");
        str_nulls->append(0);
        MutableColumnPtr str_col = NullableColumn::create(std::move(str_data), std::move(str_nulls));

        MutableColumns typed;
        typed.emplace_back(std::move(int_col));
        typed.emplace_back(std::move(str_col));

        c1->set_shredded_columns({"a", "b"}, {TYPE_BIGINT_DESC, TYPE_VARCHAR_DESC}, std::move(typed),
                                 std::move(metadata), std::move(remain));
    }
    ASSERT_EQ(2, c1->size());
    ASSERT_EQ(2u, c1->shredded_paths().size());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    auto c2 = VariantColumn::create();
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), buffer.data() + buffer.size(), c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);

    ASSERT_EQ(2, c2->size());
    ASSERT_EQ(2u, c2->shredded_paths().size());
    ASSERT_EQ("a", c2->shredded_paths()[0]);
    ASSERT_EQ("b", c2->shredded_paths()[1]);
    ASSERT_EQ(TYPE_BIGINT, c2->shredded_types()[0].type);
    ASSERT_EQ(TYPE_VARCHAR, c2->shredded_types()[1].type);
    ASSERT_EQ(TypeDescriptor::MAX_VARCHAR_LENGTH, c2->shredded_types()[1].len);
    ASSERT_EQ(0, c2->find_shredded_path("a"));
    ASSERT_EQ(1, c2->find_shredded_path("b"));
    ASSERT_EQ(-1, c2->find_shredded_path("c"));
    ASSERT_EQ(2u, c2->typed_columns().size());

    // verify BIGINT column
    const auto& int_tc = c2->typed_columns()[0];
    ASSERT_FALSE(int_tc->is_null(0));
    ASSERT_EQ(42, int_tc->get(0).get_int64());
    ASSERT_TRUE(int_tc->is_null(1));

    // verify VARCHAR column
    const auto& str_tc = c2->typed_columns()[1];
    ASSERT_FALSE(str_tc->is_null(0));
    ASSERT_EQ("hello", str_tc->get(0).get_slice().to_string());
    ASSERT_FALSE(str_tc->is_null(1));
    ASSERT_EQ("world", str_tc->get(1).get_slice().to_string());
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, hll_column_failed_deserialize) {
    // The HLL registers allocator is process-global and normally installed by RuntimeEnv::init(). Install the same
    // MemChunkAllocator-backed one so the failpoint below is reachable when this case runs alone.
    HyperLogLog::RegistersAllocator allocator;
    allocator.allocate = [](size_t size, void*, MemChunk* chunk) { return MemChunkAllocator::allocate(size, chunk); };
    allocator.free = [](const MemChunk& chunk, void*) { MemChunkAllocator::free(chunk); };
    Status st = HyperLogLog::set_registers_allocator(allocator);
    ASSERT_TRUE(st.ok() || st.is_already_exist()) << st;

    auto c1 = HyperLogLogColumn::create();
    // prepare a sparse-encoded HLL (few non-zero registers)
    HyperLogLog sparse_hll;
    for (int i = 0; i < 200; ++i) {
        sparse_hll.update(HashUtil::murmur_hash64A(&i, sizeof(i), HashUtil::MURMUR_SEED));
    }
    // prepare a full-encoded HLL (many non-zero registers)
    HyperLogLog full_hll;
    for (int i = 0; i < 5000; ++i) {
        full_hll.update(HashUtil::murmur_hash64A(&i, sizeof(i), HashUtil::MURMUR_SEED));
    }
    c1->append(&sparse_hll);
    c1->append(&full_hll);
    ASSERT_EQ(2, c1->size());

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data()));

#ifdef FIU_ENABLE
    (void)::starrocks::fp_mem_chunk_allocator_allocate_fail.name();
#endif
    auto* fp = failpoint::FailPointRegistry::GetInstance()->get("mem_chunk_allocator_allocate_fail");
    ASSERT_NE(fp, nullptr);
    PFailPointTriggerMode mode;
    mode.set_mode(FailPointTriggerModeType::ENABLE);
    fp->setMode(mode);

    auto c2 = HyperLogLogColumn::create();
    ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(2, c2->size());
    for (int i = 0; i < c2->size(); ++i) {
        const HyperLogLog* h = c2->get(i).get_hyperloglog();
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(0, h->estimate_cardinality()); // should be empty after failed deserialize
    }

    mode.set_mode(FailPointTriggerModeType::DISABLE);
    fp->setMode(mode);
}
#endif

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, decimal_column) {
    auto c1 = DecimalColumn::create();

    c1->append(DecimalV2Value(1));
    c1->append(DecimalV2Value(2));
    c1->append(DecimalV2Value(3));

    ASSERT_EQ(sizeof(uint32_t) + c1->size() * sizeof(DecimalV2Value), ColumnArraySerde::max_serialized_size(*c1));

    auto c2 = DecimalColumn::create();

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    for (size_t i = 0; i < c1->size(); i++) {
        ASSERT_EQ(c1->get_data()[i], c2->get_data()[i]);
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        for (size_t i = 0; i < c1->size(); i++) {
            ASSERT_EQ(c1->get_data()[i], c2->get_data()[i]);
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, int_column) {
    std::vector<int32_t> numbers{1, 2, 3, 4, 5, 6, 7};
    auto c1 = Int32Column::create();
    auto c2 = Int32Column::create();
    c1->append_numbers(numbers.data(), numbers.size() * sizeof(int32_t));

    ASSERT_EQ(sizeof(uint32_t) + c1->size() * sizeof(int32_t), ColumnArraySerde::max_serialized_size(*c1));

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    for (size_t i = 0; i < numbers.size(); i++) {
        ASSERT_EQ(ColumnHelper::as_raw_column<FixedLengthColumn<int32_t>>(c1.get())->get_data()[i],
                  ColumnHelper::as_raw_column<FixedLengthColumn<int32_t>>(c2.get())->get_data()[i]);
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        for (size_t i = 0; i < numbers.size(); i++) {
            ASSERT_EQ(ColumnHelper::as_raw_column<FixedLengthColumn<int32_t>>(c1.get())->get_data()[i],
                      ColumnHelper::as_raw_column<FixedLengthColumn<int32_t>>(c2.get())->get_data()[i]);
        }
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), true, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), true, level));
        for (size_t i = 0; i < numbers.size(); i++) {
            ASSERT_EQ(ColumnHelper::as_raw_column<FixedLengthColumn<int32_t>>(c1.get())->get_data()[i],
                      ColumnHelper::as_raw_column<FixedLengthColumn<int32_t>>(c2.get())->get_data()[i]);
        }
    }
}

PARALLEL_TEST(ColumnArraySerdeTest, corrupted_data) {
    constexpr int encode_level = 2;
    std::vector<uint8_t> buffer;
    {
        const auto* end = buffer.data() + buffer.size();
        auto c3 = Int32Column::create();
        ASSERT_ERROR(ColumnArraySerde::deserialize(buffer.data(), end, c3.get(), false, encode_level));
    }
    {
        // insufficient data
        buffer.resize(10);
        buffer[0] = 0xFF;
        buffer[1] = 0xFF;
        buffer[2] = 0xFF;
        buffer[3] = 0xFF;
        auto c1 = Int32Column::create();
        const auto* end = buffer.data() + buffer.size();
        ASSERT_ERROR(ColumnArraySerde::deserialize(buffer.data(), end, c1.get(), false, encode_level));
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, double_column) {
    std::vector<double> numbers{1.0, 2, 3.3, 4, 5.9, 6, 7};
    auto c1 = DoubleColumn::create();
    auto c2 = DoubleColumn::create();
    c1->append_numbers(numbers.data(), numbers.size() * sizeof(double));

    ASSERT_EQ(sizeof(uint32_t) + c1->size() * sizeof(double), ColumnArraySerde::max_serialized_size(*c1));

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(end, p1);
    ASSERT_EQ(end, p2);
    for (size_t i = 0; i < numbers.size(); i++) {
        ASSERT_EQ(ColumnHelper::as_raw_column<FixedLengthColumn<double>>(c1.get())->get_data()[i],
                  ColumnHelper::as_raw_column<FixedLengthColumn<double>>(c2.get())->get_data()[i]);
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        for (size_t i = 0; i < numbers.size(); i++) {
            ASSERT_EQ(ColumnHelper::as_raw_column<FixedLengthColumn<double>>(c1.get())->get_data()[i],
                      ColumnHelper::as_raw_column<FixedLengthColumn<double>>(c2.get())->get_data()[i]);
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, nullable_int32_column) {
    std::vector<int32_t> numbers{1, 2, 3, 4, 5, 6, 7};
    auto c1 = NullableColumn::create(Int32Column::create(), NullColumn::create());
    auto c2 = NullableColumn::create(Int32Column::create(), NullColumn::create());
    c1->append_numbers(numbers.data(), numbers.size() * sizeof(int32_t));
    c1->append_nulls(2);

    ASSERT_EQ(ColumnArraySerde::max_serialized_size(*c1->null_column()) +
                      ColumnArraySerde::max_serialized_size(*c1->data_column()),
              ColumnArraySerde::max_serialized_size(*c1));

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    for (size_t i = 0; i < c1->size(); i++) {
        ASSERT_EQ(c1->is_null(i), c2->is_null(i));
        if (!c1->is_null(i)) {
            ASSERT_EQ(c1->get(i).get_int32(), c2->get(i).get_int32());
        }
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        for (size_t i = 0; i < c1->size(); i++) {
            ASSERT_EQ(c1->is_null(i), c2->is_null(i));
            if (!c1->is_null(i)) {
                ASSERT_EQ(c1->get(i).get_int32(), c2->get(i).get_int32());
            }
        }
    }
}

PARALLEL_TEST(ColumnArraySerdeTest, nullable_column_all_null_encoding) {
    constexpr size_t kRows = 100;
    constexpr int kLevel = 7 | ENCODE_ALL_NULL;

    auto all_null = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
    all_null->append_nulls(kRows);

    // Without the bit the layout is untouched: no tag, both sub-columns serialized in full.
    ASSERT_EQ(ColumnArraySerde::max_serialized_size(*all_null->null_column(), 7) +
                      ColumnArraySerde::max_serialized_size(*all_null->data_column(), 7),
              ColumnArraySerde::max_serialized_size(*all_null, 7));

    // With the bit an all-NULL column costs a tag plus the row count, whatever its length.
    const auto all_null_size = ColumnArraySerde::max_serialized_size(*all_null, kLevel);
    ASSERT_EQ(static_cast<int64_t>(sizeof(uint8_t) + sizeof(uint32_t)), all_null_size);
    ASSERT_LT(all_null_size, ColumnArraySerde::max_serialized_size(*all_null, 7));

    std::vector<uint8_t> buffer(all_null_size);
    ASSIGN_OR_ABORT(auto* write_end, ColumnArraySerde::serialize(*all_null, buffer.data(), false, kLevel));
    ASSERT_EQ(buffer.data() + buffer.size(), write_end);

    auto restored = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
    ASSIGN_OR_ABORT(auto* read_end,
                    ColumnArraySerde::deserialize(buffer.data(), write_end, restored.get(), false, kLevel));
    ASSERT_EQ(write_end, read_end);
    ASSERT_EQ(kRows, restored->size());
    ASSERT_EQ(kRows, restored->data_column()->size());
    ASSERT_TRUE(restored->has_null());
    for (size_t i = 0; i < kRows; i++) {
        ASSERT_TRUE(restored->is_null(i));
    }
}

PARALLEL_TEST(ColumnArraySerdeTest, nullable_column_all_null_encoding_keeps_mixed_values) {
    constexpr int kLevel = 7 | ENCODE_ALL_NULL;

    std::vector<Slice> strings{{"aaa"}, {"bbbb"}};
    auto mixed = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
    ASSERT_TRUE(mixed->append_strings(strings.data(), strings.size()));
    mixed->append_nulls(1);

    std::vector<uint8_t> buffer(ColumnArraySerde::max_serialized_size(*mixed, kLevel));
    ASSIGN_OR_ABORT(auto* write_end, ColumnArraySerde::serialize(*mixed, buffer.data(), false, kLevel));

    auto restored = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
    ASSIGN_OR_ABORT(auto* read_end,
                    ColumnArraySerde::deserialize(buffer.data(), write_end, restored.get(), false, kLevel));
    ASSERT_EQ(write_end, read_end);
    ASSERT_EQ(mixed->size(), restored->size());
    for (size_t i = 0; i < mixed->size(); i++) {
        ASSERT_EQ(mixed->is_null(i), restored->is_null(i));
        if (!mixed->is_null(i)) {
            ASSERT_EQ(mixed->get(i).get_slice(), restored->get(i).get_slice());
        }
    }

    // An unknown tag is rejected instead of being read as column data.
    buffer[0] = 0x7f;
    auto corrupted = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
    ASSERT_FALSE(ColumnArraySerde::deserialize(buffer.data(), write_end, corrupted.get(), false, kLevel).status().ok());
}

// _is_all_null() short-circuits on the first non-NULL row rather than counting every NULL, so the
// cases that decide the branch are: no NULL at all (has_null() false), a NULL run broken at the
// very end (worst case for the memchr), and a NULL run broken at the very start. All three must
// take the legacy layout and round-trip byte-exactly.
PARALLEL_TEST(ColumnArraySerdeTest, nullable_column_all_null_detection_edges) {
    constexpr size_t kRows = 97; // deliberately not a multiple of any SIMD width
    constexpr int kLevel = 7 | ENCODE_ALL_NULL;

    auto round_trip = [&](const NullableColumn::Ptr& column, bool expect_compact) {
        const auto size_with_bit = ColumnArraySerde::max_serialized_size(*column, kLevel);
        // A compact payload is tag + row count; anything else still carries both sub-columns.
        ASSERT_EQ(expect_compact, size_with_bit == static_cast<int64_t>(sizeof(uint8_t) + sizeof(uint32_t)));

        std::vector<uint8_t> buffer(size_with_bit);
        ASSIGN_OR_ABORT(auto* write_end, ColumnArraySerde::serialize(*column, buffer.data(), false, kLevel));
        auto restored = NullableColumn::create(Int32Column::create(), NullColumn::create());
        ASSIGN_OR_ABORT(auto* read_end,
                        ColumnArraySerde::deserialize(buffer.data(), write_end, restored.get(), false, kLevel));
        ASSERT_EQ(write_end, read_end);
        ASSERT_EQ(column->size(), restored->size());
        for (size_t i = 0; i < column->size(); i++) {
            ASSERT_EQ(column->is_null(i), restored->is_null(i)) << "row " << i;
            if (!column->is_null(i)) {
                ASSERT_EQ(column->get(i).get_int32(), restored->get(i).get_int32()) << "row " << i;
            }
        }
    };

    // No NULL anywhere: has_null() is false, so the predicate must reject it without scanning.
    auto dense = NullableColumn::create(Int32Column::create(), NullColumn::create());
    for (size_t i = 0; i < kRows; i++) {
        dense->append_datum(Datum(static_cast<int32_t>(i)));
    }
    ASSERT_FALSE(dense->has_null());
    round_trip(dense, /*expect_compact=*/false);

    // NULL everywhere except the LAST row -- the memchr has to walk the whole null column.
    auto last_set = NullableColumn::create(Int32Column::create(), NullColumn::create());
    last_set->append_nulls(kRows - 1);
    last_set->append_datum(Datum(static_cast<int32_t>(42)));
    ASSERT_TRUE(last_set->has_null());
    round_trip(last_set, /*expect_compact=*/false);

    // NULL everywhere except the FIRST row -- the common bail-immediately shape.
    auto first_set = NullableColumn::create(Int32Column::create(), NullColumn::create());
    first_set->append_datum(Datum(static_cast<int32_t>(7)));
    first_set->append_nulls(kRows - 1);
    ASSERT_TRUE(first_set->has_null());
    round_trip(first_set, /*expect_compact=*/false);

    // Genuinely all NULL: still detected, still compact.
    auto all_null = NullableColumn::create(Int32Column::create(), NullColumn::create());
    all_null->append_nulls(kRows);
    round_trip(all_null, /*expect_compact=*/true);
}

PARALLEL_TEST(EncodeContextTest, all_null_bit_survives_the_compression_ratio_check) {
    constexpr int kLevel = 7 | ENCODE_ALL_NULL;

    // A column whose payload does not shrink loses its compression bits, but must keep
    // ENCODE_ALL_NULL: that bit selects a layout, and the writer and reader agree on it through
    // the per-column level recorded in the payload header.
    auto incompressible = EncodeContext::get_encode_context_shared_ptr(1, kLevel);
    for (int i = 0; i < 5; i++) {
        incompressible->update(0, 1000, 1000);
        incompressible->adjust_encode_levels();
    }
    ASSERT_EQ(ENCODE_ALL_NULL, incompressible->get_encode_level(0));

    auto compressible = EncodeContext::get_encode_context_shared_ptr(1, kLevel);
    for (int i = 0; i < 5; i++) {
        compressible->update(0, 1000, 100);
        compressible->adjust_encode_levels();
    }
    ASSERT_EQ(kLevel, compressible->get_encode_level(0));
}
// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, binary_column) {
    std::vector<Slice> strings{{"bbb"}, {"bbc"}, {"ccc"}};
    auto c1 = BinaryColumn::create();
    auto c2 = BinaryColumn::create();
    c1->append_strings(strings.data(), strings.size());

    ASSERT_EQ(c1->get_immutable_bytes().size() + c1->get_offset().size() * sizeof(uint32_t) + sizeof(uint32_t) * 2,
              ColumnArraySerde::max_serialized_size(*c1));

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    for (size_t i = 0; i < c1->size(); i++) {
        ASSERT_EQ(c1->get_slice(i), c2->get_slice(i));
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        for (size_t i = 0; i < c1->size(); i++) {
            ASSERT_EQ(c1->get_slice(i), c2->get_slice(i));
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, binary_column_serialize_rejects_unrepresentable_payload) {
    auto column = make_unrepresentable_binary_column();
    ASSERT_FALSE(column->is_payload_size_representable().ok());

    std::vector<uint8_t> buffer(16);
    auto st = ColumnArraySerde::serialize(*column, buffer.data());
    ASSERT_FALSE(st.ok());
    ASSERT_TRUE(st.status().is_capacity_limit_exceeded()) << st.status();
    ASSERT_NE(std::string::npos, std::string(st.status().message()).find("byte payload size")) << st.status();
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, const_column) {
    auto create_const_column = [](int32_t value, size_t size) {
        auto c = Int32Column::create();
        c->append_numbers(&value, sizeof(value));
        return ConstColumn::create(std::move(c), size);
    };

    auto c1 = create_const_column(100, 10);
    auto c2 = c1->clone_empty();

    ASSERT_EQ(sizeof(uint64_t) + ColumnArraySerde::max_serialized_size(*c1->data_column()),
              ColumnArraySerde::max_serialized_size(*c1));

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2.get()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    ASSERT_EQ(c1->size(), c2->size());
    for (size_t i = 0; i < c1->size(); i++) {
        ASSERT_EQ(c1->get(i).get_int32(), c2->get(i).get_int32());
    }

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));
        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2.get(), false, level));
        for (size_t i = 0; i < c1->size(); i++) {
            ASSERT_EQ(c1->get(i).get_int32(), c2->get(i).get_int32());
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ColumnArraySerdeTest, array_column) {
    auto off1 = UInt32Column::create();
    auto elem1 = NullableColumn::create(Int32Column::create(), NullColumn ::create());
    auto c1 = ArrayColumn::create(elem1, off1);

    // insert [1, 2, 3], [4, 5, 6]
    elem1->append_datum(1);
    elem1->append_datum(2);
    elem1->append_datum(3);
    off1->append(3);

    elem1->append_datum(4);
    elem1->append_datum(5);
    elem1->append_datum(6);
    off1->append(6);

    ASSERT_EQ(ColumnArraySerde::max_serialized_size(c1->offsets()) +
                      ColumnArraySerde::max_serialized_size(c1->elements()),
              ColumnArraySerde::max_serialized_size(*c1));

    std::vector<uint8_t> buffer;
    buffer.resize(ColumnArraySerde::max_serialized_size(*c1));
    const auto* end = buffer.data() + buffer.size();
    ASSIGN_OR_ABORT(auto p1, ColumnArraySerde::serialize(*c1, buffer.data()));
    ASSERT_EQ(buffer.data() + buffer.size(), p1);

    auto off2 = UInt32Column::create();
    auto elem2 = NullableColumn::create(Int32Column::create(), NullColumn ::create());
    auto c2 = ArrayColumn::create(elem1, off2);

    ASSIGN_OR_ABORT(auto p2, ColumnArraySerde::deserialize(buffer.data(), end, c2->as_mutable_raw_ptr()));
    ASSERT_EQ(buffer.data() + buffer.size(), p2);
    ASSERT_EQ("[1,2,3]", c2->debug_item(0));
    ASSERT_EQ("[4,5,6]", c2->debug_item(1));

    for (auto level = -1; level < 8; ++level) {
        buffer.resize(ColumnArraySerde::max_serialized_size(*c1, level));
        const auto* end = buffer.data() + buffer.size();
        ASSERT_OK(ColumnArraySerde::serialize(*c1, buffer.data(), false, level));

        off2 = UInt32Column::create();
        elem2 = NullableColumn::create(Int32Column::create(), NullColumn ::create());
        c2 = ArrayColumn::create(elem1, off2);

        ASSERT_OK(ColumnArraySerde::deserialize(buffer.data(), end, c2->as_mutable_raw_ptr(), false, level));

        ASSERT_EQ("[1,2,3]", c2->debug_item(0));
        ASSERT_EQ("[4,5,6]", c2->debug_item(1));
    }
}

static Status verify_streamvbyte_serde_roundtrip(const Column& src, Column* dst, bool sorted, int encode_level) {
    std::vector<uint8_t> buffer(ColumnArraySerde::max_serialized_size(src, encode_level));
    ASSIGN_OR_RETURN(uint8_t* end_ptr, ColumnArraySerde::serialize(src, buffer.data(), sorted, encode_level));
    if (static_cast<size_t>(end_ptr - buffer.data()) > buffer.size()) {
        return Status::InternalError("serialized size exceeds max_serialized_size");
    }
    ASSIGN_OR_RETURN(const uint8_t* read_end,
                     ColumnArraySerde::deserialize(buffer.data(), end_ptr, dst, sorted, encode_level));
    if (read_end != end_ptr) {
        return Status::InternalError("deserialized end pointer does not match serialized end pointer");
    }
    return Status::OK();
}

// NOLINTNEXTLINE
TEST(ColumnArraySerdeTest, streamvbyte_int32_unsorted_and_sorted_multiblock_roundtrip) {
    const std::vector<size_t> i32_counts{64, 65, 95, 128, 257, 1024, 4099};
    const std::vector<int> encode_levels{2, 7};

    for (size_t count : i32_counts) {
        // Pattern 1: small deltas (<= 255), exercising keys == 0 16-bit prefix-sum fast path.
        auto small_delta_col = Int32Column::create();
        small_delta_col->reserve(count);
        uint32_t acc_small = 7;
        for (size_t i = 0; i < count; ++i) {
            small_delta_col->append(static_cast<int32_t>(acc_small));
            acc_small += static_cast<uint32_t>((i % 251) + 1);
        }

        // Pattern 2: multi-width deltas (1B, 2B, 3B, 4B), transitioning from keys == 0 in the first
        // 64 elements to mixed non-zero keys in subsequent blocks.
        auto mixed_delta_col = Int32Column::create();
        mixed_delta_col->reserve(count);
        uint32_t acc_mixed = 42;
        for (size_t i = 0; i < count; ++i) {
            if (i < 64) {
                acc_mixed += static_cast<uint32_t>((i % 200) + 1);
            } else {
                switch (i & 3) {
                case 0:
                    acc_mixed += 5u;
                    break;
                case 1:
                    acc_mixed += 1024u;
                    break;
                case 2:
                    acc_mixed += 131072u;
                    break;
                default:
                    acc_mixed += 20000000u;
                    break;
                }
            }
            mixed_delta_col->append(static_cast<int32_t>(acc_mixed));
        }

        for (int level : encode_levels) {
            for (bool sorted : {false, true}) {
                for (const auto* src : {small_delta_col.get(), mixed_delta_col.get()}) {
                    auto out = Int32Column::create();
                    out->resize(src->size());
                    ASSERT_TRUE(verify_streamvbyte_serde_roundtrip(*src, out.get(), sorted, level).ok())
                            << "count=" << count << " level=" << level << " sorted=" << sorted;
                    ASSERT_EQ(src->size(), out->size());
                    for (size_t i = 0; i < count; ++i) {
                        ASSERT_EQ(src->get_data()[i], out->get_data()[i])
                                << "mismatch at index " << i << " count=" << count << " level=" << level
                                << " sorted=" << sorted;
                    }
                }
            }
        }
    }

    // Int64Column across {32, 65, 1024, 4099} with negative values and large 64-bit timestamps.
    const std::vector<size_t> i64_counts{32, 65, 1024, 4099};
    for (size_t count : i64_counts) {
        auto neg_col = Int64Column::create();
        auto ts_col = Int64Column::create();
        neg_col->reserve(count);
        ts_col->reserve(count);
        for (size_t i = 0; i < count; ++i) {
            neg_col->append(-100000LL - static_cast<int64_t>(i));
            ts_col->append(1700000000000LL + static_cast<int64_t>(i) * 1000LL);
        }

        for (int level : encode_levels) {
            for (bool sorted : {false, true}) {
                for (const auto* src : {neg_col.get(), ts_col.get()}) {
                    auto out = Int64Column::create();
                    out->resize(src->size());
                    ASSERT_TRUE(verify_streamvbyte_serde_roundtrip(*src, out.get(), sorted, level).ok())
                            << "i64 count=" << count << " level=" << level << " sorted=" << sorted;
                    ASSERT_EQ(src->size(), out->size());
                    for (size_t i = 0; i < count; ++i) {
                        ASSERT_EQ(src->get_data()[i], out->get_data()[i])
                                << "i64 mismatch at index " << i << " count=" << count << " level=" << level
                                << " sorted=" << sorted;
                    }
                }
            }
        }
    }
}

// NOLINTNEXTLINE
TEST(ColumnArraySerdeTest, streamvbyte_binary_array_map_offsets_roundtrip) {
    const std::vector<int> encode_levels{2, 7};

    // 1. BinaryColumn: 256 and 1024 rows with short strings (< 256 B) and occasional strings (> 65536 B).
    for (size_t num_rows : {size_t{256}, size_t{1024}}) {
        auto bin_src = BinaryColumn::create();
        std::string large_str(70000, 'L');
        for (size_t i = 0; i < num_rows; ++i) {
            if (i == 100 || i == 200) {
                large_str[0] = static_cast<char>('A' + (i % 26));
                bin_src->append(Slice(large_str));
            } else {
                std::string short_str((i % 37) + 1, static_cast<char>('a' + (i % 26)));
                bin_src->append(Slice(short_str));
            }
        }

        for (int level : encode_levels) {
            auto bin_dst = BinaryColumn::create();
            ASSERT_TRUE(verify_streamvbyte_serde_roundtrip(*bin_src, bin_dst.get(), false, level).ok())
                    << "BinaryColumn rows=" << num_rows << " level=" << level;
            ASSERT_EQ(bin_src->size(), bin_dst->size());
            for (size_t i = 0; i < num_rows; ++i) {
                ASSERT_EQ(bin_src->get_slice(i), bin_dst->get_slice(i))
                        << "BinaryColumn mismatch at row " << i << " rows=" << num_rows << " level=" << level;
            }
        }
    }

    // 2. ArrayColumn: 256 and 1024 rows at encode_level = 2 and 7.
    for (size_t num_rows : {size_t{256}, size_t{1024}}) {
        auto offsets = UInt32Column::create();
        offsets->append(0);
        auto elements = NullableColumn::create(Int32Column::create(), NullColumn::create());
        uint32_t cur_offset = 0;
        for (size_t i = 0; i < num_rows; ++i) {
            size_t len = (i % 5);
            for (size_t j = 0; j < len; ++j) {
                if ((i + j) % 7 == 0) {
                    elements->append_nulls(1);
                } else {
                    elements->append_datum(Datum(static_cast<int32_t>(i * 10 + j)));
                }
            }
            cur_offset += static_cast<uint32_t>(len);
            offsets->append(cur_offset);
        }
        auto arr_src = ArrayColumn::create(std::move(elements), std::move(offsets));

        for (int level : encode_levels) {
            auto dst_offsets = UInt32Column::create();
            auto dst_elements = NullableColumn::create(Int32Column::create(), NullColumn::create());
            auto arr_dst = ArrayColumn::create(std::move(dst_elements), std::move(dst_offsets));
            ASSERT_TRUE(verify_streamvbyte_serde_roundtrip(*arr_src, arr_dst.get(), false, level).ok())
                    << "ArrayColumn rows=" << num_rows << " level=" << level;
            ASSERT_EQ(arr_src->size(), arr_dst->size());
            for (size_t i = 0; i < num_rows; ++i) {
                ASSERT_EQ(arr_src->debug_item(i), arr_dst->debug_item(i))
                        << "ArrayColumn mismatch at row " << i << " rows=" << num_rows << " level=" << level;
            }
        }
    }

    // 3. MapColumn: 256 rows at encode_level = 2 and 7.
    {
        constexpr size_t kMapRows = 256;
        auto offsets = UInt32Column::create();
        offsets->append(0);
        auto keys = NullableColumn::create(Int32Column::create(), NullColumn::create());
        auto values = NullableColumn::create(Int32Column::create(), NullColumn::create());
        uint32_t cur_offset = 0;
        for (size_t i = 0; i < kMapRows; ++i) {
            size_t entries = (i % 4) + 1;
            for (size_t j = 0; j < entries; ++j) {
                keys->append_datum(Datum(static_cast<int32_t>(i * 100 + j)));
                if ((i + j) % 5 == 0) {
                    values->append_nulls(1);
                } else {
                    values->append_datum(Datum(static_cast<int32_t>(i * 1000 + j)));
                }
            }
            cur_offset += static_cast<uint32_t>(entries);
            offsets->append(cur_offset);
        }
        auto map_src = MapColumn::create(std::move(keys), std::move(values), std::move(offsets));

        for (int level : encode_levels) {
            auto dst_offsets = UInt32Column::create();
            auto dst_keys = NullableColumn::create(Int32Column::create(), NullColumn::create());
            auto dst_values = NullableColumn::create(Int32Column::create(), NullColumn::create());
            auto map_dst = MapColumn::create(std::move(dst_keys), std::move(dst_values), std::move(dst_offsets));
            ASSERT_TRUE(verify_streamvbyte_serde_roundtrip(*map_src, map_dst.get(), false, level).ok())
                    << "MapColumn level=" << level;
            ASSERT_EQ(map_src->size(), map_dst->size());
            for (size_t i = 0; i < kMapRows; ++i) {
                ASSERT_EQ(map_src->debug_item(i), map_dst->debug_item(i))
                        << "MapColumn mismatch at row " << i << " level=" << level;
            }
        }
    }
}

// NOLINTNEXTLINE
TEST(ColumnArraySerdeTest, streamvbyte_golden_wire_determinism) {
    constexpr size_t kCount = 64;
    constexpr size_t kHeaderSize = sizeof(uint32_t) + sizeof(uint64_t); // raw_bytes (4B) + compressed_bytes (8B)
    constexpr int kEncodeLevel = 2;

    // Case 1: Unsorted mixed-width 64-element Int32Column producing 128 compressed bytes
    // (16 control bytes + 112 data bytes: each 4-int group has widths 1B + 1B + 2B + 3B = 7B).
    {
        auto mixed_col = Int32Column::create();
        for (size_t g = 0; g < kCount / 4; ++g) {
            mixed_col->append(static_cast<int32_t>(1 + g));         // 1 byte
            mixed_col->append(static_cast<int32_t>(100 + g));       // 1 byte
            mixed_col->append(static_cast<int32_t>(300 + g * 17));  // 2 bytes
            mixed_col->append(static_cast<int32_t>(70000 + g * 3)); // 3 bytes
        }
        std::vector<uint8_t> serde_buf(ColumnArraySerde::max_serialized_size(*mixed_col, kEncodeLevel));
        ASSIGN_OR_ABORT(uint8_t* write_end,
                        ColumnArraySerde::serialize(*mixed_col, serde_buf.data(), false, kEncodeLevel));

        std::vector<uint8_t> direct_buf(streamvbyte_max_compressedbytes(kCount));
        size_t direct_bytes =
                streamvbyte_encode(reinterpret_cast<const uint32_t*>(mixed_col->raw_data()), kCount, direct_buf.data());

        ASSERT_EQ(128u, direct_bytes);
        ASSERT_EQ(kCount * sizeof(int32_t), decode_fixed32_le(serde_buf.data()));
        ASSERT_EQ(direct_bytes, decode_fixed64_le(serde_buf.data() + sizeof(uint32_t)));
        ASSERT_EQ(kHeaderSize + direct_bytes, static_cast<size_t>(write_end - serde_buf.data()));
        ASSERT_EQ(0, std::memcmp(serde_buf.data() + kHeaderSize, direct_buf.data(), direct_bytes));

        auto restored = Int32Column::create();
        restored->resize(kCount);
        ASSIGN_OR_ABORT(const uint8_t* read_end, ColumnArraySerde::deserialize(serde_buf.data(), write_end,
                                                                               restored.get(), false, kEncodeLevel));
        ASSERT_EQ(write_end, read_end);
        ASSERT_EQ(0, std::memcmp(mixed_col->raw_data(), restored->raw_data(), kCount * sizeof(int32_t)));
    }

    // Case 2: Unsorted 2-byte values where all 16 control bytes are 0x55.
    {
        auto two_byte_col = Int32Column::create();
        for (size_t i = 0; i < kCount; ++i) {
            two_byte_col->append(static_cast<int32_t>(256 + i * 3));
        }
        std::vector<uint8_t> serde_buf(ColumnArraySerde::max_serialized_size(*two_byte_col, kEncodeLevel));
        ASSIGN_OR_ABORT(uint8_t* write_end,
                        ColumnArraySerde::serialize(*two_byte_col, serde_buf.data(), false, kEncodeLevel));

        std::vector<uint8_t> direct_buf(streamvbyte_max_compressedbytes(kCount));
        size_t direct_bytes = streamvbyte_encode(reinterpret_cast<const uint32_t*>(two_byte_col->raw_data()), kCount,
                                                 direct_buf.data());

        ASSERT_EQ(144u, direct_bytes);
        for (size_t k = 0; k < 16; ++k) {
            ASSERT_EQ(0x55, serde_buf[kHeaderSize + k]);
        }
        ASSERT_EQ(0, std::memcmp(serde_buf.data() + kHeaderSize, direct_buf.data(), direct_bytes));

        auto restored = Int32Column::create();
        restored->resize(kCount);
        ASSIGN_OR_ABORT(const uint8_t* read_end, ColumnArraySerde::deserialize(serde_buf.data(), write_end,
                                                                               restored.get(), false, kEncodeLevel));
        ASSERT_EQ(write_end, read_end);
        ASSERT_EQ(0, std::memcmp(two_byte_col->raw_data(), restored->raw_data(), kCount * sizeof(int32_t)));
    }

    // Case 3: Sorted 1-byte deltas (100 + i * 3) producing 80 compressed bytes
    // (16 control bytes of 0x00 + 64 data bytes).
    {
        auto sorted_col = Int32Column::create();
        for (size_t i = 0; i < kCount; ++i) {
            sorted_col->append(static_cast<int32_t>(100 + i * 3));
        }
        std::vector<uint8_t> serde_buf(ColumnArraySerde::max_serialized_size(*sorted_col, kEncodeLevel));
        ASSIGN_OR_ABORT(uint8_t* write_end,
                        ColumnArraySerde::serialize(*sorted_col, serde_buf.data(), true, kEncodeLevel));

        std::vector<uint8_t> direct_buf(streamvbyte_max_compressedbytes(kCount));
        size_t direct_bytes = streamvbyte_delta_encode(reinterpret_cast<const uint32_t*>(sorted_col->raw_data()),
                                                       kCount, direct_buf.data(), 0);

        ASSERT_EQ(80u, direct_bytes);
        ASSERT_EQ(kCount * sizeof(int32_t), decode_fixed32_le(serde_buf.data()));
        ASSERT_EQ(direct_bytes, decode_fixed64_le(serde_buf.data() + sizeof(uint32_t)));
        ASSERT_EQ(kHeaderSize + direct_bytes, static_cast<size_t>(write_end - serde_buf.data()));
        for (size_t k = 0; k < 16; ++k) {
            ASSERT_EQ(0x00, serde_buf[kHeaderSize + k]);
        }
        ASSERT_EQ(0, std::memcmp(serde_buf.data() + kHeaderSize, direct_buf.data(), direct_bytes));

        auto restored = Int32Column::create();
        restored->resize(kCount);
        ASSIGN_OR_ABORT(const uint8_t* read_end,
                        ColumnArraySerde::deserialize(serde_buf.data(), write_end, restored.get(), true, kEncodeLevel));
        ASSERT_EQ(write_end, read_end);
        ASSERT_EQ(0, std::memcmp(sorted_col->raw_data(), restored->raw_data(), kCount * sizeof(int32_t)));
    }
}

// NOLINTNEXTLINE
TEST(ColumnArraySerdeTest, corrupted_payload_bounds_check) {
    constexpr size_t kRows = 256;
    constexpr int kEncodeLevel = 2;

    auto col = Int32Column::create();
    for (size_t i = 0; i < kRows; ++i) {
        col->append(static_cast<int32_t>(i * 7 + 3));
    }

    std::vector<uint8_t> buffer(ColumnArraySerde::max_serialized_size(*col, kEncodeLevel) + 32, 0);
    ASSIGN_OR_ABORT(uint8_t* write_end, ColumnArraySerde::serialize(*col, buffer.data(), false, kEncodeLevel));
    const uint64_t compressed_bytes = decode_fixed64_le(buffer.data() + sizeof(uint32_t));

    // 1. Corrupt compressed_bytes header (+8 bytes while keeping end large enough) -> encode_size != decode_size.
    {
        auto corrupted = buffer;
        encode_fixed64_le(corrupted.data() + sizeof(uint32_t), compressed_bytes + 8);
        const uint8_t* padded_end = write_end + 8;
        auto dst = Int32Column::create();
        dst->resize(kRows);
        auto st = ColumnArraySerde::deserialize(corrupted.data(), padded_end, dst.get(), false, kEncodeLevel);
        ASSERT_FALSE(st.ok());
        ASSERT_TRUE(st.status().is_internal_error()) << st.status();
    }

    // 2. Truncated buffer (end < buff + sizeof(uint32_t) + sizeof(uint64_t) + compressed_bytes).
    {
        auto dst = Int32Column::create();
        dst->resize(kRows);
        auto st = ColumnArraySerde::deserialize(buffer.data(), write_end - 1, dst.get(), false, kEncodeLevel);
        ASSERT_FALSE(st.ok());
        ASSERT_TRUE(st.status().is_internal_error()) << st.status();
    }
}

namespace protobuf_serde_test {

std::string make_string(size_t i) {
    return std::string("c").append(std::to_string(static_cast<int32_t>(i)));
}

FieldPtr make_field(size_t i) {
    return std::make_shared<Field>(i, make_string(i), get_type_info(TYPE_INT), false);
}

Fields make_fields(size_t size) {
    Fields fields;
    for (size_t i = 0; i < size; i++) {
        fields.emplace_back(make_field(i));
    }
    return fields;
}

SchemaPtr make_schema(size_t i) {
    Fields fields = make_fields(i);
    return std::make_shared<Schema>(fields);
}

ColumnPtr make_column(size_t start) {
    auto column = FixedLengthColumn<int32_t>::create();
    for (int i = 0; i < 100; i++) {
        column->append(start + i);
    }
    return column;
}

// Chunk(Columns&&, SchemaPtr) leaves _slot_id_to_index empty, and ProtobufChunkSerde::serialize()
// DCHECKs that it has one entry per column, so any chunk that goes through the full serialize()
// has to be built slot-mapped.
Chunk::SlotHashMap make_slot_map(size_t size) {
    Chunk::SlotHashMap slot_map;
    slot_map.reserve(std::max<size_t>(1, size * 2));
    for (size_t i = 0; i < size; i++) {
        slot_map[static_cast<SlotId>(i)] = i;
    }
    return slot_map;
}

// A nullable varchar whose every row is NULL: the shape that 985 of the reported table's 993
// columns have, and the only shape whose layout ENCODE_ALL_NULL changes.
ColumnPtr make_all_null_varchar(size_t rows) {
    auto column = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
    column->append_nulls(rows);
    return column;
}

ColumnPtr make_populated_int(size_t rows) {
    auto column = NullableColumn::create(Int32Column::create(), NullColumn::create());
    for (size_t i = 0; i < rows; i++) {
        column->append_datum(Datum(static_cast<int32_t>(i)));
    }
    return column;
}

// The worst case for the all-NULL probe: NULL everywhere except the LAST row, so memchr must walk
// the whole null column before answering "not all-NULL". The compaction can never fire on a column
// shaped like this, so every byte that scan touches is pure cost.
ColumnPtr make_almost_all_null_int(size_t rows) {
    auto column = NullableColumn::create(Int32Column::create(), NullColumn::create());
    column->append_nulls(rows - 1);
    column->append_datum(Datum(static_cast<int32_t>(7)));
    return column;
}

// Meta for a chunk of |all_null_columns| nullable varchars followed by |int_columns| nullable ints.
ProtobufChunkMeta make_wide_meta(size_t all_null_columns, size_t int_columns) {
    const size_t total = all_null_columns + int_columns;
    ProtobufChunkMeta meta;
    meta.types.resize(total);
    meta.is_nulls.assign(total, true);
    meta.is_consts.assign(total, false);
    for (size_t i = 0; i < total; i++) {
        meta.slot_id_to_index[static_cast<SlotId>(i)] = i;
        meta.types[i] = i < all_null_columns ? TYPE_VARCHAR_DESC : TYPE_INT_DESC;
    }
    return meta;
}

Columns make_columns(size_t size) {
    Columns columns;
    for (size_t i = 0; i < size; i++) {
        columns.emplace_back(make_column(i));
    }
    return columns;
}

} // namespace protobuf_serde_test

// NOLINTNEXTLINE
PARALLEL_TEST(ProtobufChunkSerde, test_serde) {
    auto chunk = std::make_unique<Chunk>(protobuf_serde_test::make_columns(2), protobuf_serde_test::make_schema(2));

    StatusOr<ChunkPB> res = serde::ProtobufChunkSerde::serialize_without_meta(*chunk);
    ASSERT_TRUE(res.ok()) << res.status();
    const std::string& serialized_data = res->data();

    ProtobufChunkMeta meta;
    meta.slot_id_to_index[0] = 0;
    meta.slot_id_to_index[1] = 1;
    meta.is_nulls.resize(2, false);
    meta.is_consts.resize(2, false);
    meta.types.resize(2);
    meta.types[0] = TYPE_INT_DESC;
    meta.types[1] = TYPE_INT_DESC;

    ProtobufChunkDeserializer deserializer(meta);
    auto chunk_or = deserializer.deserialize(serialized_data);
    ASSERT_TRUE(chunk_or.ok()) << chunk_or.status();
    Chunk& new_chunk = *chunk_or;
    ASSERT_EQ(new_chunk.num_rows(), chunk->num_rows());
    for (size_t i = 0; i < chunk->columns().size(); ++i) {
        ASSERT_EQ(chunk->columns()[i]->size(), new_chunk.columns()[i]->size());
        for (size_t j = 0; j < chunk->columns()[i]->size(); ++j) {
            ASSERT_EQ(chunk->columns()[i]->get(j).get_int32(), new_chunk.columns()[i]->get(j).get_int32());
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ProtobufChunkSerde, exchange_ignores_the_spill_only_all_null_bit) {
    constexpr size_t kRows = 64;
    constexpr int kLegacyLevel = 7;
    constexpr int kLevelWithBit = 7 | ENCODE_ALL_NULL;

    // An all-NULL nullable column is where the two layouts differ, so it is the only shape that
    // can detect the bit leaking onto the wire.
    auto nullable = NullableColumn::create(Int32Column::create(), NullColumn::create());
    nullable->append_nulls(kRows);
    Columns columns;
    columns.emplace_back(std::move(nullable));
    auto chunk = std::make_unique<Chunk>(std::move(columns), protobuf_serde_test::make_slot_map(1));

    // A sender whose transmission_encode_level carries the bit must still emit the legacy layout,
    // or a BE of an older version could not parse it.
    auto legacy_context = EncodeContext::get_encode_context_shared_ptr(1, kLegacyLevel);
    auto with_bit_context = EncodeContext::get_encode_context_shared_ptr(1, kLevelWithBit);
    ASSIGN_OR_ABORT(auto legacy_pb, ProtobufChunkSerde::serialize(*chunk, legacy_context));
    ASSIGN_OR_ABORT(auto with_bit_pb, ProtobufChunkSerde::serialize(*chunk, with_bit_context));
    // Compare only the content. serialize_without_meta() sizes the buffer with
    // resize_uninitialized() and keeps STREAMVBYTE_PADDING bytes past serialized_size that no
    // writer ever fills, so the tail of data() is whatever the allocator handed out.
    ASSERT_EQ(legacy_pb.serialized_size(), with_bit_pb.serialized_size());
    ASSERT_EQ(legacy_pb.data().substr(0, legacy_pb.serialized_size()),
              with_bit_pb.data().substr(0, with_bit_pb.serialized_size()));

    // The reverse direction: a sender predating the bit treats it as unused, so it advertises the
    // level unchanged in ChunkPB while sending the legacy layout. A receiver applies the SENDER's
    // level, so it has to ignore the bit rather than read the first payload byte as the tag.
    with_bit_pb.clear_encode_level();
    with_bit_pb.add_encode_level(kLevelWithBit);

    ProtobufChunkMeta meta;
    meta.slot_id_to_index[0] = 0;
    meta.is_nulls.resize(1, true);
    meta.is_consts.resize(1, false);
    meta.types.resize(1);
    meta.types[0] = TYPE_INT_DESC;

    ProtobufChunkDeserializer deserializer(meta, &with_bit_pb, kLevelWithBit);
    ASSIGN_OR_ABORT(auto restored, deserializer.deserialize(with_bit_pb.data()));
    ASSERT_EQ(kRows, restored.num_rows());
    for (size_t i = 0; i < kRows; i++) {
        ASSERT_TRUE(restored.columns()[0]->is_null(i));
    }
}

// The tablet sink negotiates ENCODE_ALL_NULL with the receiving BE, so unlike the exchange it is
// allowed to keep the bit. Both settings of the flag are checked: with it the payload collapses to
// a tag plus a row count, without it the very same context still emits the legacy layout.
PARALLEL_TEST(ProtobufChunkSerde, tablet_sink_keeps_the_negotiated_all_null_bit) {
    constexpr size_t kRows = 64;
    constexpr int kLevel = ENCODE_ALL_NULL;

    Columns columns;
    columns.emplace_back(protobuf_serde_test::make_all_null_varchar(kRows));
    auto chunk = std::make_unique<Chunk>(std::move(columns), protobuf_serde_test::make_slot_map(1));

    auto negotiated_ctx = EncodeContext::get_encode_context_shared_ptr(1, kLevel);
    auto stripped_ctx = EncodeContext::get_encode_context_shared_ptr(1, kLevel);
    ASSIGN_OR_ABORT(auto negotiated_pb,
                    ProtobufChunkSerde::serialize(*chunk, negotiated_ctx, /*all_null_negotiated=*/true));
    ASSIGN_OR_ABORT(auto stripped_pb,
                    ProtobufChunkSerde::serialize(*chunk, stripped_ctx, /*all_null_negotiated=*/false));

    // 8 bytes of chunk header (version + row count), then the tag and the row count.
    ASSERT_EQ(8 + 1 + 4, negotiated_pb.serialized_size());
    ASSERT_LT(negotiated_pb.serialized_size(), stripped_pb.serialized_size());

    negotiated_ctx->set_encode_levels_in_pb(&negotiated_pb);
    ASSERT_EQ(1, negotiated_pb.encode_level_size());
    ASSERT_EQ(kLevel, negotiated_pb.encode_level(0));

    auto meta = protobuf_serde_test::make_wide_meta(1, 0);
    ProtobufChunkDeserializer deserializer(meta, &negotiated_pb, kLevel, /*all_null_negotiated=*/true);
    ASSIGN_OR_ABORT(auto restored, deserializer.deserialize(negotiated_pb.data()));
    ASSERT_EQ(kRows, restored.num_rows());
    for (size_t i = 0; i < kRows; i++) {
        ASSERT_TRUE(restored.columns()[0]->is_null(i));
    }
}

// A receiver that never advertised the bit must not apply it even if a sender records it in
// ChunkPB: it would read the first byte of the legacy payload as the tag. This is the tablet sink
// equivalent of the exchange's one-way-masking hazard.
PARALLEL_TEST(ProtobufChunkSerde, tablet_sink_ignores_the_bit_it_did_not_negotiate) {
    constexpr size_t kRows = 32;

    Columns columns;
    columns.emplace_back(protobuf_serde_test::make_all_null_varchar(kRows));
    auto chunk = std::make_unique<Chunk>(std::move(columns), protobuf_serde_test::make_slot_map(1));

    // Legacy layout on the wire, but ChunkPB claims the bit.
    ASSIGN_OR_ABORT(auto legacy_pb, ProtobufChunkSerde::serialize(*chunk));
    legacy_pb.clear_encode_level();
    legacy_pb.add_encode_level(ENCODE_ALL_NULL);

    auto meta = protobuf_serde_test::make_wide_meta(1, 0);
    // A receiver with the feature off passes 0 as the gate, which drops the sender's levels
    // wholesale and parses at level 0 -- exactly what such a BE advertised it would do.
    ProtobufChunkDeserializer des(meta, &legacy_pb, /*encode_level=*/0, /*all_null_negotiated=*/false);
    ASSIGN_OR_ABORT(auto restored, des.deserialize(legacy_pb.data()));
    ASSERT_EQ(kRows, restored.num_rows());
    for (size_t i = 0; i < kRows; i++) {
        ASSERT_TRUE(restored.columns()[0]->is_null(i));
    }
}

// The no-regression side of the negotiation: what does turning the bit on cost a table that has
// no all-NULL columns at all? Two shapes, both fully populated: the 8-column narrow PK table from
// the reported workload, and a 100-column medium table. Asserts the exact format delta and reports
// the CPU delta, which is the number a wide cluster run cannot resolve.
PARALLEL_TEST(ProtobufChunkSerde, negotiated_bit_costs_one_tag_byte_per_nullable_column) {
    constexpr size_t kRows = 4096;
    constexpr int kLevel = ENCODE_ALL_NULL;
    constexpr int kReps = 20;

    struct Shape {
        const char* name;
        size_t nullable_cols;
        size_t non_nullable_cols;
    };
    for (const auto& shape : {Shape{"narrow(8)", 6, 2}, Shape{"medium(100)", 80, 20}}) {
        const size_t total = shape.nullable_cols + shape.non_nullable_cols;
        Columns columns;
        for (size_t i = 0; i < shape.nullable_cols; i++) {
            columns.emplace_back(protobuf_serde_test::make_populated_int(kRows)); // nullable, no NULLs
        }
        for (size_t i = 0; i < shape.non_nullable_cols; i++) {
            auto c = Int32Column::create();
            for (size_t r = 0; r < kRows; r++) {
                c->append(static_cast<int32_t>(r));
            }
            columns.emplace_back(std::move(c));
        }
        auto chunk = std::make_unique<Chunk>(std::move(columns), protobuf_serde_test::make_slot_map(total));

        ASSIGN_OR_ABORT(auto legacy_pb, ProtobufChunkSerde::serialize(*chunk));
        auto ctx = EncodeContext::get_encode_context_shared_ptr(total, kLevel);
        ASSIGN_OR_ABORT(auto negotiated_pb, ProtobufChunkSerde::serialize(*chunk, ctx, /*all_null_negotiated=*/true));

        // The ONLY format change on data like this is a one-byte tag per NULLABLE column. Columns
        // that are not nullable are untouched, and no column's payload is re-encoded.
        ASSERT_EQ(legacy_pb.serialized_size() + static_cast<int64_t>(shape.nullable_cols),
                  negotiated_pb.serialized_size())
                << shape.name;

        // One timed pass. Whichever arm runs first absorbs the cold-cache and first-touch
        // allocation cost, which is worth more than the effect being measured -- a naive
        // "legacy then negotiated" ordering reported the negotiated path as ~19% FASTER even
        // though it strictly does more work. So: warm both arms first, then alternate which one
        // leads on each rep so the first-position penalty cancels.
        auto one_pass = [&](bool negotiated, const std::shared_ptr<EncodeContext>& c) {
            const auto start = std::chrono::steady_clock::now();
            auto res =
                    negotiated ? ProtobufChunkSerde::serialize(*chunk, c, true) : ProtobufChunkSerde::serialize(*chunk);
            const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                            .count();
            CHECK(res.ok());
            return static_cast<double>(ns) / 1000.0;
        };
        auto ctx_for = [&]() { return EncodeContext::get_encode_context_shared_ptr(total, kLevel); };

        for (int i = 0; i < 5; i++) { // warmup, untimed
            auto w = ctx_for();
            one_pass(false, nullptr);
            one_pass(true, w);
        }
        double legacy_us = 0, negotiated_us = 0;
        for (int i = 0; i < kReps; i++) {
            auto c = ctx_for();
            if (i % 2 == 0) {
                legacy_us += one_pass(false, nullptr);
                negotiated_us += one_pass(true, c);
            } else {
                negotiated_us += one_pass(true, c);
                legacy_us += one_pass(false, nullptr);
            }
        }
        legacy_us /= kReps;
        negotiated_us /= kReps;

        std::cerr << "[negotiated bit overhead] " << shape.name << " rows=" << kRows
                  << " nullable=" << shape.nullable_cols << "\n  bytes: " << legacy_pb.serialized_size() << " -> "
                  << negotiated_pb.serialized_size() << " (+" << shape.nullable_cols << ")"
                  << "\n  serialize: " << legacy_us << " us -> " << negotiated_us << " us ("
                  << (100.0 * (negotiated_us - legacy_us) / legacy_us) << "%)" << std::endl;

        // Values must survive the negotiated path untouched.
        auto meta = protobuf_serde_test::make_wide_meta(0, 0);
        meta.types.resize(total, TYPE_INT_DESC);
        meta.is_nulls.assign(total, false);
        for (size_t i = 0; i < shape.nullable_cols; i++) {
            meta.is_nulls[i] = true;
        }
        meta.is_consts.assign(total, false);
        for (size_t i = 0; i < total; i++) {
            meta.slot_id_to_index[static_cast<SlotId>(i)] = i;
        }
        negotiated_pb.clear_encode_level();
        ctx->set_encode_levels_in_pb(&negotiated_pb);
        ProtobufChunkDeserializer des(meta, &negotiated_pb, kLevel, /*all_null_negotiated=*/true);
        ASSIGN_OR_ABORT(auto restored, des.deserialize(negotiated_pb.data()));
        ASSERT_EQ(kRows, restored.num_rows()) << shape.name;
        for (size_t i = 0; i < total; i++) {
            ASSERT_FALSE(restored.columns()[i]->is_null(11)) << shape.name << " col " << i;
            ASSERT_EQ(11, restored.columns()[i]->get(11).get_int32()) << shape.name << " col " << i;
        }
    }
}

// Risk 1 from the review: on a table that never benefits, the per-chunk per-column all-NULL probe
// is pure added cost. The cluster A/B cannot answer this -- within-arm wall-clock spread there
// reached 28%, far larger than the effect -- so measure the probe directly, against the work it
// gates, on the shape that maximises it: every column NULL except its last row, which forces the
// memchr to walk the entire null column and still answer "not all-NULL".
PARALLEL_TEST(ProtobufChunkSerde, all_null_probe_cost_is_small_against_serialization) {
    constexpr size_t kRows = 4096;
    constexpr size_t kColumns = 80;
    constexpr int kReps = 20;

    Columns columns;
    for (size_t i = 0; i < kColumns; i++) {
        columns.emplace_back(protobuf_serde_test::make_almost_all_null_int(kRows));
    }
    auto chunk = std::make_unique<Chunk>(std::move(columns), protobuf_serde_test::make_slot_map(kColumns));

    auto probe = [&]() {
        size_t hits = 0;
        for (const auto& column : chunk->columns()) {
            if (serde::is_all_null_column(*column)) hits++;
        }
        return hits;
    };
    // Not one column qualifies, so the sink keeps the legacy path and the payload is byte-identical
    // to before this change. The probe buys nothing here, which is exactly why its cost matters.
    ASSERT_EQ(0u, probe());

    auto timed = [](auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        return static_cast<double>(ns) / 1000.0;
    };
    auto probe_pass = [&]() { return timed([&]() { CHECK(probe() == 0); }); };
    auto serialize_pass = [&]() {
        return timed([&]() {
            auto res = ProtobufChunkSerde::serialize(*chunk);
            CHECK(res.ok());
        });
    };

    for (int i = 0; i < 5; i++) { // warmup, untimed -- see the ordering-bias note above
        probe_pass();
        serialize_pass();
    }
    double probe_us = 0, serialize_us = 0;
    for (int i = 0; i < kReps; i++) {
        if (i % 2 == 0) {
            probe_us += probe_pass();
            serialize_us += serialize_pass();
        } else {
            serialize_us += serialize_pass();
            probe_us += probe_pass();
        }
    }
    probe_us /= kReps;
    serialize_us /= kReps;

    std::cerr << "[all-NULL probe cost] worst case rows=" << kRows << " columns=" << kColumns
              << " (every column NULL except its last row)\n  probe: " << probe_us << " us, serialize: " << serialize_us
              << " us (" << (100.0 * probe_us / serialize_us) << "% of serialization)" << std::endl;

    // A structural bound, kept deliberately wide so ordinary CI-machine variance cannot flake it:
    // the probe reads one byte per row per column, while serialization moves the values themselves,
    // so the gate cannot approach the cost of the work it gates.
    ASSERT_LT(probe_us, serialize_us) << "probe " << probe_us << " us vs serialize " << serialize_us << " us";
}

// Quantifies what the bit is worth on this RPC, on the schema that motivated it: 993 columns of
// which 985 are entirely NULL. The load RPC defaults to NO_COMPRESSION, so these are wire bytes.
PARALLEL_TEST(ProtobufChunkSerde, tablet_sink_all_null_payload_shrinks_a_wide_mostly_null_chunk) {
    constexpr size_t kAllNullColumns = 985;
    constexpr size_t kIntColumns = 8;
    constexpr size_t kColumns = kAllNullColumns + kIntColumns;
    constexpr size_t kRows = 4096; // the default chunk size
    constexpr int kLevel = ENCODE_ALL_NULL;

    Columns columns;
    for (size_t i = 0; i < kAllNullColumns; i++) {
        columns.emplace_back(protobuf_serde_test::make_all_null_varchar(kRows));
    }
    for (size_t i = 0; i < kIntColumns; i++) {
        columns.emplace_back(protobuf_serde_test::make_populated_int(kRows));
    }
    auto chunk = std::make_unique<Chunk>(std::move(columns), protobuf_serde_test::make_slot_map(kColumns));

    // Legacy: exactly what a sender that has not negotiated the bit emits today.
    ASSIGN_OR_ABORT(auto legacy_pb, ProtobufChunkSerde::serialize(*chunk));
    auto negotiated_ctx = EncodeContext::get_encode_context_shared_ptr(kColumns, kLevel);
    ASSIGN_OR_ABORT(auto negotiated_pb,
                    ProtobufChunkSerde::serialize(*chunk, negotiated_ctx, /*all_null_negotiated=*/true));
    negotiated_ctx->set_encode_levels_in_pb(&negotiated_pb);

    const int64_t legacy_bytes = legacy_pb.serialized_size();
    const int64_t negotiated_bytes = negotiated_pb.serialized_size();

    // At level 0 each all-NULL varchar costs a 1-byte null flag plus a 4-byte offset per row; with
    // the bit it costs a 1-byte tag plus a 4-byte row count per chunk, so its payload stops
    // scaling with the row count altogether. Asserting an order of magnitude rather than an exact
    // byte count keeps this from breaking on unrelated layout changes, while still failing if the
    // short circuit stops firing on this shape.
    ASSERT_GT(legacy_bytes, negotiated_bytes * 50);

    auto meta = protobuf_serde_test::make_wide_meta(kAllNullColumns, kIntColumns);
    auto time_deserialize = [&](const ChunkPB& pb, int level, bool negotiated) {
        ProtobufChunkDeserializer des(meta, &pb, level, negotiated);
        const auto start = std::chrono::steady_clock::now();
        auto res = des.deserialize(pb.data());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(res.ok()) << res.status();
        CHECK_EQ(kRows, res->num_rows());
        return std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    };
    const int64_t legacy_us = time_deserialize(legacy_pb, 0, false);
    const int64_t negotiated_us = time_deserialize(negotiated_pb, kLevel, true);

    // Reported, not asserted: wall time on a shared CI runner is too noisy to gate on, but it is
    // the other half of what this RPC pays for the empty columns.
    std::cerr << "[all-null tablet sink payload] columns=" << kColumns << " (" << kAllNullColumns
              << " all NULL) rows=" << kRows << "\n  bytes: " << legacy_bytes << " -> " << negotiated_bytes << " ("
              << (legacy_bytes / static_cast<double>(negotiated_bytes)) << "x, "
              << (legacy_bytes / static_cast<double>(kRows)) << " -> "
              << (negotiated_bytes / static_cast<double>(kRows)) << " B/row)\n  deserialize: " << legacy_us << " us -> "
              << negotiated_us << " us" << std::endl;

    // The rows still round-trip: every varchar NULL, every int its value.
    ProtobufChunkDeserializer des(meta, &negotiated_pb, kLevel, /*all_null_negotiated=*/true);
    ASSIGN_OR_ABORT(auto restored, des.deserialize(negotiated_pb.data()));
    ASSERT_EQ(kRows, restored.num_rows());
    for (size_t i = 0; i < kAllNullColumns; i++) {
        ASSERT_EQ(kRows, restored.columns()[i]->size());
        ASSERT_TRUE(restored.columns()[i]->is_null(0));
        ASSERT_TRUE(restored.columns()[i]->is_null(kRows - 1));
    }
    for (size_t i = kAllNullColumns; i < kColumns; i++) {
        ASSERT_FALSE(restored.columns()[i]->is_null(7));
        ASSERT_EQ(7, restored.columns()[i]->get(7).get_int32());
    }
}

PARALLEL_TEST(ProtobufChunkSerde, deserialize_with_schema) {
    auto chunk = std::make_unique<Chunk>(protobuf_serde_test::make_columns(2), protobuf_serde_test::make_schema(2));

    StatusOr<ChunkPB> res = serde::ProtobufChunkSerde::serialize_without_meta(*chunk);
    ASSERT_TRUE(res.ok()) << res.status();
    const std::string& serialized_data = res->data();

    auto chunk_or = serde::ProtobufChunkSerde::deserialize_with_schema(*chunk->schema(), serialized_data);
    ASSERT_TRUE(chunk_or.ok()) << chunk_or.status();
    Chunk& new_chunk = *chunk_or;
    ASSERT_EQ(new_chunk.num_rows(), chunk->num_rows());
    for (size_t i = 0; i < chunk->columns().size(); ++i) {
        ASSERT_EQ(chunk->columns()[i]->size(), new_chunk.columns()[i]->size());
        for (size_t j = 0; j < chunk->columns()[i]->size(); ++j) {
            ASSERT_EQ(chunk->columns()[i]->get(j).get_int32(), new_chunk.columns()[i]->get(j).get_int32());
        }
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(ProtobufChunkSerde, TestChunkWithExtraData) {
    auto chunk = std::make_unique<Chunk>(protobuf_serde_test::make_columns(2), protobuf_serde_test::make_schema(2));
    auto extra_data_meta = std::vector<ChunkExtraColumnsMeta>{
            ChunkExtraColumnsMeta{.type = TYPE_INT_DESC, .is_null = false, .is_const = false}};
    auto extra_data_cols = protobuf_serde_test::make_columns(2);
    auto extra_data = std::make_shared<ChunkExtraColumnsData>(std::move(extra_data_meta), std::move(extra_data_cols));
    chunk->set_extra_data(extra_data);

    StatusOr<ChunkPB> res = serde::ProtobufChunkSerde::serialize_without_meta(*chunk);
    ASSERT_TRUE(res.ok()) << res.status();
    const std::string& serialized_data = res->data();

    ProtobufChunkMeta meta;
    meta.slot_id_to_index[0] = 0;
    meta.slot_id_to_index[1] = 1;
    meta.is_nulls.resize(2, false);
    meta.is_consts.resize(2, false);
    meta.types.resize(2);
    meta.types[0] = TYPE_INT_DESC;
    meta.types[1] = TYPE_INT_DESC;
    meta.extra_data_metas = std::vector<ChunkExtraColumnsMeta>{
            ChunkExtraColumnsMeta{.type = TYPE_INT_DESC, .is_null = false, .is_const = false}};

    ProtobufChunkDeserializer deserializer(meta);
    auto chunk_or = deserializer.deserialize(serialized_data);
    ASSERT_TRUE(chunk_or.ok()) << chunk_or.status();

    // check original chunk data
    Chunk& new_chunk = *chunk_or;
    ASSERT_EQ(new_chunk.num_rows(), chunk->num_rows());
    for (size_t i = 0; i < chunk->columns().size(); ++i) {
        ASSERT_EQ(chunk->columns()[i]->size(), new_chunk.columns()[i]->size());
        for (size_t j = 0; j < chunk->columns()[i]->size(); ++j) {
            ASSERT_EQ(chunk->columns()[i]->get(j).get_int32(), new_chunk.columns()[i]->get(j).get_int32());
        }
    }

    // check extra chunk data
    DCHECK(new_chunk.has_extra_data());
    auto new_extra_data = dynamic_cast<ChunkExtraColumnsData*>(new_chunk.get_extra_data().get());
    auto old_extra_data = dynamic_cast<ChunkExtraColumnsData*>(chunk->get_extra_data().get());
    for (size_t i = 0; i < new_extra_data->columns().size(); ++i) {
        ASSERT_EQ(old_extra_data->columns()[i]->size(), new_extra_data->columns()[i]->size());
        for (size_t j = 0; j < old_extra_data->columns()[i]->size(); ++j) {
            ASSERT_EQ(old_extra_data->columns()[i]->get(j).get_int32(),
                      new_extra_data->columns()[i]->get(j).get_int32());
        }
    }
}

} // namespace starrocks::serde
