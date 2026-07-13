/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 * This file is derived from history.cpp of Barq Core / Realm Core and
 * modified by the llbt project as described in
 * roots_replication.hpp.
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

#include <llbt/core/roots_replication.hpp>

#include <llbt/impl/cont_transact_hist.hpp>
#include <llbt/replication.hpp>
#include <llbt/db.hpp>

using namespace llbt;

namespace {

/// The registry itself is bound and mutated by llbt::core's transactions
/// (they parent it directly at the history slot). This History therefore
/// stores nothing; every hook is a coherent no-op.
class RootsHistory : public _impl::History {
public:
    void update_from_ref_and_version(ref_type, version_type) override {}
    void update_from_parent(version_type) override {}
    void get_changesets(version_type, version_type, BinaryIterator*) const noexcept override {}
    void set_oldest_bound_version(version_type) override {}
    void verify() const override {}
};

class RootsReplication : public Replication {
public:
    version_type prepare_changeset(const char*, size_t, version_type orig_version) override
    {
        // No changesets are stored; a commit simply produces the next version.
        return orig_version + 1;
    }

    HistoryType get_history_type() const noexcept override
    {
        // Reuses the in-file type value of the in-barq history (the top-array
        // slot layout is identical); the schema version tells them apart.
        return hist_InBarq;
    }

    int get_history_schema_version() const noexcept override
    {
        return core_detail::g_llbt_roots_schema_version;
    }

    bool is_upgradable_history_schema(int stored_schema_version) const noexcept override
    {
        return stored_schema_version == 1001 || stored_schema_version == 1002 ||
               stored_schema_version == core_detail::g_llbt_roots_schema_version;
    }

    void upgrade_history_schema(int) override {}

    _impl::History* _get_history_write() override
    {
        return &m_history;
    }

    std::unique_ptr<_impl::History> _create_history_read() override
    {
        return std::make_unique<RootsHistory>();
    }

private:
    RootsHistory m_history;
};

} // unnamed namespace

namespace llbt::core_detail {

std::unique_ptr<Replication> make_roots_replication()
{
    return std::unique_ptr<Replication>(new RootsReplication());
}

} // namespace llbt::core_detail
