#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/lifecycle_provider_occurrence.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxLifecycleExpectedDomains = 256U;
inline constexpr std::size_t kMaxLifecycleResolvedDomains = 256U;
inline constexpr std::size_t kMaxLifecycleExpectedBindings = 4096U;
inline constexpr std::size_t kMaxLifecycleResolvedBindings = 4096U;
inline constexpr std::size_t kMaxLifecycleFacts = 256U;
inline constexpr std::size_t kMaxLifecycleCurrentResources = 4096U;
inline constexpr std::size_t kMaxLifecycleIdentityBytes = 128U;
inline constexpr std::size_t kMaxLifecycleRequestLabelBytes = 256U;
inline constexpr std::size_t kMaxLifecycleErrorCodeBytes = 128U;
inline constexpr std::size_t kMaxLifecycleErrorMessageBytes = 1024U;
inline constexpr std::size_t kMaxLifecycleErrorDetails = 8U;
/// The response cap of one component call (a controller-manager
/// switch or hardware-state request, a vendor service): no answer within
/// min(this cap, the step's remaining time) ends the wait. 10 s until
/// measured live.
inline constexpr std::int64_t kComponentCallResponseCapNs = 10'000'000'000LL;
inline constexpr std::size_t kMaxLifecycleErrorDetailKeyBytes = 64U;
inline constexpr std::size_t kMaxLifecycleErrorDetailValueBytes = 256U;
inline constexpr std::size_t kMaxLifecycleKinematicSourceProjections = 4096U;
inline constexpr std::size_t kMaxLifecycleKinematicContributingBindings = 8192U;
inline constexpr std::size_t kMaxLifecycleFactTokenBytes = 128U;
inline constexpr std::size_t kMaxLifecycleProviderOccurrences = 256U;
inline constexpr std::int64_t kMaxLifecycleTransactionTimeoutNs = 3'600'000'000'000LL;

enum class ComponentLifecycleStateDTO : std::uint8_t {
  kUndeclared = 0,
  kDeclared,
  kPrepared,
  kActive,
  kDegraded,
  kFaulted,
  kDisabling,
  kDisabled,
};

enum class ExpectedActiveControlConfigurationPresenceDTO : std::uint8_t {
  kAbsent = 0,
  kPresent,
};

enum class LifecycleFactValueKindDTO : std::uint8_t {
  kUnknown = 0,
  kBoolean,
  kInteger,
  kToken,
};

[[nodiscard]] constexpr std::string_view to_string(ComponentLifecycleStateDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleStateDTO::kUndeclared:
      return "undeclared";
    case ComponentLifecycleStateDTO::kDeclared:
      return "declared";
    case ComponentLifecycleStateDTO::kPrepared:
      return "prepared";
    case ComponentLifecycleStateDTO::kActive:
      return "active";
    case ComponentLifecycleStateDTO::kDegraded:
      return "degraded";
    case ComponentLifecycleStateDTO::kFaulted:
      return "faulted";
    case ComponentLifecycleStateDTO::kDisabling:
      return "disabling";
    case ComponentLifecycleStateDTO::kDisabled:
      return "disabled";
  }
  return {};
}

struct ExpectedActiveControlConfigurationDTO final {
  ExpectedActiveControlConfigurationPresenceDTO presence;
  std::optional<ActiveControlConfigurationId> active_control_configuration_id;
  std::optional<ActiveControlConfigurationGeneration> active_control_configuration_generation;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ExpectedActiveControlConfigurationDTO&,
                         const ExpectedActiveControlConfigurationDTO&) = default;
};

struct ExpectedControlBindingRevisionDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlRouteId control_route_id;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ExpectedControlBindingRevisionDTO&,
                         const ExpectedControlBindingRevisionDTO&) = default;
};

struct LifecycleFactValueDTO final {
  LifecycleFactValueKindDTO kind;
  std::optional<bool> boolean_value;
  std::optional<std::int64_t> integer_value;
  std::optional<std::string> token_value;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleFactValueDTO&, const LifecycleFactValueDTO&) = default;
};

struct LifecycleFactDTO final {
  LifecycleFactId lifecycle_fact_id;
  LifecycleFactValueDTO value;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleFactDTO&, const LifecycleFactDTO&) = default;
};

struct BrandLifecycleObservationDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  std::uint64_t source_sequence;
  std::uint64_t observation_generation;
  std::int64_t source_timestamp_ns;
  std::int64_t received_timestamp_ns;
  std::int64_t fresh_until_ns;
  ComponentLifecycleStateDTO observed_state;
  std::vector<LifecycleFactDTO> facts;
  // Bounded diagnostics from the same observation. They never determine
  // lifecycle eligibility, fault classification, or containment.
  std::optional<ErrorDTO> diagnostic{};
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const BrandLifecycleObservationDTO&,
                         const BrandLifecycleObservationDTO&) = default;
};

struct ProviderDomainActiveConfigurationDTO final {
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeUnitId source_runtime_unit_id;
  RuntimeEpoch source_runtime_epoch;
  std::uint64_t source_sequence;
  ContentDigest signal_configuration_digest;
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const ProviderDomainActiveConfigurationDTO&,
                         const ProviderDomainActiveConfigurationDTO&) = default;
};

struct LifecycleDomainConfigurationIdentityDTO final {
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  friend bool operator==(const LifecycleDomainConfigurationIdentityDTO&,
                         const LifecycleDomainConfigurationIdentityDTO&) = default;
};

struct ResolvedControllerDomainScopeDTO final {
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  std::optional<ActiveControlConfigurationDTO> active_control_configuration;
  std::vector<ExpectedControlBindingRevisionDTO> current_control_bindings;
  std::vector<ResourceId> current_resources;
  friend bool operator==(const ResolvedControllerDomainScopeDTO&,
                         const ResolvedControllerDomainScopeDTO&) = default;
};

struct ResolvedProviderDomainScopeDTO final {
  ProviderControlDomainDTO provider;
  RuntimeEpoch provider_runtime_epoch;
  std::optional<ProviderDomainActiveConfigurationDTO> active_control_configuration;
  /// Genuine non-coordinate output channels; input-only providers have none.
  std::vector<ResourceId> output_resources{};
  friend bool operator==(const ResolvedProviderDomainScopeDTO&,
                         const ResolvedProviderDomainScopeDTO&) = default;
};

struct ResolvedLifecycleControlDomainScopeDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  std::variant<ResolvedControllerDomainScopeDTO, ResolvedProviderDomainScopeDTO> payload;

  ResolvedLifecycleControlDomainScopeDTO(ControlDomainId id, ControlDomainRevision revision,
                                         ControllerTopologyId topology,
                                         TopologyRevision topology_revision,
                                         std::optional<ActiveControlConfigurationDTO> configuration,
                                         std::vector<ExpectedControlBindingRevisionDTO> bindings,
                                         std::vector<ResourceId> resources)
      : control_domain_id(std::move(id)),
        control_domain_revision(std::move(revision)),
        payload(ResolvedControllerDomainScopeDTO{std::move(topology), std::move(topology_revision),
                                                 std::move(configuration), std::move(bindings),
                                                 std::move(resources)}) {}
  ResolvedLifecycleControlDomainScopeDTO(ControlDomainId id, ControlDomainRevision revision,
                                         ResolvedProviderDomainScopeDTO provider)
      : control_domain_id(std::move(id)),
        control_domain_revision(std::move(revision)),
        payload(std::move(provider)) {}

  [[nodiscard]] ResolvedControllerDomainScopeDTO& controller() {
    return std::get<ResolvedControllerDomainScopeDTO>(payload);
  }
  [[nodiscard]] const ResolvedControllerDomainScopeDTO& controller() const {
    return std::get<ResolvedControllerDomainScopeDTO>(payload);
  }
  [[nodiscard]] const ResolvedProviderDomainScopeDTO* provider() const noexcept {
    return std::get_if<ResolvedProviderDomainScopeDTO>(&payload);
  }
  [[nodiscard]] const std::vector<ExpectedControlBindingRevisionDTO>& control_bindings()
      const noexcept {
    static const std::vector<ExpectedControlBindingRevisionDTO> none;
    if (const auto* physical = std::get_if<ResolvedControllerDomainScopeDTO>(&payload))
      return physical->current_control_bindings;
    return none;
  }
  [[nodiscard]] const std::vector<ResourceId>& physical_resources() const noexcept {
    static const std::vector<ResourceId> none;
    if (const auto* physical = std::get_if<ResolvedControllerDomainScopeDTO>(&payload))
      return physical->current_resources;
    return none;
  }
  [[nodiscard]] const std::vector<ResourceId>& resource_ids() const noexcept {
    if (const auto* driver = provider()) return driver->output_resources;
    return controller().current_resources;
  }
  [[nodiscard]] std::optional<LifecycleDomainConfigurationIdentityDTO> configuration_identity()
      const {
    return std::visit(
        [](const auto& scope) -> std::optional<LifecycleDomainConfigurationIdentityDTO> {
          if (!scope.active_control_configuration) return std::nullopt;
          return LifecycleDomainConfigurationIdentityDTO{
              scope.active_control_configuration->active_control_configuration_id,
              scope.active_control_configuration->active_control_configuration_generation};
        },
        payload);
  }

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ResolvedLifecycleControlDomainScopeDTO&,
                         const ResolvedLifecycleControlDomainScopeDTO&) = default;
};

struct ComponentLifecycleCurrentSnapshotDTO final {
  ContentDigest snapshot_digest;
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ComponentLifecycleStateDTO component_lifecycle_state;
  ActiveBundlePublicationReceiptDTO active_publication_receipt;
  std::vector<ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> provider_occurrences;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleCurrentSnapshotDTO&,
                         const ComponentLifecycleCurrentSnapshotDTO&) = default;
};

inline constexpr std::string_view kComponentLifecycleCurrentSnapshotIdentityDomain =
    "component-lifecycle-current-snapshot";
inline constexpr std::string_view kComponentLifecycleCurrentSnapshotApiVersion =
    "manyforge.component-lifecycle-current-snapshot.v1";

/// Computes the canonical identity of every semantic snapshot field except
/// `snapshot_digest`.  The input must otherwise be a complete locally valid
/// snapshot; no default, inferred fact, or caller expectation is introduced.
[[nodiscard]] Result<ContentDigest> ComputeComponentLifecycleCurrentSnapshotDigest(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot);

/// Independently verifies both the closed snapshot shape and its canonical
/// digest.  This is the common readback boundary for production composition.
[[nodiscard]] Result<void> VerifyComponentLifecycleCurrentSnapshot(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot);

}  // namespace manyforge::core
