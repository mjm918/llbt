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

#include <algorithm>
#include <cstring>

#include <llbt/array_blobs_big.hpp>
#include <llbt/column_integer.hpp>
#include <llbt/impl/destroy_guard.hpp>


using namespace llbt;

void ArrayBigBlobs::create(const BinaryData* values, size_t count)
{
    Array::create(type_HasRefs, true, count, 0);
    _impl::DeepArrayRefDestroyGuard child_guard(m_alloc);
    for (size_t i = 0; i < count; ++i) {
        if (values[i].is_null())
            continue;
        if (values[i].size() > ArrayBlob::max_binary_size) {
            ArrayBlob blob(m_alloc);
            blob.create();
            _impl::DeepArrayDestroyGuard blob_guard(&blob);
            ref_type ref = blob.add(values[i].data(), values[i].size());
            Array::set_as_ref(i, ref);
            blob_guard.release();
            continue;
        }
        size_t bytes = (NodeHeader::header_size + values[i].size() + 7) & ~size_t(7);
        size_t capacity = std::max(bytes, size_t(Node::initial_capacity));
        MemRef mem = m_alloc.alloc(capacity);
        child_guard.reset(mem.get_ref());
        Node::init_header(mem.get_addr(), false, false, false, NodeHeader::wtype_Ignore, 0,
                          values[i].size(), capacity);
        char* output = Array::get_data_from_header(mem.get_addr());
        if (values[i].size())
            std::memcpy(output, values[i].data(), values[i].size());
        std::memset(output + values[i].size(), 0,
                    bytes - NodeHeader::header_size - values[i].size());
        Array::set_as_ref(i, mem.get_ref());
        child_guard.release();
    }
}

BinaryData ArrayBigBlobs::get_at(size_t ndx, size_t& pos) const noexcept
{
    ref_type ref = get_as_ref(ndx);
    if (ref == 0)
        return {}; // llbt::null();

    ArrayBlob blob(m_alloc);
    blob.init_from_ref(ref);

    return blob.get_at(pos);
}


void ArrayBigBlobs::add(BinaryData value, bool add_zero_term)
{
    LLBT_ASSERT_7(value.size(), ==, 0, ||, value.data(), !=, 0);

    if (value.is_null()) {
        Array::add(0); // Throws
    }
    else {
        ArrayBlob new_blob(m_alloc);
        new_blob.create();                                                      // Throws
        ref_type ref = new_blob.add(value.data(), value.size(), add_zero_term); // Throws
        Array::add(from_ref(ref));                                              // Throws
    }
}


void ArrayBigBlobs::set(size_t ndx, BinaryData value, bool add_zero_term)
{
    LLBT_ASSERT_3(ndx, <, size());
    LLBT_ASSERT_7(value.size(), ==, 0, ||, value.data(), !=, 0);

    ref_type ref = get_as_ref(ndx);

    auto make_blob = [&]() {
        size_t value_size = value.size() + size_t(add_zero_term);
        if (value_size > ArrayBlob::max_binary_size) {
            ArrayBlob blob(m_alloc);
            blob.create();
            _impl::DeepArrayDestroyGuard guard(&blob);
            ref_type new_ref = blob.add(value.data(), value.size(), add_zero_term);
            Array::set_as_ref(ndx, new_ref);
            guard.release();
            return new_ref;
        }
        size_t bytes = (NodeHeader::header_size + value_size + 7) & ~size_t(7);
        size_t capacity = std::max(bytes, size_t(Node::initial_capacity));
        MemRef mem = m_alloc.alloc(capacity);
        _impl::DeepArrayRefDestroyGuard guard(mem.get_ref(), m_alloc);
        Node::init_header(mem.get_addr(), false, false, false, NodeHeader::wtype_Ignore, 0,
                          value_size, capacity);
        char* output = Array::get_data_from_header(mem.get_addr());
        if (value.size())
            std::memcpy(output, value.data(), value.size());
        if (add_zero_term)
            output[value.size()] = 0;
        std::memset(output + value_size, 0, bytes - NodeHeader::header_size - value_size);
        Array::set_as_ref(ndx, mem.get_ref());
        guard.release();
        return mem.get_ref();
    };

    if (ref == 0 && value.is_null()) {
        return;
    }
    else if (ref == 0 && value.data() != nullptr) {
        make_blob();
        return;
    }
    else if (ref != 0 && value.data() != nullptr) {
        char* header = m_alloc.translate(ref);
        if (!Array::get_context_flag_from_header(header) && m_alloc.is_read_only(ref)) {
            make_blob();
            Array::destroy(ref, m_alloc);
            return;
        }
        if (Array::get_context_flag_from_header(header)) {
            Array arr(m_alloc);
            arr.init_from_mem(MemRef(header, ref, m_alloc));
            arr.set_parent(this, ndx);
            ref_type new_ref =
                arr.blob_replace(0, arr.blob_size(), value.data(), value.size(), add_zero_term); // Throws
            if (new_ref != ref) {
                Array::set_as_ref(ndx, new_ref);
            }
        }
        else {
            ArrayBlob blob(m_alloc);
            blob.init_from_mem(MemRef(header, ref, m_alloc));
            blob.set_parent(this, ndx);
            ref_type new_ref = blob.replace(0, blob.blob_size(), value.data(), value.size(), add_zero_term); // Throws
            if (new_ref != ref) {
                Array::set_as_ref(ndx, new_ref);
            }
        }
        return;
    }
    else if (ref != 0 && value.is_null()) {
        Array::destroy_deep(ref, get_alloc());
        Array::set(ndx, 0);
        return;
    }
    LLBT_ASSERT(false);
}


void ArrayBigBlobs::insert(size_t ndx, BinaryData value, bool add_zero_term)
{
    LLBT_ASSERT_3(ndx, <=, size());
    LLBT_ASSERT_7(value.size(), ==, 0, ||, value.data(), !=, 0);

    if (value.is_null()) {
        Array::insert(ndx, 0); // Throws
    }
    else {
        ArrayBlob new_blob(m_alloc);
        new_blob.create();                                                      // Throws
        ref_type ref = new_blob.add(value.data(), value.size(), add_zero_term); // Throws

        Array::insert(ndx, int64_t(ref)); // Throws
    }
}


size_t ArrayBigBlobs::count(BinaryData value, bool is_string, size_t begin, size_t end) const noexcept
{
    size_t num_matches = 0;

    size_t begin_2 = begin;
    for (;;) {
        size_t ndx = find_first(value, is_string, begin_2, end);
        if (ndx == not_found)
            break;
        ++num_matches;
        begin_2 = ndx + 1;
    }

    return num_matches;
}


size_t ArrayBigBlobs::find_first(BinaryData value, bool is_string, size_t begin, size_t end) const noexcept
{
    if (end == npos)
        end = m_size;
    LLBT_ASSERT_11(begin, <=, m_size, &&, end, <=, m_size, &&, begin, <=, end);

    // When strings are stored as blobs, they are always zero-terminated
    // but the value we get as input might not be.
    size_t value_size = value.size();
    size_t full_size = is_string ? value_size + 1 : value_size;

    if (value.is_null()) {
        for (size_t i = begin; i != end; ++i) {
            ref_type ref = get_as_ref(i);
            if (ref == 0)
                return i;
        }
    }
    else {
        for (size_t i = begin; i != end; ++i) {
            ref_type ref = get_as_ref(i);
            if (ref) {
                const char* blob_header = get_alloc().translate(ref);
                size_t sz = get_size_from_header(blob_header);
                if (sz == full_size) {
                    const char* blob_value = ArrayBlob::get(blob_header, 0);
                    if (std::equal(blob_value, blob_value + value_size, value.data()))
                        return i;
                }
            }
        }
    }

    return not_found;
}


void ArrayBigBlobs::find_all(IntegerColumn& result, BinaryData value, bool is_string, size_t add_offset, size_t begin,
                             size_t end)
{
    size_t begin_2 = begin;
    for (;;) {
        size_t ndx = find_first(value, is_string, begin_2, end);
        if (ndx == not_found)
            break;
        result.add(add_offset + ndx); // Throws
        begin_2 = ndx + 1;
    }
}


void ArrayBigBlobs::verify() const
{
#ifdef LLBT_DEBUG
    LLBT_ASSERT(has_refs());
    for (size_t i = 0; i < size(); ++i) {
        ref_type blob_ref = Array::get_as_ref(i);
        // 0 is used to indicate llbt::null()
        if (blob_ref != 0) {
            ArrayBlob blob(m_alloc);
            blob.init_from_ref(blob_ref);
            blob.verify();
        }
    }
#endif
}
