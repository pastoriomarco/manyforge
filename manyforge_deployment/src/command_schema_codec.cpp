#include "manyforge_deployment/command_schema_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw CommandSchemaCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue &require_member(const CanonicalValue &object,
                                                   std::string_view name) {
  const auto *member = object.find_object_member(name);
  if (member == nullptr) {
    fail("command schema normalized value is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view name) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("command schema normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view name) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("command schema normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

[[nodiscard]] std::uint64_t
require_positive_integer(const CanonicalValue &value, std::string_view name) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) ||
      std::floor(*number) != *number || *number < 1.0 || *number > 4096.0) {
    fail("command schema normalized cardinality is outside [1, 4096]: " +
         std::string(name));
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] core::CoordinateKind
parse_coordinate_kind(std::string_view token) {
  if (token == "angular") {
    return core::CoordinateKind::kAngular;
  }
  if (token == "linear") {
    return core::CoordinateKind::kLinear;
  }
  fail("command schema coordinate kind is outside the closed enum");
}

[[nodiscard]] core::CommandCallerOptionValueKind
parse_option_kind(std::string_view token) {
  if (token == "nonnegative_duration_seconds") {
    return core::CommandCallerOptionValueKind::kNonnegativeDurationSeconds;
  }
  if (token == "bool") {
    return core::CommandCallerOptionValueKind::kBool;
  }
  fail("command schema caller option kind is outside the closed enum");
}

[[nodiscard]] core::CommandSchemaDTO
decode_normalized(const CanonicalValue &normalized) {
  std::vector<core::CommandSchemaCompatibleCapabilityDTO> capabilities;
  for (const auto &item :
       require_array(require_member(normalized, "compatible_capabilities"),
                     "compatible_capabilities")) {
    capabilities.push_back(core::CommandSchemaCompatibleCapabilityDTO{
        core::CapabilityId(require_string(require_member(item, "capability_id"),
                                          "capability_id")),
        core::CapabilityDefinitionRevision(require_string(
            require_member(item, "capability_definition_revision"),
            "capability_definition_revision")),
    });
  }

  const auto &api_version =
      require_string(require_member(normalized, "api_version"), "api_version");
  if (api_version != core::kCommandSchemaApiVersion) {
    fail("command schema API version is outside the closed family");
  }
  const auto &command_value = require_member(normalized, "command_value");
  if (require_string(require_member(command_value, "value_kind"),
                     "value_kind") != "gripper_position") {
    fail("command schema value kind is outside the qualified I6 family");
  }
  std::vector<core::CoordinateKind> position_kinds;
  for (const auto &item :
       require_array(require_member(command_value, "accepted_position_kinds"),
                     "accepted_position_kinds")) {
    position_kinds.push_back(
        parse_coordinate_kind(require_string(item, "accepted_position_kind")));
  }
  const std::uint64_t minimum = require_positive_integer(
      require_member(command_value, "minimum_resource_count"),
      "minimum_resource_count");
  const std::uint64_t maximum = require_positive_integer(
      require_member(command_value, "maximum_resource_count"),
      "maximum_resource_count");
  if (minimum > maximum) {
    fail("command schema minimum resource count exceeds its maximum");
  }
  if (require_string(require_member(command_value, "max_effort_contract"),
                     "max_effort_contract") !=
      "optional_nonnegative_matching_coordinate") {
    fail("command schema max-effort contract is outside the closed enum");
  }

  std::vector<core::ResourceRoleId> resource_selectors;
  for (const auto &item :
       require_array(require_member(normalized, "resource_role_selectors"),
                     "resource_role_selectors")) {
    resource_selectors.emplace_back(
        require_string(item, "resource_role_selector"));
  }
  std::vector<core::ChannelRoleId> channel_selectors;
  for (const auto &item :
       require_array(require_member(normalized, "channel_role_selectors"),
                     "channel_role_selectors")) {
    channel_selectors.emplace_back(
        require_string(item, "channel_role_selector"));
  }
  std::vector<core::CommandCallerOptionDTO> options;
  for (const auto &item : require_array(
           require_member(normalized, "caller_options"), "caller_options")) {
    const auto *required =
        std::get_if<bool>(&require_member(item, "required").storage());
    if (required == nullptr) {
      fail("command schema caller option required field is not boolean");
    }
    options.push_back(core::CommandCallerOptionDTO{
        core::CommandOptionId(
            require_string(require_member(item, "option_id"), "option_id")),
        parse_option_kind(
            require_string(require_member(item, "value_kind"), "value_kind")),
        *required,
    });
  }

  std::vector<core::CommandStopEvidenceRequirement> stop_requirements;
  for (const auto &item :
       require_array(require_member(normalized, "stop_evidence_requirements"),
                     "stop_evidence_requirements")) {
    if (require_string(item, "stop_evidence_requirement") !=
        "command_terminal") {
      fail("command schema Stop requirement is outside the closed enum");
    }
    stop_requirements.push_back(
        core::CommandStopEvidenceRequirement::kCommandTerminal);
  }

  if (require_string(require_member(normalized, "completion_rule"),
                     "completion_rule") != "goal_fenced_target_reached" ||
      require_string(require_member(normalized, "confirmation_rule"),
                     "confirmation_rule") != "binding_feedback_required") {
    fail("command schema settlement rule is outside the closed enum");
  }

  return core::CommandSchemaDTO{
      core::CommandSchemaId(
          require_string(require_member(normalized, "command_schema_id"),
                         "command_schema_id")),
      core::CommandSchemaRevision(
          require_string(require_member(normalized, "command_schema_revision"),
                         "command_schema_revision")),
      std::move(capabilities),
      core::GripperPositionCommandValueContractDTO{
          std::move(position_kinds), minimum, maximum,
          core::CommandEffortLimitContract::
              kOptionalNonnegativeMatchingCoordinate},
      std::move(resource_selectors),
      std::move(channel_selectors),
      std::move(options),
      core::CommandCompletionRule::kGoalFencedTargetReached,
      core::CommandConfirmationRule::kBindingFeedbackRequired,
      std::move(stop_requirements),
  };
}

} // namespace

QualifiedCommandSchema resolve_and_qualify_command_schema(
    const core::CommandSchemaCatalogRecordRefDTO &reference,
    const core::ContentAddressedStoreKey &resolved_store_key,
    std::string_view stored_bytes) {
  if (stored_bytes.size() > kMaxArtifactEncodedBytes ||
      reference.content_addressed_store_key.empty() ||
      resolved_store_key.empty()) {
    fail("command-schema bytes or store key violate fixed bounds");
  }
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *family = registry.find_qualified_catalog_schema(
      core::CatalogRecordKind::kCommandSchema, reference.schema_id,
      reference.schema_revision);
  if (family == nullptr || family->compiled_schema() == nullptr) {
    fail("command-schema catalog reference selects an unqualified schema");
  }
  if (reference.schema_id.value() != core::kCommandSchemaApiVersion) {
    fail("command-schema catalog reference selects the wrong schema family");
  }
  if (reference.content_addressed_store_key != resolved_store_key) {
    fail("command-schema bytes were resolved under a different store key");
  }
  if (reference.content_digest.value() != sha256_digest(stored_bytes)) {
    fail("command-schema exact stored-byte digest does not match");
  }

  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      *family->compiled_schema(), parse_strict_json_model_yaml(stored_bytes));
  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("command_schema"),
      require_string(
          require_member(normalized.normalized_root(), "api_version"),
          "api_version"),
      "command_schema_revision",
  };
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("command_schema_revision does not match normalized semantic content");
  }
  auto decoded = decode_normalized(normalized.normalized_root());
  if (decoded.command_schema_id != reference.record_id ||
      decoded.command_schema_revision != reference.record_revision) {
    fail("command-schema typed ID/revision selector does not match referent");
  }
  if (decoded != core::builtin_gripper_position_command_schema()) {
    fail("command schema does not match the build-owned qualified I6 family");
  }
  return QualifiedCommandSchema(reference, std::move(decoded),
                                normalized.normalized_root());
}

} // namespace manyforge::deployment
