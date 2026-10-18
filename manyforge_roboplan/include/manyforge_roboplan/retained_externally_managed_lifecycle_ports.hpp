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
  std::optional<core::ErrorDTO> diagnostic{};
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool
  operator==(const RetainedExternallyManagedDomainSnapshot &,
             const RetainedExternallyManagedDomainSnapshot &) = default;
};

using RetainedExternallyManagedDomainSnapshotSource =
    std::function<core::Result<RetainedExternallyManagedDomainSnapshot>(
        const core::ControlDomainId &, core::TimePoint)>;

/// Outcome of one host-owned lifecycle primitive of an externally managed
/// product (for example a ros2_control controller switch requested by CCLC).
struct RetainedExternallyManagedPrimitiveOutcome final {
  core::LifecyclePrimitiveDispatchStateDTO dispatch{
      core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched};
  core::LifecyclePrimitiveEffectDTO effect{core::LifecyclePrimitiveEffectDTO::kNone};
  std::optional<core::LifecyclePrimitiveKnownResultDTO> known{};
  std::optional<core::ErrorDTO> error{};
};

/// Executes one lifecycle primitive of the selected product for one control
/// domain. Its postcondition is then established only by Observe.
using RetainedExternallyManagedPrimitiveDispatcher =
    std::function<RetainedExternallyManagedPrimitiveOutcome(
        const core::ControlDomainId &, const core::LifecyclePrimitiveId &,
        std::stop_token)>;

/// The real driver owner receives the entire immutable CCLC intent so it can
/// check expected_observation_generation under its own mutation lock. This
/// callback does not transfer CCLC transaction/deadline ownership.
using RetainedProviderDomainPrimitiveDispatcher =
    std::function<RetainedExternallyManagedPrimitiveOutcome(
        const core::BrandLifecyclePrimitiveIntentDTO &, std::stop_token)>;

/// Brand lifecycle adapter for an externally managed product.
///
/// Observe normalizes the selected product snapshot into CCLC truth. Without a
/// dispatcher, Dispatch always returns known-no-effect/not-dispatched: only the
/// external owner may change its controller or transport lifecycle. With one,
/// CCLC primitives of the product's catalog reach the host's single owner of
/// that product's mutations (design R6: controllers loaded inactive, CCLC
/// activates them).
class RetainedExternallyManagedBrandLifecyclePort final
    : public core::IBrandLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>
  Create(core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
         RetainedExternallyManagedDomainSnapshotSource snapshot_source,
         core::IClock &clock, core::Duration observation_freshness_ns,
         RetainedExternallyManagedPrimitiveDispatcher dispatcher = {});

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>
  CreateForProviderDomain(
      core::ControllableComponentId component,
      core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
      RetainedExternallyManagedDomainSnapshotSource snapshot_source,
      core::IClock &clock, core::Duration observation_freshness_ns,
      RetainedProviderDomainPrimitiveDispatcher dispatcher);

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
