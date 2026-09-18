#include "manyforge_deployment/runtime_policy_codec.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <manyforge_core/types/runtime_policy.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw RuntimePolicyCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& member(const CanonicalValue& object,
                                           std::string_view name) {
  const auto* value = object.find_object_member(name);
  if (value == nullptr) {
    fail("runtime policy normalized value is missing " + std::string(name));
  }
  return *value;
}

[[nodiscard]] const std::string& text(const CanonicalValue& value,
                                      std::string_view name) {
  const auto* result = std::get_if<std::string>(&value.storage());
  if (result == nullptr) {
    fail("runtime policy normalized field is not a string: " +
         std::string(name));
  }
  return *result;
}

[[nodiscard]] const CanonicalValue::Array& array(const CanonicalValue& value,
                                                 std::string_view name) {
  const auto* result =
      std::get_if<CanonicalValue::Array>(&value.storage());
  if (result == nullptr) {
    fail("runtime policy normalized field is not an array: " +
         std::string(name));
  }
  return *result;
}

[[nodiscard]] bool boolean(const CanonicalValue& value,
                           std::string_view name) {
  const auto* result = std::get_if<bool>(&value.storage());
  if (result == nullptr) {
    fail("runtime policy normalized field is not boolean: " +
         std::string(name));
  }
  return *result;
}

[[nodiscard]] core::RuntimeOpenLoopGraspAcknowledgementDTO
decode_open_loop_acknowledgement(const CanonicalValue& value) {
  const std::string& environment =
      text(member(value, "environment"),
           "open_loop_grasp_acknowledgements.environment");
  core::OpenLoopGraspEnvironmentDTO parsed_environment;
  if (environment == "simulation") {
    parsed_environment = core::OpenLoopGraspEnvironmentDTO::kSimulation;
  } else if (environment == "commissioned_hardware") {
    parsed_environment =
        core::OpenLoopGraspEnvironmentDTO::kCommissionedHardware;
  } else {
    fail("runtime policy has an unknown open-loop grasp environment");
  }
  return core::RuntimeOpenLoopGraspAcknowledgementDTO{
      core::ControllableComponentId(
          text(member(value, "controllable_component_id"),
               "open_loop_grasp_acknowledgements.controllable_component_id")),
      core::CommandTargetId(
          text(member(value, "command_target_id"),
               "open_loop_grasp_acknowledgements.command_target_id")),
      parsed_environment,
      boolean(member(value, "allowed"),
              "open_loop_grasp_acknowledgements.allowed")};
}

[[nodiscard]] core::RuntimePolicyDTO decode_normalized(
    const CanonicalValue& normalized) {
  std::optional<std::string> description;
  if (const auto* value = normalized.find_object_member("description");
      value != nullptr) {
    description = text(*value, "description");
  }

  std::vector<core::InterruptPolicyId> interrupt_policy_ids;
  for (const auto& value :
       array(member(normalized, "interrupt_policy_ids"),
             "interrupt_policy_ids")) {
    interrupt_policy_ids.emplace_back(text(value, "interrupt_policy_ids[]"));
  }

  std::vector<core::RuntimeOpenLoopGraspAcknowledgementDTO>
      open_loop_acknowledgements;
  const auto& acknowledgement_values = array(
      member(normalized, "open_loop_grasp_acknowledgements"),
      "open_loop_grasp_acknowledgements");
  open_loop_acknowledgements.reserve(acknowledgement_values.size());
  for (const auto& value : acknowledgement_values) {
    open_loop_acknowledgements.push_back(
        decode_open_loop_acknowledgement(value));
  }

  return core::RuntimePolicyDTO{
      core::RuntimePolicyId(
          text(member(normalized, "runtime_policy_id"), "runtime_policy_id")),
      core::RuntimePolicyRevision(
          text(member(normalized, "runtime_policy_revision"),
               "runtime_policy_revision")),
      std::move(description),
      std::move(interrupt_policy_ids),
      std::move(open_loop_acknowledgements)};
}

}  // namespace

QualifiedRuntimePolicy resolve_and_qualify_runtime_policy(
    const core::RuntimePolicyCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kRuntimePolicyApiVersion, "runtime policy");
        } catch (const std::exception& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("runtime_policy"),
      std::string(core::kRuntimePolicyApiVersion),
      "runtime_policy_revision"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("runtime_policy_revision does not match normalized semantic content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  if (decoded.runtime_policy_id != reference.record_id ||
      decoded.runtime_policy_revision != reference.record_revision) {
    fail("runtime-policy typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedRuntimePolicy(reference, std::move(decoded),
                                normalized.normalized_root());
}

}  // namespace manyforge::deployment
