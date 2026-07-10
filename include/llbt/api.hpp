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
#ifndef LLBT_API_HPP
#define LLBT_API_HPP

// The llbt public surface — a single-file embedded page store:
//
//   llbt::util::File / File::Map<T>   files, locking, mmap        (raw I/O)
//   llbt::core::Store / Tx            one file, crash-safe commits, MVCC
//   named roots                       durable anchors ("buckets")
//   llbt::core::Tree<T> + Cursor      durable B+tree sequences
//   Tx::alloc() + Array nodes         bring your own data structure
//
// See examples/ for each layer in action.

#include <llbt/util/file.hpp>
#include <llbt/core.hpp>

#endif // LLBT_API_HPP
