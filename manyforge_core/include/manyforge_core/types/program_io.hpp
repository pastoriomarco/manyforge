#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/io_signal.hpp>

namespace manyforge::core {

/// Internal request bound to the existing captured Program authority. The
/// caller names portable roles, never an asserted physical signal identity.
struct ProgramIoWatchRequestDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ProgramBindingId program_binding_id;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  ProgramNodeExecutionScopeDTO node;
  TargetRoleId target_role_id;
  ResourceRoleId resource_role_id;
  bool expected_value;
};

/// Exact captured attempt end when the registration reply was unavailable.
/// This grants no signal/Run authority and carries no asserted watch token.
struct ProgramIoWatchScopeReleaseRequestDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ProgramBindingId program_binding_id;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  ProgramNodeExecutionScopeDTO node;
};

}  // namespace manyforge::core
