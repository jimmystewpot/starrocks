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

#include <benchmark/benchmark.h>
#include <streamvbyte.h>
#include <streamvbytedelta.h>

#include <cstdint>
#include <string>
#include <vector>

#include "base/string/slice.h"
#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "column/serde/column_array_serde.h"
#include "common/logging.h"

namespace starrocks {
namespace {

enum class Int32Distribution {
    kUnsortedSmall,
    kUnsortedMixed,
    kUnsortedLarge,
    kSortedSmallDelta,
    kSortedMixedDelta,
};

Int32Column::MutablePtr make_int32_column(size_t num_rows, Int32Distribution dist) {
    auto col = Int32Column::create();
    col->reserve(num_rows);
    uint32_t acc = 100;
    for (size_t i = 0; i < num_rows; ++i) {
        uint32_t val = 0;
        switch (dist) {
        case Int32Distribution::kUnsortedSmall:
            val = static_cast<uint32_t>((i * 73) & 0xFFU);
            break;
        case Int32Distribution::kUnsortedMixed: {
            const size_t bucket = i & 3U;
            if (bucket == 0) {
                val = static_cast<uint32_t>(i & 0x7FU);
            } else if (bucket == 1) {
                val = static_cast<uint32_t>(0x0100U + (i & 0x3FFFU));
            } else if (bucket == 2) {
                val = static_cast<uint32_t>(0x010000U + (i & 0x1FFFFFU));
            } else {
                val = static_cast<uint32_t>(0x01000000U + (i * 2654435761U));
            }
            break;
        }
        case Int32Distribution::kUnsortedLarge:
            val = static_cast<uint32_t>(0x10000000U + (i * 2654435761U));
            break;
        case Int32Distribution::kSortedSmallDelta:
            acc += static_cast<uint32_t>(1U + (i % 15U));
            val = acc;
            break;
        case Int32Distribution::kSortedMixedDelta:
            if ((i & 7U) == 0) {
                acc += 1024U + static_cast<uint32_t>((i * 17U) & 0xFFFFU);
            } else {
                acc += static_cast<uint32_t>(1U + (i % 31U));
            }
            val = acc;
            break;
        }
        col->append(static_cast<int32_t>(val));
    }
    return col;
}

Int64Column::MutablePtr make_int64_mixed_column(size_t num_rows) {
    auto col = Int64Column::create();
    col->reserve(num_rows);
    for (size_t i = 0; i < num_rows; ++i) {
        int64_t val = 0;
        if ((i & 1U) == 0) {
            val = static_cast<int64_t>(i & 0xFFU);
        } else {
            val = (static_cast<int64_t>(0x01020304ULL + i) << 32) | static_cast<int64_t>((i * 131U) & 0xFFFFU);
        }
        col->append(val);
    }
    return col;
}

BinaryColumn::MutablePtr make_binary_column(size_t num_rows, bool mixed_lengths) {
    auto col = BinaryColumn::create();
    std::string payload(512, 'x');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + (i % 26));
    }
    for (size_t i = 0; i < num_rows; ++i) {
        size_t len = 0;
        if (!mixed_lengths) {
            len = 4U + (i % 16U);
        } else {
            len = ((i & 15U) == 0) ? (280U + (i % 120U)) : (8U + (i % 24U));
        }
        col->append(Slice(payload.data(), len));
    }
    return col;
}

ArrayColumn::MutablePtr make_array_column(size_t num_rows) {
    auto offsets = UInt32Column::create();
    auto elements = NullableColumn::create(Int32Column::create(), NullColumn::create());
    auto col = ArrayColumn::create(std::move(elements), std::move(offsets));

    uint32_t cur_offset = 0;
    auto* elem_col = col->elements_column_raw_ptr();
    auto* off_col = col->offsets_column_raw_ptr();
    for (size_t i = 0; i < num_rows; ++i) {
        const uint32_t arr_len = static_cast<uint32_t>(1U + (i % 4U));
        for (uint32_t j = 0; j < arr_len; ++j) {
            elem_col->append_datum(Datum(static_cast<int32_t>((i + j) & 0x7FU)));
        }
        cur_offset += arr_len;
        off_col->append(cur_offset);
    }
    return col;
}

template <Int32Distribution kDist, bool kSorted>
void BM_Int32_Serialize_Impl(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_int32_column(num_rows, kDist);
    const size_t raw_bytes = num_rows * sizeof(int32_t);
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));

    size_t serialized_bytes = 0;
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::serialize(*src, buffer.data(), kSorted, encode_level);
        CHECK(res.ok());
        uint8_t* end_ptr = res.value();
        serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());
        benchmark::DoNotOptimize(end_ptr);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

template <Int32Distribution kDist, bool kSorted>
void BM_Int32_Deserialize_Impl(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_int32_column(num_rows, kDist);
    const size_t raw_bytes = num_rows * sizeof(int32_t);
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));
    auto ser_res = serde::ColumnArraySerde::serialize(*src, buffer.data(), kSorted, encode_level);
    CHECK(ser_res.ok());
    const uint8_t* end_ptr = ser_res.value();
    const auto serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());

    auto dst = Int32Column::create();
    dst->resize(num_rows);
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::deserialize(buffer.data(), end_ptr, dst.get(), kSorted, encode_level);
        CHECK(res.ok());
        benchmark::DoNotOptimize(dst->raw_data());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

void BM_Int32_Unsorted_Small_Serialize(benchmark::State& state) {
    BM_Int32_Serialize_Impl<Int32Distribution::kUnsortedSmall, false>(state);
}
void BM_Int32_Unsorted_Small_Deserialize(benchmark::State& state) {
    BM_Int32_Deserialize_Impl<Int32Distribution::kUnsortedSmall, false>(state);
}

void BM_Int32_Unsorted_Mixed_Serialize(benchmark::State& state) {
    BM_Int32_Serialize_Impl<Int32Distribution::kUnsortedMixed, false>(state);
}
void BM_Int32_Unsorted_Mixed_Deserialize(benchmark::State& state) {
    BM_Int32_Deserialize_Impl<Int32Distribution::kUnsortedMixed, false>(state);
}

void BM_Int32_Unsorted_Large_Serialize(benchmark::State& state) {
    BM_Int32_Serialize_Impl<Int32Distribution::kUnsortedLarge, false>(state);
}
void BM_Int32_Unsorted_Large_Deserialize(benchmark::State& state) {
    BM_Int32_Deserialize_Impl<Int32Distribution::kUnsortedLarge, false>(state);
}

void BM_Int32_Sorted_SmallDelta_Serialize(benchmark::State& state) {
    BM_Int32_Serialize_Impl<Int32Distribution::kSortedSmallDelta, true>(state);
}
void BM_Int32_Sorted_SmallDelta_Deserialize(benchmark::State& state) {
    BM_Int32_Deserialize_Impl<Int32Distribution::kSortedSmallDelta, true>(state);
}

void BM_Int32_Sorted_MixedDelta_Serialize(benchmark::State& state) {
    BM_Int32_Serialize_Impl<Int32Distribution::kSortedMixedDelta, true>(state);
}
void BM_Int32_Sorted_MixedDelta_Deserialize(benchmark::State& state) {
    BM_Int32_Deserialize_Impl<Int32Distribution::kSortedMixedDelta, true>(state);
}

void BM_Int64_Unsorted_Mixed_Serialize(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_int64_mixed_column(num_rows);
    const size_t raw_bytes = num_rows * sizeof(int64_t);
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));

    size_t serialized_bytes = 0;
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::serialize(*src, buffer.data(), false, encode_level);
        CHECK(res.ok());
        uint8_t* end_ptr = res.value();
        serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());
        benchmark::DoNotOptimize(end_ptr);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

void BM_Int64_Unsorted_Mixed_Deserialize(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_int64_mixed_column(num_rows);
    const size_t raw_bytes = num_rows * sizeof(int64_t);
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));
    auto ser_res = serde::ColumnArraySerde::serialize(*src, buffer.data(), false, encode_level);
    CHECK(ser_res.ok());
    const uint8_t* end_ptr = ser_res.value();
    const auto serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());

    auto dst = Int64Column::create();
    dst->resize(num_rows);
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::deserialize(buffer.data(), end_ptr, dst.get(), false, encode_level);
        CHECK(res.ok());
        benchmark::DoNotOptimize(dst->raw_data());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

template <bool kMixedLengths>
void BM_Binary_Offsets_Serialize_Impl(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_binary_column(num_rows, kMixedLengths);
    const size_t raw_bytes = src->byte_size();
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));

    size_t serialized_bytes = 0;
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::serialize(*src, buffer.data(), false, encode_level);
        CHECK(res.ok());
        uint8_t* end_ptr = res.value();
        serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());
        benchmark::DoNotOptimize(end_ptr);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

template <bool kMixedLengths>
void BM_Binary_Offsets_Deserialize_Impl(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_binary_column(num_rows, kMixedLengths);
    const size_t raw_bytes = src->byte_size();
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));
    auto ser_res = serde::ColumnArraySerde::serialize(*src, buffer.data(), false, encode_level);
    CHECK(ser_res.ok());
    const uint8_t* end_ptr = ser_res.value();
    const auto serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());

    auto dst = BinaryColumn::create();
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::deserialize(buffer.data(), end_ptr, dst.get(), false, encode_level);
        CHECK(res.ok());
        benchmark::DoNotOptimize(dst->get_immutable_bytes().data());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

void BM_Binary_Offsets_ShortStrings_Serialize(benchmark::State& state) {
    BM_Binary_Offsets_Serialize_Impl<false>(state);
}
void BM_Binary_Offsets_ShortStrings_Deserialize(benchmark::State& state) {
    BM_Binary_Offsets_Deserialize_Impl<false>(state);
}

void BM_Binary_Offsets_MixedStrings_Serialize(benchmark::State& state) {
    BM_Binary_Offsets_Serialize_Impl<true>(state);
}
void BM_Binary_Offsets_MixedStrings_Deserialize(benchmark::State& state) {
    BM_Binary_Offsets_Deserialize_Impl<true>(state);
}

void BM_Array_Offsets_And_Elements_Serialize(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_array_column(num_rows);
    const size_t raw_bytes = src->byte_size();
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));

    size_t serialized_bytes = 0;
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::serialize(*src, buffer.data(), false, encode_level);
        CHECK(res.ok());
        uint8_t* end_ptr = res.value();
        serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());
        benchmark::DoNotOptimize(end_ptr);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

void BM_Array_Offsets_And_Elements_Deserialize(benchmark::State& state) {
    const auto num_rows = static_cast<size_t>(state.range(0));
    const int encode_level = static_cast<int>(state.range(1));
    auto src = make_array_column(num_rows);
    const size_t raw_bytes = src->byte_size();
    const int64_t max_size = serde::ColumnArraySerde::max_serialized_size(*src, encode_level);
    std::vector<uint8_t> buffer(static_cast<size_t>(max_size));
    auto ser_res = serde::ColumnArraySerde::serialize(*src, buffer.data(), false, encode_level);
    CHECK(ser_res.ok());
    const uint8_t* end_ptr = ser_res.value();
    const auto serialized_bytes = static_cast<size_t>(end_ptr - buffer.data());

    auto dst = ArrayColumn::create(NullableColumn::create(Int32Column::create(), NullColumn::create()),
                                   UInt32Column::create());
    for (auto _ : state) {
        auto res = serde::ColumnArraySerde::deserialize(buffer.data(), end_ptr, dst.get(), false, encode_level);
        CHECK(res.ok());
        benchmark::DoNotOptimize(dst->offsets().raw_data());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(num_rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(raw_bytes));
    state.counters["compression_ratio"] = static_cast<double>(raw_bytes) / static_cast<double>(serialized_bytes);
}

#define REGISTER_SERDE_BENCH(name)                                     \
    BENCHMARK(name##_Serialize)->ArgsProduct({{4096, 32768}, {0, 2}}); \
    BENCHMARK(name##_Deserialize)->ArgsProduct({{4096, 32768}, {0, 2}})

REGISTER_SERDE_BENCH(BM_Int32_Unsorted_Small);
REGISTER_SERDE_BENCH(BM_Int32_Unsorted_Mixed);
REGISTER_SERDE_BENCH(BM_Int32_Unsorted_Large);
REGISTER_SERDE_BENCH(BM_Int32_Sorted_SmallDelta);
REGISTER_SERDE_BENCH(BM_Int32_Sorted_MixedDelta);
REGISTER_SERDE_BENCH(BM_Int64_Unsorted_Mixed);
REGISTER_SERDE_BENCH(BM_Binary_Offsets_ShortStrings);
REGISTER_SERDE_BENCH(BM_Binary_Offsets_MixedStrings);
REGISTER_SERDE_BENCH(BM_Array_Offsets_And_Elements);

} // namespace
} // namespace starrocks

BENCHMARK_MAIN();
