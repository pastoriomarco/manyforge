#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/program_reference.hpp>

namespace manyforge::core {

/// Internal request bound to the existing captured Program authority. The
/// caller names the sealed Program's direct component/signal reference.
struct ProgramIoWatchRequestDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ProgramBindingId program_binding_id;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  ProgramNodeExecutionScopeDTO node;
  ProgramSignalReferenceDTO signal;
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
