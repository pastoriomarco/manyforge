#pragma once

#include "manyforge_component_manager/runtime_observation.hpp"

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::component_manager {

class ExecutionHostUnitAdmissionError final
    : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Effect-free admission for one exact retained execution product.
///
/// The value exposes only the build-owned installed target identity and fixed
/// child arguments. It never exposes a caller-selected executable, argument
/// vector, environment, transport address, process handle, or readiness
/// claim.
class ExecutionHostUnitAdmission final {
public:
  ExecutionHostUnitAdmission(
      const ExecutionHostUnitAdmission &) = default;
  ExecutionHostUnitAdmission &
  operator=(const ExecutionHostUnitAdmission &) = default;
  ExecutionHostUnitAdmission(
      ExecutionHostUnitAdmission &&) noexcept = default;
  ExecutionHostUnitAdmission &
  operator=(ExecutionHostUnitAdmission &&) noexcept = default;
  ~ExecutionHostUnitAdmission() = default;

  [[nodiscard]] const manyforge::deployment::
      VerifiedRetainedRuntimeUnitProduct &
      product() const noexcept {
    return product_;
  }
  [[nodiscard]] const std::string &implementation_revision() const noexcept {
    return implementation_revision_;
  }
  [[nodiscard]] const std::string&
  runtime_contract_offer_digest() const noexcept {
    return runtime_contract_offer_digest_;
  }
  [[nodiscard]] const std::string &installed_target_id() const noexcept {
    return installed_target_id_;
  }
  [[nodiscard]] const std::string &
  candidate_worker_installed_target_id() const noexcept {
    return candidate_worker_installed_target_id_;
  }

  [[nodiscard]] std::vector<std::string>
  launch_arguments(int inherited_channel_fd,
                   const manyforge::core::RuntimeEpoch &runtime_epoch) const;
  [[nodiscard]] std::string expected_ready_packet(
      const manyforge::core::RuntimeEpoch &runtime_epoch) const;
  [[nodiscard]] std::string
  expected_status_packet(const manyforge::core::RuntimeEpoch &runtime_epoch,
                         std::uint64_t source_report_sequence) const;

  /// Validates the exact existing status identity plus an optional ordered
  /// canonical uint64 host-notice counter pair. Counters confer no readiness.
  [[nodiscard]] RuntimeUnitSourceReport
  parse_status_packet(const manyforge::core::RuntimeEpoch &runtime_epoch,
                      std::string_view packet) const;

private:
  ExecutionHostUnitAdmission(
      manyforge::deployment::VerifiedRetainedRuntimeUnitProduct product,
      std::string implementation_revision, std::string installed_target_id,
      std::string candidate_worker_installed_target_id,
      std::string runtime_contract_offer_digest)
      : product_(std::move(product)),
        implementation_revision_(std::move(implementation_revision)),
        installed_target_id_(std::move(installed_target_id)),
        candidate_worker_installed_target_id_(
            std::move(candidate_worker_installed_target_id)),
        runtime_contract_offer_digest_(
            std::move(runtime_contract_offer_digest)) {}

  manyforge::deployment::VerifiedRetainedRuntimeUnitProduct product_;
  std::string implementation_revision_;
  std::string installed_target_id_;
  std::string candidate_worker_installed_target_id_;
  std::string runtime_contract_offer_digest_;

  friend std::optional<ExecutionHostUnitAdmission>
  VerifyExecutionHostUnitForHost(
      const manyforge::deployment::RetainedBundle &,
      const manyforge::core::RuntimeUnitId &,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource &,
      std::string_view, std::string_view);
  friend std::optional<ExecutionHostUnitAdmission>
  VerifyExecutionHostUnitForHost(
      const manyforge::deployment::RetainedBundle &,
      const manyforge::core::RuntimeUnitId &,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource &,
      std::string_view, std::span<const std::string>);
};

/// Admit one exact execution host product for the selected host platform.
///
/// Missing runtime-unit coordinates return no value. A present product whose
/// sealed descriptor or host contract differs rejects before a process,
/// filesystem, scene, ROS, or robot effect.
[[nodiscard]] std::optional<ExecutionHostUnitAdmission>
VerifyExecutionHostUnitForHost(
    const manyforge::deployment::RetainedBundle &bundle,
    const manyforge::core::RuntimeUnitId &runtime_unit_id,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string_view operating_system,
    std::string_view launch_environment_contract);

[[nodiscard]] std::optional<ExecutionHostUnitAdmission>
VerifyExecutionHostUnitForHost(
    const manyforge::deployment::RetainedBundle &bundle,
    const manyforge::core::RuntimeUnitId &runtime_unit_id,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string_view operating_system,
    std::span<const std::string> launch_environment_contracts);

} // namespace manyforge::component_manager
