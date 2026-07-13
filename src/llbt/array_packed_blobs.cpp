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

#include <llbt/array_packed_blobs.hpp>

#include <llbt/array_blob.hpp>
#include <llbt/impl/destroy_guard.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace llbt;

namespace {

uint16_t read_u16(const char* p) noexcept
{
    return uint16_t(uint8_t(p[0])) | (uint16_t(uint8_t(p[1])) << 8);
}

uint32_t read_u32(const char* p) noexcept
{
    return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) |
           (uint32_t(uint8_t(p[2])) << 16) | (uint32_t(uint8_t(p[3])) << 24);
}

void write_u16(char* p, uint16_t value) noexcept
{
    p[0] = char(value);
    p[1] = char(value >> 8);
}

void write_u32(char* p, uint32_t value) noexcept
{
    p[0] = char(value);
    p[1] = char(value >> 8);
    p[2] = char(value >> 16);
    p[3] = char(value >> 24);
}

size_t block_end(const BinaryData* values, size_t count, size_t begin)
{
    size_t used = ArrayPackedBlobs::target_block_size == 0 ? 0 : 16;
    size_t end = begin;
    while (end < count) {
        const size_t value_size = values[end].size();
        const size_t need = 8 + value_size;
        const bool standalone = value_size > ArrayPackedBlobs::max_inline_block_size;
        if (end != begin && (standalone || used + need > ArrayPackedBlobs::target_block_size))
            break;
        if (end == begin && standalone)
            return begin + 1;
        used += need;
        ++end;
    }
    return end;
}

} // anonymous namespace

ArrayPackedBlobs::ArrayPackedBlobs(Allocator& alloc) noexcept
    : Array(alloc)
    , m_base_blocks(alloc)
    , m_base_ends(alloc)
    , m_delta_blocks(alloc)
    , m_overrides(alloc)
    , m_order(alloc)
{
    m_base_blocks.set_parent(this, 1);
    m_base_ends.set_parent(this, 2);
    m_delta_blocks.set_parent(this, 3);
    m_overrides.set_parent(this, 4);
    m_order.set_parent(this, 5);
}

bool ArrayPackedBlobs::is_packed(const char* header) noexcept
{
    if (!Array::get_context_flag_from_header(header) || !Array::get_hasrefs_from_header(header) ||
        Array::get_size_from_header(header) != 6)
        return false;
    RefOrTagged marker = Array::get_as_ref_or_tagged(header, 0);
    return marker.is_tagged() && marker.get_as_int() == packed_magic;
}

bool ArrayPackedBlobs::can_pack(const BinaryData* values, size_t count) noexcept
{
    if (!values || count == 0)
        return false;
    constexpr size_t overhead = NodeHeader::header_size + block_header_size + directory_entry_size;
    constexpr size_t max_value = ArrayBlob::max_binary_size > overhead ? ArrayBlob::max_binary_size - overhead : 0;
    for (size_t i = 0; i < count; ++i) {
        if (values[i].size() > max_value)
            return false;
    }
    return true;
}

MemRef ArrayPackedBlobs::create_block(Allocator& alloc, const BinaryData* values, size_t count)
{
    if (count == 0 || count > std::numeric_limits<uint16_t>::max())
        throw std::invalid_argument("invalid packed block record count");

    size_t payload_size = 0;
    for (size_t i = 0; i < count; ++i) {
        if (values[i].size() > std::numeric_limits<uint32_t>::max() - payload_size)
            throw std::length_error("packed binary block is too large");
        payload_size += values[i].size();
    }
    const size_t data_size = block_header_size + count * directory_entry_size + payload_size;
    if (data_size > ArrayBlob::max_binary_size)
        throw std::length_error("packed binary block is too large");

    const size_t bytes = (NodeHeader::header_size + data_size + 7) & ~size_t(7);
    const size_t capacity = std::max(bytes, size_t(Node::initial_capacity));
    MemRef mem = alloc.alloc(capacity);
    Node::init_header(mem.get_addr(), false, false, false, NodeHeader::wtype_Ignore, 0, data_size, capacity);

    char* data = Array::get_data_from_header(mem.get_addr());
    write_u32(data + 0, block_magic);
    write_u16(data + 4, block_version);
    write_u16(data + 6, uint16_t(count));
    write_u32(data + 8, uint32_t(count * directory_entry_size));
    write_u32(data + 12, uint32_t(payload_size));

    char* directory = data + block_header_size;
    char* payload = directory + count * directory_entry_size;
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        write_u32(directory + i * directory_entry_size, uint32_t(offset));
        write_u32(directory + i * directory_entry_size + 4,
                  values[i].is_null() ? null_length : uint32_t(values[i].size()));
        if (values[i].size()) {
            std::memcpy(payload + offset, values[i].data(), values[i].size());
            offset += values[i].size();
        }
    }
    std::memset(data + data_size, 0, bytes - NodeHeader::header_size - data_size);
    return mem;
}

size_t ArrayPackedBlobs::block_count(const char* header) noexcept
{
    const char* data = Array::get_data_from_header(header);
    LLBT_ASSERT_DEBUG(read_u32(data) == block_magic);
    LLBT_ASSERT_DEBUG(read_u16(data + 4) == block_version);
    return read_u16(data + 6);
}

BinaryData ArrayPackedBlobs::get_from_block(const char* header, size_t row) noexcept
{
    const char* data = Array::get_data_from_header(header);
    LLBT_ASSERT_DEBUG(read_u32(data) == block_magic);
    LLBT_ASSERT_DEBUG(read_u16(data + 4) == block_version);
    const size_t count = read_u16(data + 6);
    LLBT_ASSERT_DEBUG(row < count);
    const size_t directory_size = read_u32(data + 8);
    const size_t payload_size = read_u32(data + 12);
    const char* entry = data + block_header_size + row * directory_entry_size;
    const size_t offset = read_u32(entry);
    const uint32_t length = read_u32(entry + 4);
    if (length == null_length)
        return {};
    LLBT_ASSERT_DEBUG(offset + length <= payload_size);
    return BinaryData(data + block_header_size + directory_size + offset, length);
}

void ArrayPackedBlobs::append_base_blocks(const BinaryData* values, size_t count)
{
    size_t total = m_base_ends.size() ? size_t(m_base_ends.back()) : 0;
    for (size_t begin = 0; begin < count;) {
        size_t end = block_end(values, count, begin);
        MemRef block = create_block(m_alloc, values + begin, end - begin);
        _impl::DeepArrayRefDestroyGuard guard(block.get_ref(), m_alloc);
        m_base_blocks.add(RefOrTagged::make_ref(block.get_ref()));
        guard.release();
        total += end - begin;
        m_base_ends.add(int64_t(total));
        begin = end;
    }
}

void ArrayPackedBlobs::create(const BinaryData* values, size_t count)
{
    if (count && !values)
        throw std::invalid_argument("null packed binary input");
    if (!can_pack(values, count))
        throw std::length_error("binary value is too large for a packed leaf");

    Array top(m_alloc);
    _impl::DeepArrayDestroyGuard top_guard(&top);
    top.create(type_HasRefs, true);
    top.add(RefOrTagged::make_tagged(packed_magic));

    Array base_blocks(m_alloc);
    _impl::DeepArrayDestroyGuard base_blocks_guard(&base_blocks);
    base_blocks.create(type_HasRefs);
    Array base_ends(m_alloc);
    _impl::DeepArrayDestroyGuard base_ends_guard(&base_ends);
    base_ends.create(type_Normal);

    // Build base blocks before attaching the child arrays to the descriptor.
    m_base_blocks.set_parent(nullptr, 0);
    m_base_ends.set_parent(nullptr, 0);
    m_base_blocks.init_from_mem(base_blocks.get_mem());
    m_base_ends.init_from_mem(base_ends.get_mem());
    append_base_blocks(values, count);

    top.add(RefOrTagged::make_ref(m_base_blocks.get_ref()));
    base_blocks_guard.release();
    top.add(RefOrTagged::make_ref(m_base_ends.get_ref()));
    base_ends_guard.release();

    Array delta_blocks(m_alloc);
    _impl::DeepArrayDestroyGuard delta_guard(&delta_blocks);
    delta_blocks.create(type_HasRefs);
    top.add(RefOrTagged::make_ref(delta_blocks.get_ref()));
    delta_guard.release();

    Array overrides(m_alloc);
    _impl::DeepArrayDestroyGuard overrides_guard(&overrides);
    overrides.create(type_Normal, false, count, 0);
    top.add(RefOrTagged::make_ref(overrides.get_ref()));
    overrides_guard.release();

    Array order(m_alloc);
    _impl::DeepArrayDestroyGuard order_guard(&order);
    order.create(type_Normal, false, count, 0);
    for (size_t i = 0; i < count; ++i)
        order.set(i, int64_t(i));
    top.add(RefOrTagged::make_ref(order.get_ref()));
    order_guard.release();

    init_from_mem(top.get_mem());
    top_guard.release();
}

void ArrayPackedBlobs::init_from_mem(MemRef mem) noexcept
{
    Array::init_from_mem(mem);
    LLBT_ASSERT_DEBUG(is_packed(mem.get_addr()));
    m_base_blocks.set_parent(this, 1);
    m_base_ends.set_parent(this, 2);
    m_delta_blocks.set_parent(this, 3);
    m_overrides.set_parent(this, 4);
    m_order.set_parent(this, 5);
    m_base_blocks.init_from_ref(get_as_ref(1));
    m_base_ends.init_from_ref(get_as_ref(2));
    m_delta_blocks.init_from_ref(get_as_ref(3));
    m_overrides.init_from_ref(get_as_ref(4));
    m_order.init_from_ref(get_as_ref(5));
}

void ArrayPackedBlobs::init_from_ref(ref_type ref) noexcept
{
    init_from_mem(MemRef(m_alloc.translate(ref), ref, m_alloc));
}

void ArrayPackedBlobs::init_from_parent() noexcept
{
    init_from_ref(get_ref_from_parent());
}

size_t ArrayPackedBlobs::size() const noexcept
{
    return m_order.size();
}

BinaryData ArrayPackedBlobs::get_slot(size_t slot) const noexcept
{
    const uint64_t encoded = uint64_t(m_overrides.get(slot));
    if (encoded) {
        const uint64_t location = encoded - 1;
        const size_t block_ndx = size_t(location >> 16);
        const size_t row = size_t(location & 0xffff);
        ref_type ref = m_delta_blocks.get_as_ref(block_ndx);
        return get_from_block(m_alloc.translate(ref), row);
    }

    size_t block_ndx = 0;
    size_t previous_end = 0;
    while (block_ndx < m_base_ends.size() && slot >= size_t(m_base_ends.get(block_ndx))) {
        previous_end = size_t(m_base_ends.get(block_ndx));
        ++block_ndx;
    }
    LLBT_ASSERT_DEBUG(block_ndx < m_base_blocks.size());
    return get_from_block(m_alloc.translate(m_base_blocks.get_as_ref(block_ndx)), slot - previous_end);
}

BinaryData ArrayPackedBlobs::get(size_t ndx) const noexcept
{
    LLBT_ASSERT_DEBUG(ndx < size());
    return get_slot(size_t(m_order.get(ndx)));
}

BinaryData ArrayPackedBlobs::get_at(size_t ndx, size_t& pos) const noexcept
{
    pos = 0;
    return get(ndx);
}

bool ArrayPackedBlobs::is_null(size_t ndx) const noexcept
{
    return get(ndx).is_null();
}

size_t ArrayPackedBlobs::append_delta_block(const BinaryData* values, size_t count)
{
    MemRef block = create_block(m_alloc, values, count);
    _impl::DeepArrayRefDestroyGuard guard(block.get_ref(), m_alloc);
    const size_t block_ndx = m_delta_blocks.size();
    m_delta_blocks.add(RefOrTagged::make_ref(block.get_ref()));
    guard.release();
    return block_ndx;
}

void ArrayPackedBlobs::append(const BinaryData* values, size_t count)
{
    if (count && !values)
        throw std::invalid_argument("null packed binary append input");
    for (size_t begin = 0; begin < count;) {
        const size_t end = block_end(values, count, begin);
        const size_t block_ndx = append_delta_block(values + begin, end - begin);
        for (size_t row = 0; begin + row < end; ++row) {
            const size_t slot = m_overrides.size();
            const uint64_t location = (uint64_t(block_ndx) << 16) | row;
            m_overrides.add(int64_t(location + 1));
            m_order.add(int64_t(slot));
        }
        begin = end;
    }
}

void ArrayPackedBlobs::add(BinaryData value)
{
    append(&value, 1);
}

void ArrayPackedBlobs::insert(size_t ndx, BinaryData value)
{
    LLBT_ASSERT_DEBUG(ndx <= size());
    const size_t block_ndx = append_delta_block(&value, 1);
    const size_t slot = m_overrides.size();
    const uint64_t location = uint64_t(block_ndx) << 16;
    m_overrides.add(int64_t(location + 1));
    m_order.insert(ndx, int64_t(slot));
}

void ArrayPackedBlobs::set(size_t ndx, BinaryData value)
{
    set_many(&ndx, &value, 1, 0);
}

void ArrayPackedBlobs::set_many(const size_t* positions, const BinaryData* values, size_t count,
                                size_t position_base)
{
    for (size_t begin = 0; begin < count;) {
        const size_t end = block_end(values, count, begin);
        const size_t block_ndx = append_delta_block(values + begin, end - begin);
        for (size_t row = 0; begin + row < end; ++row) {
            const size_t local_ndx = positions[begin + row] - position_base;
            LLBT_ASSERT_DEBUG(local_ndx < size());
            const size_t slot = size_t(m_order.get(local_ndx));
            const uint64_t location = (uint64_t(block_ndx) << 16) | row;
            m_overrides.set(slot, int64_t(location + 1));
        }
        begin = end;
    }
}

void ArrayPackedBlobs::erase(size_t ndx)
{
    LLBT_ASSERT_DEBUG(ndx < size());
    m_order.erase(ndx);
}

void ArrayPackedBlobs::truncate(size_t new_size)
{
    if (new_size < size())
        m_order.truncate(new_size);
}

void ArrayPackedBlobs::clear()
{
    m_order.clear();
}

void ArrayPackedBlobs::destroy() noexcept
{
    destroy_deep();
}

size_t ArrayPackedBlobs::find_first(BinaryData value, size_t begin, size_t end) const noexcept
{
    if (end == npos)
        end = size();
    for (size_t i = begin; i < end; ++i) {
        BinaryData candidate = get(i);
        if (candidate.is_null() != value.is_null() || candidate.size() != value.size())
            continue;
        if (candidate.is_null() || candidate.size() == 0 ||
            std::memcmp(candidate.data(), value.data(), candidate.size()) == 0)
            return i;
    }
    return not_found;
}

bool ArrayPackedBlobs::needs_normalize() const noexcept
{
    if (m_delta_blocks.size() != 0)
        return true;
    const size_t base_size = m_base_ends.size() ? size_t(m_base_ends.back()) : 0;
    if (m_order.size() != base_size || m_overrides.size() != base_size)
        return true;
    for (size_t i = 0; i < m_order.size(); ++i) {
        if (m_order.get(i) != int64_t(i) || m_overrides.get(i) != 0)
            return true;
    }
    return false;
}

BinaryData ArrayPackedBlobs::get(const char* header, size_t ndx, Allocator& alloc) noexcept
{
    const char* order = alloc.translate(to_ref(Array::get(header, 5)));
    const size_t slot = size_t(Array::get(order, ndx));
    const char* overrides = alloc.translate(to_ref(Array::get(header, 4)));
    const uint64_t encoded = uint64_t(Array::get(overrides, slot));
    if (encoded) {
        const uint64_t location = encoded - 1;
        const char* delta_blocks = alloc.translate(to_ref(Array::get(header, 3)));
        ref_type block_ref = to_ref(Array::get(delta_blocks, size_t(location >> 16)));
        return get_from_block(alloc.translate(block_ref), size_t(location & 0xffff));
    }

    const char* base_blocks = alloc.translate(to_ref(Array::get(header, 1)));
    const char* base_ends = alloc.translate(to_ref(Array::get(header, 2)));
    size_t block_ndx = 0;
    size_t previous_end = 0;
    const size_t num_blocks = Array::get_size_from_header(base_ends);
    while (block_ndx < num_blocks && slot >= size_t(Array::get(base_ends, block_ndx))) {
        previous_end = size_t(Array::get(base_ends, block_ndx));
        ++block_ndx;
    }
    ref_type block_ref = to_ref(Array::get(base_blocks, block_ndx));
    return get_from_block(alloc.translate(block_ref), slot - previous_end);
}

void ArrayPackedBlobs::verify() const
{
#ifdef LLBT_DEBUG
    Array::verify();
    m_base_blocks.verify();
    m_base_ends.verify();
    m_delta_blocks.verify();
    m_overrides.verify();
    m_order.verify();
    LLBT_ASSERT(m_base_blocks.size() == m_base_ends.size());
    size_t previous = 0;
    for (size_t i = 0; i < m_base_blocks.size(); ++i) {
        ref_type ref = m_base_blocks.get_as_ref(i);
        size_t count = block_count(m_alloc.translate(ref));
        size_t end = size_t(m_base_ends.get(i));
        LLBT_ASSERT(end == previous + count);
        previous = end;
    }
    for (size_t i = 0; i < m_order.size(); ++i)
        LLBT_ASSERT(size_t(m_order.get(i)) < m_overrides.size());
#endif
}
