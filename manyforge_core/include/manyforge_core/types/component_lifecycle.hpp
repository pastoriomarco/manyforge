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
inline constexpr std::size_t kMaxLifecycleExpectedBindings = 4096U;
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

struct ResolvedProviderDomainScopeDTO final {
  ProviderControlDomainDTO provider;
  RuntimeEpoch provider_runtime_epoch;
  /// Genuine non-coordinate output channels; input-only providers have none.
  std::vector<ResourceId> output_resources{};
  friend bool operator==(const ResolvedProviderDomainScopeDTO&,
                         const ResolvedProviderDomainScopeDTO&) = default;
};

struct ResolvedLifecycleControlDomainScopeDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ResolvedProviderDomainScopeDTO payload;

  ResolvedLifecycleControlDomainScopeDTO(ControlDomainId id, ControlDomainRevision revision,
                                         ResolvedProviderDomainScopeDTO provider)
      : control_domain_id(std::move(id)),
        control_domain_revision(std::move(revision)),
        payload(std::move(provider)) {}

  [[nodiscard]] const ResolvedProviderDomainScopeDTO* provider() const noexcept {
    return &payload;
  }
  [[nodiscard]] const std::vector<ResourceId>& resource_ids() const noexcept {
    return payload.output_resources;
  }

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ResolvedLifecycleControlDomainScopeDTO&,
                         const ResolvedLifecycleControlDomainScopeDTO&) = default;
};

}  // namespace manyforge::core
