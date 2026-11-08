#pragma once

#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <utility>

namespace manyforge::deployment {

class VerifiedControllerLifecycleExpectedDomain;

/// Independently decode one complete controller-lifecycle expectation from
/// the exact retained control-domain product.
///
/// `desired_configuration` is a complete candidate domain configuration, not
/// a caller-owned topology or endpoint declaration. The decoder accepts it
/// only when every binding, route, activation set, controller, interface, and
/// endpoint joins the verified retained product exactly. The source runtime
/// identity comes from the execution host's current runtime snapshot and is
/// not discovered or substituted by this deployment reader.
/// Only a strict selected pneumatic `output_mode:none` row can seal
/// physical-controller-not-applicable. Logical bindings/routes/endpoints and
/// source identities remain exact; commanded and all other domains retain
/// the unchanged required-physical-controller guards.
[[nodiscard]] VerifiedControllerLifecycleExpectedDomain
load_verified_controller_lifecycle_expected_domain(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::DesiredControlConfigurationDTO &desired_configuration,
    core::RuntimeUnitId source_runtime_unit_id,
    core::RuntimeEpoch source_runtime_epoch);

/// Sealed deployment-qualified controller-lifecycle expectation.
///
/// Public consumers may copy and inspect this value, but only the exact
/// retained-product decoder above can construct it. A route owner can
/// therefore keep the authority-bearing boundary sealed while passing the
/// underlying ROS-independent DTO to Core's pure evaluator and adapter port.
class VerifiedControllerLifecycleExpectedDomain final {
public:
  VerifiedControllerLifecycleExpectedDomain(
      const VerifiedControllerLifecycleExpectedDomain &) = default;
  VerifiedControllerLifecycleExpectedDomain &
  operator=(const VerifiedControllerLifecycleExpectedDomain &) = default;
  VerifiedControllerLifecycleExpectedDomain(
      VerifiedControllerLifecycleExpectedDomain &&) noexcept = default;
  VerifiedControllerLifecycleExpectedDomain &
  operator=(VerifiedControllerLifecycleExpectedDomain &&) noexcept = default;
  ~VerifiedControllerLifecycleExpectedDomain() = default;

  [[nodiscard]] const core::ControllerLifecycleExpectedDomainDTO &
  value() const noexcept {
    return value_.value();
  }

  [[nodiscard]] const core::ControllerLifecycleExpectedDomain &
  sealed() const noexcept {
    return value_;
  }

private:
  explicit VerifiedControllerLifecycleExpectedDomain(
      core::ControllerLifecycleExpectedDomain value)
      : value_(std::move(value)) {}

  core::ControllerLifecycleExpectedDomain value_;

  friend VerifiedControllerLifecycleExpectedDomain
  load_verified_controller_lifecycle_expected_domain(
      const RetainedBundle &retained_bundle,
      const IVerifiedRetainedProductArtifactSource &artifact_source,
      const core::DesiredControlConfigurationDTO &desired_configuration,
      core::RuntimeUnitId source_runtime_unit_id,
      core::RuntimeEpoch source_runtime_epoch);
};

} // namespace manyforge::deployment
