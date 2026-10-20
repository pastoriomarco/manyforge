#include <manyforge_core/io_rule_outcome_codec.hpp>
#include "execution_host_responses.hpp"
#include "execution_host_posix.hpp"
#include "cell_admission_command_codec.hpp"
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
#include <manyforge_roboplan/controller_launch_process.hpp>
#endif
#include <iostream>
#include <manyforge_deployment/artifact_codec.hpp>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace manyforge::roboplan_backend::host_detail {

[[nodiscard]] CanonicalValue ErrorValue(const core::ErrorDTO &error) {
  CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto &[key, value] : error.details) {
    details.emplace_back(key, CanonicalValue(value));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue(error.code)},
      {"details", CanonicalValue(std::move(details))},
      {"message", CanonicalValue(error.message)},
  });
}

[[nodiscard]] CanonicalValue
ErrorResponse(std::string code, std::string message,
              std::map<std::string, std::string> details) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error", ErrorValue(core::ErrorDTO{std::move(code), std::move(message),
                                          std::move(details)})},
      {"ok", CanonicalValue(false)},
  });
}

[[nodiscard]] CanonicalValue ExecutionOwnerNotReadyIdentityResponse(
    const core::ResourceAddressedExecutionKernel &owner,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto admission = owner.GetAuthoringAdmission();
  return CanonicalValue(CanonicalValue::Object{
      {"admission_generation", CanonicalValue(static_cast<double>(
                                   admission.admission_generation.value()))},
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error",
       ErrorValue(core::ErrorDTO{
           "execution_owner_not_ready",
           "the managed execution owner has no active physical product",
           {{"owner_epoch", owner_epoch.value()}}})},
      {"ok", CanonicalValue(false)},
  });
}

[[nodiscard]] CanonicalValue PoseValue(const core::Pose3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation", CanonicalValue(CanonicalValue::Object{
                          {"w", CanonicalValue(value.orientation.w)},
                          {"x", CanonicalValue(value.orientation.x)},
                          {"y", CanonicalValue(value.orientation.y)},
                          {"z", CanonicalValue(value.orientation.z)},
                      })},
      {"position_m", CanonicalValue(CanonicalValue::Object{
                         {"x_m", CanonicalValue(value.position_m.x_m)},
                         {"y_m", CanonicalValue(value.position_m.y_m)},
                         {"z_m", CanonicalValue(value.position_m.z_m)},
                     })},
  });
}

[[nodiscard]] CanonicalValue
SceneRevisionValue(const core::SceneRevision &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"content_hash", CanonicalValue(value.content_hash.value())},
      {"revision_id", CanonicalValue(static_cast<double>(value.revision_id))},
      {"scene_id", CanonicalValue(value.scene_id.value())},
  });
}

[[nodiscard]] CanonicalValue MotionPreviewReceiptValue(
    const core::MotionPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const core::RuntimeParticipantBindingDTO &direct_control_binding,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto &binding = direct_control_binding;
  const auto &final_state = preview.final_controlled_state;
  Require(
      final_state.resources.size() == binding.resources.size() &&
          final_state.joint_names.size() == binding.resources.size() &&
          final_state.positions.size() == binding.resources.size() &&
          preview.start_state.joint_names.size() == binding.resources.size() &&
          preview.start_state.positions_rad.size() == binding.resources.size(),
      "motion preview result crosses its direct-control resource shape");

  CanonicalValue::Array source_joint_names;
  CanonicalValue::Array start_positions;
  CanonicalValue::Array final_positions;
  source_joint_names.reserve(binding.resources.size());
  start_positions.reserve(binding.resources.size());
  final_positions.reserve(binding.resources.size());
  for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
    const auto &resource = binding.resources[index];
    Require(resource.source_joint_name.has_value() &&
                final_state.resources[index] == resource.resource_id &&
                final_state.joint_names[index] ==
                    resource.compiled_resource_name &&
                preview.start_state.joint_names[index] ==
                    resource.compiled_resource_name,
            "motion preview result crosses its source-local binding");
    const auto *angular =
        std::get_if<core::AngularPositionDTO>(&final_state.positions[index]);
    Require(angular != nullptr,
            "joint motion preview returned a non-angular terminal state");
    source_joint_names.emplace_back(*resource.source_joint_name);
    start_positions.emplace_back(preview.start_state.positions_rad[index]);
    final_positions.emplace_back(angular->value_rad);
  }

  CanonicalValue::Object receipt{
      {"collision_world_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(preview.collision_world_revision.content_hash)},
           {"revision_id", CanonicalValue(static_cast<double>(
                               preview.collision_world_revision.revision_id))},
       })},
      {"commit_eligible", CanonicalValue(true)},
      {"component_role_id", CanonicalValue(binding.component_role_id.value())},
      {"final_positions_rad", CanonicalValue(std::move(final_positions))},
      {"final_tcp_pose", PoseValue(preview.final_tcp_pose)},
      {"graph_revision", CanonicalValue(binding.graph_revision.value())},
      {"intent_digest", CanonicalValue(intent_digest.value())},
      {"program_binding_id",
       CanonicalValue(binding.program_binding_id.value())},
      {"program_id", CanonicalValue(binding.program_id.value())},
      {"program_revision", CanonicalValue(binding.program_revision.value())},
      {"program_run_id", CanonicalValue(intent.program_run_id.value())},
      {"request_id", CanonicalValue(intent.request_context.request_id.value())},
      {"request_owner_epoch", CanonicalValue(owner_epoch.value())},
      {"required_capability_id",
       CanonicalValue(
           intent.participants.front().required_capability_id.value())},
      {"retained_bundle_id",
       CanonicalValue(binding.retained_bundle_id.value())},
      {"scene_revision",
       SceneRevisionValue(
           preview.projection_binding.projection_revision.scene_revision)},
      {"schema_version",
       CanonicalValue("manyforge.direct_control_motion_preview.v1")},
      {"source_joint_names", CanonicalValue(std::move(source_joint_names))},
      {"start_positions_rad", CanonicalValue(std::move(start_positions))},
      {"target_role_id", CanonicalValue(binding.target_role_id.value())},
  };
  const core::ContentDigest receipt_digest(content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(receipt))));
  receipt.emplace_back("receipt_digest",
                       CanonicalValue(receipt_digest.value()));
  return CanonicalValue(std::move(receipt));
}

[[nodiscard]] CanonicalValue CapabilityCommandPreviewReceiptValue(
    const core::CapabilityCommandPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const std::optional<RetainedGripperPositionLimits> &limits,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto &binding = preview.binding;
  Require(binding.resources.size() == 1U,
          "capability-command preview has no exact scalar resource");
  const auto &resource = binding.resources.front();
  const bool model_backed = resource.source_joint_name.has_value();
  Require(
      intent.expected_scene_revision.has_value() &&
          binding.command_schema.has_value() &&
          preview.command_request.gripper_id ==
              binding.command_target_id.value() &&
          preview.command_request.joint_name ==
              resource.compiled_resource_name &&
          model_backed == limits.has_value() &&
          (!limits.has_value() ||
           (limits->coordinate_kind == resource.coordinate_kind &&
            std::isfinite(limits->lower_position) &&
            std::isfinite(limits->upper_position) &&
            limits->lower_position <= limits->upper_position &&
            limits->lower_position <= preview.command_request.target_position &&
            preview.command_request.target_position <=
                limits->upper_position)) &&
          std::isfinite(preview.command_request.target_position),
      "capability-command preview crosses its direct-control binding");
  const auto coordinate_kind =
      resource.coordinate_kind == core::CoordinateKind::kAngular ? "angular_rad"
                                                                 : "linear_m";
  const auto &source_coordinate_name = model_backed
                                           ? *resource.source_joint_name
                                           : resource.compiled_resource_name;
  CanonicalValue::Object receipt{
      {"command_schema_id",
       CanonicalValue(binding.command_schema->command_schema_id.value())},
      {"command_schema_revision",
       CanonicalValue(binding.command_schema->command_schema_revision.value())},
      {"commit_eligible", CanonicalValue(true)},
      {"component_role_id", CanonicalValue(binding.component_role_id.value())},
      {"coordinate_kind", CanonicalValue(coordinate_kind)},
      {"graph_revision", CanonicalValue(binding.graph_revision.value())},
      {"intent_digest", CanonicalValue(intent_digest.value())},
      {"max_position", limits.has_value()
                           ? CanonicalValue(limits->upper_position)
                           : CanonicalValue(nullptr)},
      {"min_position", limits.has_value()
                           ? CanonicalValue(limits->lower_position)
                           : CanonicalValue(nullptr)},
      {"program_binding_id",
       CanonicalValue(binding.program_binding_id.value())},
      {"program_id", CanonicalValue(binding.program_id.value())},
      {"program_revision", CanonicalValue(binding.program_revision.value())},
      {"program_run_id", CanonicalValue(intent.program_run_id.value())},
      {"request_id", CanonicalValue(intent.request_context.request_id.value())},
      {"request_owner_epoch", CanonicalValue(owner_epoch.value())},
      {"required_capability_id",
       CanonicalValue(
           intent.participants.front().required_capability_id.value())},
      {"retained_bundle_id",
       CanonicalValue(binding.retained_bundle_id.value())},
      {"scene_revision", SceneRevisionValue(*intent.expected_scene_revision)},
      {"schema_version",
       CanonicalValue(
           "manyforge.direct_control_capability_command_preview.v1")},
      {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
      {"source_kind",
       CanonicalValue(model_backed ? "model_role" : "component_local")},
      {"target_position",
       CanonicalValue(preview.command_request.target_position)},
      {"target_role_id", CanonicalValue(binding.target_role_id.value())},
  };
  const core::ContentDigest receipt_digest(content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(receipt))));
  receipt.emplace_back("receipt_digest",
                       CanonicalValue(receipt_digest.value()));
  return CanonicalValue(std::move(receipt));
}

[[nodiscard]] CanonicalValue
MotionSpecValue(const core::MotionSpecDTO &motion) {
  return std::visit(
      [](const auto &value) -> CanonicalValue {
        using Motion = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Motion, core::JointGoalDTO>) {
          CanonicalValue::Array joint_names;
          CanonicalValue::Array positions;
          joint_names.reserve(value.joint_names.size());
          positions.reserve(value.positions_rad.size());
          for (const auto &name : value.joint_names) {
            joint_names.emplace_back(name);
          }
          for (const auto position : value.positions_rad) {
            positions.emplace_back(position);
          }
          return CanonicalValue(CanonicalValue::Object{
              {"joint_names", CanonicalValue(std::move(joint_names))},
              {"kind", CanonicalValue("joint_goal")},
              {"positions_rad", CanonicalValue(std::move(positions))},
          });
        } else if constexpr (std::is_same_v<Motion, core::PoseGoalDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"goal", PoseValue(value.goal)},
              {"kind", CanonicalValue("pose_goal")},
          });
        } else if constexpr (std::is_same_v<Motion, core::NamedTargetDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("named_target")},
              {"name", CanonicalValue(value.name)},
          });
        } else {
          CanonicalValue::Array waypoints;
          waypoints.reserve(value.waypoints.size());
          for (const auto &waypoint : value.waypoints) {
            waypoints.push_back(PoseValue(waypoint));
          }
          return CanonicalValue(CanonicalValue::Object{
              {"fallback_policy",
               CanonicalValue(
                   value.fallback_policy ==
                           core::CartesianFallbackPolicyDTO::kFailClosed
                       ? "fail_closed"
                       : "allow_joint_space_fallback")},
              {"kind", CanonicalValue("cartesian_path")},
              {"max_angular_acceleration_rad_s2",
               CanonicalValue(value.max_angular_acceleration_rad_s2)},
              {"max_blend_deviation_rad",
               CanonicalValue(value.max_blend_deviation_rad)},
              {"max_angular_speed_rad_s",
               CanonicalValue(value.max_angular_speed_rad_s)},
              {"max_linear_acceleration_mps2",
               CanonicalValue(value.max_linear_acceleration_mps2)},
              {"max_rotation_step_rad",
               CanonicalValue(value.max_rotation_step_rad)},
              {"max_translation_step_m",
               CanonicalValue(value.max_translation_step_m)},
              {"speed_mode", CanonicalValue("bounded")},
              {"waypoints", CanonicalValue(std::move(waypoints))},
          });
        }
      },
      motion);
}

[[nodiscard]] CanonicalValue
ProjectionRevisionValue(const core::ProjectionRevisionDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(value.scene_revision.content_hash.value())},
           {"revision_id", CanonicalValue(static_cast<double>(
                               value.scene_revision.revision_id))},
           {"scene_id", CanonicalValue(value.scene_revision.scene_id.value())},
       })},
  });
}

[[nodiscard]] CanonicalValue
PickCandidateValue(const core::PickPoseCandidateDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"approach_pose", PoseValue(value.approach_pose)},
      {"candidate_index",
       CanonicalValue(static_cast<double>(value.candidate_index))},
      {"min_tcp_z_applied", CanonicalValue(value.min_tcp_z_applied)},
      {"pick_cartesian_path",
       MotionSpecValue(core::MotionSpecDTO{value.descend_path})},
      {"pick_pose", PoseValue(value.pick_pose)},
      {"retreat_cartesian_path",
       MotionSpecValue(core::MotionSpecDTO{value.retreat_path})},
      {"retreat_pose", PoseValue(value.retreat_pose)},
  });
}

[[nodiscard]] CanonicalValue
PickPlacePlanningResultValue(const std::string_view sequence_id,
                             const core::PickPlaceExecutionKindDTO kind,
                             const core::PickPlacePlanningResultDTO &value) {
  CanonicalValue::Array reports;
  reports.reserve(value.reports.size());
  for (const auto &report : value.reports) {
    CanonicalValue::Array stages;
    stages.reserve(report.stages.size());
    for (const auto &stage : report.stages) {
      stages.emplace_back(CanonicalValue::Object{
          {"error", stage.error.has_value() ? ErrorValue(*stage.error)
                                            : CanonicalValue(nullptr)},
          {"stage", CanonicalValue(std::string(core::to_string(stage.stage)))},
          {"succeeded", CanonicalValue(stage.succeeded)},
          {"trajectory_duration_s",
           CanonicalValue(stage.trajectory_duration_s)},
          {"trajectory_waypoint_count", CanonicalValue(static_cast<double>(
                                            stage.trajectory_waypoint_count))},
      });
    }
    reports.emplace_back(CanonicalValue::Object{
        {"candidate_index",
         CanonicalValue(static_cast<double>(report.candidate_index))},
        {"stages", CanonicalValue(std::move(stages))},
    });
  }
  CanonicalValue plan(nullptr);
  if (value.plan.has_value()) {
    const auto &planned = *value.plan;
    plan = CanonicalValue(CanonicalValue::Object{
        {"approach_motion", MotionSpecValue(planned.approach_motion)},
        {"descend_motion", MotionSpecValue(planned.descend_motion)},
        {"predecessor_projection_revision",
         ProjectionRevisionValue(planned.predecessor_projection_revision)},
        {"preview_successor_projection_revision",
         ProjectionRevisionValue(
             planned.preview_successor_projection_revision)},
        {"retreat_motion", MotionSpecValue(planned.retreat_motion)},
    });
  }
  CanonicalValue last_attempted_candidate(nullptr);
  if (value.last_attempted_pick_candidate.has_value()) {
    last_attempted_candidate =
        PickCandidateValue(*value.last_attempted_pick_candidate);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"attempted_candidate_count",
       CanonicalValue(static_cast<double>(value.attempted_candidate_count))},
      {"kind", CanonicalValue(kind == core::PickPlaceExecutionKindDTO::kPick
                                  ? "pick"
                                  : "place")},
      {"last_attempted_candidate", std::move(last_attempted_candidate)},
      {"plan", std::move(plan)},
      {"reports", CanonicalValue(std::move(reports))},
      {"schema_version",
       CanonicalValue("manyforge.pick_place_planning_result.v1")},
      {"selected_object_id", CanonicalValue(value.selected_object_id.value())},
      {"sequence_id", CanonicalValue(std::string(sequence_id))},
      {"status", CanonicalValue(std::string(core::to_string(value.status)))},
  });
}

[[nodiscard]] std::string_view StateName(core::OperationStateKind state) {
  switch (state) {
  case core::OperationStateKind::kQueued:
    return "queued";
  case core::OperationStateKind::kResolving:
    return "resolving";
  case core::OperationStateKind::kPlanning:
    return "planning";
  case core::OperationStateKind::kValidating:
    return "validating";
  case core::OperationStateKind::kPrepared:
    return "prepared";
  case core::OperationStateKind::kDispatching:
    return "dispatching";
  case core::OperationStateKind::kExecuting:
    return "executing";
  case core::OperationStateKind::kStopping:
    return "stopping";
  case core::OperationStateKind::kCompleted:
    return "completed";
  case core::OperationStateKind::kStopped:
    return "stopped";
  case core::OperationStateKind::kFailed:
    return "failed";
  case core::OperationStateKind::kInterrupted:
    return "interrupted";
  case core::OperationStateKind::kInterventionRequired:
    return "intervention_required";
  case core::OperationStateKind::kCancelledPreDispatch:
    return "cancelled_pre_dispatch";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
AcceptedValue(const core::OperationAcceptedDTO &value) {
  CanonicalValue::Array assignments;
  assignments.reserve(value.participant_assignments.size());
  for (const auto &assignment : value.participant_assignments) {
    assignments.emplace_back(CanonicalValue::Object{
        {"input_index",
         CanonicalValue(static_cast<double>(assignment.input_index))},
        {"participant_id", CanonicalValue(assignment.participant_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision",
       CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participant_assignments", CanonicalValue(std::move(assignments))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] std::string_view ControlName(core::OperationControlKind control) {
  switch (control) {
  case core::OperationControlKind::kReset:
    return "reset";
  case core::OperationControlKind::kAcknowledgeCompleted:
    return "acknowledge_completed";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
ControlAcceptedValue(const core::OperationControlAcceptedDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_against_revision",
       CanonicalValue(
           static_cast<double>(value.accepted_against_revision.value()))},
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"control", CanonicalValue(std::string(ControlName(value.control)))},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision_after_acceptance",
       CanonicalValue(static_cast<double>(
           value.operation_revision_after_acceptance.value()))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] CanonicalValue
StopAcceptedValue(const core::StopAcceptedDTO &value) {
  const auto diagnostic_target = [](const auto &target) {
    return std::visit(
        [](const auto &selected) -> CanonicalValue {
          using Target = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<Target, std::monostate>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("cell")},
                {"value", CanonicalValue(nullptr)},
            });
          } else if constexpr (std::is_same_v<Target, core::OperationId>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("operation_id")},
                {"value", CanonicalValue(selected.value())},
            });
          } else if constexpr (std::is_same_v<Target,
                                              core::ControllableComponentId>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("controllable_component_id")},
                {"value", CanonicalValue(selected.value())},
            });
          } else {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("resource_id")},
                {"value", CanonicalValue(selected.value())},
            });
          }
        },
        target);
  };
  const auto diagnostic_value = [](const auto &candidate) {
    return std::visit(
        [](const auto &selected) -> CanonicalValue {
          using Value = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<Value, std::uint64_t>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("unsigned_integer")},
                {"value", CanonicalValue(static_cast<double>(selected))},
            });
          } else if constexpr (std::is_same_v<Value, core::RequestOwnerEpoch>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("request_owner_epoch")},
                {"value", CanonicalValue(selected.value())},
            });
          } else if constexpr (std::is_same_v<Value, core::GraphRevision>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("graph_revision")},
                {"value", CanonicalValue(selected.value())},
            });
          } else if constexpr (std::is_same_v<Value, core::OperationRevision>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("operation_revision")},
                {"value",
                 CanonicalValue(static_cast<double>(selected.value()))},
            });
          } else if constexpr (std::is_same_v<
                                   Value, core::ComponentLifecycleGeneration>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("component_lifecycle_generation")},
                {"value",
                 CanonicalValue(static_cast<double>(selected.value()))},
            });
          } else {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("stop_generation")},
                {"value",
                 CanonicalValue(static_cast<double>(selected.value()))},
            });
          }
        },
        candidate);
  };

  CanonicalValue::Array stale_diagnostics;
  stale_diagnostics.reserve(value.stale_diagnostics.size());
  for (const auto &diagnostic : value.stale_diagnostics) {
    stale_diagnostics.emplace_back(CanonicalValue::Object{
        {"current", diagnostic_value(diagnostic.current)},
        {"expected", diagnostic_value(diagnostic.expected)},
        {"field",
         CanonicalValue(std::string(core::to_string(diagnostic.field)))},
        {"target", diagnostic_target(diagnostic.target)},
    });
  }

  CanonicalValue::Array affected_operations;
  affected_operations.reserve(value.affected_operation_revisions.size());
  for (const auto &operation : value.affected_operation_revisions) {
    affected_operations.emplace_back(CanonicalValue::Object{
        {"operation_id", CanonicalValue(operation.operation_id.value())},
        {"operation_revision_after_acceptance",
         CanonicalValue(static_cast<double>(
             operation.operation_revision_after_acceptance.value()))},
    });
  }

  CanonicalValue::Array predecessors;
  predecessors.reserve(value.joined_predecessor_stops.size());
  for (const auto &predecessor : value.joined_predecessor_stops) {
    predecessors.emplace_back(CanonicalValue::Object{
        {"captured_scope_digest",
         CanonicalValue(predecessor.captured_scope_digest.value())},
        {"predecessor_stop_generation",
         CanonicalValue(static_cast<double>(
             predecessor.predecessor_stop_generation.value()))},
        {"predecessor_stop_id",
         CanonicalValue(predecessor.predecessor_stop_id.value())},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"affected_operation_revisions",
       CanonicalValue(std::move(affected_operations))},
      {"captured_scope_digest",
       CanonicalValue(value.captured_scope_digest.value())},
      {"joined_predecessor_stops", CanonicalValue(std::move(predecessors))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"requested_selector_digest",
       CanonicalValue(value.requested_selector_digest.value())},
      {"resolution_disposition", CanonicalValue(std::string(core::to_string(
                                     value.resolution_disposition)))},
      {"resolution_reason",
       CanonicalValue(std::string(core::to_string(value.resolution_reason)))},
      {"stale_diagnostics", CanonicalValue(std::move(stale_diagnostics))},
      {"stop_generation",
       CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
  });
}

[[nodiscard]] std::string_view StopProjectionStateName(
    const core::StopDurableProjectionStateDTO state) noexcept {
  switch (state) {
  case core::StopDurableProjectionStateDTO::kNotApplicable:
    return "not_applicable";
  case core::StopDurableProjectionStateDTO::kPending:
    return "pending";
  case core::StopDurableProjectionStateDTO::kRecorded:
    return "recorded";
  case core::StopDurableProjectionStateDTO::kFailed:
    return "failed";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopStatusStateName(const core::StopStatusStateDTO state) noexcept {
  switch (state) {
  case core::StopStatusStateDTO::kAccepted:
    return "accepted";
  case core::StopStatusStateDTO::kStopping:
    return "stopping";
  case core::StopStatusStateDTO::kCertifying:
    return "certifying";
  case core::StopStatusStateDTO::kSucceeded:
    return "succeeded";
  case core::StopStatusStateDTO::kPartial:
    return "partial";
  case core::StopStatusStateDTO::kFailed:
    return "failed";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopFaultTriggerName(const core::StopFaultTriggerDTO trigger) noexcept {
  switch (trigger) {
  case core::StopFaultTriggerDTO::kScopePromotion:
    return "scope_promotion";
  case core::StopFaultTriggerDTO::kEscalated:
    return "escalated";
  case core::StopFaultTriggerDTO::kPartial:
    return "partial";
  case core::StopFaultTriggerDTO::kFailedUncontained:
    return "failed_uncontained";
  case core::StopFaultTriggerDTO::kPhysicalRestUnproven:
    return "physical_rest_unproven";
  case core::StopFaultTriggerDTO::kPersistenceDegraded:
    return "persistence_degraded";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopFaultLatchStateName(const core::StopFaultLatchStateDTO state) noexcept {
  switch (state) {
  case core::StopFaultLatchStateDTO::kLatched:
    return "latched";
  case core::StopFaultLatchStateDTO::kRecoveryPending:
    return "recovery_pending";
  case core::StopFaultLatchStateDTO::kCleared:
    return "cleared";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopFaultRecoveryStatusName(
    const core::StopFaultRecoveryStatusDTO status) noexcept {
  switch (status) {
  case core::StopFaultRecoveryStatusDTO::kWaitingForObservation:
    return "waiting_for_observation";
  case core::StopFaultRecoveryStatusDTO::kWaitingForQuiescence:
    return "waiting_for_quiescence";
  case core::StopFaultRecoveryStatusDTO::kWaitingForConfiguration:
    return "waiting_for_configuration";
  case core::StopFaultRecoveryStatusDTO::kWaitingForExternalReconciliation:
    return "waiting_for_external_reconciliation";
  case core::StopFaultRecoveryStatusDTO::kWaitingForDurableRecord:
    return "waiting_for_durable_record";
  case core::StopFaultRecoveryStatusDTO::kReady:
    return "ready";
  case core::StopFaultRecoveryStatusDTO::kRecoveryPending:
    return "recovery_pending";
  case core::StopFaultRecoveryStatusDTO::kCleared:
    return "cleared";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopFaultRecoveryEvidenceKindName(
    const core::StopFaultRecoveryEvidenceKindDTO kind) noexcept {
  switch (kind) {
  case core::StopFaultRecoveryEvidenceKindDTO::kSettlement:
    return "settlement";
  case core::StopFaultRecoveryEvidenceKindDTO::kLateQuiescence:
    return "late_quiescence";
  case core::StopFaultRecoveryEvidenceKindDTO::kConfiguration:
    return "configuration";
  case core::StopFaultRecoveryEvidenceKindDTO::kExternalReconciliation:
    return "external_reconciliation";
  case core::StopFaultRecoveryEvidenceKindDTO::kBackendOrLifecycleReset:
    return "backend_or_lifecycle_reset";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
StopCertificateRefValue(const core::StopCertificateRefDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"stop_certificate_id",
       CanonicalValue(value.stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(
           static_cast<double>(value.stop_certificate_revision.value()))},
      {"stop_generation",
       CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue
ProgramExecutionScopeValue(const core::ProgramExecutionScopeStateDTO &value) {
  // Group sparse node indices by state. Repeating field names for each of
  // 100,000 captured nodes exceeds the bounded native response envelope.
  CanonicalValue::Array ready, held, stopping, faulted, pending;
  for (const auto &node : value.held_nodes) {
    auto *indices =
        node.state == core::ProgramNodeHoldStateDTO::kReady      ? &ready
        : node.state == core::ProgramNodeHoldStateDTO::kHeld       ? &held
        : node.state == core::ProgramNodeHoldStateDTO::kStopping ? &stopping
        : node.state == core::ProgramNodeHoldStateDTO::kFaulted  ? &faulted
                                                                 : nullptr;
    if (indices != nullptr)
      indices->emplace_back(static_cast<double>(node.node_index));
  }
  for (const auto &effect : value.pending_effects) {
    pending.emplace_back(CanonicalValue::Object{
        {"node_index", CanonicalValue(static_cast<double>(effect.node_index))},
        {"operation_id", CanonicalValue(effect.operation_id.value())},
        {"participant_id", CanonicalValue(effect.participant_id.value())},
        {"stop_id", CanonicalValue(effect.stop_id.value())},
        {"stop_generation",
         CanonicalValue(static_cast<double>(effect.stop_generation.value()))}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.program_execution_scope.v2")},
      {"request_owner_epoch",
       CanonicalValue(value.request_owner_epoch.value())},
      {"program_runner_owner_epoch",
       CanonicalValue(value.program_runner_owner_epoch.value())},
      {"claim_generation",
       CanonicalValue(static_cast<double>(value.claim_generation.value()))},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"selection_revision",
       CanonicalValue(static_cast<double>(value.selection_revision))},
      {"ready_node_indices", CanonicalValue(std::move(ready))},
      {"held_node_indices", CanonicalValue(std::move(held))},
      {"stopping_node_indices", CanonicalValue(std::move(stopping))},
      {"faulted_node_indices", CanonicalValue(std::move(faulted))},
      {"pending_effects", CanonicalValue(std::move(pending))}});
}

[[nodiscard]] CanonicalValue ProgramSelectionAcceptedValue(
    const core::ProgramExecutionSelectionAcceptedDTO &value,
    const std::vector<std::string> &group_ids) {
  CanonicalValue::Array groups;
  for (const auto &group : group_ids)
    groups.emplace_back(group);
  const auto indices = [](const auto &source) {
    CanonicalValue::Array result;
    for (const auto index : source)
      result.emplace_back(static_cast<double>(index));
    return CanonicalValue(std::move(result));
  };
  CanonicalValue containment(nullptr);
  if (value.fresh_containment.has_value()) {
    const auto &stop = *value.fresh_containment;
    // Existing operation/Stop queries own recovery evidence. This result
    // identifies physical work without duplicating its budget/certificate.
    containment = CanonicalValue(CanonicalValue::Object{
        {"stop_id", CanonicalValue(stop.stop_id.value())},
        {"stop_generation",
         CanonicalValue(static_cast<double>(stop.stop_generation.value()))},
        {"resolution_disposition", CanonicalValue(std::string(core::to_string(
                                       stop.resolution_disposition)))}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"state", value.state.has_value()
                    ? ProgramExecutionScopeValue(*value.state)
                    : CanonicalValue(nullptr)},
      {"group_ids", CanonicalValue(std::move(groups))},
      {"widened_node_indices", indices(value.widened_node_indices)},
      {"fresh_containment", std::move(containment)},
      {"selection_error", value.selection_error.has_value()
                              ? ErrorValue(*value.selection_error)
                              : CanonicalValue(nullptr)},
      {"replay_status",
       CanonicalValue(value.replay_status ==
                              core::RequestReplayStatusDTO::kReplayedExact
                          ? "replayed_exact"
                          : "accepted_new")}});
}

[[nodiscard]] CanonicalValue
StopStatusSummaryValue(const core::StopStatusDTO &value) {
  const auto terminal_projection = StopProjectionStateName(value.terminal_projection.state);
  Require(terminal_projection == "pending" || terminal_projection == "recorded" ||
              terminal_projection == "failed",
          "Stop terminal projection status is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"request_id", CanonicalValue(value.accepted.request_id.value())},
      {"schema_version", CanonicalValue("manyforge.execution_stop_status.v1")},
      {"state", CanonicalValue(std::string(StopStatusStateName(value.state)))},
      {"stop_generation", CanonicalValue(static_cast<double>(
                              value.accepted.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.accepted.stop_id.value())},
      {"terminal_projection", CanonicalValue(std::string(terminal_projection))},
  });
}

[[nodiscard]] CanonicalValue
StopProjectionStatusValue(const core::StopDurableProjectionStatusDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"error", value.error.has_value() ? ErrorValue(*value.error)
                                        : CanonicalValue(nullptr)},
      {"retry_generation",
       value.retry_generation.has_value()
           ? CanonicalValue(static_cast<double>(*value.retry_generation))
           : CanonicalValue(nullptr)},
      {"state",
       CanonicalValue(std::string(StopProjectionStateName(value.state)))},
  });
}

[[nodiscard]] CanonicalValue
StopFaultLatchValue(const core::StopFaultLatchDTO &value) {
  CanonicalValue::Array captured_operations;
  captured_operations.reserve(value.captured_operation_ids.size());
  for (const auto &operation : value.captured_operation_ids) {
    captured_operations.emplace_back(operation.value());
  }
  CanonicalValue::Array captured_resources;
  captured_resources.reserve(value.captured_resources.size());
  for (const auto &resource : value.captured_resources) {
    captured_resources.emplace_back(resource.value());
  }
  CanonicalValue::Array evidence_refs;
  evidence_refs.reserve(value.evidence_refs.size());
  for (const auto &evidence : value.evidence_refs) {
    evidence_refs.emplace_back(CanonicalValue::Object{
        {"evidence_digest", CanonicalValue(evidence.evidence_digest.value())},
        {"kind", CanonicalValue(std::string(
                     StopFaultRecoveryEvidenceKindName(evidence.kind)))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"captured_operation_ids",
       CanonicalValue(std::move(captured_operations))},
      {"captured_resources", CanonicalValue(std::move(captured_resources))},
      {"cell_id", CanonicalValue(value.cell_id.value())},
      {"clear_projection", StopProjectionStatusValue(value.clear_projection)},
      {"evidence_refs", CanonicalValue(std::move(evidence_refs))},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"latch_generation",
       CanonicalValue(static_cast<double>(value.latch_generation.value()))},
      {"latch_id", CanonicalValue(value.latch_id.value())},
      {"persistence_degraded", CanonicalValue(value.persistence_degraded)},
      {"reason_code", CanonicalValue(value.reason_code)},
      {"recovery_assessment_revision",
       CanonicalValue(static_cast<double>(value.recovery_assessment_revision))},
      {"recovery_status",
       CanonicalValue(
           std::string(StopFaultRecoveryStatusName(value.recovery_status)))},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"state",
       CanonicalValue(std::string(StopFaultLatchStateName(value.state)))},
      {"stop_certificate",
       value.stop_certificate.has_value()
           ? StopCertificateRefValue(*value.stop_certificate)
           : CanonicalValue(nullptr)},
      {"stop_generation",
       CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
      {"terminal_projection",
       StopProjectionStatusValue(value.terminal_projection)},
      {"trigger",
       CanonicalValue(std::string(StopFaultTriggerName(value.trigger)))},
  });
}

[[nodiscard]] CanonicalValue
StopFaultOfferValue(const core::StopFaultRecoveryOfferDTO &offer) {
  Require(!core::stop_fault_recovery_offer_field_error(offer).has_value(),
          "Stop-fault offer is not a valid atomic owner projection");
  CanonicalValue::Array rows;
  rows.reserve(offer.configuration_expectations.size());
  for (const auto &expectation : offer.configuration_expectations) {
    rows.emplace_back(CanonicalValue::Object{
        {"active_control_configuration_generation",
         CanonicalValue(static_cast<double>(
             expectation.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(expectation.active_control_configuration_id.value())},
        {"control_domain_id",
         CanonicalValue(expectation.control_domain_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"configuration_expectations", CanonicalValue(std::move(rows))},
      {"latch", StopFaultLatchValue(offer.latch)},
      {"recovery_unavailable_reason",
       offer.recovery_unavailable_reason
           ? CanonicalValue(*offer.recovery_unavailable_reason)
           : CanonicalValue(nullptr)},
  });
}

namespace {
CanonicalValue GlobalRuleContextValue(const core::IoRuleStateContextDTO &context) {
  Require(!core::io_rule_state_context_field_error(context),
          "global rule context is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"owner_epoch", CanonicalValue(context.owner_epoch.value())},
      {"catalog_generation", CanonicalValue(std::to_string(context.catalog_generation))},
      {"active_publication_generation", CanonicalValue(static_cast<double>(
          context.active_publication_generation.value()))},
      {"bundle", CanonicalValue(context.retained_bundle_id.value())},
      {"graph", CanonicalValue(context.graph_revision.value())}});
}
CanonicalValue GlobalRuleStateValue(const core::IoRuleStateContextDTO &context,
                                    const core::IoRuleStateDTO &row) {
  Require(!core::io_rule_state_field_error(context, row),
          "global rule state is invalid");
  CanonicalValue acquisition(nullptr);
  if (row.triggering_acquisition) {
    const auto &a = *row.triggering_acquisition;
    acquisition = CanonicalValue(CanonicalValue::Object{
        {"source_id", CanonicalValue(a.source_id.value())},
        {"source_epoch", CanonicalValue(a.incarnation.value())},
        {"source_sequence", CanonicalValue(std::to_string(a.source_sequence))},
        {"source_cursor", IoSourceCursorValue(a.source_cursor)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"rule_id", CanonicalValue(row.rule_id.value())},
      {"state", CanonicalValue(std::string(core::io_global_rule_state_token(row.state)))},
      {"triggering_acquisition", std::move(acquisition)}});
}
} // namespace

CanonicalValue GlobalIoRuleRecoveryOfferValue(
    const core::IoGlobalRuleRecoveryOfferDTO &offer) {
  const auto &selector = offer.selector;
  Require(!selector.rule_id.empty() &&
              selector.rule_id.value().find('\0') == std::string::npos &&
              selector.expected_rule_registration_id > 0U,
          "global rule recovery selector is invalid");
  const bool loss = offer.latch_cause == core::IoGlobalRuleLatchCauseDTO::kSourceLoss;
  Require(loss || offer.latch_cause == core::IoGlobalRuleLatchCauseDTO::kCondition,
          "global rule latch cause is invalid");
  const bool automatic = offer.post_latch_disposition ==
                         core::InterruptPostLatchDisposition::kAutoClearOnSignalRelease;
  Require((automatic || offer.post_latch_disposition ==
                            core::InterruptPostLatchDisposition::kAwaitOperatorAck) &&
              (!loss || !automatic),
          "global source loss has no operator recovery disposition");
  return CanonicalValue(CanonicalValue::Object{
      {"selector", CanonicalValue(CanonicalValue::Object{
          {"context", GlobalRuleContextValue(selector.context)},
          {"rule_id", CanonicalValue(selector.rule_id.value())},
          {"expected_rule_registration_id", CanonicalValue(
              std::to_string(selector.expected_rule_registration_id))}})},
      {"current_condition", offer.current_condition
          ? CanonicalValue(*offer.current_condition) : CanonicalValue(nullptr)},
      {"post_latch_disposition", CanonicalValue(automatic
          ? "auto_clear_on_signal_release" : "await_operator_ack")},
      {"latch_cause", CanonicalValue(loss ? "source_loss" : "condition")}});
}

CanonicalValue GlobalIoRuleRecoveryResultValue(
    const core::IoGlobalRuleRecoveryResultDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"request_id", CanonicalValue(value.request_id.value())},
      {"replay_status", CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"accepted", CanonicalValue(value.accepted)},
      {"context", GlobalRuleContextValue(value.context)},
      {"rule", GlobalRuleStateValue(value.context, value.rule)},
      {"error", value.error ? ErrorValue(*value.error) : CanonicalValue(nullptr)}});
}

[[nodiscard]] CanonicalValue
StopFaultRecoveryResultValue(const core::StopFaultRecoveryResultDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted", CanonicalValue(value.accepted)},
      {"error", value.error.has_value() ? ErrorValue(*value.error)
                                        : CanonicalValue(nullptr)},
      {"latch", StopFaultLatchValue(value.latch)},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] std::string_view
StopCertificateResultName(core::StopCertificateResultDTO result) noexcept {
  switch (result) {
  case core::StopCertificateResultDTO::kSucceededNominal:
    return "succeeded_nominal";
  case core::StopCertificateResultDTO::kSucceededEscalated:
    return "succeeded_escalated";
  case core::StopCertificateResultDTO::kPartial:
    return "partial";
  case core::StopCertificateResultDTO::kFailedUncontained:
    return "failed_uncontained";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopStageOutcomeName(core::ParticipantStopStageOutcomeDTO outcome) noexcept {
  switch (outcome) {
  case core::ParticipantStopStageOutcomeDTO::kSucceeded:
    return "succeeded";
  case core::ParticipantStopStageOutcomeDTO::kPartial:
    return "partial";
  case core::ParticipantStopStageOutcomeDTO::kFailed:
    return "failed";
  case core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect:
    return "superseded_no_effect";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopAttemptDispositionName(
    const core::StopStageAttemptDispositionDTO disposition) noexcept {
  switch (disposition) {
  case core::StopStageAttemptDispositionDTO::kNotDispatched:
    return "not_dispatched";
  case core::StopStageAttemptDispositionDTO::kDispatched:
    return "dispatched";
  case core::StopStageAttemptDispositionDTO::kEffectUncertain:
    return "effect_uncertain";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopEvidenceStateName(const core::StopEvidenceItemStateDTO state) noexcept {
  switch (state) {
  case core::StopEvidenceItemStateDTO::kRequiredPending:
    return "required_pending";
  case core::StopEvidenceItemStateDTO::kSatisfied:
    return "satisfied";
  case core::StopEvidenceItemStateDTO::kMissing:
    return "missing";
  case core::StopEvidenceItemStateDTO::kFailed:
    return "failed";
  case core::StopEvidenceItemStateDTO::kInapplicable:
    return "inapplicable";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopStageAuthorityName(
    const core::StopStageResultAuthorityDTO authority) noexcept {
  switch (authority) {
  case core::StopStageResultAuthorityDTO::kAuthoritative:
    return "authoritative";
  case core::StopStageResultAuthorityDTO::kSupersededLate:
    return "superseded_late";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopParticipantSettlementName(
    const core::StopParticipantScopeFactsDTO::TerminalSettlementState
        settlement) noexcept {
  using State = core::StopParticipantScopeFactsDTO::TerminalSettlementState;
  switch (settlement) {
  case State::kNotRequired:
    return "not_required";
  case State::kPending:
    return "pending";
  case State::kSettled:
    return "settled";
  case State::kContainedUnsettled:
    return "contained_unsettled";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopSelectorKindName(const core::StopRequestedSelectorKindDTO kind) noexcept {
  switch (kind) {
  case core::StopRequestedSelectorKindDTO::kOrdinaryOperations:
    return "ordinary_operations";
  case core::StopRequestedSelectorKindDTO::kOrdinaryComponents:
    return "ordinary_components";
  case core::StopRequestedSelectorKindDTO::kOrdinaryResources:
    return "ordinary_resources";
  case core::StopRequestedSelectorKindDTO::kExplicitFull:
    return "explicit_full";
  case core::StopRequestedSelectorKindDTO::kMalformedIngress:
    return "malformed_ingress";
  case core::StopRequestedSelectorKindDTO::kInternalProgramRun:
    return "internal_program_run";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
MotionInterruptStopModeName(const core::MotionInterruptStopMode mode) noexcept {
  switch (mode) {
  case core::MotionInterruptStopMode::kSoftOnPath:
    return "soft_on_path";
  case core::MotionInterruptStopMode::kSpringBackHold:
    return "spring_back_hold";
  case core::MotionInterruptStopMode::kHardDeceleration:
    return "hard_deceleration";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
StopHistoryJsonValue(const core::JsonValueDTO &value) {
  return std::visit(
      [](const auto &current) -> CanonicalValue {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Value, bool> ||
                             std::is_same_v<Value, double> ||
                             std::is_same_v<Value, std::string>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value, core::JsonValueDTO::Array>) {
          CanonicalValue::Array projected;
          projected.reserve(current.size());
          for (const auto &item : current) {
            projected.emplace_back(StopHistoryJsonValue(item));
          }
          return CanonicalValue(std::move(projected));
        } else {
          CanonicalValue::Object projected;
          projected.reserve(current.size());
          for (const auto &[name, item] : current) {
            projected.emplace_back(name, StopHistoryJsonValue(item));
          }
          return CanonicalValue(std::move(projected));
        }
      },
      value.value);
}

[[nodiscard]] CanonicalValue
StopHistoryStructuredValue(const core::InterruptStructuredValue &value) {
  CanonicalValue::Object projected;
  projected.reserve(value.size());
  for (const auto &[name, item] : value) {
    projected.emplace_back(
        name,
        std::visit([](const auto &typed) { return CanonicalValue(typed); },
                   item));
  }
  return CanonicalValue(std::move(projected));
}

[[nodiscard]] CanonicalValue
StopHistoryAccelerationValue(const core::CoordinateAccelerationDTO &value) {
  return std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Acceleration = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Acceleration,
                                     core::AngularAccelerationDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("angular")},
              {"value_rad_s2", CanonicalValue(typed.value_rad_s2)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("linear")},
              {"value_m_s2", CanonicalValue(typed.value_m_s2)},
          });
        }
      },
      value);
}

[[nodiscard]] CanonicalValue
StopHistoryVelocityValue(const core::CoordinateVelocityDTO &value) {
  return std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Velocity = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Velocity, core::AngularVelocityDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("angular")},
              {"value_rad_s", CanonicalValue(typed.value_rad_s)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("linear")},
              {"value_m_s", CanonicalValue(typed.value_m_s)},
          });
        }
      },
      value);
}

[[nodiscard]] CanonicalValue
StopHistoryScopeKeyValue(const core::StopScopeEntryKeyDTO &key) {
  const auto subject = std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Subject = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Subject,
                                     core::StopParticipantScopeKeyDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("participant")},
              {"operation_id", CanonicalValue(typed.operation_id.value())},
              {"participant_id", CanonicalValue(typed.participant_id.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("resource")},
              {"resource_id", CanonicalValue(typed.value())},
          });
        }
      },
      key.subject);
  return CanonicalValue(CanonicalValue::Object{
      {"origin_stop_generation",
       CanonicalValue(static_cast<double>(key.origin_stop_generation.value()))},
      {"origin_stop_id", CanonicalValue(key.origin_stop_id.value())},
      {"subject", subject},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryScopeKeyArray(const std::vector<core::StopScopeEntryKeyDTO> &keys) {
  CanonicalValue::Array values;
  values.reserve(keys.size());
  for (const auto &key : keys) {
    values.emplace_back(StopHistoryScopeKeyValue(key));
  }
  return CanonicalValue(std::move(values));
}

[[nodiscard]] CanonicalValue
StopHistoryProfileValue(const core::MotionInterruptStopProfileDTO &profile) {
  CanonicalValue::Object jerk;
  for (const auto &[joint, limit] : profile.per_joint_max_jerk) {
    jerk.emplace_back(
        joint,
        std::visit(
            [](const auto &value) -> CanonicalValue {
              if constexpr (requires { value.value_rad_s3; })
                return CanonicalValue(CanonicalValue::Object{
                    {"coordinate_kind", CanonicalValue("angular_rad_s3")},
                    {"value_rad_s3", CanonicalValue(value.value_rad_s3)}});
              else
                return CanonicalValue(CanonicalValue::Object{
                    {"coordinate_kind", CanonicalValue("linear_m_s3")},
                    {"value_m_s3", CanonicalValue(value.value_m_s3)}});
            },
            limit));
  }
  CanonicalValue::Object deceleration;
  deceleration.reserve(profile.per_joint_max_deceleration.size());
  for (const auto &[joint, limit] : profile.per_joint_max_deceleration) {
    deceleration.emplace_back(joint, StopHistoryAccelerationValue(limit));
  }
  CanonicalValue settle(nullptr);
  if (profile.per_joint_settle_velocity_threshold.has_value()) {
    CanonicalValue::Object thresholds;
    thresholds.reserve(profile.per_joint_settle_velocity_threshold->size());
    for (const auto &[joint, limit] :
         *profile.per_joint_settle_velocity_threshold) {
      thresholds.emplace_back(joint, StopHistoryVelocityValue(limit));
    }
    settle = CanonicalValue(std::move(thresholds));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"allow_natural_completion_on_short_path",
       CanonicalValue(profile.allow_natural_completion_on_short_path)},
      {"deceleration_time_s", profile.deceleration_time_s.has_value()
                                  ? CanonicalValue(*profile.deceleration_time_s)
                                  : CanonicalValue(nullptr)},
      {"diagnostics", StopHistoryStructuredValue(profile.diagnostics)},
      {"jerk_limits_enabled", profile.jerk_limits_enabled.has_value()
                                  ? CanonicalValue(*profile.jerk_limits_enabled)
                                  : CanonicalValue(nullptr)},
      {"per_joint_max_jerk", CanonicalValue(std::move(jerk))},
      {"per_joint_max_deceleration", CanonicalValue(std::move(deceleration))},
      {"per_joint_settle_velocity_threshold", std::move(settle)},
      {"profile_id", CanonicalValue(profile.profile_id)},
      {"recoverable_substitution_mode",
       profile.recoverable_substitution_mode.has_value()
           ? CanonicalValue(std::string(MotionInterruptStopModeName(
                 *profile.recoverable_substitution_mode)))
           : CanonicalValue(nullptr)},
      {"require_backend_confirmation",
       CanonicalValue(profile.require_backend_confirmation)},
      {"stop_mode", CanonicalValue(std::string(
                        MotionInterruptStopModeName(profile.stop_mode)))},
  });
}

[[nodiscard]] CanonicalValue StopHistoryEvidenceSourceValue(
    const core::ResolvedStopEvidenceSourceDTO &source) {
  const auto &target = source.runtime_target;
  CanonicalValue contract(nullptr);
  if (source.provider_contract_ref.has_value()) {
    contract = CanonicalValue(CanonicalValue::Object{
        {"contract_schema_id",
         CanonicalValue(
             source.provider_contract_ref->contract_schema_id.value())},
        {"contract_schema_revision",
         CanonicalValue(
             source.provider_contract_ref->contract_schema_revision.value())},
        {"provider_contract_role_id",
         CanonicalValue(
             source.provider_contract_ref->provider_contract_role_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract", std::move(contract)},
      {"runtime_target",
       CanonicalValue(CanonicalValue::Object{
           {"content_digest", CanonicalValue(target.content_digest.value())},
           {"endpoint_role_id",
            target.endpoint_role_id.has_value()
                ? CanonicalValue(target.endpoint_role_id->value())
                : CanonicalValue(nullptr)},
           {"provider_instance_id",
            CanonicalValue(target.provider_instance_id.value())},
           {"provider_manifest_id",
            CanonicalValue(target.provider_manifest_id.value())},
           {"provider_manifest_revision",
            CanonicalValue(target.provider_manifest_revision.value())},
           {"provider_runtime_epoch",
            CanonicalValue(target.provider_runtime_epoch.value())},
           {"runtime_unit_epoch",
            CanonicalValue(target.runtime_unit_epoch.value())},
           {"runtime_unit_id", CanonicalValue(target.runtime_unit_id.value())},
       })},
      {"source_kind",
       CanonicalValue(std::string(core::to_string(source.source_kind)))},
      {"state_authority_class",
       source.state_authority_class.has_value()
           ? CanonicalValue(
                 std::string(core::to_string(*source.state_authority_class)))
           : CanonicalValue(nullptr)},
      {"state_authority_id",
       source.state_authority_id.has_value()
           ? CanonicalValue(source.state_authority_id->value())
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryEvidenceItemValue(const core::StopEvidenceItemDTO &item) {
  return CanonicalValue(CanonicalValue::Object{
      {"error", item.error.has_value() ? ErrorValue(*item.error)
                                       : CanonicalValue(nullptr)},
      {"evidence", item.evidence.has_value()
                       ? StopHistoryJsonValue(*item.evidence)
                       : CanonicalValue(nullptr)},
      {"evidence_item_id", CanonicalValue(item.evidence_item_id.value())},
      {"required_class",
       CanonicalValue(std::string(core::to_string(item.required_class)))},
      {"resolved_source", StopHistoryEvidenceSourceValue(item.resolved_source)},
      {"source_age_at_evaluation_ns",
       item.source_age_at_evaluation_ns.has_value()
           ? CanonicalValue(
                 static_cast<double>(*item.source_age_at_evaluation_ns))
           : CanonicalValue(nullptr)},
      {"source_epoch", item.source_epoch.has_value()
                           ? CanonicalValue(item.source_epoch->value())
                           : CanonicalValue(nullptr)},
      {"source_sequence",
       item.source_sequence.has_value()
           ? CanonicalValue(static_cast<double>(*item.source_sequence))
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(StopEvidenceStateName(item.state)))},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryAttemptValue(const core::StopAttemptedStageResultDTO &attempt,
                        const std::int64_t accepted_at_ns) {
  const auto &result = attempt.result;
  const auto &request = result.request;
  Require(request.stage_index <
              request.participant_stop_plan.stage_commands.size(),
          "terminal Stop stage has no matching prepared command");
  const auto &prepared =
      request.participant_stop_plan.stage_commands[request.stage_index];
  const auto &stage = prepared.resolved_stage;
  CanonicalValue::Array resources;
  resources.reserve(request.resources.size());
  for (const auto &resource : request.resources) {
    resources.emplace_back(resource.value());
  }
  CanonicalValue::Array evidence;
  evidence.reserve(result.evidence_items.size());
  for (const auto &item : result.evidence_items) {
    evidence.emplace_back(StopHistoryEvidenceItemValue(item));
  }
  const auto acceptance_latency =
      attempt.completed_at_ns >= accepted_at_ns
          ? CanonicalValue(
                static_cast<double>(attempt.completed_at_ns - accepted_at_ns))
          : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance_to_stage_latency_ns", acceptance_latency},
      {"actual_stop_mode", result.actual_provider_contract_mode.has_value()
                               ? CanonicalValue(std::string(core::to_string(
                                     *result.actual_provider_contract_mode)))
                               : CanonicalValue(nullptr)},
      {"attempt_disposition",
       CanonicalValue(std::string(
           StopAttemptDispositionName(result.attempt_disposition)))},
      {"authority",
       CanonicalValue(std::string(StopStageAuthorityName(attempt.authority)))},
      {"completed_at_ns", manyforge::content_identity::nonnegative_int64_value(
                              attempt.completed_at_ns)},
      {"controller_acknowledgement_sequence",
       result.controller_acknowledgement_sequence.has_value()
           ? CanonicalValue(static_cast<double>(
                 *result.controller_acknowledgement_sequence))
           : CanonicalValue(nullptr)},
      {"error", result.error.has_value() ? ErrorValue(*result.error)
                                         : CanonicalValue(nullptr)},
      {"evidence_items", CanonicalValue(std::move(evidence))},
      {"operation_id", CanonicalValue(request.operation_id.value())},
      {"operation_revision",
       CanonicalValue(static_cast<double>(request.operation_revision.value()))},
      {"outcome",
       CanonicalValue(std::string(StopStageOutcomeName(result.outcome)))},
      {"participant_id", CanonicalValue(request.participant_id.value())},
      {"participant_stop_invocation_plan_digest",
       CanonicalValue(request.participant_stop_plan
                          .participant_stop_invocation_plan_digest.value())},
      {"profile",
       request.participant_stop_plan.participant_stop_profile.has_value()
           ? StopHistoryProfileValue(
                 *request.participant_stop_plan.participant_stop_profile)
           : CanonicalValue(nullptr)},
      {"profile_purpose",
       CanonicalValue(std::string(core::to_string(
           request.participant_stop_plan.participant_stop_profile_purpose)))},
      {"profile_source", prepared.parameter_source.has_value()
                             ? CanonicalValue(std::string(
                                   core::to_string(*prepared.parameter_source)))
                             : CanonicalValue(nullptr)},
      {"provider_runtime_epoch",
       CanonicalValue(request.provider_runtime_epoch.value())},
      {"receiver_elapsed_duration_ns",
       CanonicalValue(
           static_cast<double>(result.receiver_elapsed_duration_ns))},
      {"recoverable_substitution_reason",
       result.recoverable_substitution_reason.has_value()
           ? CanonicalValue(*result.recoverable_substitution_reason)
           : CanonicalValue(nullptr)},
      {"resolved_stop_base_plan_id",
       CanonicalValue(
           request.participant_stop_plan.resolved_stop_base_plan_id.value())},
      {"resolved_stop_scope_policy_digest",
       CanonicalValue(request.resolved_stop_scope_policy_digest.value())},
      {"resources", CanonicalValue(std::move(resources))},
      {"result_sequence",
       CanonicalValue(static_cast<double>(attempt.result_sequence))},
      {"source_epoch", result.source_epoch.has_value()
                           ? CanonicalValue(result.source_epoch->value())
                           : CanonicalValue(nullptr)},
      {"source_sequence",
       result.source_sequence.has_value()
           ? CanonicalValue(static_cast<double>(*result.source_sequence))
           : CanonicalValue(nullptr)},
      {"stage",
       CanonicalValue(CanonicalValue::Object{
           {"action_class",
            CanonicalValue(std::string(core::to_string(
                stage.command_qualification.stop_action_class)))},
           {"command_kind", CanonicalValue(std::string(core::to_string(
                                stage.containment_command.command_kind)))},
           {"default_contract_parameters",
            stage.default_contract_parameters.has_value()
                ? StopHistoryJsonValue(
                      core::JsonValueDTO{*stage.default_contract_parameters})
                : CanonicalValue(nullptr)},
           {"stage_id", CanonicalValue(stage.stop_escalation_stage_id.value())},
           {"stage_index",
            CanonicalValue(static_cast<double>(stage.stage_index))},
           {"stage_kind",
            CanonicalValue(std::string(core::to_string(stage.stage_kind)))},
           {"validated_contract_parameters",
            prepared.validated_contract_parameters.has_value()
                ? StopHistoryJsonValue(core::JsonValueDTO{
                      *prepared.validated_contract_parameters})
                : CanonicalValue(nullptr)},
       })},
      {"stop_evidence_policy_id",
       CanonicalValue(request.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(request.stop_evidence_policy_revision.value())},
      {"scope_key", StopHistoryScopeKeyValue(attempt.scope_key)},
  });
}

[[nodiscard]] CanonicalValue
StopHistorySettlementValue(const core::StopScopeEntryDTO &entry) {
  CanonicalValue facts = std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Facts = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Facts,
                                     core::StopParticipantScopeFactsDTO>) {
          CanonicalValue::Array resources;
          resources.reserve(typed.resources.size());
          for (const auto &resource : typed.resources) {
            resources.emplace_back(resource.value());
          }
          return CanonicalValue(CanonicalValue::Object{
              {"command_target_id",
               CanonicalValue(typed.command_target_id.value())},
              {"controllable_component_id",
               CanonicalValue(typed.controllable_component_id.value())},
              {"controller_goal_generation",
               CanonicalValue(static_cast<double>(
                   typed.controller_goal_generation.value()))},
              {"controller_goal_id",
               CanonicalValue(typed.controller_goal_id.value())},
              {"kind", CanonicalValue("participant")},
              {"provider_runtime_epoch",
               CanonicalValue(typed.provider_runtime_epoch.value())},
              {"resources", CanonicalValue(std::move(resources))},
              {"terminal_settlement",
               CanonicalValue(std::string(
                   StopParticipantSettlementName(typed.terminal_settlement)))},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("resource")},
              {"resource_id", CanonicalValue(typed.value())},
              {"terminal_settlement", CanonicalValue("not_required")},
          });
        }
      },
      entry.facts);
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             entry.accepted_at_ns)},
      {"dispatch_state",
       CanonicalValue(static_cast<double>(entry.dispatch_state))},
      {"facts", std::move(facts)},
      {"participant_stop_invocation_plan_digest",
       entry.participant_stop_invocation_plan_digest.has_value()
           ? CanonicalValue(
                 entry.participant_stop_invocation_plan_digest->value())
           : CanonicalValue(nullptr)},
      {"scope_key", StopHistoryScopeKeyValue(entry.key)},
      {"stop_evidence_policy_id",
       entry.stop_evidence_policy_id.has_value()
           ? CanonicalValue(entry.stop_evidence_policy_id->value())
           : CanonicalValue(nullptr)},
      {"stop_evidence_policy_revision",
       entry.stop_evidence_policy_revision.has_value()
           ? CanonicalValue(entry.stop_evidence_policy_revision->value())
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryFaultValue(const std::optional<core::StopFaultLatchDTO> &fault) {
  if (!fault.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"captured_operation_count", CanonicalValue(static_cast<double>(
                                       fault->captured_operation_ids.size()))},
      {"captured_resource_count",
       CanonicalValue(static_cast<double>(fault->captured_resources.size()))},
      {"cell_id", CanonicalValue(fault->cell_id.value())},
      {"latch_generation",
       CanonicalValue(static_cast<double>(fault->latch_generation.value()))},
      {"latch_id", CanonicalValue(fault->latch_id.value())},
      {"persistence_degraded", CanonicalValue(fault->persistence_degraded)},
      {"reason_code", CanonicalValue(fault->reason_code)},
      {"recovery_assessment_revision",
       CanonicalValue(
           static_cast<double>(fault->recovery_assessment_revision))},
      {"recovery_status",
       CanonicalValue(
           std::string(StopFaultRecoveryStatusName(fault->recovery_status)))},
      {"state",
       CanonicalValue(std::string(StopFaultLatchStateName(fault->state)))},
      {"stop_certificate",
       fault->stop_certificate.has_value()
           ? StopCertificateRefValue(*fault->stop_certificate)
           : CanonicalValue(nullptr)},
      {"stop_generation",
       CanonicalValue(static_cast<double>(fault->stop_generation.value()))},
      {"stop_id", CanonicalValue(fault->stop_id.value())},
      {"trigger",
       CanonicalValue(std::string(StopFaultTriggerName(fault->trigger)))},
  });
}

[[nodiscard]] CanonicalValue StopTerminalHistoryProjectionValue(
    const core::StopJournalTerminalProjectionDTO &projection,
    const std::string_view source_owner_epoch,
    const std::string_view durable_projection_digest,
    const bool include_details) {
  const auto &status = projection.stop_status;
  const auto &certificate = projection.certificate;
  CanonicalValue::Array attempted_stages;
  CanonicalValue::Array participant_settlement;
  CanonicalValue::Array certificate_evidence;
  CanonicalValue::Array predecessor_certificates;
  if (include_details) {
    attempted_stages.reserve(certificate.attempted_stage_results.size());
    for (const auto &attempt : certificate.attempted_stage_results) {
      attempted_stages.emplace_back(
          StopHistoryAttemptValue(attempt, status.accepted.accepted_at_ns));
    }
    participant_settlement.reserve(certificate.scope_entries.size());
    for (const auto &entry : certificate.scope_entries) {
      participant_settlement.emplace_back(StopHistorySettlementValue(entry));
    }
    certificate_evidence.reserve(certificate.evidence_items.size());
    for (const auto &item : certificate.evidence_items) {
      certificate_evidence.emplace_back(CanonicalValue::Object{
          {"evidence_item_id",
           CanonicalValue(item.evidence_item.evidence_item_id.value())},
          {"required_class", CanonicalValue(std::string(core::to_string(
                                 item.evidence_item.required_class)))},
          {"result_sequence",
           CanonicalValue(static_cast<double>(item.result_sequence))},
          {"scope_key", StopHistoryScopeKeyValue(item.scope_key)},
          {"stage_id", CanonicalValue(item.stage_id.value())},
          {"stage_index",
           CanonicalValue(static_cast<double>(item.stage_index))},
          {"state", CanonicalValue(std::string(
                        StopEvidenceStateName(item.evidence_item.state)))},
      });
    }
    predecessor_certificates.reserve(
        certificate.predecessor_certificates.size());
    for (const auto &reference : certificate.predecessor_certificates) {
      predecessor_certificates.emplace_back(StopCertificateRefValue(reference));
    }
  }
  const auto duration =
      certificate.completed_at_ns >= status.accepted.accepted_at_ns
          ? CanonicalValue(static_cast<double>(certificate.completed_at_ns -
                                               status.accepted.accepted_at_ns))
          : CanonicalValue(nullptr);
  const bool escalated =
      certificate.result == core::StopCertificateResultDTO::kSucceededEscalated;
  const bool hard_stop_floor_used = std::any_of(
      certificate.attempted_stage_results.begin(),
      certificate.attempted_stage_results.end(), [](const auto &attempt) {
        const auto &request = attempt.result.request;
        return request.stage_index <
                   request.participant_stop_plan.stage_commands.size() &&
               request.participant_stop_plan.stage_commands[request.stage_index]
                       .resolved_stage.command_qualification
                       .stop_action_class != core::StopActionClass::kControlled;
      });
  const auto acceptance =
      include_details
          ? StopAcceptedValue(status.accepted)
          : CanonicalValue(CanonicalValue::Object{
                {"accepted_at_ns",
                 manyforge::content_identity::nonnegative_int64_value(
                     status.accepted.accepted_at_ns)},
                {"request_id",
                 CanonicalValue(status.accepted.request_id.value())},
                {"stop_generation",
                 CanonicalValue(static_cast<double>(
                     status.accepted.stop_generation.value()))},
                {"stop_id", CanonicalValue(status.accepted.stop_id.value())},
            });
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", acceptance},
      {"acceptance_to_terminal_latency_ns", duration},
      {"api_version", CanonicalValue("manyforge.stop_terminal_projection.v1")},
      {"attempted_stage_count",
       CanonicalValue(
           static_cast<double>(certificate.attempted_stage_results.size()))},
      {"attempted_stages", CanonicalValue(std::move(attempted_stages))},
      {"certificate_evidence", CanonicalValue(std::move(certificate_evidence))},
      {"certificate_evidence_count",
       CanonicalValue(static_cast<double>(certificate.evidence_items.size()))},
      {"completed_at_ns", manyforge::content_identity::nonnegative_int64_value(
                              certificate.completed_at_ns)},
      {"details_truncated", CanonicalValue(!include_details)},
      {"dispatched_stopped",
       include_details
           ? StopHistoryScopeKeyArray(certificate.dispatched_stopped)
           : CanonicalValue(CanonicalValue::Array{})},
      {"dispatched_stopped_count", CanonicalValue(static_cast<double>(
                                       certificate.dispatched_stopped.size()))},
      {"durable_projection_digest",
       CanonicalValue(std::string(durable_projection_digest))},
      {"escalated", CanonicalValue(escalated)},
      {"failed", include_details ? StopHistoryScopeKeyArray(certificate.failed)
                                 : CanonicalValue(CanonicalValue::Array{})},
      {"failed_count",
       CanonicalValue(static_cast<double>(certificate.failed.size()))},
      {"fault_latch", StopHistoryFaultValue(projection.fault_latch)},
      {"hard_stop_floor_used", CanonicalValue(hard_stop_floor_used)},
      {"owner_epoch", CanonicalValue(std::string(source_owner_epoch))},
      {"participant_settlement",
       CanonicalValue(std::move(participant_settlement))},
      {"participant_settlement_count",
       CanonicalValue(static_cast<double>(certificate.scope_entries.size()))},
      {"pre_dispatch_cancelled",
       include_details
           ? StopHistoryScopeKeyArray(certificate.pre_dispatch_cancelled)
           : CanonicalValue(CanonicalValue::Array{})},
      {"pre_dispatch_cancelled_count",
       CanonicalValue(
           static_cast<double>(certificate.pre_dispatch_cancelled.size()))},
      {"predecessor_certificates",
       CanonicalValue(std::move(predecessor_certificates))},
      {"predecessor_certificate_count",
       CanonicalValue(
           static_cast<double>(certificate.predecessor_certificates.size()))},
      {"record_kind", CanonicalValue("stop_terminal_projected")},
      {"request_id", CanonicalValue(status.accepted.request_id.value())},
      {"requested_selector",
       CanonicalValue(CanonicalValue::Object{
           {"kind", CanonicalValue(std::string(StopSelectorKindName(
                        certificate.requested_selector.kind)))},
           {"malformed_ingress_reason",
            certificate.requested_selector.malformed_ingress_reason.has_value()
                ? CanonicalValue(
                      *certificate.requested_selector.malformed_ingress_reason)
                : CanonicalValue(nullptr)},
           {"requested_selector_digest",
            CanonicalValue(certificate.requested_selector
                               .requested_selector_digest.value())},
       })},
      {"resolution_disposition", CanonicalValue(std::string(core::to_string(
                                     certificate.resolution_disposition)))},
      {"resolution_reason", CanonicalValue(std::string(core::to_string(
                                certificate.resolution_reason)))},
      {"result", CanonicalValue(std::string(
                     StopCertificateResultName(certificate.result)))},
      {"scope_entry_count",
       CanonicalValue(static_cast<double>(certificate.scope_entries.size()))},
      {"state", CanonicalValue(std::string(StopStatusStateName(status.state)))},
      {"stop_certificate_id",
       CanonicalValue(certificate.stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(
           static_cast<double>(certificate.stop_certificate_revision.value()))},
      {"stop_generation", CanonicalValue(static_cast<double>(
                              certificate.stop_generation.value()))},
      {"stop_id", CanonicalValue(certificate.stop_id.value())},
      {"terminal_error", certificate.terminal_error.has_value()
                             ? ErrorValue(*certificate.terminal_error)
                             : CanonicalValue(nullptr)},
      {"terminal_projection",
       StopProjectionStatusValue(status.terminal_projection)},
      {"unsettled", include_details
                        ? StopHistoryScopeKeyArray(certificate.unsettled)
                        : CanonicalValue(CanonicalValue::Array{})},
      {"unsettled_count",
       CanonicalValue(static_cast<double>(certificate.unsettled.size()))},
  });
}

[[nodiscard]] CanonicalValue StopFaultClearHistoryProjectionValue(
    const core::StopJournalFaultClearProjectionDTO &projection,
    const std::string_view source_owner_epoch) {
  const auto &result = projection.recovery_result;
  const auto &latch = result.latch;
  Require(
      result.accepted && !result.error.has_value() &&
          latch.state == core::StopFaultLatchStateDTO::kCleared &&
          latch.stop_certificate.has_value(),
      "durable Stop fault-clear projection is not a successful exact clear");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.stop_fault_clear_projection.v1")},
      {"fault_latch",
       StopHistoryFaultValue(std::optional<core::StopFaultLatchDTO>(latch))},
      {"owner_epoch", CanonicalValue(std::string(source_owner_epoch))},
      {"record_kind", CanonicalValue("stop_fault_cleared")},
      {"request_id", CanonicalValue(result.request_id.value())},
      {"result",
       CanonicalValue(CanonicalValue::Object{
           {"accepted", CanonicalValue(true)},
           {"error", CanonicalValue(nullptr)},
           {"replay_status",
            CanonicalValue(std::string(core::to_string(result.replay_status)))},
           {"request_id", CanonicalValue(result.request_id.value())},
       })},
      {"stop_certificate_id",
       CanonicalValue(latch.stop_certificate->stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(static_cast<double>(
           latch.stop_certificate->stop_certificate_revision.value()))},
      {"stop_generation",
       CanonicalValue(static_cast<double>(latch.stop_generation.value()))},
      {"stop_id", CanonicalValue(latch.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue BoundedStopTerminalHistoryProjectionValue(
    const core::StopJournalTerminalProjectionDTO &projection,
    const std::string_view source_owner_epoch,
    const std::string_view durable_projection_digest) {
  auto detailed = StopTerminalHistoryProjectionValue(
      projection, source_owner_epoch, durable_projection_digest, true);
  if (content_identity::canonical_json(detailed).size() <=
      kMaximumStopHistoryProjectionBytes) {
    return detailed;
  }
  auto bounded = StopTerminalHistoryProjectionValue(
      projection, source_owner_epoch, durable_projection_digest, false);
  Require(content_identity::canonical_json(bounded).size() <=
              kMaximumStopHistoryProjectionBytes,
          "bounded terminal Stop history projection exceeds its byte limit");
  return bounded;
}

[[nodiscard]] CanonicalValue
CellStopActionValue(const core::CellStopActionProjectionDTO &value) {
  CanonicalValue terminal(nullptr);
  if (value.latest_terminal.has_value()) {
    CanonicalValue::Array stages;
    stages.reserve(value.latest_terminal->executed_stages.size());
    for (const auto &stage : value.latest_terminal->executed_stages) {
      stages.emplace_back(CanonicalValue::Object{
          {"action_class",
           CanonicalValue(std::string(core::to_string(stage.action_class)))},
          {"executed_mode", stage.executed_mode.has_value()
                                ? CanonicalValue(std::string(
                                      core::to_string(*stage.executed_mode)))
                                : CanonicalValue(nullptr)},
          {"operation_id", CanonicalValue(stage.operation_id.value())},
          {"outcome",
           CanonicalValue(std::string(StopStageOutcomeName(stage.outcome)))},
          {"participant_id", CanonicalValue(stage.participant_id.value())},
          {"stage_id", CanonicalValue(stage.stage_id.value())},
          {"stage_index",
           CanonicalValue(static_cast<double>(stage.stage_index))},
          {"substitution_reason",
           stage.substitution_reason.has_value()
               ? CanonicalValue(*stage.substitution_reason)
               : CanonicalValue(nullptr)},
      });
    }
    terminal = CanonicalValue(CanonicalValue::Object{
        {"certificate",
         StopCertificateRefValue(value.latest_terminal->certificate)},
        {"escalated", CanonicalValue(value.latest_terminal->escalated)},
        {"executed_stages", CanonicalValue(std::move(stages))},
        {"hard_stop_floor_used",
         CanonicalValue(value.latest_terminal->hard_stop_floor_used)},
        {"result", CanonicalValue(std::string(StopCertificateResultName(
                       value.latest_terminal->result)))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"available", CanonicalValue(true)},
      {"expected_stop_generation",
       CanonicalValue(
           static_cast<double>(value.expected_stop_generation.value()))},
      {"fault",
       value.fault.has_value()
           ? CanonicalValue(CanonicalValue::Object{
                 {"certificate",
                  value.fault->certificate.has_value()
                      ? StopCertificateRefValue(*value.fault->certificate)
                      : CanonicalValue(nullptr)},
                 {"latch_generation",
                  CanonicalValue(static_cast<double>(
                      value.fault->latch_generation.value()))},
                 {"latch_id", CanonicalValue(value.fault->latch_id.value())},
                 {"reason_code", CanonicalValue(value.fault->reason_code)},
                 {"recovery_status",
                  CanonicalValue(std::string(StopFaultRecoveryStatusName(
                      value.fault->recovery_status)))},
                 {"state", CanonicalValue(std::string(
                               StopFaultLatchStateName(value.fault->state)))},
                 {"trigger", CanonicalValue(std::string(
                                 StopFaultTriggerName(value.fault->trigger)))},
             })
           : CanonicalValue(nullptr)},
      {"latest_terminal", std::move(terminal)},
      {"unavailable_reason", CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
RecoveryAssessmentValue(const core::KernelRecoveryAssessmentDTO &value) {
  CanonicalValue::Array actions;
  actions.reserve(value.allowed_actions.size());
  for (const auto action : value.allowed_actions) {
    actions.emplace_back(core::recovery_action_name(action));
  }
  CanonicalValue::Array evidence;
  evidence.reserve(value.evidence_refs.size());
  for (const auto &reference : value.evidence_refs) {
    evidence.emplace_back(reference);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"allowed_actions", CanonicalValue(std::move(actions))},
      {"assessed_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.assessed_at_ns)},
      {"assessment_revision",
       CanonicalValue(static_cast<double>(value.assessment_revision))},
      {"backend_stop_confirmed", CanonicalValue(value.backend_stop_confirmed)},
      {"control_command_epoch",
       CanonicalValue(static_cast<double>(value.control_command_epoch))},
      {"evidence_refs", CanonicalValue(std::move(evidence))},
      {"execution_id", CanonicalValue(static_cast<double>(value.execution_id))},
      {"execution_state",
       CanonicalValue(std::string(core::to_string(value.execution_state)))},
      {"latch_id", CanonicalValue(value.latch_id)},
      {"observed_state_at_rest", CanonicalValue(value.observed_state_at_rest)},
      {"observed_state_authority_profile",
       CanonicalValue(value.observed_state_authority_profile)},
      {"observed_state_available",
       CanonicalValue(value.observed_state_available)},
      {"observed_state_fresh", CanonicalValue(value.observed_state_fresh)},
      {"observed_state_measured",
       CanonicalValue(value.observed_state_measured)},
      {"observed_state_sequence",
       CanonicalValue(static_cast<double>(value.observed_state_sequence))},
      {"observed_state_source_id",
       CanonicalValue(value.observed_state_source_id)},
      {"observed_state_valid_until_ns",
       manyforge::content_identity::nonnegative_int64_value(
           value.observed_state_valid_until_ns)},
      {"policy_id", CanonicalValue(value.policy_id)},
      {"preemption_result",
       CanonicalValue(std::string(
           core::interrupt_preemption_result_name(value.preemption_result)))},
      {"queued_latch_present", CanonicalValue(value.queued_latch_present)},
      {"reason_code", CanonicalValue(value.reason_code)},
      {"recovery_class", CanonicalValue(std::string(
                             core::recovery_class_name(value.recovery_class)))},
      {"eligible_close", CanonicalValue(value.eligible_close)},
      {"retry_admissible", CanonicalValue(value.retry_admissible)},
      {"retry_eligibility",
       CanonicalValue(CanonicalValue::Object{
           {"cause", CanonicalValue(std::string(core::motion_retry_cause_name(
                         value.retry_eligibility.cause)))},
           {"effect", CanonicalValue(std::string(core::motion_retry_effect_name(
                          value.retry_eligibility.effect)))},
           {"eligible", CanonicalValue(value.retry_eligibility.eligible)},
           {"reason", CanonicalValue(std::string(
                          core::motion_retry_ineligibility_reason_name(
                              value.retry_eligibility.reason)))},
       })},
      {"schema_version", CanonicalValue(value.schema_version)},
      {"source_id", CanonicalValue(value.source_id)},
      {"status", CanonicalValue(std::string(
                     core::recovery_assessment_status_name(value.status)))},
      {"stop_certificate_id", CanonicalValue(value.stop_certificate_id)},
      {"stop_certificate_revision",
       CanonicalValue(static_cast<double>(value.stop_certificate_revision))},
      {"stop_owner_active", CanonicalValue(value.stop_owner_active)},
      {"stop_owner_generation",
       CanonicalValue(static_cast<double>(value.stop_owner_generation))},
      {"stronger_authority_active",
       CanonicalValue(value.stronger_authority_active)},
  });
}

[[nodiscard]] CanonicalValue RecoveryParticipantsValue(
    const std::vector<core::OperationRecoveryParticipantDTO> &values) {
  CanonicalValue::Array participants;
  participants.reserve(values.size());
  for (const auto &value : values) {
    CanonicalValue::Array resources;
    resources.reserve(value.resource_ids.size());
    for (const auto &resource : value.resource_ids) {
      resources.emplace_back(resource.value());
    }
    participants.emplace_back(CanonicalValue::Object{
        {"active_control_configuration_generation",
         value.active_control_configuration_generation
             ? CanonicalValue(static_cast<double>(
                   value.active_control_configuration_generation->value()))
             : CanonicalValue(nullptr)},
        {"active_control_configuration_id",
         value.active_control_configuration_id
             ? CanonicalValue(value.active_control_configuration_id->value())
             : CanonicalValue(nullptr)},
        {"capability_id", CanonicalValue(value.capability_id.value())},
        {"capability_provider_binding_id",
         value.capability_provider_binding_id.has_value()
             ? CanonicalValue(value.capability_provider_binding_id->value())
             : CanonicalValue(nullptr)},
        {"command_target_id", CanonicalValue(value.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(value.controllable_component_id.value())},
        {"control_binding_id",
         value.control_binding_id
             ? CanonicalValue(value.control_binding_id->value())
             : CanonicalValue(nullptr)},
        {"control_domain_id", CanonicalValue(value.control_domain_id.value())},
        {"control_route_id",
         value.control_route_id
             ? CanonicalValue(value.control_route_id->value())
             : CanonicalValue(nullptr)},
        {"operation_participant_id",
         CanonicalValue(value.operation_participant_id.value())},
        {"resource_ids", CanonicalValue(std::move(resources))},
    });
  }
  return CanonicalValue(std::move(participants));
}

[[nodiscard]] CanonicalValue
RecoveryActionResultValue(const core::KernelRecoveryActionResultDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted", CanonicalValue(value.accepted)},
      {"action",
       CanonicalValue(std::string(core::recovery_action_name(value.action)))},
      {"error", value.error.has_value() ? ErrorValue(*value.error)
                                        : CanonicalValue(nullptr)},
      {"request_id", CanonicalValue(value.request_id)},
      {"resulting_assessment",
       RecoveryAssessmentValue(value.resulting_assessment)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlSelectorValue(
    const core::RuntimeScaleControlSelectorDTO &value) {
  if (value.group_targets.empty()) {
    return CanonicalValue(CanonicalValue::Object{
        {"component_role_id", CanonicalValue(value.component_role_id.value())},
        {"program_binding_id",
         CanonicalValue(value.program_binding_id.value())},
        {"target_role_id", CanonicalValue(value.target_role_id.value())},
    });
  }
  CanonicalValue::Array targets;
  targets.reserve(value.group_targets.size());
  for (const auto &target : value.group_targets) {
    targets.emplace_back(CanonicalValue::Object{
        {"component_role_id", CanonicalValue(target.component_role_id.value())},
        {"target_role_id", CanonicalValue(target.target_role_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"group_targets", CanonicalValue(std::move(targets))},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlAcceptedValue(
    const core::RuntimeScaleControlAcceptedDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"active_operation_id",
       value.active_operation_id.has_value()
           ? CanonicalValue(value.active_operation_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_revision",
       value.active_operation_revision.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.active_operation_revision->value()))
           : CanonicalValue(nullptr)},
      {"active_operation_scale_generation",
       value.active_operation_scale_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 value.active_operation_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"control_generation",
       CanonicalValue(static_cast<double>(value.control_generation.value()))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"requested_scale", CanonicalValue(value.requested_scale)},
      {"selector", RuntimeScaleControlSelectorValue(value.selector)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlStatusValue(
    const core::RuntimeScaleControlStatusDTO &value) {
  CanonicalValue::Array group_members;
  group_members.reserve(value.group_members.size());
  for (const auto &member : value.group_members) {
    group_members.emplace_back(CanonicalValue::Object{
        {"active_control_configuration_generation",
         CanonicalValue(static_cast<double>(
             member.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(member.active_control_configuration_id.value())},
        {"control_binding_id",
         CanonicalValue(member.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(member.control_binding_revision.value())},
        {"control_domain_id", CanonicalValue(member.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(member.control_domain_revision.value())},
        {"scaling_domain_id", CanonicalValue(member.scaling_domain_id.value())},
        {"component_role_id",
         CanonicalValue(member.target.component_role_id.value())},
        {"target_role_id",
         CanonicalValue(member.target.target_role_id.value())},
    });
  }
  CanonicalValue::Object object{
      {"active_control_configuration_generation",
       value.active_control_configuration_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 value.active_control_configuration_generation->value()))
           : CanonicalValue(nullptr)},
      {"active_control_configuration_id",
       value.active_control_configuration_id.has_value()
           ? CanonicalValue(value.active_control_configuration_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_id",
       value.active_operation_id.has_value()
           ? CanonicalValue(value.active_operation_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_revision",
       value.active_operation_revision.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.active_operation_revision->value()))
           : CanonicalValue(nullptr)},
      {"active_operation_scale_generation",
       value.active_operation_scale_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 value.active_operation_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"application_state", value.application_state.has_value()
                                ? CanonicalValue(std::string(core::to_string(
                                      *value.application_state)))
                                : CanonicalValue(nullptr)},
      {"control_binding_id",
       value.control_binding_id.has_value()
           ? CanonicalValue(value.control_binding_id->value())
           : CanonicalValue(nullptr)},
      {"control_binding_revision",
       value.control_binding_revision.has_value()
           ? CanonicalValue(value.control_binding_revision->value())
           : CanonicalValue(nullptr)},
      {"control_domain_id",
       value.control_domain_id.has_value()
           ? CanonicalValue(value.control_domain_id->value())
           : CanonicalValue(nullptr)},
      {"control_domain_revision",
       value.control_domain_revision.has_value()
           ? CanonicalValue(value.control_domain_revision->value())
           : CanonicalValue(nullptr)},
      {"control_generation",
       CanonicalValue(static_cast<double>(value.control_generation.value()))},
      {"effective_scale", value.effective_scale.has_value()
                              ? CanonicalValue(*value.effective_scale)
                              : CanonicalValue(nullptr)},
      {"frozen", CanonicalValue(value.frozen)},
      {"reason", value.reason.has_value() ? ErrorValue(*value.reason)
                                          : CanonicalValue(nullptr)},
      {"requested_scale", CanonicalValue(value.requested_scale)},
      {"scaling_domain_id",
       value.scaling_domain_id.has_value()
           ? CanonicalValue(value.scaling_domain_id->value())
           : CanonicalValue(nullptr)},
      {"selector", RuntimeScaleControlSelectorValue(value.selector)},
      {"supported", CanonicalValue(value.supported)},
  };
  if (!group_members.empty()) {
    object.emplace_back("group_members",
                        CanonicalValue(std::move(group_members)));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue
OperationStateValue(const core::OperationStateDTO &value) {
  CanonicalValue::Array participants;
  participants.reserve(value.participants.size());
  for (const auto &participant : value.participants) {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    // Durable operation details remain authoritative. This status-observed
    // notice is best-effort; one last key suppresses consecutive repeats only.
    if (participant.terminal_error &&
        participant.terminal_error->details.contains("runtime_collision_reason")) {
      try {
        const auto notice = content_identity::canonical_json(
            CanonicalValue(CanonicalValue::Object{
                {"operation_id", CanonicalValue(value.operation_id.value())},
                {"operation_revision", CanonicalValue(static_cast<double>(
                                           value.operation_revision.value()))},
                {"participant_id", CanonicalValue(participant.participant_id.value())},
                {"error", ErrorValue(*participant.terminal_error)},
            }));
        const auto key = content_identity::sha256_digest(notice);
        static std::mutex notice_mutex;
        static std::string last_notice_key;
        std::unique_lock lock(notice_mutex, std::try_to_lock);
        if (lock.owns_lock() && last_notice_key != key) {
          last_notice_key = key;
          // Keep identity and decisive cause before context: enqueue caps the line.
          host_controller_launch_supervisor().enqueue_host_log(
              "ERROR manyforge runtime collision: operation=" + value.operation_id.value() +
              " revision=" + std::to_string(value.operation_revision.value()) +
              " participant=" + participant.participant_id.value() +
              " reason=" + participant.terminal_error->details.at("runtime_collision_reason") +
              " context=" + notice);
        }
      } catch (...) {
        // Diagnostic formatting/output never changes a status response.
      }
    }
#endif
    CanonicalValue::Array resources;
    resources.reserve(participant.resources.size());
    for (const auto &resource : participant.resources) {
      resources.emplace_back(resource.value());
    }
    const auto *controller = participant.binding_identity.controller();
    const auto *provider = participant.binding_identity.capability_provider();
    if (participant.output_evidence)
      Require(!core::to_string(*participant.output_evidence).empty() &&
                  participant.payload_kind ==
                      core::OperationParticipantPayloadKind::kCapabilityCommand,
              "I/O output evidence is invalid");
    CanonicalValue::Object participant_value{
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"capability_provider_binding_id",
         provider != nullptr
             ? CanonicalValue(provider->capability_provider_binding_id.value())
             : CanonicalValue(nullptr)},
        {"capability_provider_binding_revision",
         provider != nullptr
             ? CanonicalValue(
                   provider->capability_provider_binding_revision.value())
             : CanonicalValue(nullptr)},
        {"command_target_id",
         CanonicalValue(participant.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"control_binding_id",
         controller ? CanonicalValue(controller->control_binding_id.value())
                    : CanonicalValue(nullptr)},
        {"control_binding_revision",
         controller
             ? CanonicalValue(controller->control_binding_revision.value())
             : CanonicalValue(nullptr)},
        {"control_domain_id",
         CanonicalValue(
             participant.binding_identity.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(
             participant.binding_identity.control_domain_revision.value())},
        {"command_generation",
         CanonicalValue(static_cast<double>(
             participant.command_generation.value()))},
        {"command_id",
         CanonicalValue(participant.command_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"payload_kind",
         CanonicalValue(participant.payload_kind ==
                                core::OperationParticipantPayloadKind::kMotion
                            ? "motion"
                            : "capability_command")},
        {"progress", CanonicalValue(participant.progress)},
        {"resources", CanonicalValue(std::move(resources))},
        {"state", CanonicalValue(std::string(StateName(participant.state)))},
        {"terminal_error", participant.terminal_error.has_value()
                               ? ErrorValue(*participant.terminal_error)
                               : CanonicalValue(nullptr)},
    };
    if (participant.terminal_fact) {
      const auto &fact = *participant.terminal_fact;
      participant_value.emplace_back(
          "terminal_fact",
          CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue(core::to_string(fact.kind))},
              {"effect", CanonicalValue(core::to_string(fact.effect))},
              {"error", fact.error ? ErrorValue(*fact.error)
                                   : CanonicalValue(nullptr)}}));
    }
    if (participant.execution_scope) {
      participant_value.emplace_back(
          "execution_scope",
          CanonicalValue(CanonicalValue::Object{
              {"node_instance_id",
               CanonicalValue(
                   participant.execution_scope->node_instance_id.value())},
              {"node_attempt_id",
               CanonicalValue(participant.execution_scope->node_attempt_id)}}));
    }
    if (!participant.io_rule_outcomes.empty()) {
      const auto qualified = core::ValidateIoRuleOutcomeParticipant(
          value.operation_id, value.operation_revision, participant);
      Require(qualified.ok(), "I/O rule outcome relabels its containing participant");
      const auto encoded = core::EncodeIoRuleOutcomes(participant.io_rule_outcomes);
      Require(encoded.ok(), "I/O rule outcome evidence is invalid");
      participant_value.emplace_back("io_rule_outcomes",
          content_identity::parse_canonical_json(encoded.value()));
    }
    if (participant.output_evidence)
      participant_value.emplace_back("output_evidence",
                                     CanonicalValue(std::string(core::to_string(
                                         *participant.output_evidence))));
    participants.emplace_back(std::move(participant_value));
  }
  CanonicalValue::Object operation_value{
      {"active_stop_generation",
       value.active_stop_generation.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.active_stop_generation->value()))
           : CanonicalValue(nullptr)},
      {"active_stop_id", value.active_stop_id.has_value()
                             ? CanonicalValue(value.active_stop_id->value())
                             : CanonicalValue(nullptr)},
      {"coordination_policy",
       CanonicalValue(value.coordination_policy ==
                              core::OperationCoordinationPolicy::kCoordinatedAll
                          ? "coordinated_all"
                          : "independent")},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision",
       CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"runtime_scale_generation",
       value.runtime_scale_generation.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.runtime_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(StateName(value.state)))},
      {"updated_at_ns", manyforge::content_identity::nonnegative_int64_value(
                            value.updated_at_ns)},
  };
  if (value.preemption) {
    const auto& fact = *value.preemption;
    operation_value.emplace_back("preemption", CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue(std::string(core::to_string(fact.kind)))},
        {"by", CanonicalValue(CanonicalValue::Object{
            {"operation_id", CanonicalValue(fact.by.operation_id.value())},
            {"node_instance_id", CanonicalValue(fact.by.node_instance_id.value())},
            {"node_attempt_id", CanonicalValue(fact.by.node_attempt_id)}})}}));
  }
  if (value.motion_takeover) {
    const auto &decision = *value.motion_takeover;
    operation_value.emplace_back(
        "motion_takeover",
        CanonicalValue(CanonicalValue::Object{
            {"incumbent_operation_id",
             CanonicalValue(decision.incumbent_operation_id.value())},
            {"path", CanonicalValue(std::string(core::to_string(decision.path)))},
            {"cause", decision.cause ? ErrorValue(*decision.cause)
                                     : CanonicalValue(nullptr)},
            {"decided_at_ns", content_identity::nonnegative_int64_value(
                                  decision.decided_at_ns)},
            {"elapsed_since_acceptance_ns",
             content_identity::nonnegative_int64_value(
                 decision.elapsed_since_acceptance_ns)},
            {"preparation_budget_ns", content_identity::nonnegative_int64_value(
                                          decision.preparation_budget_ns)},
            {"stop_certificate",
             decision.stop_certificate
                 ? StopCertificateRefValue(*decision.stop_certificate)
                 : CanonicalValue(nullptr)},
        }));
  }
  return CanonicalValue(std::move(operation_value));
}

[[nodiscard]] CanonicalValue RuntimeFaultsValue(
    const core::ResourceAddressedExecutionKernel &owner) {
  const auto journal_fault = owner.get_operation_journal_write_fault();
  const auto runner_loss = owner.get_program_runner_loss();
  CanonicalValue journal_value(nullptr);
  if (journal_fault) {
    const auto &fault = *journal_fault;
    journal_value = CanonicalValue(CanonicalValue::Object{
        {"request_owner_epoch", CanonicalValue(fault.request_owner_epoch.value())},
        {"write_kind", CanonicalValue(fault.write_kind)},
        {"error", ErrorValue(fault.error)},
        {"operation_id", fault.operation_id
             ? CanonicalValue(fault.operation_id->value()) : CanonicalValue(nullptr)},
        {"operation_revision", fault.operation_revision
             ? CanonicalValue(static_cast<double>(fault.operation_revision->value()))
             : CanonicalValue(nullptr)},
        {"request_id", fault.request_id
             ? CanonicalValue(fault.request_id->value()) : CanonicalValue(nullptr)},
        {"program_run_id", fault.program_run_id
             ? CanonicalValue(fault.program_run_id->value()) : CanonicalValue(nullptr)},
        {"execution_scope", fault.execution_scope
             ? CanonicalValue(CanonicalValue::Object{
                   {"node_instance_id", CanonicalValue(fault.execution_scope->node_instance_id.value())},
                   {"node_attempt_id", CanonicalValue(fault.execution_scope->node_attempt_id)}})
             : CanonicalValue(nullptr)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"operation_journal_write_fault", std::move(journal_value)},
      {"program_runner_loss", runner_loss
           ? detail::ProgramRunnerLossValue(*runner_loss, owner.request_owner_epoch())
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
OperationEventPageValue(const core::OperationJournalEventPageDTO &page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto &event : page.events) {
    CanonicalValue payload;
    std::string payload_digest = event.payload_digest;
    if (event.record_kind == "stop_terminal_projected") {
      const auto terminal =
          core::DecodeStopJournalTerminalProjection(event.payload_json);
      Require(terminal.ok(),
              "durable terminal Stop projection could not be decoded: " +
                  (terminal.ok() ? std::string{} : terminal.error().message));
      payload = BoundedStopTerminalHistoryProjectionValue(
          terminal.value(), event.owner_epoch.value(), event.payload_digest);
      payload_digest = content_identity::sha256_digest(
          content_identity::canonical_json(payload));
    } else if (event.record_kind == "stop_fault_cleared") {
      const auto cleared =
          core::DecodeStopJournalFaultClearProjection(event.payload_json);
      Require(cleared.ok(),
              "durable Stop fault-clear projection could not be decoded: " +
                  (cleared.ok() ? std::string{} : cleared.error().message));
      payload = StopFaultClearHistoryProjectionValue(cleared.value(),
                                                     event.owner_epoch.value());
      payload_digest = content_identity::sha256_digest(
          content_identity::canonical_json(payload));
    } else {
      try {
        payload =
            deployment::parse_strict_json_model_yaml(event.payload_json).root();
      } catch (const std::exception &error) {
        Fail("durable operation event payload is not strict JSON: " +
             std::string(error.what()));
      }
      Require(content_identity::canonical_json(payload) == event.payload_json,
              "durable operation event payload is not canonical JSON");
    }
    events.emplace_back(CanonicalValue::Object{
        {"effect_pending", CanonicalValue(event.effect_pending)},
        {"occurred_at_ns", manyforge::content_identity::nonnegative_int64_value(
                               event.occurred_at_ns)},
        {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
        {"operation_id", event.operation_id.has_value()
                             ? CanonicalValue(event.operation_id->value())
                             : CanonicalValue(nullptr)},
        {"operation_revision", event.operation_revision.has_value()
                                   ? CanonicalValue(static_cast<double>(
                                         event.operation_revision->value()))
                                   : CanonicalValue(nullptr)},
        {"owner_epoch", CanonicalValue(event.owner_epoch.value())},
        {"payload", std::move(payload)},
        {"payload_digest", CanonicalValue(std::move(payload_digest))},
        {"record_kind", CanonicalValue(event.record_kind)},
        {"request_id", event.request_id.has_value()
                           ? CanonicalValue(event.request_id->value())
                           : CanonicalValue(nullptr)},
        {"sequence", CanonicalValue(static_cast<double>(event.sequence))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"has_more", CanonicalValue(page.has_more)},
      {"next_after_sequence",
       CanonicalValue(static_cast<double>(page.next_after_sequence))},
      {"owner_epoch", CanonicalValue(page.owner_epoch.value())},
      {"schema_version", CanonicalValue("manyforge.execution_event_page.v1")},
  });
}

[[nodiscard]] CanonicalValue
CoordinatePositionValue(const core::CoordinatePositionDTO &position) {
  if (const auto *angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular_rad")},
        {"value_rad", CanonicalValue(angular->value_rad)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear_m")},
      {"value_m",
       CanonicalValue(std::get<core::LinearPositionDTO>(position).value_m)},
  });
}
namespace {

std::string_view IoQuality(core::IoSignalAvailability value) {
  switch (value) {
    case core::IoSignalAvailability::kMissing: return "missing";
    case core::IoSignalAvailability::kFresh: return "fresh";
    case core::IoSignalAvailability::kStale: return "stale";
    case core::IoSignalAvailability::kInvalid: return "invalid";
    case core::IoSignalAvailability::kUnavailable: return "unavailable";
  }
  Fail("I/O signal availability is invalid");
}

CanonicalValue IoMatchedSampleValue(const core::IoSignalSampleDTO& sample) {
  const auto& publication = sample.publication;
  Require(publication.validity == core::IoSampleValidity::kValid &&
              publication.value.has_value(), "I/O watch matched sample is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue(publication.signal_id.controllable_component_id.value())},
      {"signal_name", CanonicalValue(publication.signal_id.name)},
      {"source_id", CanonicalValue(publication.source_id.value())},
      {"source_epoch", CanonicalValue(publication.incarnation.value())},
      {"source_sequence", CanonicalValue(std::to_string(publication.source_sequence))},
      {"value", CanonicalValue(*publication.value)},
      {"received_at_ns", content_identity::nonnegative_int64_value(sample.received_at_ns)}});
}

}  // namespace

CanonicalValue IoSourceCursorValue(const core::IoSourceCursorDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"owner_epoch", CanonicalValue(value.owner_epoch.value())},
      {"catalog_generation",
       CanonicalValue(std::to_string(value.catalog_generation))},
      {"source_id", CanonicalValue(value.source_id.value())},
      {"record_sequence",
       CanonicalValue(std::to_string(value.record_sequence))}});
}
CanonicalValue IoSignalObservationValue(const core::IoSignalObservationDTO& value) {
  const auto& sample = value.latest_sample;
  const auto error = value.source_error.has_value()
      ? value.source_error : sample ? sample->publication.error : std::nullopt;
  CanonicalValue liveness(nullptr);
  if (value.descriptor.liveness) {
    const auto mode = *value.descriptor.liveness;
    const char *name = nullptr;
    switch (mode) {
    case core::IoSignalLivenessMode::kInProcess:
      name = "in_process";
      break;
    case core::IoSignalLivenessMode::kPeriodic:
      name = "periodic";
      break;
    case core::IoSignalLivenessMode::kChangeOnly:
      name = "change_only";
      break;
    default:
      throw std::invalid_argument("Invalid I/O liveness descriptor");
    }
    liveness = CanonicalValue(CanonicalValue::Object{
        {"mode", CanonicalValue(name)},
        {"warning", mode == core::IoSignalLivenessMode::kChangeOnly
                        ? CanonicalValue("liveness_unproven")
                        : CanonicalValue(nullptr)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"liveness", std::move(liveness)},
      {"signal_name", CanonicalValue(value.descriptor.signal_id.name)},
      {"value_type", CanonicalValue("bool")},
      {"value", sample && sample->publication.value.has_value()
                    ? CanonicalValue(*sample->publication.value)
                    : CanonicalValue(nullptr)},
      {"quality", CanonicalValue(std::string(IoQuality(value.availability)))},
      {"source_id", CanonicalValue(value.descriptor.source_id.value())},
      {"source_epoch",
       value.installed_incarnation
           ? CanonicalValue(value.installed_incarnation->value())
           : CanonicalValue(nullptr)},
      {"source_sequence", sample ? CanonicalValue(std::to_string(
                                       sample->publication.source_sequence))
                                 : CanonicalValue(nullptr)},
      {"receipt_age_ns",
       sample && value.evaluated_at_ns >= sample->received_at_ns
           ? content_identity::nonnegative_int64_value(value.evaluated_at_ns -
                                                       sample->received_at_ns)
           : CanonicalValue(nullptr)},
      {"continuity_loss_generation",
       CanonicalValue(std::to_string(value.continuity_loss_generation))},
      {"error", error ? ErrorValue(*error) : CanonicalValue(nullptr)}});
}

CanonicalValue IoWatchRegistrationValue(const core::IoWatchRegistrationDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"owner_epoch", CanonicalValue(value.owner_epoch.value())},
      {"catalog_generation", CanonicalValue(std::to_string(value.catalog_generation))},
      {"registration_id", CanonicalValue(std::to_string(value.registration_id))},
      {"scope", CanonicalValue(CanonicalValue::Object{
          {"program_runner_owner_epoch", CanonicalValue(value.scope.program_runner_owner_epoch.value())},
          {"program_run_id", CanonicalValue(value.scope.program_run_id.value())},
          {"node", CanonicalValue(CanonicalValue::Object{
              {"node_instance_id", CanonicalValue(value.scope.node.node_instance_id.value())},
              {"node_attempt_id", CanonicalValue(value.scope.node.node_attempt_id)}})}})}});
}

CanonicalValue IoWatchResultValue(const core::IoWatchResultDTO& value) {
  const auto state = [&]() -> std::string_view {
    switch (value.state) {
      case core::IoWatchState::kPending: return "pending";
      case core::IoWatchState::kMatched: return "matched";
      case core::IoWatchState::kReleased: return "released";
    }
    Fail("I/O watch state is invalid");
  }();
  return CanonicalValue(CanonicalValue::Object{
      {"state", CanonicalValue(std::string(state))},
      {"observation", value.observation ? IoSignalObservationValue(*value.observation)
                                        : CanonicalValue(nullptr)},
      {"matched_sample", value.matched_sample ? IoMatchedSampleValue(*value.matched_sample)
                                              : CanonicalValue(nullptr)}});
}

CanonicalValue::Object InterruptRuleEventMetadataFields(
    const std::optional<core::IoRuleStateProjectionDTO> &actual,
    const core::RequestOwnerEpoch &owner_epoch) {
  if (!actual) return {{"interrupt_rule_catalog", CanonicalValue(nullptr)},
                       {"interrupt_rule_state_baseline", CanonicalValue(nullptr)}};
  const auto applied_fields = [&](const core::IoRuleAppliedContextDTO &context,
                                  std::string_view status) {
    const auto identity = [](const auto &id) {
      return !id.empty() && id.value().find('\0') == std::string::npos;
    };
    Require(identity(context.owner_epoch) && context.owner_epoch == owner_epoch &&
                identity(context.retained_bundle_id) &&
                identity(context.graph_revision) &&
                context.active_publication_generation.nonzero() &&
                context.active_publication_generation.value() <=
                    9'007'199'254'740'991ULL,
            "rule metadata has no exact successful Applied context");
    return CanonicalValue::Object{
        {"status", CanonicalValue(std::string(status))},
        {"owner_epoch", CanonicalValue(context.owner_epoch.value())},
        {"active_publication_generation",
         CanonicalValue(static_cast<double>(
             context.active_publication_generation.value()))},
        {"bundle", CanonicalValue(context.retained_bundle_id.value())},
        {"graph", CanonicalValue(context.graph_revision.value())}};
  };
  const auto rule_fields = [](const auto &rules, bool waiting_only) {
    CanonicalValue::Array ids;
    CanonicalValue::Array waiting;
    std::optional<core::InterruptPolicyId> previous;
    for (const auto &rule : rules) {
      Require(!rule.policy_id.empty() &&
                  rule.policy_id.value().find('\0') == std::string::npos &&
                  (!previous || *previous < rule.policy_id),
              "rule catalog IDs are not canonical");
      ids.emplace_back(rule.policy_id.value());
      previous = rule.policy_id;
      Require(!waiting_only ||
                  (!rule.waiting_for_program_keys.empty() &&
                   rule.required_signal_sources.empty()),
              "no-service waiting rule has an actual signal dependency");
      if (rule.waiting_for_program_keys.empty())
        continue;
      Require(!rule.condition, "waiting rule has a checked condition");
      CanonicalValue::Array keys;
      std::string previous_key;
      for (const auto &key : rule.waiting_for_program_keys) {
        Require(!key.empty() && key.find('\0') == std::string::npos &&
                    previous_key < key,
                "waiting rule keys are not canonical");
        keys.emplace_back(key);
        previous_key = key;
      }
      waiting.emplace_back(CanonicalValue::Object{
          {"rule_id", CanonicalValue(rule.policy_id.value())},
          {"keys", CanonicalValue(std::move(keys))}});
    }
    CanonicalValue::Object fields{
        {"rule_ids", CanonicalValue(std::move(ids))}};
    if (!waiting.empty())
      fields.emplace_back("waiting_for_program", CanonicalValue(std::move(waiting)));
    return fields;
  };
  if (actual->waiting_for_program) {
    const auto &waiting = *actual->waiting_for_program;
    Require(actual->status == core::IoRuleStateProjectionStatusDTO::kPending &&
                !actual->catalog && !actual->baseline &&
                !actual->verified_empty && !waiting.rules.empty(),
            "waiting rule projection has an incompatible state snapshot");
    auto fields = applied_fields(waiting.context, "waiting_for_program");
    for (auto &field : rule_fields(waiting.rules, true))
      fields.push_back(std::move(field));
    return {{"interrupt_rule_catalog", CanonicalValue(std::move(fields))},
            {"interrupt_rule_state_baseline",
             CanonicalValue(CanonicalValue::Object{
                 {"status", CanonicalValue("projection_pending")}})}};
  }
  if (actual->verified_empty) {
    const auto &context = *actual->verified_empty;
    Require((actual->status == core::IoRuleStateProjectionStatusDTO::kReady ||
             actual->status == core::IoRuleStateProjectionStatusDTO::kPending) &&
                !actual->catalog && !actual->baseline,
            "verified-empty rule projection has no exact successful Applied "
            "context");
    return {{"interrupt_rule_catalog",
             CanonicalValue(applied_fields(context, "verified_empty"))},
            {"interrupt_rule_state_baseline",
             actual->status == core::IoRuleStateProjectionStatusDTO::kPending
                 ? CanonicalValue(CanonicalValue::Object{
                       {"status", CanonicalValue("projection_pending")}})
                 : CanonicalValue(nullptr)}};
  }
  Require(actual->catalog != nullptr, "rule projection has no actual catalog");
  const auto &catalog = *actual->catalog;
  const auto &context = catalog.context;
  Require(!core::io_rule_state_context_field_error(context) && context.owner_epoch == owner_epoch,
          "rule catalog context differs from the actual event owner");
  auto rules = rule_fields(catalog.rules, false);
  const bool waiting = std::any_of(
      catalog.rules.begin(), catalog.rules.end(),
      [](const auto &rule) { return !rule.waiting_for_program_keys.empty(); });
  Require(!waiting ||
              (!catalog.program_binding_id &&
               actual->status == core::IoRuleStateProjectionStatusDTO::kPending),
          "waiting rules have an Applied Program or a fabricated ready cut");
  CanonicalValue::Array keys;
  std::string previous_key;
  for (const auto &key : catalog.referenced_program_blackboard_keys) {
    Require(!key.empty() && key.find('\0') == std::string::npos && previous_key < key,
            "rule catalog key union is not canonical");
    keys.emplace_back(key);
    previous_key = key;
  }
  auto catalog_fields = GlobalRuleContextValue(context).as_object();
  for (auto &field : rules)
    catalog_fields.push_back(std::move(field));
  catalog_fields.emplace_back("referenced_program_blackboard_keys", CanonicalValue(std::move(keys)));
  CanonicalValue metadata(std::move(catalog_fields));
  CanonicalValue baseline(nullptr);
  if (actual->status == core::IoRuleStateProjectionStatusDTO::kPending) {
    Require(!actual->baseline, "pending rule projection has a ready baseline");
    baseline = CanonicalValue(CanonicalValue::Object{{"status", CanonicalValue("projection_pending")}});
  } else {
    Require(actual->status == core::IoRuleStateProjectionStatusDTO::kReady && actual->baseline &&
                actual->baseline->context == context &&
                !core::io_rule_state_baseline_field_error(*actual->baseline),
            "ready rule baseline has no exact same-catalog cut");
    const auto &snapshot = *actual->baseline;
    Require(snapshot.states.size() == catalog.rules.size(), "rule baseline omits actual catalog rows");
    CanonicalValue::Array states;
    for (std::size_t index = 0; index < snapshot.states.size(); ++index) {
      const auto &row = snapshot.states[index];
      Require(row.rule_id == catalog.rules[index].policy_id, "rule baseline relabels catalog membership");
      states.emplace_back(GlobalRuleStateValue(context, row));
    }
    baseline = CanonicalValue(CanonicalValue::Object{
        {"status", CanonicalValue("ready")},
        {"snapshot", CanonicalValue(CanonicalValue::Object{
            {"owner_epoch", CanonicalValue(context.owner_epoch.value())},
            {"catalog_generation", CanonicalValue(std::to_string(context.catalog_generation))},
            {"active_publication_generation", CanonicalValue(static_cast<double>(context.active_publication_generation.value()))},
            {"retained_bundle_id", CanonicalValue(context.retained_bundle_id.value())},
            {"graph_revision", CanonicalValue(context.graph_revision.value())},
            {"through_sequence", CanonicalValue(static_cast<double>(snapshot.through_sequence))},
            {"states", CanonicalValue(std::move(states))}})}});
  }
  return {{"interrupt_rule_catalog", std::move(metadata)},
          {"interrupt_rule_state_baseline", std::move(baseline)}};
}

} // namespace manyforge::roboplan_backend::host_detail
