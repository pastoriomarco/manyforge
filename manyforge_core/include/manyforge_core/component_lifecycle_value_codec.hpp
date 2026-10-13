#pragma once

#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Closed mechanical values only. Decoding never verifies a live receipt or
/// grants admission, qualification, effect dispatch, or replay authority.
[[nodiscard]] std::string EncodeComponentLifecycleIntent(
    const ComponentLifecycleIntentDTO& intent);
[[nodiscard]] ComponentLifecycleIntentDTO DecodeComponentLifecycleIntent(
    std::string_view bytes);
[[nodiscard]] std::string EncodeSelectedComponentBringUpRequestResult(
    const CCLCLifecycleRequestResultDTO& result);
[[nodiscard]] CCLCLifecycleRequestResultDTO DecodeSelectedComponentBringUpRequestResult(
    std::string_view bytes);

/// Stable original request identity, distinct from the full owner offer hash.
/// Transport attribution is omitted exactly as in intent replay equality.
[[nodiscard]] ContentDigest SelectedComponentBringUpSemanticRequestDigest(
    const ComponentLifecycleIntentDTO& intent,
    const ProjectionRevisionDTO& candidate_projection_revision,
    const ContentDigest& candidate_retained_bundle_digest,
    std::uint64_t candidate_retained_bundle_bytes,
    const RuntimeUnitId& runtime_unit_id,
    const RuntimeEpoch& runtime_epoch,
    const RuntimeEpoch& manager_runtime_epoch);

/// Closed Q91 Prepare/Enable result under an existing Apply parent. A
/// restored value is observation data and cannot recreate a continuation.
[[nodiscard]] std::string EncodeApplyDelegatedComponentLifecycleRequestResult(
    const CCLCLifecycleRequestResultDTO& result);
[[nodiscard]] CCLCLifecycleRequestResultDTO DecodeApplyDelegatedComponentLifecycleRequestResult(
    std::string_view bytes);
/// Mechanical parent evidence only; decoding never proves a live held owner.
[[nodiscard]] std::string EncodeApplyComponentBringUpAuthority(
    const ApplyComponentBringUpAuthorityDTO& authority);
[[nodiscard]] ApplyComponentBringUpAuthorityDTO DecodeApplyComponentBringUpAuthority(
    std::string_view bytes);
[[nodiscard]] ContentDigest ApplyDelegatedComponentLifecycleSemanticRequestDigest(
    const ComponentLifecycleIntentDTO& intent,
    const ProjectionRevisionDTO& candidate_projection_revision,
    const ContentDigest& candidate_retained_bundle_digest,
    std::uint64_t candidate_retained_bundle_bytes,
    const RuntimeUnitId& runtime_unit_id,
    const RuntimeEpoch& runtime_epoch,
    const RuntimeEpoch& manager_runtime_epoch,
    const ApplyComponentBringUpReferenceDTO& parent);

}  // namespace manyforge::core
