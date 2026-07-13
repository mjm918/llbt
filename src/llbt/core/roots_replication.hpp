/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 * This file is derived from history.cpp of Barq Core / Realm Core and
 * modified by the llbt project: the history stores no
 * changesets — its only job is to satisfy the Replication/History contract
 * so that llbt::core can anchor its named-root registry in the history
 * slot of the file's top array.
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
#ifndef LLBT_CORE_ROOTS_REPLICATION_HPP
#define LLBT_CORE_ROOTS_REPLICATION_HPP

#include <memory>

namespace llbt {
class Replication;
namespace core_detail {

/// Schema version stored in the file's history slots for llbt::core files.
/// Distinct from Barq's in-barq history schema so that opening a Barq file
/// with llbt (or vice versa) fails cleanly instead of misreading data.
// Schema 1003 marks format-25 files which may contain packed binary leaves.
// Builds which only know schema 1002 reject these files before reading a
// packed descriptor as the old big-blob layout.
constexpr int g_llbt_roots_schema_version = 1003;

/// Registry layout, anchored at the top array's history-ref slot:
///   registry[0] = ref  -> BPlusTree<StringData>  (root names)
///   registry[1] = ref  -> Array(hasRefs)         (root refs, parallel)
///   registry[2] = ref  -> Array(tagged ints)     (type tags, parallel)
///   registry[3] = ref  -> commit journal descriptor
constexpr int g_llbt_registry_size = 4;

std::unique_ptr<Replication> make_roots_replication();

} // namespace core_detail
} // namespace llbt

#endif // LLBT_CORE_ROOTS_REPLICATION_HPP
