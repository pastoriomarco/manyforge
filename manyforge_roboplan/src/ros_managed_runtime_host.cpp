#include "ros_managed_runtime_host.hpp"
#include "applied_global_io_rule_installation.hpp"
#include "applied_program_blackboard_installation.hpp"
#include "applied_program_owner_reader.hpp"
#include "selected_program_products.hpp"
#include "host_io_provider_installation.hpp"
#include "host_component_runtime.hpp"
#include "host_io_signal_runtime.hpp"
#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/passive_component_adapter.hpp>
#include <manyforge_roboplan/ros2_control_lane_family.hpp>
#include <manyforge_roboplan/ros2_control_component_adapter.hpp>
#include <ament_index_cpp/get_package_share_path.hpp>
#include <charconv>
#include "io_provider_lifecycle_product.hpp"
#include "program_reset_position_observation.hpp"
#include "simulated_plc_readiness.hpp"
#include <manyforge_deployment/execution_runtime_contract.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <manyforge_core/admission_timing_diagnostic.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <iterator>
#include <limits>
#include <type_traits>
#include <variant>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_component_manager/site_settings.hpp>
#include <manyforge_component_manager/manager_named_apply_coordinator.hpp>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_roboplan/mock_boolean_input_provider.hpp>
#include <manyforge_roboplan/ros_boolean_input_provider.hpp>
#include <manyforge_core/time/host_boot_identity.hpp>
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_measured_robot_geometry.hpp>
#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <manyforge_roboplan/retained_component_owner.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_view_predecessor.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/simulated_plc_launch.hpp>
#include <manyforge_roboplan/pose_source_launch.hpp>
#include <manyforge_roboplan/component_stream_sample_bridge.hpp>
#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_tool_context.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <manyforge_roboplan/roboplan_scene_factory.hpp>
#include <manyforge_scene/logical_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <fstream>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "execution_host_posix.hpp"
#include "execution_host_commands.hpp"
#include "execution_host_responses.hpp"
#include "execution_host_scene_bridge.hpp"
#include "execution_host_surface.hpp"
#include "ros_host_composition.hpp"
#include <manyforge_roboplan/retained_gripper_route_publication_owner.hpp>

#include "cell_admission_command_codec.hpp"
#include "kernel_apply_fence_rpc_internal.hpp"
#include "execution_motion_target_limits_internal.hpp"
#include "managed_kernel_apply_fence_owner.hpp"
#include "managed_projection_registry.hpp"
#include "committed_tool_payload_follower.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include "native_projection_rpc_internal.hpp"
#include "owner_settlement_readers.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include "retained_execution_host_internal.hpp"
#include "selected_runtime_composition_rpc_internal.hpp"
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>

#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
#include "focused_pick_place_planning_fixture.hpp"
#endif


namespace manyforge::roboplan_backend {
using namespace host_detail;
namespace {

using DiagnosticClock = std::chrono::steady_clock;
using DiagnosticTime = DiagnosticClock::time_point;

struct RpcTiming final {
  DiagnosticTime received{};
  DiagnosticTime dispatch{};
  DiagnosticTime handler_end{};
  std::chrono::nanoseconds composition_wait{};
  std::chrono::nanoseconds servicing_wait{};
  std::chrono::nanoseconds journal_read{};
  std::chrono::nanoseconds finalize_body{};
  std::chrono::nanoseconds candidate_load{};
  std::chrono::nanoseconds projection_select{};
};

struct TimingSpan final {
  std::chrono::nanoseconds &total;
  DiagnosticTime start{DiagnosticClock::now()};
  ~TimingSpan() { total += DiagnosticClock::now() - start; }
};

// One line per kind per second. The diagnostic never changes the RPC outcome.
void LogSlowRpc(const std::string_view kind, const RpcTiming &timing,
                const DiagnosticTime sent,
                const std::string_view apply_id = {},
                const std::string_view request_id = {}) noexcept {
  try {
    const auto end = DiagnosticClock::now();
    if (sent - timing.received <= std::chrono::milliseconds(200))
      return;
    constexpr std::array<std::string_view, 11> kinds = {
        "status", "selected_finalize", "cell_admission_get",
        "cell_admission_acquire", "cell_admission_release",
        "cell_admission_reconcile", "cell_admission_recovery_receipt",
        "cell_admission_update_readiness", "cell_admission_publish_liveness",
        "cell_admission_validate_program_reset_scene", "other"};
    static std::array<std::atomic<std::int64_t>, kinds.size()> last{};
    std::size_t index = kinds.size() - 1;
    for (std::size_t i = 0; i + 1 < kinds.size(); ++i)
      if (kinds[i] == kind) { index = i; break; }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end.time_since_epoch()).count();
    auto previous = last[index].load(std::memory_order_relaxed);
    do {
      if (now_ns - previous < 1000000000LL) return;
    } while (!last[index].compare_exchange_weak(previous, now_ns,
                                                 std::memory_order_relaxed));
    const auto ns = [](const DiagnosticTime value) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
          value.time_since_epoch()).count();
    };
    std::ostringstream line;
    line << "slow kernel rpc pid=" << ::getpid() << " kind=" << kinds[index]
         << " apply_id=" << apply_id << " request_id=" << request_id
         << " received_ns=" << ns(timing.received)
         << " dispatch_ns=" << ns(timing.dispatch)
         << " handler_end_ns=" << ns(timing.handler_end)
         << " reply_sent_ns=" << ns(sent)
         << " composition_wait_ns=" << timing.composition_wait.count()
         << " servicing_wait_ns=" << timing.servicing_wait.count()
         << " owner_wait_ns=unavailable"
         << " finalize_parent_journal_ns=" << timing.journal_read.count()
         << " finalize_body_ns=" << timing.finalize_body.count()
         << " finalize_candidate_load_ns=" << timing.candidate_load.count()
         << " finalize_projection_select_ns=" << timing.projection_select.count() << '\n';
    std::cerr << line.str();
  } catch (...) {}
}


// Emitted only after the admission dispatch scope has released its locks.
void LogAdmissionTiming(std::string_view command, std::string_view request_id,
                        const RpcTiming &rpc,
                        const core::detail::AdmissionTimingData &data, bool send_failed) noexcept {
  try {
    const bool release = command == "cell_admission_release";
    const bool finalize = command == "selected_finalize";
    if (!release && !finalize && command != "cell_admission_acquire") return;
    if (!send_failed && rpc.handler_end - rpc.received <= std::chrono::milliseconds(200) &&
        data.event_count == 0) return;
    static std::array<std::atomic<std::int64_t>, 3> last{};
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        DiagnosticClock::now().time_since_epoch()).count();
    if (!send_failed) {
      auto &slot = last[finalize ? 2 : release ? 1 : 0];
      auto old = slot.load(std::memory_order_relaxed);
      do {
        if (now - old < 1000000000LL) return;
      } while (!slot.compare_exchange_weak(old, now, std::memory_order_relaxed));
    }
    const auto ns = [](DiagnosticTime value) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
    };
    std::ostringstream line;
    line << "slow host core timing pid=" << ::getpid() << " tid=" << ::syscall(SYS_gettid)
         << " command=" << command << (finalize ? " apply_id=" : " request_id=") << request_id
         << " send_failed=" << (send_failed ? 1 : 0)
         << " received_ns=" << ns(rpc.received) << " handler_end_ns=" << ns(rpc.handler_end)
         << " submission_wait_ns=" << data.submission_wait.count()
         << " admission_wait_ns=" << data.admission_wait.count()
         << " transition_wait_ns=" << data.transition_wait.count()
         << " owner_wait_ns=" << data.owner_wait.count()
         << " settlement_ns=" << data.settlement.count() << " journal_ns=" << data.journal.count()
         << " refresh_ns=" << data.refresh.count() << " journal_events_dropped=" << data.dropped;
    for (std::size_t i = 0; i < data.event_count; ++i) {
      const auto &event = data.events[i];
      line << " journal_stage=" << event.stage << ",elapsed_ns=" << event.elapsed_ns
           << (event.rows_kind == core::detail::AdmissionJournalRowsKind::kTableCardinality
                   ? ",table_cardinality=" : ",returned_rows=")
           << event.rows << ",direct_changes=" << event.changes;
      if (event.outcome) line << ",outcome=" << event.outcome;
    }
    line << '\n';
    std::cerr << line.str();
  } catch (...) {}
}

namespace scene = manyforge::scene;

// Active TCP (M7): the robot's TCP list, TCP-A1's `robot_tcps` of its
// direct-control binding's robot row (design 1.2).
[[nodiscard]] std::vector<RobotTcp>
DirectControlRobotTcps(const deployment::ProgramBinding &binding) {
  for (const auto &target : binding.value().target_role_bindings)
    if (target.target_role_id.value() == deployment::kDirectControlTargetRoleId)
      return target.robot_tcps.value_or(std::vector<RobotTcp>{});
  return {};
}

// CONTRACTS §12: one component's model lane row as the Deployment compiled
// it (`controller_profiles[].lane_row`, canonical JSON of the model's
// `lanes.<lane>` object), typed as 3b's ModelLaneRow. Scalar keys other than
// the typed ones are adapter keys (YAML scalar text); non-scalar keys (e.g.
// xArm's `io`) stay in `value`. A relative `controllers_file` resolves
// against the model folder (`<manyforge_example_models>/models/<family>`).
struct ComponentLane final {
  std::string lane;
  std::string model_family_id;
  ModelLaneRow row;
  CanonicalValue value;
};

[[nodiscard]] std::string LaneScalarText(const CanonicalValue &value) {
  if (value.is_string()) return value.as_string();
  if (value.is_bool()) return value.as_bool() ? "true" : "false";
  std::array<char, 64> text{};
  const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value.as_number());
  Require(error == std::errc{}, "lane row number cannot be written");
  return std::string(text.data(), end);
}

/// One model `lanes.<lane>` row (canonical JSON) of `component_id`, typed.
[[nodiscard]] ComponentLane ParseComponentLane(const std::string &component_id,
                                               const std::string &lane_name,
                                               const std::string &model_family_id,
                                               const std::string &lane_row) {
  ComponentLane lane{lane_name, model_family_id, {},
                     content_identity::parse_canonical_json(lane_row)};
  Require(lane.value.is_object(), "component " + component_id + " lane row is not an object");
  for (const auto &[key, member] : lane.value.as_object()) {
    if (key == "adapter") {
      lane.row.adapter = member.as_string();
    } else if (key == "controller") {
      lane.row.controller = member.as_string();
    } else if (key == "arrival_tolerance_rad") {
      lane.row.arrival_tolerance_rad = member.as_number();
    } else if (key == "goal_time_s") {
      lane.row.goal_time_s = member.as_number();
    } else if (key == "controllers_file") {
      std::filesystem::path file(member.as_string());
      lane.row.controllers_file =
          file.is_absolute()
              ? file
              : ament_index_cpp::get_package_share_path("manyforge_example_models") /
                    "models" / model_family_id / file;
    } else if (!member.is_object() && !member.is_array() && !member.is_null()) {
      lane.row.adapter_keys.emplace(key, LaneScalarText(member));
    }
  }
  Require(!lane.row.adapter.empty(), "component " + component_id + " lane row names no adapter");
  return lane;
}

[[nodiscard]] ComponentLane SelectedComponentLane(
    const deployment::ros2_control_execution_runtime_contract::Configuration::ControllerProfile
        &profile) {
  return ParseComponentLane(profile.controllable_component_id.value(), profile.lane,
                            profile.model_family_id, profile.lane_row);
}

/// The component's own streams as the host's stream registry reports them
/// (CONTRACTS §1 `streams[]`).
[[nodiscard]] Ros2ControlComponentAdapter::StreamsFn
ComponentStreams(const core::ControllableComponentId &component) {
  return [component](std::int64_t) {
    std::vector<core::ComponentStream> streams;
    for (const auto &row : host_component_stream_health()->Read(component))
      streams.push_back({row.stream_id,
                         row.state == core::StreamLivenessStateDTO::kLive    ? core::StreamFreshness::kFresh
                         : row.state == core::StreamLivenessStateDTO::kStale ? core::StreamFreshness::kStale
                                                                             : core::StreamFreshness::kNone,
                         row.age_ns});
    return streams;
  };
}

template <typename Resolver>
[[nodiscard]] RetainedRuntimeParticipantResolver
MakeRetainedRuntimeParticipantResolver(const Resolver &resolver) {
  return [resolver](const core::OperationIntentDTO &operation,
                    const std::size_t participant_index) {
    return resolver->ResolveExecutionParticipant(operation, participant_index);
  };
}

template <typename MakeResolver>
[[nodiscard]] RetainedRuntimeParticipantResolverFactory
MakeCandidateParticipantResolverFactory(MakeResolver make_resolver) {
  return [make_resolver = std::move(make_resolver)](
             std::vector<core::RuntimeParticipantBinding> bindings) {
    auto resolver = make_resolver(std::move(bindings));
    return MakeRetainedRuntimeParticipantResolver(resolver);
  };
}

[[nodiscard]] std::vector<RoboPlanJointLimitInput>
RoboPlanJointLimitsFromRetainedModel(
    const deployment::UrdfDocumentIr &urdf) {
  std::vector<RoboPlanJointLimitInput> result;
  result.reserve(urdf.joints.size());
  for (const auto &joint : urdf.joints) {
    if (joint.type == deployment::UrdfJointType::kFixed ||
        joint.type == deployment::UrdfJointType::kFloating ||
        joint.type == deployment::UrdfJointType::kPlanar ||
        joint.mimic.has_value()) {
      continue;
    }
    Require(joint.limit.has_value(),
            "retained movable joint '" + joint.name +
                "' has no explicit dynamics limits");
    const auto &limit = *joint.limit;
    Require(std::isfinite(limit.velocity) && limit.velocity > 0.0,
            "retained movable joint '" + joint.name +
                "' has no positive velocity limit");

    RoboPlanJointLimitInput row;
    row.joint_name = joint.name;
    row.maximum_velocity = {limit.velocity};
    if (joint.type == deployment::UrdfJointType::kRevolute ||
        joint.type == deployment::UrdfJointType::kPrismatic) {
      Require(limit.authored_lower.has_value() &&
                  limit.authored_upper.has_value(),
              "retained bounded joint '" + joint.name +
                  "' has incomplete position limits");
      row.minimum_position = std::vector<double>{limit.lower};
      row.maximum_position = std::vector<double>{limit.upper};
    }
    if (limit.acceleration.has_value()) {
      row.maximum_acceleration =
          std::vector<double>{*limit.acceleration};
    }
    result.push_back(std::move(row));
  }
  std::sort(result.begin(), result.end(), [](const auto &left,
                                              const auto &right) {
    return left.joint_name < right.joint_name;
  });
  Require(std::adjacent_find(
              result.begin(), result.end(), [](const auto &left,
                                                const auto &right) {
                return left.joint_name == right.joint_name;
              }) == result.end(),
          "retained model repeats a movable joint limit");
  return result;
}

// The RoboPlan worker reloads this base model by path whenever it restarts;
// the stage that owns those files must outlive every worker generation.
[[nodiscard]] RoboPlanWorkerModelDTO NativeWorkerBaseModel(
    const RetainedPlanningModelStage &stage,
    std::vector<RoboPlanJointLimitInput> joint_limits) {
  return RoboPlanWorkerModelDTO{"manyforge_i6_execution_host_worker",
                                stage.loader_urdf_path(),
                                stage.srdf_path(),
                                stage.package_paths(),
                                std::move(joint_limits),
                                {},
                                {}};
}

// One candidate RoboPlan scene and its exact state projector for the given
// bindings, built from the staged candidate whole-cell planning model. The
// projector registers each target with its state channel's freshness budget
// (one per binding, same order).
struct CandidateRetainedModelScene final {
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
};

template <typename RetainedBundle, typename ModelMap, typename PlanningModel>
[[nodiscard]] CandidateRetainedModelScene BuildCandidateRetainedModelScene(
    std::string_view scene_name, const RetainedPlanningModelStage &stage,
    const PlanningModel &planning_model, const RetainedBundle &bundle,
    const ModelMap &model_map,
    std::vector<core::RuntimeParticipantBinding> bindings,
    std::vector<std::int64_t> freshness_budgets_ns) {
  const auto srdf = planning_model.srdf_bytes();
  const auto documents = deployment::parse_model_documents_for_projection(
      planning_model.urdf_bytes(),
      srdf.empty() ? std::optional<std::string_view>{}
                   : std::optional<std::string_view>{srdf});
  CandidateRetainedModelScene result;
  result.scene = make_roboplan_scene(RoboPlanSceneInput{
      std::string(scene_name), std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(),
      RoboPlanJointLimitsFromRetainedModel(documents.urdf)});
  result.scene_mutex = std::make_shared<std::mutex>();
  result.state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          result.scene, result.scene_mutex, bundle, model_map, planning_model,
          std::move(bindings), std::move(freshness_budgets_ns)));
  return result;
}

struct ManagedComponentProjectionSet final {
  core::ProjectionRevisionDTO projection_revision;
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      by_component;
  // Representatives do not own every provider required by the published model.
  // Keep the complete exact selection alive through build and installation.
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required_projection_pins;
};

[[nodiscard]] std::optional<ManagedComponentProjectionSet>
SelectManagedComponentProjections(const ManagedRuntimeChildConfig &config,
                                  const deployment::RetainedBundle &bundle,
                                  const ManagedProjectionRegistry &projections,
                                  const core::ProjectionRevisionDTO &expected,
                                  const bool allow_current_scene_revision) {
  const auto unavailable = [](const std::string &detail) {
    static std::mutex mutex;
    static std::string last_detail;
    std::lock_guard lock(mutex);
    if (last_detail != detail) {
      last_detail = detail;
      std::cerr << "managed component projection selection: " << detail
                << '\n';
    }
    return std::optional<ManagedComponentProjectionSet>{};
  };
  ManagedComponentProjectionSet selected;
  bool revision_selected = false;
  std::shared_ptr<RetainedPlannerSceneProjection> cell_projection;
  std::set<core::ControllableComponentId> motion_projection_components;
  const auto supported_component = [&](const core::ControllableComponentId &id) {
    return std::any_of(bundle.value().cell_graph.control_bindings.begin(),
                       bundle.value().cell_graph.control_bindings.end(),
                       [&](const auto &binding) {
                         return binding.controllable_component_id == id &&
                                binding.provider_runtime_unit_id ==
                                    core::RuntimeUnitId(config.runtime_unit_id) &&
                                (binding.capability_id.value() ==
                                     "execution.trajectory.execute" ||
                                 binding.capability_id.value() ==
                                     "tool.gripper.command");
                       });
  };
  for (const auto &provider : bundle.value().capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            provider.capability_id, provider.capability_definition_revision)) {
      continue;
    }
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : provider.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      Require(runtime != nullptr && !runtime->empty(),
              "managed joint planner has a malformed runtime endpoint");
      runtime_sources.insert(*runtime);
    }
    if (runtime_sources.size() != 1U ||
        runtime_sources.begin()->value() != config.runtime_unit_id) {
      continue;
    }
    const core::ProjectionExecutionHostBindingDTO binding{
        provider.capability_provider_binding_id,
        provider.capability_provider_binding_revision,
        core::RuntimeUnitId(config.runtime_unit_id),
        core::RuntimeEpoch(config.runtime_epoch)};
    auto revision = expected;
    auto projection = projections.FindActiveProjection(revision, binding);
    if (projection == nullptr && allow_current_scene_revision) {
      const auto current =
          projections.FindCurrentProjectionRevision(expected, binding);
      if (!current.has_value())
        return unavailable("no current revision for " +
                           binding.provider_binding_id.value());
      revision = *current;
      projection = projections.FindActiveProjection(revision, binding);
    }
    if (projection == nullptr)
      return unavailable("no active projection for " +
                         binding.provider_binding_id.value());
    selected.required_projection_pins.push_back(projection);
    if (cell_projection == nullptr)
      cell_projection = projection;
    if (!revision_selected) {
      selected.projection_revision = revision;
      revision_selected = true;
    } else {
      if (selected.projection_revision != revision)
        return unavailable("component projections have different revisions");
    }
    if (const auto *target =
            std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
                &provider.target);
        target != nullptr &&
        supported_component(target->controllable_component_id)) {
      // A motion owner must use its own joint-goal projection. Other native
      // providers may share that component but cannot silently substitute
      // their projection for the planner's bound one.
      if (provider.capability_id.value() == "planning.motion.joint_goal") {
        Require(motion_projection_components
                    .insert(target->controllable_component_id)
                    .second,
                "managed motion component has multiple joint planners");
        selected.by_component[target->controllable_component_id] = projection;
      } else {
        selected.by_component.try_emplace(target->controllable_component_id,
                                          projection);
      }
    }
  }
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id ==
            core::RuntimeUnitId(config.runtime_unit_id) &&
        binding.capability_id.value() == "execution.trajectory.execute" &&
        !motion_projection_components.contains(
            binding.controllable_component_id))
      return unavailable("motion component has no active native projection");
  }
  if (!revision_selected) {
    selected.projection_revision = expected;
    return selected;
  }
  // A gripper with an independent controller need not have a motion planner.
  // Its frame/Scene view is the same cell projection already selected for the
  // motion owners; this does not create a planner or a projection lease for
  // the gripper.
  Require(cell_projection != nullptr,
          "selected native projection has no cell model");
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id !=
            core::RuntimeUnitId(config.runtime_unit_id) ||
        (binding.capability_id.value() != "execution.trajectory.execute" &&
         binding.capability_id.value() != "tool.gripper.command"))
      continue;
    selected.by_component.try_emplace(binding.controllable_component_id,
                                      cell_projection);
  }
  return selected;
}

[[nodiscard]] int
RunManagedRuntimeChild(const ManagedRuntimeChildConfig &config);
[[nodiscard]] RoboPlanCandidateBindingDTO
CandidateBinding(const core::RuntimeParticipantBinding &runtime,
                 const deployment::VerifiedRetainedModelMap &model_map) {
  const auto &value = runtime.value();
  Require(value.model_groups.size() == 1U,
          "motion binding has no sole retained model group");
  const auto &group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &candidate) {
        return candidate.model_instance_id == group.model_instance_id;
      });
  Require(model != value.model_bindings.end(),
          "motion model group has no retained model revision");
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  resources.reserve(value.resources.size());
  compiled_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  auto frames = retained_motion_planner_frames(runtime, &model_map);
  return RoboPlanCandidateBindingDTO{
      value.controllable_component_id, value.command_target_id,
      group.model_instance_id,         model->model_revision,
      group.compiled_group_name,       std::move(resources),
      std::move(compiled_joint_names), std::move(frames)};
}

[[nodiscard]] core::LogicalSceneSnapshotDTO
EmptyScene(const core::CellId &cell_id, const core::SceneRevision &revision) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::LogicalSceneSnapshotDTO snapshot{revision, cell_id,           0U, 0U,
                                         0U,       std::move(policy), {}, {}};
  Require(core::logical_scene_identity::ComputeLogicalSceneContentHash(
              snapshot) == revision.content_hash &&
              snapshot.locally_valid(),
          "active scene revision is not the exact canonical empty scene; "
          "live scene consumption starts at N5");
  return snapshot;
}

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO
ActiveFacts(const deployment::RuntimeProgramTargetBinding &primary,
            std::span<const deployment::RuntimeProgramTargetBinding> supporting,
            const core::RuntimeEpoch &runtime_epoch,
            core::ActiveControlConfigurationId active_configuration_id) {
  Require(primary.desired_route().has_value(),
          "runtime binding has no desired I6 route");
  std::set<core::RuntimeUnitId> runtime_units(
      primary.component().runtime_unit_ids.begin(),
      primary.component().runtime_unit_ids.end());
  if (primary.control_domain().has_value()) {
    runtime_units.insert(primary.control_domain()->runtime_unit_ids.begin(),
                         primary.control_domain()->runtime_unit_ids.end());
  }
  std::map<core::ProviderInstanceId, core::RuntimeUnitId> providers;
  const auto insert_provider = [&](const core::ProviderInstanceId &provider,
                                   const core::RuntimeUnitId &runtime_unit) {
    const auto [found, inserted] = providers.emplace(provider, runtime_unit);
    Require(inserted || found->second == runtime_unit,
            "runtime provider is assigned to conflicting runtime units");
    runtime_units.insert(runtime_unit);
  };
  if (primary.control_binding().has_value()) {
    insert_provider(primary.control_binding()->provider_instance_id,
                    primary.control_binding()->provider_runtime_unit_id);
  }
  if (primary.capability_provider_binding().has_value()) {
    insert_provider(
        primary.capability_provider_binding()->provider_instance_id,
        retained_provider_runtime_unit(*primary.capability_provider_binding()));
  }
  for (const auto &support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      insert_provider(
          support.capability_provider_binding()->provider_instance_id,
          retained_provider_runtime_unit(
              *support.capability_provider_binding()));
    }
  }
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  runtime_epochs.reserve(runtime_units.size());
  for (const auto &runtime_unit : runtime_units) {
    runtime_epochs.push_back({runtime_unit, runtime_epoch});
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(providers.size());
  for (const auto &[provider, runtime_unit] : providers) {
    provider_epochs.push_back({provider, runtime_unit, runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      *primary.desired_route(), std::move(active_configuration_id),
      core::ActiveControlConfigurationGeneration(1U), std::move(runtime_epochs),
      std::move(provider_epochs)};
}

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
BuildControlBindingRepresentatives(
    const deployment::ProgramBinding &direct_control_binding,
    const core::RuntimeParticipantBinding &primary_binding,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const core::RuntimeEpoch &runtime_epoch,
    const core::ActiveControlConfigurationId &active_configuration_id) {
  const auto &primary = primary_binding.value();
  Require(primary.control_path.has_value(),
          "direct-control primary has no exact control path");
  const auto &primary_path = *primary.control_path;
  const auto &targets = direct_control_binding.value().target_role_bindings;
  const auto target =
      std::find_if(targets.begin(), targets.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   primary.controllable_component_id &&
               candidate.command_target_id == primary.command_target_id;
      });
  Require(target != targets.end() &&
              std::find_if(std::next(target), targets.end(),
                           [&](const auto &candidate) {
                             return candidate.controllable_component_id ==
                                        primary.controllable_component_id &&
                                    candidate.command_target_id ==
                                        primary.command_target_id;
                           }) == targets.end(),
          "direct-control physical target is absent or ambiguous");

  std::vector<core::RuntimeParticipantBinding> representatives;
  for (const auto &capability : target->capability_bindings) {
    if (!capability.control_binding.has_value()) {
      continue;
    }
    if (capability.capability_id == primary.capability_id) {
      representatives.push_back(primary_binding);
      continue;
    }
    const auto resolved = deployment::resolve_runtime_program_target(
        direct_control_binding, target->target_role_id,
        capability.capability_id, bundle, model_map);
    auto sealed = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            resolved,
            ActiveFacts(resolved, {}, runtime_epoch, active_configuration_id)));
    if (!sealed.ok()) {
      Fail("direct-control control-binding representative failed "
           "verification: " +
           sealed.error().message);
    }
    representatives.push_back(std::move(sealed.value()));
  }
  std::sort(representatives.begin(), representatives.end(),
            [](const auto &left, const auto &right) {
              return left.value().control_path->control_binding_id <
                     right.value().control_path->control_binding_id;
            });

  const auto &desired = bundle.value().desired_control_configurations;
  const auto configuration =
      std::find_if(desired.begin(), desired.end(), [&](const auto &candidate) {
        return candidate.control_domain_id == primary_path.control_domain_id;
      });
  Require(configuration != desired.end() &&
              std::find_if(std::next(configuration), desired.end(),
                           [&](const auto &candidate) {
                             return candidate.control_domain_id ==
                                    primary_path.control_domain_id;
                           }) == desired.end() &&
              representatives.size() == configuration->binding_routes.size(),
          "direct-control target does not cover its exact desired domain");
  for (const auto &route : configuration->binding_routes) {
    const auto found = std::find_if(
        representatives.begin(), representatives.end(),
        [&](const auto &candidate) {
          const auto &path = *candidate.value().control_path;
          return path.control_binding_id == route.control_binding_id &&
                 path.control_binding_revision ==
                     route.control_binding_revision &&
                 path.control_route_id == route.control_route_id;
        });
    Require(found != representatives.end(),
            "direct-control target omits one desired control binding");
  }
  return representatives;
}

[[nodiscard]] CanonicalValue
ManualControlContractValue(const core::RuntimeParticipantBindingDTO &binding) {
  CanonicalValue::Array supporting_providers;
  supporting_providers.reserve(
      binding.supporting_capability_provider_paths.size());
  for (const auto &path : binding.supporting_capability_provider_paths) {
    supporting_providers.emplace_back(CanonicalValue::Object{
        {"capability_definition_revision",
         CanonicalValue(path.capability_definition_revision.value())},
        {"capability_id", CanonicalValue(path.capability_id.value())},
        {"capability_provider_binding_id",
         CanonicalValue(path.capability_provider_binding_id.value())},
        {"capability_provider_binding_revision",
         CanonicalValue(path.capability_provider_binding_revision.value())},
        {"provider_manifest_id",
         CanonicalValue(path.provider_manifest_id.value())},
        {"provider_manifest_revision",
         CanonicalValue(path.provider_manifest_revision.value())},
    });
  }
  CanonicalValue command_schema(nullptr);
  if (binding.command_schema.has_value()) {
    command_schema = CanonicalValue(CanonicalValue::Object{
        {"command_schema_id",
         CanonicalValue(binding.command_schema->command_schema_id.value())},
        {"command_schema_revision",
         CanonicalValue(
             binding.command_schema->command_schema_revision.value())},
    });
  }
  CanonicalValue control_path(nullptr);
  if (binding.control_path.has_value()) {
    const auto &path = *binding.control_path;
    control_path = CanonicalValue(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(path.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(path.control_binding_revision.value())},
        {"control_interface_contract_id",
         CanonicalValue(path.control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(path.control_interface_revision.value())},
        {"controller_topology_id",
         CanonicalValue(path.controller_topology_id.value())},
        {"stop_evidence_policy_id",
         CanonicalValue(path.stop_evidence_policy_id.value())},
        {"stop_evidence_policy_revision",
         CanonicalValue(path.stop_evidence_policy_revision.value())},
        {"topology_revision", CanonicalValue(path.topology_revision.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"command_schema", std::move(command_schema)},
      {"control_path", std::move(control_path)},
      {"operation_capability_definition_revision",
       CanonicalValue(binding.capability_definition_revision.value())},
      {"operation_capability_id",
       CanonicalValue(binding.capability_id.value())},
      {"supporting_providers", CanonicalValue(std::move(supporting_providers))},
  });
}

[[nodiscard]] CanonicalValue
CanonicalNullableNumber(const std::optional<double> &value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(value.value());
}

[[nodiscard]] CanonicalValue
CanonicalNullableText(const std::optional<std::string> &value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(*value);
}

[[nodiscard]] CanonicalValue BuildManualControlGripperDescriptor(
    const core::RuntimeParticipantBinding &gripper_binding,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::vector<deployment::ros2_control_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents) {
  const auto &gripper = gripper_binding.value();
  Require(gripper.resources.size() == 1U,
          "manual-control descriptor crosses its exact resource inventory");
  const auto &resource = gripper.resources.front();
  CanonicalValue::Array followers;
  if (gripper.control_path.has_value()) {
    const auto *coordinate_map = model_map.find_binding(
        gripper.controllable_component_id, gripper.command_target_id,
        gripper.control_path->control_binding_id);
    Require(coordinate_map != nullptr,
            "manual-control gripper descriptor has no retained coordinate map");
    const auto row = std::find_if(
        coordinate_map->resources.begin(), coordinate_map->resources.end(),
        [&](const auto &candidate) {
          return candidate.resource_id == resource.resource_id;
        });
    Require(row != coordinate_map->resources.end(),
            "manual-control gripper descriptor has no exact resource row");
    Require(resource.source_joint_name.has_value()
                ? !row->coordinates.empty() &&
                      !row->component_local_coordinate_kind.has_value()
                : row->coordinates.empty() &&
                      row->component_local_coordinate_kind ==
                          resource.coordinate_kind,
            "manual-control gripper descriptor crosses coordinate semantics");
    for (const auto &coordinate : row->coordinates) {
      if (!coordinate.derived_mimic) {
        continue;
      }
      followers.emplace_back(CanonicalValue::Object{
          {"compiled_coordinate_name",
           CanonicalValue(coordinate.compiled_joint_name)},
          {"multiplier", CanonicalValue(coordinate.multiplier)},
          {"offset", CanonicalValue(coordinate.offset)},
          {"source_coordinate_name",
           CanonicalValue(coordinate.source_joint_name)},
      });
    }
  }

  std::map<std::string, CanonicalValue> named_presets;
  std::optional<core::ModelInstanceId> gripper_model_instance_id;
  std::optional<std::string> source_group_name;
  if (documents.srdf.has_value() && resource.source_joint_name.has_value()) {
    const auto group = std::find_if(
        gripper.model_groups.begin(), gripper.model_groups.end(),
        [&](const auto &candidate) {
          return std::find(candidate.expanded_compiled_joint_names.begin(),
                           candidate.expanded_compiled_joint_names.end(),
                           resource.compiled_resource_name) !=
                 candidate.expanded_compiled_joint_names.end();
        });
    Require(
        group != gripper.model_groups.end() &&
            std::find_if(
                std::next(group), gripper.model_groups.end(),
                [&](const auto &candidate) {
                  return std::find(
                             candidate.expanded_compiled_joint_names.begin(),
                             candidate.expanded_compiled_joint_names.end(),
                             resource.compiled_resource_name) !=
                         candidate.expanded_compiled_joint_names.end();
                }) == gripper.model_groups.end(),
        "manual-control gripper resource has no unique retained group");
    const auto source_group = std::find_if(
        model_map.symbols().begin(), model_map.symbols().end(),
        [&](const auto &symbol) {
          return symbol.source.model_instance_id == group->model_instance_id &&
                 symbol.source.kind ==
                     deployment::OccurrenceSymbolKind::kSrdfGroup &&
                 symbol.compiled_name == group->compiled_group_name;
        });
    Require(source_group != model_map.symbols().end(),
            "manual-control gripper group has no retained source identity");
    gripper_model_instance_id = group->model_instance_id;
    source_group_name = source_group->source.source_name;
    std::map<std::string, std::string> source_state_names;
    for (const auto &symbol : model_map.symbols()) {
      if (symbol.source.model_instance_id != group->model_instance_id ||
          symbol.source.kind !=
              deployment::OccurrenceSymbolKind::kSrdfGroupState) {
        continue;
      }
      Require(source_state_names
                  .emplace(symbol.compiled_name, symbol.source.source_name)
                  .second,
              "manual-control preset has no unique retained source name");
    }
    std::map<std::string, std::string> source_coordinate_names;
    for (const auto &coordinate : model_map.coordinates()) {
      if (coordinate.model_instance_id != group->model_instance_id) {
        continue;
      }
      Require(source_coordinate_names
                  .emplace(coordinate.compiled_joint_name,
                           coordinate.source_joint_name)
                  .second,
              "manual-control preset coordinate is not unique");
    }
    for (const auto &state : documents.srdf->group_states) {
      if (state.group_name != group->compiled_group_name) {
        continue;
      }
      const auto master = std::find_if(
          state.joints.begin(), state.joints.end(), [&](const auto &joint) {
            return joint.joint_name == resource.compiled_resource_name;
          });
      if (master == state.joints.end()) {
        continue;
      }
      const auto symbol = source_state_names.find(state.name);
      Require(symbol != source_state_names.end(),
              "manual-control preset has no unique retained source name");
      CanonicalValue::Array coordinates;
      coordinates.reserve(state.joints.size());
      for (const auto &joint : state.joints) {
        const auto source = source_coordinate_names.find(joint.joint_name);
        Require(source != source_coordinate_names.end(),
                "manual-control preset names an unknown retained coordinate");
        coordinates.emplace_back(CanonicalValue::Object{
            {"position", CanonicalValue(joint.value)},
            {"source_coordinate_name", CanonicalValue(source->second)},
        });
      }
      Require(
          named_presets
              .emplace(
                  symbol->second,
                  CanonicalValue(CanonicalValue::Object{
                      {"coordinates", CanonicalValue(std::move(coordinates))},
                      {"master_position", CanonicalValue(master->value)},
                      {"name", CanonicalValue(symbol->second)},
                  }))
              .second,
          "manual-control preset source name is not unique");
    }
  }
  CanonicalValue::Array presets;
  presets.reserve(named_presets.size());
  for (auto &entry : named_presets) {
    presets.emplace_back(std::move(entry.second));
  }

  const auto authored_configuration = std::find_if(
      gripper_target_configurations.begin(),
      gripper_target_configurations.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   gripper.controllable_component_id &&
               candidate.command_target_id == gripper.command_target_id;
      });
  const auto has_authored_configuration =
      authored_configuration != gripper_target_configurations.end();
  if (has_authored_configuration) {
    Require(gripper_model_instance_id.has_value() &&
                source_group_name.has_value(),
            "authored gripper configuration has no exact retained model group");
    Require(
        named_presets.contains(authored_configuration->open_state_ref) &&
            named_presets.contains(authored_configuration->closed_state_ref),
        "authored gripper state reference is not a retained SRDF preset");
    // Merge 6 (TCP-D): a link of the gripper's own model or, when it has
    // none of that name, of a tool mounted on the gripper's component.
    const auto require_source_link = [&](const std::string &source_name,
                                         const std::string_view field) {
      const auto links_on = [&](const auto &on_model) {
        return std::count_if(
            model_map.symbols().begin(), model_map.symbols().end(),
            [&](const auto &symbol) {
              return on_model(symbol.source.model_instance_id) &&
                     symbol.source.kind ==
                         deployment::OccurrenceSymbolKind::kLink &&
                     symbol.source.source_name == source_name;
            });
      };
      const auto own = links_on([&](const core::ModelInstanceId &instance) {
        return instance == *gripper_model_instance_id;
      });
      // B4: the model's owner and the mounts come from the cell kinematic
      // index.
      const auto &index = model_map.kinematic_index();
      Require(own == 1 ||
                  (own == 0 &&
                   links_on([&](const core::ModelInstanceId &instance) {
                     const auto *owner = index.owner_of(instance);
                     return owner != nullptr &&
                            *owner != gripper.controllable_component_id &&
                            index.carries(gripper.controllable_component_id,
                                          *owner);
                   }) == 1),
              "authored gripper " + std::string(field) +
                  " is not one exact retained source link");
    };
    require_source_link(authored_configuration->mount_frame, "mount frame");
    require_source_link(authored_configuration->attach_frame, "attach frame");
    require_source_link(authored_configuration->tcp_frame, "TCP frame");
    for (const auto &link : authored_configuration->touch_links) {
      require_source_link(link, "touch link");
    }
    for (const auto &frame : authored_configuration->auxiliary_frames) {
      require_source_link(frame, "auxiliary frame");
    }
  }
  CanonicalValue::Array touch_links;
  CanonicalValue::Array auxiliary_frames;
  std::optional<std::string> description;
  std::optional<std::string> actuation_kind;
  std::optional<std::string> output_mode;
  std::optional<std::string> open_state_ref;
  std::optional<std::string> closed_state_ref;
  std::optional<double> command_duration_s;
  std::optional<std::string> mount_frame;
  std::optional<std::string> attach_frame;
  std::optional<std::string> tcp_frame;
  if (has_authored_configuration) {
    description = authored_configuration->description;
    actuation_kind = authored_configuration->actuation_kind;
    output_mode = authored_configuration->output_mode == core::GripperOutputMode::kNone ? "none" : "commanded";
    open_state_ref = authored_configuration->open_state_ref;
    closed_state_ref = authored_configuration->closed_state_ref;
    command_duration_s = authored_configuration->command_duration_s;
    mount_frame = authored_configuration->mount_frame;
    attach_frame = authored_configuration->attach_frame;
    tcp_frame = authored_configuration->tcp_frame;
    touch_links.reserve(authored_configuration->touch_links.size());
    for (const auto &link : authored_configuration->touch_links) {
      touch_links.emplace_back(link);
    }
    auxiliary_frames.reserve(authored_configuration->auxiliary_frames.size());
    for (const auto &frame : authored_configuration->auxiliary_frames) {
      auxiliary_frames.emplace_back(frame);
    }
  }

  std::optional<std::string> state_authority_id;
  std::optional<std::string> state_authority_class;
  std::optional<std::string> control_interface_contract_id;
  if (gripper.control_path.has_value()) {
    state_authority_id = gripper.control_path->state_authority_id.value();
    state_authority_class = std::string(
        core::to_string(gripper.control_path->state_authority_class));
    control_interface_contract_id =
        gripper.control_path->control_interface_contract_id.value();
  }

  const auto coordinate_kind =
      resource.coordinate_kind == core::CoordinateKind::kAngular ? "angular_rad"
                                                                 : "linear_m";
  CanonicalValue::Object descriptor{
      {"actuation_kind", CanonicalNullableText(actuation_kind)},
      {"output_mode", CanonicalNullableText(output_mode)},
      {"attach_frame", CanonicalNullableText(attach_frame)},
      {"auxiliary_frames", CanonicalValue(std::move(auxiliary_frames))},
      {"closed_state_ref", CanonicalNullableText(closed_state_ref)},
      {"command_target_id", CanonicalValue(gripper.command_target_id.value())},
      {"command_duration_s", CanonicalNullableNumber(command_duration_s)},
      {"command_kind", CanonicalValue("gripper_position")},
      {"component_role_id", CanonicalValue(gripper.component_role_id.value())},
      {"completion_policy", CanonicalValue(actuation_kind == "pneumatic"
          ? (output_mode == "none" || (has_authored_configuration && authored_configuration->pneumatic_io)
                 ? "no_output" : "output_delivery")
          : "goal_fenced_target_reached")},
      {"control_interface_contract_id",
       CanonicalNullableText(control_interface_contract_id)},
      {"contracts", ManualControlContractValue(gripper)},
      {"controllable_component_id",
       CanonicalValue(gripper.controllable_component_id.value())},
      {"coordinate_kind", CanonicalValue(coordinate_kind)},
      {"description", CanonicalNullableText(description)},
      {"grasp_confirmation_policy",
       CanonicalValue(actuation_kind == "pneumatic" ? "not_required" : "binding_feedback_required")},
      {"mimic_followers", CanonicalValue(std::move(followers))},
      {"max_position", gripper_limits.has_value()
                           ? CanonicalValue(gripper_limits->upper_position)
                           : CanonicalValue(nullptr)},
      {"min_position", gripper_limits.has_value()
                           ? CanonicalValue(gripper_limits->lower_position)
                           : CanonicalValue(nullptr)},
      {"mount_frame", CanonicalNullableText(mount_frame)},
      {"named_presets", CanonicalValue(std::move(presets))},
      {"open_state_ref", CanonicalNullableText(open_state_ref)},
      {"schema_version",
       CanonicalValue("manyforge.manual_control_gripper_descriptor.v3")},
      {"source_coordinate_name",
       CanonicalValue(resource.source_joint_name.has_value()
                          ? *resource.source_joint_name
                          : resource.compiled_resource_name)},
      {"source_group_name", CanonicalNullableText(source_group_name)},
      {"source_kind", CanonicalValue(resource.source_joint_name.has_value()
                                         ? "model_role"
                                         : "component_local")},
      {"state_authority_class", CanonicalNullableText(state_authority_class)},
      {"state_authority_id", CanonicalNullableText(state_authority_id)},
      {"target_role_id", CanonicalValue(gripper.target_role_id.value())},
      {"tcp_frame", CanonicalNullableText(tcp_frame)},
      {"touch_links", CanonicalValue(std::move(touch_links))},
      {"transport_kind", CanonicalValue("resolved_binding")},
      {"unit", CanonicalValue(coordinate_kind == std::string_view("angular_rad")
                                  ? "rad"
                                  : "m")},
  };
  // Opt-in typed grasp confirmation: present only when the authored row
  // requires it, so every other descriptor is unchanged.
  if (has_authored_configuration && authored_configuration->grasp_confirmation &&
      authored_configuration->grasp_confirmation->required)
    descriptor.emplace_back("grasp_confirmation", CanonicalValue("required"));
  return CanonicalValue(std::move(descriptor));
}

[[nodiscard]] CanonicalValue BuildManualControlMotionDescriptor(
    const core::RuntimeParticipantBinding &motion_binding,
    const core::JointLimitsDTO &motion_limits,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &motion_participant_policy,
    const std::optional<double> &deployment_maximum_tcp_linear_speed_mps,
    const deployment::execution_runtime_contract::
        OrdinaryMotionJerkSettings *ordinary_jerk_settings,
    const std::vector<deployment::execution_runtime_contract::
                          Configuration::DirectControlJointLimit>
        &direct_control_joint_limits,
    const deployment::VerifiedRetainedModelMap &model_map,
    const std::vector<deployment::DirectControlTcp> &motion_tcps) {
  const auto &motion = motion_binding.value();
  // Keep the selected flange child even when it has no declared TCP entry.
  const auto *end_effector_component = model_map.kinematic_index().tool_on(
      motion.controllable_component_id, "flange");
  Require(
      motion.resources.size() == motion_limits.joint_names.size() &&
          motion.resources.size() == motion_limits.position_lower_rad.size() &&
          motion.resources.size() == motion_limits.position_upper_rad.size() &&
          motion.resources.size() == motion_limits.velocity_max_rad_s.size() &&
          (motion_limits.acceleration_max_rad_s2.empty() ||
           motion.resources.size() ==
               motion_limits.acceleration_max_rad_s2.size()),
      "manual-control descriptor crosses its exact resource inventory");

  CanonicalValue::Array frames;
  frames.reserve(motion.frame_bindings.size());
  for (const auto &frame : motion.frame_bindings) {
    frames.emplace_back(CanonicalValue::Object{
        {"compiled_frame_name", CanonicalValue(frame.compiled_frame_name)},
        {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
        {"model_instance_id", CanonicalValue(frame.model_instance_id.value())},
        {"model_revision", CanonicalValue(frame.model_revision.value())},
    });
  }
  // TCP-C: the TCPs Robot Control can move at (deployment computes the
  // default, owner names and one row per compiled frame).
  CanonicalValue::Array tcps;
  tcps.reserve(motion_tcps.size());
  for (const auto &tcp : motion_tcps) {
    const auto &entry = tcp.binding.entry;
    const auto &frame = tcp.binding.frame;
    CanonicalValue::Array contact_body_roles;
    contact_body_roles.reserve(frame.contact_body_role_ids.size());
    for (const auto &role : frame.contact_body_role_ids)
      contact_body_roles.emplace_back(role.value());
    CanonicalValue::Object tcp_row{
        {"default", CanonicalValue(tcp.binding.end_effector)},
        {"display", CanonicalValue(tcp.display)},
        {"entry", CanonicalValue(CanonicalValue::Object{
                      {"component_id", CanonicalValue(entry.component_id.value())},
                      {"target_id", CanonicalValue(entry.target_id.value())},
                      {"frame_role_id", CanonicalValue(entry.frame_role_id.value())},
                  })},
        {"frame", CanonicalValue(CanonicalValue::Object{
                      {"compiled_frame_name", CanonicalValue(frame.compiled_frame_name)},
                      {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
                      {"model_instance_id", CanonicalValue(frame.model_instance_id.value())},
                      {"model_revision", CanonicalValue(frame.model_revision.value())},
                      {"contact_body_role_ids", CanonicalValue(std::move(contact_body_roles))},
                  })},
        {"mounted_elsewhere", CanonicalValue(tcp.mounted_elsewhere)},
        {"mounted_here", CanonicalValue(tcp.mounted_here)},
    };
    // Interim legacy endpoint role for exact rows already carried by the
    // direct-control binding; A4 removes it with the old endpoint selector.
    if (tcp.target_role_id.has_value())
      tcp_row.emplace_back("target_role_id",
                           CanonicalValue(tcp.target_role_id->value()));
    tcps.emplace_back(CanonicalValue(std::move(tcp_row)));
  }
  CanonicalValue::Array joints;
  joints.reserve(motion.resources.size());
  for (std::size_t index = 0U; index < motion.resources.size(); ++index) {
    const auto &resource = motion.resources[index];
    Require(resource.source_joint_name.has_value(),
            "manual-control motion descriptor has a non-model coordinate");
    Require(motion_limits.joint_names[index] == resource.compiled_resource_name,
            "manual-control motion descriptor crosses its exact joint order");
    joints.emplace_back(CanonicalValue::Object{
        {"compiled_joint_name",
         CanonicalValue(resource.compiled_resource_name)},
        {"max_acceleration_rad_s2",
         motion_limits.acceleration_max_rad_s2.empty()
             ? CanonicalValue(nullptr)
             : CanonicalValue(motion_limits.acceleration_max_rad_s2[index])},
        {"max_position_rad",
         CanonicalValue(motion_limits.position_upper_rad[index])},
        {"max_velocity_rad_s",
         CanonicalValue(motion_limits.velocity_max_rad_s[index])},
        {"min_position_rad",
         CanonicalValue(motion_limits.position_lower_rad[index])},
        {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
        {"unit", CanonicalValue("rad")},
    });
  }
  CanonicalValue operational_limit(nullptr);
  if (deployment_maximum_tcp_linear_speed_mps.has_value()) {
    operational_limit = CanonicalValue(CanonicalValue::Object{
        {"maximum_tcp_linear_speed_mps",
         CanonicalValue(*deployment_maximum_tcp_linear_speed_mps)},
    });
  }
  CanonicalValue::Array operational_joint_limits;
  CanonicalValue::Array ordinary_jerk_joints;
  operational_joint_limits.reserve(motion.resources.size());
  for (std::size_t index = 0U; index < motion.resources.size(); ++index) {
    const auto &resource = motion.resources[index];
    const auto authored = std::find_if(
        direct_control_joint_limits.begin(), direct_control_joint_limits.end(),
        [&](const auto &candidate) {
          return candidate.controllable_component_id ==
                     motion.controllable_component_id &&
                 candidate.command_target_id == motion.command_target_id &&
                 resource.source_joint_name.has_value() &&
                 candidate.source_joint_name == *resource.source_joint_name;
        });
    std::optional<double> authored_velocity;
    std::optional<double> authored_acceleration;
    std::optional<double> authored_jerk;
    if (authored != direct_control_joint_limits.end()) {
      authored_velocity = authored->max_velocity_rad_s;
      authored_acceleration = authored->max_acceleration_rad_s2;
      authored_jerk = authored->max_jerk_rad_s3;
    }
    const auto model_acceleration =
        motion_limits.acceleration_max_rad_s2.empty()
            ? std::optional<double>{}
            : std::optional<double>{
                  motion_limits.acceleration_max_rad_s2[index]};
    auto effective_acceleration = model_acceleration;
    if (authored_acceleration.has_value() &&
        (!effective_acceleration.has_value() ||
         authored_acceleration.value() < effective_acceleration.value())) {
      effective_acceleration = authored_acceleration;
    }
    auto effective_velocity = motion_limits.velocity_max_rad_s[index];
    if (authored_velocity.has_value()) {
      effective_velocity =
          std::min(effective_velocity, authored_velocity.value());
    }
    auto effective_jerk = CanonicalNullableNumber(authored_jerk);
    if (motion_participant_policy &&
        motion_participant_policy->value().ordinary_motion_jerk) {
      const auto &profile =
          *motion_participant_policy->value().ordinary_motion_jerk;
      if (!profile.enabled) {
        effective_jerk = CanonicalValue(nullptr);
      } else {
        const double ceiling =
            std::get<core::AngularJerkDTO>(
                profile.limits.at(*resource.source_joint_name))
                .value_rad_s3;
        effective_jerk = CanonicalValue(
            authored_jerk ? std::min(*authored_jerk, ceiling) : ceiling);
      }
    }
    if (ordinary_jerk_settings != nullptr) {
      const auto &settings = *ordinary_jerk_settings;
      const auto *profile =
          motion_participant_policy &&
                  motion_participant_policy->value().ordinary_motion_jerk
              ? &*motion_participant_policy->value().ordinary_motion_jerk
              : nullptr;
      if (settings.enabled) {
        Require(profile != nullptr && profile->enabled,
                "enabled ordinary jerk descriptor requires its resolved policy");
      } else {
        effective_jerk = CanonicalValue(nullptr);
      }
      const auto setting = std::find_if(
          settings.joint_limits.begin(), settings.joint_limits.end(),
          [&](const auto &row) {
            return row.source_joint_name == *resource.source_joint_name;
          });
      const char *source =
          !settings.enabled ? "off"
          : setting != settings.joint_limits.end() && setting->override_rad_s3
              ? "override"
              : "preset";
      ordinary_jerk_joints.emplace_back(CanonicalValue::Object{
          {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
          {"source", CanonicalValue(source)},
          {"maximum_rad_s3",
           settings.enabled
               ? CanonicalValue(
                     std::get<core::AngularJerkDTO>(
                         profile->limits.at(*resource.source_joint_name))
                         .value_rad_s3)
               : CanonicalValue(nullptr)},
          {"preset_rad_s3",
           model_acceleration
               ? CanonicalValue(core::motion_jerk_preset_limit(
                     *model_acceleration, settings.preset_reference_time_s))
               : CanonicalValue(nullptr)},
      });
    }
    operational_joint_limits.emplace_back(CanonicalValue::Object{
        {"deployment_max_acceleration_rad_s2",
         CanonicalNullableNumber(authored_acceleration)},
        {"deployment_max_jerk_rad_s3", CanonicalNullableNumber(authored_jerk)},
        {"deployment_max_velocity_rad_s",
         CanonicalNullableNumber(authored_velocity)},
        {"effective_max_acceleration_rad_s2",
         CanonicalNullableNumber(effective_acceleration)},
        {"effective_max_jerk_rad_s3", std::move(effective_jerk)},
        {"effective_max_velocity_rad_s", CanonicalValue(effective_velocity)},
        {"model_max_acceleration_rad_s2",
         CanonicalNullableNumber(model_acceleration)},
        {"model_max_velocity_rad_s",
         CanonicalValue(motion_limits.velocity_max_rad_s[index])},
        {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
    });
  }
  for (const auto &authored : direct_control_joint_limits) {
    if (authored.controllable_component_id !=
            motion.controllable_component_id ||
        authored.command_target_id != motion.command_target_id) {
      continue;
    }
    Require(std::any_of(motion.resources.begin(), motion.resources.end(),
                        [&](const auto &resource) {
                          return resource.source_joint_name.has_value() &&
                                 *resource.source_joint_name ==
                                     authored.source_joint_name;
                        }),
            "direct-control joint limit names an unknown source joint");
  }
  CanonicalValue motion_descriptor(CanonicalValue::Object{
      {"command_target_id", CanonicalValue(motion.command_target_id.value())},
      {"component_role_id", CanonicalValue(motion.component_role_id.value())},
      {"contracts", ManualControlContractValue(motion)},
      {"controllable_component_id",
       CanonicalValue(motion.controllable_component_id.value())},
      {"end_effector_component_id",
       end_effector_component == nullptr
           ? CanonicalValue(nullptr)
           : CanonicalValue(end_effector_component->value())},
      {"frames", CanonicalValue(std::move(frames))},
      {"joints", CanonicalValue(std::move(joints))},
      {"operational_joint_limits",
       CanonicalValue(std::move(operational_joint_limits))},
      {"operational_limit", std::move(operational_limit)},
      {"ordinary_jerk",
       ordinary_jerk_settings != nullptr
           ? CanonicalValue(CanonicalValue::Object{
                 {"enabled", CanonicalValue(ordinary_jerk_settings->enabled)},
                 {"preset_reference_time_s",
                  CanonicalValue(
                      ordinary_jerk_settings->preset_reference_time_s)},
                 {"joint_limits",
                  CanonicalValue(std::move(ordinary_jerk_joints))}})
           : CanonicalValue(nullptr)},
      {"schema_version",
       CanonicalValue("manyforge.manual_control_motion_descriptor.v6")},
      {"target_role_id", CanonicalValue(motion.target_role_id.value())},
      {"tcps", CanonicalValue(std::move(tcps))},
  });

  return motion_descriptor;
}



/// A view whose component is only its adapter (CONTRACTS §6: e.g. a sensor
/// whose family runs its own process): no execution runtime, gripper or I/O
/// driver, so no Kernel registration, backend or collision resources.
[[nodiscard]] bool IsAdapterOnlyView(const Composition &view) {
  return view.component_adapter && !view.runtime && !view.gripper_runtime &&
         !(view.io_provider_drivers && !view.io_provider_drivers->empty());
}

[[nodiscard]] std::map<core::ControllableComponentId, const Composition *>
IndexNativeComponentCompositions(const Composition &cell) {
  std::map<core::ControllableComponentId, const Composition *> result;
  const auto include = [&](const Composition &composition) {
    const bool io = composition.io_provider_drivers && !composition.io_provider_drivers->empty();
    Require(composition.component_runtime_identity() != nullptr &&
                (composition.runtime || composition.gripper_runtime || io ||
                 IsAdapterOnlyView(composition)) &&
                !(composition.runtime && composition.gripper_runtime),
            "native component composition has no exact runtime owner");
    if (io) {
      Require(composition.component_runtime_pins &&
                  composition.component_runtime_pins->matches(composition.runtime,
                      composition.gripper_runtime, composition.io_provider_drivers),
              "component lifetime pin omits or replaces an actual member");
      Require(composition.bundle != nullptr, "I/O owner has no retained product");
      std::vector<core::ControlDomainId> actual;
      for (const auto& driver : *composition.io_provider_drivers) {
        Require(driver && driver->domain().locally_valid() &&
                    driver->domain().provider()->provider.controllable_component_id ==
                        composition.controllable_component_id,
                "I/O driver group has another component or invalid actual occurrence");
        actual.push_back(driver->domain().control_domain_id);
        const auto expected = std::find_if(composition.bundle->value().cell_graph.control_domains.begin(),
            composition.bundle->value().cell_graph.control_domains.end(), [&](const auto& domain) {
              return domain.control_domain_id == driver->domain().control_domain_id;
            });
        Require(expected != composition.bundle->value().cell_graph.control_domains.end() &&
                    expected->provider() && expected->control_domain_revision == driver->domain().control_domain_revision &&
                    *expected->provider() == driver->domain().provider()->provider,
                "I/O driver group differs from its exact sealed domain membership");
      }
      std::vector<core::ControlDomainId> expected;
      for (const auto& domain : composition.bundle->value().cell_graph.control_domains)
        if (domain.provider() && domain.provider()->controllable_component_id == composition.controllable_component_id)
          expected.push_back(domain.control_domain_id);
      Require(actual == expected && std::adjacent_find(actual.begin(), actual.end()) == actual.end(),
              "I/O driver group omits or repeats an actual component domain");
    }
    Require(result.emplace(composition.controllable_component_id,
                           &composition).second,
            "native cell contains duplicate component runtime owners");
  };
  include(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr,
            "native cell contains a missing component runtime owner");
    include(*participant);
  }
  return result;
}

// Exact existing scalar owner/configuration facts distinguish an intentionally
// absent physical launch from a lost Commanded/arm process. Never infer None
// from a null process, a target name, or unavailable feedback.
[[nodiscard]] bool IsLocalNoOutputGripperOwner(const Composition& owner) {
  if (owner.runtime || !owner.gripper_runtime || !owner.direct_control_gripper_binding)
    return false;
  const auto binding = owner.gripper_runtime->binding();
  const auto* backend = owner.gripper_runtime->backend();
  const auto& target = binding.value().command_target_id.value();
  return backend && binding.value().controllable_component_id ==
                        owner.direct_control_gripper_binding->value().controllable_component_id &&
         binding.value().command_target_id ==
             owner.direct_control_gripper_binding->value().command_target_id &&
         backend->gripper_actuation_kind(target) == core::GripperActuationKind::kPneumatic &&
         backend->gripper_output_mode(target) == core::GripperOutputMode::kNone;
}

struct VerifiedLocalNoneProcessOwner final {
  std::shared_ptr<RetainedGripperRuntime> runtime;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  core::RuntimeParticipantBinding binding;
  component_manager::ManagerComponentProcessOccurrenceDTO occurrence;
};

[[nodiscard]] VerifiedLocalNoneProcessOwner ReadVerifiedLocalNoneProcessOwner(
    const Composition& owner, const deployment::SiteFacts& site_facts,
    const component_manager::ManagerComponentProcessInventoryRequestDTO& request) {
  Require(IsLocalNoOutputGripperOwner(owner) && !owner.controller_process &&
              owner.bundle && owner.cell_runtime && owner.ros2_control_runtime_handle &&
              owner.active_addressed_kernel() == &owner.cell_runtime->addressed_kernel(),
          "local None process occurrence has no exact installed logical owner");
  const auto registration = owner.gripper_runtime->registration();
  const auto& binding = registration.binding;
  const auto& value = binding.value();
  const auto* backend = owner.gripper_runtime->backend();
  const auto* actual_backend = dynamic_cast<const ros2_control_execution::Ros2GripperCommandBackend*>(backend);
  // Each crossing named in the refusal (which fact differs).
  std::string crossing;
  const auto check = [&crossing](const bool holds, const char *fact) {
    if (!holds) crossing += std::string(crossing.empty() ? "" : ",") + fact;
  };
  check(actual_backend != nullptr, "backend");
  check(value.control_path.has_value(), "control_path");
  check(registration.engine == owner.gripper_runtime->participant_engine(), "engine");
  check(registration.stop_execution_port == owner.gripper_runtime->participant_engine(),
        "stop_port");
  // Its Stop readiness is sealed when its route opens (Enable): a prepared,
  // not yet enabled gripper has none, and is still this installed owner (a
  // New or Load right after a Load must inventory it, R3).
  check(!registration.stop_readiness.has_value() ||
            registration.stop_readiness->value().expected_controller_domain.physical_expectation ==
                core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone,
        "stop_readiness");
  check(owner.ros2_control_runtime_handle->OwnsGripper(backend, binding), "handle_owner");
  check(value.retained_bundle_id == request.predecessor_projection_revision.retained_bundle_id,
        "retained_bundle");
  check(value.graph_revision == request.predecessor_projection_revision.graph_revision,
        "graph_revision");
  check(value.runtime_unit_epochs == std::vector<core::RuntimeUnitEpochBindingDTO>{
                                         {request.runtime_unit_id, request.runtime_epoch}},
        "runtime_unit_epoch");
  Require(crossing.empty(),
          "local None process occurrence crosses its actual binding/engine/Stop owner (" +
              crossing + ")");
  const auto logical = actual_backend->gripper_transport_readiness(value.command_target_id.value());
  Require(logical.configured && logical.logical_owner_ready &&
              logical.output_mode == core::GripperOutputMode::kNone,
          "local None process occurrence has no current configured logical backend");
  const auto& path = *value.control_path;
  // CONTRACTS §15: its route opens only after Enable; the occurrence is the
  // installed logical owner itself.
  component_manager::ManagerComponentProcessOccurrenceDTO occurrence{
      value.controllable_component_id, {}, 0, component_manager::ProcessPresenceState::kAbsent,
      core::ControllerLaunchCleanupState::kNotRequested, {},
      deployment::derive_component_runtime_identity(*owner.bundle, site_facts,
                                                    value.controllable_component_id),
      component_manager::ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal,
      component_manager::ManagerPneumaticNoneProcessOwnerDTO{
          value.command_target_id, path.control_binding_id, path.control_binding_revision}};
  const auto verified = component_manager::ValidateManagerPneumaticNoneProcessOwner(
      *owner.bundle, site_facts, request, occurrence);
  Require(verified.ok(), verified.ok() ? std::string{} : verified.error().message);
  return {owner.gripper_runtime, owner.ros2_control_runtime_handle, binding,
          std::move(occurrence)};
}

[[maybe_unused]] void
AttachProductPickPlacePlanningOwner(Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.scene_authority_proxy != nullptr &&
              composition.runtime != nullptr &&
              composition.active_addressed_kernel() != nullptr,
          "production pick/place join has no scene or addressed owner");
  if (composition.pick_place_planning_owner != nullptr) {
    return;
  }
  composition.pick_place_planning_owner =
      std::make_unique<core::AddressedPickPlacePlanningOwner>(
          *composition.active_addressed_kernel(),
          *composition.scene_authority_proxy,
          *composition.scene_authority_proxy,
          *composition.planner_scene_projection,
          composition.runtime->planning_scene_synchronizer.get());
}

void InstallDirectSceneFrameResolver(ManagedProjectionRegistry &projections,
                                     Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.bundle != nullptr &&
              composition.scene_frame_resolver != nullptr,
          "command-capable native projection has no direct frame resolver");
  projections.InstallActiveFrameResolverSet(*composition.bundle,
                                            composition.projection_revision,
                                            composition.scene_frame_resolver);
}



std::function<ControllerLaunchReadinessEvidence()>
CachedControllerLaunchReadinessSource(
    const std::shared_ptr<RetainedRos2ControlRuntimeHandle> &handle) {
  return [weak = std::weak_ptr<RetainedRos2ControlRuntimeHandle>(handle)] {
    ControllerLaunchReadinessEvidence facts;
    const auto handle = weak.lock();
    if (!handle)
      return facts;
    const auto activity = handle->arm_activity();
    facts.controller_manager_present = activity.controller_manager_present;
    facts.controller_observed = activity.observed;
    if (activity.observed) {
      facts.controller_loaded = activity.controller_loaded;
      facts.arm_controller_active = activity.controller_active;
      facts.hardware_ready = activity.hardware_active;
    }
    if (const auto port = handle->current_port()) {
      facts.known = true;
      const auto readiness = port->ReadReadiness();
      facts.controller_bound = readiness.controller_bound;
      facts.binding_epoch = readiness.binding_epoch.value();
      const auto sample = port->ReadSample();
      facts.joint_sample_available = sample.has_value();
      if (sample)
        facts.sample_age_ns = std::max<core::TimePoint>(
            0, WallClock{}.now() - sample->local_receipt_stamp_ns);
    }
    return facts;
  };
}


struct RetainedComponentSnapshot final {
  std::shared_ptr<RetainedExecutionHost> runtime;
  std::shared_ptr<RetainedGripperRuntime> gripper_runtime;
  std::shared_ptr<const RetainedPlanningModelStage> stage;
  // The cell planning-model product `stage` was staged from.
  core::RetainedProductDTO planning_model_product;
  std::optional<core::JointLimitsDTO> joint_limits;
  CanonicalValue motion_descriptor;
  CanonicalValue gripper_descriptor;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control_runtime_handle;
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  std::shared_ptr<const IoProviderDriverGroup> io_provider_drivers{};
  std::shared_ptr<const ComponentRuntimePins> component_runtime_pins{};
  /// CONTRACTS §15: a retained component keeps its adapter (state, action
  /// in progress, `last_action`).
  std::shared_ptr<core::IComponentAdapter> component_adapter{};
  /// CONTRACTS §16: an arm member keeps its owner (built with it) and the
  /// owner's handle and family for its I/O members.
  std::optional<core::CoordinatedComponent> owner_component{};
  std::shared_ptr<void> owner_handle{};
  const ComponentAdapterFamily *owner_family{};
  std::optional<std::string> owner_id{};
  std::optional<std::string> member_role{};
};

/// A retained component keeps its membership and, on an arm member, its
/// owner (CONTRACTS §15, §16).
void CarryRetainedMembership(Composition &composition,
                             const RetainedComponentSnapshot &predecessor) {
  composition.owner_component = predecessor.owner_component;
  composition.owner_handle = predecessor.owner_handle;
  composition.owner_family = predecessor.owner_family;
  composition.owner_id = predecessor.owner_id;
  composition.member_role = predecessor.member_role;
}

// CP9: the host's stream registry follows the installed product's
// Deployment overrides and logs its edge-triggered transitions.
void ApplyComponentStreamOverrides(const deployment::RetainedBundle *bundle) {
  const auto health = host_component_stream_health();
  health->SetLog([](const std::string &line) {
    host_controller_launch_supervisor().enqueue_host_log(line);
  });
  const auto applied = health->SetOverrides(
      bundle == nullptr ? std::vector<core::StreamParameterOverrideDTO>{}
                        : deployment::ComponentStreamParameterOverrides(*bundle));
  if (!applied.ok())
    host_controller_launch_supervisor().enqueue_host_log(
        "WARN manyforge component stream overrides refused: " + applied.error().message + "\n");
}

// One explicit native-host lifetime, shared only by resources given this
// provider. No ROS context/participant is initialized for an empty host. The
// provider captures only this small state, never a Composition or host pointer.
[[nodiscard]] ros2_control_execution::Ros2TransportResources::ContextProvider
MakeHostRosContextProvider() {
  struct State final {
    std::mutex mutex;
    std::shared_ptr<rclcpp::Context> context;
  };
  return [state = std::make_shared<State>()] {
    std::lock_guard lock(state->mutex);
    if (!state->context) {
      auto context = std::make_shared<rclcpp::Context>();
      int argc = 0;
      char **argv = nullptr;
      context->init(argc, argv);
      // Publish only a successfully initialized context. No reset, global
      // cache, domain merge or background retry exists here.
      state->context = std::move(context);
    }
    return state->context;
  };
}

/// CONTRACTS §15: one Manager `HostApplyRequest` with what the host derives
/// for it: the Apply fence it holds, the public predecessor and both
/// products' per-component delta.
struct HostApplySelection final {
  core::ApplyId apply_id;
  core::ApplyGeneration apply_generation;
  core::ProjectionRevisionDTO predecessor_projection_revision;
  core::ProjectionRevisionDTO candidate_projection_revision;
  core::ActiveBundlePublicationGeneration expected_predecessor_publication_generation;
  core::ContentDigest expected_predecessor_public_pointer_digest;
  core::ConfigurationFenceId configuration_fence_id;
  core::ConfigurationFenceGeneration configuration_fence_generation;
  core::ConfigurationFenceScopeDTO configuration_fence_scope;
  core::RuntimeEpoch manager_runtime_epoch;
  deployment::ApplyComponentDeltaDTO component_delta;
  std::vector<core::ControllableComponentId> removed_components;
  std::vector<core::ControlDomainId> removed_domains;

  [[nodiscard]] core::ConfigurationFenceStatusQueryDTO fence_query() const {
    return {{apply_id, apply_generation}, configuration_fence_id,
            configuration_fence_generation, configuration_fence_scope,
            manager_runtime_epoch};
  }
  friend bool operator==(const HostApplySelection &, const HostApplySelection &) = default;
};

/// The participation path's input (planning model and scene, Program
/// bindings, runtime participant bindings, route open and re-seal): the
/// bundle, model map and package root live here only. A component build
/// function gets a ComponentBuildInput (ComponentBuildInputOf) instead (FP-2).
struct CompositionBuildInput final {
  std::filesystem::path package_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  core::CellId cell_id;
  std::optional<core::RuntimeUnitId> runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedModelMap model_map;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  std::vector<core::RuntimeParticipantBinding> motion_program_aliases;
  std::shared_ptr<const deployment::ProgramBindingContract> status_program;
  std::shared_ptr<const deployment::ProgramBinding> status_program_binding;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  std::shared_ptr<WallClock> shared_clock;
  std::shared_ptr<SimpleScheduler> shared_scheduler;
  std::filesystem::path manager_run_root;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::optional<core::ControllableComponentId> selected_component_id{};
  std::shared_ptr<RetainedObservationWake> observation_wake{};
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      component_planner_scene_projections{};
  // Immutable Programs generated by Program Apply may live outside the
  // retained Deployment package. This is a read-only point-lookup source.
  std::filesystem::path program_artifact_root{};
  // Present only for a Manager-selected Apply composition. The startup
  // (current) composition has no Apply delta.
  std::optional<deployment::ApplyComponentDeltaDTO> managed_component_delta{};
  std::map<core::ControllableComponentId, RetainedComponentSnapshot>
      retained_component_snapshots{};
  // The selected package root may be either retained descriptor in a managed
  // child. Candidate workers receive whichever exact root was selected as FD 6.
  std::optional<int> retained_package_root_fd{};
  // Manager-opened scene-asset root (managed FD 9), shared by the collision
  // model and the candidate worker. Absent: relative scene meshes fail closed.
  std::optional<int> scene_asset_root_fd{};
  std::string controller_request_context{};
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required_projection_pins{};
  std::optional<core::NamedApplyPublicationTransactionDTO> origin_named_apply{};
  // Root cell selection only. Physical component construction keeps its
  // existing direct-control inputs and component ownership unchanged.
  std::optional<detail::SelectedProgramProducts> selected_program_products{};
  ros2_control_execution::Ros2TransportResources::ContextProvider ros_context_provider{};
  std::shared_ptr<ros2_control_execution::VolatileMotionTrace> motion_trace{};
  std::vector<core::RuntimeRouteConfigurationReadbackDTO> retained_routes{};
  // CP8 step 1: the machine's site facts (`manyforge.site.v1`); a Real
  // component resolves its robot address here at activation.
  std::optional<component_manager::SiteSettings> site_settings{};
  // CONTRACTS §6: the installed adapter families, the host node their
  // adapters use, and the host entries a family's adapters call: the
  // driver restart of a component (its controller launch) and the I/O
  // ingress of the families' I/O updates.
  const ComponentAdapterRegistry *adapter_registry{};
  rclcpp::Node::SharedPtr adapter_node{};
  std::function<core::Result<void>(const std::string &component_id, std::stop_token)>
      restart_driver{};
  ComponentIoUpdateSink io_updates{};
};

[[nodiscard]] core::CellAdmissionSettlementValidator
BuildOwnerSettlementValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal, core::CellId cell_id) {
  if (manager_apply_journal.empty() && logical_scene_database.empty() &&
      program_cycle_journal.empty()) {
    return {};
  }
  return
      [manager_apply_journal = std::move(manager_apply_journal),
       logical_scene_database = std::move(logical_scene_database),
       program_cycle_journal = std::move(program_cycle_journal),
       cell_id = std::move(cell_id)](
          const core::CellAdmissionClaimDTO &claim,
          const core::CellAdmissionClaimReleaseDTO &release) {
        switch (claim.intent.claim_kind) {
        case core::CellAdmissionClaimKindDTO::kProgramCycle:
          return detail::ValidateProgramCycleSettlementReadOnly(
              program_cycle_journal, claim, release);
        case core::CellAdmissionClaimKindDTO::kProgramReset:
          return detail::ValidateProgramResetSettlementReadOnly(
              program_cycle_journal, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoringMutation:
          // Composer local edits own no durable or runtime effect. Kernel's
          // exact claim/release identity validation is therefore the complete
          // settlement proof; requiring a second Composer journal would only
          // duplicate the in-memory workspace owner.
          return core::Result<void>::Ok();
        case core::CellAdmissionClaimKindDTO::kManagerApply:
          return component_manager::
              ValidateManagerApplyCellAdmissionSettlementReadOnly(
                  manager_apply_journal, cell_id, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoredScene:
          return scene::ValidateAuthoredSceneCellAdmissionSettlementReadOnly(
              logical_scene_database, cell_id, claim, release);
        default:
          return core::Result<void>::Err(core::ErrorDTO{
              "invariant_violation",
              "cell-admission owner settlement reader is unavailable",
              {{"reason", "settlement_owner_reader_unavailable"}}});
        }
      };
}

[[nodiscard]] core::CellAdmissionReconcileValidator
BuildOwnerReconciliationValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path program_cycle_journal, core::CellId cell_id) {
  return [manager_apply_journal = std::move(manager_apply_journal),
          program_cycle_journal = std::move(program_cycle_journal),
          cell_id = std::move(cell_id)](
             const std::vector<core::CellAdmissionClaimDTO> &held_claims,
             const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
                 &current_owner_epochs,
             const core::CellAdmissionReconcileDTO &reconcile) {
    std::optional<core::CellAdmissionOwnerRoleDTO> changed_role;
    const auto note_changed_role = [&](const auto role) {
      if (changed_role.has_value() && *changed_role != role) {
        return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
            core::ErrorDTO{
                "invariant_violation",
                "owner reconciliation may replace exactly one role",
                {{"reason",
                  "multiple_owner_role_reconciliation_unsupported"}}});
      }
      changed_role = role;
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Ok({});
    };
    for (const auto &current : current_owner_epochs) {
      const auto requested = std::find_if(
          reconcile.current_owner_epochs.begin(),
          reconcile.current_owner_epochs.end(), [&](const auto &owner) {
            return owner.owner_role == current.owner_role;
          });
      if (requested == reconcile.current_owner_epochs.end() ||
          requested->owner_epoch != current.owner_epoch) {
        const auto noted = note_changed_role(current.owner_role);
        if (!noted.ok())
          return noted;
      }
    }
    for (const auto &requested : reconcile.current_owner_epochs) {
      const auto current =
          std::find_if(current_owner_epochs.begin(), current_owner_epochs.end(),
                       [&](const auto &owner) {
                         return owner.owner_role == requested.owner_role;
                       });
      if (current == current_owner_epochs.end()) {
        const auto noted = note_changed_role(requested.owner_role);
        if (!noted.ok())
          return noted;
      }
    }
    if (!changed_role.has_value()) {
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
          core::ErrorDTO{
              "invariant_violation",
              "owner reconciliation has no supported replacement",
              {{"reason", "owner_role_reconciliation_unsupported"}}});
    }
    switch (*changed_role) {
    case core::CellAdmissionOwnerRoleDTO::kProgramRunner:
      return detail::ValidateProgramRunnerOwnerReconciliationReadOnly(
          program_cycle_journal, held_claims, current_owner_epochs, reconcile);
    case core::CellAdmissionOwnerRoleDTO::kAuthoringOwner:
      return component_manager::
          ValidateManagerAuthoringOwnerReconciliationReadOnly(
              manager_apply_journal, cell_id, held_claims,
              current_owner_epochs, reconcile);
    default:
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
          core::ErrorDTO{
              "invariant_violation",
              "owner role has no durable reconciliation reader",
              {{"reason", "owner_role_reconciliation_unsupported"}}});
    }
  };
}

struct NativePhysicalBindingSet final {
  // Each native component may provide either capability independently. The
  // managed cell aggregates these per-component records; this value must not
  // combine capabilities owned by two different components.
  std::shared_ptr<const deployment::ProgramBinding> motion_program;
  std::shared_ptr<const deployment::ProgramBinding> gripper_program;
  std::optional<core::RuntimeParticipantBinding> motion;
  std::optional<core::RuntimeParticipantBinding> gripper;
  std::vector<core::RuntimeParticipantBinding> representatives;
  std::optional<core::TargetRoleId> status_motion_role;
  std::optional<core::TargetRoleId> status_gripper_role;
};

template <typename PhysicalBindingInput>
[[nodiscard]] NativePhysicalBindingSet ResolveNativePhysicalBindings(
    const PhysicalBindingInput &input,
    const deployment::DirectControlProgramBindingInventory &inventory,
    std::span<const core::RuntimeRouteConfigurationReadbackDTO> retained_routes =
        {}) {
  const auto select_optional_binding =
      [&](const auto &bindings, const std::string_view capability_id) {
        std::shared_ptr<const deployment::ProgramBinding> selected;
        for (const auto &binding : bindings) {
          const auto target = deployment::resolve_runtime_program_target(
              *binding,
              core::TargetRoleId(
                  std::string(deployment::kDirectControlTargetRoleId)),
              core::CapabilityId(std::string(capability_id)), *input.bundle,
              input.model_map);
          if (input.selected_component_id.has_value() &&
              target.component().controllable_component_id !=
                  *input.selected_component_id)
            continue;
          Require(selected == nullptr,
                  "native composition selection has multiple matching targets");
          selected = binding;
        }
        return selected;
      };
  auto motion_program = select_optional_binding(
      inventory.motion, "execution.trajectory.execute");
  std::optional<deployment::RuntimeProgramTargetBinding> motion_target;
  if (motion_program != nullptr) {
    motion_target = deployment::resolve_runtime_program_target(
        *motion_program,
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("execution.trajectory.execute"), *input.bundle,
        input.model_map);
  }
  auto gripper_program = select_optional_binding(inventory.gripper,
                                                 "tool.gripper.command");
  std::optional<deployment::RuntimeProgramTargetBinding> gripper_target;
  if (gripper_program != nullptr) {
    gripper_target = deployment::resolve_runtime_program_target(
        *gripper_program,
        core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("tool.gripper.command"), *input.bundle,
        input.model_map);
  }

  // A component is an arm or a tool, never both (a tool is its own
  // component): one component resolves its motion or its gripper target.
  Require(!(motion_target.has_value() && gripper_target.has_value()),
          "native component binding combines motion and gripper capabilities");
  Require(
      !input.selected_component_id.has_value() || motion_target.has_value() ||
          gripper_target.has_value() ||
          IsProviderOnlyNativeComponent(input.bundle->value().cell_graph,
                                        *input.selected_component_id),
      "selected native component has no supported direct-control capability");

  std::optional<core::TargetRoleId> status_motion_role;
  std::optional<core::TargetRoleId> status_gripper_role;
  if (input.status_program_binding != nullptr) {
    const auto selected_target_for =
        [&](const std::optional<core::TargetRoleId> &role,
                                         const std::string_view capability_id)
        -> std::optional<deployment::RuntimeProgramTargetBinding> {
      if (!role.has_value()) return std::nullopt;
      const auto found = std::find_if(
          input.status_program_binding->value().target_role_bindings.begin(),
          input.status_program_binding->value().target_role_bindings.end(),
          [&](const auto &target) {
            return target.target_role_id == *role &&
                   std::any_of(target.capability_bindings.begin(),
                               target.capability_bindings.end(),
                               [&](const auto &capability) {
                                 return capability.capability_id.value() ==
                                        capability_id;
                               });
          });
      if (found ==
          input.status_program_binding->value().target_role_bindings.end()) {
        return std::nullopt;
      }
      return deployment::resolve_runtime_program_target(
          *input.status_program_binding, *role,
          core::CapabilityId(std::string(capability_id)), *input.bundle,
          input.model_map);
    };
    const auto status_role_for = [&](const auto &physical_target,
                                     const auto &selected_target,
                                     const std::optional<core::TargetRoleId>
                                         &selected_role,
                                     const std::string_view capability_name)
        -> std::optional<core::TargetRoleId> {
      if (!physical_target.has_value())
        return std::nullopt;
      const auto &physical_owner =
          physical_target->component().controllable_component_id;
      const auto &physical_command_target =
          physical_target->target_role().command_target_id;
      if (!selected_target.has_value())
        return core::TargetRoleId(physical_command_target.value());
      const bool selection_addresses_this_component =
          selected_target->component().controllable_component_id ==
              (input.selected_component_id.has_value()
                   ? *input.selected_component_id
                   : physical_owner);
      if (!selection_addresses_this_component) {
        // The selected Program may legitimately use this capability on a
        // different independently owned component. Keep this component's
        // direct-control status address tied to its own verified target.
        return core::TargetRoleId(physical_command_target.value());
      }
      Require(selected_target->component().controllable_component_id ==
                  physical_owner &&
                  selected_role.has_value() &&
                  selected_target->target_role().command_target_id ==
                      physical_command_target,
              "selected Program " + std::string(capability_name) +
                  " target does not match its native component capability");
      return *selected_role;
    };
    const auto selected_motion_target = selected_target_for(
        input.status_motion_target_role_id, "execution.trajectory.execute");
    status_motion_role = status_role_for(
        motion_target, selected_motion_target,
        input.status_motion_target_role_id, "motion");
    const auto selected_gripper_target = selected_target_for(
        input.status_gripper_target_role_id, "tool.gripper.command");
    status_gripper_role = status_role_for(
        gripper_target, selected_gripper_target,
        input.status_gripper_target_role_id, "gripper");
  } else {
    if (motion_target.has_value()) {
      status_motion_role = core::TargetRoleId(
          motion_target->target_role().command_target_id.value());
    }
    if (gripper_target.has_value()) {
      status_gripper_role = core::TargetRoleId(
          gripper_target->target_role().command_target_id.value());
    }
  }

  std::optional<core::RuntimeParticipantBinding> motion;
  if (motion_target.has_value()) {
    std::vector<deployment::RuntimeProgramTargetBinding> motion_support;
    const auto &target_role = motion_program->value().target_role_bindings.front();
    for (const std::string_view capability_id :
         {std::string_view("planning.motion.cartesian_path"),
          std::string_view("planning.motion.joint_goal"),
          std::string_view("planning.motion.pose_goal")}) {
      if (std::any_of(target_role.capability_bindings.begin(),
                      target_role.capability_bindings.end(),
                      [&](const auto &capability) {
                        return capability.capability_id.value() == capability_id;
                      })) {
        motion_support.push_back(deployment::resolve_runtime_program_target(
            *motion_program,
            core::TargetRoleId(
                std::string(deployment::kDirectControlTargetRoleId)),
            core::CapabilityId(std::string(capability_id)), *input.bundle,
            input.model_map));
      }
    }
    Require(std::any_of(motion_support.begin(), motion_support.end(),
                        [](const auto &support) {
                          return support.capability().capability_id.value() ==
                                 "planning.motion.joint_goal";
                        }),
            "direct-control motion binding has no joint planner");
    auto verified_motion = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            *motion_target,
            ActiveFacts(
                *motion_target,
                std::span<const deployment::RuntimeProgramTargetBinding>(
                    motion_support),
                input.runtime_epoch,
                core::ActiveControlConfigurationId("active.i6.native.arm")),
            std::span<const deployment::RuntimeProgramTargetBinding>(
                motion_support)));
    if (!verified_motion.ok()) {
      Fail("direct-control motion participant binding failed verification: " +
           verified_motion.error().message);
    }
    motion = std::move(verified_motion.value());
  }

  std::optional<core::RuntimeParticipantBinding> gripper;
  if (gripper_target.has_value()) {
    auto verified_gripper = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            *gripper_target,
            ActiveFacts(
                *gripper_target,
                std::span<const deployment::RuntimeProgramTargetBinding>{},
                input.runtime_epoch,
                core::ActiveControlConfigurationId(
                    "active.i6.native.gripper"))));
    if (!verified_gripper.ok()) {
      Fail("direct-control gripper participant binding failed verification: " +
           verified_gripper.error().message);
    }
    gripper = std::move(verified_gripper.value());
  }
  std::vector<core::RuntimeParticipantBinding> representatives;
  if (motion.has_value()) {
    auto motion_representatives = BuildControlBindingRepresentatives(
        *motion_program, *motion, *input.bundle, input.model_map,
        input.runtime_epoch,
        core::ActiveControlConfigurationId("active.i6.native.arm"));
    representatives.insert(
        representatives.end(),
        std::make_move_iterator(motion_representatives.begin()),
        std::make_move_iterator(motion_representatives.end()));
  }
  if (gripper.has_value()) {
    auto gripper_representatives = BuildControlBindingRepresentatives(
        *gripper_program, *gripper, *input.bundle, input.model_map,
        input.runtime_epoch,
        core::ActiveControlConfigurationId("active.i6.native.gripper"));
    representatives.insert(
        representatives.end(),
        std::make_move_iterator(gripper_representatives.begin()),
        std::make_move_iterator(gripper_representatives.end()));
  }
  const auto rebase_retained = [&](core::RuntimeParticipantBinding &binding) {
    if (!binding.value().control_path.has_value())
      return;
    const auto &path = *binding.value().control_path;
    const auto retained = std::find_if(
        retained_routes.begin(), retained_routes.end(), [&](const auto &row) {
          return row.active_configuration.control_domain_id ==
                 path.control_domain_id;
        });
    if (retained == retained_routes.end())
      return;
    Require(retained->active_configuration.source_runtime_epoch ==
                path.provider_runtime_epoch,
            "retained native candidate route crosses its provider epoch");
    auto rebound = detail::rebase_runtime_binding_to_predecessor(
        binding, retained->active_configuration);
    if (!rebound.ok())
      Fail("retained native candidate route cannot be rebased: " +
           rebound.error().message);
    binding = std::move(rebound.value());
  };
  if (motion.has_value())
    rebase_retained(*motion);
  if (gripper.has_value())
    rebase_retained(*gripper);
  for (auto &representative : representatives)
    rebase_retained(representative);
  std::sort(representatives.begin(), representatives.end(),
            [](const auto &left, const auto &right) {
      return left.value().control_path->control_binding_id <
             right.value().control_path->control_binding_id;
    });
  return {std::move(motion_program), std::move(gripper_program),
          std::move(motion), std::move(gripper),
          std::move(representatives), std::move(status_motion_role),
          std::move(status_gripper_role)};
}

struct StagedRetainedTcpRebind final {
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  std::vector<RobotTcp> rows;
  core::ProgramBindingId direct_binding;
  std::shared_ptr<const RetainedToolContextSource> source;
  std::shared_ptr<RetainedPlannerSceneProjection> scene_projection;
};

[[nodiscard]] std::vector<StagedRetainedTcpRebind>
StageRetainedTcpRebinds(const CompositionBuildInput &input) {
  std::vector<StagedRetainedTcpRebind> result;
  if (std::none_of(input.retained_component_snapshots.begin(),
                   input.retained_component_snapshots.end(), [](const auto &row) {
        return row.second.runtime && row.second.ros2_control_runtime_handle;
      })) return result;
  const deployment::FilesystemVerifiedCatalogArtifactSource catalogs(input.package_root);
  const auto inventory = deployment::compile_direct_control_program_binding_inventory(
      *input.bundle, input.model_map, catalogs);
  const deployment::FilesystemVerifiedRetainedPlanningModelSource models(input.package_root);
  const auto model = models.get_verified(deployment::RetainedPlanningModelQuery{
      input.bundle->retained_bundle_id(),
      input.bundle->value().compiled_products.planning_collision_model.product_id});
  Require(model.has_value(), "retained TCP candidate model is unavailable");
  const auto srdf = model->srdf_bytes();
  const auto documents = deployment::parse_model_documents_for_projection(
      model->urdf_bytes(), srdf.empty() ? std::optional<std::string_view>{}
                                      : std::optional<std::string_view>{srdf});
  Require(input.runtime_unit_id.has_value(), "retained TCP candidate has no runtime unit");
  const auto configuration = deployment::ros2_control_execution_runtime_contract::
      require_configuration(*input.bundle, *input.runtime_unit_id);
  for (const auto &[component, predecessor] : input.retained_component_snapshots) {
    if (!predecessor.runtime || !predecessor.ros2_control_runtime_handle) continue;
    auto selected = input;
    selected.selected_component_id = component;
    const auto physical = ResolveNativePhysicalBindings(selected, inventory, input.retained_routes);
    Require(physical.motion && physical.motion_program, "retained robot TCP binding is unavailable");
    const auto &motion = physical.motion->value();
    const auto *profile = deployment::ros2_control_execution_runtime_contract::
        find_controller_profile(configuration, component);
    Require(profile != nullptr, "retained robot controller profile is unavailable");
    std::shared_ptr<const RetainedToolContextSource> source;
    auto projection = input.planner_scene_projection;
    if (const auto found = input.component_planner_scene_projections.find(component);
        found != input.component_planner_scene_projections.end()) projection = found->second;
    if (predecessor.ros2_control_runtime_handle->HasToolContext()) {
      const std::weak_ptr<RetainedPlannerSceneProjection> weak = projection;
      source = std::make_shared<const RetainedToolContextSource>(
          documents.urdf,
          retained_tool_context_frames(input.bundle->value().cell_graph, input.model_map,
                                       model->model_source_urdfs(), component, motion.command_target_id),
          [weak] { const auto locked = weak.lock();
                   return locked ? locked->logical_snapshot() : nullptr; });
    }
    result.push_back({predecessor.ros2_control_runtime_handle,
                      DirectControlRobotTcps(*physical.motion_program),
                      physical.motion_program->program_binding_id(), std::move(source),
                      std::move(projection)});
  }
  return result;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input);

/// The planner worker's settings from the host's planner settings.
[[nodiscard]] RoboPlanCandidateComputerConfig
PlannerComputerConfig(const deployment::PlannerSettings &settings) {
  return RoboPlanCandidateComputerConfig{
      .rng_seed = settings.rng_seed,
      // The same step validates the executed curve (kernel config).
      .collision_check_step_rad = settings.collision_check_step_rad,
      .planner_search = settings.search,
      .planner_optimize = settings.optimize,
      .planner_n = settings.n,
      .planner_max_nodes = settings.max_nodes,
      .cartesian_timing = settings.cartesian_timing,
      .motion_timing = settings.motion_timing};
}


[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
ResealRetainedCandidateRegistrations(
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &predecessor_registrations,
    const deployment::RetainedBundle &candidate_bundle,
    const std::filesystem::path &package_root,
    const std::vector<core::RuntimeParticipantBinding> &representatives) {
  const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root);
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  result.reserve(representatives.size());
  for (const auto &binding : representatives) {
    const auto &value = binding.value();
    Require(value.control_path.has_value(),
            "retained native candidate binding has no control route");
    const auto old = std::find_if(
        predecessor_registrations.begin(),
        predecessor_registrations.end(),
        [&](const auto &registration) {
          const auto &before = registration.binding.value();
          return before.controllable_component_id ==
                     value.controllable_component_id &&
                 before.command_target_id == value.command_target_id &&
                 before.capability_id == value.capability_id &&
                 before.control_path.has_value() &&
                 before.control_path->control_binding_id ==
                     value.control_path->control_binding_id;
        });
    Require(old != predecessor_registrations.end(),
            "retained native candidate has no predecessor registration");
    auto registration = *old;
    registration.binding = binding;
    if (old->stop_readiness.has_value()) {
      const auto desired = std::find_if(
          candidate_bundle.value().desired_control_configurations.begin(),
          candidate_bundle.value().desired_control_configurations.end(),
          [&](const auto &row) {
            return row.control_domain_id ==
                   value.control_path->control_domain_id;
          });
      Require(desired !=
                  candidate_bundle.value().desired_control_configurations.end(),
              "retained native candidate has no desired controller domain");
      const auto expected =
          deployment::load_verified_controller_lifecycle_expected_domain(
              candidate_bundle, artifacts, *desired,
              value.control_path->provider_runtime_unit_id,
              value.control_path->provider_runtime_epoch);
      auto readiness =
          core::rebind_runtime_participant_stop_readiness_product(
              *old->stop_readiness, old->binding, binding,
              expected.sealed());
      if (!readiness.ok()) {
        const auto reason = readiness.error().details.find("reason");
        Fail("retained native Stop readiness cannot be re-sealed: " +
             readiness.error().message +
             (reason == readiness.error().details.end()
                  ? std::string{}
                  : " [reason=" + reason->second + "]"));
      }
      registration.stop_readiness = std::move(readiness.value());
    }
    // The takeover budgets are cell context: a retained component takes the
    // candidate's (as NativeCellComponentRegistrations seals the cell's).
    const bool trajectory =
        value.capability_id.value() == "execution.trajectory.execute";
    registration.motion_takeover_preparation_budget_s =
        trajectory ? candidate_bundle.value().motion_takeover_preparation_budget_s
                   : std::nullopt;
    registration.motion_takeover_planning_budget_s =
        trajectory ? candidate_bundle.value().motion_takeover_planning_budget_s
                   : std::nullopt;
    result.push_back(std::move(registration));
  }
  Require(result.size() == representatives.size(),
          "retained native candidate registration set is incomplete");
  const auto predecessor_rows = std::count_if(
      predecessor_registrations.begin(),
      predecessor_registrations.end(),
      [&](const auto &registration) {
        return !representatives.empty() &&
               registration.binding.value().controllable_component_id ==
                   representatives.front().value().controllable_component_id;
      });
  Require(static_cast<std::size_t>(predecessor_rows) == result.size(),
          "retained native candidate cannot discard a predecessor registration");
  return result;
}

[[nodiscard]] RetainedSceneFrameCoordinateAuthority
SceneFrameCoordinateAuthority(
    const deployment::RetainedBundle &bundle,
    const core::RuntimeParticipantBinding &binding) {
  const auto required = RetainedSceneFrameCoordinateAuthority::kRequiredPublication;
  const auto &value = binding.value();
  if (value.capability_id.value() != "tool.gripper.command")
    return required;
  Require(value.control_path.has_value(),
          "scene-frame gripper has no exact retained control path");
  const auto &runtime_id = value.control_path->provider_runtime_unit_id;
  const auto &units = bundle.value().cell_graph.runtime_units;
  const auto runtime = std::find_if(units.begin(), units.end(), [&](const auto &row) {
    return row.runtime_unit_id == runtime_id;
  });
  Require(runtime != units.end(),
          "scene-frame gripper has no retained provider runtime");
  if (runtime->runtime_profile_id.value() !=
      deployment::ros2_control_execution_runtime_contract::kRuntimeProfileId)
    return required;
  const auto configuration =
      deployment::ros2_control_execution_runtime_contract::require_component_configuration(
          bundle, runtime_id, value.controllable_component_id);
  return retained_ros2_control_gripper_actuation_kind(configuration, binding) ==
                 core::GripperActuationKind::kPneumatic
             ? RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic
             : required;
}

[[nodiscard]] RetainedSceneFrameStateSourceInput
SceneFrameSource(const RetainedObservedStateChannel &channel,
                 std::shared_ptr<const core::IObservedStateProvider> provider,
                 const RetainedSceneFrameCoordinateAuthority authority) {
  return {channel.query_context(), channel.source_kind(),
          authority == RetainedSceneFrameCoordinateAuthority::kRequiredPublication
              ? std::move(provider) : nullptr,
          authority};
}

// The frame resolver and geometric-contact input share this exact qualified
// physical source inventory. The numeric alias pins the same channel even
// when its frame authority is deliberately unavailable (pneumatic).
[[nodiscard]] std::vector<host_detail::CellFrameStateSource>
CollectCellFrameStateSources(const Composition &root,
                             const deployment::RetainedBundle &bundle,
                             bool skip_provider_only = false) {
  std::vector<host_detail::CellFrameStateSource> sources;
  const auto include_channel = [&](const auto &channel) {
    if (channel == nullptr)
      return;
    const auto *retained_channel =
        dynamic_cast<const RetainedObservedStateChannel *>(channel.get());
    Require(retained_channel != nullptr,
            "native cell frame source is not route-aware");
    const auto channel_binding = retained_channel->binding();
    const auto &resources = channel_binding.value().resources;
    if (std::none_of(resources.begin(), resources.end(), [](const auto &resource) {
          return resource.source_joint_name.has_value();
        }))
      return;
    Require(std::all_of(resources.begin(), resources.end(), [](const auto &resource) {
              return resource.source_joint_name.has_value();
            }), "native cell frame source mixes local and model coordinates");
    const auto *publication = channel->addressed_publication();
    sources.push_back({SceneFrameSource(*retained_channel,
        std::shared_ptr<const core::IObservedStateProvider>(channel, publication),
        SceneFrameCoordinateAuthority(bundle, channel_binding)),
        std::shared_ptr<const core::ObservedRobotStatePublication>(
            channel, &publication->numeric_publication())});
  };
  const auto include_owner = [&](const Composition &view) {
    if (IsAdapterOnlyView(view)) return;  // no observed state channel
    if (skip_provider_only && !view.runtime && !view.gripper_runtime &&
        view.io_provider_drivers)
      return;
    Require(view.component_runtime_identity() != nullptr,
            "native cell frame resolver has no component owner");
    if (view.runtime != nullptr) {
      include_channel(view.runtime->motion_state_channel);
    } else {
      include_channel(view.gripper_runtime->state_channel());
    }
  };
  include_owner(root);
  for (const auto &peer : root.participant_compositions)
    include_owner(*peer);
  return sources;
}


[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input) {
  Require(input.selected_component_id.has_value() &&
              input.runtime_unit_id.has_value() &&
              input.status_program == nullptr &&
              input.status_program_binding == nullptr &&
              input.bundle != nullptr,
          "retained native component view is not a managed physical target");
  const auto predecessor = input.retained_component_snapshots.find(
      *input.selected_component_id);
  Require(predecessor != input.retained_component_snapshots.end() &&
              (predecessor->second.runtime != nullptr ||
               predecessor->second.gripper_runtime != nullptr) &&
              predecessor->second.stage != nullptr,
          "retained native component has no pinned predecessor owner");
  Require(input.planner_scene_projection != nullptr ||
              predecessor->second.runtime == nullptr,
          "retained motion controller has no candidate Scene projection");
  if (predecessor->second.gripper_runtime != nullptr) {
    Require(predecessor->second.runtime == nullptr,
            "retained native component has two controller owners");
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
        input.package_root);
    const auto inventory =
        deployment::compile_direct_control_program_binding_inventory(
            *input.bundle, input.model_map, catalog_store);
    auto physical = ResolveNativePhysicalBindings(
        input, inventory, input.retained_routes);
    // A Deployment Apply that retains the gripper re-seals its binding into
    // the candidate bundle (new bundle and graph revision); the physical
    // controller must stay the same, as for a retained arm.
    Require(!physical.motion.has_value() && physical.gripper.has_value() &&
                physical.gripper_program != nullptr &&
                retained_bindings_preserve_physical_owner_after_product_reseal(
                    predecessor->second.gripper_runtime->binding(),
                    *physical.gripper),
            "retained gripper changed its physical capability or controller");
    auto candidate_registrations =
        ResealRetainedCandidateRegistrations(
            predecessor->second.gripper_runtime->registrations(),
            *input.bundle, input.package_root, physical.representatives);
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto retained_model = source.get_verified(
        deployment::RetainedPlanningModelQuery{
            input.bundle->retained_bundle_id(),
            input.bundle->value().compiled_products.planning_collision_model
                .product_id});
    Require(retained_model.has_value() && input.collision_shared_model != nullptr,
            "retained gripper has no verified candidate model");
    const auto old_channel = std::dynamic_pointer_cast<RetainedObservedStateChannel>(
        predecessor->second.gripper_runtime->state_channel());
    Require(old_channel != nullptr,
            "retained gripper has no observed-state channel");
    // A model-backed gripper projects its observations into a scene of the
    // whole-cell model. When that model changes, its candidate projector is
    // prepared here and replaces the predecessor's under the Apply fence.
    auto gripper_stage = predecessor->second.stage;
    std::shared_ptr<RetainedKinematicStateProjector> candidate_tool_projector;
    if (old_channel->kinematic_projector() != nullptr &&
        predecessor->second.planning_model_product !=
            input.bundle->value().compiled_products.planning_collision_model) {
      auto candidate_stage = std::make_shared<const RetainedPlanningModelStage>(
          RetainedPlanningModelStage::create_verified(
              *retained_model, input.execution_run_root));
      auto candidate_scene = BuildCandidateRetainedModelScene(
          "manyforge_i6_native_gripper_host", *candidate_stage,
          *retained_model, *input.bundle, input.model_map,
          {*physical.gripper}, {old_channel->query_context().freshness_budget_ns});
      const auto limits =
          predecessor->second.gripper_runtime->position_limits();
      const auto &resource = physical.gripper->value().resources.front();
      const auto joint =
          candidate_scene.scene->getJointInfo(resource.compiled_resource_name);
      Require(limits.has_value() && joint.has_value() &&
                  joint->limits.min_position.size() == 1U &&
                  joint->limits.max_position.size() == 1U &&
                  limits->coordinate_kind == resource.coordinate_kind &&
                  limits->lower_position == joint->limits.min_position[0] &&
                  limits->upper_position == joint->limits.max_position[0],
              "candidate cell model changes the retained gripper limits");
      candidate_tool_projector = std::move(candidate_scene.state_projector);
      gripper_stage = std::move(candidate_stage);
    }
    std::vector<RetainedSceneFrameStateSourceInput> state_sources;
    const auto gripper_binding =
        predecessor->second.gripper_runtime->binding();
    const auto &resources = gripper_binding.value().resources;
    if (std::any_of(resources.begin(), resources.end(),
                    [](const auto &resource) {
                      return resource.source_joint_name.has_value();
                    })) {
      state_sources.push_back(SceneFrameSource(*old_channel,
          std::shared_ptr<const core::IObservedStateProvider>(
              old_channel, old_channel->addressed_publication()),
          SceneFrameCoordinateAuthority(*input.bundle, *physical.gripper)));
    }
    auto frame_resolver = RetainedSceneFrameResolver::create_verified(
        input.collision_shared_model, *input.bundle, input.model_map,
        *retained_model, *input.runtime_unit_id, input.runtime_epoch,
        std::move(state_sources), input.selected_component_id);
    Require(frame_resolver != nullptr,
            "retained gripper has no candidate frame resolver");
    const std::vector<core::ProgramBindingId> direct_binding_ids{
        physical.gripper_program->program_binding_id()};
    auto target_resolver =
        std::make_shared<ProductBackedRuntimeParticipantResolver>(
            input.package_root, input.bundle, input.model_map,
            std::vector<core::RuntimeParticipantBinding>{*physical.gripper},
            direct_binding_ids, input.program_artifact_root);
    RetainedRuntimeParticipantResolverFactory resolver_factory =
        MakeCandidateParticipantResolverFactory(
            [package_root = input.package_root, bundle = input.bundle,
             model_map = input.model_map,
             program_artifact_root = input.program_artifact_root,
             direct_binding_ids](
                std::vector<core::RuntimeParticipantBinding> bindings) {
              return std::make_shared<ProductBackedRuntimeParticipantResolver>(
                  package_root, bundle, model_map, std::move(bindings),
                  direct_binding_ids, program_artifact_root);
            });
    auto composition = std::make_unique<Composition>(
        *input.selected_component_id, std::move(input.bundle), nullptr, nullptr,
        nullptr, std::move(physical.gripper_program), std::nullopt,
        std::move(physical.gripper), std::nullopt,
        CanonicalValue(nullptr), predecessor->second.gripper_descriptor,
        std::move(gripper_stage), nullptr,
        std::move(input.projection_revision),
        input.publication_generation, std::nullopt,
        physical.status_gripper_role);
    composition->gripper_runtime = predecessor->second.gripper_runtime;
    composition->ros2_control_runtime_handle = predecessor->second.ros2_control_runtime_handle;
    composition->component_adapter = predecessor->second.component_adapter;
    CarryRetainedMembership(*composition, predecessor->second);
    // The retained gripper keeps its controller process, as a retained arm does.
    composition->controller_process = predecessor->second.controller_process;
    composition->controller_request_context = input.controller_request_context;
    composition->candidate_tool_state_projector =
        std::move(candidate_tool_projector);
    composition->target_resolver = std::move(target_resolver);
    composition->candidate_control_representatives =
        std::move(physical.representatives);
    composition->candidate_participant_resolver_factory =
        std::move(resolver_factory);
    composition->candidate_component_registrations =
        std::move(candidate_registrations);
    composition->scene_frame_resolver = std::move(frame_resolver);
    composition->planner_scene_projection =
        std::move(input.planner_scene_projection);
    composition->required_projection_pins = std::move(input.required_projection_pins);
    if (composition->planner_scene_projection != nullptr) {
      composition->scene_authority_proxy =
          std::make_shared<ManagerSceneAuthorityProxy>(
              std::move(input.manager_run_root), composition->planner_scene_projection);
    }
    return composition;
  }
  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical = ResolveNativePhysicalBindings(
      input, inventory, input.retained_routes);
  Require(physical.motion.has_value() &&
              physical.status_motion_role.has_value(),
          "retained native controller changed its physical capability set");
  auto candidate_registrations =
      ResealRetainedCandidateRegistrations(
          predecessor->second.runtime->participant_registrations,
          *input.bundle, input.package_root,
          physical.representatives);

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  const auto retained_model = planning_source.get_verified(
      deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          input.bundle->value().compiled_products.planning_collision_model
              .product_id});
  Require(retained_model.has_value() && input.collision_shared_model != nullptr,
          "retained native view has no verified candidate model");
  // Runtime identity excludes the shared cell planning model (other
  // components, passive models, root poses). The retained worker plans in
  // that whole-cell model, so a changed product stages the candidate model
  // here and replaces the worker's model under the Apply fence.
  // The same holds for every in-process model consumer (scene, state
  // projector, kinematics, validator, collision projector): all of them are
  // prepared here from the candidate model and the candidate bindings that
  // the re-seal installs, and replace the predecessor generation together.
  auto planning_stage = predecessor->second.stage;
  std::optional<RoboPlanWorkerModelDTO> candidate_planner_model;
  std::shared_ptr<const RetainedKinematicModelGeneration>
      candidate_model_generation;
  if (predecessor->second.planning_model_product !=
      input.bundle->value().compiled_products.planning_collision_model) {
    auto candidate_stage = std::make_shared<const RetainedPlanningModelStage>(
        RetainedPlanningModelStage::create_verified(*retained_model,
                                                    input.execution_run_root));
    const auto candidate_srdf = retained_model->srdf_bytes();
    const auto candidate_documents =
        deployment::parse_model_documents_for_projection(
            retained_model->urdf_bytes(),
            candidate_srdf.empty()
                ? std::optional<std::string_view>{}
                : std::optional<std::string_view>{candidate_srdf});
    candidate_planner_model = NativeWorkerBaseModel(
        *candidate_stage,
        RoboPlanJointLimitsFromRetainedModel(candidate_documents.urdf));
    std::vector<core::RuntimeParticipantBinding> projected_bindings{
        physical.motion.value()};
    std::vector<std::int64_t> projected_budgets{
        predecessor->second.runtime->motion_state_channel->query_context()
            .freshness_budget_ns};
    auto candidate_scene = BuildCandidateRetainedModelScene(
        "manyforge_i6_execution_host", *candidate_stage,
        *retained_model, *input.bundle, input.model_map,
        std::move(projected_bindings), std::move(projected_budgets));
    auto mounted_frame_names = retained_motion_planner_frame_names(
        physical.motion.value(), &input.model_map);
    candidate_model_generation = make_retained_motion_model_generation(
        physical.motion.value(), std::move(candidate_scene.scene),
        std::move(candidate_scene.scene_mutex),
        std::move(candidate_scene.state_projector),
        std::move(mounted_frame_names));
    planning_stage = std::move(candidate_stage);
  }
  std::vector<RetainedSceneFrameStateSourceInput> state_sources;
  const auto add_state_source = [&](const auto &channel) {
    if (channel == nullptr)
      return;
    const auto channel_binding = channel->binding();
    const auto &resources = channel_binding.value().resources;
    if (std::none_of(resources.begin(), resources.end(),
                     [](const auto &resource) {
                       return resource.source_joint_name.has_value();
                     }))
      return;
    Require(std::all_of(resources.begin(), resources.end(),
                        [](const auto &resource) {
                          return resource.source_joint_name.has_value();
                        }),
            "retained native state channel mixes model and local resources");
    state_sources.push_back(SceneFrameSource(*channel,
        std::shared_ptr<const core::IObservedStateProvider>(
            channel, channel->addressed_publication()),
        SceneFrameCoordinateAuthority(*input.bundle, channel_binding)));
  };
  add_state_source(predecessor->second.runtime->motion_state_channel);
  auto frame_resolver = RetainedSceneFrameResolver::create_verified(
      input.collision_shared_model, *input.bundle, input.model_map,
      *retained_model, *input.runtime_unit_id, input.runtime_epoch,
      std::move(state_sources), input.selected_component_id);
  Require(frame_resolver != nullptr &&
              frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "retained native candidate frame resolver has another product");

  std::vector<core::ProgramBindingId> direct_binding_ids{
      physical.motion_program->program_binding_id()};
  std::vector<core::RuntimeParticipantBinding> physical_bindings{
      physical.motion.value()};
  auto target_resolver =
      std::make_shared<ProductBackedRuntimeParticipantResolver>(
          input.package_root, input.bundle, input.model_map,
          std::move(physical_bindings), direct_binding_ids,
          input.program_artifact_root);
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      MakeCandidateParticipantResolverFactory(
          [package_root = input.package_root, bundle = input.bundle,
           model_map = input.model_map,
           program_artifact_root = input.program_artifact_root,
           direct_binding_ids = std::move(direct_binding_ids)](
              std::vector<core::RuntimeParticipantBinding> bindings) {
            return std::make_shared<ProductBackedRuntimeParticipantResolver>(
                package_root, bundle, model_map, std::move(bindings),
                direct_binding_ids, program_artifact_root);
          });
  auto composition = std::make_unique<Composition>(
      *input.selected_component_id, std::move(input.bundle), nullptr, nullptr,
      std::move(physical.motion_program), nullptr,
      std::move(physical.motion.value()), std::nullopt,
      predecessor->second.joint_limits, predecessor->second.motion_descriptor,
      CanonicalValue(nullptr), std::move(planning_stage),
      predecessor->second.runtime, std::move(input.projection_revision),
      input.publication_generation, std::move(*physical.status_motion_role),
      std::nullopt);
  Require(predecessor->second.ros2_control_runtime_handle != nullptr,
          "retained ROS component has no physical handle owner");
  composition->ros2_control_runtime_handle =
      predecessor->second.ros2_control_runtime_handle;
  composition->component_adapter = predecessor->second.component_adapter;
  CarryRetainedMembership(*composition, predecessor->second);
  composition->StageRobotTcpReader();
  composition->controller_process = predecessor->second.controller_process;
  composition->controller_request_context = input.controller_request_context;
  composition->candidate_planner_model = std::move(candidate_planner_model);
  composition->candidate_model_generation =
      std::move(candidate_model_generation);
  composition->candidate_planner_settings = deployment::planner_settings(
      deployment::cell_context(*composition->bundle), *input.runtime_unit_id);
  composition->target_resolver = std::move(target_resolver);
  composition->candidate_control_representatives =
      std::move(physical.representatives);
  composition->candidate_component_registrations =
      std::move(candidate_registrations);
  composition->candidate_participant_resolver_factory =
      std::move(resolver_factory);
  composition->candidate_collision_publisher = input.collision_publisher;
  composition->candidate_collision_model = input.collision_shared_model;
  composition->candidate_geometric_contact_scene_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  composition->candidate_runtime_collision_engine =
      std::make_shared<collision::ManyForgeCollisionEngine>(
          input.collision_shared_model,
          collision::ManyForgeCollisionEngine::make_shared_scene_cache(
              input.collision_shared_model));
  const auto validation_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  for (std::size_t index = 0;
       index < predecessor->second.runtime->validation_collision_engines.size();
       ++index) {
    composition->candidate_validation_collision_engines.push_back(
        std::make_shared<collision::ManyForgeCollisionEngine>(
            input.collision_shared_model, validation_cache));
  }
  const auto lookahead_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  for (std::size_t index = 0;
       index < predecessor->second.runtime->lookahead_collision_engines.size();
       ++index) {
    composition->candidate_lookahead_collision_engines.push_back(
        std::make_shared<collision::ManyForgeCollisionEngine>(
            input.collision_shared_model, lookahead_cache));
  }
  composition->scene_frame_resolver = std::move(frame_resolver);
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  composition->scene_authority_proxy =
      std::make_shared<ManagerSceneAuthorityProxy>(
          std::move(input.manager_run_root), composition->planner_scene_projection);
  return composition;
}


[[nodiscard]] core::ObservedStateSourceKind
RoutePublicationSource(const core::StateAuthorityClass authority) {
  switch (authority) {
  case core::StateAuthorityClass::kMeasured:
    return core::ObservedStateSourceKind::kJointState;
  case core::StateAuthorityClass::kCommanded:
    return core::ObservedStateSourceKind::kCommandedState;
  case core::StateAuthorityClass::kPredicted:
    return core::ObservedStateSourceKind::kPredictedState;
  }
  throw std::invalid_argument("unknown retained state-authority class");
}

/// CONTRACTS §6, §15 (`open_routes`): after a component's successful
/// `enable`, publish the route of every control domain it owns, built from
/// the bundle's compiled routes (`desired_control_configurations`) and the
/// cell's installed registrations. No route-selection transaction. Each
/// selected binding gets its sealed Stop readiness here (installed
/// registrations carry none), and the route goes through the cell's one
/// publication path (the arm runtime, or in a cell without one the grippers'
/// route owner), which also rebinds its motion/measured sources.
template <typename RouteOwner>
[[nodiscard]] core::Result<void> OpenComponentRoutes(
    const deployment::RetainedBundle &bundle, const std::filesystem::path &package_root,
    const core::ActiveBundlePublicationGeneration publication,
    RouteOwner &route_owner, const RetainedCellRuntime &cell,
    const core::ControllableComponentId &component) {
  using Result = core::Result<void>;
  const auto unavailable = [&](std::string message) {
    return Result::Err({"component_route_unavailable", std::move(message),
                        {{"component_id", component.value()}}});
  };
  const auto &registrations = cell.participant_registrations();
  for (const auto &desired : bundle.value().desired_control_configurations) {
    std::vector<core::RuntimeParticipantBinding> representatives;
    for (const auto &route : desired.binding_routes) {
      const auto found = std::find_if(registrations.begin(), registrations.end(), [&](const auto &row) {
        const auto &path = row.binding.value().control_path;
        return path && path->control_domain_id == desired.control_domain_id &&
               path->control_binding_id == route.control_binding_id &&
               path->control_binding_revision == route.control_binding_revision;
      });
      if (found != registrations.end()) representatives.push_back(found->binding);
    }
    if (representatives.empty() ||
        representatives.front().value().controllable_component_id != component)
      continue;
    if (representatives.size() != desired.binding_routes.size())
      return unavailable("A compiled route of " + component.value() + " has no installed participant.");
    const auto &source = *representatives.front().value().control_path;
    std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
    for (const auto &route : desired.binding_routes)
      routes.push_back({route.control_binding_id, route.control_binding_revision, route.control_route_id});
    std::sort(routes.begin(), routes.end(), core::CanonicalActiveControlConfigurationBindingRouteLess);
    std::map<core::ResourceId, core::KinematicResourceSourceProjectionDTO> projections;
    for (const auto &representative : representatives) {
      const auto &value = representative.value();
      const auto &path = *value.control_path;
      for (const auto &resource : value.resources) {
        auto [found, inserted] = projections.try_emplace(
            resource.resource_id,
            core::KinematicResourceSourceProjectionDTO{
                resource.resource_id, {}, path.provider_instance_id,
                path.provider_manifest_id, path.provider_manifest_revision,
                path.state_authority_id, path.state_authority_class,
                RoutePublicationSource(path.state_authority_class)});
        static_cast<void>(inserted);
        found->second.contributing_control_bindings.push_back(
            {path.control_binding_id, path.control_binding_revision});
      }
    }
    core::ActiveControlConfigurationDTO configuration{
        source.active_control_configuration_id, desired.control_domain_id,
        desired.control_domain_revision, std::move(routes), desired.activation_set_id,
        desired.controller_topology_id, desired.topology_revision,
        source.provider_runtime_unit_id, source.provider_runtime_epoch,
        publication.value(), {}, source.active_control_configuration_generation,
        core::ActiveControlConfigurationStateDTO::kReconciled};
    for (auto &[resource, projection] : projections) {
      static_cast<void>(resource);
      std::sort(projection.contributing_control_bindings.begin(),
                projection.contributing_control_bindings.end(), [](const auto &lhs, const auto &rhs) {
                  return std::tie(lhs.control_binding_id, lhs.control_binding_revision) <
                         std::tie(rhs.control_binding_id, rhs.control_binding_revision);
                });
      configuration.kinematic_resource_source_projections.push_back(std::move(projection));
    }
    // One resolved Stop base plan per selected binding (M7 readiness), sealed
    // for the component's one lifecycle generation (1, as the host reports
    // in `component_lifecycles`).
    const auto physical = route_owner.ReadCurrentPhysicalParticipants(configuration);
    if (!physical.ok()) return Result::Err(physical.error());
    const core::ComponentLifecycleGeneration lifecycle_generation(1U);
    const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(package_root);
    std::vector<RouteReadinessParticipant> participants;
    try {
      const auto expected = deployment::load_verified_controller_lifecycle_expected_domain(
          bundle, artifacts, desired, source.provider_runtime_unit_id,
          source.provider_runtime_epoch);
      for (const auto &binding : physical.value()) {
        auto plan = deployment::resolve_verified_stop_base_plan(bundle, artifacts, expected,
                                                                configuration, binding);
        const auto plan_id = plan.resolved_stop_base_plan_id;
        auto readiness = deployment::seal_runtime_participant_stop_readiness(
            bundle, std::move(plan), lifecycle_generation, expected, binding);
        if (!readiness.ok()) return Result::Err(readiness.error());
        const auto &path = *binding.value().control_path;
        participants.push_back(
            {binding, std::move(readiness.value()),
             {binding.value().controllable_component_id, lifecycle_generation,
              path.control_binding_id, path.control_binding_revision, plan_id}});
      }
    } catch (const std::exception &exception) {
      return unavailable("The Stop readiness of " + component.value() +
                         " could not be resolved: " + exception.what());
    }
    const auto published =
        route_owner.PublishAndReadBackRouteConfiguration(configuration, std::move(participants));
    if (!published.ok()) return Result::Err(published.error());
  }
  return Result::Ok();
}

struct MockResetPositionComponent final {
  std::shared_ptr<RetainedExecutionHost> runtime;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  MockResetPositionTarget target;
};

core::ErrorDTO ResetPositionError(std::string reason, std::string message,
                                 bool effect_possible = false) {
  if (effect_possible)
    message += " Restart the Workbench before trying again; the Reset claim remains held.";
  return {"program_reset_position_unavailable", std::move(message),
          {{"reason", std::move(reason)},
           {"effect_disposition", effect_possible ? "unknown" : "known_no_effect"}}};
}

// The one raw source of the stock writer's acknowledgement: the arm's
// prepared measured source, in the target's joint order. It is never
// commanded-state authority and never feeds the frame/model channel.
core::Result<core::RequiredRobotMeasuredStateSource> CaptureMockResetMeasuredSource(
    const MockResetPositionComponent& component) {
  using Result = core::Result<core::RequiredRobotMeasuredStateSource>;
  const auto found = component.runtime->TryReadMotionMeasuredSource();
  const auto& binding = component.runtime->motion_binding.value();
  if (!found || !found->reader ||
      found->descriptor.publication.query_context.resources.size() != component.target.positions.size())
    return Result::Err(ResetPositionError("reset_position_raw_source_missing", "The Mock arm has no exact raw state source."));
  for (std::size_t i = 0; i < binding.resources.size(); ++i)
    if (found->descriptor.publication.query_context.resources[i] != binding.resources[i].resource_id)
      return Result::Err(ResetPositionError("reset_position_raw_source_order", "The Mock arm raw state order is not its target order."));
  return Result::Ok(*found);
}

core::Result<core::ObservedStateSnapshotDTO> ReadMockResetMeasuredSource(
    const MockResetPositionComponent& component, const core::RequiredRobotMeasuredStateSource& source) {
  return source.reader->current_state(source.descriptor.publication.query_context,
                                      component.runtime->clock->now());
}

// One arm of `owner` whose reset_position writer is configured (Mock,
// Isaac), or why not.
core::Result<MockResetPositionComponent> CaptureMockResetPositionComponent(
    const Composition& owner) {
  using Result = core::Result<MockResetPositionComponent>;
  const auto unavailable = [](const char* reason, const char* message) {
    return Result::Err(ResetPositionError(reason, message));
  };
  if (!owner.bundle || !owner.runtime || !owner.ros2_control_runtime_handle)
    return unavailable("reset_position_arm_owner_missing", "A Deployment arm has no current supported runtime.");
  const auto& runtime = owner.runtime;
  const auto& handle = owner.ros2_control_runtime_handle;
  const auto target = handle->mock_reset_position_target();
  // A simulated arm has no device address (lane real only): its spec takes
  // no site fact.
  const auto identity = deployment::derive_component_runtime_identity(
      *owner.bundle, deployment::SiteFacts{}, owner.controllable_component_id);
  if (!target || !identity || target->component_runtime_identity != identity->value() ||
      target->joint_names != handle->controller_joint_names() || target->positions.empty() ||
      target->joint_names.size() != target->positions.size() ||
      !std::all_of(target->positions.begin(), target->positions.end(), [](double value) { return std::isfinite(value); }))
    return unavailable("reset_position_simulator_unsupported", "Position reset requires an arm whose adapter declares it (Mock, Isaac); real hardware never does.");
  const auto& binding = runtime->motion_binding.value();
  if (!binding.control_path || binding.resources.size() != target->positions.size() ||
      !owner.measured_joint_state_lane || !runtime->motion_state_channel ||
      runtime->motion_state_channel->source_kind() != core::ObservedStateSourceKind::kCommandedState)
    return unavailable("reset_position_binding_unsupported", "A Mock arm has no exact measured initial-position binding.");
  for (std::size_t index = 0; index < binding.resources.size(); ++index) {
    if (binding.resources[index].source_joint_name != std::optional(target->joint_names[index]))
      return unavailable("reset_position_joint_order_mismatch", "The Mock initial-position order differs from its current binding.");
  }
  return Result::Ok({runtime, handle, *target});
}

// The host's part of one `reset_position` (the write's acknowledgement and
// the commanded-state projection), for the component's action and the
// Program's Reset alike. The acknowledgement is one fresh exact sample of the
// hardware: Mock's loopback state (the measured joint states) or, with a
// `command_echo_topic`, its command output (Isaac). `effect_possible` turns
// true once the first switch may have been sent.
MockResetPositionPhaseCallback MockResetPhase(const MockResetPositionComponent& component,
                                              std::shared_ptr<bool> effect_possible,
                                              const std::stop_token stop_token) {
  using Result = core::Result<void>;
  auto baseline = std::make_shared<std::optional<core::ObservedStateSnapshotDTO>>();
  auto echo_baseline = std::make_shared<std::uint64_t>(0U);
  return [component, effect_possible, stop_token, baseline, echo_baseline](
             const MockResetPositionTransition& next,
             const MockResetPositionStepDeadline deadline) -> Result {
    if (next.phase == MockResetPositionPhase::kContaining) return Result::Ok();
    const auto wake = component.handle->mock_reset_observation_wake();
    if (!wake)
      return Result::Err(ResetPositionError("reset_position_observation_missing",
                                            "The arm observation wake is absent.", *effect_possible));
    if (next.phase == MockResetPositionPhase::kSwitchingToWriter && !next.switch_acknowledged)
      *effect_possible = true;  // The following switch may be sent.
    const auto in_scope = [] { return Result::Ok(); };
    const auto project = [&]() -> Result {
      const std::vector<double> zero(component.target.positions.size(), 0.0);
      const auto initialized = component.runtime->motion_state_channel->publish_commanded_initialization_and_project(
          component.target.positions, zero, component.runtime->clock->now());
      if (!initialized.applied())
        return Result::Err(ResetPositionError("reset_position_command_publication_failed",
                                              "The backend commanded initial pose could not be published and projected.", true));
      return Result::Ok();
    };
    if (next.phase == MockResetPositionPhase::kSettingState && component.target.command_echo_topic) {
      if (!next.target_published) {
        const auto echo = component.handle->reset_command_echo();
        *echo_baseline = echo ? echo->sequence : 0U;
        return Result::Ok();
      }
      const auto published_at = component.runtime->clock->now();
      auto acknowledged = WaitForMockResetObservation(*wake, stop_token, deadline, [&]() -> core::Result<bool> {
        const auto echo = component.handle->reset_command_echo();
        return core::Result<bool>::Ok(echo && ExactResetCommandEcho(*echo, component.target.positions,
                                                                    *echo_baseline, published_at));
      }, in_scope);
      if (!acknowledged.ok()) return acknowledged;
      return project();
    }
    if (next.phase == MockResetPositionPhase::kSettingState) {
      const auto source = CaptureMockResetMeasuredSource(component);
      if (!source.ok()) return Result::Err(source.error());
      if (!next.target_published) {
        auto sample = ReadMockResetMeasuredSource(component, source.value());
        if (!sample.ok()) return Result::Err(sample.error());
        *baseline = std::move(sample.value());
        return Result::Ok();
      }
      if (!*baseline)
        return Result::Err(ResetPositionError("reset_position_write_baseline_missing",
                                              "The Mock writer has no pre-publication source receipt.", true));
      const auto published_at = component.runtime->clock->now();
      auto acknowledged = WaitForMockResetObservation(*wake, stop_token, deadline, [&]() -> core::Result<bool> {
        auto sample = ReadMockResetMeasuredSource(component, source.value());
        if (!sample.ok()) return core::Result<bool>::Ok(false);
        return core::Result<bool>::Ok(sample.value().source_epoch == (*baseline)->source_epoch &&
            ExactMockResetMeasuredTarget(sample.value(), component.target.positions,
                (*baseline)->sequence, (*baseline)->source_timestamp_ns, published_at));
      }, in_scope);
      if (!acknowledged.ok()) return acknowledged;
      return project();
    }
    if (next.switch_acknowledged) {
      // A positive response permits association with ordinary CM/action
      // observation. Unknown responses never reach this readback barrier.
      return WaitForMockResetObservation(*wake, stop_token, deadline, [&]() -> core::Result<bool> {
        return core::Result<bool>::Ok(next.phase == MockResetPositionPhase::kSwitchingToWriter
            ? component.handle->mock_reset_writer_active()
            : component.handle->mock_reset_successor_ready());
      }, in_scope);
    }
    return Result::Ok();
  };
}

// Every motion arm of `composition`, each an enabled arm whose controller
// and writer are ready and whose simulation plays (SIM-1); refuses before the
// first effect.
core::Result<std::vector<MockResetPositionComponent>>
CaptureMockResetPositionComponents(Composition& composition,
                                   const std::vector<core::ComponentState>& states) {
  using Result = core::Result<std::vector<MockResetPositionComponent>>;
  const auto unavailable = [](const char* reason, const char* message) {
    return Result::Err(ResetPositionError(reason, message));
  };
  if (!composition.bundle)
    return unavailable("reset_position_deployment_unavailable", "The current Deployment is not available for position reset.");
  std::set<core::ControllableComponentId> required;
  for (const auto& binding : composition.bundle->value().cell_graph.control_bindings) {
    if (binding.capability_id == core::CapabilityId("execution.trajectory.execute"))
      required.insert(binding.controllable_component_id);
  }
  if (required.empty())
    return unavailable("reset_position_no_arm", "The current Deployment has no arm that supports position reset.");
  const auto owners = IndexNativeComponentCompositions(composition);
  std::vector<MockResetPositionComponent> captured;
  for (const auto& component : required) {
    const auto owner = owners.find(component);
    if (owner == owners.end())
      return unavailable("reset_position_arm_owner_missing", "A Deployment arm has no current supported runtime.");
    auto arm = CaptureMockResetPositionComponent(*owner->second);
    if (!arm.ok()) return Result::Err(arm.error());
    const auto state = std::find_if(states.begin(), states.end(), [&](const auto& row) {
      return row.component_id == component.value();
    });
    if (state == states.end() || state->lifecycle != core::ComponentLifecycle::kEnabled)
      return unavailable("reset_position_arm_inactive", "Every Deployment arm must already be enabled to reset its position.");
    // SIM-1: the write can't take effect while the simulation isn't playing.
    if (state->reason && state->reason->code == "simulation_not_playing")
      return unavailable("simulation_not_playing", "The simulation is not playing.");
    const auto port = arm.value().handle->current_port();
    if (!port)
      return unavailable("reset_position_controller_unavailable", "An arm controller is unavailable.");
    const auto readiness = port->ReadReadiness();
    if (!readiness.controller_active || !readiness.controller_bound || !readiness.hardware_active ||
        !arm.value().handle->mock_reset_writer_inactive())
      return unavailable("reset_position_controller_not_active", "An arm must have an active normal controller before position reset.");
    auto source = CaptureMockResetMeasuredSource(arm.value());
    if (!source.ok())
      return Result::Err(ResetPositionError("reset_position_raw_source_unavailable", source.error().message));
    const auto sample = ReadMockResetMeasuredSource(arm.value(), source.value());
    if (!sample.ok())
      return Result::Err(ResetPositionError("reset_position_raw_source_unavailable", sample.error().message));
    captured.push_back(std::move(arm.value()));
  }
  return Result::Ok(std::move(captured));
}

core::Result<void> RunMockResetPosition(
    Composition& composition, const std::vector<core::ComponentState>& states,
    const ProgramResetPositionRequest& request,
    const core::RequestOwnerEpoch& owner_epoch, const std::stop_token stop_token) {
  using Result = core::Result<void>;
  if (!composition.bundle || request.owner_epoch != owner_epoch ||
      request.active_publication_generation != composition.publication_generation ||
      request.retained_bundle_id != composition.bundle->retained_bundle_id() ||
      request.graph_revision != composition.bundle->value().cell_graph.graph_revision ||
      !composition.active_addressed_kernel())
    return Result::Err(ResetPositionError("reset_position_deployment_changed", "The selected Deployment identity is no longer current."));
  auto authorized = composition.active_addressed_kernel()->ValidateProgramResetPositionAuthorization(request.authorization);
  if (!authorized.ok()) {
    auto error = authorized.error();
    error.details["effect_disposition"] = "known_no_effect";
    return Result::Err(std::move(error));
  }
  auto captured = CaptureMockResetPositionComponents(composition, states);
  if (!captured.ok()) return Result::Err(captured.error());
  const auto effect_possible = std::make_shared<bool>(false);
  for (const auto& component : captured.value()) {
    const auto switched = component.handle->ResetMockPosition(
        stop_token, MockResetPhase(component, effect_possible, stop_token));
    if (switched.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded) {
      auto error = switched.error.value_or(ResetPositionError(
          "reset_position_switch_failed", "The position reset did not complete.", *effect_possible));
      error.details["effect_disposition"] = *effect_possible ? "unknown" : "known_no_effect";
      if (*effect_possible) error.message += " Restart the Workbench before trying again; the Reset claim remains held.";
      return Result::Err(std::move(error));
    }
  }
  return Result::Ok();
}

/// The arm's latest fresh measured joints in axis order (its prepared
/// measured source, fed by the joint state broadcaster), or none.
[[nodiscard]] std::function<std::optional<std::vector<double>>()>
MeasuredJointsOf(const std::shared_ptr<RetainedExecutionHost> &arm) {
  if (!arm) return {};
  return [weak = std::weak_ptr<RetainedExecutionHost>(arm)]() -> std::optional<std::vector<double>> {
    const auto runtime = weak.lock();
    if (!runtime) return std::nullopt;
    const auto source = runtime->TryReadMotionMeasuredSource();
    if (!source || !source->reader) return std::nullopt;
    const auto state = source->reader->current_state(source->descriptor.publication.query_context,
                                                     runtime->clock->now());
    if (!state.ok() || !state.value().complete) return std::nullopt;
    std::vector<double> joints;
    for (const auto &position : state.value().positions)
      joints.push_back(std::holds_alternative<core::AngularPositionDTO>(position)
                           ? std::get<core::AngularPositionDTO>(position).value_rad
                           : std::get<core::LinearPositionDTO>(position).value_m);
    return joints;
  };
}

/// CONTRACTS §6: the adapter family a component's model lane row names
/// (`adapter`), from the installed registry.
/// CONTRACTS §6: the adapter families installed in the ament resource index.
[[nodiscard]] ComponentAdapterRegistry LoadInstalledAdapterRegistry() {
  auto loaded = ComponentAdapterRegistry::LoadInstalled();
  Require(loaded.ok(), loaded.ok() ? std::string{} : loaded.error().message);
  return std::move(loaded.value());
}

[[nodiscard]] const ComponentAdapterFamily &ComponentFamily(
    const ComponentAdapterRegistry *registry, const ComponentLane &lane) {
  Require(registry != nullptr, "the host has no adapter registry");
  const auto *family = registry->Find(lane.row.adapter);
  if (family == nullptr)
    throw core::TypedError(core::ErrorDTO{"component_adapter_unknown",
                                          "No adapter is installed for '" + lane.row.adapter + "'",
                                          {{"adapter_family", lane.row.adapter}}},
                           "execution_host");
  return *family;
}

/// CONTRACTS §6, §16: the adapters of one ros2_control component, made by its
/// family through the registry around the ros2_control lane adapter. A
/// family's tool-context hook goes on the component's ros2_control
/// connection with the robot's tool context sources (`tool_context_source`);
/// its display joint source drives the arm's display channel.
///
/// An arm member (`profile.owner_component_id`) runs its machine's process,
/// so its owner is built here first (BOUNDARY §4): with the machine's lane
/// row by that row's family (the owner's handle then goes to the arm and the
/// I/O members), or, on a lane where the machine model has no row, as the
/// host's passive adapter (the arm then is built owner-less).
/// A file-name-safe form of `text` (controller launch artifact folders).
[[nodiscard]] std::string LaunchPathToken(std::string text) {
  for (auto &character : text)
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '-' || character == '_' ||
          character == '.'))
      character = '_';
  return text;
}

/// FP-2 (N17 S4): the host handles a component build function is given: the
/// installed adapter families and the host entries their adapters call, the
/// shared clock and ROS context, the run roots, the runtime identity and the
/// bring-up bound. No bundle, model map or package root.
struct ComponentBuildHost final {
  /// CONTRACTS §6: the installed adapter families, the host node their
  /// adapters use, and the host entries a family's adapters call: the driver
  /// restart of a component (its controller launch) and the I/O ingress of
  /// the families' I/O updates.
  const ComponentAdapterRegistry *adapter_registry{};
  rclcpp::Node::SharedPtr adapter_node;
  std::function<core::Result<void>(const std::string &component_id, std::stop_token)>
      restart_driver;
  ComponentIoUpdateSink io_updates;
  std::shared_ptr<WallClock> shared_clock;
  ros2_control_execution::Ros2TransportResources::ContextProvider ros_context_provider;
  std::shared_ptr<ros2_control_execution::VolatileMotionTrace> motion_trace;
  std::filesystem::path execution_run_root;
  std::filesystem::path manager_run_root;
  /// Where this composition's arm and machine launches write their files (per
  /// bundle and runtime epoch).
  std::filesystem::path launch_artifacts_root;
  std::optional<core::RuntimeUnitId> runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;
  std::string controller_request_context;
  /// LaunchSettleBound: how long a ros2_control launch may take to settle.
  std::chrono::nanoseconds launch_settle_bound{};
  /// A Manager-selected Apply composition starts its ROS backends at install.
  bool defer_ros_start{false};
  /// R5: a generic I/O provider's lifecycle catalog record (its provider
  /// manifest), the installed record the spec's `providers` pin.
  std::function<std::optional<deployment::QualifiedProviderManifest>(
      const core::ProviderManifestId &, const core::ProviderManifestRevision &)>
      provider_manifest;
};

/// FP-2 (N17 S4): everything a component build function (launch, adapters,
/// backend configuration, owner process, I/O, sensors) reads: the component's
/// spec, an arm member's owner's spec (whose fingerprint is in the member's
/// spec) and the host handles. The bundle, model map and package root stay on
/// the participation path (CompositionBuildInput: planning model and scene,
/// Program bindings and TCPs, the runtime participant binding, route open and
/// re-seal), which hands a build function only what it resolved there (a
/// participant binding, a projected initial position, a tool-context source).
struct ComponentBuildInput final {
  deployment::ComponentSpec spec;
  std::optional<deployment::ComponentSpec> owner_spec;
  ComponentBuildHost host;
};

/// M7's bring-up bound (the Manager's `HostApplyTimeout`): the sum of the
/// lifecycle command times of the components this composition installs or
/// removes (every component at start-up); one default when it changes none.
[[nodiscard]] std::chrono::nanoseconds LaunchSettleBound(const CompositionBuildInput &input) {
  const auto budgets = deployment::ros2_control_execution_runtime_contract::
      component_composition_budgets_ns(input.bundle->value().cell_graph);
  std::chrono::nanoseconds total{0};
  if (input.managed_component_delta) {
    for (const auto &row : input.managed_component_delta->components) {
      if (row.disposition == deployment::ApplyComponentDisposition::kRetain) continue;
      const auto found = budgets.find(row.controllable_component_id.value());
      total += std::chrono::nanoseconds(
          found == budgets.end() ? deployment::kDefaultLifecycleBudgetNs : found->second);
    }
  } else {
    for (const auto &[id, budget] : budgets) total += std::chrono::nanoseconds(budget);
  }
  return total > std::chrono::nanoseconds::zero()
             ? total
             : std::chrono::nanoseconds(deployment::kDefaultLifecycleBudgetNs);
}

[[nodiscard]] ComponentBuildHost ComponentBuildHostOf(const CompositionBuildInput &input) {
  ComponentBuildHost host{
      input.adapter_registry,
      input.adapter_node,
      input.restart_driver,
      input.io_updates,
      input.shared_clock,
      input.ros_context_provider,
      input.motion_trace,
      input.execution_run_root,
      input.manager_run_root,
      input.execution_run_root / "controller-launch-artifacts" /
          LaunchPathToken(input.bundle->retained_bundle_id().value()) /
          LaunchPathToken(input.runtime_epoch.value()),
      input.runtime_unit_id,
      input.runtime_epoch,
      input.controller_request_context,
      LaunchSettleBound(input),
      input.managed_component_delta.has_value(),
      {}};
  host.provider_manifest = [manifests = std::make_shared<
                                const deployment::FilesystemVerifiedRetainedProviderManifestSource>(
                                input.package_root),
                            bundle = input.bundle->retained_bundle_id()](
                               const core::ProviderManifestId &id,
                               const core::ProviderManifestRevision &revision) {
    return manifests->get_verified({bundle, id, revision});
  };
  return host;
}

/// The build input of one component of `bundle`: the participation path's
/// one door to the component build functions.
[[nodiscard]] ComponentBuildInput ComponentBuildInputOf(
    const deployment::RetainedBundle &bundle, const deployment::SiteFacts &site_facts,
    ComponentBuildHost host, const core::ControllableComponentId &component) {
  ComponentBuildInput input{deployment::component_spec(bundle, site_facts, component),
                            std::nullopt, std::move(host)};
  if (const auto *profile = deployment::ros2_control_execution_runtime_contract::
          find_controller_profile(input.spec.host_configuration, component);
      profile != nullptr && profile->owner_component_id)
    input.owner_spec = deployment::component_spec(
        bundle, site_facts, core::ControllableComponentId(*profile->owner_component_id));
  return input;
}

[[nodiscard]] ComponentBuildInput ComponentBuildInputOf(
    const CompositionBuildInput &input, const core::ControllableComponentId &component) {
  return ComponentBuildInputOf(*input.bundle, component_manager::SiteFactsOf(input.site_settings),
                               ComponentBuildHostOf(input), component);
}

/// A ros2_control launch: its Prepare first waits for the launch to settle.
void InstallControllerLaunchSettle(RetainedRos2ControlRuntimeHandle &handle,
                                   const std::shared_ptr<ControllerLaunchProcess> &process,
                                   const std::chrono::nanoseconds bound) {
  handle.InstallControllerLaunchSettle(
      [process] { return process->spawners_settled(); }, process->started_at(),
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(bound));
}

/// A started (or kept) controller launch its composition has not adopted
/// yet: discarded if the build fails first.
struct DiscardUnadoptedLaunch final {
  void operator()(ControllerLaunchSupervisor::Ensured *launch) const {
    static_cast<void>(host_controller_launch_supervisor().discard_candidate(*launch));
    delete launch;
  }
};
using PendingLaunch = std::unique_ptr<ControllerLaunchSupervisor::Ensured, DiscardUnadoptedLaunch>;

/// The adapters of one ros2_control component and, on an arm member, its
/// owner (CONTRACTS §6, §16), for its composition to adopt.
struct Ros2ControlComponentAdapters final {
  std::shared_ptr<core::IComponentAdapter> component_adapter;
  std::optional<std::string> owner_id;
  std::optional<std::string> member_role;
  std::optional<core::CoordinatedComponent> owner_component;
  std::shared_ptr<void> owner_handle;
  const ComponentAdapterFamily *owner_family{};
};

void AdoptRos2ControlComponentAdapters(Composition &composition,
                                       Ros2ControlComponentAdapters adapters) {
  composition.component_adapter = std::move(adapters.component_adapter);
  if (adapters.owner_id) composition.owner_id = std::move(adapters.owner_id);
  if (adapters.member_role) composition.member_role = std::move(adapters.member_role);
  if (adapters.owner_component) composition.owner_component = std::move(adapters.owner_component);
  if (adapters.owner_handle) composition.owner_handle = std::move(adapters.owner_handle);
  if (adapters.owner_family) composition.owner_family = adapters.owner_family;
}

/// From the component's build input only (FP-2), on its ros2_control
/// connection (`handle`) and, for an arm, its runtime (display joints,
/// measured joints); an arm member also from its owner's spec.
[[nodiscard]] Ros2ControlComponentAdapters MakeRos2ControlComponentAdapters(
    const ComponentBuildInput &build,
    const std::shared_ptr<RetainedRos2ControlRuntimeHandle> &handle,
    const std::shared_ptr<RetainedExecutionHost> &runtime, const ComponentLane &lane,
    const ComponentAdapterFamily &family, const std::string &address,
    MockResetPositionPhaseCallback reset_phase,
    const std::function<std::shared_ptr<const RetainedToolContextSource>()> &tool_context_source =
        {},
    const std::optional<std::chrono::nanoseconds> goal_result_bound = std::nullopt) {
  const auto &spec = build.spec;
  const auto *owner = build.owner_spec ? &*build.owner_spec : nullptr;
  const auto &input = build.host;
  const auto &component = spec.controllable_component_id;
  Require(input.adapter_node != nullptr,
          "component " + component.value() + " has no adapter node");
  // The compiled lifecycle command time: the deadline of its prepare, enable,
  // disable and recover (an owner's is its own).
  const std::chrono::nanoseconds lifecycle_timeout(spec.lifecycle_command_timeout_ns);
  auto lane_adapter = std::make_shared<Ros2ControlComponentAdapter>(
      handle, lifecycle_timeout, std::move(reset_phase), ComponentStreams(component),
      family.lane != nullptr && family.lane->simulated, goal_result_bound);
  std::function<core::Result<void>(std::stop_token)> restart_driver;
  if (input.restart_driver)
    restart_driver = [restart = input.restart_driver, id = component.value()](
                         const std::stop_token stop) { return restart(id, stop); };
  ComponentAdapterContext context{component,
                                  lane.lane,
                                  lane.row,
                                  lane.value,
                                  "",
                                  nullptr,
                                  controller_runtime_namespace(component.value()),
                                  address,
                                  input.adapter_node,
                                  input.shared_clock,
                                  handle,
                                  std::move(lane_adapter),
                                  MeasuredJointsOf(runtime),
                                  std::move(restart_driver),
                                  input.io_updates,
                                  false,
                                  lifecycle_timeout,
                                  spec.cell_id.value(),
                                  {}};
  Ros2ControlComponentAdapters result;
  const auto *profile = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(spec.host_configuration, component);
  if (owner != nullptr && profile != nullptr && profile->owner_component_id) {
    Require(profile->member_role.has_value() &&
                owner->controllable_component_id.value() == *profile->owner_component_id,
            "member " + component.value() + " has no machine row");
    const auto &machines = owner->host_configuration.machines;
    const auto machine = std::find_if(machines.begin(), machines.end(), [&](const auto &row) {
      return row.controllable_component_id == owner->controllable_component_id;
    });
    Require(machine != machines.end() && owner->execution_host == spec.execution_host,
            "member " + component.value() + " names an owner that isn't in its runtime unit");
    result.owner_id = *profile->owner_component_id;
    result.member_role = *profile->member_role;
    if (machine->machine_lane_row) {
      const auto owner_lane = ParseComponentLane(*profile->owner_component_id, machine->lane, "",
                                                 *machine->machine_lane_row);
      auto owner_context = context;
      owner_context.component_id = owner->controllable_component_id;
      owner_context.row = owner_lane.row;
      owner_context.lane_row = owner_lane.value;
      owner_context.machine_owner = true;
      owner_context.lifecycle_command_timeout =
          std::chrono::nanoseconds(owner->lifecycle_command_timeout_ns);
      auto made_owner = input.adapter_registry->Make(owner_context);
      if (!made_owner.ok()) throw core::TypedError(made_owner.error(), "execution_host");
      Require(made_owner.value().owner_handle != nullptr,
              "owner " + *profile->owner_component_id + " returned no handle for its members");
      result.owner_component =
          core::CoordinatedComponent{*profile->owner_component_id, made_owner.value().component};
      result.owner_handle = made_owner.value().owner_handle;
      result.owner_family = &ComponentFamily(input.adapter_registry, owner_lane);
      context.role = *profile->member_role;
      context.owner = result.owner_handle;
    } else {
      result.owner_component = core::CoordinatedComponent{
          *profile->owner_component_id, std::make_shared<PassiveComponentAdapter>()};
    }
  }
  auto made = input.adapter_registry->Make(context);
  if (!made.ok()) throw core::TypedError(made.error(), "execution_host");
  if (made.value().tool_context) {
    const auto source = tool_context_source ? tool_context_source() : nullptr;
    Require(source != nullptr, "component " + component.value() + " has no tool context to apply");
    handle->InstallToolContext(
        *made.value().tool_context,
        [source](const std::optional<std::string> &tcp_frame) { return source->Compose(tcp_frame); },
        [source](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &scene) {
          return source->ComposePayload(scene);
        },
        [source](const std::optional<std::string> &frame,
                 const core::LogicalSceneSnapshotDTO &scene) {
          return source->Compose(frame, scene);
        });
  }
  if (made.value().display_joints)
    handle->InstallDisplayJointSource(*made.value().display_joints, runtime->motion_state_channel);
  result.component_adapter = std::move(made.value().component);
  return result;
}

/// CONTRACTS §16, BOUNDARY §4: the adapter of a real-lane I/O member (its
/// signals are `component_adapter` rows): made by its owner's family with the
/// owner's handle from the arm composition that built the owner, and its
/// declared signals and bank as its row (`{adapter, bank, inputs, outputs}`).
/// From the member's build input only (FP-2): its device row and its signal
/// rows; with its owner's handle and family (the owner's fingerprint is in
/// the member's spec).
[[nodiscard]] std::shared_ptr<core::IComponentAdapter> MakeIoMemberAdapter(
    const ComponentBuildInput &build, std::shared_ptr<void> owner_handle,
    const ComponentAdapterFamily &owner_family) {
  const auto &spec = build.spec;
  const auto &input = build.host;
  const auto &id = spec.controllable_component_id.value();
  Require(spec.io_device && spec.io_device->owner_component_id && spec.io_device->member_role,
          "I/O component " + id + " has machine signals but no owner");
  const auto &device = *spec.io_device;
  Require(owner_handle != nullptr, "I/O member " + id + " has no owner built with a lane adapter");
  std::vector<deployment::RetainedIoSignalConfiguration> rows;
  for (const auto &row : spec.io_signals)
    if (row.provider.profile == deployment::kComponentAdapterIoProfile) rows.push_back(row);
  std::optional<std::string> bank;
  CanonicalValue::Array inputs;
  CanonicalValue::Array outputs;
  for (const auto &row : rows) {
    const auto found = row.provider.adapter_fields.find("bank");
    const auto *named = found == row.provider.adapter_fields.end()
                            ? nullptr
                            : std::get_if<std::string>(&found->second.value);
    Require(named != nullptr && (!bank || *bank == *named),
            "I/O member " + id + " signals name no single bank");
    bank = *named;
    (row.descriptor.direction == core::IoSignalDirection::kInput ? inputs : outputs)
        .emplace_back(CanonicalValue(row.descriptor.signal_id.name));
  }
  Require(bank.has_value(), "I/O member " + id + " has no signals");
  ComponentLane lane;
  lane.lane = "real";
  lane.row.adapter = std::string(owner_family.family);
  lane.row.adapter_keys.emplace("bank", *bank);
  lane.value = CanonicalValue(CanonicalValue::Object{
      {"adapter", CanonicalValue(lane.row.adapter)},
      {"bank", CanonicalValue(*bank)},
      {"inputs", CanonicalValue(std::move(inputs))},
      {"outputs", CanonicalValue(std::move(outputs))}});
  ComponentAdapterContext context{device.controllable_component_id,
                                  lane.lane,
                                  lane.row,
                                  lane.value,
                                  *device.member_role,
                                  std::move(owner_handle),
                                  controller_runtime_namespace(id),
                                  {},
                                  input.adapter_node,
                                  input.shared_clock,
                                  nullptr,
                                  nullptr,
                                  {},
                                  {},
                                  input.io_updates,
                                  false,
                                  std::chrono::nanoseconds(device.lifecycle_command_timeout_ns),
                                  {},
                                  {}};
  Require(input.adapter_registry != nullptr, "the host has no adapter registry");
  auto made = input.adapter_registry->Make(context);
  if (!made.ok()) throw core::TypedError(made.error(), "execution_host");
  return std::move(made.value().component);
}

/// What a machine owner's process installation leaves for its view.
struct MachineOwnerProcess final {
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  ControllerLaunchSupervisor::Ensured prepared_controller_launch;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  std::shared_ptr<core::IComponentAdapter> component_adapter;
  std::shared_ptr<void> owner_handle;
  const ComponentAdapterFamily *owner_family{};
};

/// CONTRACTS §16: the owner of a machine whose arm is deactivated runs the
/// machine's process itself, on the same ros2_control launch path as an arm:
/// the arm's description and the family's launch contribution without the
/// arm (no arm controllers, hardware left inactive, frames off the cell's
/// TF). Its ros2_control connection observes only the process; the owner is
/// made by its family as on an arm's composition (`machine_owner`), with the
/// connection as its lane adapter, and its handle and family serve its I/O
/// members. The view keeps the process, so the owner's driver restart finds
/// it.
/// From the owner's build input only (FP-2): its machine row, address and
/// lifecycle command time.
[[nodiscard]] MachineOwnerProcess MakeMachineOwnerProcess(const ComponentBuildInput &build) {
  const auto &spec = build.spec;
  const auto &input = build.host;
  const auto &id = spec.controllable_component_id.value();
  const auto &machines = spec.host_configuration.machines;
  const auto row = std::find_if(machines.begin(), machines.end(), [&](const auto &candidate) {
    return candidate.controllable_component_id == spec.controllable_component_id;
  });
  Require(input.adapter_registry != nullptr && input.adapter_node != nullptr &&
              input.shared_clock != nullptr &&
              row != machines.end() && row->machine_lane_row && row->deactivated_arm,
          "machine " + id + " has no process to run");
  const auto &machine = *row;
  const auto &arm = *machine.deactivated_arm;
  const auto owner_lane = ParseComponentLane(id, machine.lane, "", *machine.machine_lane_row);
  const auto arm_lane = ParseComponentLane(id, machine.lane, arm.model_family_id, arm.lane_row);
  std::string address;
  if (machine.device_address_ref) {
    const auto resolved =
        component_manager::ResolveSpecDeviceAddress(spec, *machine.device_address_ref);
    if (!resolved.ok()) throw host_detail::SiteAddressRefusal(resolved.error(), id);
    address = resolved.value();
  }
  LaunchInstance instance;
  instance.artifacts_root = input.launch_artifacts_root;
  instance.controllable_component_id = id;
  instance.model_urdf = arm.description;
  instance.arm_joint_names = arm.joint_names;
  instance.arm_deactivated = true;
  instance.address = address;
  const auto written = write_launch_artifacts(
      arm_lane.row, input.adapter_registry->LaunchContributionOf(arm_lane.row, false), instance);
  const auto ensured = host_controller_launch_supervisor().ensure(
      ros2_control_launch_command(written), written.directory,
      input.controller_request_context.empty()
          ? "Bootstrap machine=" + id + " epoch=" + input.runtime_epoch.value()
          : input.controller_request_context);
  MachineOwnerProcess result;
  result.controller_process = ensured.process;
  result.prepared_controller_launch = ensured;
  std::cerr << "manyforge controller launch: component=" << id << " (arm deactivated) decision="
            << (ensured.decision == ControllerLaunchDecision::kKeep    ? "kept"
                : ensured.decision == ControllerLaunchDecision::kStart ? "started"
                                                                       : "restarted")
            << " pid=" << ensured.process->pid() << std::endl;

  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  const auto bound = handle->BindProcessOnly(
      std::make_unique<ros2_control_execution::Ros2TransportResources>(
          "manyforge_ros2_control_execution_backend", written.runtime_namespace, 1U,
          input.ros_context_provider),
      *input.shared_clock,
      RetainedRos2ControlArmController{
          retained_ros2_control_scoped_name(written.runtime_namespace, kControllerManagerNodeName),
          "", written.hardware_component_name},
      input.shared_clock);
  Require(bound.ok(), bound.ok() ? std::string{} : bound.error().message);
  handle->InstallControllerProcessFaultSource(
      ControllerLaunchSupervisor::fault_source(ensured.process,
                                               CachedControllerLaunchReadinessSource(handle)),
      [process = ensured.process] {
        return core::Result<std::optional<std::string>>::Ok(
            ProjectControllerLaunchFault(process->ReadCachedFaultState()));
      });
  const std::chrono::nanoseconds timeout(spec.lifecycle_command_timeout_ns);
  // Its launch starts within the owner's lifecycle command time (M7's
  // bring-up bound for one component): until then its Prepare awaits the
  // driver's first report instead of restarting it.
  handle->InstallControllerLaunchSettle(
      [process = ensured.process] { return process->spawners_settled(); },
      ensured.process->started_at(),
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout));
  result.handle = handle;

  std::function<core::Result<void>(std::stop_token)> restart_driver;
  if (input.restart_driver)
    restart_driver = [restart = input.restart_driver, id](const std::stop_token stop) {
      return restart(id, stop);
    };
  ComponentAdapterContext context{spec.controllable_component_id,
                                  owner_lane.lane,
                                  owner_lane.row,
                                  owner_lane.value,
                                  "",
                                  nullptr,
                                  written.runtime_namespace,
                                  address,
                                  input.adapter_node,
                                  input.shared_clock,
                                  handle,
                                  std::make_shared<Ros2ControlComponentAdapter>(handle, timeout),
                                  {},
                                  std::move(restart_driver),
                                  input.io_updates,
                                  true,
                                  timeout,
                                  spec.cell_id.value(),
                                  {}};
  auto owner = input.adapter_registry->Make(context);
  if (!owner.ok()) throw core::TypedError(owner.error(), "execution_host");
  Require(owner.value().owner_handle != nullptr,
          "owner " + id + " returned no handle for its members");
  result.component_adapter = owner.value().component;
  result.owner_handle = owner.value().owner_handle;
  result.owner_family = &ComponentFamily(input.adapter_registry, owner_lane);
  return result;
}

void AdoptMachineOwnerProcess(Composition &view, MachineOwnerProcess process,
                              const std::string &request_context) {
  view.controller_process = std::move(process.controller_process);
  view.controller_request_context = request_context;
  view.prepared_controller_launch = std::move(process.prepared_controller_launch);
  view.ros2_control_runtime_handle = std::move(process.handle);
  view.component_adapter = std::move(process.component_adapter);
  view.owner_handle = std::move(process.owner_handle);
  view.owner_family = process.owner_family;
}

/// CONTRACTS §6 component process: a component's own `ros2 launch` under the
/// host's supervisor (lane kComponent), started by its adapter family.
/// Destroying it stops the process.
class HostComponentProcess final : public IComponentProcess {
 public:
  explicit HostComponentProcess(std::shared_ptr<ControllerLaunchProcess> process)
      : process_(std::move(process)) {}
  ~HostComponentProcess() override {
    try {
      static_cast<void>(host_controller_launch_supervisor().retire_exact(process_));
    } catch (const std::exception &error) {
      std::cerr << "manyforge component process stop failed: component="
                << process_->command().controllable_component_id << " error=" << error.what()
                << std::endl;
    }
  }
  [[nodiscard]] std::optional<std::string> exited() override { return process_->exited(); }

 private:
  std::shared_ptr<ControllerLaunchProcess> process_;
};

/// The host's component process launcher of one component: its process
/// output and ROS logs go below `artifacts_root`. Throws
/// ControllerLaunchProcessError when the launch can't start.
[[nodiscard]] ComponentProcessLauncher HostComponentProcessLauncher(
    std::string component_id, std::filesystem::path artifacts_root) {
  return [component_id = std::move(component_id), artifacts_root = std::move(artifacts_root)](
             const ComponentProcessCommand &request) -> std::unique_ptr<IComponentProcess> {
    ControllerLaunchCommand command;
    command.controllable_component_id = component_id;
    command.lane = ControllerLaunchLane::kComponent;
    command.argv = {"ros2", "launch", request.package, request.launch_file};
    command.argv.insert(command.argv.end(), request.arguments.begin(), request.arguments.end());
    std::ostringstream identity;
    identity << "component\n" << component_id << '\n';
    for (const auto &argument : command.argv) identity << argument << '\n';
    command.identity = identity.str();
    std::filesystem::create_directories(artifacts_root);
    auto ensured = host_controller_launch_supervisor().ensure(command, artifacts_root,
                                                              "Prepare component=" + component_id);
    std::cerr << "manyforge component process launch: component=" << component_id
              << " pid=" << ensured.process->pid() << std::endl;
    return std::make_unique<HostComponentProcess>(std::move(ensured.process));
  };
}

/// The adapter of one sensor (its `sensors` row), from its build input only (FP-2):
/// made by the family its model's adapter row names, with the row's
/// Deployment settings set over the adapter row (the effective values). A
/// sensor has no lane (`context.lane` empty).
[[nodiscard]] std::shared_ptr<core::IComponentAdapter> MakeSensorAdapter(
    const ComponentBuildInput &build) {
  const auto &spec = build.spec;
  const auto &input = build.host;
  const auto *found = deployment::ros2_control_execution_runtime_contract::find_sensor(
      spec.host_configuration, spec.controllable_component_id);
  const auto &id = spec.controllable_component_id.value();
  Require(found != nullptr, "sensor " + id + " has no sensor row");
  const auto &sensor = *found;
  const auto row = content_identity::parse_canonical_json(sensor.adapter_row);
  const auto settings = content_identity::parse_canonical_json(sensor.settings);
  Require(row.is_object() && settings.is_object(),
          "sensor " + id + " has no object adapter row or settings");
  CanonicalValue::Object merged = row.as_object();
  for (const auto &[key, value] : settings.as_object()) {
    const auto found = std::find_if(merged.begin(), merged.end(),
                                    [&](const auto &member) { return member.first == key; });
    if (found != merged.end())
      found->second = value;
    else
      merged.emplace_back(key, value);
  }
  const auto lane = ParseComponentLane(
      id, "", "", content_identity::canonical_json(CanonicalValue(std::move(merged))));
  ComponentAdapterContext context{sensor.controllable_component_id,
                                  lane.lane,
                                  lane.row,
                                  lane.value,
                                  "",
                                  nullptr,
                                  controller_runtime_namespace(id),
                                  {},
                                  input.adapter_node,
                                  input.shared_clock,
                                  nullptr,
                                  nullptr,
                                  {},
                                  {},
                                  input.io_updates,
                                  false,
                                  std::chrono::nanoseconds(spec.lifecycle_command_timeout_ns),
                                  spec.cell_id.value(),
                                  HostComponentProcessLauncher(
                                      id, input.execution_run_root /
                                              "controller-launch-artifacts" / id)};
  Require(input.adapter_registry != nullptr, "the host has no adapter registry");
  auto made = input.adapter_registry->Make(context);
  if (!made.ok()) throw core::TypedError(made.error(), "execution_host");
  return std::move(made.value().component);
}

/// The composition of one component in the installed cell (the root or a
/// participant), or null. Caller holds `access.mutex`.
[[nodiscard]] Composition *FindComponentComposition(const ExecutionCompositionAccess &access,
                                                    const std::string &component_id) {
  if (access.current == nullptr) return nullptr;
  if (access.current->controllable_component_id.value() == component_id) return access.current;
  for (const auto &peer : access.current->participant_compositions)
    if (peer->controllable_component_id.value() == component_id) return peer.get();
  return nullptr;
}

/// CONTRACTS §6 driver-restart entry: stops the component's controller launch
/// (which runs its driver) and starts it again on the same command; the
/// component's ros2_control connection then reads the new process's exit.
[[nodiscard]] core::Result<void> RestartComponentDriver(ExecutionCompositionAccess &access,
                                                        const std::string &component_id) {
  const auto failed = [&](std::string message) {
    return core::Result<void>::Err(core::ErrorDTO{"component_driver_restart_failed",
                                                  std::move(message),
                                                  {{"component_id", component_id}}});
  };
  std::shared_ptr<ControllerLaunchProcess> process;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  {
    std::shared_lock lock(access.mutex);
    const auto *composition = FindComponentComposition(access, component_id);
    if (composition == nullptr || !composition->controller_process ||
        !composition->ros2_control_runtime_handle)
      return failed("The component has no driver process to restart");
    process = composition->controller_process;
    handle = composition->ros2_control_runtime_handle;
  }
  ControllerLaunchSupervisor::Ensured restarted;
  try {
    restarted = host_controller_launch_supervisor().restart(
        process, "Driver restart component=" + component_id);
  } catch (const std::exception &error) {
    return failed(std::string("The driver could not be restarted: ") + error.what());
  }
  {
    std::unique_lock lock(access.mutex);
    if (auto *composition = FindComponentComposition(access, component_id);
        composition != nullptr && composition->controller_process == process)
      composition->controller_process = restarted.process;
  }
  handle->ReplaceControllerProcessFaultSource(
      ControllerLaunchSupervisor::fault_source(restarted.process,
                                               CachedControllerLaunchReadinessSource(handle)),
      [next = restarted.process] {
        return core::Result<std::optional<std::string>>::Ok(
            ProjectControllerLaunchFault(next->ReadCachedFaultState()));
      });
  // A Prepare waits for the restarted launch's spawners, not the stopped one's.
  handle->ReplaceControllerLaunchSettle(
      [next = restarted.process] { return next->spawners_settled(); },
      restarted.process->started_at());
  return core::Result<void>::Ok();
}

/// CONTRACTS §6 I/O update sink: an adapter family's I/O updates enter the
/// installed cell's I/O ingress.
void PublishComponentIoUpdates(const ExecutionCompositionAccess &access,
                               const std::string &component_id,
                               const std::vector<core::IoSignalUpdateDTO> &updates,
                               const std::int64_t received_at_ns) {
  std::shared_ptr<HostIoSignalRuntime> runtime;
  {
    std::shared_lock lock(access.mutex);
    const auto *product = access.product();
    if (product != nullptr) runtime = product->io_signal_runtime;
  }
  if (runtime) runtime->PublishAdapterUpdates(component_id, updates, received_at_ns);
}

/// CONTRACTS §12: one arm's launch instance, from its spec only (FP-2) and
/// its resolved address. Its controller profile row carries the compiled
/// description (with the static frames the model URDF lacks: offset frames,
/// custom TCPs, mounted tools, so robot_state_publisher publishes them on
/// /tf_static), the arm joints and the operator Stop decelerations.
[[nodiscard]] LaunchInstance ArmLaunchInstance(
    const deployment::ComponentSpec &spec, const ComponentAdapterFamily &family,
    const std::vector<double> &initial_joint_positions_rad, const std::string &address,
    std::filesystem::path artifacts_root) {
  const auto *profile = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(spec.host_configuration, spec.controllable_component_id);
  Require(profile != nullptr, "ROS component has no controller profile in its runtime unit");
  LaunchInstance instance;
  instance.artifacts_root = std::move(artifacts_root);
  instance.controllable_component_id = spec.controllable_component_id.value();
  // The instance views the description, which the caller's spec keeps.
  instance.model_urdf = profile->description;
  instance.arm_joint_names = profile->joint_names;
  // Mock and Isaac start at the Deployment's initial pose; a vendor's driver
  // reports its own state and gets no seed.
  if (family.lane != nullptr && family.lane->seeds_initial_pose)
    instance.initial_joint_positions_rad = initial_joint_positions_rad;
  // Q48: the Deployment's operator Stop deceleration per arm joint (every lane).
  instance.max_deceleration_on_cancel_rad_s2 = profile->max_deceleration_on_cancel_rad_s2;
  instance.address = address;
  instance.frame_prefix = profile->frame_prefix;
  instance.component_runtime_identity = deployment::component_spec_fingerprint(spec).value();
  return instance;
}

/// An arm's launch choices, from its build input only (FP-2): its controller
/// profile, its lane row, the family that row names (CONTRACTS §6, §12: a lane
/// family, Mock or Isaac, brings its own execution configuration; a vendor
/// driver on the generic Real configuration reports its own state) and its
/// resolved address.
struct ArmLaunchSelection final {
  const deployment::ros2_control_execution_runtime_contract::Configuration::ControllerProfile
      *profile{};
  ComponentLane lane;
  const ComponentAdapterFamily *family{};
  std::string address;
};

[[nodiscard]] ArmLaunchSelection SelectArmLaunch(const ComponentBuildInput &build) {
  const auto &component = build.spec.controllable_component_id;
  ArmLaunchSelection arm;
  arm.profile = deployment::ros2_control_execution_runtime_contract::find_controller_profile(
      build.spec.host_configuration, component);
  Require(arm.profile != nullptr, "ROS component has no controller profile in its runtime unit");
  arm.lane = SelectedComponentLane(*arm.profile);
  arm.family = &ComponentFamily(build.host.adapter_registry, arm.lane);
  if (arm.profile->device_address_ref.has_value()) {
    // CP8 step 1: the robot's address is a site fact, resolved in its spec.
    const auto address =
        component_manager::ResolveSpecDeviceAddress(build.spec, *arm.profile->device_address_ref);
    if (!address.ok()) throw host_detail::SiteAddressRefusal(address.error(), component.value());
    arm.address = address.value();
  }
  return arm;
}

/// CONTRACTS §12: an arm's launch, written per activation and component from
/// its model lane row (`components[].lane` selects the model's
/// `lanes.<lane>`), the lane or vendor adapter's contribution and the
/// instance values, with its evidence record.
[[nodiscard]] ControllerLaunchArtifacts WriteArmLaunch(
    const ComponentBuildInput &build, const ArmLaunchSelection &arm,
    const std::vector<double> &initial_joint_positions_rad) {
  const auto instance = ArmLaunchInstance(build.spec, *arm.family, initial_joint_positions_rad,
                                          arm.address, build.host.launch_artifacts_root);
  auto written = write_launch_artifacts(
      arm.lane.row, build.host.adapter_registry->LaunchContributionOf(arm.lane.row, true),
      instance);
  const auto evidence = controller_launch_artifacts_evidence(written);
  {
    std::ofstream record(written.directory / "controller_launch_artifacts.json",
                         std::ios::binary | std::ios::trunc);
    record << evidence << '\n';
    Require(static_cast<bool>(record), "ROS controller launch evidence could not be written");
  }
  std::cerr << "manyforge controller launch artifacts: " << evidence << std::endl;
  return written;
}

/// An arm's written launch, started (or kept, Q21) before its backend binds.
[[nodiscard]] PendingLaunch EnsureArmLaunch(const ComponentBuildInput &build,
                                            const ControllerLaunchArtifacts &written) {
  const auto &host = build.host;
  const auto ensured = host_controller_launch_supervisor().ensure(
      ros2_control_launch_command(written), written.directory,
      host.controller_request_context.empty()
          ? "Bootstrap runtime=" +
                (host.runtime_unit_id ? host.runtime_unit_id->value() : std::string("unmanaged")) +
                " epoch=" + host.runtime_epoch.value()
          : host.controller_request_context);
  PendingLaunch launch(new ControllerLaunchSupervisor::Ensured(ensured));
  std::cerr << "manyforge controller launch: component=" << written.controllable_component_id
            << " decision="
            << (ensured.decision == ControllerLaunchDecision::kKeep    ? "kept"
                : ensured.decision == ControllerLaunchDecision::kStart ? "started"
                                                                       : "restarted")
            << " pid=" << ensured.process->pid() << std::endl;
  return launch;
}

/// An arm's ros2_control execution configuration, from its build input
/// (FP-2) and its participant binding (R8): its lane family's, else the
/// generic Real one, with the operator Stop decelerations its JTC launches
/// with (Q48) and its own `measured_joints` override (CP9, the spec's stream
/// overrides) as the in-motion staleness check.
[[nodiscard]] RetainedRos2ControlBackendConfig ArmBackendConfig(
    const ComponentBuildInput &build, const ArmLaunchSelection &arm,
    const ControllerLaunchArtifacts &written, const core::RuntimeParticipantBinding &motion) {
  const auto &configuration = build.spec.host_configuration;
  const auto &cancel_by_source_joint = arm.profile->max_deceleration_on_cancel_rad_s2;
  auto config =
      arm.family->lane != nullptr
          ? arm.family->lane->arm_backend_config(
                Ros2ControlArmLaneInput{configuration, motion, arm.lane.row, written,
                                        cancel_by_source_joint, build.spec.stream_overrides})
          : make_retained_real_ros2_control_backend_config(configuration, arm.lane.row, written,
                                                           motion, cancel_by_source_joint,
                                                           build.spec.stream_overrides);
  config.ros_context_provider = build.host.ros_context_provider;
  config.arm.adapter.motion_trace = build.host.motion_trace;
  config.defer_ros_start = build.host.defer_ros_start;
  return config;
}

/// A tool's launch choices, from its build input only (FP-2): whether it has
/// no output (lane-free, D4: no lane row needed) and otherwise its controller
/// profile, lane row and the family whose tool hook (CONTRACTS §6) runs its
/// launch, gripper transport and measured lane.
struct ToolLaunchSelection final {
  const deployment::ros2_control_execution_runtime_contract::Configuration::ControllerProfile
      *profile{};
  bool no_output{};
  std::optional<ComponentLane> lane;
  const ComponentAdapterFamily *family{};
};

[[nodiscard]] ToolLaunchSelection SelectToolLaunch(const ComponentBuildInput &build,
                                                   const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
  const auto &configuration = build.spec.host_configuration;
  ToolLaunchSelection tool;
  tool.profile = deployment::ros2_control_execution_runtime_contract::find_controller_profile(
      configuration, value.controllable_component_id);
  const auto authored = std::find_if(configuration.gripper_target_configurations.begin(),
      configuration.gripper_target_configurations.end(), [&](const auto& row) {
        return row.controllable_component_id == value.controllable_component_id &&
               row.command_target_id == value.command_target_id;
      });
  Require(authored != configuration.gripper_target_configurations.end(),
          "ROS gripper-only component has no exact authored gripper configuration");
  tool.no_output = authored->output_mode == core::GripperOutputMode::kNone || authored->pneumatic_io;
  Require(!tool.no_output || authored->actuation_kind == "pneumatic",
          "No output requires its strict pneumatic gripper target");
  // A no-output tool is lane-free (D4): it needs no lane row.
  Require(tool.no_output || tool.profile != nullptr,
          "ROS gripper-only component has no lane row in its runtime unit");
  if (!tool.no_output) {
    tool.lane = SelectedComponentLane(*tool.profile);
    tool.family = &ComponentFamily(build.host.adapter_registry, *tool.lane);
    Require(tool.family->lane != nullptr && tool.family->lane->tool != nullptr,
            "ROS gripper-only component's lane runs no tools of its own");
  }
  return tool;
}

/// What a tool's launch leaves for its gripper runtime: its backend
/// configuration, its started launch (none when lane-free) and its
/// ros2_control connection.
struct ToolLaunch final {
  RetainedRos2ControlBackendConfig config;
  PendingLaunch launch;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
};

/// From the tool's build input (FP-2), its participant binding (R8), its
/// projected initial gripper position (seeds simulated hardware) and the
/// runtime unit that runs it.
[[nodiscard]] ToolLaunch LaunchTool(const ComponentBuildInput &build,
                                    const ToolLaunchSelection &tool,
                                    const core::RuntimeParticipantBinding &binding,
                                    const double initial, const core::RuntimeUnitId &local) {
  const auto &value = binding.value();
  const auto &configuration = build.spec.host_configuration;
  const auto &host = build.host;
  ToolLaunch result;
  auto &config = result.config;
  config.node_name = "manyforge_ros2_control_execution_backend";
  config.node_namespace = controller_runtime_namespace(value.controllable_component_id.value());
  config.executor_thread_count = 3U;
  if (tool.no_output) {
    ros2_control_execution::Ros2GripperActionConfig action;
    action.group_id = value.model_groups.front().compiled_group_name;
    action.joint_name = value.resources.front().compiled_resource_name;
    action.state_joint_name = *value.resources.front().source_joint_name;
    config.gripper = std::move(action);
    configure_retained_gripper_output_mode(config, configuration, binding);
    if (tool.profile != nullptr)
      config.arm.measured_joint_state_lane = retained_measured_joint_state_lane(
          configuration, value.controllable_component_id, config.node_namespace + "/joint_states");
  } else {
    const auto artifacts_root = host.execution_run_root / "controller-launch-artifacts" /
        value.controllable_component_id.value();
    // A tool with its own launch publishes its static frames there (its
    // row's compiled description); a no-output tool has no process, and its
    // parent's launch publishes them.
    const auto command = tool.family->lane->tool(
        Ros2ControlToolLaneInput{configuration, binding, *tool.profile, tool.lane->row,
                                 artifacts_root, initial, config.node_namespace},
        config);
    result.launch.reset(new ControllerLaunchSupervisor::Ensured(
        host_controller_launch_supervisor().ensure(command,
            artifacts_root / value.controllable_component_id.value(),
            host.controller_request_context.empty()
                ? "Bootstrap runtime=" + local.value() + " epoch=" + host.runtime_epoch.value()
                : host.controller_request_context)));
  }
  const auto &launch = result.launch;
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  if (launch) handle->InstallControllerProcessFaultSource(
      ControllerLaunchSupervisor::fault_source(launch->process, CachedControllerLaunchReadinessSource(handle)),
      [process = launch->process] { return core::Result<std::optional<std::string>>::Ok(
          ProjectControllerLaunchFault(process->ReadCachedFaultState())); });
  // An Isaac tool's launch is its bridge: no controllers to spawn.
  if (launch && launch->process->command().lane == ControllerLaunchLane::kRos2Control)
    InstallControllerLaunchSettle(*handle, launch->process, host.launch_settle_bound);
  config.defer_ros_start = host.defer_ros_start;
  config.measured_receiver_clock = host.shared_clock;
  config.ros_context_provider = host.ros_context_provider;
  config.arm.adapter.motion_trace = host.motion_trace;
  result.handle = std::move(handle);
  return result;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildGripperOnlyProductComposition(CompositionBuildInput input, NativePhysicalBindingSet physical) {
  Require(!physical.motion && physical.gripper && physical.gripper_program &&
              physical.status_gripper_role && input.shared_clock && input.shared_scheduler,
          "ROS gripper-only component requires its exact physical target and stable cell time");
  const auto binding = *physical.gripper;
  const auto& value = binding.value();
  Require(value.resources.size() == 1U && value.model_groups.size() == 1U &&
              value.resources.front().source_joint_name && value.control_path,
          "ROS gripper-only component requires one genuine model-backed scalar joint");
  const auto local = input.runtime_unit_id.value_or(value.runtime_unit_epochs.front().runtime_unit_id);
  // FP-2: the tool is built from its build input; its rows of the host's
  // configuration are its own only.
  const auto component = ComponentBuildInputOf(input, value.controllable_component_id);
  const auto &configuration = component.spec.host_configuration;
  const auto tool = SelectToolLaunch(component, binding);
  const deployment::FilesystemVerifiedRetainedPlanningModelSource model_source(input.package_root);
  const auto model = model_source.get_verified({input.bundle->retained_bundle_id(),
      input.bundle->value().compiled_products.planning_collision_model.product_id});
  Require(model.has_value() && input.collision_shared_model,
          "ROS gripper-only component has no verified cell geometry");
  auto stage = std::make_shared<const RetainedPlanningModelStage>(
      RetainedPlanningModelStage::create_verified(*model, input.execution_run_root));
  auto state_config = retained_ros2_control_gripper_state_config(binding);
  auto projected = BuildCandidateRetainedModelScene("manyforge_ros_gripper_host", *stage,
      *model, *input.bundle, input.model_map, {binding}, {state_config.freshness_budget_ns});
  const auto joint = projected.scene->getJointInfo(value.resources.front().compiled_resource_name);
  Require(joint && joint->limits.min_position.size() == 1U && joint->limits.max_position.size() == 1U,
          "ROS gripper-only component has no exact scalar model limits");
  const RetainedGripperPositionLimits limits{value.resources.front().coordinate_kind,
      joint->limits.min_position[0], joint->limits.max_position[0]};
  auto channel = std::make_shared<RetainedObservedStateChannel>(binding, projected.state_projector,
      std::move(state_config), input.shared_clock.get(), input.observation_wake);
  Require(channel->initialize_from_projection(input.shared_clock->now()).applied(),
          "ROS gripper-only component could not seed its genuine model projection");
  const auto observed = projected.state_projector->current_state(
      value.control_path->control_binding_id, value.control_path->control_binding_revision);
  Require(observed.ok() && observed.value().positions.size() == 1U,
          "ROS gripper-only component has no exact projected initial scalar");
  const auto& position = observed.value().positions.front();
  const double initial = std::holds_alternative<core::AngularPositionDTO>(position)
      ? std::get<core::AngularPositionDTO>(position).value_rad : std::get<core::LinearPositionDTO>(position).value_m;
  auto launched = LaunchTool(component, tool, binding, initial, local);
  auto &handle = launched.handle;
  // CP9: the tool's own `measured_joints` stream row, set by its lane (an
  // Isaac tool reads its joints from the shared Isaac joint states). A
  // lane-free tool (D4) declares no streams and has no measured lane.
  const auto measured_lane = launched.config.arm.measured_joint_state_lane;
  auto backend = make_retained_ros2_control_gripper_backend(std::move(launched.config), binding, channel,
      *input.shared_clock, *input.shared_scheduler, handle);
  RetainedGripperRuntimeInput gripper_input{
      binding, std::move(backend), input.shared_clock, channel, nullptr, {}, true,
      [handle](const auto& target, const auto& state, const auto&) {
        return handle->RebindGripperRoute(target, state);
      }, input.shared_scheduler, limits};
  // A tool with a lane (pneumatic or not) stages its own descriptor, so the
  // lane's `measured_joints` and `gripper_feedback` streams are declared and
  // fed; only the lane-free tool has no measured source.
  gripper_input.prepare_measured_sources = [handle, clock = input.shared_clock, measured_lane](
      const core::RuntimeParticipantBinding& current, const core::ActiveControlConfigurationDTO& configuration)
      -> core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>> {
    using Result = core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>;
    if (!measured_lane)
      return Result::Ok(std::make_shared<const RetainedPreparedRobotMeasuredSources>(
          RetainedPreparedRobotMeasuredSources{{current.value().controllable_component_id, {}}, [] {}}));
    auto descriptor = make_retained_ros2_control_measured_state_descriptor(current, configuration, *measured_lane);
    if (!descriptor.ok()) return Result::Err(descriptor.error());
    auto staged = handle->StageMeasuredStateSources({descriptor.value()}, clock,
        std::optional<core::RuntimeParticipantBinding>{}, std::optional(current));
    if (!staged.ok()) return Result::Err(staged.error());
    if (!staged.value()) return Result::Ok(nullptr);
    auto publication = std::move(staged.value());
    core::RobotMeasuredStateSourceView sources{current.value().controllable_component_id, {}};
    for (std::size_t index = 0; index < publication->descriptors().size(); ++index)
      sources.sources.push_back({publication->descriptors()[index], publication->readers()[index]});
    return Result::Ok(std::make_shared<const RetainedPreparedRobotMeasuredSources>(
        RetainedPreparedRobotMeasuredSources{std::move(sources), [publication] { publication->EnablePublication(); }}));
  };
  gripper_input.control_binding_representatives = physical.representatives;
  auto owner = std::make_shared<RetainedGripperRuntime>(std::move(gripper_input));
  const auto srdf = model->srdf_bytes();
  const auto documents = deployment::parse_model_documents_for_projection(model->urdf_bytes(),
      srdf.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{srdf});
  auto descriptor = BuildManualControlGripperDescriptor(binding, limits,
      configuration.gripper_target_configurations, input.model_map, documents);
  const auto make_resolver = [package_root = input.package_root, bundle = input.bundle,
      model_map = input.model_map, program_artifact_root = input.program_artifact_root,
      program_id = physical.gripper_program->program_binding_id()](auto bindings) {
    return std::make_shared<ProductBackedRuntimeParticipantResolver>(package_root, bundle,
        model_map, std::move(bindings), std::vector<core::ProgramBindingId>{program_id}, program_artifact_root);
  };
  auto resolver = make_resolver(std::vector<core::RuntimeParticipantBinding>{binding});
  std::vector<RetainedSceneFrameStateSourceInput> frame_sources;
  frame_sources.push_back(SceneFrameSource(*channel,
      std::shared_ptr<const core::IObservedStateProvider>(channel, channel->addressed_publication()),
      SceneFrameCoordinateAuthority(*input.bundle, binding)));
  auto frames = RetainedSceneFrameResolver::create_verified(input.collision_shared_model, *input.bundle,
      input.model_map, *model, local, input.runtime_epoch, std::move(frame_sources),
      input.selected_component_id);
  auto composition = std::make_unique<Composition>(value.controllable_component_id, input.bundle,
      input.status_program, input.status_program_binding, nullptr, physical.gripper_program,
      std::nullopt, binding, std::nullopt, CanonicalValue(nullptr), std::move(descriptor),
      stage, nullptr, input.projection_revision, input.publication_generation, std::nullopt,
      physical.status_gripper_role);
  composition->gripper_runtime = std::move(owner);
  composition->ros2_control_runtime_handle = std::move(handle);
  composition->measured_joint_state_lane = measured_lane;
  composition->controller_request_context = input.controller_request_context;
  if (launched.launch) {
    composition->controller_process = launched.launch->process;
    composition->prepared_controller_launch = *launched.launch;
    delete launched.launch.release();
  }
  composition->target_resolver = std::move(resolver);
  composition->candidate_participant_resolver_factory =
      MakeCandidateParticipantResolverFactory(make_resolver);
  composition->scene_frame_resolver = std::move(frames);
  // As the retained path: a mounted tool shares the cell's planner projection.
  composition->planner_scene_projection = std::move(input.planner_scene_projection);
  composition->candidate_control_representatives = std::move(physical.representatives);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  // CONTRACTS §6: a lane-free tool (no output) runs no process; the host's
  // passive adapter makes it a component that can be enabled.
  if (tool.lane)
    AdoptRos2ControlComponentAdapters(
        *composition, MakeRos2ControlComponentAdapters(
                          component, composition->ros2_control_runtime_handle,
                          composition->runtime, *tool.lane, *tool.family, {}, {}));
  else
    composition->component_adapter = std::make_shared<PassiveComponentAdapter>();
  return composition;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildSingleProductComposition(CompositionBuildInput input) {
  if (input.selected_component_id.has_value() &&
      input.retained_component_snapshots.contains(*input.selected_component_id))
    return BuildRetainedProductComposition(std::move(input));
  if ((input.shared_clock == nullptr) != (input.shared_scheduler == nullptr)) {
    throw std::invalid_argument("execution host clock and scheduler "
                                "capabilities must be supplied together");
  }
  Require(input.bundle != nullptr &&
              input.bundle->value().cell_graph.cell_id == input.cell_id &&
              (!input.runtime_unit_id.has_value() ||
               !input.runtime_unit_id->empty()) &&
              !input.runtime_epoch.empty() &&
              input.model_map.query().retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.graph_revision ==
                  input.bundle->value().cell_graph.graph_revision &&
              input.publication_generation.nonzero() &&
              (input.status_program == nullptr) ==
                  (input.status_program_binding == nullptr) &&
              (input.planner_scene_projection == nullptr ||
               !input.manager_run_root.empty()),
          "native physical composition inputs do not share one active "
          "product");
  const auto &products = input.bundle->value().compiled_products;
  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  auto direct_control_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical_bindings =
      ResolveNativePhysicalBindings(input, direct_control_inventory);
  if (!physical_bindings.motion && physical_bindings.gripper)
    return BuildGripperOnlyProductComposition(std::move(input), std::move(physical_bindings));
  Require(physical_bindings.motion.has_value() &&
              physical_bindings.status_motion_role.has_value(),
          "motion-backed native composition has no motion target");
  auto direct_control_binding = std::move(physical_bindings.motion_program);
  auto direct_control_motion_binding = std::move(physical_bindings.motion);
  auto control_binding_representatives =
      std::move(physical_bindings.representatives);
  input.status_motion_target_role_id =
      std::move(*physical_bindings.status_motion_role);
  Require(input.runtime_unit_id.has_value(),
          "native execution has no local runtime unit");
  const auto local_runtime_unit_id = input.runtime_unit_id;
  const auto contains_local_runtime = [&](const auto &binding) {
    const auto &runtime_units = binding.value().runtime_unit_epochs;
    return std::find_if(runtime_units.begin(), runtime_units.end(),
                        [&](const auto &runtime_unit) {
                          return runtime_unit.runtime_unit_id ==
                                     *local_runtime_unit_id &&
                                 runtime_unit.runtime_epoch ==
                                     input.runtime_epoch;
                        }) != runtime_units.end();
  };
  Require(contains_local_runtime(direct_control_motion_binding.value()),
          "native execution local runtime is absent from its exact "
          "participant bindings");
  // FP-2: the component is built from its build input (its spec, and an arm
  // member also its owner's, whose fingerprint is in the member's spec); its
  // rows of the host's configuration are its own only.
  const auto component = ComponentBuildInputOf(
      input, direct_control_motion_binding.value().value().controllable_component_id);
  const auto &retained_runtime_configuration = component.spec.host_configuration;
  // Cell context: a retained component takes later ones at Apply
  // (RefreshRetainedPlannerSettings).
  const auto planner_settings = deployment::planner_settings(
      deployment::cell_context(*input.bundle), *local_runtime_unit_id);
  std::optional<double> deployment_motion_target_limit;
  std::vector<core::ControlBindingDTO> motion_limit_bindings;
  motion_limit_bindings.reserve(direct_control_inventory.motion.size());
  for (const auto &binding : direct_control_inventory.motion) {
    const auto target = deployment::resolve_runtime_program_target(
        *binding,
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("execution.trajectory.execute"), *input.bundle,
        input.model_map);
    Require(target.control_binding().has_value(),
            "motion target has no exact control binding");
    motion_limit_bindings.push_back(*target.control_binding());
  }
  // The spec holds this component's limits only.
  constexpr bool whole_runtime_motion_limits = false;
  std::vector<std::pair<core::ControllableComponentId,
                        core::CommandTargetId>> configured_motion_limits;
  configured_motion_limits.reserve(
      retained_runtime_configuration.motion_target_limits.size());
  std::transform(
      retained_runtime_configuration.motion_target_limits.begin(),
      retained_runtime_configuration.motion_target_limits.end(),
      std::back_inserter(configured_motion_limits), [](const auto &limit) {
        return std::pair{limit.controllable_component_id,
                         limit.command_target_id};
      });
  Require(execution_motion_target_limit_keys_match(
              motion_limit_bindings, configured_motion_limits,
              *local_runtime_unit_id,
              direct_control_motion_binding.value().value().controllable_component_id,
              whole_runtime_motion_limits),
          "execution Deployment must contain exactly one TCP-speed maximum for "
          "every motion-capable target and no unknown target");
  const auto motion_limit =
      std::find_if(retained_runtime_configuration.motion_target_limits.begin(),
                   retained_runtime_configuration.motion_target_limits.end(),
                   [&](const auto &candidate) {
                     return candidate.controllable_component_id ==
                                direct_control_motion_binding.value()
                                    .value()
                                    .controllable_component_id &&
                            candidate.command_target_id ==
                                direct_control_motion_binding.value()
                                    .value()
                                    .command_target_id;
                   });
  Require(motion_limit !=
              retained_runtime_configuration.motion_target_limits.end(),
          "execution Deployment has no limit for the exact motion target");
  deployment_motion_target_limit = motion_limit->maximum_tcp_linear_speed_mps;
  // The spec's initial state is already this component's slice.
  auto selected_initial_joint_positions =
      retained_runtime_configuration.initial_joint_positions_rad;
  const auto selected_resource_count =
      direct_control_motion_binding.value().value().resources.size();
  Require(selected_initial_joint_positions.size() == selected_resource_count,
          "native initial-state selection differs from the exact motion binding");
  const auto execution_tick_period_ns = static_cast<core::Duration>(
      std::llround(1'000'000'000.0 /
                   static_cast<double>(
                       retained_runtime_configuration.execution_tick_hz)));
  const auto collision_monitor_period_ns = static_cast<core::Duration>(
      std::llround(retained_runtime_configuration.collision_monitor_period_ms *
                   1'000'000.0));
  Require(execution_tick_period_ns > 0 && collision_monitor_period_ns > 0,
          "native execution retained runtime cadences are invalid");

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  auto retained_model =
      planning_source.get_verified(deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          products.planning_collision_model.product_id});
  Require(retained_model.has_value(),
          "active retained planning model is absent or invalid");
  const auto retained_srdf = retained_model->srdf_bytes();
  const auto retained_model_documents =
      deployment::parse_model_documents_for_projection(
          retained_model->urdf_bytes(),
          retained_srdf.empty()
              ? std::optional<std::string_view>{}
              : std::optional<std::string_view>{retained_srdf});
  const auto roboplan_joint_limits = RoboPlanJointLimitsFromRetainedModel(
      retained_model_documents.urdf);
  auto stage = RetainedPlanningModelStage::create_verified(
      *retained_model, input.execution_run_root);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "manyforge_i6_execution_host",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), roboplan_joint_limits});
  auto scene_mutex = std::make_shared<std::mutex>();
  // Each target registers its own channel's budget: the motion channel's
  // (Real: measured_joints; Mock/Isaac: 0) and the commanded gripper's (0),
  // e.g. a Real measured arm beside its no-output pneumatic gripper.
  std::vector<core::RuntimeParticipantBinding> projected_state_bindings{
      direct_control_motion_binding.value()};
  std::vector<std::int64_t> projected_state_budgets{
      retained_ros2_control_motion_state_budget_ns(
          retained_runtime_configuration,
          direct_control_motion_binding.value())};
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *input.bundle, input.model_map, *retained_model,
          std::move(projected_state_bindings),
          std::move(projected_state_budgets)));

  auto publisher = std::move(input.collision_publisher);
  auto collision_model = std::move(input.collision_shared_model);
  if (publisher == nullptr) {
    const auto &group =
        direct_control_motion_binding.value().value().model_groups.front();
    // The retained package root is not a scene-mesh root: relative scene
    // meshes resolve only below the separately approved scene-asset root.
    collision_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_i6_execution_host", stage.loader_urdf_path(),
        stage.srdf_path(), stage.package_paths(), group.compiled_group_name,
        input.scene_asset_root_fd);
    auto collision_projector = RetainedCollisionWorldProjector::create_verified(
        collision_model, *input.bundle, input.model_map, *retained_model);
    const auto logical_scene =
        EmptyScene(input.bundle->value().cell_graph.cell_id,
                   input.projection_revision.scene_revision);
    auto publisher_result = RetainedCollisionWorldPublisher::create(
        std::move(collision_projector), "i6-execution-host",
        logical_scene, WallClock{}.now(), [] { return WallClock{}.now(); });
    if (!publisher_result.ok()) {
      Fail("retained collision publication failed: " +
           publisher_result.error().message);
    }
    publisher = std::move(publisher_result.value());
  }
  Require(publisher != nullptr && collision_model != nullptr,
          "native composition has no authoritative collision projection");
  const auto inventory = publisher->current_scene_snapshot_lease_inventory();
  Require(inventory.has_value() &&
              core::SameProjectionIdentity(
                  inventory->projection_binding.projection_revision,
                  input.projection_revision),
          "native collision publisher does not expose the active product "
          "projection");

  auto candidate_binding =
      CandidateBinding(direct_control_motion_binding.value(), input.model_map);
  auto candidate_client =
      std::make_unique<ProcessContainedRoboPlanCandidateClient>(
          NativeWorkerBaseModel(stage, roboplan_joint_limits),
          candidate_binding,
          PlannerComputerConfig(planner_settings),
          ProcessContainedCandidateClientConfig{
              .worker_executable = input.candidate_worker,
              .require_retained_projection = true,
              .scene_asset_root_fd = input.scene_asset_root_fd,
              .retained_package_root_fd = input.retained_package_root_fd,
              // The worker must load exactly the scene-mesh bytes this
              // composition's collision model loaded (or first pinned).
              .scene_mesh_content_digest =
                  [collision_model](const std::string &uri) {
                    return collision::ManyForgeCollisionEngine::
                        scene_mesh_content_digest(collision_model, uri);
                  }});
  Require(candidate_client->requires_retained_projection(),
          "production native planner must consume the exact retained scene "
          "projection");
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding,
      std::move(candidate_client));
  std::vector<core::ProgramBindingId> intrinsic_direct_bindings{
      direct_control_binding->program_binding_id()};
  const auto make_participant_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       intrinsic_direct_bindings = std::move(intrinsic_direct_bindings)](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            intrinsic_direct_bindings, program_artifact_root);
      };
  const RetainedRuntimeParticipantResolverFactory participant_resolver_factory =
      MakeCandidateParticipantResolverFactory(make_participant_resolver);
  auto target_resolver =
      make_participant_resolver({direct_control_motion_binding.value()});
  RetainedRuntimeParticipantResolver participant_resolver =
      MakeRetainedRuntimeParticipantResolver(target_resolver);
  const auto reference_collision_model = collision_model;
  const auto reference_runtime_unit_id = *local_runtime_unit_id;
  const auto reference_runtime_epoch = input.runtime_epoch;
  const auto reference_component_id = input.selected_component_id;
  RetainedExecutionHostInput runtime_input{
      direct_control_motion_binding.value(),
      {},
      std::move(input.motion_program_aliases),
      scene,
      scene_mutex,
      state_projector,
      publisher,
      collision_model,
      std::move(planner),
      [&, reference_collision_model, reference_runtime_unit_id,
       reference_runtime_epoch, reference_component_id](const auto &channels) {
        std::vector<RetainedSceneFrameStateSourceInput> state_sources;
        state_sources.reserve(channels.size());
        for (const auto &channel : channels) {
          const auto binding = channel->binding();
          const auto &resources = binding.value().resources;
          const bool has_model_resource = std::any_of(
              resources.begin(), resources.end(), [](const auto &resource) {
                return resource.source_joint_name.has_value();
              });
          if (!has_model_resource) {
            continue;
          }
          Require(std::all_of(resources.begin(), resources.end(),
                              [](const auto &resource) {
                                return resource.source_joint_name.has_value();
                              }),
                  "native scene-frame state channel mixes model and "
                  "component-local resources");
          state_sources.push_back(SceneFrameSource(*channel,
              std::shared_ptr<const core::IObservedStateProvider>(
                  channel, channel->addressed_publication()),
              SceneFrameCoordinateAuthority(*input.bundle, binding)));
        }
        return RetainedSceneFrameResolver::create_verified(
            reference_collision_model, *input.bundle, input.model_map,
            *retained_model, reference_runtime_unit_id, reference_runtime_epoch,
            std::move(state_sources), reference_component_id);
      },
      std::move(participant_resolver),
      participant_resolver_factory,
      {},
      {},
      {},
      {},
      {}};
  runtime_input.planner_model_map = input.model_map;
  runtime_input.control_binding_representatives =
      std::move(control_binding_representatives);
  // The arm's channels and its render-only measured source share one wake.
  runtime_input.observation_wake =
      input.observation_wake != nullptr
          ? std::move(input.observation_wake)
          : std::make_shared<RetainedObservationWake>();
  runtime_input.deployment_maximum_tcp_linear_speed_mps =
      deployment_motion_target_limit;
  // V2-9: RoboPlan times ordinary moves within the authored jerk limits.
  runtime_input.ordinary_motion_jerk = motion_limit->ordinary_motion_jerk;
  // Q37 (as main): the Deployment-authored fault Stop profile is the motion
  // target's fault profile, read directly; absent means none.
  runtime_input.fault_stop_profile_from_deployment = true;
  for (const auto &authored :
       retained_runtime_configuration.default_fault_stop_profiles) {
    if (authored.controllable_component_id ==
            runtime_input.motion_binding.value().controllable_component_id &&
        authored.command_target_id ==
            runtime_input.motion_binding.value().command_target_id) {
      runtime_input.deployment_fault_stop_profile = authored.profile;
    }
  }
  if (input.planner_scene_projection != nullptr) {
    runtime_input.planning_scene_synchronizer =
        std::make_shared<ManagerPlanningSceneSynchronizer>(
            input.manager_run_root, input.bundle->retained_bundle_id(),
            input.bundle->value().cell_graph.graph_revision,
            input.publication_generation, publisher);
  }
  // CONTRACTS §12: this component's launch, then started (or kept, Q21)
  // before the backend binds.
  std::shared_ptr<const RetainedToolContextSource> tool_context_source;
  const auto &motion_value = direct_control_motion_binding.value().value();
  const auto arm = SelectArmLaunch(component);
  const auto &family = *arm.family;
  const auto written = WriteArmLaunch(component, arm, selected_initial_joint_positions);
  if (family.lane == nullptr) {
    // TOOLCTX: the robot's TCP and payload, which its vendor family's
    // tool-context hook applies at Enable and before each new motion.
    const std::weak_ptr scene_projection{input.planner_scene_projection};
    tool_context_source = std::make_shared<const RetainedToolContextSource>(
        retained_model_documents.urdf,
        retained_tool_context_frames(
            input.bundle->value().cell_graph, input.model_map,
            retained_model->model_source_urdfs(),
            motion_value.controllable_component_id,
            motion_value.command_target_id),
        [scene_projection] {
          const auto projection = scene_projection.lock();
          return projection ? projection->logical_snapshot() : nullptr;
        });
    // The robot reports its own pose: no deployment initial pose seeds it.
    selected_initial_joint_positions.clear();
  }
  auto prepared_launch = EnsureArmLaunch(component, written);
  auto controller_process = prepared_launch->process;
  auto ros2_control_runtime_handle =
      RetainedRos2ControlRuntimeHandle::Create();
  // Active TCP (M7, design 2): the robot's TCP list and the checkbox (M6b:
  // the Deployment's "Send TCP offset to controller"), on every lane; unknown
  // at this start, so the End-effector TCP (T-MQ3).
  ros2_control_runtime_handle->InstallActiveTcp(
      motion_value.controllable_component_id,
      DirectControlRobotTcps(*direct_control_binding), arm.profile->controller_tcp_offset_sync,
      direct_control_binding->program_binding_id());
  if (input.planner_scene_projection) {
    const std::weak_ptr scene_projection{input.planner_scene_projection};
    ros2_control_runtime_handle->InstallCommittedSceneSource(
        [scene_projection] {
          const auto projection = scene_projection.lock();
          return projection ? projection->logical_snapshot() : nullptr;
        });
  }
  ros2_control_runtime_handle->InstallControllerProcessFaultSource(
      ControllerLaunchSupervisor::fault_source(
          controller_process, CachedControllerLaunchReadinessSource(
                                  ros2_control_runtime_handle)),
      [controller_process] {
        return core::Result<std::optional<std::string>>::Ok(
            ProjectControllerLaunchFault(
                controller_process->ReadCachedFaultState()));
      });
  InstallControllerLaunchSettle(*ros2_control_runtime_handle, controller_process,
                                component.host.launch_settle_bound);
  auto ros2_control_backend_config =
      ArmBackendConfig(component, arm, written, direct_control_motion_binding.value());
  // A subordinate runtime retains the exact supplied pair, or creates both.
  const auto measured_receiver_clock = input.shared_clock != nullptr
                                           ? input.shared_clock
                                           : std::make_shared<WallClock>();
  ros2_control_backend_config.measured_receiver_clock = measured_receiver_clock;
  const auto measured_lane =
      ros2_control_backend_config.arm.measured_joint_state_lane;
  // This runtime composes commanded arm state only, so its measured source is
  // also the viewer's render-only measured comparison and wakes that stream.
  runtime_input.prepare_measured_sources =
      [ros2_control_runtime_handle, measured_receiver_clock, measured_lane,
       render_wake = runtime_input.observation_wake](
          const core::RuntimeParticipantBinding &motion,
          const std::vector<core::ActiveControlConfigurationDTO> &configurations)
      -> core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>> {
    using Result =
        core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>;
    if (!measured_lane) {
      // Build-owned absence is unavailable measurement, not an additional
      // commanded-route or configuration Apply admission condition.
      return Result::Ok(nullptr);
    }
    std::vector<core::MeasuredStateSourceDescriptor> descriptors;
    bool source_configuration_absent = false;
    const auto add_binding = [&](const core::RuntimeParticipantBinding &binding)
        -> core::Result<void> {
      const auto &value = binding.value();
      // Component-local resources do not represent model geometry. The
      // independently complete inventory still covers all model-backed rows.
      if (std::all_of(value.resources.begin(), value.resources.end(),
                      [](const auto &row) { return !row.source_joint_name; }))
        return core::Result<void>::Ok();
      if (!value.control_path)
        return core::Result<void>::Err(
            {"measured_source_unavailable", "physical control path is absent",
             {{"reason", "measured_control_path_missing"}}});
      const auto configuration = std::find_if(
          configurations.begin(), configurations.end(), [&](const auto &row) {
            return row.control_domain_id == value.control_path->control_domain_id;
          });
      if (configuration == configurations.end()) {
        // Actual absence is not a foreign/malformed association. The
        // otherwise valid selected route still publishes without qualifying
        // measurement while another required domain is not yet current.
        source_configuration_absent = true;
        return core::Result<void>::Ok();
      }
      auto descriptor = make_retained_ros2_control_measured_state_descriptor(
          binding, *configuration, *measured_lane);
      if (!descriptor.ok()) return core::Result<void>::Err(descriptor.error());
      descriptors.push_back(std::move(descriptor.value()));
      return core::Result<void>::Ok();
    };
    auto added = add_binding(motion);
    if (!added.ok()) return Result::Err(added.error());
    if (source_configuration_absent) return Result::Ok(nullptr);
    auto staged = ros2_control_runtime_handle->StageMeasuredStateSources(
        descriptors, measured_receiver_clock, motion, std::nullopt, render_wake);
    if (!staged.ok()) return Result::Err(staged.error());
    if (!staged.value()) return Result::Ok(nullptr);
    if (staged.value()->descriptors().size() != staged.value()->readers().size())
      return Result::Err(
          {"measured_source_unavailable", "prepared measured source rows differ",
           {{"reason", "measured_prepared_scope_mismatch"}}});
    core::RobotMeasuredStateSourceView sources{motion.value().controllable_component_id, {}};
    for (std::size_t row = 0; row < staged.value()->descriptors().size(); ++row)
      sources.sources.push_back(
          {staged.value()->descriptors()[row], staged.value()->readers()[row]});
    auto publication = std::move(staged.value());
    return Result::Ok(std::make_shared<const RetainedPreparedRobotMeasuredSources>(
        RetainedPreparedRobotMeasuredSources{
            std::move(sources),
            [publication] { publication->EnablePublication(); }}));
  };
  // The lane's bound for a goal's result after its expected end
  // (`hard_stop_completion_timeout_s`): the arm's activity is `unknown` once
  // an open goal's result is that late.
  const std::chrono::nanoseconds goal_result_bound(
      ros2_control_backend_config.participant.stop.hard_stop_completion_timeout_ns);
  runtime_input.motion_state_channel_config =
      retained_ros2_control_motion_state_config(ros2_control_backend_config);
  runtime_input.backend_factory = make_retained_ros2_control_backend_factory(
      std::move(ros2_control_backend_config), ros2_control_runtime_handle);
  auto kernel_config = core::ExecutionKernelConfig{};
  kernel_config.collision_monitor_period_ns = collision_monitor_period_ns;
  const auto milliseconds_to_ns = [](const double value) {
    return static_cast<core::Duration>(std::llround(value * 1'000'000.0));
  };
  if (retained_runtime_configuration.runtime_collision_immediate_query_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_immediate_query_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_immediate_query_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_query_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_lookahead_query_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_lookahead_query_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_cycle_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_lookahead_cycle_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_lookahead_cycle_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_s
          .has_value()) {
    kernel_config.runtime_collision_lookahead_horizon_ns =
        static_cast<core::Duration>(std::llround(
            *retained_runtime_configuration.runtime_collision_lookahead_s *
            1'000'000'000.0));
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_max_samples
          .has_value()) {
    kernel_config.runtime_collision_lookahead_max_samples =
        *retained_runtime_configuration.runtime_collision_lookahead_max_samples;
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_workers
          .has_value()) {
    kernel_config.runtime_collision_lookahead_workers = static_cast<int>(
        *retained_runtime_configuration.runtime_collision_lookahead_workers);
    kernel_config.runtime_collision_lookahead_workers_declared = true;
  }
  if (retained_runtime_configuration.validation_collision_workers.has_value()) {
    kernel_config.validation_collision_workers = static_cast<int>(
        *retained_runtime_configuration.validation_collision_workers);
    kernel_config.validation_collision_workers_declared = true;
  }
  kernel_config.collision_clearance_m = planner_settings.collision_clearance_m;
  kernel_config.collision_check_fresh_plans = planner_settings.collision_check_fresh_plans;
  RetainedExecutionHostConfig runtime_config{
      core::CacheConfig{},
      std::move(kernel_config),
      SimpleSchedulerConfig::from_environment(),
      std::move(selected_initial_joint_positions)};
  auto receiver_scheduler = input.shared_scheduler != nullptr
                                ? input.shared_scheduler
                                : std::make_shared<SimpleScheduler>(runtime_config.scheduler);
  const auto runtime_collision_immediate_query_budget_ns =
      runtime_config.kernel.runtime_collision_immediate_query_budget_ns;
  auto runtime = create_retained_execution_host_participants(
      std::move(runtime_input), std::move(runtime_config),
      measured_receiver_clock, std::move(receiver_scheduler));
  runtime->collision_monitor_period_ns = collision_monitor_period_ns;
  runtime->runtime_collision_immediate_query_budget_ns =
      runtime_collision_immediate_query_budget_ns;
  auto direct_scene_frame_resolver =
      std::dynamic_pointer_cast<RetainedSceneFrameResolver>(
          runtime->reference_resolver);
  Require(direct_scene_frame_resolver != nullptr &&
              direct_scene_frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "native physical composition did not retain its exact direct frame "
          "resolver");
  auto direct_control_joint_limits = runtime->kinematic_model->joint_limits(
      direct_control_motion_binding.value()
          .value()
          .controllable_component_id.value(),
      direct_control_motion_binding.value().value().command_target_id.value());
  if (!direct_control_joint_limits.ok()) {
    Fail("direct-control joint limits are unavailable: " +
         direct_control_joint_limits.error().message);
  }
  auto manual_control_motion_descriptor = BuildManualControlMotionDescriptor(
      direct_control_motion_binding.value(),
      direct_control_joint_limits.value(),
      runtime->motion_participant_policy,
      runtime->deployment_maximum_tcp_linear_speed_mps,
      &motion_limit->ordinary_motion_jerk, {}, input.model_map,
      deployment::direct_control_motion_tcps(
          *direct_control_binding, direct_control_inventory, *input.bundle,
          input.model_map));
  const auto selected_component_id =
      input.selected_component_id.value_or(
          direct_control_motion_binding.value()
              .value()
              .controllable_component_id);
  auto composition = std::make_unique<Composition>(
      selected_component_id,
      std::move(input.bundle), std::move(input.status_program),
      std::move(input.status_program_binding),
      std::move(direct_control_binding), nullptr,
      std::move(direct_control_motion_binding.value()), std::nullopt,
      std::move(direct_control_joint_limits.value()),
      std::move(manual_control_motion_descriptor), CanonicalValue(nullptr),
      std::make_shared<const RetainedPlanningModelStage>(std::move(stage)),
      std::move(runtime), std::move(input.projection_revision),
      input.publication_generation,
      std::move(input.status_motion_target_role_id), std::nullopt);
  composition->ros2_control_runtime_handle =
      std::move(ros2_control_runtime_handle);
  composition->StageRobotTcpReader();
  composition->measured_joint_state_lane = measured_lane;
  composition->controller_process = std::move(controller_process);
  composition->controller_request_context = input.controller_request_context;
  composition->prepared_controller_launch = *prepared_launch;
  delete prepared_launch.release();
  // A launch with a reset-position writer (Mock, Isaac) serves reset_position.
  MockResetPositionPhaseCallback reset_phase;
  if (written.mock_reset_position)
    reset_phase = MockResetPhase({composition->runtime, composition->ros2_control_runtime_handle,
                                  *written.mock_reset_position},
                                 std::make_shared<bool>(false), {});
  AdoptRos2ControlComponentAdapters(
      *composition,
      MakeRos2ControlComponentAdapters(
          component, composition->ros2_control_runtime_handle, composition->runtime,
          arm.lane, family, arm.address, std::move(reset_phase),
          [tool_context_source] { return tool_context_source; }, goal_result_bound));
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  composition->scene_frame_resolver = std::move(direct_scene_frame_resolver);
  composition->target_resolver = std::move(target_resolver);
  composition->candidate_participant_resolver_factory =
      participant_resolver_factory;
  if (composition->planner_scene_projection != nullptr) {
    composition->scene_authority_proxy =
        std::make_shared<ManagerSceneAuthorityProxy>(
            std::move(input.manager_run_root), composition->planner_scene_projection);
  }
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  auto fixture = process_fixture::MakeFocusedPickPlacePlanningFixture();
  composition->pick_place_planning_fixture_lifetime =
      std::move(fixture.lifetime);
  composition->pick_place_planning_owner = std::move(fixture.owner);
#endif
  return composition;
}

// The simulated PLC plan of a component, from its spec only (FP-2): whether
// the host its I/O rows run on is simulated, and those rows (ROS-free
// selection in plan_simulated_plc).
[[nodiscard]] std::optional<SimulatedPlcPlan> SimulatedPlcPlanFor(
    const deployment::ComponentSpec &spec) {
  // CP8 step 1 (the cell-wide `mode` is deleted): a ROS unit is simulated
  // unless one of the components it hosts has measured (real) state authority.
  if (!spec.io_host_simulated) return std::nullopt;
  std::vector<SimulatedPlcRow> rows;
  for (const auto &row : spec.io_signals)
    rows.push_back({row.provider.profile, row.provider.topic, row.provider.service});
  return plan_simulated_plc(*spec.io_host_simulated, spec.controllable_component_id.value(),
                            rows, simulated_plc_config_dir_from_environment());
}

// The spec of `component` in `bundle`, or none when the bundle lacks it.
[[nodiscard]] std::optional<deployment::ComponentSpec> SpecIn(
    const deployment::RetainedBundle &bundle, const deployment::SiteFacts &site_facts,
    const core::ControllableComponentId &component) {
  const auto &components = bundle.value().cell_graph.controllable_components;
  if (std::none_of(components.begin(), components.end(), [&](const auto &row) {
        return row.controllable_component_id == component;
      }))
    return std::nullopt;
  return deployment::component_spec(bundle, site_facts, component);
}

// D4 (keep like external hardware): a rebuilt device keeps its running PLC
// process when the candidate's launch identity is unchanged.
[[nodiscard]] bool SimulatedPlcSurvivesRebuild(
    const std::shared_ptr<ControllerLaunchProcess> &process,
    const std::optional<deployment::ComponentSpec> &candidate) {
  if (!process || process->command().lane != ControllerLaunchLane::kSimulatedPlc)
    return false;
  return simulated_plc_launch_survives(
      process.get(), candidate ? SimulatedPlcPlanFor(*candidate) : std::nullopt);
}

// sim-plc-peer-design-02/03 §4: a pure generic ROS I/O device in a
// simulated unit (no hosted real component) gets a Manager-owned PLC process, started
// before Prepare by the same supervisor as the mock robot. Equal launch
// identity keeps it (and its values); a changed one restarts it; its exit is
// the device's component fault; removal/Close retire it via controller_process.
// `installed_process` is set when the drivers are borrowed from the exact
// retained installed owner: that owner's PLC must be kept unchanged.
void EnsureSimulatedPlc(
    Composition &view, const ComponentBuildInput &build,
    const std::string &bootstrap_context, const bool borrowed_drivers,
    const std::shared_ptr<ControllerLaunchProcess> &installed_process) {
  const auto &spec = build.spec;
  const auto &ros_context = build.host.ros_context_provider;
  const auto &execution_run_root = build.host.execution_run_root;
  const auto &request_context = build.host.controller_request_context;
  if (view.runtime || view.gripper_runtime || !view.io_provider_drivers ||
      view.io_provider_drivers->empty())
    return;
  const auto plan = SimulatedPlcPlanFor(spec);
  if (!plan) return;
  Require(static_cast<bool>(ros_context),
          "simulated_plc_not_ready: device=" + plan->device_id +
              " has no host ROS context");
  const auto artifacts_root =
      execution_run_root / "controller-launch-artifacts" / plan->device_id;
  std::filesystem::create_directories(artifacts_root);
  const auto ensured = host_detail::StartSimulatedPlcCandidate(
      host_controller_launch_supervisor(), *plan, artifacts_root,
      request_context.empty() ? bootstrap_context : request_context,
      *view.io_provider_drivers, borrowed_drivers, installed_process,
      [&](const std::shared_ptr<ControllerLaunchProcess> &process) {
        // The device's lifecycle budget, as every launched component's
        // settle: the PLC's own start (~2 s: rclpy, its duplicate check) and
        // the discovery of its endpoints on a busy graph exceeded a fixed
        // 5 s and refused Loads (`simulated_plc_not_ready`).
        return host_detail::WaitForSimulatedPlcEndpoints(
            ros_context(), *plan,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::nanoseconds(spec.lifecycle_command_timeout_ns)),
            [process] { return process->exited().has_value(); });
      });
  // A later build failure's ~Composition discards a fresh start; a kept
  // (borrowed) process is never discarded by this candidate.
  view.controller_process = ensured.process;
  view.controller_request_context = request_context;
  view.prepared_controller_launch = ensured;
  std::cerr << "manyforge simulated PLC launch: component=" << plan->device_id
            << " decision="
            << (ensured.decision == ControllerLaunchDecision::kKeep
                    ? "kept"
                    : ensured.decision == ControllerLaunchDecision::kStart
                          ? "started"
                          : "restarted")
            << " pid=" << ensured.process->pid()
            << " config_dir=" << plan->config_dir.string() << std::endl;
}

// CP9: the observer plan of a `pose_source` or `ai_model` component, from its
// spec only (FP-2), or none when the component has neither row.
[[nodiscard]] std::optional<PoseSourcePlan> PoseSourcePlanFor(
    const deployment::ComponentSpec &spec, const std::filesystem::path &execution_run_root,
    const std::filesystem::path &manager_run_root) {
  namespace contract = deployment::ros2_control_execution_runtime_contract;
  const auto &component = spec.controllable_component_id;
  const auto &cell = spec.cell_id.value();
  if (const auto *row = contract::find_pose_source(spec.host_configuration, component))
    return plan_pose_source(cell, component.value(), row->observer_configuration,
                            execution_run_root, manager_run_root);
  if (const auto *row = contract::find_ai_model(spec.host_configuration, component))
    return plan_ai_model(cell, component.value(), contract::kAiModelStreamId,
                         row->base_url, row->model, row->api_key_env,
                         execution_run_root);
  return std::nullopt;
}

// As the simulated PLC: a rebuilt pose source (AI model) keeps its running
// observer when the candidate's launch identity (document, Manager root) is
// unchanged.
[[nodiscard]] bool PoseSourceSurvivesRebuild(
    const std::shared_ptr<ControllerLaunchProcess> &process,
    const std::optional<deployment::ComponentSpec> &candidate,
    const std::filesystem::path &execution_run_root,
    const std::filesystem::path &manager_run_root) {
  if (!process || (process->command().lane != ControllerLaunchLane::kPoseSource &&
                   process->command().lane != ControllerLaunchLane::kAiModel))
    return false;
  return pose_source_launch_survives(
      process.get(), candidate ? PoseSourcePlanFor(*candidate, execution_run_root,
                                                   manager_run_root)
                               : std::nullopt);
}

// CP9 pose source / AI model: a model-free component whose provider driver
// has no I/O members. A fresh driver is installed here with its declared
// streams (`warning` + `auto`; Deployment overrides apply): one per pose
// binding (budget = binding lifetime, message stamps), or the AI model's one
// `endpoint` stream (arrival). Its observer process is started or kept by the
// supervisor like the simulated PLC, and never awaited: the Manager may be
// waiting on this host. The process exit is the driver's fault (typed
// `pose_source_exited` / `pose_source_config_invalid`, `ai_model_exited` /
// `ai_model_config_invalid`); an unreachable model endpoint only stales the
// stream.
void EnsurePoseSource(
    Composition &view, const ComponentBuildInput &build,
    const std::string &bootstrap_context, const bool borrowed_drivers,
    const std::shared_ptr<ControllerLaunchProcess> &installed_process) {
  const auto &spec = build.spec;
  const auto &request_context = build.host.controller_request_context;
  if (view.runtime || view.gripper_runtime || !view.io_provider_drivers)
    return;
  std::shared_ptr<IoProviderDriverRecord> driver;
  for (const auto &record : *view.io_provider_drivers)
    if (record && record->stream_observer()) driver = record;
  if (!driver) return;
  Require(spec.execution_host.has_value(), "stream observer has no host runtime unit");
  namespace contract = deployment::ros2_control_execution_runtime_contract;
  const auto planned =
      PoseSourcePlanFor(spec, build.host.execution_run_root, build.host.manager_run_root);
  Require(planned.has_value(), "stream observer component has no Deployment row");
  const auto &plan = *planned;
  if (!borrowed_drivers) {
    std::vector<core::StreamDeclarationDTO> declarations;
    if (plan.lane == ControllerLaunchLane::kAiModel) {
      declarations.push_back({std::string(contract::kAiModelStreamId),
                              kAiModelStreamBudgetNs,
                              core::StreamSeverityDTO::kWarning,
                              core::StreamRecoveryDTO::kAuto,
                              core::StreamTimestampSourceDTO::kArrival});
    } else {
      const auto *row = contract::find_pose_source(spec.host_configuration,
                                                   spec.controllable_component_id);
      for (const auto &binding : row->bindings)
        declarations.push_back({contract::pose_source_stream_id(binding),
                                binding.lifetime_ns,
                                core::StreamSeverityDTO::kWarning,
                                core::StreamRecoveryDTO::kAuto,
                                core::StreamTimestampSourceDTO::kMessageStamp});
    }
    std::vector<std::unique_ptr<core::StreamRecorder>> streams;
    for (auto &declaration : declarations) {
      auto declared = host_component_stream_health()->Declare(
          view.controllable_component_id, std::move(declaration));
      Require(declared.ok(), "observer stream cannot be declared: " +
                                 (declared.ok() ? std::string{}
                                                : declared.error().message));
      streams.push_back(std::move(declared.value()));
    }
    auto delivery = driver->AcquireDelivery();
    const auto installed = driver->InstallObserverStreamsLocked(std::move(streams));
    Require(installed.ok(), "observer driver cannot be installed: " +
                                (installed.ok() ? std::string{}
                                                : installed.error().message));
  }
  write_pose_source_document(plan);
  auto &supervisor = host_controller_launch_supervisor();
  const auto ensured = supervisor.ensure(
      pose_source_launch_command(plan, RandomToken(plan.component_id + ".")),
      plan.artifacts_root,
      request_context.empty() ? bootstrap_context : request_context);
  if (borrowed_drivers) {
    // The retained driver observes exactly the installed process.
    if (ensured.decision != ControllerLaunchDecision::kKeep ||
        ensured.process != installed_process) {
      static_cast<void>(supervisor.discard_candidate(ensured));
      Fail(std::string(plan.lane == ControllerLaunchLane::kAiModel
                           ? "ai_model_not_ready"
                           : "pose_source_not_ready") +
           ": component=" + plan.component_id +
           " installed process is no longer running; the retained component "
           "cannot change its observer");
    }
  } else {
    driver->SetSupervisedProcessFaultSource(
        pose_source_fault_source(ensured.process));
  }
  // A later build failure's ~Composition discards a fresh start; a kept
  // (borrowed) process is never discarded by this candidate.
  view.controller_process = ensured.process;
  view.controller_request_context = request_context;
  view.prepared_controller_launch = ensured;
  std::cerr << "manyforge pose source launch: component=" << plan.component_id
            << " decision="
            << (ensured.decision == ControllerLaunchDecision::kKeep
                    ? "kept"
                    : ensured.decision == ControllerLaunchDecision::kStart
                          ? "started"
                          : "restarted")
            << " pid=" << ensured.process->pid()
            << " site_config=" << plan.site_config_path.string() << std::endl;
}

// The generic I/O drivers of a component, from its build input only (FP-2):
// one per control domain it provides, with that domain's output resources; a
// retained component (`previous`) keeps its exact installed ones.
[[nodiscard]] IoProviderDriverGroup MakeIoProviderDrivers(
    const ComponentBuildInput &build, const RetainedComponentSnapshot *previous) {
  const auto &spec = build.spec;
  const auto &host = build.host;
  IoProviderDriverGroup drivers;
  for (const auto& domain : spec.provider_domains) {
    const auto* provider = domain.provider();
    Require(host.shared_clock && host.runtime_unit_id &&
                provider->provider_runtime_unit_id == *host.runtime_unit_id,
            "generic I/O driver has no actual shared-host time/unit");
    std::vector<core::ResourceId> outputs;
    for (const auto& row : spec.io_signals)
      if (row.runtime_unit_id == provider->provider_runtime_unit_id &&
          row.descriptor.signal_id.controllable_component_id == provider->controllable_component_id &&
          std::binary_search(provider->signal_names.begin(), provider->signal_names.end(), row.descriptor.signal_id.name) &&
          row.descriptor.output_resource) outputs.push_back(*row.descriptor.output_resource);
    std::sort(outputs.begin(), outputs.end());
    outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
    core::ResolvedLifecycleControlDomainScopeDTO scope{domain.control_domain_id, domain.control_domain_revision,
        core::ResolvedProviderDomainScopeDTO{*provider, host.runtime_epoch, std::nullopt, std::move(outputs)}};
    core::ResolvedLifecycleProviderOccurrenceDTO occurrence{domain.control_domain_id, domain.control_domain_revision,
        provider->provider_instance_id, provider->provider_manifest_id, provider->provider_manifest_revision,
        provider->provider_runtime_unit_id, host.runtime_epoch, provider->lifecycle_plan_catalog_digest};
    if (previous != nullptr) {
      Require(previous->io_provider_drivers != nullptr,
              "retained component lost its complete I/O driver group");
      const auto retained = std::find_if(previous->io_provider_drivers->begin(),
          previous->io_provider_drivers->end(), [&](const auto& driver) {
            return driver && driver->domain() == scope && driver->occurrence() == occurrence;
          });
      Require(retained != previous->io_provider_drivers->end() && (*retained)->resources_installed(),
              "retained I/O driver differs from its exact domain/membership/epoch");
      drivers.push_back(*retained);
    } else {
      const auto manifest = host.provider_manifest(provider->provider_manifest_id,
                                                   provider->provider_manifest_revision);
      Require(manifest.has_value(), "generic I/O driver has no actual qualified manifest");
      auto driver = IoProviderDriverRecord::Create(std::move(scope), std::move(occurrence),
          manifest->value(), host.shared_clock);
      Require(driver.ok(), "generic I/O driver could not be constructed from its exact lifecycle catalog");
      drivers.push_back(std::move(driver.value()));
    }
  }
  return drivers;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildProductComposition(CompositionBuildInput input) {
  if (input.managed_component_delta.has_value()) {
    Require(input.bundle != nullptr && input.managed_component_delta->valid(),
            "managed composition has an invalid component delta or bundle");
    std::vector<core::ControllableComponentId> selected_bundle_components;
    selected_bundle_components.reserve(
        input.bundle->value().cell_graph.controllable_components.size());
    for (const auto &component :
         input.bundle->value().cell_graph.controllable_components) {
      selected_bundle_components.push_back(
          component.controllable_component_id);
    }
    std::sort(selected_bundle_components.begin(),
              selected_bundle_components.end());

    std::vector<core::ControllableComponentId> delta_candidate_components;
    for (const auto &change : input.managed_component_delta->components) {
      if (change.disposition == deployment::ApplyComponentDisposition::kRemove)
        continue;
      delta_candidate_components.push_back(change.controllable_component_id);
    }
    Require(delta_candidate_components == selected_bundle_components,
            "managed component delta rows differ from the selected bundle");
  }

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  const auto make_io_view = [bundle = input.bundle,
                             projection = input.projection_revision,
                             generation = input.publication_generation](const auto& component) {
    return std::make_unique<Composition>(component, bundle, nullptr, nullptr,
        nullptr, nullptr, std::nullopt, std::nullopt, std::nullopt,
        CanonicalValue(nullptr), CanonicalValue(nullptr), nullptr, nullptr,
        projection, generation, std::nullopt, std::nullopt);
  };
  std::vector<core::ControllableComponentId> io_component_ids;
  for (const auto& domain : input.bundle->value().cell_graph.control_domains)
    if (domain.provider()) io_component_ids.push_back(domain.provider()->controllable_component_id);
  // A sensor (`sensors` row) is a standalone component like an I/O device:
  // its own view, whose adapter its family makes (CONTRACTS §6).
  std::vector<deployment::ros2_control_execution_runtime_contract::Configuration::Sensor> sensors;
  // CONTRACTS §16: a machine whose arm is deactivated has no arm composition
  // to build its owner; its owner is a view of its own, like a sensor.
  std::vector<deployment::ros2_control_execution_runtime_contract::Configuration::Machine>
      armless_machines;
  if (input.runtime_unit_id) {
    const auto configuration =
        deployment::ros2_control_execution_runtime_contract::require_configuration(
            *input.bundle, *input.runtime_unit_id);
    sensors = configuration.sensors;
    armless_machines = retained_ros2_control_armless_machines(configuration);
  }
  for (const auto& sensor : sensors) io_component_ids.push_back(sensor.controllable_component_id);
  for (const auto& machine : armless_machines)
    io_component_ids.push_back(machine.controllable_component_id);
  std::sort(io_component_ids.begin(), io_component_ids.end());
  io_component_ids.erase(std::unique(io_component_ids.begin(), io_component_ids.end()), io_component_ids.end());
  // FP-2: each view's component is built from its build input only.
  const auto site_facts = component_manager::SiteFactsOf(input.site_settings);
  const auto with_io_bindings = [bindings = inventory.io,
      sensors, armless_machines, site_facts, host = ComponentBuildHostOf(input),
      bundle = input.bundle, package_root = input.package_root,
      model_map = input.model_map, artifact_root = input.program_artifact_root,
      snapshots = input.retained_component_snapshots,
      io_component_ids, make_io_view,
      origin = input.origin_named_apply,
      selected_program = input.selected_program_products](std::unique_ptr<Composition> composition) {
    composition->origin_named_apply = origin;
    if (selected_program) {
      composition->program = selected_program->program;
      composition->program_binding = selected_program->binding;
      composition->selected_program_outcome = selected_program->outcome;
    }
    composition->direct_control_io_program_bindings = bindings;
    if (!composition->target_resolver && !io_component_ids.empty())
      composition->target_resolver = std::make_shared<ProductBackedRuntimeParticipantResolver>(
          package_root, bundle, model_map, std::vector<core::RuntimeParticipantBinding>{},
          std::vector<core::ProgramBindingId>{}, artifact_root);
    std::vector<Composition*> views{composition.get()};
    for (const auto& peer : composition->participant_compositions) views.push_back(peer.get());
    for (const auto& component : io_component_ids) {
      if (std::none_of(views.begin(), views.end(), [&](const auto* view) {
            return view->controllable_component_id == component;
          })) {
        composition->participant_compositions.push_back(make_io_view(component));
        views.push_back(composition->participant_compositions.back().get());
      }
    }
    // Owners before their I/O members, which are made with the owner's handle.
    std::stable_partition(views.begin(), views.end(), [&](const auto* view) {
      return std::any_of(armless_machines.begin(), armless_machines.end(), [&](const auto& row) {
        return row.controllable_component_id == view->controllable_component_id;
      });
    });
    for (auto* view : views) {
      const auto previous = snapshots.find(view->controllable_component_id);
      const auto component =
          ComponentBuildInputOf(*bundle, site_facts, host, view->controllable_component_id);
      const auto &spec = component.spec;
      // A retained sensor keeps its adapter (and so its process).
      if (const auto sensor = std::find_if(sensors.begin(), sensors.end(), [&](const auto& row) {
            return row.controllable_component_id == view->controllable_component_id;
          });
          sensor != sensors.end()) {
        view->component_adapter = previous != snapshots.end()
            ? previous->second.component_adapter
            : MakeSensorAdapter(component);
        Require(view->component_adapter != nullptr, "retained sensor lost its adapter");
        continue;
      }
      // The owner of a machine with its arm deactivated: where the machine
      // has a process (a lane row), the owner runs it and its family serves
      // the I/O members; otherwise the host's passive adapter (its I/O
      // members are Mock I/O). A retained owner keeps its adapter, handle and
      // process.
      if (const auto machine = std::find_if(armless_machines.begin(), armless_machines.end(),
              [&](const auto& row) {
                return row.controllable_component_id == view->controllable_component_id;
              });
          machine != armless_machines.end()) {
        if (previous != snapshots.end()) {
          view->component_adapter = previous->second.component_adapter;
          view->ros2_control_runtime_handle = previous->second.ros2_control_runtime_handle;
          view->controller_process = previous->second.controller_process;
          CarryRetainedMembership(*view, previous->second);
        } else if (const auto &own = spec.host_configuration.machines;
                   std::any_of(own.begin(), own.end(), [](const auto& row) {
                     return row.machine_lane_row.has_value();
                   })) {
          AdoptMachineOwnerProcess(*view, MakeMachineOwnerProcess(component),
                                   host.controller_request_context);
        } else {
          view->component_adapter = std::make_shared<PassiveComponentAdapter>();
        }
        Require(view->component_adapter != nullptr, "retained machine owner lost its adapter");
        view->owner_component =
            core::CoordinatedComponent{machine->controllable_component_id.value(),
                                       view->component_adapter};
        continue;
      }
      // CONTRACTS §16: an I/O member carries its owner and role; on the real
      // lane (`component_adapter` signals) its owner's family serves it
      // through the owner's connection, with no generic I/O driver.
      const auto& device = spec.io_device;
      if (device && device->owner_component_id) {
        view->owner_id = device->owner_component_id;
        view->member_role = device->member_role;
      }
      if (std::any_of(spec.io_signals.begin(), spec.io_signals.end(), [](const auto& row) {
            return row.provider.profile == deployment::kComponentAdapterIoProfile;
          })) {
        Require(device.has_value(), "I/O member has no device row");
        if (previous != snapshots.end()) {
          view->component_adapter = previous->second.component_adapter;
        } else {
          // Its owner's view, which built the owner with a lane adapter.
          const Composition* owner = nullptr;
          if (device->owner_component_id)
            for (const auto* candidate : views)
              if (candidate->owner_component &&
                  candidate->owner_component->component_id == *device->owner_component_id)
                owner = candidate;
          Require(owner != nullptr && owner->owner_handle != nullptr && owner->owner_family != nullptr,
                  "I/O member " + view->controllable_component_id.value() +
                      " has no owner built with a lane adapter");
          view->component_adapter =
              MakeIoMemberAdapter(component, owner->owner_handle, *owner->owner_family);
        }
        Require(view->component_adapter != nullptr, "retained I/O member lost its adapter");
        continue;
      }
      auto drivers = MakeIoProviderDrivers(
          component, previous != snapshots.end() ? &previous->second : nullptr);
      if (drivers.empty()) continue;
      if (previous != snapshots.end()) {
        Require(previous->second.io_provider_drivers && *previous->second.io_provider_drivers == drivers &&
                    previous->second.component_runtime_pins &&
                    previous->second.component_runtime_pins->matches(view->runtime, view->gripper_runtime,
                        previous->second.io_provider_drivers),
                "retained component replaced or omitted an actual runtime member");
        view->io_provider_drivers = previous->second.io_provider_drivers;
        view->component_runtime_pins = previous->second.component_runtime_pins;
      } else {
        view->io_provider_drivers = std::make_shared<const IoProviderDriverGroup>(std::move(drivers));
        view->component_runtime_pins = std::make_shared<const ComponentRuntimePins>(
            ComponentRuntimePins{view->runtime, view->gripper_runtime, view->io_provider_drivers});
      }
      const auto bootstrap_context = "Bootstrap runtime=" +
          (host.runtime_unit_id ? host.runtime_unit_id->value() : std::string("unmanaged")) +
          " epoch=" + host.runtime_epoch.value();
      EnsureSimulatedPlc(*view, component, bootstrap_context,
          previous != snapshots.end(),
          previous != snapshots.end() ? previous->second.controller_process : nullptr);
      EnsurePoseSource(*view, component, bootstrap_context,
          previous != snapshots.end(),
          previous != snapshots.end() ? previous->second.controller_process : nullptr);
    }
    return composition;
  };
  const auto sorted_component_ids = [&](const auto &bindings,
                                        const std::string_view capability_id) {
    std::vector<core::ControllableComponentId> component_ids;
    component_ids.reserve(bindings.size());
    for (const auto &binding : bindings) {
      const auto target = deployment::resolve_runtime_program_target(
          *binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId(std::string(capability_id)), *input.bundle,
          input.model_map);
      component_ids.push_back(target.component().controllable_component_id);
    }
    std::sort(component_ids.begin(), component_ids.end());
    Require(std::adjacent_find(component_ids.begin(), component_ids.end()) ==
                component_ids.end(),
            "native direct-control inventory repeats a capability for one "
            "component");
    return component_ids;
  };
  const auto motion_component_ids = sorted_component_ids(
      inventory.motion, "execution.trajectory.execute");
  const auto gripper_component_ids =
      sorted_component_ids(inventory.gripper, "tool.gripper.command");
  if (motion_component_ids.empty() && gripper_component_ids.empty()) {
    Require(!io_component_ids.empty() && input.runtime_unit_id &&
                input.shared_clock && input.shared_scheduler,
            "I/O-only composition requires its qualified domains and actual managed shared owner");
    auto composition = make_io_view(io_component_ids.front());
    // The selected root owns the actual status Program even without a motion
    // runtime. Peer component views do not become additional Program owners.
    composition->program = input.status_program;
    composition->program_binding = input.status_program_binding;
    return with_io_bindings(std::move(composition));
  }
  // A scalar-only cell has no motion Scene projection. Its frames still use
  // the complete immutable retained model, with the same source/asset checks.
  if (motion_component_ids.empty() && input.collision_shared_model == nullptr) {
    auto selected = input;
    selected.selected_component_id = gripper_component_ids.front();
    const auto physical = ResolveNativePhysicalBindings(selected, inventory);
    Require(!physical.motion && physical.gripper &&
                physical.gripper->value().model_groups.size() == 1U &&
                physical.gripper->value().retained_bundle_id ==
                    input.bundle->retained_bundle_id() &&
                physical.gripper->value().graph_revision ==
                    input.bundle->value().cell_graph.graph_revision &&
                input.model_map.query().retained_bundle_id ==
                    input.bundle->retained_bundle_id(),
            "scalar cell geometry has no exact retained target and model group");
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto model = source.get_verified(
        {input.bundle->retained_bundle_id(),
         input.bundle->value().compiled_products.planning_collision_model.product_id});
    Require(model.has_value(),
            "scalar cell geometry has no verified retained model");
    auto stage = std::make_shared<const RetainedPlanningModelStage>(
        RetainedPlanningModelStage::create_verified(*model, input.execution_run_root));
    auto collision_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_ros_scalar_cell", stage->loader_urdf_path(),
        stage->srdf_path(), stage->package_paths(),
        physical.gripper->value().model_groups.front().compiled_group_name,
        input.scene_asset_root_fd);
    // Keep the loader's private asset tree alive with this existing shared
    // model. The pair retires the model before its stage; no projection or
    // state authority is created, and retained component scene stages stay put.
    const auto *model_pointer = collision_model.get();
    auto lifetime = std::make_shared<const std::pair<
        std::shared_ptr<const RetainedPlanningModelStage>,
        collision::ManyForgeCollisionEngine::SharedModelPtr>>(
            std::move(stage), std::move(collision_model));
    input.collision_shared_model =
        collision::ManyForgeCollisionEngine::SharedModelPtr(
            std::move(lifetime), model_pointer);
  }
  if (motion_component_ids.empty() && gripper_component_ids.size() == 1U) {
    input.selected_component_id = gripper_component_ids.front();
    if (input.retained_component_snapshots.contains(gripper_component_ids.front()))
      return with_io_bindings(BuildRetainedProductComposition(std::move(input)));
    return with_io_bindings(BuildSingleProductComposition(std::move(input)));
  }
  if (motion_component_ids.size() == 1U && gripper_component_ids.empty()) {
    input.selected_component_id = motion_component_ids.front();
    if (input.retained_component_snapshots.contains(
            motion_component_ids.front()))
      return with_io_bindings(BuildRetainedProductComposition(std::move(input)));
    return with_io_bindings(BuildSingleProductComposition(std::move(input)));
  }
  Require(
      input.runtime_unit_id.has_value() &&
          (!input.component_planner_scene_projections.empty() ||
           motion_component_ids.empty()) &&
          input.shared_clock != nullptr && input.shared_scheduler != nullptr &&
          input.status_program == nullptr &&
          input.status_program_binding == nullptr,
      "multi-robot native composition requires the managed shared-owner path");

  auto component_ids = motion_component_ids;
  component_ids.insert(component_ids.end(), gripper_component_ids.begin(), gripper_component_ids.end());
  std::sort(component_ids.begin(), component_ids.end());
  component_ids.erase(std::unique(component_ids.begin(), component_ids.end()), component_ids.end());
  // Preserve the existing motion root when present; independent scalar
  // components add owners without changing any arm's local runtime.
  if (!motion_component_ids.empty()) {
    const auto first_motion = std::find(component_ids.begin(), component_ids.end(), motion_component_ids.front());
    std::rotate(component_ids.begin(), first_motion, std::next(first_motion));
  }

  std::vector<std::unique_ptr<Composition>> compositions;
  compositions.reserve(component_ids.size());
  if (input.observation_wake == nullptr)
    input.observation_wake = std::make_shared<RetainedObservationWake>();
  for (const auto &component_id : component_ids) {
    auto selected = input;
    selected.selected_component_id = component_id;
    const auto projection =
        selected.component_planner_scene_projections.find(component_id);
    if (projection != selected.component_planner_scene_projections.end()) {
      Require(projection->second != nullptr,
              "native component has a null Scene projection");
      selected.collision_publisher = projection->second;
      selected.collision_shared_model =
          projection->second->collision_shared_model();
      selected.planner_scene_projection = projection->second;
    } else {
      Require(!std::binary_search(motion_component_ids.begin(),
                                  motion_component_ids.end(), component_id) &&
                  selected.collision_shared_model != nullptr,
              "native motion component has no Scene projection");
      selected.collision_publisher.reset();
      selected.planner_scene_projection.reset();
    }
    const auto retain_component = [&](auto &aliases) {
      std::erase_if(aliases, [&](const auto &binding) {
        return binding.value().controllable_component_id != component_id;
      });
    };
    retain_component(selected.motion_program_aliases);
    if (selected.retained_component_snapshots.contains(component_id))
      compositions.push_back(
          BuildRetainedProductComposition(std::move(selected)));
    else
      compositions.push_back(
          BuildSingleProductComposition(std::move(selected)));
  }

  std::vector<core::ProgramBindingId> intrinsic_direct_bindings;
  intrinsic_direct_bindings.reserve(inventory.motion.size() +
                                    inventory.gripper.size());
  for (const auto &binding : inventory.motion)
    intrinsic_direct_bindings.push_back(binding->program_binding_id());
  for (const auto &binding : inventory.gripper)
    intrinsic_direct_bindings.push_back(binding->program_binding_id());
  std::sort(intrinsic_direct_bindings.begin(), intrinsic_direct_bindings.end());
  intrinsic_direct_bindings.erase(std::unique(intrinsic_direct_bindings.begin(),
                                              intrinsic_direct_bindings.end()),
                                  intrinsic_direct_bindings.end());
  const auto make_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       intrinsic_direct_bindings = std::move(intrinsic_direct_bindings)](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        // The runtime (re)join composes only the robots' participants; a
        // tool's gripper-only owner joins at the cell install. Name only the
        // direct bindings this call composes (as NativeCellParticipantResolverFactory).
        auto intrinsic = intrinsic_direct_bindings;
        std::erase_if(intrinsic, [&](const auto &id) {
          return std::none_of(physical_bindings.begin(), physical_bindings.end(),
                              [&](const auto &binding) {
                                return binding.value().program_binding_id == id;
                              });
        });
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            std::move(intrinsic), program_artifact_root);
      };
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      MakeCandidateParticipantResolverFactory(make_resolver);

  auto primary = std::move(compositions.front());
  compositions.erase(compositions.begin());
  const bool has_retained_owner =
      !input.retained_component_snapshots.empty();
  if (!primary->runtime)
    primary->candidate_participant_resolver_factory = resolver_factory;
  if (!has_retained_owner) {
    std::vector<std::shared_ptr<RetainedExecutionHost>> peers;
    peers.reserve(compositions.size());
    for (const auto &composition : compositions) {
      if (composition->runtime != nullptr)
        peers.push_back(composition->runtime);
    }
    if (primary->runtime != nullptr)
      join_retained_execution_host_participants(primary->runtime,
                                                std::move(peers),
                                                resolver_factory);
  } else {
    primary->candidate_participant_resolver_factory = resolver_factory;
  }
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  physical_bindings.reserve(component_ids.size());
  if (primary->direct_control_motion_binding.has_value())
    physical_bindings.push_back(*primary->direct_control_motion_binding);
  if (primary->direct_control_gripper_binding.has_value())
    physical_bindings.push_back(*primary->direct_control_gripper_binding);
  for (const auto &composition : compositions) {
    if (composition->direct_control_motion_binding.has_value())
      physical_bindings.push_back(*composition->direct_control_motion_binding);
    if (composition->direct_control_gripper_binding.has_value())
      physical_bindings.push_back(*composition->direct_control_gripper_binding);
  }
  primary->target_resolver = make_resolver(std::move(physical_bindings));
  primary->participant_compositions = std::move(compositions);
  // Scene is a cell authority. Its component-relative frames may refer to
  // any installed robot, not just the component selected as this cell's
  // addressed-Kernel root. Keep each engine's local resolver separate, but
  // install one verified cell resolver over every live state source.
  {
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto retained_model = source.get_verified(
        deployment::RetainedPlanningModelQuery{
            input.bundle->retained_bundle_id(),
            input.bundle->value().compiled_products.planning_collision_model
                .product_id});
    Require(retained_model.has_value() && input.runtime_unit_id.has_value() &&
                input.collision_shared_model != nullptr,
            "native cell frame resolver has no candidate model or owner");
    auto collected = CollectCellFrameStateSources(*primary, *input.bundle);
    std::vector<RetainedSceneFrameStateSourceInput> state_sources;
    for (const auto &source : collected)
      state_sources.push_back(source.frame_source);
    auto cell_frame_resolver = RetainedSceneFrameResolver::create_verified(
        input.collision_shared_model, *input.bundle, input.model_map,
        *retained_model, *input.runtime_unit_id, input.runtime_epoch,
        std::move(state_sources));
    Require(cell_frame_resolver != nullptr,
            "native cell frame resolver could not be built");
    // A component's Program may name a frame on another installed component.
    // Candidate views all use the same verified cell source set. A retained
    // engine is re-sealed only at fenced installation; fresh engines are
    // private and may receive the resolver now.
    const auto install_candidate_resolver = [&](Composition &view) {
      view.scene_frame_resolver = cell_frame_resolver;
      if (input.retained_component_snapshots.contains(
              view.controllable_component_id))
        return;
      if (view.gripper_runtime != nullptr)
        return;
      Require(view.runtime != nullptr &&
                  view.runtime->reference_resolver_proxy != nullptr,
              "native candidate has no frame-resolution owner");
      view.runtime->reference_resolver = cell_frame_resolver;
      view.runtime->reference_resolver_proxy->ReplaceTarget(
          cell_frame_resolver);
    };
    install_candidate_resolver(*primary);
    for (auto &peer : primary->participant_compositions)
      install_candidate_resolver(*peer);
  }
  return with_io_bindings(std::move(primary));
}

// The retained components with a view of their own in `candidate`: an armed
// machine's owner rides on its arm member's view, which retains it.
[[nodiscard]] std::vector<core::ControllableComponentId>
RetainedComponentIds(
    const HostApplySelection &request, const Composition &candidate) {
  const auto carried = MemberCarriedOwnerIds(candidate);
  std::vector<core::ControllableComponentId> ids;
  for (const auto &change : request.component_delta.components) {
    if (change.disposition == deployment::ApplyComponentDisposition::kRetain &&
        !std::binary_search(carried.begin(), carried.end(), change.controllable_component_id))
      ids.push_back(change.controllable_component_id);
  }
  Require(std::is_sorted(ids.begin(), ids.end()) &&
              std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "retained host Apply repeats a component identity");
  return ids;
}

// Read-only: every retained owner of `candidate` is its predecessor's. Runs
// before the Apply touches any installed owner.
void RequireRetainedCandidateOwners(
    const Composition &candidate, const Composition &predecessor,
    const HostApplySelection &request) {
  const auto old = IndexNativeComponentCompositions(predecessor);
  const auto next = IndexNativeComponentCompositions(candidate);
  for (const auto &component_id : RetainedComponentIds(request, candidate)) {
    const auto previous = old.find(component_id);
    const auto selected = next.find(component_id);
    Require(previous != old.end() && selected != next.end() &&
                previous->second->component_runtime_identity() != nullptr &&
                previous->second->component_runtime_identity() ==
                    selected->second->component_runtime_identity() &&
                (!(selected->second->runtime || selected->second->gripper_runtime) ||
                 (selected->second->scene_frame_resolver != nullptr &&
                  !selected->second->candidate_component_registrations.empty())),
            "retained native owner differs from the planned predecessor");
  }
}

void ResealRetainedCandidateOwners(
    Composition &candidate, const Composition &predecessor,
    const HostApplySelection &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  auto next = IndexNativeComponentCompositions(candidate);
  for (const auto &component_id : RetainedComponentIds(request, candidate)) {
    auto &view = *next.at(component_id);
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native component lost the exact parent Apply fence");
    if (IsAdapterOnlyView(view)) continue;  // its adapter is retained as is
    if (!view.runtime && !view.gripper_runtime) {
      Require(view.io_provider_drivers && view.component_runtime_pins,
              "retained I/O component has no actual driver group");
      continue; // Actual leaf rebind belongs to the same installer's Apply barrier.
    }
    auto physical_registrations = view.candidate_component_registrations;
    if (view.gripper_runtime != nullptr) {
      Require(view.runtime == nullptr &&
                  !view.candidate_component_registrations.empty() &&
                  view.direct_control_gripper_binding.has_value(),
              "retained gripper candidate has no exact physical participant");
      std::vector<core::RuntimeParticipantBinding> representatives;
      std::vector<std::optional<core::RuntimeParticipantStopReadiness>> readiness;
      for (const auto& row : physical_registrations) {
        representatives.push_back(row.binding);
        readiness.push_back(row.stop_readiness);
      }
      auto staged = view.gripper_runtime->PrepareRegistrations(representatives, readiness);
      Require(staged.ok(), staged.ok() ? std::string{} : staged.error().message);
      const auto primary = std::find_if(staged.value().begin(), staged.value().end(), [&](const auto& row) {
        return row.binding.value() == view.direct_control_gripper_binding->value();
      });
      Require(primary != staged.value().end(), "retained gripper candidate omits its exact primary command");
      const auto channel =
          std::dynamic_pointer_cast<RetainedObservedStateChannel>(
              view.gripper_runtime->state_channel());
      Require(channel != nullptr,
              "retained gripper has no rebindable observed-state owner");
      Require(predecessor.cell_runtime != nullptr &&
                  predecessor.cell_runtime->clock() != nullptr,
              "retained gripper has no stable cell clock");
      const auto rebound = channel->RebindRoute(
          *view.direct_control_gripper_binding,
          predecessor.cell_runtime->clock()->now());
      if (!rebound.ok())
        Fail("retained gripper state route cannot be re-sealed: " +
             rebound.error().message);
      const auto installed = view.gripper_runtime->RebindRoute(
          *view.direct_control_gripper_binding, channel,
          primary->stop_readiness);
      if (!installed.ok())
        Fail("retained gripper controller cannot be re-sealed: " +
             installed.error().message);
      view.gripper_runtime->AdoptVerifiedRegistrations(std::move(staged.value()));
      continue;
    }
    Require(view.runtime != nullptr &&
                view.direct_control_motion_binding.has_value(),
            "retained motion candidate has no physical owner");
    RetainedExecutionHostResealInput reseal{
        *view.direct_control_motion_binding,
        {},
        view.candidate_control_representatives,
        std::move(physical_registrations),
        {},
        nullptr,
        view.scene_frame_resolver,
        request.configuration_fence_id,
        request.configuration_fence_scope,
        admission_gate};
    auto result = reseal_retained_execution_host_for_apply(*view.runtime,
                                                           std::move(reseal));
    if (!result.ok()) {
      Fail("retained native owner cannot be re-sealed under Apply: " +
           result.error().message);
    }
  }
}

void RefreshRetainedCandidateCollision(
    Composition &candidate,
    const HostApplySelection &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native collision candidate repeats an owner");
  }
  for (const auto &component_id : RetainedComponentIds(request, candidate)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end() &&
                found->second->component_runtime_identity() != nullptr,
            "retained native collision owner is absent");
    auto &view = *found->second;
    // A standalone gripper has no planner or collision-engine resources to
    // reconfigure. Its geometry changes through the Scene projection below.
    if (view.gripper_runtime != nullptr || (!view.runtime && view.io_provider_drivers) ||
        IsAdapterOnlyView(view))
      continue;
    auto &runtime = *view.runtime;
    Require(runtime.participant_engine != nullptr &&
                view.candidate_collision_publisher != nullptr &&
                view.candidate_collision_model != nullptr &&
                view.candidate_runtime_collision_engine != nullptr,
            "retained native candidate collision resources are incomplete");
    core::CollisionPipelineResourceOwners owners{
        view.candidate_collision_publisher,
        view.candidate_runtime_collision_engine,
        view.candidate_validation_collision_engines,
        view.candidate_lookahead_collision_engines};
    auto installed = runtime.participant_engine
                         ->replace_collision_resources_under_apply_fence(
                             request.configuration_fence_id,
                             request.configuration_fence_scope, admission_gate,
                             std::move(owners));
    if (!installed.ok())
      Fail("retained native collision refresh failed: " +
           installed.error().message);
    auto readback = runtime.participant_engine->collision_resource_set_readback();
    const auto candidate_policy =
        view.candidate_collision_publisher->collision_policy_snapshot();
    Require(readback.ok() && readback.value() == installed.value() &&
                readback.value().resources.snapshot_publisher ==
                    view.candidate_collision_publisher &&
                readback.value().resources.runtime_monitor_engine ==
                    view.candidate_runtime_collision_engine &&
                readback.value().resources.validation_engines ==
                    view.candidate_validation_collision_engines &&
                readback.value().resources.runtime_lookahead_engines ==
                    view.candidate_lookahead_collision_engines &&
                candidate_policy.has_value() &&
                readback.value().coordinator_policy_snapshot != nullptr &&
                *readback.value().coordinator_policy_snapshot ==
                    *candidate_policy,
            "retained native collision readback differs from the candidate");
    runtime.collision_publisher = view.candidate_collision_publisher;
    runtime.collision_shared_model = view.candidate_collision_model;
    runtime.geometric_contact_scene_cache =
        view.candidate_geometric_contact_scene_cache;
    runtime.runtime_collision_engine =
        view.candidate_runtime_collision_engine;
    runtime.validation_collision_engines =
        view.candidate_validation_collision_engines;
    runtime.lookahead_collision_engines =
        view.candidate_lookahead_collision_engines;
  }
}

// A retained controller keeps its identities (controller, Kernel, planner
// client, observed sources) but every model-dependent computation follows the
// candidate's whole-cell planning model: the worker's base model, the
// in-process scene with its kinematics, validator and collision projector,
// and each model-backed state projector; a motion controller's planner and
// Kernel also take the candidate's planner settings (seed, collision step,
// clearance). The candidate composition owns the
// stage whose files the replacement worker loads. Collision resources were
// installed just before; this completes one coherent model generation and
// reads it back before admission can reopen. A failure is rethrown as the
// Apply failure; nothing is restored and the fence keeps admission closed.
void RefreshRetainedCandidateModels(
    Composition &candidate,
    const HostApplySelection &request,
    core::IResourceOperationAdmissionGate &admission_gate,
    core::IClock &clock) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native model candidate repeats an owner");
  }
  for (const auto &component_id : RetainedComponentIds(request, candidate)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end(),
            "retained native model owner is absent");
    auto &view = *found->second;
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native model refresh lost the exact parent Apply fence");
    if (view.gripper_runtime != nullptr) {
      if (view.candidate_tool_state_projector == nullptr)
        continue;
      const auto channel =
          std::dynamic_pointer_cast<RetainedObservedStateChannel>(
              view.gripper_runtime->state_channel());
      const auto *backend = view.gripper_runtime->backend();
      Require(channel != nullptr && backend != nullptr,
              "retained gripper has no model-backed observed-state owner");
      Require(backend
                      ->get_gripper_status(view.gripper_runtime->binding()
                                               .value()
                                               .command_target_id.value())
                      .state != core::GripperCommandState::kExecuting,
              "retained gripper has active command work during model refresh");
      const auto replaced = channel->ReplaceKinematicProjector(
          view.candidate_tool_state_projector, clock.now());
      if (!replaced.ok())
        Fail("retained gripper rejected the candidate cell model: " +
             replaced.error().message);
      Require(channel->kinematic_projector() ==
                  view.candidate_tool_state_projector,
              "retained gripper does not read back the candidate model");
      view.candidate_tool_state_projector.reset();
      continue;
    }
    Require(view.candidate_planner_model.has_value() ==
                (view.candidate_model_generation != nullptr),
            "retained native model candidate is incomplete");
    if (view.candidate_planner_settings.has_value()) {
      // Planner settings are cell context: the retained planner and Kernel
      // take the candidate's.
      Require(view.runtime != nullptr && view.runtime->planner != nullptr &&
                  view.runtime->participant_engine != nullptr,
              "retained native planner has no settings owner");
      const auto settings = view.runtime->planner->ReplaceRetainedSettings(
          PlannerComputerConfig(*view.candidate_planner_settings));
      if (!settings.ok())
        Fail("retained native planner rejected the candidate planner settings: " +
             settings.error().message);
      view.runtime->participant_engine->set_collision_clearance_m(
          view.candidate_planner_settings->collision_clearance_m);
      view.runtime->participant_engine->set_collision_check_fresh_plans(
          view.candidate_planner_settings->collision_check_fresh_plans);
      view.candidate_planner_settings.reset();
    }
    if (!view.candidate_planner_model.has_value())
      continue;
    Require(view.runtime != nullptr && view.runtime->planner != nullptr &&
                view.stage != nullptr &&
                view.candidate_planner_model->urdf_path ==
                    view.stage->loader_urdf_path(),
            "retained native planner has no staged candidate model");
    const auto replaced =
        view.runtime->planner->ReplaceRetainedModel(*view.candidate_planner_model);
    if (!replaced.ok())
      Fail("retained native planner rejected the candidate planning model: " +
           replaced.error().message);
    const auto refreshed = refresh_retained_execution_host_model_for_apply(
        *view.runtime,
        RetainedExecutionHostModelRefreshInput{
            view.candidate_model_generation, request.configuration_fence_id,
            request.configuration_fence_scope, admission_gate});
    if (!refreshed.ok())
      Fail("retained native model consumers rejected the candidate cell "
           "model: " +
           refreshed.error().message);
    view.candidate_planner_model.reset();
    view.candidate_model_generation.reset();
  }
}


[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
PhysicalControlBindings(const Composition &composition) {
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  if (composition.runtime != nullptr) {
    physical_bindings =
        RetainedComponentOwner(composition.runtime).physical_bindings();
  } else if (composition.gripper_runtime != nullptr) {
    physical_bindings = RetainedComponentOwner(
                            composition.gripper_runtime)
                            .physical_bindings();
  }
  for (const auto &participant : composition.participant_compositions) {
    auto nested = PhysicalControlBindings(*participant);
    physical_bindings.insert(physical_bindings.end(),
                             std::make_move_iterator(nested.begin()),
                             std::make_move_iterator(nested.end()));
  }
  return physical_bindings;
}

[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
NativeCellComponentRegistrations(const Composition &cell) {
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  std::set<core::ControllableComponentId> owners;
  const auto append = [&](const Composition &view) {
    Require(owners.insert(view.controllable_component_id).second,
            "native cell repeats a component registration owner");
    auto rows = view.component_registrations();
    Require(!rows.empty() || (view.io_provider_drivers &&
                !view.io_provider_drivers->empty()) || IsAdapterOnlyView(view),
            "native cell component has no qualified execution/input owner");
    for (auto &row : rows) {
      row.motion_takeover_preparation_budget_s =
          row.binding.value().capability_id.value() ==
                  "execution.trajectory.execute"
              ? cell.bundle->value().motion_takeover_preparation_budget_s
              : std::nullopt;
      row.motion_takeover_planning_budget_s =
          row.binding.value().capability_id.value() ==
                  "execution.trajectory.execute"
              ? cell.bundle->value().motion_takeover_planning_budget_s
              : std::nullopt;
    }
    result.insert(result.end(), std::make_move_iterator(rows.begin()),
                  std::make_move_iterator(rows.end()));
  };
  append(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing component owner");
    append(*participant);
  }
  return result;
}

// Paused resource construction precedes Kernel registration. The caller
// holds the same existing Apply/composition mutation pin as installation.
void PrepareCompositionIoResources(Composition &composition,
    RetainedCellRuntime &cell,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> &shared_transport,
    const ros2_control_execution::Ros2TransportResources::ContextProvider &ros_context_provider,
    std::span<const CompositionIoResourcePredecessor> existing = {}) {
  const auto rows = deployment::ResolveRetainedIoSignalCatalog(composition.bundle->value().cell_graph);
  Require(rows.ok(), "retained I/O source configuration is invalid");
  if (std::any_of(rows.value().begin(), rows.value().end(), [](const auto &row) {
        return row.provider.profile == "geometric_contact";
      }))
    composition.cell_frame_state_sources = CollectCellFrameStateSources(
        composition, *composition.bundle, true);
  const auto &domains = composition.bundle->value().cell_graph.control_domains;
  const bool stream_observer = std::any_of(domains.begin(), domains.end(), [](const auto &domain) {
    if (!domain.provider()) return false;
    const auto &manifest = domain.provider()->provider_manifest_id.value();
    return manifest == "manyforge.provider.pose_source" ||
           manifest == "manyforge.provider.ai_model";
  });
  const bool needs_ros = stream_observer ||
      std::any_of(rows.value().begin(), rows.value().end(), [](const auto &row) {
    return row.provider.profile == "ros_bool" ||
           row.provider.profile == "ros_set_bool" ||
           row.provider.profile == "ros2_control_gpio";
  });
  auto transport = shared_transport;
  const bool created_transport = needs_ros && !transport;
  if (created_transport) {
    transport = std::make_shared<ros2_control_execution::Ros2TransportResources>(
        "manyforge_generic_io", "/", 3U, ros_context_provider);
    // No component health reads this host-wide transport: its executor fault
    // contains the cell like any other safety-path callback throw.
    transport->set_executor_fault_observer(
        [scheduler = cell.scheduler()](const std::string_view reason_code) {
          scheduler->contain_safety_path_fault(reason_code);
        });
  }
  composition.io_transport = transport;
  const auto prepared = PrepareCompositionIoProviderResources(composition, cell,
      transport ? transport->node() : nullptr, existing);
  if (!prepared.ok())
    throw core::TypedError(prepared.error().code,
                           "actual I/O resource preparation failed: " +
                               prepared.error().message,
                           prepared.error().details);
  if (stream_observer) {
    // One bridge per host process: Python observer sinks push samples
    // into the streams their host-side driver declared.
    static std::mutex bridge_mutex;
    static std::weak_ptr<void> installed_bridge;
    std::lock_guard bridge_lock(bridge_mutex);
    auto bridge = installed_bridge.lock();
    if (!bridge) {
      bridge = InstallComponentStreamSampleBridge(transport->node(),
                                                  host_component_stream_health());
      installed_bridge = bridge;
    }
    composition.component_stream_sample_bridge = std::move(bridge);
  }
  if (created_transport) {
    transport->start();
    // Publish only a successfully prepared, running host transport. Later
    // compositions reuse its executor without starting it a second time.
    shared_transport = std::move(transport);
  }
}

// Candidate resources may acquire real samples before successful Apply, but
// the active Kernel service/catalog changes only at the terminal owner commit.
void InstallCompositionIoSignals(
    host_detail::ExecutionProduct &composition, RetainedCellRuntime &cell,
    const core::ConfigurationFenceStatusQueryDTO *held_fence = nullptr,
    const core::CellAdmissionClaimDTO *actual_parent = nullptr) {
  Require((held_fence == nullptr) == (actual_parent == nullptr),
          "global candidate installation lost its exact Apply parent");
  const auto candidate = composition.io_signal_runtime
      ? composition.io_signal_runtime->service() : nullptr;
  const core::IoRuleAppliedContextDTO context{
      cell.addressed_kernel().request_owner_epoch(),
      composition.publication_generation,
      composition.bundle->retained_bundle_id(),
      composition.bundle->value().cell_graph.graph_revision};
  Require(!composition.compiled_global_io_rules ||
              !composition.compiled_waiting_global_io_rules,
          "global candidate has two compiled products");
  const bool same_catalog =
      candidate && composition.compiled_global_io_rules &&
      composition.io_signal_service == candidate &&
      composition.compiled_global_io_rules->context.owner_epoch ==
          context.owner_epoch &&
      composition.compiled_global_io_rules->context
              .active_publication_generation ==
          context.active_publication_generation &&
      composition.compiled_global_io_rules->context.retained_bundle_id ==
          context.retained_bundle_id &&
      composition.compiled_global_io_rules->context.graph_revision ==
          context.graph_revision;
  const bool same_waiting =
      !candidate && !composition.program && !composition.program_binding &&
      !composition.compiled_global_io_rules &&
      composition.compiled_waiting_global_io_rules &&
      composition.io_signal_service == candidate &&
      composition.compiled_waiting_global_io_rules->context == context;
  if (!same_catalog && !same_waiting) {
    const auto compiled = CompileGlobalIoRuleCatalog(
        context, *composition.bundle, composition.program.get(),
        composition.program_binding.get(), candidate);
    Require(compiled.ok(),
            "actual global rule compilation failed: " +
                (compiled.ok() ? std::string{} : compiled.error().message));
    composition.compiled_global_io_rules = compiled.value().catalog;
    composition.compiled_waiting_global_io_rules =
        compiled.value().waiting_for_program;
  } else if (same_catalog) {
    const auto actual = candidate->RuleServiceConfiguration();
    Require(
        actual.ok() && actual.value().owner_epoch == context.owner_epoch &&
            actual.value().catalog_generation ==
                composition.compiled_global_io_rules->context
                    .catalog_generation,
        "retained global compiler product lost its actual service identity");
  }
  if (held_fence) {
    const auto staged = cell.addressed_kernel().StageGlobalIoRuleCatalog(
        context, composition.compiled_global_io_rules, candidate, *held_fence,
        *actual_parent, composition.compiled_waiting_global_io_rules);
    Require(staged.ok(),
            "stable Kernel rejected actual staged global rules: " +
                (staged.ok() ? std::string{} : staged.error().message));
  }
  if (composition.io_signal_runtime && composition.io_signal_service != candidate)
    composition.io_signal_runtime->Start();
  composition.io_signal_service = candidate;
}

// Capture the same immutable sealed product as normal participant resolution.
// The inventory is supplied by the actual stable-cell publication each time;
// authored direct I/O IDs grant nothing unless that actual output is composed.
[[nodiscard]] RetainedRuntimeParticipantResolverFactory
NativeCellParticipantResolverFactory(const Composition &cell) {
  Require(cell.target_resolver != nullptr,
          "native cell has no product-backed participant resolver");
  std::vector<core::ProgramBindingId> direct_io;
  for (const auto &binding : cell.direct_control_io_program_bindings)
    direct_io.push_back(binding->program_binding_id());
  return [resolver = cell.target_resolver, direct_io = std::move(direct_io)](
             std::vector<core::RuntimeParticipantBinding> current) {
    auto intrinsic = direct_io;
    std::erase_if(intrinsic, [&](const auto &id) {
      return std::none_of(current.begin(), current.end(), [&](const auto &row) {
        return row.value().program_binding_id == id;
      });
    });
    auto product = resolver->WithComposedParticipants(std::move(current),
                                                     std::move(intrinsic));
    return RetainedRuntimeParticipantResolver{
        [product](const core::OperationIntentDTO &operation, std::size_t index) {
          return product->ResolveExecutionParticipant(operation, index);
        }};
  };
}

[[nodiscard]] core::RuntimeParticipantAdmissionResolver
NativeCellParticipantResolver(
    const Composition &cell,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations,
    RetainedRuntimeParticipantResolverFactory *staged_factory = nullptr) {
  auto factory = NativeCellParticipantResolverFactory(cell);
  std::vector<core::RuntimeParticipantBinding> current;
  for (const auto &row : registrations) current.push_back(row.binding);
  auto resolver = make_retained_cell_kernel_participant_resolver(
      factory(std::move(current)), registrations);
  if (staged_factory) staged_factory->swap(factory);
  return resolver;
}

// Caller retains the same composition/install exclusion from exact Kernel
// publication through adoption, before any command access.
// Core locks have returned; the native method takes only its existing route
// mutex. All factory allocation preceded publication; adoption swaps it.
void AdoptNativeCellParticipantResolverFactory(
    Composition &cell, RetainedRuntimeParticipantResolverFactory &&factory) {
  if (cell.runtime)
    cell.runtime->AdoptVerifiedParticipantResolverFactory(std::move(factory));
}

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
CellControlBindingRepresentatives(const Composition &composition) {
  auto representatives =
      composition.candidate_control_representatives.empty()
          ? (composition.runtime != nullptr
                 ? composition.runtime->control_binding_representatives
                 : composition.gripper_runtime
                     ? std::vector<core::RuntimeParticipantBinding>{composition.gripper_runtime->binding()}
                     : std::vector<core::RuntimeParticipantBinding>{})
          : composition.candidate_control_representatives;
  for (const auto &participant : composition.participant_compositions) {
    auto nested = CellControlBindingRepresentatives(*participant);
    representatives.insert(representatives.end(),
                           std::make_move_iterator(nested.begin()),
                           std::make_move_iterator(nested.end()));
  }
  return representatives;
}

/// The route publication owner of a cell without an arm runtime (M7's
/// gripper-only cell port): the first gripper is the root, the others its
/// peers by domain. A reconciled publication replaces the gripper's rows in
/// the cell's installed registration set and resolves participants with the
/// current product's resolver. None without a gripper.
[[nodiscard]] std::shared_ptr<RetainedGripperRoutePublicationOwner>
MakeGripperOnlyCellRouteOwner(std::vector<std::shared_ptr<RetainedGripperRuntime>> grippers,
                              RetainedCellRuntime &cell,
                              const ExecutionCompositionAccess &access) {
  if (grippers.empty()) return nullptr;
  auto candidate = [&cell, &access](const auto &, const auto &replacements) {
    using Result = core::Result<RetainedGripperRoutePublicationOwner::KernelCandidate>;
    auto rows = cell.participant_registrations();
    for (const auto &row : replacements) {
      const auto &next = row.binding.value();
      const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &before) {
        const auto &value = before.binding.value();
        return before.engine == row.engine && value.control_path && next.control_path &&
               value.controllable_component_id == next.controllable_component_id &&
               value.command_target_id == next.command_target_id &&
               value.capability_id == next.capability_id &&
               value.control_path->control_binding_id == next.control_path->control_binding_id;
      });
      if (found == rows.end())
        return Result::Err({"missing_reference",
                            "A gripper route candidate has no installed registration.", {}});
      *found = row;
    }
    // `open_routes` holds the composition lock, so `current` is installed.
    std::vector<core::RuntimeParticipantBinding> composed;
    for (const auto &row : rows) composed.push_back(row.binding);
    auto resolver = make_retained_cell_kernel_participant_resolver(
        NativeCellParticipantResolverFactory(*access.current)(std::move(composed)), rows);
    return Result::Ok({std::move(rows), std::move(resolver), {}});
  };
  std::map<core::ControlDomainId, std::shared_ptr<RetainedGripperRoutePublicationOwner>> peers;
  std::shared_ptr<RetainedGripperRoutePublicationOwner> root;
  for (auto &gripper : grippers) {
    const auto domain = gripper->binding().value().control_path->control_domain_id;
    auto owner = std::make_shared<RetainedGripperRoutePublicationOwner>(
        std::move(gripper), *cell.clock(), cell.addressed_kernel(), candidate, &cell);
    if (!root) root = std::move(owner);
    else peers.emplace(domain, std::move(owner));
  }
  root->BindParticipantPeers(root, std::move(peers));
  return root;
}

void ConfigureProgramResetPositionAccess(ExecutionCompositionAccess& access,
                                        const core::RequestOwnerEpoch& owner_epoch) {
  // Callers install callbacks before ingress. Component states come from the
  // cell's component runtime (R4), never a second lifecycle owner.
  access.reset_robot_position_capability = [&access](Composition& composition) {
    using Result = core::Result<ProgramResetPositionCapability>;
    try {
      const auto components = CaptureMockResetPositionComponents(
          composition, access.component_runtime->Read().states);
      if (!components.ok()) return Result::Ok({false, components.error().message});
      return Result::Ok({true, std::nullopt});
    } catch (const std::exception& error) {
      return Result::Ok({false, std::string("Position reset capability is unavailable: ") + error.what()});
    }
  };
  access.reset_robot_position = [owner_epoch, &access](Composition& composition,
      const ProgramResetPositionRequest& request, std::stop_token stop_token) {
    try {
      return RunMockResetPosition(composition, access.component_runtime->Read().states,
                                  request, owner_epoch, stop_token);
    } catch (const std::exception& error) {
      return core::Result<void>::Err(ResetPositionError("reset_position_effect_exception", error.what(), true));
    }
  };
}

void PrepareInstalledMeasuredSourcesAfterContext(const Composition &composition) noexcept {
  try {
    for (const auto &[component, view] : IndexNativeComponentCompositions(composition)) {
      static_cast<void>(component);
      if (!view) continue;
      if (!view->runtime && view->gripper_runtime) {
        auto prepared = view->gripper_runtime->PrepareCurrentMeasuredSources();
        if (!prepared.ok()) std::cerr << "manyforge measured gripper source unavailable: component="
            << component.value() << " code=" << prepared.error().code << " message=" << prepared.error().message << '\n';
        continue;
      }
      if (!view->runtime) continue;
      const auto bindings = RetainedComponentOwner(view->runtime).physical_bindings();
      const bool has_geometry = std::any_of(bindings.begin(), bindings.end(),
          [](const auto &binding) {
        return std::any_of(binding.value().resources.begin(), binding.value().resources.end(),
                           [](const auto &resource) { return resource.source_joint_name.has_value(); });
      });
      if (has_geometry) {
        // Stage only after the existing context factory qualified its closure.
        // Missing measurement does not add Apply/lifecycle admission rules.
        const auto prepared = view->runtime->PrepareCurrentMeasuredSources();
        if (!prepared.ok()) {
          std::cerr << "manyforge measured source preparation: component=" << component.value()
                    << " code=" << prepared.error().code
                    << " message=" << prepared.error().message << '\n';
        }
      }
    }
  } catch (const std::exception &error) {
    // This new optional measurement preparation must not convert an already
    // qualified context into an Apply failure. Existing capture stays absent.
    try { std::cerr << "manyforge measured source preparation failed: " << error.what() << '\n'; }
    catch (...) {}
  } catch (...) {
    try { std::cerr << "manyforge measured source preparation failed: unknown exception\n"; }
    catch (...) {}
  }
}

struct ManagedBundleAtPinnedRoot final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::filesystem::path package_root;
};

/// Resolve an exact retained bundle from the two directory descriptors pinned
/// for this managed child. The selected package root has precedence; the
/// activation root is only the explicitly supplied fallback, never an
/// ambient filesystem search path.
[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadManagedBundleAtPinnedRoot(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id) {
  const std::array<std::filesystem::path, 2U> roots{
      config.package_root, config.program_artifact_root};
  for (std::size_t index = 0U; index < roots.size(); ++index) {
    const auto &root = roots[index];
    if (root.empty() || (index == 1U && root == roots.front())) continue;
    const deployment::FilesystemVerifiedRetainedBundleSource source(root);
    auto bundle = source.get_verified(retained_bundle_id);
    if (bundle != nullptr)
      return ManagedBundleAtPinnedRoot{std::move(bundle), root};
  }
  return std::nullopt;
}

[[nodiscard]] std::filesystem::path ManagedProgramArtifactRoot(
    const ManagedRuntimeChildConfig &config,
    const std::filesystem::path &selected_package_root) {
  return config.program_artifact_root == selected_package_root
             ? std::filesystem::path{}
             : config.program_artifact_root;
}

[[nodiscard]] int ManagedPackageRootDescriptor(
    const ManagedRuntimeChildConfig &config,
    const std::filesystem::path &selected_package_root) {
  if (selected_package_root == config.package_root) {
    return config.package_root_fd;
  }
  if (!config.program_artifact_root.empty() &&
      selected_package_root == config.program_artifact_root) {
    return config.program_artifact_root_fd;
  }
  Fail("managed candidate package root is not one of the pinned descriptors");
}

// The zero-component path uses the same actual sealed products and source
// service as a physical composition, without manufacturing a component owner.
[[nodiscard]] std::shared_ptr<host_detail::ExecutionProduct>
BuildManagedLogicalExecutionProduct(
    const ManagedRuntimeChildConfig &config,
    std::shared_ptr<const deployment::RetainedBundle> bundle,
    const std::filesystem::path &package_root,
    const core::ProjectionRevisionDTO &projection,
    core::ActiveBundlePublicationGeneration publication,
    const core::NamedApplyPublicationTransactionDTO &origin,
    const std::shared_ptr<core::IClock> &clock,
    const std::shared_ptr<core::IScheduler> &scheduler) {
  Require(bundle && bundle->value().cell_graph.controllable_components.empty() &&
              bundle->retained_bundle_id() == projection.retained_bundle_id &&
              bundle->value().cell_graph.graph_revision == projection.graph_revision,
          "logical cell product is not the exact zero-component selection");
  const auto &maps = bundle->value().compiled_products.symbol_role_maps;
  Require(maps.size() <= 1U,
          "logical cell product has ambiguous retained model maps");
  std::optional<deployment::VerifiedRetainedModelMap> map;
  if (!maps.empty()) {
    const deployment::FilesystemVerifiedRetainedModelMapSource source(package_root);
    map = source.get_verified({bundle->retained_bundle_id(), maps.front().product_id});
    Require(map.has_value(), "logical cell retained model map is absent or invalid");
  }
  const auto selected = detail::ReadSelectedProgramProducts(
      config.manager_state_root, core::CellId(config.cell_id), origin, *bundle,
      map ? &*map : nullptr, package_root,
      ManagedProgramArtifactRoot(config, package_root));
  Require(selected.ok(), "logical cell Program products are unavailable: " +
                            (selected.ok() ? std::string{} : selected.error().message));
  auto product = std::make_shared<host_detail::ExecutionProduct>(
      std::move(bundle), selected.value().program, selected.value().binding,
      projection, publication);
  product->origin_named_apply = origin;
  product->selected_program_outcome = selected.value().outcome;
  const auto keys = CompiledProgramBlackboardKeyCapacity(*product);
  Require(keys.ok(), "logical cell blackboard union cannot be compiled");
  if (keys.value() > 0U)
    product->io_signal_runtime = HostIoSignalRuntime::Create(
        core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
        publication.value(), {}, clock, scheduler, keys.value());
  return product;
}

[[nodiscard]] std::optional<CompositionBuildInput>
TryBuildCurrentManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    bool &predecessor_composition_pending) {
  predecessor_composition_pending = false;
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  if (!integrity.current_generation.nonzero()) {
    publication_store.Close();
    return std::nullopt;
  }
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "managed active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  // Verify the published bundle before deciding which projection providers
  // this runtime unit actually requires. A gripper-only unit may require no
  // native planner projection at all.
  auto selected_bundle = LoadManagedBundleAtPinnedRoot(
      config, receipt.request.candidate_retained_bundle_id);
  if (!selected_bundle.has_value()) {
    Fail("managed active publication names an unavailable retained bundle: " +
         receipt.request.candidate_retained_bundle_id.value());
  }
  auto bundle = std::move(selected_bundle->bundle);
  const auto &selected_package_root = selected_bundle->package_root;
  const auto &published_projection_revision =
      receipt.request.candidate_projection_revision;
  if (bundle->value().cell_graph.cell_id != core::CellId(config.cell_id) ||
      bundle->value().cell_graph.graph_revision !=
          receipt.request.candidate_graph_revision ||
      bundle->retained_bundle_id() !=
          published_projection_revision.retained_bundle_id ||
      bundle->value().cell_graph.graph_revision !=
          published_projection_revision.graph_revision) {
    Fail("managed active publication and retained bundle disagree: "
         "publication_bundle=" +
         receipt.request.candidate_retained_bundle_id.value() +
         ", publication_graph=" +
         receipt.request.candidate_graph_revision.value() +
         ", projection_bundle=" +
         published_projection_revision.retained_bundle_id.value() +
         ", projection_graph=" +
         published_projection_revision.graph_revision.value() +
         ", loaded_bundle=" + bundle->retained_bundle_id().value() +
         ", loaded_graph=" +
         bundle->value().cell_graph.graph_revision.value());
  }
  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }
  if (!projections.AllRequiredProjectionsHealthy(
          *bundle, receipt.request.candidate_projection_revision)) {
    predecessor_composition_pending = true;
    return std::nullopt;
  }

  auto component_projections = SelectManagedComponentProjections(
      config, *bundle, projections,
      receipt.request.candidate_projection_revision, true);
  if (!component_projections.has_value()) {
    predecessor_composition_pending = true;
    return std::nullopt;
  }
  const auto projection_revision = component_projections->projection_revision;
  auto projection = component_projections->by_component.empty()
                        ? nullptr
                        : component_projections->by_component.begin()->second;
  // The model map is still mandatory when no motion planner is present: it
  // seals the gripper's component/target identity and any authored frames.
  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      selected_package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");
  auto collision_model = projection != nullptr
                             ? projection->collision_shared_model()
                             : collision::ManyForgeCollisionEngine::SharedModelPtr{};
  Require(projection == nullptr || collision_model != nullptr,
          "managed active projection has no collision model");

  auto planner_scene_projection = projection;
  auto input = CompositionBuildInput{
      selected_package_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
      std::move(bundle),
      std::move(*model_map),
      projection_revision,
      receipt.publication_generation,
      std::move(projection),
      std::move(collision_model),
      {},
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      {},
      {},
      config.manager_run_root,
      std::move(planner_scene_projection),
      {},
      {},
      std::move(component_projections->by_component),
      ManagedProgramArtifactRoot(config, selected_package_root),
      {},
      {},
      ManagedPackageRootDescriptor(config, selected_package_root),
      config.scene_asset_root_fd,
      [&receipt, &config] {
        if (const auto *apply =
                std::get_if<core::NamedApplyPublicationTransactionDTO>(
                    &receipt.request.transaction_ref))
          return "Apply=" + apply->apply_id.value() + " generation=" +
                 std::to_string(apply->apply_generation.value());
        return "Bootstrap runtime=" + config.runtime_unit_id +
               " epoch=" + config.runtime_epoch;
      }(),
  };
  input.required_projection_pins = std::move(component_projections->required_projection_pins);
  input.site_settings = config.site_settings;
  if (const auto *named = std::get_if<core::NamedApplyPublicationTransactionDTO>(
          &receipt.request.transaction_ref)) {
    input.origin_named_apply = *named;
    const auto selected_program = detail::ReadSelectedProgramProducts(
        config.manager_state_root, core::CellId(config.cell_id), *named,
        *input.bundle, input.model_map, input.package_root, input.program_artifact_root);
    Require(selected_program.ok(), "current selected Program products are unavailable");
    input.selected_program_products = selected_program.value();
  }
  return input;
}

[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadExactManagedCandidateBundle(const ManagedRuntimeChildConfig &config,
                                const HostApplySelection &request) {
  auto selected = LoadManagedBundleAtPinnedRoot(
      config, request.candidate_projection_revision.retained_bundle_id);
  Require(selected.has_value() &&
              selected->bundle->value().cell_graph.cell_id == core::CellId(config.cell_id) &&
              selected->bundle->value().cell_graph.graph_revision ==
                  request.candidate_projection_revision.graph_revision,
          "managed selected retained bundle identity differs");
  return selected;
}

struct ManagedBootstrapStopProduct final {
  std::vector<core::ResourceId> resource_inventory;
  bool requires_physical_composition{};
};

[[nodiscard]] ManagedBootstrapStopProduct LoadManagedBootstrapStopProduct(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id,
    const core::GraphRevision &graph_revision) {
  const auto selected =
      LoadManagedBundleAtPinnedRoot(config, retained_bundle_id);
  Require(selected.has_value() && selected->bundle != nullptr &&
              selected->bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              selected->bundle->retained_bundle_id() == retained_bundle_id &&
              selected->bundle->value().cell_graph.graph_revision ==
                  graph_revision,
          "managed bootstrap Stop policy differs from the retained product");

  std::set<core::ResourceId> resources;
  for (const auto &binding :
       selected->bundle->value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id.value() != config.runtime_unit_id)
      continue;
    for (const auto &resource : binding.resource_bindings)
      resources.insert(resource.resource_id);
  }
  Require(resources.size() <= core::kMaxKernelResourceSlots,
          "managed bootstrap resource inventory exceeds the Kernel bound");
  return {
      {resources.begin(), resources.end()},
      !selected->bundle->value().cell_graph.controllable_components.empty()};
}

[[nodiscard]] std::optional<CompositionBuildInput> BuildManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    const HostApplySelection &request,
    std::shared_ptr<WallClock> shared_clock,
    std::shared_ptr<SimpleScheduler> shared_scheduler) {
  auto selected_candidate = LoadExactManagedCandidateBundle(config, request);
  Require(selected_candidate.has_value(),
          "managed selected retained bundle is unavailable from pinned roots");
  auto bundle = std::move(selected_candidate->bundle);
  const auto &candidate_package_root = selected_candidate->package_root;
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation ==
                  request.expected_predecessor_publication_generation &&
              integrity.current_pointer_digest ==
                  request.expected_predecessor_public_pointer_digest.value(),
          "managed composition predecessor publication changed before "
          "installation");
  const auto publication = publication_store.ReadActiveBundlePublication(
      request.expected_predecessor_publication_generation);
  Require(publication.ok() && publication.value().valid() &&
              publication.value().publication_generation ==
                  request.expected_predecessor_publication_generation &&
              publication.value().durable_public_pointer_content_digest ==
                  request.expected_predecessor_public_pointer_digest,
          "managed predecessor publication cannot be verified");
  publication_store.Close();

  const auto predecessor_location = LoadManagedBundleAtPinnedRoot(
      config, request.predecessor_projection_revision.retained_bundle_id);
  const auto predecessor = predecessor_location.has_value()
                               ? predecessor_location->bundle
                               : nullptr;
  Require(predecessor != nullptr &&
              predecessor->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              predecessor->value().cell_graph.graph_revision ==
                  request.predecessor_projection_revision.graph_revision &&
              deployment::derive_apply_component_delta(
                  *predecessor, *bundle,
                  component_manager::SiteFactsOf(config.site_settings)) ==
                  request.component_delta,
          "managed component delta differs from the verified predecessor and candidate");

  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }

  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      candidate_package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");

  // Candidate projections hold the latest Scene, which may be newer than the
  // revision this Apply recorded at its request.
  auto component_projections = SelectManagedComponentProjections(
      config, *bundle, projections, request.candidate_projection_revision,
      true);
  Require(component_projections.has_value(),
          "managed selected robot projections are not active at the candidate "
          "revision");
  auto projection = component_projections->by_component.empty()
                        ? nullptr
                        : component_projections->by_component.begin()->second;
  auto collision_model = projection != nullptr
                             ? projection->collision_shared_model()
                             : collision::ManyForgeCollisionEngine::SharedModelPtr{};
  Require(projection == nullptr || collision_model != nullptr,
          "managed active projection has no collision model");

  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  auto planner_scene_projection = projection;
  auto input = CompositionBuildInput{
      candidate_package_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
      std::move(bundle),
      std::move(*model_map),
      request.candidate_projection_revision,
      core::ActiveBundlePublicationGeneration(predecessor_generation + 1U),
      std::move(projection),
      std::move(collision_model),
      {},
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      std::move(shared_clock),
      std::move(shared_scheduler),
      config.manager_run_root,
      std::move(planner_scene_projection),
      {},
      {},
      std::move(component_projections->by_component),
      ManagedProgramArtifactRoot(config, candidate_package_root),
      request.component_delta,
      {},
      ManagedPackageRootDescriptor(config, candidate_package_root),
      config.scene_asset_root_fd,
      "Apply=" + request.apply_id.value() +
          " generation=" + std::to_string(request.apply_generation.value()),
  };
  input.required_projection_pins = std::move(component_projections->required_projection_pins);
  input.site_settings = config.site_settings;
  input.origin_named_apply = core::NamedApplyPublicationTransactionDTO{
      request.apply_id, request.apply_generation};
  const auto selected_program = detail::ReadSelectedProgramProducts(
      config.manager_state_root, core::CellId(config.cell_id), *input.origin_named_apply,
      *input.bundle, input.model_map, input.package_root, input.program_artifact_root);
  Require(selected_program.ok(), "candidate selected Program products are unavailable");
  input.selected_program_products = selected_program.value();
  return input;
}

[[nodiscard]] bool IsExactRetainedViewApply(
    const HostApplySelection &request) {
  return !request.component_delta.components.empty() &&
         request.predecessor_projection_revision.retained_bundle_id ==
             request.candidate_projection_revision.retained_bundle_id &&
         request.predecessor_projection_revision.graph_revision ==
             request.candidate_projection_revision.graph_revision &&
         std::all_of(request.component_delta.components.begin(),
                     request.component_delta.components.end(),
                     [](const auto &row) {
                       return row.disposition ==
                              deployment::ApplyComponentDisposition::kRetain;
                     });
}

void RefreshRetainedPublicationView(
    Composition &cell,
    const std::map<core::ControllableComponentId,
                   std::shared_ptr<RetainedPlannerSceneProjection>>
        &projections,
    const std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> &required_projection_pins,
    const core::ProjectionRevisionDTO &revision,
    const core::ActiveBundlePublicationGeneration generation,
    const std::filesystem::path &manager_run_root) {
  // Copy before any view mutation; allocation failure retains the old owners.
  auto replacement_pins = required_projection_pins;
  const auto replace_pins = [&] {
    for (auto &peer : cell.participant_compositions)
      peer->required_projection_pins.clear();
    cell.required_projection_pins = std::move(replacement_pins);
  };
  std::map<core::ControllableComponentId, Composition *> components;
  components.emplace(cell.controllable_component_id, &cell);
  for (auto &peer : cell.participant_compositions) {
    Require(peer != nullptr &&
                components.emplace(peer->controllable_component_id,
                                   peer.get()).second,
            "retained native publication has duplicate component owners");
  }
  if (projections.empty()) {
    // No component advertises a native planner projection. Scene revision is
    // still owned by the canonical Scene; there is no planner to re-anchor.
    for (const auto &[component_id, composition] : components) {
      static_cast<void>(component_id);
      Require((composition->gripper_runtime != nullptr || composition->io_provider_drivers) &&
                  composition->runtime == nullptr &&
                  composition->planner_scene_projection == nullptr,
              "unprojected retained view contains a motion controller");
      composition->projection_revision = revision;
      composition->publication_generation = generation;
    }
    replace_pins();
    return;
  }
  const auto projected_components = std::count_if(components.begin(), components.end(), [](const auto& row) {
    return row.second->runtime || row.second->gripper_runtime;
  });
  Require(static_cast<std::size_t>(projected_components) == projections.size(),
          "retained native publication projection set changed components");
  // First validate the entire candidate set. A failed validation does not
  // re-anchor even one planner; any later effect still runs under admission.
  for (const auto &[component_id, composition] : components) {
    if (!composition->runtime && !composition->gripper_runtime) {
      Require((composition->io_provider_drivers || IsAdapterOnlyView(*composition)) &&
                  !projections.contains(component_id),
              "model-free I/O component has an unexpected physical projection");
      continue;
    }
    const auto projection = projections.find(component_id);
    Require(projection != projections.end() && projection->second != nullptr &&
                composition->component_runtime_identity() != nullptr &&
                composition->planner_scene_projection == projection->second,
            "retained native controller has no stable Scene projection owner");
    if (composition->runtime != nullptr) {
      Require(composition->runtime->planner != nullptr &&
                  composition->runtime->planning_scene_synchronizer != nullptr,
              "retained motion controller has no stable planning owner");
    }
    const auto inventory =
        projection->second->current_scene_snapshot_lease_inventory();
    Require(inventory.has_value() &&
                core::SameProjectionIdentity(
                    inventory->projection_binding.projection_revision, revision),
            "retained native projection does not match the candidate Scene");
  }
  for (const auto &[component_id, composition] : components) {
    if (!composition->runtime && !composition->gripper_runtime) {
      composition->projection_revision = revision;
      composition->publication_generation = generation;
      continue;
    }
    const auto &projection = projections.at(component_id);
    const auto inventory =
        projection->current_scene_snapshot_lease_inventory();
    if (composition->runtime != nullptr) {
      auto &runtime = *composition->runtime;
      const auto rebound = runtime.planner->RebindProjection(
          runtime.planner->projection_binding(), inventory->projection_binding);
      if (!rebound.ok())
        Fail("retained native planner rejected the candidate Scene: " +
             rebound.error().message);
      runtime.planning_scene_synchronizer->ReplaceTarget(
          std::make_shared<ManagerPlanningSceneSynchronizer>(
              manager_run_root, revision.retained_bundle_id,
              revision.graph_revision, generation, projection));
    }
    composition->projection_revision = revision;
    composition->publication_generation = generation;
  }
  replace_pins();
}

[[nodiscard]] int
RunManagedRuntimeChild(const ManagedRuntimeChildConfig &config) {
  // This host's site facts (its site settings snapshot, fixed for the
  // process): every component spec it derives uses them, as the Manager does.
  const auto site_facts = component_manager::SiteFactsOf(config.site_settings);
  using ShutdownPhase = component_manager::detail::NativeShutdownPhase;
  using component_manager::detail::NativeShutdownOwnerPhase;
  component_manager::detail::NativeShutdownProjection shutdown_projection(
      config.runtime_epoch);
  const auto host_ros_context_provider = MakeHostRosContextProvider();
  const auto host_motion_trace =
      std::make_shared<ros2_control_execution::VolatileMotionTrace>();
  // These outlive every borrowed host owner and the SAME servicing lane.
  std::mutex manager_liveness_failure_mutex;
  std::exception_ptr manager_liveness_failure;
  std::mutex manager_liveness_servicing_mutex;
  bool teardown_only = false; // protected by servicing mutex
  std::atomic<bool> shutdown_status_servicing{false};
  std::jthread manager_liveness_lane;
  NativeShutdownOwnerPhase liveness_lane_destructor_phase{
      shutdown_projection, ShutdownPhase::kServicingLaneDrain,
      manager_liveness_lane};
  const auto offer_digest = SelectedExecutionOfferDigest();
  const std::string ready_packet =
      "manyforge.execution-host-ready.v2 " + config.runtime_unit_id + " " +
      config.runtime_epoch + " " + offer_digest + " " +
      config.product_identity + " " + config.configuration_identity;
  std::uint64_t source_report_sequence = 0U;
  ManagedKernelAdmissionGate admission_gate;
  // Set once to the main-scope cell owner before managed requests are served.
  // Product installations retain this Kernel; the fence callback never looks
  // up a replaceable component composition or calls back into the fence owner.
  core::ResourceAddressedExecutionKernel *stable_kernel_owner{};
  ManagedKernelApplyFenceOwner fence_owner(
      ManagedKernelApplyFenceOwnerConfig{
          config.execution_state_root / "kernel-apply-fence.sqlite3",
          config.manager_state_root,
          core::CellId(config.cell_id),
          core::RuntimeUnitId(config.runtime_unit_id),
          core::RuntimeEpoch(config.runtime_epoch),
          core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
          [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
          },
      },
      admission_gate);
  NativeShutdownOwnerPhase fence_destructor_phase{
      shutdown_projection, ShutdownPhase::kFenceOwnerDestruction, fence_owner};
  const ManagedProjectionRegistryConfig projection_config{
      config.package_root,       config.program_artifact_root,
      config.execution_run_root, config.runtime_unit_id,
      config.runtime_epoch,      config.scene_asset_root_fd};
  // The registry outlives the follower; disconnect before follower teardown.
  struct ProjectionSnapshotWake final {
    std::mutex mutex;
    std::function<void()> notify;
  } projection_snapshot_wake;
  ManagedProjectionRegistry projections(
      projection_config,
      MakeFilesystemManagedProjectionAssetSource(projection_config),
      [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
      },
      [&] {
        std::lock_guard lock(projection_snapshot_wake.mutex);
        if (projection_snapshot_wake.notify) projection_snapshot_wake.notify();
      });
  NativeShutdownOwnerPhase projection_destructor_phase{
      shutdown_projection, ShutdownPhase::kProjectionDestruction, projections};
  // CONTRACTS §6: the installed adapter families (ament resource index
  // `manyforge_component_adapters`). Declared before every composition and
  // the component runtime: their family adapters are code of these loaded
  // libraries, so the registry (which unloads them) is destroyed last.
  const auto adapter_registry = LoadInstalledAdapterRegistry();
  std::mutex managed_composition_mutex;
  // Bounded observation history on this host owner, not Composition pins.
  // Replacing a composition/Apply never depends on reading these receipts.
  struct ComponentProcessStopResultEntry final {
    component_manager::ManagerComponentProcessStopResultDTO result;
    std::uint64_t insertion_order;
  };
  std::map<std::string, ComponentProcessStopResultEntry> component_process_stop_results;
  std::uint64_t component_process_stop_result_order{};
  bool component_process_stop_retention_exhaustion_logged{};
  // A failed fenced installation may already have re-rooted retained
  // participants toward the private candidate. Keep that candidate alive
  // until after the old active/retired views are destroyed; discarding it
  // would strand non-owning root and Stop-reader pointers while the Apply
  // remains command-closed.
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> generic_io_transport;
  std::unique_ptr<Composition> failed_prepared_composition;
  NativeShutdownOwnerPhase failed_destructor_phase{
      shutdown_projection, ShutdownPhase::kFailedCompositionDestruction,
      failed_prepared_composition};
  std::unique_ptr<Composition> active_managed_composition;
  NativeShutdownOwnerPhase active_destructor_phase{
      shutdown_projection, ShutdownPhase::kActiveCompositionDestruction,
      active_managed_composition};
  std::unique_ptr<Composition> retired_managed_composition;
  NativeShutdownOwnerPhase retired_destructor_phase{
      shutdown_projection, ShutdownPhase::kRetiredCompositionDestruction,
      retired_managed_composition};
  // Snapshot engine identities separately: the runtime registration vector
  // remains authoritative and may be shared with a future active wrapper.
  std::set<core::IResolvedParticipantExecutionEngine *>
      retired_registration_engines;
  std::unique_ptr<Composition> prepared_managed_composition;
  NativeShutdownOwnerPhase prepared_destructor_phase{
      shutdown_projection, ShutdownPhase::kPreparedCompositionDestruction,
      prepared_managed_composition};
  std::shared_ptr<WallClock> detached_owner_clock;
  std::shared_ptr<SimpleScheduler> detached_owner_scheduler;
  NativeShutdownOwnerPhase scheduler_destructor_phase{
      shutdown_projection, ShutdownPhase::kSchedulerOwnerRelease,
      detached_owner_scheduler};
  std::unique_ptr<RetainedCellRuntime> stable_cell_runtime_owner;
  NativeShutdownOwnerPhase cell_kernel_destructor_phase{
      shutdown_projection, ShutdownPhase::kKernelDestruction,
      stable_cell_runtime_owner};
  std::shared_ptr<RetainedObservationWake> stable_observation_wake;
  std::unique_ptr<core::SqliteOperationJournal> detached_operation_journal;
  std::unique_ptr<core::ResourceAddressedExecutionKernel>
      detached_addressed_kernel;
  NativeShutdownOwnerPhase detached_kernel_destructor_phase{
      shutdown_projection, ShutdownPhase::kKernelDestruction,
      detached_addressed_kernel};
  // The stable Kernel owns the Stop latch across product reconfiguration.
  // Command dispatch already pins the active composition with its shared
  // lifetime lock, so this atomic selects only that same current product's
  // facts adapter; it is not a second recovery owner.
  core::SqliteOperationJournal *stable_operation_journal{};
  const auto read_current_publication = [&] {
    component_manager::SqliteActiveBundlePublicationStore publication_store(
        config.manager_state_root /
            std::string(
                component_manager::kActiveBundlePublicationStoreFilename),
        core::CellId(config.cell_id));
    const auto integrity = publication_store.VerifyIntegrity();
    Require(integrity.current_generation.nonzero(),
            "managed active retained-bundle publication is absent");
    auto publication = publication_store.ReadActiveBundlePublication(
        integrity.current_generation);
    publication_store.Close();
    Require(publication.ok() && publication.value().valid(),
            "managed active retained-bundle publication cannot be verified");
    return std::move(publication.value());
  };
  auto bootstrap_retained_bundle_id = config.retained_bundle_id;
  auto bootstrap_graph_revision = config.graph_revision;
  if (!config.program_cycle_journal.empty()) {
    const auto retained_run =
        detail::ReadActiveProgramRunProductForReconciliation(
            config.program_cycle_journal);
    if (!retained_run.ok()) {
      Fail(retained_run.error().message);
    }
    if (retained_run.value().has_value()) {
      bootstrap_retained_bundle_id = retained_run.value()->retained_bundle_id;
      bootstrap_graph_revision = retained_run.value()->graph_revision;
    }
  }
  detached_owner_clock = std::make_shared<WallClock>();
  detached_owner_scheduler = std::make_shared<SimpleScheduler>(
      SimpleSchedulerConfig::from_environment());
  // Keep the existing stable scheduling substrate through product exchanges.
  // The first Background-role startup belongs to construction, never status.
  const auto controller_cleanup_scheduler = detached_owner_scheduler;
  const auto controller_cleanup_clock = detached_owner_clock;
  static_cast<void>(controller_cleanup_scheduler->schedule(
      controller_cleanup_clock->now(), [] {},
      core::SchedulerPriority::Background));
  struct GenericIoTransportQuiescence final {
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> &resources;
    ~GenericIoTransportQuiescence() {
      if (resources) resources->quiesce();
    }
  } generic_io_quiescence{generic_io_transport};
  core::ResourceAddressedExecutionKernelConfig bootstrap_kernel_config{
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::CellId(config.cell_id),
      1024U,
      {},
      &admission_gate,
      std::nullopt};
  bootstrap_kernel_config.io_rule_evidence_processing_notification =
      [](const std::shared_ptr<core::IIoSignalService>& affected) {
        // Core invokes this only for the actual affected service, after its
        // health/state scopes unwind. Queue work; never re-enter the Kernel.
        if (auto runtime = std::dynamic_pointer_cast<HostIoSignalRuntime>(affected))
          runtime->RequestRuleEvidenceProcessing();
      };
  bootstrap_kernel_config.empty_runtime_retained_bundle_id =
      bootstrap_retained_bundle_id;
  bootstrap_kernel_config.empty_runtime_graph_revision =
      bootstrap_graph_revision;
  bootstrap_kernel_config.prequalified_max_live_stop_participants =
      kNativeMaximumLiveStopParticipants;
  auto bootstrap_stop = LoadManagedBootstrapStopProduct(
      config, bootstrap_retained_bundle_id, bootstrap_graph_revision);
  const auto bootstrap_cell = LoadManagedBundleAtPinnedRoot(config, bootstrap_retained_bundle_id);
  Require(bootstrap_cell && bootstrap_cell->bundle,
          "Isaac attach bootstrap has no verified retained bundle");
  const bool bootstrap_requires_physical_composition =
      bootstrap_stop.requires_physical_composition;
  bootstrap_kernel_config.empty_runtime_resource_inventory =
      std::move(bootstrap_stop.resource_inventory);
  bootstrap_kernel_config.cell_admission_claim_capacity = 1024U;
  // The managed cell hosts the Headless Alarm Lifecycle Owner: Run/Resume
  // and motion-producing recovery evaluate its published attention. The
  // control plane is single-host: only projections from this host boot are
  // accepted, so owner incarnations are comparable (spec 530 §11).
  bootstrap_kernel_config.operator_alarm_attention =
      core::OperatorAlarmAttentionPolicyDTO{
          core::kOperatorAlarmAttentionMaximumSnapshotAgeNs,
          core::ReadHostBootStamp().boot_id};
  bootstrap_kernel_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.program_cycle_journal,
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.logical_scene_database, config.program_cycle_journal,
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_program_reset_status_source =
      [path = config.program_cycle_journal](const auto purpose, const auto& intent, const auto& claim) {
        return detail::ReadProgramResetStatusReadOnly(path, purpose, intent, claim);
      };
  RetainedCellRuntimeInput cell_input{
      .clock = detached_owner_clock,
      .scheduler = detached_owner_scheduler,
      .operation_journal_path =
          config.execution_state_root / std::string(core::kOperationJournalFilename),
      .participant_registrations = {},
      .kernel_config = std::move(bootstrap_kernel_config)};
  stable_cell_runtime_owner = std::make_unique<RetainedCellRuntime>(std::move(cell_input));
  stable_observation_wake = stable_cell_runtime_owner->observation_wake();
  stable_kernel_owner = &stable_cell_runtime_owner->addressed_kernel();
  stable_operation_journal = &stable_cell_runtime_owner->operation_journal();
  Require(stable_kernel_owner != nullptr && stable_operation_journal != nullptr,
          "managed native cell owner could not be created");

  if (bootstrap_requires_physical_composition) {
    Require(stable_kernel_owner->SetRouteRecoveryPending(true).ok(),
            "selected physical runtime could not close admission before "
            "composition initialization");
  }
  // Active TCP (M7, design 4): each robot appends its confirmed changes to
  // the stable cell journal (the `events` stream and History), stamped on
  // the Kernel's clock, under its own lock. A failed write is logged; results
  // and reads still carry the value and its sequence.
  const auto bind_active_tcp_events = [&](const Composition &view) {
    if (!view.ros2_control_runtime_handle) return;
    view.ros2_control_runtime_handle->BindActiveTcpEvents(
        [kernel = stable_kernel_owner, journal = stable_operation_journal,
         clock = detached_owner_clock](core::ActiveTcpChangeDTO change) {
          change.occurred_at_ns = clock->now();
          const auto recorded =
              journal->RecordActiveTcpChanged(kernel->request_owner_epoch(), change);
          if (!recorded.ok())
            std::cerr << "manyforge Active TCP change not journaled: "
                      << recorded.error().code << ": " << recorded.error().message << std::endl;
        });
  };
  admission_gate.BindResourceTable(
      stable_kernel_owner->resource_admission_table());
  component_manager::detail::ManagerLivenessCommandAdmission
      manager_liveness_admission;
  auto manager_liveness_scheduler = std::make_unique<SimpleScheduler>(
      SimpleSchedulerConfig::from_environment());
  // Its RuntimeSafety Expire callback is a safety path: a throw contains the
  // cell through the same sink as the shared scheduler. The stable Kernel is
  // declared first, so it outlives this receiver and its workers.
  manager_liveness_scheduler->set_safety_path_fault_sink(
      [kernel = stable_kernel_owner](const std::string_view reason_code) {
        kernel->ContainInternalFault(reason_code);
      });
  component_manager::detail::ChildOwnedManagerLivenessReceiver
      manager_liveness_receiver(std::make_shared<WallClock>(),
                                std::move(manager_liveness_scheduler));
  NativeShutdownOwnerPhase receiver_destructor_phase{
      shutdown_projection, ShutdownPhase::kLivenessReceiverDestruction,
      manager_liveness_receiver};
  std::atomic<bool> manager_liveness_installed{false};
  std::optional<core::ManagerLivenessReceiverConfig>
      manager_liveness_receiver_config;
  std::optional<core::ActiveBundlePublicationGeneration>
      manager_liveness_publication_generation;
  std::optional<core::ContentDigest> manager_liveness_public_pointer_digest;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
  std::set<std::pair<std::string, std::string>> retired_manager_epochs;
  auto execution_composition_owner =
      std::make_shared<ExecutionCompositionAccess>();
  execution_composition_owner->motion_trace = host_motion_trace;
  auto &execution_composition = *execution_composition_owner;
  const auto bind_cell_runtime_to_composition =
      [&](Composition &composition) {
        bind_active_tcp_events(composition);
        for (const auto &participant : composition.participant_compositions)
          if (participant != nullptr) bind_active_tcp_events(*participant);
        composition.cell_runtime = stable_cell_runtime_owner.get();
        std::vector<std::shared_ptr<RetainedGripperRuntime>>
            independent_grippers;
        if (composition.gripper_runtime != nullptr) {
          composition.gripper_runtime->BindCellKernel(*stable_kernel_owner);
          independent_grippers.push_back(composition.gripper_runtime);
        }
        for (const auto &participant : composition.participant_compositions) {
          Require(participant != nullptr,
                  "native cell has a missing component composition");
          participant->cell_runtime = stable_cell_runtime_owner.get();
          if (participant->gripper_runtime != nullptr) {
            participant->gripper_runtime->BindCellKernel(*stable_kernel_owner);
            independent_grippers.push_back(participant->gripper_runtime);
          }
          if (participant->runtime != nullptr)
            participant->runtime->cell_runtime_owner =
                stable_cell_runtime_owner.get();
        }
        if (composition.runtime != nullptr) {
          composition.runtime->cell_runtime_owner =
              stable_cell_runtime_owner.get();
          composition.runtime->BindIndependentGripperPeers(
              composition.runtime, std::move(independent_grippers));
        } else {
          composition.gripper_route_owner = MakeGripperOnlyCellRouteOwner(
              std::move(independent_grippers), *stable_cell_runtime_owner,
              execution_composition);
        }
      };
  // CONTRACTS §6: the host node the installed families' adapters use.
  const auto adapter_transport = std::make_shared<ros2_control_execution::Ros2TransportResources>(
      "manyforge_component_adapters", "/manyforge", 2U, host_ros_context_provider);
  // Spins for the host's lifetime: adapters add their subscriptions and
  // clients to this node as compositions install them.
  adapter_transport->start();
  // CONTRACTS §6: the host entries a family's adapters call.
  const auto restart_component_driver = [&execution_composition](
                                            const std::string &component_id, std::stop_token) {
    return RestartComponentDriver(execution_composition, component_id);
  };
  const ComponentIoUpdateSink component_io_updates =
      [&execution_composition](const std::string &component_id,
                               const std::vector<core::IoSignalUpdateDTO> &updates,
                               const std::int64_t received_at_ns) {
        PublishComponentIoUpdates(execution_composition, component_id, updates, received_at_ns);
      };
  execution_composition.addressed_kernel = stable_kernel_owner;
  execution_composition.operation_journal = stable_operation_journal;
  // CONTRACTS §1-§6: the cell's one component runtime, kept across Apply.
  // Every state change feeds the Kernel's cell latch (§5) and wakes the event
  // stream.
  // A successful `enable` opens the component's routes (`open_routes`)
  // before the action reports success.
  execution_composition.component_runtime = std::make_shared<HostComponentRuntime>(
      *detached_owner_clock,
      HostComponentRuntime::Hooks{
          [kernel = stable_kernel_owner](std::vector<core::ComponentState> states) {
            kernel->observe_component_states(std::move(states));
          },
          [&config, &execution_composition, &stable_cell_runtime_owner](
              const std::string &component_id) {
            std::shared_lock lock(execution_composition.mutex);
            const core::ControllableComponentId component(component_id);
            const auto *selected = execution_composition.product();
            // A component without compiled routes (sensor, I/O, passive,
            // simulator, owner) has nothing to open.
            if (selected == nullptr || !selected->bundle ||
                !HasCompiledComponentRoutes(
                    selected->bundle->value().desired_control_configurations,
                    selected->bundle->value().cell_graph.control_bindings, component))
              return core::Result<void>::Ok();
            const auto unavailable = [&](std::string message) {
              return core::Result<void>::Err(
                  {"component_route_unavailable", std::move(message),
                   {{"component_id", component_id}}});
            };
            const auto *product = execution_composition.current;
            if (product == nullptr || !stable_cell_runtime_owner ||
                (!product->runtime && !product->gripper_route_owner))
              return unavailable("The cell has no installed product with a route owner.");
            const auto package =
                LoadManagedBundleAtPinnedRoot(config, product->bundle->retained_bundle_id());
            if (!package) return unavailable("The installed product's package is unavailable.");
            if (product->runtime)
              return OpenComponentRoutes(*product->bundle, package->package_root,
                                         product->publication_generation, *product->runtime,
                                         *stable_cell_runtime_owner, component);
            return OpenComponentRoutes(*product->bundle, package->package_root,
                                       product->publication_generation,
                                       *product->gripper_route_owner,
                                       *stable_cell_runtime_owner, component);
          },
          [] { core::execution_event_change_signal().Notify(); }});
  // CONTRACTS §2: every Stop the Kernel accepts interrupts the component
  // actions in progress.
  stable_kernel_owner->set_stop_listener(
      [runtime = std::weak_ptr<HostComponentRuntime>(execution_composition.component_runtime)] {
        if (const auto owner = runtime.lock()) owner->InterruptActions();
      });
  CommittedToolPayloadFollower payload_follower([&] {
    std::shared_lock lock(execution_composition.mutex);
    if (!execution_composition.current) return;
    const auto apply = [](const Composition &component) {
      if (component.direct_control_motion_binding &&
          component.ros2_control_runtime_handle && component.planner_scene_projection) {
        static_cast<void>(component.ros2_control_runtime_handle->ApplyLatestCommittedToolPayload());
      }
    };
    apply(*execution_composition.current);
    for (const auto &peer : execution_composition.current->participant_compositions)
      apply(*peer);
  });
  const auto wake_payload_follower = [&] { payload_follower.Notify(); };
  {
    std::lock_guard lock(projection_snapshot_wake.mutex);
    projection_snapshot_wake.notify = wake_payload_follower;
  }
  struct ClearProjectionSnapshotWake final {
    ProjectionSnapshotWake &wake;
    ~ClearProjectionSnapshotWake() {
      std::lock_guard lock(wake.mutex);
      wake.notify = {};
    }
  } clear_projection_snapshot_wake{projection_snapshot_wake};
  execution_composition.prepare_program_blackboard_context = [&](const auto &scope) {
    return EnsureAppliedProgramBlackboardContext(
        execution_composition, *stable_kernel_owner, config.manager_state_root,
        core::CellId(config.cell_id), scope);
  };
  bool current_composition_building = false;
  bool current_predecessor_composition_pending = false;

  // Program Reset of Mock arm positions reads the component runtime (R4).
  ConfigureProgramResetPositionAccess(execution_composition,
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch));

  ManagedExecutionSurfaceControl execution_surface_control(
      config.execution_run_root, &execution_composition,
      &manager_liveness_admission,
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch));
  std::jthread execution_surface([&](const std::stop_token stop_token) {
    RunManagedExecutionCommandSurface(execution_surface_control, stop_token);
  });
  {
    std::unique_lock lock(execution_surface_control.state_mutex);
    const auto signaled = execution_surface_control.state_changed.wait_for(
        lock, std::chrono::seconds(5), [&] {
          return execution_surface_control.ready ||
                 execution_surface_control.failure != nullptr;
        });
    Require(signaled, "managed execution command surface startup timed out");
    if (execution_surface_control.failure != nullptr) {
      std::rethrow_exception(execution_surface_control.failure);
    }
    Require(execution_surface_control.ready,
            "managed execution command surface did not become ready");
  }
  // CONTRACTS §15: the last Apply this host installed (the Manager then
  // publishes it); the liveness rebind to its successor checks it.
  std::optional<HostApplySelection> installed_apply;
  std::atomic<std::shared_ptr<const HostApplySelection>> installed_liveness_handoff;
  // The actual Manager terminal is durable before this existing release RPC.
  // Validate its original full parent before committing staged globals; failed
  // Apply keeps the ordinary release path and never activates the candidate.
  std::function<core::Result<void>()> restore_current_logical_product;
  const auto activate_globals_before_claim_release =
      [&](const core::CellAdmissionClaimReleaseDTO &release)
      -> core::Result<void> {
    using Reply = core::Result<void>;
    auto claim = stable_kernel_owner->ReadHeldCellAdmissionClaim(
        core::CellAdmissionClaimKindDTO::kManagerApply,
        release.owner_transaction_id);
    if (!claim)
      claim = stable_kernel_owner->ReadHeldCellAdmissionClaim(
          core::CellAdmissionClaimKindDTO::kAuthoringMutation,
          release.owner_transaction_id);
    if (!claim)
      return Reply::Ok();
    std::optional<core::NamedApplyPublicationTransactionDTO> origin;
    {
      std::lock_guard lock(managed_composition_mutex);
      std::shared_lock execution_lock(execution_composition.mutex);
      if (const auto *product = execution_composition.product())
        origin = product->origin_named_apply;
      else if (installed_apply)
        origin = core::NamedApplyPublicationTransactionDTO{
            installed_apply->apply_id, installed_apply->apply_generation};
    }
    if (!origin)
      return Reply::Ok();
    const auto database =
        config.manager_state_root /
        std::string(component_manager::kManagerApplyJournalFilename);
    const core::CellId cell(config.cell_id);
    const auto observed = component_manager::ReadManagerApplyStatusReadOnly(
        database, cell, *origin);
    if (!observed.ok())
      return Reply::Err(observed.error());
    const auto &stored = observed.value().intent.intent;
    const bool owns_parent =
        stored.parent_authoring_claim
            ? *stored.parent_authoring_claim == *claim
            : claim->intent.claim_kind ==
                      core::CellAdmissionClaimKindDTO::kManagerApply &&
                  claim->intent.owner_transaction_id ==
                      origin->apply_id.value();
    if (!owns_parent)
      return Reply::Ok();
    if (release.owner_role != claim->intent.owner_role ||
        release.owner_epoch != claim->intent.owner_epoch ||
        release.request_id != claim->intent.request_id ||
        release.request_payload_digest !=
            claim->intent.request_payload_digest ||
        release.claim_generation != claim->claim_generation ||
        release.acquired_admission_generation !=
            claim->acquired_admission_generation)
      return Reply::Err(
          {"invalid_request",
           "Global activation release differs from its actual parent",
           {{"reason", "io_global_rule_parent_mismatch"}}});
    if (!observed.value().terminal_outcome)
      return Reply::Err({"invalid_state_transition",
                         "Global activation has no actual terminal Apply",
                         {{"reason", "io_global_rule_terminal_unavailable"}}});
    if (!observed.value().terminal_outcome->terminal_outcome.succeeded())
      return Reply::Ok();
    if (release.settlement.evidence_digest !=
        observed.value().terminal_outcome->terminal_outcome_digest)
      return Reply::Err(
          {"invalid_request",
           "Global activation release has a foreign terminal digest",
           {{"reason", "io_global_rule_parent_mismatch"}}});
    const auto valid =
        component_manager::ReadManagerApplyConfigurationStatusReadOnly(
            database, cell, *origin, *claim);
    if (!valid.ok())
      return Reply::Err(valid.error());
    bool has_product;
    {
      std::shared_lock lock(execution_composition.mutex);
      has_product = execution_composition.product() != nullptr;
    }
    if (!has_product && restore_current_logical_product) {
      const auto restored = restore_current_logical_product();
      if (!restored.ok()) return restored;
    }
    return EnsureAppliedGlobalIoRuleCatalog(execution_composition,
                                            *stable_kernel_owner,
                                            config.manager_state_root, cell);
  };
  const auto confirm_installed_projection_set = [&](const auto &request) {
    auto bundle = active_managed_composition != nullptr
                      ? active_managed_composition->bundle
                      : std::shared_ptr<const deployment::RetainedBundle>{};
    if (bundle == nullptr ||
        bundle->retained_bundle_id() !=
            request.candidate_projection_revision.retained_bundle_id) {
      const auto selected = LoadManagedBundleAtPinnedRoot(
          config, request.candidate_projection_revision.retained_bundle_id);
      Require(selected.has_value(),
              "installed projection provider set has no verified bundle");
      bundle = selected->bundle;
    }
    Require(bundle->value().cell_graph.graph_revision ==
                request.candidate_projection_revision.graph_revision,
            "installed projection provider set has a different graph");
    projections.ConfirmInstalledProjectionSet(*bundle);
  };
  const auto publish_installed_liveness_handoff = [&](const auto &request) {
    installed_liveness_handoff.store(
        std::make_shared<const HostApplySelection>(request),
        std::memory_order_release);
  };
  std::exception_ptr managed_composition_failure;
  std::jthread current_composition_builder;
  bool current_composition_builder_started = false;

  const auto stable_addressed_kernel = [&]() -> core::ResourceAddressedExecutionKernel* {
    return stable_kernel_owner;
  };

  const auto await_manager_loss_containment = [&] {
    if (!manager_liveness_installed.load(std::memory_order_acquire)) {
      return;
    }
    while (g_stop_requested == 0 &&
           manager_liveness_receiver.get()->status().state !=
               core::ManagerLivenessStateDTO::kExpired) {
      int waited = -1;
      do {
        waited = ::poll(nullptr, 0U, 25);
      } while (waited < 0 && errno == EINTR && g_stop_requested == 0);
      Require(waited >= 0, "Manager-loss containment wait failed");
    }
  };

  const auto snapshot_registration_engines = [](const Composition &composition) {
    std::set<core::IResolvedParticipantExecutionEngine *> engines;
    // Use the same complete inventory installed in the stable Kernel. Its
    // borrowed subscriptions include subordinate I/O output engines too.
    for (const auto &registration : NativeCellComponentRegistrations(composition)) {
      Require(registration.engine != nullptr,
              "composition registration has no execution engine");
      engines.insert(registration.engine);
    }
    // A qualified input-only component legitimately has no engine.
    return engines;
  };

  const auto try_release_retired_composition = [&] {
    std::unique_lock lock(managed_composition_mutex);
    auto *const owner = stable_addressed_kernel();
    if (owner == nullptr) {
      return;
    }
    if (retired_managed_composition == nullptr) {
      Require(retired_registration_engines.empty(),
              "retired engine snapshot has no retired composition");
      projections.ReclaimRetiredProjections();
      return;
    }
    std::set<core::IResolvedParticipantExecutionEngine *> active_engines;
    if (active_managed_composition != nullptr) {
      active_engines = snapshot_registration_engines(*active_managed_composition);
    }
    for (auto *engine : retired_registration_engines) {
      if (active_engines.contains(engine)) {
        continue;
      }
      if (owner->participant_engine_in_use(engine)) {
        return;
      }
    }
    const auto *const retiring = retired_managed_composition.get();
    std::vector<std::shared_ptr<ControllerLaunchProcess>> removed_processes;
    std::set<const ControllerLaunchProcess *> active_processes;
    if (active_managed_composition) {
      for (const auto &[id, view] :
           IndexNativeComponentCompositions(*active_managed_composition)) {
        static_cast<void>(id);
        if (view->controller_process)
          active_processes.insert(view->controller_process.get());
      }
    }
    for (const auto &[id, view] :
         IndexNativeComponentCompositions(*retired_managed_composition)) {
      static_cast<void>(id);
      if (view->controller_process &&
          !active_processes.contains(view->controller_process.get()))
        removed_processes.push_back(view->controller_process);
    }
    // The same Manager channel owns composition replacement. Keep its
    // retired Composition, model and engine pins until exact group cleanup
    // completes; operational Stop does not wait on this mutex.
    lock.unlock();
    for (const auto &process : removed_processes)
      static_cast<void>(
          host_controller_launch_supervisor().retire_exact(process));
    lock.lock();
    // Only Manager dispatch mutates the retired owner/fence; the cell owner is stable.
    Require(retired_managed_composition.get() == retiring &&
                stable_addressed_kernel() == owner,
            "retired composition changed during exact process cleanup");
    for (auto engine = retired_registration_engines.begin();
         engine != retired_registration_engines.end();) {
      if (active_engines.contains(*engine)) {
        ++engine;
        continue;
      }
      if (!owner->release_retired_engine(*engine).ok()) {
        return;
      }
      engine = retired_registration_engines.erase(engine);
    }
    retired_registration_engines.clear();
    retired_managed_composition.reset();
    projections.ReclaimRetiredProjections();
  };

  // A Scenario composition refusal fails only its own Apply (through the
  // composition RPC); the main loop keeps the runtime running for it.
  const auto rethrow_composition_failure = [&](const bool keep_running_on_refusal = false) {
    std::exception_ptr failure;
    {
      std::lock_guard lock(managed_composition_mutex);
      failure = managed_composition_failure;
    }
    if (failure != nullptr &&
        !(keep_running_on_refusal && host_detail::IsScenarioCompositionRefusal(failure))) {
      std::rethrow_exception(failure);
    }
    {
      std::lock_guard lock(execution_surface_control.state_mutex);
      failure = execution_surface_control.failure;
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  };

  const auto try_start_current_composition = [&] {
    {
      std::lock_guard lock(managed_composition_mutex);
      if (active_managed_composition != nullptr ||
          current_composition_builder_started || installed_apply.has_value()) {
        return;
      }
    }
    bool predecessor_pending = false;
    auto input = TryBuildCurrentManagedCompositionInput(
        config, projections, predecessor_pending);
    {
      std::lock_guard lock(managed_composition_mutex);
      current_predecessor_composition_pending = predecessor_pending;
    }
    if (!input.has_value()) {
      return;
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      if (active_managed_composition != nullptr ||
          current_composition_builder_started || installed_apply.has_value()) {
        return;
      }
      input->shared_clock = detached_owner_clock;
      input->ros_context_provider = host_ros_context_provider;
      input->motion_trace = host_motion_trace;
      input->shared_scheduler = detached_owner_scheduler;
      input->observation_wake = stable_observation_wake;
      input->adapter_registry = &adapter_registry;
      input->adapter_node = adapter_transport->node();
      input->restart_driver = restart_component_driver;
      input->io_updates = component_io_updates;
      current_predecessor_composition_pending = false;
      current_composition_builder_started = true;
      Require(stable_kernel_owner->SetRouteRecoveryPending(true).ok(),
              "initial runtime composition could not close admission");
      current_composition_building = true;
    }
    current_composition_builder =
        std::jthread([&, input = std::move(*input)]() mutable {
          try {
            std::cerr << "native composition phase: materializing runtime\n";
            auto built = BuildProductComposition(std::move(input));
            std::cerr << "native composition phase: runtime materialized\n";
            InstallDirectSceneFrameResolver(projections, *built);
            Composition *installed = nullptr;
            // Removed components' action workers join after the locks are
            // released: their Enable/Reboot takes the composition lock.
            std::vector<std::jthread> retired_component_actions;
            {
              std::lock_guard lock(managed_composition_mutex);
              std::unique_lock execution_lock(execution_composition.mutex);
              Require(stable_kernel_owner != nullptr &&
                          stable_operation_journal != nullptr &&
                          detached_owner_clock != nullptr &&
                          detached_owner_scheduler != nullptr,
                      "current managed composition has no stable cell owner");
              Require(built->component_runtime_identity() &&
                          (!built->runtime || (built->runtime->clock == detached_owner_clock &&
                           built->runtime->scheduler == detached_owner_scheduler)),
                      "current managed composition does not share the stable cell time owners");
              PrepareCompositionIoResources(*built, *stable_cell_runtime_owner,
                                            generic_io_transport, host_ros_context_provider);
              auto current_registrations = NativeCellComponentRegistrations(*built);
              RetainedRuntimeParticipantResolverFactory staged_factory;
              auto current_resolver = NativeCellParticipantResolver(*built, current_registrations, &staged_factory);
              auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_product(
                  std::move(current_registrations), std::move(current_resolver));
              if (!reconfigured.ok()) Fail("stable Kernel rejected current runtime participants: " + reconfigured.error().message);
              AdoptNativeCellParticipantResolverFactory(*built, std::move(staged_factory));
              built->controller_launch_adopted = true;
              for (auto &peer : built->participant_compositions)
                peer->controller_launch_adopted = true;
              active_managed_composition = std::move(built);
              ApplyComponentStreamOverrides(active_managed_composition->bundle.get());
              if (active_managed_composition->runtime != nullptr)
                active_managed_composition->runtime->BindCellRuntimeOwner(
                    *stable_cell_runtime_owner, config.runtime_epoch, config.cell_id);
              bind_cell_runtime_to_composition(*active_managed_composition);
              BindHostIoRuleEvidenceProcessor(*active_managed_composition, execution_composition_owner);
              InstallCompositionIoSignals(*active_managed_composition, *stable_cell_runtime_owner);
              retired_component_actions = execution_composition.replace_current(
                  active_managed_composition.get());
              wake_payload_follower();
              execution_composition.addressed_kernel =
                  active_managed_composition->active_addressed_kernel();
              installed = active_managed_composition.get();
            }
            std::cerr << "native composition phase: stable Kernel bound\n";
            // CONTRACTS §15 (host restart): every component starts
            // not_prepared/offline; routes open at each component's Enable.
            // Ordinary initial Bootstrap is not successful NamedApply metadata.
            // Preserve its actual signal-service installation only when it has
            // no authored global rules; never fabricate verified_empty.
            const auto restored =
                !installed->origin_named_apply &&
                        !installed->compiled_waiting_global_io_rules &&
                        (!installed->compiled_global_io_rules ||
                         installed->compiled_global_io_rules->rules.empty())
                    ? [&] {
                        const auto signals = stable_kernel_owner->InstallProgramIoSignalService(
                            installed->io_signal_service,
                            installed->bundle->retained_bundle_id(),
                            installed->bundle->value().cell_graph.graph_revision);
                        if (signals.ok() && installed->io_signal_runtime)
                          installed->io_signal_runtime->RequestRuleEvidenceProcessing();
                        return signals;
                      }()
                    : RestoreAppliedGlobalIoRuleCatalog(
                          execution_composition, *stable_kernel_owner,
                          config.manager_state_root, core::CellId(config.cell_id));
            Require(restored.ok(), "initial runtime could not restore its global rules");
            PrepareInstalledMeasuredSourcesAfterContext(*installed);
            {
              std::lock_guard lock(managed_composition_mutex);
              Require(active_managed_composition.get() == installed,
                      "initial managed composition changed during start-up");
              Require(stable_kernel_owner->SetRouteRecoveryPending(false).ok(),
                      "initial runtime could not open admission");
              current_composition_building = false;
            }
          } catch (...) {
            std::lock_guard lock(managed_composition_mutex);
            managed_composition_failure = std::current_exception();
            current_composition_building = false;
          }
        });
  };

  // CONTRACTS §15: the one Apply call of the Manager, made while it holds
  // this Apply's Kernel configuration fence. A known refusal is a failed
  // result; anything else throws and the RPC reports it (effects unknown).
  RpcTiming composition_timing;
  std::chrono::steady_clock::time_point composition_request_received{};
  std::int64_t composition_request_received_unix_ms = 0;
  const auto apply_on_host = [&](const component_manager::HostApplyRequest &request)
      -> component_manager::HostApplyResult {
    using component_manager::HostApplyOutcome;
    const auto failed = [](core::ErrorDTO error) {
      return component_manager::HostApplyResult{HostApplyOutcome::kFailed, std::move(error)};
    };
    Require(request.valid(), "host Apply request is invalid");
    Require(manager_liveness_receiver_config.has_value(),
            "host Apply arrived before the Manager's liveness lease");
    const auto fence = fence_owner.HeldApplyFence(
        request.apply, manager_liveness_receiver_config->manager_runtime_epoch);
    Require(fence.has_value(), "host Apply has no held Apply fence");

    // The cell's start-up composition finishes before an Apply changes it.
    try_release_retired_composition();
    try_start_current_composition();
    if (current_composition_builder.joinable()) current_composition_builder.join();
    rethrow_composition_failure();

    // Refuse before any effect: an enabled component is never replaced or
    // removed (dead or faulted ones are accepted, and so is one whose Disable
    // failed: its process is stopped below like a dead one, CONTRACTS §11).
    {
      std::vector<std::string> changed;
      for (const auto &component : request.removed_components) changed.push_back(component.value());
      for (const auto &component : request.components)
        if (!std::binary_search(request.retained_components.begin(),
                                request.retained_components.end(), component))
          changed.push_back(component.value());
      const auto states = execution_composition.component_runtime->Read().states;
      for (const auto &component : changed) {
        const auto state = std::find_if(states.begin(), states.end(), [&](const auto &row) {
          return row.component_id == component;
        });
        if (state != states.end() && host_detail::LeavingComponentRefusesApply(*state))
          return failed({"apply_requires_inactive_component",
                         "Disable " + component + " before this Apply replaces or removes it.",
                         {{"component_id", component}}});
      }
    }

    const auto predecessor = read_current_publication();
    const auto predecessor_bundle = LoadManagedBundleAtPinnedRoot(
        config, predecessor.request.candidate_retained_bundle_id);
    const auto candidate_bundle =
        LoadManagedBundleAtPinnedRoot(config, request.candidate_retained_bundle_id);
    Require(predecessor_bundle && candidate_bundle &&
                candidate_bundle->bundle->value().cell_graph.graph_revision ==
                    request.candidate_graph_revision,
            "host Apply products are unavailable from the pinned roots");
    // The Scene revision here is the predecessor publication's; the Manager
    // publishes the one its Scene owner held when the Apply began, so the
    // publication is matched by projection identity, not Scene revision.
    const HostApplySelection selection{
        request.apply.apply_id,
        request.apply.apply_generation,
        predecessor.request.candidate_projection_revision,
        {request.candidate_retained_bundle_id, request.candidate_graph_revision,
         predecessor.request.candidate_projection_revision.scene_revision},
        predecessor.publication_generation,
        predecessor.durable_public_pointer_content_digest,
        fence->configuration_fence_id,
        fence->configuration_fence_generation,
        fence->expected_scope,
        fence->manager_runtime_epoch,
        deployment::derive_apply_component_delta(*predecessor_bundle->bundle,
                                                 *candidate_bundle->bundle, site_facts),
        request.removed_components,
        request.removed_domains};
    const auto fence_query = selection.fence_query();
    std::cerr << "native composition install: start apply=" << selection.apply_id.value()
              << " received_unix_ms=" << composition_request_received_unix_ms << '\n';
    const auto install_started = std::chrono::steady_clock::now();
    const auto install_ms = [&install_started] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - install_started)
          .count();
    };

    auto parent_claim = stable_kernel_owner->ReadHeldCellAdmissionClaim(
        core::CellAdmissionClaimKindDTO::kManagerApply, selection.apply_id.value());
    if (!parent_claim)
      parent_claim = stable_kernel_owner->ReadHeldCellAdmissionClaim(
          core::CellAdmissionClaimKindDTO::kAuthoringMutation);
    Require(parent_claim.has_value(), "host Apply has no held parent claim");

    // The installed owners this Apply keeps (same runtime identity, live
    // process) and the processes of removed and replaced components.
    std::map<core::ControllableComponentId, RetainedComponentSnapshot> retained_snapshots;
    std::vector<core::ControllableComponentId> actual_retained_components;
    std::vector<std::shared_ptr<ControllerLaunchProcess>> processes_to_retire;
    {
      std::lock_guard lock(managed_composition_mutex);
      std::map<core::ControllableComponentId, const Composition *> keepers;
      std::shared_lock execution_lock(execution_composition.mutex);
      const std::array<const Composition *, 3> roots{active_managed_composition.get(),
                                                     retired_managed_composition.get(),
                                                     failed_prepared_composition.get()};
      std::set<const Composition *> included_roots;
      std::set<const ControllerLaunchProcess *> included_processes;
      for (const auto *root : roots) {
        if (!root || !included_roots.insert(root).second) continue;
        for (const auto &[id, owner] : IndexNativeComponentCompositions(*root)) {
          const bool owns_io = owner->io_provider_drivers && !owner->io_provider_drivers->empty();
          const bool owns_physical = owner->runtime || owner->gripper_runtime;
          const auto wanted =
              deployment::derive_component_runtime_identity(*candidate_bundle->bundle, site_facts, id);
          const auto installed =
              deployment::derive_component_runtime_identity(*owner->bundle, site_facts, id);
          const bool owned_io_live = owns_io &&
              std::all_of(owner->io_provider_drivers->begin(), owner->io_provider_drivers->end(),
                          [](const auto &driver) { return driver && driver->resources_installed(); });
          // A dead simulated PLC (pure I/O owner) is not a keeper either:
          // its component is rebuilt and the PLC restarted by this Apply.
          const bool launch_live = owned_launch_live(owner->controller_process.get(), owns_physical,
                                                     IsLocalNoOutputGripperOwner(*owner));
          if (installed && wanted == installed && launch_live && (!owns_io || owned_io_live))
            keepers.emplace(id, owner);
        }
      }
      for (const auto &[id, owner] : keepers) {
        actual_retained_components.push_back(id);
        // An armed machine's owner rides on its kept arm member (CONTRACTS
        // §16): the arm's identity includes the owner's, so it is kept too.
        if (owner->owner_component && owner->owner_component->component_id != id.value() &&
            !keepers.contains(core::ControllableComponentId(owner->owner_component->component_id)))
          actual_retained_components.emplace_back(owner->owner_component->component_id);
        retained_snapshots.emplace(
            id, RetainedComponentSnapshot{
                    owner->runtime, owner->gripper_runtime, owner->stage,
                    owner->bundle->value().compiled_products.planning_collision_model,
                    owner->direct_control_joint_limits, owner->direct_control_motion_descriptor,
                    owner->direct_control_gripper_descriptor, owner->ros2_control_runtime_handle,
                    owner->controller_process, owner->io_provider_drivers,
                    owner->component_runtime_pins, owner->component_adapter,
                    owner->owner_component, owner->owner_handle, owner->owner_family,
                    owner->owner_id, owner->member_role});
      }
      std::sort(actual_retained_components.begin(), actual_retained_components.end());
      actual_retained_components.erase(
          std::unique(actual_retained_components.begin(), actual_retained_components.end()),
          actual_retained_components.end());
      for (const auto *root : included_roots)
        for (const auto &[id, owner] : IndexNativeComponentCompositions(*root)) {
          const auto kept = keepers.find(id);
          if (kept != keepers.end() &&
              ((owner->component_runtime_pins &&
                kept->second->component_runtime_pins == owner->component_runtime_pins) ||
               (owner->controller_process
                    ? kept->second->controller_process == owner->controller_process
                    : IsLocalNoOutputGripperOwner(*owner) &&
                          IsLocalNoOutputGripperOwner(*kept->second) &&
                          kept->second->gripper_runtime == owner->gripper_runtime)))
            continue;
          if (owner->controller_process &&
              included_processes.insert(owner->controller_process.get()).second &&
              !SimulatedPlcSurvivesRebuild(owner->controller_process,
                                           SpecIn(*candidate_bundle->bundle, site_facts, id)) &&
              !PoseSourceSurvivesRebuild(owner->controller_process,
                                         SpecIn(*candidate_bundle->bundle, site_facts, id),
                                         config.execution_run_root, config.manager_run_root))
            processes_to_retire.push_back(owner->controller_process);
        }
    }

    // Stop the processes of removed and replaced components (no clean
    // disable: the refusal above kept every enabled one whose Disable didn't
    // fail), before the candidate starts any: a machine's process moving
    // between its arm and its owner never runs twice on one controller
    // (CONTRACTS §11, §16).
    for (const auto &process : processes_to_retire) {
      if (process->cleanup_observation().state != ControllerLaunchCleanupState::kDrained)
        static_cast<void>(host_controller_launch_supervisor().retire_exact(process));
      const auto cleanup = process->cleanup_observation();
      Require(cleanup.state == ControllerLaunchCleanupState::kDrained && cleanup.receipt,
              "actual_installed_state_unknown: old component group has not drained; restart "
              "the workbench container");
    }

    // Build the candidate (no effect on the installed cell yet).
    auto input = BuildManagedCompositionInput(config, projections, selection,
                                              detached_owner_clock, detached_owner_scheduler);
    if (input) {
      input->ros_context_provider = host_ros_context_provider;
      input->motion_trace = host_motion_trace;
      input->retained_component_snapshots = std::move(retained_snapshots);
      input->observation_wake = stable_observation_wake;
      input->adapter_registry = &adapter_registry;
      input->adapter_node = adapter_transport->node();
      input->restart_driver = restart_component_driver;
      input->io_updates = component_io_updates;
    }
    const bool retain_view = input.has_value() && IsExactRetainedViewApply(selection) &&
                             actual_retained_components.size() ==
                                 input->bundle->value().cell_graph.controllable_components.size() &&
                             active_managed_composition != nullptr;
    const auto staged_tcp = input ? StageRetainedTcpRebinds(*input)
                                  : std::vector<StagedRetainedTcpRebind>{};
    std::shared_ptr<host_detail::ExecutionProduct> logical_product;
    try {
      if (input && !retain_view)
        prepared_managed_composition = BuildProductComposition(std::move(*input));
      if (!input)
        logical_product = BuildManagedLogicalExecutionProduct(
            config, candidate_bundle->bundle, candidate_bundle->package_root,
            selection.candidate_projection_revision,
            core::ActiveBundlePublicationGeneration(
                selection.expected_predecessor_publication_generation.value() + 1U),
            {selection.apply_id, selection.apply_generation}, detached_owner_clock,
            detached_owner_scheduler);
    } catch (const std::exception &error) {
      // A data/setup inconsistency found while composing refuses this
      // Apply only: typed, with its cause, logged once here.
      prepared_managed_composition.reset();
      std::cerr << "ERROR native composition refused: apply=" << selection.apply_id.value()
                << " cause=" << error.what() << '\n';
      return failed(host_detail::ScenarioCompositionRefusal(error).error());
    }

    // Removed components' action workers join after the locks are released:
    // their Enable/Reboot takes the composition lock.
    std::vector<std::jthread> retired_component_actions;
    // Set at the first change to the installed cell (a retired process
    // counts). A failure before it leaves the predecessor installed, and the
    // host says so (`effect: none`), so the Manager releases the Apply.
    bool installed_effect = !processes_to_retire.empty();
    try {
      fence_owner.RunWithExactHeldPredecessor(
          fence_query, selection.expected_predecessor_publication_generation,
          selection.expected_predecessor_public_pointer_digest, [&] {
        std::lock_guard lock(managed_composition_mutex);
        std::unique_lock execution_lock(execution_composition.mutex);
        Require(retired_managed_composition == nullptr && retired_registration_engines.empty(),
                "a predecessor runtime engine remains lifetime-pinned");
        const std::array<const Composition *, 3> io_predecessors{
            active_managed_composition.get(), retired_managed_composition.get(),
            failed_prepared_composition.get()};
        const auto io_resources = CaptureCompositionIoResourcePredecessors(io_predecessors);
        if (retain_view) {
          // A Program/Scene-only Apply keeps every component owner and
          // refreshes only its publication view.
          auto &candidate = *active_managed_composition;
          Require(RetainedViewFollowsPredecessor(
                      candidate.bundle->retained_bundle_id(), candidate.projection_revision,
                      input->bundle->retained_bundle_id(),
                      selection.predecessor_projection_revision),
                  "retained native view crossed its verified predecessor");
          installed_effect = true;
          RefreshRetainedPublicationView(candidate, input->component_planner_scene_projections,
                                         input->required_projection_pins,
                                         selection.candidate_projection_revision,
                                         input->publication_generation, config.manager_run_root);
          candidate.bundle = std::move(input->bundle);
          candidate.origin_named_apply = input->origin_named_apply;
          if (input->selected_program_products) {
            const auto &selected = *input->selected_program_products;
            candidate.program = selected.program;
            candidate.program_binding = selected.binding;
            candidate.selected_program_outcome = selected.outcome;
          }
          PrepareCompositionIoResources(candidate, *stable_cell_runtime_owner, generic_io_transport,
                                        host_ros_context_provider, io_resources);
          InstallDirectSceneFrameResolver(projections, candidate);
          std::vector<core::ControllableComponentId> retained_components;
          for (const auto &[component, view] : IndexNativeComponentCompositions(candidate)) {
            // An adapter-only component has no Kernel registration.
            if (!IsAdapterOnlyView(*view)) retained_components.push_back(component);
          }
          auto registrations = NativeCellComponentRegistrations(candidate);
          RetainedRuntimeParticipantResolverFactory staged_factory;
          auto resolver = NativeCellParticipantResolver(candidate, registrations, &staged_factory);
          const auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_component_set(
              std::move(registrations), std::move(resolver), candidate.bundle->retained_bundle_id(),
              candidate.bundle->value().cell_graph.graph_revision, selection.configuration_fence_id,
              selection.configuration_fence_scope, retained_components);
          if (!reconfigured.ok())
            Fail("stable Kernel rejected retained product transition: " + reconfigured.error().message);
          AdoptNativeCellParticipantResolverFactory(candidate, std::move(staged_factory));
        } else if (logical_product) {
          installed_effect = true;
          auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_component_set(
              {}, {}, selection.candidate_projection_revision.retained_bundle_id,
              selection.candidate_projection_revision.graph_revision,
              selection.configuration_fence_id, selection.configuration_fence_scope, {});
          if (!reconfigured.ok())
            Fail("stable Kernel rejected the empty selected runtime: " + reconfigured.error().message);
          for (auto &worker : execution_composition.replace_logical_product(logical_product))
            retired_component_actions.push_back(std::move(worker));
          BindHostIoRuleEvidenceProcessor(*logical_product, execution_composition_owner);
          InstallCompositionIoSignals(*logical_product, *stable_cell_runtime_owner, &fence_query,
                                      &*parent_claim);
          if (active_managed_composition) {
            retired_registration_engines = snapshot_registration_engines(*active_managed_composition);
            retired_managed_composition = std::move(active_managed_composition);
          }
        } else {
          auto &prepared = prepared_managed_composition;
          Require(prepared && (!prepared->runtime ||
                               (prepared->runtime->clock == detached_owner_clock &&
                                prepared->runtime->scheduler == detached_owner_scheduler)),
                  "prepared runtime crossed the stable Kernel time capabilities");
          std::optional<std::set<core::IResolvedParticipantExecutionEngine *>> predecessor_engines;
          if (active_managed_composition)
            predecessor_engines = snapshot_registration_engines(*active_managed_composition);
          const auto retained_component_ids = RetainedComponentIds(selection, *prepared);
          if (!retained_component_ids.empty()) {
            Require(active_managed_composition != nullptr &&
                        (prepared->runtime == nullptr ||
                         prepared->candidate_participant_resolver_factory),
                    "retained host Apply has no predecessor or candidate participant resolver");
            RequireRetainedCandidateOwners(*prepared, *active_managed_composition, selection);
            installed_effect = true;
            ResealRetainedCandidateOwners(*prepared, *active_managed_composition, selection,
                                          admission_gate);
            RefreshRetainedCandidateCollision(*prepared, selection, admission_gate);
            RefreshRetainedCandidateModels(*prepared, selection, admission_gate,
                                           prepared->runtime != nullptr
                                               ? *prepared->runtime->clock
                                               : *stable_cell_runtime_owner->clock());
            if (prepared->runtime != nullptr) {
              std::vector<std::shared_ptr<RetainedExecutionHost>> peers;
              for (const auto &component : prepared->participant_compositions)
                if (component->runtime != nullptr) peers.push_back(component->runtime);
              const auto rejoined = rejoin_retained_execution_host_participants(
                  prepared->runtime, std::move(peers),
                  prepared->candidate_participant_resolver_factory, admission_gate,
                  selection.configuration_fence_id, selection.configuration_fence_scope);
              if (!rejoined.ok())
                Fail("retained native cell participant rejoin failed: " + rejoined.error().message);
            }
            std::map<core::ControllableComponentId, std::shared_ptr<RetainedPlannerSceneProjection>>
                selected_projections;
            const auto include_projection = [&](const Composition &view) {
              if (view.planner_scene_projection != nullptr)
                selected_projections.emplace(view.controllable_component_id,
                                             view.planner_scene_projection);
            };
            include_projection(*prepared);
            for (const auto &component : prepared->participant_compositions)
              include_projection(*component);
            RefreshRetainedPublicationView(*prepared, selected_projections,
                                           prepared->required_projection_pins,
                                           selection.candidate_projection_revision,
                                           prepared->publication_generation,
                                           config.manager_run_root);
          }
          installed_effect = true;
          PrepareCompositionIoResources(*prepared, *stable_cell_runtime_owner, generic_io_transport,
                                        host_ros_context_provider, io_resources);
          auto registrations = NativeCellComponentRegistrations(*prepared);
          RetainedRuntimeParticipantResolverFactory staged_factory;
          auto resolver = NativeCellParticipantResolver(*prepared, registrations, &staged_factory);
          const auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_component_set(
              std::move(registrations), std::move(resolver),
              selection.candidate_projection_revision.retained_bundle_id,
              selection.candidate_projection_revision.graph_revision,
              selection.configuration_fence_id, selection.configuration_fence_scope,
              retained_component_ids);
          if (!reconfigured.ok())
            Fail("stable Kernel rejected the selected runtime participants: " +
                 reconfigured.error().message);
          std::cerr << "native composition install: Kernel reconfigured elapsed_ms="
                    << install_ms() << '\n';
          AdoptNativeCellParticipantResolverFactory(*prepared, std::move(staged_factory));
          prepared->controller_launch_adopted = true;
          for (auto &peer : prepared->participant_compositions)
            peer->controller_launch_adopted = true;
          if (active_managed_composition) {
            retired_registration_engines = std::move(*predecessor_engines);
            retired_managed_composition = std::move(active_managed_composition);
          }
          // The stable Kernel and its registered engines outlive every
          // fallible step below; the candidate is retained under the fence.
          active_managed_composition = std::move(prepared);
          auto &candidate = *active_managed_composition;
          ApplyComponentStreamOverrides(candidate.bundle.get());
          if (candidate.runtime != nullptr)
            candidate.runtime->BindCellRuntimeOwner(*stable_cell_runtime_owner,
                                                    config.runtime_epoch, config.cell_id);
          bind_cell_runtime_to_composition(candidate);
          if (!selection.configuration_fence_scope.resources.empty()) {
            const auto staged =
                candidate.active_addressed_kernel()
                    ->stage_current_runtime_configuration_facts_under_apply_fence(
                        selection.configuration_fence_scope);
            if (!staged.ok())
              Fail("stable Kernel rejected candidate configuration staging: " +
                   staged.error().message);
          }
          // Executor start does not wait for a controller or issue a home goal.
          for (const auto &[component_id, view] : IndexNativeComponentCompositions(candidate)) {
            if (view->ros2_control_runtime_handle)
              view->ros2_control_runtime_handle->StartBackend();
            else if (!IsAdapterOnlyView(*view))
              Require(view->io_provider_drivers && !view->io_provider_drivers->empty() &&
                          std::all_of(view->io_provider_drivers->begin(),
                                      view->io_provider_drivers->end(), [](const auto &record) {
                                        return record && record->resources_installed();
                                      }),
                      "installed component " + component_id.value() +
                          " has no actual transport resources");
          }
          InstallDirectSceneFrameResolver(projections, candidate);
#if !defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
          if (candidate.runtime != nullptr) AttachProductPickPlacePlanningOwner(candidate);
#endif
        }
        if (active_managed_composition) {
          BindHostIoRuleEvidenceProcessor(*active_managed_composition, execution_composition_owner);
          InstallCompositionIoSignals(*active_managed_composition, *stable_cell_runtime_owner,
                                      &fence_query, &*parent_claim);
          // The selected cell's components are the coordinated ones
          // (SetComponents): retained ones keep their adapter, state and
          // `last_action`; added and replaced ones start not_prepared/offline.
          for (auto &worker : execution_composition.replace_current(active_managed_composition.get()))
            retired_component_actions.push_back(std::move(worker));
          wake_payload_follower();
          const auto context = "Apply=" + selection.apply_id.value() +
                               " generation=" + std::to_string(selection.apply_generation.value());
          const auto update_context = [&](Composition &component) {
            component.controller_request_context = context;
            if (component.controller_process)
              component.controller_process->update_request_context(context);
            // Every Apply starts without stored trajectories.
            if (component.runtime) component.runtime->cache->invalidate_all();
          };
          update_context(*active_managed_composition);
          for (auto &peer : active_managed_composition->participant_compositions)
            update_context(*peer);
        }
        execution_composition.addressed_kernel = stable_addressed_kernel();
        // Retained robots take this Apply's TCP rows and tool context now,
        // while the parent fence still blocks their motion.
        for (const auto &item : staged_tcp) {
          RetainedToolContextSourceFn source;
          CommittedToolContextSourceFn committed;
          CommittedToolContextSourceFn enable_source;
          std::function<std::optional<core::Pose3DTO>(const std::string &)> pose;
          if (item.source) {
            source = [value = item.source](const auto &frame) { return value->Compose(frame); };
            committed = [value = item.source](const auto &, const auto &scene) {
              return value->ComposePayload(scene);
            };
            enable_source = [value = item.source](const auto &frame, const auto &scene) {
              return value->Compose(frame, scene);
            };
            pose = [value = item.source](const auto &frame) { return value->PoseInFlange(frame); };
          }
          const std::weak_ptr scene_projection{item.scene_projection};
          CommittedSceneSourceFn scene_source;
          if (item.scene_projection)
            scene_source = [scene_projection] {
              const auto projection = scene_projection.lock();
              return projection ? projection->logical_snapshot() : nullptr;
            };
          const auto rebound = item.handle->RebindRetainedToolContext(
              item.rows, item.direct_binding, std::move(source), std::move(committed),
              std::move(pose), std::move(scene_source),
              std::move(enable_source));
          if (!rebound.ok()) Fail("retained TCP rebind failed: " + rebound.error().message);
        }
        std::cerr << "native composition install: installed elapsed_ms=" << install_ms() << '\n';
      });
    } catch (...) {
      if (prepared_managed_composition) {
        prepared_managed_composition->discard_unadopted_controller_launches();
        failed_prepared_composition = std::move(prepared_managed_composition);
      }
      if (installed_effect) throw;
      auto error = host_detail::HostApplyFailureBeforeEffect(std::current_exception());
      std::cerr << "native selected composition failed before effect: " << error.message << '\n';
      return failed(std::move(error));
    }
    if (active_managed_composition) PrepareInstalledMeasuredSourcesAfterContext(*active_managed_composition);
    // CONTRACTS §8: every rule returns to `active_at_start` at Load.
    const auto reset = stable_kernel_owner->reset_rule_activation();
    Require(reset.ok(), "host Apply could not reset rule activation");
    {
      std::lock_guard lock(managed_composition_mutex);
      installed_apply = selection;
    }
    confirm_installed_projection_set(selection);
    publish_installed_liveness_handoff(selection);
    try_release_retired_composition();
    return {HostApplyOutcome::kSucceeded, std::nullopt};
  };

  const auto handle_manager_liveness = [&](const std::string_view packet) {
    const auto decoded =
        component_manager::detail::DecodeManagerLivenessRpcRequest(packet);
    const auto required = std::find_if(
        decoded.required_set.required_receivers.begin(),
        decoded.required_set.required_receivers.end(), [&](const auto &row) {
          return row.runtime_unit_id.value() == config.runtime_unit_id;
        });
    Require(required != decoded.required_set.required_receivers.end() &&
                decoded.required_set.cell_id.value() == config.cell_id &&
                decoded.receiver_runtime_unit_id == required->runtime_unit_id &&
                required->runtime_epoch.value() == config.runtime_epoch,
            "Manager liveness request names a foreign receiver");
    Require(decoded.policy == deployment::execution_runtime_contract::
                                  manager_liveness_policy() &&
                required->runtime_policy_id ==
                    decoded.policy.runtime_policy_id &&
                required->runtime_policy_revision ==
                    decoded.policy.runtime_policy_revision,
            "Manager liveness request differs from retained policy");

    const auto request_matches_publication = [&](const auto &publication) {
      return publication.publication_generation ==
                 decoded.active_publication_generation &&
             publication.request.candidate_retained_bundle_id ==
                 decoded.required_set.active_bundle_id &&
             publication.request.candidate_graph_revision ==
                 decoded.active_graph_revision &&
             publication.durable_public_pointer_content_digest ==
                 decoded.active_public_pointer_digest;
    };
    const auto require_durable_current_publication = [&]() {
      auto publication = read_current_publication();
      if (!request_matches_publication(publication) &&
          decoded.active_publication_generation.value() <
              publication.publication_generation.value()) {
        return std::optional<core::ActiveBundlePublicationReceiptDTO>{};
      }
      Require(request_matches_publication(publication),
              "Manager liveness request has foreign active-publication "
              "identity");
      return std::optional<core::ActiveBundlePublicationReceiptDTO>(
          std::move(publication));
    };

    const core::ManagerLivenessReceiverConfig receiver_config{
        decoded.required_set.cell_id,
        required->runtime_unit_id,
        required->runtime_epoch,
        decoded.required_set.manager_runtime_unit_id,
        decoded.required_set.manager_runtime_epoch,
        decoded.required_set.manager_owner_epoch,
        decoded.policy,
        decoded.required_set.active_bundle_id,
        decoded.required_set.required_runtime_set_digest};
    if (manager_liveness_receiver.get() == nullptr) {
      if (!require_durable_current_publication().has_value()) {
        return component_manager::detail::EncodeManagerLivenessRpcRejection(
            "superseded_active_publication");
      }
      // Initial construction and exact claim settlement own attachment.
      // Their Kernel barrier remains closed through controller readback;
      // this independent lease handler never performs controller I/O.
      const auto installed =
          stable_kernel_owner->ReplaceManagerLivenessRequiredSet(
              decoded.required_set);
      Require(installed.ok(),
              "Kernel rejected the exact Manager-liveness required set");
      manager_liveness_receiver.Install(
          receiver_config,
          [&manager_liveness_admission](
              const core::ManagerLivenessLossIntentDTO &intent) {
            return manager_liveness_admission.CloseAndAttest(intent);
          },
          *stable_kernel_owner, *stable_kernel_owner);
      manager_liveness_installed.store(true, std::memory_order_release);
      manager_liveness_receiver_config = receiver_config;
      manager_liveness_publication_generation =
          decoded.active_publication_generation;
      manager_liveness_public_pointer_digest =
          decoded.active_public_pointer_digest;
      manager_liveness_required_set = decoded.required_set;
    } else {
      Require(manager_liveness_receiver_config.has_value() &&
                  manager_liveness_receiver_config->cell_id ==
                      receiver_config.cell_id &&
                  manager_liveness_receiver_config->receiver_runtime_unit_id ==
                      receiver_config.receiver_runtime_unit_id &&
                  manager_liveness_receiver_config->receiver_runtime_epoch ==
                      receiver_config.receiver_runtime_epoch &&
                  manager_liveness_receiver_config->policy ==
                      receiver_config.policy &&
                  manager_liveness_receiver_config->manager_runtime_unit_id ==
                      receiver_config.manager_runtime_unit_id &&
                  manager_liveness_publication_generation.has_value() &&
                  manager_liveness_public_pointer_digest.has_value(),
              "Manager liveness request changed immutable receiver evidence");
      const bool publication_changed =
          *manager_liveness_publication_generation !=
              decoded.active_publication_generation ||
          *manager_liveness_public_pointer_digest !=
              decoded.active_public_pointer_digest;
      Require(!publication_changed ||
                  (manager_liveness_receiver_config->manager_runtime_epoch ==
                       receiver_config.manager_runtime_epoch &&
                   manager_liveness_receiver_config->manager_owner_epoch ==
                       receiver_config.manager_owner_epoch),
              "Manager liveness publication rebind changed Manager identity");
      if (manager_liveness_receiver_config->manager_runtime_epoch !=
              receiver_config.manager_runtime_epoch ||
          manager_liveness_receiver_config->manager_owner_epoch !=
              receiver_config.manager_owner_epoch) {
        const auto candidate_epoch =
            std::pair{receiver_config.manager_runtime_epoch.value(),
                      receiver_config.manager_owner_epoch.value()};
        Require(!retired_manager_epochs.contains(candidate_epoch),
                "stale Manager epoch cannot replace the current Manager");
        Require(retired_manager_epochs.size() < 64U,
                "Manager epoch replacement history is exhausted");
        retired_manager_epochs.emplace(
            manager_liveness_receiver_config->manager_runtime_epoch.value(),
            manager_liveness_receiver_config->manager_owner_epoch.value());
        const auto replaced =
            manager_liveness_receiver.get()->BeginManagerEpochReplacement(
                receiver_config.manager_runtime_unit_id,
                receiver_config.manager_runtime_epoch,
                receiver_config.manager_owner_epoch);
        Require(replaced.ok(),
                "Manager liveness epoch replacement was rejected");
        manager_liveness_receiver_config->manager_runtime_unit_id =
            receiver_config.manager_runtime_unit_id;
        manager_liveness_receiver_config->manager_runtime_epoch =
            receiver_config.manager_runtime_epoch;
        manager_liveness_receiver_config->manager_owner_epoch =
            receiver_config.manager_owner_epoch;
      }
      if (publication_changed) {
        const auto current_publication = require_durable_current_publication();
        if (!current_publication.has_value()) {
          return component_manager::detail::EncodeManagerLivenessRpcRejection(
              "superseded_active_publication");
        }
        const auto installed_request =
            installed_liveness_handoff.load(std::memory_order_acquire);
        const auto *const named =
            std::get_if<core::NamedApplyPublicationTransactionDTO>(
                &current_publication->request.transaction_ref);
        Require(
            named != nullptr && installed_request != nullptr &&
                installed_request->apply_id == named->apply_id &&
                installed_request->apply_generation ==
                    named->apply_generation &&
                // The Scene revision is Apply-time evidence (the Manager
                // records its Scene owner's), never a binding.
                core::SameProjectionIdentity(
                    installed_request->candidate_projection_revision,
                    current_publication->request
                        .candidate_projection_revision) &&
                installed_request->manager_runtime_epoch ==
                    receiver_config.manager_runtime_epoch &&
                installed_request->expected_predecessor_publication_generation
                            .value() +
                        1U ==
                    current_publication->publication_generation.value(),
            "Manager liveness successor lacks exact installed composition "
            "evidence");
        // The exact installed handoff authorizes this lease rebind. The
        // subsequent Apply/authoring claim settlement owns attachment, with
        // ordinary execution closed by the Kernel's route-recovery barrier.
        manager_liveness_admission.CloseForRequiredSetRebind();
        const auto installed =
            stable_kernel_owner->ReplaceManagerLivenessRequiredSet(
                decoded.required_set);
        Require(installed.ok(),
                "Kernel rejected the successor Manager-liveness required set");
        const auto rebound = manager_liveness_receiver.get()->RebindRequiredSet(
            receiver_config.active_bundle_id,
            receiver_config.required_runtime_set_digest);
        Require(rebound.ok(),
                "Manager-liveness receiver rejected the successor required-set "
                "rebind");
        manager_liveness_receiver_config->active_bundle_id =
            receiver_config.active_bundle_id;
        manager_liveness_receiver_config->required_runtime_set_digest =
            receiver_config.required_runtime_set_digest;
        manager_liveness_required_set = decoded.required_set;
        manager_liveness_publication_generation =
            decoded.active_publication_generation;
        manager_liveness_public_pointer_digest =
            decoded.active_public_pointer_digest;
      } else {
        Require(
            manager_liveness_required_set.has_value() &&
                *manager_liveness_required_set == decoded.required_set &&
                manager_liveness_receiver_config->active_bundle_id ==
                    receiver_config.active_bundle_id &&
                manager_liveness_receiver_config->required_runtime_set_digest ==
                    receiver_config.required_runtime_set_digest,
            "Manager liveness request changed required-set identity "
            "without a publication transition");
      }
    }
    const auto admission_generation = manager_liveness_admission.generation();
    const auto renewed =
        manager_liveness_receiver.get()->Renew(decoded.heartbeat);
    Require(renewed.ok(), "Manager heartbeat renewal was rejected");
    if (decoded.reconciliation.has_value() &&
        manager_liveness_receiver.get()->status().command_fenced) {
      const auto reconciled =
          manager_liveness_receiver.get()->CompleteReconciliation(
              *decoded.reconciliation);
      Require(reconciled.ok(), "Manager liveness reconciliation was rejected");
    }
    const auto status = manager_liveness_receiver.get()->status();
    if (status.state == core::ManagerLivenessStateDTO::kCurrent &&
        !status.command_fenced) {
      manager_liveness_admission.OpenIfUnchanged(admission_generation);
    }
    return component_manager::detail::EncodeManagerLivenessRpcResponse(
        core::ManagerLivenessObservationDTO{
            status, decoded.required_set.active_bundle_id,
            decoded.required_set.required_runtime_set_digest,
            core::ComputeManagerLivenessStatusDigest(status)});
  };

  const auto rethrow_manager_liveness_failure = [&] {
    std::lock_guard lock(manager_liveness_failure_mutex);
    if (manager_liveness_failure != nullptr) {
      std::rethrow_exception(manager_liveness_failure);
    }
  };
  manager_liveness_lane = std::jthread([&](const std::stop_token stop_token) {
    try {
      bool ordinary_status_available = true;
      const char *operational_stuck_drain = nullptr;
      while (!stop_token.stop_requested()) {
        std::array<pollfd, 2> descriptors{
            {{config.manager_liveness_channel_fd, POLLIN | POLLHUP, 0},
             {shutdown_status_servicing.load(std::memory_order_acquire) &&
                      ordinary_status_available
                  ? config.manager_channel_fd
                  : -1,
              POLLIN | POLLHUP, 0}}};
        int ready = -1;
        do {
          ready = ::poll(descriptors.data(), descriptors.size(), 100);
        } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
        Require(ready >= 0, "poll managed runtime liveness channel failed");
        if (ready == 0) {
          continue;
        }
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
          const auto &descriptor = descriptors[index];
          if (descriptor.revents == 0)
            continue;
          if ((descriptor.revents & POLLHUP) != 0 &&
              (descriptor.revents & POLLIN) == 0) {
            if (index == 0U)
              return;
            ordinary_status_available = false;
            continue;
          }
          if (index == 1U) {
            // The sole ordinary reader has exited before this fd is enabled.
            // Reject all effects here. Diagnostic failure disables only status;
            // it cannot stop or block the independent heartbeat servicing path.
            try {
              Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
                      "shutdown progress channel became invalid");
              const auto packet = ReceiveManagedRuntimePacket(descriptor.fd);
              if (!packet) {
                ordinary_status_available = false;
                continue;
              }
              Require(
                  component_manager::detail::IsNativeShutdownRpcPacket(
                      *packet) &&
                      component_manager::detail::DecodeNativeShutdownRpcRequest(
                          *packet) == config.runtime_epoch,
                  "shutdown progress request names a foreign protocol or "
                  "epoch");
              const auto response =
                  component_manager::detail::EncodeNativeShutdownRpcResponse(
                      shutdown_projection.Read());
              // Status is optional observation. Never spend the ordinary 750ms
              // send wait on this same lane while heartbeat renewal is due.
              const auto sent =
                  ::send(descriptor.fd, response.data(), response.size(),
                         MSG_NOSIGNAL | MSG_DONTWAIT);
              if (sent != static_cast<ssize_t>(response.size()))
                ordinary_status_available = false;
            } catch (...) {
              ordinary_status_available = false;
            }
            continue;
          }
          Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
                  "managed runtime liveness channel became invalid");
          const auto packet = ReceiveManagedRuntimePacket(descriptor.fd);
          const auto liveness_received = DiagnosticClock::now();
          if (!packet)
            return;
          RpcTiming timing{.received = liveness_received};
          DiagnosticTime reply_sent{};
          std::string_view timing_kind;
          {
            const auto liveness_lock_start = DiagnosticClock::now();
            std::lock_guard servicing_lock(manager_liveness_servicing_mutex);
            timing.servicing_wait = DiagnosticClock::now() - liveness_lock_start;
            if (teardown_only) {
              // No borrowed Kernel/runtime access after the exclusion transition.
              SendManagedRuntimePacket(
                  config.manager_liveness_channel_fd,
                  component_manager::detail::EncodeManagerLivenessRpcRejection(
                      "runtime_shutting_down"));
              continue;
            }
            if (component_manager::detail::IsManagerLivenessRpcPacket(*packet)) {
              SendManagedRuntimePacket(
                  config.manager_liveness_channel_fd,
                  host_detail::DispatchHeartbeatForDrain(
                      core::FirstStuckDrainName(),
                      shutdown_status_servicing.load(std::memory_order_acquire),
                      operational_stuck_drain,
                      [&] { return handle_manager_liveness(*packet); },
                      [](const char *drain) {
                        return component_manager::detail::
                            EncodeManagerLivenessRpcRejection(
                                std::string("drain_stuck.") + drain);
                      }));
              continue;
            }
            Require(
                component_manager::detail::IsKernelCellAdmissionRpcPacket(
                    *packet),
                "dedicated Manager-liveness channel received another protocol");
            const auto decoded =
                component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                    *packet);
            const bool publish_liveness =
                decoded.command == "cell_admission_publish_liveness" &&
                std::holds_alternative<core::ManagerLivenessObservationDTO>(
                    decoded.request);
            const bool update_readiness =
                decoded.command == "cell_admission_update_readiness" &&
                std::holds_alternative<core::CellAdmissionReadinessSnapshotDTO>(
                    decoded.request);
            // Only the ordered fail-closed stream belongs on this channel.  All
            // user/authoring admission commands remain on the ordinary channel.
            Require(publish_liveness || update_readiness,
                    "dedicated Manager-liveness channel received an ordinary "
                    "cell-admission command");
            timing.dispatch = DiagnosticClock::now();
            timing_kind = publish_liveness ? "cell_admission_publish_liveness"
                                           : "cell_admission_update_readiness";
            component_manager::detail::KernelCellAdmissionRpcResponse response;
            std::shared_ptr<core::IIoSignalService> actual_retired_service;
            try {
              response =
                  component_manager::detail::DispatchKernelCellAdmissionRpc(
                      *stable_kernel_owner, decoded, &actual_retired_service);
            } catch (const std::exception &error) {
              response = core::ErrorDTO{
                  "cell_admission_request_failed", error.what(), {}};
            } catch (...) {
              response = core::ErrorDTO{"cell_admission_request_failed",
                                        "cell-admission request failed",
                                        {}};
            }
            // No Access lookup on this independent liveness lane. The actual
            // affected runtime only queues its existing post-lock processor.
            if (auto runtime = std::dynamic_pointer_cast<HostIoSignalRuntime>(actual_retired_service))
              runtime->RequestRuleEvidenceProcessing();
            timing.handler_end = DiagnosticClock::now();
            SendManagedRuntimePacket(
                config.manager_liveness_channel_fd,
                component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                    decoded.command, response));
            reply_sent = DiagnosticClock::now();
          }
          LogSlowRpc(timing_kind, timing, reply_sent);
        }
      }
    } catch (...) {
      {
        std::lock_guard lock(manager_liveness_failure_mutex);
        manager_liveness_failure = std::current_exception();
      }
      static_cast<void>(::shutdown(config.manager_channel_fd, SHUT_RDWR));
    }
  });

  // Keep renewal servicing and captured state alive until every composition
  // builder and execution lane has drained, including exception unwinding.
  // Otherwise reverse declaration order stops renewal before that drain while
  // the receiver's independent watchdog is still armed.
  struct DrainBeforeLivenessShutdown {
    std::array<std::jthread *, 2> workers;
    ManagedProjectionRegistry *projections;
    component_manager::detail::NativeShutdownProjection &progress;
    std::mutex &servicing_mutex;
    bool &teardown_only;
    std::atomic<bool> &status_servicing;
    ExecutionCompositionAccess *execution_access;
    ~DrainBeforeLivenessShutdown() {
      // Keep existing renewals through group/worker/projection drain, as
      // before. Their original receiver can still be armed during those drains.
      progress.Enter(ShutdownPhase::kGroupRetirement);
      // This destructor runs on the sole ordinary reader after that reader's
      // loop/receive has exited, including unwinding. Publish the handoff once.
      status_servicing.store(true, std::memory_order_release);
      HandleStopSignal(0); // same existing stop/wake fact, including barriers
      for (auto *worker : workers)
        worker->request_stop();
      auto snapshot = host_controller_launch_supervisor().BeginHostShutdown();
      auto &supervisor = host_controller_launch_supervisor();
      while (true) {
        const auto actual = supervisor.PollHostShutdown(snapshot);
        progress.Groups(actual.total, actual.pending, actual.signalled,
                        actual.escalated, actual.reaped, actual.drained,
                        actual.unknown, actual.pending_pid);
        if (actual.pending == 0U) {
          supervisor.enqueue_host_log(
              "native_shutdown_group_retirement completed total=" +
              std::to_string(actual.total) +
              " signalled=" + std::to_string(actual.signalled) +
              " escalated=" + std::to_string(actual.escalated) +
              " reaped=" + std::to_string(actual.reaped) +
              " drained=" + std::to_string(actual.drained) +
              " unknown=" + std::to_string(actual.unknown));
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      const std::array phases{ShutdownPhase::kExecutionDrain,
                              ShutdownPhase::kCurrentBuilderDrain};
      for (std::size_t i = 0; i < workers.size(); ++i) {
        progress.Enter(phases[i]);
        if (workers[i]->joinable()) {
          workers[i]->join();
          progress.WorkerJoined(); // only an actual completed join
        }
      }
      progress.Enter(ShutdownPhase::kProjectionDrain);
      projections->Drain();
      {
        // Finish the last ordinary private handler before any borrowed owner
        // destructor; all subsequent status uses only the outer projection.
        std::lock_guard lock(servicing_mutex);
        teardown_only = true;
      }
      // Wait for scheduled evidence calls holding the same shared lease, then
      // make queued callbacks inert before any borrowed Kernel is destroyed.
      execution_access->detach_addressed_owner();
    }
  } drain_before_liveness_shutdown{
      {&execution_surface, &current_composition_builder},
      &projections,
      shutdown_projection,
      manager_liveness_servicing_mutex,
      teardown_only,
      shutdown_status_servicing,
      &execution_composition
  };

  // Empty published startup has no physical builder/before-Start callback.
  // Reuse this existing owner cadence after real Manager health arrives, with
  // the same facade/publication and sealed bundle; no extra polling lane.
  const auto restore_empty_startup_globals_impl = [&]() -> core::Result<void> {
    using Reply = core::Result<void>;
    if (!manager_liveness_installed.load(std::memory_order_acquire))
      return Reply::Ok();
    const auto manager = manager_liveness_receiver.get()->status();
    if (manager.state != core::ManagerLivenessStateDTO::kCurrent ||
        manager.command_fenced)
      return Reply::Ok();
    // Private Apply installation can precede its publication. The
    // same exclusion is independently enforced by Core activation/restore.
    if (admission_gate.has_held_configuration_fences())
      return Reply::Ok();
    std::optional<core::ActiveBundlePublicationReceiptDTO> selected;
    std::optional<HostApplySelection> installed_request;
    {
      std::lock_guard lock(managed_composition_mutex);
      std::shared_lock execution_lock(execution_composition.mutex);
      if (active_managed_composition || execution_composition.current ||
          current_composition_building)
        return Reply::Ok();
      installed_request = installed_apply;
    }
    selected = read_current_publication();
    if (!LoadManagedBundleAtPinnedRoot(config, selected->request.candidate_retained_bundle_id)
             ->bundle->value().cell_graph.controllable_components.empty())
      return Reply::Ok();
    const auto *origin = std::get_if<core::NamedApplyPublicationTransactionDTO>(
        &selected->request.transaction_ref);
    if (!origin)
      return Reply::Ok(); // Ordinary Bootstrap remains unknown.
    const auto pointer = component_manager::ReadActiveBundlePublicationReadOnly(
        config.manager_state_root /
            std::string(component_manager::kActiveBundlePublicationStoreFilename),
        core::CellId(config.cell_id));
    if (!pointer.ok())
      return Reply::Err(pointer.error());
    if (pointer.value() != *selected)
      return Reply::Ok(); // Proved superseded observation, no installation effect.
    {
      // Terminal publication can precede the existing successor heartbeat.
      // Join that actual lease handoff before activating its logical product.
      std::lock_guard liveness_lock(manager_liveness_servicing_mutex);
      if (!manager_liveness_required_set ||
          manager_liveness_required_set->active_bundle_id !=
              selected->request.candidate_retained_bundle_id ||
          manager_liveness_publication_generation !=
              selected->publication_generation ||
          manager_liveness_public_pointer_digest !=
              selected->durable_public_pointer_content_digest)
        return Reply::Ok();
      const auto current = manager_liveness_receiver.get()->status();
      if (current.state != core::ManagerLivenessStateDTO::kCurrent ||
          current.command_fenced)
        return Reply::Ok();
    }
    if (installed_request &&
        (installed_request->apply_id != origin->apply_id ||
         installed_request->apply_generation != origin->apply_generation ||
         !core::SameProjectionIdentity(installed_request->candidate_projection_revision,
                                       selected->request.candidate_projection_revision)))
      return Reply::Err({"stale_generation", "Logical startup differs from its actual installed request", {}});
    const core::IoRuleAppliedContextDTO context{
        stable_kernel_owner->request_owner_epoch(),
        selected->publication_generation,
        selected->request.candidate_retained_bundle_id,
        selected->request.candidate_graph_revision};
    const auto projected =
        stable_kernel_owner->ReadInterruptRuleStateBaseline();
    {
      // Installation metadata is factual even while a state baseline is
      // pending. An already committed exact tuple must not toggle recovery.
      std::lock_guard lock(managed_composition_mutex);
      std::shared_lock execution_lock(execution_composition.mutex);
      const auto *product = execution_composition.product();
      if (projected && product && !execution_composition.current &&
          installed_apply == installed_request &&
          product->publication_generation == selected->publication_generation &&
          product->origin_named_apply == *origin &&
          product->bundle->retained_bundle_id() == context.retained_bundle_id &&
          product->bundle->value().cell_graph.graph_revision == context.graph_revision) {
        if (!product->compiled_global_io_rules &&
            !product->compiled_waiting_global_io_rules &&
            !product->io_signal_service && !projected->catalog &&
            !projected->waiting_for_program &&
            projected->verified_empty == context)
          return Reply::Ok();
        if (product->compiled_waiting_global_io_rules &&
            !product->compiled_global_io_rules && !product->io_signal_service &&
            !projected->catalog && !projected->verified_empty &&
            projected->waiting_for_program ==
                product->compiled_waiting_global_io_rules &&
            product->compiled_waiting_global_io_rules->context == context)
          return Reply::Ok();
        if (product->compiled_global_io_rules && product->io_signal_service &&
            !product->compiled_waiting_global_io_rules &&
            projected->catalog == product->compiled_global_io_rules &&
            !projected->waiting_for_program && !projected->verified_empty) {
          const auto &catalog_context = product->compiled_global_io_rules->context;
          const auto service = product->io_signal_service->RuleServiceConfiguration();
          if (service.ok() && service.value().owner_epoch == context.owner_epoch &&
              service.value().catalog_generation == catalog_context.catalog_generation &&
              catalog_context.owner_epoch == context.owner_epoch &&
              catalog_context.active_publication_generation == context.active_publication_generation &&
              catalog_context.retained_bundle_id == context.retained_bundle_id &&
              catalog_context.graph_revision == context.graph_revision)
            return Reply::Ok();
        }
      }
    }
    const auto status = component_manager::ReadManagerApplyStatusReadOnly(
        config.manager_state_root /
            std::string(component_manager::kManagerApplyJournalFilename),
        core::CellId(config.cell_id), *origin);
    if (!status.ok())
      return Reply::Err(status.error());
    const auto &intent = status.value().intent.intent;
    if (intent.apply_id != origin->apply_id ||
        intent.apply_generation != origin->apply_generation ||
        intent.candidate_retained_bundle_id != context.retained_bundle_id ||
        intent.candidate_graph_revision != context.graph_revision ||
        (status.value().publication_receipt &&
         status.value().publication_receipt->publication.publication_receipt != *selected))
      return Reply::Err({"stale_generation", "Logical startup has a foreign immutable Apply source", {}});
    if (!status.value().terminal_outcome)
      return Reply::Ok(); // Exact current original NamedApply is still pending.
    const auto actual = detail::ReadAppliedOwnerReadOnly(
        config.manager_state_root, core::CellId(config.cell_id),
        context.active_publication_generation);
    if (!actual.ok())
      return Reply::Err(actual.error());
    if (actual.value().publication != *selected)
      return Reply::Err({"stale_generation",
                         "Empty startup lost its actual Applied receipt",
                         {}});
    const auto selected_bundle =
        LoadManagedBundleAtPinnedRoot(config, context.retained_bundle_id);
    if (!selected_bundle)
      return Reply::Err(
          {"stale_generation", "Logical startup lost its sealed product", {}});
    std::shared_ptr<host_detail::ExecutionProduct> predecessor_logical;
    {
      std::shared_lock lock(execution_composition.mutex);
      predecessor_logical = execution_composition.logical_product;
    }
    const bool same_logical = predecessor_logical &&
        predecessor_logical->publication_generation == selected->publication_generation &&
        predecessor_logical->origin_named_apply == *origin &&
        predecessor_logical->bundle->retained_bundle_id() == context.retained_bundle_id &&
        predecessor_logical->bundle->value().cell_graph.graph_revision == context.graph_revision;
    const bool installed_logical = same_logical && installed_request.has_value();
    auto logical = same_logical ? predecessor_logical :
        BuildManagedLogicalExecutionProduct(
            config, selected_bundle->bundle, selected_bundle->package_root,
            selected->request.candidate_projection_revision,
            selected->publication_generation, *origin, detached_owner_clock,
            detached_owner_scheduler);
    // Removed components' action workers join after the locks are released.
    std::vector<std::jthread> retired_component_actions;
    std::lock_guard lock(managed_composition_mutex);
    {
      std::unique_lock execution_lock(execution_composition.mutex);
      if (active_managed_composition || execution_composition.current ||
          installed_apply != installed_request ||
          execution_composition.logical_product != predecessor_logical ||
          logical->publication_generation != selected->publication_generation ||
          logical->origin_named_apply != *origin ||
          logical->bundle->retained_bundle_id() != context.retained_bundle_id ||
          logical->bundle->value().cell_graph.graph_revision != context.graph_revision)
        return Reply::Ok(); // Exact final pin proved supersession, no effects.
      if (!installed_logical) {
        const auto excluded = stable_kernel_owner->SetRouteRecoveryPending(true);
        if (!excluded.ok())
          return excluded;
        retired_component_actions = execution_composition.replace_logical_product(logical);
        BindHostIoRuleEvidenceProcessor(*logical, execution_composition_owner);
        InstallCompositionIoSignals(*logical, *stable_cell_runtime_owner);
      }
    }
    // The shared helper reads the actual terminal proof outside composition/Core
    // locks and retains the final product pin through its bounded commit.
    // A warm Apply already staged this exact product under its parent fence.
    // The startup cadence can observe Succeeded before claim-release activation;
    // consume that staged slot through the same idempotent activation path.
    if (installed_logical)
      return EnsureAppliedGlobalIoRuleCatalog(
          execution_composition, *stable_kernel_owner, config.manager_state_root,
          core::CellId(config.cell_id));
    const auto installed = RestoreAppliedGlobalIoRuleCatalog(
        execution_composition, *stable_kernel_owner, config.manager_state_root,
        core::CellId(config.cell_id));
    if (!installed.ok())
      return installed;
    return stable_kernel_owner->SetRouteRecoveryPending(false);
  };
  const auto restore_empty_startup_globals = [&]() -> core::Result<void> {
    try {
      const auto restored = restore_empty_startup_globals_impl();
      if (!restored.ok()) {
        // Preserve the actual error response and the existing first failure;
        // a failed install/source is not an automatic cadence retry policy.
        auto message = restored.error().code + ": " + restored.error().message;
        if (const auto reason = restored.error().details.find("reason");
            reason != restored.error().details.end())
          message += "; reason=" + reason->second;
        const auto failure =
            std::make_exception_ptr(std::runtime_error(std::move(message)));
        std::lock_guard lock(managed_composition_mutex);
        if (!managed_composition_failure)
          managed_composition_failure = failure;
      }
      return restored;
    } catch (...) {
      std::lock_guard lock(managed_composition_mutex);
      if (!managed_composition_failure)
        managed_composition_failure = std::current_exception();
      throw;
    }
  };
  // Both ordinary startup and post-Succeeded Release use this SAME actual
  // facade/publication/product source. No additional recovery owner or poller.
  restore_current_logical_product = restore_empty_startup_globals;
  bool process_inventory_owner_changing_logged = false;
  while (g_stop_requested == 0) {
    rethrow_manager_liveness_failure();
    rethrow_composition_failure(true);
    try_start_current_composition();
    try_release_retired_composition();
    const auto global_startup = restore_empty_startup_globals();
    if (!global_startup.ok())
      rethrow_composition_failure();
    pollfd descriptor{config.manager_channel_fd, POLLIN | POLLHUP, 0};
    int ready = -1;
    do {
      ready = ::poll(&descriptor, 1U, 1000);
    } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
    if (g_stop_requested != 0) {
      break;
    }
    if (ready < 0) {
      FailErrno("poll managed runtime parent channel");
    }
    if ((descriptor.revents & POLLHUP) != 0 &&
        (descriptor.revents & POLLIN) == 0) {
      await_manager_loss_containment();
      rethrow_manager_liveness_failure();
      return 0;
    }
    Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
            "managed runtime parent channel became invalid");
    if ((descriptor.revents & POLLIN) == 0) {
      continue;
    }
    const auto packet = ReceiveManagedRuntimePacket(config.manager_channel_fd);
    const auto rpc_received = DiagnosticClock::now();
    if (!packet.has_value()) {
      rethrow_manager_liveness_failure();
      await_manager_loss_containment();
      return 0;
    }
    const std::string_view request(*packet);
    if (request == "hello") {
      SendManagedRuntimePacket(config.manager_channel_fd, ready_packet);
      continue;
    }
    if (request == "status") {
      RpcTiming timing{.received = rpc_received, .dispatch = DiagnosticClock::now()};
      DiagnosticTime reply_sent{};
      {
        struct InstalledControllerOccurrence final {
          std::shared_ptr<ControllerLaunchProcess> process;
          std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
        };
        std::vector<InstalledControllerOccurrence> controller_occurrences;
        {
          const auto lock_start = DiagnosticClock::now();
          std::lock_guard lock(managed_composition_mutex);
          timing.composition_wait += DiagnosticClock::now() - lock_start;
          // Every installed occurrence, including inactive/no-target components.
          // A changing composition does not erase the predecessor's actual owner.
          if (active_managed_composition) {
            const auto retain = [&](const Composition &component) {
              if (component.controller_process)
                controller_occurrences.push_back(
                    {component.controller_process,
                     component.ros2_control_runtime_handle});
            };
            retain(*active_managed_composition);
            for (const auto &peer :
                 active_managed_composition->participant_compositions)
              retain(*peer);
          }
        }
        // Only cached installed-handle readiness, waitid(WNOHANG), a short
        // evidence lock and an already-started lane enqueue run here. Group
        // waits/scan/reap execute off this 750ms RPC lane.
        for (const auto &occurrence : controller_occurrences)
          static_cast<void>(
              host_controller_launch_supervisor().request_unexpected_exit_cleanup(
                  occurrence.process, *controller_cleanup_scheduler,
                  controller_cleanup_clock->now(),
                  CachedControllerLaunchReadinessSource(occurrence.handle)));
        Require(source_report_sequence !=
                    std::numeric_limits<std::uint64_t>::max(),
                "managed runtime source sequence is exhausted");
        ++source_report_sequence;
        auto source_status =
            "manyforge.execution-host-status.v2 " + config.runtime_unit_id +
            " " + config.runtime_epoch + " " +
            std::to_string(source_report_sequence) + " 1 serving healthy " +
            offer_digest + " " + config.product_identity + " " +
            config.configuration_identity + " " +
            std::string(SelectedExecutionRuntimeRole()) +
            " closed_no_active_effect";
        const auto host_counts =
            host_controller_launch_supervisor().host_output_counters();
        source_status += " dropped_host_notice_lines=" +
                         std::to_string(host_counts.dropped_forward_lines) +
                         " dropped_host_notice_bytes=" +
                         std::to_string(host_counts.dropped_forward_bytes);
        timing.handler_end = DiagnosticClock::now();
        SendManagedRuntimePacket(config.manager_channel_fd, source_status);
        reply_sent = DiagnosticClock::now();
      }
      LogSlowRpc("status", timing, reply_sent);
      continue;
    }
    if (request == "shutdown") {
      SendManagedRuntimePacket(config.manager_channel_fd, "shutdown-accepted");
      return 0;
    }
    if (component_manager::detail::IsManagerLivenessRpcPacket(request)) {
      Fail("Manager-liveness protocol is forbidden on the ordinary managed "
           "runtime channel");
    }
    if (component_manager::detail::IsSelectedRuntimeCompositionRpcPacket(
            request)) {
      composition_timing = RpcTiming{.received = rpc_received};
      core::detail::AdmissionTimingScope core_timing;
      bool timing_finalize = false;
      std::string timing_apply_id;
      composition_request_received = std::chrono::steady_clock::now();
      composition_request_received_unix_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      component_manager::detail::SelectedRuntimeCompositionRpcResponse
          response =
              component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                  core::ErrorDTO{"selected_runtime_composition_request_failed",
                                 "selected runtime-composition request was not "
                                 "dispatched",
                                 {}}};
      try {
        const auto decoded = component_manager::detail::
            DecodeSelectedRuntimeCompositionRpcRequestVariant(request);
        composition_timing.dispatch = DiagnosticClock::now();
        response = std::visit(
            [&](const auto &value)
                -> component_manager::detail::SelectedRuntimeCompositionRpcResponse {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<T, component_manager::HostApplyRequest>) {
                timing_finalize = true;
                timing_apply_id = value.apply.apply_id.value().substr(0, 128);
                for (auto &ch : timing_apply_id)
                  if (ch <= ' ' || ch >= 127 || ch == '=') ch = '_';
                TimingSpan body{composition_timing.finalize_body};
                return apply_on_host(value);
              } else if constexpr (
                  std::is_same_v<
                      T, component_manager::
                             ManagerComponentProcessInventoryRequestDTO>) {
                using Result = component_manager::
                    ManagerComponentProcessInventoryResultDTO;
                const auto failure = [](const std::string &code,
                                        const std::string &message) {
                  return component_manager::detail::
                      SelectedRuntimeCompositionRpcFailure{
                          core::ErrorDTO{code, message, {}}};
                };
                if (!value.valid() ||
                    value.runtime_unit_id.value() != config.runtime_unit_id ||
                    value.runtime_epoch.value() != config.runtime_epoch)
                  return failure("installed_process_inspection_scope_stale",
                                 "installed process read names another actual "
                                 "host occurrence");
                const auto current_manager = [&] {
                  if (!manager_liveness_installed.load(
                          std::memory_order_acquire))
                    return false;
                  const auto observed =
                      manager_liveness_receiver.get()->status();
                  return observed.state ==
                             core::ManagerLivenessStateDTO::kCurrent &&
                         !observed.command_fenced &&
                         observed.manager_runtime_epoch ==
                             value.manager_runtime_epoch &&
                         observed.receiver_runtime_unit_id ==
                             value.runtime_unit_id &&
                         observed.receiver_runtime_epoch == value.runtime_epoch;
                };
                const auto same_publication = [&](const auto &publication) {
                  return publication.publication_generation ==
                             value
                                 .expected_predecessor_publication_generation &&
                         publication.durable_public_pointer_content_digest ==
                             value.expected_predecessor_public_pointer_digest &&
                         publication.request.candidate_projection_revision ==
                             value.predecessor_projection_revision;
                };
                if (!current_manager() ||
                    !same_publication(read_current_publication()))
                  return failure("installed_process_inspection_scope_stale",
                                 "installed process read lost its current "
                                 "Manager or publication scope");
                Result result{value, {}};
                bool changing = false;
                struct CapturedOccurrence final {
                  core::ControllableComponentId component_id;
                  std::shared_ptr<ControllerLaunchProcess> process;
                  std::shared_ptr<const deployment::RetainedBundle> bundle;
                };
                std::vector<CapturedOccurrence> captures;
                std::vector<VerifiedLocalNoneProcessOwner> local_owners;
                std::set<const RetainedGripperRuntime*> included_local_owners;
                // Same acquisition order as the existing candidate inspection:
                // managed composition mutex, then execution-composition read
                // lock.
                const auto roots = [&] {
                  return std::array<const Composition *, 5>{
                      active_managed_composition.get(),
                      retired_managed_composition.get(),
                      prepared_managed_composition.get(),
                      failed_prepared_composition.get(),
                      execution_composition.current};
                };
                const auto owners_changing = [&] {
                  return current_composition_building ||
                         prepared_managed_composition ||
                         managed_composition_failure != nullptr;
                };
                std::array<const Composition *, 5> before{};
                {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  before = roots();
                  changing = owners_changing();
                  std::set<const Composition *> included;
                  for (const auto *root : before) {
                    if (!root || !included.insert(root).second)
                      continue;
                    for (const auto &[component, view] :
                         IndexNativeComponentCompositions(*root)) {
                      if (!view->controller_process) {
                        // A local owner has no child process to retain from an
                        // uninstalled/retired view. Only the current installed
                        // composition can supply its exact logical occurrence.
                        const bool installed_root = root == active_managed_composition.get() ||
                            (!active_managed_composition && root == execution_composition.current);
                        if (installed_root && IsLocalNoOutputGripperOwner(*view) &&
                            included_local_owners.insert(view->gripper_runtime.get()).second) {
                          if (local_owners.size() == component_manager::kMaximumComponentProcessInventoryEntries)
                            return failure("installed_process_inventory_exceeds_bound", "installed logical owner union exceeds its bound");
                          local_owners.push_back(ReadVerifiedLocalNoneProcessOwner(*view, site_facts, value));
                        }
                        continue;
                      }
                      // At most five owner views can refer to the same process.
                      // Bound captures, then enforce the distinct union below.
                      if (captures.size() ==
                          before.size() *
                              component_manager::
                                  kMaximumComponentProcessInventoryEntries)
                        return failure(
                            "installed_process_inventory_exceeds_bound",
                            "installed process union exceeds its bound; no "
                            "truncated inventory returned");
                      captures.push_back(
                          {component, view->controller_process, view->bundle});
                    }
                  }
                }
                std::map<const ControllerLaunchProcess *, std::size_t>
                    included_processes;
                for (const auto &captured : captures) {
                  const auto &process = captured.process;
                  if (process->command().controllable_component_id !=
                      captured.component_id.value())
                    return failure("installed_process_inventory_invalid",
                                   "installed process occurrence has foreign "
                                   "component attribution");
                  const auto identity =
                      captured.bundle
                          ? deployment::derive_component_runtime_identity(
                                *captured.bundle, site_facts, captured.component_id)
                          : std::nullopt;
                  const auto duplicate = included_processes.find(process.get());
                  if (duplicate != included_processes.end()) {
                    const auto &previous =
                        result.process_occurrences[duplicate->second];
                    if (previous.controllable_component_id !=
                            captured.component_id ||
                        previous.installed_runtime_identity != identity)
                      return failure("installed_process_inventory_invalid",
                                     "shared process has inconsistent verified "
                                     "owner runtime attribution");
                    continue;
                  }
                  const auto cleanup = process->cleanup_observation();
                  // Observe the exact owned leader. These are this host's
                  // known launches, not an exhaustive external process census.
                  const auto presence =
                      process->exited()
                          ? component_manager::ProcessPresenceState::kAbsent
                          : component_manager::ProcessPresenceState::kPresent;
                  if (result.process_occurrences.size() ==
                      component_manager::
                          kMaximumComponentProcessInventoryEntries)
                    return failure("installed_process_inventory_exceeds_bound",
                                   "installed process union exceeds its bound; "
                                   "no truncated inventory returned");
                  included_processes.emplace(process.get(),
                                             result.process_occurrences.size());
                  result.process_occurrences.push_back(
                      {captured.component_id, process->command().identity,
                       process->pid(), presence, cleanup.state, cleanup.receipt,
                       identity});
                }
                if (!changing && value.include_component_lifecycles) {
                  const auto previous = LoadManagedBundleAtPinnedRoot(
                      config,
                      value.predecessor_projection_revision.retained_bundle_id);
                  if (!previous || !previous->bundle)
                    return failure("installed_component_lifecycle_unavailable",
                                   "current component product is unavailable");
                  // CONTRACTS §1: observation only, from the component runtime.
                  const auto states = execution_composition.component_runtime->Read().states;
                  for (const auto &component :
                       previous->bundle->value()
                           .cell_graph.controllable_components) {
                    const auto state = std::find_if(states.begin(), states.end(), [&](const auto &row) {
                      return row.component_id == component.controllable_component_id.value();
                    });
                    const auto lifecycle =
                        state == states.end() ? core::ComponentLifecycle::kNotPrepared : state->lifecycle;
                    result.component_lifecycles.push_back(
                        {component.controllable_component_id, core::ComponentLifecycleGeneration(1U),
                         lifecycle == core::ComponentLifecycle::kEnabled    ? core::ComponentLifecycleStateDTO::kActive
                         : lifecycle == core::ComponentLifecycle::kPrepared ? core::ComponentLifecycleStateDTO::kPrepared
                         : lifecycle == core::ComponentLifecycle::kFault    ? core::ComponentLifecycleStateDTO::kFaulted
                                                                            : core::ComponentLifecycleStateDTO::kDeclared});
                  }
                  std::sort(result.component_lifecycles.begin(),
                            result.component_lifecycles.end(),
                            [](const auto &a, const auto &b) {
                              return a.controllable_component_id <
                                     b.controllable_component_id;
                            });
                }
                {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  changing = changing || owners_changing() || roots() != before;
                  for (const auto& local : local_owners) {
                    auto* const installed_kernel = stable_addressed_kernel();
                    if (!installed_kernel)
                      return failure("installed_process_inspection_scope_stale", "installed logical owner lost its stable Kernel");
                    if (local.runtime->binding().value() != local.binding.value() ||
                        !local.handle->OwnsGripper(local.runtime->backend(), local.binding))
                      changing = true;
                    if (result.process_occurrences.size() == component_manager::kMaximumComponentProcessInventoryEntries)
                      return failure("installed_process_inventory_exceeds_bound", "complete physical/logical owner union exceeds its bound");
                    result.process_occurrences.push_back(local.occurrence);
                  }
                }
                std::sort(result.process_occurrences.begin(),
                          result.process_occurrences.end(),
                          [](const auto &a, const auto &b) {
                            return std::tie(a.controllable_component_id,
                                            a.launch_identity, a.leader_pid) <
                                   std::tie(b.controllable_component_id,
                                            b.launch_identity, b.leader_pid);
                          });
                if (!current_manager() ||
                    !same_publication(read_current_publication()))
                  return failure("installed_process_inspection_scope_stale",
                                 "installed process read lost its original "
                                 "scope during observation");
                // Close and Apply use this host's known launch union, also
                // after restart. Unobserved external processes are outside this
                // inventory; unsettled current owners still remain unavailable.
                if (changing) {
                  result.error = core::ErrorDTO{
                      "process_inventory_owner_changing",
                      "installed component owners changed or remain unsettled",
                      {{"runtime_unit_id", value.runtime_unit_id.value()},
                       {"runtime_epoch", value.runtime_epoch.value()}}};
                  if (!process_inventory_owner_changing_logged) {
                    std::cerr << "installed component process inventory unknown: "
                                 "reason=process_inventory_owner_changing"
                              << " runtime=" << value.runtime_unit_id.value()
                              << " epoch=" << value.runtime_epoch.value() << '\n';
                    process_inventory_owner_changing_logged = true;
                  }
                }
                if (!result.valid())
                  return failure("installed_process_inventory_invalid",
                                 "installed process union is not a bounded "
                                 "exact occurrence inventory");
                return result;
              } else if constexpr (
                  std::is_same_v<T, component_manager::ManagerComponentProcessStopRequestDTO> ||
                  std::is_same_v<T, component_manager::ManagerComponentProcessStopReadRequestDTO>) {
                constexpr bool read_only = std::is_same_v<T,
                    component_manager::ManagerComponentProcessStopReadRequestDTO>;
                const auto &original = [&]() -> const component_manager::ManagerComponentProcessStopRequestDTO & {
                  if constexpr (read_only) return value.request;
                  else return value;
                }();
                Require(original.valid() && original.runtime_unit_id.value() == config.runtime_unit_id &&
                            original.runtime_epoch.value() == config.runtime_epoch,
                        "component Stop names another host occurrence");
                using StopResult = component_manager::ManagerComponentProcessStopResultDTO;
                const auto unknown = [&](const char *code, const char *message) {
                  return StopResult{original, core::ControllerLaunchCleanupState::kUnknown,
                                    std::nullopt, core::ErrorDTO{code, message, {}}};
                };
                const auto component = original.controllable_component_id.value();
                std::shared_ptr<ControllerLaunchProcess> process;
                std::unique_ptr<core::ICellCloseAdmissionLease> close_join;
                bool retain_result = true;
                {
                  std::lock_guard lock(managed_composition_mutex);
                  const auto cached = component_process_stop_results.find(component);
                  if constexpr (read_only) {
                    if (cached == component_process_stop_results.end() ||
                        cached->second.result.request != original)
                      return unknown("component_process_stop_result_unavailable",
                                     "the exact original Stop result is missing, superseded or evicted");
                    auto result = cached->second.result; // Copy before consuming.
                    component_process_stop_results.erase(cached);
                    return result;
                  } else if (cached != component_process_stop_results.end() &&
                             cached->second.result.request == original) {
                    return cached->second.result; // Exact duplicate never signals again.
                  }
                  Require(manager_liveness_installed.load(std::memory_order_acquire),
                          "component Stop has no current Manager observation");
                  const auto manager = manager_liveness_receiver.get()->status();
                  Require(manager.state == core::ManagerLivenessStateDTO::kCurrent &&
                              !manager.command_fenced &&
                              manager.manager_runtime_epoch == original.manager_runtime_epoch &&
                              manager.receiver_runtime_unit_id == original.runtime_unit_id &&
                              manager.receiver_runtime_epoch == original.runtime_epoch,
                          "component Stop names an unavailable Manager occurrence");
                  const auto publication = read_current_publication();
                  Require(publication.publication_generation == original.expected_predecessor_publication_generation &&
                              publication.durable_public_pointer_content_digest == original.expected_predecessor_public_pointer_digest &&
                              publication.request.candidate_projection_revision == original.predecessor_projection_revision,
                          "component Stop predecessor publication changed");
                  Require(!current_composition_building && !prepared_managed_composition,
                          "component Stop cannot cross an in-progress composition effect");
                  Composition *occurrence{};
                  const auto inspect = [&](Composition *root) {
                    if (!root) return;
                    const auto include = [&](Composition &item) {
                      if (item.controllable_component_id == original.controllable_component_id &&
                          item.controller_process && item.controller_process->pid() == original.leader_pid &&
                          item.controller_process->command().identity == original.launch_identity) {
                        Require(!process || process == item.controller_process,
                                "component Stop has contradictory launch occurrence owners");
                        occurrence = &item;
                        process = item.controller_process;
                      }
                    };
                    include(*root);
                    for (auto &peer : root->participant_compositions) {
                      Require(peer != nullptr, "component Stop has a missing physical owner");
                      include(*peer);
                    }
                  };
                  {
                    // Same five-root union and managed -> execution read lock
                    // order as the passive inventory. Keep the actual shared
                    // process lease after releasing the execution read lock.
                    std::shared_lock execution_lock(execution_composition.mutex);
                    inspect(active_managed_composition.get());
                    inspect(retired_managed_composition.get());
                    inspect(prepared_managed_composition.get());
                    inspect(failed_prepared_composition.get());
                    inspect(execution_composition.current);
                  }
                  Require(occurrence && process, "component Stop exact launch owner is unavailable");
                  auto *const kernel = stable_addressed_kernel();
                  Require(kernel, "component Stop has no current Kernel owner");
                  auto acquired_join = kernel->AcquireCellCloseAdmissionLease();
                  if (!acquired_join.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{acquired_join.error()};
                  close_join = std::move(acquired_join.value());
                  const auto prerequisites = close_join->ValidatePrerequisites();
                  if (!prerequisites.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{prerequisites.error()};
                  // DX583: receipt retention never refuses the mandatory exact
                  // Stop. Unresolved entries remain; when all slots are live,
                  // return the immediate actual outcome without retaining it.
                  if (cached == component_process_stop_results.end() &&
                      component_process_stop_results.size() >= component_manager::kMaximumComponentProcessInventoryEntries) {
                    auto oldest = component_process_stop_results.end();
                    for (auto it = component_process_stop_results.begin();
                         it != component_process_stop_results.end(); ++it) {
                      if (it->second.result.state == core::ControllerLaunchCleanupState::kDrained &&
                          (oldest == component_process_stop_results.end() ||
                           it->second.insertion_order < oldest->second.insertion_order)) oldest = it;
                    }
                    if (oldest == component_process_stop_results.end()) {
                      retain_result = false;
                      if (!component_process_stop_retention_exhaustion_logged) {
                        component_process_stop_retention_exhaustion_logged = true;
                        std::cerr << "component process Stop receipt retention exhausted; exact Stop proceeds, later result read is unknown\n";
                      }
                    } else component_process_stop_results.erase(oldest);
                  }
                  if (component_process_stop_result_order == std::numeric_limits<std::uint64_t>::max())
                    retain_result = false;
                  if (retain_result) component_process_stop_results.insert_or_assign(component,
                      ComponentProcessStopResultEntry{
                          StopResult{original, core::ControllerLaunchCleanupState::kPending, std::nullopt},
                          ++component_process_stop_result_order});
                }
                StopResult result{original, core::ControllerLaunchCleanupState::kUnknown, std::nullopt};
                try {
                  // Same original retirement lane: no composition/map lock
                  // spans SIGINT/grace/KILL; the shared exact process lease does.
                  const auto before = process->cleanup_observation();
                  const auto before_effect = close_join->ValidatePrerequisites();
                  if (!before_effect.ok()) {
                    result = StopResult{original, core::ControllerLaunchCleanupState::kUnknown, std::nullopt, before_effect.error()};
                  } else {
                  if (before.state != ControllerLaunchCleanupState::kDrained)
                    Require(host_controller_launch_supervisor().retire_exact(process),
                            "component Stop launch occurrence was superseded");
                  const auto observed = process->cleanup_observation();
                  result.state = observed.state;
                  result.receipt = observed.receipt;
                  Require(result.valid(), "component Stop cached drain receipt is foreign");
                  }
                } catch (const std::exception &error) {
                  result = unknown("component_process_stop_outcome_unknown", error.what());
                } catch (...) {
                  result = unknown("component_process_stop_outcome_unknown", "process Stop has no exact terminal result");
                }
                // Physical retirement and exact receipt validation are done.
                // Do not reacquire composition while retaining Kernel
                // admission: builders use composition -> Kernel.
                close_join.reset();
                {
                  std::lock_guard lock(managed_composition_mutex);
                  const auto cached = component_process_stop_results.find(component);
                  if (retain_result) {
                    Require(cached != component_process_stop_results.end() && cached->second.result.request == original,
                            "component Stop result owner changed during retirement");
                    cached->second.result = result;
                  }
                }
                return result;
              }
            }, decoded);
      } catch (const std::exception &error) {
        auto failure = host_detail::SelectedRuntimeCompositionRpcError(error);
        // A composition refusal was logged once where it was found.
        if (failure.code != component_manager::kScenarioCompositionRefusedCode)
          std::cerr << "native selected composition failed: " << error.what() << '\n';
        response =
            component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                std::move(failure)};
      } catch (...) {
        response =
            component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                core::ErrorDTO{"selected_runtime_composition_request_failed",
                               "selected runtime-composition request failed",
                               {}}};
      }
      composition_timing.handler_end = DiagnosticClock::now();
      bool send_started = false;
      try {
        SendManagedRuntimePacket(
            config.manager_channel_fd,
            [&] {
              auto encoded = component_manager::detail::
                  EncodeSelectedRuntimeCompositionRpcResponse(response);
              send_started = true;
              return encoded;
            }());
      } catch (...) {
        if (timing_finalize && send_started)
          LogAdmissionTiming("selected_finalize", timing_apply_id, composition_timing,
                             core_timing.data, true);
        throw;
      }
      if (timing_finalize) {
        LogAdmissionTiming("selected_finalize", timing_apply_id, composition_timing,
                           core_timing.data, false);
        LogSlowRpc("selected_finalize", composition_timing, DiagnosticClock::now(), timing_apply_id);
      }
      continue;
    }
    if (component_manager::detail::IsKernelApplyFenceRpcPacket(request)) {
      component_manager::detail::KernelApplyFenceRpcResponse response =
          component_manager::detail::KernelApplyFenceRpcFailure{
              core::ErrorDTO{"kernel_apply_fence_request_failed",
                             "Kernel Apply-fence request was not dispatched",
                             {}}};
      try {
        const auto decoded =
            component_manager::detail::DecodeKernelApplyFenceRpcRequest(
                request);
        response = std::visit(
            [&](const auto &value)
                -> component_manager::detail::KernelApplyFenceRpcResponse {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<
                                T, core::ConfigurationFenceAdmissionQueryDTO>) {
                return fence_owner.ObserveAdmission(value);
              } else if constexpr (
                  std::is_same_v<T, core::ApplyConfigurationFenceIntentDTO>) {
                auto *const kernel = stable_addressed_kernel();
                const auto fresh = kernel->ReadHeldCellAdmissionClaim(
                    core::CellAdmissionClaimKindDTO::kManagerApply,
                    value.owner_transaction.apply_id.value());
                if (fresh) {
                  const auto parent = component_manager::
                      ReadManagerApplyConfigurationStatusReadOnly(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          core::CellId(config.cell_id),
                          {value.owner_transaction.apply_id,
                           value.owner_transaction.apply_generation},
                          *fresh);
                  Require(parent.ok() && !parent.value().terminal_outcome,
                          "configuration Apply fence has no exact live "
                          "ordinary parent");
                  return fence_owner.Acquire(
                      value,
                      [&](const auto &old_intent,
                          const auto &accepted) -> core::Result<void> {
                        const auto old_claim =
                            kernel->ReadHeldCellAdmissionClaim(
                                core::CellAdmissionClaimKindDTO::kManagerApply,
                                old_intent.owner_transaction.apply_id.value());
                        if (!old_claim)
                          return core::Result<void>::Err(
                              {"configuration_apply_original_claim_unavailable",
                               "overlapping barrier has no exact terminal "
                               "NamedApply authority",
                               {}});
                        const std::array old_claims{*old_claim};
                        const auto terminal = component_manager::
                            ValidateTerminalManagerApplyClaimsReadOnly(
                                config.manager_state_root /
                                    std::string(
                                        component_manager::
                                            kManagerApplyJournalFilename),
                                core::CellId(config.cell_id), old_claims);
                        if (!terminal.ok())
                          return terminal;
                        const auto old = component_manager::
                            ReadManagerApplyConfigurationStatusReadOnly(
                                config.manager_state_root /
                                    std::string(
                                        component_manager::
                                            kManagerApplyJournalFilename),
                                core::CellId(config.cell_id),
                                {old_intent.owner_transaction.apply_id,
                                 old_intent.owner_transaction.apply_generation},
                                *old_claim);
                        if (!old.ok())
                          return core::Result<void>::Err(old.error());
                        if (!old.value().fence_attempt ||
                            !old.value().fence_acceptance ||
                            old.value().fence_attempt->fence_intent !=
                                old_intent ||
                            old.value().fence_acceptance->accepted_fence !=
                                accepted ||
                            old.value()
                                    .fence_acceptance->fence_attempt_digest !=
                                old.value().fence_attempt->fence_attempt_digest)
                          return core::Result<void>::Err(
                              {"configuration_apply_original_fence_mismatch",
                               "overlapping barrier differs from its immutable "
                               "terminal NamedApply fence",
                               {}});
                        return core::Result<void>::Ok();
                      });
                }
                return fence_owner.Acquire(value);
              } else if constexpr (
                  std::is_same_v<T, core::ApplyFenceAcquireAttemptQueryDTO>) {
                return fence_owner.ReconcileAcquireAttempt(value);
              } else if constexpr (std::is_same_v<
                                       T, core::ApplyFenceRehydrateDTO>) {
                return fence_owner.Rehydrate(value);
              } else if constexpr (
                  std::is_same_v<T, core::ConfigurationFenceStatusQueryDTO>) {
                return fence_owner.QueryStatus(value);
              } else {
                return fence_owner.Release(value);
              }
            },
            decoded);
      } catch (const std::exception &error) {
        response = component_manager::detail::KernelApplyFenceRpcFailure{
            core::ErrorDTO{
                "kernel_apply_fence_request_failed", error.what(), {}}};
      } catch (...) {
        response = component_manager::detail::KernelApplyFenceRpcFailure{
            core::ErrorDTO{"kernel_apply_fence_request_failed",
                           "Kernel Apply-fence request failed",
                           {}}};
      }
      SendManagedRuntimePacket(
          config.manager_channel_fd,
          component_manager::detail::EncodeKernelApplyFenceRpcResponse(
              response));
      continue;
    }
    if (component_manager::detail::IsKernelCellAdmissionRpcPacket(request)) {
      RpcTiming timing{.received = rpc_received};
      DiagnosticTime reply_sent{};
      std::string response_command = "cell_admission_get";
      std::string timing_request_id;
      core::detail::AdmissionTimingScope core_timing;
      bool send_started = false;
      try {
      {
        component_manager::detail::KernelCellAdmissionRpcResponse response =
            core::ErrorDTO{"cell_admission_request_failed",
                           "cell-admission request was not dispatched",
                           {}};
        std::shared_ptr<core::IIoSignalService> actual_retired_service;
        try {
          const auto decoded =
              component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                  request);
          response_command = decoded.command;
          try {
            const core::RequestId *id = nullptr;
            if (const auto *intent = std::get_if<core::CellAdmissionClaimIntentDTO>(&decoded.request))
              id = &intent->request_id;
            else if (const auto *release = std::get_if<core::CellAdmissionClaimReleaseDTO>(&decoded.request))
              id = &release->request_id;
            else if (const auto *recovery = std::get_if<core::CellAdmissionRecoveryReceiptQueryDTO>(&decoded.request))
              id = &recovery->intent.request_id;
            if (id) {
              timing_request_id = id->value().substr(0, 128);
              for (auto &ch : timing_request_id)
                if (ch <= ' ' || ch >= 127 || ch == '=') ch = '_';
            }
          } catch (...) {}
          timing.dispatch = DiagnosticClock::now();
          if (PreemptsPlanning(decoded)) {
            PreemptManagedExecutionSurfacePlanning(execution_surface_control);
          }
          core::ResourceAddressedExecutionKernel *owner = nullptr;
          {
            const auto lock_start = DiagnosticClock::now();
            std::lock_guard lock(managed_composition_mutex);
            timing.composition_wait += DiagnosticClock::now() - lock_start;
            owner = stable_addressed_kernel();
            Require(owner != nullptr, "stable Kernel is not installed");
          }
          const auto *const acquire_intent =
              std::get_if<core::CellAdmissionClaimIntentDTO>(&decoded.request);
          const auto *const release =
              std::get_if<core::CellAdmissionClaimReleaseDTO>(&decoded.request);
          const auto globals =
              decoded.command == "cell_admission_release" && release
                  ? activate_globals_before_claim_release(*release)
                  : core::Result<void>::Ok();
          if (!globals.ok()) {
            response = globals.error();
          } else if (decoded.command == "cell_admission_release" && release &&
                     release->owner_role ==
                         core::CellAdmissionOwnerRoleDTO::kManager) {
            // Ordinary fresh success settles only authenticated terminal Apply
            // predecessors. No lifecycle claim or linked activation uses this
            // path.
            const auto settle =
                [&]() -> core::Result<core::AuthoringAdmissionDTO> {
              const auto fresh = owner->ReadHeldCellAdmissionClaim(
                  core::CellAdmissionClaimKindDTO::kManagerApply,
                  release->owner_transaction_id);
              if (!fresh)
                return owner->ReleaseCellAdmissionClaim(*release);
              const auto database =
                  config.manager_state_root /
                  std::string(component_manager::kManagerApplyJournalFilename);
              const core::CellId cell(config.cell_id);
              const auto parent =
                  component_manager::ReadManagerApplyStatusByClaimIntentReadOnly(
                      database, cell, fresh->intent);
              if (!parent.ok())
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    parent.error());
              if (!parent.value().terminal_outcome ||
                  !parent.value().terminal_outcome->terminal_outcome.succeeded())
                return owner->ReleaseCellAdmissionClaim(*release);
              auto old = owner->ReadHeldCellAdmissionClaims(
                  core::CellAdmissionClaimKindDTO::kManagerApply);
              std::erase(old, *fresh);
              if (old.empty())
                return owner->ReleaseCellAdmissionClaim(*release);
              const auto success =
                  component_manager::ValidateFreshManagerApplySuccessReadOnly(
                      database, cell, *fresh, *release);
              if (!success.ok())
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    success.error());
              const auto terminal =
                  component_manager::ValidateTerminalManagerApplyClaimsReadOnly(
                      database, cell, old);
              if (!terminal.ok())
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    terminal.error());
              const auto manager = manager_liveness_receiver.get()->status();
              if (manager.state != core::ManagerLivenessStateDTO::kCurrent ||
                  manager.command_fenced ||
                  manager.manager_owner_epoch != fresh->intent.owner_epoch)
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    {"actual_installed_state_unknown",
                     "fresh success lost its current Manager; restart the "
                     "runtime",
                     {}});
              std::vector<core::ConfigurationFenceStatusDTO> fences;
              fences.reserve(old.size());
              for (const auto &claim : old) {
                const auto previous =
                    component_manager::ReadManagerApplyStatusByClaimIntentReadOnly(
                        database, cell, claim.intent);
                if (!previous.ok())
                  return core::Result<core::AuthoringAdmissionDTO>::Err(
                      previous.error());
                if (previous.value().fence_attempt.has_value() !=
                        previous.value().fence_acceptance.has_value())
                  return core::Result<core::AuthoringAdmissionDTO>::Err(
                      {"configuration_apply_original_fence_unavailable",
                       "old Apply has no complete ordinary fence authority",
                       {}});
                // A pre-effect terminal may legitimately own no physical fence.
                if (!previous.value().fence_attempt)
                  continue;
                const auto &attempt = *previous.value().fence_attempt;
                const auto &acceptance = *previous.value().fence_acceptance;
                if (!acceptance.accepted_fence.matches(attempt.fence_intent) ||
                    acceptance.fence_attempt_digest !=
                        attempt.fence_attempt_digest)
                  return core::Result<core::AuthoringAdmissionDTO>::Err(
                      {"configuration_apply_original_fence_mismatch",
                       "old Apply fence differs from its immutable acceptance",
                       {}});
                const auto &accepted = acceptance.accepted_fence;
                const bool gate_held = admission_gate.exact_fence_is_held(
                    accepted.configuration_fence_id, accepted.captured_scope);
                if (gate_held) {
                  const auto observed =
                      admission_gate.Observe(accepted.captured_scope);
                  for (const auto &domain : accepted.captured_scope.domains) {
                    if (domain.candidate_control_domain_revision &&
                        std::none_of(
                            observed.active_control_configurations.begin(),
                            observed.active_control_configurations.end(),
                            [&](const auto &configuration) {
                              return configuration.control_domain_id ==
                                     domain.control_domain_id;
                            }))
                      return core::Result<core::AuthoringAdmissionDTO>::Err(
                          {"configuration_apply_old_domain_unavailable",
                           "old fence domain has no current configuration; "
                           "restart "
                           "the runtime",
                           {}});
                  }
                }
                const core::ConfigurationFenceStatusQueryDTO query{
                    attempt.fence_intent.owner_transaction,
                    accepted.configuration_fence_id,
                    accepted.configuration_fence_generation,
                    accepted.captured_scope, manager.manager_runtime_epoch};
                const auto status = fence_owner.QueryStatus(query);
                if (!status.matches(query) ||
                    (status.state == core::ConfigurationFenceState::kHeld &&
                     !gate_held) ||
                    status.reconciliation_evidence
                            .affected_queued_operation_count != 0U ||
                    status.reconciliation_evidence
                            .affected_active_operation_count != 0U)
                  return core::Result<core::AuthoringAdmissionDTO>::Err(
                      {"configuration_apply_old_fence_unsettled",
                       "old fence is not exactly idle and reconciled",
                       {}});
                fences.push_back(status);
              }
              // Complete all reads/allocations and all-domain checks before the
              // first release. The fence owner takes its own mutex then admission
              // exclusion; never call it from the Kernel's claim validator.
              for (const auto &status : fences)
                if (status.state != core::ConfigurationFenceState::kHeld &&
                    status.state != core::ConfigurationFenceState::kReleased)
                  return core::Result<core::AuthoringAdmissionDTO>::Err(
                      {"configuration_apply_old_fence_unknown",
                       "old fence outcome is unavailable; restart the workbench "
                       "container",
                       {}});
              for (const auto &status : fences) {
                if (status.state == core::ConfigurationFenceState::kReleased)
                  continue;
                static_cast<void>(
                    fence_owner.Release({status.reconciliation_evidence,
                                         manager.manager_runtime_epoch}));
              }
              const auto settled = owner->ReleaseTerminalApplyClaimsAfterSuccess(
                  *fresh, *release,
                  [&](const auto &actual_fresh, const auto &actual_release,
                      const auto actual_old) -> core::Result<void> {
                    if (actual_old.size() != old.size() ||
                        !std::equal(actual_old.begin(), actual_old.end(),
                                    old.begin()))
                      return core::Result<void>::Err(
                          {"configuration_apply_old_claims_changed",
                           "old Apply claims changed before fresh-success "
                           "settlement",
                           {}});
                    for (const auto &status : fences)
                      if (admission_gate.exact_fence_is_held(
                              status.echoed_query.configuration_fence_id,
                              status.echoed_query.expected_scope))
                        return core::Result<void>::Err(
                            {"configuration_apply_old_fence_held",
                             "old Apply fence remains held",
                             {}});
                    const auto valid_success = component_manager::
                        ValidateFreshManagerApplySuccessReadOnly(
                            database, cell, actual_fresh, actual_release);
                    if (!valid_success.ok())
                      return valid_success;
                    return component_manager::
                        ValidateTerminalManagerApplyClaimsReadOnly(database, cell,
                                                                   actual_old);
                  });
              if (!settled.ok())
                return settled;
              return owner->ReleaseCellAdmissionClaim(*release);
            };
            const auto result = settle();
            response =
                result.ok()
                    ? component_manager::detail::KernelCellAdmissionRpcResponse(
                          result.value())
                    : component_manager::detail::KernelCellAdmissionRpcResponse(
                          result.error());
          } else if (decoded.command == "cell_admission_acquire" &&
                     acquire_intent &&
                     acquire_intent->claim_kind ==
                         core::CellAdmissionClaimKindDTO::kManagerApply) {
            // Classify ordinary versus linked only from a successful complete
            // owner snapshot. Read errors never select ordinary permission.
            const auto read = component_manager::ReadManagerApplyStatusByClaimIntentReadOnly(
                config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
                core::CellId(config.cell_id), *acquire_intent);
            if (!read.ok()) {
              response = read.error();
            } else {
              const auto existing_claim = owner->ReadHeldCellAdmissionClaim(
                  core::CellAdmissionClaimKindDTO::kManagerApply,
                  acquire_intent->owner_transaction_id);
              std::shared_ptr<const deployment::RetainedBundle>
                  actual_candidate_product;
              // Exact retained acquisition is observation even after publication
              // or during uncertainty. Classify it before any fresh-product read.
              if (!existing_claim || existing_claim->intent != *acquire_intent) {
                const auto &parent = read.value();
                const auto predecessor = read_current_publication();
                const auto previous = LoadManagedBundleAtPinnedRoot(config,
                    parent.intent.intent.expected_active_projection_revision.retained_bundle_id);
                const auto candidate = LoadManagedBundleAtPinnedRoot(config,
                    parent.intent.intent.candidate_retained_bundle_id);
                Require(previous && previous->bundle && candidate && candidate->bundle &&
                            core::SameActiveDeployment(predecessor.request.candidate_projection_revision,
                                parent.intent.intent.expected_active_projection_revision),
                        "configuration Apply lacks its exact current/candidate products");
                actual_candidate_product = candidate->bundle;
              }
              const auto lock_start = DiagnosticClock::now();
              std::lock_guard lock(managed_composition_mutex);
              timing.composition_wait += DiagnosticClock::now() - lock_start;
              const auto acquired =
                  owner->AcquireConfigurationApplyCellAdmissionClaim(
                      *acquire_intent,
                      [&](const auto &intent, const auto old_claims, const auto &,
                          const auto &manager_epoch) -> core::Result<void> {
                        const auto manager =
                            manager_liveness_receiver.get()->status();
                        if (current_composition_building ||
                            manager.state !=
                                core::ManagerLivenessStateDTO::kCurrent ||
                            manager.command_fenced ||
                            manager.manager_owner_epoch != intent.owner_epoch ||
                            manager.manager_runtime_epoch != manager_epoch ||
                            manager.receiver_runtime_unit_id !=
                                core::RuntimeUnitId(config.runtime_unit_id) ||
                            manager.receiver_runtime_epoch !=
                                core::RuntimeEpoch(config.runtime_epoch))
                          return core::Result<void>::Err(
                              {"actual_installed_state_unknown",
                               "configuration Apply lost its current installed "
                               "owner; restart the workbench container",
                               {}});
                        const auto database =
                            config.manager_state_root /
                            std::string(
                                component_manager::kManagerApplyJournalFilename);
                        const auto terminal = component_manager::
                            ValidateTerminalManagerApplyClaimsReadOnly(
                                database, core::CellId(config.cell_id),
                                old_claims);
                        if (!terminal.ok())
                          return terminal;
                        Require(actual_candidate_product != nullptr,
                                "configuration Apply has no exact fresh "
                                "candidate product");
                        for (const auto &old_claim : old_claims) {
                          const auto old = component_manager::
                              ReadManagerApplyStatusByClaimIntentReadOnly(
                                  database, core::CellId(config.cell_id),
                                  old_claim.intent);
                          if (!old.ok())
                            return core::Result<void>::Err(old.error());
                          if (!old.value().fence_acceptance)
                            continue;
                          const auto &accepted =
                              old.value().fence_acceptance->accepted_fence;
                          if (!admission_gate.exact_fence_is_held(
                                  accepted.configuration_fence_id,
                                  accepted.captured_scope))
                            continue;
                          for (const auto &domain :
                               accepted.captured_scope.domains)
                            if (domain.candidate_control_domain_revision &&
                                std::none_of(
                                    actual_candidate_product->value()
                                        .desired_control_configurations.begin(),
                                    actual_candidate_product->value()
                                        .desired_control_configurations.end(),
                                    [&](const auto &wanted) {
                                      return wanted.control_domain_id ==
                                             domain.control_domain_id;
                                    }))
                              return core::Result<void>::Err(
                                  {"configuration_apply_old_domain_unavailable",
                                   "old fence domain would be removed without a "
                                   "settlement representation; restart the "
                                   "runtime",
                                   {}});
                        }
                        return core::Result<void>::Ok();
                      });
              response =
                  acquired.ok()
                      ? component_manager::detail::KernelCellAdmissionRpcResponse(
                            acquired.value())
                      : component_manager::detail::KernelCellAdmissionRpcResponse(
                            acquired.error());
            }
          } else {
            const auto lock_start = DiagnosticClock::now();
            std::lock_guard lock(managed_composition_mutex);
            timing.composition_wait += DiagnosticClock::now() - lock_start;
            response = component_manager::detail::DispatchKernelCellAdmissionRpc(
                *owner, decoded, &actual_retired_service);
          }
        } catch (const std::exception &error) {
          response =
              core::ErrorDTO{"cell_admission_request_failed", error.what(), {}};
        } catch (...) {
          response = core::ErrorDTO{"cell_admission_request_failed",
                                    "cell-admission request failed",
                                    {}};
        }
        timing.handler_end = DiagnosticClock::now();
        // Includes an actual teardown followed by an error. All Core and
        // managed-composition locks are released before queueing this wake.
        if (auto runtime = std::dynamic_pointer_cast<HostIoSignalRuntime>(actual_retired_service))
          runtime->RequestRuleEvidenceProcessing();
        std::string encoded_response;
        try {
          encoded_response =
              component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                  response_command, response);
        } catch (...) {
          encoded_response =
              component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                  response_command,
                  core::ErrorDTO{
                      "cell_admission_response_oversized",
                      "cell-admission response exceeds the bounded private "
                      "transport",
                      {}});
        }
        send_started = true;
        SendManagedRuntimePacket(config.manager_channel_fd, encoded_response);
        send_started = false;
        reply_sent = DiagnosticClock::now();
      }
      } catch (...) {
        if (send_started)
          LogAdmissionTiming(response_command, timing_request_id, timing, core_timing.data, true);
        throw;
      }
      LogAdmissionTiming(response_command, timing_request_id, timing, core_timing.data, false);
      LogSlowRpc(response_command, timing, reply_sent, {}, timing_request_id);
      continue;
    }
    component_manager::detail::NativeProjectionRpcResponse response;
    try {
      response = projections.Dispatch(
          component_manager::detail::DecodeNativeProjectionRpcRequest(request));
    } catch (const std::exception &error) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed", error.what(), {}}};
    } catch (...) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed",
                         "native projection request failed",
                         {}}};
    }
    SendManagedRuntimePacket(
        config.manager_channel_fd,
        component_manager::detail::EncodeNativeProjectionRpcResponse(response));
  }
  return 0;
}

} // namespace
} // namespace manyforge::roboplan_backend

namespace manyforge::roboplan_backend::host_detail {

int RunExecutionHostMain(int argc, char **argv) {
  try {
    manyforge::roboplan_backend::InstallSignalHandlers();
    // The execution host runs only as the Manager's managed child.
    return manyforge::roboplan_backend::RunManagedRuntimeChild(
        manyforge::roboplan_backend::ParseManagedRuntimeArguments(argc, argv));
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Execution host failed with an unknown error\n";
    return 71;
  }
}

} // namespace manyforge::roboplan_backend::host_detail
