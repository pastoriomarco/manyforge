#include "manyforge_component_manager/reference_runtime_unit_admission.hpp"

#include "manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp"

#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::component_manager {

std::vector<std::string> ReferenceRuntimeUnitAdmission::launch_arguments(
    const core::RuntimeEpoch &runtime_epoch) const {
  if (runtime_epoch.empty()) {
    throw std::invalid_argument("reference runtime launch epoch is empty");
  }
  return {
      "--runtime-unit-id",        runtime_unit_id_,        "--runtime-epoch",
      runtime_epoch.value(),      "--product-identity",    product_identity_,
      "--configuration-identity", configuration_identity_,
  };
}

std::string ReferenceRuntimeUnitAdmission::expected_status_line(
    const core::RuntimeEpoch &runtime_epoch,
    const std::uint64_t source_report_sequence) const {
  if (runtime_epoch.empty() || source_report_sequence == 0U) {
    throw std::invalid_argument("reference runtime status identity is invalid");
  }
  return "manyforge.reference-runtime-status.v1 " + runtime_unit_id_ + " " +
         runtime_epoch.value() + " " + std::to_string(source_report_sequence) +
         " 1 serving healthy " + product_identity_ + " " +
         configuration_identity_ + " " + runtime_role_id_ +
         " closed_no_active_effect";
}

std::optional<ReferenceRuntimeUnitAdmission> VerifyReferenceRuntimeUnitForHost(
    const deployment::RetainedBundle &bundle,
    const core::RuntimeUnitId &runtime_unit_id,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    std::string_view operating_system, std::string_view shell_contract) {
  const auto product = deployment::verify_reference_runtime_unit_product(
      bundle, runtime_unit_id, artifact_source);
  if (!product.has_value()) {
    return std::nullopt;
  }
  if (operating_system != product->operating_system() ||
      shell_contract != product->shell_contract()) {
    throw ReferenceRuntimeUnitAdmissionError(
        "reference runtime product is incompatible with the selected host");
  }
  if (product->process_adapter_contract_id() !=
          kRetainedPosixProcessAdapterContractId ||
      product->retrieval_contract_id() != "manyforge.retained_raw_artifact" ||
      product->runtime_role_id() != "manager_fixture" ||
      product->status_endpoint_role_id() != "runtime_status") {
    throw ReferenceRuntimeUnitAdmissionError(
        "reference runtime product does not select the I4 adapter contract");
  }

  const std::string runtime_id = product->runtime_unit_id().value();
  const std::string product_identity = product->product_identity().value();
  return ReferenceRuntimeUnitAdmission(
      runtime_id, product->retained_bundle_id(), product->product_id(),
      product->product_contract_id(), product->product_contract_revision(),
      product_identity, product->configuration_identity().value(),
      product->launchable_artifact_ref().content_digest.value(),
      std::string(product->launchable_bytes()), "shutdown\n",
      std::string(product->runtime_role_id()),
      std::string(product->status_endpoint_role_id()),
      std::string(product->status_endpoint_contract_id()),
      std::string(product->status_endpoint_contract_revision()),
      std::string(product->status_transport()),
      std::string(product->operating_system()),
      std::string(product->shell_contract()),
      std::string(product->process_adapter_contract_id()),
      std::string(product->process_adapter_contract_revision()),
      std::string(product->retrieval_contract_id()),
      std::string(product->retrieval_contract_revision()));
}

} // namespace manyforge::component_manager
