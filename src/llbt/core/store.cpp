/*
** llbt — Low Level Binary Tree
**
** The files authored by the llbt project (such as this one) are dedicated
** to the public domain. To the extent possible under law, all copyright and
** related rights are waived. You may use, copy, modify, merge, publish,
** distribute, sublicense, sell, fork, or build upon them for any purpose,
** commercial or not, with or without attribution, with no conditions and no
** warranty.
**
** This applies only to files authored by the llbt project. The imported
** storage engine underneath keeps its own copyright and the Apache License
** 2.0; see LICENSE and NOTICE.
*/
#include <llbt/core.hpp>

#include <llbt/group.hpp>
#include <llbt/db_options.hpp>

using namespace llbt;
using namespace llbt::core;
using gf = llbt::_impl::GroupFriend;

// ---- Registry -------------------------------------------------------------

namespace llbt::core::detail {

void Registry::bind_read(Transaction& tr)
{
    writable = false;
    ref_type ref = gf::get_history_ref(tr);
    if (!ref)
        return; // fresh file: empty registry
    top.init_from_ref(ref);
    if (auto names_ref = top.get_as_ref(0))
        names.init_from_ref(names_ref);
    if (auto roots_ref = top.get_as_ref(1))
        roots.init_from_ref(roots_ref);
    if (auto types_ref = top.get_as_ref(2))
        types.init_from_ref(types_ref);
}

void Registry::bind_write(Transaction& tr)
{
    writable = true;
    ref_type ref = gf::get_history_ref(tr);
    if (ref) {
        // Anchor the accessor chain at the history slot, then bind children.
        gf::set_history_parent(tr, top); // parents `top` at the history slot
        top.init_from_ref(ref);
        names.set_parent(&top, 0);
        roots.set_parent(&top, 1);
        types.set_parent(&top, 2);
        names.init_from_ref(top.get_as_ref(0));
        roots.init_from_ref(top.get_as_ref(1));
        types.init_from_ref(top.get_as_ref(2));
        return;
    }

    // First write to this file: create the registry and anchor it.
    gf::prepare_history_parent(tr, top, Replication::hist_InBarq,
                               core_detail::g_llbt_roots_schema_version, 0);
    top.create(NodeHeader::type_HasRefs, false, core_detail::g_llbt_registry_size, 0);
    top.update_parent();

    names.set_parent(&top, 0);
    names.create();
    roots.set_parent(&top, 1);
    roots.create(NodeHeader::type_HasRefs);
    roots.update_parent();
    types.set_parent(&top, 2);
    types.create(NodeHeader::type_Normal);
    types.update_parent();
}

size_t Registry::count() const noexcept
{
    return names.is_attached() ? names.size() : 0;
}

size_t Registry::find(StringData name_value) const noexcept
{
    if (!names.is_attached())
        return size_t(-1);
    return names.find_first(name_value);
}

StringData Registry::name(size_t i) const noexcept
{
    return names.get(i);
}

NodeRef Registry::root(size_t i) const noexcept
{
    return roots.get_as_ref(i);
}

int64_t Registry::type(size_t i) const noexcept
{
    return types.get(i);
}

size_t Registry::add(StringData name_value, int64_t type_tag)
{
    names.add(name_value);
    roots.add(0);
    types.add(type_tag);
    return names.size() - 1;
}

void Registry::set_root_ref(size_t i, NodeRef ref)
{
    ref_type old = roots.get_as_ref(i);
    if (old && old != ref)
        Array::destroy_deep(old, roots.get_alloc());
    roots.set_as_ref(i, ref);
}

void Registry::erase(size_t i)
{
    if (ref_type old = roots.get_as_ref(i))
        Array::destroy_deep(old, roots.get_alloc());
    names.erase(i);
    roots.erase(i);
    types.erase(i);
}

} // namespace llbt::core::detail

// ---- Store ----------------------------------------------------------------

namespace llbt::core {

StoreRef Store::open(const std::string& path, const Options& options)
{
    auto store = StoreRef(new Store());
    store->m_repl = core_detail::make_roots_replication();

    DBOptions db_options;
    db_options.encryption_key = options.encryption_key;
    if (options.no_sync)
        db_options.durability = DBOptions::Durability::Unsafe;
    db_options.single_process = options.single_process;
    store->m_db = DB::create(*store->m_repl, path, db_options);
    return store;
}

StoreRef Store::open_in_memory(const Options&)
{
    auto store = StoreRef(new Store());
    store->m_in_memory = true;

    // No file: the roots replication is owned by the DB (moved in), and the
    // allocator runs on an anonymous in-memory buffer. Durability::MemOnly is
    // the only mode that makes sense with no disk behind it.
    DBOptions db_options;
    db_options.durability = DBOptions::Durability::MemOnly;
    store->m_db = DB::create_in_memory(core_detail::make_roots_replication(),
                                       "<llbt in-memory>", db_options);
    return store;
}

Store::~Store() = default;

Tx Store::begin_read()
{
    return Tx(m_db->start_read(), shared_from_this(), false);
}

Tx Store::begin_write()
{
    return Tx(m_db->start_write(), shared_from_this(), true);
}

bool Store::compact()
{
    if (m_in_memory)
        return false; // nothing to rewrite: no backing file
    return m_db->compact();
}

std::string Store::path() const
{
    return m_db->get_path();
}

} // namespace llbt::core
