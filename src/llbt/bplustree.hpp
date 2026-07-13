/*************************************************************************
 *
 * Copyright 2018 Realm Inc.
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

#ifndef LLBT_BPLUSTREE_HPP
#define LLBT_BPLUSTREE_HPP

#include <llbt/aggregate_ops.hpp>
#include <llbt/array_unsigned.hpp>
#include <llbt/column_type_traits.hpp>
#include <llbt/decimal128.hpp>
#include <llbt/timestamp.hpp>
#include <llbt/object_id.hpp>
#include <llbt/util/function_ref.hpp>

namespace llbt {

class BPlusTreeBase;
class BPlusTreeInner;

/*****************************************************************************/
/* BPlusTreeNode                                                             */
/* Base class for all nodes in the BPlusTree. Provides an abstract interface */
/* that can be used by the BPlusTreeBase class to manipulate the tree.       */
/*****************************************************************************/
class BPlusTreeNode {
public:
    struct State {
        int64_t split_offset;
        size_t split_size;
        size_t insert_count = 1;
        ref_type append_leaf_ref = 0;
    };

    // Insert an element at 'insert_pos'. May cause node to be split
    using InsertFunc = util::FunctionRef<size_t(BPlusTreeNode*, size_t insert_pos)>;
    // Access element at 'ndx'. Insertion/deletion not allowed
    using AccessFunc = util::FunctionRef<void(BPlusTreeNode*, size_t ndx)>;
    // Erase element at erase_pos. May cause nodes to be merged
    using EraseFunc = util::FunctionRef<size_t(BPlusTreeNode*, size_t erase_pos)>;
    // Function to be called for all leaves in the tree until the function
    // returns 'IteratorControl::Stop'. 'offset' gives index of the first element in the leaf.
    using TraverseFunc = util::FunctionRef<IteratorControl(BPlusTreeNode*, size_t offset)>;

    BPlusTreeNode(BPlusTreeBase* tree)
        : m_tree(tree)
    {
    }

    void change_owner(BPlusTreeBase* tree)
    {
        m_tree = tree;
    }

    bool get_context_flag() const noexcept;
    void set_context_flag(bool) noexcept;

    virtual ~BPlusTreeNode();

    virtual bool is_leaf() const = 0;
    virtual bool is_compact() const = 0;
    virtual ref_type get_ref() const = 0;

    virtual void init_from_ref(ref_type ref) noexcept = 0;

    virtual void bp_set_parent(ArrayParent* parent, size_t ndx_in_parent) = 0;
    virtual void update_parent() = 0;

    // Number of elements in this node
    virtual size_t get_node_size() const = 0;
    // Size of subtree
    virtual size_t get_tree_size() const = 0;

    virtual ref_type bptree_insert(size_t n, State& state, InsertFunc) = 0;
    virtual void bptree_access(size_t n, AccessFunc) = 0;
    virtual size_t bptree_erase(size_t n, EraseFunc) = 0;
    virtual bool bptree_traverse(TraverseFunc) = 0;

    // Move elements over in new node, starting with element at position 'ndx'.
    // If this is an inner node, the index offsets should be adjusted with 'adj'
    virtual void move(BPlusTreeNode* new_node, size_t ndx, int64_t offset_adj) = 0;
    virtual void verify() const = 0;

protected:
    BPlusTreeBase* m_tree;
};

/*****************************************************************************/
/* BPlusTreeLeaf                                                             */
/* Base class for all leaf nodes.                                            */
/*****************************************************************************/
class BPlusTreeLeaf : public BPlusTreeNode {
public:
    using BPlusTreeNode::BPlusTreeNode;

    bool is_leaf() const override
    {
        return true;
    }

    bool is_compact() const override
    {
        return true;
    }

    ref_type bptree_insert(size_t n, State& state, InsertFunc) override;
    void bptree_access(size_t n, AccessFunc) override;
    size_t bptree_erase(size_t n, EraseFunc) override;
    bool bptree_traverse(TraverseFunc) override;
};

/*****************************************************************************/
/* BPlusTreeBase                                                             */
/* Base class for the actual tree classes.                                   */
/*****************************************************************************/
class BPlusTreeBase {
public:
    BPlusTreeBase(Allocator& alloc)
        : m_alloc(alloc)
    {
        invalidate_leaf_cache();
    }
    virtual ~BPlusTreeBase();


    Allocator& get_alloc() const
    {
        return m_alloc;
    }

    bool is_attached() const
    {
        return bool(m_root);
    }

    void detach()
    {
        m_root = nullptr;
    }

    bool get_context_flag() const noexcept
    {
        return m_root->get_context_flag();
    }

    void set_context_flag(bool cf) noexcept
    {
        m_root->set_context_flag(cf);
    }

    size_t size() const
    {
        return m_size;
    }

    static size_t size_from_header(const char* header);

    bool is_empty() const
    {
        return m_size == 0;
    }

    ref_type get_ref() const
    {
        LLBT_ASSERT(is_attached());
        return m_root->get_ref();
    }

    void init_from_ref(ref_type ref)
    {
        auto new_root = create_root_from_ref(ref);
        new_root->bp_set_parent(m_parent, m_ndx_in_parent);

        m_root = std::move(new_root);

        invalidate_leaf_cache();
        m_size = m_root->get_tree_size();
    }

    bool init_from_parent()
    {
        if (ref_type ref = m_parent->get_child_ref(m_ndx_in_parent)) {
            init_from_ref(ref);
            return true;
        }
        return false;
    }

    void set_parent(ArrayParent* parent, size_t ndx_in_parent)
    {
        m_parent = parent;
        m_ndx_in_parent = ndx_in_parent;
        if (is_attached())
            m_root->bp_set_parent(parent, ndx_in_parent);
    }

    virtual void erase(size_t) = 0;
    virtual void clear() = 0;
    virtual void swap(size_t, size_t) = 0;

    void create();
    void destroy();
    void verify() const
    {
        m_root->verify();
    }

protected:
    template <class U>
    struct LeafTypeTrait {
        using type = typename ColumnTypeTraits<U>::cluster_leaf_type;
    };

    friend class BPlusTreeInner;
    friend class BPlusTreeLeaf;

    std::unique_ptr<BPlusTreeNode> m_root;
    Allocator& m_alloc;
    ArrayParent* m_parent = nullptr;
    size_t m_ndx_in_parent = 0;
    size_t m_size = 0;
    size_t m_cached_leaf_begin;
    size_t m_cached_leaf_end;

    void set_leaf_bounds(size_t b, size_t e)
    {
        m_cached_leaf_begin = b;
        m_cached_leaf_end = e;
    }

    void invalidate_leaf_cache()
    {
        m_cached_leaf_begin = size_t(-1);
        m_cached_leaf_end = size_t(-1);
    }

    void adjust_leaf_bounds(int incr)
    {
        m_cached_leaf_end += incr;
    }

    void bptree_insert(size_t n, BPlusTreeNode::InsertFunc func, size_t insert_count = 1,
                       ref_type append_leaf_ref = 0);
    void bptree_erase(size_t n, BPlusTreeNode::EraseFunc func);

    // Create an un-attached leaf node
    virtual std::unique_ptr<BPlusTreeLeaf> create_leaf_node() = 0;
    // Create a leaf node and initialize it with 'ref'
    virtual std::unique_ptr<BPlusTreeLeaf> init_leaf_node(ref_type ref) = 0;

    // Initialize the leaf cache with 'mem'
    virtual BPlusTreeLeaf* cache_leaf(MemRef mem) = 0;
    virtual void replace_root(std::unique_ptr<BPlusTreeNode> new_root);
    std::unique_ptr<BPlusTreeNode> create_root_from_ref(ref_type ref);

    // Adopt `leaf_refs` as the entire content of this (empty) tree: build the
    // compact inner levels bottom-up and install the root. Every leaf must be
    // full (LLBT_MAX_BPNODE_SIZE) except possibly the last; `total_size` is the
    // overall element count. `leaf_refs` is consumed (used as level scratch).
    void bulk_adopt_leaves(std::vector<ref_type>& leaf_refs, size_t total_size);
};

template <>
struct BPlusTreeBase::LeafTypeTrait<ObjKey> {
    using type = ArrayKeyNonNullable;
};

/*****************************************************************************/
/* BPlusTree                                                                 */
/* Actual implementation of the BPlusTree to hold elements of type T.        */
/*****************************************************************************/
template <class T>
class BPlusTree : public BPlusTreeBase {
public:
    using LeafArray = typename LeafTypeTrait<T>::type;
    using value_type = T;

    /**
     * Actual class for the leaves. Maps the abstract interface defined
     * in BPlusTreeNode onto the specific array class
     **/
    class LeafNode : public BPlusTreeLeaf, public LeafArray {
    public:
        LeafNode(BPlusTreeBase* tree)
            : BPlusTreeLeaf(tree)
            , LeafArray(tree->get_alloc())
        {
        }

        void init_from_ref(ref_type ref) noexcept override
        {
            LeafArray::init_from_ref(ref);
        }

        ref_type get_ref() const override
        {
            return LeafArray::get_ref();
        }

        void bp_set_parent(llbt::ArrayParent* p, size_t n) override
        {
            LeafArray::set_parent(p, n);
        }

        void update_parent() override
        {
            LeafArray::update_parent();
        }

        size_t get_node_size() const override
        {
            return LeafArray::size();
        }

        size_t get_tree_size() const override
        {
            return LeafArray::size();
        }

        void move(BPlusTreeNode* new_node, size_t ndx, int64_t) override
        {
            LeafNode* dst(static_cast<LeafNode*>(new_node));
            LeafArray::move(*dst, ndx);
        }
        void verify() const override
        {
            LeafArray::verify();
        }
    };

    BPlusTree(Allocator& alloc)
        : BPlusTreeBase(alloc)
        , m_leaf_cache(this)
    {
    }

    /************ Tree manipulation functions ************/

    static T default_value(bool nullable = false)
    {
        return LeafArray::default_value(nullable);
    }

    void add(T value)
    {
        insert(npos, value);
    }

    void insert(size_t n, T value)
    {
        auto func = [value](BPlusTreeNode* node, size_t ndx) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            leaf->LeafArray::insert(ndx, value);
            return leaf->size();
        };

        bptree_insert(n, func);
        m_size++;
    }

    inline T get(size_t n) const
    {
        // Fast path
        if (m_cached_leaf_begin <= n && n < m_cached_leaf_end) {
            return m_leaf_cache.get(n - m_cached_leaf_begin);
        }
        else {
            // Slow path
            return get_uncached(n);
        }
    }

    LLBT_NOINLINE T get_uncached(size_t n) const
    {
        T value;

        auto func = [&value](BPlusTreeNode* node, size_t ndx) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            value = leaf->get(ndx);
        };

        m_root->bptree_access(n, func);

        return value;
    }

    std::vector<T> get_all() const
    {
        std::vector<T> all_values;
        all_values.reserve(m_size);

        auto func = [&all_values](BPlusTreeNode* node, size_t) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            size_t sz = leaf->size();
            for (size_t i = 0; i < sz; i++) {
                all_values.push_back(leaf->get(i));
            }
            return IteratorControl::AdvanceToNext;
        };

        m_root->bptree_traverse(func);

        return all_values;
    }

    // Append `count` values pulled chunk-wise from `fill(offset, n, out)`. An
    // empty tree is assembled bottom-up. A non-empty tree fills its tail leaf
    // with one descent at a time and uses the normal split path only when a new
    // leaf is needed.
    template <typename Filler>
    void add_from(size_t count, Filler&& fill)
    {
        if (count == 0)
            return;
        std::vector<T> buf(std::min(count, size_t(LLBT_MAX_BPNODE_SIZE)));
        if (m_size != 0) {
            size_t offset = 0;
            while (offset < count) {
                size_t tail_size = 0;
                m_root->bptree_access(m_size - 1, [&](BPlusTreeNode* node, size_t) {
                    tail_size = node->get_node_size();
                });
                if (tail_size == LLBT_MAX_BPNODE_SIZE) {
                    size_t n = std::min(count - offset, size_t(LLBT_MAX_BPNODE_SIZE));
                    fill(offset, n, buf.data());
                    LeafNode leaf(this);
                    if constexpr (std::is_same_v<T, BinaryData>) {
                        leaf.LeafArray::create(buf.data(), n);
                    }
                    else {
                        leaf.create();
                        for (size_t j = 0; j < n; ++j)
                            leaf.LeafArray::add(buf[j]);
                    }
                    auto unreachable = [](BPlusTreeNode*, size_t) -> size_t {
                        LLBT_UNREACHABLE();
                    };
                    bptree_insert(npos, unreachable, n, leaf.get_ref());
                    m_size += n;
                    offset += n;
                    continue;
                }
                size_t n = std::min(count - offset, size_t(LLBT_MAX_BPNODE_SIZE) - tail_size);
                fill(offset, n, buf.data());
                auto append = [&](BPlusTreeNode* node, size_t) {
                    LeafNode* leaf = static_cast<LeafNode*>(node);
                    if constexpr (std::is_same_v<T, BinaryData>) {
                        leaf->LeafArray::append(buf.data(), n);
                    }
                    else {
                        for (size_t j = 0; j < n; ++j)
                            leaf->LeafArray::add(buf[j]);
                    }
                    return leaf->size();
                };
                bptree_insert(npos, append, n);
                m_size += n;
                offset += n;
            }
            return;
        }
        std::vector<ref_type> leaves;
        leaves.reserve((count + LLBT_MAX_BPNODE_SIZE - 1) / LLBT_MAX_BPNODE_SIZE);
        for (size_t offset = 0; offset < count;) {
            size_t n = std::min(count - offset, size_t(LLBT_MAX_BPNODE_SIZE));
            fill(offset, n, buf.data());
            LeafNode leaf(this);
            if constexpr (std::is_same_v<T, BinaryData>) {
                leaf.LeafArray::create(buf.data(), n);
            }
            else {
                leaf.create();
                for (size_t j = 0; j < n; ++j)
                    leaf.LeafArray::add(buf[j]);
            }
            leaves.push_back(leaf.get_ref());
            offset += n;
        }
        bulk_adopt_leaves(leaves, count);
    }

    // Append values [0, count) — the write-side twin of get_range.
    void add_range(const T* values, size_t count)
    {
        add_from(count, [values](size_t offset, size_t n, T* out) {
            std::copy(values + offset, values + offset + n, out);
        });
    }

    // Copy values [n, n + count) into out as signed 8-bit values — a straight
    // byte copy per leaf while the leaf stores width-8 data (the caller
    // guarantees every value fits int8; narrower leaves fall back to
    // per-element reads). Integer trees only.
    void get_range_i8(size_t n, size_t count, int8_t* out) const
    {
        static_assert(std::is_same_v<T, int64_t>, "byte reads are for integer trees");
        LLBT_ASSERT_DEBUG(n + count <= size());
        while (count > 0) {
            size_t copied = 0;
            auto func = [&](BPlusTreeNode* node, size_t ndx) {
                LeafNode* leaf = static_cast<LeafNode*>(node);
                size_t avail = leaf->size() - ndx;
                copied = std::min(count, avail);
                if (leaf->get_width() == 8) {
                    const int8_t* src = reinterpret_cast<const int8_t*>(leaf->m_data) + ndx;
                    std::copy(src, src + copied, out);
                }
                else {
                    for (size_t i = 0; i < copied; i++)
                        out[i] = int8_t(leaf->get(ndx + i));
                }
            };
            m_root->bptree_access(n, func);
            n += copied;
            out += copied;
            count -= copied;
        }
    }

    // Copy values [n, n + count) into out — one tree descent per touched leaf
    // instead of one per element.
    void get_range(size_t n, size_t count, T* out) const
    {
        LLBT_ASSERT_DEBUG(n + count <= size());
        while (count > 0) {
            size_t copied = 0;
            auto func = [&](BPlusTreeNode* node, size_t ndx) {
                LeafNode* leaf = static_cast<LeafNode*>(node);
                size_t avail = leaf->size() - ndx;
                copied = std::min(count, avail);
                for (size_t i = 0; i < copied; i++)
                    out[i] = leaf->get(ndx + i);
            };
            m_root->bptree_access(n, func);
            n += copied;
            out += copied;
            count -= copied;
        }
    }

    void set(size_t n, T value)
    {
        auto func = [value](BPlusTreeNode* node, size_t ndx) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            leaf->set(ndx, value);
        };

        m_root->bptree_access(n, func);
    }

    void set_many(const size_t* positions, const T* values, size_t count)
    {
        if (count == 0)
            return;
        if (!positions || !values)
            throw std::invalid_argument("null bulk set input");
        for (size_t i = 0; i < count; ++i) {
            if (positions[i] >= m_size)
                throw std::out_of_range("bulk set position");
            if (i && positions[i - 1] >= positions[i])
                throw std::invalid_argument("bulk set positions must be sorted and unique");
        }

        size_t next = 0;
        m_root->bptree_traverse([&](BPlusTreeNode* node, size_t offset) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            size_t end = offset + leaf->size();
            if constexpr (std::is_same_v<T, BinaryData>) {
                size_t first = next;
                while (next < count && positions[next] < end)
                    ++next;
                leaf->LeafArray::set_many(positions + first, values + first, next - first, offset);
            }
            else {
                while (next < count && positions[next] < end) {
                    leaf->set(positions[next] - offset, values[next]);
                    ++next;
                }
            }
            return next == count ? IteratorControl::Stop : IteratorControl::AdvanceToNext;
        });
        invalidate_leaf_cache();
    }

    void erase_many(const size_t* positions, size_t count)
    {
        if (count == 0)
            return;
        if (!positions)
            throw std::invalid_argument("null bulk erase input");
        for (size_t i = 0; i < count; ++i) {
            if (positions[i] >= m_size)
                throw std::out_of_range("bulk erase position");
            if (i && positions[i - 1] >= positions[i])
                throw std::invalid_argument("bulk erase positions must be sorted and unique");
        }
        for (size_t i = count; i > 0; --i)
            erase(positions[i - 1]);
    }

    void swap(size_t ndx1, size_t ndx2) override
    {
        if constexpr (std::is_same_v<T, StringData> || std::is_same_v<T, BinaryData>) {
            struct SwapBuffer {
                std::string val;
                bool n;
                SwapBuffer(T v)
                    : val(v.data(), v.size())
                    , n(v.is_null())
                {
                }
                T get()
                {
                    return n ? T() : T(val);
                }
            };
            SwapBuffer tmp1{get(ndx1)};
            SwapBuffer tmp2{get(ndx2)};
            set(ndx1, tmp2.get());
            set(ndx2, tmp1.get());
        }
        else if constexpr (std::is_same_v<T, Mixed>) {
            std::string buf1;
            std::string buf2;
            Mixed tmp1 = get(ndx1);
            Mixed tmp2 = get(ndx2);
            if (tmp1.is_type(type_String, type_Binary)) {
                tmp1.use_buffer(buf1);
            }
            if (tmp2.is_type(type_String, type_Binary)) {
                tmp2.use_buffer(buf2);
            }
            set(ndx1, tmp2);
            set(ndx2, tmp1);
        }
        else {
            T tmp = get(ndx1);
            set(ndx1, get(ndx2));
            set(ndx2, tmp);
        }
    }

    void erase(size_t n) override
    {
        auto func = [](BPlusTreeNode* node, size_t ndx) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            leaf->LeafArray::erase(ndx);
            return leaf->size();
        };

        bptree_erase(n, func);
        m_size--;
    }

    void clear() override
    {
        if (m_root->is_leaf()) {
            LeafNode* leaf = static_cast<LeafNode*>(m_root.get());
            leaf->clear();
        }
        else {
            destroy();
            create();
            if (m_parent) {
                m_parent->update_child_ref(m_ndx_in_parent, get_ref());
            }
        }
        m_size = 0;
    }

    void traverse(BPlusTreeNode::TraverseFunc func) const
    {
        if (m_root) {
            m_root->bptree_traverse(func);
        }
    }

    size_t find_first(T value) const noexcept
    {
        size_t result = llbt::npos;

        auto func = [&result, value](BPlusTreeNode* node, size_t offset) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            size_t sz = leaf->size();
            auto i = leaf->find_first(value, 0, sz);
            if (i < sz) {
                result = i + offset;
                return IteratorControl::Stop;
            }
            return IteratorControl::AdvanceToNext;
        };

        m_root->bptree_traverse(func);

        return result;
    }

    /// Lowest index whose element compares greater than or equal to `value`
    /// (like std::lower_bound). Meaningful only while the caller keeps the
    /// elements sorted — e.g. by always inserting at the returned index; the
    /// tree itself does not enforce an ordering.
    size_t lower_bound(T value) const
    {
        if (m_size == 0)
            return 0;
        size_t base = 0;
        ref_type ref = m_root->get_ref();
        char* header = m_alloc.translate(ref);
        while (NodeHeader::get_is_inner_bptree_node_from_header(header)) {
            // inner node layout: slot 0 = offsets ref or tagged
            // elems-per-child, slots [1, n-1) = children, last slot =
            // tagged tree size
            const size_t nchildren = NodeHeader::get_size_from_header(header) - 2;
            // Pick the child immediately before the first child whose first
            // element is >= value. That earlier child can end in values equal
            // to `value`; choosing a child which begins with `value` would
            // skip duplicates spanning a child boundary.
            size_t left = 1, right = nchildren;
            while (left < right) {
                const size_t mid = left + (right - left) / 2;
                const ref_type child_ref = to_ref(Array::get(header, mid + 1));
                if (subtree_first_value(child_ref) < value)
                    left = mid + 1;
                else
                    right = mid;
            }
            const size_t child_ndx = left - 1;
            if (child_ndx > 0) {
                const int64_t slot0 = Array::get(header, 0);
                if (slot0 & 1) {
                    // compact form: equal number of elements per child
                    base += size_t(slot0 >> 1) * child_ndx;
                }
                else {
                    // offsets array holds cumulative counts; entry c-1 is
                    // where child c starts
                    ArrayUnsigned offsets(m_alloc);
                    offsets.init_from_ref(to_ref(slot0));
                    base += size_t(offsets.get(child_ndx - 1));
                }
            }
            ref = to_ref(Array::get(header, child_ndx + 1));
            header = m_alloc.translate(ref);
        }
        LeafArray leaf(m_alloc);
        leaf.init_from_ref(ref);
        size_t left = 0, right = leaf.size();
        while (left < right) {
            const size_t mid = left + (right - left) / 2;
            if (leaf.get(mid) < value)
                left = mid + 1;
            else
                right = mid;
        }
        return base + left;
    }

    template <typename Func>
    void find_all(T value, Func&& callback) const noexcept
    {
        auto func = [&callback, value](BPlusTreeNode* node, size_t offset) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            size_t i = -1, sz = leaf->size();
            while ((i = leaf->find_first(value, i + 1, sz)) < sz) {
                callback(i + offset);
            }
            return IteratorControl::AdvanceToNext;
        };

        m_root->bptree_traverse(func);
    }

    template <typename Func>
    void for_all(Func&& callback) const
    {
        using Ret = std::invoke_result_t<Func, T>;
        m_root->bptree_traverse([&callback](BPlusTreeNode* node, size_t) {
            LeafNode* leaf = static_cast<LeafNode*>(node);
            size_t sz = leaf->size();
            for (size_t i = 0; i < sz; i++) {
                if constexpr (std::is_same_v<Ret, void>) {
                    callback(leaf->get(i));
                }
                else {
                    if (!callback(leaf->get(i)))
                        return IteratorControl::Stop;
                }
            }
            return IteratorControl::AdvanceToNext;
        });
    }

    void split_if_needed()
    {
        while (m_root->get_node_size() > LLBT_MAX_BPNODE_SIZE) {
            split_root();
        }
    }

    bool normalize_packed_blobs()
    {
        if constexpr (!std::is_same_v<T, BinaryData>) {
            return false;
        }
        else {
            bool changed = false;
            m_root->bptree_traverse([&](BPlusTreeNode* node, size_t) {
                LeafNode* leaf = static_cast<LeafNode*>(node);
                changed = leaf->LeafArray::normalize_packed() || changed;
                return IteratorControl::AdvanceToNext;
            });
            if (changed)
                invalidate_leaf_cache();
            return changed;
        }
    }

protected:
    LeafNode m_leaf_cache;

    // First element of the subtree rooted at `ref` (its leftmost leaf's
    // element 0).
    T subtree_first_value(ref_type ref) const
    {
        char* header = m_alloc.translate(ref);
        while (NodeHeader::get_is_inner_bptree_node_from_header(header)) {
            // the first child ref lives in slot 1 (slot 0 is offsets/epc)
            ref = to_ref(Array::get(header, 1));
            header = m_alloc.translate(ref);
        }
        LeafArray leaf(m_alloc);
        leaf.init_from_mem(MemRef(header, ref, m_alloc));
        return leaf.get(0);
    }

    /******** Implementation of abstract interface *******/

    std::unique_ptr<BPlusTreeLeaf> create_leaf_node() override
    {
        std::unique_ptr<BPlusTreeLeaf> leaf = std::make_unique<LeafNode>(this);
        static_cast<LeafNode*>(leaf.get())->create();
        return leaf;
    }
    std::unique_ptr<BPlusTreeLeaf> init_leaf_node(ref_type ref) override
    {
        std::unique_ptr<BPlusTreeLeaf> leaf = std::make_unique<LeafNode>(this);
        leaf->init_from_ref(ref);
        return leaf;
    }
    BPlusTreeLeaf* cache_leaf(MemRef mem) override
    {
        m_leaf_cache.init_from_mem(mem);
        return &m_leaf_cache;
    }
    void replace_root(std::unique_ptr<BPlusTreeNode> new_root) override
    {
        // Only copy context flag over in a linklist.
        // The flag is in use in other list types
        if constexpr (std::is_same_v<T, ObjKey>) {
            auto cf = m_root ? m_root->get_context_flag() : false;
            BPlusTreeBase::replace_root(std::move(new_root));
            m_root->set_context_flag(cf);
        }
        else {
            BPlusTreeBase::replace_root(std::move(new_root));
        }
    }

    template <class R>
    friend R bptree_sum(const BPlusTree<T>& tree);

    void split_root();
};

template <class T>
using SumAggType = typename aggregate_operations::Sum<typename util::RemoveOptional<T>::type>;

template <class T>
typename SumAggType<T>::ResultType bptree_sum(const BPlusTree<T>& tree, size_t* return_cnt = nullptr)
{
    SumAggType<T> agg;

    auto func = [&agg](BPlusTreeNode* node, size_t) {
        auto leaf = static_cast<typename BPlusTree<T>::LeafNode*>(node);
        size_t sz = leaf->size();
        for (size_t i = 0; i < sz; i++) {
            auto val = leaf->get(i);
            agg.accumulate(val);
        }
        return IteratorControl::AdvanceToNext;
    };

    tree.traverse(func);

    if (return_cnt)
        *return_cnt = agg.items_counted();

    return agg.result();
}

template <class AggType, class T>
util::Optional<typename util::RemoveOptional<T>::type> bptree_min_max(const BPlusTree<T>& tree,
                                                                      size_t* return_ndx = nullptr)
{
    AggType agg;
    if (tree.size() == 0) {
        if (return_ndx)
            *return_ndx = not_found;
        return util::none;
    }

    auto func = [&agg, return_ndx](BPlusTreeNode* node, size_t offset) {
        auto leaf = static_cast<typename BPlusTree<T>::LeafNode*>(node);
        size_t sz = leaf->size();
        for (size_t i = 0; i < sz; i++) {
            auto val_or_null = leaf->get(i);
            bool found_new_min = agg.accumulate(val_or_null);
            if (found_new_min && return_ndx) {
                *return_ndx = i + offset;
            }
        }
        return IteratorControl::AdvanceToNext;
    };

    tree.traverse(func);

    return agg.is_null() ? util::none : std::optional{agg.result()};
}

template <class T>
using MinAggType = typename aggregate_operations::Minimum<typename util::RemoveOptional<T>::type>;

template <class T>
util::Optional<typename util::RemoveOptional<T>::type> bptree_minimum(const BPlusTree<T>& tree,
                                                                      size_t* return_ndx = nullptr)
{
    return bptree_min_max<MinAggType<T>, T>(tree, return_ndx);
}

template <class T>
using MaxAggType = typename aggregate_operations::Maximum<typename util::RemoveOptional<T>::type>;

template <class T>
util::Optional<typename util::RemoveOptional<T>::type> bptree_maximum(const BPlusTree<T>& tree,
                                                                      size_t* return_ndx = nullptr)
{
    return bptree_min_max<MaxAggType<T>, T>(tree, return_ndx);
}

template <class T>
ColumnAverageType<T> bptree_average(const BPlusTree<T>& tree, size_t* return_cnt = nullptr)
{
    size_t cnt;
    auto sum = bptree_sum(tree, &cnt);
    ColumnAverageType<T> avg{};
    if (cnt != 0)
        avg = ColumnAverageType<T>(sum) / cnt;
    if (return_cnt)
        *return_cnt = cnt;
    return avg;
}
} // namespace llbt

#endif /* LLBT_BPLUSTREE_HPP */
