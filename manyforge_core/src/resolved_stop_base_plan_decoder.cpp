#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stop_fallback_codec_internal.hpp"

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeUnsignedInteger = 9'007'199'254'740'991ULL;
constexpr std::int64_t kMaximumSafeSignedInteger = 9'007'199'254'740'991LL;
constexpr std::size_t kMaximumParameterDepth = 32U;
constexpr std::size_t kMaximumParameterNodes = 4096U;
constexpr std::size_t kMaximumParameterStringBytes = 65'536U;

class DecodeError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail_decode(std::string reason) {
  throw DecodeError(std::move(reason));
}

[[nodiscard]] bool contains_name(const std::initializer_list<std::string_view> names,
                                 const std::string_view candidate) {
  for (const auto name : names) {
    if (name == candidate) {
      return true;
    }
  }
  return false;
}

void exact_object(const CanonicalValue& value,
                  const std::initializer_list<std::string_view> required,
                  const std::initializer_list<std::string_view> optional,
                  const std::string_view context) {
  if (!value.is_object()) {
    fail_decode(std::string(context) + " is not an object");
  }
  for (const auto name : required) {
    if (value.find_object_member(name) == nullptr) {
      fail_decode(std::string(context) + " is missing " + std::string(name));
    }
  }
  for (const auto& [name, member] : value.as_object()) {
    static_cast<void>(member);
    if (!contains_name(required, name) && !contains_name(optional, name)) {
      fail_decode(std::string(context) + " contains unknown member " + name);
    }
  }
}

[[nodiscard]] const CanonicalValue& member(const CanonicalValue& object,
                                           const std::string_view name,
                                           const std::string_view context) {
  const auto* found = object.find_object_member(name);
  if (found == nullptr) {
    fail_decode(std::string(context) + " is missing " + std::string(name));
  }
  return *found;
}

[[nodiscard]] const CanonicalValue* optional_member(const CanonicalValue& object,
                                                    const std::string_view name) {
  return object.find_object_member(name);
}

[[nodiscard]] const std::string& text(const CanonicalValue& value, const std::string_view context) {
  if (!value.is_string()) {
    fail_decode(std::string(context) + " is not a string");
  }
  return value.as_string();
}

[[nodiscard]] const CanonicalValue::Array& bounded_array(const CanonicalValue& value,
                                                         const std::size_t minimum,
                                                         const std::size_t maximum,
                                                         const std::string_view context) {
  if (!value.is_array() || value.as_array().size() < minimum || value.as_array().size() > maximum) {
    fail_decode(std::string(context) + " is outside its array bound");
  }
  return value.as_array();
}

[[nodiscard]] std::uint64_t unsigned_integer(const CanonicalValue& value,
                                             const std::string_view context) {
  if (!value.is_number()) {
    fail_decode(std::string(context) + " is not a number");
  }
  const double number = value.as_number();
  if (number < 0.0 || number > static_cast<double>(kMaximumSafeUnsignedInteger) ||
      std::trunc(number) != number) {
    fail_decode(std::string(context) + " is not an I-JSON-safe unsigned integer");
  }
  return static_cast<std::uint64_t>(number);
}

[[nodiscard]] std::int64_t signed_integer(const CanonicalValue& value,
                                          const std::string_view context) {
  if (!value.is_number()) {
    fail_decode(std::string(context) + " is not a number");
  }
  const double number = value.as_number();
  if (number < static_cast<double>(-kMaximumSafeSignedInteger) ||
      number > static_cast<double>(kMaximumSafeSignedInteger) || std::trunc(number) != number) {
    fail_decode(std::string(context) + " is not an I-JSON-safe signed integer");
  }
  return static_cast<std::int64_t>(number);
}

template <typename Enum>
[[nodiscard]] Enum enum_value(
    const CanonicalValue& value,
    const std::initializer_list<std::pair<std::string_view, Enum>> alternatives,
    const std::string_view context) {
  const auto& spelling = text(value, context);
  for (const auto& [candidate, result] : alternatives) {
    if (candidate == spelling) {
      return result;
    }
  }
  fail_decode(std::string(context) + " has an unknown enum spelling");
}

[[nodiscard]] ContainmentGranularity containment_granularity(const CanonicalValue& value) {
  return enum_value<ContainmentGranularity>(
      value,
      {{"resource", ContainmentGranularity::kResource},
       {"controller_set", ContainmentGranularity::kControllerSet},
       {"control_domain", ContainmentGranularity::kControlDomain}},
      "containment_granularity");
}

[[nodiscard]] StopContainmentCommandKind command_kind(const CanonicalValue& value) {
  return enum_value<StopContainmentCommandKind>(
      value,
      {{"controller_goal_cancel", StopContainmentCommandKind::kControllerGoalCancel},
       {"motion_controlled_stop", StopContainmentCommandKind::kMotionControlledStop},
       {"capability_command_cancel", StopContainmentCommandKind::kCapabilityCommandCancel},
       {"safe_output_command", StopContainmentCommandKind::kSafeOutputCommand},
       {"logical_cancel", StopContainmentCommandKind::kLogicalCancel},
       {"registered", StopContainmentCommandKind::kRegistered}},
      "command_kind");
}

[[nodiscard]] StopEscalationStageKind stage_kind(const CanonicalValue& value) {
  return enum_value<StopEscalationStageKind>(
      value,
      {{"nominal", StopEscalationStageKind::kNominal},
       {"controlled_fallback", StopEscalationStageKind::kControlledFallback},
       {"software_hard", StopEscalationStageKind::kSoftwareHard},
       {"safety_system_request", StopEscalationStageKind::kSafetySystemRequest}},
      "stage_kind");
}

[[nodiscard]] StopEvidenceRequirementKind evidence_kind(const CanonicalValue& value) {
  return enum_value<StopEvidenceRequirementKind>(
      value,
      {{"controller_goal_terminal", StopEvidenceRequirementKind::kControllerGoalTerminal},
       {"motion_rest", StopEvidenceRequirementKind::kMotionRest},
       {"discrete_acknowledgement", StopEvidenceRequirementKind::kDiscreteAcknowledgement},
       {"io_safe_output", StopEvidenceRequirementKind::kIoSafeOutput},
       {"logical_cancellation_no_continuing_effect",
        StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect},
       {"registered", StopEvidenceRequirementKind::kRegistered}},
      "evidence_kind");
}

[[nodiscard]] StopEvidenceSourceKind evidence_source_kind(const CanonicalValue& value) {
  return enum_value<StopEvidenceSourceKind>(
      value,
      {{"controller_goal_status", StopEvidenceSourceKind::kControllerGoalStatus},
       {"binding_state_authority", StopEvidenceSourceKind::kBindingStateAuthority},
       {"command_target_status", StopEvidenceSourceKind::kCommandTargetStatus},
       {"provider_evidence", StopEvidenceSourceKind::kProviderEvidence}},
      "source_kind");
}

[[nodiscard]] StopEvidenceObservationKind observation_kind(const CanonicalValue& value) {
  return enum_value<StopEvidenceObservationKind>(
      value,
      {{"command_result", StopEvidenceObservationKind::kCommandResult},
       {"post_command_sample", StopEvidenceObservationKind::kPostCommandSample},
       {"post_command_dwell", StopEvidenceObservationKind::kPostCommandDwell}},
      "observation_kind");
}

[[nodiscard]] StateAuthorityClass authority_class(const CanonicalValue& value) {
  return enum_value<StateAuthorityClass>(value,
                                         {{"measured", StateAuthorityClass::kMeasured},
                                          {"commanded", StateAuthorityClass::kCommanded},
                                          {"predicted", StateAuthorityClass::kPredicted}},
                                         "state_authority_class");
}

[[nodiscard]] StopActionClass action_class(const CanonicalValue& value) {
  return enum_value<StopActionClass>(
      value,
      {{"controlled", StopActionClass::kControlled},
       {"software_hard", StopActionClass::kSoftwareHard},
       {"safety_system_request", StopActionClass::kSafetySystemRequest}},
      "stop_action_class");
}

[[nodiscard]] SafetySystemRequestActionClass safety_action_class(const CanonicalValue& value) {
  return enum_value<SafetySystemRequestActionClass>(
      value,
      {{"protective_stop_request", SafetySystemRequestActionClass::kProtectiveStopRequest},
       {"emergency_stop_request", SafetySystemRequestActionClass::kEmergencyStopRequest}},
      "safety_system_request_action_class");
}

[[nodiscard]] MotionControlledStopMode motion_stop_mode(const CanonicalValue& value) {
  return enum_value<MotionControlledStopMode>(
      value,
      {{"soft_on_path", MotionControlledStopMode::kSoftOnPath},
       {"spring_back_hold", MotionControlledStopMode::kSpringBackHold},
       {"spring_back_fast", MotionControlledStopMode::kSpringBackFast},
       {"hard_deceleration", MotionControlledStopMode::kHardDeceleration}},
      "motion controlled Stop mode");
}

[[nodiscard]] ExternalResourceKind external_resource_kind(const CanonicalValue& value) {
  return enum_value<ExternalResourceKind>(
      value,
      {{"model_endpoint", ExternalResourceKind::kModelEndpoint},
       {"content_store", ExternalResourceKind::kContentStore},
       {"data_source", ExternalResourceKind::kDataSource},
       {"service_endpoint", ExternalResourceKind::kServiceEndpoint}},
      "external_resource_kind");
}

[[nodiscard]] ProviderContractRefDTO decode_contract(const CanonicalValue& value) {
  exact_object(value,
               {"provider_contract_role_id", "contract_schema_id", "contract_schema_revision"}, {},
               "provider contract reference");
  return {ProviderContractRoleId(text(member(value, "provider_contract_role_id", "contract"),
                                      "provider_contract_role_id")),
          SchemaId(text(member(value, "contract_schema_id", "contract"), "contract_schema_id")),
          SchemaRevision(text(member(value, "contract_schema_revision", "contract"),
                              "contract_schema_revision"))};
}

[[nodiscard]] RetainedArtifactRefDTO decode_artifact(const CanonicalValue& value) {
  exact_object(value,
               {"artifact_role_id", "media_type", "byte_size", "content_digest",
                "content_addressed_store_key"},
               {}, "retained artifact reference");
  return {ArtifactRoleId(text(member(value, "artifact_role_id", "artifact"), "artifact_role_id")),
          text(member(value, "media_type", "artifact"), "media_type"),
          unsigned_integer(member(value, "byte_size", "artifact"), "byte_size"),
          ContentDigest(text(member(value, "content_digest", "artifact"), "content_digest")),
          ContentAddressedStoreKey(text(member(value, "content_addressed_store_key", "artifact"),
                                        "content_addressed_store_key"))};
}

[[nodiscard]] StopContainmentCommandDTO decode_command(const CanonicalValue& value) {
  exact_object(value, {"command"}, {}, "containment command");
  const auto& command = member(value, "command", "containment command");
  exact_object(command, {"command_kind"}, {"provider_contract_ref", "registered_command_kind_id"},
               "command body");
  std::optional<ProviderContractRefDTO> contract;
  if (const auto* item = optional_member(command, "provider_contract_ref")) {
    contract = decode_contract(*item);
  }
  std::optional<RegisteredStopCommandKindId> registered;
  if (const auto* item = optional_member(command, "registered_command_kind_id")) {
    registered = RegisteredStopCommandKindId(text(*item, "registered_command_kind_id"));
  }
  return {command_kind(member(command, "command_kind", "command body")), std::move(contract),
          std::move(registered)};
}

[[nodiscard]] ResolvedStopRuntimeTargetDTO decode_runtime_target(const CanonicalValue& value) {
  exact_object(value,
               {"provider_instance_id", "provider_manifest_id", "provider_manifest_revision",
                "runtime_unit_id", "product_id", "product_contract_id", "product_contract_revision",
                "content_digest", "provider_runtime_epoch", "runtime_unit_epoch"},
               {"endpoint_role_id", "logical_endpoint_symbol", "external_resource_alias",
                "external_resource_kind", "site_overlay_id", "site_overlay_revision",
                "site_overlay_non_secret_digest", "site_overlay_value_ref"},
               "resolved runtime target");
  std::optional<EndpointRoleId> endpoint_role;
  std::optional<std::string> endpoint_symbol;
  std::optional<ExternalResourceAlias> alias;
  std::optional<ExternalResourceKind> kind;
  std::optional<SiteOverlayId> overlay_id;
  std::optional<SiteOverlayRevision> overlay_revision;
  std::optional<ContentDigest> overlay_digest;
  std::optional<SiteOverlayValueRef> overlay_value;
  if (const auto* item = optional_member(value, "endpoint_role_id")) {
    endpoint_role = EndpointRoleId(text(*item, "endpoint_role_id"));
  }
  if (const auto* item = optional_member(value, "logical_endpoint_symbol")) {
    endpoint_symbol = text(*item, "logical_endpoint_symbol");
  }
  if (const auto* item = optional_member(value, "external_resource_alias")) {
    alias = ExternalResourceAlias(text(*item, "external_resource_alias"));
  }
  if (const auto* item = optional_member(value, "external_resource_kind")) {
    kind = external_resource_kind(*item);
  }
  if (const auto* item = optional_member(value, "site_overlay_id")) {
    overlay_id = SiteOverlayId(text(*item, "site_overlay_id"));
  }
  if (const auto* item = optional_member(value, "site_overlay_revision")) {
    overlay_revision = SiteOverlayRevision(text(*item, "site_overlay_revision"));
  }
  if (const auto* item = optional_member(value, "site_overlay_non_secret_digest")) {
    overlay_digest = ContentDigest(text(*item, "site_overlay_non_secret_digest"));
  }
  if (const auto* item = optional_member(value, "site_overlay_value_ref")) {
    overlay_value = SiteOverlayValueRef(text(*item, "site_overlay_value_ref"));
  }
  return {
      ProviderInstanceId(
          text(member(value, "provider_instance_id", "runtime target"), "provider_instance_id")),
      ProviderManifestId(
          text(member(value, "provider_manifest_id", "runtime target"), "provider_manifest_id")),
      ProviderManifestRevision(text(member(value, "provider_manifest_revision", "runtime target"),
                                    "provider_manifest_revision")),
      RuntimeUnitId(text(member(value, "runtime_unit_id", "runtime target"), "runtime_unit_id")),
      ProductId(text(member(value, "product_id", "runtime target"), "product_id")),
      ProductContractId(
          text(member(value, "product_contract_id", "runtime target"), "product_contract_id")),
      ProductContractRevision(text(member(value, "product_contract_revision", "runtime target"),
                                   "product_contract_revision")),
      ContentDigest(text(member(value, "content_digest", "runtime target"), "content_digest")),
      RuntimeEpoch(text(member(value, "provider_runtime_epoch", "runtime target"),
                        "provider_runtime_epoch")),
      RuntimeEpoch(
          text(member(value, "runtime_unit_epoch", "runtime target"), "runtime_unit_epoch")),
      std::move(endpoint_role),
      std::move(endpoint_symbol),
      std::move(alias),
      std::move(kind),
      std::move(overlay_id),
      std::move(overlay_revision),
      std::move(overlay_digest),
      std::move(overlay_value)};
}

[[nodiscard]] ControlledStopSubstitutionContractDTO decode_substitution(
    const CanonicalValue& value) {
  exact_object(value, {"provider_contract_ref", "requested_stop_mode"},
               {"executed_stop_mode", "fallback_stop_modes"},
               "controlled Stop substitution contract");
  const auto primary = motion_stop_mode(member(value, "requested_stop_mode", "substitution"));
  const auto* legacy = optional_member(value, "executed_stop_mode");
  const auto* modes = optional_member(value, "fallback_stop_modes");
  if ((legacy == nullptr) == (modes == nullptr))
    fail_decode("substitution needs exactly one fallback field");
  std::vector<MotionControlledStopMode> fallbacks;
  if (legacy) {
    if (primary != MotionControlledStopMode::kSoftOnPath ||
        motion_stop_mode(*legacy) != MotionControlledStopMode::kSpringBackHold)
      fail_decode("invalid legacy controlled Stop substitution");
    fallbacks = {MotionControlledStopMode::kSpringBackHold};
  } else {
    if (!modes->is_array() || modes->as_array().empty() || modes->as_array().size() > 2U)
      fail_decode("invalid controlled Stop fallback list");
    for (const auto& mode : modes->as_array()) fallbacks.push_back(motion_stop_mode(mode));
  }
  if (!motion_controlled_stop_fallbacks_valid(primary, fallbacks)) fail_decode("invalid fallback order");
  return {decode_contract(member(value, "provider_contract_ref", "substitution")), primary,
          std::move(fallbacks)};
}

[[nodiscard]] ResolvedStopCommandQualificationDTO decode_qualification(
    const CanonicalValue& value) {
  exact_object(
      value, {"stop_action_class"},
      {"controlled_substitution_contract",
       "safety_system_request_action_class", "commissioned_stop_interface_id",
       "commissioned_stop_interface_revision", "commissioning_evidence_artifact_ref"},
      "resolved command qualification");
  ResolvedStopCommandQualificationDTO result;
  result.stop_action_class = action_class(member(value, "stop_action_class", "qualification"));
  if (const auto* item = optional_member(value, "controlled_substitution_contract")) {
    result.controlled_substitution_contract = decode_substitution(*item);
  }
  if (const auto* item = optional_member(value, "safety_system_request_action_class")) {
    result.safety_system_request_action_class = safety_action_class(*item);
  }
  if (const auto* item = optional_member(value, "commissioned_stop_interface_id")) {
    result.commissioned_stop_interface_id =
        CommissionedStopInterfaceId(text(*item, "commissioned_stop_interface_id"));
  }
  if (const auto* item = optional_member(value, "commissioned_stop_interface_revision")) {
    result.commissioned_stop_interface_revision =
        CommissionedStopInterfaceRevision(text(*item, "commissioned_stop_interface_revision"));
  }
  if (const auto* item = optional_member(value, "commissioning_evidence_artifact_ref")) {
    result.commissioning_evidence_artifact_ref = decode_artifact(*item);
  }
  return result;
}

[[nodiscard]] ResolvedStopContainmentScopeDTO decode_scope(const CanonicalValue& value) {
  const auto granularity =
      containment_granularity(member(value, "containment_granularity", "containment scope"));
  if (granularity == ContainmentGranularity::kResource) {
    exact_object(value, {"activation_set_id", "containment_granularity", "resource_ids"}, {},
                 "resource containment scope");
    const auto& resources_value = member(value, "resource_ids", "resource scope");
    const auto& resource_rows = bounded_array(resources_value, 1U, 4096U, "resource_ids");
    std::vector<ResourceId> resources;
    resources.reserve(resource_rows.size());
    for (const auto& item : resource_rows) {
      resources.emplace_back(text(item, "resource_id"));
    }
    return ResolvedStopResourceContainmentScopeDTO{
        ActivationSetId(
            text(member(value, "activation_set_id", "resource scope"), "activation_set_id")),
        std::move(resources)};
  }
  if (granularity == ContainmentGranularity::kControllerSet) {
    exact_object(value, {"activation_set_id", "containment_granularity", "controllers"}, {},
                 "controller-set containment scope");
    const auto& controllers_value = member(value, "controllers", "controller-set scope");
    const auto& controller_rows = bounded_array(controllers_value, 1U, 256U, "controllers");
    std::vector<ResolvedStopControllerContainmentRowDTO> controllers;
    controllers.reserve(controller_rows.size());
    for (const auto& row : controller_rows) {
      exact_object(row, {"controller_role_id", "logical_controller_symbol"}, {},
                   "controller containment row");
      controllers.push_back(
          {ControllerRoleId(
               text(member(row, "controller_role_id", "controller row"), "controller_role_id")),
           text(member(row, "logical_controller_symbol", "controller row"),
                "logical_controller_symbol")});
    }
    return ResolvedStopControllerSetContainmentScopeDTO{
        ActivationSetId(
            text(member(value, "activation_set_id", "controller-set scope"), "activation_set_id")),
        std::move(controllers)};
  }
  exact_object(value,
               {"activation_set_id", "containment_granularity", "control_domain_id",
                "control_domain_revision"},
               {}, "control-domain containment scope");
  return ResolvedStopControlDomainContainmentScopeDTO{
      ActivationSetId(
          text(member(value, "activation_set_id", "domain scope"), "activation_set_id")),
      ControlDomainId(
          text(member(value, "control_domain_id", "domain scope"), "control_domain_id")),
      ControlDomainRevision(text(member(value, "control_domain_revision", "domain scope"),
                                 "control_domain_revision"))};
}

[[nodiscard]] StopEvidenceSourceRequirementDTO decode_source_requirement(
    const CanonicalValue& value) {
  const auto source = evidence_source_kind(member(value, "source_kind", "source requirement"));
  std::optional<EndpointRoleId> endpoint;
  std::vector<StateAuthorityClass> authorities;
  if (source == StopEvidenceSourceKind::kBindingStateAuthority) {
    exact_object(value,
                 {"source_kind", "freshness_budget_ns", "state_endpoint_role_id",
                  "allowed_authority_classes"},
                 {}, "binding-state source requirement");
    endpoint = EndpointRoleId(text(member(value, "state_endpoint_role_id", "source requirement"),
                                   "state_endpoint_role_id"));
    const auto& rows = member(value, "allowed_authority_classes", "source requirement");
    const auto& authority_rows = bounded_array(rows, 1U, 3U, "allowed_authority_classes");
    authorities.reserve(authority_rows.size());
    for (const auto& row : authority_rows) {
      authorities.push_back(authority_class(row));
    }
  } else if (source == StopEvidenceSourceKind::kProviderEvidence) {
    exact_object(value, {"source_kind", "freshness_budget_ns", "evidence_endpoint_role_id"}, {},
                 "provider-evidence source requirement");
    endpoint = EndpointRoleId(text(member(value, "evidence_endpoint_role_id", "source requirement"),
                                   "evidence_endpoint_role_id"));
  } else {
    exact_object(value, {"source_kind", "freshness_budget_ns"}, {}, "status source requirement");
  }
  return {source,
          signed_integer(member(value, "freshness_budget_ns", "source requirement"),
                         "freshness_budget_ns"),
          std::move(endpoint), std::move(authorities)};
}

[[nodiscard]] StopEvidenceObservationDTO decode_observation(const CanonicalValue& value) {
  exact_object(value, {"observation_kind"}, {"minimum_dwell_ns"}, "evidence observation semantics");
  std::optional<std::int64_t> dwell;
  if (const auto* item = optional_member(value, "minimum_dwell_ns")) {
    dwell = signed_integer(*item, "minimum_dwell_ns");
  }
  return {observation_kind(member(value, "observation_kind", "observation semantics")), dwell};
}

[[nodiscard]] StopEvidenceRequirementDTO decode_requirement(const CanonicalValue& value) {
  exact_object(value,
               {"evidence_item_id", "evidence", "source_requirement",
                "observation_semantics", "resolved_source"},
               {}, "resolved evidence requirement");
  const auto& evidence = member(value, "evidence", "evidence requirement");
  exact_object(evidence, {"evidence_kind"},
               {"provider_contract_ref", "registered_evidence_kind_id"}, "evidence body");
  std::optional<ProviderContractRefDTO> contract;
  std::optional<RegisteredStopEvidenceKindId> registered;
  if (const auto* item = optional_member(evidence, "provider_contract_ref")) {
    contract = decode_contract(*item);
  }
  if (const auto* item = optional_member(evidence, "registered_evidence_kind_id")) {
    registered = RegisteredStopEvidenceKindId(text(*item, "registered_evidence_kind_id"));
  }
  return {StopEvidenceItemId(
              text(member(value, "evidence_item_id", "evidence requirement"), "evidence_item_id")),
          evidence_kind(member(evidence, "evidence_kind", "evidence body")),
          std::move(contract),
          std::move(registered),
          decode_source_requirement(member(value, "source_requirement", "evidence requirement")),
          decode_observation(member(value, "observation_semantics", "evidence requirement"))};
}

[[nodiscard]] ResolvedStopEvidenceSourceDTO decode_resolved_source(const CanonicalValue& value) {
  exact_object(value, {"source_kind", "runtime_target"},
               {"state_authority_id", "state_authority_class", "provider_contract_ref"},
               "resolved evidence source");
  std::optional<StateAuthorityId> authority_id;
  std::optional<StateAuthorityClass> authority;
  std::optional<ProviderContractRefDTO> contract;
  if (const auto* item = optional_member(value, "state_authority_id")) {
    authority_id = StateAuthorityId(text(*item, "state_authority_id"));
  }
  if (const auto* item = optional_member(value, "state_authority_class")) {
    authority = authority_class(*item);
  }
  if (const auto* item = optional_member(value, "provider_contract_ref")) {
    contract = decode_contract(*item);
  }
  return {evidence_source_kind(member(value, "source_kind", "resolved source")),
          decode_runtime_target(member(value, "runtime_target", "resolved source")),
          std::move(authority_id), std::move(authority), std::move(contract)};
}

[[nodiscard]] ResolvedStopEvidenceRequirementDTO decode_resolved_requirement(
    const CanonicalValue& value) {
  return {decode_requirement(value),
          decode_resolved_source(member(value, "resolved_source", "evidence requirement"))};
}

[[nodiscard]] JsonValueDTO decode_parameter_value(const CanonicalValue& value,
                                                  const std::size_t depth, std::size_t& nodes) {
  if (depth > kMaximumParameterDepth || ++nodes > kMaximumParameterNodes) {
    fail_decode("contract parameters exceed their structural bound");
  }
  JsonValueDTO result;
  if (value.is_null()) {
    result.value = nullptr;
  } else if (value.is_bool()) {
    result.value = value.as_bool();
  } else if (value.is_number()) {
    result.value = value.as_number();
  } else if (value.is_string()) {
    if (value.as_string().size() > kMaximumParameterStringBytes) {
      fail_decode("contract parameter string exceeds its bound");
    }
    result.value = value.as_string();
  } else if (value.is_array()) {
    JsonValueDTO::Array rows;
    rows.reserve(value.as_array().size());
    for (const auto& row : value.as_array()) {
      rows.push_back(decode_parameter_value(row, depth + 1U, nodes));
    }
    result.value = std::move(rows);
  } else {
    JsonObjectDTO object;
    for (const auto& [name, child] : value.as_object()) {
      if (name.size() > kMaximumParameterStringBytes) {
        fail_decode("contract parameter member name exceeds its bound");
      }
      object.emplace(name, decode_parameter_value(child, depth + 1U, nodes));
    }
    result.value = std::move(object);
  }
  return result;
}

[[nodiscard]] JsonObjectDTO decode_parameter_object(const CanonicalValue& value) {
  if (!value.is_object()) {
    fail_decode("default_contract_parameters is not an object");
  }
  std::size_t nodes = 0U;
  auto decoded = decode_parameter_value(value, 0U, nodes);
  auto parameters = std::get<JsonObjectDTO>(std::move(decoded.value));
  detail::normalize_stop_fallback_parameters(parameters);
  return parameters;
}

[[nodiscard]] ResolvedStopEscalationStageDTO decode_stage(const CanonicalValue& value) {
  exact_object(value,
               {"stage_index", "stop_escalation_stage_id", "stage_kind", "containment_granularity",
                "containment_command", "command_qualification", "command_target",
                "containment_scope", "evidence_conjunction", "evidence_requirements"},
               {"default_contract_parameters"}, "resolved escalation stage");
  std::optional<JsonObjectDTO> parameters;
  if (const auto* item = optional_member(value, "default_contract_parameters")) {
    parameters = decode_parameter_object(*item);
  }
  const auto& evidence_value = member(value, "evidence_requirements", "stage");
  const auto& evidence_rows = bounded_array(evidence_value, 1U, 64U, "evidence_requirements");
  std::vector<ResolvedStopEvidenceRequirementDTO> evidence;
  evidence.reserve(evidence_rows.size());
  for (const auto& row : evidence_rows) {
    evidence.push_back(decode_resolved_requirement(row));
  }
  return {unsigned_integer(member(value, "stage_index", "stage"), "stage_index"),
          StopEscalationStageId(
              text(member(value, "stop_escalation_stage_id", "stage"), "stop_escalation_stage_id")),
          stage_kind(member(value, "stage_kind", "stage")),
          containment_granularity(member(value, "containment_granularity", "stage")),
          decode_command(member(value, "containment_command", "stage")),
          std::move(parameters),
          decode_qualification(member(value, "command_qualification", "stage")),
          decode_runtime_target(member(value, "command_target", "stage")),
          decode_scope(member(value, "containment_scope", "stage")),
          enum_value<StopEvidenceConjunction>(member(value, "evidence_conjunction", "stage"),
                                              {{"all_of", StopEvidenceConjunction::kAllOf}},
                                              "evidence_conjunction"),
          std::move(evidence)};
}


[[nodiscard]] ResolvedStopTemplateRefDTO decode_template(const CanonicalValue& value) {
  exact_object(value,
               {"product_id", "product_contract_id", "product_contract_revision", "artifact_ref",
                "logical_stop_symbol"},
               {}, "resolved Stop template reference");
  return {ProductId(text(member(value, "product_id", "template"), "product_id")),
          ProductContractId(
              text(member(value, "product_contract_id", "template"), "product_contract_id")),
          ProductContractRevision(text(member(value, "product_contract_revision", "template"),
                                       "product_contract_revision")),
          decode_artifact(member(value, "artifact_ref", "template")),
          text(member(value, "logical_stop_symbol", "template"), "logical_stop_symbol")};
}

[[nodiscard]] ResolvedStopBasePlanDTO decode_plan(const CanonicalValue& value) {
  exact_object(value,
               {"api_version",
                "resolved_stop_base_plan_id",
                "cell_id",
                "retained_bundle_id",
                "graph_revision",
                "source_template",
                "control_domain_id",
                "control_domain_revision",
                "control_binding_id",
                "control_binding_revision",
                "controller_topology_id",
                "topology_revision",
                "control_route_id",
                "activation_set_id",
                "active_control_configuration_id",
                "active_control_configuration_generation",
                "source_runtime_unit_id",
                "source_runtime_epoch",
                "source_sequence",
                "stop_evidence_policy_id",
                "stop_evidence_policy_revision",
                "containment_granularity",
                "escalation_stages"},
               {"gripper_actuation_kind"}, "resolved Stop base plan");
  if (text(member(value, "api_version", "base plan"), "api_version") !=
      kResolvedStopBasePlanApiVersion) {
    fail_decode("resolved Stop base-plan API version is unsupported");
  }
  const auto& stages_value = member(value, "escalation_stages", "base plan");
  const auto& stage_rows = bounded_array(stages_value, 1U, 8U, "escalation_stages");
  std::vector<ResolvedStopEscalationStageDTO> stages;
  stages.reserve(stage_rows.size());
  for (const auto& row : stage_rows) {
    stages.push_back(decode_stage(row));
  }
  std::optional<GripperActuationKind> actuation;
  if (const auto* kind = optional_member(value, "gripper_actuation_kind")) {
    const auto& spelling = text(*kind, "gripper_actuation_kind");
    if (spelling == "pneumatic") actuation = GripperActuationKind::kPneumatic;
    else if (spelling == "position_commanded") actuation = GripperActuationKind::kPositionCommanded;
    else fail_decode("resolved Stop plan gripper actuation kind is invalid");
  }
  return {
      ResolvedStopBasePlanId(text(member(value, "resolved_stop_base_plan_id", "base plan"),
                                  "resolved_stop_base_plan_id")),
      CellId(text(member(value, "cell_id", "base plan"), "cell_id")),
      RetainedBundleId(
          text(member(value, "retained_bundle_id", "base plan"), "retained_bundle_id")),
      GraphRevision(text(member(value, "graph_revision", "base plan"), "graph_revision")),
      decode_template(member(value, "source_template", "base plan")),
      ControlDomainId(text(member(value, "control_domain_id", "base plan"), "control_domain_id")),
      ControlDomainRevision(
          text(member(value, "control_domain_revision", "base plan"), "control_domain_revision")),
      ControlBindingId(
          text(member(value, "control_binding_id", "base plan"), "control_binding_id")),
      ControlBindingRevision(
          text(member(value, "control_binding_revision", "base plan"), "control_binding_revision")),
      ControllerTopologyId(
          text(member(value, "controller_topology_id", "base plan"), "controller_topology_id")),
      TopologyRevision(text(member(value, "topology_revision", "base plan"), "topology_revision")),
      ControlRouteId(text(member(value, "control_route_id", "base plan"), "control_route_id")),
      ActivationSetId(text(member(value, "activation_set_id", "base plan"), "activation_set_id")),
      ActiveControlConfigurationId(
          text(member(value, "active_control_configuration_id", "base plan"),
               "active_control_configuration_id")),
      ActiveControlConfigurationGeneration(
          unsigned_integer(member(value, "active_control_configuration_generation", "base plan"),
                           "active_control_configuration_generation")),
      RuntimeUnitId(
          text(member(value, "source_runtime_unit_id", "base plan"), "source_runtime_unit_id")),
      RuntimeEpoch(
          text(member(value, "source_runtime_epoch", "base plan"), "source_runtime_epoch")),
      unsigned_integer(member(value, "source_sequence", "base plan"), "source_sequence"),
      StopEvidencePolicyId(
          text(member(value, "stop_evidence_policy_id", "base plan"), "stop_evidence_policy_id")),
      StopEvidencePolicyRevision(text(member(value, "stop_evidence_policy_revision", "base plan"),
                                      "stop_evidence_policy_revision")),
      containment_granularity(member(value, "containment_granularity", "base plan")),
      std::move(stages),
      actuation};
}

}  // namespace

Result<ResolvedStopBasePlanDTO> DecodeResolvedStopBasePlan(const std::string_view canonical_bytes) {
  try {
    const auto normalized = content_identity::parse_canonical_json(canonical_bytes);
    auto decoded = decode_plan(normalized);
    const auto verified = VerifyResolvedStopBasePlan(decoded);
    if (!verified.ok()) {
      return Result<ResolvedStopBasePlanDTO>::Err(verified.error());
    }
    const auto encoded = EncodeResolvedStopBasePlan(decoded);
    if (!encoded.ok()) {
      return Result<ResolvedStopBasePlanDTO>::Err(encoded.error());
    }
    if (encoded.value() != canonical_bytes) {
      return Result<ResolvedStopBasePlanDTO>::Err(
          ErrorDTO{"resolved_stop_base_plan_not_normalized",
                   "Resolved Stop base-plan bytes are not the exact canonical DTO encoding.",
                   {{"reason", "decoded value does not reproduce the supplied bytes"}}});
    }
    return Result<ResolvedStopBasePlanDTO>::Ok(std::move(decoded));
  } catch (const std::exception& error) {
    return Result<ResolvedStopBasePlanDTO>::Err(
        ErrorDTO{"resolved_stop_base_plan_decode_failed",
                 "Resolved Stop base-plan bytes failed closed parsing or validation.",
                 {{"reason", error.what()}}});
  }
}

}  // namespace manyforge::core
