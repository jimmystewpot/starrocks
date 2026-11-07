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

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "base/string/string_searcher.h"
#include "base/string/utf8.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/column_viewer.h"
#include "exprs/function_context.h"
#include "exprs/string_functions.h"

namespace starrocks {

static inline Slice split_part_slice(const Slice& haystack, const Slice& delimiter, const StringSearcher& searcher,
                                     int32_t part_number) {
    if (delimiter.size == 0) {
        // Keep consistent with split; non-positive or out-of-range part_number returns empty slice.
        if (part_number <= 0 || static_cast<size_t>(part_number) > haystack.size) {
            return Slice("", 0);
        }
        size_t char_size = 0;
        size_t h = 0;
        for (int32_t num = 0; h < haystack.size && num < part_number - 1; h += char_size) {
            // Clamp to the remaining bytes so a tail lead byte never overshoots the input.
            char_size = std::min<size_t>(UTF8_BYTE_LENGTH_TABLE[static_cast<unsigned char>(haystack.data[h])],
                                         haystack.size - h);
            ++num;
        }
        if (h >= haystack.size) {
            return Slice("", 0);
        }
        // A truncated/invalid UTF-8 lead byte at the tail can claim more bytes than
        // remain; clamp to avoid an out-of-bounds read that leaks adjacent memory.
        char_size = std::min<size_t>(UTF8_BYTE_LENGTH_TABLE[static_cast<unsigned char>(haystack.data[h])],
                                     haystack.size - h);
        return Slice(haystack.data + h, char_size);
    }

    if (part_number == 0) {
        return Slice("", 0);
    }

    const char* const begin = haystack.data;
    const char* const end = begin + haystack.size;

    if (part_number > 0) {
        if (part_number > 1 && static_cast<size_t>(part_number - 1) > haystack.size) {
            return Slice("", 0);
        }
        const char* cur = begin;
        for (int32_t num = 0; num < part_number; ++num) {
            const char* pos = (delimiter.size == 1)
                                      ? static_cast<const char*>(std::memchr(
                                                cur, static_cast<uint8_t>(delimiter.data[0]),
                                                static_cast<size_t>(end - cur)))
                                      : searcher.search(cur, end);
            if (pos != nullptr && pos != end) {
                if (num + 1 == part_number) {
                    return Slice(cur, static_cast<size_t>(pos - cur));
                }
                cur = pos + delimiter.size;
            } else {
                return (num + 1 == part_number) ? Slice(cur, static_cast<size_t>(end - cur)) : Slice("", 0);
            }
        }
        return Slice("", 0);
    } else {
        const int64_t target = -static_cast<int64_t>(part_number);
        if (target > 1 && static_cast<uint64_t>(target - 1) > haystack.size) {
            return Slice("", 0);
        }
        const char* cur_end = end;
        for (int64_t num = 0; num < target; ++num) {
            const char* pos = searcher.rsearch(begin, cur_end);
            if (pos != cur_end) {
                if (num + 1 == target) {
                    const char* slice_begin = pos + delimiter.size;
                    return Slice(slice_begin, static_cast<size_t>(cur_end - slice_begin));
                }
                cur_end = pos;
            } else {
                return (num + 1 == target) ? Slice(begin, static_cast<size_t>(cur_end - begin)) : Slice("", 0);
            }
        }
        return Slice("", 0);
    }
}

/**
 * @param: [haystack, delimiter, part_number]
 * @paramType: [BinaryColumn, BinaryColumn, IntColumn]
 * @return: BinaryColumn
 */
StatusOr<ColumnPtr> StringFunctions::split_part(FunctionContext* context, const starrocks::Columns& columns) {
    DCHECK_EQ(columns.size(), 3);

    const size_t size = columns[0]->size();
    if (columns[0]->only_null() || columns[1]->only_null() || columns[2]->only_null()) {
        return ColumnHelper::create_const_column<TYPE_VARCHAR>("", size);
    }

    if (columns[2]->is_constant()) {
        // if part_number is 0, return an empty string.
        int32_t part_number = ColumnHelper::get_const_value<TYPE_INT>(columns[2]);
        if (part_number == 0) {
            return ColumnHelper::create_const_column<TYPE_VARCHAR>("", size);
        }
    }

    if (columns[1]->is_constant()) {
        const Slice delimiter = ColumnHelper::get_const_value<TYPE_VARCHAR>(columns[1]);
        if (delimiter.size == 0 && columns[2]->is_constant() &&
            ColumnHelper::get_const_value<TYPE_INT>(columns[2]) <= 0) {
            return ColumnHelper::create_const_column<TYPE_VARCHAR>("", size);
        }

        const StringSearcher searcher(delimiter.data, delimiter.size);

        if (ColumnHelper::is_all_const(columns)) {
            const Slice haystack = ColumnHelper::get_const_value<TYPE_VARCHAR>(columns[0]);
            const int32_t part_number = ColumnHelper::get_const_value<TYPE_INT>(columns[2]);
            const Slice s = split_part_slice(haystack, delimiter, searcher, part_number);
            return ColumnHelper::create_const_column<TYPE_VARCHAR>(s, size);
        }

        if (!columns[0]->is_constant()) {
            const BinaryColumn* src_binary = ColumnHelper::get_binary_column(columns[0].get());
            const auto* src_data = reinterpret_cast<const char*>(src_binary->raw_bytes());
            const size_t max_bytes = src_binary->get_immutable_bytes().size();

            auto dst = BinaryColumn::create();
            BinaryColumn::Bytes& dst_bytes = dst->get_bytes();
            BinaryColumn::Offsets& dst_offsets = dst->get_offset();
            dst_offsets.make_room(size + 1, max_bytes);
            dst_offsets.set(0, 0);
            dst_bytes.reserve(std::min<size_t>(max_bytes, 1ULL << 32));

            const uint8_t* str_nulls =
                    columns[0]->has_null() ? ColumnHelper::get_null_data_ptr(columns[0].get()) : nullptr;

            if (columns[2]->is_constant()) {
                const int32_t part_number = ColumnHelper::get_const_value<TYPE_INT>(columns[2]);
                src_binary->get_offset().visit_storage([&](const auto& src_offset_buf) {
                    dst_offsets.visit_storage([&](auto& dst_offset_buf) {
                        using DstOffset = typename std::decay_t<decltype(dst_offset_buf)>::value_type;
                        const auto* src_offset_data = src_offset_buf.data();
                        auto* dst_offset_data = dst_offset_buf.data();
                        if (str_nulls == nullptr) {
                            for (size_t i = 0; i < size; ++i) {
                                const auto start = src_offset_data[i];
                                const Slice hay(src_data + start, static_cast<size_t>(src_offset_data[i + 1] - start));
                                const Slice s = split_part_slice(hay, delimiter, searcher, part_number);
                                if (s.size > 0) {
                                    dst_bytes.insert(dst_bytes.end(), reinterpret_cast<const uint8_t*>(s.data),
                                                     reinterpret_cast<const uint8_t*>(s.data) + s.size);
                                }
                                dst_offset_data[i + 1] = static_cast<DstOffset>(dst_bytes.size());
                            }
                        } else {
                            for (size_t i = 0; i < size; ++i) {
                                if (str_nulls[i] == 0) {
                                    const auto start = src_offset_data[i];
                                    const Slice hay(src_data + start,
                                                    static_cast<size_t>(src_offset_data[i + 1] - start));
                                    const Slice s = split_part_slice(hay, delimiter, searcher, part_number);
                                    if (s.size > 0) {
                                        dst_bytes.insert(dst_bytes.end(), reinterpret_cast<const uint8_t*>(s.data),
                                                         reinterpret_cast<const uint8_t*>(s.data) + s.size);
                                    }
                                }
                                dst_offset_data[i + 1] = static_cast<DstOffset>(dst_bytes.size());
                            }
                        }
                    });
                });
            } else {
                const auto* part_col = ColumnHelper::get_data_column_by_type<TYPE_INT>(columns[2].get());
                const int32_t* part_data = part_col->get_data().data();
                const uint8_t* part_nulls =
                        columns[2]->has_null() ? ColumnHelper::get_null_data_ptr(columns[2].get()) : nullptr;
                src_binary->get_offset().visit_storage([&](const auto& src_offset_buf) {
                    dst_offsets.visit_storage([&](auto& dst_offset_buf) {
                        using DstOffset = typename std::decay_t<decltype(dst_offset_buf)>::value_type;
                        const auto* src_offset_data = src_offset_buf.data();
                        auto* dst_offset_data = dst_offset_buf.data();
                        if (str_nulls == nullptr && part_nulls == nullptr) {
                            for (size_t i = 0; i < size; ++i) {
                                const auto start = src_offset_data[i];
                                const Slice hay(src_data + start, static_cast<size_t>(src_offset_data[i + 1] - start));
                                const Slice s = split_part_slice(hay, delimiter, searcher, part_data[i]);
                                if (s.size > 0) {
                                    dst_bytes.insert(dst_bytes.end(), reinterpret_cast<const uint8_t*>(s.data),
                                                     reinterpret_cast<const uint8_t*>(s.data) + s.size);
                                }
                                dst_offset_data[i + 1] = static_cast<DstOffset>(dst_bytes.size());
                            }
                        } else {
                            for (size_t i = 0; i < size; ++i) {
                                const bool is_null = (str_nulls != nullptr && str_nulls[i] != 0) ||
                                                     (part_nulls != nullptr && part_nulls[i] != 0);
                                if (!is_null) {
                                    const auto start = src_offset_data[i];
                                    const Slice hay(src_data + start,
                                                    static_cast<size_t>(src_offset_data[i + 1] - start));
                                    const Slice s = split_part_slice(hay, delimiter, searcher, part_data[i]);
                                    if (s.size > 0) {
                                        dst_bytes.insert(dst_bytes.end(), reinterpret_cast<const uint8_t*>(s.data),
                                                         reinterpret_cast<const uint8_t*>(s.data) + s.size);
                                    }
                                }
                                dst_offset_data[i + 1] = static_cast<DstOffset>(dst_bytes.size());
                            }
                        }
                    });
                });
            }
            return dst;
        }
    }

    ColumnViewer<TYPE_VARCHAR> haystack_viewer(columns[0]);
    ColumnViewer<TYPE_VARCHAR> delimiter_viewer(columns[1]);
    ColumnViewer<TYPE_INT> part_number_viewer(columns[2]);

    auto dst = BinaryColumn::create();
    BinaryColumn::Bytes& dst_bytes = dst->get_bytes();
    BinaryColumn::Offsets& dst_offsets = dst->get_offset();
    dst_offsets.reserve(size + 1);

    const bool is_const_delim = columns[1]->is_constant();
    const Slice const_delim = is_const_delim ? delimiter_viewer.value(0) : Slice("", 0);
    const StringSearcher const_searcher(const_delim.data, const_delim.size);

    for (size_t i = 0; i < size; ++i) {
        if (haystack_viewer.is_null(i) || delimiter_viewer.is_null(i) || part_number_viewer.is_null(i)) {
            dst_offsets.push_back(dst_bytes.size());
            continue;
        }

        const int32_t part_number = part_number_viewer.value(i);
        const Slice haystack = haystack_viewer.value(i);
        Slice s;
        if (is_const_delim) {
            s = split_part_slice(haystack, const_delim, const_searcher, part_number);
        } else {
            const Slice delimiter = delimiter_viewer.value(i);
            const StringSearcher searcher(delimiter.data, delimiter.size);
            s = split_part_slice(haystack, delimiter, searcher, part_number);
        }
        if (s.size > 0) {
            dst_bytes.insert(dst_bytes.end(), reinterpret_cast<const uint8_t*>(s.data),
                             reinterpret_cast<const uint8_t*>(s.data) + s.size);
        }
        dst_offsets.push_back(dst_bytes.size());
    }
    return dst;
}

} // namespace starrocks
