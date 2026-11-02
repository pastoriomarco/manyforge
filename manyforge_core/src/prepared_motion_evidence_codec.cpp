#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_motion_evidence_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
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

[[nodiscard]] double finite_number(const CanonicalValue& value) {
  if (!value.is_number() || !std::isfinite(value.as_number())) {
    fail("finite_number_required");
  }
  return value.as_number();
}

[[nodiscard]] CanonicalValue numbers(const std::vector<double>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const double value : values) {
    if (!std::isfinite(value)) fail("finite_number_required");
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::vector<double> number_array(const CanonicalValue& value) {
  if (!value.is_array()) fail("array_required");
  std::vector<double> result;
  result.reserve(value.as_array().size());
  for (const auto& item : value.as_array()) {
    result.push_back(finite_number(item));
  }
  return result;
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
      !portable_identifier(result.provider_manifest_id.value()) ||
      !content_identity::is_sha256_digest(result.provider_manifest_revision.value()) ||
      !portable_identifier(result.provider_runtime_unit_id.value()) ||
      !portable_identifier(result.provider_runtime_epoch.value())) {
    fail("planner_capability_binding_invalid");
  }
  return result;
}

[[nodiscard]] CanonicalValue planning_value(const PreparedMotionPlanningEvidenceDTO& value,
                                            const MovingReplacementInputDTO* replacement) {
  const auto encoded_result = EncodePreparedPlanResult(value.planning_result, replacement);
  if (!encoded_result.ok() || !value.planning_result.trajectory.has_value() ||
      value.planning_result.error.has_value()) {
    fail("accepted_planning_result_required");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"planning_result", embedded(encoded_result.value())},
      {"planning_attempt_index", CanonicalValue(static_cast<double>(value.planning_attempt_index))},
      {"planner_capability_binding", provider_binding_value(value.planner_capability_binding)}});
}

[[nodiscard]] PreparedMotionPlanningEvidenceDTO parse_planning(
    const CanonicalValue& value, const MovingReplacementInputDTO* replacement) {
  exact(value, {"planning_result", "planning_attempt_index", "planner_capability_binding"});
  const auto result_bytes = content_identity::canonical_json(member(value, "planning_result"));
  auto result = DecodePreparedPlanResult(result_bytes, replacement);
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

[[nodiscard]] CanonicalValue cartesian_fidelity_value(
    const std::optional<CartesianFidelityMetricsDTO>& value) {
  if (!value) return CanonicalValue(nullptr);
  if (!valid_cartesian_fidelity_metrics(*value)) fail("cartesian_fidelity_metrics_invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"certified_max_rotation_error_rad", CanonicalValue(value->certified_max_rotation_error_rad)},
      {"certified_max_translation_error_m",
       CanonicalValue(value->certified_max_translation_error_m)},
      {"measured_max_rotation_error_rad", CanonicalValue(value->measured_max_rotation_error_rad)},
      {"measured_max_translation_error_m", CanonicalValue(value->measured_max_translation_error_m)},
      {"requested_rotation_tolerance_rad", CanonicalValue(value->requested_rotation_tolerance_rad)},
      {"requested_translation_tolerance_m",
       CanonicalValue(value->requested_translation_tolerance_m)},
      {"unsampled_rotation_bound_rad", CanonicalValue(value->unsampled_rotation_bound_rad)},
      {"unsampled_translation_bound_m", CanonicalValue(value->unsampled_translation_bound_m)},
  });
}

[[nodiscard]] std::optional<CartesianFidelityMetricsDTO> parse_cartesian_fidelity(
    const CanonicalValue& value) {
  if (value.is_null()) return std::nullopt;
  exact(value, {"certified_max_rotation_error_rad", "certified_max_translation_error_m",
                "measured_max_rotation_error_rad", "measured_max_translation_error_m",
                "requested_rotation_tolerance_rad", "requested_translation_tolerance_m",
                "unsampled_rotation_bound_rad", "unsampled_translation_bound_m"});
  CartesianFidelityMetricsDTO result{
      finite_number(member(value, "requested_translation_tolerance_m")),
      finite_number(member(value, "requested_rotation_tolerance_rad")),
      finite_number(member(value, "measured_max_translation_error_m")),
      finite_number(member(value, "measured_max_rotation_error_rad")),
      finite_number(member(value, "unsampled_translation_bound_m")),
      finite_number(member(value, "unsampled_rotation_bound_rad")),
      finite_number(member(value, "certified_max_translation_error_m")),
      finite_number(member(value, "certified_max_rotation_error_rad"))};
  if (!valid_cartesian_fidelity_metrics(result))
    fail("cartesian_fidelity_metrics_invalid");
  return result;
}

[[nodiscard]] CanonicalValue metrics_value(const TrajectoryValidationMetricsDTO& metrics) {
  const auto metrics_valid = [](const std::vector<double>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](const double item) { return std::isfinite(item) && item >= 0.0; });
  };
  if (!std::isfinite(metrics.trajectory_duration_s) || metrics.trajectory_duration_s < 0.0 ||
      metrics.peak_abs_joint_velocity_rad_s.empty() ||
      !metrics_valid(metrics.peak_abs_joint_velocity_rad_s) ||
      (!metrics.peak_abs_joint_acceleration_rad_s2.empty() &&
       metrics.peak_abs_joint_acceleration_rad_s2.size() !=
           metrics.peak_abs_joint_velocity_rad_s.size()) ||
      !metrics_valid(metrics.peak_abs_joint_acceleration_rad_s2) ||
      (metrics.peak_tcp_linear_speed_mps.has_value() &&
       (!std::isfinite(*metrics.peak_tcp_linear_speed_mps) ||
        *metrics.peak_tcp_linear_speed_mps < 0.0)) ||
      (metrics.cartesian_fidelity_metrics.has_value() &&
       !valid_cartesian_fidelity_metrics(*metrics.cartesian_fidelity_metrics)) ||
      (metrics.peak_tcp_linear_speed_frame.has_value() &&
       (metrics.peak_tcp_linear_speed_frame->empty() ||
        !metrics.peak_tcp_linear_speed_mps.has_value()))) {
    fail("trajectory_validation_metrics_invalid");
  }
  CanonicalValue::Object metrics_fields{
      {"peak_abs_joint_acceleration_rad_s2",
       metrics.peak_abs_joint_acceleration_rad_s2.empty()
           ? CanonicalValue(nullptr)
           : numbers(metrics.peak_abs_joint_acceleration_rad_s2)},
      {"peak_abs_joint_velocity_rad_s", numbers(metrics.peak_abs_joint_velocity_rad_s)},
      {"peak_tcp_linear_speed_mps", metrics.peak_tcp_linear_speed_mps.has_value()
                                        ? CanonicalValue(*metrics.peak_tcp_linear_speed_mps)
                                        : CanonicalValue(nullptr)},
      {"trajectory_duration_s", CanonicalValue(metrics.trajectory_duration_s)},
  };
  if (metrics.cartesian_fidelity_metrics)
    metrics_fields.emplace_back("cartesian_fidelity_metrics",
                                cartesian_fidelity_value(metrics.cartesian_fidelity_metrics));
  if (metrics.peak_tcp_linear_speed_frame)
    metrics_fields.emplace_back("peak_tcp_linear_speed_frame",
                                CanonicalValue(*metrics.peak_tcp_linear_speed_frame));
  return CanonicalValue(std::move(metrics_fields));
}

[[nodiscard]] TrajectoryValidationMetricsDTO parse_metrics(const CanonicalValue& metrics_value) {
  const auto* fidelity_value = metrics_value.find_object_member("cartesian_fidelity_metrics");
  const auto* speed_frame_value =
      metrics_value.find_object_member("peak_tcp_linear_speed_frame");
  if (!metrics_value.is_object() ||
      metrics_value.as_object().size() !=
          4U + (fidelity_value ? 1U : 0U) + (speed_frame_value ? 1U : 0U))
    fail("unknown_or_missing_member");
  for (const auto name : {"peak_abs_joint_acceleration_rad_s2", "peak_abs_joint_velocity_rad_s",
                          "peak_tcp_linear_speed_mps", "trajectory_duration_s"})
    static_cast<void>(member(metrics_value, name));
  const auto& acceleration_value = member(metrics_value, "peak_abs_joint_acceleration_rad_s2");
  const auto& tcp_value = member(metrics_value, "peak_tcp_linear_speed_mps");
  TrajectoryValidationMetricsDTO metrics{
      finite_number(member(metrics_value, "trajectory_duration_s")),
      number_array(member(metrics_value, "peak_abs_joint_velocity_rad_s")),
      acceleration_value.is_null() ? std::vector<double>{} : number_array(acceleration_value),
      tcp_value.is_null() ? std::nullopt : std::optional<double>{finite_number(tcp_value)},
      fidelity_value ? parse_cartesian_fidelity(*fidelity_value) : std::nullopt};
  if (speed_frame_value) {
    metrics.peak_tcp_linear_speed_frame = text(*speed_frame_value);
    if (metrics.peak_tcp_linear_speed_frame->empty() || !metrics.peak_tcp_linear_speed_mps)
      fail("trajectory_validation_metrics_invalid");
  }
  const auto metrics_valid = [](const std::vector<double>& values) {
    return std::all_of(values.begin(), values.end(), [](const double item) { return item >= 0.0; });
  };
  if (metrics.trajectory_duration_s < 0.0 || metrics.peak_abs_joint_velocity_rad_s.empty() ||
      !metrics_valid(metrics.peak_abs_joint_velocity_rad_s) ||
      (!metrics.peak_abs_joint_acceleration_rad_s2.empty() &&
       metrics.peak_abs_joint_acceleration_rad_s2.size() !=
           metrics.peak_abs_joint_velocity_rad_s.size()) ||
      !metrics_valid(metrics.peak_abs_joint_acceleration_rad_s2) ||
      (metrics.peak_tcp_linear_speed_mps.has_value() && *metrics.peak_tcp_linear_speed_mps < 0.0) ||
      (metrics.cartesian_fidelity_metrics.has_value() &&
       !valid_cartesian_fidelity_metrics(*metrics.cartesian_fidelity_metrics))) {
    fail("trajectory_validation_metrics_invalid");
  }
  return metrics;
}

void validate_piece_diagnostics(const MovingReplacementValidationEvidenceDTO& pair) {
  // Only the complete candidate carries authored-path fidelity authority.
  if (pair.unchanged_prefix.cartesian_fidelity_metrics ||
      pair.changed_future.cartesian_fidelity_metrics)
    fail("moving_replacement_piece_fidelity_forbidden");
}

[[nodiscard]] CanonicalValue validation_value(const PreparedMotionValidationEvidenceDTO& value) {
  if (!portable_identifier(value.validation_revision.value()) || !value.noncollision_result.valid ||
      value.noncollision_result.error || !value.noncollision_result.trajectory_metrics ||
      !value.collision_decision.collision_world_revision ||
      !same_revision(*value.collision_decision.collision_world_revision,
                     value.collision_world_revision)) {
    fail("accepted_validation_evidence_required");
  }
  CanonicalValue::Object noncollision{
      {"trajectory_metrics", metrics_value(*value.noncollision_result.trajectory_metrics)},
      {"valid", CanonicalValue(true)}};
  if (value.noncollision_result.moving_replacement) {
    const auto& pair = *value.noncollision_result.moving_replacement;
    validate_piece_diagnostics(pair);
    noncollision.emplace_back("moving_replacement",
                              CanonicalValue(CanonicalValue::Object{
                                  {"unchanged_prefix", metrics_value(pair.unchanged_prefix)},
                                  {"changed_future", metrics_value(pair.changed_future)}}));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"validation_revision", CanonicalValue(value.validation_revision.value())},
      {"noncollision_result", CanonicalValue(std::move(noncollision))},
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
  const auto* moving = noncollision.find_object_member("moving_replacement");
  exact(noncollision, moving
                          ? std::initializer_list<std::string_view>{"trajectory_metrics", "valid",
                                                                    "moving_replacement"}
                          : std::initializer_list<std::string_view>{"trajectory_metrics", "valid"});
  if (!boolean(member(noncollision, "valid"))) fail("accepted_noncollision_result_required");
  PreparedMotionValidationEvidenceDTO result{
      ValidationRevision(validation_revision),
      ValidationResultDTO{true, std::nullopt,
                          parse_metrics(member(noncollision, "trajectory_metrics"))},
      parse_world_revision(member(value, "collision_world_revision")),
      parse_policy_revision(member(value, "collision_policy_revision")),
      parse_decision(member(value, "collision_decision"))};
  if (moving) {
    exact(*moving, {"unchanged_prefix", "changed_future"});
    result.noncollision_result.moving_replacement =
        MovingReplacementValidationEvidenceDTO{parse_metrics(member(*moving, "unchanged_prefix")),
                                               parse_metrics(member(*moving, "changed_future"))};
    validate_piece_diagnostics(*result.noncollision_result.moving_replacement);
  }
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
    const PreparedMotionPlanningEvidenceDTO& value, const MovingReplacementInputDTO* replacement) {
  return encode(value,
                [replacement](const auto& item) { return planning_value(item, replacement); });
}

Result<PreparedMotionPlanningEvidenceDTO> DecodePreparedMotionPlanningEvidence(
    std::string_view canonical_bytes, const MovingReplacementInputDTO* replacement) {
  return decode<PreparedMotionPlanningEvidenceDTO>(
      canonical_bytes,
      [replacement](const auto& item) { return parse_planning(item, replacement); });
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
