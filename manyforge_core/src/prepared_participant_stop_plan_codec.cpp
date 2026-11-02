#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/stop_timing.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "stop_fallback_codec_internal.hpp"

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeUnsignedInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumSourceJointRows = 256U;

class ValidationError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(std::string reason) {
  throw ValidationError(std::move(reason));
}

template <typename Value>
[[nodiscard]] Result<Value> reject(std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{"prepared_participant_stop_plan_invalid",
               "the prepared participant Stop plan is not an exact closed Prepare product",
               {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool portable_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumIdentityBytes) {
    return false;
  }
  const auto alphanumeric = [](const unsigned char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
    return alphanumeric(static_cast<unsigned char>(character)) || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] constexpr std::string_view interrupt_mode_string(
    MotionInterruptStopMode value) noexcept {
  switch (value) {
    case MotionInterruptStopMode::kSpringBackHold:
      return "spring_back_hold";
    case MotionInterruptStopMode::kSoftOnPath:
      return "soft_on_path";
    case MotionInterruptStopMode::kHardDeceleration:
      return "hard_deceleration";
    case MotionInterruptStopMode::kSpringBackFast:
      return "spring_back_fast";
  }
  return {};
}

void require_identifier(std::string_view value, std::string reason) {
  if (!portable_identifier(value)) {
    fail(std::move(reason));
  }
}

[[nodiscard]] CanonicalValue safe_unsigned(std::uint64_t value) {
  if (value > kMaximumSafeUnsignedInteger) {
    fail("operation revision exceeds the I-JSON safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue json_value(const JsonValueDTO& value) {
  return std::visit(
      [](const auto& item) -> CanonicalValue {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Item, bool> || std::is_same_v<Item, double> ||
                             std::is_same_v<Item, std::string>) {
          return CanonicalValue(item);
        } else if constexpr (std::is_same_v<Item, JsonValueDTO::Array>) {
          CanonicalValue::Array result;
          result.reserve(item.size());
          for (const auto& child : item) {
            result.emplace_back(json_value(child));
          }
          return CanonicalValue(std::move(result));
        } else {
          CanonicalValue::Object result;
          result.reserve(item.size());
          for (const auto& [name, child] : item) {
            result.emplace_back(name, json_value(child));
          }
          return CanonicalValue(std::move(result));
        }
      },
      value.value);
}

[[nodiscard]] CanonicalValue json_object(const JsonObjectDTO& object) {
  JsonValueDTO wrapper;
  wrapper.value = object;
  return json_value(wrapper);
}

[[nodiscard]] JsonObjectDTO acceleration_value(const CoordinateAccelerationDTO& acceleration) {
  return std::visit(
      [](const auto& value) -> JsonObjectDTO {
        if constexpr (requires { value.value_rad_s2; }) {
          return {{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s2")}},
                  {"value_rad_s2", JsonValueDTO{value.value_rad_s2}}};
        } else {
          return {{"coordinate_kind", JsonValueDTO{std::string("linear_m_s2")}},
                  {"value_m_s2", JsonValueDTO{value.value_m_s2}}};
        }
      },
      acceleration);
}

[[nodiscard]] JsonObjectDTO velocity_value(const CoordinateVelocityDTO& velocity) {
  return std::visit(
      [](const auto& value) -> JsonObjectDTO {
        if constexpr (requires { value.value_rad_s; }) {
          return {{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s")}},
                  {"value_rad_s", JsonValueDTO{value.value_rad_s}}};
        } else {
          return {{"coordinate_kind", JsonValueDTO{std::string("linear_m_s")}},
                  {"value_m_s", JsonValueDTO{value.value_m_s}}};
        }
      },
      velocity);
}

[[nodiscard]] JsonObjectDTO jerk_value(const CoordinateJerkDTO& jerk) {
  return std::visit(
      [](const auto& value) -> JsonObjectDTO {
        if constexpr (requires { value.value_rad_s3; })
          return {{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s3")}},
                  {"value_rad_s3", JsonValueDTO{value.value_rad_s3}}};
        else
          return {{"coordinate_kind", JsonValueDTO{std::string("linear_m_s3")}},
                  {"value_m_s3", JsonValueDTO{value.value_m_s3}}};
      },
      jerk);
}

[[nodiscard]] JsonObjectDTO contract_parameters_value(
    const MotionControlledStopContractParametersDTO& parameters) {
  return std::visit(
      [](const auto& selected) {
        using Selected = std::decay_t<decltype(selected)>;
        JsonObjectDTO result{
            {"stop_mode", JsonValueDTO{std::string(to_string(selected.stop_mode))}}};
        JsonValueDTO::Array limits;
        limits.reserve(selected.per_resource_limits.size());
        for (const auto& limit : selected.per_resource_limits) {
          JsonObjectDTO row{{"resource_id", JsonValueDTO{limit.resource_id.value()}},
                            {"settle_velocity_threshold",
                             JsonValueDTO{velocity_value(limit.settle_velocity_threshold)}}};
          if constexpr (!std::is_same_v<Selected, HardDecelerationStopParametersDTO>) {
            row.emplace("max_deceleration",
                        JsonValueDTO{acceleration_value(limit.max_deceleration)});
            if (limit.max_jerk) row.emplace("max_jerk", JsonValueDTO{jerk_value(*limit.max_jerk)});
          }
          limits.emplace_back(JsonValueDTO{std::move(row)});
        }
        result.emplace("per_resource_limits", JsonValueDTO{std::move(limits)});
        if constexpr (!std::is_same_v<Selected, HardDecelerationStopParametersDTO>) {
          result.emplace("deceleration_time_s", JsonValueDTO{selected.deceleration_time_s});
          if (selected.max_stop_time_s.has_value())
            result.emplace("max_stop_time_s", JsonValueDTO{*selected.max_stop_time_s});
          result.emplace("jerk_limits_enabled", JsonValueDTO{selected.jerk_limits_enabled});
          result.emplace("allow_natural_completion_on_short_path",
                         JsonValueDTO{selected.allow_natural_completion_on_short_path});
        }
        if constexpr (!std::is_same_v<Selected, HardDecelerationStopParametersDTO>) {
          if (!selected.fallback_stop_modes.empty()) {
            JsonValueDTO::Array modes;
            for (const auto mode : selected.fallback_stop_modes)
              modes.push_back(JsonValueDTO{std::string(to_string(mode))});
            result.emplace("fallback_stop_modes", JsonValueDTO{std::move(modes)});
          }
        }
        return result;
      },
      parameters);
}

[[nodiscard]] CanonicalValue coordinate_acceleration_value(const CoordinateAccelerationDTO& value) {
  return json_object(acceleration_value(value));
}

[[nodiscard]] CanonicalValue coordinate_velocity_value(const CoordinateVelocityDTO& value) {
  return json_object(velocity_value(value));
}

[[nodiscard]] CanonicalValue profile_value(const MotionInterruptStopProfileDTO& profile) {
  CanonicalValue::Object fields{
      {"profile_id", CanonicalValue(profile.profile_id)},
      {"stop_mode", CanonicalValue(std::string(interrupt_mode_string(profile.stop_mode)))},
      {"allow_natural_completion_on_short_path",
       CanonicalValue(profile.allow_natural_completion_on_short_path)},
      {"require_backend_confirmation", CanonicalValue(profile.require_backend_confirmation)}};
  if (profile.jerk_limits_enabled)
    fields.emplace_back("jerk_limits_enabled", CanonicalValue(*profile.jerk_limits_enabled));
  if (!profile.per_joint_max_jerk.empty()) {
    CanonicalValue::Object limits;
    for (const auto& [joint, limit] : profile.per_joint_max_jerk)
      limits.emplace_back(joint, json_object(jerk_value(limit)));
    fields.emplace_back("per_joint_max_jerk", CanonicalValue(std::move(limits)));
  }
  if (profile.deceleration_time_s.has_value()) {
    fields.emplace_back("deceleration_time_s", CanonicalValue(*profile.deceleration_time_s));
  }
  if (profile.max_stop_time_s.has_value()) {
    fields.emplace_back("max_stop_time_s", CanonicalValue(*profile.max_stop_time_s));
  }
  if (!profile.per_joint_max_deceleration.empty()) {
    CanonicalValue::Object limits;
    limits.reserve(profile.per_joint_max_deceleration.size());
    for (const auto& [joint, limit] : profile.per_joint_max_deceleration) {
      limits.emplace_back(joint, coordinate_acceleration_value(limit));
    }
    fields.emplace_back("per_joint_max_deceleration", CanonicalValue(std::move(limits)));
  }
  if (!profile.fallback_stop_modes.empty()) {
    CanonicalValue::Array modes;
    for (const auto mode : profile.fallback_stop_modes)
      modes.emplace_back(std::string(interrupt_mode_string(mode)));
    fields.emplace_back("fallback_stop_modes", CanonicalValue(std::move(modes)));
  }
  if (profile.per_joint_settle_velocity_threshold.has_value()) {
    CanonicalValue::Object thresholds;
    thresholds.reserve(profile.per_joint_settle_velocity_threshold->size());
    for (const auto& [joint, threshold] : *profile.per_joint_settle_velocity_threshold) {
      thresholds.emplace_back(joint, coordinate_velocity_value(threshold));
    }
    fields.emplace_back("per_joint_settle_velocity_threshold",
                        CanonicalValue(std::move(thresholds)));
  }
  if (!profile.diagnostics.empty()) {
    CanonicalValue::Object diagnostics;
    diagnostics.reserve(profile.diagnostics.size());
    for (const auto& [name, scalar] : profile.diagnostics) {
      diagnostics.emplace_back(
          name, std::visit([](const auto& value) { return CanonicalValue(value); }, scalar));
    }
    fields.emplace_back("diagnostics", CanonicalValue(std::move(diagnostics)));
  }
  return CanonicalValue(std::move(fields));
}

void require_closed_object(const CanonicalValue& value,
                           std::initializer_list<std::string_view> required,
                           std::initializer_list<std::string_view> optional,
                           std::string_view reason) {
  if (!value.is_object()) {
    fail(std::string(reason));
  }
  for (const auto name : required) {
    if (value.find_object_member(name) == nullptr) {
      fail(std::string(reason));
    }
  }
  for (const auto& [name, unused] : value.as_object()) {
    static_cast<void>(unused);
    const auto known = [&](const auto& names) {
      return std::find(names.begin(), names.end(), name) != names.end();
    };
    if (!known(required) && !known(optional)) {
      fail(std::string(reason));
    }
  }
}

[[nodiscard]] const CanonicalValue& required_member(const CanonicalValue& value,
                                                    std::string_view name,
                                                    std::string_view reason) {
  const auto* member = value.find_object_member(name);
  if (member == nullptr) {
    fail(std::string(reason));
  }
  return *member;
}

[[nodiscard]] std::string required_string(const CanonicalValue& value, std::string_view name,
                                          std::string_view reason) {
  const auto& member = required_member(value, name, reason);
  if (!member.is_string()) {
    fail(std::string(reason));
  }
  return member.as_string();
}

[[nodiscard]] bool required_bool(const CanonicalValue& value, std::string_view name,
                                 std::string_view reason) {
  const auto& member = required_member(value, name, reason);
  if (!member.is_bool()) {
    fail(std::string(reason));
  }
  return member.as_bool();
}

[[nodiscard]] double required_positive_number(const CanonicalValue& value, std::string_view name,
                                              std::string_view reason) {
  const auto& member = required_member(value, name, reason);
  if (!member.is_number() || !std::isfinite(member.as_number()) || member.as_number() <= 0.0) {
    fail(std::string(reason));
  }
  return member.as_number();
}

[[nodiscard]] MotionInterruptStopMode parse_profile_mode(std::string_view value) {
  if (value == "spring_back_hold") {
    return MotionInterruptStopMode::kSpringBackHold;
  }
  if (value == "spring_back_fast") {
    return MotionInterruptStopMode::kSpringBackFast;
  }
  if (value == "soft_on_path") {
    return MotionInterruptStopMode::kSoftOnPath;
  }
  if (value == "hard_deceleration") {
    return MotionInterruptStopMode::kHardDeceleration;
  }
  fail("prepared Stop profile mode is invalid");
}

[[nodiscard]] CoordinateAccelerationDTO parse_acceleration(const CanonicalValue& value) {
  require_closed_object(value, {"coordinate_kind"}, {"value_rad_s2", "value_m_s2"},
                        "prepared Stop profile acceleration is invalid");
  const auto kind = required_string(value, "coordinate_kind",
                                    "prepared Stop profile acceleration kind is invalid");
  if (kind == "angular_rad_s2") {
    if (value.as_object().size() != 2U || value.find_object_member("value_m_s2") != nullptr) {
      fail("prepared Stop profile acceleration fields are invalid");
    }
    return AngularAccelerationDTO{required_positive_number(
        value, "value_rad_s2", "prepared Stop profile acceleration is invalid")};
  }
  if (kind == "linear_m_s2") {
    if (value.as_object().size() != 2U || value.find_object_member("value_rad_s2") != nullptr) {
      fail("prepared Stop profile acceleration fields are invalid");
    }
    return LinearAccelerationDTO{required_positive_number(
        value, "value_m_s2", "prepared Stop profile acceleration is invalid")};
  }
  fail("prepared Stop profile acceleration kind is invalid");
}

[[nodiscard]] CoordinateVelocityDTO parse_velocity(const CanonicalValue& value) {
  require_closed_object(value, {"coordinate_kind"}, {"value_rad_s", "value_m_s"},
                        "prepared Stop profile velocity is invalid");
  const auto kind =
      required_string(value, "coordinate_kind", "prepared Stop profile velocity kind is invalid");
  if (kind == "angular_rad_s") {
    if (value.as_object().size() != 2U || value.find_object_member("value_m_s") != nullptr) {
      fail("prepared Stop profile velocity fields are invalid");
    }
    return AngularVelocityDTO{required_positive_number(
        value, "value_rad_s", "prepared Stop profile velocity is invalid")};
  }
  if (kind == "linear_m_s") {
    if (value.as_object().size() != 2U || value.find_object_member("value_rad_s") != nullptr) {
      fail("prepared Stop profile velocity fields are invalid");
    }
    return LinearVelocityDTO{
        required_positive_number(value, "value_m_s", "prepared Stop profile velocity is invalid")};
  }
  fail("prepared Stop profile velocity kind is invalid");
}

[[nodiscard]] MotionInterruptStopProfileDTO parse_profile(const CanonicalValue& value) {
  require_closed_object(value,
                        {"profile_id", "stop_mode", "allow_natural_completion_on_short_path",
                         "require_backend_confirmation"},
                        {"deceleration_time_s", "max_stop_time_s", "per_joint_max_deceleration",
                         "recoverable_substitution_mode", "fallback_stop_modes",
                         "per_joint_settle_velocity_threshold", "diagnostics", "per_joint_max_jerk",
                         "jerk_limits_enabled"},
                        "prepared Stop profile object is invalid");

  MotionInterruptStopProfileDTO profile;
  profile.jerk_limits_enabled =
      value.find_object_member("jerk_limits_enabled")
          ? std::optional(required_bool(value, "jerk_limits_enabled",
                                        "prepared Stop jerk activation is invalid"))
          : std::nullopt;
  if (const auto* limits = value.find_object_member("per_joint_max_jerk")) {
    if (!limits->is_object() || limits->as_object().empty() ||
        limits->as_object().size() > kMaximumSourceJointRows)
      fail("prepared Stop jerk map is invalid");
    for (const auto& [joint, limit] : limits->as_object()) {
      if (!content_identity::is_source_symbol(joint)) fail("prepared Stop source joint is invalid");
      const auto kind =
          required_string(limit, "coordinate_kind", "prepared Stop jerk kind is invalid");
      const bool angular = kind == "angular_rad_s3";
      if (!angular && kind != "linear_m_s3") fail("prepared Stop jerk kind is invalid");
      const std::string_view key = angular ? "value_rad_s3" : "value_m_s3";
      require_closed_object(limit, {"coordinate_kind", key}, {}, "prepared Stop jerk is invalid");
      const auto scalar = required_positive_number(limit, key, "prepared Stop jerk is invalid");
      profile.per_joint_max_jerk.emplace(joint, angular ? CoordinateJerkDTO{AngularJerkDTO{scalar}}
                                                        : CoordinateJerkDTO{LinearJerkDTO{scalar}});
    }
  }
  profile.profile_id = required_string(value, "profile_id", "prepared Stop profile ID is invalid");
  profile.stop_mode = parse_profile_mode(
      required_string(value, "stop_mode", "prepared Stop profile mode is invalid"));
  profile.allow_natural_completion_on_short_path =
      required_bool(value, "allow_natural_completion_on_short_path",
                    "prepared Stop profile natural-completion flag is invalid");
  profile.require_backend_confirmation = required_bool(
      value, "require_backend_confirmation", "prepared Stop profile confirmation flag is invalid");

  if (value.find_object_member("deceleration_time_s") != nullptr) {
    profile.deceleration_time_s = required_positive_number(
        value, "deceleration_time_s", "prepared Stop profile duration is invalid");
  }
  if (value.find_object_member("max_stop_time_s") != nullptr) {
    profile.max_stop_time_s = required_positive_number(
        value, "max_stop_time_s", "prepared Stop profile maximum Stop time is invalid");
    if (!max_stop_time_valid(profile.max_stop_time_s))
      fail("prepared Stop profile maximum Stop time is invalid");
  }
  if (const auto* limits = value.find_object_member("per_joint_max_deceleration");
      limits != nullptr) {
    if (!limits->is_object() || limits->as_object().empty() ||
        limits->as_object().size() > kMaximumSourceJointRows) {
      fail("prepared Stop profile acceleration map is invalid");
    }
    for (const auto& [joint, limit] : limits->as_object()) {
      if (!content_identity::is_source_symbol(joint))
        fail("prepared Stop profile source joint is invalid");
      profile.per_joint_max_deceleration.emplace(joint, parse_acceleration(limit));
    }
  }
  const auto* legacy = value.find_object_member("recoverable_substitution_mode");
  const auto* fallbacks = value.find_object_member("fallback_stop_modes");
  if (legacy && fallbacks) fail("prepared Stop profile has conflicting fallback fields");
  if (legacy) {
    if (!legacy->is_string() || profile.stop_mode != MotionInterruptStopMode::kSoftOnPath ||
        legacy->as_string() != "spring_back_hold")
      fail("invalid legacy Stop fallback");
    profile.fallback_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  }
  if (fallbacks) {
    if (!fallbacks->is_array() || fallbacks->as_array().size() > 2U)
      fail("prepared Stop fallback list is invalid");
    for (const auto& mode : fallbacks->as_array()) {
      if (!mode.is_string()) fail("prepared Stop fallback mode is invalid");
      profile.fallback_stop_modes.push_back(parse_profile_mode(mode.as_string()));
    }
  }
  if (const auto* thresholds = value.find_object_member("per_joint_settle_velocity_threshold");
      thresholds != nullptr) {
    if (!thresholds->is_object() || thresholds->as_object().empty() ||
        thresholds->as_object().size() > kMaximumSourceJointRows) {
      fail("prepared Stop profile settle-threshold map is invalid");
    }
    std::map<std::string, CoordinateVelocityDTO> parsed;
    for (const auto& [joint, threshold] : thresholds->as_object()) {
      if (!content_identity::is_source_symbol(joint))
        fail("prepared Stop profile source joint is invalid");
      parsed.emplace(joint, parse_velocity(threshold));
    }
    profile.per_joint_settle_velocity_threshold = std::move(parsed);
  }
  if (const auto* diagnostics = value.find_object_member("diagnostics"); diagnostics != nullptr) {
    if (!diagnostics->is_object() ||
        diagnostics->as_object().size() > kMaxInterruptStructuredValueEntries) {
      fail("prepared Stop profile diagnostics are invalid");
    }
    for (const auto& [name, scalar] : diagnostics->as_object()) {
      if (name.empty() || name.size() > kMaxInterruptStructuredValueKeyBytes) {
        fail("prepared Stop profile diagnostic key is invalid");
      }
      if (scalar.is_bool()) {
        profile.diagnostics.emplace(name, interrupt_structured_bool(scalar.as_bool()));
      } else if (scalar.is_number() && std::isfinite(scalar.as_number())) {
        profile.diagnostics.emplace(name, interrupt_structured_number(scalar.as_number()));
      } else if (scalar.is_string() &&
                 scalar.as_string().size() <= kMaxInterruptStructuredValueStringBytes) {
        profile.diagnostics.emplace(name, interrupt_structured_string(scalar.as_string()));
      } else {
        fail("prepared Stop profile diagnostic value is invalid");
      }
    }
  }
  if (!motion_interrupt_stop_profile_field_error(profile).empty() ||
      interrupt_structured_value_error(profile.diagnostics).has_value()) {
    fail("prepared Stop profile fields are invalid");
  }
  return profile;
}

struct BaseWireRows final {
  CanonicalValue::Array escalation_stages;
};

[[nodiscard]] BaseWireRows base_wire_rows(const ResolvedStopBasePlanDTO& base_plan) {
  const auto encoded = EncodeResolvedStopBasePlan(base_plan);
  if (!encoded.ok()) {
    fail("resolved Stop base plan is not verified");
  }
  const auto root = content_identity::parse_canonical_json(encoded.value());
  const auto* stages = root.find_object_member("escalation_stages");
  if (stages == nullptr || !stages->is_array()) {
    fail("resolved Stop base plan lost canonical wire rows");
  }
  return {stages->as_array()};
}

[[nodiscard]] CanonicalValue plan_value(const PreparedParticipantStopPlanDTO& plan,
                                        const ResolvedStopBasePlanDTO& base_plan,
                                        bool include_digest) {
  const auto base_wire = base_wire_rows(base_plan);
  if (base_wire.escalation_stages.size() != plan.stage_commands.size()) {
    fail("prepared stage count differs from the base plan");
  }
  CanonicalValue::Array stages;
  stages.reserve(plan.stage_commands.size());
  for (std::size_t index = 0U; index < plan.stage_commands.size(); ++index) {
    const auto& stage = plan.stage_commands[index];
    CanonicalValue::Object fields{{"resolved_stage", base_wire.escalation_stages[index]}};
    if (stage.validated_contract_parameters.has_value()) {
      fields.emplace_back("validated_contract_parameters",
                          json_object(*stage.validated_contract_parameters));
      fields.emplace_back("parameter_source",
                          CanonicalValue(std::string(to_string(*stage.parameter_source))));
    }
    stages.emplace_back(std::move(fields));
  }
  CanonicalValue::Object fields{
      {"api_version", CanonicalValue(std::string(kPreparedParticipantStopPlanApiVersion))},
      {"resolved_stop_base_plan_id", CanonicalValue(plan.resolved_stop_base_plan_id.value())},
      {"operation_id", CanonicalValue(plan.operation_id.value())},
      {"operation_revision", safe_unsigned(plan.operation_revision.value())},
      {"participant_id", CanonicalValue(plan.participant_id.value())},
      {"program_id", CanonicalValue(plan.program_id.value())},
      {"program_revision", CanonicalValue(plan.program_revision.value())},
      {"program_binding_id", CanonicalValue(plan.program_binding_id.value())},
      {"program_run_id", CanonicalValue(plan.program_run_id.value())},
      {"participant_stop_profile_purpose",
       CanonicalValue(std::string(to_string(plan.participant_stop_profile_purpose)))},
      {"stage_commands", CanonicalValue(std::move(stages))}};
  if (plan.participant_stop_profile.has_value()) {
    fields.emplace_back("participant_stop_profile", profile_value(*plan.participant_stop_profile));
  }
  if (plan.motion_interrupt_scope_id.has_value()) {
    fields.emplace_back("motion_interrupt_scope_id",
                        CanonicalValue(*plan.motion_interrupt_scope_id));
  }
  if (include_digest) {
    fields.emplace_back("participant_stop_invocation_plan_digest",
                        CanonicalValue(plan.participant_stop_invocation_plan_digest.value()));
  }
  return detail::stop_fallback_wire_fields(CanonicalValue(std::move(fields)), false);
}

[[nodiscard]] ContentDigest computed_digest(const PreparedParticipantStopPlanDTO& plan,
                                            const ResolvedStopBasePlanDTO& base_plan) {
  return ContentDigest(content_identity::framed_sha256(kParticipantStopInvocationPlanIdentityDomain,
                                                       kPreparedParticipantStopPlanApiVersion,
                                                       plan_value(plan, base_plan, false)));
}

[[nodiscard]] bool legacy_digest(const PreparedParticipantStopPlanDTO& plan,
                                 const ResolvedStopBasePlanDTO& base) {
  try {
    const auto wire = detail::stop_fallback_wire_fields(plan_value(plan, base, false), true);
    return content_identity::digest_equal(
        content_identity::framed_sha256(kParticipantStopInvocationPlanIdentityDomain,
                                        kPreparedParticipantStopPlanApiVersion, wire),
        plan.participant_stop_invocation_plan_digest.value());
  } catch (const std::invalid_argument&) {
    return false;
  }
}

void validate_context(const PreparedParticipantStopPlanContextDTO& context) {
  require_identifier(context.operation_id.value(), "operation ID is invalid");
  if (!context.operation_revision.nonzero() ||
      context.operation_revision.value() > kMaximumSafeUnsignedInteger) {
    fail("operation revision is invalid");
  }
  require_identifier(context.participant_id.value(), "participant ID is invalid");
  require_identifier(context.program_id.value(), "program ID is invalid");
  if (!content_identity::is_sha256_digest(context.program_revision.value())) {
    fail("program revision is invalid");
  }
  if (!content_identity::is_sha256_digest(context.program_binding_id.value())) {
    fail("Program Binding ID is invalid");
  }
  require_identifier(context.program_run_id.value(), "program run ID is invalid");
  if (to_string(context.participant_stop_profile_purpose).empty()) {
    fail("participant Stop profile purpose is invalid");
  }
  const bool profile_present = context.participant_stop_profile.has_value();
  const bool ordinary =
      context.participant_stop_profile_purpose == ParticipantStopProfilePurposeDTO::kOrdinary;
  const bool scope = context.participant_stop_profile_purpose ==
                     ParticipantStopProfilePurposeDTO::kMotionInterruptScope;
  if (profile_present == ordinary || context.motion_interrupt_scope_id.has_value() != scope) {
    fail("participant Stop profile purpose fields are inconsistent");
  }
  if (scope) {
    require_identifier(*context.motion_interrupt_scope_id, "motion interrupt scope ID is invalid");
  }
}

[[nodiscard]] PreparedParticipantStopPlanContextDTO context_from_plan(
    const PreparedParticipantStopPlanDTO& plan, MotionInterruptNodeCategory node_category) {
  return {plan.operation_id,
          plan.operation_revision,
          plan.participant_id,
          plan.program_id,
          plan.program_revision,
          plan.program_binding_id,
          plan.program_run_id,
          plan.participant_stop_profile,
          plan.participant_stop_profile_purpose,
          plan.motion_interrupt_scope_id,
          node_category};
}

}  // namespace

Result<MotionInterruptStopProfileDTO> DecodeMotionInterruptStopProfile(std::string_view bytes) {
  try {
    return Result<MotionInterruptStopProfileDTO>::Ok(
        parse_profile(content_identity::parse_canonical_json(bytes)));
  } catch (const ValidationError& error) {
    return reject<MotionInterruptStopProfileDTO>(error.what());
  } catch (const std::exception&) {
    return reject<MotionInterruptStopProfileDTO>("canonical_decoding_failed");
  }
}

Result<PreparedParticipantStopPlanDTO> MakePreparedParticipantStopPlan(
    const ResolvedStopBasePlanDTO& verified_base_plan,
    PreparedParticipantStopPlanContextDTO context,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits) {
  try {
    if (!VerifyResolvedStopBasePlan(verified_base_plan).ok()) {
      fail("resolved Stop base plan is not verified");
    }
    validate_context(context);

    std::optional<JsonObjectDTO> mapped_parameters;
    if (context.participant_stop_profile.has_value()) {
      auto frozen = resolve_motion_interrupt_stop_jerk_limits(*context.participant_stop_profile);
      if (!frozen.ok()) fail(frozen.error().message);
      context.participant_stop_profile = std::move(frozen.value());
      const auto& nominal = verified_base_plan.escalation_stages.front();
      if (nominal.containment_command.command_kind !=
              StopContainmentCommandKind::kMotionControlledStop ||
          !nominal.containment_command.provider_contract_ref.has_value() ||
          nominal.containment_command.provider_contract_ref->contract_schema_id.value() !=
              kMotionControlledStopContractSchemaId ||
          (nominal.containment_command.provider_contract_ref->contract_schema_revision.value() !=
               kMotionControlledStopContractSchemaRevision &&
           nominal.containment_command.provider_contract_ref->contract_schema_revision.value() !=
               "sha256:f6ac4acc7a4aa33a30eb3180c917b3b60d1469fa6ed4820c5fe2d68c0c2a8f84")) {
        fail("participant profile does not select the qualified motion contract");
      }
      if (!context.participant_stop_profile->fallback_stop_modes.empty()) {
        const auto& qualification = nominal.command_qualification.controlled_substitution_contract;
        if (!qualification)
          fail("participant profile requests an unsupported fallback");
        for (const auto mode : context.participant_stop_profile->fallback_stop_modes) {
          if (std::none_of(qualification->fallback_stop_modes.begin(),
                           qualification->fallback_stop_modes.end(), [&](const auto allowed) {
                             return interrupt_mode_string(mode) == to_string(allowed);
                           }))
            fail("participant profile requests an unsupported fallback");
        }
      }
      auto mapped = map_motion_interrupt_stop_profile_to_contract_parameters(
          *context.participant_stop_profile, context.node_category, resources,
          retained_default_limits);
      if (!mapped.ok()) {
        fail(mapped.error().details.at("reason"));
      }
      mapped_parameters = contract_parameters_value(mapped.value());
    }

    PreparedParticipantStopPlanDTO plan{ContentDigest("pending"),
                                        verified_base_plan.resolved_stop_base_plan_id,
                                        std::move(context.operation_id),
                                        context.operation_revision,
                                        std::move(context.participant_id),
                                        std::move(context.program_id),
                                        std::move(context.program_revision),
                                        std::move(context.program_binding_id),
                                        std::move(context.program_run_id),
                                        std::move(context.participant_stop_profile),
                                        context.participant_stop_profile_purpose,
                                        std::move(context.motion_interrupt_scope_id),
                                        {}};
    plan.stage_commands.reserve(verified_base_plan.escalation_stages.size());
    for (std::size_t index = 0U; index < verified_base_plan.escalation_stages.size(); ++index) {
      const auto& resolved = verified_base_plan.escalation_stages[index];
      const bool provider_contract = resolved.containment_command.provider_contract_ref.has_value();
      PreparedStopStageCommandDTO prepared{resolved, std::nullopt, std::nullopt};
      if (provider_contract) {
        if (index == 0U && mapped_parameters.has_value()) {
          prepared.validated_contract_parameters = std::move(mapped_parameters);
          prepared.parameter_source = StopStageParameterSourceDTO::kParticipantProfileMapping;
        } else {
          if (!resolved.default_contract_parameters.has_value()) {
            fail("provider-contract stage has no deployment parameters");
          }
          prepared.validated_contract_parameters = resolved.default_contract_parameters;
          prepared.parameter_source = StopStageParameterSourceDTO::kDeploymentDefault;
          if (resolved.containment_command.command_kind ==
              StopContainmentCommandKind::kMotionControlledStop) {
            const auto& contract = *resolved.containment_command.provider_contract_ref;
            if (contract.contract_schema_id.value() != kMotionControlledStopContractSchemaId ||
                (contract.contract_schema_revision.value() !=
                     kMotionControlledStopContractSchemaRevision &&
                 contract.contract_schema_revision.value() !=
                     "sha256:f6ac4acc7a4aa33a30eb3180c917b3b60d1469fa6ed4820c5fe2d68c0c2a8f84"))
              fail("controlled default does not select the qualified motion contract");
            const auto profile = map_motion_controlled_stop_contract_parameters_to_profile(
                *resolved.default_contract_parameters, resources, "deployment_default");
            if (!profile.ok()) fail(profile.error().message);
            const auto mapped = map_motion_interrupt_stop_profile_to_contract_parameters(
                profile.value(), context.node_category, resources, retained_default_limits);
            if (!mapped.ok()) fail(mapped.error().message);
            prepared.validated_contract_parameters = contract_parameters_value(mapped.value());
          }
        }
      }
      plan.stage_commands.push_back(std::move(prepared));
    }
    plan.participant_stop_invocation_plan_digest = computed_digest(plan, verified_base_plan);
    const auto bytes = content_identity::canonical_json(plan_value(plan, verified_base_plan, true));
    if (bytes.size() > content_identity::kMaximumCanonicalJsonBytes) {
      fail("prepared participant Stop plan exceeds the canonical byte bound");
    }
    static_cast<void>(content_identity::parse_canonical_json(bytes));
    return Result<PreparedParticipantStopPlanDTO>::Ok(std::move(plan));
  } catch (const ValidationError& error) {
    return reject<PreparedParticipantStopPlanDTO>(error.what());
  } catch (const std::exception&) {
    return reject<PreparedParticipantStopPlanDTO>("canonicalization_failed");
  }
}

Result<void> VerifyPreparedParticipantStopPlan(
    const ResolvedStopBasePlanDTO& verified_base_plan, const PreparedParticipantStopPlanDTO& plan,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category) {
  auto expected =
      MakePreparedParticipantStopPlan(verified_base_plan, context_from_plan(plan, node_category),
                                      resources, retained_default_limits);
  if (!expected.ok()) {
    return Result<void>::Err(expected.error());
  }
  auto comparable = plan;
  comparable.participant_stop_invocation_plan_digest =
      expected.value().participant_stop_invocation_plan_digest;
  if (expected.value() != comparable ||
      (plan.participant_stop_invocation_plan_digest !=
           expected.value().participant_stop_invocation_plan_digest &&
       !legacy_digest(plan, verified_base_plan))) {
    return Result<void>::Err(
        ErrorDTO{"prepared_participant_stop_plan_digest_mismatch",
                 "the prepared participant Stop plan differs from its verified referents",
                 {{"reason", "prepared_participant_stop_plan_mismatch"}}});
  }
  return Result<void>::Ok();
}

Result<std::string> EncodePreparedParticipantStopPlan(
    const ResolvedStopBasePlanDTO& verified_base_plan, const PreparedParticipantStopPlanDTO& plan,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category) {
  const auto verified = VerifyPreparedParticipantStopPlan(verified_base_plan, plan, resources,
                                                          retained_default_limits, node_category);
  if (!verified.ok()) {
    return Result<std::string>::Err(verified.error());
  }
  try {
    auto wire = plan_value(plan, verified_base_plan, true);
    if (plan.participant_stop_invocation_plan_digest != computed_digest(plan, verified_base_plan) &&
        legacy_digest(plan, verified_base_plan))
      wire = detail::stop_fallback_wire_fields(wire, true);
    return Result<std::string>::Ok(content_identity::canonical_json(wire));
  } catch (const std::exception&) {
    return reject<std::string>("canonical_encoding_failed");
  }
}

}  // namespace manyforge::core
