#pragma once

#include <manyforge_core/types/program_io.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <cstddef>
#include <optional>
#include <vector>

namespace manyforge::core {

/// Node-local literal simulation data. These portable roles never assert a
/// physical signal identity. Mock providers publish the values unconditionally;
/// no provider evaluates a condition or selects an expected/trip value.
struct ProgramMockSimulationChangeDTO final {
  std::uint64_t after_ms;
  bool value;
  friend bool operator==(const ProgramMockSimulationChangeDTO&,
                         const ProgramMockSimulationChangeDTO&) = default;
};
struct ProgramMockSimulationSignalDTO final {
  TargetRoleId target_role_id;
  ResourceRoleId resource_role_id;
  bool start_value;
  std::optional<ProgramMockSimulationChangeDTO> change;
  friend bool operator==(const ProgramMockSimulationSignalDTO&,
                         const ProgramMockSimulationSignalDTO&) = default;
};
/// Current captured Run attribution, shared by the Run-wide clock receipt.
struct ProgramRunMockSimulationScopeDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ProgramBindingId program_binding_id;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  friend bool operator==(const ProgramRunMockSimulationScopeDTO&,
                         const ProgramRunMockSimulationScopeDTO&) = default;
};
struct ProgramRunMockSimulationPauseRequestDTO final {
  ProgramRunMockSimulationScopeDTO scope;
  /// Existing Runner revision: ordering only, never new admission authority.
  std::uint64_t run_revision;
  bool paused;
  /// Complete actual captured-node Hold mask, canonical instance-ID order.
  /// The existing Runner revision orders this WHOLE snapshot, including mask.
  std::vector<ProgramNodeInstanceId> held_node_instance_ids;
  friend bool operator==(const ProgramRunMockSimulationPauseRequestDTO&,
                         const ProgramRunMockSimulationPauseRequestDTO&) = default;
};
struct ProgramNodeMockSimulationScopeDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ProgramBindingId program_binding_id;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  ProgramNodeExecutionScopeDTO node;
  /// Positive full-width uint64 decimal text on the private wire. Ordering
  /// and idempotency only; current captured Run/claim supplies authority.
  std::uint64_t node_start_index;
  friend bool operator==(const ProgramNodeMockSimulationScopeDTO&,
                         const ProgramNodeMockSimulationScopeDTO&) = default;
};
struct ProgramNodeMockSimulationRequestDTO final {
  ProgramNodeMockSimulationScopeDTO scope;
  std::vector<ProgramMockSimulationSignalDTO> signals;
  friend bool operator==(const ProgramNodeMockSimulationRequestDTO&,
                         const ProgramNodeMockSimulationRequestDTO&) = default;
};
/// Trusted same-service observations resolved under the current Kernel Run
/// lock. One element per request row, in the SAME canonical selector order.
/// Public commands cannot construct or supply this source proof.
struct ResolvedProgramMockSimulationSignalDTO final {
  IoSignalId signal_id;
  ContentDigest signal_descriptor_digest;
  std::uint64_t catalog_generation;
  std::optional<IoSourceIncarnation> source_incarnation;
  friend bool operator==(const ResolvedProgramMockSimulationSignalDTO&,
                         const ResolvedProgramMockSimulationSignalDTO&) = default;
};
enum class ProgramMockSimulationSignalDisposition : std::uint8_t {
  kApplied,
  kInactive,
};
struct ProgramMockSimulationSignalReceiptDTO final {
  TargetRoleId target_role_id;
  ResourceRoleId resource_role_id;
  ProgramMockSimulationSignalDisposition disposition;
  friend bool operator==(const ProgramMockSimulationSignalReceiptDTO&,
                         const ProgramMockSimulationSignalReceiptDTO&) = default;
};
struct ProgramNodeMockSimulationAcknowledgementDTO final {
  ProgramNodeMockSimulationScopeDTO scope;
  /// Original host monotonic receipt anchor. Not a Runner-clock comparison or
  /// a new wall-clock/currentness grant. Exact replay returns the same value.
  TimePoint started_at_ns;
  std::vector<ProgramMockSimulationSignalReceiptDTO> signals;
  friend bool operator==(const ProgramNodeMockSimulationAcknowledgementDTO&,
                         const ProgramNodeMockSimulationAcknowledgementDTO&) = default;
};

}  // namespace manyforge::core
