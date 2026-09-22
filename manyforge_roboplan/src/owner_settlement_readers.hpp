#pragma once

#include <filesystem>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <vector>

namespace manyforge::roboplan_backend::detail {

struct ActiveProgramRunProductIdentity final {
  core::ProgramRunId program_run_id;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;

  friend bool operator==(const ActiveProgramRunProductIdentity &,
                         const ActiveProgramRunProductIdentity &) = default;
};

/// Return the exact retained product captured by a nonterminal Program run
/// that a replacement owner must contain. Idle, terminal, and current-owner
/// rows return no value. The Python journal remains the sole lifecycle owner;
/// this function is a bounded read-only recovery join.
[[nodiscard]] core::Result<std::optional<ActiveProgramRunProductIdentity>>
ReadActiveProgramRunProductForReconciliation(
    const std::filesystem::path &database_path);

[[nodiscard]] core::Result<void> ValidateProgramCycleSettlementReadOnly(
    const std::filesystem::path &database_path,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release);

[[nodiscard]] core::Result<core::CellAdmissionReconcileValidationDTO>
ValidateProgramRunnerOwnerReconciliationReadOnly(
    const std::filesystem::path &database_path,
    const std::vector<core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const core::CellAdmissionReconcileDTO &reconcile);

} // namespace manyforge::roboplan_backend::detail
