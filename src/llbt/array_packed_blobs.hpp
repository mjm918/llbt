/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Copyright (c) 2026 Mohammad Julfikar
 *************************************************************************/

#ifndef LLBT_ARRAY_PACKED_BLOBS_HPP
#define LLBT_ARRAY_PACKED_BLOBS_HPP

#include <llbt/array.hpp>
#include <llbt/binary_data.hpp>

namespace llbt {

// Append-oriented binary leaf. Values live in immutable packed blocks. The
// small mutable arrays contain logical order and replacement locations, so an
// update or erase never copies a packed base block.
class ArrayPackedBlobs : public Array {
public:
    explicit ArrayPackedBlobs(Allocator&) noexcept;

    ArrayPackedBlobs& operator=(const ArrayPackedBlobs&) = delete;
    ArrayPackedBlobs(const ArrayPackedBlobs&) = delete;

    static constexpr size_t target_block_size = 256 * 1024;
    static constexpr size_t max_inline_block_size = 1024 * 1024;

    static bool is_packed(const char* header) noexcept;
    static bool can_pack(const BinaryData* values, size_t count) noexcept;
    static BinaryData get(const char* header, size_t ndx, Allocator&) noexcept;

    void create(const BinaryData* values, size_t count);
    void init_from_mem(MemRef) noexcept;
    void init_from_ref(ref_type ref) noexcept;
    void init_from_parent() noexcept;

    size_t size() const noexcept;
    BinaryData get(size_t ndx) const noexcept;
    BinaryData get_at(size_t ndx, size_t& pos) const noexcept;
    bool is_null(size_t ndx) const noexcept;

    void add(BinaryData value);
    void append(const BinaryData* values, size_t count);
    void insert(size_t ndx, BinaryData value);
    void set(size_t ndx, BinaryData value);
    void set_many(const size_t* positions, const BinaryData* values, size_t count, size_t position_base);
    void erase(size_t ndx);
    void truncate(size_t new_size);
    void clear();
    void destroy() noexcept;

    size_t find_first(BinaryData value, size_t begin, size_t end) const noexcept;
    bool needs_normalize() const noexcept;
    void verify() const;

private:
    static constexpr uint64_t packed_magic = 0x4c4c42545041434bULL; // "LLBTPACK"
    static constexpr uint32_t block_magic = 0x4b4c4250U;            // "PBLK"
    static constexpr uint16_t block_version = 1;
    static constexpr uint32_t null_length = 0xffffffffU;
    static constexpr size_t block_header_size = 16;
    static constexpr size_t directory_entry_size = 8;

    Array m_base_blocks;
    Array m_base_ends;
    Array m_delta_blocks;
    Array m_overrides;
    Array m_order;

    static MemRef create_block(Allocator&, const BinaryData* values, size_t count);
    static BinaryData get_from_block(const char* header, size_t row) noexcept;
    static size_t block_count(const char* header) noexcept;
    BinaryData get_slot(size_t slot) const noexcept;
    size_t append_delta_block(const BinaryData* values, size_t count);
    void append_base_blocks(const BinaryData* values, size_t count);
};

} // namespace llbt

#endif // LLBT_ARRAY_PACKED_BLOBS_HPP
