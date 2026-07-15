#include <manyforge_core/cache/trajectory_cache_intent.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

class IntentIdentityBuilder {
 public:
  IntentIdentityBuilder& mix(std::string_view value) {
    append_byte(kTypeString);
    append_uint64_le(static_cast<std::uint64_t>(value.size()));
    bytes_.append(value.data(), value.size());
    return *this;
  }

  IntentIdentityBuilder& mix(std::int64_t value) {
    append_byte(kTypeInt64);
    append_uint64_le(static_cast<std::uint64_t>(value));
    return *this;
  }

  IntentIdentityBuilder& mix(std::uint64_t value) {
    append_byte(kTypeUInt64);
    append_uint64_le(value);
    return *this;
  }

  IntentIdentityBuilder& mix(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    append_byte(kTypeDouble);
    append_uint64_le(bits);
    return *this;
  }

  [[nodiscard]] std::string finish() && { return std::move(bytes_); }

 private:
  void append_byte(unsigned char value) {
    bytes_.push_back(static_cast<char>(value));
  }

  void append_uint64_le(std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      append_byte(static_cast<unsigned char>((value >> (8U * index)) & 0xffU));
    }
  }

  static constexpr unsigned char kTypeString = 0x01U;
  static constexpr unsigned char kTypeInt64 = 0x02U;
  static constexpr unsigned char kTypeUInt64 = 0x03U;
  static constexpr unsigned char kTypeDouble = 0x04U;

  std::string bytes_;
};

void mix_pose(IntentIdentityBuilder& identity, const Pose3DTO& pose) {
  identity.mix(pose.position_m.x_m)
      .mix(pose.position_m.y_m)
      .mix(pose.position_m.z_m)
      .mix(pose.orientation.x)
      .mix(pose.orientation.y)
      .mix(pose.orientation.z)
      .mix(pose.orientation.w);
}

void mix_reference(IntentIdentityBuilder& identity,
                   const ReferenceDTO& reference) {
  identity.mix(static_cast<std::int64_t>(reference.type))
      .mix(reference.id)
      .mix(static_cast<std::int64_t>(reference.pose.has_value()));
  if (reference.pose.has_value()) {
    mix_pose(identity, *reference.pose);
  }
}

void mix_motion(IntentIdentityBuilder& identity, const MotionSpecDTO& motion) {
  std::visit(
      [&identity](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, JointGoalDTO>) {
          const bool duplicate_names = [&value] {
            auto names = value.joint_names;
            std::sort(names.begin(), names.end());
            return std::adjacent_find(names.begin(), names.end()) != names.end();
          }();
          const bool malformed =
              value.joint_names.size() != value.positions_rad.size() ||
              duplicate_names;
          if (malformed) {
            // Preserve every raw byte of a malformed request so it can never
            // alias a different malformed request before validation rejects it.
            identity.mix("joint_goal_malformed")
                .mix(static_cast<std::uint64_t>(value.joint_names.size()));
            for (const auto& joint_name : value.joint_names) {
              identity.mix(joint_name);
            }
            identity.mix(
                static_cast<std::uint64_t>(value.positions_rad.size()));
            for (const auto position : value.positions_rad) {
              identity.mix(position);
            }
            return;
          }
          std::vector<std::pair<std::string, double>> joints;
          joints.reserve(value.joint_names.size());
          for (std::size_t index = 0; index < value.joint_names.size(); ++index) {
            joints.emplace_back(value.joint_names[index],
                                value.positions_rad[index]);
          }
          std::sort(joints.begin(), joints.end(),
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.first < rhs.first;
                    });
          identity.mix("joint_goal")
              .mix(static_cast<std::uint64_t>(joints.size()));
          for (const auto& [joint_name, position] : joints) {
            identity.mix(joint_name).mix(position);
          }
        } else if constexpr (std::is_same_v<T, PoseGoalDTO>) {
          identity.mix("pose_goal");
          mix_pose(identity, value.goal);
        } else if constexpr (std::is_same_v<T, NamedTargetDTO>) {
          identity.mix("named_target").mix(value.name);
        } else if constexpr (std::is_same_v<T, CartesianPathDTO>) {
          identity.mix("cartesian_path")
              .mix(static_cast<std::uint64_t>(value.waypoints.size()));
          for (const auto& waypoint : value.waypoints) {
            mix_pose(identity, waypoint);
          }
          identity.mix(value.max_translation_step_m)
              .mix(value.max_rotation_step_rad)
              .mix(static_cast<std::int64_t>(value.speed_mode))
              .mix(value.max_linear_speed_mps)
              .mix(value.max_angular_speed_rad_s)
              .mix(value.max_linear_acceleration_mps2)
              .mix(value.max_angular_acceleration_rad_s2)
              .mix(static_cast<std::int64_t>(value.fallback_policy));
        }
      },
      motion);
}

}  // namespace

std::string trajectory_cache_intent_identity(
    const ExecuteRequestDTO& request) {
  IntentIdentityBuilder identity;
  identity.mix("manyforge_core/trajectory_cache_intent/v3")
      .mix(request.robot_id)
      .mix(request.group_id)
      .mix(request.base_frame)
      .mix(request.tcp_frame)
      .mix(request.contact_policy_id)
      .mix(static_cast<std::int64_t>(request.target_mode))
      .mix(static_cast<std::int64_t>(request.planning_profile))
      .mix(static_cast<std::int64_t>(request.target_reference.has_value()));
  if (request.target_reference.has_value()) {
    mix_reference(identity, *request.target_reference);
  }
  identity.mix(
      static_cast<std::int64_t>(request.offset_reference.has_value()));
  if (request.offset_reference.has_value()) {
    mix_reference(identity, *request.offset_reference);
  }
  mix_motion(identity, request.motion);
  identity.mix(request.tolerances.joint_position_tol_rad)
      .mix(request.tolerances.translation_tol_m)
      .mix(request.tolerances.rotation_tol_rad)
      .mix(request.speed_profile.velocity_scale)
      .mix(request.speed_profile.acceleration_scale)
      .mix(static_cast<std::int64_t>(
          request.speed_profile.max_tcp_linear_speed_mps.has_value()));
  if (request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
    identity.mix(*request.speed_profile.max_tcp_linear_speed_mps);
  }
  // Planning budget can change which feasible route is returned. Execution
  // timeout governs only transport after planning and is intentionally absent.
  identity.mix(request.timeouts.planning_timeout_s);
  identity.mix(static_cast<std::uint64_t>(
      request.per_joint_dynamics_limits.size()));
  for (const auto& [joint_name, limits] :
       request.per_joint_dynamics_limits) {
    identity.mix(joint_name)
        .mix(static_cast<std::int64_t>(limits.max_velocity_rad_s.has_value()));
    if (limits.max_velocity_rad_s.has_value()) {
      identity.mix(*limits.max_velocity_rad_s);
    }
    identity.mix(static_cast<std::int64_t>(
        limits.max_acceleration_rad_s2.has_value()));
    if (limits.max_acceleration_rad_s2.has_value()) {
      identity.mix(*limits.max_acceleration_rad_s2);
    }
    identity.mix(
        static_cast<std::int64_t>(limits.max_jerk_rad_s3.has_value()));
    if (limits.max_jerk_rad_s3.has_value()) {
      identity.mix(*limits.max_jerk_rad_s3);
    }
  }
  return std::move(identity).finish();
}

}  // namespace manyforge::core
