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

#pragma STDC FP_CONTRACT OFF

#include "types/ddsketch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace starrocks {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "DDSketch requires a little-endian architecture");

namespace {

inline size_t stepped_capacity(size_t needed, uint32_t max_bins) {
    if (needed == 0) {
        return 0;
    }
    if (needed == 1) {
        return 1;
    }
    const size_t step = CollapsingLowestDenseStore::kGrowthStep;
    const size_t rounded = ((needed + step - 1) / step) * step;
    return std::min<size_t>(max_bins, rounded);
}

inline size_t varint_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

inline uint8_t* write_varint(uint8_t* p, uint64_t v) {
    while (v >= 0x80) {
        *p++ = static_cast<uint8_t>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    *p++ = static_cast<uint8_t>(v);
    return p;
}

inline uint8_t* write_double_le(uint8_t* p, double d) {
    std::memcpy(p, &d, sizeof(double));
    return p + sizeof(double);
}

inline uint32_t zigzag_encode_sint32(int32_t v) {
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

inline int32_t zigzag_decode_sint32(uint32_t n) {
    return static_cast<int32_t>((n >> 1) ^ (~(n & 1) + 1));
}

Status read_varint(const uint8_t*& p, const uint8_t* end, uint64_t* out) {
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (p >= end) {
            return Status::InvalidArgument("ddsketch: malformed varint");
        }
        const uint8_t byte = *p++;
        if (i == 9 && byte > 1) {
            return Status::InvalidArgument("ddsketch: malformed varint");
        }
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *out = result;
            return Status::OK();
        }
        shift += 7;
    }
    return Status::InvalidArgument("ddsketch: malformed varint");
}

Status read_double_le(const uint8_t*& p, const uint8_t* end, double* out) {
    if (static_cast<size_t>(end - p) < sizeof(double)) {
        return Status::InvalidArgument("ddsketch: truncated 64-bit protobuf field");
    }
    std::memcpy(out, p, sizeof(double));
    p += sizeof(double);
    return Status::OK();
}

Status skip_unknown_field(uint32_t wire_type, const uint8_t*& p, const uint8_t* end) {
    switch (wire_type) {
    case 0: {
        uint64_t dummy = 0;
        return read_varint(p, end, &dummy);
    }
    case 1: {
        if (static_cast<size_t>(end - p) < 8) {
            return Status::InvalidArgument("ddsketch: truncated 64-bit protobuf field");
        }
        p += 8;
        return Status::OK();
    }
    case 2: {
        uint64_t len = 0;
        Status st = read_varint(p, end, &len);
        if (!st.ok()) {
            return st;
        }
        if (len > static_cast<uint64_t>(end - p)) {
            return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
        }
        p += len;
        return Status::OK();
    }
    case 5: {
        if (static_cast<size_t>(end - p) < 4) {
            return Status::InvalidArgument("ddsketch: truncated 32-bit protobuf field");
        }
        p += 4;
        return Status::OK();
    }
    default:
        return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
    }
}

bool validate_wire_count(double c, uint64_t* out) {
    if (!std::isfinite(c) || c < 0.0 || c > static_cast<double>(DDSketch::kMaxExactDoubleCount) || std::floor(c) != c) {
        return false;
    }
    *out = static_cast<uint64_t>(c);
    return true;
}

size_t store_body_serialized_size(const CollapsingLowestDenseStore& store) {
    const size_t num_bins = store.bins().size();
    const size_t packed_bytes = num_bins * sizeof(double);
    // Field 2: tag (1 byte) + varint(packed_bytes) + packed_bytes
    size_t len = 1 + varint_size(packed_bytes) + packed_bytes;
    if (store.offset() != 0) {
        // Field 3: tag (1 byte) + zigzag varint(offset)
        len += 1 + varint_size(zigzag_encode_sint32(store.offset()));
    }
    return len;
}

uint8_t* serialize_store(uint8_t* p, uint8_t field_tag, const CollapsingLowestDenseStore& store) {
    if (store.is_empty()) {
        return p;
    }
    const size_t body_len = store_body_serialized_size(store);
    *p++ = field_tag;
    p = write_varint(p, body_len);

    // Field 2: packed repeated double contiguous_bin_counts (tag 0x12)
    const auto& bins = store.bins();
    const size_t packed_bytes = bins.size() * sizeof(double);
    *p++ = 0x12;
    p = write_varint(p, packed_bytes);
    for (uint64_t count : bins) {
        p = write_double_le(p, static_cast<double>(count));
    }

    // Field 3: sint32 contiguous_bin_index_offset (tag 0x18), omitted if 0
    if (store.offset() != 0) {
        *p++ = 0x18;
        p = write_varint(p, zigzag_encode_sint32(store.offset()));
    }
    return p;
}

Status parse_index_mapping(const uint8_t* p, const uint8_t* end, double* out_gamma, double* out_offset) {
    double gamma = 0.0;
    double index_offset = 0.0;
    uint64_t interpolation = 0;

    while (p < end) {
        uint64_t tag = 0;
        Status st = read_varint(p, end, &tag);
        if (!st.ok()) {
            return st;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 0) {
            return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
        }
        if (wire_type == 3 || wire_type == 4 || wire_type == 6 || wire_type == 7) {
            return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
        }

        if (field_num == 1) {
            if (wire_type != 1) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping.gamma");
            }
            st = read_double_le(p, end, &gamma);
            if (!st.ok()) {
                return st;
            }
        } else if (field_num == 2) {
            if (wire_type != 1) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping.index_offset");
            }
            st = read_double_le(p, end, &index_offset);
            if (!st.ok()) {
                return st;
            }
        } else if (field_num == 3) {
            if (wire_type != 0) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping.interpolation");
            }
            st = read_varint(p, end, &interpolation);
            if (!st.ok()) {
                return st;
            }
        } else {
            st = skip_unknown_field(wire_type, p, end);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (interpolation != 0) {
        return Status::InvalidArgument(
                "ddsketch: unsupported IndexMapping interpolation mode; only NONE (logarithmic) is supported");
    }

    constexpr double kMinGamma = (1.0 + DDSketch::kMinRelativeAccuracy) / (1.0 - DDSketch::kMinRelativeAccuracy) - 1e-9;
    constexpr double kMaxGamma = (1.0 + DDSketch::kMaxRelativeAccuracy) / (1.0 - DDSketch::kMaxRelativeAccuracy) + 1e-9;
    if (!std::isfinite(gamma) || gamma < kMinGamma || gamma > kMaxGamma) {
        return Status::InvalidArgument("ddsketch: IndexMapping gamma out of valid range");
    }
    if (!std::isfinite(index_offset) || index_offset < -1e6 || index_offset > 1e6) {
        return Status::InvalidArgument("ddsketch: IndexMapping index_offset out of valid range");
    }

    *out_gamma = gamma;
    *out_offset = index_offset;
    return Status::OK();
}

Status parse_store_submessage(const uint8_t* p, const uint8_t* end, uint32_t* total_wire_entries,
                              std::vector<std::pair<int32_t, uint64_t>>* entries) {
    std::vector<uint64_t> contiguous_counts;
    int32_t contiguous_offset = 0;
    bool has_contiguous_offset = false;

    while (p < end) {
        uint64_t tag = 0;
        Status st = read_varint(p, end, &tag);
        if (!st.ok()) {
            return st;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 0) {
            return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
        }
        if (wire_type == 3 || wire_type == 4 || wire_type == 6 || wire_type == 7) {
            return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
        }

        if (field_num == 1) {
            // map<sint32, double> bin_counts entry
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for Store.bin_counts");
            }
            uint64_t entry_len = 0;
            st = read_varint(p, end, &entry_len);
            if (!st.ok()) {
                return st;
            }
            if (entry_len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            if (*total_wire_entries >= DDSketch::kMaxWireBins) {
                return Status::InvalidArgument("ddsketch: wire store exceeds maximum allowed bin entries (65536)");
            }
            ++(*total_wire_entries);

            const uint8_t* ep = p;
            const uint8_t* eend = p + entry_len;
            p = eend;

            int32_t bin_key = 0;
            double bin_val = 0.0;
            while (ep < eend) {
                uint64_t etag = 0;
                st = read_varint(ep, eend, &etag);
                if (!st.ok()) {
                    return st;
                }
                const uint32_t efield = static_cast<uint32_t>(etag >> 3);
                const uint32_t ewt = static_cast<uint32_t>(etag & 0x07);
                if (efield == 0) {
                    return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
                }
                if (ewt == 3 || ewt == 4 || ewt == 6 || ewt == 7) {
                    return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
                }
                if (efield == 1) {
                    if (ewt != 0) {
                        return Status::InvalidArgument("ddsketch: invalid wire type for map key");
                    }
                    uint64_t raw_key = 0;
                    st = read_varint(ep, eend, &raw_key);
                    if (!st.ok()) {
                        return st;
                    }
                    if (raw_key > std::numeric_limits<uint32_t>::max()) {
                        return Status::InvalidArgument("ddsketch: bin index out of range");
                    }
                    bin_key = zigzag_decode_sint32(static_cast<uint32_t>(raw_key));
                } else if (efield == 2) {
                    if (ewt != 1) {
                        return Status::InvalidArgument("ddsketch: invalid wire type for map value");
                    }
                    st = read_double_le(ep, eend, &bin_val);
                    if (!st.ok()) {
                        return st;
                    }
                } else {
                    st = skip_unknown_field(ewt, ep, eend);
                    if (!st.ok()) {
                        return st;
                    }
                }
            }

            if (bin_key < DDSketch::kMinBinIndex || bin_key > DDSketch::kMaxBinIndex) {
                return Status::InvalidArgument("ddsketch: bin index out of range");
            }
            uint64_t ucount = 0;
            if (!validate_wire_count(bin_val, &ucount)) {
                return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
            }
            if (ucount > 0) {
                entries->emplace_back(bin_key, ucount);
            }
        } else if (field_num == 2) {
            if (wire_type == 2) {
                // Packed repeated double contiguous_bin_counts
                uint64_t byte_len = 0;
                st = read_varint(p, end, &byte_len);
                if (!st.ok()) {
                    return st;
                }
                if (byte_len > static_cast<uint64_t>(end - p)) {
                    return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
                }
                if (byte_len % sizeof(double) != 0) {
                    return Status::InvalidArgument(
                            "ddsketch: packed contiguous_bin_counts length must be multiple of 8");
                }
                const uint64_t num_bins = byte_len / sizeof(double);
                if (num_bins > DDSketch::kMaxWireBins - *total_wire_entries) {
                    return Status::InvalidArgument("ddsketch: wire store exceeds maximum allowed bin entries (65536)");
                }
                *total_wire_entries += static_cast<uint32_t>(num_bins);
                contiguous_counts.reserve(contiguous_counts.size() + static_cast<size_t>(num_bins));
                for (uint64_t i = 0; i < num_bins; ++i) {
                    double d = 0.0;
                    std::memcpy(&d, p, sizeof(double));
                    p += sizeof(double);
                    uint64_t ucount = 0;
                    if (!validate_wire_count(d, &ucount)) {
                        return Status::InvalidArgument(
                                "ddsketch: bin count must be an exact non-negative integer <= 2^53");
                    }
                    contiguous_counts.push_back(ucount);
                }
            } else if (wire_type == 1) {
                // Unpacked repeated double contiguous_bin_counts
                if (*total_wire_entries >= DDSketch::kMaxWireBins) {
                    return Status::InvalidArgument("ddsketch: wire store exceeds maximum allowed bin entries (65536)");
                }
                ++(*total_wire_entries);
                double d = 0.0;
                st = read_double_le(p, end, &d);
                if (!st.ok()) {
                    return st;
                }
                uint64_t ucount = 0;
                if (!validate_wire_count(d, &ucount)) {
                    return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
                }
                contiguous_counts.push_back(ucount);
            } else {
                return Status::InvalidArgument("ddsketch: invalid wire type for Store.contiguous_bin_counts");
            }
        } else if (field_num == 3) {
            if (wire_type != 0) {
                return Status::InvalidArgument("ddsketch: invalid wire type for Store.contiguous_bin_index_offset");
            }
            uint64_t raw_offset = 0;
            st = read_varint(p, end, &raw_offset);
            if (!st.ok()) {
                return st;
            }
            if (raw_offset > std::numeric_limits<uint32_t>::max()) {
                return Status::InvalidArgument("ddsketch: bin index out of range");
            }
            contiguous_offset = zigzag_decode_sint32(static_cast<uint32_t>(raw_offset));
            has_contiguous_offset = true;
        } else {
            st = skip_unknown_field(wire_type, p, end);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (has_contiguous_offset &&
        (contiguous_offset < DDSketch::kMinBinIndex || contiguous_offset > DDSketch::kMaxBinIndex)) {
        return Status::InvalidArgument("ddsketch: bin index out of range");
    }

    for (size_t i = 0; i < contiguous_counts.size(); ++i) {
        const int64_t idx = static_cast<int64_t>(contiguous_offset) + static_cast<int64_t>(i);
        if (idx < DDSketch::kMinBinIndex || idx > DDSketch::kMaxBinIndex) {
            return Status::InvalidArgument("ddsketch: bin index out of range");
        }
        if (contiguous_counts[i] > 0) {
            entries->emplace_back(static_cast<int32_t>(idx), contiguous_counts[i]);
        }
    }
    return Status::OK();
}

Status build_store_from_entries(std::vector<std::pair<int32_t, uint64_t>>* entries,
                                CollapsingLowestDenseStore* out_store) {
    out_store->clear();
    if (entries->empty()) {
        return Status::OK();
    }
    std::sort(entries->begin(), entries->end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& kv : *entries) {
        if (out_store->total_count() > DDSketch::kMaxExactDoubleCount - kv.second) {
            return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
        }
        out_store->add(kv.first, kv.second);
    }
    out_store->trim();
    return Status::OK();
}

} // namespace

// ============================================================================
// CollapsingLowestDenseStore
// ============================================================================

void CollapsingLowestDenseStore::clear() {
    _offset = 0;
    _total_count = 0;
    std::vector<uint64_t>().swap(_bins);
}

void CollapsingLowestDenseStore::trim() {
    if (_total_count == 0 || _bins.empty()) {
        clear();
        return;
    }
    size_t first = 0;
    while (first < _bins.size() && _bins[first] == 0) {
        ++first;
    }
    if (first == _bins.size()) {
        clear();
        return;
    }
    size_t last = _bins.size() - 1;
    while (last > first && _bins[last] == 0) {
        --last;
    }
    if (first > 0) {
        const size_t new_len = last - first + 1;
        std::memmove(_bins.data(), _bins.data() + first, new_len * sizeof(uint64_t));
        _bins.resize(new_len);
        _offset = static_cast<int32_t>(static_cast<int64_t>(_offset) + static_cast<int64_t>(first));
    } else if (last + 1 < _bins.size()) {
        _bins.resize(last + 1);
    }
}

void CollapsingLowestDenseStore::add(int32_t index, uint64_t count) {
    if (count == 0) {
        return;
    }
    if (_bins.empty()) {
        _offset = index;
        _total_count = count;
        if (_bins.capacity() == 0) {
            _bins.reserve(1);
        }
        _bins.assign(1, count);
        return;
    }

    const int64_t cur_min = _offset;
    const int64_t cur_max = static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1;
    const int64_t idx = index;

    if (idx >= cur_min && idx <= cur_max) {
        _bins[static_cast<size_t>(idx - cur_min)] += count;
        _total_count += count;
        return;
    }

    const int64_t max_bins_i64 = static_cast<int64_t>(_max_bins);
    if (idx < cur_min) {
        const int64_t new_min = std::max(idx, cur_max - max_bins_i64 + 1);
        if (new_min == cur_min) {
            _bins[0] += count;
            _total_count += count;
            return;
        }
        const size_t shift = static_cast<size_t>(cur_min - new_min);
        const size_t old_len = _bins.size();
        const size_t new_len = old_len + shift;
        if (new_len > _bins.capacity()) {
            _bins.reserve(stepped_capacity(new_len, _max_bins));
        }
        _bins.resize(new_len, 0);
        std::memmove(_bins.data() + shift, _bins.data(), old_len * sizeof(uint64_t));
        std::memset(_bins.data(), 0, shift * sizeof(uint64_t));
        _offset = static_cast<int32_t>(new_min);
        _bins[0] += count;
        _total_count += count;
        return;
    }

    // idx > cur_max
    const int64_t new_min = std::max(cur_min, idx - max_bins_i64 + 1);
    const size_t new_len = static_cast<size_t>(idx - new_min + 1);
    if (new_len > _bins.capacity()) {
        _bins.reserve(stepped_capacity(new_len, _max_bins));
    }

    if (new_min > cur_max) {
        const uint64_t collapsed_sum = _total_count;
        _bins.assign(new_len, 0);
        _offset = static_cast<int32_t>(new_min);
        _bins[0] += collapsed_sum;
        _bins[new_len - 1] += count;
        _total_count += count;
    } else if (new_min > cur_min) {
        const size_t drop = static_cast<size_t>(new_min - cur_min);
        uint64_t collapsed_sum = 0;
        for (size_t i = 0; i <= drop; ++i) {
            collapsed_sum += _bins[i];
        }
        const size_t kept = _bins.size() - drop;
        _bins[drop] = collapsed_sum;
        std::memmove(_bins.data(), _bins.data() + drop, kept * sizeof(uint64_t));
        _bins.resize(new_len, 0);
        if (new_len > kept) {
            std::memset(_bins.data() + kept, 0, (new_len - kept) * sizeof(uint64_t));
        }
        _offset = static_cast<int32_t>(new_min);
        _bins[new_len - 1] += count;
        _total_count += count;
    } else {
        _bins.resize(new_len, 0);
        _bins[new_len - 1] += count;
        _total_count += count;
    }
}

void CollapsingLowestDenseStore::merge(const CollapsingLowestDenseStore& other) {
    if (other.is_empty()) {
        return;
    }
    if (is_empty()) {
        if (other._bins.size() <= _max_bins) {
            _offset = other._offset;
            _total_count = other._total_count;
            const size_t n = other._bins.size();
            if (n > _bins.capacity()) {
                _bins.reserve(stepped_capacity(n, _max_bins));
            }
            _bins.assign(other._bins.begin(), other._bins.end());
            trim();
            return;
        }
        for (size_t i = 0; i < other._bins.size(); ++i) {
            if (other._bins[i] > 0) {
                add(static_cast<int32_t>(static_cast<int64_t>(other._offset) + static_cast<int64_t>(i)),
                    other._bins[i]);
            }
        }
        trim();
        return;
    }

    const int64_t a_min = _offset;
    const int64_t a_max = static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1;
    const int64_t b_min = other._offset;
    const int64_t b_max = static_cast<int64_t>(other._offset) + static_cast<int64_t>(other._bins.size()) - 1;

    const int64_t max_idx = std::max(a_max, b_max);
    const int64_t min_idx = std::min(a_min, b_min);
    const int64_t max_bins_i64 = static_cast<int64_t>(_max_bins);
    const int64_t new_min = std::max(min_idx, max_idx - max_bins_i64 + 1);
    const size_t new_len = static_cast<size_t>(max_idx - new_min + 1);

    if (new_len > _bins.capacity()) {
        _bins.reserve(stepped_capacity(new_len, _max_bins));
    }

    if (new_min > a_max) {
        const uint64_t collapsed_sum = _total_count;
        _bins.assign(new_len, 0);
        _bins[0] = collapsed_sum;
    } else if (new_min > a_min) {
        const size_t drop = static_cast<size_t>(new_min - a_min);
        uint64_t collapsed_sum = 0;
        for (size_t i = 0; i <= drop; ++i) {
            collapsed_sum += _bins[i];
        }
        const size_t kept = _bins.size() - drop;
        _bins[drop] = collapsed_sum;
        std::memmove(_bins.data(), _bins.data() + drop, kept * sizeof(uint64_t));
        _bins.resize(new_len, 0);
        if (new_len > kept) {
            std::memset(_bins.data() + kept, 0, (new_len - kept) * sizeof(uint64_t));
        }
    } else {
        const size_t shift = static_cast<size_t>(a_min - new_min);
        const size_t old_len = _bins.size();
        _bins.resize(new_len, 0);
        if (shift > 0) {
            std::memmove(_bins.data() + shift, _bins.data(), old_len * sizeof(uint64_t));
            std::memset(_bins.data(), 0, shift * sizeof(uint64_t));
        }
    }
    _offset = static_cast<int32_t>(new_min);

    for (size_t i = 0; i < other._bins.size(); ++i) {
        const uint64_t c = other._bins[i];
        if (c == 0) {
            continue;
        }
        const int64_t idx = b_min + static_cast<int64_t>(i);
        const size_t pos = (idx <= new_min) ? 0 : static_cast<size_t>(idx - new_min);
        _bins[pos] += c;
    }
    _total_count += other._total_count;
    trim();
}

// ============================================================================
// DDSketch
// ============================================================================

DDSketch::DDSketch() {
    const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    _init_mapping_from_gamma(default_gamma, 0.0);
    _mapping_initialized = false;
}

DDSketch::DDSketch(double relative_accuracy, uint32_t max_bins) {
    reinit(relative_accuracy, max_bins);
}

Status DDSketch::validate_relative_accuracy(double relative_accuracy) {
    if (!std::isfinite(relative_accuracy) || relative_accuracy < kMinRelativeAccuracy ||
        relative_accuracy > kMaxRelativeAccuracy) {
        return Status::InvalidArgument("ddsketch relative_accuracy must be a constant in [0.001, 0.5]");
    }
    return Status::OK();
}

void DDSketch::_init_mapping_from_gamma(double gamma, double index_offset) {
    _gamma = gamma;
    _index_offset = index_offset;
    _multiplier = 1.0 / std::log(_gamma);
    const double min_exp = std::exp((static_cast<double>(kMinBinIndex) - _index_offset) / _multiplier);
    _min_indexable_value = std::max(std::numeric_limits<double>::min() * _gamma, min_exp);
    const double max_exp = std::exp((static_cast<double>(kMaxBinIndex) - _index_offset) / _multiplier);
    _max_indexable_value = std::min(std::numeric_limits<double>::max() / _gamma, max_exp);
}

void DDSketch::_ensure_default_mapping() {
    if (!_mapping_initialized) {
        const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
        _init_mapping_from_gamma(default_gamma, 0.0);
        _mapping_initialized = true;
    }
}

void DDSketch::reinit(double relative_accuracy, uint32_t max_bins) {
    double acc = relative_accuracy;
    if (!std::isfinite(acc) || acc < kMinRelativeAccuracy || acc > kMaxRelativeAccuracy) {
        acc = kDefaultRelativeAccuracy;
    }
    const double gamma = (1.0 + acc) / (1.0 - acc);
    _init_mapping_from_gamma(gamma, 0.0);
    _mapping_initialized = true;
    _zero_count = 0;
    _positive_store = CollapsingLowestDenseStore(max_bins);
    _negative_store = CollapsingLowestDenseStore(max_bins);
}

void DDSketch::clear() {
    const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    _init_mapping_from_gamma(default_gamma, 0.0);
    _mapping_initialized = false;
    _zero_count = 0;
    _positive_store.clear();
    _negative_store.clear();
}

int32_t DDSketch::key(double value) const {
    const double abs_val = std::abs(value);
    if (abs_val > _max_indexable_value) {
        return kMaxBinIndex;
    }
    if (abs_val < _min_indexable_value) {
        return kMinBinIndex;
    }
    double scaled = std::log(abs_val) * _multiplier;
    if (_index_offset != 0.0) {
        scaled += _index_offset;
    }
    const double ceiled = std::ceil(scaled);
    if (ceiled <= static_cast<double>(kMinBinIndex)) {
        return kMinBinIndex;
    }
    if (ceiled >= static_cast<double>(kMaxBinIndex)) {
        return kMaxBinIndex;
    }
    return static_cast<int32_t>(ceiled);
}

double DDSketch::bin_value(int32_t index) const {
    return (2.0 / (1.0 + _gamma)) * std::exp((static_cast<double>(index) - _index_offset) / _multiplier);
}

void DDSketch::add(double value) {
    if (!std::isfinite(value)) {
        return;
    }
    _ensure_default_mapping();
    if (value >= _min_indexable_value) {
        _positive_store.add(key(value), 1);
    } else if (value <= -_min_indexable_value) {
        _negative_store.add(key(-value), 1);
    } else {
        _zero_count += 1;
    }
}

Status DDSketch::add_weighted(double value, int64_t weight) {
    if (weight <= 0) {
        return Status::OK();
    }
    const uint64_t uweight = static_cast<uint64_t>(weight);
    if (uweight > kMaxExactDoubleCount) {
        return Status::InvalidArgument("ddsketch weight exceeds 2^53");
    }
    if (!std::isfinite(value)) {
        return Status::OK();
    }
    if (total_count() > kMaxExactDoubleCount - uweight) {
        return Status::InvalidArgument("ddsketch: total count exceeds 2^53");
    }
    _ensure_default_mapping();
    if (value >= _min_indexable_value) {
        _positive_store.add(key(value), uweight);
    } else if (value <= -_min_indexable_value) {
        _negative_store.add(key(-value), uweight);
    } else {
        _zero_count += uweight;
    }
    return Status::OK();
}

void DDSketch::add(double value, double weight) {
    if (!std::isfinite(value) || !std::isfinite(weight) || weight <= 0.0 ||
        weight > static_cast<double>(kMaxExactDoubleCount)) {
        return;
    }
    const uint64_t uweight = static_cast<uint64_t>(weight);
    if (uweight == 0 || total_count() > kMaxExactDoubleCount - uweight) {
        return;
    }
    _ensure_default_mapping();
    if (value >= _min_indexable_value) {
        _positive_store.add(key(value), uweight);
    } else if (value <= -_min_indexable_value) {
        _negative_store.add(key(-value), uweight);
    } else {
        _zero_count += uweight;
    }
}

Status DDSketch::merge(const DDSketch& other) {
    if (!_mapping_initialized) {
        if (!other._mapping_initialized) {
            return Status::OK();
        }
        _init_mapping_from_gamma(other._gamma, other._index_offset);
        _mapping_initialized = true;
        _zero_count = other._zero_count;
        _positive_store.merge(other._positive_store);
        _negative_store.merge(other._negative_store);
        return Status::OK();
    }

    if (!other._mapping_initialized) {
        return Status::OK();
    }

    const bool gamma_compatible = std::abs(_gamma - other._gamma) <= 1e-12 * _gamma;
    const bool offset_compatible = std::abs(_index_offset - other._index_offset) <= 1e-12;
    if (!gamma_compatible || !offset_compatible) {
        return Status::InvalidArgument(
                "ddsketch_combine: cannot merge sketches with incompatible IndexMapping (gamma/offset mismatch)");
    }

    const uint64_t other_total = other.total_count();
    if (other_total > kMaxExactDoubleCount || total_count() > kMaxExactDoubleCount - other_total) {
        return Status::InvalidArgument("ddsketch: merged total count exceeds 2^53");
    }

    _zero_count += other._zero_count;
    _positive_store.merge(other._positive_store);
    _negative_store.merge(other._negative_store);
    return Status::OK();
}

double DDSketch::quantile(double q) const {
    const uint64_t n = total_count();
    if (n == 0 || std::isnan(q) || q < 0.0 || q > 1.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double rank = q * static_cast<double>(n - 1);
    uint64_t cumulative = 0;

    // 1. Traverse negative store from highest magnitude bin down to 0 (most negative to least negative)
    const auto& neg_bins = _negative_store.bins();
    const int32_t neg_offset = _negative_store.offset();
    for (size_t idx = neg_bins.size(); idx > 0; --idx) {
        const size_t i = idx - 1;
        cumulative += neg_bins[i];
        if (static_cast<double>(cumulative) > rank) {
            return -bin_value(static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i)));
        }
    }

    // 2. Zero count (always returns positive +0.0)
    cumulative += _zero_count;
    if (static_cast<double>(cumulative) > rank) {
        return 0.0;
    }

    // 3. Traverse positive store from 0 up to bins.size() - 1
    const auto& pos_bins = _positive_store.bins();
    const int32_t pos_offset = _positive_store.offset();
    for (size_t i = 0; i < pos_bins.size(); ++i) {
        cumulative += pos_bins[i];
        if (static_cast<double>(cumulative) > rank) {
            return bin_value(static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i)));
        }
    }

    if (!pos_bins.empty()) {
        return bin_value(
                static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(pos_bins.size() - 1)));
    }
    if (_zero_count > 0) {
        return 0.0;
    }
    return -bin_value(neg_offset);
}

double DDSketch::cdf(double value) const {
    const uint64_t n = total_count();
    if (n == 0 || std::isnan(value)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value == -std::numeric_limits<double>::infinity()) {
        return 0.0;
    }
    if (value == std::numeric_limits<double>::infinity()) {
        return 1.0;
    }

    uint64_t le_count = 0;
    if (value < 0.0) {
        const auto& neg_bins = _negative_store.bins();
        const int32_t neg_offset = _negative_store.offset();
        for (size_t i = 0; i < neg_bins.size(); ++i) {
            if (neg_bins[i] == 0) {
                continue;
            }
            const int32_t bin_idx = static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i));
            if (-bin_value(bin_idx) <= value) {
                le_count += neg_bins[i];
            }
        }
    } else if (value < _min_indexable_value) {
        le_count = _negative_store.total_count() + _zero_count;
    } else {
        le_count = _negative_store.total_count() + _zero_count;
        const auto& pos_bins = _positive_store.bins();
        const int32_t pos_offset = _positive_store.offset();
        for (size_t i = 0; i < pos_bins.size(); ++i) {
            if (pos_bins[i] == 0) {
                continue;
            }
            const int32_t bin_idx = static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i));
            if (bin_value(bin_idx) <= value) {
                le_count += pos_bins[i];
            }
        }
    }

    return static_cast<double>(le_count) / static_cast<double>(n);
}

double DDSketch::sum() const {
    if (is_empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double total = 0.0;
    const auto& neg_bins = _negative_store.bins();
    const int32_t neg_offset = _negative_store.offset();
    for (size_t idx = neg_bins.size(); idx > 0; --idx) {
        const size_t i = idx - 1;
        if (neg_bins[i] == 0) {
            continue;
        }
        const int32_t bin_idx = static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i));
        total -= bin_value(bin_idx) * static_cast<double>(neg_bins[i]);
    }

    const auto& pos_bins = _positive_store.bins();
    const int32_t pos_offset = _positive_store.offset();
    for (size_t i = 0; i < pos_bins.size(); ++i) {
        if (pos_bins[i] == 0) {
            continue;
        }
        const int32_t bin_idx = static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i));
        total += bin_value(bin_idx) * static_cast<double>(pos_bins[i]);
    }

    if (!std::isfinite(total)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return total;
}

double DDSketch::avg() const {
    const uint64_t n = total_count();
    if (n == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double s = sum();
    if (!std::isfinite(s)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return s / static_cast<double>(n);
}

uint64_t DDSketch::serialize_size() const {
    const double eff_offset = _mapping_initialized ? _index_offset : 0.0;
    const size_t mapping_body_len = (eff_offset != 0.0) ? 18 : 9;
    // Field 1: tag (1) + varint(mapping_body_len) (1) + mapping_body_len
    uint64_t total_len = 1 + varint_size(mapping_body_len) + mapping_body_len;

    if (!_positive_store.is_empty()) {
        const size_t body_len = store_body_serialized_size(_positive_store);
        total_len += 1 + varint_size(body_len) + body_len;
    }
    if (!_negative_store.is_empty()) {
        const size_t body_len = store_body_serialized_size(_negative_store);
        total_len += 1 + varint_size(body_len) + body_len;
    }
    if (_zero_count > 0) {
        total_len += 1 + sizeof(double);
    }
    return total_len;
}

size_t DDSketch::serialize(uint8_t* writer) const {
    uint8_t* p = writer;
    const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    const double eff_gamma = _mapping_initialized ? _gamma : default_gamma;
    const double eff_offset = _mapping_initialized ? _index_offset : 0.0;
    const size_t mapping_body_len = (eff_offset != 0.0) ? 18 : 9;

    // Field 1: IndexMapping (tag 0x0a)
    *p++ = 0x0a;
    p = write_varint(p, mapping_body_len);
    *p++ = 0x09; // Field 1: double gamma
    p = write_double_le(p, eff_gamma);
    if (eff_offset != 0.0) {
        *p++ = 0x11; // Field 2: double index_offset
        p = write_double_le(p, eff_offset);
    }

    // Field 2: Store positive_values (tag 0x12)
    p = serialize_store(p, 0x12, _positive_store);

    // Field 3: Store negative_values (tag 0x1a)
    p = serialize_store(p, 0x1a, _negative_store);

    // Field 4: double zero_count (tag 0x21)
    if (_zero_count > 0) {
        *p++ = 0x21;
        p = write_double_le(p, static_cast<double>(_zero_count));
    }

    return static_cast<size_t>(p - writer);
}

Status DDSketch::deserialize(const Slice& src) {
    if (src.empty()) {
        clear();
        return Status::OK();
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(src.data);
    const uint8_t* end = p + src.size;

    bool has_mapping = false;
    double parsed_gamma = 0.0;
    double parsed_offset = 0.0;
    uint64_t parsed_zero_count = 0;

    uint32_t pos_wire_entries = 0;
    uint32_t neg_wire_entries = 0;
    std::vector<std::pair<int32_t, uint64_t>> pos_entries;
    std::vector<std::pair<int32_t, uint64_t>> neg_entries;

    while (p < end) {
        uint64_t tag = 0;
        Status st = read_varint(p, end, &tag);
        if (!st.ok()) {
            return st;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 0) {
            return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
        }
        if (wire_type == 3 || wire_type == 4 || wire_type == 6 || wire_type == 7) {
            return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
        }

        if (field_num == 1) {
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping");
            }
            uint64_t len = 0;
            st = read_varint(p, end, &len);
            if (!st.ok()) {
                return st;
            }
            if (len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            st = parse_index_mapping(p, p + len, &parsed_gamma, &parsed_offset);
            if (!st.ok()) {
                return st;
            }
            has_mapping = true;
            p += len;
        } else if (field_num == 2) {
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for positive_values");
            }
            uint64_t len = 0;
            st = read_varint(p, end, &len);
            if (!st.ok()) {
                return st;
            }
            if (len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            st = parse_store_submessage(p, p + len, &pos_wire_entries, &pos_entries);
            if (!st.ok()) {
                return st;
            }
            p += len;
        } else if (field_num == 3) {
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for negative_values");
            }
            uint64_t len = 0;
            st = read_varint(p, end, &len);
            if (!st.ok()) {
                return st;
            }
            if (len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            st = parse_store_submessage(p, p + len, &neg_wire_entries, &neg_entries);
            if (!st.ok()) {
                return st;
            }
            p += len;
        } else if (field_num == 4) {
            if (wire_type != 1) {
                return Status::InvalidArgument("ddsketch: invalid wire type for zero_count");
            }
            double zc = 0.0;
            st = read_double_le(p, end, &zc);
            if (!st.ok()) {
                return st;
            }
            if (!validate_wire_count(zc, &parsed_zero_count)) {
                return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
            }
        } else {
            st = skip_unknown_field(wire_type, p, end);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (!has_mapping) {
        return Status::InvalidArgument("ddsketch: missing IndexMapping");
    }

    CollapsingLowestDenseStore new_pos(_positive_store.max_bins());
    CollapsingLowestDenseStore new_neg(_negative_store.max_bins());
    Status st = build_store_from_entries(&pos_entries, &new_pos);
    if (!st.ok()) {
        return st;
    }
    st = build_store_from_entries(&neg_entries, &new_neg);
    if (!st.ok()) {
        return st;
    }

    if (new_pos.total_count() > kMaxExactDoubleCount - parsed_zero_count ||
        new_neg.total_count() > kMaxExactDoubleCount - parsed_zero_count - new_pos.total_count()) {
        return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
    }

    _init_mapping_from_gamma(parsed_gamma, parsed_offset);
    _mapping_initialized = true;
    _zero_count = parsed_zero_count;
    _positive_store = std::move(new_pos);
    _negative_store = std::move(new_neg);
    return Status::OK();
}

} // namespace starrocks
