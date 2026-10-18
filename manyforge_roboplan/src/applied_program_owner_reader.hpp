#pragma once

#include <filesystem>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/program_blackboard_context.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>

namespace manyforge::roboplan_backend::detail {

/// Read-only actual successful NamedApply publication. This is evidence,
/// not permission to activate a catalog without the final owner fence.
struct AppliedOwnerSnapshot final {
  core::ActiveBundlePublicationReceiptDTO publication;
  core::ContentDigest apply_intent_digest;
  core::ContentDigest terminal_outcome_digest;
};

[[nodiscard]] core::Result<AppliedOwnerSnapshot> ReadAppliedOwnerReadOnly(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    core::ActiveBundlePublicationGeneration expected_publication_generation);

/// Immutable observation of the real current named-Apply/Program join. This
/// grants no source installation or cycle authority; the final owner fence
/// must recheck the publication and Runner occurrence without filesystem I/O.
struct AppliedProgramOwnerSnapshot final {
  core::ActiveBundlePublicationReceiptDTO publication;
  core::ContentDigest apply_intent_digest;
  core::ContentDigest terminal_outcome_digest;
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
  core::ProgramBindingId program_binding_id;
};

[[nodiscard]] core::Result<AppliedProgramOwnerSnapshot>
ReadAppliedProgramOwnerReadOnly(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    core::ActiveBundlePublicationGeneration expected_publication_generation);

/// Pure sealed-product join after the read-only actual NamedApply observation.
/// No filesystem/owner callback is retained. The caller must still perform the
/// actual final composition/admission/service/currentness install fence.
[[nodiscard]] core::Result<core::AppliedProgramBlackboardContextDTO>
CompileAppliedProgramBlackboardContext(
    const AppliedProgramOwnerSnapshot &applied,
    const core::RequestOwnerEpoch &request_owner_epoch,
    std::shared_ptr<const deployment::ProgramBindingContract> program,
    std::shared_ptr<const deployment::ProgramBinding> binding,
    const deployment::RetainedBundle &bundle);

} // namespace manyforge::roboplan_backend::detail
