#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/request_key.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Public caller identity carried into the internal owner-to-owner fence
/// request: the request's 16-byte key and its label. Transport attribution is
/// deliberately not part of this value.
struct PublicRequestContextDTO final {
  RequestKey request_key;
  std::string source_label;

  friend bool operator==(const PublicRequestContextDTO&, const PublicRequestContextDTO&) = default;
};

/// Transport-derived provenance added by the receiving adapter. Public
/// request payloads never populate or promote this value themselves.
enum class RequestTransportDTO {
  kRos,
  kHttp,
  kInternal,
};

enum class RequestAssuranceDTO {
  kUnverified,
  kExternallyAuthenticated,
};

struct RequestAttributionDTO final {
  RequestTransportDTO transport = RequestTransportDTO::kInternal;
  RequestAssuranceDTO assurance = RequestAssuranceDTO::kUnverified;
  std::optional<std::string> principal_id;

  [[nodiscard]] bool locally_valid() const noexcept {
    if (principal_id.has_value() && principal_id->empty()) {
      return false;
    }
    switch (transport) {
      case RequestTransportDTO::kRos:
      case RequestTransportDTO::kHttp:
      case RequestTransportDTO::kInternal:
        break;
      default:
        return false;
    }
    switch (assurance) {
      case RequestAssuranceDTO::kUnverified:
        return !principal_id.has_value();
      case RequestAssuranceDTO::kExternallyAuthenticated:
        return true;
    }
    return false;
  }

  friend bool operator==(const RequestAttributionDTO&, const RequestAttributionDTO&) = default;
};

enum class RequestReplayStatusDTO {
  kAcceptedNew,
  kReplayedExact,
  kRequestResultExpired,
  kRequestIdConflict,
  kRequestLedgerFull,
};

[[nodiscard]] constexpr std::string_view to_string(RequestReplayStatusDTO status) noexcept {
  switch (status) {
    case RequestReplayStatusDTO::kAcceptedNew:
      return "accepted_new";
    case RequestReplayStatusDTO::kReplayedExact:
      return "replayed_exact";
    case RequestReplayStatusDTO::kRequestResultExpired:
      return "request_result_expired";
    case RequestReplayStatusDTO::kRequestIdConflict:
      return "request_id_conflict";
    case RequestReplayStatusDTO::kRequestLedgerFull:
      return "request_ledger_full";
  }
  return {};
}

[[nodiscard]] constexpr bool IsKnownRequestReplayStatus(RequestReplayStatusDTO status) noexcept {
  switch (status) {
    case RequestReplayStatusDTO::kAcceptedNew:
    case RequestReplayStatusDTO::kReplayedExact:
    case RequestReplayStatusDTO::kRequestResultExpired:
    case RequestReplayStatusDTO::kRequestIdConflict:
    case RequestReplayStatusDTO::kRequestLedgerFull:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsAcceptedRequestReplayStatus(RequestReplayStatusDTO status) noexcept {
  switch (status) {
    case RequestReplayStatusDTO::kAcceptedNew:
    case RequestReplayStatusDTO::kReplayedExact:
      return true;
    case RequestReplayStatusDTO::kRequestResultExpired:
    case RequestReplayStatusDTO::kRequestIdConflict:
    case RequestReplayStatusDTO::kRequestLedgerFull:
      return false;
  }
  return false;
}

struct ApplyFenceOwnerDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;

  [[nodiscard]] bool valid() const noexcept {
    return apply_id.nonzero() && apply_generation.nonzero();
  }

  friend bool operator==(const ApplyFenceOwnerDTO&, const ApplyFenceOwnerDTO&) = default;
};

struct ControllerConfigurationFenceDomainDTO final {
  friend bool operator==(const ControllerConfigurationFenceDomainDTO&,
                         const ControllerConfigurationFenceDomainDTO&) = default;
};

struct ProviderConfigurationFenceOccurrenceDTO final {
  ControllableComponentId controllable_component_id;
  ProviderInstanceId provider_instance_id;
  ContentDigest signal_configuration_digest;
  std::vector<ResourceId> output_resources{};
  [[nodiscard]] bool valid() const noexcept {
    return !controllable_component_id.empty() && !provider_instance_id.empty() &&
           !signal_configuration_digest.empty() &&
           std::is_sorted(output_resources.begin(), output_resources.end()) &&
           std::adjacent_find(output_resources.begin(), output_resources.end()) ==
               output_resources.end() &&
           std::none_of(output_resources.begin(), output_resources.end(),
                        [](const auto& id) { return id.empty(); });
  }
  friend bool operator==(const ProviderConfigurationFenceOccurrenceDTO&,
                         const ProviderConfigurationFenceOccurrenceDTO&) = default;
};

struct ProviderConfigurationFenceDomainDTO final {
  std::optional<ProviderConfigurationFenceOccurrenceDTO> predecessor_provider;
  std::optional<ProviderConfigurationFenceOccurrenceDTO> candidate_provider;
  friend bool operator==(const ProviderConfigurationFenceDomainDTO&,
                         const ProviderConfigurationFenceDomainDTO&) = default;
};

struct ConfigurationFenceDomainScopeDTO final {
  ControlDomainId control_domain_id;
  std::optional<ControlDomainRevision> predecessor_control_domain_revision;
  std::optional<ControlDomainRevision> candidate_control_domain_revision;
  std::variant<ControllerConfigurationFenceDomainDTO, ProviderConfigurationFenceDomainDTO> payload{
      ControllerConfigurationFenceDomainDTO{}};

  [[nodiscard]] bool valid() const noexcept {
    const bool common = !control_domain_id.empty() &&
                        (predecessor_control_domain_revision.has_value() ||
                         candidate_control_domain_revision.has_value()) &&
                        (!predecessor_control_domain_revision.has_value() ||
                         !predecessor_control_domain_revision->empty()) &&
                        (!candidate_control_domain_revision.has_value() ||
                         !candidate_control_domain_revision->empty());
    if (!common || payload.valueless_by_exception()) return false;
    if (const auto* provider = std::get_if<ProviderConfigurationFenceDomainDTO>(&payload))
      return (provider->predecessor_provider || provider->candidate_provider) &&
             (!provider->predecessor_provider ||
              (predecessor_control_domain_revision && provider->predecessor_provider->valid())) &&
             (!provider->candidate_provider ||
              (candidate_control_domain_revision && provider->candidate_provider->valid()));
    return true;
  }

  [[nodiscard]] bool has_controller_side() const noexcept {
    const auto* provider = std::get_if<ProviderConfigurationFenceDomainDTO>(&payload);
    return !provider || (predecessor_control_domain_revision && !provider->predecessor_provider) ||
           (candidate_control_domain_revision && !provider->candidate_provider);
  }
  [[nodiscard]] bool has_controller_predecessor() const noexcept {
    if (!predecessor_control_domain_revision) return false;
    const auto* provider = std::get_if<ProviderConfigurationFenceDomainDTO>(&payload);
    return !provider || !provider->predecessor_provider;
  }
  [[nodiscard]] bool has_controller_candidate() const noexcept {
    if (!candidate_control_domain_revision) return false;
    const auto* provider = std::get_if<ProviderConfigurationFenceDomainDTO>(&payload);
    return !provider || !provider->candidate_provider;
  }

  friend bool operator==(const ConfigurationFenceDomainScopeDTO&,
                         const ConfigurationFenceDomainScopeDTO&) = default;
};

struct ConfigurationFenceBindingScopeDTO final {
  ControlBindingId control_binding_id;
  std::optional<ControlBindingRevision> predecessor_control_binding_revision;
  std::optional<ControlBindingRevision> candidate_control_binding_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !control_binding_id.empty() &&
           (predecessor_control_binding_revision.has_value() ||
            candidate_control_binding_revision.has_value()) &&
           (!predecessor_control_binding_revision.has_value() ||
            !predecessor_control_binding_revision->empty()) &&
           (!candidate_control_binding_revision.has_value() ||
            !candidate_control_binding_revision->empty());
  }

  friend bool operator==(const ConfigurationFenceBindingScopeDTO&,
                         const ConfigurationFenceBindingScopeDTO&) = default;
};

/// One flat whole-Apply admission scope.
///
/// The collections are canonical, sorted, unique, and bounded by the
/// qualified deployment. They are deliberately not partitioned by component:
/// one domain or resource may span components and must be fenced atomically.
/// Controller contributions retain their nonempty physical collections.
/// Provider contributions carry exact typed component/provider proof and do
/// not invent controller bindings or physical resources. The
/// deployment-owned pure derivation proves the binding/resource overlap
/// closure; this value layer validates only the closed canonical shape.
struct ConfigurationFenceScopeDTO final {
  std::vector<ControllableComponentId> controllable_component_ids;
  std::vector<ConfigurationFenceDomainScopeDTO> domains;
  std::vector<ConfigurationFenceBindingScopeDTO> bindings;
  std::vector<ResourceId> resources;

  [[nodiscard]] bool valid() const noexcept {
    const bool all_empty = controllable_component_ids.empty() && domains.empty() &&
                           bindings.empty() && resources.empty();
    if (all_empty) {
      return true;
    }
    const bool controller = std::any_of(domains.begin(), domains.end(), [](const auto& domain) {
      return domain.has_controller_side();
    });
    if (controllable_component_ids.empty() || domains.empty() ||
        (controller && (bindings.empty() || resources.empty())) ||
        (!controller && !bindings.empty())) {
      return false;
    }

    if (!std::is_sorted(controllable_component_ids.begin(), controllable_component_ids.end()) ||
        std::adjacent_find(controllable_component_ids.begin(), controllable_component_ids.end()) !=
            controllable_component_ids.end()) {
      return false;
    }
    for (const auto& component_id : controllable_component_ids) {
      if (component_id.empty()) {
        return false;
      }
    }

    const auto domain_less = [](const ConfigurationFenceDomainScopeDTO& lhs,
                                const ConfigurationFenceDomainScopeDTO& rhs) noexcept {
      return lhs.control_domain_id < rhs.control_domain_id;
    };
    if (!std::is_sorted(domains.begin(), domains.end(), domain_less) ||
        std::adjacent_find(domains.begin(), domains.end(),
                           [](const ConfigurationFenceDomainScopeDTO& lhs,
                              const ConfigurationFenceDomainScopeDTO& rhs) noexcept {
                             return lhs.control_domain_id == rhs.control_domain_id;
                           }) != domains.end()) {
      return false;
    }

    const auto binding_less = [](const ConfigurationFenceBindingScopeDTO& lhs,
                                 const ConfigurationFenceBindingScopeDTO& rhs) noexcept {
      return lhs.control_binding_id < rhs.control_binding_id;
    };
    if (!std::is_sorted(bindings.begin(), bindings.end(), binding_less) ||
        std::adjacent_find(bindings.begin(), bindings.end(),
                           [](const ConfigurationFenceBindingScopeDTO& lhs,
                              const ConfigurationFenceBindingScopeDTO& rhs) noexcept {
                             return lhs.control_binding_id == rhs.control_binding_id;
                           }) != bindings.end() ||
        !std::is_sorted(resources.begin(), resources.end()) ||
        std::adjacent_find(resources.begin(), resources.end()) != resources.end()) {
      return false;
    }

    for (const auto& domain : domains) {
      if (!domain.valid()) {
        return false;
      }
      if (const auto* provider =
              std::get_if<ProviderConfigurationFenceDomainDTO>(&domain.payload)) {
        for (const auto* occurrence :
             {&provider->predecessor_provider, &provider->candidate_provider}) {
          if (*occurrence && !std::binary_search(controllable_component_ids.begin(),
                                                 controllable_component_ids.end(),
                                                 (*occurrence)->controllable_component_id))
            return false;
          if (*occurrence && !std::includes(resources.begin(), resources.end(),
                                            (*occurrence)->output_resources.begin(),
                                            (*occurrence)->output_resources.end()))
            return false;
        }
      }
    }
    if (!controller) {
      for (const auto& resource : resources) {
        const bool owned = std::any_of(domains.begin(), domains.end(), [&](const auto& domain) {
          const auto* provider = std::get_if<ProviderConfigurationFenceDomainDTO>(&domain.payload);
          if (!provider) return false;
          const auto contains = [&](const auto& occurrence) {
            return occurrence && std::binary_search(occurrence->output_resources.begin(),
                                                    occurrence->output_resources.end(), resource);
          };
          return contains(provider->predecessor_provider) || contains(provider->candidate_provider);
        });
        if (!owned) return false;
      }
    }
    for (const auto& binding : bindings) {
      if (!binding.valid()) {
        return false;
      }
    }
    for (const auto& resource : resources) {
      if (resource.empty()) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(const ConfigurationFenceScopeDTO&,
                         const ConfigurationFenceScopeDTO&) = default;
};

enum class ApplyFenceBusyDispositionDTO {
  kRejectIfAffectedBusy,
};

[[nodiscard]] constexpr std::string_view to_string(
    ApplyFenceBusyDispositionDTO disposition) noexcept {
  switch (disposition) {
    case ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy:
      return "reject_if_affected_busy";
  }
  return {};
}

[[nodiscard]] constexpr bool IsKnownApplyFenceBusyDisposition(
    ApplyFenceBusyDispositionDTO disposition) noexcept {
  return disposition == ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy;
}

struct DomainActiveControlConfigurationDTO final {
  ControlDomainId control_domain_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !control_domain_id.empty() && !active_control_configuration_id.empty() &&
           active_control_configuration_generation.nonzero();
  }

  friend bool operator==(const DomainActiveControlConfigurationDTO&,
                         const DomainActiveControlConfigurationDTO&) = default;
};

/// The predecessor rows an Apply fence pins: at most one valid row per
/// controller-predecessor domain of `scope`, in scope order. A domain with no
/// reconciled active configuration (its component was never Enabled, or was
/// Disabled) has no row; the execution owner decides which domains those are.
[[nodiscard]] inline bool PredecessorConfigurationRowsValid(
    const ConfigurationFenceScopeDTO& scope,
    const std::vector<DomainActiveControlConfigurationDTO>& rows) noexcept {
  std::size_t index = 0U;
  for (const auto& domain : scope.domains) {
    if (!domain.has_controller_predecessor() || index >= rows.size() ||
        rows[index].control_domain_id != domain.control_domain_id) {
      continue;
    }
    if (!rows[index++].valid()) {
      return false;
    }
  }
  return index == rows.size();
}

/// Apply-specific projection of the generic configuration-fence intent.
///
/// The generic `ConfigurationFenceIntentDTO` name remains reserved for spec
/// 210's later Apply/lifecycle/route-selection union. Component Manager can
/// construct only this Apply-owned value.
struct ApplyConfigurationFenceIntentDTO final {
  PublicRequestContextDTO owner_request_context;
  ApplyFenceOwnerDTO owner_transaction;
  ConfigurationFenceScopeDTO scope;
  ApplyFenceBusyDispositionDTO busy_disposition;
  RetainedBundleId expected_retained_bundle_id;
  GraphRevision expected_graph_revision;
  std::uint64_t expected_operation_resource_generation;
  ConfigurationFenceGeneration expected_fence_generation;
  std::vector<DomainActiveControlConfigurationDTO> expected_active_control_configurations;
  std::string reason;

  [[nodiscard]] bool valid() const noexcept {
    if (owner_request_context.source_label.empty() || !owner_transaction.valid() ||
        !scope.valid() || !IsKnownApplyFenceBusyDisposition(busy_disposition) ||
        expected_retained_bundle_id.empty() || expected_graph_revision.empty() || reason.empty()) {
      return false;
    }

    const auto configuration_less = [](const DomainActiveControlConfigurationDTO& lhs,
                                       const DomainActiveControlConfigurationDTO& rhs) noexcept {
      return lhs.control_domain_id < rhs.control_domain_id;
    };
    if (!std::is_sorted(expected_active_control_configurations.begin(),
                        expected_active_control_configurations.end(), configuration_less) ||
        std::adjacent_find(expected_active_control_configurations.begin(),
                           expected_active_control_configurations.end(),
                           [](const DomainActiveControlConfigurationDTO& lhs,
                              const DomainActiveControlConfigurationDTO& rhs) noexcept {
                             return lhs.control_domain_id == rhs.control_domain_id;
                           }) != expected_active_control_configurations.end()) {
      return false;
    }

    return PredecessorConfigurationRowsValid(scope, expected_active_control_configurations);
  }

  friend bool operator==(const ApplyConfigurationFenceIntentDTO&,
                         const ApplyConfigurationFenceIntentDTO&) = default;
};

struct ConfigurationFenceAcceptedDTO final {
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  ConfigurationFenceScopeDTO captured_scope;
  std::int64_t accepted_at_ns;
  RequestReplayStatusDTO replay_status;

  [[nodiscard]] bool valid() const noexcept {
    return !configuration_fence_id.empty() && configuration_fence_generation.nonzero() &&
           captured_scope.valid() && accepted_at_ns >= 0 &&
           IsAcceptedRequestReplayStatus(replay_status);
  }

  /// Exact successful Acquire relation.
  ///
  /// `expected_fence_generation` is a compare-and-swap predecessor. A new
  /// acceptance is its immediate successor; exact request replay returns that
  /// same accepted identity and therefore satisfies the same relation.
  [[nodiscard]] bool matches(const ApplyConfigurationFenceIntentDTO& intent) const noexcept {
    const auto predecessor = intent.expected_fence_generation.value();
    return valid() && intent.valid() && captured_scope == intent.scope &&
           predecessor < std::numeric_limits<std::uint64_t>::max() &&
           configuration_fence_generation.value() == predecessor + 1U;
  }

  friend bool operator==(const ConfigurationFenceAcceptedDTO&,
                         const ConfigurationFenceAcceptedDTO&) = default;
};

enum class ConfigurationFenceState {
  kHeld,
  kReleased,
};

[[nodiscard]] constexpr std::string_view to_string(ConfigurationFenceState state) noexcept {
  switch (state) {
    case ConfigurationFenceState::kHeld:
      return "held";
    case ConfigurationFenceState::kReleased:
      return "released";
  }
  return {};
}

/// The execution host's runtime unit and fence ledger as an Apply sees them.
struct ExecutionHostEpochDTO final {
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;
  ContentDigest durable_stop_fence_ledger_digest;
  ActiveBundlePublicationGeneration publication_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !runtime_unit_id.empty() && !runtime_epoch.empty() &&
           !durable_stop_fence_ledger_digest.empty() && publication_generation.nonzero();
  }

  friend bool operator==(const ExecutionHostEpochDTO&, const ExecutionHostEpochDTO&) = default;
};

/// Read-only Kernel admission observation used to construct one exact
/// compare-and-swap Apply-fence request.
///
/// This is sampled once on the Manager command strand before Acquire. It is
/// not caller authorization and it is not consulted on motion hot paths.
/// Acquire rechecks every returned generation and active-configuration row
/// atomically with reject-if-busy admission, so a concurrent operation or
/// configuration change cannot be accepted from a stale observation.
struct ConfigurationFenceAdmissionQueryDTO final {
  ConfigurationFenceScopeDTO affected_scope;

  [[nodiscard]] bool valid() const noexcept {
    return affected_scope.valid();
  }

  friend bool operator==(const ConfigurationFenceAdmissionQueryDTO&,
                         const ConfigurationFenceAdmissionQueryDTO&) = default;
};

struct ConfigurationFenceAdmissionSnapshotDTO final {
  ConfigurationFenceAdmissionQueryDTO echoed_query;
  ExecutionHostEpochDTO execution_host_epoch;
  ProjectionRevisionDTO active_projection_revision;
  ContentDigest active_public_pointer_digest;
  std::uint64_t operation_resource_generation;
  ConfigurationFenceGeneration configuration_fence_generation;
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations;
  std::uint64_t affected_queued_operation_count;
  std::uint64_t affected_active_operation_count;
  ContentDigest result_digest;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!echoed_query.valid() || !execution_host_epoch.valid() ||
        !active_projection_revision.valid() ||
        active_projection_revision.retained_bundle_id.empty() ||
        active_projection_revision.graph_revision.empty() || active_public_pointer_digest.empty() ||
        result_digest.empty()) {
      return false;
    }

    return PredecessorConfigurationRowsValid(echoed_query.affected_scope,
                                             active_control_configurations);
  }

  [[nodiscard]] bool matches(const ConfigurationFenceAdmissionQueryDTO& query) const noexcept {
    return presence_valid() && query.valid() && echoed_query == query;
  }

  friend bool operator==(const ConfigurationFenceAdmissionSnapshotDTO&,
                         const ConfigurationFenceAdmissionSnapshotDTO&) = default;
};

/// Kernel-owned observation used to compare-and-swap an exact fence release.
///
/// It binds the fence identity and whole-Apply scope to the current Kernel
/// incarnation, active projection/publication, the control configurations
/// currently active for the scope's controller domains (a domain with none,
/// e.g. new to the cell before install, has no row), affected-operation
/// counts, and an owner-computed digest.
struct ConfigurationFenceReconciliationEvidenceDTO final {
  ExecutionHostEpochDTO execution_host_epoch;
  ApplyFenceOwnerDTO owner_transaction;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  ConfigurationFenceScopeDTO reconciled_scope;
  std::uint64_t operation_resource_generation;
  GraphRevision active_graph_revision;
  ProjectionRevisionDTO active_projection_revision;
  ActiveBundlePublicationGeneration active_publication_generation;
  ContentDigest active_public_pointer_digest;
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations;
  std::uint64_t affected_queued_operation_count;
  std::uint64_t affected_active_operation_count;
  ContentDigest evidence_digest;

  [[nodiscard]] bool valid() const noexcept {
    if (!execution_host_epoch.valid() || !owner_transaction.valid() ||
        configuration_fence_id.empty() || !configuration_fence_generation.nonzero() ||
        !reconciled_scope.valid() || active_graph_revision.empty() ||
        !active_projection_revision.valid() ||
        active_projection_revision.graph_revision != active_graph_revision ||
        !active_publication_generation.nonzero() || active_public_pointer_digest.empty() ||
        evidence_digest.empty() ||
        execution_host_epoch.publication_generation != active_publication_generation) {
      return false;
    }

    std::size_t configuration_index = 0U;
    for (const auto& domain : reconciled_scope.domains) {
      if (!domain.has_controller_side() ||
          configuration_index >= active_control_configurations.size() ||
          active_control_configurations[configuration_index].control_domain_id !=
              domain.control_domain_id) {
        continue;
      }
      if (!active_control_configurations[configuration_index++].valid()) {
        return false;
      }
    }
    return configuration_index == active_control_configurations.size();
  }

  friend bool operator==(const ConfigurationFenceReconciliationEvidenceDTO&,
                         const ConfigurationFenceReconciliationEvidenceDTO&) = default;
};

/// Exact compare-and-swap release request. The evidence is obtained only from
/// the Kernel status capability after scene Finalize is durable, or after the
/// execution host refused the Apply before any effect.
struct ConfigurationFenceReleaseDTO final {
  ConfigurationFenceReconciliationEvidenceDTO reconciliation_evidence;

  [[nodiscard]] bool valid() const noexcept {
    return reconciliation_evidence.valid() &&
           reconciliation_evidence.affected_queued_operation_count == 0U &&
           reconciliation_evidence.affected_active_operation_count == 0U;
  }

  friend bool operator==(const ConfigurationFenceReleaseDTO&,
                         const ConfigurationFenceReleaseDTO&) = default;
};

struct ConfigurationFenceReleasedDTO final {
  ConfigurationFenceReleaseDTO request;
  ExecutionHostEpochDTO execution_host_epoch;
  std::int64_t released_at_ns;
  RequestReplayStatusDTO replay_status;
  ContentDigest release_receipt_digest;

  [[nodiscard]] bool valid() const noexcept {
    return request.valid() && execution_host_epoch.valid() &&
           execution_host_epoch == request.reconciliation_evidence.execution_host_epoch &&
           released_at_ns >= 0 && IsAcceptedRequestReplayStatus(replay_status) &&
           !release_receipt_digest.empty();
  }

  friend bool operator==(const ConfigurationFenceReleasedDTO&,
                         const ConfigurationFenceReleasedDTO&) = default;
};

struct ConfigurationFenceStatusQueryDTO final {
  ApplyFenceOwnerDTO owner_transaction;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  ConfigurationFenceScopeDTO expected_scope;

  [[nodiscard]] bool valid() const noexcept {
    return owner_transaction.valid() && !configuration_fence_id.empty() &&
           configuration_fence_generation.nonzero() && expected_scope.valid();
  }

  friend bool operator==(const ConfigurationFenceStatusQueryDTO&,
                         const ConfigurationFenceStatusQueryDTO&) = default;
};

struct ConfigurationFenceStatusDTO final {
  ConfigurationFenceStatusQueryDTO echoed_query;
  ConfigurationFenceReconciliationEvidenceDTO reconciliation_evidence;
  ConfigurationFenceState state;
  bool admission_fenced;
  std::optional<ConfigurationFenceReleasedDTO> release_receipt;
  std::string required_compensation_or_operator_direction;
  ContentDigest result_digest;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!echoed_query.valid() || !reconciliation_evidence.valid() || result_digest.empty() ||
        reconciliation_evidence.owner_transaction != echoed_query.owner_transaction ||
        reconciliation_evidence.configuration_fence_id != echoed_query.configuration_fence_id ||
        reconciliation_evidence.configuration_fence_generation !=
            echoed_query.configuration_fence_generation ||
        reconciliation_evidence.reconciled_scope != echoed_query.expected_scope ||
        (error.has_value() && error->code.empty())) {
      return false;
    }
    switch (state) {
      case ConfigurationFenceState::kHeld:
        return admission_fenced && !release_receipt.has_value() &&
               required_compensation_or_operator_direction.empty() && !error.has_value();
      case ConfigurationFenceState::kReleased:
        return !admission_fenced && release_receipt.has_value() && release_receipt->valid() &&
               release_receipt->request.reconciliation_evidence == reconciliation_evidence &&
               required_compensation_or_operator_direction.empty() && !error.has_value();
    }
    return false;
  }

  [[nodiscard]] bool matches(const ConfigurationFenceStatusQueryDTO& query) const noexcept {
    return presence_valid() && query.valid() && echoed_query == query;
  }
};

/// Exact durable Manager attempt used to reconcile a lost Acquire response,
/// or to observe the original held fence before candidate installation.
/// The latter read grants no retry or acceptance write authority.
///
/// The complete immutable intent is included deliberately: request identity
/// alone cannot distinguish an exact result from an ID conflict. The attempt
/// digest binds this value to the Manager's append-only journal record.
struct ApplyFenceAcquireAttemptQueryDTO final {
  ApplyConfigurationFenceIntentDTO persisted_intent;
  ContentDigest persisted_attempt_digest;

  [[nodiscard]] bool valid() const noexcept {
    return persisted_intent.valid() && !persisted_attempt_digest.empty();
  }

  friend bool operator==(const ApplyFenceAcquireAttemptQueryDTO&,
                         const ApplyFenceAcquireAttemptQueryDTO&) = default;
};

enum class ApplyFenceAcquireAttemptReconcileState {
  kAcceptedExact,
  kNotSeen,
  kConflicting,
  kUncertain,
};

[[nodiscard]] constexpr std::string_view to_string(
    ApplyFenceAcquireAttemptReconcileState state) noexcept {
  switch (state) {
    case ApplyFenceAcquireAttemptReconcileState::kAcceptedExact:
      return "accepted_exact";
    case ApplyFenceAcquireAttemptReconcileState::kNotSeen:
      return "not_seen";
    case ApplyFenceAcquireAttemptReconcileState::kConflicting:
      return "conflicting";
    case ApplyFenceAcquireAttemptReconcileState::kUncertain:
      return "uncertain";
  }
  return {};
}

/// Closed read-only result for an exact Manager acquire-attempt query.
/// Scene configuration adoption also reads an already accepted original attempt before
/// installation; that observation does not enter the lost-Acquire recovery workflow.
///
/// `kNotSeen` is the sole state that permits retrying the exact persisted
/// Acquire request. `kAcceptedExact` supplies the result that Manager must
/// append. Conflict or uncertainty always
/// leaves admission fenced and requires explicit recovery direction.
struct ApplyFenceAcquireAttemptReconcileResultDTO final {
  ExecutionHostEpochDTO execution_host_epoch;
  ApplyFenceAcquireAttemptQueryDTO echoed_query;
  ApplyFenceAcquireAttemptReconcileState state;
  std::optional<ConfigurationFenceAcceptedDTO> accepted_fence;
  bool admission_fenced;
  std::string required_compensation_or_operator_direction;
  ContentDigest result_digest;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!execution_host_epoch.valid() || !echoed_query.valid() || result_digest.empty()) {
      return false;
    }
    const bool has_error = error.has_value();
    if (has_error && error->code.empty()) {
      return false;
    }

    switch (state) {
      case ApplyFenceAcquireAttemptReconcileState::kAcceptedExact:
        return accepted_fence.has_value() &&
               accepted_fence->matches(echoed_query.persisted_intent) && admission_fenced &&
               !has_error && required_compensation_or_operator_direction.empty();
      case ApplyFenceAcquireAttemptReconcileState::kNotSeen:
        return !accepted_fence.has_value() && !has_error &&
               required_compensation_or_operator_direction.empty();
      case ApplyFenceAcquireAttemptReconcileState::kConflicting:
      case ApplyFenceAcquireAttemptReconcileState::kUncertain:
        return !accepted_fence.has_value() && admission_fenced && has_error &&
               !required_compensation_or_operator_direction.empty();
    }
    return false;
  }

  [[nodiscard]] bool matches(const ApplyFenceAcquireAttemptQueryDTO& query) const noexcept {
    return presence_valid() && query.valid() && echoed_query == query;
  }
};

}  // namespace manyforge::core
