#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/ports/icontroller_lifecycle_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace manyforge::roboplan_backend {

/// One immutable read of an externally managed controller/provider domain.
///
/// The selected product owns the source and its generation.  CCLC adapters
/// consume this value only as observation: it carries no command callback and
/// cannot be used to activate, deactivate, or otherwise mutate the external
/// controller.  A generation must advance whenever any field changes.
struct RetainedExternallyManagedDomainSnapshot final {
  std::uint64_t source_generation{};
  core::ComponentLifecycleStateDTO component_state{
      core::ComponentLifecycleStateDTO::kUndeclared};
  bool exact_controller_configuration_active{};
  std::vector<core::LifecycleFactDTO> facts;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool
  operator==(const RetainedExternallyManagedDomainSnapshot &,
             const RetainedExternallyManagedDomainSnapshot &) = default;
};

using RetainedExternallyManagedDomainSnapshotSource =
    std::function<core::Result<RetainedExternallyManagedDomainSnapshot>(
        const core::ControlDomainId &, core::TimePoint)>;

/// Observer-only brand lifecycle adapter for an external product owner.
///
/// Observe normalizes the selected product snapshot into CCLC truth. Dispatch
/// always returns known-no-effect/not-dispatched: only the external owner may
/// change its controller or transport lifecycle.
class RetainedExternallyManagedBrandLifecyclePort final
    : public core::IBrandLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>
  Create(core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
         RetainedExternallyManagedDomainSnapshotSource snapshot_source,
         core::IClock &clock, core::Duration observation_freshness_ns);

  [[nodiscard]] core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override;

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation) override;

private:
  struct State;

  explicit RetainedExternallyManagedBrandLifecyclePort(
      std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

/// Observer-only controller lifecycle adapter for an external product owner.
///
/// The exact expected controller/configuration/endpoint projection is still
/// decoded from the retained product.  A source snapshot may prove that exact
/// configuration active or report it absent; it cannot substitute another
/// route. ApplyTransitionStep never calls controller-manager services.
class RetainedExternallyManagedControllerLifecyclePort final
    : public core::IControllerLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedExternallyManagedControllerLifecyclePort>>
  Create(core::ControllerLifecycleExpectedDomain expected_domain,
         std::vector<core::RuntimeParticipantBinding> physical_bindings,
         RetainedExternallyManagedDomainSnapshotSource snapshot_source,
         core::IClock &clock, core::Duration observation_freshness_ns);

  [[nodiscard]] core::Result<core::ControllerLifecycleObservationDTO>
  Observe(const core::ControllerLifecycleObservationQueryDTO &query) override;

  [[nodiscard]] core::ControllerLifecycleTransitionResultDTO
  ApplyTransitionStep(
      const core::ControllerLifecycleTransitionIntentDTO &intent,
      std::stop_token cancellation) override;

private:
  RetainedExternallyManagedControllerLifecyclePort(
      core::ControllerLifecycleExpectedDomain expected_domain,
      RetainedExternallyManagedDomainSnapshotSource snapshot_source,
      core::IClock &clock, core::Duration observation_freshness_ns);

  core::ControllerLifecycleExpectedDomain expected_domain_;
  RetainedExternallyManagedDomainSnapshotSource snapshot_source_;
  core::IClock &clock_;
  core::Duration observation_freshness_ns_;
  std::uint64_t next_source_sequence_{1U};
  std::uint64_t last_external_generation_{};
  std::optional<RetainedExternallyManagedDomainSnapshot>
      last_external_snapshot_;
  std::mutex mutex_;
};

} // namespace manyforge::roboplan_backend
