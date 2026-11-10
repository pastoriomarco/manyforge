#include "roboplan_worker_protocol.hpp"

#include <bit>
#include <cmath>
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

  [[nodiscard]] bool at_end() const noexcept {
    return offset_ == bytes_.size();
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
          encoder.optional(value.speed_mode,
                           [](Encoder& out, core::CartesianSpeedModeDTO mode) {
                             out.u8(static_cast<std::uint8_t>(mode));
                           });
          encoder.floating(value.max_angular_speed_rad_s);
          encoder.floating(value.max_linear_acceleration_mps2);
          encoder.floating(value.max_angular_acceleration_rad_s2);
          encoder.u8(static_cast<std::uint8_t>(value.fallback_policy));
          write_double_vector(encoder, value.blend_radii_m);
        }
      },
      motion);
}

[[nodiscard]] core::CartesianSpeedModeDTO read_cartesian_speed_mode(Decoder& decoder) {
  const auto tag = decoder.u8();
  if (tag > static_cast<std::uint8_t>(core::CartesianSpeedModeDTO::kTimeOptimal)) {
    fail("Cartesian speed-mode tag is invalid");
  }
  return static_cast<core::CartesianSpeedModeDTO>(tag);
}

[[nodiscard]] core::MotionTimingDTO read_motion_timing(Decoder& decoder) {
  const auto tag = decoder.u8();
  if (tag > static_cast<std::uint8_t>(core::MotionTimingDTO::kFast)) {
    fail("motion-timing tag is invalid");
  }
  return static_cast<core::MotionTimingDTO>(tag);
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
      value.speed_mode = decoder.optional<core::CartesianSpeedModeDTO>(
          read_cartesian_speed_mode);
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
      value.blend_radii_m = read_double_vector(decoder);
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

void write_contact_pairs(
    Encoder& encoder,
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  encoder.vector(pairs, [](Encoder& out,
                           const std::pair<std::string, std::string>& pair) {
    out.string(pair.first);
    out.string(pair.second);
  });
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>>
read_contact_pairs(Decoder& decoder) {
  return decoder.vector<std::pair<std::string, std::string>>(
      [](Decoder& input) {
        std::pair<std::string, std::string> pair{input.string(),
                                                 input.string()};
        if (pair.first.empty() || pair.second <= pair.first) {
          fail("contact-allowed pair is invalid");
        }
        return pair;
      });
}

[[nodiscard]] core::PlannerSearchDTO read_planner_search(Decoder& decoder) {
  const auto tag = decoder.u8();
  if (tag > static_cast<std::uint8_t>(core::PlannerSearchDTO::kRrtConnect)) {
    fail("planner search tag is invalid");
  }
  return static_cast<core::PlannerSearchDTO>(tag);
}

[[nodiscard]] core::PlannerOptimizeDTO read_planner_optimize(Decoder& decoder) {
  const auto tag = decoder.u8();
  if (tag > static_cast<std::uint8_t>(core::PlannerOptimizeDTO::kBestOfN)) {
    fail("planner optimize tag is invalid");
  }
  return static_cast<core::PlannerOptimizeDTO>(tag);
}

void write_planner_override(Encoder& encoder,
                            const core::PlannerOverrideDTO& value) {
  encoder.optional(value.search, [](Encoder& out, core::PlannerSearchDTO item) {
    out.u8(static_cast<std::uint8_t>(item));
  });
  encoder.optional(value.optimize,
                   [](Encoder& out, core::PlannerOptimizeDTO item) {
                     out.u8(static_cast<std::uint8_t>(item));
                   });
  encoder.optional(value.n, [](Encoder& out, std::uint32_t item) { out.u32(item); });
  write_optional_double(encoder, value.collision_check_step_rad);
  encoder.optional(value.max_nodes,
                   [](Encoder& out, std::uint32_t item) { out.u32(item); });
}

[[nodiscard]] core::PlannerOverrideDTO read_planner_override(Decoder& decoder) {
  core::PlannerOverrideDTO value;
  value.search = decoder.optional<core::PlannerSearchDTO>(read_planner_search);
  value.optimize =
      decoder.optional<core::PlannerOptimizeDTO>(read_planner_optimize);
  value.n = decoder.optional<std::uint32_t>(
      [](Decoder& input) { return input.u32(); });
  value.collision_check_step_rad = read_optional_double(decoder);
  value.max_nodes = decoder.optional<std::uint32_t>(
      [](Decoder& input) { return input.u32(); });
  return value;
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
            input.floating(), input.floating()};
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
        out.optional(geometry.scene_mesh_content_digest,
                     [](Encoder& nested, const std::string& digest) {
                       nested.string(digest);
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
  encoder.floating(value.candidate_config.collision_check_step_rad);
  encoder.u8(static_cast<std::uint8_t>(value.candidate_config.planner_search));
  encoder.u8(static_cast<std::uint8_t>(value.candidate_config.planner_optimize));
  encoder.u32(value.candidate_config.planner_n);
  encoder.u32(value.candidate_config.planner_max_nodes);
  encoder.u8(static_cast<std::uint8_t>(value.candidate_config.cartesian_timing));
  encoder.u8(static_cast<std::uint8_t>(value.candidate_config.motion_timing));
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
            read_pose(input),
            input.optional<std::filesystem::path>([](Decoder& nested) {
              return std::filesystem::path(nested.string());
            }),
            input.optional<std::string>(
                [](Decoder& nested) { return nested.string(); })};
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
  candidate_config.collision_check_step_rad = decoder.floating();
  candidate_config.planner_search = read_planner_search(decoder);
  candidate_config.planner_optimize = read_planner_optimize(decoder);
  candidate_config.planner_n = decoder.u32();
  candidate_config.planner_max_nodes = decoder.u32();
  candidate_config.cartesian_timing = read_cartesian_speed_mode(decoder);
  candidate_config.motion_timing = read_motion_timing(decoder);
  decoder.require_end();
  return WorkerInitializationDTO{std::move(model), std::move(binding),
                                 std::move(candidate_config)};
}

std::string encode_candidate_request(
    const RoboPlanCandidateRequestDTO& value,
    std::optional<std::int64_t> planning_deadline_ns) {
  if (planning_deadline_ns &&
      (!value.moving_replacement || !value.moving_replacement->remaining_planning_budget_s))
    fail("moving planning deadline needs a planning budget");
  if (value.collision_mode != core::PlannerCollisionMode::kEnforced &&
      value.collision_mode != core::PlannerCollisionMode::kDiagnosticDisabled)
    fail("invalid collision mode");
  const bool diagnostic = value.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled;
  const bool cartesian = std::holds_alternative<core::CartesianPathDTO>(value.motion);
  if ((!diagnostic && value.diagnostic_qualified_target) ||
      (diagnostic && !cartesian && !value.diagnostic_qualified_target) ||
      diagnostic != value.diagnostic_deadline_ns.has_value() ||
      (diagnostic && value.moving_replacement))
    fail("invalid diagnostic plan fields");
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
  write_planner_override(encoder, value.planner);
  write_dynamics(encoder, value.dynamics_limits);
  encoder.floating(value.planning_timeout_s);
  encoder.u32(value.planning_attempt_index);
  encoder.optional(value.action_span, write_action_span);
  encoder.vector(value.collision_pair_margins,
                 [](Encoder& out, const RoboPlanPairMarginDTO& margin) {
                   out.string(margin.first);
                   out.string(margin.second);
                   out.floating(margin.margin_m);
                 });
  write_contact_pairs(encoder, value.contact_allowed_pairs);
  encoder.boolean(value.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled);
  encoder.optional(value.diagnostic_qualified_target, write_controlled_state);
  encoder.optional(value.diagnostic_deadline_ns, [](Encoder& out, std::int64_t stamp) {
    if (stamp <= 0) fail("diagnostic deadline is invalid");
    out.u64(static_cast<std::uint64_t>(stamp));
  });
  // Omit the complete extension when absent, preserving ordinary payloads.
  if (value.moving_replacement) {
    encoder.boolean(true);
    const auto &moving = *value.moving_replacement;
    write_strong(encoder, moving.source_goal.controller_goal_id);
    encoder.u64(moving.source_goal.controller_goal_generation.value());
    encoder.u64(moving.source_binding_epoch.value());
    encoder.floating(moving.source_cursor_s);
    write_trajectory(encoder, moving.source_prefix_trajectory);
    encoder.floating(moving.divergence_cursor_s);
    write_controlled_state(encoder, moving.suffix_start_state);
    write_frame(encoder, moving.source_base_frame);
    write_frame(encoder, moving.source_tcp_frame);
    encoder.floating(moving.source_speed_profile.velocity_scale);
    encoder.floating(moving.source_speed_profile.acceleration_scale);
    write_optional_double(encoder,
                          moving.source_speed_profile.max_tcp_linear_speed_mps);
    write_dynamics(encoder, moving.source_per_resource_dynamics_limits);
    encoder.floating(moving.remaining_preparation_budget_s);
    // Keep legacy moving payloads byte-identical when the new cap is absent.
    if (moving.remaining_planning_budget_s) {
      encoder.boolean(true);
      encoder.floating(*moving.remaining_planning_budget_s);
      encoder.optional(planning_deadline_ns, [](Encoder& out, std::int64_t deadline) {
        if (deadline <= 0) fail("moving planning deadline is invalid");
        out.u64(static_cast<std::uint64_t>(deadline));
      });
    } else if (planning_deadline_ns) {
      fail("moving planning deadline needs a planning budget");
    }
  }
  return std::move(encoder).finish();
}

RoboPlanCandidateRequestDTO decode_candidate_request(
    std::string_view bytes, std::optional<std::int64_t>* planning_deadline_ns) {
  if (planning_deadline_ns) planning_deadline_ns->reset();
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
      core::SpeedProfileDTO{}, core::PlannerOverrideDTO{},
      {}, 0.0, 0U, std::nullopt};
  value.speed_profile.velocity_scale = decoder.floating();
  value.speed_profile.acceleration_scale = decoder.floating();
  value.speed_profile.max_tcp_linear_speed_mps =
      read_optional_double(decoder);
  value.planner = read_planner_override(decoder);
  value.dynamics_limits = read_dynamics(decoder);
  value.planning_timeout_s = decoder.floating();
  value.planning_attempt_index = decoder.u32();
  value.action_span =
      decoder.optional<core::ActionSpanDTO>(read_action_span);
  value.collision_pair_margins = decoder.vector<RoboPlanPairMarginDTO>(
      [](Decoder& input) {
        RoboPlanPairMarginDTO margin{input.string(), input.string(), 0.0};
        margin.margin_m = input.floating();
        if (margin.first.empty() || margin.second <= margin.first ||
            !std::isfinite(margin.margin_m) || margin.margin_m < 0.0) {
          fail("clearance margin pair is invalid");
        }
        return margin;
      });
  value.contact_allowed_pairs = read_contact_pairs(decoder);
  value.collision_mode = decoder.boolean() ? core::PlannerCollisionMode::kDiagnosticDisabled
                                          : core::PlannerCollisionMode::kEnforced;
  value.diagnostic_qualified_target = decoder.optional<core::ControlledStateDTO>(read_controlled_state);
  value.diagnostic_deadline_ns = decoder.optional<std::int64_t>([](Decoder& in) {
    const auto stamp = in.u64();
    if (stamp == 0U || stamp > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      fail("diagnostic deadline is invalid");
    return static_cast<std::int64_t>(stamp);
  });
  const bool diagnostic = value.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled;
  const bool cartesian = std::holds_alternative<core::CartesianPathDTO>(value.motion);
  if ((!diagnostic && value.diagnostic_qualified_target) ||
      (diagnostic && !cartesian && !value.diagnostic_qualified_target) ||
      diagnostic != value.diagnostic_deadline_ns.has_value())
    fail("invalid diagnostic plan fields");
  if (!decoder.at_end()) {
    if (diagnostic) fail("diagnostic plan cannot be a moving replacement");
    if (!decoder.boolean()) {
      fail("absent moving-replacement extension must be omitted");
    }
    value.moving_replacement = core::MovingReplacementInputDTO{
        core::ExecutionGoalKey{read_strong<core::ControllerGoalId>(decoder),
                               core::ControllerGoalGeneration(decoder.u64())},
        core::ExecutionBindingEpoch(decoder.u64()),
        decoder.floating(),
        read_trajectory(decoder),
        decoder.floating(),
        read_controlled_state(decoder),
        read_frame(decoder),
        read_frame(decoder),
        core::SpeedProfileDTO{decoder.floating(), decoder.floating(),
                              read_optional_double(decoder)},
        read_dynamics(decoder),
        decoder.floating()};
    if (!decoder.at_end()) {
      if (!decoder.boolean()) fail("absent moving-planning extension must be omitted");
      value.moving_replacement->remaining_planning_budget_s = decoder.floating();
      const auto deadline = decoder.optional<std::int64_t>([](Decoder& input) {
        const auto stamp = input.u64();
        if (stamp == 0U || stamp > static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max()))
          fail("moving planning deadline is invalid");
        return static_cast<std::int64_t>(stamp);
      });
      if (planning_deadline_ns) *planning_deadline_ns = deadline;
    }
  }
  decoder.require_end();
  return value;
}

std::string encode_trajectory_result(
    const core::Result<core::TrajectoryDTO>& value,
    const std::optional<core::ControlledStateDTO>& qualified_target) {
  Encoder encoder;
  encoder.boolean(value.ok());
  if (value.ok()) write_trajectory(encoder, value.value());
  else write_error(encoder, value.error());
  encoder.optional(qualified_target, write_controlled_state);
  return std::move(encoder).finish();
}

core::Result<core::TrajectoryDTO> decode_trajectory_result(
    std::string_view bytes, std::optional<core::ControlledStateDTO>* qualified_target) {
  Decoder decoder(bytes);
  const bool ok = decoder.boolean();
  if (ok) {
    auto value = read_trajectory(decoder);
    auto target = decoder.optional<core::ControlledStateDTO>(read_controlled_state);
    decoder.require_end();
    if (qualified_target) *qualified_target = std::move(target);
    return core::Result<core::TrajectoryDTO>::Ok(std::move(value));
  }
  auto value = read_error(decoder);
  auto target = decoder.optional<core::ControlledStateDTO>(read_controlled_state);
  decoder.require_end();
  if (qualified_target) *qualified_target = std::move(target);
  return core::Result<core::TrajectoryDTO>::Err(std::move(value));
}

std::string encode_ik_request(const WorkerIkRequestDTO& value) {
  if (value.collision_mode != core::PlannerCollisionMode::kEnforced &&
      value.collision_mode != core::PlannerCollisionMode::kDiagnosticDisabled)
    fail("invalid collision mode");
  const bool diagnostic = value.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled;
  if (diagnostic != value.diagnostic_deadline_ns.has_value() ||
      diagnostic != value.diagnostic_tolerances.has_value())
    fail("invalid diagnostic IK fields");
  Encoder encoder;
  write_pose(encoder, value.goal);
  write_frame(encoder, value.base_frame);
  write_frame(encoder, value.tcp_frame);
  write_controlled_state(encoder, value.seed_state);
  encoder.floating(value.timeout_s);
  write_contact_pairs(encoder, value.contact_allowed_pairs);
  encoder.boolean(value.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled);
  encoder.optional(value.diagnostic_deadline_ns, [](Encoder& out, std::int64_t stamp) {
    if (stamp <= 0) fail("diagnostic deadline is invalid");
    out.u64(static_cast<std::uint64_t>(stamp));
  });
  encoder.optional(value.diagnostic_tolerances, [](Encoder& out, const core::TolerancesDTO& t) {
    out.floating(t.joint_position_tol_rad);
    out.floating(t.translation_tol_m);
    out.floating(t.rotation_tol_rad);
  });
  return std::move(encoder).finish();
}

WorkerIkRequestDTO decode_ik_request(std::string_view bytes) {
  Decoder decoder(bytes);
  WorkerIkRequestDTO value{read_pose(decoder),
                           read_frame(decoder), read_frame(decoder),
                           read_controlled_state(decoder),
                           decoder.floating(),
                           read_contact_pairs(decoder)};
  value.collision_mode = decoder.boolean() ? core::PlannerCollisionMode::kDiagnosticDisabled
                                          : core::PlannerCollisionMode::kEnforced;
  value.diagnostic_deadline_ns = decoder.optional<std::int64_t>([](Decoder& in) {
    const auto stamp = in.u64();
    if (stamp == 0U || stamp > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      fail("diagnostic deadline is invalid");
    return static_cast<std::int64_t>(stamp);
  });
  value.diagnostic_tolerances = decoder.optional<core::TolerancesDTO>([](Decoder& in) {
    return core::TolerancesDTO{in.floating(), in.floating(), in.floating()};
  });
  const bool diagnostic = value.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled;
  if (diagnostic != value.diagnostic_deadline_ns.has_value() ||
      diagnostic != value.diagnostic_tolerances.has_value())
    fail("invalid diagnostic IK fields");
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
