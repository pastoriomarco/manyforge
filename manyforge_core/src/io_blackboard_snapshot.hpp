#pragma once

#include <manyforge_core/types/io_blackboard.hpp>
#include <memory>

namespace manyforge::core::detail {

// The one snapshot DTO lives in types/io_blackboard.hpp so rule evidence can
// retain the same immutable source-owned pointer without copying values.

/// Pure preflight and construction: previous remains unchanged on every path.
/// A null previous accepts only baseline 1/0 with exact trusted binding W.
/// Otherwise only a nonempty changed-key subset at exactly previous+1 is valid.
/// Source replacement and actual sequence-loss/ACK decisions belong to the
/// existing source owner. Null values preserve each key's declared type and
/// mark only that key unavailable. Result values contain full W, name sorted.
///
/// maximum_value_bytes counts each retained key name once plus scalar payload:
/// unavailable=0, bool=1, int64/float64=8, string=actual bytes. The source owner
/// separately qualifies compiled key counts/identity limits; no smaller string
/// cap or new public capacity setting is introduced here.
[[nodiscard]] Result<std::shared_ptr<const IoProgramBlackboardSnapshotDTO>>
ValidateAndBuildProgramBlackboardSnapshot(
    const IoProgramBlackboardSourceBindingDTO& binding,
    const std::shared_ptr<const IoProgramBlackboardSnapshotDTO>& previous,
    const IoProgramBlackboardPublicationDTO& publication);

}  // namespace manyforge::core::detail
