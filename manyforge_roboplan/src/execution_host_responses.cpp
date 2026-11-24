#include <manyforge_core/io_rule_outcome_codec.hpp>
#include "execution_host_responses.hpp"
#include "execution_host_posix.hpp"
#include "cell_admission_command_codec.hpp"
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/types/operation_member_id_json.hpp>
#include <manyforge_core/types/typed_error.hpp>
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
#include <manyforge_roboplan/controller_launch_process.hpp>
#endif
#include <iostream>
#include <manyforge_deployment/artifact_codec.hpp>
#include <mutex>
#include <set>
#include <cmath>
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

[[nodiscard]] CanonicalValue ErrorValue(const core::ErrorDTO &error,
                                        const std::string_view producer) {
  auto value = ErrorValue(error);
  auto object = value.as_object();
  object.emplace_back("producer", CanonicalValue(std::string(producer)));
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue
ErrorResponse(std::string code, std::string message,
              std::map<std::string, std::string> details) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error", ErrorValue(core::ErrorDTO{std::move(code), std::move(message),
                                          std::move(details)}, "kernel")},
      {"ok", CanonicalValue(false)},
  });
}

[[nodiscard]] core::ErrorDTO ExecutionCommandExceptionError(
    const std::exception &error) {
  core::ErrorDTO result{"execution_command_invalid", error.what(), {}};
  if (dynamic_cast<const core::TypedError *>(&error) != nullptr) {
    const auto cause = core::CauseOf(error, result.code, "kernel");
    result.details.emplace("cause_code", cause.error.code);
    result.details.emplace("cause_producer", cause.producer);
  }
  return result;
}

[[nodiscard]] CanonicalValue ExecutionOwnerNotReadyIdentityResponse(
    const core::ResourceAddressedExecutionKernel &owner) {
  const auto admission = owner.GetAuthoringAdmission();
  return CanonicalValue(CanonicalValue::Object{
      {"admission_generation", CanonicalValue(static_cast<double>(
                                   admission.admission_generation.value()))},
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error",
       ErrorValue(core::ErrorDTO{
           "execution_owner_not_ready",
           "the managed execution owner has no active physical product",
           {}}, "kernel")},
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

[[nodiscard]] CanonicalValue MotionPathPreviewValue(
    const core::MotionPathPreview &path,
    const core::RuntimeParticipantBindingDTO &binding,
    const std::string *diagnostic_reason) {
  Require(!path.joint_names.empty() && path.joint_names.size() <= 256U &&
              path.joint_names.size() == binding.resources.size() &&
              path.pairs.size() <= 64U && !path.samples.empty() &&
              path.samples.size() <= 32U,
          "motion path preview exceeds its shape limits");
  CanonicalValue::Array source_names;
  std::set<std::string> compiled_names, local_names;
  for (std::size_t i = 0; i < path.joint_names.size(); ++i) {
    const auto &resource = binding.resources[i];
    Require(resource.source_joint_name.has_value() &&
                !resource.source_joint_name->empty() &&
                path.joint_names[i] == resource.compiled_resource_name &&
                compiled_names.insert(path.joint_names[i]).second &&
                local_names.insert(*resource.source_joint_name).second,
            "motion path preview crosses its source-local binding");
    source_names.emplace_back(*resource.source_joint_name);
  }
  CanonicalValue::Array pairs;
  std::optional<std::pair<std::string, std::string>> previous_pair;
  for (const auto &pair : path.pairs) {
    const auto pair_id = std::make_pair(pair.body_a, pair.body_b);
    Require(!pair.body_a.empty() && !pair.body_b.empty() &&
                pair.body_a < pair.body_b &&
                (!previous_pair.has_value() || *previous_pair < pair_id),
            "motion path preview contains an invalid pair");
    previous_pair = pair_id;
    pairs.emplace_back(CanonicalValue::Object{
        {"body_a", CanonicalValue(pair.body_a)},
        {"body_b", CanonicalValue(pair.body_b)}});
  }
  CanonicalValue::Array samples;
  double previous_time = -1.0;
  for (const auto &sample : path.samples) {
    Require(std::isfinite(sample.time_from_start_s) &&
                sample.time_from_start_s >= 0.0 &&
                sample.time_from_start_s > previous_time &&
                sample.positions_rad.size() == path.joint_names.size(),
            "motion path preview contains an invalid sample");
    previous_time = sample.time_from_start_s;
    CanonicalValue::Array positions, contacts;
    for (const double position : sample.positions_rad) {
      Require(std::isfinite(position), "motion path preview contains a nonfinite position");
      positions.emplace_back(position);
    }
    std::set<std::size_t> seen;
    for (const auto &contact : sample.contacts) {
      Require(contact.pair_index < path.pairs.size() &&
                  seen.insert(contact.pair_index).second,
              "motion path preview contains an invalid contact");
      const char *disposition = nullptr;
      switch (contact.disposition) {
        case core::MotionPreviewContactDisposition::kAllowed: disposition = "allowed"; break;
        case core::MotionPreviewContactDisposition::kRefused: disposition = "refused"; break;
        case core::MotionPreviewContactDisposition::kUnknown: disposition = "unknown"; break;
      }
      Require(disposition != nullptr, "motion path preview contains an invalid disposition");
      contacts.emplace_back(CanonicalValue::Object{
          {"pair_index", CanonicalValue(static_cast<double>(contact.pair_index))},
          {"disposition", CanonicalValue(disposition)}});
    }
    const char *status = nullptr;
    switch (sample.status) {
      case core::MotionPreviewSampleStatus::kChecked: status = "checked"; break;
      case core::MotionPreviewSampleStatus::kUnknown: status = "unknown"; break;
    }
    Require(status != nullptr, "motion path preview contains an invalid status");
    CanonicalValue::Object sample_value{
        {"time_from_start_s", CanonicalValue(sample.time_from_start_s)},
        {"positions_rad", CanonicalValue(std::move(positions))},
        {"contacts", CanonicalValue(std::move(contacts))},
        {"status", CanonicalValue(status)}};
    if (diagnostic_reason != nullptr) {
      CanonicalValue::Array violations;
      std::set<std::size_t> seen_violations;
      for (const auto &violation : sample.clearance_violations) {
        Require(violation.pair_index < path.pairs.size() &&
                    seen_violations.insert(violation.pair_index).second &&
                    std::isfinite(violation.distance_m) &&
                    std::isfinite(violation.required_clearance_m) &&
                    violation.distance_m > 0.0 &&
                    violation.distance_m < violation.required_clearance_m,
                "motion path preview contains an invalid clearance violation");
        violations.emplace_back(CanonicalValue::Object{
            {"pair_index", CanonicalValue(static_cast<double>(violation.pair_index))},
            {"distance_m", CanonicalValue(violation.distance_m)},
            {"required_clearance_m", CanonicalValue(violation.required_clearance_m)}});
      }
      sample_value.emplace_back("clearance_violations", CanonicalValue(std::move(violations)));
    }
    samples.emplace_back(std::move(sample_value));
  }
  CanonicalValue::Object value{
      {"source_joint_names", CanonicalValue(std::move(source_names))},
      {"pairs", CanonicalValue(std::move(pairs))},
      {"samples", CanonicalValue(std::move(samples))}};
  if (diagnostic_reason != nullptr) {
    Require(!diagnostic_reason->empty() &&
                (path.diagnostic_kind == "validated_path" ||
                 path.diagnostic_kind == "collision_disabled_path" ||
                 path.diagnostic_kind == "collision_disabled_ik_pose" ||
                 path.diagnostic_kind == "collision_disabled_ik_path"),
            "motion path preview contains an invalid diagnostic identity");
    Require(path.diagnostic_kind != "collision_disabled_ik_pose" || path.samples.size() == 1U,
            "motion IK diagnostic must contain exactly one sample");
    Require(path.diagnostic_kind != "collision_disabled_ik_path" || path.samples.size() >= 2U,
            "motion IK path diagnostic must contain at least two samples");
    value.emplace_back("schema_version", CanonicalValue("manyforge.motion_preview_diagnostic.v2"));
    value.emplace_back("kind", CanonicalValue(path.diagnostic_kind));
    value.emplace_back("reason", CanonicalValue(*diagnostic_reason));
  }
  return CanonicalValue(std::move(value));
}

[[nodiscard]] CanonicalValue MotionPreviewReceiptValue(
    const core::MotionPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const core::RuntimeParticipantBindingDTO &direct_control_binding,
    const CanonicalValue &path_preview) {
  const auto &binding = direct_control_binding;
  const auto &final_state = preview.final_controlled_state;
  Require(preview.resolved_tcp.has_value(),
          "motion preview has no exact Active TCP capture");
  const auto &tcp = *preview.resolved_tcp;
  constexpr std::uint64_t kMaxExactJsonInteger = 9007199254740991ULL;
  Require(tcp.registration_generation > 0U &&
              tcp.registration_generation <= kMaxExactJsonInteger &&
              tcp.sequence <= kMaxExactJsonInteger,
          "motion preview Active TCP capture is not representable");
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
      {"final_positions_rad", CanonicalValue(std::move(final_positions))},
      {"final_tcp_pose", PoseValue(preview.final_tcp_pose)},
      {"graph_revision", CanonicalValue(binding.graph_revision.value())},
      {"intent_digest", CanonicalValue(intent_digest.value())},
      {"program_binding_id",
       CanonicalValue(binding.program_binding_id.value())},
      {"program_revision", CanonicalValue(binding.program_revision.value())},
      {"program_run_id", RunValue(intent.program_run_id)},
      {"request_key", CanonicalValue(RequestKeyText(intent.request_context.request_key))},
      {"required_capability_id",
       CanonicalValue(
           intent.participants.front().required_capability_id.value())},
      {"retained_bundle_id",
       CanonicalValue(binding.retained_bundle_id.value())},
      {"scene_revision",
       SceneRevisionValue(
           preview.projection_binding.projection_revision.scene_revision)},
      {"schema_version",
       CanonicalValue("manyforge.direct_control_motion_preview.v5")},
      {"resolved_tcp", CanonicalValue(CanonicalValue::Object{
          {"entry", CanonicalValue(CanonicalValue::Object{
              {"target", CanonicalValue(CanonicalValue::Object{
                  {"component_id", CanonicalValue(tcp.entry.target.component_id.value())},
                  {"command_target_id", CanonicalValue(tcp.entry.target.command_target_id.value())}})},
              {"frame", CanonicalValue(CanonicalValue::Object{
                  {"component_id", CanonicalValue(tcp.entry.frame.component_id.value())},
                  {"frame_id", CanonicalValue(tcp.entry.frame.frame_id.value())}})}})},
          {"source", CanonicalValue(
              tcp.source == core::PreviewTcpCaptureSource::kJointFlangeFallback ? "joint_flange_fallback" :
              tcp.source == core::PreviewTcpCaptureSource::kExplicitEntry ? "explicit_entry" : "active_tcp")},
          {"registration_generation", CanonicalValue(static_cast<double>(tcp.registration_generation))},
          {"sequence", CanonicalValue(static_cast<double>(tcp.sequence))}})},
      {"path_preview", path_preview},
      {"source_joint_names", CanonicalValue(std::move(source_joint_names))},
      {"start_positions_rad", CanonicalValue(std::move(start_positions))},
      {"target", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(binding.controllable_component_id.value())},
          {"command_target_id", CanonicalValue(binding.command_target_id.value())}})},
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
    const std::optional<RetainedGripperPositionLimits> &limits) {
  const auto &binding = preview.binding;
  Require(binding.resources.size() == 1U,
          "capability-command preview has no exact scalar resource");
  const auto &resource = binding.resources.front();
  const bool model_backed = resource.source_joint_name.has_value();
  Require(
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
      {"program_revision", CanonicalValue(binding.program_revision.value())},
      {"program_run_id", RunValue(intent.program_run_id)},
      {"request_key", CanonicalValue(RequestKeyText(intent.request_context.request_key))},
      {"required_capability_id",
       CanonicalValue(
           intent.participants.front().required_capability_id.value())},
      {"retained_bundle_id",
       CanonicalValue(binding.retained_bundle_id.value())},
      {"schema_version",
       CanonicalValue(
           "manyforge.direct_control_capability_command_preview.v3")},
      {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
      {"source_kind",
       CanonicalValue(model_backed ? "model_role" : "component_local")},
      {"target_position",
       CanonicalValue(preview.command_request.target_position)},
      {"target", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(binding.controllable_component_id.value())},
          {"command_target_id", CanonicalValue(binding.command_target_id.value())}})},
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
          CanonicalValue::Object pose{
              {"goal", PoseValue(value.goal)},
              {"kind", CanonicalValue("pose_goal")},
          };
          if (value.linear) {
            const auto &linear = *value.linear;
            pose.emplace_back("max_angular_acceleration_rad_s2",
                              CanonicalValue(linear.max_angular_acceleration_rad_s2));
            pose.emplace_back("max_angular_speed_rad_s",
                              CanonicalValue(linear.max_angular_speed_rad_s));
            pose.emplace_back("max_linear_acceleration_mps2",
                              CanonicalValue(linear.max_linear_acceleration_mps2));
            pose.emplace_back("max_rotation_step_rad",
                              CanonicalValue(linear.max_rotation_step_rad));
            pose.emplace_back("max_translation_step_m",
                              CanonicalValue(linear.max_translation_step_m));
            pose.emplace_back("motion_type", CanonicalValue("linear"));
            if (linear.speed_mode) {
              pose.emplace_back("speed_mode",
                                CanonicalValue(std::string(
                                    core::cartesian_speed_mode_name(
                                        *linear.speed_mode))));
            }
          }
          return CanonicalValue(std::move(pose));
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
          CanonicalValue::Array blend_radii;
          blend_radii.reserve(value.blend_radii_m.size());
          for (const double radius : value.blend_radii_m) {
            blend_radii.emplace_back(radius);
          }
          CanonicalValue::Object path{
              {"blend_radii_m", CanonicalValue(std::move(blend_radii))},
              {"fallback_policy",
               CanonicalValue(
                   value.fallback_policy ==
                           core::CartesianFallbackPolicyDTO::kFailClosed
                       ? "fail_closed"
                       : "allow_joint_space_fallback")},
              {"kind", CanonicalValue("cartesian_path")},
              {"max_angular_acceleration_rad_s2",
               CanonicalValue(value.max_angular_acceleration_rad_s2)},
              {"max_angular_speed_rad_s",
               CanonicalValue(value.max_angular_speed_rad_s)},
              {"max_linear_acceleration_mps2",
               CanonicalValue(value.max_linear_acceleration_mps2)},
              {"max_rotation_step_rad",
               CanonicalValue(value.max_rotation_step_rad)},
              {"max_translation_step_m",
               CanonicalValue(value.max_translation_step_m)},
              {"waypoints", CanonicalValue(std::move(waypoints))},
          };
          if (value.speed_mode) {
            path.emplace_back("speed_mode",
                              CanonicalValue(std::string(
                                  core::cartesian_speed_mode_name(
                                      *value.speed_mode))));
          }
          return CanonicalValue(std::move(path));
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
        {"participant_id", core::OperationParticipantIdValue(assignment.participant_id)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"operation_id", CanonicalValue(static_cast<double>(value.operation_id.value()))},
      {"operation_revision",
       CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participant_assignments", CanonicalValue(std::move(assignments))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_key", CanonicalValue(RequestKeyText(value.request_key))},
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
      {"operation_id", CanonicalValue(static_cast<double>(value.operation_id.value()))},
      {"operation_revision_after_acceptance",
       CanonicalValue(static_cast<double>(
           value.operation_revision_after_acceptance.value()))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_key", CanonicalValue(RequestKeyText(value.request_key))},
  });
}

[[nodiscard]] CanonicalValue
StopAcceptedValue(const core::StopAcceptedDTO &value) {
  CanonicalValue::Array covered;
  covered.reserve(value.covered.size());
  for (const auto &participant : value.covered) {
    covered.emplace_back(CanonicalValue::Object{
        {"operation_id",
         CanonicalValue(static_cast<double>(participant.operation_id.value()))},
        {"participant_ordinal",
         CanonicalValue(static_cast<double>(participant.participant_ordinal))},
        {"started", CanonicalValue(participant.started)},
    });
  }
  CanonicalValue::Array stale;
  stale.reserve(value.stale_diagnostics.size());
  for (const auto &diagnostic : value.stale_diagnostics) {
    stale.emplace_back(CanonicalValue::Object{
        {"current_operation_revision",
         CanonicalValue(static_cast<double>(
             diagnostic.current_operation_revision.value()))},
        {"expected_operation_revision",
         CanonicalValue(static_cast<double>(
             diagnostic.expected_operation_revision.value()))},
        {"operation_id",
         CanonicalValue(static_cast<double>(diagnostic.operation_id.value()))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"applied", CanonicalValue(std::string(core::to_string(value.applied)))},
      {"covered", CanonicalValue(std::move(covered))},
      {"event_id", CanonicalValue(static_cast<double>(value.event_id.value()))},
      {"hold_revision", CanonicalValue(static_cast<double>(value.hold_revision))},
      {"program_run_id", RunValue(value.program_run_id)},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_key", CanonicalValue(RequestKeyText(value.request_key))},
      {"requested",
       CanonicalValue(std::string(core::to_string(value.requested)))},
      {"stale_diagnostics", CanonicalValue(std::move(stale))},
      {"stale_total", CanonicalValue(static_cast<double>(value.stale_total))},
      {"widen_reason",
       CanonicalValue(std::string(core::to_string(value.widen_reason)))},
  });
}

[[nodiscard]] CanonicalValue
StopEventIdValue(const std::optional<core::StopEventId> &value) {
  return value ? CanonicalValue(static_cast<double>(value->value()))
               : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue
ProgramExecutionScopeValue(const core::ProgramExecutionScopeStateDTO &value) {
  // Group sparse node indices by state. Repeating field names for each of
  // 100,000 captured nodes exceeds the bounded native response envelope.
  CanonicalValue::Array ready, held, stopping, faulted;
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
  // OD-342: [held node index, requesting Program node index] pairs.
  CanonicalValue::Array program_held;
  for (const auto &hold : value.program_held_nodes)
    program_held.emplace_back(CanonicalValue(CanonicalValue::Array{
        CanonicalValue(static_cast<double>(hold.node_index)),
        CanonicalValue(static_cast<double>(hold.stopped_by_node_index))}));
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue(std::string(core::kProgramExecutionScopeSchema))},
      {"claim_generation",
       CanonicalValue(static_cast<double>(value.claim_generation.value()))},
      {"program_run_id", CanonicalValue(static_cast<double>(value.program_run_id.value()))},
      {"hold_revision", CanonicalValue(static_cast<double>(value.hold_revision))},
      {"ready_node_indices", CanonicalValue(std::move(ready))},
      {"held_node_indices", CanonicalValue(std::move(held))},
      {"stopping_node_indices", CanonicalValue(std::move(stopping))},
      {"faulted_node_indices", CanonicalValue(std::move(faulted))},
      {"selection_release_block_reason",
       value.selection_release_block_reason.has_value()
           ? CanonicalValue(std::string(
                 core::program_selection_release_block_reason_token(
                     *value.selection_release_block_reason)))
           : CanonicalValue(nullptr)},
      // A rule's `hold` held the run (stopped_by: rule).
      {"stopped_by", value.stopped_by_rule_id ? CanonicalValue("rule") : CanonicalValue(nullptr)},
      {"rule_id", value.stopped_by_rule_id ? CanonicalValue(*value.stopped_by_rule_id)
                                           : CanonicalValue(nullptr)},
      // Held nodes a Program node's own Stop holds (stopped_by: program).
      {"program_held_nodes", CanonicalValue(std::move(program_held))},
      {"frozen", CanonicalValue(std::string(core::to_string(value.frozen)))},
      {"frozen_by", StopEventIdValue(value.frozen_by)},
      {"all_groups_held", CanonicalValue(value.all_groups_held)}});
}

[[nodiscard]] CanonicalValue
ProgramRunHoldValue(const std::optional<core::ProgramRunHoldFactsDTO> &value) {
  if (!value) return CanonicalValue(nullptr);
  const auto &hold = *value;
  return CanonicalValue(CanonicalValue::Object{
      {"program_run_id", CanonicalValue(static_cast<double>(hold.program_run_id.value()))},
      {"hold_revision", CanonicalValue(static_cast<double>(hold.hold_revision))},
      {"frozen", CanonicalValue(std::string(core::to_string(hold.frozen)))},
      {"frozen_by", StopEventIdValue(hold.frozen_by)},
      {"all_groups_held", CanonicalValue(hold.all_groups_held)},
      {"held_by_rule", hold.held_by_rule ? CanonicalValue(hold.held_by_rule->value())
                                         : CanonicalValue(nullptr)},
      // The freezing event's requested kind and source label (null when not
      // frozen or its record is no longer resident).
      {"frozen_kind", hold.frozen_kind
                          ? CanonicalValue(std::string(core::to_string(*hold.frozen_kind)))
                          : CanonicalValue(nullptr)},
      {"frozen_source_label", hold.frozen_source_label
                                  ? CanonicalValue(*hold.frozen_source_label)
                                  : CanonicalValue(nullptr)}});
}

void AppendStopStreamFacts(CanonicalValue::Object &state,
                           const core::StopStreamFactsDTO &facts) {
  state.emplace_back("program_run_hold", ProgramRunHoldValue(facts.program_run_hold));
  state.emplace_back("last_stop_event_id", StopEventIdValue(facts.last_stop_event_id));
  state.emplace_back("stop_progress_sequence",
                     CanonicalValue(static_cast<double>(facts.stop_progress_sequence)));
}

namespace {

[[nodiscard]] CanonicalValue OptionalText(const std::optional<std::string> &text) {
  return text ? CanonicalValue(*text) : CanonicalValue(nullptr);
}

[[nodiscard]] std::string_view FactErrorName(const core::OperationTableFactError error) {
  switch (error) {
  case core::OperationTableFactError::kNone:
    return "none";
  case core::OperationTableFactError::kSameAsTerminalError:
    return "same";
  case core::OperationTableFactError::kDifferent:
    return "different";
  }
  return "different";
}

[[nodiscard]] CanonicalValue
OperationTableEntryValue(const core::OperationTableEntryDTO &entry) {
  CanonicalValue monitor_stop(nullptr);
  if (entry.runtime_monitor_stop) {
    const auto &stop = *entry.runtime_monitor_stop;
    monitor_stop = CanonicalValue(CanonicalValue::Object{
        {"reason", CanonicalValue(std::string(stop.reason.view()))},
        {"request_key", CanonicalValue(core::RequestKeyText(stop.request_key))},
        {"source_label", CanonicalValue(std::string(stop.source_label.view()))},
    });
  }
  CanonicalValue::Object value{
      {"complete", CanonicalValue(entry.complete)},
      {"operation_id", CanonicalValue(static_cast<double>(entry.operation_id.value()))},
      {"operation_revision",
       CanonicalValue(static_cast<double>(entry.operation_revision.value()))},
      {"pending_control",
       entry.pending_control
           ? CanonicalValue(std::string(ControlName(*entry.pending_control)))
           : CanonicalValue(nullptr)},
      {"runtime_monitor_stop", std::move(monitor_stop)},
      {"state", CanonicalValue(std::string(StateName(entry.state)))},
      {"stop_event_id", StopEventIdValue(entry.stop_event_id)},
  };
  if (!entry.complete) return CanonicalValue(std::move(value));
  CanonicalValue::Array resources;
  for (const auto &resource : entry.resources) resources.emplace_back(resource.value());
  CanonicalValue::Array participants;
  for (const auto &participant : entry.participants) {
    CanonicalValue fact(nullptr);
    if (participant.terminal_fact) {
      fact = CanonicalValue(CanonicalValue::Object{
          {"effect", CanonicalValue(core::to_string(participant.terminal_fact->effect))},
          {"error",
           CanonicalValue(std::string(FactErrorName(participant.terminal_fact->error)))},
          {"kind", CanonicalValue(core::to_string(participant.terminal_fact->kind))},
      });
    }
    participants.emplace_back(CanonicalValue::Object{
        {"execution_scope", CanonicalValue(participant.execution_scope)},
        {"state", CanonicalValue(std::string(StateName(participant.state)))},
        {"terminal_error",
         participant.terminal_error
             ? CanonicalValue(CanonicalValue::Object{
                   {"reason", OptionalText(participant.terminal_error_reason)}})
             : CanonicalValue(nullptr)},
        {"terminal_fact", std::move(fact)},
    });
  }
  CanonicalValue error(nullptr);
  if (entry.terminal_error) {
    error = CanonicalValue(CanonicalValue::Object{
        {"code", CanonicalValue(entry.terminal_error->code)},
        {"message", CanonicalValue(entry.terminal_error->message)},
        {"message_truncated", CanonicalValue(entry.terminal_error->message_truncated)},
        {"reason", OptionalText(entry.terminal_error->reason)},
    });
  }
  value.emplace_back("participants", CanonicalValue(std::move(participants)));
  value.emplace_back("program_binding_id", CanonicalValue(entry.program_binding_id));
  value.emplace_back("program_run_id", RunValue(entry.program_run_id));
  value.emplace_back("resource_total",
                     CanonicalValue(static_cast<double>(entry.resource_total)));
  value.emplace_back("resources", CanonicalValue(std::move(resources)));
  value.emplace_back("terminal_error", std::move(error));
  return CanonicalValue(std::move(value));
}

} // namespace

CanonicalValue OperationTableValue(const core::OperationTableDTO &table) {
  CanonicalValue::Array faulted;
  faulted.reserve(table.faulted.size());
  for (const auto &entry : table.faulted)
    faulted.push_back(OperationTableEntryValue(entry));
  // OD-213: every resident operation with the `read` reply's members but
  // `resident` (true for each) and `active_tcp` (the state's own baseline).
  CanonicalValue::Array resident;
  resident.reserve(table.resident.size());
  for (const auto &projection : table.resident) {
    auto operation = OperationTableStateValue(projection.operation);
    if (content_identity::canonical_json(operation).size() >
        core::kOperationTableMaximumResidentOperationBytes)
      operation = CanonicalValue(nullptr);
    resident.emplace_back(CanonicalValue::Object{
        {"control_pending", CanonicalValue(projection.control_pending)},
        {"last_control", projection.last_control.has_value()
                             ? ControlAcceptedValue(*projection.last_control)
                             : CanonicalValue(nullptr)},
        {"operation", std::move(operation)},
        {"operation_id",
         CanonicalValue(static_cast<double>(projection.operation.operation_id.value()))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"change", CanonicalValue(static_cast<double>(table.change))},
      {"faulted", CanonicalValue(std::move(faulted))},
      {"faulted_total", CanonicalValue(static_cast<double>(table.faulted_total))},
      {"resident", CanonicalValue(std::move(resident))},
      {"resident_total", CanonicalValue(static_cast<double>(table.resident_total))},
  });
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
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue(std::string(core::kProgramExecutionSelectionSchema))},
      {"state", value.state.has_value()
                    ? ProgramExecutionScopeValue(*value.state)
                    : CanonicalValue(nullptr)},
      {"group_ids", CanonicalValue(std::move(groups))},
      {"selected_node_indices", indices(value.selected_node_indices)},
      {"widened_node_indices", indices(value.widened_node_indices)},
      {"stop", StopAcceptedValue(value.stop)}});
}

[[nodiscard]] CanonicalValue
StopEventStatusValue(const core::StopEventStatusDTO &value) {
  CanonicalValue::Array covered;
  covered.reserve(value.covered.size());
  for (const auto &entry : value.covered) {
    covered.emplace_back(CanonicalValue::Object{
        {"operation_id", CanonicalValue(static_cast<double>(entry.operation_id.value()))},
        {"participant_ordinal",
         CanonicalValue(static_cast<double>(entry.participant_ordinal))},
        {"started", CanonicalValue(entry.started)},
        {"done", CanonicalValue(entry.done)},
        {"phase", CanonicalValue(std::string(core::to_string(entry.phase)))},
        {"containing_stage_index",
         entry.containing_stage_index
             ? CanonicalValue(static_cast<double>(*entry.containing_stage_index))
             : CanonicalValue(nullptr)},
        {"escalated", CanonicalValue(entry.escalated)},
        {"stop_path_collision", CanonicalValue(entry.stop_path_collision)},
        {"error_code", entry.error_code ? CanonicalValue(*entry.error_code)
                                        : CanonicalValue(nullptr)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version", CanonicalValue(std::string(core::kExecutionStopStatusSchema))},
      {"event_id", CanonicalValue(static_cast<double>(value.event_id.value()))},
      {"request_key", CanonicalValue(RequestKeyText(value.request_key))},
      {"requested", CanonicalValue(std::string(core::to_string(value.requested)))},
      {"applied", CanonicalValue(std::string(core::to_string(value.applied)))},
      {"widen_reason", CanonicalValue(std::string(core::to_string(value.widen_reason)))},
      {"state", CanonicalValue(value.complete ? "complete" : "pending")},
      {"result", CanonicalValue(std::string(core::to_string(value.result)))},
      {"covered", CanonicalValue(std::move(covered))},
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"completed_at_ns",
       value.completed_at_ns
           ? manyforge::content_identity::nonnegative_int64_value(*value.completed_at_ns)
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
CellStopActionValue(const core::CellStopActionProjectionDTO &value) {
  CanonicalValue completed(nullptr);
  if (value.last_completed.has_value()) {
    const auto &last = *value.last_completed;
    completed = CanonicalValue(CanonicalValue::Object{
        {"event_id", CanonicalValue(static_cast<double>(last.event_id.value()))},
        {"applied", CanonicalValue(std::string(core::to_string(last.applied)))},
        {"result", CanonicalValue(std::string(core::to_string(last.result)))},
        {"escalated", CanonicalValue(last.escalated)},
        {"completed_at_ns",
         manyforge::content_identity::nonnegative_int64_value(last.completed_at_ns)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"available", CanonicalValue(true)},
      {"last_event_id", StopEventIdValue(value.last_event_id)},
      {"last_completed", std::move(completed)},
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
      {"stop_event_id", CanonicalValue(static_cast<double>(value.stop_event_id))},
      {"stop_phase", CanonicalValue(value.stop_phase)},
      {"containing_stage_id", CanonicalValue(value.containing_stage_id)},
      {"stop_owner_active", CanonicalValue(value.stop_owner_active)},
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
         CanonicalValue(static_cast<double>(
             value.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(value.active_control_configuration_id.value())},
        {"capability_id", CanonicalValue(value.capability_id.value())},
        {"capability_provider_binding_id",
         value.capability_provider_binding_id.has_value()
             ? CanonicalValue(value.capability_provider_binding_id->value())
             : CanonicalValue(nullptr)},
        {"command_target_id", CanonicalValue(value.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(value.controllable_component_id.value())},
        {"control_binding_id", CanonicalValue(value.control_binding_id.value())},
        {"control_domain_id", CanonicalValue(value.control_domain_id.value())},
        {"control_route_id", CanonicalValue(value.control_route_id.value())},
        {"operation_participant_id",
         core::OperationParticipantIdValue(value.operation_participant_id)},
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
      {"request_key", CanonicalValue(core::RequestKeyText(value.request_key))},
      {"resulting_assessment",
       RecoveryAssessmentValue(value.resulting_assessment)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlSelectorValue(
    const core::RuntimeScaleControlSelectorDTO &value) {
  if (value.group_targets.empty()) {
    return CanonicalValue(CanonicalValue::Object{
        {"component_id", CanonicalValue(value.component_id.value())},
        {"program_binding_id",
         CanonicalValue(value.program_binding_id.value())},
        {"command_target_id", CanonicalValue(value.command_target_id.value())},
    });
  }
  CanonicalValue::Array targets;
  targets.reserve(value.group_targets.size());
  for (const auto &target : value.group_targets) {
    targets.emplace_back(CanonicalValue::Object{
        {"component_id", CanonicalValue(target.component_id.value())},
        {"command_target_id", CanonicalValue(target.command_target_id.value())},
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
           ? CanonicalValue(static_cast<double>(value.active_operation_id->value()))
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
      {"request_key", CanonicalValue(RequestKeyText(value.request_key))},
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
        {"component_id",
         CanonicalValue(member.target.component_id.value())},
        {"command_target_id",
         CanonicalValue(member.target.command_target_id.value())},
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
           ? CanonicalValue(static_cast<double>(value.active_operation_id->value()))
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
ParticipantStopObservationValue(const core::ParticipantStopObservationDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"phase", CanonicalValue(std::string(core::to_string(value.phase)))},
      {"attempt", CanonicalValue(static_cast<double>(value.attempt))},
      {"stop_event_id", CanonicalValue(static_cast<double>(value.stop_event_id.value()))},
      {"mode", CanonicalValue(std::string(core::to_string(value.mode)))},
      {"attribution_class",
       CanonicalValue(std::string(core::to_string(value.attribution_class)))},
      {"plan_digest", value.plan_digest ? CanonicalValue(value.plan_digest->value())
                                        : CanonicalValue(nullptr)},
      {"containing_stage_id", value.containing_stage_id
                                  ? CanonicalValue(value.containing_stage_id->value())
                                  : CanonicalValue(nullptr)},
      {"containing_stage_index",
       value.containing_stage_index
           ? CanonicalValue(static_cast<double>(*value.containing_stage_index))
           : CanonicalValue(nullptr)},
      {"escalated", CanonicalValue(value.escalated)},
  });
}

namespace {

// `for_table`: the event stream's operations table (OD-213) leaves out what
// changes at every progress sample (participants' `progress`, the update
// time) and the collision log line (written by `read` replies only).
[[nodiscard]] CanonicalValue
EncodeOperationState(const core::OperationStateDTO &value, const bool for_table) {
  CanonicalValue::Array participants;
  participants.reserve(value.participants.size());
  for (const auto &participant : value.participants) {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    // Durable operation details remain authoritative. This status-observed
    // notice is best-effort; one last key suppresses consecutive repeats only.
    if (!for_table && participant.terminal_error &&
        participant.terminal_error->details.contains("runtime_collision_reason")) {
      try {
        const auto notice = content_identity::canonical_json(
            CanonicalValue(CanonicalValue::Object{
                {"operation_id", CanonicalValue(static_cast<double>(value.operation_id.value()))},
                {"operation_revision", CanonicalValue(static_cast<double>(
                                           value.operation_revision.value()))},
                {"participant_id", core::OperationParticipantIdValue(participant.participant_id)},
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
              "ERROR manyforge runtime collision: operation=" +
                  std::to_string(value.operation_id.value()) +
              " revision=" + std::to_string(value.operation_revision.value()) +
              " participant=" + std::to_string(participant.participant_id.ordinal) +
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
    const auto &controller = participant.binding_identity.controller;
    const auto *provider = participant.binding_identity.capability_provider();
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
         CanonicalValue(controller.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(controller.control_binding_revision.value())},
        {"control_domain_id",
         CanonicalValue(
             participant.binding_identity.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(
             participant.binding_identity.control_domain_revision.value())},
        {"command_id",
         core::ControllerGoalIdValue(participant.command_id)},
        {"participant_id", core::OperationParticipantIdValue(participant.participant_id)},
        {"payload_kind",
         CanonicalValue(participant.payload_kind ==
                                core::OperationParticipantPayloadKind::kMotion
                            ? "motion"
                            : "capability_command")},
        {"resources", CanonicalValue(std::move(resources))},
        {"state", CanonicalValue(std::string(StateName(participant.state)))},
        {"terminal_error", participant.terminal_error.has_value()
                               ? ErrorValue(*participant.terminal_error)
                               : CanonicalValue(nullptr)},
        {"stop", participant.stop ? ParticipantStopObservationValue(*participant.stop)
                                  : CanonicalValue(nullptr)},
    };
    if (!for_table)
      participant_value.emplace_back("progress", CanonicalValue(participant.progress));
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
               CanonicalValue(static_cast<double>(
                   participant.execution_scope->node_attempt_id.value()))}}));
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
    participants.emplace_back(std::move(participant_value));
  }
  CanonicalValue::Object operation_value{
      {"stop_event_id", StopEventIdValue(value.stop_event_id)},
      {"coordination_policy",
       CanonicalValue(value.coordination_policy ==
                              core::OperationCoordinationPolicy::kCoordinatedAll
                          ? "coordinated_all"
                          : "independent")},
      {"operation_id", CanonicalValue(static_cast<double>(value.operation_id.value()))},
      {"operation_revision",
       CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_run_id", RunValue(value.program_run_id)},
      {"runtime_scale_generation",
       value.runtime_scale_generation.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.runtime_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(StateName(value.state)))},
  };
  if (!for_table)
    operation_value.emplace_back(
        "updated_at_ns", manyforge::content_identity::nonnegative_int64_value(value.updated_at_ns));
  if (value.preemption) {
    const auto& fact = *value.preemption;
    operation_value.emplace_back("preemption", CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue(std::string(core::to_string(fact.kind)))},
        {"by", CanonicalValue(CanonicalValue::Object{
            {"operation_id", CanonicalValue(static_cast<double>(fact.by.operation_id.value()))},
            {"node_instance_id", CanonicalValue(fact.by.node_instance_id.value())},
            {"node_attempt_id",
             CanonicalValue(static_cast<double>(fact.by.node_attempt_id.value()))}})}}));
  }
  if (value.motion_takeover) {
    const auto &decision = *value.motion_takeover;
    operation_value.emplace_back(
        "motion_takeover",
        CanonicalValue(CanonicalValue::Object{
            {"incumbent_operation_id",
             CanonicalValue(static_cast<double>(decision.incumbent_operation_id.value()))},
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
            {"planning_budget_ns", content_identity::nonnegative_int64_value(
                                       decision.planning_budget_ns)},
            {"stop_event_id", StopEventIdValue(decision.stop_event_id)},
        }));
  }
  return CanonicalValue(std::move(operation_value));
}

} // namespace

[[nodiscard]] CanonicalValue
OperationStateValue(const core::OperationStateDTO &value) {
  return EncodeOperationState(value, false);
}

[[nodiscard]] CanonicalValue
OperationTableStateValue(const core::OperationStateDTO &value) {
  return EncodeOperationState(value, true);
}

[[nodiscard]] CanonicalValue RuntimeFaultsValue(
    const core::ResourceAddressedExecutionKernel &owner) {
  const auto journal_fault = owner.get_operation_journal_write_fault();
  const auto runner_loss = owner.get_program_runner_loss();
  const auto internal_fault = owner.get_internal_fault();
  CanonicalValue journal_value(nullptr);
  if (journal_fault) {
    const auto &fault = *journal_fault;
    journal_value = CanonicalValue(CanonicalValue::Object{
        {"write_kind", CanonicalValue(fault.write_kind)},
        {"error", ErrorValue(fault.error)},
        {"operation_id", fault.operation_id
             ? CanonicalValue(static_cast<double>(fault.operation_id->value())) : CanonicalValue(nullptr)},
        {"operation_revision", fault.operation_revision
             ? CanonicalValue(static_cast<double>(fault.operation_revision->value()))
             : CanonicalValue(nullptr)},
        {"request_key", fault.request_key
             ? CanonicalValue(RequestKeyText(*fault.request_key)) : CanonicalValue(nullptr)},
        {"program_run_id", RunValue(fault.program_run_id)},
        {"execution_scope", fault.execution_scope
             ? CanonicalValue(CanonicalValue::Object{
                   {"node_instance_id", CanonicalValue(fault.execution_scope->node_instance_id.value())},
                   {"node_attempt_id", CanonicalValue(static_cast<double>(
                                           fault.execution_scope->node_attempt_id.value()))}})
             : CanonicalValue(nullptr)},
        // OD-215: the journal queue's "full" level for the Headless disk check.
        {"queue_full", CanonicalValue(fault.queue_full)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"operation_journal_write_fault", std::move(journal_value)},
      {"program_runner_loss", runner_loss
           ? detail::ProgramRunnerLossValue(*runner_loss, owner.application_start_id())
           : CanonicalValue(nullptr)},
      {"kernel_internal_fault", internal_fault
           ? CanonicalValue(CanonicalValue::Object{
                 {"reason_code", CanonicalValue(internal_fault->reason_code)},
                 {"later_count",
                  CanonicalValue(static_cast<double>(internal_fault->later_count))}})
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue RuntimeCollisionChecksRowValue(
    const std::string_view controllable_component_id,
    const core::RuntimeCollisionMonitorTimingMetrics &timing,
    const core::Duration check_period_ns) {
  // A late check is one that ended one or more whole periods after its
  // deadline; the periods it overran are skipped, never caught up.
  return CanonicalValue(CanonicalValue::Object{
      {"check_period_ns", CanonicalValue(static_cast<double>(check_period_ns))},
      {"checks_late", CanonicalValue(static_cast<double>(timing.late_periods))},
      {"checks_run", CanonicalValue(static_cast<double>(timing.callbacks_started))},
      {"controllable_component_id",
       CanonicalValue(std::string(controllable_component_id))},
      {"periods_skipped", CanonicalValue(static_cast<double>(timing.missed_periods))},
  });
}

[[nodiscard]] CanonicalValue
OperationEventPageValue(const core::OperationJournalEventPageDTO &page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto &event : page.events) {
    // Stop event rows (`stop_event_accepted`/`stop_event_completed`) carry
    // their `manyforge.stop_event.v1` payload unchanged, like every row.
    CanonicalValue payload;
    std::string payload_digest = event.payload_digest;
    try {
      payload =
          deployment::parse_strict_json_model_yaml(event.payload_json).root();
    } catch (const std::exception &error) {
      Fail("durable operation event payload is not strict JSON: " +
           std::string(error.what()));
    }
    Require(content_identity::canonical_json(payload) == event.payload_json,
            "durable operation event payload is not canonical JSON");
    events.emplace_back(CanonicalValue::Object{
        {"occurred_at_ns", manyforge::content_identity::nonnegative_int64_value(
                               event.occurred_at_ns)},
        {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
        {"operation_id", event.operation_id.has_value()
                             ? CanonicalValue(static_cast<double>(event.operation_id->value()))
                             : CanonicalValue(nullptr)},
        {"operation_revision", event.operation_revision.has_value()
                                   ? CanonicalValue(static_cast<double>(
                                         event.operation_revision->value()))
                                   : CanonicalValue(nullptr)},
        {"payload", std::move(payload)},
        {"payload_digest", CanonicalValue(std::move(payload_digest))},
        {"record_kind", CanonicalValue(event.record_kind)},
        {"request_key", event.request_key.has_value()
                           ? CanonicalValue(RequestKeyText(*event.request_key))
                           : CanonicalValue(nullptr)},
        {"sequence", CanonicalValue(static_cast<double>(event.sequence))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"has_more", CanonicalValue(page.has_more)},
      {"next_after_sequence",
       CanonicalValue(static_cast<double>(page.next_after_sequence))},
      {"schema_version", CanonicalValue("manyforge.execution_event_page.v3")},
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
      {"catalog_generation", CanonicalValue(std::to_string(value.catalog_generation))},
      {"registration_id", CanonicalValue(std::to_string(value.registration_id))},
      {"scope", CanonicalValue(CanonicalValue::Object{
          {"program_run_id", CanonicalValue(static_cast<double>(value.scope.program_run_id.value()))},
          {"node", CanonicalValue(CanonicalValue::Object{
              {"node_instance_id", CanonicalValue(value.scope.node.node_instance_id.value())},
              {"node_attempt_id",
               CanonicalValue(static_cast<double>(value.scope.node.node_attempt_id.value()))}})}})}});
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

namespace {
CanonicalValue GlobalRuleContextValue(const core::IoRuleStateContextDTO &context) {
  Require(!core::io_rule_state_context_field_error(context),
          "global rule context is invalid");
  return CanonicalValue(CanonicalValue::Object{
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
  CanonicalValue::Object fields{
      {"rule_id", CanonicalValue(row.rule_id.value())},
      {"state", CanonicalValue(std::string(core::io_global_rule_state_token(row.state)))},
      {"triggering_acquisition", std::move(acquisition)}};
  if (row.unavailable_reason)
    fields.emplace_back("unavailable_reason", ErrorValue(*row.unavailable_reason));
  return CanonicalValue(std::move(fields));
}
} // namespace

CanonicalValue::Object InterruptRuleEventMetadataFields(
    const std::optional<core::IoRuleStateProjectionDTO> &actual) {
  if (!actual) return {{"interrupt_rule_catalog", CanonicalValue(nullptr)},
                       {"interrupt_rule_state_baseline", CanonicalValue(nullptr)}};
  const auto applied_fields = [&](const core::IoRuleAppliedContextDTO &context,
                                  std::string_view status) {
    const auto identity = [](const auto &id) {
      return !id.empty() && id.value().find('\0') == std::string::npos;
    };
    Require(identity(context.retained_bundle_id) &&
                identity(context.graph_revision) &&
                context.active_publication_generation.nonzero() &&
                context.active_publication_generation.value() <=
                    9'007'199'254'740'991ULL,
            "rule metadata has no exact successful Applied context");
    return CanonicalValue::Object{
        {"status", CanonicalValue(std::string(status))},
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
    Require(actual->status == core::IoRuleStateProjectionStatusDTO::kReady &&
                !actual->catalog && !actual->baseline,
            "verified-empty rule projection has no exact successful Applied "
            "context");
    return {{"interrupt_rule_catalog",
             CanonicalValue(applied_fields(context, "verified_empty"))},
            {"interrupt_rule_state_baseline", CanonicalValue(nullptr)}};
  }
  Require(actual->catalog != nullptr, "rule projection has no actual catalog");
  const auto &catalog = *actual->catalog;
  const auto &context = catalog.context;
  Require(!core::io_rule_state_context_field_error(context),
          "rule catalog context is invalid");
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
            "ready rule baseline does not match its catalog");
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
            {"catalog_generation", CanonicalValue(std::to_string(context.catalog_generation))},
            {"active_publication_generation", CanonicalValue(static_cast<double>(context.active_publication_generation.value()))},
            {"retained_bundle_id", CanonicalValue(context.retained_bundle_id.value())},
            {"graph_revision", CanonicalValue(context.graph_revision.value())},
            {"states", CanonicalValue(std::move(states))}})}});
  }
  return {{"interrupt_rule_catalog", std::move(metadata)},
          {"interrupt_rule_state_baseline", std::move(baseline)}};
}

namespace {
CanonicalValue Text(const std::string_view value) { return CanonicalValue(std::string(value)); }
CanonicalValue Nanoseconds(const std::int64_t value) {
  return content_identity::nonnegative_int64_value(std::max<std::int64_t>(value, 0));
}
/// Optional contract fields are omitted when absent.
void OptionalText(CanonicalValue::Object &object, const char *name,
                  const std::optional<std::string> &value) {
  if (value) object.emplace_back(name, CanonicalValue(*value));
}
}  // namespace

std::string SignalIdText(const core::IoSignalId &signal) {
  return signal.controllable_component_id.value() + "/" + signal.name;
}

std::optional<core::IoSignalId> ParseSignalIdText(const std::string_view text) {
  const auto slash = text.find('/');
  if (slash == std::string_view::npos || slash == 0U || slash + 1U == text.size())
    return std::nullopt;
  return core::IoSignalId{core::ControllableComponentId(std::string(text.substr(0, slash))),
                          std::string(text.substr(slash + 1U))};
}

CanonicalValue FaultValue(const core::Fault &fault) {
  CanonicalValue::Object object{
      {"clearable", CanonicalValue(fault.clearable)},
      {"code", Text(core::to_string(fault.code))},
      {"message", CanonicalValue(fault.message)},
      {"operator_action", Text(core::to_string(fault.operator_action))},
      {"since_ns", Nanoseconds(fault.since_ns)}};
  OptionalText(object, "vendor_code", fault.vendor_code);
  OptionalText(object, "vendor_text", fault.vendor_text);
  if (!fault.concerns.empty()) {
    CanonicalValue::Array concerns;
    for (const auto &component_id : fault.concerns) concerns.emplace_back(component_id);
    object.emplace_back("concerns", CanonicalValue(std::move(concerns)));
  }
  return CanonicalValue(std::move(object));
}

CanonicalValue ActionResultValue(const core::ActionResult &result) {
  CanonicalValue::Object object{
      {"action", Text(core::to_string(result.action))},
      {"finished_at_ns", Nanoseconds(result.finished_at_ns)},
      {"outcome", Text(core::to_string(result.outcome))},
      {"started_at_ns", Nanoseconds(result.started_at_ns)}};
  if (result.error) {
    CanonicalValue::Object error{{"code", CanonicalValue(result.error->code)},
                                 {"message", CanonicalValue(result.error->message)}};
    OptionalText(error, "vendor_code", result.error->vendor_code);
    OptionalText(error, "vendor_text", result.error->vendor_text);
    object.emplace_back("error", CanonicalValue(std::move(error)));
  }
  if (!result.phases.empty()) {
    CanonicalValue::Array phases;
    for (const auto &phase : result.phases) {
      CanonicalValue::Object item{{"component_id", CanonicalValue(phase.component_id)},
                                  {"action", Text(core::to_string(phase.action))},
                                  {"outcome", Text(core::to_string(phase.outcome))}};
      if (phase.error) {
        CanonicalValue::Object error{{"code", CanonicalValue(phase.error->code)},
                                     {"message", CanonicalValue(phase.error->message)}};
        OptionalText(error, "vendor_code", phase.error->vendor_code);
        OptionalText(error, "vendor_text", phase.error->vendor_text);
        item.emplace_back("error", CanonicalValue(std::move(error)));
      }
      phases.emplace_back(std::move(item));
    }
    object.emplace_back("phases", CanonicalValue(std::move(phases)));
  }
  return CanonicalValue(std::move(object));
}

CanonicalValue ComponentStopValue(const core::ComponentStop &stop) {
  CanonicalValue::Object object{{"code", Text(core::to_string(stop.code))},
                                {"message", CanonicalValue(stop.message)},
                                {"since_ns", Nanoseconds(stop.since_ns)}};
  OptionalText(object, "vendor_code", stop.vendor_code);
  OptionalText(object, "vendor_text", stop.vendor_text);
  return CanonicalValue(std::move(object));
}
CanonicalValue ComponentStateValue(const core::ComponentState &state) {
  CanonicalValue::Array streams;
  for (const auto &stream : state.streams) {
    CanonicalValue::Object row{{"freshness", Text(core::to_string(stream.freshness))},
                               {"stream_id", CanonicalValue(stream.stream_id)}};
    if (stream.age_ns) row.emplace_back("age_ns", Nanoseconds(*stream.age_ns));
    streams.emplace_back(std::move(row));
  }
  CanonicalValue::Array capabilities;
  for (const auto capability : state.capabilities)
    capabilities.emplace_back(Text(core::to_string(capability)));
  CanonicalValue::Object object{
      {"activity", Text(core::to_string(state.activity))},
      {"capabilities", CanonicalValue(std::move(capabilities))},
      {"component_id", CanonicalValue(state.component_id)},
      {"lifecycle", Text(core::to_string(state.lifecycle))},
      {"observed_at_ns", Nanoseconds(state.observed_at_ns)},
      {"streams", CanonicalValue(std::move(streams))}};
  if (!state.details.empty()) {
    CanonicalValue::Array details;
    for (const auto &detail : state.details)
      details.emplace_back(CanonicalValue::Object{{"name", CanonicalValue(detail.name)},
                                                  {"value", CanonicalValue(detail.value)}});
    object.emplace_back("details", CanonicalValue(std::move(details)));
  }
  if (!state.endpoints.empty()) {
    CanonicalValue::Array endpoints;
    for (const auto &endpoint : state.endpoints)
      endpoints.emplace_back(CanonicalValue::Object{{"role_id", CanonicalValue(endpoint.role_id)},
                                                    {"url", CanonicalValue(endpoint.url)}});
    object.emplace_back("endpoints", CanonicalValue(std::move(endpoints)));
  }
  if (state.action)
    object.emplace_back("action", CanonicalValue(CanonicalValue::Object{
                                      {"action", Text(core::to_string(state.action->action))},
                                      {"started_at_ns", Nanoseconds(state.action->started_at_ns)}}));
  if (state.fault) object.emplace_back("fault", FaultValue(*state.fault));
  if (state.stop) object.emplace_back("stop", ComponentStopValue(*state.stop));
  OptionalText(object, "owner_id", state.owner_id);
  OptionalText(object, "role", state.role);
  if (state.last_action) object.emplace_back("last_action", ActionResultValue(*state.last_action));
  if (state.reason)
    object.emplace_back("reason", CanonicalValue(CanonicalValue::Object{
                                      {"code", CanonicalValue(state.reason->code)},
                                      {"message", CanonicalValue(state.reason->message)}}));
  return CanonicalValue(std::move(object));
}

CanonicalValue CellLatchValue(const core::CellLatch &latch) {
  CanonicalValue::Array causes;
  for (const auto &cause : latch.causes) {
    CanonicalValue::Object row{{"component_id", CanonicalValue(cause.component_id)},
                               {"kind", Text(core::to_string(cause.kind))}};
    if (cause.fault) row.emplace_back("fault", FaultValue(*cause.fault));
    if (cause.stop) row.emplace_back("stop", ComponentStopValue(*cause.stop));
    row.emplace_back("reason", cause.reason
                                   ? CanonicalValue(CanonicalValue::Object{
                                         {"code", CanonicalValue(cause.reason->code)},
                                         {"message", CanonicalValue(cause.reason->message)}})
                                   : CanonicalValue(nullptr));
    causes.emplace_back(std::move(row));
  }
  return CanonicalValue(CanonicalValue::Object{{"causes", CanonicalValue(std::move(causes))},
                                               {"latched", CanonicalValue(latch.latched)}});
}

CanonicalValue RuleHoldsValue(const std::vector<core::ErrorDTO> &holds) {
  CanonicalValue::Array rows;
  for (const auto &hold : holds) rows.emplace_back(ErrorValue(hold));
  return CanonicalValue(std::move(rows));
}

CanonicalValue SignalSampleValue(const core::SignalSample &sample,
                                 const core::IoSignalDescriptorDTO &descriptor, const bool age) {
  CanonicalValue value(nullptr);
  if (sample.value) {
    if (const auto *boolean = std::get_if<bool>(&*sample.value))
      value = CanonicalValue(*boolean);
    else
      value = CanonicalValue(std::get<double>(*sample.value));
  }
  const auto source = [&]() -> std::string_view {
    switch (sample.source) {
      case core::IoSignalSampleSource::kDevice:
        return "device";
      case core::IoSignalSampleSource::kManyforgeWrite:
        return "manyforge_write";
      case core::IoSignalSampleSource::kOutside:
        return "outside";
      case core::IoSignalSampleSource::kNotReadable:
        return "not_readable";
    }
    return "device";
  }();
  CanonicalValue::Object object{{"fresh", CanonicalValue(sample.fresh)},
                                {"signal_id", CanonicalValue(SignalIdText(sample.signal_id))},
                                {"source", Text(source)},
                                {"value", std::move(value)}};
  if (age) object.emplace_back("age_ns", Nanoseconds(sample.age_ns));
  if (descriptor.direction == core::IoSignalDirection::kOutput)
    object.emplace_back("controller_owned", CanonicalValue(descriptor.controller_owned));
  return CanonicalValue(std::move(object));
}

} // namespace manyforge::roboplan_backend::host_detail
