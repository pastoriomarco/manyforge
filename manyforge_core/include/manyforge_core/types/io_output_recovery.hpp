#pragma once

#include <manyforge_core/types/io_output.hpp>

namespace manyforge::core {

/// Summary of actual completed local Disable then Enable in existing CCLC
/// durable history. No remote queue drain or output undo is asserted.
struct IoOutputLocalResynchronizationProofDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ControllableComponentId controllable_component_id;
  ResolvedLifecycleProviderOccurrenceDTO provider;
  LifecycleTransactionId disable_transaction_id;
  LifecycleTransactionId enable_transaction_id;
  ComponentLifecycleGeneration disabled_generation;
  ComponentLifecycleGeneration enabled_generation;
  friend bool operator==(const IoOutputLocalResynchronizationProofDTO&,
                         const IoOutputLocalResynchronizationProofDTO&) = default;
};

/// Exact process-local Kernel authorization after durable operator acceptance.
/// Public payloads/providers cannot supply this authority. Retiring local
/// correlation preserves unknown-effect history; remote work may still execute.
struct IoOutputResetAuthorityDTO final {
  IoOutputCommandIdentityDTO command;
  RequestId acknowledgement_request_id;
  OperationRevision acknowledged_operation_revision;
  IoOutputLocalResynchronizationProofDTO local_resynchronization;
  friend bool operator==(const IoOutputResetAuthorityDTO&,
                         const IoOutputResetAuthorityDTO&) = default;
};

}  // namespace manyforge::core
