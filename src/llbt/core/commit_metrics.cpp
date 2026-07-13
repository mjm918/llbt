/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
**
** Dedicated to the public domain under the same terms as the llbt-authored
** core API files. See LICENSE and NOTICE.
*/
#include <llbt/commit_metrics.hpp>

namespace llbt::core {

void CommitMetrics::reset() noexcept
{
    *this = CommitMetrics{};
}

} // namespace llbt::core
