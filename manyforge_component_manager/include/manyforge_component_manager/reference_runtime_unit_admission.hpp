#pragma once

#include <cstdint>
#include <manyforge_core/types/retained_product.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment {
class IVerifiedRetainedProductArtifactSource;
class RetainedBundle;
} // namespace manyforge::deployment

namespace manyforge::component_manager {

class ReferenceRuntimeUnitAdmissionError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Effect-free launch admission for the one I3 reference runtime product.
///
/// The value contains only contract-fixed process inputs derived from an
/// independently verified retained product. It has no executable path,
/// environment map, process handle, start/stop operation, or runtime status
/// claim. I4 later owns trusted artifact materialization and process effects.
class ReferenceRuntimeUnitAdmission final {
public:
  ReferenceRuntimeUnitAdmission(const ReferenceRuntimeUnitAdmission &) =
      default;
  ReferenceRuntimeUnitAdmission &
  operator=(const ReferenceRuntimeUnitAdmission &) = default;
  ReferenceRuntimeUnitAdmission(ReferenceRuntimeUnitAdmission &&) noexcept =
      default;
  ReferenceRuntimeUnitAdmission &
  operator=(ReferenceRuntimeUnitAdmission &&) noexcept = default;
  ~ReferenceRuntimeUnitAdmission() = default;

  [[nodiscard]] const std::string &runtime_unit_id() const noexcept {
    return runtime_unit_id_;
  }
  [[nodiscard]] const core::RetainedBundleId &
  retained_bundle_id() const noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::ProductId &product_id() const noexcept {
    return product_id_;
  }
  [[nodiscard]] const core::ProductContractId &
  product_contract_id() const noexcept {
    return product_contract_id_;
  }
  [[nodiscard]] const core::ProductContractRevision &
  product_contract_revision() const noexcept {
    return product_contract_revision_;
  }
  [[nodiscard]] const std::string &product_identity() const noexcept {
    return product_identity_;
  }
  [[nodiscard]] const std::string &configuration_identity() const noexcept {
    return configuration_identity_;
  }
  [[nodiscard]] const std::string &launchable_content_digest() const noexcept {
    return launchable_content_digest_;
  }
  [[nodiscard]] std::string_view launchable_bytes() const noexcept {
    return launchable_bytes_;
  }
  [[nodiscard]] std::vector<std::string>
  launch_arguments(const core::RuntimeEpoch &runtime_epoch) const;
  [[nodiscard]] std::string
  expected_status_line(const core::RuntimeEpoch &runtime_epoch,
                       std::uint64_t source_report_sequence) const;
  [[nodiscard]] std::string_view status_command() const noexcept {
    return "status\n";
  }
  [[nodiscard]] std::string_view shutdown_command() const noexcept {
    return shutdown_command_;
  }
  [[nodiscard]] const std::string &runtime_role_id() const noexcept {
    return runtime_role_id_;
  }
  [[nodiscard]] const std::string &status_endpoint_role_id() const noexcept {
    return status_endpoint_role_id_;
  }
  [[nodiscard]] const std::string &
  status_endpoint_contract_id() const noexcept {
    return status_endpoint_contract_id_;
  }
  [[nodiscard]] const std::string &
  status_endpoint_contract_revision() const noexcept {
    return status_endpoint_contract_revision_;
  }
  [[nodiscard]] const std::string &status_transport() const noexcept {
    return status_transport_;
  }
  [[nodiscard]] const std::string &operating_system() const noexcept {
    return operating_system_;
  }
  [[nodiscard]] const std::string &shell_contract() const noexcept {
    return shell_contract_;
  }
  [[nodiscard]] const std::string &
  process_adapter_contract_id() const noexcept {
    return process_adapter_contract_id_;
  }
  [[nodiscard]] const std::string &
  process_adapter_contract_revision() const noexcept {
    return process_adapter_contract_revision_;
  }
  [[nodiscard]] const std::string &retrieval_contract_id() const noexcept {
    return retrieval_contract_id_;
  }
  [[nodiscard]] const std::string &
  retrieval_contract_revision() const noexcept {
    return retrieval_contract_revision_;
  }

private:
  ReferenceRuntimeUnitAdmission(
      std::string runtime_unit_id, core::RetainedBundleId retained_bundle_id,
      core::ProductId product_id, core::ProductContractId product_contract_id,
      core::ProductContractRevision product_contract_revision,
      std::string product_identity, std::string configuration_identity,
      std::string launchable_content_digest, std::string launchable_bytes,
      std::string shutdown_command, std::string runtime_role_id,
      std::string status_endpoint_role_id,
      std::string status_endpoint_contract_id,
      std::string status_endpoint_contract_revision,
      std::string status_transport, std::string operating_system,
      std::string shell_contract, std::string process_adapter_contract_id,
      std::string process_adapter_contract_revision,
      std::string retrieval_contract_id,
      std::string retrieval_contract_revision)
      : runtime_unit_id_(std::move(runtime_unit_id)),
        retained_bundle_id_(std::move(retained_bundle_id)),
        product_id_(std::move(product_id)),
        product_contract_id_(std::move(product_contract_id)),
        product_contract_revision_(std::move(product_contract_revision)),
        product_identity_(std::move(product_identity)),
        configuration_identity_(std::move(configuration_identity)),
        launchable_content_digest_(std::move(launchable_content_digest)),
        launchable_bytes_(std::move(launchable_bytes)),
        shutdown_command_(std::move(shutdown_command)),
        runtime_role_id_(std::move(runtime_role_id)),
        status_endpoint_role_id_(std::move(status_endpoint_role_id)),
        status_endpoint_contract_id_(std::move(status_endpoint_contract_id)),
        status_endpoint_contract_revision_(
            std::move(status_endpoint_contract_revision)),
        status_transport_(std::move(status_transport)),
        operating_system_(std::move(operating_system)),
        shell_contract_(std::move(shell_contract)),
        process_adapter_contract_id_(std::move(process_adapter_contract_id)),
        process_adapter_contract_revision_(
            std::move(process_adapter_contract_revision)),
        retrieval_contract_id_(std::move(retrieval_contract_id)),
        retrieval_contract_revision_(std::move(retrieval_contract_revision)) {}

  std::string runtime_unit_id_;
  core::RetainedBundleId retained_bundle_id_;
  core::ProductId product_id_;
  core::ProductContractId product_contract_id_;
  core::ProductContractRevision product_contract_revision_;
  std::string product_identity_;
  std::string configuration_identity_;
  std::string launchable_content_digest_;
  std::string launchable_bytes_;
  std::string shutdown_command_;
  std::string runtime_role_id_;
  std::string status_endpoint_role_id_;
  std::string status_endpoint_contract_id_;
  std::string status_endpoint_contract_revision_;
  std::string status_transport_;
  std::string operating_system_;
  std::string shell_contract_;
  std::string process_adapter_contract_id_;
  std::string process_adapter_contract_revision_;
  std::string retrieval_contract_id_;
  std::string retrieval_contract_revision_;

  friend std::optional<ReferenceRuntimeUnitAdmission>
  VerifyReferenceRuntimeUnitForHost(
      const deployment::RetainedBundle &, const core::RuntimeUnitId &,
      const deployment::IVerifiedRetainedProductArtifactSource &,
      std::string_view, std::string_view);
};

/// Admit the exact I3 reference runtime product for one host platform.
///
/// Missing runtime-unit coordinates return no value. Any present but
/// unverifiable product or incompatible host rejects before a process,
/// filesystem, ROS, or runtime-unit effect.
[[nodiscard]] std::optional<ReferenceRuntimeUnitAdmission>
VerifyReferenceRuntimeUnitForHost(
    const deployment::RetainedBundle &bundle,
    const core::RuntimeUnitId &runtime_unit_id,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    std::string_view operating_system, std::string_view shell_contract);

} // namespace manyforge::component_manager
