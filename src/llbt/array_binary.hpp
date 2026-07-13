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

#ifndef LLBT_ARRAY_BINARY_HPP
#define LLBT_ARRAY_BINARY_HPP

#include <llbt/array_blobs_small.hpp>
#include <llbt/array_blobs_big.hpp>
#include <llbt/array_packed_blobs.hpp>

namespace llbt {

class ArrayBinary : public ArrayPayload {
public:
    using value_type = BinaryData;

    explicit ArrayBinary(Allocator&);

    static BinaryData default_value(bool nullable)
    {
        return nullable ? BinaryData{} : BinaryData{"", 0};
    }

    void create();
    /// Create an append-optimized packed leaf when the whole batch fits in one
    /// blob. A later large-value update converts it once to the normal
    /// update-friendly representation.
    void create(const BinaryData* values, size_t count);

    ref_type get_ref() const
    {
        return m_arr->get_ref();
    }

    void set_parent(ArrayParent* parent, size_t ndx_in_parent) noexcept override
    {
        m_arr->set_parent(parent, ndx_in_parent);
    }

    void update_parent()
    {
        m_arr->update_parent();
    }

    void init_from_mem(MemRef mem) noexcept;
    void init_from_ref(ref_type ref) noexcept override
    {
        init_from_mem(MemRef(m_alloc.translate(ref), ref, m_alloc));
    }
    void init_from_parent();

    size_t size() const;

    void add(BinaryData value);
    void append(const BinaryData* values, size_t count);
    void set(size_t ndx, BinaryData value);
    void set_many(const size_t* positions, const BinaryData* values, size_t count, size_t position_base);
    void set_null(size_t ndx)
    {
        set(ndx, BinaryData{});
    }
    void insert(size_t ndx, BinaryData value);
    BinaryData get(size_t ndx) const;
    BinaryData get_at(size_t ndx, size_t& pos) const;
    Mixed get_any(size_t ndx) const override;
    bool is_null(size_t ndx) const;
    void erase(size_t ndx);
    void move(ArrayBinary& dst, size_t ndx);
    void clear();
    bool normalize_packed();

    size_t find_first(BinaryData value, size_t begin, size_t end) const noexcept;

    /// Get the specified element without the cost of constructing an
    /// array instance. If an array instance is already available, or
    /// you need to get multiple values, then this method will be
    /// slower.
    static BinaryData get(const char* header, size_t ndx, Allocator& alloc) noexcept;

    void verify() const;

private:
    static constexpr size_t small_blob_max_size = 64;

    enum class Kind : unsigned char {
        Small,
        Big,
        Packed,
    };

    Allocator& m_alloc;
    Array* m_arr;
    static constexpr size_t storage_size =
        std::max(sizeof(ArrayPackedBlobs), std::max(sizeof(ArraySmallBlobs), sizeof(ArrayBigBlobs)));
    alignas(ArrayPackedBlobs) std::byte m_storage[storage_size];
    Kind m_kind = Kind::Small;

    bool upgrade_leaf(size_t value_size);
    void replace_with_packed(const BinaryData* values, size_t count);
    void replace_with_empty();
};

inline BinaryData ArrayBinary::get(const char* header, size_t ndx, Allocator& alloc) noexcept
{
    bool is_big = Array::get_context_flag_from_header(header);
    if (!is_big) {
        return ArraySmallBlobs::get(header, ndx, alloc);
    }
    else if (ArrayPackedBlobs::is_packed(header)) {
        return ArrayPackedBlobs::get(header, ndx, alloc);
    }
    else {
        return ArrayBigBlobs::get(header, ndx, alloc);
    }
}
} // namespace llbt

#endif /* SRC_BARQ_ARRAY_BINARY_HPP_ */
