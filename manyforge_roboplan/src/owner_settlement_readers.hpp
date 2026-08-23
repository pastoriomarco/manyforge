#pragma once

#include <filesystem>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <vector>

namespace manyforge::roboplan_backend::detail {

[[nodiscard]] core::Result<void> ValidateProgramCycleSettlementReadOnly(
    const std::filesystem::path &database_path,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release);

[[nodiscard]] core::Result<void> ValidateAuthoringSettlementReadOnly(
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

[[nodiscard]] core::Result<core::CellAdmissionReconcileValidationDTO>
ValidateAuthoringOwnerReconciliationReadOnly(
    const std::filesystem::path &database_path,
    const std::vector<core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const core::CellAdmissionReconcileDTO &reconcile);

} // namespace manyforge::roboplan_backend::detail
