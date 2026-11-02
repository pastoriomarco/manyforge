#include <algorithm>
#include <cstdint>
#include <cstring>
#include <manyforge_core/cache/trajectory_cache_intent.hpp>
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
  void append_byte(unsigned char value) { bytes_.push_back(static_cast<char>(value)); }

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

void mix_reference(IntentIdentityBuilder& identity, const ReferenceDTO& reference) {
  identity.mix(static_cast<std::int64_t>(reference.type))
      .mix(reference_identity_value(reference))
      .mix(static_cast<std::int64_t>(reference.pose.has_value()));
  if (reference.pose.has_value()) {
    mix_pose(identity, *reference.pose);
  }
  identity.mix(static_cast<std::int64_t>(reference.target.has_value()));
  if (reference.target.has_value()) {
    identity.mix(reference.target->target_role_id.value())
        .mix(reference.target->controllable_component_id.value())
        .mix(reference.target->command_target_id.value());
  }
}

void mix_resolved_frame(IntentIdentityBuilder& identity, const ResolvedFrameDTO& frame) {
  std::visit(
      [&identity](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ResolvedCellFrameDTO>) {
          identity.mix("cell_frame").mix(value.cell_id.value()).mix(value.compiled_frame_name);
        } else if constexpr (std::is_same_v<T, ResolvedComponentModelFrameDTO>) {
          identity.mix("component_model_frame")
              .mix(value.controllable_component_id.value())
              .mix(value.model_instance_id.value())
              .mix(value.frame_role_id.value())
              .mix(value.compiled_frame_name)
              .mix(value.model_revision.value());
        } else {
          identity.mix("passive_model_frame")
              .mix(value.model_instance_id.value())
              .mix(value.frame_role_id.value())
              .mix(value.compiled_frame_name)
              .mix(value.model_revision.value());
        }
      },
      frame);
}

void mix_resolved_binding(IntentIdentityBuilder& identity,
                          const ResolvedExecutionBindingDTO& binding) {
  identity.mix(binding.program_id.value())
      .mix(binding.program_revision.value())
      .mix(binding.program_binding_id.value())
      .mix(binding.controllable_component_id.value())
      .mix(binding.component_type_id.value())
      .mix(binding.component_type_revision.value())
      .mix(binding.command_target_id.value())
      .mix(binding.capability_id.value())
      .mix(binding.capability_definition_revision.value())
      .mix(static_cast<std::uint64_t>(binding.model_bindings.size()));
  for (const auto& model : binding.model_bindings) {
    identity.mix(model.model_instance_id.value()).mix(model.model_revision.value());
  }
  const auto& group = binding.model_group_binding;
  identity.mix(group.model_group_role_id.value())
      .mix(group.model_instance_id.value())
      .mix(group.model_revision.value())
      .mix(group.compiled_group_name)
      .mix(static_cast<std::uint64_t>(group.compiled_joint_names.size()));
  for (const auto& joint_name : group.compiled_joint_names) {
    identity.mix(joint_name);
  }
  identity.mix(binding.control_domain_id.value())
      .mix(binding.control_domain_revision.value())
      .mix(binding.control_binding_id.value())
      .mix(binding.control_binding_revision.value())
      .mix(binding.control_interface_contract_id.value())
      .mix(binding.control_interface_revision.value())
      .mix(binding.controller_topology_id.value())
      .mix(binding.topology_revision.value())
      .mix(binding.control_route_id.value())
      .mix(binding.activation_set_id.value())
      .mix(binding.active_control_configuration_id.value())
      .mix(binding.active_control_configuration_generation.value())
      .mix(binding.provider_instance_id.value())
      .mix(binding.provider_manifest_id.value())
      .mix(binding.provider_manifest_revision.value())
      .mix(binding.provider_runtime_epoch.value())
      .mix(static_cast<std::uint64_t>(binding.runtime_unit_bindings.size()));
  for (const auto& runtime : binding.runtime_unit_bindings) {
    identity.mix(runtime.runtime_unit_id.value()).mix(runtime.runtime_epoch.value());
  }
  identity.mix(static_cast<std::uint64_t>(binding.resources.size()));
  for (const auto& resource : binding.resources) {
    identity.mix(resource.value());
  }
  mix_resolved_frame(identity, binding.base_frame);
  mix_resolved_frame(identity, binding.tcp_frame);
  identity.mix(static_cast<std::uint64_t>(binding.component_attachment_revisions.size()));
  for (const auto& revision : binding.component_attachment_revisions) {
    identity.mix(revision.value());
  }
  identity.mix(static_cast<std::uint64_t>(binding.carried_object_attachment_revisions.size()));
  for (const auto& revision : binding.carried_object_attachment_revisions) {
    identity.mix(revision.value());
  }
}

void mix_motion(IntentIdentityBuilder& identity, const ResolvedMotionSpecDTO& motion) {
  std::visit(
      [&identity](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ResolvedJointGoalDTO>) {
          const bool duplicate_names = [&value] {
            auto names = value.joint_names;
            std::sort(names.begin(), names.end());
            return std::adjacent_find(names.begin(), names.end()) != names.end();
          }();
          const bool malformed = value.joint_names.size() != value.positions_rad.size() ||
                                 value.resources.size() != value.joint_names.size() ||
                                 duplicate_names;
          if (malformed) {
            // Preserve every raw byte of a malformed request so it can never
            // alias a different malformed request before validation rejects it.
            identity.mix("joint_goal_malformed")
                .mix(static_cast<std::uint64_t>(value.joint_names.size()));
            for (const auto& joint_name : value.joint_names) {
              identity.mix(joint_name);
            }
            identity.mix(static_cast<std::uint64_t>(value.positions_rad.size()));
            for (const auto position : value.positions_rad) {
              identity.mix(position);
            }
            identity.mix(static_cast<std::uint64_t>(value.resources.size()));
            for (const auto& resource : value.resources) {
              identity.mix(resource.value());
            }
            return;
          }
          identity.mix("joint_goal").mix(static_cast<std::uint64_t>(value.joint_names.size()));
          for (std::size_t index = 0; index < value.joint_names.size(); ++index) {
            identity.mix(value.resources[index].value())
                .mix(value.joint_names[index])
                .mix(value.positions_rad[index]);
          }
        } else if constexpr (std::is_same_v<T, PoseGoalDTO>) {
          identity.mix("pose_goal");
          mix_pose(identity, value.goal);
        } else if constexpr (std::is_same_v<T, NamedTargetDTO>) {
          identity.mix("named_target").mix(value.name);
        } else if constexpr (std::is_same_v<T, CartesianPathDTO>) {
          identity.mix("cartesian_path").mix(static_cast<std::uint64_t>(value.waypoints.size()));
          for (const auto& waypoint : value.waypoints) {
            mix_pose(identity, waypoint);
          }
          identity.mix(value.max_translation_step_m)
              .mix(value.max_rotation_step_rad)
              .mix(value.speed_mode ? static_cast<std::int64_t>(*value.speed_mode) : -1)
              .mix(value.max_angular_speed_rad_s)
              .mix(value.max_linear_acceleration_mps2)
              .mix(value.max_angular_acceleration_rad_s2)
              .mix(static_cast<std::int64_t>(value.fallback_policy))
              .mix(static_cast<std::uint64_t>(value.blend_radii_m.size()));
          for (const double radius : value.blend_radii_m) identity.mix(radius);
        }
      },
      motion);
}

}  // namespace

TrajectoryCacheKey trajectory_cache_key(const ExecuteRequestDTO& request) {
  return TrajectoryCacheKey{
      request.binding.program_revision,
      request.binding.program_binding_id,
      request.binding.program_run_id,
      MoveId(request.move_id),
      request.binding.controllable_component_id,
      request.binding.command_target_id,
      request.binding.capability_id,
  };
}

std::string trajectory_cache_intent_identity(const ExecuteRequestDTO& request) {
  IntentIdentityBuilder identity;
  identity.mix("manyforge_core/trajectory_cache_intent/v7");
  mix_resolved_binding(identity, request.binding);
  identity.mix(request.contact_policy_id)
      .mix(static_cast<std::int64_t>(request.target_mode))
      .mix(static_cast<std::int64_t>(request.planner.search.has_value()
                                         ? static_cast<int>(*request.planner.search)
                                         : -1))
      .mix(static_cast<std::int64_t>(request.planner.optimize.has_value()
                                         ? static_cast<int>(*request.planner.optimize)
                                         : -1))
      .mix(static_cast<std::int64_t>(request.planner.n.value_or(0U)))
      .mix(request.planner.collision_check_step_rad.value_or(0.0))
      .mix(static_cast<std::int64_t>(request.planner.max_nodes.value_or(0U)))
      .mix(static_cast<std::int64_t>(request.target_reference.has_value()));
  if (request.target_reference.has_value()) {
    mix_reference(identity, *request.target_reference);
  }
  identity.mix(static_cast<std::int64_t>(request.offset_reference.has_value()));
  if (request.offset_reference.has_value()) {
    mix_reference(identity, *request.offset_reference);
  }
  identity.mix(static_cast<std::int64_t>(request.goal_frame_reference.has_value()));
  if (request.goal_frame_reference.has_value()) {
    mix_reference(identity, *request.goal_frame_reference);
  }
  identity.mix(static_cast<std::uint64_t>(request.runtime_object_bindings.size()));
  for (const auto& runtime_object : request.runtime_object_bindings) {
    identity.mix(runtime_object.runtime_object_id.value())
        .mix(runtime_object.scene_resource_id.value());
  }
  mix_motion(identity, request.motion);
  identity.mix(request.tolerances.joint_position_tol_rad)
      .mix(request.tolerances.translation_tol_m)
      .mix(request.tolerances.rotation_tol_rad)
      .mix(request.speed_profile.velocity_scale)
      .mix(request.speed_profile.acceleration_scale)
      .mix(static_cast<std::int64_t>(request.speed_profile.max_tcp_linear_speed_mps.has_value()));
  if (request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
    identity.mix(*request.speed_profile.max_tcp_linear_speed_mps);
  }
  // Planning budget can change which feasible route is returned. Execution
  // timeout governs only transport after planning and is intentionally absent.
  identity.mix(request.timeouts.planning_timeout_s);
  identity.mix(static_cast<std::uint64_t>(request.per_resource_dynamics_limits.size()));
  for (const auto& limit : request.per_resource_dynamics_limits) {
    const auto& limits = limit.limit;
    identity.mix(limit.resource_id.value())
        .mix(limit.compiled_joint_name)
        .mix(static_cast<std::int64_t>(limits.max_velocity_rad_s.has_value()));
    if (limits.max_velocity_rad_s.has_value()) {
      identity.mix(*limits.max_velocity_rad_s);
    }
    identity.mix(static_cast<std::int64_t>(limits.max_acceleration_rad_s2.has_value()));
    if (limits.max_acceleration_rad_s2.has_value()) {
      identity.mix(*limits.max_acceleration_rad_s2);
    }
    identity.mix(static_cast<std::int64_t>(limits.max_jerk_rad_s3.has_value()));
    if (limits.max_jerk_rad_s3.has_value()) {
      identity.mix(*limits.max_jerk_rad_s3);
    }
  }
  return std::move(identity).finish();
}

}  // namespace manyforge::core
