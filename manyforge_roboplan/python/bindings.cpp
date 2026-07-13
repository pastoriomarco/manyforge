#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <chrono>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

#include <Eigen/Dense>

#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
#include <manyforge_roboplan/ros2_control_kernel_factory.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#endif

namespace py = pybind11;
using namespace pybind11::literals;
using namespace manyforge::roboplan_backend;
using namespace manyforge::core;
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
namespace ros2_exec = manyforge::ros2_control_execution;
#endif

namespace {

py::dict action_span_to_python(const ActionSpanDTO& span);
std::optional<ActionSpanDTO> action_span_from_python(const py::dict& request);
void populate_request_correlation(ExecuteRequestDTO& req,
                                  const py::dict& request);
double finite_scale_from_python(const py::handle& value,
                                const char* field_name);

std::shared_ptr<ExecutionKernel> require_open_kernel(NativeKernelBundle& bundle) {
  return bundle.kernel_snapshot();
}

class PlannerProjectionLeaseGuard {
 public:
  explicit PlannerProjectionLeaseGuard(NativeKernelBundle& bundle)
      : bundle_(&bundle) {}
  ~PlannerProjectionLeaseGuard() {
    if (bundle_ && !retention_id_.empty() && bundle_->scene_authority) {
      bundle_->scene_authority->release_scene_snapshot_lease_inventory(
          retention_id_);
    }
  }
  PlannerProjectionLeaseGuard(const PlannerProjectionLeaseGuard&) = delete;
  PlannerProjectionLeaseGuard& operator=(const PlannerProjectionLeaseGuard&) =
      delete;

  Result<void> bind(PlanRequestDTO& request) {
    static std::atomic<std::uint64_t> next_id{1};
    if (!bundle_->scene_authority) {
      return Result<void>::Err(ErrorDTO{
          kErrorExecutionRejected,
          "Planner scene projection authority is unavailable",
          {{"reason", "scene_projection_authority_unavailable"}}});
    }
    retention_id_ =
        "direct_plan_" +
        std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
    auto retained =
        bundle_->scene_authority->retain_scene_snapshot_lease_inventory(
            retention_id_);
    if (!retained || !retained->bundle.world ||
        retained->retention_id.empty()) {
      retention_id_.clear();
      return Result<void>::Err(ErrorDTO{
          kErrorExecutionRejected,
          "Could not retain an exact scene projection for planning",
          {{"reason", "planner_projection_retention_failed"}}});
    }
    request.scene_binding = SceneRevisionBindingDTO{
        retained->bundle.world->revision, retained->retention_id};
    inventory_ = std::move(*retained);
    return Result<void>::Ok();
  }

  const CollisionWorldLeaseInventory& inventory() const {
    if (!inventory_.has_value()) {
      throw std::logic_error("Planner projection lease was not acquired");
    }
    return *inventory_;
  }

 private:
  NativeKernelBundle* bundle_;
  std::string retention_id_;
  std::optional<CollisionWorldLeaseInventory> inventory_;
};

std::unique_lock<std::mutex> lock_bundle_scene(NativeKernelBundle& bundle) {
  if (!bundle.scene_mutex) {
    return {};
  }
  return std::unique_lock<std::mutex>(*bundle.scene_mutex);
}

std::string canonical_joint_name(const NativeKernelBundle& bundle,
                                 const std::string& view_joint_name) {
  const auto it = bundle.view_to_canonical_joint_names.find(view_joint_name);
  return it == bundle.view_to_canonical_joint_names.end() ? view_joint_name
                                                         : it->second;
}

std::vector<std::string> canonical_joint_names(
    const NativeKernelBundle& bundle,
    const std::vector<std::string>& view_joint_names) {
  std::vector<std::string> result;
  result.reserve(view_joint_names.size());
  for (const auto& name : view_joint_names) {
    result.push_back(canonical_joint_name(bundle, name));
  }
  return result;
}

std::vector<double> compact_group_joint_positions(
    NativeKernelBundle& bundle,
    const std::string& group_name) {
  const auto maybe_group_info = bundle.scene->getJointGroupInfo(group_name);
  if (!maybe_group_info) {
    throw std::runtime_error(
        "Failed to get compact joint positions for group '" + group_name +
        "': " + maybe_group_info.error());
  }
  const auto& group_info = *maybe_group_info;
  const auto q = bundle.scene->getCurrentJointPositions();
  std::vector<double> result;
  result.reserve(group_info.q_indices.size());
  for (Eigen::Index i = 0; i < group_info.q_indices.size(); ++i) {
    result.push_back(q[group_info.q_indices[i]]);
  }
  return result;
}

std::vector<double> group_joint_positions_with_mimics(
    NativeKernelBundle& bundle,
    const std::string& group_name) {
  const auto maybe_group_info = bundle.scene->getJointGroupInfo(group_name);
  if (!maybe_group_info) {
    throw std::runtime_error(
        "Failed to get joint positions for group '" + group_name + "': " +
        maybe_group_info.error());
  }
  const auto& group_info = *maybe_group_info;
  if (group_info.joint_names.size() ==
      static_cast<size_t>(group_info.q_indices.size())) {
    return compact_group_joint_positions(bundle, group_name);
  }

  const auto q_with_mimics = bundle.scene->getCurrentJointPositionsWithMimics();
  const auto& all_joint_names = bundle.scene->getJointNamesWithMimics();
  std::map<std::string, Eigen::Index> index_by_name;
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(all_joint_names.size());
       ++i) {
    index_by_name[all_joint_names[static_cast<size_t>(i)]] = i;
  }

  std::vector<double> result;
  result.reserve(group_info.joint_names.size());
  for (const auto& joint_name : group_info.joint_names) {
    const auto index_it = index_by_name.find(joint_name);
    if (index_it == index_by_name.end() ||
        index_it->second < 0 ||
        index_it->second >= q_with_mimics.size()) {
      throw std::runtime_error(
          "Failed to get joint positions for group '" + group_name +
          "': joint '" + joint_name +
          "' is not present in the mimic-expanded model readout");
    }
    result.push_back(q_with_mimics[index_it->second]);
  }
  return result;
}

Eigen::VectorXd vector_from_std(const std::vector<double>& positions) {
  Eigen::VectorXd q(static_cast<Eigen::Index>(positions.size()));
  for (size_t i = 0; i < positions.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = positions[i];
  }
  return q;
}

Eigen::VectorXd compact_group_positions_from_public_input(
    NativeKernelBundle& bundle,
    const std::string& group_name,
    const std::vector<double>& positions) {
  const auto maybe_group_info = bundle.scene->getJointGroupInfo(group_name);
  if (!maybe_group_info) {
    throw std::runtime_error(
        "Failed to set joint positions for group '" + group_name + "': " +
        maybe_group_info.error());
  }
  const auto& group_info = *maybe_group_info;
  if (positions.size() == static_cast<size_t>(group_info.q_indices.size())) {
    return vector_from_std(positions);
  }
  if (positions.size() != group_info.joint_names.size()) {
    throw std::runtime_error(
        "Failed to set joint positions for group '" + group_name +
        "': expected either " +
        std::to_string(static_cast<size_t>(group_info.q_indices.size())) +
        " compact commandable positions or " +
        std::to_string(group_info.joint_names.size()) +
        " mimic-expanded group positions, got " +
        std::to_string(positions.size()));
  }

  Eigen::VectorXd compact(group_info.q_indices.size());
  Eigen::Index compact_index = 0;
  for (size_t i = 0; i < group_info.joint_names.size(); ++i) {
    const auto maybe_joint_info =
        bundle.scene->getJointInfo(group_info.joint_names[i]);
    if (!maybe_joint_info) {
      throw std::runtime_error(
          "Failed to set joint positions for group '" + group_name +
          "': " + maybe_joint_info.error());
    }
    if (maybe_joint_info->mimic_info.has_value()) {
      continue;
    }
    if (maybe_joint_info->num_position_dofs != 1) {
      throw std::runtime_error(
          "Failed to set joint positions for group '" + group_name +
          "': expanded public input is only supported for scalar joints");
    }
    if (compact_index >= compact.size()) {
      throw std::runtime_error(
          "Failed to set joint positions for group '" + group_name +
          "': expanded input contains more commandable joints than q slots");
    }
    compact[compact_index++] = positions[i];
  }
  if (compact_index != compact.size()) {
    throw std::runtime_error(
        "Failed to set joint positions for group '" + group_name +
        "': expanded input did not cover every commandable q slot");
  }
  return compact;
}

py::dict error_details_to_python(
    const std::map<std::string, std::string>& details) {
  py::dict d;
  for (const auto& [key, value] : details) {
    d[py::cast(key)] = value;
  }
  return d;
}

py::dict error_result_to_python(const ErrorDTO& error) {
  py::dict result;
  result["ok"] = false;
  result["error"] = error.message;
  result["error_code"] = error.code;
  result["error_details"] = error_details_to_python(error.details);
  return result;
}

const char* collision_engine_status_to_string(CollisionEngineStatus status) {
  switch (status) {
    case CollisionEngineStatus::kOk:
      return "ok";
    case CollisionEngineStatus::kTimeout:
      return "timeout";
    case CollisionEngineStatus::kUnsupportedGeometry:
      return "unsupported_geometry";
    case CollisionEngineStatus::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
    case CollisionEngineStatus::kCancelled:
      return "cancelled";
    case CollisionEngineStatus::kEngineError:
      return "engine_error";
  }
  return "unknown";
}

const char* collision_decision_to_string(CollisionDecisionResult decision) {
  switch (decision) {
    case CollisionDecisionResult::kPass:
      return "pass";
    case CollisionDecisionResult::kFail:
      return "fail";
    case CollisionDecisionResult::kWarn:
      return "warn";
    case CollisionDecisionResult::kStopRequested:
      return "stop_requested";
    case CollisionDecisionResult::kInconclusiveFail:
      return "inconclusive_fail";
  }
  return "unknown";
}

const char* collision_pair_classification_to_string(
    CollisionPairClassification classification) {
  switch (classification) {
    case CollisionPairClassification::kBlockingCollision:
      return "blocking_collision";
    case CollisionPairClassification::kAllowedStaticStatic:
      return "allowed_static_static";
    case CollisionPairClassification::kSupportContactStart:
      return "support_contact_start";
    case CollisionPairClassification::kSupportContactContinued:
      return "support_contact_continued";
    case CollisionPairClassification::kSupportContactSeparated:
      return "support_contact_separated";
    case CollisionPairClassification::kSupportContactRecontactViolation:
      return "support_contact_recontact_violation";
    case CollisionPairClassification::kAllowedTouchLink:
      return "allowed_touch_link";
    case CollisionPairClassification::kAllowedContactGrasp:
      return "allowed_contact_grasp";
    case CollisionPairClassification::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
  }
  return "unknown";
}

py::dict collision_pair_to_python(const CollisionPairId& pair) {
  py::dict d;
  d["body_a"] = pair.body_a;
  d["body_b"] = pair.body_b;
  return d;
}

py::dict collision_result_to_python(const CollisionResult& result) {
  py::dict d;
  d["in_collision"] = result.in_collision;
  d["engine_id"] = result.engine_id;
  d["engine_version"] = result.engine_version;
  d["engine_status"] =
      collision_engine_status_to_string(result.engine_status);
  d["decision"] = collision_decision_to_string(result.decision);
  d["query_duration_ns"] =
      result.query_duration_ns.has_value()
          ? py::cast(*result.query_duration_ns)
          : py::none();
  d["contact_body_a"] = result.contact_body_a.has_value()
                            ? py::cast(*result.contact_body_a)
                            : py::none();
  d["contact_body_b"] = result.contact_body_b.has_value()
                            ? py::cast(*result.contact_body_b)
                            : py::none();
  if (result.collision_world_revision.has_value()) {
    py::dict revision;
    revision["revision_id"] = result.collision_world_revision->revision_id;
    revision["content_hash"] = result.collision_world_revision->content_hash;
    d["collision_world_revision"] = revision;
  } else {
    d["collision_world_revision"] = py::none();
  }

  py::list pairs;
  for (const auto& pair : result.normalized_pairs) {
    pairs.append(collision_pair_to_python(pair));
  }
  d["normalized_pairs"] = pairs;

  py::list classified;
  for (const auto& pair_result : result.normalized_pairs_classified) {
    py::dict item;
    item["pair"] = collision_pair_to_python(pair_result.pair);
    item["classification"] =
        collision_pair_classification_to_string(pair_result.classification);
    item["is_first_failure"] = pair_result.is_first_failure;
    classified.append(item);
  }
  d["normalized_pairs_classified"] = classified;

  py::list clearances;
  for (const auto& clearance : result.pair_clearances) {
    py::dict item;
    item["pair"] = collision_pair_to_python(clearance.pair);
    item["signed_clearance_m"] = clearance.signed_clearance_m;
    clearances.append(item);
  }
  d["pair_clearances"] = clearances;
  d["min_clearance_m"] = result.min_clearance_m.has_value()
                             ? py::cast(*result.min_clearance_m)
                             : py::none();
  if (result.min_clearance_pair.has_value()) {
    d["min_clearance_pair"] =
        collision_pair_to_python(*result.min_clearance_pair);
  } else {
    d["min_clearance_pair"] = py::none();
  }
  d["raw_diagnostics"] = error_details_to_python(result.raw_diagnostics);
  return d;
}

// Helper: convert Result<uint64_t> to Python dict
py::dict result_to_python(const Result<uint64_t>& result) {
  py::dict d;
  d["ok"] = result.ok();
  if (result.ok()) {
    d["execution_id"] = result.value();
  } else {
    d["error"] = result.error().message;
  }
  return d;
}

// Helper: convert ExecutionStatus to Python dict
py::dict status_to_python(const ExecutionStatus& status) {
  py::dict d;
  d["state"] = static_cast<int>(status.state);
  d["state_name"] = to_string(status.state);
  d["execution_id"] = status.execution_id;
  d["progress"] = status.progress;
  d["active_move_id"] = status.active_move_id;
  d["last_error"] = to_string(status.last_error);
  return d;
}

// Helper: convert ExecutionSnapshot to Python dict
py::dict snapshot_to_python(const ExecutionSnapshot& snapshot) {
  py::dict d;
  d["timestamp_ns"] = snapshot.timestamp_ns;
  d["sequence_number"] = snapshot.sequence_number;
  d["execution_id"] = snapshot.execution_id;
  d["move_id"] = snapshot.move_id;
  d["state"] = static_cast<int>(snapshot.state);
  d["state_name"] = to_string(snapshot.state);
  d["progress"] = snapshot.progress;
  d["joint_positions_rad"] = snapshot.joint_positions_rad;
  d["state_source"] = snapshot.state_source;
  d["source_timestamp_ns"] = snapshot.source_timestamp_ns;
  d["source_age_ns"] = snapshot.source_age_ns;
  if (snapshot.error.has_value()) {
    d["error"] = snapshot.error->message;
    d["error_code"] = snapshot.error->code;
    d["error_details"] = error_details_to_python(snapshot.error->details);
  } else {
    d["error"] = py::none();
    d["error_code"] = py::none();
    d["error_details"] = py::dict();
  }
  if (snapshot.action_span) {
    d["action_span"] = action_span_to_python(*snapshot.action_span);
  } else {
    d["action_span"] = py::none();
  }
  if (!snapshot.contact_policy_id.empty()) {
    d["contact_policy_id"] = snapshot.contact_policy_id;
  } else {
    d["contact_policy_id"] = py::none();
  }
  return d;
}

// Helper: convert Pose3DTO to Python dict
py::dict pose_to_python(const Pose3DTO& pose) {
  py::dict d;
  d["position_m"] = py::make_tuple(
      pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m);
  d["orientation_xyzw"] = py::make_tuple(
      pose.orientation.x, pose.orientation.y,
      pose.orientation.z, pose.orientation.w);
  return d;
}

Pose3DTO multiply_poses_for_direct_plan(const Pose3DTO& a, const Pose3DTO& b) {
  auto pose_to_matrix = [](const Pose3DTO& pose) {
    Eigen::Matrix4d matrix = Eigen::Matrix4d::Identity();
    const double x = pose.orientation.x;
    const double y = pose.orientation.y;
    const double z = pose.orientation.z;
    const double w = pose.orientation.w;
    matrix(0, 0) = 1 - 2 * y * y - 2 * z * z;
    matrix(0, 1) = 2 * x * y - 2 * z * w;
    matrix(0, 2) = 2 * x * z + 2 * y * w;
    matrix(1, 0) = 2 * x * y + 2 * z * w;
    matrix(1, 1) = 1 - 2 * x * x - 2 * z * z;
    matrix(1, 2) = 2 * y * z - 2 * x * w;
    matrix(2, 0) = 2 * x * z - 2 * y * w;
    matrix(2, 1) = 2 * y * z + 2 * x * w;
    matrix(2, 2) = 1 - 2 * x * x - 2 * y * y;
    matrix(0, 3) = pose.position_m.x_m;
    matrix(1, 3) = pose.position_m.y_m;
    matrix(2, 3) = pose.position_m.z_m;
    return matrix;
  };

  auto matrix_to_pose = [](const Eigen::Matrix4d& matrix) {
    Pose3DTO pose;
    pose.position_m = {matrix(0, 3), matrix(1, 3), matrix(2, 3)};

    const Eigen::Matrix3d rotation = matrix.block<3, 3>(0, 0);
    Eigen::Quaterniond quaternion(rotation);
    quaternion.normalize();
    pose.orientation = {
        quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w()};
    return pose;
  };

  return matrix_to_pose(pose_to_matrix(a) * pose_to_matrix(b));
}

Pose3DTO pose_with_position_and_orientation_for_direct_plan(
    const Vector3DTO& position,
    const QuaternionDTO& orientation) {
  Pose3DTO pose;
  pose.position_m = position;
  pose.orientation = orientation;
  return pose;
}

std::optional<ErrorDTO> resolve_motion_spec_for_direct_plan(
    NativeKernelBundle& bundle,
    const ExecuteRequestDTO& request,
    const SceneRevisionBindingDTO& scene_binding,
    MotionSpecDTO& resolved_motion) {
  if (const auto* relative_move = std::get_if<RelativeMoveDTO>(&request.motion)) {
    auto target_pose = bundle.reference_resolver->resolve_reference_pose(
        relative_move->target_reference, request.base_frame, scene_binding);
    if (!target_pose.ok()) {
      return target_pose.error();
    }

    Pose3DTO offset_basis_pose = target_pose.value();
    if (relative_move->offset_reference.has_value()) {
      auto offset_reference_pose =
          bundle.reference_resolver->resolve_reference_pose(
              *relative_move->offset_reference, request.base_frame,
              scene_binding);
      if (!offset_reference_pose.ok()) {
        return offset_reference_pose.error();
      }
      offset_basis_pose = offset_reference_pose.value();
    }

    PoseGoalDTO pose_goal;
    pose_goal.goal = multiply_poses_for_direct_plan(
        pose_with_position_and_orientation_for_direct_plan(
            target_pose.value().position_m,
            offset_basis_pose.orientation),
        relative_move->offset);
    resolved_motion = MotionSpecDTO{pose_goal};
    return std::nullopt;
  }

  resolved_motion = request.motion;
  return std::nullopt;
}

ValidationResultDTO validate_direct_planned_trajectory(
    NativeKernelBundle& bundle, const TrajectoryDTO& trajectory,
    const ExecuteRequestDTO& request,
    const CollisionWorldLeaseInventory& retained_scene) {
  ValidationOptionsDTO options;
  options.tcp_frame = request.tcp_frame;
  options.base_frame = request.base_frame;
  options.max_tcp_linear_speed_mps =
      request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
  options.run_collision_checks = false;

  CollisionCancellationToken cancel;
  auto noncollision = bundle.trajectory_validator->validate_non_collision(
      trajectory, options, cancel);
  if (!noncollision.valid) {
    return noncollision;
  }

  auto observed = bundle.observed_state_provider->current_state(
      bundle.clock->now());
  if (!observed.ok()) {
    return ValidationResultDTO{false, observed.error()};
  }
  auto projected = bundle.collision_trajectory_projector->expand_for_collision(
      trajectory, observed.value());
  if (!projected.ok()) {
    return ValidationResultDTO{false, projected.error()};
  }
  if (!retained_scene.bundle.world ||
      !retained_scene.bundle.policy.has_value()) {
    return ValidationResultDTO{
        false,
        ErrorDTO{kErrorExecutionRejected,
                 "Retained collision scene is incomplete",
                 {{"reason", "direct_validation_scene_incomplete"}}}};
  }
  if (bundle.collision_validation_engines.empty() ||
      !bundle.collision_validation_engines.front()) {
    return ValidationResultDTO{
        false,
        ErrorDTO{kErrorExecutionRejected,
                 "Authoritative collision validation engine is unavailable",
                 {{"reason", "direct_validation_engine_unavailable"}}}};
  }

  CollisionValidationPassConfig config;
  config.policy_snapshot = *retained_scene.bundle.policy;
  config.require_pose_source_borrow = false;
  config.surface = CollisionSurface::kPreExecutionValidation;
  config.contact_policy_id = request.contact_policy_id;
  CollisionValidationCoordinator coordinator(std::move(config));
  auto collision = coordinator.validate_trajectory(
      *retained_scene.bundle.world,
      *bundle.collision_validation_engines.front(), projected.value(), cancel);
  if (!collision.decision.accepted) {
    std::map<std::string, std::string> details{
        {"reason", "direct_authoritative_collision_validation_failed"},
        {"collision_world_revision_id",
         std::to_string(collision.borrowed_world_revision.revision_id)},
        {"collision_world_content_hash",
         collision.borrowed_world_revision.content_hash}};
    if (collision.decision.first_failing_waypoint.has_value()) {
      details["first_failing_waypoint"] = std::to_string(
          *collision.decision.first_failing_waypoint);
    }
    if (collision.decision.first_failing_time_ns.has_value()) {
      details["first_failing_time_ns"] =
          std::to_string(*collision.decision.first_failing_time_ns);
    }
    append_collision_failure_diagnostics(details, collision);
    return ValidationResultDTO{
        false,
        ErrorDTO{kErrorValidationCollision,
                 pre_dispatch_collision_validation_message(
                     details, collision.decision.action),
                 std::move(details)}};
  }
  return ValidationResultDTO{true, std::nullopt};
}

Pose3DTO pose_from_python(const py::dict& pose_dict) {
  Pose3DTO pose{};
  pose.position_m = {0.0, 0.0, 0.0};
  pose.orientation = {0.0, 0.0, 0.0, 1.0};
  if (pose_dict.contains("position_m")) {
    auto pos = pose_dict["position_m"].cast<std::vector<double>>();
    if (pos.size() != 3) {
      throw std::invalid_argument("position_m must have exactly 3 elements");
    }
    pose.position_m = {pos[0], pos[1], pos[2]};
  }
  if (pose_dict.contains("orientation_xyzw")) {
    auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
    if (orient.size() != 4) {
      throw std::invalid_argument("orientation_xyzw must have exactly 4 elements");
    }
    pose.orientation = {orient[0], orient[1], orient[2], orient[3]};
  }
  return pose;
}

std::vector<SupportContactDTO> support_contacts_from_python(const py::dict& request) {
  std::vector<SupportContactDTO> contacts;
  if (!request.contains("support_contacts") || request["support_contacts"].is_none()) {
    return contacts;
  }
  if (!py::isinstance<py::list>(request["support_contacts"])) {
    throw std::invalid_argument("support_contacts must be a list");
  }
  for (const auto item : request["support_contacts"].cast<py::list>()) {
    if (!py::isinstance<py::dict>(item)) {
      throw std::invalid_argument("support_contacts entries must be dictionaries");
    }
    const py::dict contact_dict = py::reinterpret_borrow<py::dict>(item);
    if (!contact_dict.contains("support_object_id")) {
      throw std::invalid_argument(
          "support_contacts entries require support_object_id");
    }
    SupportContactDTO contact;
    contact.support_object_id =
        contact_dict["support_object_id"].cast<std::string>();
    if (contact.support_object_id.empty()) {
      throw std::invalid_argument("support_object_id must not be empty");
    }
    auto nonnegative_finite = [](double value, const char* field) {
      if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(std::string(field) + " must be finite and >= 0");
      }
    };
    if (contact_dict.contains("mode")) {
      const auto mode = contact_dict["mode"].cast<std::string>();
      if (mode == "contact_release") {
        contact.mode = CollisionContactPolicyMode::kContactRelease;
      } else if (mode == "contact_process") {
        contact.mode = CollisionContactPolicyMode::kContactProcess;
      } else {
        throw std::invalid_argument(
            "support_contacts mode must be contact_release or contact_process");
      }
    }
    if (contact_dict.contains("enforce_max_start_penetration")) {
      contact.enforce_max_start_penetration =
          contact_dict["enforce_max_start_penetration"].cast<bool>();
    }
    if (contact_dict.contains("max_start_penetration_m")) {
      contact.max_start_penetration_m =
          contact_dict["max_start_penetration_m"].cast<double>();
      nonnegative_finite(contact.max_start_penetration_m,
                         "max_start_penetration_m");
    }
    if (contact_dict.contains("release_tolerance_m")) {
      contact.release_tolerance_m =
          contact_dict["release_tolerance_m"].cast<double>();
      nonnegative_finite(contact.release_tolerance_m, "release_tolerance_m");
    }
    if (contact_dict.contains("min_release_progress_m")) {
      contact.min_release_progress_m =
          contact_dict["min_release_progress_m"].cast<double>();
      nonnegative_finite(contact.min_release_progress_m,
                         "min_release_progress_m");
    }
    if (contact_dict.contains("min_separation_clearance_m")) {
      contact.min_separation_clearance_m =
          contact_dict["min_separation_clearance_m"].cast<double>();
      nonnegative_finite(contact.min_separation_clearance_m,
                         "min_separation_clearance_m");
    }
    if (contact_dict.contains("require_separation_by_end")) {
      contact.require_separation_by_end =
          contact_dict["require_separation_by_end"].cast<bool>();
    }
    if (contact_dict.contains("require_strictly_decreasing_penetration")) {
      contact.require_strictly_decreasing_penetration =
          contact_dict["require_strictly_decreasing_penetration"].cast<bool>();
    }
    if (contact_dict.contains("allow_recontact")) {
      contact.allow_recontact =
          contact_dict["allow_recontact"].cast<bool>();
    }
    contacts.push_back(std::move(contact));
  }
  return contacts;
}

py::dict support_contact_to_python(const SupportContactDTO& contact) {
  py::dict d;
  d["support_object_id"] = contact.support_object_id;
  d["mode"] = contact.mode == CollisionContactPolicyMode::kContactProcess
                  ? "contact_process"
                  : "contact_release";
  d["enforce_max_start_penetration"] = contact.enforce_max_start_penetration;
  d["max_start_penetration_m"] = contact.max_start_penetration_m;
  d["release_tolerance_m"] = contact.release_tolerance_m;
  d["min_release_progress_m"] = contact.min_release_progress_m;
  d["min_separation_clearance_m"] = contact.min_separation_clearance_m;
  d["require_separation_by_end"] = contact.require_separation_by_end;
  d["require_strictly_decreasing_penetration"] =
      contact.require_strictly_decreasing_penetration;
  d["allow_recontact"] = contact.allow_recontact;
  return d;
}

py::list support_contacts_to_python(
    const std::vector<SupportContactDTO>& contacts) {
  py::list out;
  for (const auto& contact : contacts) {
    out.append(support_contact_to_python(contact));
  }
  return out;
}

// Helper: convert RobotStateDTO to Python dict
py::dict robot_state_to_python(const RobotStateDTO& state) {
  py::dict d;
  d["joint_names"] = state.joint_names;
  d["positions_rad"] = state.positions_rad;
  d["velocities_rad_s"] = state.velocities_rad_s;
  return d;
}

RobotStateDTO observer_robot_state(NativeKernelBundle& self) {
  if (!self.observed_state_provider || !self.clock) {
    throw std::runtime_error("Observed robot-state authority is unavailable");
  }
  auto state = self.observed_state_provider->current_state(self.clock->now());
  if (!state.ok()) {
    throw std::runtime_error(state.error().message);
  }
  return std::move(state.value());
}

std::vector<std::string> qualified_group_joint_names(
    NativeKernelBundle& self, const std::string& group_id) {
  if (!self.kinematics) {
    throw std::runtime_error("Robot kinematics authority is unavailable");
  }
  auto names = self.kinematics->group_joint_names(group_id);
  if (!names.ok()) {
    throw std::runtime_error(names.error().message);
  }
  return std::move(names.value());
}

py::dict trajectory_to_python(const TrajectoryDTO& trajectory) {
  py::dict d;
  d["joint_names"] = trajectory.joint_names;
  py::list waypoints;
  for (const auto& wp : trajectory.waypoints) {
    py::dict wp_dict;
    wp_dict["time_from_start_s"] = wp.time_from_start_s;
    wp_dict["positions_rad"] = wp.positions_rad;
    wp_dict["velocities_rad_s"] = wp.velocities_rad_s;
    wp_dict["accelerations_rad_s2"] = wp.accelerations_rad_s2;
    waypoints.append(wp_dict);
  }
  d["waypoints"] = waypoints;
  return d;
}

template <typename T>
py::object optional_to_python(const std::optional<T>& value) {
  return value.has_value() ? py::cast(*value) : py::none();
}

py::dict reference_to_python(const ReferenceDTO& reference) {
  py::dict d;
  switch (reference.type) {
    case ReferenceDTO::Type::kFrame:
      d["type"] = "frame";
      break;
    case ReferenceDTO::Type::kObject:
      d["type"] = "object";
      break;
    case ReferenceDTO::Type::kRobotLink:
      d["type"] = "robot_link";
      break;
    case ReferenceDTO::Type::kPose:
      d["type"] = "pose";
      break;
  }
  d["id"] = reference.id;
  if (reference.pose.has_value()) {
    d["pose"] = pose_to_python(*reference.pose);
  }
  return d;
}

py::dict motion_spec_to_python(const MotionSpecDTO& motion) {
  py::dict d;
  if (const auto* joint = std::get_if<JointGoalDTO>(&motion)) {
    d["type"] = "joint_goal";
    d["joint_names"] = joint->joint_names;
    d["positions_rad"] = joint->positions_rad;
    return d;
  }
  if (const auto* pose = std::get_if<PoseGoalDTO>(&motion)) {
    d["type"] = "pose_goal";
    d["pose"] = pose_to_python(pose->goal);
    return d;
  }
  if (const auto* named = std::get_if<NamedTargetDTO>(&motion)) {
    d["type"] = "named_target";
    d["name"] = named->name;
    return d;
  }
  if (const auto* relative = std::get_if<RelativeMoveDTO>(&motion)) {
    d["type"] = "relative_move";
    d["target_reference"] = reference_to_python(relative->target_reference);
    d["offset"] = pose_to_python(relative->offset);
    if (relative->offset_reference.has_value()) {
      d["offset_reference"] =
          reference_to_python(*relative->offset_reference);
    } else {
      d["offset_reference"] = py::none();
    }
    return d;
  }
  const auto& cartesian = std::get<CartesianPathDTO>(motion);
  d["type"] = "cartesian_path";
  py::list waypoints;
  for (const auto& waypoint : cartesian.waypoints) {
    waypoints.append(pose_to_python(waypoint));
  }
  d["waypoints"] = std::move(waypoints);
  d["max_translation_step_m"] = cartesian.max_translation_step_m;
  d["max_rotation_step_rad"] = cartesian.max_rotation_step_rad;
  return d;
}

py::dict plan_request_to_python(const PlanRequestDTO& request) {
  py::dict d;
  d["robot_id"] = request.robot_id;
  d["group_id"] = request.group_id;
  d["move_id"] = request.move_id;
  d["base_frame"] = request.base_frame;
  d["tcp_frame"] = request.tcp_frame;
  d["start_state"] = robot_state_to_python(request.start_state);
  d["motion"] = motion_spec_to_python(request.motion);
  d["tolerances"] = py::dict(
      "joint_position_tol_rad"_a =
          request.tolerances.joint_position_tol_rad,
      "translation_tol_m"_a = request.tolerances.translation_tol_m,
      "rotation_tol_rad"_a = request.tolerances.rotation_tol_rad);
  py::dict speed_profile;
  speed_profile["velocity_scale"] = request.speed_profile.velocity_scale;
  speed_profile["acceleration_scale"] =
      request.speed_profile.acceleration_scale;
  speed_profile["max_tcp_linear_speed_mps"] =
      optional_to_python(request.speed_profile.max_tcp_linear_speed_mps);
  d["speed_profile"] = std::move(speed_profile);
  d["planning_timeout_s"] = request.planning_timeout_s;
  d["planning_attempt_index"] = request.planning_attempt_index;
  py::dict limits;
  for (const auto& [joint_name, limit] : request.per_joint_dynamics_limits) {
    py::dict value;
    value["max_velocity_rad_s"] =
        optional_to_python(limit.max_velocity_rad_s);
    value["max_acceleration_rad_s2"] =
        optional_to_python(limit.max_acceleration_rad_s2);
    value["max_jerk_rad_s3"] = optional_to_python(limit.max_jerk_rad_s3);
    limits[py::str(joint_name)] = std::move(value);
  }
  d["per_joint_dynamics_limits"] = std::move(limits);
  d["contact_policy_id"] = request.contact_policy_id;
  d["action_span"] = request.action_span.has_value()
                         ? py::object(action_span_to_python(*request.action_span))
                         : py::object(py::none());
  d["scene_binding"] = py::dict(
      "revision_id"_a = request.scene_binding.revision.revision_id,
      "content_hash"_a = request.scene_binding.revision.content_hash,
      "projection_lease_id"_a = request.scene_binding.projection_lease_id);
  return d;
}

py::dict planning_metrics_to_python(
    const RoboPlanPlanningPipelineMetrics& metrics) {
  py::dict d;
  d["total_duration_ns"] = metrics.total_duration_ns;
  d["total_collision_query_count"] = metrics.total_collision_query_count;
  d["total_collision_query_duration_ns"] =
      metrics.total_collision_query_duration_ns;
  d["collision_probe_available"] = metrics.collision_probe_available;
  d["measurement_complete"] = metrics.measurement_complete;
  d["succeeded"] = metrics.succeeded;
  d["error_code"] = metrics.error_code;
  d["waypoint_count"] = metrics.waypoint_count;
  d["trajectory_duration_s"] = metrics.trajectory_duration_s;
  d["rng_base_seed"] = optional_to_python(metrics.rng_base_seed);
  d["rng_seed"] = optional_to_python(metrics.rng_seed);
  d["shortcut_rng_seed_argument"] =
      optional_to_python(metrics.shortcut_rng_seed_argument);
  d["planning_attempt_index"] = metrics.planning_attempt_index;
  py::dict stages;
  for (const auto& [name, stage] : metrics.stages) {
    stages[py::str(name)] = py::dict(
        "duration_ns"_a = stage.duration_ns,
        "collision_query_count"_a = stage.collision_query_count,
        "collision_query_duration_ns"_a =
            stage.collision_query_duration_ns);
  }
  d["stages"] = std::move(stages);
  return d;
}

py::dict planning_qualification_record_to_python(
    const RoboPlanPlanningQualificationRecord& record) {
  py::dict d;
  d["sequence"] = record.sequence;
  d["request"] = plan_request_to_python(record.request);
  py::dict result;
  result["scene_revision"] = py::dict(
      "revision_id"_a = record.result.scene_revision.revision_id,
      "content_hash"_a = record.result.scene_revision.content_hash);
  result["trajectory"] = record.result.trajectory.has_value()
                             ? py::object(trajectory_to_python(
                                   *record.result.trajectory))
                             : py::object(py::none());
  if (record.result.error.has_value()) {
    result["error"] = py::dict(
        "code"_a = record.result.error->code,
        "message"_a = record.result.error->message,
        "details"_a = error_details_to_python(record.result.error->details));
  } else {
    result["error"] = py::none();
  }
  d["result"] = std::move(result);
  d["metrics"] = planning_metrics_to_python(record.metrics);
  return d;
}

// Helper: convert ObjectStateDTO to Python dict
py::dict object_state_to_python(const ObjectStateDTO& state) {
  py::dict d;
  d["object_id"] = state.object_id;
  d["exists"] = state.exists;
  d["attached"] = state.attached;
  if (state.frame_id.has_value()) {
    d["frame_id"] = *state.frame_id;
  } else {
    d["frame_id"] = py::none();
  }
  return d;
}

py::dict scene_trace_event_to_python(
    const RoboPlanProjectionBackend::SceneTraceEvent& event) {
  py::dict d;
  d["timestamp_ns"] = event.timestamp_ns;
  d["source"] = "backend";
  d["event"] = event.event;
  d["object_id"] = event.object_id;
  d["success"] = event.success;
  if (event.frame_id.has_value()) {
    d["frame_id"] = *event.frame_id;
  } else {
    d["frame_id"] = py::none();
  }
  d["attached"] = event.attached;
  if (event.pose_in_frame.has_value()) {
    d["pose_in_frame"] = pose_to_python(*event.pose_in_frame);
  } else {
    d["pose_in_frame"] = py::none();
  }
  if (event.pose_in_universe.has_value()) {
    d["pose_in_universe"] = pose_to_python(*event.pose_in_universe);
  } else {
    d["pose_in_universe"] = py::none();
  }
  if (event.error.has_value()) {
    d["error"] = *event.error;
  } else {
    d["error"] = py::none();
  }
  return d;
}

// Helper: KernelEventType to string
const char* kernel_event_type_to_string(KernelEventType type) {
  switch (type) {
    case KernelEventType::kStateTransition: return "state_transition";
    case KernelEventType::kPlanStarted: return "plan_started";
    case KernelEventType::kPlanCompleted: return "plan_completed";
    case KernelEventType::kCacheHit: return "cache_hit";
    case KernelEventType::kCacheMiss: return "cache_miss";
    case KernelEventType::kCacheInvalidated: return "cache_invalidated";
    case KernelEventType::kValidationStarted: return "validation_started";
    case KernelEventType::kValidationPassed: return "validation_passed";
    case KernelEventType::kValidationFailed: return "validation_failed";
    case KernelEventType::kExecuteStarted: return "execute_started";
    case KernelEventType::kExecuteCompleted: return "execute_completed";
    case KernelEventType::kStopRequested: return "stop_requested";
    case KernelEventType::kStopIssued: return "stop_issued";
    case KernelEventType::kPauseRequested: return "pause_requested";
    case KernelEventType::kResumeRequested: return "resume_requested";
    case KernelEventType::kRuntimeVelocityScaleChanged:
      return "runtime_velocity_scale_changed";
    case KernelEventType::kCompletedAcknowledged: return "acknowledge_completed";
    case KernelEventType::kFailedValidationAcknowledged:
      return "acknowledge_failed_validation";
    case KernelEventType::kInterventionAcknowledged: return "acknowledge_intervention";
    case KernelEventType::kErrorRaised: return "error_raised";
    case KernelEventType::kCollisionDetected: return "collision_detected";
    case KernelEventType::kInterruptSignalIngested: return "interrupt_signal_ingested";
    case KernelEventType::kInterruptSignalRejectedStale: return "interrupt_signal_rejected_stale";
    case KernelEventType::kInterruptSignalRejectedClassViolation:
      return "interrupt_signal_rejected_class_violation";
    case KernelEventType::kInterruptSignalRejected: return "interrupt_signal_rejected";
    case KernelEventType::kInterruptLatched: return "interrupt_latched";
    case KernelEventType::kInterruptSuppressedByActiveMask:
      return "interrupt_suppressed_by_active_mask";
    case KernelEventType::kInterruptQueued: return "interrupt_queued";
    case KernelEventType::kInterruptDroppedQueueOverflow:
      return "interrupt_dropped_queue_overflow";
    case KernelEventType::kInterruptPreemptApplied: return "interrupt_preempt_applied";
    case KernelEventType::kInterruptDispositionApplied:
      return "interrupt_disposition_applied";
    case KernelEventType::kInterruptCleared: return "interrupt_cleared";
    case KernelEventType::kInterruptIngestOverflow: return "interrupt_ingest_overflow";
    case KernelEventType::kInterruptSourceClassViolation:
      return "interrupt_source_class_violation";
    case KernelEventType::kInterruptRecoveryHandlerStarted:
      return "interrupt_recovery_handler_started";
    case KernelEventType::kInterruptRecoveryHandlerCompleted:
      return "interrupt_recovery_handler_completed";
    case KernelEventType::kMotionInterruptScopeArmed:
      return "motion_interrupt_scope_armed";
    case KernelEventType::kMotionInterruptScopeReaped:
      return "motion_interrupt_scope_reaped";
    case KernelEventType::kMotionInterruptArmFailed:
      return "motion_interrupt_arm_failed";
    case KernelEventType::kMotionInterruptQueuedUpdateDropped:
      return "motion_interrupt_queued_update_dropped";
    case KernelEventType::kMotionTripSecondarySignal:
      return "motion_trip_secondary_signal";
    case KernelEventType::kMotionTripFailed: return "motion_trip_failed";
    case KernelEventType::kMotionTripCompleted: return "motion_trip_completed";
    case KernelEventType::kMotionExecutionOutcomeRecorded:
      return "motion_execution_outcome_recorded";
    case KernelEventType::kStopCertificatePublished:
      return "stop_certificate_published";
    case KernelEventType::kRecoveryAssessmentChanged:
      return "recovery_assessment_changed";
    case KernelEventType::kRecoveryActionAccepted:
      return "recovery_action_accepted";
    case KernelEventType::kRecoveryActionRejected:
      return "recovery_action_rejected";
    case KernelEventType::kRuntimeCollisionCoverage:
      return "runtime_collision_coverage";
    default: return "unknown";
  }
}

py::dict recovery_assessment_to_python(
    const KernelRecoveryAssessmentDTO& assessment) {
  py::dict d;
  d["schema_version"] = assessment.schema_version;
  d["assessment_revision"] = assessment.assessment_revision;
  d["assessed_at_ns"] = assessment.assessed_at_ns;
  d["control_command_epoch"] = assessment.control_command_epoch;
  d["execution_id"] = assessment.execution_id;
  d["execution_state"] = to_string(assessment.execution_state);
  d["recovery_class"] = recovery_class_name(assessment.recovery_class);
  d["status"] = recovery_assessment_status_name(assessment.status);
  d["latch_id"] = assessment.latch_id;
  d["source_id"] = assessment.source_id;
  d["policy_id"] = assessment.policy_id;
  d["preemption_result"] =
      interrupt_preemption_result_name(assessment.preemption_result);
  d["stop_owner_active"] = assessment.stop_owner_active;
  d["stop_owner_generation"] = assessment.stop_owner_generation;
  d["backend_stop_confirmed"] = assessment.backend_stop_confirmed;
  d["stop_certificate_id"] = assessment.stop_certificate_id;
  d["observed_state_available"] = assessment.observed_state_available;
  d["observed_state_sequence"] = assessment.observed_state_sequence;
  d["observed_state_source_id"] = assessment.observed_state_source_id;
  d["observed_state_authority_profile"] =
      assessment.observed_state_authority_profile;
  d["observed_state_measured"] = assessment.observed_state_measured;
  d["observed_state_valid_until_ns"] =
      assessment.observed_state_valid_until_ns;
  d["observed_state_fresh"] = assessment.observed_state_fresh;
  d["observed_state_at_rest"] = assessment.observed_state_at_rest;
  d["stronger_authority_active"] = assessment.stronger_authority_active;
  d["queued_latch_present"] = assessment.queued_latch_present;
  d["retry_admissible"] = assessment.retry_admissible;
  py::list actions;
  for (const auto action : assessment.allowed_actions) {
    actions.append(std::string(recovery_action_name(action)));
  }
  d["allowed_actions"] = actions;
  d["reason_code"] = assessment.reason_code;
  py::list refs;
  for (const auto& ref : assessment.evidence_refs) {
    refs.append(ref);
  }
  d["evidence_refs"] = refs;
  return d;
}

// Helper: convert KernelEvent to Python dict
py::dict kernel_event_to_python(const KernelEvent& event) {
  py::dict d;
  d["type"] = kernel_event_type_to_string(event.type);
  d["execution_id"] = event.execution_id;
  d["timestamp_ns"] = event.timestamp_ns;
  d["sequence"] = event.sequence;
  if (event.type == KernelEventType::kStateTransition) {
    d["from_state"] = to_string(event.from_state);
    d["to_state"] = to_string(event.to_state);
  }
  if (event.error.has_value()) {
    py::dict err;
    err["code"] = event.error->code;
    err["message"] = event.error->message;
    err["details"] = error_details_to_python(event.error->details);
    d["error"] = err;
  }
  if (!event.details.empty()) {
    py::dict details;
    for (const auto& [key, value] : event.details) {
      details[py::cast(key)] = value;
    }
    d["details"] = details;
  }
  // Lift correlation fields to the top level for parity with the mock kernel's
  // event shape; durable history and UI filters join on these keys without
  // needing to parse the free-form details map.
  for (const auto* key :
       {"program_run_id",
        "action_source",
        "action_span_id",
        "action_span_program_run_id",
        "node_instance_id",
        "node_attempt_id",
        "parent_node_attempt_id",
        "contact_policy_id"}) {
    auto it = event.details.find(key);
    if (it != event.details.end()) {
      d[key] = it->second;
    }
  }
  return d;
}

// Helper: convert stop/pause/resume/reset results
py::dict command_result(bool accepted, const std::optional<ErrorDTO>& error) {
  py::dict d;
  d["accepted"] = accepted;
  if (error.has_value()) {
    d["error"] = error->message;
    d["error_code"] = error->code;
    d["error_details"] = error_details_to_python(error->details);
  } else {
    d["error_code"] = py::none();
    d["error_details"] = py::dict();
  }
  return d;
}

py::dict gripper_command_result_to_python(
    const GripperCommandResultDTO& result) {
  py::dict d;
  d["accepted"] = result.accepted;
  d["command_id"] = result.command_id;
  d["generation"] = result.generation;
  if (result.error.has_value()) {
    d["error"] = result.error->message;
    d["reason_code"] = result.error->code;
    d["error_details"] = error_details_to_python(result.error->details);
  } else {
    d["error"] = py::none();
    d["reason_code"] = py::none();
    d["error_details"] = py::dict();
  }
  return d;
}

py::dict gripper_status_to_python(const GripperCommandStatusDTO& status) {
  py::dict d;
  d["state"] = gripper_command_state_name(status.state);
  d["gripper_id"] = status.gripper_id;
  d["command_id"] = status.command_id;
  d["generation"] = status.generation;
  d["revision"] = status.revision;
  d["progress"] = status.progress;
  d["current_position"] = status.current_position.has_value()
                              ? py::cast(*status.current_position)
                              : py::none();
  d["target_position"] = status.target_position.has_value()
                             ? py::cast(*status.target_position)
                             : py::none();
  d["commanded_position"] = status.commanded_position.has_value()
                                ? py::cast(*status.commanded_position)
                                : py::none();
  d["observed_position"] = status.observed_position.has_value()
                               ? py::cast(*status.observed_position)
                               : py::none();
  d["commanded_state"] = status.commanded_state;
  d["actuation_kind"] = status.actuation_kind;
  d["state_authority"] = status.state_authority;
  d["completion_policy"] = status.completion_policy;
  d["grasp_confirmation_policy"] = status.grasp_confirmation_policy;
  d["transport_kind"] = status.transport_kind;
  d["evidence"] = status.evidence;
  d["evidence_state"] = status.evidence_state;
  d["projection_state"] =
      gripper_projection_state_name(status.projection_state);
  d["projection_reason"] = status.projection_reason;
  if (status.error.has_value()) {
    d["error"] = status.error->message;
    d["reason_code"] = status.error->code;
    d["error_details"] = error_details_to_python(status.error->details);
  } else {
    d["error"] = py::none();
    d["reason_code"] = py::none();
    d["error_details"] = py::dict();
  }
  return d;
}

GripperCommandRequestDTO gripper_request_from_python(py::dict request) {
  GripperCommandRequestDTO dto;
  dto.gripper_id = request["gripper_id"].cast<std::string>();
  dto.target_position =
      finite_scale_from_python(request["target_position"], "target_position");
  dto.duration_s =
      finite_scale_from_python(request["duration_s"], "duration_s");
  if (request.contains("max_effort") && !request["max_effort"].is_none()) {
    dto.max_effort =
        finite_scale_from_python(request["max_effort"], "max_effort");
  }
  const auto optional_string = [&request](const char* key) {
    return request.contains(key) && !request[key].is_none()
               ? request[key].cast<std::string>()
               : std::string{};
  };
  dto.group_id = optional_string("group_id");
  dto.joint_name = optional_string("joint_name");
  dto.commanded_state = optional_string("commanded_state");
  dto.actuation_kind = optional_string("actuation_kind");
  dto.state_authority = optional_string("state_authority");
  dto.completion_policy = optional_string("completion_policy");
  dto.grasp_confirmation_policy =
      optional_string("grasp_confirmation_policy");
  dto.transport_kind = optional_string("transport_kind");
  dto.evidence_state = optional_string("evidence_state");
  if (request.contains("evidence") && !request["evidence"].is_none()) {
    dto.evidence = request["evidence"].cast<std::vector<std::string>>();
  }
  return dto;
}

std::optional<std::uint64_t> optional_uint64_from_python(py::object value) {
  if (value.is_none()) {
    return std::nullopt;
  }
  return value.cast<std::uint64_t>();
}

[[maybe_unused]] bool error_reason_is_stale_control_command(
    const std::optional<ErrorDTO>& error) {
  if (!error.has_value()) {
    return false;
  }
  const auto it = error->details.find("reason_code");
  return it != error->details.end() && it->second == "stale_control_command";
}

// Helper: convert MetricsSnapshot to Python dict
py::dict metrics_to_python(const MetricsSnapshot& metrics) {
  py::dict d;
  d["cache_hits"] = metrics.cache_hits;
  d["cache_misses"] = metrics.cache_misses;
  d["cache_invalidations"] = metrics.cache_invalidations;
  d["plans_succeeded"] = metrics.plans_succeeded;
  d["plans_failed"] = metrics.plans_failed;
  d["validations_passed"] = metrics.validations_passed;
  d["validations_failed"] = metrics.validations_failed;
  d["executions_completed"] = metrics.executions_completed;
  d["executions_failed"] = metrics.executions_failed;
  d["stops_issued"] = metrics.stops_issued;
  d["collision_detections"] = metrics.collision_detections;
  d["runtime_collision_ticks_started"] =
      metrics.runtime_collision_ticks_started;
  d["runtime_collision_ticks_completed"] =
      metrics.runtime_collision_ticks_completed;
  d["runtime_collision_ticks_faulted"] =
      metrics.runtime_collision_ticks_faulted;
  d["runtime_collision_tick_max_ns"] =
      metrics.runtime_collision_tick_max_ns;
  d["runtime_collision_tick_start_jitter_max_ns"] =
      metrics.runtime_collision_tick_start_jitter_max_ns;
  d["runtime_collision_tick_late_periods"] =
      metrics.runtime_collision_tick_late_periods;
  d["runtime_collision_tick_missed_periods"] =
      metrics.runtime_collision_tick_missed_periods;
  d["runtime_collision_tick_max_consecutive_late_periods"] =
      metrics.runtime_collision_tick_max_consecutive_late_periods;
  d["runtime_collision_stop_windows_started"] =
      metrics.runtime_collision_stop_windows_started;
  d["runtime_collision_stop_windows_completed"] =
      metrics.runtime_collision_stop_windows_completed;
  d["runtime_collision_stop_ticks_attempted"] =
      metrics.runtime_collision_stop_ticks_attempted;
  d["runtime_collision_stop_queries_executed"] =
      metrics.runtime_collision_stop_queries_executed;
  d["runtime_collision_stop_unavailable_ticks"] =
      metrics.runtime_collision_stop_unavailable_ticks;
  d["runtime_collision_stop_longest_unavailable_ns"] =
      metrics.runtime_collision_stop_longest_unavailable_ns;
  d["runtime_collision_stop_zero_check_windows"] =
      metrics.runtime_collision_stop_zero_check_windows;
  d["runtime_collision_last_stop_ticks_attempted"] =
      metrics.runtime_collision_last_stop_ticks_attempted;
  d["runtime_collision_last_stop_queries_executed"] =
      metrics.runtime_collision_last_stop_queries_executed;
  d["runtime_collision_last_stop_unavailable_ticks"] =
      metrics.runtime_collision_last_stop_unavailable_ticks;
  d["runtime_collision_last_stop_longest_unavailable_ns"] =
      metrics.runtime_collision_last_stop_longest_unavailable_ns;
  d["runtime_collision_freshest_observed_source_age_ns"] =
      metrics.runtime_collision_freshest_observed_source_age_ns;
  d["runtime_collision_freshest_observed_source_id"] =
      metrics.runtime_collision_freshest_observed_source_id;
  d["runtime_collision_unavailable_events_suppressed"] =
      metrics.runtime_collision_unavailable_events_suppressed;
  d["observed_state_reader_retry_exhausted_total"] =
      metrics.observed_state_reader_retry_exhausted_total;
  d["validation_deferred_runtime_monitor_active"] =
      metrics.validation_deferred_runtime_monitor_active;
  d["runtime_collision_snapshot_refresh_failures"] =
      metrics.runtime_collision_snapshot_refresh_failures;
  d["snapshots_published"] = metrics.snapshots_published;
  d["event_subscriber_exceptions"] = metrics.event_subscriber_exceptions;
  d["observer_lag_max_ns"] = metrics.observer_lag_max_ns;
  d["events_emitted"] = metrics.events_emitted;
  d["flight_recorder_records"] = metrics.flight_recorder_records;
  return d;
}

py::dict shape_to_python(const CollisionShapeDTO& shape) {
  py::dict d;
  if (const auto* box = std::get_if<BoxShapeDTO>(&shape)) {
    d["type"] = "box";
    d["box_dimensions_m"] = py::make_tuple(
        box->dimensions_m.x_m, box->dimensions_m.y_m, box->dimensions_m.z_m);
    return d;
  }
  if (const auto* sphere = std::get_if<SphereShapeDTO>(&shape)) {
    d["type"] = "sphere";
    d["sphere_radius_m"] = sphere->radius_m;
    return d;
  }
  if (const auto* cylinder = std::get_if<CylinderShapeDTO>(&shape)) {
    d["type"] = "cylinder";
    d["cylinder_radius_m"] = cylinder->radius_m;
    d["cylinder_height_m"] = cylinder->height_m;
    return d;
  }
  if (const auto* mesh = std::get_if<MeshShapeDTO>(&shape)) {
    d["type"] = "mesh";
    d["mesh_resource_uri"] = mesh->resource_uri;
    d["mesh_scale"] = py::make_tuple(mesh->scale.x_m, mesh->scale.y_m, mesh->scale.z_m);
    return d;
  }
  d["type"] = "unknown";
  return d;
}

template <typename SceneObject>
py::dict scene_object_snapshot_to_python(const SceneObject& object) {
  py::dict d;
  d["object_id"] = object.object_id;
  d["shape"] = shape_to_python(object.shape);
  d["pose_in_frame"] = pose_to_python(object.pose_in_frame);
  d["pose_in_universe"] = pose_to_python(object.pose_in_universe);
  d["frame_id"] = object.frame_id;
  d["attached"] = object.attached;
  d["touch_links"] = object.touch_links;
  d["support_contacts"] = support_contacts_to_python(object.support_contacts);
  d["source_id"] = optional_to_python(object.source_id);
  d["source_timestamp_ns"] = optional_to_python(object.source_timestamp_ns);
  d["source_freshness_budget_ns"] =
      optional_to_python(object.source_freshness_budget_ns);
  d["scene_admission_role"] = object.scene_admission_role.has_value()
                                  ? py::object(py::str(to_string(
                                        *object.scene_admission_role)))
                                  : py::object(py::none());
  d["scene_admission_lease_max_age_ns"] =
      optional_to_python(object.scene_admission_lease_max_age_ns);
  d["scene_admission_target_drift_tolerance_m"] = optional_to_python(
      object.scene_admission_target_drift_tolerance_m);
  d["scene_admission_target_angular_tolerance_rad"] = optional_to_python(
      object.scene_admission_target_angular_tolerance_rad);
  d["scene_admission_obstacle_drift_tolerance_m"] = optional_to_python(
      object.scene_admission_obstacle_drift_tolerance_m);
  d["scene_admission_obstacle_angular_tolerance_rad"] = optional_to_python(
      object.scene_admission_obstacle_angular_tolerance_rad);
  d["scene_admission_allow_runtime_dynamic_obstacle"] =
      object.scene_admission_allow_runtime_dynamic_obstacle;
  return d;
}

py::dict collision_policy_to_python(const CollisionPolicySnapshot& policy) {
  py::dict d;
  d["policy_id"] = policy.policy_id;
  d["revision"] = py::dict(
      "revision_id"_a = policy.revision.revision_id,
      "content_hash"_a = policy.revision.content_hash);
  d["require_full_diagnostics"] = policy.require_full_diagnostics;
  py::list static_pairs;
  for (const auto& pair : policy.allowed_static_static_pairs) {
    static_pairs.append(collision_pair_to_python(pair));
  }
  d["allowed_static_static_pairs"] = std::move(static_pairs);
  py::list touch_pairs;
  for (const auto& pair : policy.allowed_touch_link_pairs) {
    touch_pairs.append(collision_pair_to_python(pair));
  }
  d["allowed_touch_link_pairs"] = std::move(touch_pairs);
  py::list support_contacts;
  for (const auto& contact : policy.support_contacts) {
    py::dict value;
    value["pair"] = collision_pair_to_python(contact.pair);
    value["mode"] = contact.mode == CollisionContactPolicyMode::kContactProcess
                        ? "contact_process"
                        : "contact_release";
    value["enforce_max_start_penetration"] =
        contact.enforce_max_start_penetration;
    value["max_start_penetration_m"] = contact.max_start_penetration_m;
    value["release_tolerance_m"] = contact.release_tolerance_m;
    value["min_release_progress_m"] = contact.min_release_progress_m;
    value["min_separation_clearance_m"] =
        contact.min_separation_clearance_m;
    value["require_separation_by_end"] = contact.require_separation_by_end;
    value["require_strictly_decreasing_penetration"] =
        contact.require_strictly_decreasing_penetration;
    value["allow_recontact"] = contact.allow_recontact;
    support_contacts.append(std::move(value));
  }
  d["support_contacts"] = std::move(support_contacts);
  py::list contact_grasps;
  for (const auto& grasp : policy.contact_grasps) {
    contact_grasps.append(py::dict(
        "policy_id"_a = grasp.policy_id,
        "selected_object_id"_a = grasp.selected_object_id,
        "allowed_robot_links"_a = grasp.allowed_robot_links,
        "max_penetration_m"_a = grasp.max_penetration_m,
        "terminal_window_start_fraction"_a =
            grasp.terminal_window_start_fraction));
  }
  d["contact_grasps"] = std::move(contact_grasps);
  return d;
}

py::dict logical_scene_snapshot_to_python(
    const LogicalSceneSnapshot& snapshot) {
  py::dict d;
  d["revision"] = py::dict(
      "revision_id"_a = snapshot.revision.revision_id,
      "content_hash"_a = snapshot.revision.content_hash);
  d["transaction_sequence"] = snapshot.transaction_sequence;
  d["scene_admission_epoch"] = snapshot.admission_epoch;
  d["topology_epoch"] = snapshot.topology_epoch;
  d["policy_epoch"] = snapshot.policy_epoch;
  d["world_frame"] = snapshot.world_frame;
  d["configured_collision_policy"] =
      collision_policy_to_python(snapshot.configured_collision_policy);
  py::list robots;
  for (const auto& robot : snapshot.robot_placements) {
    robots.append(py::dict(
        "robot_id"_a = robot.robot_id,
        "pose_in_world"_a = pose_to_python(robot.pose_in_world)));
  }
  d["robot_placements"] = std::move(robots);
  py::list objects;
  for (const auto& object : snapshot.objects) {
    objects.append(scene_object_snapshot_to_python(object));
  }
  d["objects"] = std::move(objects);
  return d;
}

double finite_scale_from_python(const py::handle& value, const char* field_name) {
  const double result = value.cast<double>();
  if (!std::isfinite(result)) {
    throw std::invalid_argument(std::string(field_name) + " must be finite");
  }
  return result;
}

[[maybe_unused]] std::size_t size_t_from_python(const py::handle& value,
                                                const char* field_name) {
  if (!py::isinstance<py::int_>(value)) {
    throw std::invalid_argument(std::string(field_name) + " must be an integer");
  }
  std::int64_t parsed = 0;
  try {
    parsed = value.cast<std::int64_t>();
  } catch (const py::cast_error&) {
    throw std::invalid_argument(
        std::string(field_name) + " must fit in a signed 64-bit integer");
  }
  if (parsed < 0) {
    throw std::invalid_argument(std::string(field_name) + " must be >= 0");
  }
  return static_cast<std::size_t>(parsed);
}

py::dict action_span_to_python(const ActionSpanDTO& span) {
  py::dict d;
  d["action_source"] = to_string(span.action_source);
  d["action_span_id"] = span.action_span_id;
  if (!span.program_run_id.empty()) {
    d["program_run_id"] = span.program_run_id;
  }
  if (!span.node_instance_id.empty()) {
    d["node_instance_id"] = span.node_instance_id;
  }
  if (!span.node_attempt_id.empty()) {
    d["node_attempt_id"] = span.node_attempt_id;
  }
  if (!span.parent_node_attempt_id.empty()) {
    d["parent_node_attempt_id"] = span.parent_node_attempt_id;
  }
  return d;
}

std::string optional_string_from_python_dict(const py::dict& dict,
                                             const char* key) {
  if (!dict.contains(key) || dict[key].is_none()) {
    return {};
  }
  return dict[key].cast<std::string>();
}

std::optional<ActionSpanDTO> action_span_from_python(const py::dict& request) {
  if (!request.contains("action_span") || request["action_span"].is_none()) {
    return std::nullopt;
  }
  const auto span_dict = request["action_span"].cast<py::dict>();
  ActionSpanDTO span;
  std::string source = optional_string_from_python_dict(span_dict,
                                                        "action_source");
  if (source.empty()) {
    source = "system";
  }
  auto parsed_source = action_source_from_string(source);
  if (!parsed_source) {
    throw std::invalid_argument("action_span.action_source is invalid");
  }
  span.action_source = *parsed_source;
  span.action_span_id = optional_string_from_python_dict(span_dict,
                                                         "action_span_id");
  span.program_run_id = optional_string_from_python_dict(span_dict,
                                                        "program_run_id");
  span.node_instance_id = optional_string_from_python_dict(span_dict,
                                                          "node_instance_id");
  span.node_attempt_id = optional_string_from_python_dict(span_dict,
                                                         "node_attempt_id");
  span.parent_node_attempt_id = optional_string_from_python_dict(
      span_dict, "parent_node_attempt_id");
  return span;
}

void populate_request_correlation(ExecuteRequestDTO& req,
                                  const py::dict& request) {
  req.program_run_id = optional_string_from_python_dict(request,
                                                       "program_run_id");
  req.action_span = action_span_from_python(request);
  if (request.contains("contact_policy_id") &&
      !request["contact_policy_id"].is_none()) {
    req.contact_policy_id = request["contact_policy_id"].cast<std::string>();
  }
}

// Parse the optional per_joint_dynamics_limits map (Phase 4-P5.1K) from a
// request dict into the native DTO. Shape:
//   {joint_name: {"max_velocity_rad_s": f, "max_acceleration_rad_s2": f,
//                 "max_jerk_rad_s3": f}}
// Only present numeric fields are read; each must be finite and strictly
// positive, otherwise the request fails closed before planning
// (invalid_joint_dynamics_limit). Jerk is carried through to the backend,
// which rejects it where the active path cannot enforce it
// (unsupported_joint_dynamics_limit). An absent/None/empty map preserves
// prior behavior.
void parse_per_joint_dynamics_limits(const py::dict& request,
                                     PerJointDynamicsLimitsDTO& out) {
  if (!request.contains("per_joint_dynamics_limits")) {
    return;
  }
  auto raw = request["per_joint_dynamics_limits"];
  if (raw.is_none()) {
    return;
  }
  auto limits = raw.cast<py::dict>();
  for (auto item : limits) {
    const std::string joint_name = py::cast<std::string>(item.first);
    auto entry = item.second.cast<py::dict>();
    JointDynamicsLimitDTO limit;
    auto read_positive =
        [&](const char* key, const char* dim) -> std::optional<double> {
      if (!entry.contains(key) || entry[key].is_none()) {
        return std::nullopt;
      }
      const double d = entry[key].cast<double>();
      if (!std::isfinite(d) || d <= 0.0) {
        throw std::invalid_argument(
            std::string("per_joint_dynamics_limits['") + joint_name + "']." +
            key + " (" + dim + ") must be finite and strictly positive");
      }
      return d;
    };
    limit.max_velocity_rad_s = read_positive("max_velocity_rad_s", "velocity");
    limit.max_acceleration_rad_s2 =
        read_positive("max_acceleration_rad_s2", "acceleration");
    limit.max_jerk_rad_s3 = read_positive("max_jerk_rad_s3", "jerk");
    out[joint_name] = limit;
  }
}

// Helper used by both the motion-scoped scope parser (above) and the
// kernel-config parser (default_fault_stop_profile /
// operator_stop_profile). The body mirrors the inline parser in the
// scope path so behavior stays identical; the `context` parameter names
// the source for error messages.
MotionInterruptStopProfileDTO stop_profile_from_python(
    const py::dict& sp, const char* context) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = sp["profile_id"].cast<std::string>();
  const std::string sm = sp["stop_mode"].cast<std::string>();
  if (sm == "spring_back_hold") {
    profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  } else if (sm == "soft_on_path") {
    profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  } else if (sm == "hard_deceleration") {
    profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  } else {
    throw std::invalid_argument(
        std::string(context) + ".stop_mode: unknown value '" + sm +
        "' (expected 'spring_back_hold', 'soft_on_path', or "
        "'hard_deceleration')");
  }
  if (sp.contains("deceleration_time_s") &&
      !sp["deceleration_time_s"].is_none()) {
    profile.deceleration_time_s = sp["deceleration_time_s"].cast<double>();
  }
  if (sp.contains("max_stop_time_s") && !sp["max_stop_time_s"].is_none()) {
    profile.max_stop_time_s = sp["max_stop_time_s"].cast<double>();
  }
  if (sp.contains("settle_velocity_threshold_rad_s") &&
      !sp["settle_velocity_threshold_rad_s"].is_none()) {
    profile.settle_velocity_threshold_rad_s =
        sp["settle_velocity_threshold_rad_s"].cast<double>();
  }
  if (sp.contains("joint_deceleration_profile_id") &&
      !sp["joint_deceleration_profile_id"].is_none()) {
    profile.joint_deceleration_profile_id =
        sp["joint_deceleration_profile_id"].cast<std::string>();
  }
  if (sp.contains("per_joint_max_deceleration_rad_s2") &&
      !sp["per_joint_max_deceleration_rad_s2"].is_none()) {
    profile.per_joint_max_deceleration_rad_s2 =
        sp["per_joint_max_deceleration_rad_s2"]
            .cast<std::map<std::string, double>>();
  }
  if (sp.contains("allowed_fallback_modes") &&
      !sp["allowed_fallback_modes"].is_none()) {
    for (auto m : sp["allowed_fallback_modes"].cast<py::list>()) {
      const std::string ms = m.cast<std::string>();
      if (ms == "spring_back_hold") {
        profile.allowed_fallback_modes.push_back(
            MotionInterruptStopMode::kSpringBackHold);
      } else if (ms == "soft_on_path") {
        profile.allowed_fallback_modes.push_back(
            MotionInterruptStopMode::kSoftOnPath);
      } else if (ms == "hard_deceleration") {
        profile.allowed_fallback_modes.push_back(
            MotionInterruptStopMode::kHardDeceleration);
      } else {
        throw std::invalid_argument(
            std::string(context) + ".allowed_fallback_modes: unknown value '" +
            ms +
            "' (expected 'spring_back_hold', 'soft_on_path', "
            "or 'hard_deceleration')");
      }
    }
  }
  profile.allow_natural_completion_on_short_path =
      sp.attr("get")("allow_natural_completion_on_short_path", true)
          .cast<bool>();
  profile.require_backend_confirmation =
      sp.attr("get")("require_backend_confirmation", true).cast<bool>();
  // Fail closed at the config-parse boundary: a stop profile missing the
  // fields its mode requires (e.g. soft_on_path without a positive
  // deceleration_time_s or per-joint limits) must fail the deployment LOAD,
  // not silently pass readiness and only fail when an operator presses Stop.
  const std::string field_error =
      motion_interrupt_stop_profile_field_error(profile);
  if (!field_error.empty()) {
    throw std::invalid_argument(std::string(context) + ": " + field_error);
  }
  return profile;
}

CollisionPolicySnapshot collision_policy_snapshot_from_python(
    const py::dict& policy_dict, const char* context) {
  CollisionPolicySnapshot policy;
  if (!policy_dict.contains("policy_id") || policy_dict["policy_id"].is_none()) {
    throw std::invalid_argument(std::string(context) + ".policy_id is required");
  }
  policy.policy_id = policy_dict["policy_id"].cast<std::string>();
  if (policy.policy_id.empty()) {
    throw std::invalid_argument(std::string(context) + ".policy_id must not be empty");
  }
  if (policy_dict.contains("revision_id") && !policy_dict["revision_id"].is_none()) {
    policy.revision.revision_id =
        policy_dict["revision_id"].cast<std::uint64_t>();
  }
  if (policy_dict.contains("content_hash") && !policy_dict["content_hash"].is_none()) {
    policy.revision.content_hash =
        policy_dict["content_hash"].cast<std::string>();
  }
  if (policy_dict.contains("require_full_diagnostics") &&
      !policy_dict["require_full_diagnostics"].is_none()) {
    policy.require_full_diagnostics =
        policy_dict["require_full_diagnostics"].cast<bool>();
  }
  if (policy_dict.contains("contact_grasps") &&
      !policy_dict["contact_grasps"].is_none()) {
    if (!py::isinstance<py::list>(policy_dict["contact_grasps"])) {
      throw std::invalid_argument(
          std::string(context) + ".contact_grasps must be a list");
    }
    for (const auto item : policy_dict["contact_grasps"].cast<py::list>()) {
      if (!py::isinstance<py::dict>(item)) {
        throw std::invalid_argument(
            std::string(context) + ".contact_grasps entries must be dictionaries");
      }
      py::dict raw = item.cast<py::dict>();
      ContactGraspPolicyEntry entry;
      if (!raw.contains("policy_id") || raw["policy_id"].is_none()) {
        throw std::invalid_argument(
            std::string(context) + ".contact_grasps[].policy_id is required");
      }
      entry.policy_id = raw["policy_id"].cast<std::string>();
      if (entry.policy_id.empty()) {
        throw std::invalid_argument(
            std::string(context) + ".contact_grasps[].policy_id must not be empty");
      }
      if (!raw.contains("selected_object_id") ||
          raw["selected_object_id"].is_none()) {
        throw std::invalid_argument(
            std::string(context) +
            ".contact_grasps[].selected_object_id is required");
      }
      entry.selected_object_id = raw["selected_object_id"].cast<std::string>();
      if (entry.selected_object_id.empty()) {
        throw std::invalid_argument(
            std::string(context) +
            ".contact_grasps[].selected_object_id must not be empty");
      }
      if (!raw.contains("allowed_robot_links") ||
          !py::isinstance<py::list>(raw["allowed_robot_links"])) {
        throw std::invalid_argument(
            std::string(context) +
            ".contact_grasps[].allowed_robot_links must be a list");
      }
      for (const auto link : raw["allowed_robot_links"].cast<py::list>()) {
        entry.allowed_robot_links.push_back(link.cast<std::string>());
      }
      if (entry.allowed_robot_links.empty()) {
        throw std::invalid_argument(
            std::string(context) +
            ".contact_grasps[].allowed_robot_links must not be empty");
      }
      if (raw.contains("max_penetration_m") &&
          !raw["max_penetration_m"].is_none()) {
        entry.max_penetration_m = raw["max_penetration_m"].cast<double>();
      }
      if (!std::isfinite(entry.max_penetration_m) ||
          entry.max_penetration_m < 0.0) {
        throw std::invalid_argument(
            std::string(context) +
            ".contact_grasps[].max_penetration_m must be finite and >= 0");
      }
      if (raw.contains("terminal_window_start_fraction") &&
          !raw["terminal_window_start_fraction"].is_none()) {
        entry.terminal_window_start_fraction =
            raw["terminal_window_start_fraction"].cast<double>();
      }
      if (!std::isfinite(entry.terminal_window_start_fraction) ||
          entry.terminal_window_start_fraction < 0.0 ||
          entry.terminal_window_start_fraction > 1.0) {
        throw std::invalid_argument(
            std::string(context) +
            ".contact_grasps[].terminal_window_start_fraction must be in [0, 1]");
      }
      policy.contact_grasps.push_back(std::move(entry));
    }
  }
  return policy;
}

ExecutionKernelConfig kernel_config_from_python(const py::object& config_obj) {
  ExecutionKernelConfig config;
  if (config_obj.is_none()) {
    return config;
  }

  py::dict config_dict = config_obj.cast<py::dict>();
  auto read_seconds = [&](const char* s_key, const char* ms_key,
                          double current) {
    if (config_dict.contains(s_key)) {
      return finite_scale_from_python(config_dict[s_key], s_key);
    }
    if (config_dict.contains(ms_key)) {
      return finite_scale_from_python(config_dict[ms_key], ms_key) / 1000.0;
    }
    return current;
  };
  if (config_dict.contains("collision_monitor_period_ns")) {
    const double period_ns =
        finite_scale_from_python(config_dict["collision_monitor_period_ns"],
                                 "collision_monitor_period_ns");
    if (period_ns <= 0.0) {
      throw std::invalid_argument(
          "collision_monitor_period_ns must be > 0");
    }
    config.collision_monitor_period_ns =
        static_cast<int64_t>(period_ns);
  }
  if (config_dict.contains("runtime_collision_query_timeout_ns")) {
    const double timeout_ns =
        finite_scale_from_python(config_dict["runtime_collision_query_timeout_ns"],
                                 "runtime_collision_query_timeout_ns");
    if (timeout_ns <= 0.0) {
      throw std::invalid_argument(
          "runtime_collision_query_timeout_ns must be > 0");
    }
    config.runtime_collision_query_timeout_ns =
        static_cast<int64_t>(timeout_ns);
  }
  if (config_dict.contains("runtime_observed_state_max_age_ns")) {
    const double max_age_ns = finite_scale_from_python(
        config_dict["runtime_observed_state_max_age_ns"],
        "runtime_observed_state_max_age_ns");
    if (max_age_ns < 0.0) {
      throw std::invalid_argument(
          "runtime_observed_state_max_age_ns must be >= 0");
    }
    config.runtime_observed_state_max_age_ns =
        static_cast<int64_t>(max_age_ns);
  }
  if (config_dict.contains("recovery_require_measured_observation")) {
    config.recovery_require_measured_observation =
        config_dict["recovery_require_measured_observation"].cast<bool>();
  }
  if (config_dict.contains("recovery_ready_dwell_ns")) {
    config.recovery_ready_dwell_ns = static_cast<int64_t>(
        finite_scale_from_python(config_dict["recovery_ready_dwell_ns"],
                                 "recovery_ready_dwell_ns"));
  }
  if (config_dict.contains("recovery_ready_min_samples")) {
    config.recovery_ready_min_samples = static_cast<std::uint32_t>(
        size_t_from_python(config_dict["recovery_ready_min_samples"],
                           "recovery_ready_min_samples"));
  }
  if (config_dict.contains("recovery_observed_joint_order_token")) {
    config.recovery_observed_joint_order_token =
        config_dict["recovery_observed_joint_order_token"].cast<std::string>();
  }
  if (config_dict.contains("recovery_at_rest_velocity_threshold_rad_s")) {
    config.recovery_at_rest_velocity_threshold_rad_s =
        finite_scale_from_python(
            config_dict["recovery_at_rest_velocity_threshold_rad_s"],
            "recovery_at_rest_velocity_threshold_rad_s");
  }
  if (config_dict.contains("recovery_observation_poll_period_ns")) {
    config.recovery_observation_poll_period_ns = static_cast<int64_t>(
        finite_scale_from_python(
            config_dict["recovery_observation_poll_period_ns"],
            "recovery_observation_poll_period_ns"));
  }
  if (config_dict.contains("validation_collision_query_timeout_ns")) {
    const double timeout_ns = finite_scale_from_python(
        config_dict["validation_collision_query_timeout_ns"],
        "validation_collision_query_timeout_ns");
    if (timeout_ns <= 0.0) {
      throw std::invalid_argument(
          "validation_collision_query_timeout_ns must be > 0");
    }
    config.validation_collision_query_timeout_ns =
        static_cast<int64_t>(timeout_ns);
  }
  if (config_dict.contains("validation_collision_wall_timeout_ns")) {
    const double timeout_ns = finite_scale_from_python(
        config_dict["validation_collision_wall_timeout_ns"],
        "validation_collision_wall_timeout_ns");
    if (timeout_ns <= 0.0) {
      throw std::invalid_argument(
          "validation_collision_wall_timeout_ns must be > 0");
    }
    config.validation_collision_wall_timeout_ns =
        static_cast<int64_t>(timeout_ns);
  }
  if (config_dict.contains("runtime_dynamic_body_max_age_cap_ns")) {
    const double cap_ns = finite_scale_from_python(
        config_dict["runtime_dynamic_body_max_age_cap_ns"],
        "runtime_dynamic_body_max_age_cap_ns");
    // Zero is a sentinel: disable the kernel-side cap while keeping
    // body-local freshness checks active. Negative values are invalid.
    if (cap_ns < 0.0) {
      throw std::invalid_argument(
          "runtime_dynamic_body_max_age_cap_ns must be >= 0");
    }
    config.runtime_dynamic_body_max_age_cap_ns =
        static_cast<int64_t>(cap_ns);
  }
  config.native_reset_completion_timeout_s =
      read_seconds("native_reset_completion_timeout_s",
                   "native_reset_completion_timeout_ms",
                   config.native_reset_completion_timeout_s);
  if (config.native_reset_completion_timeout_s <= 0.0) {
    throw std::invalid_argument(
        "native_reset_completion_timeout_s must be > 0");
  }
  config.single_shot_ik_timeout_s =
      read_seconds("single_shot_ik_timeout_s", "single_shot_ik_timeout_ms",
                   config.single_shot_ik_timeout_s);
  if (config.single_shot_ik_timeout_s <= 0.0) {
    throw std::invalid_argument("single_shot_ik_timeout_s must be > 0");
  }
  config.planner_max_planning_time_s =
      read_seconds("planner_max_planning_time_s",
                   "planner_max_planning_time_ms",
                   config.planner_max_planning_time_s);
  if (config.planner_max_planning_time_s <= 0.0) {
    throw std::invalid_argument("planner_max_planning_time_s must be > 0");
  }
  if (config_dict.contains("rng_seed") &&
      !config_dict["rng_seed"].is_none()) {
    const auto seed = config_dict["rng_seed"].cast<std::uint64_t>();
    if (seed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("rng_seed must fit in uint32");
    }
    config.planner_rng_seed = static_cast<std::uint32_t>(seed);
  }
  if (config_dict.contains(
          "authoritative_validation_collision_replan_attempts")) {
    const double attempts = finite_scale_from_python(
        config_dict["authoritative_validation_collision_replan_attempts"],
        "authoritative_validation_collision_replan_attempts");
    if (attempts < 0.0) {
      throw std::invalid_argument(
          "authoritative_validation_collision_replan_attempts must be >= 0");
    }
    config.authoritative_validation_collision_replan_attempts =
        static_cast<int>(attempts);
  }
  if (config_dict.contains("baseline_collision_policy_snapshot") &&
      !config_dict["baseline_collision_policy_snapshot"].is_none()) {
    config.baseline_collision_policy_snapshot =
        collision_policy_snapshot_from_python(
            config_dict["baseline_collision_policy_snapshot"].cast<py::dict>(),
            "baseline_collision_policy_snapshot");
  }
  // Two optional controlled-stop profiles surfaced to Python so
  // deployment YAML can carry them. Recommended defaults are
  // spring_back_hold (fault) and soft_on_path (operator-stop); both
  // default to unset → legacy backend.stop() so existing deployments
  // see no behavior change.
  if (config_dict.contains("default_fault_stop_profile") &&
      !config_dict["default_fault_stop_profile"].is_none()) {
    config.default_fault_stop_profile =
        std::make_shared<MotionInterruptStopProfileDTO>(
            stop_profile_from_python(
                config_dict["default_fault_stop_profile"].cast<py::dict>(),
                "default_fault_stop_profile"));
  }
  if (config_dict.contains("operator_stop_profile") &&
      !config_dict["operator_stop_profile"].is_none()) {
    config.operator_stop_profile =
        std::make_shared<MotionInterruptStopProfileDTO>(
            stop_profile_from_python(
                config_dict["operator_stop_profile"].cast<py::dict>(),
                "operator_stop_profile"));
  }
  return config;
}

#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
ros2_exec::Ros2ControlExecutionBackendConfig ros2_control_config_from_python(
    const py::object& config_obj) {
  if (config_obj.is_none()) {
    throw std::invalid_argument("ros2_control config must not be None");
  }
  py::dict config_dict = config_obj.cast<py::dict>();
  ros2_exec::Ros2ControlExecutionBackendConfig config;
  if (config_dict.contains("node_name")) {
    config.node_name = config_dict["node_name"].cast<std::string>();
  }
  if (config_dict.contains("action_name")) {
    config.action_name = config_dict["action_name"].cast<std::string>();
  }
  if (config_dict.contains("arm_action")) {
    config.action_name = config_dict["arm_action"].cast<std::string>();
  }
  if (config_dict.contains("joint_state_topic")) {
    config.joint_state_topic = config_dict["joint_state_topic"].cast<std::string>();
  }
  if (config_dict.contains("controller_state_topic")) {
    config.controller_state_topic =
        config_dict["controller_state_topic"].cast<std::string>();
  }
  if (config_dict.contains("required_joints")) {
    config.required_joints =
        config_dict["required_joints"].cast<std::vector<std::string>>();
  }
  if (config_dict.contains("observed_state_source_precedence")) {
    config.observed_state_source_precedence =
        config_dict["observed_state_source_precedence"]
            .cast<std::vector<std::string>>();
  }
  if (config_dict.contains("observed_state_authority_profile_id")) {
    config.observed_state_authority_profile_id =
        config_dict["observed_state_authority_profile_id"].cast<std::string>();
  }
  if (config_dict.contains("executor_thread_count")) {
    config.executor_thread_count = size_t_from_python(
        config_dict["executor_thread_count"], "executor_thread_count");
  }
  auto read_seconds = [&](const char* s_key, const char* ms_key, double current) {
    if (config_dict.contains(s_key)) {
      return finite_scale_from_python(config_dict[s_key], s_key);
    }
    if (config_dict.contains(ms_key)) {
      return finite_scale_from_python(config_dict[ms_key], ms_key) / 1000.0;
    }
    return current;
  };
  config.joint_state_max_age_s =
      read_seconds("joint_state_max_age_s", "joint_state_max_age_ms",
                   config.joint_state_max_age_s);
  config.wait_for_state_timeout_s =
      read_seconds("wait_for_state_timeout_s", "wait_for_state_timeout_ms",
                   config.wait_for_state_timeout_s);
  config.wait_for_action_server_timeout_s =
      read_seconds("wait_for_action_server_timeout_s",
                   "wait_for_action_server_timeout_ms",
                   config.wait_for_action_server_timeout_s);
  config.tracking_error_timeout_s =
      read_seconds("tracking_error_timeout_s", "tracking_error_timeout_ms",
                   config.tracking_error_timeout_s);
  config.monitor_period_s =
      read_seconds("monitor_period_s", "monitor_period_ms", config.monitor_period_s);
  config.stop_completion_timeout_s =
      read_seconds("stop_completion_timeout_s", "stop_completion_timeout_ms",
                   config.stop_completion_timeout_s);
  config.sim_reset_motion_duration_s =
      read_seconds("sim_reset_motion_duration_s",
                   "sim_reset_motion_duration_ms",
                   config.sim_reset_motion_duration_s);

  if (config_dict.contains("start_tolerance_rad")) {
    config.start_tolerance_rad =
        finite_scale_from_python(config_dict["start_tolerance_rad"],
                                 "start_tolerance_rad");
  }
  if (config_dict.contains("final_tolerance_rad")) {
    config.final_tolerance_rad =
        finite_scale_from_python(config_dict["final_tolerance_rad"],
                                 "final_tolerance_rad");
  }
  if (config_dict.contains("tracking_error_tolerance_rad")) {
    config.tracking_error_tolerance_rad =
        finite_scale_from_python(config_dict["tracking_error_tolerance_rad"],
                                 "tracking_error_tolerance_rad");
  }
  if (config_dict.contains("require_fresh_state_before_planning")) {
    config.require_fresh_state_before_planning =
        config_dict["require_fresh_state_before_planning"].cast<bool>();
  }
  if (config_dict.contains("disable_controller_path_tolerances")) {
    config.disable_controller_path_tolerances =
        config_dict["disable_controller_path_tolerances"].cast<bool>();
  }
  std::optional<std::string> execution_state_source_mode;
  std::optional<std::string> execution_state_authority_profile;
  if (config_dict.contains("execution_state_source_mode") &&
      !config_dict["execution_state_source_mode"].is_none()) {
    execution_state_source_mode =
        config_dict["execution_state_source_mode"].cast<std::string>();
  }
  if (config_dict.contains("execution_state_authority_profile") &&
      !config_dict["execution_state_authority_profile"].is_none()) {
    execution_state_authority_profile =
        config_dict["execution_state_authority_profile"].cast<std::string>();
  }
  if (execution_state_source_mode.has_value() &&
      execution_state_authority_profile.has_value() &&
      *execution_state_source_mode != *execution_state_authority_profile) {
    throw std::invalid_argument(
        "execution_state_source_mode and execution_state_authority_profile "
        "must match when both are provided");
  }
  if (execution_state_authority_profile.has_value()) {
    config.execution_state_source_mode = *execution_state_authority_profile;
  } else if (execution_state_source_mode.has_value()) {
    config.execution_state_source_mode = *execution_state_source_mode;
  }
  if (config_dict.contains("initial_joint_positions_rad")) {
    config.initial_joint_positions_rad =
        config_dict["initial_joint_positions_rad"].cast<std::vector<double>>();
  }
  if (config_dict.contains("command_gate_topic")) {
    config.command_gate_topic =
        config_dict["command_gate_topic"].cast<std::string>();
  }
  if (config_dict.contains("speed_scaling_topic")) {
    config.speed_scaling_topic =
        config_dict["speed_scaling_topic"].cast<std::string>();
  }
  if (config_dict.contains("enable_jtc_speed_scaling_input")) {
    config.enable_jtc_speed_scaling_input =
        config_dict["enable_jtc_speed_scaling_input"].cast<bool>();
  }
  if (config_dict.contains("decelerate_on_cancel")) {
    config.decelerate_on_cancel =
        config_dict["decelerate_on_cancel"].cast<bool>();
  }
  if (config_dict.contains("velocity_state_interface_joints")) {
    const auto joints =
        config_dict["velocity_state_interface_joints"].cast<std::vector<std::string>>();
    config.velocity_state_interface_joints =
        std::set<std::string>(joints.begin(), joints.end());
  }
  if (config_dict.contains("max_deceleration_on_cancel_rad_s2")) {
    config.max_deceleration_on_cancel_rad_s2 =
        config_dict["max_deceleration_on_cancel_rad_s2"]
            .cast<std::map<std::string, double>>();
  }
  if (config_dict.contains("enable_soft_on_path_path_ramp")) {
    config.enable_soft_on_path_path_ramp =
        config_dict["enable_soft_on_path_path_ramp"].cast<bool>();
  }
  config.soft_on_path_stop_traj_delta_s =
      read_seconds("soft_on_path_stop_traj_delta_s",
                   "soft_on_path_stop_traj_delta_ms",
                   config.soft_on_path_stop_traj_delta_s);
  config.soft_on_path_sample_period_s =
      read_seconds("soft_on_path_sample_period_s",
                   "soft_on_path_sample_period_ms",
                   config.soft_on_path_sample_period_s);
  if (config_dict.contains("soft_on_path_max_initial_path_deviation_rad")) {
    auto val = config_dict["soft_on_path_max_initial_path_deviation_rad"];
    if (!val.is_none()) {
      config.soft_on_path_max_initial_path_deviation_rad =
          finite_scale_from_python(val,
                                   "soft_on_path_max_initial_path_deviation_rad");
    }
  }
  if (config_dict.contains(
          "soft_on_path_max_initial_velocity_deviation_rad_s")) {
    auto val =
        config_dict["soft_on_path_max_initial_velocity_deviation_rad_s"];
    if (!val.is_none()) {
      config.soft_on_path_max_initial_velocity_deviation_rad_s =
          finite_scale_from_python(
              val,
              "soft_on_path_max_initial_velocity_deviation_rad_s");
    }
  }
  if (config_dict.contains("soft_on_path_max_jerk_rad_s3")) {
    config.soft_on_path_max_jerk_rad_s3 =
        config_dict["soft_on_path_max_jerk_rad_s3"]
            .cast<std::map<std::string, double>>();
  }
  if (config_dict.contains("soft_on_path_per_joint_max_jerk_rad_s3")) {
    config.soft_on_path_max_jerk_rad_s3 =
        config_dict["soft_on_path_per_joint_max_jerk_rad_s3"]
            .cast<std::map<std::string, double>>();
  }
  if (config_dict.contains("allow_soft_on_path_model_velocity_fallback")) {
    config.allow_soft_on_path_model_velocity_fallback =
        config_dict["allow_soft_on_path_model_velocity_fallback"].cast<bool>();
  }
  if (config_dict.contains("enable_soft_on_path_experimental")) {
    config.enable_soft_on_path_experimental =
        config_dict["enable_soft_on_path_experimental"].cast<bool>();
  }
  return config;
}

ros2_exec::Ros2ControlExecutionBackend& require_ros2_control_execution(
    NativeKernelBundle& self) {
  auto* backend =
      dynamic_cast<ros2_exec::Ros2ControlExecutionBackend*>(self.execution.get());
  if (backend == nullptr) {
    throw std::runtime_error(
        "Native kernel execution backend is not ros2_control");
  }
  return *backend;
}
#endif

}  // namespace

PYBIND11_MODULE(manyforge_roboplan_py, m) {
  m.doc() = "Native C++ kernel with RoboPlan backends (no Python trampolines)";

  // Expose NativeKernelBundle as opaque holder
  py::class_<NativeKernelBundle, std::shared_ptr<NativeKernelBundle>>(m, "NativeKernelBundle")
      .def("set_robot_model_authority_metadata",
           [](NativeKernelBundle& self, py::dict metadata) {
             self.canonical_model_fingerprint =
                 metadata.attr("get")("canonical_fingerprint", "")
                     .cast<std::string>();
             self.roboplan_view_fingerprint =
                 metadata.attr("get")("view_fingerprint", "")
                     .cast<std::string>();
             self.canonical_to_view_joint_names =
                 metadata
                     .attr("get")("canonical_to_view_joint_names", py::dict())
                     .cast<std::map<std::string, std::string>>();
             self.view_to_canonical_joint_names =
                 metadata
                     .attr("get")("view_to_canonical_joint_names", py::dict())
                     .cast<std::map<std::string, std::string>>();
             self.group_commandable_joint_names =
                 metadata
                     .attr("get")("group_commandable_joint_names", py::dict())
                     .cast<std::map<std::string, std::vector<std::string>>>();
             if (self.execution_state_sink) {
               auto roboplan_sink =
                   std::dynamic_pointer_cast<RoboPlanExecutionStateSink>(
                       self.execution_state_sink);
               if (roboplan_sink) {
                 roboplan_sink->set_robot_model_authority_metadata(
                     self.canonical_to_view_joint_names,
                     self.view_to_canonical_joint_names,
                     self.group_commandable_joint_names);
               }
             }
           },
           py::arg("metadata"),
           "Attach Robot Model Authority metadata for canonical public readout.")
      .def("close",
           [](NativeKernelBundle& self) {
             py::gil_scoped_release release;
             self.close();
           },
           "Close the native kernel and join owned workers with the GIL released.")
      .def("closed", [](NativeKernelBundle& self) { return self.closed(); })
      .def("start_planning_qualification_capture",
           [](NativeKernelBundle& self, std::size_t capacity) {
             py::gil_scoped_release release;
             return self.start_planning_qualification_capture(capacity);
           },
           py::arg("capacity") = 128)
      .def("drain_planning_qualification_capture",
           [](NativeKernelBundle& self, std::uint64_t generation) {
             const auto batch = [&]() {
               py::gil_scoped_release release;
               return self.drain_planning_qualification_capture(generation);
             }();
             py::list out;
             for (const auto& record : batch.records) {
               out.append(planning_qualification_record_to_python(record));
             }
             return py::dict("records"_a = std::move(out),
                             "dropped_total"_a = batch.dropped_total,
                             "in_flight"_a = batch.in_flight);
           },
           py::arg("generation"))
      .def("stop_planning_qualification_capture",
           [](NativeKernelBundle& self, std::uint64_t generation,
              std::uint64_t idle_timeout_ms) {
             const auto batch = [&]() {
               py::gil_scoped_release release;
               return self.stop_planning_qualification_capture(
                   generation, std::chrono::milliseconds(idle_timeout_ms));
             }();
             py::list out;
             for (const auto& record : batch.records) {
               out.append(planning_qualification_record_to_python(record));
             }
             return py::dict("records"_a = std::move(out),
                             "dropped_total"_a = batch.dropped_total,
                             "in_flight"_a = batch.in_flight);
           },
           py::arg("generation"), py::arg("idle_timeout_ms") = 10'000)
      .def("planning_qualification_capture_status",
           [](NativeKernelBundle& self) {
             const auto values = [&]() {
               py::gil_scoped_release release;
               return std::array<std::uint64_t, 3>{
                   self.planning_qualification_generation(),
                   self.planning_qualification_dropped_total(),
                   self.planning_qualification_in_flight()};
             }();
             return py::dict("generation"_a = values[0],
                             "dropped_total"_a = values[1],
                             "in_flight"_a = values[2]);
           })
      .def("execute", [](NativeKernelBundle& self, py::dict request) -> py::dict {
        // Build ExecuteRequestDTO from Python dict
        ExecuteRequestDTO req;
        req.robot_id = request["robot_id"].cast<std::string>();
        req.group_id = request["group_id"].cast<std::string>();
        req.move_id = request["move_id"].cast<std::string>();
        req.base_frame = request["base_frame"].cast<std::string>();
        req.tcp_frame = request["tcp_frame"].cast<std::string>();
        populate_request_correlation(req, request);

        // Tolerances
        req.tolerances.joint_position_tol_rad = request.attr("get")("joint_position_tol_rad", 0.05).cast<double>();
        req.tolerances.translation_tol_m = request.attr("get")("translation_tol_m", 0.005).cast<double>();
        req.tolerances.rotation_tol_rad = request.attr("get")("rotation_tol_rad", 0.05).cast<double>();

        // Speed profile
        req.speed_profile.velocity_scale = finite_scale_from_python(
            request.attr("get")("velocity_scale", 1.0),
            "velocity_scale");
        req.speed_profile.acceleration_scale = finite_scale_from_python(
            request.attr("get")("acceleration_scale", 1.0),
            "acceleration_scale");
        if (request.contains("max_tcp_linear_speed_mps")) {
          auto val = request["max_tcp_linear_speed_mps"];
          if (!val.is_none()) {
            double tcp_speed = val.cast<double>();
            if (tcp_speed > 0.0) {
              req.speed_profile.max_tcp_linear_speed_mps = tcp_speed;
            }
          }
        }

        // Per-joint dynamics caps (Phase 4-P5.1K). Fail closed on malformed
        // values before planning; the backend enforces vel/accel and rejects
        // jerk where it cannot honor it.
        parse_per_joint_dynamics_limits(request, req.per_joint_dynamics_limits);

        // Timeouts
        req.timeouts.planning_timeout_s = request.attr("get")("planning_timeout_s", 5.0).cast<double>();
        req.timeouts.execution_timeout_s = request.attr("get")("execution_timeout_s", 10.0).cast<double>();

        // Cache mode
        std::string cache_mode_str = request.attr("get")("cache_mode", "default").cast<std::string>();
        req.cache_mode = (cache_mode_str == "disable") ? CacheMode::kDisable : CacheMode::kDefault;

        // Motion type
        std::string motion_type = request["motion_type"].cast<std::string>();
        if (motion_type == "joint_goal") {
          JointGoalDTO goal;
          goal.joint_names = request["joint_goal_names"].cast<std::vector<std::string>>();
          goal.positions_rad = request["joint_goal_positions_rad"].cast<std::vector<double>>();
          req.motion = goal;
        } else if (motion_type == "pose_goal") {
          PoseGoalDTO goal;
          auto pose_dict = request["pose_goal"];
          auto pos = pose_dict["position_m"].cast<std::vector<double>>();
          auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.goal.position_m = {pos[0], pos[1], pos[2]};
          goal.goal.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        } else if (motion_type == "relative_move") {
          RelativeMoveDTO goal;
          auto rel_dict = request["relative_move"];
          auto parse_reference_dict = [](const py::dict& ref_dict,
                                         const std::string& field_name) {
            ReferenceDTO reference;
            const std::string ref_type = ref_dict["type"].cast<std::string>();
            if (ref_type == "frame") {
              reference.type = ReferenceDTO::Type::kFrame;
            } else if (ref_type == "object") {
              reference.type = ReferenceDTO::Type::kObject;
            } else if (ref_type == "robot_link") {
              reference.type = ReferenceDTO::Type::kRobotLink;
            } else if (ref_type == "pose") {
              reference.type = ReferenceDTO::Type::kPose;
            } else {
              throw std::runtime_error("Unsupported reference type: " + ref_type);
            }
            if (ref_dict.contains("id")) {
              reference.id = ref_dict["id"].cast<std::string>();
            }
            if (reference.type == ReferenceDTO::Type::kPose) {
              if (!ref_dict.contains("pose")) {
                throw std::runtime_error(
                    "Pose reference requires " + field_name + ".pose");
              }
              auto pose_dict = ref_dict["pose"];
              auto pos = pose_dict["position_m"].cast<std::vector<double>>();
              auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
              reference.pose = Pose3DTO{};
              reference.pose->position_m = {pos[0], pos[1], pos[2]};
              reference.pose->orientation = {orient[0], orient[1], orient[2], orient[3]};
            }
            return reference;
          };

          goal.target_reference = parse_reference_dict(
              rel_dict["target_reference"].cast<py::dict>(), "target_reference");
          if (rel_dict.contains("offset_reference")) {
            goal.offset_reference = parse_reference_dict(
                rel_dict["offset_reference"].cast<py::dict>(), "offset_reference");
          }

          auto offset_dict = rel_dict["offset"];
          auto pos = offset_dict["position_m"].cast<std::vector<double>>();
          auto orient = offset_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.offset.position_m = {pos[0], pos[1], pos[2]};
          goal.offset.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        }

        // Motion-scoped interrupt scopes
        // (spec/210 §3.10 + spec/260 §2.6). Parse each scope's
        // stop_profile + node_category so MoveUntilSignal /
        // ProbeSearchMove can drive the kernel arm path. Without
        // this parsing the scopes would be silently dropped at the
        // Python/native boundary and the move would run unscoped.
        if (request.contains("motion_interrupt_scopes")) {
          auto scopes_obj = request["motion_interrupt_scopes"];
          if (!scopes_obj.is_none()) {
            auto scopes_list = scopes_obj.cast<py::list>();
            for (auto handle : scopes_list) {
              auto sd = handle.cast<py::dict>();
              MotionInterruptScopeDTO scope;
              scope.scope_id = sd["scope_id"].cast<std::string>();
              scope.policy_id = sd["policy_id"].cast<std::string>();
              const std::string activation =
                  sd.attr("get")("activation_mode",
                                  std::string("require_inactive_at_arm"))
                      .cast<std::string>();
              // A typoed or stale client value MUST fail before
              // dispatch. Silently
              // coercing "trip_if_already_inactive" → require_inactive
              // would dispatch a different scope than the operator
              // configured.
              if (activation == "require_inactive_at_arm") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kRequireInactiveAtArm;
              } else if (activation == "fresh_edge_after_arm") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kFreshEdgeAfterArm;
              } else if (activation == "trip_if_already_active") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kTripIfAlreadyActive;
              } else if (activation == "maintain_level_with_hysteresis") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kMaintainLevelWithHysteresis;
              } else {
                throw std::invalid_argument(
                    std::string("motion_interrupt_scopes[].activation_mode: "
                                "unknown value '") +
                    activation +
                    "' (expected one of require_inactive_at_arm, "
                    "fresh_edge_after_arm, trip_if_already_active, "
                    "maintain_level_with_hysteresis)");
              }
              if (sd.contains("arm_timeout_ns") &&
                  !sd["arm_timeout_ns"].is_none()) {
                scope.arm_timeout_ns =
                    sd["arm_timeout_ns"].cast<std::int64_t>();
              }
              if (sd.contains("result_binding_key") &&
                  !sd["result_binding_key"].is_none()) {
                scope.result_binding_key =
                    sd["result_binding_key"].cast<std::string>();
              }
              scope.required = sd.attr("get")("required", true).cast<bool>();
              const std::string node_category =
                  sd.attr("get")("node_category", std::string("generic"))
                      .cast<std::string>();
              if (node_category == "generic") {
                scope.node_category = MotionInterruptNodeCategory::kGeneric;
              } else if (node_category == "precision_contact") {
                scope.node_category =
                    MotionInterruptNodeCategory::kPrecisionContact;
              } else {
                throw std::invalid_argument(
                    std::string("motion_interrupt_scopes[].node_category: "
                                "unknown value '") +
                    node_category +
                    "' (expected 'generic' or 'precision_contact')");
              }
              if (sd.contains("stop_profile") &&
                  !sd["stop_profile"].is_none()) {
                // Single source of truth for parsing
                // `MotionInterruptStopProfileDTO` from Python. Same
                // helper feeds the kernel-config `default_fault_stop_profile`
                // and `operator_stop_profile` fields.
                scope.stop_profile = stop_profile_from_python(
                    sd["stop_profile"].cast<py::dict>(),
                    "motion_interrupt_scopes[].stop_profile");
              }
              req.motion_interrupt_scopes.push_back(std::move(scope));
            }
          }
        }

        auto kernel = require_open_kernel(self);
        auto result = [&]() {
          py::gil_scoped_release release;
          return kernel->execute(req);
        }();
        return result_to_python(result);
      })
      .def("get_execution_outcome",
           [](NativeKernelBundle& self, std::uint64_t execution_id)
               -> py::object {
             // Expose the kernel's terminal motion outcome
             // store so the Python BT nodes can map terminal_cause to
             // py_trees Status. Returns None when no outcome is
             // recorded (e.g., the execution id was never seen or
             // FIFO-evicted).
             auto kernel = require_open_kernel(self);
             auto outcome_opt = [&]() {
               py::gil_scoped_release release;
               return kernel->get_execution_outcome(execution_id);
             }();
             if (!outcome_opt) {
               return py::none();
             }
             const auto& outcome = *outcome_opt;
             py::dict d;
             d["execution_id"] = outcome.execution_id;
             d["move_id"] = outcome.move_id;
             if (outcome.action_span) {
               d["action_span"] = action_span_to_python(*outcome.action_span);
             } else {
               d["action_span"] = py::none();
             }
             if (!outcome.contact_policy_id.empty()) {
               d["contact_policy_id"] = outcome.contact_policy_id;
             } else {
               d["contact_policy_id"] = py::none();
             }
             const char* cause = "completed_normally";
             switch (outcome.terminal_cause) {
               using enum MotionExecutionTerminalCause;
               case kCompletedNormally: cause = "completed_normally"; break;
               case kSignalTrip:        cause = "signal_trip"; break;
               case kNoSignalTrip:      cause = "no_signal_trip"; break;
               case kStopped:           cause = "stopped"; break;
               case kFailed:            cause = "failed"; break;
               case kInterruptedFault:  cause = "interrupted_fault"; break;
             }
             d["terminal_cause"] = std::string(cause);
             if (outcome.trip) {
               const auto& trip = *outcome.trip;
               py::dict td;
               td["latch_id"] = trip.latch_id;
               td["source_id"] = trip.source_id;
               td["policy_id"] = trip.policy_id;
               td["scope_id"] = trip.scope_id;
               const char* esm = "spring_back_hold";
               switch (trip.executed_stop_mode) {
                 using enum MotionInterruptStopMode;
                 case kSpringBackHold:    esm = "spring_back_hold"; break;
                 case kSoftOnPath:        esm = "soft_on_path"; break;
                 case kHardDeceleration:  esm = "hard_deceleration"; break;
               }
               td["executed_stop_mode"] = std::string(esm);
               const char* scs = "diagnostic_other";
               switch (trip.stop_command_source) {
                 using enum MotionInterruptStopCommandSource;
                 case kFjtPreemptHold:           scs = "fjt_preempt_hold"; break;
                 case kFjtPreemptPathRamp:       scs = "fjt_preempt_path_ramp"; break;
                 case kJtcSpeedScalingRamp:      scs = "jtc_speed_scaling_ramp"; break;
                 case kJtcCancelDecelerateOnCancel:
                   scs = "jtc_cancel_decelerate_on_cancel"; break;
                 case kJtcCancelUnprofiledHold:
                   scs = "jtc_cancel_unprofiled_hold"; break;
                 case kNativeSimulatedStop:      scs = "native_simulated_stop"; break;
                 case kNativeSimulatedPathRamp:
                   scs = "native_simulated_path_ramp"; break;
                 case kRobotNativeStop:          scs = "robot_native_stop"; break;
                 case kDiagnosticOther:          scs = "diagnostic_other"; break;
               }
               td["stop_command_source"] = std::string(scs);
               td["backend_stop_confirmed"] =
                   trip.stop_snapshot.backend_stop_confirmed;
               // Full stop snapshot fidelity. Downstream consumers
               // need source +
               // state + captured_at_ns so they can decide whether
               // the certified rest position is acceptable for the
               // node's precision contract.
               const char* sss = "execution_backend_feedback";
               switch (trip.stop_snapshot.source) {
                 using enum ExecutionStopSnapshotSource;
                 case kExecutionBackendFeedback:
                   sss = "execution_backend_feedback"; break;
                 case kKernelExecutionSnapshot:
                   sss = "kernel_execution_snapshot"; break;
                 case kObservedStateProvider:
                   sss = "observed_state_provider"; break;
               }
               td["stop_snapshot_source"] = std::string(sss);
               td["stop_snapshot_captured_at_ns"] =
                   trip.stop_snapshot.captured_at_ns;
               py::dict rs;
               rs["joint_names"] = trip.stop_snapshot.robot_state.joint_names;
               rs["positions_rad"] =
                   trip.stop_snapshot.robot_state.positions_rad;
               rs["velocities_rad_s"] =
                   trip.stop_snapshot.robot_state.velocities_rad_s;
               td["stop_snapshot_robot_state"] = rs;
               // Measured settle signal (M3): the audit/HMI boundary must see
               // confirmed-at-rest, not only positions. Absent values mean the
               // settle verdict is UNKNOWN (no measured velocity evidence).
               if (trip.stop_snapshot.max_abs_velocity_rad_s) {
                 td["stop_snapshot_max_abs_velocity_rad_s"] =
                     *trip.stop_snapshot.max_abs_velocity_rad_s;
               }
               if (trip.stop_snapshot.at_rest) {
                 td["stop_snapshot_at_rest"] = *trip.stop_snapshot.at_rest;
               }
               if (trip.stop_distance_rad) {
                 td["stop_distance_rad"] = *trip.stop_distance_rad;
               }
               if (trip.path_deviation_max_rad) {
                 td["path_deviation_max_rad"] = *trip.path_deviation_max_rad;
               }
               if (trip.initial_path_deviation_rad) {
                 td["initial_path_deviation_rad"] =
                     *trip.initial_path_deviation_rad;
               }
               if (trip.fallback_reason) {
                 td["fallback_reason"] = *trip.fallback_reason;
               }
               if (trip.result_binding_key) {
                 td["result_binding_key"] = *trip.result_binding_key;
               }
               td["signal_to_latch_ns"] = trip.signal_to_latch_ns;
               td["latch_to_backend_stop_call_ns"] =
                   trip.latch_to_backend_stop_call_ns;
               td["latch_to_stop_confirmed_ns"] =
                   trip.latch_to_stop_confirmed_ns;
               // Trip diagnostics: bounded JSON-safe string map; surface
               // every entry so the BT/runtime can read scaffold flags
               // (e.g. soft_on_path_scaffold_phase) without re-querying
               // the backend.
               py::dict trip_diag;
               for (const auto& [k, v] : trip.diagnostics) {
                 trip_diag[py::str(k)] = v;
               }
               td["diagnostics"] = trip_diag;
               // Requested vs executed: the executed_stop_mode above
               // describes physical behavior; expose the original
               // requested mode separately so audit can verify the
               // scope's profile matches what the operator authored.
               const char* req_mode = "spring_back_hold";
               switch (trip.requested_stop_profile.stop_mode) {
                 using enum MotionInterruptStopMode;
                 case kSpringBackHold:    req_mode = "spring_back_hold"; break;
                 case kSoftOnPath:        req_mode = "soft_on_path"; break;
                 case kHardDeceleration:  req_mode = "hard_deceleration"; break;
               }
               td["requested_stop_mode"] = std::string(req_mode);
               td["requested_stop_profile_id"] =
                   trip.requested_stop_profile.profile_id;
               d["trip"] = td;
             }
             if (outcome.error) {
               py::dict ed;
               ed["code"] = outcome.error->code;
               ed["message"] = outcome.error->message;
               py::dict details;
               for (const auto& [k, v] : outcome.error->details) {
                 details[py::str(k)] = v;
               }
               ed["details"] = details;
               d["error"] = ed;
             }
             return d;
           })
      .def("preview_motion", [](NativeKernelBundle& self,
                                 py::dict request,
                                 py::object start_state_obj) -> py::dict {
        ExecuteRequestDTO req;
        req.robot_id = request["robot_id"].cast<std::string>();
        req.group_id = request["group_id"].cast<std::string>();
        req.move_id = request["move_id"].cast<std::string>();
        req.base_frame = request["base_frame"].cast<std::string>();
        req.tcp_frame = request["tcp_frame"].cast<std::string>();
        populate_request_correlation(req, request);

        req.tolerances.joint_position_tol_rad = request.attr("get")("joint_position_tol_rad", 0.05).cast<double>();
        req.tolerances.translation_tol_m = request.attr("get")("translation_tol_m", 0.005).cast<double>();
        req.tolerances.rotation_tol_rad = request.attr("get")("rotation_tol_rad", 0.05).cast<double>();

        req.speed_profile.velocity_scale = finite_scale_from_python(
            request.attr("get")("velocity_scale", 1.0),
            "velocity_scale");
        req.speed_profile.acceleration_scale = finite_scale_from_python(
            request.attr("get")("acceleration_scale", 1.0),
            "acceleration_scale");
        if (request.contains("max_tcp_linear_speed_mps")) {
          auto val = request["max_tcp_linear_speed_mps"];
          if (!val.is_none()) {
            double tcp_speed = val.cast<double>();
            if (tcp_speed > 0.0) {
              req.speed_profile.max_tcp_linear_speed_mps = tcp_speed;
            }
          }
        }

        // Per-joint dynamics caps (Phase 4-P5.1K). Fail closed on malformed
        // values before planning; the backend enforces vel/accel and rejects
        // jerk where it cannot honor it.
        parse_per_joint_dynamics_limits(request, req.per_joint_dynamics_limits);

        req.timeouts.planning_timeout_s = request.attr("get")("planning_timeout_s", 5.0).cast<double>();
        req.timeouts.execution_timeout_s = request.attr("get")("execution_timeout_s", 10.0).cast<double>();

        std::string cache_mode_str = request.attr("get")("cache_mode", "default").cast<std::string>();
        req.cache_mode = (cache_mode_str == "disable") ? CacheMode::kDisable : CacheMode::kDefault;

        std::string motion_type = request["motion_type"].cast<std::string>();
        if (motion_type == "joint_goal") {
          JointGoalDTO goal;
          goal.joint_names = request["joint_goal_names"].cast<std::vector<std::string>>();
          goal.positions_rad = request["joint_goal_positions_rad"].cast<std::vector<double>>();
          req.motion = goal;
        } else if (motion_type == "pose_goal") {
          PoseGoalDTO goal;
          auto pose_dict = request["pose_goal"];
          auto pos = pose_dict["position_m"].cast<std::vector<double>>();
          auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.goal.position_m = {pos[0], pos[1], pos[2]};
          goal.goal.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        } else if (motion_type == "relative_move") {
          RelativeMoveDTO goal;
          auto rel_dict = request["relative_move"];
          auto parse_reference_dict = [](const py::dict& ref_dict,
                                         const std::string& field_name) {
            ReferenceDTO reference;
            const std::string ref_type = ref_dict["type"].cast<std::string>();
            if (ref_type == "frame") {
              reference.type = ReferenceDTO::Type::kFrame;
            } else if (ref_type == "object") {
              reference.type = ReferenceDTO::Type::kObject;
            } else if (ref_type == "robot_link") {
              reference.type = ReferenceDTO::Type::kRobotLink;
            } else if (ref_type == "pose") {
              reference.type = ReferenceDTO::Type::kPose;
            } else {
              throw std::runtime_error("Unsupported reference type: " + ref_type);
            }
            if (ref_dict.contains("id")) {
              reference.id = ref_dict["id"].cast<std::string>();
            }
            if (reference.type == ReferenceDTO::Type::kPose) {
              if (!ref_dict.contains("pose")) {
                throw std::runtime_error(
                    "Pose reference requires " + field_name + ".pose");
              }
              auto pose_dict = ref_dict["pose"];
              auto pos = pose_dict["position_m"].cast<std::vector<double>>();
              auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
              reference.pose = Pose3DTO{};
              reference.pose->position_m = {pos[0], pos[1], pos[2]};
              reference.pose->orientation = {orient[0], orient[1], orient[2], orient[3]};
            }
            return reference;
          };

          goal.target_reference = parse_reference_dict(
              rel_dict["target_reference"].cast<py::dict>(), "target_reference");
          if (rel_dict.contains("offset_reference")) {
            goal.offset_reference = parse_reference_dict(
                rel_dict["offset_reference"].cast<py::dict>(), "offset_reference");
          }

          auto offset_dict = rel_dict["offset"];
          auto pos = offset_dict["position_m"].cast<std::vector<double>>();
          auto orient = offset_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.offset.position_m = {pos[0], pos[1], pos[2]};
          goal.offset.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        }

        auto kernel = require_open_kernel(self);
        if (auto admission_error =
                kernel->collision_validation_admission_error()) {
          return error_result_to_python(*admission_error);
        }

        RobotStateDTO start_state;
        if (start_state_obj.is_none()) {
          start_state = [&]() {
            py::gil_scoped_release release;
            return observer_robot_state(self);
          }();
        } else {
          py::dict start_dict = start_state_obj.cast<py::dict>();
          start_state.joint_names =
              start_dict["joint_names"].cast<std::vector<std::string>>();
          start_state.positions_rad =
              start_dict["positions_rad"].cast<std::vector<double>>();
          if (start_dict.contains("velocities_rad_s")) {
            start_state.velocities_rad_s =
                start_dict["velocities_rad_s"].cast<std::vector<double>>();
          }
        }

        PlanRequestDTO plan_request;
        plan_request.robot_id = req.robot_id;
        plan_request.group_id = req.group_id;
        plan_request.move_id = req.move_id;
        plan_request.base_frame = req.base_frame;
        plan_request.tcp_frame = req.tcp_frame;
        plan_request.start_state = start_state;
        PlannerProjectionLeaseGuard projection_lease(self);
        auto projection_binding = projection_lease.bind(plan_request);
        if (!projection_binding.ok()) {
          return error_result_to_python(projection_binding.error());
        }
        MotionSpecDTO resolved_motion;
        if (auto error = resolve_motion_spec_for_direct_plan(
                self, req, plan_request.scene_binding, resolved_motion)) {
          py::dict result;
          result["ok"] = false;
          result["error"] = error->message;
          result["error_details"] = error_details_to_python(error->details);
          return result;
        }
        plan_request.motion = std::move(resolved_motion);
        plan_request.tolerances = req.tolerances;
        plan_request.speed_profile = req.speed_profile;
        plan_request.per_joint_dynamics_limits = req.per_joint_dynamics_limits;
        plan_request.planning_timeout_s = req.timeouts.planning_timeout_s;

        auto plan_result = [&]() {
          py::gil_scoped_release release;
          return self.planner->plan(plan_request);
        }();
        py::dict result;
        if (!plan_result.trajectory.has_value()) {
          result["ok"] = false;
          if (plan_result.error.has_value()) {
            result["error"] = plan_result.error->message;
            result["error_details"] =
                error_details_to_python(plan_result.error->details);
          } else {
            result["error"] = "Planning preview failed";
          }
          return result;
        }

        auto validation_result = [&]() {
          py::gil_scoped_release release;
          return validate_direct_planned_trajectory(
              self, *plan_result.trajectory, req,
              projection_lease.inventory());
        }();
        if (!validation_result.valid) {
          result["ok"] = false;
          if (validation_result.error.has_value()) {
            result["error"] = validation_result.error->message;
            result["error_details"] =
                error_details_to_python(validation_result.error->details);
          } else {
            result["error"] = "Trajectory validation failed";
          }
          return result;
        }

        const auto& trajectory = *plan_result.trajectory;
        RobotStateDTO final_state;
        final_state.joint_names = trajectory.joint_names;
        if (!trajectory.waypoints.empty()) {
          const auto& final_wp = trajectory.waypoints.back();
          final_state.positions_rad = final_wp.positions_rad;
          final_state.velocities_rad_s = final_wp.velocities_rad_s;
        } else {
          final_state = start_state;
        }

        FkRequestDTO fk_request;
        fk_request.robot_state = final_state;
        fk_request.target_frame = req.tcp_frame;
        fk_request.base_frame = req.base_frame;
        auto final_tcp_pose = [&]() {
          py::gil_scoped_release release;
          return self.kinematics->forward_kinematics(fk_request);
        }();
        if (!final_tcp_pose.ok()) {
          result["ok"] = false;
          result["error"] = final_tcp_pose.error().message;
          result["error_details"] =
              error_details_to_python(final_tcp_pose.error().details);
          return result;
        }

        result["ok"] = true;
        result["trajectory_duration_s"] =
            trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
        result["trajectory_waypoint_count"] = trajectory.waypoints.size();
        result["final_state"] = robot_state_to_python(final_state);
        result["final_tcp_pose"] = pose_to_python(final_tcp_pose.value());
        return result;
      }, py::arg("request"), py::arg("start_state") = py::none())
      .def("plan_motion", [](NativeKernelBundle& self,
                              py::dict request,
                              py::object start_state_obj) -> py::dict {
        ExecuteRequestDTO req;
        req.robot_id = request["robot_id"].cast<std::string>();
        req.group_id = request["group_id"].cast<std::string>();
        req.move_id = request["move_id"].cast<std::string>();
        req.base_frame = request["base_frame"].cast<std::string>();
        req.tcp_frame = request["tcp_frame"].cast<std::string>();
        populate_request_correlation(req, request);

        req.tolerances.joint_position_tol_rad = request.attr("get")("joint_position_tol_rad", 0.05).cast<double>();
        req.tolerances.translation_tol_m = request.attr("get")("translation_tol_m", 0.005).cast<double>();
        req.tolerances.rotation_tol_rad = request.attr("get")("rotation_tol_rad", 0.05).cast<double>();

        req.speed_profile.velocity_scale = finite_scale_from_python(
            request.attr("get")("velocity_scale", 1.0),
            "velocity_scale");
        req.speed_profile.acceleration_scale = finite_scale_from_python(
            request.attr("get")("acceleration_scale", 1.0),
            "acceleration_scale");
        if (request.contains("max_tcp_linear_speed_mps")) {
          auto val = request["max_tcp_linear_speed_mps"];
          if (!val.is_none()) {
            double tcp_speed = val.cast<double>();
            if (tcp_speed > 0.0) {
              req.speed_profile.max_tcp_linear_speed_mps = tcp_speed;
            }
          }
        }

        // Per-joint dynamics caps (Phase 4-P5.1K). Fail closed on malformed
        // values before planning; the backend enforces vel/accel and rejects
        // jerk where it cannot honor it.
        parse_per_joint_dynamics_limits(request, req.per_joint_dynamics_limits);

        req.timeouts.planning_timeout_s = request.attr("get")("planning_timeout_s", 5.0).cast<double>();
        req.timeouts.execution_timeout_s = request.attr("get")("execution_timeout_s", 10.0).cast<double>();

        std::string cache_mode_str = request.attr("get")("cache_mode", "default").cast<std::string>();
        req.cache_mode = (cache_mode_str == "disable") ? CacheMode::kDisable : CacheMode::kDefault;

        std::string motion_type = request["motion_type"].cast<std::string>();
        if (motion_type == "joint_goal") {
          JointGoalDTO goal;
          goal.joint_names = request["joint_goal_names"].cast<std::vector<std::string>>();
          goal.positions_rad = request["joint_goal_positions_rad"].cast<std::vector<double>>();
          req.motion = goal;
        } else if (motion_type == "pose_goal") {
          PoseGoalDTO goal;
          auto pose_dict = request["pose_goal"];
          auto pos = pose_dict["position_m"].cast<std::vector<double>>();
          auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.goal.position_m = {pos[0], pos[1], pos[2]};
          goal.goal.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        } else if (motion_type == "relative_move") {
          RelativeMoveDTO goal;
          auto rel_dict = request["relative_move"];
          auto parse_reference_dict = [](const py::dict& ref_dict,
                                         const std::string& field_name) {
            ReferenceDTO reference;
            const std::string ref_type = ref_dict["type"].cast<std::string>();
            if (ref_type == "frame") {
              reference.type = ReferenceDTO::Type::kFrame;
            } else if (ref_type == "object") {
              reference.type = ReferenceDTO::Type::kObject;
            } else if (ref_type == "robot_link") {
              reference.type = ReferenceDTO::Type::kRobotLink;
            } else if (ref_type == "pose") {
              reference.type = ReferenceDTO::Type::kPose;
            } else {
              throw std::runtime_error("Unsupported reference type: " + ref_type);
            }
            if (ref_dict.contains("id")) {
              reference.id = ref_dict["id"].cast<std::string>();
            }
            if (reference.type == ReferenceDTO::Type::kPose) {
              if (!ref_dict.contains("pose")) {
                throw std::runtime_error(
                    "Pose reference requires " + field_name + ".pose");
              }
              auto pose_dict = ref_dict["pose"];
              auto pos = pose_dict["position_m"].cast<std::vector<double>>();
              auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
              reference.pose = Pose3DTO{};
              reference.pose->position_m = {pos[0], pos[1], pos[2]};
              reference.pose->orientation = {orient[0], orient[1], orient[2], orient[3]};
            }
            return reference;
          };

          goal.target_reference = parse_reference_dict(
              rel_dict["target_reference"].cast<py::dict>(), "target_reference");
          if (rel_dict.contains("offset_reference")) {
            goal.offset_reference = parse_reference_dict(
                rel_dict["offset_reference"].cast<py::dict>(), "offset_reference");
          }

          auto offset_dict = rel_dict["offset"];
          auto pos = offset_dict["position_m"].cast<std::vector<double>>();
          auto orient = offset_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.offset.position_m = {pos[0], pos[1], pos[2]};
          goal.offset.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        }

        auto kernel = require_open_kernel(self);
        if (auto admission_error =
                kernel->collision_validation_admission_error()) {
          return error_result_to_python(*admission_error);
        }

        RobotStateDTO start_state;
        if (start_state_obj.is_none()) {
          start_state = [&]() {
            py::gil_scoped_release release;
            return observer_robot_state(self);
          }();
        } else {
          py::dict start_dict = start_state_obj.cast<py::dict>();
          start_state.joint_names =
              start_dict["joint_names"].cast<std::vector<std::string>>();
          start_state.positions_rad =
              start_dict["positions_rad"].cast<std::vector<double>>();
          if (start_dict.contains("velocities_rad_s")) {
            start_state.velocities_rad_s =
                start_dict["velocities_rad_s"].cast<std::vector<double>>();
          }
        }

        PlanRequestDTO plan_request;
        plan_request.robot_id = req.robot_id;
        plan_request.group_id = req.group_id;
        plan_request.move_id = req.move_id;
        plan_request.base_frame = req.base_frame;
        plan_request.tcp_frame = req.tcp_frame;
        plan_request.start_state = start_state;
        PlannerProjectionLeaseGuard projection_lease(self);
        auto projection_binding = projection_lease.bind(plan_request);
        if (!projection_binding.ok()) {
          return error_result_to_python(projection_binding.error());
        }
        MotionSpecDTO resolved_motion;
        if (auto error = resolve_motion_spec_for_direct_plan(
                self, req, plan_request.scene_binding, resolved_motion)) {
          py::dict result;
          result["ok"] = false;
          result["error"] = error->message;
          result["error_details"] = error_details_to_python(error->details);
          return result;
        }
        plan_request.motion = std::move(resolved_motion);
        plan_request.tolerances = req.tolerances;
        plan_request.speed_profile = req.speed_profile;
        plan_request.per_joint_dynamics_limits = req.per_joint_dynamics_limits;
        plan_request.planning_timeout_s = req.timeouts.planning_timeout_s;

        auto plan_result = [&]() {
          py::gil_scoped_release release;
          return self.planner->plan(plan_request);
        }();
        py::dict result;
        if (!plan_result.trajectory.has_value()) {
          result["ok"] = false;
          if (plan_result.error.has_value()) {
            result["error"] = plan_result.error->message;
            result["error_details"] =
                error_details_to_python(plan_result.error->details);
          } else {
            result["error"] = "Planning failed";
          }
          return result;
        }

        auto validation_result = [&]() {
          py::gil_scoped_release release;
          return validate_direct_planned_trajectory(
              self, *plan_result.trajectory, req,
              projection_lease.inventory());
        }();
        if (!validation_result.valid) {
          result["ok"] = false;
          if (validation_result.error.has_value()) {
            result["error"] = validation_result.error->message;
            result["error_details"] =
                error_details_to_python(validation_result.error->details);
          } else {
            result["error"] = "Trajectory validation failed";
          }
          return result;
        }

        const auto& trajectory = *plan_result.trajectory;
        RobotStateDTO final_state;
        final_state.joint_names = trajectory.joint_names;
        if (!trajectory.waypoints.empty()) {
          const auto& final_wp = trajectory.waypoints.back();
          final_state.positions_rad = final_wp.positions_rad;
          final_state.velocities_rad_s = final_wp.velocities_rad_s;
        } else {
          final_state = start_state;
        }

        FkRequestDTO fk_request;
        fk_request.robot_state = final_state;
        fk_request.target_frame = req.tcp_frame;
        fk_request.base_frame = req.base_frame;
        auto final_tcp_pose = [&]() {
          py::gil_scoped_release release;
          return self.kinematics->forward_kinematics(fk_request);
        }();
        if (!final_tcp_pose.ok()) {
          result["ok"] = false;
          result["error"] = final_tcp_pose.error().message;
          result["error_details"] =
              error_details_to_python(final_tcp_pose.error().details);
          return result;
        }

        result["ok"] = true;
        result["trajectory_duration_s"] =
            trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
        result["trajectory_waypoint_count"] = trajectory.waypoints.size();
        result["final_state"] = robot_state_to_python(final_state);
        result["final_tcp_pose"] = pose_to_python(final_tcp_pose.value());
        result["trajectory"] = trajectory_to_python(trajectory);
        return result;
      }, py::arg("request"), py::arg("start_state") = py::none(),
      "Plan and validate a motion, returning the time-parameterized trajectory.")
      .def("stop", [](NativeKernelBundle& self) -> py::dict {
        auto kernel = require_open_kernel(self);
        auto r = [&]() {
          py::gil_scoped_release release;
          return kernel->stop();
        }();
        return command_result(r.accepted, r.error);
      })
      .def("pause", [](NativeKernelBundle& self) -> py::dict {
        auto kernel = require_open_kernel(self);
        auto r = [&]() {
          py::gil_scoped_release release;
          return kernel->pause();
        }();
        return command_result(r.accepted, r.error);
      })
      .def("control_command_epoch", [](NativeKernelBundle& self) {
        auto kernel = require_open_kernel(self);
        py::gil_scoped_release release;
        return kernel->control_command_epoch();
      })
      .def("bump_control_command_epoch", [](NativeKernelBundle& self) {
        auto kernel = require_open_kernel(self);
        py::gil_scoped_release release;
        return kernel->bump_control_command_epoch();
      })
      .def("resume", [](NativeKernelBundle& self,
                        py::object expected_epoch) -> py::dict {
        auto kernel = require_open_kernel(self);
        auto epoch = optional_uint64_from_python(expected_epoch);
        auto r = [&]() {
          py::gil_scoped_release release;
          return kernel->resume(epoch);
        }();
        return command_result(r.accepted, r.error);
      }, py::arg("expected_epoch") = py::none())
      .def("reset", [](NativeKernelBundle& self,
                       py::object expected_epoch) -> py::dict {
        auto kernel = require_open_kernel(self);
        auto epoch = optional_uint64_from_python(expected_epoch);
        auto r = [&]() {
          py::gil_scoped_release release;
          return kernel->reset(epoch);
        }();
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
        // The kernel rejects reset while already idle, but ROS-side auxiliary
        // channels such as JointState gripper commands can still hold adapter
        // state when used directly from the native bundle. Keep the low-level
        // binding idempotent so reset reliably clears the deployment adapter.
        if (!r.accepted && !error_reason_is_stale_control_command(r.error)) {
          try {
            auto backend_result = [&]() {
              py::gil_scoped_release release;
              return require_ros2_control_execution(self).reset();
            }();
            if (backend_result.accepted) {
              return command_result(backend_result.accepted,
                                    backend_result.error);
            }
          } catch (const std::runtime_error&) {
            // Non-ROS bundles keep the kernel result below.
          }
        }
#endif
        return command_result(r.accepted, r.error);
      }, py::arg("expected_epoch") = py::none())
      .def("get_motion_interrupt_stop_capability",
           [](NativeKernelBundle& self) -> py::dict {
             // Read-only view of the active execution backend's advertised
             // stop capability, used by the composer's deployment load-time
             // gate to fail closed when a configured stop profile names an
             // unsupported mode. Backend-agnostic (native + ros2_control).
             auto kernel = require_open_kernel(self);
             py::dict d;
             py::list modes;
             const auto cap = kernel->get_motion_interrupt_stop_capability();
             const auto mode_to_string =
                 [](MotionInterruptStopMode m) -> const char* {
               switch (m) {
                 case MotionInterruptStopMode::kSpringBackHold:
                   return "spring_back_hold";
                 case MotionInterruptStopMode::kSoftOnPath:
                   return "soft_on_path";
                 case MotionInterruptStopMode::kHardDeceleration:
                   return "hard_deceleration";
               }
               return "unknown";
             };
             for (const auto& m : cap.supported_stop_modes) {
               modes.append(std::string(mode_to_string(m)));
             }
             if (cap.capability_profile_id) {
               d["capability_profile_id"] = *cap.capability_profile_id;
             } else {
               d["capability_profile_id"] = py::none();
             }
             const auto set_optional_string =
                 [&](const char* key, const std::optional<std::string>& value) {
                   if (value) {
                     d[key] = *value;
                   } else {
                     d[key] = py::none();
                   }
                 };
             set_optional_string("hard_stop_level", cap.hard_stop_level);
             set_optional_string("derivative_proof_level",
                                 cap.derivative_proof_level);
             set_optional_string("scaling_source_visibility",
                                 cap.scaling_source_visibility);
             set_optional_string("scaling_source_probe_status",
                                 cap.scaling_source_probe_status);
             py::dict unsupported_reasons;
             for (const auto& [mode, reason] : cap.unsupported_reasons) {
               unsupported_reasons[py::str(mode)] = reason;
             }
             d["unsupported_reasons"] = unsupported_reasons;
             d["supported_stop_modes"] = modes;
             return d;
           })
      .def("acknowledge_completed", [](NativeKernelBundle& self,
                                        uint64_t execution_id,
                                        py::object expected_epoch) -> py::dict {
        auto kernel = require_open_kernel(self);
        auto epoch = optional_uint64_from_python(expected_epoch);
        auto r = [&]() {
          py::gil_scoped_release release;
          return kernel->acknowledge_completed(execution_id, epoch);
        }();
        return command_result(r.accepted, r.error);
      }, py::arg("execution_id"), py::arg("expected_epoch") = py::none())
      .def("get_recovery_assessment",
           [](NativeKernelBundle& self) -> py::dict {
             auto kernel = require_open_kernel(self);
             auto assessment = [&]() {
               py::gil_scoped_release release;
               return kernel->get_recovery_assessment();
             }();
             return recovery_assessment_to_python(assessment);
           })
      .def("apply_recovery_action",
           [](NativeKernelBundle& self, py::dict request_dict) -> py::dict {
             auto kernel = require_open_kernel(self);
             KernelRecoveryActionRequestDTO request;
             request.request_id =
                 request_dict.attr("get")("request_id", "").cast<std::string>();
             const auto action_name =
                 request_dict.attr("get")("action", "").cast<std::string>();
             if (action_name == "retry_motion_from_current_state") {
               request.action = RecoveryAction::kRetryMotionFromCurrentState;
             } else if (action_name == "acknowledge_and_continue") {
               request.action = RecoveryAction::kAcknowledgeAndContinue;
             } else if (action_name == "retry_after_scene_repair") {
               request.action = RecoveryAction::kRetryAfterSceneRepair;
             } else if (action_name == "replan_from_stopped_state") {
               request.action = RecoveryAction::kReplanFromStoppedState;
             } else if (action_name == "reset_cycle") {
               request.action = RecoveryAction::kResetCycle;
             } else {
               py::dict rejected;
               rejected["request_id"] =
                   request_dict.attr("get")("request_id", "");
               rejected["action"] = action_name;
               rejected["accepted"] = false;
               py::dict error_details;
               error_details["reason"] = "action_not_allowed";
               py::dict error;
               error["code"] = "recovery_action_rejected";
               error["message"] =
                   std::string("unknown recovery action: ") + action_name;
               error["details"] = error_details;
               rejected["error"] = error;
               const auto assessment = [&]() {
                 py::gil_scoped_release release;
                 return kernel->get_recovery_assessment();
               }();
               rejected["resulting_assessment"] =
                   recovery_assessment_to_python(assessment);
               return rejected;
             }
             request.principal_id =
                 request_dict.attr("get")("principal_id", "").cast<std::string>();
             request.expected_assessment_revision =
                 request_dict.attr("get")("expected_assessment_revision", 0)
                     .cast<std::uint64_t>();
             request.expected_control_command_epoch =
                 request_dict.attr("get")("expected_control_command_epoch", 0)
                     .cast<std::uint64_t>();
             request.expected_execution_id =
                 request_dict.attr("get")("expected_execution_id", 0)
                     .cast<std::uint64_t>();
             request.expected_latch_id =
                 request_dict.attr("get")("expected_latch_id", "")
                     .cast<std::string>();
             request.expected_stop_certificate_id =
                 request_dict.attr("get")("expected_stop_certificate_id", "")
                     .cast<std::string>();
             request.expected_stop_owner_generation =
                 request_dict.attr("get")("expected_stop_owner_generation", 0)
                     .cast<std::uint64_t>();
             request.reason =
                 request_dict.attr("get")("reason", "").cast<std::string>();
             auto result = [&]() {
               py::gil_scoped_release release;
               return kernel->apply_recovery_action(request);
             }();
             py::dict d;
             d["request_id"] = result.request_id;
             d["action"] = recovery_action_name(result.action);
             d["accepted"] = result.accepted;
             d["resulting_assessment"] =
                 recovery_assessment_to_python(result.resulting_assessment);
             if (result.error.has_value()) {
               py::dict details;
               for (const auto& [key, value] : result.error->details) {
                 details[py::str(key)] = value;
               }
               py::dict error;
               error["code"] = result.error->code;
               error["message"] = result.error->message;
               error["details"] = details;
               d["error"] = error;
             } else {
               d["error"] = py::none();
             }
             return d;
           },
           py::arg("request"))
      .def("set_runtime_velocity_scale",
           [](NativeKernelBundle& self, double scale, py::object principal_id) {
             std::optional<std::string> principal;
             if (!principal_id.is_none()) {
               principal = principal_id.cast<std::string>();
             }
             auto kernel = require_open_kernel(self);
             py::gil_scoped_release release;
             kernel->set_runtime_velocity_scale(scale, std::move(principal));
           },
           py::arg("scale"),
           py::arg("principal_id") = py::none())
      .def("get_status", [](NativeKernelBundle& self) -> py::dict {
        auto kernel = require_open_kernel(self);
        const auto status = [&]() {
          py::gil_scoped_release release;
          return kernel->get_status();
        }();
        return status_to_python(status);
      })
      .def("get_execution_snapshot", [](NativeKernelBundle& self) -> py::dict {
        auto kernel = require_open_kernel(self);
        const auto snapshot = [&]() {
          py::gil_scoped_release release;
          return kernel->get_execution_snapshot();
        }();
        return snapshot_to_python(snapshot);
      })
      .def("clock_anchor", [](NativeKernelBundle& self) -> py::dict {
        // Monotonic<->wall anchor for the observability pipeline (plan OW2):
        // sample the SAME IClock instance that stamps kernel events, and wall
        // time, back-to-back so no drain latency sits between them.
        (void)require_open_kernel(self);
        std::int64_t monotonic_ns = 0;
        std::int64_t wall_time_ns = 0;
        {
          py::gil_scoped_release release;
          monotonic_ns = self.clock->now();
          wall_time_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
        }
        py::dict out;
        out["monotonic_ns"] = monotonic_ns;
        out["wall_time_ns"] = wall_time_ns;
        return out;
      })
      .def("set_flight_recorder_dir", [](NativeKernelBundle& self,
                                         const std::string& directory) {
        auto kernel = require_open_kernel(self);
        py::gil_scoped_release release;
        kernel->set_flight_recorder_directory(directory);
      }, py::arg("directory"))
      .def("dump_flight_record", [](NativeKernelBundle& self,
                                    const std::string& reason) -> std::string {
        auto kernel = require_open_kernel(self);
        py::gil_scoped_release release;
        return kernel->dump_flight_record(reason.c_str());
      }, py::arg("reason"))
      .def("get_metrics", [](NativeKernelBundle& self) -> py::dict {
        auto kernel = require_open_kernel(self);
        const auto metrics = [&]() {
          py::gil_scoped_release release;
          return kernel->get_metrics();
        }();
        auto out = metrics_to_python(metrics);
        if (auto* native_execution =
                dynamic_cast<RoboPlanExecutionBackend*>(self.execution.get())) {
          const auto projection_metrics = [&]() {
            py::gil_scoped_release release;
            return native_execution->scene_projection_metrics();
          }();
          out["scene_projection_misses_total"] =
              projection_metrics.misses_total;
          out["scene_projection_successful_updates_total"] =
              projection_metrics.successful_updates_total;
          out["scene_projection_lag_max_ns"] =
              projection_metrics.lag_max_ns;
          out["scene_projection_last_success_timestamp_ns"] =
              projection_metrics.last_success_timestamp_ns;
          out["scene_projection_last_success_age_ns"] =
              projection_metrics.last_success_age_ns;
          out["scene_projection_terminal_retries_scheduled_total"] =
              projection_metrics.terminal_retries_scheduled_total;
          out["scene_projection_terminal_retry_successes_total"] =
              projection_metrics.terminal_retry_successes_total;
          out["scene_projection_terminal_retry_exhausted_total"] =
              projection_metrics.terminal_retry_exhausted_total;
        }
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
        if (auto* ros_execution =
                dynamic_cast<ros2_exec::Ros2ControlExecutionBackend*>(
                    self.execution.get())) {
          const auto projection_metrics = [&]() {
            py::gil_scoped_release release;
            return ros_execution->scene_projection_metrics();
          }();
          out["scene_projection_misses_total"] =
              projection_metrics.misses_total;
          out["scene_projection_successful_updates_total"] =
              projection_metrics.successful_updates_total;
          out["scene_projection_lag_max_ns"] =
              projection_metrics.lag_max_ns;
          out["scene_projection_last_success_timestamp_ns"] =
              projection_metrics.last_success_timestamp_ns;
          out["scene_projection_last_success_age_ns"] =
              projection_metrics.last_success_age_ns;
          out["scene_projection_terminal_retries_scheduled_total"] =
              projection_metrics.terminal_retries_scheduled_total;
          out["scene_projection_terminal_retry_successes_total"] =
              projection_metrics.terminal_retry_successes_total;
          out["scene_projection_terminal_retry_exhausted_total"] =
              projection_metrics.terminal_retry_exhausted_total;
          const auto progress_metrics = [&]() {
            py::gil_scoped_release release;
            return ros_execution->controller_progress_metrics();
          }();
          out["controller_executor_thread_count"] =
              progress_metrics.executor_thread_count;
          out["controller_state_callbacks_total"] =
              progress_metrics.state_callbacks_total;
          out["controller_action_feedback_callbacks_total"] =
              progress_metrics.action_feedback_callbacks_total;
          out["controller_action_result_callbacks_total"] =
              progress_metrics.action_result_callbacks_total;
          out["controller_command_progress_samples_total"] =
              progress_metrics.command_progress_samples_total;
          out["controller_cancel_confirmations_total"] =
              progress_metrics.cancel_confirmations_total;
          out["controller_stop_confirmations_total"] =
              progress_metrics.stop_confirmations_total;
          out["controller_latest_state_sample_age_ns"] =
              progress_metrics.latest_state_sample_age_ns;
          out["controller_last_command_progress_latency_ns"] =
              progress_metrics.last_command_progress_latency_ns;
          out["controller_max_command_progress_latency_ns"] =
              progress_metrics.max_command_progress_latency_ns;
          out["controller_last_action_result_latency_ns"] =
              progress_metrics.last_action_result_latency_ns;
          out["controller_max_action_result_latency_ns"] =
              progress_metrics.max_action_result_latency_ns;
          out["controller_last_cancel_latency_ns"] =
              progress_metrics.last_cancel_latency_ns;
          out["controller_max_cancel_latency_ns"] =
              progress_metrics.max_cancel_latency_ns;
          out["controller_last_stop_confirmation_latency_ns"] =
              progress_metrics.last_stop_confirmation_latency_ns;
          out["controller_max_stop_confirmation_latency_ns"] =
              progress_metrics.max_stop_confirmation_latency_ns;
          out["controller_observed_state_reader_retry_exhausted_total"] =
              progress_metrics.observed_state_reader_retry_exhausted_total;
          py::list observed_sources;
          for (const auto& source : progress_metrics.observed_state_sources) {
            py::dict source_metrics;
            source_metrics["source_kind"] =
                observed_robot_state_source_kind_name(source.source_kind);
            source_metrics["source_id"] = source.source_id;
            source_metrics["published_total"] = source.published_total;
            source_metrics["incomplete_total"] = source.incomplete_total;
            source_metrics["writer_busy_total"] = source.writer_busy_total;
            source_metrics["latest_received_timestamp_ns"] =
                source.latest_received_timestamp_ns;
            source_metrics["max_interarrival_ns"] =
                source.max_interarrival_ns;
            observed_sources.append(std::move(source_metrics));
          }
          out["controller_observed_state_sources"] =
              std::move(observed_sources);
        }
#endif
        return out;
      })
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
      .def("configure_ros_gripper_action",
          [](NativeKernelBundle& self, const std::string& gripper_id,
             py::dict config_dict) {
            ros2_exec::Ros2GripperActionConfig config;
            if (config_dict.contains("action_name")) {
              config.action_name =
                  config_dict["action_name"].cast<std::string>();
            }
            if (config_dict.contains("transport_kind")) {
              config.transport_kind =
                  config_dict["transport_kind"].cast<std::string>();
            }
            if (config_dict.contains("command_topic")) {
              config.command_topic =
                  config_dict["command_topic"].cast<std::string>();
            }
            if (config_dict.contains("state_topic")) {
              config.state_topic =
                  config_dict["state_topic"].cast<std::string>();
            }
            if (config_dict.contains("command_joint_names")) {
              config.command_joint_names =
                  config_dict["command_joint_names"].cast<std::vector<std::string>>();
            }
            if (config_dict.contains("group_id")) {
              config.group_id = config_dict["group_id"].cast<std::string>();
            }
            if (config_dict.contains("joint_name")) {
              config.joint_name =
                  canonical_joint_name(
                      self, config_dict["joint_name"].cast<std::string>());
            }
            if (config_dict.contains("completion_policy")) {
              config.completion_policy =
                  config_dict["completion_policy"].cast<std::string>();
            }
            if (config_dict.contains("open_position")) {
              config.open_position =
                  finite_scale_from_python(config_dict["open_position"],
                                           "open_position");
            }
            if (config_dict.contains("closed_position")) {
              config.closed_position =
                  finite_scale_from_python(config_dict["closed_position"],
                                           "closed_position");
            }
            if (config_dict.contains("open_effort")) {
              config.open_effort =
                  finite_scale_from_python(config_dict["open_effort"],
                                           "open_effort");
            }
            if (config_dict.contains("close_effort")) {
              config.close_effort =
                  finite_scale_from_python(config_dict["close_effort"],
                                           "close_effort");
            }
            if (config_dict.contains("publish_nominal_position")) {
              config.publish_nominal_position =
                  config_dict["publish_nominal_position"].cast<bool>();
            }
            if (config_dict.contains("state_freshness_timeout_s")) {
              config.state_freshness_timeout_s =
                  finite_scale_from_python(
                      config_dict["state_freshness_timeout_s"],
                      "state_freshness_timeout_s");
            }
            if (config_dict.contains("wait_for_action_server_timeout_s")) {
              config.wait_for_action_server_timeout_s =
                  finite_scale_from_python(
                      config_dict["wait_for_action_server_timeout_s"],
                      "wait_for_action_server_timeout_s");
            }
            if (config_dict.contains("result_timeout_s")) {
              config.result_timeout_s =
                  finite_scale_from_python(config_dict["result_timeout_s"],
                                           "result_timeout_s");
            }
            require_ros2_control_execution(self).configure_gripper_action(
                gripper_id, std::move(config));
          },
          py::arg("gripper_id"), py::arg("config"))
      // Command-enable gate: the ros2_control backend is the single
      // publishing authority (reliable + transient_local). Returns false
      // when the deployment configures no gate topic.
      .def("set_command_gate", [](NativeKernelBundle& self, bool enabled) {
        auto& backend = require_ros2_control_execution(self);
        py::gil_scoped_release release;
        return backend.request_command_gate(enabled);
      }, py::arg("enabled"))
      .def("command_gate_enabled", [](NativeKernelBundle& self) {
        auto& backend = require_ros2_control_execution(self);
        py::gil_scoped_release release;
        return backend.command_gate_enabled();
      })
      .def("command_gate_configured", [](NativeKernelBundle& self) {
        auto& backend = require_ros2_control_execution(self);
        py::gil_scoped_release release;
        return backend.command_gate_configured();
      })
#endif
      .def("command_gripper",
          [](NativeKernelBundle& self, py::dict request) -> py::dict {
            auto kernel = require_open_kernel(self);
            const auto dto = gripper_request_from_python(std::move(request));
            const auto result = [&]() {
              py::gil_scoped_release release;
              return kernel->command_gripper(dto);
            }();
            return gripper_command_result_to_python(result);
          },
          py::arg("request"))
      .def("get_gripper_status",
          [](NativeKernelBundle& self,
             const std::string& gripper_id) -> py::dict {
            auto kernel = require_open_kernel(self);
            const auto status = [&]() {
              py::gil_scoped_release release;
              return kernel->get_gripper_status(gripper_id);
            }();
            return gripper_status_to_python(status);
          },
          py::arg("gripper_id"))
      .def("mark_gripper_grasp_lost",
          [](NativeKernelBundle& self, const std::string& gripper_id,
             std::uint64_t expected_command_id,
             std::uint64_t expected_generation,
             const std::string& evidence_label, const std::string& message,
             const std::string& reason_code) -> py::dict {
            auto kernel = require_open_kernel(self);
            const auto result = [&]() {
              py::gil_scoped_release release;
              return kernel->mark_gripper_grasp_lost(
                  gripper_id, expected_command_id, expected_generation,
                  evidence_label, message, reason_code);
            }();
            return gripper_command_result_to_python(result);
          },
          py::arg("gripper_id"), py::arg("expected_command_id"),
          py::arg("expected_generation"), py::arg("evidence_label"),
          py::arg("message"),
          py::arg("reason_code") = "grasp_confirmation_lost")
      .def("get_scene_object_ids", [](NativeKernelBundle& self) {
        const auto objects = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->scene_objects();
        }();
        std::vector<std::string> ids;
        ids.reserve(objects.size());
        for (const auto& object : objects) {
          ids.push_back(object.object_id);
        }
        return ids;
      })
      .def("get_scene_authority_status", [](NativeKernelBundle& self) {
        const auto status = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->authority_status();
        }();
        py::dict result;
        result["revision_id"] = status.revision.revision_id;
        result["content_hash"] = status.revision.content_hash;
        result["transaction_sequence"] = status.transaction_sequence;
        result["scene_admission_epoch"] = status.admission_epoch;
        result["topology_epoch"] = status.topology_epoch;
        result["policy_epoch"] = status.policy_epoch;
        return result;
      })
      .def("get_scene_authority_snapshot", [](NativeKernelBundle& self) {
        const auto snapshot = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->logical_snapshot();
        }();
        if (!snapshot) {
          throw std::runtime_error(
              "Logical scene authority snapshot is unavailable");
        }
        return logical_scene_snapshot_to_python(*snapshot);
      })
      .def("get_joint_names", [](NativeKernelBundle& self) {
        const auto names = [&]() {
          py::gil_scoped_release release;
          return qualified_group_joint_names(self, self.group_name);
        }();
        return canonical_joint_names(self, names);
      })
      .def("get_scene_objects", [](NativeKernelBundle& self) -> py::list {
        const auto objects = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->scene_objects();
        }();
        py::list result;
        for (const auto& object : objects) {
          result.append(scene_object_snapshot_to_python(object));
        }
        return result;
      })
      .def("invalidate_trajectories", [](NativeKernelBundle& self) {
        // Audit 6E (finding 2.13): release the GIL through the native cache
        // mutation, matching the neighbouring defs — under scene-mutation
        // contention these otherwise pin the GIL for milliseconds and stall
        // unrelated Python threads (status pollers, visual publisher).
        py::gil_scoped_release release;
        self.cache->invalidate_all();
      })
      .def("set_robot_base_pose", [](NativeKernelBundle& self, py::dict pose) {
        // Convert from Python BEFORE releasing the GIL (pose_from_python
        // touches the py::dict); only the native mutation runs unlocked.
        auto native_pose = pose_from_python(pose);
        const auto result = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->set_robot_base_pose(
              SetRobotBasePoseRequestDTO{self.robot_id,
                                         std::move(native_pose)});
        }();
        if (!result.success) {
          throw std::runtime_error(
              result.error ? result.error->message
                           : "Failed to commit robot base pose");
        }
        {
          py::gil_scoped_release release;
          self.cache->invalidate_all();
        }
      }, py::arg("pose"))
      .def("scene_edit", [](NativeKernelBundle& self, py::dict request) -> py::dict {
        py::dict result;
        std::string op = request["operation"].cast<std::string>();

        if (op == "add" || op == "upsert") {
          AddObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          req.support_contacts = support_contacts_from_python(request);

          // Pose
          if (request.contains("pose_position_m")) {
            auto pos = request["pose_position_m"].cast<std::vector<double>>();
            req.pose.position_m = {pos[0], pos[1], pos[2]};
          }
          if (request.contains("pose_orientation_xyzw")) {
            auto orient = request["pose_orientation_xyzw"].cast<std::vector<double>>();
            req.pose.orientation = {orient[0], orient[1], orient[2], orient[3]};
          } else {
            req.pose.orientation = {0, 0, 0, 1};
          }

          // Shape
          std::string shape_type = request.attr("get")("shape_type", "box").cast<std::string>();
          if (shape_type == "box") {
            if (!request.contains("box_dimensions_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=box requires box_dimensions_m";
              return result;
            }
            auto dims = request["box_dimensions_m"].cast<std::vector<double>>();
            if (dims.size() != 3) {
              result["success"] = false;
              result["error"] = "box_dimensions_m must have exactly 3 elements";
              return result;
            }
            req.shape = BoxShapeDTO{{dims[0], dims[1], dims[2]}};
          } else if (shape_type == "sphere") {
            if (!request.contains("sphere_radius_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=sphere requires sphere_radius_m";
              return result;
            }
            double radius = request["sphere_radius_m"].cast<double>();
            req.shape = SphereShapeDTO{radius};
          } else if (shape_type == "cylinder") {
            if (!request.contains("cylinder_radius_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=cylinder requires cylinder_radius_m";
              return result;
            }
            if (!request.contains("cylinder_height_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=cylinder requires cylinder_height_m";
              return result;
            }
            double radius = request["cylinder_radius_m"].cast<double>();
            double height = request["cylinder_height_m"].cast<double>();
            req.shape = CylinderShapeDTO{radius, height};
          } else if (shape_type == "mesh") {
            if (!request.contains("mesh_resource_uri")) {
              result["success"] = false;
              result["error"] = "add with shape_type=mesh requires mesh_resource_uri";
              return result;
            }
            MeshShapeDTO mesh;
            mesh.resource_uri = request["mesh_resource_uri"].cast<std::string>();
            mesh.scale = {1.0, 1.0, 1.0};
            if (request.contains("mesh_scale")) {
              auto scale = request["mesh_scale"].cast<std::vector<double>>();
              if (scale.size() != 3) {
                result["success"] = false;
                result["error"] = "mesh_scale must have exactly 3 elements";
                return result;
              }
              mesh.scale = {scale[0], scale[1], scale[2]};
            }
            req.shape = mesh;
          } else {
            result["success"] = false;
            result["error"] = "Unsupported shape_type for native backend: " + shape_type;
            return result;
          }

          if (op == "upsert") {
            auto state = [&]() {
              py::gil_scoped_release release;
              return self.scene_authority->object_state(req.object_id);
            }();
            if (state.exists && state.attached) {
              result["success"] = false;
              result["error"] = "Cannot upsert attached object";
              return result;
            }
            if (state.exists) {
              RemoveObjectRequestDTO remove_req;
              remove_req.object_id = req.object_id;
              auto remove_result = [&]() {
                py::gil_scoped_release release;
                return self.scene_authority->remove_object(remove_req);
              }();
              if (!remove_result.success) {
                result["success"] = false;
                if (remove_result.error.has_value()) {
                  result["error"] = remove_result.error->message;
                } else {
                  result["error"] = "Failed to remove existing object before upsert";
                }
                return result;
              }
            }
          }

          auto r = [&]() {
            py::gil_scoped_release release;
            return self.scene_authority->add_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;

        } else if (op == "remove") {
          RemoveObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.scene_authority->remove_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;

        } else if (op == "update_pose") {
          UpdateObjectPoseRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          if (request.contains("pose_position_m")) {
            auto pos = request["pose_position_m"].cast<std::vector<double>>();
            req.pose.position_m = {pos[0], pos[1], pos[2]};
          }
          if (request.contains("pose_orientation_xyzw")) {
            auto orient = request["pose_orientation_xyzw"].cast<std::vector<double>>();
            req.pose.orientation = {orient[0], orient[1], orient[2], orient[3]};
          } else {
            req.pose.orientation = {0, 0, 0, 1};
          }
          if (request.contains("source_timestamp_ns") &&
              !request["source_timestamp_ns"].is_none()) {
            req.source_timestamp_ns =
                request["source_timestamp_ns"].cast<int64_t>();
          }
          if (request.contains("source_freshness_budget_ns") &&
              !request["source_freshness_budget_ns"].is_none()) {
            req.source_freshness_budget_ns =
                request["source_freshness_budget_ns"].cast<int64_t>();
          }
          if (request.contains("source_id") && !request["source_id"].is_none()) {
            req.source_id = request["source_id"].cast<std::string>();
          }
          if (request.contains("scene_admission_role") &&
              !request["scene_admission_role"].is_none()) {
            req.scene_admission_role =
                request["scene_admission_role"].cast<std::string>();
          }
          if (request.contains("scene_admission_lease_max_age_ns") &&
              !request["scene_admission_lease_max_age_ns"].is_none()) {
            req.scene_admission_lease_max_age_ns =
                request["scene_admission_lease_max_age_ns"].cast<int64_t>();
          }
          if (request.contains("scene_admission_target_drift_tolerance_m") &&
              !request["scene_admission_target_drift_tolerance_m"].is_none()) {
            req.scene_admission_target_drift_tolerance_m =
                request["scene_admission_target_drift_tolerance_m"].cast<double>();
          }
          if (request.contains("scene_admission_target_angular_tolerance_rad") &&
              !request["scene_admission_target_angular_tolerance_rad"].is_none()) {
            req.scene_admission_target_angular_tolerance_rad =
                request["scene_admission_target_angular_tolerance_rad"].cast<double>();
          }
          if (request.contains("scene_admission_obstacle_drift_tolerance_m") &&
              !request["scene_admission_obstacle_drift_tolerance_m"].is_none()) {
            req.scene_admission_obstacle_drift_tolerance_m =
                request["scene_admission_obstacle_drift_tolerance_m"].cast<double>();
          }
          if (request.contains("scene_admission_obstacle_angular_tolerance_rad") &&
              !request["scene_admission_obstacle_angular_tolerance_rad"].is_none()) {
            req.scene_admission_obstacle_angular_tolerance_rad =
                request["scene_admission_obstacle_angular_tolerance_rad"].cast<double>();
          }
          if (request.contains("scene_admission_allow_runtime_dynamic_obstacle") &&
              !request["scene_admission_allow_runtime_dynamic_obstacle"].is_none()) {
            req.scene_admission_allow_runtime_dynamic_obstacle =
                request["scene_admission_allow_runtime_dynamic_obstacle"].cast<bool>();
          }
          if (request.contains("expected_attached") &&
              !request["expected_attached"].is_none()) {
            req.expected_attached = request["expected_attached"].cast<bool>();
          }
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.scene_authority->update_object_pose(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) {
            result["error"] = r.error->message;
            result["error_code"] = r.error->code;
            result["error_details"] = r.error->details;
          }
          if (r.update_status.has_value()) result["update_status"] = *r.update_status;
          result["collision_world_revision_changed"] =
              r.collision_world_revision_changed;
          result["collision_world_freshness_changed"] =
              r.collision_world_freshness_changed;
        } else if (op == "attach") {
          AttachObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "").cast<std::string>();
          if (request.contains("touch_links") &&
              !request["touch_links"].is_none()) {
            req.touch_links = request["touch_links"].cast<std::vector<std::string>>();
          }
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.scene_authority->attach_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else if (op == "detach") {
          DetachObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.scene_authority->detach_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else {
          result["success"] = false;
          result["error"] = "Unsupported scene edit operation: " + op;
        }

        return result;
      })
      .def("set_joint_positions", [](NativeKernelBundle& self,
                                      const std::vector<double>& positions) {
        auto kernel = require_open_kernel(self);
        Eigen::VectorXd q(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
          q[i] = positions[i];
        }
        {
          py::gil_scoped_release release;
          auto scene_lock = lock_bundle_scene(self);
          if (self.scene &&
              q.size() != self.scene->getCurrentJointPositions().size()) {
            q = self.scene->toFullJointPositions(self.group_name, q);
          }
          self.scene->setJointPositions(q);
        }
        {
          py::gil_scoped_release release;
          auto refreshed = kernel->refresh_observed_state_snapshot();
          if (!refreshed.ok()) {
            throw std::runtime_error(refreshed.error().message);
          }
        }
      }, py::arg("positions"),
      "Set the robot's current joint positions in the planning scene.")
      .def("get_group_joint_names", [](NativeKernelBundle& self,
                                        const std::string& group_name) {
        std::vector<std::string> joint_names;
        {
          py::gil_scoped_release release;
          auto scene_lock = lock_bundle_scene(self);
          const auto maybe_group_info = self.scene->getJointGroupInfo(group_name);
          if (!maybe_group_info) {
            throw std::runtime_error(
                "Failed to get joint names for group '" + group_name + "': " +
                maybe_group_info.error());
          }
          joint_names = maybe_group_info->joint_names;
        }
        return canonical_joint_names(self, joint_names);
      }, py::arg("group_name"),
      "Get ordered joint names for a specific SRDF group.")
      .def("get_group_joint_positions", [](NativeKernelBundle& self,
                                           const std::string& group_name) {
        py::gil_scoped_release release;
        auto scene_lock = lock_bundle_scene(self);
        return group_joint_positions_with_mimics(self, group_name);
      }, py::arg("group_name"),
      "Get current canonical joint positions for a specific SRDF group, including mimics.")
      .def("get_group_commandable_joint_positions",
           [](NativeKernelBundle& self, const std::string& group_name) {
             py::gil_scoped_release release;
             auto scene_lock = lock_bundle_scene(self);
             return compact_group_joint_positions(self, group_name);
           }, py::arg("group_name"),
           "Get compact commandable q-slot positions for a specific SRDF group.")
      .def("set_group_joint_positions", [](NativeKernelBundle& self,
                                           const std::string& group_name,
                                           const std::vector<double>& positions) {
        auto kernel = require_open_kernel(self);
        {
          py::gil_scoped_release release;
          auto scene_lock = lock_bundle_scene(self);
          Eigen::VectorXd q =
              compact_group_positions_from_public_input(self, group_name, positions);
          if (self.scene &&
              q.size() != self.scene->getCurrentJointPositions().size()) {
            q = self.scene->toFullJointPositions(group_name, q);
          }
          self.scene->setJointPositions(q);
        }
        {
          py::gil_scoped_release release;
          auto refreshed = kernel->refresh_observed_state_snapshot();
          if (!refreshed.ok()) {
            throw std::runtime_error(refreshed.error().message);
          }
        }
      }, py::arg("group_name"), py::arg("positions"),
      "Set joint positions for a specific SRDF group in the planning scene.")
      .def("collision_check_positions", [](NativeKernelBundle& self,
                                            const std::vector<double>& positions) {
        RobotStateDTO state;
        state.joint_names = [&]() {
          py::gil_scoped_release release;
          return qualified_group_joint_names(self, self.group_name);
        }();
        state.positions_rad = positions;
        CollisionQuery query;
        query.query_id = "debug_authoritative_collision_check_positions";
        query.surface = CollisionSurface::kDiagnosticCompare;
        query.robot_state = std::move(state);
        query.sampling_mode = CollisionSamplingMode::kWaypointOnly;
        const auto result = [&]() {
          py::gil_scoped_release release;
          auto snapshot = self.scene_authority->snapshot();
          auto policy = self.scene_authority->collision_policy_snapshot();
          if (!snapshot || !policy ||
              self.collision_validation_engines.empty() ||
              !self.collision_validation_engines.front()) {
            throw std::runtime_error(
                "Authoritative collision diagnostic resources are unavailable");
          }
          query.collision_world_revision = snapshot->revision;
          CollisionCancellationToken cancel;
          return self.collision_validation_engines.front()->check(
              *snapshot, query, *policy, cancel);
        }();
        py::dict d;
        d["in_collision"] = result.in_collision;
        if (result.contact_body_a.has_value()) {
          d["contact_body_a"] = *result.contact_body_a;
        } else {
          d["contact_body_a"] = py::none();
        }
        if (result.contact_body_b.has_value()) {
          d["contact_body_b"] = *result.contact_body_b;
        } else {
          d["contact_body_b"] = py::none();
        }
        return d;
      }, py::arg("positions"),
      "Check collisions for active-group joint positions.")
      .def("collision_compare_positions", [](
              NativeKernelBundle& self,
              const std::vector<double>& positions,
              std::optional<std::int64_t> waypoint_index) {
        RobotStateDTO state;
        state.joint_names = [&]() {
          py::gil_scoped_release release;
          return qualified_group_joint_names(self, self.group_name);
        }();
        state.positions_rad = positions;

        CollisionQuery query;
        query.query_id = "debug_collision_compare_positions";
        query.surface = CollisionSurface::kDiagnosticCompare;
        query.robot_state = state;
        query.waypoint_index = waypoint_index;
        query.sampling_mode = CollisionSamplingMode::kWaypointOnly;

        auto snapshot = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->snapshot();
        }();
        py::dict result;
        if (!snapshot) {
          result["ok"] = false;
          result["error"] = "collision_world_snapshot_unavailable";
          return result;
        }
        query.collision_world_revision = snapshot->revision;

        auto policy = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->collision_policy_snapshot()
              .value_or(CollisionPolicySnapshot{});
        }();

        CollisionResult diagnostic_result = [&]() {
          py::gil_scoped_release release;
          return self.collision_diagnostic_comparator->compare(query);
        }();

        if (self.collision_validation_engines.empty() ||
            !self.collision_validation_engines.front()) {
          result["ok"] = false;
          result["error"] = "manyforge_collision_engine_unavailable";
          result["diagnostic"] =
              collision_result_to_python(diagnostic_result);
          return result;
        }

        CollisionCancellationToken cancel;
        CollisionResult manyforge_result = [&]() {
          py::gil_scoped_release release;
          return self.collision_validation_engines.front()->check(
              *snapshot, query, policy, cancel);
        }();

        const bool in_collision_match =
            diagnostic_result.in_collision == manyforge_result.in_collision;
        const bool decision_match =
            diagnostic_result.decision == manyforge_result.decision;
        const bool pair_count_match =
            diagnostic_result.normalized_pairs.size() ==
            manyforge_result.normalized_pairs.size();
        const bool normalized_pairs_match =
            diagnostic_result.normalized_pairs ==
            manyforge_result.normalized_pairs;

        result["ok"] = true;
        result["in_collision_match"] = in_collision_match;
        result["decision_match"] = decision_match;
        result["pair_count_match"] = pair_count_match;
        result["normalized_pairs_match"] = normalized_pairs_match;
        result["match"] = in_collision_match && decision_match &&
                          normalized_pairs_match;
        result["snapshot_revision_id"] = snapshot->revision.revision_id;
        result["snapshot_content_hash"] = snapshot->revision.content_hash;
        result["diagnostic"] =
            collision_result_to_python(diagnostic_result);
        result["manyforge_collision"] =
            collision_result_to_python(manyforge_result);
        return result;
      }, py::arg("positions"), py::arg("waypoint_index") = py::none(),
      "Compare diagnostic and authoritative collision engines for positions.")
      .def("get_cached_trajectory", [](NativeKernelBundle& self,
                                       const std::string& move_id) -> py::object {
        auto cached = self.cache->peek_unvalidated(move_id);
        if (!cached.has_value() || cached->waypoints.empty()) {
          return py::none();
        }
        py::dict result;
        result["joint_names"] = cached->joint_names;
        py::list waypoints;
        for (const auto& wp : cached->waypoints) {
          py::dict wp_dict;
          wp_dict["time_from_start_s"] = wp.time_from_start_s;
          wp_dict["positions_rad"] = wp.positions_rad;
          waypoints.append(wp_dict);
        }
        result["waypoints"] = waypoints;
        return result;
      }, py::arg("move_id"), "Get cached trajectory waypoints for a move_id.")
      .def("get_link_pose", [](NativeKernelBundle& self,
                                const std::string& target_frame,
                                const std::string& base_frame) -> py::object {
        try {
          const auto poses = [&]() {
            py::gil_scoped_release release;
            const auto state = observer_robot_state(self);
            return self.kinematics->forward_kinematics_batch(
                state, std::vector<std::string>{target_frame}, base_frame);
          }();
          if (!poses.ok()) {
            return py::none();
          }
          const auto it = poses.value().find(target_frame);
          if (it == poses.value().end()) {
            return py::none();
          }
          return py::object(pose_to_python(it->second));
        } catch (...) {
          return py::none();
        }
      })
      .def("get_link_poses", [](NativeKernelBundle& self,
                                 const std::vector<std::string>& target_frames,
                                 const std::string& base_frame) -> py::dict {
        const auto poses = [&]() {
          py::gil_scoped_release release;
          const auto state = observer_robot_state(self);
          return self.kinematics->forward_kinematics_batch(
              state, target_frames, base_frame);
        }();
        if (!poses.ok()) {
          throw std::runtime_error(poses.error().message);
        }
        py::dict result;
        for (const auto& [target_frame, pose] : poses.value()) {
          result[py::str(target_frame)] = pose_to_python(pose);
        }
        return result;
      })
      .def("get_object_pose", [](NativeKernelBundle& self,
                                  const std::string& object_id,
                                  const std::string& base_frame) -> py::object {
        try {
          auto pose = [&]() {
            py::gil_scoped_release release;
            return self.scene_authority->object_pose(object_id, base_frame);
          }();
          if (!pose.ok()) {
            return py::object(py::none());
          }
          return py::object(pose_to_python(pose.value()));
        } catch (...) {
          return py::none();
        }
      })
      .def("get_object_state", [](NativeKernelBundle& self,
                                   const std::string& object_id) -> py::dict {
        const auto state = [&]() {
          py::gil_scoped_release release;
          return self.scene_authority->object_state(object_id);
        }();
        return object_state_to_python(state);
      })
      .def("get_scene_trace", [](NativeKernelBundle& self, int limit) -> py::list {
        const auto events = [&]() {
          py::gil_scoped_release release;
          return self.scene_projection->scene_trace(
              limit < 0 ? 0 : static_cast<size_t>(limit));
        }();
        py::list result;
        for (const auto& event : events) {
          result.append(scene_trace_event_to_python(event));
        }
        return result;
      }, py::arg("limit") = -1)
      .def("clear_scene_trace", [](NativeKernelBundle& self) {
        self.scene_projection->clear_scene_trace();
      })
      .def("scene_update_status_counters", [](NativeKernelBundle& self) -> py::dict {
        const auto counters = [&]() {
          py::gil_scoped_release release;
          return self.scene_projection->scene_update_status_counters();
        }();
        py::dict result;
        for (const auto& [key, value] : counters) {
          result[py::str(key)] = value;
        }
        return result;
      })
      .def("drain_kernel_events", [](NativeKernelBundle& self) -> py::list {
        py::list result;
        for (const auto& event : self.event_collector.drain()) {
          result.append(kernel_event_to_python(event));
        }
        return result;
      })
      .def("drain_kernel_events_with_stats",
           [](NativeKernelBundle& self) -> py::dict {
        auto stats = self.event_collector.drain_with_stats();
        py::list events;
        for (const auto& event : stats.events) {
          events.append(kernel_event_to_python(event));
        }
        py::dict out;
        out["events"] = std::move(events);
        out["dropped_total"] = stats.dropped_total;
        return out;
      })
      .def("wait_for_kernel_events",
           [](NativeKernelBundle& self, double timeout_s) {
             if (!std::isfinite(timeout_s) || timeout_s < 0.0 ||
                 timeout_s > 1.0) {
               throw py::value_error(
                   "timeout_s must be finite and in [0, 1]");
             }
             const auto timeout = std::chrono::duration_cast<
                 std::chrono::milliseconds>(
                 std::chrono::duration<double>(timeout_s));
             py::gil_scoped_release release;
             return self.event_collector.wait_for_events(timeout);
           },
           py::arg("timeout_s"))
      // Compute all link poses at a hypothetical robot configuration, for the
      // Composer Move-To / validated Robot Control candidate "ghost robot"
      // preview (no-motion authoring).
      // Stateless w.r.t. the live scene: builds a RobotStateDTO from the
      // caller's joint vector (or IK result), then calls FK per link.
      // For kind=="cartesian": runs IK first using the supplied tcp_pose
      // and seed_joint_positions_rad; if IK fails, returns
      // {reachable: False, reason, tcp_pose} so the frontend can render
      // the TCP link only (red gripper-shaped ghost) without the rest
      // of the robot.
      // For kind=="joint": uses joint_positions_rad directly.
      .def("compute_target_link_poses",
           [](NativeKernelBundle& self, py::dict req) -> py::dict {
             py::dict out;
             const std::string kind = req.contains("kind")
                 ? req["kind"].cast<std::string>() : std::string("joint");
             const std::string base_frame = req.contains("base_frame")
                 ? req["base_frame"].cast<std::string>() : std::string("base");
             std::vector<std::string> link_names;
             if (req.contains("link_names") && !req["link_names"].is_none()) {
               for (const auto& n : req["link_names"].cast<py::list>()) {
                 link_names.push_back(n.cast<std::string>());
               }
             }
             RobotStateDTO state;
             if (req.contains("joint_names") && !req["joint_names"].is_none()) {
               for (const auto& n : req["joint_names"].cast<py::list>()) {
                 state.joint_names.push_back(n.cast<std::string>());
               }
             }
             if (req.contains("joint_positions_rad")
                 && !req["joint_positions_rad"].is_none()) {
               for (const auto& v : req["joint_positions_rad"].cast<py::list>()) {
                 state.positions_rad.push_back(v.cast<double>());
               }
             }

             if (kind == "cartesian") {
               IkRequestDTO ik_req;
               const auto pos = req["tcp_position_m"].cast<py::list>();
               const auto quat = req["tcp_orientation_xyzw"].cast<py::list>();
               ik_req.goal_pose.position_m.x_m = pos[0].cast<double>();
               ik_req.goal_pose.position_m.y_m = pos[1].cast<double>();
               ik_req.goal_pose.position_m.z_m = pos[2].cast<double>();
               ik_req.goal_pose.orientation.x = quat[0].cast<double>();
               ik_req.goal_pose.orientation.y = quat[1].cast<double>();
               ik_req.goal_pose.orientation.z = quat[2].cast<double>();
               ik_req.goal_pose.orientation.w = quat[3].cast<double>();
               ik_req.base_frame = base_frame;
               ik_req.tcp_frame = req["tcp_frame"].cast<std::string>();
               if (!state.joint_names.empty()
                   && state.positions_rad.size() == state.joint_names.size()) {
                 ik_req.seed_state = state;
               } else {
                 ik_req.seed_state = observer_robot_state(self);
               }
               auto ik_result = [&]() {
                 py::gil_scoped_release release;
                 return self.planner->solve_ik(ik_req);
               }();
               if (!ik_result.ok) {
                 out["reachable"] = false;
                 out["reason"] = ik_result.error
                     ? ik_result.error->code : std::string("ik_failed");
                 out["tcp_pose"] = py::dict(
                     py::arg("position_m") = py::make_tuple(
                         ik_req.goal_pose.position_m.x_m,
                         ik_req.goal_pose.position_m.y_m,
                         ik_req.goal_pose.position_m.z_m),
                     py::arg("orientation_xyzw") = py::make_tuple(
                         ik_req.goal_pose.orientation.x,
                         ik_req.goal_pose.orientation.y,
                         ik_req.goal_pose.orientation.z,
                         ik_req.goal_pose.orientation.w));
                 return out;
               }
               state.joint_names = ik_result.joint_names;
               state.positions_rad = ik_result.positions_rad;
             }

             // FK for each requested link at the resolved joint state.
             py::dict link_poses;
             for (const auto& link_name : link_names) {
               FkRequestDTO fk_req;
               fk_req.robot_state = state;
               fk_req.target_frame = link_name;
               fk_req.base_frame = base_frame;
               try {
                 auto pose = [&]() {
                   py::gil_scoped_release release;
                   return self.kinematics->forward_kinematics(fk_req);
                 }();
                 if (!pose.ok()) {
                   continue;
                 }
                 py::dict d;
                 d["position_m"] = py::make_tuple(
                     pose.value().position_m.x_m,
                     pose.value().position_m.y_m,
                     pose.value().position_m.z_m);
                 d["orientation_xyzw"] = py::make_tuple(
                     pose.value().orientation.x,
                     pose.value().orientation.y,
                     pose.value().orientation.z,
                     pose.value().orientation.w);
                 link_poses[py::str(link_name)] = d;
               } catch (...) {
                 // Skip links FK can't resolve — frontend just won't
                 // render them at the target pose.
               }
             }
             out["reachable"] = true;
             out["joint_names"] = py::cast(state.joint_names);
             out["joint_positions_rad"] = py::cast(state.positions_rad);
             out["link_poses"] = link_poses;
             return out;
           });

  // Factory function
  m.def("create_native_kernel",
        [](const std::string& model_name, const std::string& group_name,
           py::object config, const std::string& logical_robot_id) {
          return create_native_kernel(
              model_name, group_name, kernel_config_from_python(config),
              logical_robot_id);
        },
        py::arg("model_name") = "ur5_gripper",
        py::arg("group_name") = "arm",
        py::arg("config") = py::none(),
        py::arg("logical_robot_id") = "",
        "Create a native C++ kernel with real RoboPlan backends.");

  m.def("create_native_kernel_from_paths",
        [](const std::string& model_name, const std::string& group_name,
           const std::string& urdf_path, const std::string& srdf_path,
           const std::vector<std::string>& package_paths, py::object config,
           const std::string& logical_robot_id) {
          return create_native_kernel(
              model_name, group_name, urdf_path, srdf_path, package_paths,
              kernel_config_from_python(config), logical_robot_id);
        },
        py::arg("model_name"),
        py::arg("group_name"),
        py::arg("urdf_path"),
        py::arg("srdf_path"),
        py::arg("package_paths") = std::vector<std::string>{},
        py::arg("config") = py::none(),
        py::arg("logical_robot_id") = "",
        "Create a native C++ kernel from explicit URDF/SRDF/package bundle paths.");

#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
  m.def("create_ros2_control_kernel_from_paths",
        [](const std::string& model_name, const std::string& group_name,
           const std::string& urdf_path, const std::string& srdf_path,
           const std::vector<std::string>& package_paths, py::object ros_config,
           py::object kernel_config, const std::string& logical_robot_id) {
          return create_ros2_control_kernel(
              model_name, group_name, urdf_path, srdf_path, package_paths,
              ros2_control_config_from_python(ros_config),
              kernel_config_from_python(kernel_config), logical_robot_id);
        },
        py::arg("model_name"),
        py::arg("group_name"),
        py::arg("urdf_path"),
        py::arg("srdf_path"),
        py::arg("package_paths") = std::vector<std::string>{},
        py::arg("ros_config") = py::none(),
        py::arg("kernel_config") = py::none(),
        py::arg("logical_robot_id") = "",
        "Create a native C++ kernel with RoboPlan planning and ros2_control execution.");
#endif
}
