#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_motion_evidence_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;
constexpr double kMaximumSafeInteger = 9'007'199'254'740'991.0;
constexpr std::size_t kMaximumIdentityBytes = 128U;

class Invalid final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(std::string reason) {
  throw Invalid(std::move(reason));
}

template <class Value>
[[nodiscard]] Result<Value> rejected(std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{"prepared_motion_evidence_invalid",
               "prepared motion evidence is not an exact closed Prepare product",
               {{"reason", std::move(reason)}}});
}

[[nodiscard]] const CanonicalValue& member(const CanonicalValue& value, std::string_view name) {
  const auto* result = value.find_object_member(name);
  if (result == nullptr) fail("required_member_missing");
  return *result;
}

void exact(const CanonicalValue& value, std::initializer_list<std::string_view> names) {
  if (!value.is_object() || value.as_object().size() != names.size()) {
    fail("unknown_or_missing_member");
  }
  for (const auto name : names) static_cast<void>(member(value, name));
}

[[nodiscard]] std::string text(const CanonicalValue& value) {
  if (!value.is_string()) fail("string_required");
  return value.as_string();
}

[[nodiscard]] bool boolean(const CanonicalValue& value) {
  if (!value.is_bool()) fail("boolean_required");
  return value.as_bool();
}

[[nodiscard]] std::uint64_t unsigned_integer(const CanonicalValue& value, bool nonzero = false) {
  if (!value.is_number() || value.as_number() < (nonzero ? 1.0 : 0.0) ||
      value.as_number() > kMaximumSafeInteger ||
      std::floor(value.as_number()) != value.as_number()) {
    fail("safe_unsigned_integer_required");
  }
  return static_cast<std::uint64_t>(value.as_number());
}

[[nodiscard]] bool portable_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumIdentityBytes) return false;
  const auto alpha_numeric = [](unsigned char character) {
    return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (!alpha_numeric(static_cast<unsigned char>(value.front()))) return false;
  return std::all_of(value.begin() + 1, value.end(), [&](char character) {
    return alpha_numeric(static_cast<unsigned char>(character)) || character == '.' ||
           character == '_' || character == '-';
  });
}

void require_digest(std::string_view value, std::string reason) {
  if (!content_identity::is_sha256_digest(value)) fail(std::move(reason));
}

[[nodiscard]] CanonicalValue embedded(std::string bytes) {
  return content_identity::parse_canonical_json(bytes);
}

[[nodiscard]] CanonicalValue provider_binding_value(const PlannerCapabilityBindingDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"capability_id", CanonicalValue(value.capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(value.capability_definition_revision.value())},
      {"capability_provider_binding_id",
       CanonicalValue(value.capability_provider_binding_id.value())},
      {"capability_provider_binding_revision",
       CanonicalValue(value.capability_provider_binding_revision.value())},
      {"provider_instance_id", CanonicalValue(value.provider_instance_id.value())},
      {"provider_manifest_id", CanonicalValue(value.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(value.provider_manifest_revision.value())},
      {"provider_runtime_unit_id", CanonicalValue(value.provider_runtime_unit_id.value())},
      {"provider_runtime_epoch", CanonicalValue(value.provider_runtime_epoch.value())}});
}

[[nodiscard]] PlannerCapabilityBindingDTO parse_provider_binding(const CanonicalValue& value) {
  exact(value,
        {"capability_id", "capability_definition_revision", "capability_provider_binding_id",
         "capability_provider_binding_revision", "provider_instance_id", "provider_manifest_id",
         "provider_manifest_revision", "provider_runtime_unit_id", "provider_runtime_epoch"});
  PlannerCapabilityBindingDTO result{
      CapabilityId(text(member(value, "capability_id"))),
      CapabilityDefinitionRevision(text(member(value, "capability_definition_revision"))),
      CapabilityProviderBindingId(text(member(value, "capability_provider_binding_id"))),
      CapabilityProviderBindingRevision(
          text(member(value, "capability_provider_binding_revision"))),
      ProviderInstanceId(text(member(value, "provider_instance_id"))),
      ProviderManifestId(text(member(value, "provider_manifest_id"))),
      ProviderManifestRevision(text(member(value, "provider_manifest_revision"))),
      RuntimeUnitId(text(member(value, "provider_runtime_unit_id"))),
      RuntimeEpoch(text(member(value, "provider_runtime_epoch")))};
  if (!portable_identifier(result.capability_id.value()) ||
      !content_identity::is_sha256_digest(result.capability_definition_revision.value()) ||
      !content_identity::is_sha256_digest(result.capability_provider_binding_id.value()) ||
      !content_identity::is_sha256_digest(result.capability_provider_binding_revision.value()) ||
      !portable_identifier(result.provider_instance_id.value()) ||
      !content_identity::is_sha256_digest(result.provider_manifest_id.value()) ||
      !content_identity::is_sha256_digest(result.provider_manifest_revision.value()) ||
      !portable_identifier(result.provider_runtime_unit_id.value()) ||
      !portable_identifier(result.provider_runtime_epoch.value())) {
    fail("planner_capability_binding_invalid");
  }
  return result;
}

[[nodiscard]] CanonicalValue planning_value(const PreparedMotionPlanningEvidenceDTO& value) {
  const auto encoded_result = EncodePreparedPlanResult(value.planning_result);
  if (!encoded_result.ok() || !value.planning_result.trajectory.has_value() ||
      value.planning_result.error.has_value()) {
    fail("accepted_planning_result_required");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"planning_result", embedded(encoded_result.value())},
      {"planning_attempt_index", CanonicalValue(static_cast<double>(value.planning_attempt_index))},
      {"planner_capability_binding", provider_binding_value(value.planner_capability_binding)}});
}

[[nodiscard]] PreparedMotionPlanningEvidenceDTO parse_planning(const CanonicalValue& value) {
  exact(value, {"planning_result", "planning_attempt_index", "planner_capability_binding"});
  const auto result_bytes = content_identity::canonical_json(member(value, "planning_result"));
  auto result = DecodePreparedPlanResult(result_bytes);
  if (!result.ok() || !result.value().trajectory.has_value() || result.value().error.has_value()) {
    fail("accepted_planning_result_required");
  }
  const auto attempt = unsigned_integer(member(value, "planning_attempt_index"));
  if (attempt > std::numeric_limits<std::uint32_t>::max()) fail("planning_attempt_index_invalid");
  return {std::move(result.value()), static_cast<std::uint32_t>(attempt),
          parse_provider_binding(member(value, "planner_capability_binding"))};
}

[[nodiscard]] CanonicalValue revision_value(const CollisionWorldRevision& value) {
  if (value.revision_id == 0U || static_cast<double>(value.revision_id) > kMaximumSafeInteger) {
    fail("collision_world_revision_id_invalid");
  }
  require_digest(value.content_hash, "collision_world_content_hash_invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"revision_id", CanonicalValue(static_cast<double>(value.revision_id))},
      {"content_hash", CanonicalValue(value.content_hash)}});
}

[[nodiscard]] CollisionWorldRevision parse_world_revision(const CanonicalValue& value) {
  exact(value, {"revision_id", "content_hash"});
  CollisionWorldRevision result{unsigned_integer(member(value, "revision_id"), true),
                                text(member(value, "content_hash"))};
  require_digest(result.content_hash, "collision_world_content_hash_invalid");
  return result;
}

[[nodiscard]] CanonicalValue revision_value(const CollisionPolicyRevision& value) {
  if (value.revision_id == 0U || static_cast<double>(value.revision_id) > kMaximumSafeInteger) {
    fail("collision_policy_revision_id_invalid");
  }
  require_digest(value.content_hash, "collision_policy_content_hash_invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"revision_id", CanonicalValue(static_cast<double>(value.revision_id))},
      {"content_hash", CanonicalValue(value.content_hash)}});
}

[[nodiscard]] CollisionPolicyRevision parse_policy_revision(const CanonicalValue& value) {
  exact(value, {"revision_id", "content_hash"});
  CollisionPolicyRevision result{unsigned_integer(member(value, "revision_id"), true),
                                 text(member(value, "content_hash"))};
  require_digest(result.content_hash, "collision_policy_content_hash_invalid");
  return result;
}

[[nodiscard]] CanonicalValue decision_value(const CollisionDecision& value) {
  if (!value.accepted || value.severity != CollisionDecisionSeverity::kNone ||
      value.action != CollisionDecisionAction::kDispatch || value.first_failing_waypoint ||
      value.first_failing_time_ns || !value.collision_world_revision || value.failure_archive_id) {
    fail("accepted_collision_decision_required");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted", CanonicalValue(true)},
      {"severity", CanonicalValue("none")},
      {"action", CanonicalValue("dispatch")},
      {"collision_world_revision", revision_value(*value.collision_world_revision)}});
}

[[nodiscard]] bool same_revision(const CollisionWorldRevision& lhs,
                                 const CollisionWorldRevision& rhs) noexcept {
  return lhs.revision_id == rhs.revision_id && lhs.content_hash == rhs.content_hash;
}

[[nodiscard]] CollisionDecision parse_decision(const CanonicalValue& value) {
  exact(value, {"accepted", "severity", "action", "collision_world_revision"});
  if (!boolean(member(value, "accepted")) || text(member(value, "severity")) != "none" ||
      text(member(value, "action")) != "dispatch") {
    fail("accepted_collision_decision_required");
  }
  CollisionDecision result;
  result.accepted = true;
  result.severity = CollisionDecisionSeverity::kNone;
  result.action = CollisionDecisionAction::kDispatch;
  result.collision_world_revision = parse_world_revision(member(value, "collision_world_revision"));
  return result;
}

[[nodiscard]] CanonicalValue validation_value(const PreparedMotionValidationEvidenceDTO& value) {
  if (!portable_identifier(value.validation_revision.value()) || !value.noncollision_result.valid ||
      value.noncollision_result.error || !value.collision_decision.collision_world_revision ||
      !same_revision(*value.collision_decision.collision_world_revision,
                     value.collision_world_revision)) {
    fail("accepted_validation_evidence_required");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"validation_revision", CanonicalValue(value.validation_revision.value())},
      {"noncollision_result",
       CanonicalValue(CanonicalValue::Object{{"valid", CanonicalValue(true)}})},
      {"collision_world_revision", revision_value(value.collision_world_revision)},
      {"collision_policy_revision", revision_value(value.collision_policy_revision)},
      {"collision_decision", decision_value(value.collision_decision)}});
}

[[nodiscard]] PreparedMotionValidationEvidenceDTO parse_validation(const CanonicalValue& value) {
  exact(value, {"validation_revision", "noncollision_result", "collision_world_revision",
                "collision_policy_revision", "collision_decision"});
  const auto validation_revision = text(member(value, "validation_revision"));
  if (!portable_identifier(validation_revision)) fail("validation_revision_invalid");
  const auto& noncollision = member(value, "noncollision_result");
  exact(noncollision, {"valid"});
  if (!boolean(member(noncollision, "valid"))) fail("accepted_noncollision_result_required");
  PreparedMotionValidationEvidenceDTO result{
      ValidationRevision(validation_revision), ValidationResultDTO{true, std::nullopt},
      parse_world_revision(member(value, "collision_world_revision")),
      parse_policy_revision(member(value, "collision_policy_revision")),
      parse_decision(member(value, "collision_decision"))};
  if (!result.collision_decision.collision_world_revision ||
      !same_revision(*result.collision_decision.collision_world_revision,
                     result.collision_world_revision)) {
    fail("collision_world_revision_mismatch");
  }
  return result;
}

template <class Value, class Encoder>
[[nodiscard]] Result<std::string> encode(const Value& value, Encoder&& encoder) {
  try {
    const auto bytes = content_identity::canonical_json(encoder(value));
    if (bytes.size() > content_identity::kMaximumCanonicalJsonBytes) {
      fail("canonical_byte_bound_exceeded");
    }
    return Result<std::string>::Ok(bytes);
  } catch (const std::exception& error) {
    return rejected<std::string>(error.what());
  }
}

template <class Value, class Decoder>
[[nodiscard]] Result<Value> decode(std::string_view bytes, Decoder&& decoder) {
  try {
    if (bytes.size() > content_identity::kMaximumCanonicalJsonBytes) {
      fail("canonical_byte_bound_exceeded");
    }
    auto value = decoder(content_identity::parse_canonical_json(bytes));
    return Result<Value>::Ok(std::move(value));
  } catch (const std::exception& error) {
    return rejected<Value>(error.what());
  }
}

}  // namespace

Result<std::string> EncodePreparedMotionPlanningEvidence(
    const PreparedMotionPlanningEvidenceDTO& value) {
  return encode(value, planning_value);
}

Result<PreparedMotionPlanningEvidenceDTO> DecodePreparedMotionPlanningEvidence(
    std::string_view canonical_bytes) {
  return decode<PreparedMotionPlanningEvidenceDTO>(canonical_bytes, parse_planning);
}

Result<std::string> EncodePreparedMotionValidationEvidence(
    const PreparedMotionValidationEvidenceDTO& value) {
  return encode(value, validation_value);
}

Result<PreparedMotionValidationEvidenceDTO> DecodePreparedMotionValidationEvidence(
    std::string_view canonical_bytes) {
  return decode<PreparedMotionValidationEvidenceDTO>(canonical_bytes, parse_validation);
}

}  // namespace manyforge::core
