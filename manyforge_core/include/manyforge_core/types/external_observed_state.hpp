#pragma once

#include <cstdint>
#include <manyforge_core/types/observed_state.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <string>
#include <vector>

namespace manyforge::core {

/// Immutable deployment/instance contract for one external observed-state
/// source. It contains no host, socket, ROS, process, or caller-authority
/// identity; a transport adapter binds those private details separately.
struct ExternalObservedStateSourceContractDTO final {
  CellId cell_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ProjectionStageGeneration projection_stage_generation;
  RuntimeUnitId receiver_runtime_unit_id;
  RuntimeEpoch receiver_runtime_epoch;
  RuntimeUnitId source_runtime_unit_id;
  std::vector<ModelInstanceId> model_instance_ids;
  ObservedStateQueryContextDTO query_context;
  std::string source_id;
  ObservedStateSourceKind publication_source{ObservedStateSourceKind::kJointState};

  friend bool operator==(const ExternalObservedStateSourceContractDTO&,
                         const ExternalObservedStateSourceContractDTO&) = default;
};

/// One transport-decoded external publication. The contract digest binds the
/// sample to the receiver's construction-fixed shared-cell/instance contract.
/// `receiver_received_timestamp_ns` is stamped once in the receiving machine's
/// monotonic clock domain; sender-local receipt timestamps are never trusted
/// for freshness or cross-source skew.
struct ExternalObservedStatePublicationDTO final {
  ContentDigest source_contract_digest;
  ObservedStatePublicationDTO publication;
  std::int64_t receiver_received_timestamp_ns{};

  friend bool operator==(const ExternalObservedStatePublicationDTO&,
                         const ExternalObservedStatePublicationDTO&) = default;
};

enum class ExternalObservedStateIngestDisposition : std::uint8_t {
  kAccepted = 0,
  kDuplicate,
};

/// Validate the closed source contract, including canonical instance/resource
/// ordering and authority/freshness compatibility.
[[nodiscard]] Result<void> validate_external_observed_state_source_contract(
    const ExternalObservedStateSourceContractDTO& contract);

/// Deterministic content identity used by a transport handshake and every
/// subsequent compact sample. This is runtime-generated compatibility
/// identity, not authentication or a command permission.
[[nodiscard]] Result<ContentDigest> external_observed_state_source_contract_digest(
    const ExternalObservedStateSourceContractDTO& contract);

}  // namespace manyforge::core
