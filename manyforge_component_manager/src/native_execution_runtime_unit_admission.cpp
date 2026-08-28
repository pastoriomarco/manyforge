#include "manyforge_component_manager/native_execution_runtime_unit_admission.hpp"

#include <algorithm>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <string>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace native_contract =
    manyforge::deployment::native_execution_runtime_contract;
namespace ros2_control_contract =
    manyforge::deployment::ros2_control_execution_runtime_contract;

constexpr std::string_view kLaunchApiVersion =
    "manyforge.installed_target_launch.v1";
constexpr std::string_view kReadyApiVersion =
    "manyforge.native-execution-ready.v2";
constexpr std::string_view kStatusApiVersion =
    "manyforge.native-execution-status.v2";

[[noreturn]] void Fail(const std::string &detail) {
  throw NativeExecutionRuntimeUnitAdmissionError(detail);
}

[[nodiscard]] const manyforge::deployment::CanonicalValue::Object &
RequireObject(const manyforge::deployment::CanonicalValue &value,
              const std::string_view label) {
  const auto *object =
      std::get_if<manyforge::deployment::CanonicalValue::Object>(
          &value.storage());
  if (object == nullptr) {
    Fail(std::string(label) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const std::string &
RequireText(const manyforge::deployment::CanonicalValue::Object &object,
            const std::string_view name) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    Fail("native execution launch descriptor is missing " + std::string(name));
  }
  const auto *text = std::get_if<std::string>(&found->second.storage());
  if (text == nullptr || text->empty()) {
    Fail("native execution launch descriptor field is not text: " +
         std::string(name));
  }
  return *text;
}

struct LaunchDescriptor final {
  std::string implementation_revision;
  std::string installed_target_id;
};

struct RuntimeContractFacts final {
  std::string_view product_contract_id;
  std::string_view implementation_id;
  std::string implementation_revision;
  std::string_view installed_target_id;
  std::string_view candidate_worker_installed_target_id;
  std::string_view runtime_role_id;
  std::string_view status_endpoint_role_id;
  std::string_view status_endpoint_contract_id;
  std::string_view process_adapter_contract_id;
  std::string_view retrieval_contract_id;
  std::string_view runtime_contract_offer_digest;
};

[[nodiscard]] std::string_view OfferDigestForFamily(
    const std::string_view implementation_id) {
  const auto &offers =
      manyforge::deployment::runtime_contract_declaration_registry()
          .package_manifest_set()
          .offers;
  const auto found = std::find_if(
      offers.begin(), offers.end(), [&](const auto &offer) {
        return offer.runtime_implementation_id.value() == implementation_id;
      });
  if (found == offers.end()) {
    Fail("execution runtime has no component-owned contract offer");
  }
  return found->offer_digest.value();
}

[[nodiscard]] RuntimeContractFacts
RequireRuntimeContract(const std::string_view product_contract_id) {
  if (product_contract_id == native_contract::kProductContractId) {
    return {native_contract::kProductContractId,
            native_contract::kImplementationId,
            native_contract::implementation_revision(),
            native_contract::kInstalledTargetId,
            native_contract::kCandidateWorkerInstalledTargetId,
            native_contract::kRuntimeRoleId,
            native_contract::kStatusEndpointRoleId,
            native_contract::kStatusEndpointContractId,
            native_contract::kProcessAdapterContractId,
            native_contract::kRetrievalContractId,
            OfferDigestForFamily(native_contract::kImplementationId)};
  }
  if (product_contract_id == ros2_control_contract::kProductContractId) {
    return {ros2_control_contract::kProductContractId,
            ros2_control_contract::kImplementationId,
            ros2_control_contract::implementation_revision(),
            ros2_control_contract::kInstalledTargetId,
            ros2_control_contract::kCandidateWorkerInstalledTargetId,
            ros2_control_contract::kRuntimeRoleId,
            ros2_control_contract::kStatusEndpointRoleId,
            ros2_control_contract::kStatusEndpointContractId,
            ros2_control_contract::kProcessAdapterContractId,
            ros2_control_contract::kRetrievalContractId,
            OfferDigestForFamily(ros2_control_contract::kImplementationId)};
  }
  Fail("execution admission received an unsupported runtime product");
}

[[nodiscard]] LaunchDescriptor
ParseLaunchDescriptor(const std::string_view bytes,
                      const RuntimeContractFacts &contract) {
  manyforge::deployment::CanonicalValue root;
  try {
    root = manyforge::deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    Fail("execution launch descriptor is invalid: " +
         std::string(error.what()));
  }
  if (manyforge::deployment::canonical_json(root) != bytes) {
    Fail("execution launch descriptor is not canonical JSON");
  }
  const auto &object =
      RequireObject(root, "native execution launch descriptor");
  if (object.size() != 4U ||
      RequireText(object, "api_version") != kLaunchApiVersion ||
      RequireText(object, "implementation_id") != contract.implementation_id ||
      RequireText(object, "target_id") != contract.installed_target_id) {
    Fail("execution launch descriptor differs from its closed shape");
  }
  const auto implementation_revision =
      RequireText(object, "implementation_revision");
  if (implementation_revision != contract.implementation_revision) {
    Fail("execution launch descriptor selects another implementation "
         "revision");
  }
  return {implementation_revision, RequireText(object, "target_id")};
}

} // namespace

std::vector<std::string> NativeExecutionRuntimeUnitAdmission::launch_arguments(
    const int inherited_channel_fd,
    const manyforge::core::RuntimeEpoch &runtime_epoch) const {
  if (inherited_channel_fd < 0 || runtime_epoch.empty()) {
    throw std::invalid_argument(
        "native execution launch channel or runtime epoch is invalid");
  }
  return {
      "--manyforge-managed-runtime-channel-fd",
      std::to_string(inherited_channel_fd),
      "--runtime-unit-id",
      product_.runtime_unit_id().value(),
      "--runtime-epoch",
      runtime_epoch.value(),
      "--product-identity",
      product_.product_identity().value(),
      "--configuration-identity",
      product_.configuration_identity().value(),
  };
}

std::string NativeExecutionRuntimeUnitAdmission::expected_ready_packet(
    const manyforge::core::RuntimeEpoch &runtime_epoch) const {
  if (runtime_epoch.empty()) {
    throw std::invalid_argument("native execution ready epoch is empty");
  }
  return std::string(kReadyApiVersion) + " " +
         product_.runtime_unit_id().value() + " " + runtime_epoch.value() +
         " " + runtime_contract_offer_digest_ + " " +
         product_.product_identity().value() + " " +
         product_.configuration_identity().value();
}

std::string NativeExecutionRuntimeUnitAdmission::expected_status_packet(
    const manyforge::core::RuntimeEpoch &runtime_epoch,
    const std::uint64_t source_report_sequence) const {
  if (runtime_epoch.empty() || source_report_sequence == 0U) {
    throw std::invalid_argument("native execution status identity is invalid");
  }
  return std::string(kStatusApiVersion) + " " +
         product_.runtime_unit_id().value() + " " + runtime_epoch.value() +
         " " + std::to_string(source_report_sequence) + " 1 serving healthy " +
         runtime_contract_offer_digest_ + " " +
         product_.product_identity().value() +
         " " + product_.configuration_identity().value() + " " +
         std::string(product_.runtime_role_id()) +
         " closed_no_active_effect";
}

std::optional<NativeExecutionRuntimeUnitAdmission>
VerifyNativeExecutionRuntimeUnitForHost(
    const manyforge::deployment::RetainedBundle &bundle,
    const manyforge::core::RuntimeUnitId &runtime_unit_id,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    const std::string_view operating_system,
    const std::string_view launch_environment_contract) {
  const std::vector<std::string> launch_environment_contracts{
      std::string(launch_environment_contract)};
  return VerifyNativeExecutionRuntimeUnitForHost(
      bundle, runtime_unit_id, artifact_source, operating_system,
      launch_environment_contracts);
}

std::optional<NativeExecutionRuntimeUnitAdmission>
VerifyNativeExecutionRuntimeUnitForHost(
    const manyforge::deployment::RetainedBundle &bundle,
    const manyforge::core::RuntimeUnitId &runtime_unit_id,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    const std::string_view operating_system,
    const std::span<const std::string> launch_environment_contracts) {
  auto product = manyforge::deployment::verify_runtime_unit_product(
      bundle, runtime_unit_id, artifact_source);
  if (!product.has_value()) {
    return std::nullopt;
  }
  const RuntimeContractFacts contract =
      RequireRuntimeContract(product->product_contract_id().value());
  const auto runtime_unit = std::find_if(
      bundle.value().cell_graph.runtime_units.begin(),
      bundle.value().cell_graph.runtime_units.end(),
      [&](const auto &candidate) {
        return candidate.runtime_unit_id == runtime_unit_id;
      });
  const auto requirement = std::find_if(
      bundle.value().runtime_contract_requirements.begin(),
      bundle.value().runtime_contract_requirements.end(),
      [&](const auto &candidate) {
        return candidate.runtime_unit_id == runtime_unit_id;
      });
  const auto *qualified =
      runtime_unit == bundle.value().cell_graph.runtime_units.end()
          ? nullptr
          : manyforge::deployment::runtime_contract_declaration_registry()
                .find(runtime_unit->runtime_profile_id,
                      runtime_unit->runtime_profile_revision,
                      product->product_contract_id(),
                      product->product_contract_revision());
  if (qualified == nullptr ||
      requirement == bundle.value().runtime_contract_requirements.end() ||
      qualified->offer().offer_digest.value() !=
          contract.runtime_contract_offer_digest ||
      !manyforge::core::CheckRuntimeContractCompatibility(
           qualified->offer(), requirement->requirement)
           .ok()) {
    Fail("execution runtime contract admission is not qualified");
  }
  const bool launch_environment_matches = std::find(
      launch_environment_contracts.begin(), launch_environment_contracts.end(),
      product->launch_environment_contract()) !=
                                          launch_environment_contracts.end();
  if (operating_system != product->operating_system() ||
      !launch_environment_matches) {
    Fail("execution runtime product is incompatible with the selected "
         "host");
  }
  if (product->runtime_role_id() != contract.runtime_role_id ||
      product->status_endpoint_role_id() != contract.status_endpoint_role_id ||
      product->status_endpoint_contract_id() !=
          contract.status_endpoint_contract_id ||
      product->status_transport() != "adapter_private" ||
      product->process_adapter_contract_id() !=
          contract.process_adapter_contract_id ||
      product->retrieval_contract_id() != contract.retrieval_contract_id) {
    Fail("execution runtime product does not select the closed Manager "
         "adapter");
  }
  auto descriptor = ParseLaunchDescriptor(product->launchable_bytes(), contract);
  return NativeExecutionRuntimeUnitAdmission(
      std::move(*product), std::move(descriptor.implementation_revision),
      std::move(descriptor.installed_target_id),
      std::string(contract.candidate_worker_installed_target_id),
      std::string(contract.runtime_contract_offer_digest));
}

} // namespace manyforge::component_manager
