#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {
using content_identity::CanonicalValue;
constexpr std::size_t kMaxCoordinates = 4096U;
constexpr std::size_t kMaxWaypoints = 1'000'000U;

class Invalid final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};
[[noreturn]] void fail(std::string reason) {
  throw Invalid(std::move(reason));
}
template <class T>
Result<T> rejected(std::string reason) {
  return Result<T>::Err(ErrorDTO{"prepared_planning_value_invalid",
                                 "prepared planning value is not exact and closed",
                                 {{"reason", std::move(reason)}}});
}
const CanonicalValue::Object& object(const CanonicalValue& value) {
  if (!value.is_object()) fail("object_required");
  return value.as_object();
}
const CanonicalValue& member(const CanonicalValue& value, std::string_view name) {
  const auto* result = value.find_object_member(name);
  if (result == nullptr) fail("required_member_missing");
  return *result;
}
void exact(const CanonicalValue& value, std::initializer_list<std::string_view> names) {
  const auto& fields = object(value);
  if (fields.size() != names.size()) fail("unknown_or_missing_member");
  for (const auto name : names) {
    static_cast<void>(member(value, name));
  }
}
std::string text(const CanonicalValue& value) {
  if (!value.is_string()) fail("string_required");
  return value.as_string();
}
double number(const CanonicalValue& value) {
  if (!value.is_number() || !std::isfinite(value.as_number())) fail("finite_number_required");
  return value.as_number();
}
std::uint64_t safe_unsigned(const CanonicalValue& value, bool nonzero = false) {
  const auto item = number(value);
  if (item < (nonzero ? 1.0 : 0.0) || item > 9'007'199'254'740'991.0 || std::floor(item) != item) {
    fail("safe_unsigned_integer_required");
  }
  return static_cast<std::uint64_t>(item);
}
CanonicalValue strings(const std::vector<std::string>& values) {
  CanonicalValue::Array rows;
  rows.reserve(values.size());
  for (const auto& value : values) rows.emplace_back(value);
  return CanonicalValue(std::move(rows));
}
template <class Id>
CanonicalValue ids(const std::vector<Id>& values) {
  CanonicalValue::Array rows;
  rows.reserve(values.size());
  for (const auto& value : values) rows.emplace_back(value.value());
  return CanonicalValue(std::move(rows));
}
CanonicalValue numbers(const std::vector<double>& values) {
  CanonicalValue::Array rows;
  rows.reserve(values.size());
  for (double value : values) rows.emplace_back(value);
  return CanonicalValue(std::move(rows));
}
std::vector<std::string> get_strings(const CanonicalValue& value) {
  if (!value.is_array()) fail("array_required");
  std::vector<std::string> result;
  result.reserve(value.as_array().size());
  for (const auto& row : value.as_array()) result.push_back(text(row));
  return result;
}
template <class Id>
std::vector<Id> get_ids(const CanonicalValue& value) {
  std::vector<Id> result;
  for (auto& item : get_strings(value)) result.emplace_back(std::move(item));
  return result;
}
std::vector<double> get_numbers(const CanonicalValue& value) {
  if (!value.is_array()) fail("array_required");
  std::vector<double> result;
  result.reserve(value.as_array().size());
  for (const auto& row : value.as_array()) result.push_back(number(row));
  return result;
}
void validate_trajectory(const TrajectoryDTO& value) {
  const auto width = value.resources.size();
  if (width == 0U || width > kMaxCoordinates || value.joint_names.size() != width ||
      value.waypoints.size() < 2U || value.waypoints.size() > kMaxWaypoints ||
      std::any_of(value.resources.begin(), value.resources.end(),
                  [](const auto& id) { return id.empty(); }) ||
      std::any_of(value.joint_names.begin(), value.joint_names.end(),
                  [](const auto& name) { return name.empty(); }) ||
      std::set<ResourceId>(value.resources.begin(), value.resources.end()).size() != width) {
    fail("trajectory_envelope_invalid");
  }
  double prior = -1.0;
  for (const auto& waypoint : value.waypoints) {
    if (!std::isfinite(waypoint.time_from_start_s) || waypoint.time_from_start_s < 0.0 ||
        waypoint.time_from_start_s <= prior || waypoint.positions_rad.size() != width ||
        (!waypoint.velocities_rad_s.empty() && waypoint.velocities_rad_s.size() != width) ||
        (!waypoint.accelerations_rad_s2.empty() && waypoint.accelerations_rad_s2.size() != width)) {
      fail("trajectory_waypoint_invalid");
    }
    const auto finite = [](double item) { return std::isfinite(item); };
    if (!std::all_of(waypoint.positions_rad.begin(), waypoint.positions_rad.end(), finite) ||
        !std::all_of(waypoint.velocities_rad_s.begin(), waypoint.velocities_rad_s.end(), finite) ||
        !std::all_of(waypoint.accelerations_rad_s2.begin(), waypoint.accelerations_rad_s2.end(),
                     finite)) {
      fail("trajectory_number_invalid");
    }
    prior = waypoint.time_from_start_s;
  }
}
CanonicalValue trajectory_value(const TrajectoryDTO& value) {
  validate_trajectory(value);
  CanonicalValue::Array waypoints;
  waypoints.reserve(value.waypoints.size());
  for (const auto& waypoint : value.waypoints) {
    CanonicalValue::Object row{{"time_from_start_s", CanonicalValue(waypoint.time_from_start_s)},
                               {"positions_rad", numbers(waypoint.positions_rad)}};
    if (!waypoint.velocities_rad_s.empty())
      row.emplace_back("velocities_rad_s", numbers(waypoint.velocities_rad_s));
    if (!waypoint.accelerations_rad_s2.empty())
      row.emplace_back("accelerations_rad_s2", numbers(waypoint.accelerations_rad_s2));
    waypoints.emplace_back(std::move(row));
  }
  return CanonicalValue(
      CanonicalValue::Object{{"resources", ids(value.resources)},
                             {"joint_names", strings(value.joint_names)},
                             {"waypoints", CanonicalValue(std::move(waypoints))}});
}
TrajectoryDTO get_trajectory(const CanonicalValue& value) {
  exact(value, {"resources", "joint_names", "waypoints"});
  const auto& rows = member(value, "waypoints");
  if (!rows.is_array()) fail("trajectory_waypoints_array_required");
  TrajectoryDTO result{get_ids<ResourceId>(member(value, "resources")),
                       get_strings(member(value, "joint_names")),
                       {}};
  result.waypoints.reserve(rows.as_array().size());
  for (const auto& row : rows.as_array()) {
    const bool velocity = row.find_object_member("velocities_rad_s") != nullptr;
    const bool acceleration = row.find_object_member("accelerations_rad_s2") != nullptr;
    exact(row,
          velocity
              ? (acceleration
                     ? std::initializer_list<std::string_view>{"time_from_start_s", "positions_rad",
                                                               "velocities_rad_s",
                                                               "accelerations_rad_s2"}
                     : std::initializer_list<std::string_view>{"time_from_start_s", "positions_rad",
                                                               "velocities_rad_s"})
              : (acceleration
                     ? std::initializer_list<std::string_view>{"time_from_start_s", "positions_rad",
                                                               "accelerations_rad_s2"}
                     : std::initializer_list<std::string_view>{"time_from_start_s",
                                                               "positions_rad"}));
    result.waypoints.push_back(
        {number(member(row, "time_from_start_s")), get_numbers(member(row, "positions_rad")),
         velocity ? get_numbers(member(row, "velocities_rad_s")) : std::vector<double>{},
         acceleration ? get_numbers(member(row, "accelerations_rad_s2")) : std::vector<double>{}});
  }
  validate_trajectory(result);
  return result;
}
}  // namespace

Result<std::string> EncodePreparedTrajectory(const TrajectoryDTO& value) {
  try {
    return Result<std::string>::Ok(content_identity::canonical_json(trajectory_value(value)));
  } catch (const std::exception& error) {
    return rejected<std::string>(error.what());
  }
}
Result<TrajectoryDTO> DecodePreparedTrajectory(std::string_view bytes) {
  try {
    return Result<TrajectoryDTO>::Ok(get_trajectory(content_identity::parse_canonical_json(bytes)));
  } catch (const std::exception& error) {
    return rejected<TrajectoryDTO>(error.what());
  }
}

namespace {
CanonicalValue error_value(const ErrorDTO& error) {
  CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto& [key, value] : error.details) details.emplace_back(key, CanonicalValue(value));
  return CanonicalValue(CanonicalValue::Object{{"code", CanonicalValue(error.code)},
                                               {"message", CanonicalValue(error.message)},
                                               {"details", CanonicalValue(std::move(details))}});
}
ErrorDTO get_error(const CanonicalValue& value) {
  exact(value, {"code", "message", "details"});
  ErrorDTO result{text(member(value, "code")), text(member(value, "message")), {}};
  for (const auto& [key, item] : object(member(value, "details")))
    result.details.emplace(key, text(item));
  return result;
}
CanonicalValue projection_value(const ProjectionRevisionDTO& value) {
  if (!value.valid()) fail("projection_revision_invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"scene_id", CanonicalValue(value.scene_revision.scene_id.value())},
           {"revision_id", CanonicalValue(static_cast<double>(value.scene_revision.revision_id))},
           {"content_hash", CanonicalValue(value.scene_revision.content_hash.value())}})}});
}
ProjectionRevisionDTO get_projection(const CanonicalValue& value) {
  exact(value, {"retained_bundle_id", "graph_revision", "scene_revision"});
  const auto& scene = member(value, "scene_revision");
  exact(scene, {"scene_id", "revision_id", "content_hash"});
  const auto revision = number(member(scene, "revision_id"));
  if (revision < 1.0 || std::floor(revision) != revision || revision > 9'007'199'254'740'991.0)
    fail("scene_revision_id_invalid");
  ProjectionRevisionDTO result{
      RetainedBundleId(text(member(value, "retained_bundle_id"))),
      GraphRevision(text(member(value, "graph_revision"))),
      SceneRevision{SceneId(text(member(scene, "scene_id"))), static_cast<std::uint64_t>(revision),
                    ContentDigest(text(member(scene, "content_hash")))}};
  if (!result.valid()) fail("projection_revision_invalid");
  return result;
}
CanonicalValue plan_result_value(const PlanResultDTO& value) {
  if (value.trajectory.has_value() == value.error.has_value()) fail("plan_result_xor_invalid");
  CanonicalValue::Object fields;
  fields.reserve(value.trajectory.has_value() ? 2U : 1U);
  if (value.trajectory.has_value()) {
    fields.emplace_back("trajectory", trajectory_value(*value.trajectory));
    fields.emplace_back("projection_revision", projection_value(value.projection_revision));
  } else {
    if (value.projection_revision.valid()) fail("failed_plan_projection_forbidden");
    fields.emplace_back("error", error_value(*value.error));
  }
  return CanonicalValue(std::move(fields));
}
PlanResultDTO get_plan_result(const CanonicalValue& value) {
  const bool success = value.find_object_member("trajectory") != nullptr;
  if (success) {
    exact(value, {"trajectory", "projection_revision"});
    return PlanResultDTO{get_trajectory(member(value, "trajectory")), std::nullopt,
                         get_projection(member(value, "projection_revision"))};
  }
  exact(value, {"error"});
  return PlanResultDTO{std::nullopt, get_error(member(value, "error")), {}};
}
}  // namespace

Result<std::string> EncodePreparedPlanResult(const PlanResultDTO& value) {
  try {
    return Result<std::string>::Ok(content_identity::canonical_json(plan_result_value(value)));
  } catch (const std::exception& error) {
    return rejected<std::string>(error.what());
  }
}
Result<PlanResultDTO> DecodePreparedPlanResult(std::string_view bytes) {
  try {
    return Result<PlanResultDTO>::Ok(
        get_plan_result(content_identity::parse_canonical_json(bytes)));
  } catch (const std::exception& error) {
    return rejected<PlanResultDTO>(error.what());
  }
}

namespace {
CanonicalValue pose_value(const Pose3DTO& p) {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m",
       CanonicalValue(CanonicalValue::Object{{"x_m", CanonicalValue(p.position_m.x_m)},
                                             {"y_m", CanonicalValue(p.position_m.y_m)},
                                             {"z_m", CanonicalValue(p.position_m.z_m)}})},
      {"orientation",
       CanonicalValue(CanonicalValue::Object{{"x", CanonicalValue(p.orientation.x)},
                                             {"y", CanonicalValue(p.orientation.y)},
                                             {"z", CanonicalValue(p.orientation.z)},
                                             {"w", CanonicalValue(p.orientation.w)}})}});
}
Pose3DTO get_pose(const CanonicalValue& v) {
  exact(v, {"position_m", "orientation"});
  const auto& p = member(v, "position_m");
  const auto& q = member(v, "orientation");
  exact(p, {"x_m", "y_m", "z_m"});
  exact(q, {"x", "y", "z", "w"});
  return {{number(member(p, "x_m")), number(member(p, "y_m")), number(member(p, "z_m"))},
          {number(member(q, "x")), number(member(q, "y")), number(member(q, "z")),
           number(member(q, "w"))}};
}
CanonicalValue frame_value(const ResolvedFrameDTO& frame) {
  return std::visit(
      [](const auto& f) {
        using F = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<F, ResolvedCellFrameDTO>)
          return CanonicalValue(CanonicalValue::Object{
              {"cell_frame",
               CanonicalValue(CanonicalValue::Object{
                   {"cell_id", CanonicalValue(f.cell_id.value())},
                   {"compiled_frame_name", CanonicalValue(f.compiled_frame_name)}})}});
        else if constexpr (std::is_same_v<F, ResolvedComponentModelFrameDTO>)
          return CanonicalValue(CanonicalValue::Object{
              {"component_model_frame",
               CanonicalValue(CanonicalValue::Object{
                   {"controllable_component_id",
                    CanonicalValue(f.controllable_component_id.value())},
                   {"model_instance_id", CanonicalValue(f.model_instance_id.value())},
                   {"frame_role_id", CanonicalValue(f.frame_role_id.value())},
                   {"compiled_frame_name", CanonicalValue(f.compiled_frame_name)},
                   {"model_revision", CanonicalValue(f.model_revision.value())}})}});
        else
          return CanonicalValue(CanonicalValue::Object{
              {"passive_model_frame",
               CanonicalValue(CanonicalValue::Object{
                   {"model_instance_id", CanonicalValue(f.model_instance_id.value())},
                   {"frame_role_id", CanonicalValue(f.frame_role_id.value())},
                   {"compiled_frame_name", CanonicalValue(f.compiled_frame_name)},
                   {"model_revision", CanonicalValue(f.model_revision.value())}})}});
      },
      frame);
}
ResolvedFrameDTO get_frame(const CanonicalValue& v) {
  if (object(v).size() != 1U) fail("frame_tag_invalid");
  if (const auto* x = v.find_object_member("cell_frame")) {
    exact(*x, {"cell_id", "compiled_frame_name"});
    return ResolvedCellFrameDTO{CellId(text(member(*x, "cell_id"))),
                                text(member(*x, "compiled_frame_name"))};
  }
  if (const auto* x = v.find_object_member("component_model_frame")) {
    exact(*x, {"controllable_component_id", "model_instance_id", "frame_role_id",
               "compiled_frame_name", "model_revision"});
    return ResolvedComponentModelFrameDTO{
        ControllableComponentId(text(member(*x, "controllable_component_id"))),
        ModelInstanceId(text(member(*x, "model_instance_id"))),
        FrameRoleId(text(member(*x, "frame_role_id"))), text(member(*x, "compiled_frame_name")),
        ModelRevision(text(member(*x, "model_revision")))};
  }
  if (const auto* x = v.find_object_member("passive_model_frame")) {
    exact(*x, {"model_instance_id", "frame_role_id", "compiled_frame_name", "model_revision"});
    return ResolvedPassiveModelFrameDTO{ModelInstanceId(text(member(*x, "model_instance_id"))),
                                        FrameRoleId(text(member(*x, "frame_role_id"))),
                                        text(member(*x, "compiled_frame_name")),
                                        ModelRevision(text(member(*x, "model_revision")))};
  }
  fail("frame_tag_invalid");
}
CanonicalValue coordinate_position(const CoordinatePositionDTO& v) {
  return std::visit(
      [](const auto& x) {
        using X = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<X, AngularPositionDTO>)
          return CanonicalValue(CanonicalValue::Object{{"value_rad", CanonicalValue(x.value_rad)}});
        else
          return CanonicalValue(CanonicalValue::Object{{"value_m", CanonicalValue(x.value_m)}});
      },
      v);
}
CoordinatePositionDTO get_position(const CanonicalValue& v) {
  if (const auto* x = v.find_object_member("value_rad")) {
    exact(v, {"value_rad"});
    return AngularPositionDTO{number(*x)};
  }
  exact(v, {"value_m"});
  return LinearPositionDTO{number(member(v, "value_m"))};
}
CanonicalValue coordinate_velocity(const CoordinateVelocityDTO& v) {
  return std::visit(
      [](const auto& x) {
        using X = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<X, AngularVelocityDTO>)
          return CanonicalValue(
              CanonicalValue::Object{{"value_rad_s", CanonicalValue(x.value_rad_s)}});
        else
          return CanonicalValue(CanonicalValue::Object{{"value_m_s", CanonicalValue(x.value_m_s)}});
      },
      v);
}
CoordinateVelocityDTO get_velocity(const CanonicalValue& v) {
  if (const auto* x = v.find_object_member("value_rad_s")) {
    exact(v, {"value_rad_s"});
    return AngularVelocityDTO{number(*x)};
  }
  exact(v, {"value_m_s"});
  return LinearVelocityDTO{number(member(v, "value_m_s"))};
}
CanonicalValue state_value(const ControlledStateDTO& s) {
  CanonicalValue::Array pos;
  for (const auto& x : s.positions) pos.push_back(coordinate_position(x));
  CanonicalValue::Object f{
      {"controllable_component_id", CanonicalValue(s.controllable_component_id.value())},
      {"command_target_id", CanonicalValue(s.command_target_id.value())},
      {"control_domain_id", CanonicalValue(s.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(s.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(s.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(s.control_binding_revision.value())},
      {"controller_topology_id", CanonicalValue(s.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(s.topology_revision.value())},
      {"control_route_id", CanonicalValue(s.control_route_id.value())},
      {"active_control_configuration_id",
       CanonicalValue(s.active_control_configuration_id.value())},
      {"active_control_configuration_generation",
       CanonicalValue(static_cast<double>(s.active_control_configuration_generation.value()))},
      {"resources", ids(s.resources)},
      {"joint_names", strings(s.joint_names)},
      {"positions", CanonicalValue(std::move(pos))}};
  if (s.velocities) {
    CanonicalValue::Array a;
    for (const auto& x : *s.velocities) a.push_back(coordinate_velocity(x));
    f.emplace_back("velocities", CanonicalValue(std::move(a)));
  }
  return CanonicalValue(std::move(f));
}
ControlledStateDTO get_state(const CanonicalValue& v) {
  const bool hv = v.find_object_member("velocities") != nullptr;
  exact(
      v,
      hv ? std::initializer_list<std::string_view>{"controllable_component_id", "command_target_id",
                                                   "control_domain_id", "control_domain_revision",
                                                   "control_binding_id", "control_binding_revision",
                                                   "controller_topology_id", "topology_revision",
                                                   "control_route_id",
                                                   "active_control_configuration_id",
                                                   "active_control_configuration_generation",
                                                   "resources", "joint_names", "positions",
                                                   "velocities"}
         : std::initializer_list<std::string_view>{
               "controllable_component_id", "command_target_id", "control_domain_id",
               "control_domain_revision", "control_binding_id", "control_binding_revision",
               "controller_topology_id", "topology_revision", "control_route_id",
               "active_control_configuration_id", "active_control_configuration_generation",
               "resources", "joint_names", "positions"});
  std::vector<CoordinatePositionDTO> p;
  for (const auto& x : member(v, "positions").as_array()) p.push_back(get_position(x));
  std::optional<std::vector<CoordinateVelocityDTO>> vel;
  if (hv) {
    vel.emplace();
    for (const auto& x : member(v, "velocities").as_array()) vel->push_back(get_velocity(x));
  }
  return {ControllableComponentId(text(member(v, "controllable_component_id"))),
          CommandTargetId(text(member(v, "command_target_id"))),
          ControlDomainId(text(member(v, "control_domain_id"))),
          ControlDomainRevision(text(member(v, "control_domain_revision"))),
          ControlBindingId(text(member(v, "control_binding_id"))),
          ControlBindingRevision(text(member(v, "control_binding_revision"))),
          ControllerTopologyId(text(member(v, "controller_topology_id"))),
          TopologyRevision(text(member(v, "topology_revision"))),
          ControlRouteId(text(member(v, "control_route_id"))),
          ActiveControlConfigurationId(text(member(v, "active_control_configuration_id"))),
          ActiveControlConfigurationGeneration(
              safe_unsigned(member(v, "active_control_configuration_generation"), true)),
          get_ids<ResourceId>(member(v, "resources")),
          get_strings(member(v, "joint_names")),
          std::move(p),
          std::move(vel)};
}
std::string planning_profile(PlanningProfileDTO x) {
  switch (x) {
    case PlanningProfileDTO::kFirstFeasible:
      return "first_feasible";
    case PlanningProfileDTO::kBalanced:
      return "balanced";
    case PlanningProfileDTO::kQualityBounded:
      return "quality_bounded";
  }
  fail("planning_profile_invalid");
}
PlanningProfileDTO get_planning_profile(std::string_view x) {
  if (x == "first_feasible") return PlanningProfileDTO::kFirstFeasible;
  if (x == "balanced") return PlanningProfileDTO::kBalanced;
  if (x == "quality_bounded") return PlanningProfileDTO::kQualityBounded;
  fail("planning_profile_invalid");
}
CanonicalValue motion_value(const ResolvedMotionSpecDTO& m) {
  return std::visit(
      [](const auto& x) -> CanonicalValue {
        using X = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<X, ResolvedJointGoalDTO>)
          return CanonicalValue(CanonicalValue::Object{
              {"joint_goal", CanonicalValue(CanonicalValue::Object{
                                 {"resources", ids(x.resources)},
                                 {"joint_names", strings(x.joint_names)},
                                 {"positions_rad", numbers(x.positions_rad)}})}});
        else if constexpr (std::is_same_v<X, PoseGoalDTO>)
          return CanonicalValue(CanonicalValue::Object{{"pose_goal", pose_value(x.goal)}});
        else if constexpr (std::is_same_v<X, CartesianPathDTO>) {
          CanonicalValue::Array w;
          for (const auto& p : x.waypoints) w.push_back(pose_value(p));
          return CanonicalValue(CanonicalValue::Object{
              {"cartesian_path",
               CanonicalValue(CanonicalValue::Object{
                   {"waypoints", CanonicalValue(std::move(w))},
                   {"max_translation_step_m", CanonicalValue(x.max_translation_step_m)},
                   {"max_rotation_step_rad", CanonicalValue(x.max_rotation_step_rad)},
                   {"speed_mode", CanonicalValue("bounded")},
                   {"max_linear_speed_mps", CanonicalValue(x.max_linear_speed_mps)},
                   {"max_angular_speed_rad_s", CanonicalValue(x.max_angular_speed_rad_s)},
                   {"max_linear_acceleration_mps2", CanonicalValue(x.max_linear_acceleration_mps2)},
                   {"max_angular_acceleration_rad_s2",
                    CanonicalValue(x.max_angular_acceleration_rad_s2)},
                   {"fallback_policy",
                    CanonicalValue(x.fallback_policy == CartesianFallbackPolicyDTO::kFailClosed
                                       ? "fail_closed"
                                       : "allow_joint_space_fallback")}})}});
        } else
          fail("unresolved_named_target_forbidden");
      },
      m);
}
ResolvedMotionSpecDTO get_motion(const CanonicalValue& v) {
  if (object(v).size() != 1U) fail("motion_tag_invalid");
  if (const auto* x = v.find_object_member("joint_goal")) {
    exact(*x, {"resources", "joint_names", "positions_rad"});
    return ResolvedJointGoalDTO{get_ids<ResourceId>(member(*x, "resources")),
                                get_strings(member(*x, "joint_names")),
                                get_numbers(member(*x, "positions_rad"))};
  }
  if (const auto* x = v.find_object_member("pose_goal")) return PoseGoalDTO{get_pose(*x)};
  if (const auto* x = v.find_object_member("cartesian_path")) {
    exact(*x, {"waypoints", "max_translation_step_m", "max_rotation_step_rad", "speed_mode",
               "max_linear_speed_mps", "max_angular_speed_rad_s", "max_linear_acceleration_mps2",
               "max_angular_acceleration_rad_s2", "fallback_policy"});
    if (text(member(*x, "speed_mode")) != "bounded") fail("cartesian_speed_mode_invalid");
    std::vector<Pose3DTO> w;
    for (const auto& p : member(*x, "waypoints").as_array()) w.push_back(get_pose(p));
    const auto fp = text(member(*x, "fallback_policy"));
    if (fp != "fail_closed" && fp != "allow_joint_space_fallback")
      fail("cartesian_fallback_invalid");
    return CartesianPathDTO{std::move(w),
                            number(member(*x, "max_translation_step_m")),
                            number(member(*x, "max_rotation_step_rad")),
                            CartesianSpeedModeDTO::kBounded,
                            number(member(*x, "max_linear_speed_mps")),
                            number(member(*x, "max_angular_speed_rad_s")),
                            number(member(*x, "max_linear_acceleration_mps2")),
                            number(member(*x, "max_angular_acceleration_rad_s2")),
                            fp == "fail_closed"
                                ? CartesianFallbackPolicyDTO::kFailClosed
                                : CartesianFallbackPolicyDTO::kAllowJointSpaceFallback};
  }
  fail("motion_tag_invalid");
}
CanonicalValue planner_value(const PlannerCapabilityBindingDTO& p) {
  return CanonicalValue(CanonicalValue::Object{
      {"capability_id", CanonicalValue(p.capability_id.value())},
      {"capability_definition_revision", CanonicalValue(p.capability_definition_revision.value())},
      {"capability_provider_binding_id", CanonicalValue(p.capability_provider_binding_id.value())},
      {"capability_provider_binding_revision",
       CanonicalValue(p.capability_provider_binding_revision.value())},
      {"provider_instance_id", CanonicalValue(p.provider_instance_id.value())},
      {"provider_manifest_id", CanonicalValue(p.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(p.provider_manifest_revision.value())},
      {"provider_runtime_unit_id", CanonicalValue(p.provider_runtime_unit_id.value())},
      {"provider_runtime_epoch", CanonicalValue(p.provider_runtime_epoch.value())}});
}
PlannerCapabilityBindingDTO get_planner(const CanonicalValue& v) {
  exact(v, {"capability_id", "capability_definition_revision", "capability_provider_binding_id",
            "capability_provider_binding_revision", "provider_instance_id", "provider_manifest_id",
            "provider_manifest_revision", "provider_runtime_unit_id", "provider_runtime_epoch"});
  return {
      CapabilityId(text(member(v, "capability_id"))),
      CapabilityDefinitionRevision(text(member(v, "capability_definition_revision"))),
      CapabilityProviderBindingId(text(member(v, "capability_provider_binding_id"))),
      CapabilityProviderBindingRevision(text(member(v, "capability_provider_binding_revision"))),
      ProviderInstanceId(text(member(v, "provider_instance_id"))),
      ProviderManifestId(text(member(v, "provider_manifest_id"))),
      ProviderManifestRevision(text(member(v, "provider_manifest_revision"))),
      RuntimeUnitId(text(member(v, "provider_runtime_unit_id"))),
      RuntimeEpoch(text(member(v, "provider_runtime_epoch")))};
}
CanonicalValue plan_request_value(const PlanRequestDTO& r) {
  if (r.retained_collision_world || r.retained_collision_policy)
    fail("private_collision_handles_must_be_null");
  if (!r.active_control_configuration_generation.nonzero() ||
      static_cast<double>(r.active_control_configuration_generation.value()) >
          9'007'199'254'740'991.0) {
    fail("active_control_configuration_generation_invalid");
  }
  CanonicalValue::Array models;
  for (const auto& m : r.model_bindings)
    models.emplace_back(
        CanonicalValue::Object{{"model_instance_id", CanonicalValue(m.model_instance_id.value())},
                               {"model_revision", CanonicalValue(m.model_revision.value())}});
  CanonicalValue::Array units;
  for (const auto& u : r.runtime_unit_bindings)
    units.emplace_back(
        CanonicalValue::Object{{"runtime_unit_id", CanonicalValue(u.runtime_unit_id.value())},
                               {"runtime_epoch", CanonicalValue(u.runtime_epoch.value())}});
  CanonicalValue::Array dynamics;
  for (const auto& d : r.per_resource_dynamics_limits) {
    CanonicalValue::Object limit;
    if (d.limit.max_velocity_rad_s)
      limit.emplace_back("max_velocity_rad_s", CanonicalValue(*d.limit.max_velocity_rad_s));
    if (d.limit.max_acceleration_rad_s2)
      limit.emplace_back("max_acceleration_rad_s2",
                         CanonicalValue(*d.limit.max_acceleration_rad_s2));
    if (d.limit.max_jerk_rad_s3)
      limit.emplace_back("max_jerk_rad_s3", CanonicalValue(*d.limit.max_jerk_rad_s3));
    dynamics.emplace_back(
        CanonicalValue::Object{{"resource_id", CanonicalValue(d.resource_id.value())},
                               {"compiled_joint_name", CanonicalValue(d.compiled_joint_name)},
                               {"limit", CanonicalValue(std::move(limit))}});
  }
  CanonicalValue::Object speed{
      {"velocity_scale", CanonicalValue(r.speed_profile.velocity_scale)},
      {"acceleration_scale", CanonicalValue(r.speed_profile.acceleration_scale)}};
  if (r.speed_profile.max_tcp_linear_speed_mps)
    speed.emplace_back("max_tcp_linear_speed_mps",
                       CanonicalValue(*r.speed_profile.max_tcp_linear_speed_mps));
  CanonicalValue::Object f{
      {"operation_id", CanonicalValue(r.operation_id.value())},
      {"participant_id", CanonicalValue(r.participant_id.value())},
      {"program_id", CanonicalValue(r.program_id.value())},
      {"program_revision", CanonicalValue(r.program_revision.value())},
      {"program_binding_id", CanonicalValue(r.program_binding_id.value())},
      {"program_run_id", CanonicalValue(r.program_run_id.value())},
      {"move_id", CanonicalValue(r.move_id.value())},
      {"controllable_component_id", CanonicalValue(r.controllable_component_id.value())},
      {"component_type_id", CanonicalValue(r.component_type_id.value())},
      {"component_type_revision", CanonicalValue(r.component_type_revision.value())},
      {"command_target_id", CanonicalValue(r.command_target_id.value())},
      {"capability_id", CanonicalValue(r.capability_id.value())},
      {"capability_definition_revision", CanonicalValue(r.capability_definition_revision.value())},
      {"planner_capability_binding", planner_value(r.planner_capability_binding)},
      {"model_bindings", CanonicalValue(std::move(models))},
      {"model_group_binding",
       CanonicalValue(CanonicalValue::Object{
           {"model_group_role_id",
            CanonicalValue(r.model_group_binding.model_group_role_id.value())},
           {"model_instance_id", CanonicalValue(r.model_group_binding.model_instance_id.value())},
           {"model_revision", CanonicalValue(r.model_group_binding.model_revision.value())},
           {"compiled_group_name", CanonicalValue(r.model_group_binding.compiled_group_name)},
           {"compiled_joint_names", strings(r.model_group_binding.compiled_joint_names)}})},
      {"control_domain_id", CanonicalValue(r.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(r.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(r.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(r.control_binding_revision.value())},
      {"control_interface_contract_id", CanonicalValue(r.control_interface_contract_id.value())},
      {"control_interface_revision", CanonicalValue(r.control_interface_revision.value())},
      {"controller_topology_id", CanonicalValue(r.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(r.topology_revision.value())},
      {"control_route_id", CanonicalValue(r.control_route_id.value())},
      {"activation_set_id", CanonicalValue(r.activation_set_id.value())},
      {"active_control_configuration_id",
       CanonicalValue(r.active_control_configuration_id.value())},
      {"active_control_configuration_generation",
       CanonicalValue(static_cast<double>(r.active_control_configuration_generation.value()))},
      {"provider_instance_id", CanonicalValue(r.provider_instance_id.value())},
      {"provider_manifest_id", CanonicalValue(r.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(r.provider_manifest_revision.value())},
      {"provider_runtime_epoch", CanonicalValue(r.provider_runtime_epoch.value())},
      {"runtime_unit_bindings", CanonicalValue(std::move(units))},
      {"resources", ids(r.resources)},
      {"base_frame", frame_value(r.base_frame)},
      {"tcp_frame", frame_value(r.tcp_frame)},
      {"start_state", state_value(r.start_state)},
      {"motion", motion_value(r.motion)},
      {"tolerances",
       CanonicalValue(CanonicalValue::Object{
           {"joint_position_tol_rad", CanonicalValue(r.tolerances.joint_position_tol_rad)},
           {"translation_tol_m", CanonicalValue(r.tolerances.translation_tol_m)},
           {"rotation_tol_rad", CanonicalValue(r.tolerances.rotation_tol_rad)}})},
      {"speed_profile", CanonicalValue(std::move(speed))},
      {"planning_profile", CanonicalValue(planning_profile(r.planning_profile))},
      {"per_resource_dynamics_limits", CanonicalValue(std::move(dynamics))},
      {"planning_timeout_s", CanonicalValue(r.planning_timeout_s)},
      {"planning_attempt_index", CanonicalValue(static_cast<double>(r.planning_attempt_index))},
      {"projection_binding",
       CanonicalValue(CanonicalValue::Object{
           {"projection_revision", projection_value(r.projection_binding.projection_revision)},
           {"projection_lease_id",
            CanonicalValue(r.projection_binding.projection_lease_id.value())}})},
      {"component_attachment_revisions", ids(r.component_attachment_revisions)},
      {"carried_object_attachment_revisions", ids(r.carried_object_attachment_revisions)}};
  if (r.action_span)
    f.emplace_back(
        "action_span",
        CanonicalValue(CanonicalValue::Object{
            {"action_source", CanonicalValue(to_string(r.action_span->action_source))},
            {"action_span_id", CanonicalValue(r.action_span->action_span_id)},
            {"program_run_id", CanonicalValue(r.action_span->program_run_id)},
            {"node_instance_id", CanonicalValue(r.action_span->node_instance_id)},
            {"node_attempt_id", CanonicalValue(r.action_span->node_attempt_id)},
            {"parent_node_attempt_id", CanonicalValue(r.action_span->parent_node_attempt_id)}}));
  if (r.contact_policy_id)
    f.emplace_back("contact_policy_id", CanonicalValue(*r.contact_policy_id));
  return CanonicalValue(std::move(f));
}
PlanRequestDTO get_plan_request(const CanonicalValue& v) {
  const bool ha = v.find_object_member("action_span") != nullptr;
  const bool hc = v.find_object_member("contact_policy_id") != nullptr;
  if (object(v).size() != 47U + (ha ? 1U : 0U) + (hc ? 1U : 0U))
    fail("plan_request_member_count_invalid");
  PlanRequestDTO r{
      OperationId(text(member(v, "operation_id"))),
      OperationParticipantId(text(member(v, "participant_id"))),
      ProgramId(text(member(v, "program_id"))),
      ProgramRevision(text(member(v, "program_revision"))),
      ProgramBindingId(text(member(v, "program_binding_id"))),
      ProgramRunId(text(member(v, "program_run_id"))),
      MoveId(text(member(v, "move_id"))),
      ControllableComponentId(text(member(v, "controllable_component_id"))),
      ComponentTypeId(text(member(v, "component_type_id"))),
      ComponentTypeRevision(text(member(v, "component_type_revision"))),
      CommandTargetId(text(member(v, "command_target_id"))),
      CapabilityId(text(member(v, "capability_id"))),
      CapabilityDefinitionRevision(text(member(v, "capability_definition_revision"))),
      get_planner(member(v, "planner_capability_binding")),
      {},
      PlannerModelGroupBindingDTO{ModelGroupRoleId("pending"),
                                  ModelInstanceId("pending"),
                                  ModelRevision("pending"),
                                  "pending",
                                  {"pending"}},
      ControlDomainId(text(member(v, "control_domain_id"))),
      ControlDomainRevision(text(member(v, "control_domain_revision"))),
      ControlBindingId(text(member(v, "control_binding_id"))),
      ControlBindingRevision(text(member(v, "control_binding_revision"))),
      ControlInterfaceContractId(text(member(v, "control_interface_contract_id"))),
      ControlInterfaceRevision(text(member(v, "control_interface_revision"))),
      ControllerTopologyId(text(member(v, "controller_topology_id"))),
      TopologyRevision(text(member(v, "topology_revision"))),
      ControlRouteId(text(member(v, "control_route_id"))),
      ActivationSetId(text(member(v, "activation_set_id"))),
      ActiveControlConfigurationId(text(member(v, "active_control_configuration_id"))),
      ActiveControlConfigurationGeneration(
          safe_unsigned(member(v, "active_control_configuration_generation"), true)),
      ProviderInstanceId(text(member(v, "provider_instance_id"))),
      ProviderManifestId(text(member(v, "provider_manifest_id"))),
      ProviderManifestRevision(text(member(v, "provider_manifest_revision"))),
      RuntimeEpoch(text(member(v, "provider_runtime_epoch"))),
      {},
      {},
      std::nullopt,
      get_frame(member(v, "base_frame")),
      get_frame(member(v, "tcp_frame")),
      get_state(member(v, "start_state")),
      get_motion(member(v, "motion")),
      TolerancesDTO{0.0, 0.0, 0.0},
      SpeedProfileDTO{},
      PlanningProfileDTO::kFirstFeasible,
      std::nullopt,
      {},
      1.0,
      0U,
      ProjectionLeaseBindingDTO{
          get_projection(member(member(v, "projection_binding"), "projection_revision")),
          ProjectionLeaseId(text(member(member(v, "projection_binding"), "projection_lease_id")))},
      {},
      {},
      nullptr,
      nullptr};
  r.operation_id = OperationId(text(member(v, "operation_id")));
  r.participant_id = OperationParticipantId(text(member(v, "participant_id")));
  r.program_id = ProgramId(text(member(v, "program_id")));
  r.program_revision = ProgramRevision(text(member(v, "program_revision")));
  r.program_binding_id = ProgramBindingId(text(member(v, "program_binding_id")));
  r.program_run_id = ProgramRunId(text(member(v, "program_run_id")));
  r.move_id = MoveId(text(member(v, "move_id")));
  r.controllable_component_id =
      ControllableComponentId(text(member(v, "controllable_component_id")));
  r.component_type_id = ComponentTypeId(text(member(v, "component_type_id")));
  r.component_type_revision = ComponentTypeRevision(text(member(v, "component_type_revision")));
  r.command_target_id = CommandTargetId(text(member(v, "command_target_id")));
  r.capability_id = CapabilityId(text(member(v, "capability_id")));
  r.capability_definition_revision =
      CapabilityDefinitionRevision(text(member(v, "capability_definition_revision")));
  r.planner_capability_binding = get_planner(member(v, "planner_capability_binding"));
  for (const auto& m : member(v, "model_bindings").as_array()) {
    exact(m, {"model_instance_id", "model_revision"});
    r.model_bindings.push_back({ModelInstanceId(text(member(m, "model_instance_id"))),
                                ModelRevision(text(member(m, "model_revision")))});
  }
  const auto& g = member(v, "model_group_binding");
  exact(g, {"model_group_role_id", "model_instance_id", "model_revision", "compiled_group_name",
            "compiled_joint_names"});
  r.model_group_binding = {ModelGroupRoleId(text(member(g, "model_group_role_id"))),
                           ModelInstanceId(text(member(g, "model_instance_id"))),
                           ModelRevision(text(member(g, "model_revision"))),
                           text(member(g, "compiled_group_name")),
                           get_strings(member(g, "compiled_joint_names"))};
  r.control_domain_id = ControlDomainId(text(member(v, "control_domain_id")));
  r.control_domain_revision = ControlDomainRevision(text(member(v, "control_domain_revision")));
  r.control_binding_id = ControlBindingId(text(member(v, "control_binding_id")));
  r.control_binding_revision = ControlBindingRevision(text(member(v, "control_binding_revision")));
  r.control_interface_contract_id =
      ControlInterfaceContractId(text(member(v, "control_interface_contract_id")));
  r.control_interface_revision =
      ControlInterfaceRevision(text(member(v, "control_interface_revision")));
  r.controller_topology_id = ControllerTopologyId(text(member(v, "controller_topology_id")));
  r.topology_revision = TopologyRevision(text(member(v, "topology_revision")));
  r.control_route_id = ControlRouteId(text(member(v, "control_route_id")));
  r.activation_set_id = ActivationSetId(text(member(v, "activation_set_id")));
  r.active_control_configuration_id =
      ActiveControlConfigurationId(text(member(v, "active_control_configuration_id")));
  r.active_control_configuration_generation = ActiveControlConfigurationGeneration(
      safe_unsigned(member(v, "active_control_configuration_generation"), true));
  r.provider_instance_id = ProviderInstanceId(text(member(v, "provider_instance_id")));
  r.provider_manifest_id = ProviderManifestId(text(member(v, "provider_manifest_id")));
  r.provider_manifest_revision =
      ProviderManifestRevision(text(member(v, "provider_manifest_revision")));
  r.provider_runtime_epoch = RuntimeEpoch(text(member(v, "provider_runtime_epoch")));
  for (const auto& u : member(v, "runtime_unit_bindings").as_array()) {
    exact(u, {"runtime_unit_id", "runtime_epoch"});
    r.runtime_unit_bindings.push_back({RuntimeUnitId(text(member(u, "runtime_unit_id"))),
                                       RuntimeEpoch(text(member(u, "runtime_epoch")))});
  }
  r.resources = get_ids<ResourceId>(member(v, "resources"));
  r.base_frame = get_frame(member(v, "base_frame"));
  r.tcp_frame = get_frame(member(v, "tcp_frame"));
  r.start_state = get_state(member(v, "start_state"));
  r.motion = get_motion(member(v, "motion"));
  const auto& t = member(v, "tolerances");
  exact(t, {"joint_position_tol_rad", "translation_tol_m", "rotation_tol_rad"});
  r.tolerances = {number(member(t, "joint_position_tol_rad")),
                  number(member(t, "translation_tol_m")), number(member(t, "rotation_tol_rad"))};
  const auto& s = member(v, "speed_profile");
  const bool hm = s.find_object_member("max_tcp_linear_speed_mps") != nullptr;
  exact(s, hm ? std::initializer_list<std::string_view>{"velocity_scale", "acceleration_scale",
                                                        "max_tcp_linear_speed_mps"}
              : std::initializer_list<std::string_view>{"velocity_scale", "acceleration_scale"});
  r.speed_profile = {
      number(member(s, "velocity_scale")), number(member(s, "acceleration_scale")),
      hm ? std::optional<double>(number(member(s, "max_tcp_linear_speed_mps"))) : std::nullopt};
  r.planning_profile = get_planning_profile(text(member(v, "planning_profile")));
  for (const auto& d : member(v, "per_resource_dynamics_limits").as_array()) {
    exact(d, {"resource_id", "compiled_joint_name", "limit"});
    JointDynamicsLimitDTO l;
    const auto& z = member(d, "limit");
    if (const auto* x = z.find_object_member("max_velocity_rad_s"))
      l.max_velocity_rad_s = number(*x);
    if (const auto* x = z.find_object_member("max_acceleration_rad_s2"))
      l.max_acceleration_rad_s2 = number(*x);
    if (const auto* x = z.find_object_member("max_jerk_rad_s3")) l.max_jerk_rad_s3 = number(*x);
    r.per_resource_dynamics_limits.push_back(
        {ResourceId(text(member(d, "resource_id"))), text(member(d, "compiled_joint_name")), l});
  }
  r.planning_timeout_s = number(member(v, "planning_timeout_s"));
  const auto attempt = safe_unsigned(member(v, "planning_attempt_index"));
  if (attempt > std::numeric_limits<std::uint32_t>::max()) fail("planning_attempt_index_invalid");
  r.planning_attempt_index = static_cast<std::uint32_t>(attempt);
  const auto& pb = member(v, "projection_binding");
  exact(pb, {"projection_revision", "projection_lease_id"});
  r.projection_binding = {get_projection(member(pb, "projection_revision")),
                          ProjectionLeaseId(text(member(pb, "projection_lease_id")))};
  r.component_attachment_revisions =
      get_ids<ComponentAttachmentRevision>(member(v, "component_attachment_revisions"));
  r.carried_object_attachment_revisions =
      get_ids<CarriedObjectAttachmentRevision>(member(v, "carried_object_attachment_revisions"));
  if (hc) r.contact_policy_id = text(member(v, "contact_policy_id"));
  if (ha) {
    const auto& a = member(v, "action_span");
    exact(a, {"action_source", "action_span_id", "program_run_id", "node_instance_id",
              "node_attempt_id", "parent_node_attempt_id"});
    auto src = action_source_from_string(text(member(a, "action_source")));
    if (!src) fail("action_source_invalid");
    r.action_span = ActionSpanDTO{*src,
                                  text(member(a, "action_span_id")),
                                  text(member(a, "program_run_id")),
                                  text(member(a, "node_instance_id")),
                                  text(member(a, "node_attempt_id")),
                                  text(member(a, "parent_node_attempt_id"))};
  }
  return r;
}
}  // namespace

Result<std::string> EncodePreparedPlanRequest(const PlanRequestDTO& value) {
  try {
    return Result<std::string>::Ok(content_identity::canonical_json(plan_request_value(value)));
  } catch (const std::exception& e) {
    return rejected<std::string>(e.what());
  }
}
Result<PlanRequestDTO> DecodePreparedPlanRequest(std::string_view bytes) {
  try {
    return Result<PlanRequestDTO>::Ok(
        get_plan_request(content_identity::parse_canonical_json(bytes)));
  } catch (const std::exception& e) {
    return rejected<PlanRequestDTO>(e.what());
  }
}
Result<std::string> EncodePreparedControlledState(const ControlledStateDTO& value) {
  try {
    return Result<std::string>::Ok(content_identity::canonical_json(state_value(value)));
  } catch (const std::exception& error) {
    return rejected<std::string>(error.what());
  }
}
Result<ControlledStateDTO> DecodePreparedControlledState(std::string_view bytes) {
  try {
    return Result<ControlledStateDTO>::Ok(get_state(content_identity::parse_canonical_json(bytes)));
  } catch (const std::exception& error) {
    return rejected<ControlledStateDTO>(error.what());
  }
}
}  // namespace manyforge::core
