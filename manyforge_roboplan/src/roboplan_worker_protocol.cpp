#include "roboplan_worker_protocol.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend::worker_protocol {
namespace {

constexpr std::size_t kHardMaximumCodecBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumStringBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumVectorElements = 1'000'000U;
constexpr std::size_t kMaximumMapElements = 4096U;

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("RoboPlan worker protocol: " +
                              std::move(message));
}

class Encoder final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      bytes_.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      bytes_.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
  }

  void floating(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

  void boolean(bool value) { u8(value ? 1U : 0U); }

  void string(std::string_view value) {
    if (value.size() > kMaximumStringBytes ||
        value.size() > std::numeric_limits<std::uint32_t>::max() ||
        bytes_.size() > kHardMaximumCodecBytes - value.size() - 4U) {
      fail("encoded string exceeds its byte bound");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    bytes_.append(value);
  }

  template <typename Value, typename Write>
  void vector(const std::vector<Value>& values, Write write) {
    if (values.size() > kMaximumVectorElements ||
        values.size() > std::numeric_limits<std::uint32_t>::max()) {
      fail("encoded vector exceeds its element bound");
    }
    u32(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) {
      write(*this, value);
    }
  }

  template <typename Value, typename Write>
  void optional(const std::optional<Value>& value, Write write) {
    boolean(value.has_value());
    if (value.has_value()) {
      write(*this, *value);
    }
  }

  [[nodiscard]] std::string finish() && {
    if (bytes_.size() > kHardMaximumCodecBytes) {
      fail("encoded message exceeds its hard byte bound");
    }
    return std::move(bytes_);
  }

 private:
  std::string bytes_;
};

class Decoder final {
 public:
  explicit Decoder(std::string_view bytes) : bytes_(bytes) {
    if (bytes.size() > kHardMaximumCodecBytes) {
      fail("message exceeds its hard byte bound");
    }
  }

  [[nodiscard]] std::uint8_t u8() {
    require(1U);
    return static_cast<std::uint8_t>(bytes_[offset_++]);
  }

  [[nodiscard]] std::uint32_t u32() {
    require(4U);
    std::uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      value |= static_cast<std::uint32_t>(
                   static_cast<unsigned char>(bytes_[offset_++]))
               << shift;
    }
    return value;
  }

  [[nodiscard]] std::uint64_t u64() {
    require(8U);
    std::uint64_t value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      value |= static_cast<std::uint64_t>(
                   static_cast<unsigned char>(bytes_[offset_++]))
               << shift;
    }
    return value;
  }

  [[nodiscard]] double floating() {
    return std::bit_cast<double>(u64());
  }

  [[nodiscard]] bool boolean() {
    const auto value = u8();
    if (value > 1U) {
      fail("boolean tag is invalid");
    }
    return value != 0U;
  }

  [[nodiscard]] std::string string() {
    const auto size = static_cast<std::size_t>(u32());
    if (size > kMaximumStringBytes) {
      fail("decoded string exceeds its byte bound");
    }
    require(size);
    std::string value(bytes_.substr(offset_, size));
    offset_ += size;
    return value;
  }

  template <typename Value, typename Read>
  [[nodiscard]] std::vector<Value> vector(Read read) {
    const auto count = static_cast<std::size_t>(u32());
    if (count > kMaximumVectorElements) {
      fail("decoded vector exceeds its element bound");
    }
    std::vector<Value> values;
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      values.push_back(read(*this));
    }
    return values;
  }

  template <typename Value, typename Read>
  [[nodiscard]] std::optional<Value> optional(Read read) {
    if (!boolean()) {
      return std::nullopt;
    }
    return read(*this);
  }

  void require_end() const {
    if (offset_ != bytes_.size()) {
      fail("message contains trailing bytes");
    }
  }

 private:
  void require(std::size_t size) const {
    if (size > bytes_.size() - offset_) {
      fail("message is truncated");
    }
  }

  std::string_view bytes_;
  std::size_t offset_{};
};

template <typename Strong>
void write_strong(Encoder& encoder, const Strong& value) {
  encoder.string(value.value());
}

template <typename Strong>
[[nodiscard]] Strong read_strong(Decoder& decoder) {
  return Strong(decoder.string());
}

void write_pose(Encoder& encoder, const core::Pose3DTO& value) {
  encoder.floating(value.position_m.x_m);
  encoder.floating(value.position_m.y_m);
  encoder.floating(value.position_m.z_m);
  encoder.floating(value.orientation.x);
  encoder.floating(value.orientation.y);
  encoder.floating(value.orientation.z);
  encoder.floating(value.orientation.w);
}

[[nodiscard]] core::Pose3DTO read_pose(Decoder& decoder) {
  return {{decoder.floating(), decoder.floating(), decoder.floating()},
          {decoder.floating(), decoder.floating(), decoder.floating(),
           decoder.floating()}};
}

void write_frame(Encoder& encoder, const core::ResolvedFrameDTO& frame) {
  std::visit(
      [&encoder](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, core::ResolvedCellFrameDTO>) {
          encoder.u8(0U);
          write_strong(encoder, value.cell_id);
          encoder.string(value.compiled_frame_name);
        } else if constexpr (
            std::is_same_v<Value, core::ResolvedComponentModelFrameDTO>) {
          encoder.u8(1U);
          write_strong(encoder, value.controllable_component_id);
          write_strong(encoder, value.model_instance_id);
          write_strong(encoder, value.frame_role_id);
          encoder.string(value.compiled_frame_name);
          write_strong(encoder, value.model_revision);
        } else {
          encoder.u8(2U);
          write_strong(encoder, value.model_instance_id);
          write_strong(encoder, value.frame_role_id);
          encoder.string(value.compiled_frame_name);
          write_strong(encoder, value.model_revision);
        }
      },
      frame);
}

[[nodiscard]] core::ResolvedFrameDTO read_frame(Decoder& decoder) {
  switch (decoder.u8()) {
    case 0U:
      return core::ResolvedCellFrameDTO{
          read_strong<core::CellId>(decoder), decoder.string()};
    case 1U:
      return core::ResolvedComponentModelFrameDTO{
          read_strong<core::ControllableComponentId>(decoder),
          read_strong<core::ModelInstanceId>(decoder),
          read_strong<core::FrameRoleId>(decoder), decoder.string(),
          read_strong<core::ModelRevision>(decoder)};
    case 2U:
      return core::ResolvedPassiveModelFrameDTO{
          read_strong<core::ModelInstanceId>(decoder),
          read_strong<core::FrameRoleId>(decoder), decoder.string(),
          read_strong<core::ModelRevision>(decoder)};
    default:
      fail("resolved-frame tag is invalid");
  }
}

void write_string_vector(Encoder& encoder,
                         const std::vector<std::string>& values) {
  encoder.vector(values, [](Encoder& out, const std::string& value) {
    out.string(value);
  });
}

[[nodiscard]] std::vector<std::string> read_string_vector(Decoder& decoder) {
  return decoder.vector<std::string>(
      [](Decoder& input) { return input.string(); });
}

template <typename Strong>
void write_strong_vector(Encoder& encoder,
                         const std::vector<Strong>& values) {
  encoder.vector(values, [](Encoder& out, const Strong& value) {
    write_strong(out, value);
  });
}

template <typename Strong>
[[nodiscard]] std::vector<Strong> read_strong_vector(Decoder& decoder) {
  return decoder.vector<Strong>(
      [](Decoder& input) { return read_strong<Strong>(input); });
}

void write_double_vector(Encoder& encoder,
                         const std::vector<double>& values) {
  encoder.vector(values,
                 [](Encoder& out, double value) { out.floating(value); });
}

[[nodiscard]] std::vector<double> read_double_vector(Decoder& decoder) {
  return decoder.vector<double>(
      [](Decoder& input) { return input.floating(); });
}

void write_controlled_state(Encoder& encoder,
                            const core::ControlledStateDTO& value) {
  write_strong(encoder, value.controllable_component_id);
  write_strong(encoder, value.command_target_id);
  write_strong(encoder, value.control_domain_id);
  write_strong(encoder, value.control_domain_revision);
  write_strong(encoder, value.control_binding_id);
  write_strong(encoder, value.control_binding_revision);
  write_strong(encoder, value.controller_topology_id);
  write_strong(encoder, value.topology_revision);
  write_strong(encoder, value.control_route_id);
  write_strong(encoder, value.active_control_configuration_id);
  encoder.u64(value.active_control_configuration_generation.value());
  write_strong_vector(encoder, value.resources);
  write_string_vector(encoder, value.joint_names);
  encoder.vector(
      value.positions,
      [](Encoder& out, const core::CoordinatePositionDTO& position) {
        if (const auto* angular =
                std::get_if<core::AngularPositionDTO>(&position)) {
          out.u8(0U);
          out.floating(angular->value_rad);
        } else {
          out.u8(1U);
          out.floating(std::get<core::LinearPositionDTO>(position).value_m);
        }
      });
  encoder.optional(
      value.velocities,
      [](Encoder& out,
         const std::vector<core::CoordinateVelocityDTO>& velocities) {
        out.vector(
            velocities,
            [](Encoder& nested,
               const core::CoordinateVelocityDTO& velocity) {
              if (const auto* angular =
                      std::get_if<core::AngularVelocityDTO>(&velocity)) {
                nested.u8(0U);
                nested.floating(angular->value_rad_s);
              } else {
                nested.u8(1U);
                nested.floating(
                    std::get<core::LinearVelocityDTO>(velocity).value_m_s);
              }
            });
      });
}

[[nodiscard]] core::ControlledStateDTO read_controlled_state(
    Decoder& decoder) {
  core::ControlledStateDTO value{
      read_strong<core::ControllableComponentId>(decoder),
      read_strong<core::CommandTargetId>(decoder),
      read_strong<core::ControlDomainId>(decoder),
      read_strong<core::ControlDomainRevision>(decoder),
      read_strong<core::ControlBindingId>(decoder),
      read_strong<core::ControlBindingRevision>(decoder),
      read_strong<core::ControllerTopologyId>(decoder),
      read_strong<core::TopologyRevision>(decoder),
      read_strong<core::ControlRouteId>(decoder),
      read_strong<core::ActiveControlConfigurationId>(decoder),
      core::ActiveControlConfigurationGeneration(decoder.u64()),
      read_strong_vector<core::ResourceId>(decoder),
      read_string_vector(decoder),
      {},
      std::nullopt};
  value.positions = decoder.vector<core::CoordinatePositionDTO>(
      [](Decoder& input) -> core::CoordinatePositionDTO {
        const auto tag = input.u8();
        if (tag == 0U) {
          return core::AngularPositionDTO{input.floating()};
        }
        if (tag == 1U) {
          return core::LinearPositionDTO{input.floating()};
        }
        fail("coordinate-position tag is invalid");
      });
  value.velocities =
      decoder.optional<std::vector<core::CoordinateVelocityDTO>>(
          [](Decoder& input) {
            return input.vector<core::CoordinateVelocityDTO>(
                [](Decoder& nested) -> core::CoordinateVelocityDTO {
                  const auto tag = nested.u8();
                  if (tag == 0U) {
                    return core::AngularVelocityDTO{nested.floating()};
                  }
                  if (tag == 1U) {
                    return core::LinearVelocityDTO{nested.floating()};
                  }
                  fail("coordinate-velocity tag is invalid");
                });
          });
  return value;
}

void write_motion(Encoder& encoder,
                  const core::ResolvedMotionSpecDTO& motion) {
  std::visit(
      [&encoder](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, core::ResolvedJointGoalDTO>) {
          encoder.u8(0U);
          write_strong_vector(encoder, value.resources);
          write_string_vector(encoder, value.joint_names);
          write_double_vector(encoder, value.positions_rad);
        } else if constexpr (std::is_same_v<Value, core::PoseGoalDTO>) {
          encoder.u8(1U);
          write_pose(encoder, value.goal);
        } else if constexpr (std::is_same_v<Value, core::NamedTargetDTO>) {
          encoder.u8(2U);
          encoder.string(value.name);
        } else {
          encoder.u8(3U);
          encoder.vector(value.waypoints,
                         [](Encoder& out, const core::Pose3DTO& pose) {
                           write_pose(out, pose);
                         });
          encoder.floating(value.max_translation_step_m);
          encoder.floating(value.max_rotation_step_rad);
          encoder.u8(static_cast<std::uint8_t>(value.speed_mode));
          encoder.floating(value.max_angular_speed_rad_s);
          encoder.floating(value.max_linear_acceleration_mps2);
          encoder.floating(value.max_angular_acceleration_rad_s2);
          encoder.u8(static_cast<std::uint8_t>(value.fallback_policy));
          encoder.floating(value.max_blend_deviation_rad);
        }
      },
      motion);
}

[[nodiscard]] core::ResolvedMotionSpecDTO read_motion(Decoder& decoder) {
  switch (decoder.u8()) {
    case 0U:
      return core::ResolvedJointGoalDTO{
          read_strong_vector<core::ResourceId>(decoder),
          read_string_vector(decoder), read_double_vector(decoder)};
    case 1U:
      return core::PoseGoalDTO{read_pose(decoder)};
    case 2U:
      return core::NamedTargetDTO{decoder.string()};
    case 3U: {
      core::CartesianPathDTO value;
      value.waypoints = decoder.vector<core::Pose3DTO>(
          [](Decoder& input) { return read_pose(input); });
      value.max_translation_step_m = decoder.floating();
      value.max_rotation_step_rad = decoder.floating();
      const auto speed_mode = decoder.u8();
      if (speed_mode != static_cast<std::uint8_t>(
                            core::CartesianSpeedModeDTO::kBounded)) {
        fail("Cartesian speed-mode tag is invalid");
      }
      value.speed_mode = core::CartesianSpeedModeDTO::kBounded;
      value.max_angular_speed_rad_s = decoder.floating();
      value.max_linear_acceleration_mps2 = decoder.floating();
      value.max_angular_acceleration_rad_s2 = decoder.floating();
      const auto fallback = decoder.u8();
      if (fallback > static_cast<std::uint8_t>(
                         core::CartesianFallbackPolicyDTO::
                             kAllowJointSpaceFallback)) {
        fail("Cartesian fallback-policy tag is invalid");
      }
      value.fallback_policy =
          static_cast<core::CartesianFallbackPolicyDTO>(fallback);
      value.max_blend_deviation_rad = decoder.floating();
      return value;
    }
    default:
      fail("resolved-motion tag is invalid");
  }
}

void write_optional_double(Encoder& encoder,
                           const std::optional<double>& value) {
  encoder.optional(value,
                   [](Encoder& out, double item) { out.floating(item); });
}

[[nodiscard]] std::optional<double> read_optional_double(Decoder& decoder) {
  return decoder.optional<double>(
      [](Decoder& input) { return input.floating(); });
}

void write_dynamics(Encoder& encoder,
                    const core::PerResourceDynamicsLimitsDTO& values) {
  encoder.vector(
      values,
      [](Encoder& out,
         const core::ResolvedResourceDynamicsLimitDTO& value) {
        write_strong(out, value.resource_id);
        out.string(value.compiled_joint_name);
        write_optional_double(out, value.limit.max_velocity_rad_s);
        write_optional_double(out, value.limit.max_acceleration_rad_s2);
        write_optional_double(out, value.limit.max_jerk_rad_s3);
      });
}

[[nodiscard]] core::PerResourceDynamicsLimitsDTO read_dynamics(
    Decoder& decoder) {
  return decoder.vector<core::ResolvedResourceDynamicsLimitDTO>(
      [](Decoder& input) {
        return core::ResolvedResourceDynamicsLimitDTO{
            read_strong<core::ResourceId>(input), input.string(),
            core::JointDynamicsLimitDTO{
                read_optional_double(input), read_optional_double(input),
                read_optional_double(input)}};
      });
}

void write_action_span(Encoder& encoder, const core::ActionSpanDTO& value) {
  encoder.u8(static_cast<std::uint8_t>(value.action_source));
  encoder.string(value.action_span_id);
  encoder.string(value.program_run_id);
  encoder.string(value.node_instance_id);
  encoder.string(value.node_attempt_id);
  encoder.string(value.parent_node_attempt_id);
}

[[nodiscard]] core::ActionSpanDTO read_action_span(Decoder& decoder) {
  const auto source = decoder.u8();
  if (source > static_cast<std::uint8_t>(core::ActionSource::kSystem)) {
    fail("action-source tag is invalid");
  }
  return core::ActionSpanDTO{
      static_cast<core::ActionSource>(source), decoder.string(),
      decoder.string(), decoder.string(), decoder.string(), decoder.string()};
}

void write_binding(Encoder& encoder,
                   const RoboPlanCandidateBindingDTO& value) {
  write_strong(encoder, value.controllable_component_id);
  write_strong(encoder, value.command_target_id);
  write_strong(encoder, value.model_instance_id);
  write_strong(encoder, value.model_revision);
  encoder.string(value.compiled_group_name);
  write_strong_vector(encoder, value.resources);
  write_string_vector(encoder, value.compiled_joint_names);
  encoder.vector(value.permitted_frames, write_frame);
}

[[nodiscard]] RoboPlanCandidateBindingDTO read_binding(Decoder& decoder) {
  return RoboPlanCandidateBindingDTO{
      read_strong<core::ControllableComponentId>(decoder),
      read_strong<core::CommandTargetId>(decoder),
      read_strong<core::ModelInstanceId>(decoder),
      read_strong<core::ModelRevision>(decoder), decoder.string(),
      read_strong_vector<core::ResourceId>(decoder),
      read_string_vector(decoder),
      decoder.vector<core::ResolvedFrameDTO>(read_frame)};
}

void write_shape(Encoder& encoder, const core::CollisionShapeDTO& shape) {
  std::visit(
      [&encoder](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, core::BoxShapeDTO>) {
          encoder.u8(0U);
          encoder.floating(value.dimensions_m.x_m);
          encoder.floating(value.dimensions_m.y_m);
          encoder.floating(value.dimensions_m.z_m);
        } else if constexpr (std::is_same_v<Value, core::SphereShapeDTO>) {
          encoder.u8(1U);
          encoder.floating(value.radius_m);
        } else if constexpr (std::is_same_v<Value, core::CylinderShapeDTO>) {
          encoder.u8(2U);
          encoder.floating(value.radius_m);
          encoder.floating(value.height_m);
        } else {
          encoder.u8(3U);
          encoder.string(value.resource_uri);
          encoder.floating(value.scale.x_m);
          encoder.floating(value.scale.y_m);
          encoder.floating(value.scale.z_m);
        }
      },
      shape);
}

[[nodiscard]] core::CollisionShapeDTO read_shape(Decoder& decoder) {
  switch (decoder.u8()) {
    case 0U:
      return core::BoxShapeDTO{{decoder.floating(), decoder.floating(),
                                decoder.floating()}};
    case 1U:
      return core::SphereShapeDTO{decoder.floating()};
    case 2U:
      return core::CylinderShapeDTO{decoder.floating(), decoder.floating()};
    case 3U:
      return core::MeshShapeDTO{
          decoder.string(),
          {decoder.floating(), decoder.floating(), decoder.floating()}};
    default:
      fail("collision-shape tag is invalid");
  }
}

void write_error(Encoder& encoder, const core::ErrorDTO& value) {
  encoder.string(value.code);
  encoder.string(value.message);
  if (value.details.size() > kMaximumMapElements) {
    fail("error details exceed their element bound");
  }
  encoder.u32(static_cast<std::uint32_t>(value.details.size()));
  for (const auto& [key, detail] : value.details) {
    encoder.string(key);
    encoder.string(detail);
  }
}

[[nodiscard]] core::ErrorDTO read_error(Decoder& decoder) {
  core::ErrorDTO value{decoder.string(), decoder.string(), {}};
  const auto count = static_cast<std::size_t>(decoder.u32());
  if (count > kMaximumMapElements) {
    fail("error details exceed their element bound");
  }
  for (std::size_t index = 0U; index < count; ++index) {
    auto key = decoder.string();
    auto detail = decoder.string();
    if (!value.details.emplace(std::move(key), std::move(detail)).second) {
      fail("error details repeat a key");
    }
  }
  return value;
}

void write_trajectory(Encoder& encoder, const core::TrajectoryDTO& value) {
  write_strong_vector(encoder, value.resources);
  write_string_vector(encoder, value.joint_names);
  encoder.vector(
      value.waypoints,
      [](Encoder& out, const core::TrajectoryWaypointDTO& waypoint) {
        out.floating(waypoint.time_from_start_s);
        write_double_vector(out, waypoint.positions_rad);
        write_double_vector(out, waypoint.velocities_rad_s);
        write_double_vector(out, waypoint.accelerations_rad_s2);
      });
  encoder.optional(value.runtime_scale_bounds,
                   [](Encoder &out, const auto &bounds) {
                     out.floating(bounds.max_rate_per_s);
                     out.floating(bounds.max_rate_change_per_s2);
                     out.floating(bounds.max_stop_path_rate);
                     out.optional(bounds.nominal_time_multiplier,
                                  [](Encoder& nested, double multiplier) {
                                    nested.floating(multiplier);
                                  });
                   });
  write_double_vector(encoder, value.cartesian_waypoint_times_s);
  encoder.optional(value.cartesian_fidelity_metrics,
                   [](Encoder& out, const auto& metrics) {
                     out.floating(metrics.requested_translation_tolerance_m);
                     out.floating(metrics.requested_rotation_tolerance_rad);
                     out.floating(metrics.measured_max_translation_error_m);
                     out.floating(metrics.measured_max_rotation_error_rad);
                     out.floating(metrics.unsampled_translation_bound_m);
                     out.floating(metrics.unsampled_rotation_bound_rad);
                     out.floating(metrics.certified_max_translation_error_m);
                     out.floating(metrics.certified_max_rotation_error_rad);
                     out.floating(metrics.requested_max_blend_deviation_rad);
                     out.floating(metrics.accepted_max_blend_deviation_rad);
                     out.u32(metrics.planning_attempt_count);
                   });
}

[[nodiscard]] core::TrajectoryDTO read_trajectory(Decoder& decoder) {
  core::TrajectoryDTO value;
  value.resources = read_strong_vector<core::ResourceId>(decoder);
  value.joint_names = read_string_vector(decoder);
  value.waypoints = decoder.vector<core::TrajectoryWaypointDTO>(
      [](Decoder& input) {
        return core::TrajectoryWaypointDTO{
            input.floating(), read_double_vector(input),
            read_double_vector(input), read_double_vector(input)};
      });
  value.runtime_scale_bounds =
      decoder.optional<core::TrajectoryRuntimeScaleBoundsDTO>(
          [](Decoder &input) {
            return core::TrajectoryRuntimeScaleBoundsDTO{input.floating(),
                                                         input.floating(), input.floating(),
                                                         input.optional<double>([](Decoder& nested) {
                                                           return nested.floating();
                                                         })};
          });
  value.cartesian_waypoint_times_s = read_double_vector(decoder);
  value.cartesian_fidelity_metrics =
      decoder.optional<core::CartesianFidelityMetricsDTO>([](Decoder& input) {
        return core::CartesianFidelityMetricsDTO{
            input.floating(), input.floating(), input.floating(),
            input.floating(), input.floating(), input.floating(),
            input.floating(), input.floating(), input.floating(),
            input.floating(), input.u32()};
      });
  return value;
}

}  // namespace

std::string encode_initialization(const WorkerInitializationDTO& value) {
  Encoder encoder;
  encoder.string(value.model.native_scene_name);
  encoder.string(value.model.urdf_path.string());
  encoder.string(value.model.srdf_path.string());
  encoder.vector(
      value.model.package_paths,
      [](Encoder& out, const std::filesystem::path& path) {
        out.string(path.string());
      });
  encoder.vector(
      value.model.joint_limits,
      [](Encoder& out, const RoboPlanJointLimitInput& row) {
        out.string(row.joint_name);
        const auto write_values = [](Encoder& nested,
                                     const std::vector<double>& values) {
          write_double_vector(nested, values);
        };
        out.optional(row.minimum_position, write_values);
        out.optional(row.maximum_position, write_values);
        write_double_vector(out, row.maximum_velocity);
        out.optional(row.maximum_acceleration, write_values);
      });
  encoder.vector(
      value.model.geometries,
      [](Encoder& out, const RoboPlanWorkerGeometryDTO& geometry) {
        out.string(geometry.native_geometry_name);
        out.string(geometry.compiled_parent_frame);
        write_shape(out, geometry.shape);
        write_pose(out, geometry.pose_in_parent);
        out.optional(
            geometry.resolved_mesh_path,
            [](Encoder& nested, const std::filesystem::path& path) {
              nested.string(path.string());
            });
      });
  encoder.vector(
      value.model.allowed_collision_pairs,
      [](Encoder& out, const std::pair<std::string, std::string>& pair) {
        out.string(pair.first);
        out.string(pair.second);
      });
  write_binding(encoder, value.binding);
  encoder.optional(
      value.candidate_config.rng_seed,
      [](Encoder& out, std::uint32_t seed) { out.u32(seed); });
  encoder.u64(value.candidate_config.maximum_trajectory_samples);
  encoder.floating(value.candidate_config.trajectory_sample_period_s);
  return std::move(encoder).finish();
}

WorkerInitializationDTO decode_initialization(std::string_view bytes) {
  Decoder decoder(bytes);
  RoboPlanWorkerModelDTO model;
  model.native_scene_name = decoder.string();
  model.urdf_path = decoder.string();
  model.srdf_path = decoder.string();
  model.package_paths = decoder.vector<std::filesystem::path>(
      [](Decoder& input) { return std::filesystem::path(input.string()); });
  model.joint_limits = decoder.vector<RoboPlanJointLimitInput>(
      [](Decoder& input) {
        return RoboPlanJointLimitInput{
            input.string(),
            input.optional<std::vector<double>>(
                [](Decoder& nested) { return read_double_vector(nested); }),
            input.optional<std::vector<double>>(
                [](Decoder& nested) { return read_double_vector(nested); }),
            read_double_vector(input),
            input.optional<std::vector<double>>(
                [](Decoder& nested) { return read_double_vector(nested); })};
      });
  model.geometries = decoder.vector<RoboPlanWorkerGeometryDTO>(
      [](Decoder& input) {
        return RoboPlanWorkerGeometryDTO{
            input.string(), input.string(), read_shape(input),
            read_pose(input), input.optional<std::filesystem::path>(
                                  [](Decoder& nested) {
                                    return std::filesystem::path(
                                        nested.string());
                                  })};
      });
  model.allowed_collision_pairs =
      decoder.vector<std::pair<std::string, std::string>>(
          [](Decoder& input) {
            return std::pair<std::string, std::string>{input.string(),
                                                       input.string()};
          });
  auto binding = read_binding(decoder);
  RoboPlanCandidateComputerConfig candidate_config;
  candidate_config.rng_seed = decoder.optional<std::uint32_t>(
      [](Decoder& input) { return input.u32(); });
  const auto maximum_samples = decoder.u64();
  if (maximum_samples > std::numeric_limits<std::size_t>::max()) {
    fail("trajectory sample bound overflows size_t");
  }
  candidate_config.maximum_trajectory_samples =
      static_cast<std::size_t>(maximum_samples);
  candidate_config.trajectory_sample_period_s = decoder.floating();
  decoder.require_end();
  return WorkerInitializationDTO{std::move(model), std::move(binding),
                                 std::move(candidate_config)};
}

std::string encode_candidate_request(
    const RoboPlanCandidateRequestDTO& value) {
  Encoder encoder;
  write_strong(encoder, value.controllable_component_id);
  write_strong(encoder, value.command_target_id);
  write_strong(encoder, value.model_instance_id);
  write_strong(encoder, value.model_revision);
  write_strong_vector(encoder, value.resources);
  write_frame(encoder, value.base_frame);
  write_frame(encoder, value.tcp_frame);
  write_controlled_state(encoder, value.start_state);
  write_motion(encoder, value.motion);
  encoder.floating(value.tolerances.joint_position_tol_rad);
  encoder.floating(value.tolerances.translation_tol_m);
  encoder.floating(value.tolerances.rotation_tol_rad);
  encoder.floating(value.speed_profile.velocity_scale);
  encoder.floating(value.speed_profile.acceleration_scale);
  write_optional_double(encoder,
                        value.speed_profile.max_tcp_linear_speed_mps);
  encoder.u8(static_cast<std::uint8_t>(value.planning_profile));
  write_dynamics(encoder, value.dynamics_limits);
  encoder.floating(value.planning_timeout_s);
  encoder.u32(value.planning_attempt_index);
  encoder.optional(value.action_span, write_action_span);
  return std::move(encoder).finish();
}

RoboPlanCandidateRequestDTO decode_candidate_request(
    std::string_view bytes) {
  Decoder decoder(bytes);
  RoboPlanCandidateRequestDTO value{
      read_strong<core::ControllableComponentId>(decoder),
      read_strong<core::CommandTargetId>(decoder),
      read_strong<core::ModelInstanceId>(decoder),
      read_strong<core::ModelRevision>(decoder),
      read_strong_vector<core::ResourceId>(decoder), read_frame(decoder),
      read_frame(decoder), read_controlled_state(decoder),
      read_motion(decoder),
      core::TolerancesDTO{decoder.floating(), decoder.floating(),
                          decoder.floating()},
      core::SpeedProfileDTO{}, core::PlanningProfileDTO::kFirstFeasible,
      {}, 0.0, 0U, std::nullopt};
  value.speed_profile.velocity_scale = decoder.floating();
  value.speed_profile.acceleration_scale = decoder.floating();
  value.speed_profile.max_tcp_linear_speed_mps =
      read_optional_double(decoder);
  const auto profile = decoder.u8();
  if (profile >
      static_cast<std::uint8_t>(core::PlanningProfileDTO::kQualityBounded)) {
    fail("planning-profile tag is invalid");
  }
  value.planning_profile = static_cast<core::PlanningProfileDTO>(profile);
  value.dynamics_limits = read_dynamics(decoder);
  value.planning_timeout_s = decoder.floating();
  value.planning_attempt_index = decoder.u32();
  value.action_span =
      decoder.optional<core::ActionSpanDTO>(read_action_span);
  decoder.require_end();
  return value;
}

std::string encode_trajectory_result(
    const core::Result<core::TrajectoryDTO>& value) {
  Encoder encoder;
  encoder.boolean(value.ok());
  if (value.ok()) {
    write_trajectory(encoder, value.value());
  } else {
    write_error(encoder, value.error());
  }
  return std::move(encoder).finish();
}

core::Result<core::TrajectoryDTO> decode_trajectory_result(
    std::string_view bytes) {
  Decoder decoder(bytes);
  const bool ok = decoder.boolean();
  if (ok) {
    auto value = read_trajectory(decoder);
    decoder.require_end();
    return core::Result<core::TrajectoryDTO>::Ok(std::move(value));
  }
  auto value = read_error(decoder);
  decoder.require_end();
  return core::Result<core::TrajectoryDTO>::Err(std::move(value));
}

std::string encode_ik_request(const WorkerIkRequestDTO& value) {
  Encoder encoder;
  write_pose(encoder, value.goal);
  write_frame(encoder, value.base_frame);
  write_frame(encoder, value.tcp_frame);
  write_controlled_state(encoder, value.seed_state);
  encoder.floating(value.timeout_s);
  return std::move(encoder).finish();
}

WorkerIkRequestDTO decode_ik_request(std::string_view bytes) {
  Decoder decoder(bytes);
  WorkerIkRequestDTO value{read_pose(decoder),
                           read_frame(decoder), read_frame(decoder),
                           read_controlled_state(decoder),
                           decoder.floating()};
  decoder.require_end();
  return value;
}

std::string encode_ik_result(
    const core::Result<std::vector<double>>& value) {
  Encoder encoder;
  encoder.boolean(value.ok());
  if (value.ok()) {
    write_double_vector(encoder, value.value());
  } else {
    write_error(encoder, value.error());
  }
  return std::move(encoder).finish();
}

core::Result<std::vector<double>> decode_ik_result(
    std::string_view bytes) {
  Decoder decoder(bytes);
  const bool ok = decoder.boolean();
  if (ok) {
    auto value = read_double_vector(decoder);
    decoder.require_end();
    return core::Result<std::vector<double>>::Ok(std::move(value));
  }
  auto value = read_error(decoder);
  decoder.require_end();
  return core::Result<std::vector<double>>::Err(std::move(value));
}

std::string encode_protocol_error(std::string_view detail) {
  Encoder encoder;
  encoder.string(detail);
  return std::move(encoder).finish();
}

std::string decode_protocol_error(std::string_view bytes) {
  Decoder decoder(bytes);
  auto value = decoder.string();
  decoder.require_end();
  return value;
}

}  // namespace manyforge::roboplan_backend::worker_protocol
