#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/component_lifecycle_admission_query.hpp>
#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/ports/icontroller_lifecycle_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_roboplan/vendor_device_adapter.hpp>
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

/// Outcome of one host-owned lifecycle substep of an externally managed
/// product (for example a ros2_control controller switch run by the port's
/// transition): the handle's own outcome. The port maps it to the substep's
/// succeeded / failed / connection lost (`TransitionOutcome`, V-B2 §2.2);
/// its finer distinctions stay in the handle and its error's details.
using RetainedExternallyManagedPrimitiveOutcome = RetainedRos2ControlSwitchResult;

/// V-B2 §2.2: a substep succeeded, lost the connection (the controller
/// manager or the robot controller did not answer, D8, whatever the handle
/// outcome), or failed with the handle's error unchanged.
[[nodiscard]] inline core::LifecycleTransitionOutcomeDTO
TransitionOutcome(const RetainedExternallyManagedPrimitiveOutcome &outcome) noexcept {
  if (outcome.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded)
    return core::LifecycleTransitionOutcomeDTO::kSucceeded;
  if (outcome.error && (outcome.error->code == "controller_manager_unresponsive" ||
                        outcome.error->code == "robot_controller_unresponsive"))
    return core::LifecycleTransitionOutcomeDTO::kConnectionLost;
  return core::LifecycleTransitionOutcomeDTO::kFailed;
}

/// Executes one lifecycle substep of the selected product for one control
/// domain. Its postcondition is then established only by Observe. The
/// duration is the transition's remaining command time at the substep; a
/// blocking call may use it as its bound. A substep that runs nested
/// substeps of its own (V-B2 §2.4: Enable's `device.tool_context.sync`)
/// reports their events to the sink.
using RetainedExternallyManagedPrimitiveDispatcher =
    std::function<RetainedExternallyManagedPrimitiveOutcome(
        const core::ControlDomainId &, const core::LifecyclePrimitiveId &,
        std::stop_token, std::optional<core::Duration>,
        const core::LifecycleSubstepFeedbackSink &)>;

/// The real driver owner receives a per-substep copy of the immutable CCLC
/// intent (the substep id, the observation generation its selection used and
/// the remaining time) so it can check expected_observation_generation under
/// its own mutation lock. This callback does not transfer CCLC
/// transaction/deadline ownership.
using RetainedProviderDomainPrimitiveDispatcher =
    std::function<RetainedExternallyManagedPrimitiveOutcome(
        const core::BrandLifecyclePrimitiveIntentDTO &, std::stop_token)>;

/// Brand lifecycle adapter for an externally managed product.
///
/// Observe normalizes the selected product snapshot into CCLC truth.
/// Admission is the product catalog's own query (V-B2 §3). Without a
/// dispatcher, Dispatch always fails with nothing sent: only the external
/// owner may change its controller or transport lifecycle. With one, Dispatch
/// runs the whole CCLC transition (V-B2): the shared runner selects each
/// substep of the product's catalog from a fresh observation and it reaches
/// the host's single owner of that product's mutations (design R6:
/// controllers loaded inactive, CCLC activates them).
class RetainedExternallyManagedBrandLifecyclePort final
    : public core::IBrandLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>
  Create(core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
         core::BrandLifecyclePlanCatalogDTO catalog,
         RetainedExternallyManagedDomainSnapshotSource snapshot_source,
         core::IClock &clock, core::Duration observation_freshness_ns,
         core::Duration poll_period_ns,
         RetainedExternallyManagedPrimitiveDispatcher dispatcher = {});

  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort>>
  CreateForProviderDomain(
      core::ControllableComponentId component,
      core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
      core::BrandLifecyclePlanCatalogDTO catalog,
      RetainedExternallyManagedDomainSnapshotSource snapshot_source,
      core::IClock &clock, core::Duration observation_freshness_ns,
      core::Duration poll_period_ns,
      RetainedProviderDomainPrimitiveDispatcher dispatcher);

  [[nodiscard]] core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override;

  [[nodiscard]] const core::ILifecycleAdmission &Admission() const override;

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           const core::LifecycleSubstepFeedbackSink &feedback,
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
