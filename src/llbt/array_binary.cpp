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
 **************************************************************************/

#include <llbt/array_binary.hpp>
#include <llbt/impl/destroy_guard.hpp>
#include <llbt/mixed.hpp>

#include <vector>

using namespace llbt;

ArrayBinary::ArrayBinary(Allocator& a)
    : m_alloc(a)
{
    m_arr = new (&m_storage) ArraySmallBlobs(a);
}

void ArrayBinary::create()
{
    static_cast<ArraySmallBlobs*>(m_arr)->create();
}

void ArrayBinary::create(const BinaryData* values, size_t count)
{
    bool needs_big = false;
    size_t total_size = 0;
    for (size_t i = 0; i < count; ++i)
        needs_big = needs_big || values[i].size() > small_blob_max_size, total_size += values[i].size();

    if (!needs_big) {
        auto* blobs = static_cast<ArraySmallBlobs*>(m_arr);
        blobs->create(values, count);
        return;
    }

    if (total_size > 4096 && ArrayPackedBlobs::can_pack(values, count)) {
        auto* blobs = new (&m_storage) ArrayPackedBlobs(m_alloc);
        m_arr = blobs;
        m_kind = Kind::Packed;
        blobs->create(values, count);
        return;
    }

    auto* blobs = new (&m_storage) ArrayBigBlobs(m_alloc, true);
    m_arr = blobs;
    m_kind = Kind::Big;
    blobs->create(values, count);
}

void ArrayBinary::init_from_mem(MemRef mem) noexcept
{
    char* header = mem.get_addr();

    ArrayParent* parent = m_arr->get_parent();
    size_t ndx_in_parent = m_arr->get_ndx_in_parent();

    if (!Array::get_context_flag_from_header(header)) {
        m_kind = Kind::Small;
        auto arr = new (&m_storage) ArraySmallBlobs(m_alloc);
        m_arr = arr;
        arr->init_from_mem(mem);
    }
    else if (ArrayPackedBlobs::is_packed(header)) {
        m_kind = Kind::Packed;
        auto arr = new (&m_storage) ArrayPackedBlobs(m_alloc);
        m_arr = arr;
        arr->init_from_mem(mem);
    }
    else {
        m_kind = Kind::Big;
        auto arr = new (&m_storage) ArrayBigBlobs(m_alloc, true);
        m_arr = arr;
        arr->init_from_mem(mem);
    }

    m_arr->set_parent(parent, ndx_in_parent);
}


void ArrayBinary::init_from_parent()
{
    ref_type ref = m_arr->get_ref_from_parent();
    init_from_ref(ref);
}

size_t ArrayBinary::size() const
{
    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->size();
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->size();
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->size();
    }
    LLBT_UNREACHABLE();
}

void ArrayBinary::add(BinaryData value)
{
    if (m_kind == Kind::Packed) {
        static_cast<ArrayPackedBlobs*>(m_arr)->add(value);
        return;
    }
    bool is_big = upgrade_leaf(value.size());
    if (!is_big) {
        static_cast<ArraySmallBlobs*>(m_arr)->add(value);
    }
    else {
        static_cast<ArrayBigBlobs*>(m_arr)->add(value);
    }
}

void ArrayBinary::replace_with_packed(const BinaryData* values, size_t count)
{
    ArrayParent* parent = m_arr->get_parent();
    size_t ndx_in_parent = m_arr->get_ndx_in_parent();

    ArrayPackedBlobs replacement(m_alloc);
    replacement.create(values, count);
    _impl::DeepArrayDestroyGuard replacement_guard(&replacement);
    replacement.set_parent(parent, ndx_in_parent);
    replacement.update_parent();

    switch (m_kind) {
        case Kind::Small: static_cast<ArraySmallBlobs*>(m_arr)->destroy(); break;
        case Kind::Big: static_cast<ArrayBigBlobs*>(m_arr)->destroy(); break;
        case Kind::Packed: static_cast<ArrayPackedBlobs*>(m_arr)->destroy(); break;
    }

    ref_type new_ref = replacement.get_ref();
    auto* packed = new (&m_storage) ArrayPackedBlobs(m_alloc);
    m_arr = packed;
    m_kind = Kind::Packed;
    packed->set_parent(parent, ndx_in_parent);
    packed->init_from_ref(new_ref);
    replacement_guard.release();
}

void ArrayBinary::replace_with_empty()
{
    ArrayParent* parent = m_arr->get_parent();
    size_t ndx_in_parent = m_arr->get_ndx_in_parent();
    ArraySmallBlobs replacement(m_alloc);
    replacement.create();
    _impl::DeepArrayDestroyGuard replacement_guard(&replacement);
    replacement.set_parent(parent, ndx_in_parent);
    replacement.update_parent();

    switch (m_kind) {
        case Kind::Small: static_cast<ArraySmallBlobs*>(m_arr)->destroy(); break;
        case Kind::Big: static_cast<ArrayBigBlobs*>(m_arr)->destroy(); break;
        case Kind::Packed: static_cast<ArrayPackedBlobs*>(m_arr)->destroy(); break;
    }

    ref_type new_ref = replacement.get_ref();
    auto* small = new (&m_storage) ArraySmallBlobs(m_alloc);
    m_arr = small;
    m_kind = Kind::Small;
    small->set_parent(parent, ndx_in_parent);
    small->init_from_ref(new_ref);
    replacement_guard.release();
}

void ArrayBinary::append(const BinaryData* values, size_t count)
{
    if (count == 0)
        return;
    if (m_kind == Kind::Packed) {
        static_cast<ArrayPackedBlobs*>(m_arr)->append(values, count);
        return;
    }

    bool has_large = false;
    size_t total_size = 0;
    for (size_t i = 0; i < size(); ++i) {
        BinaryData value = get(i);
        total_size += value.size();
        has_large = has_large || value.size() > small_blob_max_size;
    }
    for (size_t i = 0; i < count; ++i) {
        total_size += values[i].size();
        has_large = has_large || values[i].size() > small_blob_max_size;
    }

    if (has_large && total_size > 4096) {
        std::vector<BinaryData> all;
        all.reserve(size() + count);
        for (size_t i = 0; i < size(); ++i)
            all.push_back(get(i));
        all.insert(all.end(), values, values + count);
        if (ArrayPackedBlobs::can_pack(all.data(), all.size())) {
            replace_with_packed(all.data(), all.size());
            return;
        }
    }

    for (size_t i = 0; i < count; ++i)
        add(values[i]);
}

void ArrayBinary::set(size_t ndx, BinaryData value)
{
    if (m_kind == Kind::Packed) {
        static_cast<ArrayPackedBlobs*>(m_arr)->set(ndx, value);
        return;
    }
    bool is_big = upgrade_leaf(value.size());
    if (!is_big) {
        static_cast<ArraySmallBlobs*>(m_arr)->set(ndx, value);
    }
    else {
        static_cast<ArrayBigBlobs*>(m_arr)->set(ndx, value);
    }
}

void ArrayBinary::set_many(const size_t* positions, const BinaryData* values, size_t count, size_t position_base)
{
    if (m_kind == Kind::Packed) {
        static_cast<ArrayPackedBlobs*>(m_arr)->set_many(positions, values, count, position_base);
        return;
    }
    for (size_t i = 0; i < count; ++i)
        set(positions[i] - position_base, values[i]);
}

void ArrayBinary::insert(size_t ndx, BinaryData value)
{
    if (m_kind == Kind::Packed) {
        static_cast<ArrayPackedBlobs*>(m_arr)->insert(ndx, value);
        return;
    }
    bool is_big = upgrade_leaf(value.size());
    if (!is_big) {
        static_cast<ArraySmallBlobs*>(m_arr)->insert(ndx, value);
    }
    else {
        static_cast<ArrayBigBlobs*>(m_arr)->insert(ndx, value);
    }
}

BinaryData ArrayBinary::get(size_t ndx) const
{
    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->get(ndx);
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->get(ndx);
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->get(ndx);
    }
    LLBT_UNREACHABLE();
}

BinaryData ArrayBinary::get_at(size_t ndx, size_t& pos) const
{
    switch (m_kind) {
        case Kind::Small:
            pos = 0;
            return static_cast<ArraySmallBlobs*>(m_arr)->get(ndx);
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->get_at(ndx, pos);
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->get_at(ndx, pos);
    }
    LLBT_UNREACHABLE();
}

Mixed ArrayBinary::get_any(size_t ndx) const
{
    return Mixed(get(ndx));
}

bool ArrayBinary::is_null(size_t ndx) const
{
    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->is_null(ndx);
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->is_null(ndx);
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->is_null(ndx);
    }
    LLBT_UNREACHABLE();
}

void ArrayBinary::erase(size_t ndx)
{
    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->erase(ndx);
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->erase(ndx);
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->erase(ndx);
    }
    LLBT_UNREACHABLE();
}

void ArrayBinary::move(ArrayBinary& dst, size_t ndx)
{
    size_t sz = size();
    std::vector<BinaryData> moved;
    moved.reserve(sz - ndx);
    for (size_t i = ndx; i < sz; i++)
        moved.push_back(get(i));
    dst.append(moved.data(), moved.size());

    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->truncate(ndx);
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->truncate(ndx);
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->truncate(ndx);
    }
    LLBT_UNREACHABLE();
}

void ArrayBinary::clear()
{
    if (m_kind == Kind::Packed) {
        replace_with_empty();
        return;
    }
    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->clear();
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->clear();
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->clear();
    }
    LLBT_UNREACHABLE();
}

bool ArrayBinary::normalize_packed()
{
    if (m_kind != Kind::Packed || !static_cast<ArrayPackedBlobs*>(m_arr)->needs_normalize())
        return false;
    if (size() == 0) {
        replace_with_empty();
        return true;
    }
    std::vector<BinaryData> values;
    values.reserve(size());
    for (size_t i = 0; i < size(); ++i)
        values.push_back(get(i));
    replace_with_packed(values.data(), values.size());
    return true;
}

size_t ArrayBinary::find_first(BinaryData value, size_t begin, size_t end) const noexcept
{
    switch (m_kind) {
        case Kind::Small: return static_cast<ArraySmallBlobs*>(m_arr)->find_first(value, false, begin, end);
        case Kind::Big: return static_cast<ArrayBigBlobs*>(m_arr)->find_first(value, false, begin, end);
        case Kind::Packed: return static_cast<ArrayPackedBlobs*>(m_arr)->find_first(value, begin, end);
    }
    LLBT_UNREACHABLE();
}


bool ArrayBinary::upgrade_leaf(size_t value_size)
{
    if (m_kind == Kind::Big)
        return true;
    LLBT_ASSERT_DEBUG(m_kind == Kind::Small);

    if (value_size <= small_blob_max_size)
        return false;

    // Upgrade root leaf from small to big blobs
    auto small_blobs = static_cast<ArraySmallBlobs*>(m_arr);
    ArrayBigBlobs big_blobs(m_alloc, true);
    big_blobs.create(); // Throws

    size_t n = small_blobs->size();
    for (size_t i = 0; i < n; i++) {
        big_blobs.add(small_blobs->get(i)); // Throws
    }
    auto parent = small_blobs->get_parent();
    auto ndx_in_parent = small_blobs->get_ndx_in_parent();
    small_blobs->destroy();

    auto arr = new (&m_storage) ArrayBigBlobs(m_alloc, true);
    arr->init_from_mem(big_blobs.get_mem());
    arr->set_parent(parent, ndx_in_parent);
    arr->update_parent(); // Throws

    m_kind = Kind::Big;
    return true;
}

void ArrayBinary::verify() const
{
#ifdef LLBT_DEBUG
    switch (m_kind) {
        case Kind::Small: static_cast<ArraySmallBlobs*>(m_arr)->verify(); break;
        case Kind::Big: static_cast<ArrayBigBlobs*>(m_arr)->verify(); break;
        case Kind::Packed: static_cast<ArrayPackedBlobs*>(m_arr)->verify(); break;
    }
#endif
}
