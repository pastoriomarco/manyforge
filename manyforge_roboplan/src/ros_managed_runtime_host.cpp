#include "ros_managed_runtime_host.hpp"
#include "applied_global_io_rule_installation.hpp"
#include "applied_program_blackboard_installation.hpp"
#include "applied_program_owner_reader.hpp"
#include "selected_program_products.hpp"
#include "host_io_provider_installation.hpp"
#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"
#include "program_reset_position_observation.hpp"
#include "simulated_plc_readiness.hpp"
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
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
#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/time/host_boot_identity.hpp>
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
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
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
#include <manyforge_roboplan/retained_measured_robot_geometry.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_component_owner.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_native_view_predecessor.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_static_frames.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/simulated_plc_launch.hpp>
#include <manyforge_roboplan/pose_source_launch.hpp>
#include <manyforge_roboplan/component_stream_sample_bridge.hpp>
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include "managed_ros2_component_installation.hpp"
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_vendor_device_status.hpp>
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
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>

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
#include "retained_native_runtime_internal.hpp"
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

// V-B2: each lifecycle worker runs one whole transition (all its substeps)
// per domain, so a cold multi-component Execute needs more than 2 (U1: 8 is
// a first value, to measure on E5 Execute from cold; route selection shares
// the pool). Each brand port polls its transition's postconditions at the
// CCLC observation poll period.
constexpr std::size_t kCCLCLifecycleWorkerCount = 8U;
constexpr core::Duration kCCLCObservationPollPeriodNs = 10'000'000LL;

// Active TCP (M7): the robot's TCP list, TCP-A1's `robot_tcps` of its
// direct-control binding's robot row (design 1.2).
[[nodiscard]] std::vector<RobotTcp>
DirectControlRobotTcps(const deployment::ProgramBinding &binding) {
  for (const auto &target : binding.value().target_role_bindings)
    if (target.target_role_id.value() == deployment::kDirectControlTargetRoleId)
      return target.robot_tcps.value_or(std::vector<RobotTcp>{});
  return {};
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
  return RoboPlanWorkerModelDTO{"manyforge_i6_native_execution_worker",
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
    const core::ProjectionNativeRuntimeBindingDTO binding{
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

struct ManualControlDescriptors final {
  CanonicalValue motion;
  CanonicalValue gripper;
};

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

[[nodiscard]] ManualControlDescriptors BuildManualControlDescriptors(
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const core::JointLimitsDTO &motion_limits,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &motion_participant_policy,
    const std::optional<double> &deployment_maximum_tcp_linear_speed_mps,
    const deployment::execution_runtime_contract::
        OrdinaryMotionJerkSettings *ordinary_jerk_settings,
    const std::vector<deployment::execution_runtime_contract::
                          Configuration::DirectControlJointLimit>
        &direct_control_joint_limits,
    const std::vector<deployment::ros2_control_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents,
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
      const auto *imported =
          setting != settings.joint_limits.end() && setting->imported
              ? &*setting->imported
              : nullptr;
      const char *source =
          !settings.enabled ? "off"
          : setting != settings.joint_limits.end() && setting->override_rad_s3
              ? "override"
          : imported != nullptr && imported->enabled ? "imported"
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
          {"imported",
           imported != nullptr
               ? CanonicalValue(CanonicalValue::Object{
                     {"maximum_rad_s3",
                      CanonicalValue(imported->maximum_rad_s3)},
                     {"enabled", CanonicalValue(imported->enabled)},
                     {"machine_required",
                      CanonicalValue(imported->machine_required)},
                     {"source_id", CanonicalValue(imported->source_id)},
                     {"source_digest",
                      CanonicalValue(imported->source_digest.value())}})
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

  if (!gripper_binding.has_value()) {
    Require(!gripper_limits.has_value(),
            "manual-control gripper limits have no physical gripper binding");
    Require(std::none_of(
                gripper_target_configurations.begin(),
                gripper_target_configurations.end(), [&](const auto &candidate) {
                  return candidate.controllable_component_id ==
                         motion.controllable_component_id;
                }),
            "manual-control gripper configuration has no physical gripper binding");
    return {std::move(motion_descriptor), CanonicalValue(nullptr)};
  }

  return {std::move(motion_descriptor),
          BuildManualControlGripperDescriptor(
              *gripper_binding, gripper_limits,
              gripper_target_configurations, model_map, documents)};
}



[[nodiscard]] std::map<core::ControllableComponentId, const Composition *>
IndexNativeComponentCompositions(const Composition &cell) {
  std::map<core::ControllableComponentId, const Composition *> result;
  const auto include = [&](const Composition &composition) {
    const bool io = composition.io_provider_drivers && !composition.io_provider_drivers->empty();
    Require(composition.component_runtime_identity() != nullptr &&
                (composition.runtime || composition.gripper_runtime || io) &&
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
  std::shared_ptr<RetainedNativeGripperRuntime> runtime;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  core::RuntimeParticipantBinding binding;
  core::RuntimeRouteConfigurationReadbackDTO route;
  component_manager::ManagerComponentProcessOccurrenceDTO occurrence;
};

[[nodiscard]] VerifiedLocalNoneProcessOwner ReadVerifiedLocalNoneProcessOwner(
    const Composition& owner,
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
  Require(actual_backend && value.control_path &&
              registration.engine == owner.gripper_runtime->participant_engine() &&
              registration.stop_execution_port == owner.gripper_runtime->participant_engine() &&
              registration.stop_readiness &&
              registration.stop_readiness->value().expected_controller_domain.physical_expectation ==
                  core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone &&
              owner.ros2_control_runtime_handle->OwnsGripper(backend, binding) &&
              value.retained_bundle_id == request.predecessor_projection_revision.retained_bundle_id &&
              value.graph_revision == request.predecessor_projection_revision.graph_revision &&
              value.runtime_unit_epochs == std::vector<core::RuntimeUnitEpochBindingDTO>{
                  {request.runtime_unit_id, request.runtime_epoch}},
          "local None process occurrence crosses its actual binding/engine/Stop owner");
  const auto logical = actual_backend->gripper_transport_readiness(value.command_target_id.value());
  Require(logical.configured && logical.logical_owner_ready &&
              logical.output_mode == core::GripperOutputMode::kNone,
          "local None process occurrence has no current configured logical backend");
  const auto& path = *value.control_path;
  std::vector<core::ActiveControlConfigurationBindingRouteDTO> expected_routes;
  std::vector<core::RouteReadinessPublicationRowDTO> expected_readiness;
  std::set<core::ControlBindingId> actual_ids;
  const auto registrations = owner.gripper_runtime->registrations();
  const auto& source_bindings = owner.bundle->value().cell_graph.control_bindings;
  for (const auto& row : registrations) {
    const auto& alias = row.binding.value();
    Require(alias.control_path && retained_bindings_share_backend_owner(binding, row.binding) &&
                row.engine == registration.engine && row.stop_execution_port == registration.stop_execution_port &&
                row.stop_readiness && row.stop_readiness->value().expected_controller_domain.physical_expectation ==
                    core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone &&
                (alias.capability_id == value.capability_id ||
                 (alias.capability_id.value() == "tool.gripper.feedback" && !alias.command_schema && !row.intrinsic_direct_control)),
            "local None occurrence has an alias outside its exact command owner");
    const auto& alias_path = *alias.control_path;
    Require(actual_ids.insert(alias_path.control_binding_id).second &&
                std::count_if(source_bindings.begin(), source_bindings.end(), [&](const auto& source) {
                  return source.control_binding_id == alias_path.control_binding_id &&
                      source.control_binding_revision == alias_path.control_binding_revision &&
                      source.controllable_component_id == alias.controllable_component_id &&
                      source.command_target_id == alias.command_target_id && source.capability_id == alias.capability_id &&
                      source.capability_definition_revision == alias.capability_definition_revision &&
                      source.control_domain_id == alias_path.control_domain_id &&
                      source.provider_runtime_unit_id == request.runtime_unit_id;
                }) == 1,
            "local None occurrence has an unknown or duplicated source binding");
    expected_routes.push_back({alias_path.control_binding_id, alias_path.control_binding_revision, alias_path.control_route_id});
    expected_readiness.push_back({alias.controllable_component_id,
        row.stop_readiness->value().component_lifecycle_generation,
        alias_path.control_binding_id, alias_path.control_binding_revision,
        row.stop_readiness->value().resolved_stop_base_plan.resolved_stop_base_plan_id});
  }
  Require(std::count_if(source_bindings.begin(), source_bindings.end(), [&](const auto& source) {
            return source.controllable_component_id == value.controllable_component_id;
          }) == static_cast<std::ptrdiff_t>(registrations.size()),
          "local None occurrence omits source-declared component bindings");
  std::sort(expected_routes.begin(), expected_routes.end(), [](const auto& left, const auto& right) {
    return left.control_binding_id < right.control_binding_id;
  });
  std::sort(expected_readiness.begin(), expected_readiness.end(), core::CanonicalRouteReadinessPublicationRowLess);
  auto route = owner.active_addressed_kernel()->current_runtime_route_configuration(path.control_domain_id);
  Require(route.ok() && route.value(),
          "local None process occurrence has no installed Kernel route");
  const auto& configuration = route.value()->active_configuration;
  Require(configuration.state == core::ActiveControlConfigurationStateDTO::kReconciled &&
              configuration.control_domain_id == path.control_domain_id &&
              configuration.control_domain_revision == path.control_domain_revision &&
              configuration.controller_topology_id == path.controller_topology_id &&
              configuration.topology_revision == path.topology_revision &&
              configuration.activation_set_id == path.activation_set_id &&
              configuration.active_control_configuration_id == path.active_control_configuration_id &&
              configuration.active_control_configuration_generation == path.active_control_configuration_generation &&
              configuration.source_runtime_unit_id == request.runtime_unit_id &&
              configuration.source_runtime_epoch == request.runtime_epoch &&
              configuration.binding_routes == expected_routes &&
              route.value()->readiness_rows == expected_readiness,
          "local None process occurrence differs from the exact Kernel route/Stop readback");
  component_manager::ManagerComponentProcessOccurrenceDTO occurrence{
      value.controllable_component_id, {}, 0, component_manager::ProcessPresenceState::kAbsent,
      core::ControllerLaunchCleanupState::kNotRequested, {},
      deployment::derive_component_runtime_identity(*owner.bundle, value.controllable_component_id),
      component_manager::ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal,
      component_manager::ManagerPneumaticNoneProcessOwnerDTO{
          value.command_target_id, path.control_binding_id, path.control_binding_revision}};
  const auto verified = component_manager::ValidateManagerPneumaticNoneProcessOwner(
      *owner.bundle, request, occurrence);
  Require(verified.ok(), verified.ok() ? std::string{} : verified.error().message);
  return {owner.gripper_runtime, owner.ros2_control_runtime_handle, binding,
          std::move(*route.value()), std::move(occurrence)};
}

[[nodiscard]] std::optional<std::string> RetainedControllerLaunchExit(
    const std::shared_ptr<ControllerLaunchProcess> &process) {
  if (!process)
    return std::nullopt;
  const auto reason = process->exited();
  if (!reason)
    return std::nullopt;
  const auto evidence = process->exit_evidence();
  return *reason + " pid=" + std::to_string(process->pid()) +
         (evidence ? " observed_utc=" + evidence->observed_utc : "");
}

void VerifyRetainedNativePredecessors(
    const Composition *active,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  const auto components = active == nullptr
                              ? std::map<core::ControllableComponentId,
                                         const Composition *>{}
                              : IndexNativeComponentCompositions(*active);
  static_cast<void>(request);
  if (active != nullptr)
    Require(active->bundle &&
                deployment::verify_retained_bundle_identity(*active->bundle),
            "actual_installed_state_unknown: installed component product is "
            "not verified");
  for (const auto &[id, owner] : components)
    Require(
        owner->bundle && owner->component_runtime_identity() &&
            deployment::derive_component_runtime_identity(*owner->bundle, id),
        "actual_installed_state_unknown: installed component lacks its real "
        "owner identity");
  return;
}

[[nodiscard]] const Composition *FindActualRetainedComponentOwner(
    const core::ControllableComponentId &component,
    const Composition &installed, const Composition *predecessor,
    const std::span<const Composition *const> other_predecessors) {
  const auto find = [&](const Composition *root) -> const Composition * {
    if (!root)
      return nullptr;
    const auto owners = IndexNativeComponentCompositions(*root);
    const auto found = owners.find(component);
    return found != owners.end() &&
                   found->second->component_runtime_identity() ==
                       installed.component_runtime_identity()
               ? found->second
               : nullptr;
  };
  if (const auto *owner = find(predecessor))
    return owner;
  for (const auto *root : other_predecessors)
    if (const auto *owner = find(root))
      return owner;
  return nullptr;
}

[[nodiscard]] core::NativeApplyPlanRequestDTO BuildNativeApplyPlanRequest(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const Composition &candidate, const Composition *predecessor,
    const std::span<const core::RuntimeParticipantBinding> representatives,
    [[maybe_unused]] const std::span<const Composition *const>
        other_predecessors = {}) {
  Require(candidate.bundle != nullptr &&
              candidate.component_runtime_identity() != nullptr &&
              candidate.active_addressed_kernel() != nullptr,
          "native Apply has no installed candidate or stable Kernel");
  const auto candidate_components = IndexNativeComponentCompositions(candidate);
  const auto predecessor_components =
      predecessor == nullptr
          ? std::map<core::ControllableComponentId, const Composition *>{}
          : IndexNativeComponentCompositions(*predecessor);
  core::NativeApplyPlanRequestDTO plan{
      {request.apply_id, request.apply_generation},
      candidate.bundle->retained_bundle_id(),
      candidate.bundle->value().cell_graph.graph_revision,
      {}, {}, {}, {}, {}, {}};
  // The immutable authored delta is echoed in the receipt. Runtime lifetime
  // follows the owner actually installed, including a restarted authored
  // Retain.
  for (const auto &[component_id, after] : candidate_components) {
    if (FindActualRetainedComponentOwner(component_id, *after, predecessor,
                                         other_predecessors))
      plan.retained_components.push_back(component_id);
  }
  for (const auto &[component_id, before] : predecessor_components) {
    static_cast<void>(before);
    if (!candidate_components.contains(component_id))
      plan.removed_components.push_back(component_id);
  }
  for (const auto *root : other_predecessors) {
    if (!root)
      continue;
    for (const auto &[component_id, before] :
         IndexNativeComponentCompositions(*root)) {
      static_cast<void>(before);
      if (!candidate_components.contains(component_id))
        plan.removed_components.push_back(component_id);
    }
  }
  std::sort(plan.removed_components.begin(), plan.removed_components.end());
  plan.removed_components.erase(std::unique(plan.removed_components.begin(),
                                            plan.removed_components.end()),
                                plan.removed_components.end());
  for (const auto &[component_id, _] : candidate_components)
    plan.components.push_back(component_id);
  const auto &desired_routes =
      candidate.bundle->value().desired_control_configurations;
  for (const auto &desired : desired_routes) {
    const auto physical = std::find_if(
        representatives.begin(), representatives.end(), [&](const auto &binding) {
          return binding.value().control_path.has_value() &&
                 binding.value().control_path->control_domain_id ==
                     desired.control_domain_id;
        });
    Require(physical != representatives.end(),
            "native candidate domain has no compiled physical binding");
    plan.compiled_routes.push_back(
        {desired.control_domain_id,
         physical->value()
             .control_path->active_control_configuration_generation});
    if (!std::binary_search(plan.retained_components.begin(),
                            plan.retained_components.end(),
                            physical->value().controllable_component_id))
      continue;
    const auto current = candidate.active_addressed_kernel()
                             ->current_runtime_route_configuration(
                                 desired.control_domain_id);
    Require(current.ok() && current.value().has_value() &&
                current.value()->operation_admission_open,
            "native retained domain has no current Kernel route");
    plan.retained_routes.push_back(current.value()->active_configuration);
  }
  if (predecessor != nullptr) {
    for (const auto &desired :
         predecessor->bundle->value().desired_control_configurations) {
      if (std::none_of(desired_routes.begin(), desired_routes.end(),
                       [&](const auto &row) {
                         return row.control_domain_id ==
                                desired.control_domain_id;
                       }))
        plan.removed_domains.push_back(desired.control_domain_id);
    }
  }
  return plan;
}

[[nodiscard]] std::vector<component_manager::ManagerRuntimeComponentReceiptDTO>
ReadNativeInstalledComponentReceipts(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const Composition *candidate, const Composition *predecessor,
    [[maybe_unused]] const std::span<const Composition *const>
        other_predecessors = {}) {
  const auto current = candidate == nullptr
                           ? std::map<core::ControllableComponentId,
                                      const Composition *>{}
                           : IndexNativeComponentCompositions(*candidate);
  const auto previous = predecessor == nullptr
                            ? std::map<core::ControllableComponentId,
                                       const Composition *>{}
                            : IndexNativeComponentCompositions(*predecessor);
  [[maybe_unused]] const bool product_changed =
      request.predecessor_projection_revision.retained_bundle_id !=
          request.candidate_projection_revision.retained_bundle_id ||
      request.predecessor_projection_revision.graph_revision !=
          request.candidate_projection_revision.graph_revision;
  std::vector<component_manager::ManagerRuntimeComponentReceiptDTO> receipts;
  for (const auto &row : request.component_delta.components) {
    if (row.disposition == deployment::ApplyComponentDisposition::kRemove)
      continue;
    const auto installed = current.find(row.controllable_component_id);
    Require(candidate != nullptr && installed != current.end(),
            "native installed component receipt has no live owner");
    const auto &component_id = installed->second->controllable_component_id;
    Require(component_id == row.controllable_component_id,
            "native installed composition has another component identity");
    const auto runtime_identity = deployment::derive_component_runtime_identity(
        *candidate->bundle, component_id);
    Require(runtime_identity.has_value() &&
                runtime_identity == row.candidate_runtime_identity,
            "native installed component differs from the candidate product");
    bool has_component_registration = false;
    for (const auto &registration :
         installed->second->component_registrations()) {
      const auto &binding = registration.binding.value();
      if (binding.controllable_component_id != component_id) {
        continue;
      }
      has_component_registration = true;
      Require(binding.retained_bundle_id ==
                      request.candidate_projection_revision.retained_bundle_id &&
                  binding.graph_revision ==
                      request.candidate_projection_revision.graph_revision,
              "native component registration is sealed to another product");
      Require(std::any_of(
                  binding.runtime_unit_epochs.begin(),
                  binding.runtime_unit_epochs.end(), [&](const auto &epoch) {
                    return epoch.runtime_unit_id == request.runtime_unit_id &&
                           epoch.runtime_epoch == request.runtime_epoch;
                  }),
              "native component registration has no current execution-host epoch");
    }
    const auto& actual_drivers = installed->second->io_provider_drivers;
    const bool input_only_driver_group = actual_drivers && !actual_drivers->empty() &&
        std::all_of(actual_drivers->begin(), actual_drivers->end(), [](const auto& driver) {
          return driver && driver->resources_installed() && driver->domain().resource_ids().empty();
        });
    Require(has_component_registration || input_only_driver_group,
            "native installed component has no runtime participant registrations");
    [[maybe_unused]] std::vector<core::RuntimeUnitEpochBindingDTO> epochs{
        {request.runtime_unit_id, request.runtime_epoch}};
    [[maybe_unused]] const auto predecessor_owner =
        previous.find(row.controllable_component_id);
    const auto *actual_predecessor = FindActualRetainedComponentOwner(
        component_id, *installed->second, predecessor, other_predecessors);
    const auto &view = *installed->second;
    if (view.io_provider_drivers) {
      for (const auto& driver : *view.io_provider_drivers) {
        Require(driver && driver->resources_installed() &&
                    driver->runtime_unit_epoch() == core::RuntimeUnitEpochBindingDTO{request.runtime_unit_id, request.runtime_epoch},
                "I/O installed receipt has no exact actual driver resources/host epoch");
      }
    }
    if (!view.runtime && !view.gripper_runtime) {
      Require(view.io_provider_drivers && view.component_runtime_pins &&
                  view.component_runtime_pins->matches({}, {}, view.io_provider_drivers),
              "I/O installed receipt lost its complete actual lifetime pin");
      component_manager::ManagerRuntimeComponentReceiptDTO receipt{
          component_id, row.disposition, *runtime_identity,
          view.host_runtime_unit_epochs(), actual_predecessor != nullptr,
          actual_predecessor != nullptr && product_changed};
      Require(receipt.valid(), "I/O component receipt is invalid");
      receipts.push_back(std::move(receipt));
      continue;
    }
    Require((view.runtime != nullptr || view.gripper_runtime != nullptr) &&
                view.ros2_control_runtime_handle != nullptr,
            "ROS installed receipt has no owned runtime handle");
    // Successful route installation can advance the live configuration
    // generation beyond the Composition's construction-time manual bindings.
    // Receipt ownership uses the current runtime bindings and registrations.
    const std::optional<core::RuntimeParticipantBinding> current_motion =
        view.runtime ? std::optional(view.runtime->motion_binding) : std::nullopt;
    const std::optional<core::RuntimeParticipantBinding> current_tool =
        view.runtime ? view.runtime->gripper_binding
                     : std::optional(view.gripper_runtime->binding());
    auto receipt = detail::ReadRos2ComponentReceipt(
        request, component_id, *runtime_identity,
        *view.ros2_control_runtime_handle,
        view.runtime ? view.runtime->execution.get() : nullptr,
        view.runtime ? view.runtime->gripper.get() : view.gripper_runtime->backend(),
        current_motion, current_tool,
        view.component_registrations(),
        view.runtime ? view.runtime->participant_engine_for(true) : nullptr,
        view.runtime ? view.runtime->participant_engine_for(false) : view.gripper_runtime->participant_engine(),
        actual_predecessor
            ? actual_predecessor->ros2_control_runtime_handle.get()
            : nullptr,
        RetainedControllerLaunchExit(view.controller_process));
    Require(receipt.valid(), "native component receipt is invalid");
    receipts.push_back(std::move(receipt));
  }
  Require(component_manager::ComponentReceiptsMatchRequest(request, receipts),
          "native installed component receipts differ from the Apply decision");
  return receipts;
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



[[nodiscard]] CanonicalValue
LaunchIncidentOutput(ControllerLaunchOutputSnapshot output) {
  const auto unsigned_value = [](const std::uint64_t value) {
    return value <= 9'007'199'254'740'991ULL
               ? CanonicalValue(static_cast<double>(value))
               : CanonicalValue(std::to_string(value));
  };
  std::uint64_t invalid_text_bytes{};
  const bool escaped_tail = !content_identity::is_utf8_nfc(output.tail);
  if (escaped_tail) {
    // Preserve otherwise unrepresentable bytes as reversible ASCII escapes.
    // Valid NFC UTF8 takes the unchanged path above.
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    for (const auto byte : output.tail) {
      if (static_cast<unsigned char>(byte) >= 0x80U) {
        const auto value = static_cast<unsigned char>(byte);
        escaped += "\\x";
        escaped += hex[value >> 4U];
        escaped += hex[value & 0x0fU];
        ++invalid_text_bytes;
      } else if (byte == '\\') {
        escaped += "\\\\";
      } else {
        escaped += byte;
      }
    }
    output.tail = std::move(escaped);
  }
  // Keep the most recent text. JSON escaping counts against the approved
  // encoded-byte cap; every trim boundary preserves a UTF8 codepoint or a
  // complete generated ASCII escape token.
  auto encoded = content_identity::canonical_json(CanonicalValue(output.tail));
  if (encoded.size() > 128U * 1024U) {
    const auto encoded_byte = [](const unsigned char byte) -> std::size_t {
      if (byte == '\b' || byte == '\t' || byte == '\n' || byte == '\f' ||
          byte == '\r' || byte == '"' || byte == '\\')
        return 2U;
      return byte < 0x20U ? 6U : 1U;
    };
    auto remaining = encoded.size();
    std::size_t removed{};
    while (remaining > 128U * 1024U && removed < output.tail.size()) {
      const auto token_bytes =
          escaped_tail && output.tail[removed] == '\\'
              ? (output.tail[removed + 1U] == 'x' ? 4U : 2U)
              : 1U;
      const auto token_end = removed + token_bytes;
      while (removed < token_end)
        remaining -=
            encoded_byte(static_cast<unsigned char>(output.tail[removed++]));
    }
    while (removed < output.tail.size() &&
           (static_cast<unsigned char>(output.tail[removed]) & 0xC0U) == 0x80U)
      ++removed;
    output.tail.erase(0U, removed);
    output.truncated = true;
  }
  const auto line_count =
      output.tail.empty() ? 0U
                          : static_cast<std::size_t>(std::count(
                                output.tail.begin(), output.tail.end(), '\n')) +
                                (output.tail.back() == '\n' ? 0U : 1U);
  return CanonicalValue(CanonicalValue::Object{
           {"tail", CanonicalValue(std::move(output.tail))},
           {"line_count", CanonicalValue(static_cast<double>(line_count))},
           {"truncated", CanonicalValue(output.truncated)},
           {"dropped_forward_lines",
            unsigned_value(output.dropped_forward_lines)},
           {"dropped_forward_bytes",
            unsigned_value(output.dropped_forward_bytes)},
           {"invalid_text_bytes", unsigned_value(invalid_text_bytes)}});
}

[[nodiscard]] std::shared_ptr<const CanonicalValue>
ControllerLaunchIncidentSnapshot(ControllerLaunchProcess &process,
                                 const ControllerLaunchExitEvidence &exit,
                                 const deployment::RetainedBundle &bundle,
                                 const std::string &runtime_id,
                                 const std::string &runtime_epoch) {
  const auto optional_text = [](const std::optional<std::string> &value) {
    return value ? CanonicalValue(*value) : CanonicalValue(nullptr);
  };
  const auto optional_bool = [](const std::optional<bool> value) {
    return value ? CanonicalValue(*value) : CanonicalValue(nullptr);
  };
  const auto optional_number = [](const std::optional<int> value) {
    return value ? CanonicalValue(static_cast<double>(*value))
                 : CanonicalValue(nullptr);
  };
  const auto unsigned_value = [](const std::uint64_t value) {
    return value <= 9'007'199'254'740'991ULL
               ? CanonicalValue(static_cast<double>(value))
               : CanonicalValue(std::to_string(value));
  };
  const auto &command = process.command();
  CanonicalValue::Array argv;
  for (const auto &argument : command.argv)
    argv.emplace_back(argument);
  const ControllerLaunchReadinessEvidence unknown;
  const auto &readiness = exit.readiness ? *exit.readiness : unknown;
  const auto configuration =
      deployment::ros2_control_execution_runtime_contract::
          component_configuration_projection(
              bundle, core::RuntimeUnitId(runtime_id),
              core::ControllableComponentId(exit.controllable_component_id));
  auto snapshot = std::make_shared<const CanonicalValue>(CanonicalValue::Object{
      {"exit", CanonicalValue(CanonicalValue::Object{
                   {"status", optional_number(exit.exit_status)},
                   {"signal", optional_number(exit.signal)},
                   {"core_dumped", CanonicalValue(exit.core_dumped)},
                   {"observed_utc", CanonicalValue(exit.observed_utc)}})},
      {"context",
       CanonicalValue(CanonicalValue::Object{
           {"controllable_component_id", CanonicalValue(exit.controllable_component_id)},
           {"request_context", CanonicalValue(exit.request_context)},
           {"runtime_unit_id", CanonicalValue(runtime_id)},
           {"runtime_epoch", CanonicalValue(runtime_epoch)},
           {"retained_bundle_id", CanonicalValue(bundle.retained_bundle_id().value())},
           {"graph_revision",
            CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
           {"generator_identity", CanonicalValue(nullptr)},
           {"launch_identity", CanonicalValue(exit.launch_identity)}})},
      {"launch_readiness",
       CanonicalValue(CanonicalValue::Object{
           {"known", optional_bool(readiness.known)},
           {"controller_manager_present",
            optional_bool(readiness.controller_manager_present)},
           {"controller_observed",
            optional_bool(readiness.controller_observed)},
           {"controller_loaded", optional_bool(readiness.controller_loaded)},
           {"controller_bound", optional_bool(readiness.controller_bound)},
           {"arm_controller_active",
            optional_bool(readiness.arm_controller_active)},
           {"hardware_ready", optional_bool(readiness.hardware_ready)},
           {"joint_sample_available",
            optional_bool(readiness.joint_sample_available)},
           {"binding_epoch", readiness.binding_epoch
                                 ? unsigned_value(*readiness.binding_epoch)
                                 : CanonicalValue(nullptr)},
           {"sample_age_ns", readiness.sample_age_ns
                                 ? content_identity::nonnegative_int64_value(
                                       *readiness.sample_age_ns)
                                 : CanonicalValue(nullptr)},
           {"captured_utc", readiness.captured_utc.empty()
                                ? CanonicalValue(nullptr)
                                : CanonicalValue(readiness.captured_utc)}})},
      {"settings",
       CanonicalValue(CanonicalValue::Object{
           {"argv", CanonicalValue(std::move(argv))},
           {"controller_profile", CanonicalValue(command.controller_profile)},
           {"runtime_namespace", CanonicalValue(command.runtime_namespace)},
           {"arm_controller_name", CanonicalValue(command.arm_controller_name)},
           {"gripper_controller_name",
            optional_text(command.gripper_controller_name)},
           {"frame_prefix", CanonicalValue(command.frame_prefix)},
           {"controllers_yaml", CanonicalValue(command.controllers_yaml)},
           {"retained_configuration_json",
            CanonicalValue(content_identity::canonical_json(configuration))}})},
      {"generated",
       CanonicalValue(CanonicalValue::Object{
           {"robot_description_sha256",
            CanonicalValue(exit.robot_description_sha256)},
           {"controllers_sha256", CanonicalValue(exit.controllers_sha256)}})},
      {"output", LaunchIncidentOutput(process.output_snapshot())},
      {"core_dump_link", CanonicalValue(nullptr)}});
  Require(core::ComponentIncidentDiagnosticSnapshotShapeIsValid(*snapshot),
          "controller launch incident snapshot exceeded its closed bound");
  return snapshot;
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


struct RetainedNativeComponentSnapshot final {
  std::shared_ptr<RetainedNativeRuntime> runtime;
  std::shared_ptr<RetainedNativeGripperRuntime> gripper_runtime;
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
};

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

struct CompositionBuildInput final {
  std::filesystem::path package_root;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  core::CellId cell_id;
  core::RequestOwnerEpoch owner_epoch;
  std::optional<core::RuntimeUnitId> runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedModelMap model_map;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  std::vector<core::RuntimeParticipantBinding> motion_program_aliases;
  std::vector<core::RuntimeParticipantBinding> gripper_program_aliases;
  ManagedKernelAdmissionGate *operation_admission_gate{};
  std::shared_ptr<const deployment::ProgramBindingContract> status_program;
  std::shared_ptr<const deployment::ProgramBinding> status_program_binding;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  bool attach_addressed_owner{true};
  std::shared_ptr<WallClock> shared_clock;
  std::shared_ptr<SimpleScheduler> shared_scheduler;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::optional<core::ControllableComponentId> selected_component_id{};
  std::shared_ptr<RetainedObservationWake> observation_wake{};
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      component_planner_scene_projections{};
  // Immutable Programs generated by Program Apply may live outside the
  // retained Deployment package. This is a read-only point-lookup source.
  std::filesystem::path program_artifact_root{};
  // Present only for a Manager-selected managed composition. Startup and
  // direct product builds have no Apply delta.
  std::optional<deployment::ApplyComponentDeltaDTO> managed_component_delta{};
  std::map<core::ControllableComponentId, RetainedNativeComponentSnapshot>
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
};

[[nodiscard]] core::CellAdmissionSettlementValidator
BuildOwnerSettlementValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  if (manager_apply_journal.empty() && logical_scene_database.empty() &&
      program_cycle_journal.empty() && lifecycle_journal.empty()) {
    return {};
  }
  return
      [manager_apply_journal = std::move(manager_apply_journal),
       logical_scene_database = std::move(logical_scene_database),
       program_cycle_journal = std::move(program_cycle_journal),
       lifecycle_journal = std::move(lifecycle_journal),
       cell_id = std::move(cell_id)](
          const core::CellAdmissionClaimDTO &claim,
          const core::CellAdmissionClaimReleaseDTO &release) {
        if (claim.intent.claim_kind ==
                core::CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
            claim.intent.owner_role ==
                core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_journal, cell_id, claim, release);
        }
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
        case core::CellAdmissionClaimKindDTO::kComponentLifecycle:
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_journal, cell_id, claim, release);
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
    std::filesystem::path program_cycle_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  return [manager_apply_journal = std::move(manager_apply_journal),
          program_cycle_journal = std::move(program_cycle_journal),
          lifecycle_journal = std::move(lifecycle_journal),
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
    case core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner:
      return core::ValidateCCLCCellAdmissionReconciliationReadOnly(
          lifecycle_journal, cell_id, held_claims, current_owner_epochs,
          reconcile);
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

  // A component-runtime value has exactly one lifecycle owner. Motion and
  // gripper targets may be independent components in the retained cell, but
  // they must be resolved through separate component-runtime invocations.
  if (motion_target.has_value() && gripper_target.has_value()) {
    Require(motion_target->component().controllable_component_id ==
                gripper_target->component().controllable_component_id,
            "native component binding combines capabilities from different owners");
  }
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
  bool real_vendor_lane{};
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
    if (predecessor.ros2_control_runtime_handle->HasToolContextSource()) {
      const std::weak_ptr<RetainedPlannerSceneProjection> weak = projection;
      source = std::make_shared<const RetainedToolContextSource>(
          documents.urdf,
          retained_tool_context_frames(input.bundle->value().cell_graph, input.model_map,
                                       model->model_source_urdfs(), component, motion.command_target_id),
          [weak] { const auto locked = weak.lock();
                   return locked ? locked->logical_snapshot() : nullptr; });
    }
    const bool real_vendor_lane = profile->real_profile.has_value() && static_cast<bool>(source);
    result.push_back({predecessor.ros2_control_runtime_handle,
                      DirectControlRobotTcps(*physical.motion_program),
                      physical.motion_program->program_binding_id(), std::move(source),
                      real_vendor_lane, std::move(projection)});
  }
  return result;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input);


[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
ResealRetainedNativeCandidateRegistrations(
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
    if (skip_provider_only && !view.runtime && !view.gripper_runtime &&
        view.io_provider_drivers)
      return;
    Require(view.component_runtime_identity() != nullptr,
            "native cell frame resolver has no component owner");
    if (view.runtime != nullptr) {
      include_channel(view.runtime->motion_state_channel);
      include_channel(view.runtime->gripper_state_channel);
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
    Require(!physical.motion.has_value() && physical.gripper.has_value() &&
                physical.gripper_program != nullptr &&
                retained_bindings_share_backend_owner(
                    predecessor->second.gripper_runtime->binding(),
                    *physical.gripper),
            "retained gripper changed its physical capability or controller");
    auto candidate_registrations =
        ResealRetainedNativeCandidateRegistrations(
            {predecessor->second.gripper_runtime->registration()},
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
              physical.status_motion_role.has_value() &&
              physical.gripper.has_value() ==
                  predecessor->second.runtime->gripper_binding.has_value(),
          "retained native controller changed its physical capability set");
  auto candidate_registrations =
      ResealRetainedNativeCandidateRegistrations(
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
    if (physical.gripper.has_value() &&
        physical.gripper->value().resources.front().source_joint_name
            .has_value()) {
      projected_bindings.push_back(*physical.gripper);
      projected_budgets.push_back(
          predecessor->second.runtime->gripper_state_channel->query_context()
              .freshness_budget_ns);
    }
    auto candidate_scene = BuildCandidateRetainedModelScene(
        "manyforge_i6_native_execution_host", *candidate_stage,
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
  if (predecessor->second.runtime->gripper_state_channel)
    add_state_source(predecessor->second.runtime->gripper_state_channel);
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
  if (physical.gripper_program != nullptr)
    direct_binding_ids.push_back(
        physical.gripper_program->program_binding_id());
  std::sort(direct_binding_ids.begin(), direct_binding_ids.end());
  std::vector<core::RuntimeParticipantBinding> physical_bindings{
      physical.motion.value()};
  if (physical.gripper.has_value())
    physical_bindings.push_back(*physical.gripper);
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
      std::move(physical.motion_program),
      std::move(physical.gripper_program), std::move(physical.motion.value()),
      std::move(physical.gripper), predecessor->second.joint_limits,
      predecessor->second.motion_descriptor,
      predecessor->second.gripper_descriptor, std::move(planning_stage),
      predecessor->second.runtime, std::move(input.projection_revision),
      input.publication_generation, std::move(*physical.status_motion_role),
      physical.status_gripper_role);
  Require(predecessor->second.ros2_control_runtime_handle != nullptr,
          "retained ROS component has no physical handle owner");
  composition->ros2_control_runtime_handle =
      predecessor->second.ros2_control_runtime_handle;
  composition->StageRobotTcpReader();
  composition->controller_process = predecessor->second.controller_process;
  composition->controller_request_context = input.controller_request_context;
  composition->candidate_planner_model = std::move(candidate_planner_model);
  composition->candidate_model_generation =
      std::move(candidate_model_generation);
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
  const auto configuration = deployment::ros2_control_execution_runtime_contract::require_configuration(
      *input.bundle, local);
  const auto* selected = deployment::ros2_control_execution_runtime_contract::find_controller_profile(
      configuration, value.controllable_component_id);
  const auto authored = std::find_if(configuration.gripper_target_configurations.begin(),
      configuration.gripper_target_configurations.end(), [&](const auto& row) {
        return row.controllable_component_id == value.controllable_component_id &&
               row.command_target_id == value.command_target_id;
      });
  Require(authored != configuration.gripper_target_configurations.end(),
          "ROS gripper-only component has no exact authored gripper configuration");
  const bool no_output = authored->output_mode == core::GripperOutputMode::kNone || authored->pneumatic_io;
  Require(!no_output || authored->actuation_kind == "pneumatic",
          "No output requires its strict pneumatic gripper target");
  // A no-output tool is lane-free (D4): it needs no controller profile.
  Require(no_output || (selected && (selected->mock_profile || selected->isaac_profile)),
          "ROS gripper-only component requires its selected Mock or Isaac controller profile");
  // Merge 6: an Isaac tool's joints stay in the robot's Isaac articulation;
  // the tool declares only its own joints, read from the shared joint states.
  const bool isaac = !no_output && selected->isaac_profile.has_value();
  std::optional<MockControllerProfileFacts> profile;
  if (!no_output && !isaac) {
    profile = mock_controller_profile_facts(*selected->mock_profile);
    Require(profile->gripper_required && profile->gripper_controller_name,
            "ROS gripper-only profile has no real gripper controller");
  }
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
  const auto instance = std::find_if(input.bundle->value().cell_graph.model_instances.begin(),
      input.bundle->value().cell_graph.model_instances.end(), [&](const auto& row) {
        return row.model_instance_id == value.model_groups.front().model_instance_id;
      });
  Require(instance != input.bundle->value().cell_graph.model_instances.end(),
          "ROS gripper-only source model instance is missing");
  const auto source = std::find_if(model->model_source_urdfs().begin(), model->model_source_urdfs().end(),
      [&](const auto& row) { return row.model_asset_id == instance->model_asset_id &&
                                  row.model_revision == instance->model_revision; });
  Require(source != model->model_source_urdfs().end(), "ROS gripper-only source URDF is missing");
  // A tool with its own launch publishes its static frames there; a no-output
  // tool has no process, and its parent's launch publishes them.
  const auto description = retained_component_description(
      source->urdf_bytes, input.bundle->value().cell_graph, input.model_map,
      model->model_source_urdfs(), configuration, value.controllable_component_id);
  struct UnadoptedLaunch final {
    const ControllerLaunchSupervisor::Ensured* receipt{};
    ~UnadoptedLaunch() {
      if (receipt) static_cast<void>(host_controller_launch_supervisor().discard_candidate(*receipt));
    }
  };
  std::optional<ControllerLaunchSupervisor::Ensured> launch;
  UnadoptedLaunch owned_launch{};
  if (!no_output) {
    const auto artifacts_root = input.execution_run_root / "controller-launch-artifacts" /
        value.controllable_component_id.value();
    const auto command = [&] {
      if (isaac) {
        // The tool's launch: its description and the profile's gripper bridge
        // (an action transport), fed its own gripper_feedback budget.
        const auto bridge = retained_isaac_gripper_bridge(*selected->isaac_profile);
        return isaac_gripper_launch_command(
            write_isaac_gripper_launch_artifacts(artifacts_root,
                value.controllable_component_id.value(), *selected->isaac_profile,
                description, selected->frame_prefix),
            bridge, bridge ? std::optional<double>(static_cast<double>(retained_component_stream(
                configuration, value.controllable_component_id,
                deployment::ros2_control_execution_runtime_contract::kGripperFeedbackStreamId)
                .budget_ns) / 1e9) : std::nullopt);
      }
      ComponentControllerLaunchInput artifacts;
      artifacts.artifacts_root = artifacts_root;
      artifacts.model_urdf = description;
      artifacts.gripper_binding = binding;
      artifacts.gripper_projected_position = initial;
      artifacts.mock_profile = selected->mock_profile;
      artifacts.frame_prefix = selected->frame_prefix;
      return mock_controller_launch_command(write_component_controller_launch_artifacts(
          std::optional<core::RuntimeParticipantBinding>{}, artifacts));
    }();
    const auto acquired = host_controller_launch_supervisor().ensure(command,
        artifacts_root / value.controllable_component_id.value(),
        input.controller_request_context.empty() ? "Bootstrap runtime=" + local.value() +
        " epoch=" + input.runtime_epoch.value() : input.controller_request_context);
    UnadoptedLaunch acquired_guard{&acquired};
    launch = acquired; // the local receipt remains guarded if this copy throws
    owned_launch.receipt = &*launch;
    acquired_guard.receipt = nullptr;
  }
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  if (launch) handle->InstallControllerProcessFaultSource(
      ControllerLaunchSupervisor::fault_source(launch->process, CachedControllerLaunchReadinessSource(handle)),
      [process = launch->process] { return core::Result<std::optional<std::string>>::Ok(
          ProjectControllerLaunchFault(process->ReadCachedFaultState())); });
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_ros2_control_execution_backend";
  config.node_namespace = controller_runtime_namespace(value.controllable_component_id.value());
  config.executor_thread_count = 3U;
  if (no_output) {
    ros2_control_execution::Ros2GripperActionConfig local;
    local.group_id = value.model_groups.front().compiled_group_name;
    local.joint_name = value.resources.front().compiled_resource_name;
    local.state_joint_name = *value.resources.front().source_joint_name;
    config.gripper = std::move(local);
  } else if (isaac) {
    configure_retained_isaac_gripper_action(config, binding, *selected->isaac_profile,
        config.node_namespace);
  } else {
    configure_retained_mock_gripper_action(config, binding, *profile,
        config.node_namespace, config.node_namespace + "/controller_manager");
  }
  configure_retained_gripper_output_mode(config, configuration, binding);
  // CP9: the gripper-only component's own `measured_joints` stream row; an
  // Isaac tool reads its joints from the shared Isaac joint states. A
  // lane-free tool (D4) declares no streams and has no measured lane.
  const auto measured_lane = selected == nullptr
      ? std::optional<RetainedMeasuredJointStateLane>{}
      : std::optional(retained_measured_joint_state_lane(configuration,
            value.controllable_component_id, isaac
                ? retained_isaac_controller_lane(*selected->isaac_profile, {}).joint_states_topic
                : controller_runtime_namespace(value.controllable_component_id.value()) + "/joint_states"));
  config.arm.measured_joint_state_lane = measured_lane;
  config.defer_ros_start = input.managed_component_delta.has_value();
  config.measured_receiver_clock = input.shared_clock;
  config.ros_context_provider = input.ros_context_provider;
  config.arm.adapter.motion_trace = input.motion_trace;
  auto backend = make_retained_ros2_control_gripper_backend(std::move(config), binding, channel,
      *input.shared_clock, *input.shared_scheduler, handle);
  RetainedNativeGripperRuntimeInput gripper_input{
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
  auto owner = std::make_shared<RetainedNativeGripperRuntime>(std::move(gripper_input));
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
  composition->controller_process = launch ? launch->process : nullptr;
  composition->controller_request_context = input.controller_request_context;
  composition->prepared_controller_launch = launch;
  owned_launch.receipt = nullptr;
  composition->target_resolver = std::move(resolver);
  composition->candidate_participant_resolver_factory =
      MakeCandidateParticipantResolverFactory(make_resolver);
  composition->scene_frame_resolver = std::move(frames);
  // As the retained path: a mounted tool shares the cell's planner projection.
  composition->planner_scene_projection = std::move(input.planner_scene_projection);
  composition->candidate_control_representatives = std::move(physical.representatives);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  return composition;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildSingleProductComposition(CompositionBuildInput input) {
  if (input.selected_component_id.has_value() &&
      input.retained_component_snapshots.contains(*input.selected_component_id))
    return BuildRetainedProductComposition(std::move(input));
  if (!input.attach_addressed_owner &&
      (input.shared_clock == nullptr) != (input.shared_scheduler == nullptr)) {
    throw std::invalid_argument("native runtime clock and scheduler "
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
  auto direct_control_gripper_program_binding =
      std::move(physical_bindings.gripper_program);
  auto direct_control_motion_binding = std::move(physical_bindings.motion);
  auto direct_control_gripper_binding = std::move(physical_bindings.gripper);
  auto control_binding_representatives =
      std::move(physical_bindings.representatives);
  input.status_motion_target_role_id =
      std::move(*physical_bindings.status_motion_role);
  input.status_gripper_target_role_id =
      physical_bindings.status_gripper_role;
  auto local_runtime_unit_id = input.runtime_unit_id;
  if (!local_runtime_unit_id.has_value()) {
    const auto &runtime_units =
        direct_control_motion_binding.value().value().runtime_unit_epochs;
    Require(runtime_units.size() == 1U,
            "standalone native execution cannot infer one local runtime "
            "unit");
    local_runtime_unit_id = runtime_units.front().runtime_unit_id;
  }
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
  Require(contains_local_runtime(direct_control_motion_binding.value()) &&
              (!direct_control_gripper_binding.has_value() ||
               contains_local_runtime(*direct_control_gripper_binding)),
          "native execution local runtime is absent from its exact "
          "participant bindings");
  const auto retained_runtime_configuration =
      deployment::ros2_control_execution_runtime_contract::
          require_configuration(*input.bundle, *local_runtime_unit_id);
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
  constexpr bool whole_runtime_motion_limits = true;
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
  // Spec 430 §1.2: under `active_tcp_and_flange` the maximum also holds at
  // the robot flange, the motion target's `flange` frame (D1), never a
  // Program role. A robot without one cannot run under that setting.
  std::optional<core::ResolvedFrameDTO> speed_check_flange_frame;
  if (motion_limit->tcp_speed_check ==
      core::TcpSpeedCheck::kActiveTcpAndFlange) {
    const auto &motion = direct_control_motion_binding.value().value();
    const auto *target = input.model_map.find_target(
        motion.controllable_component_id, motion.command_target_id);
    Require(target != nullptr, "execution Deployment has no exact motion target");
    for (const auto &exposed : target->frames) {
      const auto *frame =
          std::get_if<deployment::CompositionComponentModelFrame>(
              &exposed.frame);
      if (frame != nullptr && exposed.frame_role_id.value() == "flange")
        speed_check_flange_frame = core::ResolvedComponentModelFrameDTO{
            frame->controllable_component_id, frame->model_instance_id,
            exposed.frame_role_id, frame->compiled_frame_name,
            frame->model_revision};
    }
    Require(speed_check_flange_frame.has_value(),
            "tcp_speed_check active_tcp_and_flange requires a flange frame "
            "on the robot motion target");
  }
  auto selected_initial_joint_positions =
      retained_runtime_configuration.initial_joint_positions_rad;
  const auto selected_resource_count =
      direct_control_motion_binding.value().value().resources.size();
  if (selected_initial_joint_positions.size() != selected_resource_count) {
    struct InitialPositionSlice final {
      core::ControllableComponentId component_id;
      std::size_t count{};
    };
    std::vector<InitialPositionSlice> slices;
    for (const auto &binding : direct_control_inventory.motion) {
      const auto target = deployment::resolve_runtime_program_target(
          *binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId("execution.trajectory.execute"), *input.bundle,
          input.model_map);
      const auto verified = core::verify_runtime_participant_binding(
          deployment::build_runtime_participant_binding_dto(
              target, ActiveFacts(target, {}, input.runtime_epoch,
                                  core::ActiveControlConfigurationId(
                                      "active.i6.native.arm"))));
      Require(verified.ok(), "native initial-state target is invalid");
      slices.push_back({target.component().controllable_component_id,
                        verified.value().value().resources.size()});
    }
    std::sort(slices.begin(), slices.end(),
              [](const auto &left, const auto &right) {
                return left.component_id < right.component_id;
              });
    const auto total = std::accumulate(
        slices.begin(), slices.end(), std::size_t{0},
        [](const auto sum, const auto &slice) { return sum + slice.count; });
    Require(total == selected_initial_joint_positions.size() &&
                input.selected_component_id.has_value(),
            "native execution initial state differs from the cell motion "
            "resource order");
    std::size_t offset = 0U;
    for (const auto &slice : slices) {
      if (slice.component_id == *input.selected_component_id) {
        selected_initial_joint_positions = std::vector<double>(
            retained_runtime_configuration.initial_joint_positions_rad.begin() +
                static_cast<std::ptrdiff_t>(offset),
            retained_runtime_configuration.initial_joint_positions_rad.begin() +
                static_cast<std::ptrdiff_t>(offset + slice.count));
        break;
      }
      offset += slice.count;
    }
    Require(selected_initial_joint_positions.size() == selected_resource_count,
            "native execution initial-state slice is unavailable");
  }
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
      "manyforge_i6_native_execution_host",
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
  if (direct_control_gripper_binding.has_value() &&
      direct_control_gripper_binding->value()
          .resources.front()
          .source_joint_name.has_value()) {
    projected_state_bindings.push_back(*direct_control_gripper_binding);
    projected_state_budgets.push_back(
        retained_ros2_control_gripper_state_config(
            *direct_control_gripper_binding)
            .freshness_budget_ns);
  }
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
        "manyforge_i6_native_execution_host", stage.loader_urdf_path(),
        stage.srdf_path(), stage.package_paths(), group.compiled_group_name,
        input.scene_asset_root_fd);
    auto collision_projector = RetainedCollisionWorldProjector::create_verified(
        collision_model, *input.bundle, input.model_map, *retained_model);
    const auto logical_scene =
        EmptyScene(input.bundle->value().cell_graph.cell_id,
                   input.projection_revision.scene_revision);
    auto publisher_result = RetainedCollisionWorldPublisher::create(
        std::move(collision_projector), "i6-native-execution-host",
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
          RoboPlanCandidateComputerConfig{
              .rng_seed = retained_runtime_configuration.planner_rng_seed,
              // The same step validates the executed curve (kernel config).
              .collision_check_step_rad =
                  retained_runtime_configuration.collision_check_step_rad.value_or(
                      core::kDefaultCollisionCheckStepRad)},
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
  if (direct_control_gripper_program_binding != nullptr) {
    intrinsic_direct_bindings.push_back(
        direct_control_gripper_program_binding->program_binding_id());
  }
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
  std::vector<core::RuntimeParticipantBinding> target_physical_bindings{
      direct_control_motion_binding.value()};
  if (direct_control_gripper_binding.has_value())
    target_physical_bindings.push_back(*direct_control_gripper_binding);
  auto target_resolver =
      make_participant_resolver(std::move(target_physical_bindings));
  RetainedRuntimeParticipantResolver participant_resolver =
      MakeRetainedRuntimeParticipantResolver(target_resolver);
  const auto reference_collision_model = collision_model;
  const auto reference_runtime_unit_id = *local_runtime_unit_id;
  const auto reference_runtime_epoch = input.runtime_epoch;
  const auto reference_component_id = input.selected_component_id;
  RetainedNativeRuntimeInput runtime_input{
      direct_control_motion_binding.value(),
      direct_control_gripper_binding,
      {},
      {},
      std::move(input.motion_program_aliases),
      std::move(input.gripper_program_aliases),
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
      input.operation_admission_gate,
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
  runtime_input.speed_check_flange_frame = speed_check_flange_frame;
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
  // Q63-Q69: this component's generated controller robot_description and
  // controllers file, written to a per-activation, per-component directory
  // of the host run root before the lane is composed; their hashes and the
  // exact launch contract are activation evidence.
  // Q78/§7.5: the mock lane's controller launch is then started (or kept,
  // Q21) by this Manager-owned host before the lane's backend binds, so CCLC
  // finds the arm controller loaded inactive and activates it as before.
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  const auto discard_launch = [](ControllerLaunchSupervisor::Ensured *launch) {
    if (launch != nullptr) {
      static_cast<void>(
          host_controller_launch_supervisor().discard_candidate(*launch));
      delete launch;
    }
  };
  std::unique_ptr<ControllerLaunchSupervisor::Ensured, decltype(discard_launch)>
      prepared_launch(nullptr, discard_launch);
  std::map<std::string, double, std::less<>> mock_cancel_by_source_joint;
  bool mock_lane = false;
  bool real_lane = false;
  // R7 package 3: a Real component's vendor adapter (Mock's start-up path).
  std::shared_ptr<IRetainedDeviceStatusAdapter> device_status_adapter;
  std::shared_ptr<const RetainedToolContextSource> tool_context_source;
  std::optional<MockResetPositionTarget> mock_reset_position_target;
  // M6b/Active TCP: the Deployment's "Send TCP offset to controller".
  bool controller_tcp_offset_sync = true;
  {
    const auto &motion_value = direct_control_motion_binding.value().value();
    const auto *selected_profile =
        deployment::ros2_control_execution_runtime_contract::
            find_controller_profile(retained_runtime_configuration,
                                    motion_value.controllable_component_id);
    Require(selected_profile != nullptr,
            "ROS component has no controller profile in its runtime unit");
    controller_tcp_offset_sync = selected_profile->controller_tcp_offset_sync;
    Require(motion_value.model_groups.size() == 1U,
            "ROS component has no sole model group");
    const auto &model_instance_id =
        motion_value.model_groups.front().model_instance_id;
    const auto &model_instances =
        input.bundle->value().cell_graph.model_instances;
    const auto model_instance = std::find_if(
        model_instances.begin(), model_instances.end(),
        [&](const auto &candidate) {
          return candidate.model_instance_id == model_instance_id;
        });
    Require(model_instance != model_instances.end(),
            "ROS component model instance is absent from the retained graph");
    const auto &source_urdfs = retained_model->model_source_urdfs();
    const auto source_urdf = std::find_if(
        source_urdfs.begin(), source_urdfs.end(), [&](const auto &candidate) {
          return candidate.model_asset_id == model_instance->model_asset_id &&
                 candidate.model_revision == model_instance->model_revision;
        });
    Require(source_urdf != source_urdfs.end(),
            "ROS component model source URDF is absent from the retained "
            "planning closure");
    const auto path_token = [](std::string text) {
      for (auto &character : text) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '-' || character == '_' ||
                          character == '.';
        if (!safe) character = '_';
      }
      return text;
    };
    ComponentControllerLaunchInput artifact_input;
    artifact_input.component_runtime_identity =
        deployment::derive_component_runtime_identity(*input.bundle, motion_value.controllable_component_id).value().value();
    artifact_input.artifacts_root =
        input.execution_run_root / "controller-launch-artifacts" /
        path_token(input.bundle->retained_bundle_id().value()) /
        path_token(input.runtime_epoch.value());
    artifact_input.gripper_binding = direct_control_gripper_binding;
    artifact_input.isaac_profile = selected_profile->isaac_profile;
    artifact_input.mock_profile = selected_profile->mock_profile;
    if (selected_profile->real_profile.has_value()) {
      // CP8 step 1 (temporary selector until CP8 step 3): the robot's
      // address is a site fact, resolved here; never a fallback.
      real_lane = true;
      Require(selected_profile->device_address_ref.has_value(),
              "site_device_address_ref_unknown: real component " +
                  motion_value.controllable_component_id.value() +
                  " has no device_address_ref");
      const auto address = component_manager::ResolveSiteDeviceAddress(
          input.site_settings, *selected_profile->device_address_ref);
      Require(address.ok(),
              address.ok() ? std::string{}
                           : address.error().code + ": real component " +
                                 motion_value.controllable_component_id.value() +
                                 ": " + address.error().message);
      artifact_input.real_profile = selected_profile->real_profile;
      artifact_input.robot_ip = address.value();
      // V-B4: the robot's vendor adapter family (compiled with real_profile).
      artifact_input.adapter_family = selected_profile->adapter_family.value_or("");
      // M6b: the deployment's controller TCP offset sync (default on).
      artifact_input.controller_tcp_offset_sync =
          selected_profile->controller_tcp_offset_sync;
      // The robot reports its own pose: no deployment initial pose seeds it.
      selected_initial_joint_positions.clear();
    }
    if (selected_profile->isaac_profile.has_value()) {
      artifact_input.isaac_lane =
          retained_isaac_controller_lane(*selected_profile->isaac_profile,
                                         selected_initial_joint_positions);
    } else {
      artifact_input.initial_joint_positions_rad =
          selected_initial_joint_positions;
      const deployment::FilesystemVerifiedRetainedProductArtifactSource
          stop_artifacts(input.package_root);
      artifact_input.controlled_stop_max_deceleration_rad_s2 =
          deployment::retained_controlled_stop_max_deceleration_rad_s2(
              *input.bundle, stop_artifacts,
              direct_control_motion_binding.value());
      if (direct_control_gripper_binding.has_value() &&
          direct_control_gripper_binding->value()
              .resources.front()
              .source_joint_name.has_value()) {
        // As `main`'s native lane seeds its gripper (retained model
        // projection of the gripper binding's source joint).
        const auto &gripper_path =
            *direct_control_gripper_binding->value().control_path;
        const auto projected = state_projector->current_state(
            gripper_path.control_binding_id,
            gripper_path.control_binding_revision);
        Require(projected.ok() && projected.value().positions.size() == 1U,
                "ROS mock gripper has no retained model projection");
        const auto &position = projected.value().positions.front();
        artifact_input.gripper_projected_position =
            std::holds_alternative<core::AngularPositionDTO>(position)
                ? std::get<core::AngularPositionDTO>(position).value_rad
                : std::get<core::LinearPositionDTO>(position).value_m;
      }
    }
    if (direct_control_gripper_binding) {
      const auto row = std::find_if(retained_runtime_configuration.gripper_target_configurations.begin(),
          retained_runtime_configuration.gripper_target_configurations.end(), [&](const auto& configured) {
            return configured.controllable_component_id == direct_control_gripper_binding->value().controllable_component_id &&
                   configured.command_target_id == direct_control_gripper_binding->value().command_target_id;
          });
      Require(row != retained_runtime_configuration.gripper_target_configurations.end(),
              "ROS controller artifacts have no exact authored gripper output choice");
      artifact_input.gripper_output_mode = row->pneumatic_io
          ? core::GripperOutputMode::kNone : row->output_mode;
      artifact_input.gripper_actuation_kind =
          retained_ros2_control_gripper_actuation_kind(
              retained_runtime_configuration, *direct_control_gripper_binding);
    }
    // The description also carries the static frames the model URDF lacks
    // (offset frames, custom TCPs, mounted tools), so robot_state_publisher
    // publishes them on /tf_static.
    artifact_input.model_urdf = retained_component_description(
        source_urdf->urdf_bytes, input.bundle->value().cell_graph,
        input.model_map, source_urdfs, retained_runtime_configuration,
        motion_value.controllable_component_id);
    artifact_input.frame_prefix = selected_profile->frame_prefix;
    const auto written =
        write_component_controller_launch_artifacts(
            direct_control_motion_binding.value(), artifact_input);
    const auto evidence = controller_launch_artifacts_evidence(written);
    {
      std::ofstream record(written.directory /
                               "controller_launch_artifacts.json",
                           std::ios::binary | std::ios::trunc);
      record << evidence << '\n';
      Require(static_cast<bool>(record),
              "ROS controller launch evidence could not be written");
    }
    std::cerr << "manyforge controller launch artifacts: " << evidence
              << std::endl;
    if (written.lane == ControllerLaunchLane::kMock) {
      mock_lane = true;
      mock_reset_position_target = written.mock_reset_position;
      if (written.vendor_adapter.has_value()) {
        device_status_adapter =
            make_retained_vendor_device_status_adapter(*written.vendor_adapter);
        // TOOLCTX: the robot's TCP and payload, computed here and applied by
        // its vendor adapter at Enable and before each new motion.
        const std::weak_ptr scene_projection{input.planner_scene_projection};
        tool_context_source = std::make_shared<const RetainedToolContextSource>(
            retained_model_documents.urdf,
            retained_tool_context_frames(
                input.bundle->value().cell_graph, input.model_map, source_urdfs,
                motion_value.controllable_component_id,
                motion_value.command_target_id),
            [scene_projection] {
              const auto projection = scene_projection.lock();
              return projection ? projection->logical_snapshot() : nullptr;
            });
      }
      // Q48: the same per-joint cancel bounds the generated controllers file
      // carries, keyed by the binding's own resource-to-source-joint map.
      for (const auto &[resource_id, limit] :
           artifact_input.controlled_stop_max_deceleration_rad_s2) {
        const auto &resources = motion_value.resources;
        const auto resource = std::find_if(
            resources.begin(), resources.end(), [&](const auto &candidate) {
              return candidate.resource_id == resource_id;
            });
        Require(resource != resources.end() &&
                    resource->source_joint_name.has_value(),
                "ROS mock Stop limit names a resource outside its arm");
        mock_cancel_by_source_joint.emplace(*resource->source_joint_name,
                                            limit);
      }
      const auto command = mock_controller_launch_command(written);
      const auto ensured = host_controller_launch_supervisor().ensure(
          command, written.directory,
          input.controller_request_context.empty()
              ? "Bootstrap runtime=" +
                    (input.runtime_unit_id ? input.runtime_unit_id->value()
                                           : std::string("unmanaged")) +
                    " epoch=" + input.runtime_epoch.value()
              : input.controller_request_context);
      prepared_launch.reset(new ControllerLaunchSupervisor::Ensured(ensured));
      controller_process = ensured.process;
      std::cerr << "manyforge controller launch: component="
                << written.controllable_component_id << " decision="
                << (ensured.decision == ControllerLaunchDecision::kKeep
                        ? "kept"
                        : ensured.decision == ControllerLaunchDecision::kStart
                              ? "started"
                              : "restarted")
                << " pid=" << ensured.process->pid() << std::endl;
    } else {
      Require(artifact_input.isaac_lane.has_value(),
              "managed Isaac component has no retained controller lane");
      const auto command = isaac_controller_launch_command(
          written, *artifact_input.isaac_lane);
      const auto ensured = host_controller_launch_supervisor().ensure(
          command, written.directory,
          input.controller_request_context.empty()
              ? "Bootstrap runtime=" +
                    (input.runtime_unit_id ? input.runtime_unit_id->value()
                                           : std::string("unmanaged")) +
                    " epoch=" + input.runtime_epoch.value()
              : input.controller_request_context);
      prepared_launch.reset(new ControllerLaunchSupervisor::Ensured(ensured));
      controller_process = ensured.process;
      std::cerr << "manyforge controller launch: component="
                << written.controllable_component_id << " decision="
                << (ensured.decision == ControllerLaunchDecision::kKeep
                        ? "kept"
                        : ensured.decision == ControllerLaunchDecision::kStart
                              ? "started"
                              : "restarted")
                << " pid=" << ensured.process->pid() << std::endl;
    }
  }
  auto ros2_control_runtime_handle =
      RetainedRos2ControlRuntimeHandle::Create();
  // Active TCP (M7, design 2): the robot's TCP list and the checkbox, on
  // every lane; unknown at this start, so the End-effector TCP (T-MQ3).
  ros2_control_runtime_handle->InstallActiveTcp(
      direct_control_motion_binding.value().value().controllable_component_id,
      DirectControlRobotTcps(*direct_control_binding), controller_tcp_offset_sync,
      direct_control_binding->program_binding_id());
  if (device_status_adapter)
    ros2_control_runtime_handle->InstallDeviceStatusAdapter(
        std::move(device_status_adapter));
  if (tool_context_source) {
    ros2_control_runtime_handle->InstallToolContextSource(
        [tool_context_source](const std::optional<std::string> &tcp_frame) {
          return tool_context_source->Compose(tcp_frame);
        });
    ros2_control_runtime_handle->InstallCommittedToolContextSource(
        [tool_context_source](const std::optional<std::string> &,
                              const core::LogicalSceneSnapshotDTO &scene) {
          return tool_context_source->ComposePayload(scene);
        });
    ros2_control_runtime_handle->InstallCommittedEnableToolContextSource(
        [tool_context_source](const std::optional<std::string> &frame,
                              const core::LogicalSceneSnapshotDTO &scene) {
          return tool_context_source->Compose(frame, scene);
        });
  }
  if (input.planner_scene_projection) {
    const std::weak_ptr scene_projection{input.planner_scene_projection};
    ros2_control_runtime_handle->InstallCommittedSceneSource(
        [scene_projection] {
          const auto projection = scene_projection.lock();
          return projection ? projection->logical_snapshot() : nullptr;
        });
  }
  if (controller_process != nullptr) {
    ros2_control_runtime_handle->InstallControllerProcessFaultSource(
        ControllerLaunchSupervisor::fault_source(
            controller_process, CachedControllerLaunchReadinessSource(
                                    ros2_control_runtime_handle)),
        [controller_process] {
          return core::Result<std::optional<std::string>>::Ok(
              ProjectControllerLaunchFault(
                  controller_process->ReadCachedFaultState()));
        });
  }
  // CP9: the same Deployment stream overrides the registry receives
  // (ApplyComponentStreamOverrides) set the in-motion staleness check.
  const auto stream_overrides =
      deployment::ComponentStreamParameterOverrides(*input.bundle);
  auto ros2_control_backend_config =
      real_lane
          ? make_retained_real_ros2_control_backend_config(
                retained_runtime_configuration,
                direct_control_motion_binding.value(),
                direct_control_gripper_binding, mock_cancel_by_source_joint,
                stream_overrides)
      : mock_lane
          ? make_retained_mock_ros2_control_backend_config(
                retained_runtime_configuration,
                direct_control_motion_binding.value(),
                direct_control_gripper_binding, mock_cancel_by_source_joint,
                stream_overrides)
          : make_retained_isaac_ros2_control_backend_config(
                retained_runtime_configuration,
                direct_control_motion_binding.value(),
                direct_control_gripper_binding, stream_overrides);
  ros2_control_backend_config.arm.mock_reset_position = std::move(mock_reset_position_target);
  // The addressed wrapper historically creates its own time capabilities.
  // A subordinate runtime retains the exact supplied pair, or creates both.
  const auto measured_receiver_clock = !input.attach_addressed_owner &&
                                               input.shared_clock != nullptr
                                           ? input.shared_clock
                                           : std::make_shared<WallClock>();
  ros2_control_backend_config.measured_receiver_clock = measured_receiver_clock;
  ros2_control_backend_config.ros_context_provider = input.ros_context_provider;
  ros2_control_backend_config.arm.adapter.motion_trace = input.motion_trace;
  const auto measured_lane =
      ros2_control_backend_config.arm.measured_joint_state_lane;
  const bool pneumatic_gripper = ros2_control_backend_config.gripper &&
      ros2_control_backend_config.gripper->actuation_kind == core::GripperActuationKind::kPneumatic;
  // This runtime composes commanded arm state only, so its measured source is
  // also the viewer's render-only measured comparison and wakes that stream.
  runtime_input.prepare_measured_sources =
      [ros2_control_runtime_handle, measured_receiver_clock, measured_lane, pneumatic_gripper,
       render_wake = runtime_input.observation_wake](
          const core::RuntimeParticipantBinding &motion,
          const std::optional<core::RuntimeParticipantBinding> &gripper,
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
    if (gripper && !pneumatic_gripper) {
      added = add_binding(*gripper);
      if (!added.ok()) return Result::Err(added.error());
    }
    if (source_configuration_absent) return Result::Ok(nullptr);
    auto staged = ros2_control_runtime_handle->StageMeasuredStateSources(
        descriptors, measured_receiver_clock, motion, gripper, render_wake);
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
  ros2_control_backend_config.defer_ros_start = input.managed_component_delta.has_value();
  runtime_input.motion_state_channel_config =
      retained_ros2_control_motion_state_config(ros2_control_backend_config);
  if (direct_control_gripper_binding.has_value()) {
    runtime_input.gripper_state_channel_config =
        retained_ros2_control_gripper_state_config(
            *direct_control_gripper_binding);
  }
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
  if (retained_runtime_configuration.collision_clearance_m.has_value()) {
    kernel_config.collision_clearance_m =
        *retained_runtime_configuration.collision_clearance_m;
  }
  if (retained_runtime_configuration.collision_check_step_rad.has_value()) {
    kernel_config.collision_check_step_rad =
        *retained_runtime_configuration.collision_check_step_rad;
  }
  RetainedNativeRuntimeConfig runtime_config{
      input.owner_epoch,
      input.cell_id,
      input.execution_state_root / std::string(core::kOperationJournalFilename),
      1024U,
      core::CacheConfig{},
      std::move(kernel_config),
      SimpleSchedulerConfig::from_environment(),
      execution_tick_period_ns,
      std::move(selected_initial_joint_positions),
      kMaintainedNativeStopSurveillancePolicy,
      {},
      {},
      {},
      {},
      {}};
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
  const std::vector<core::RequiredManagerLivenessReceiverDTO> fixture_receivers;
  runtime_config.manager_liveness_required_set =
      core::ManagerLivenessRequiredSetDTO{
          input.cell_id,
          input.bundle->retained_bundle_id(),
          core::ComputeRequiredRuntimeSetDigest(fixture_receivers),
          core::RuntimeUnitId("standalone-admission-fixture.manager"),
          core::RuntimeEpoch("standalone-admission-fixture.manager-epoch"),
          core::RequestOwnerEpoch("standalone-admission-fixture.manager-owner"),
          fixture_receivers};
#endif
  const auto manager_apply_journal =
      input.manager_state_root /
      std::string(component_manager::kManagerApplyJournalFilename);
  runtime_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          manager_apply_journal, input.logical_scene_database,
          input.program_cycle_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  runtime_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          manager_apply_journal, input.program_cycle_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  runtime_config.cell_admission_program_reset_status_source =
      [path = input.program_cycle_journal](const auto purpose, const auto& intent, const auto& claim) {
        return detail::ReadProgramResetStatusReadOnly(path, purpose, intent, claim);
      };
  auto receiver_scheduler = !input.attach_addressed_owner &&
                                    input.shared_scheduler != nullptr
                                ? input.shared_scheduler
                                : std::make_shared<SimpleScheduler>(runtime_config.scheduler);
  const auto runtime_collision_immediate_query_budget_ns =
      runtime_config.kernel.runtime_collision_immediate_query_budget_ns;
  auto runtime = input.attach_addressed_owner
                     ? create_retained_native_runtime(
                           std::move(runtime_input), std::move(runtime_config),
                           measured_receiver_clock, std::move(receiver_scheduler))
                     : create_retained_native_runtime_participants(
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
  if (input.attach_addressed_owner &&
      input.operation_admission_gate != nullptr) {
    input.operation_admission_gate->BindResourceTable(
        runtime->addressed_kernel->resource_admission_table());
  }
  auto direct_control_joint_limits = runtime->kinematic_model->joint_limits(
      direct_control_motion_binding.value()
          .value()
          .controllable_component_id.value(),
      direct_control_motion_binding.value().value().command_target_id.value());
  if (!direct_control_joint_limits.ok()) {
    Fail("direct-control joint limits are unavailable: " +
         direct_control_joint_limits.error().message);
  }
  auto manual_control_descriptors = BuildManualControlDescriptors(
      direct_control_motion_binding.value(), direct_control_gripper_binding,
      direct_control_joint_limits.value(), runtime->gripper_position_limits,
      runtime->motion_participant_policy,
      runtime->deployment_maximum_tcp_linear_speed_mps,
      nullptr, {}, retained_runtime_configuration.gripper_target_configurations,
      input.model_map, retained_model_documents,
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
      std::move(direct_control_binding),
      std::move(direct_control_gripper_program_binding),
      std::move(direct_control_motion_binding.value()),
      std::move(direct_control_gripper_binding),
      std::move(direct_control_joint_limits.value()),
      std::move(manual_control_descriptors.motion),
      std::move(manual_control_descriptors.gripper),
      std::make_shared<const RetainedPlanningModelStage>(std::move(stage)),
      std::move(runtime), std::move(input.projection_revision),
      input.publication_generation,
      std::move(input.status_motion_target_role_id),
      std::move(input.status_gripper_target_role_id));
  composition->ros2_control_runtime_handle =
      std::move(ros2_control_runtime_handle);
  composition->StageRobotTcpReader();
  composition->measured_joint_state_lane = measured_lane;
  composition->controller_process = std::move(controller_process);
  composition->controller_request_context = input.controller_request_context;
  if (prepared_launch) {
    composition->prepared_controller_launch = *prepared_launch;
    delete prepared_launch.release();
  }
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
#else
  if (input.attach_addressed_owner) {
    AttachProductPickPlacePlanningOwner(*composition);
  }
#endif
  return composition;
}

// The simulated PLC plan `bundle` authors for `component`: whether its unit
// is simulated, and its own I/O rows (ROS-free selection in plan_simulated_plc).
[[nodiscard]] std::optional<SimulatedPlcPlan> SimulatedPlcPlanFor(
    const deployment::RetainedBundle &bundle,
    const std::vector<deployment::RetainedIoSignalConfiguration> &io_rows,
    const core::ControllableComponentId &component) {
  std::vector<SimulatedPlcRow> rows;
  std::optional<core::RuntimeUnitId> unit_id;
  for (const auto &row : io_rows) {
    if (row.descriptor.signal_id.controllable_component_id != component)
      continue;
    if (!unit_id) unit_id = row.runtime_unit_id;
    if (row.runtime_unit_id != *unit_id) return std::nullopt;
    rows.push_back({row.provider.profile, row.provider.topic,
                    row.provider.service});
  }
  if (!unit_id) return std::nullopt;
  // CP8 step 1 (the cell-wide `mode` is deleted): a ROS unit is simulated
  // unless one of the components it hosts has measured (real) state authority.
  const auto &graph = bundle.value().cell_graph;
  const bool simulated_unit =
      std::any_of(graph.runtime_units.begin(), graph.runtime_units.end(),
                  [&](const auto &unit) {
                    return unit.runtime_unit_id == *unit_id &&
                           unit.runtime_profile_id.value() ==
                               deployment::ros2_control_execution_runtime_contract::
                                   kRuntimeProfileId;
                  }) &&
      std::none_of(graph.control_bindings.begin(), graph.control_bindings.end(),
                   [&](const auto &binding) {
                     return binding.provider_runtime_unit_id == *unit_id &&
                            binding.state_authority_class ==
                                core::StateAuthorityClass::kMeasured;
                   });
  return plan_simulated_plc(simulated_unit, component.value(), rows,
                            simulated_plc_config_dir_from_environment());
}

// D4 (keep like external hardware): a rebuilt device keeps its running PLC
// process when the candidate's launch identity is unchanged.
[[nodiscard]] bool SimulatedPlcSurvivesRebuild(
    const std::shared_ptr<ControllerLaunchProcess> &process,
    const deployment::RetainedBundle &candidate,
    const core::ControllableComponentId &component) {
  if (!process || process->command().lane != ControllerLaunchLane::kSimulatedPlc)
    return false;
  const auto rows = deployment::ResolveRetainedIoSignalCatalog(candidate.value().cell_graph);
  return rows.ok() && simulated_plc_launch_survives(
      process.get(), SimulatedPlcPlanFor(candidate, rows.value(), component));
}

// sim-plc-peer-design-02/03 §4: a pure generic ROS I/O device in a
// simulated unit (no hosted real component) gets a Manager-owned PLC process, started
// before Prepare by the same supervisor as the mock robot. Equal launch
// identity keeps it (and its values); a changed one restarts it; its exit is
// the device's component fault; removal/Close retire it via controller_process.
// `installed_process` is set when the drivers are borrowed from the exact
// retained installed owner: that owner's PLC must be kept unchanged.
void EnsureSimulatedPlc(
    Composition &view, const deployment::RetainedBundle &bundle,
    const std::vector<deployment::RetainedIoSignalConfiguration> &io_rows,
    const ros2_control_execution::Ros2TransportResources::ContextProvider
        &ros_context,
    const std::filesystem::path &execution_run_root,
    const std::string &request_context, const std::string &bootstrap_context,
    const bool borrowed_drivers,
    const std::shared_ptr<ControllerLaunchProcess> &installed_process) {
  if (view.runtime || view.gripper_runtime || !view.io_provider_drivers ||
      view.io_provider_drivers->empty())
    return;
  const auto plan =
      SimulatedPlcPlanFor(bundle, io_rows, view.controllable_component_id);
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
        return host_detail::WaitForSimulatedPlcEndpoints(
            ros_context(), *plan, std::chrono::seconds(5),
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

// CP9: the observer plan `bundle` authors for a `pose_source` or `ai_model`
// component, or none when the component has neither row.
[[nodiscard]] std::optional<PoseSourcePlan> PoseSourcePlanFor(
    const deployment::RetainedBundle &bundle, const core::RuntimeUnitId &unit,
    const core::ControllableComponentId &component,
    const std::filesystem::path &execution_run_root,
    const std::filesystem::path &manager_run_root) {
  namespace contract = deployment::ros2_control_execution_runtime_contract;
  const auto configuration = contract::require_configuration(bundle, unit);
  const auto &cell = bundle.value().cell_graph.cell_id.value();
  if (const auto *row = contract::find_pose_source(configuration, component))
    return plan_pose_source(cell, component.value(), row->observer_configuration,
                            execution_run_root, manager_run_root);
  if (const auto *row = contract::find_ai_model(configuration, component))
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
    const deployment::RetainedBundle &candidate,
    const core::RuntimeUnitId &unit,
    const core::ControllableComponentId &component,
    const std::filesystem::path &execution_run_root,
    const std::filesystem::path &manager_run_root) {
  if (!process || (process->command().lane != ControllerLaunchLane::kPoseSource &&
                   process->command().lane != ControllerLaunchLane::kAiModel))
    return false;
  return pose_source_launch_survives(
      process.get(), PoseSourcePlanFor(candidate, unit, component,
                                       execution_run_root, manager_run_root));
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
    Composition &view, const deployment::RetainedBundle &bundle,
    const std::optional<core::RuntimeUnitId> &runtime_unit,
    const std::filesystem::path &execution_run_root,
    const std::filesystem::path &manager_run_root,
    const std::string &request_context, const std::string &bootstrap_context,
    const bool borrowed_drivers,
    const std::shared_ptr<ControllerLaunchProcess> &installed_process) {
  if (view.runtime || view.gripper_runtime || !view.io_provider_drivers)
    return;
  std::shared_ptr<IoProviderDriverRecord> driver;
  for (const auto &record : *view.io_provider_drivers)
    if (record && record->stream_observer()) driver = record;
  if (!driver) return;
  Require(runtime_unit.has_value(), "stream observer has no host runtime unit");
  namespace contract = deployment::ros2_control_execution_runtime_contract;
  const auto planned = PoseSourcePlanFor(bundle, *runtime_unit,
                                         view.controllable_component_id,
                                         execution_run_root, manager_run_root);
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
      const auto configuration =
          contract::require_configuration(bundle, *runtime_unit);
      const auto *row = contract::find_pose_source(
          configuration, view.controllable_component_id);
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
  const auto io_catalog = deployment::ResolveRetainedIoSignalCatalog(input.bundle->value().cell_graph);
  Require(io_catalog.ok(), "native I/O catalog is not qualified");
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
  std::sort(io_component_ids.begin(), io_component_ids.end());
  io_component_ids.erase(std::unique(io_component_ids.begin(), io_component_ids.end()), io_component_ids.end());
  const auto with_io_bindings = [bindings = inventory.io,
      bundle = input.bundle, package_root = input.package_root,
      model_map = input.model_map, artifact_root = input.program_artifact_root,
      clock = input.shared_clock, runtime_unit = input.runtime_unit_id,
      epoch = input.runtime_epoch, snapshots = input.retained_component_snapshots,
      io_rows = io_catalog.value(), io_component_ids, make_io_view,
      origin = input.origin_named_apply,
      selected_program = input.selected_program_products,
      ros_context = input.ros_context_provider,
      run_root = input.execution_run_root,
      manager_run_root = input.manager_run_root,
      request_context = input.controller_request_context](std::unique_ptr<Composition> composition) {
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
    const deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(package_root);
    for (auto* view : views) {
      IoProviderDriverGroup drivers;
      const auto previous = snapshots.find(view->controllable_component_id);
      for (const auto& domain : bundle->value().cell_graph.control_domains) {
        const auto* provider = domain.provider();
        if (!provider || provider->controllable_component_id != view->controllable_component_id) continue;
        Require(clock && runtime_unit && provider->provider_runtime_unit_id == *runtime_unit,
                "generic I/O driver has no actual shared-host time/unit");
        std::vector<core::ResourceId> outputs;
        for (const auto& row : io_rows)
          if (row.runtime_unit_id == provider->provider_runtime_unit_id &&
              row.descriptor.signal_id.controllable_component_id == provider->controllable_component_id &&
              std::binary_search(provider->signal_names.begin(), provider->signal_names.end(), row.descriptor.signal_id.name) &&
              row.descriptor.output_resource) outputs.push_back(*row.descriptor.output_resource);
        std::sort(outputs.begin(), outputs.end());
        outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
        core::ResolvedLifecycleControlDomainScopeDTO scope{domain.control_domain_id, domain.control_domain_revision,
            core::ResolvedProviderDomainScopeDTO{*provider, epoch, std::nullopt, std::move(outputs)}};
        core::ResolvedLifecycleProviderOccurrenceDTO occurrence{domain.control_domain_id, domain.control_domain_revision,
            provider->provider_instance_id, provider->provider_manifest_id, provider->provider_manifest_revision,
            provider->provider_runtime_unit_id, epoch, provider->lifecycle_plan_catalog_digest};
        if (previous != snapshots.end()) {
          Require(previous->second.io_provider_drivers != nullptr,
                  "retained component lost its complete I/O driver group");
          const auto retained = std::find_if(previous->second.io_provider_drivers->begin(),
              previous->second.io_provider_drivers->end(), [&](const auto& driver) {
                return driver && driver->domain() == scope && driver->occurrence() == occurrence;
              });
          Require(retained != previous->second.io_provider_drivers->end() && (*retained)->resources_installed(),
                  "retained I/O driver differs from its exact domain/membership/epoch");
          drivers.push_back(*retained);
        } else {
          const auto manifest = manifests.get_verified({bundle->retained_bundle_id(),
              provider->provider_manifest_id, provider->provider_manifest_revision});
          Require(manifest.has_value(), "generic I/O driver has no actual qualified manifest");
          auto driver = IoProviderDriverRecord::Create(std::move(scope), std::move(occurrence),
              manifest->value(), clock, 1'000'000'000LL, kCCLCObservationPollPeriodNs);
          Require(driver.ok(), "generic I/O driver could not be constructed from its exact lifecycle catalog");
          drivers.push_back(std::move(driver.value()));
        }
      }
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
          (runtime_unit ? runtime_unit->value() : std::string("unmanaged")) +
          " epoch=" + epoch.value();
      EnsureSimulatedPlc(*view, *bundle, io_rows, ros_context, run_root,
          request_context, bootstrap_context,
          previous != snapshots.end(),
          previous != snapshots.end() ? previous->second.controller_process : nullptr);
      EnsurePoseSource(*view, *bundle, runtime_unit, run_root, manager_run_root,
          request_context, bootstrap_context,
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
                !input.attach_addressed_owner && input.shared_clock && input.shared_scheduler,
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
  if (inventory.motion.size() == 1U && inventory.gripper.size() <= 1U &&
      std::includes(motion_component_ids.begin(), motion_component_ids.end(),
                    gripper_component_ids.begin(), gripper_component_ids.end())) {
    input.selected_component_id = motion_component_ids.front();
    if (input.retained_component_snapshots.contains(
            motion_component_ids.front()))
      return with_io_bindings(BuildRetainedProductComposition(std::move(input)));
    return with_io_bindings(BuildSingleProductComposition(std::move(input)));
  }
  Require(
      input.runtime_unit_id.has_value() && !input.attach_addressed_owner &&
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
    retain_component(selected.gripper_program_aliases);
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
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers;
    peers.reserve(compositions.size());
    for (const auto &composition : compositions) {
      if (composition->runtime != nullptr)
        peers.push_back(composition->runtime);
    }
    if (primary->runtime != nullptr)
      join_retained_native_runtime_participants(primary->runtime,
                                                std::move(peers),
                                                resolver_factory);
  } else {
    primary->candidate_participant_resolver_factory = resolver_factory;
  }
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  physical_bindings.reserve(component_ids.size() * 2U);
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

[[nodiscard]] std::vector<core::ControllableComponentId>
RetainedNativeComponentIds(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  std::vector<core::ControllableComponentId> ids;
  for (const auto &change : request.component_delta.components) {
    if (change.disposition == deployment::ApplyComponentDisposition::kRetain)
      ids.push_back(change.controllable_component_id);
  }
  Require(std::is_sorted(ids.begin(), ids.end()) &&
              std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "retained native Apply repeats a component identity");
  return ids;
}

void ResealRetainedNativeCandidateOwners(
    Composition &candidate, const Composition &predecessor,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  auto old = IndexNativeComponentCompositions(predecessor);
  auto next = IndexNativeComponentCompositions(candidate);
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
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
    auto &view = *selected->second;
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native component lost the exact parent Apply fence");
    if (!view.runtime && !view.gripper_runtime) {
      Require(view.io_provider_drivers && view.component_runtime_pins,
              "retained I/O component has no actual driver group");
      continue; // Actual leaf rebind belongs to the same installer's Apply barrier.
    }
    std::vector<core::RuntimeParticipantExecutionRegistration> physical_registrations;
    for (const auto& row : view.candidate_component_registrations)
      if (!row.binding.value().io_output_binding) physical_registrations.push_back(row);
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
    RetainedNativeRuntimeResealInput reseal{
        *view.direct_control_motion_binding,
        view.direct_control_gripper_binding,
        {},
        {},
        view.candidate_control_representatives,
        std::move(physical_registrations),
        {},
        nullptr,
        view.scene_frame_resolver,
        request.configuration_fence_id,
        request.configuration_fence_scope,
        admission_gate};
    auto result = reseal_retained_native_runtime_for_apply(*view.runtime,
                                                           std::move(reseal));
    if (!result.ok()) {
      Fail("retained native owner cannot be re-sealed under Apply: " +
           result.error().message);
    }
  }
}

void RefreshRetainedNativeCandidateCollision(
    Composition &candidate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native collision candidate repeats an owner");
  }
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end() &&
                found->second->component_runtime_identity() != nullptr,
            "retained native collision owner is absent");
    auto &view = *found->second;
    // A standalone gripper has no planner or collision-engine resources to
    // reconfigure. Its geometry changes through the Scene projection below.
    if (view.gripper_runtime != nullptr || (!view.runtime && view.io_provider_drivers))
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
// and each model-backed state projector. The candidate composition owns the
// stage whose files the replacement worker loads. Collision resources were
// installed just before; this completes one coherent model generation and
// reads it back before admission can reopen. A failure is rethrown as the
// Apply failure; nothing is restored and the fence keeps admission closed.
void RefreshRetainedNativeCandidateModels(
    Composition &candidate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
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
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
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
    const auto refreshed = refresh_retained_native_runtime_model_for_apply(
        *view.runtime,
        RetainedNativeRuntimeModelRefreshInput{
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

[[nodiscard]] std::unique_ptr<Composition>
BuildComposition(const HostConfig &config,
                 const core::RequestOwnerEpoch &owner_epoch,
                 const core::RuntimeEpoch &runtime_epoch,
                 ManagedKernelAdmissionGate *operation_admission_gate,
                 bool attach_addressed_owner = true,
                 std::shared_ptr<WallClock> shared_clock = {},
                 std::shared_ptr<SimpleScheduler> shared_scheduler = {},
                 ros2_control_execution::Ros2TransportResources::ContextProvider
                     ros_context_provider = {}) {
  const auto build = [&](CompositionBuildInput input) {
    input.ros_context_provider = ros_context_provider;
    return BuildProductComposition(std::move(input));
  };
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation.nonzero(),
          "active retained-bundle publication is absent");
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  const deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      config.package_root);
  auto bundle =
      bundle_source.get_verified(receipt.request.candidate_retained_bundle_id);
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->value().cell_graph.graph_revision ==
                  receipt.request.candidate_graph_revision &&
              bundle->retained_bundle_id() ==
                  receipt.request.candidate_projection_revision
                      .retained_bundle_id &&
              bundle->value().cell_graph.graph_revision ==
                  receipt.request.candidate_projection_revision.graph_revision,
          "active publication and retained bundle disagree");
  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "N3 native host requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      config.package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "active retained model map is absent or invalid");

  const deployment::FilesystemVerifiedProgramBindingSource binding_repository(
      config.package_root);
  auto program_binding = binding_repository.get_verified(
      core::ProgramBindingId(config.program_binding_id), *bundle, *model_map);
  Require(program_binding != nullptr,
          "exact retained Program Binding is absent or invalid");
  const deployment::FilesystemVerifiedProgramSource program_repository(
      config.package_root);
  auto program = program_repository.get_verified(
      program_binding->value().program_id,
      program_binding->value().program_revision, *bundle);
  Require(program != nullptr,
          "Program and exact retained Program Binding disagree");

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      config.package_root);
  auto direct_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *bundle, *model_map, catalog_store);
  if (direct_inventory.motion.empty()) {
    Require(direct_inventory.gripper.size() == 1U &&
                config.motion_target_role_id.empty() &&
                !config.gripper_target_role_id.empty(),
            "native no-motion composition requires one explicitly selected gripper target");
    // Let the component inventory choose the gripper-only owner. It creates
    // no motion engine or planner and is attached to the stable cell Kernel by
    // the same managed bootstrap path as every other component.
    return build(CompositionBuildInput{
        config.package_root,
        config.execution_state_root,
        config.execution_run_root,
        config.candidate_worker,
        core::CellId(config.cell_id),
        owner_epoch,
        std::nullopt,
        runtime_epoch,
        std::move(bundle),
        std::move(*model_map),
        receipt.request.candidate_projection_revision,
        receipt.publication_generation,
        {},
        {},
        {},
        {},
        operation_admission_gate,
        std::move(program),
        std::move(program_binding),
        std::nullopt,
        core::TargetRoleId(config.gripper_target_role_id),
        attach_addressed_owner,
        std::move(shared_clock),
        std::move(shared_scheduler),
        {},
        {},
        {},
        {},
        {},
        {},
    });
  }
  Require(direct_inventory.motion.size() == 1U &&
              direct_inventory.gripper.size() <= 1U,
          "native Program aliases have no sole physical motion target or have "
          "ambiguous gripper targets");
  const auto physical_motion = deployment::resolve_runtime_program_target(
      *direct_inventory.motion.front(),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  std::optional<deployment::RuntimeProgramTargetBinding> physical_gripper;
  if (!direct_inventory.gripper.empty()) {
    physical_gripper = deployment::resolve_runtime_program_target(
        *direct_inventory.gripper.front(),
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
  }
  Require(!config.motion_target_role_id.empty(),
          "native Program has no selected motion target role");
  std::vector<deployment::RuntimeProgramTargetBinding> physical_support;
  const auto &selected_target = program_binding->value().target_role_bindings;
  const auto selected_motion = deployment::resolve_runtime_program_target(
      *program_binding, core::TargetRoleId(config.motion_target_role_id),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  const auto selected_motion_role = std::find_if(
      selected_target.begin(), selected_target.end(), [&](const auto &role) {
        return role.target_role_id ==
               core::TargetRoleId(config.motion_target_role_id);
      });
  Require(selected_motion_role != selected_target.end(),
          "native Program motion target role is absent");
  for (const auto &capability : selected_motion_role->capability_bindings) {
    if (capability.capability_id.value().starts_with("planning.motion.")) {
      physical_support.push_back(deployment::resolve_runtime_program_target(
          *program_binding, core::TargetRoleId(config.motion_target_role_id),
          capability.capability_id, *bundle, *model_map));
    }
  }
  const auto motion_alias = core::verify_runtime_participant_binding(
      deployment::build_runtime_participant_binding_dto(
          selected_motion,
          ActiveFacts(
              selected_motion,
              std::span<const deployment::RuntimeProgramTargetBinding>(
                  physical_support),
              runtime_epoch,
              core::ActiveControlConfigurationId("active.i6.native.arm")),
          std::span<const deployment::RuntimeProgramTargetBinding>(
              physical_support)));
  Require(motion_alias.ok(), "native Program motion alias failed verification");

  std::vector<core::RuntimeParticipantBinding> motion_aliases;
  motion_aliases.push_back(motion_alias.value());
  std::vector<core::RuntimeParticipantBinding> gripper_aliases;
  const bool selected_program_has_gripper = std::any_of(
      selected_target.begin(), selected_target.end(), [](const auto &role) {
        return std::any_of(role.capability_bindings.begin(),
                           role.capability_bindings.end(), [](const auto &cap) {
                             return cap.capability_id.value() ==
                                    "tool.gripper.command";
                           });
      });
  if (selected_program_has_gripper) {
    Require(!config.gripper_target_role_id.empty() &&
                physical_gripper.has_value(),
            "native Program gripper target is not physically available");
    const auto selected_gripper = deployment::resolve_runtime_program_target(
        *program_binding, core::TargetRoleId(config.gripper_target_role_id),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
    Require(selected_gripper.component().controllable_component_id ==
                    physical_gripper->component().controllable_component_id &&
                selected_gripper.target_role().command_target_id ==
                    physical_gripper->target_role().command_target_id,
            "native Program gripper alias does not match its physical target");
    const auto gripper_alias = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            selected_gripper, ActiveFacts(selected_gripper, {}, runtime_epoch,
                                          core::ActiveControlConfigurationId(
                                              "active.i6.native.gripper"))));
    Require(gripper_alias.ok(),
            "native Program gripper alias failed verification");
    gripper_aliases.push_back(gripper_alias.value());
  }
  Require(physical_motion.component().controllable_component_id ==
                  selected_motion.component().controllable_component_id &&
              physical_motion.target_role().command_target_id ==
                  selected_motion.target_role().command_target_id &&
              (!physical_gripper.has_value() ||
               physical_gripper->component().controllable_component_id ==
                   physical_motion.component().controllable_component_id) &&
              (!selected_program_has_gripper ||
               (physical_gripper.has_value() &&
                physical_gripper->component().controllable_component_id ==
                    selected_motion.component().controllable_component_id)),
          "native Program aliases cross their physical component");

  return build(CompositionBuildInput{
      config.package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      owner_epoch,
      std::nullopt,
      runtime_epoch,
      std::move(bundle),
      std::move(*model_map),
      receipt.request.candidate_projection_revision,
      receipt.publication_generation,
      {},
      {},
      std::move(motion_aliases),
      std::move(gripper_aliases),
      operation_admission_gate,
      std::move(program),
      std::move(program_binding),
      core::TargetRoleId(config.motion_target_role_id),
      config.gripper_target_role_id.empty()
          ? std::optional<core::TargetRoleId>{}
          : std::optional<core::TargetRoleId>{
                core::TargetRoleId(config.gripper_target_role_id)},
      attach_addressed_owner,
      std::move(shared_clock),
      std::move(shared_scheduler),
      {},
      {},
      {},
      {},
      {},
      {},
  });
}


[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
PhysicalControlBindings(const Composition &composition) {
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  if (composition.runtime != nullptr) {
    physical_bindings =
        RetainedNativeComponentOwner(composition.runtime).physical_bindings();
  } else if (composition.gripper_runtime != nullptr) {
    physical_bindings = RetainedNativeComponentOwner(
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
                !view.io_provider_drivers->empty()),
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

// Paused resource construction precedes CCLC port registration. The caller
// holds the same existing Apply/composition mutation pin as installation.
void PrepareCompositionIoResources(Composition &composition,
    RetainedNativeCellRuntime &cell,
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
    return manifest == core::kPoseSourceLifecycleManifestId ||
           manifest == core::kAiModelLifecycleManifestId;
  });
  const bool needs_ros = stream_observer ||
      std::any_of(rows.value().begin(), rows.value().end(), [](const auto &row) {
    return row.provider.profile == "ros_bool" ||
           row.provider.profile == "ros_set_bool" ||
           row.provider.profile == "ur_io_states" ||
           row.provider.profile == "ur_set_io" ||
           row.provider.profile == "xarm_digital" ||
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
    host_detail::ExecutionProduct &composition, RetainedNativeCellRuntime &cell,
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
// publication through adoption, before CCLC Start/Continue or command access.
// Core locks have returned; the native method takes only its existing route
// mutex. All factory allocation preceded publication; adoption swaps it.
void AdoptNativeCellParticipantResolverFactory(
    Composition &cell, RetainedRuntimeParticipantResolverFactory &&factory) {
  if (cell.runtime)
    cell.runtime->AdoptVerifiedParticipantResolverFactory(std::move(factory));
}

void AppendNativeGripperStopSources(
    const Composition &cell,
    std::vector<RetainedStopSurveillanceSourceInput> &result) {
  const auto add_gripper = [&](const Composition &view) {
    if (view.gripper_runtime == nullptr)
      return;
    auto source = build_retained_gripper_stop_surveillance_source(
        view.gripper_runtime->binding(),
        view.gripper_runtime->stop_readiness(),
        view.gripper_runtime->state_channel(),
        view.gripper_runtime->participant_engine());
    if (source.has_value() &&
        std::none_of(result.begin(), result.end(), [&](const auto &row) {
          return row.binding.value() == source->binding.value();
        }))
      result.push_back(std::move(*source));
  };
  add_gripper(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing Stop participant");
    add_gripper(*participant);
  }
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
NativeCellStopSources(const Composition &cell) {
  std::vector<RetainedStopSurveillanceSourceInput> result;
  if (cell.runtime != nullptr && cell.runtime->stop_surveillance_owner != nullptr)
    result = cell.runtime->stop_surveillance_owner->SnapshotSources();
  std::set<core::ControllableComponentId> independent_grippers;
  const auto note_gripper = [&](const Composition &view) {
    if (view.gripper_runtime != nullptr)
      independent_grippers.insert(view.controllable_component_id);
  };
  note_gripper(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing Stop participant");
    note_gripper(*participant);
  }
  std::erase_if(result, [&](const auto &row) {
    return independent_grippers.contains(
        row.binding.value().controllable_component_id);
  });
  AppendNativeGripperStopSources(cell, result);
  if (!result.empty())
    Require(RetainedStopSurveillanceOwner::ValidateSources(result).ok(),
            "native cell has invalid Stop observation sources");
  return result;
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

[[nodiscard]] core::Result<void> VerifyNativeEngineForDomain(
    const Composition &composition, const core::ControlDomainId &domain) {
  if (composition.runtime != nullptr) {
    for (const auto &registration :
         composition.runtime->participant_registrations) {
      const auto &path = registration.binding.value().control_path;
      if (path && path->control_domain_id == domain &&
          registration.engine != nullptr &&
          registration.stop_execution_port != nullptr) {
        return core::Result<void>::Ok();
      }
    }
  }
  if (composition.gripper_runtime != nullptr) {
    const auto registration = composition.gripper_runtime->registration();
    const auto &path = registration.binding.value().control_path;
    if (path && path->control_domain_id == domain &&
        registration.engine != nullptr &&
        registration.stop_execution_port != nullptr) {
      return core::Result<void>::Ok();
    }
  }
  for (const auto &participant : composition.participant_compositions) {
    if (VerifyNativeEngineForDomain(*participant, domain).ok()) {
      return core::Result<void>::Ok();
    }
  }
  return core::Result<void>::Err(
      {"native_lifecycle_engine_unavailable",
       "the exact native participant engine is not installed for the control domain",
       {{"reason", "native_engine_or_stop_port_missing"},
        {"control_domain_id", domain.value()}}});
}

void DetachRetainedCCLC(Composition &composition);

// Both hosts adopt exactly what the Kernel reads back at the planned
// generations; a ROS plan's Declared component reads back command-closed.
[[nodiscard]] core::NativeApplyFactAdoptionDTO ReadNativeApplyFacts(
    Composition &composition,
    const core::ActiveBundlePublicationReceiptDTO &publication,
    const core::NativeApplyPlanDTO &plan) {
  Require(publication.valid() &&
              publication.publication_generation ==
                  composition.publication_generation &&
              publication.request.candidate_retained_bundle_id ==
                  composition.bundle->retained_bundle_id() &&
              plan.candidate_retained_bundle_id ==
                  composition.bundle->retained_bundle_id(),
          "native CCLC adoption does not name the installed candidate");
  core::NativeApplyFactAdoptionDTO facts{publication, {}, {}};
  const auto planned_component = [&](const core::ControllableComponentId &id) {
    const auto found = std::find_if(
        plan.components.begin(), plan.components.end(),
        [&](const auto &row) { return row.component_id == id; });
    Require(found != plan.components.end(),
            "native CCLC adoption has an unplanned component");
    return *found;
  };
  const auto representatives = CellControlBindingRepresentatives(composition);
  std::set<core::ControllableComponentId> ready_components;
  for (const auto &desired :
       composition.bundle->value().desired_control_configurations) {
    const auto engine = VerifyNativeEngineForDomain(
        composition, desired.control_domain_id);
    if (!engine.ok()) {
      Fail("native CCLC adoption has no exact engine: " +
           engine.error().message);
    }
    const auto current = composition.active_addressed_kernel()
                             ->current_runtime_route_configuration(
                                 desired.control_domain_id);
    Require(current.ok() && current.value().has_value(),
            "native CCLC adoption has no route readback");
    const auto &active = current.value()->active_configuration;
    const auto planned_route = std::find_if(
        plan.routes.begin(), plan.routes.end(), [&](const auto &row) {
          return row.control_domain_id == desired.control_domain_id;
        });
    Require(planned_route != plan.routes.end() &&
                active.active_control_configuration_generation ==
                    planned_route->generation,
            "native CCLC adoption route generation differs from the plan");
    Require(active.locally_valid() &&
                active.state ==
                    core::ActiveControlConfigurationStateDTO::kReconciled &&
                active.control_domain_id == desired.control_domain_id &&
                active.control_domain_revision ==
                    desired.control_domain_revision &&
                active.activation_set_id == desired.activation_set_id &&
                active.controller_topology_id ==
                    desired.controller_topology_id &&
                active.topology_revision == desired.topology_revision &&
                active.binding_routes.size() == desired.binding_routes.size() &&
                current.value()->readiness_rows.size() ==
                    desired.binding_routes.size(),
            "native CCLC adoption route differs from retained intent");
    for (const auto &route : desired.binding_routes) {
      Require(std::any_of(active.binding_routes.begin(),
                          active.binding_routes.end(), [&](const auto &row) {
                            return row.control_binding_id ==
                                       route.control_binding_id &&
                                   row.control_binding_revision ==
                                       route.control_binding_revision &&
                                   row.control_route_id == route.control_route_id;
                          }),
              "native CCLC adoption is missing a retained binding route");
      const auto representative = std::find_if(
          representatives.begin(), representatives.end(), [&](const auto &row) {
            return row.value().control_path.has_value() &&
                   row.value().control_path->control_domain_id ==
                       desired.control_domain_id &&
                   row.value().control_path->control_binding_id ==
                       route.control_binding_id &&
                   row.value().control_path->control_binding_revision ==
                       route.control_binding_revision;
          });
      Require(representative != representatives.end(),
              "native CCLC adoption has no physical binding representative");
      ready_components.insert(
          representative->value().controllable_component_id);
      // Route currency and component command eligibility are independent.
      // A Disabled component keeps its verified route but cannot dispatch.
      Require(current.value()->operation_admission_open,
              "native CCLC adoption route is not current");
    }
    facts.current_routes.push_back(active);
  }
  for (const auto& domain : composition.bundle->value().cell_graph.control_domains) {
    const auto* provider = domain.provider();
    if (!provider) continue;
    const auto current = composition.active_addressed_kernel()->current_runtime_provider_configuration(
        domain.control_domain_id);
    Require(current.ok() && current.value() && current.value()->locally_valid() &&
                current.value()->retained_bundle_id == composition.bundle->retained_bundle_id() &&
                current.value()->graph_revision == composition.bundle->value().cell_graph.graph_revision &&
                current.value()->domain.control_domain_revision == domain.control_domain_revision &&
                current.value()->domain.provider()->provider == *provider &&
                current.value()->component_lifecycle_generation ==
                    planned_component(provider->controllable_component_id).generation &&
                current.value()->operation_admission_open ==
                    (planned_component(provider->controllable_component_id).state == core::ComponentLifecycleStateDTO::kActive),
            "native CCLC adoption has no exact actual provider installation/readiness");
    ready_components.insert(provider->controllable_component_id);
  }
  for (const auto &component :
       composition.bundle->value().cell_graph.controllable_components) {
    Require(ready_components.erase(component.controllable_component_id) == 1U,
            "native CCLC adoption has a component without verified routes");
    const auto readiness = composition.active_addressed_kernel()
                               ->component_command_readiness(
                                   component.controllable_component_id);
    Require(readiness.ok() &&
                readiness.value().component_lifecycle_generation ==
                    planned_component(component.controllable_component_id)
                        .generation &&
                readiness.value().command_eligible ==
                    (planned_component(component.controllable_component_id)
                         .state == core::ComponentLifecycleStateDTO::kActive),
            "native CCLC adoption command eligibility differs from the plan");
    facts.components.push_back(
        planned_component(component.controllable_component_id));
  }
  Require(ready_components.empty(),
          "native CCLC adoption contains an undeclared route component");
  std::sort(facts.components.begin(), facts.components.end(),
            [](const auto &a, const auto &b) {
              return a.component_id < b.component_id;
            });
  std::sort(facts.current_routes.begin(), facts.current_routes.end(),
            [](const auto &a, const auto &b) {
              return a.control_domain_id < b.control_domain_id;
            });
  return facts;
}


// No-motion route/readiness seams borrow the same stable cell Kernel and
// retain component engines directly; neither callback captures Composition.
void EnsureGripperOnlyCellPorts(Composition& composition) {
  // Arm roots derive exact scalar delegates from their current installed
  // inventory/factory; no per-Apply map or captured predecessor is stored.
  if (composition.runtime || composition.cclc_cell_route_publication_owner) return;
  Require(composition.cell_runtime && composition.bundle,
          "no-motion CCLC has no actual stable cell/product");
  if (composition.bundle->value().cell_graph.control_bindings.empty()) {
    auto route = MakeVerifiedNoControllerRoutePublicationOwner(*composition.bundle);
    Require(route.ok(), "no-controller CCLC has a physical or unqualified domain");
    auto* const cell = composition.cell_runtime;
    // This qualified product has real provider domains but no controller
    // route to select. Actual lifecycle facts still arrive through CCLC's
    // provider ports and the existing readiness proof publication.
    composition.cclc_cell_route_publication_owner = std::move(route.value());
    composition.cclc_cell_command_readiness_port = std::make_shared<RetainedNativeCellCommandReadinessPort>(
        *cell, [factory = NativeCellParticipantResolverFactory(composition)](
                   const auto& publication, const auto& current) {
          using Result = core::Result<RetainedNativeCellReadinessCandidate>;
          RetainedNativeCellReadinessCandidate candidate;
          candidate.registrations = current;
          for (auto& row : candidate.registrations) {
            if (row.binding.value().controllable_component_id != publication.controllable_component_id ||
                !row.binding.value().io_output_binding) continue;
            auto binding = row.binding.value();
            binding.io_output_binding->component_lifecycle_generation = publication.component_lifecycle_generation;
            auto sealed = core::verify_runtime_participant_binding(std::move(binding));
            if (!sealed.ok()) return Result::Err(sealed.error());
            row.binding = std::move(sealed.value());
          }
          std::vector<core::RuntimeParticipantBinding> bindings;
          for (const auto& row : candidate.registrations) bindings.push_back(row.binding);
          candidate.participant_resolver = make_retained_cell_kernel_participant_resolver(
              factory(std::move(bindings)), candidate.registrations);
          candidate.adopt_component_owners = [] {};
          return Result::Ok(std::move(candidate));
        });
    return;
  }
  Require(composition.cell_runtime && composition.bundle &&
              composition.candidate_participant_resolver_factory,
          "no-motion CCLC has no exact stable cell/product resolver factory");
  auto* const cell = composition.cell_runtime;
  const auto owners = IndexNativeComponentCompositions(composition);
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> grippers;
  for (const auto& [component, view] : owners) {
    static_cast<void>(component);
    Require(!view->runtime && (view->gripper_runtime || view->io_provider_drivers),
            "no-motion CCLC contains a foreign physical owner");
    if (view->gripper_runtime) grippers.push_back(view->gripper_runtime);
  }
  const auto resolver_for = [factory = NativeCellParticipantResolverFactory(composition),
                             grippers](const auto& rows) {
    for (const auto& gripper : grippers) {
      const auto actual = gripper->registration();
      const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& value) {
        return value.binding.value() == actual.binding.value() &&
               value.engine == actual.engine && value.stop_execution_port == actual.stop_execution_port;
      });
      if (row == rows.end())
        throw std::invalid_argument("no-motion resolver lost its exact physical owner");
    }
    std::vector<core::RuntimeParticipantBinding> composed;
    for (const auto& row : rows) composed.push_back(row.binding);
    auto binding_resolver = factory(std::move(composed));
    if (!binding_resolver)
      throw std::invalid_argument("no-motion resolver factory returned no resolver");
    return make_retained_cell_kernel_participant_resolver(std::move(binding_resolver), rows);
  };
  auto candidate = [cell, resolver_for](const auto&, const auto& replacements) {
    using Result = core::Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>;
    auto rows = cell->participant_registrations();
    for (const auto& row : replacements) {
      const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& before) {
        return before.engine == row.engine && before.binding.value().control_path && row.binding.value().control_path &&
               before.binding.value().controllable_component_id == row.binding.value().controllable_component_id &&
               before.binding.value().command_target_id == row.binding.value().command_target_id &&
               before.binding.value().capability_id == row.binding.value().capability_id &&
               before.binding.value().control_path->control_binding_id == row.binding.value().control_path->control_binding_id;
      });
      if (found == rows.end()) return Result::Err(
          {"missing_reference", "scalar candidate has no exact installed registration", {}});
      *found = row;
    }
    auto resolver = resolver_for(rows);
    return Result::Ok({std::move(rows), std::move(resolver), {}});
  };
  std::map<core::ControlDomainId, std::shared_ptr<RetainedNativeGripperRoutePublicationOwner>> routes;
  for (const auto& gripper : grippers) {
    const auto binding = gripper->binding();
    Require(binding.value().control_path.has_value(), "scalar route owner has no physical domain");
    auto port = std::make_shared<RetainedNativeGripperRoutePublicationOwner>(
        gripper, *cell->clock(), cell->addressed_kernel(), candidate, cell);
    Require(routes.emplace(binding.value().control_path->control_domain_id, port).second,
            "scalar route inventory repeats a domain owner");
  }
  Require(composition.gripper_runtime != nullptr, "no-motion CCLC has no primary scalar owner");
  const auto primary_binding = composition.gripper_runtime->binding();
  const auto primary = routes.find(primary_binding.value().control_path->control_domain_id);
  Require(primary != routes.end(), "no-motion route map omits its primary owner");
  auto root_route = primary->second;
  routes.erase(primary);
  root_route->BindParticipantPeers(root_route, std::move(routes));
  composition.cclc_cell_route_publication_owner = root_route;
  composition.cclc_cell_command_readiness_port = std::make_unique<RetainedNativeCellCommandReadinessPort>(
      *cell, [grippers = std::move(grippers), resolver_for, root_route](const auto& publication, const auto& current) {
        using Result = core::Result<RetainedNativeCellReadinessCandidate>;
        // This operation-local lease survives the synchronous existing port's
        // validation, publication, exact readback and all cache adoption. It
        // is never stored in a resolver/context/source; same thread releases it.
        auto transition = std::make_shared<std::unique_lock<std::mutex>>(root_route->CellTransitionMutex());
        RetainedNativeCellReadinessCandidate candidate;
        candidate.registrations = current;
        for (auto& row : candidate.registrations) {
          if (row.binding.value().controllable_component_id != publication.controllable_component_id) continue;
          if (row.stop_readiness) {
            auto sealed = core::rebind_runtime_participant_stop_readiness_generation(*row.stop_readiness,
                publication.component_lifecycle_generation);
            if (!sealed.ok()) return Result::Err(sealed.error());
            row.stop_readiness = std::move(sealed.value());
          }
          if (row.binding.value().io_output_binding) {
            auto binding = row.binding.value();
            binding.io_output_binding->component_lifecycle_generation = publication.component_lifecycle_generation;
            auto sealed = core::verify_runtime_participant_binding(std::move(binding));
            if (!sealed.ok()) return Result::Err(sealed.error());
            row.binding = std::move(sealed.value());
          }
        }
        std::vector<std::tuple<std::shared_ptr<RetainedNativeGripperRuntime>,
            std::optional<core::RuntimeParticipantStopReadiness>,
            std::vector<core::RuntimeParticipantExecutionRegistration>>> staged;
        for (const auto& gripper : grippers) {
          const auto actual = gripper->registration();
          if (actual.binding.value().controllable_component_id != publication.controllable_component_id) continue;
          std::vector<core::RuntimeParticipantExecutionRegistration> aliases;
          std::optional<core::RuntimeParticipantStopReadiness> primary_readiness;
          for (const auto& registered : gripper->registrations()) {
            const auto row = std::find_if(candidate.registrations.begin(), candidate.registrations.end(),
                [&](const auto& value) {
              return value.binding.value() == registered.binding.value() &&
                     value.engine == registered.engine && value.stop_execution_port == registered.stop_execution_port;
            });
            if (row == candidate.registrations.end())
              return Result::Err({"missing_reference", "readiness candidate lost its exact scalar alias", {}});
            aliases.push_back(*row);
            if (row->binding.value() == actual.binding.value()) primary_readiness = row->stop_readiness;
          }
          staged.emplace_back(gripper, std::move(primary_readiness), std::move(aliases));
        }
        candidate.participant_resolver = resolver_for(candidate.registrations);
        candidate.adopt_component_owners = [root_route, transition = std::move(transition),
            staged = std::move(staged)]() mutable noexcept {
          for (auto& [gripper, readiness, registrations] : staged) {
            gripper->AdoptVerifiedReadiness(std::move(readiness));
            gripper->AdoptVerifiedRegistrations(std::move(registrations));
          }
        };
        return Result::Ok(std::move(candidate));
      });
}

// Prepare the same actual product used by the installed facade. Original
// delegated runtimes borrow its catalog/ports, so attachment must retain it.
void EnsureInstalledRetainedHostLifecycleProduct(
    Composition &composition, const std::filesystem::path &package_root) {
  if (composition.cclc_lifecycle_product) {
    Require(composition.cclc_provider_manifest_source != nullptr,
            "installed ROS lifecycle product lost its verified provider source");
    return;
  }
  Require((composition.runtime || composition.gripper_runtime || composition.io_provider_drivers) && composition.cell_runtime &&
              composition.active_addressed_kernel() != nullptr,
          "installed ROS lifecycle product requires the stable runtime and Kernel");
  auto provider_manifest_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProviderManifestSource>(package_root);
  auto physical_bindings = PhysicalControlBindings(composition);
  std::vector<std::shared_ptr<RetainedRos2ControlRuntimeHandle>> handles;
  std::vector<std::shared_ptr<IoProviderDriverRecord>> provider_drivers;
  for (const auto &[component_id, view] : IndexNativeComponentCompositions(composition)) {
    static_cast<void>(component_id);
    if (view->io_provider_drivers) {
      provider_drivers.insert(provider_drivers.end(), view->io_provider_drivers->begin(), view->io_provider_drivers->end());
    }
    if (view->runtime || view->gripper_runtime) {
      Require(view->ros2_control_runtime_handle != nullptr,
              "retained ROS lifecycle has no bound execution adapter");
      handles.push_back(view->ros2_control_runtime_handle);
    }
  }
  auto product = RetainedHostLifecycleProduct::Create(
      composition.bundle, *provider_manifest_source, physical_bindings, handles,
      *(composition.runtime ? composition.runtime->clock : composition.cell_runtime->clock()),
      1'000'000'000LL, kCCLCObservationPollPeriodNs, provider_drivers);
  if (!product.ok())
    Fail("native lifecycle product cannot be composed: " + product.error().message);
  composition.cclc_provider_manifest_source = std::move(provider_manifest_source);
  composition.cclc_lifecycle_product = std::move(product.value());
}

using HostCCLCReadinessDependencies = RetainedNativeCCLCReadinessDependencies;

// Invoked only at product construction after the existing route adoption.
// The actual route owner remains the source association authority; the map
// supplies complete geometry independently of source availability.
[[nodiscard]] HostCCLCReadinessDependencies
BuildInstalledCCLCReadinessDependencies(
    const Composition &composition, const std::filesystem::path &package_root) {
  using CaptureResult =
      core::Result<core::ComponentLifecycleReadinessSourceView>;
  const auto unavailable = [](core::ErrorDTO error) -> HostCCLCReadinessDependencies {
    return {nullptr, [error = std::move(error)](
        const core::ComponentLifecycleCurrentSnapshotDTO &) {
      return CaptureResult::Err(error);
    }};
  };
  const auto primary = composition.runtime;
  if ((!primary && !composition.gripper_runtime && !composition.io_provider_drivers) || composition.bundle == nullptr)
    return unavailable({"measured_source_unavailable", "the primary product owner is absent",
                        {{"reason", "measured_primary_owner_missing"}}});
  const auto &products = composition.bundle->value().compiled_products;
  if (products.symbol_role_maps.size() != 1U)
    return unavailable({"measured_source_unavailable", "the exact retained model map is absent",
                        {{"reason", "measured_model_map_missing"}}});
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(package_root);
  const auto model_map = map_source.get_verified(
      {composition.bundle->retained_bundle_id(),
       products.symbol_role_maps.front().product_id});
  if (!model_map)
    return unavailable({"measured_source_unavailable", "the retained model map is unavailable",
                        {{"reason", "measured_model_map_unavailable"}}});

  std::set<core::ControllableComponentId> model_owners;
  for (const auto &coordinate : model_map->coordinates())
    if (coordinate.owner_component_id &&
        coordinate.coordinate_kind != deployment::UrdfCoordinateKind::kNone)
      model_owners.insert(*coordinate.owner_component_id);
  std::vector<core::ControllableComponentId> robots;
  for (const auto &component : composition.bundle->value().cell_graph.controllable_components)
    if (model_owners.erase(component.controllable_component_id) != 0U)
      robots.push_back(component.controllable_component_id);
  if (!model_owners.empty())
    return unavailable({"measured_source_unavailable", "the model geometry has a foreign owner",
                        {{"reason", "measured_model_membership_mismatch"}}});

  std::map<core::ControllableComponentId, std::shared_ptr<RetainedNativeRuntime>> runtimes;
  std::map<core::ControllableComponentId, std::shared_ptr<RetainedNativeGripperRuntime>> grippers;
  std::map<core::ControllableComponentId, std::shared_ptr<const IoProviderDriverGroup>> io_drivers;
  for (const auto &[component, view] : IndexNativeComponentCompositions(composition)) {
    if (view == nullptr || (!view->runtime && !view->gripper_runtime && !view->io_provider_drivers))
      return unavailable({"measured_source_unavailable", "the installed component owner is absent",
                          {{"reason", "measured_component_owner_missing"}}});
    if (view->runtime) runtimes.emplace(component, view->runtime);
    if (view->gripper_runtime) grippers.emplace(component, view->gripper_runtime);
    if (view->io_provider_drivers) io_drivers.emplace(component, view->io_provider_drivers);
    if (std::find(robots.begin(), robots.end(), component) == robots.end())
      continue;
    // Verified physical/model rows supply immutable coordinate names below.
    // Dynamic source authority comes only from the current route capture, so
    // an adopted predecessor epoch cannot poison the static geometry map.
  }
  const auto physical = PhysicalControlBindings(composition);
  std::vector<core::RuntimeParticipantBinding> pneumatic;
  for (const auto& binding : physical) {
    if (binding.value().capability_id.value() != "tool.gripper.command") continue;
    const auto component = binding.value().controllable_component_id;
    const auto motion_owner = runtimes.find(component);
    const auto scalar_owner = grippers.find(component);
    const auto* backend = motion_owner != runtimes.end() ? motion_owner->second->gripper.get()
        : scalar_owner != grippers.end() ? scalar_owner->second->backend() : nullptr;
    Require(backend != nullptr, "qualified gripper geometry has no exact configured backend owner");
    if (backend->gripper_actuation_kind(binding.value().command_target_id.value()) ==
        core::GripperActuationKind::kPneumatic) pneumatic.push_back(binding);
  }
  auto geometry = QualifyRetainedRobotMeasuredGeometry(*model_map, robots, physical, {}, pneumatic);
  if (!geometry.ok()) {
    const auto &error = geometry.error();
    std::cerr << "retained measured geometry unavailable code=" << error.code;
    for (const auto *key : {"reason", "component_id", "control_binding_id",
                            "command_target_id", "source_joint_name",
                            "compiled_joint_name"}) {
      const auto field = error.details.find(key);
      if (field != error.details.end())
        std::cerr << " " << key << "=" << field->second;
    }
    std::cerr << "\n";
    return unavailable(error);
  }
  auto inventory = std::make_shared<const core::ConfiguredRobotMeasuredStateInventory>(
      std::move(geometry.value()));
  const auto validate_io = [io_drivers = std::move(io_drivers)](
      const core::ComponentLifecycleCurrentSnapshotDTO& snapshot) -> core::Result<void> {
    const auto unavailable = [] {
      return core::Result<void>::Err({"io_provider_lifecycle_unavailable",
          "Readiness has no exact installed I/O driver membership",
          {{"reason", "io_provider_readiness_membership_changed"}}});
    };
    if (!snapshot.locally_valid()) return unavailable();
    const auto owned = io_drivers.find(snapshot.controllable_component_id);
    std::size_t count = 0;
    for (std::size_t i = 0; i < snapshot.domains.size(); ++i) {
      if (!snapshot.domains[i].provider()) continue;
      if (owned == io_drivers.end()) return unavailable();
      const auto driver = std::find_if(owned->second->begin(), owned->second->end(), [&](const auto& record) {
        return record && record->domain() == snapshot.domains[i] &&
               record->occurrence() == snapshot.provider_occurrences[i];
      });
      if (driver == owned->second->end() || !(*driver)->resources_installed()) return unavailable();
      ++count;
    }
    if (count != (owned == io_drivers.end() ? 0U : owned->second->size())) return unavailable();
    return core::Result<void>::Ok();
  };
  deployment::CCLCReadinessSourceCapture capture =
      [primary, runtimes = std::move(runtimes), grippers = std::move(grippers), validate_io](
          const core::ComponentLifecycleCurrentSnapshotDTO &snapshot) -> CaptureResult {
    const auto io_valid = validate_io(snapshot);
    if (!io_valid.ok()) return CaptureResult::Err(io_valid.error());
    const auto wrap = [&](CaptureResult captured) -> CaptureResult {
      if (!captured.ok()) return captured;
      auto physical = std::move(captured.value().validate_current);
      captured.value().validate_current = [primary, physical = std::move(physical), validate_io, snapshot] {
        const auto current = validate_io(snapshot);
        if (!current.ok()) return current;
        return physical ? physical() : core::Result<void>::Err(
            {"measured_source_unavailable", "source association validator is absent",
             {{"reason", "measured_source_validator_missing"}}});
      };
      return captured;
    };
    // The existing motion runtime owns the complete arm-and-tool inventory.
    // The scalar owner is used only when no motion runtime exists.
    const auto found = runtimes.find(snapshot.controllable_component_id);
    if (found == runtimes.end()) {
      const auto tool = grippers.find(snapshot.controllable_component_id);
      if (tool != grippers.end())
        return wrap(tool->second->CaptureReadinessSources(snapshot, tool->second));
      if (std::all_of(snapshot.domains.begin(), snapshot.domains.end(),
                      [](const auto& domain) { return domain.provider() != nullptr; }))
        return CaptureResult::Ok({std::nullopt, [validate_io, snapshot] { return validate_io(snapshot); }});
      return CaptureResult::Err({"measured_source_unavailable",
          "the component has no exact installed runtime owner",
          {{"reason", "measured_component_owner_missing"}}});
    }
    // Retain both actual provider members and the primary cell lifetime while
    // the existing physical route/source validator runs.
    return wrap(found->second->CaptureReadinessSources(snapshot, found->second));
  };
  return {std::move(inventory), std::move(capture)};
}
struct MockResetPositionComponent final {
  std::shared_ptr<RetainedNativeRuntime> runtime;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  MockResetPositionTarget target;
  core::ComponentLifecycleCurrentSnapshotDTO snapshot;
  core::ResolvedLifecycleControlDomainScopeDTO domain;
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
};

core::ErrorDTO ResetPositionError(std::string reason, std::string message,
                                 bool effect_possible = false) {
  if (effect_possible)
    message += " Restart the Workbench before trying again; the Reset claim remains held.";
  return {"program_reset_position_unavailable", std::move(message),
          {{"reason", std::move(reason)},
           {"effect_disposition", effect_possible ? "unknown" : "known_no_effect"}}};
}

core::Result<std::vector<MockResetPositionComponent>>
CaptureMockResetPositionComponents(Composition& composition,
                                   deployment::CCLCRuntimeComposition& facade) {
  using Result = core::Result<std::vector<MockResetPositionComponent>>;
  const auto unavailable = [](const char* reason, const char* message) {
    return Result::Err(ResetPositionError(reason, message));
  };
  if (!composition.bundle ||
      !composition.cclc_attached.load(std::memory_order_acquire) ||
      composition.route_recovery_pending.load(std::memory_order_acquire))
    return unavailable("reset_position_deployment_unavailable", "The current Deployment is not available for position reset.");
  std::set<core::ControllableComponentId> required;
  for (const auto& binding : composition.bundle->value().cell_graph.control_bindings) {
    if (binding.capability_id == core::CapabilityId("execution.trajectory.execute"))
      required.insert(binding.controllable_component_id);
  }
  if (required.empty())
    return unavailable("reset_position_no_arm", "The current Deployment has no supported Mock arm.");
  const auto owners = IndexNativeComponentCompositions(composition);
  std::vector<MockResetPositionComponent> captured;
  for (const auto& component : required) {
    const auto owner = owners.find(component);
    if (owner == owners.end() || !owner->second->runtime || !owner->second->ros2_control_runtime_handle)
      return unavailable("reset_position_arm_owner_missing", "A Deployment arm has no current supported runtime.");
    const auto& runtime = owner->second->runtime;
    const auto& handle = owner->second->ros2_control_runtime_handle;
    const auto target = handle->mock_reset_position_target();
    const auto identity = deployment::derive_component_runtime_identity(*composition.bundle, component);
    if (!target || !identity || target->component_runtime_identity != identity->value() ||
        target->joint_names != handle->controller_joint_names() || target->positions.empty() ||
        target->joint_names.size() != target->positions.size() ||
        !std::all_of(target->positions.begin(), target->positions.end(), [](double value) { return std::isfinite(value); }))
      return unavailable("reset_position_simulator_unsupported", "Position reset requires a supported Mock arm; real hardware and Isaac are unavailable.");
    const auto& binding = runtime->motion_binding.value();
    if (!binding.control_path || binding.resources.size() != target->positions.size() ||
        !owner->second->measured_joint_state_lane || !runtime->motion_state_channel ||
        runtime->motion_state_channel->source_kind() != core::ObservedStateSourceKind::kCommandedState)
      return unavailable("reset_position_binding_unsupported", "A Mock arm has no exact measured initial-position binding.");
    for (std::size_t index = 0; index < binding.resources.size(); ++index) {
      if (binding.resources[index].source_joint_name != std::optional(target->joint_names[index]))
        return unavailable("reset_position_joint_order_mismatch", "The Mock initial-position order differs from its current binding.");
    }
    const auto snapshot = facade.ReadCurrentComponentSnapshot(component);
    if (!snapshot.ok()) return Result::Err(ResetPositionError("reset_position_lifecycle_unavailable", "A Mock arm has no current lifecycle observation."));
    if (snapshot.value().component_lifecycle_state != core::ComponentLifecycleStateDTO::kActive)
      return unavailable("reset_position_arm_inactive", "Every Deployment arm must already be Active to reset its position.");
    const auto& publication = snapshot.value().active_publication_receipt;
    if (publication.publication_generation != composition.publication_generation ||
        publication.request.candidate_retained_bundle_id != composition.bundle->retained_bundle_id() ||
        publication.request.candidate_graph_revision != composition.bundle->value().cell_graph.graph_revision ||
        publication.request.candidate_projection_revision != composition.projection_revision)
      return unavailable("reset_position_publication_changed", "The current lifecycle product differs from the selected Deployment.");
    const auto& path = *binding.control_path;
    const auto domain = std::find_if(snapshot.value().domains.begin(), snapshot.value().domains.end(),
        [&](const auto& row) { return row.control_domain_id == path.control_domain_id; });
    const auto provider = std::find_if(snapshot.value().provider_occurrences.begin(), snapshot.value().provider_occurrences.end(),
        [&](const auto& row) { return row.control_domain_id == path.control_domain_id; });
    if (domain == snapshot.value().domains.end() || provider == snapshot.value().provider_occurrences.end() ||
        domain->provider() || !domain->controller().active_control_configuration ||
        domain->controller().active_control_configuration->state != core::ActiveControlConfigurationStateDTO::kReconciled ||
        domain->control_domain_revision != path.control_domain_revision ||
        domain->controller().controller_topology_id != path.controller_topology_id ||
        domain->controller().topology_revision != path.topology_revision ||
        domain->controller().active_control_configuration->active_control_configuration_id != path.active_control_configuration_id ||
        domain->controller().active_control_configuration->active_control_configuration_generation != path.active_control_configuration_generation ||
        provider->control_domain_revision != path.control_domain_revision ||
        provider->provider_runtime_unit_id != path.provider_runtime_unit_id ||
        provider->provider_runtime_epoch != path.provider_runtime_epoch ||
        provider->provider_instance_id != path.provider_instance_id ||
        provider->provider_manifest_id != path.provider_manifest_id ||
        provider->provider_manifest_revision != path.provider_manifest_revision)
      return unavailable("reset_position_configuration_unavailable", "A Mock arm has no exact active provider and configuration.");
    const auto port = handle->current_port();
    if (!port)
      return unavailable("reset_position_controller_unavailable", "A Mock arm controller is unavailable.");
    const auto readiness = port->ReadReadiness();
    if (!readiness.controller_active || !readiness.controller_bound || !readiness.hardware_active ||
        !handle->mock_reset_writer_inactive())
      return unavailable("reset_position_controller_not_active", "A Mock arm must have an active normal controller before position reset.");
    captured.push_back({runtime, handle, *target, snapshot.value(), *domain, *provider});
  }
  return Result::Ok(std::move(captured));
}

// The existing route publisher pins one raw source for the stock writer ACK.
// It is not commanded-state authority and never feeds the frame/model channel.
struct MockResetMeasuredSource final {
  core::ComponentLifecycleReadinessSourceView association;
  core::RequiredRobotMeasuredStateSource source;
};

core::Result<MockResetMeasuredSource> CaptureMockResetMeasuredSource(
    const MockResetPositionComponent& component) {
  using Result = core::Result<MockResetMeasuredSource>;
  auto association = component.runtime->CaptureReadinessSources(component.snapshot, component.runtime);
  if (!association.ok()) return Result::Err(association.error());
  if (!association.value().measured_sources || !association.value().validate_current)
    return Result::Err(ResetPositionError("reset_position_raw_source_missing", "The Mock arm has no current raw state source."));
  const auto& binding = component.runtime->motion_binding.value();
  const auto& path = *binding.control_path;
  const auto& sources = association.value().measured_sources->sources;
  const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& source) {
    const auto& query = source.descriptor.publication.query_context;
    return query.control_binding_id == path.control_binding_id &&
        query.control_binding_revision == path.control_binding_revision &&
        query.control_route_id == path.control_route_id &&
        query.active_control_configuration_id == path.active_control_configuration_id &&
        query.active_control_configuration_generation == path.active_control_configuration_generation;
  });
  if (found == sources.end() || !found->reader ||
      found->descriptor.publication.query_context.resources.size() != component.target.positions.size())
    return Result::Err(ResetPositionError("reset_position_raw_source_missing", "The Mock arm has no exact raw state source."));
  for (std::size_t i = 0; i < binding.resources.size(); ++i)
    if (found->descriptor.publication.query_context.resources[i] != binding.resources[i].resource_id)
      return Result::Err(ResetPositionError("reset_position_raw_source_order", "The Mock arm raw state order is not its target order."));
  auto source = *found;
  return Result::Ok({std::move(association.value()), std::move(source)});
}

core::Result<core::ObservedStateSnapshotDTO> ReadMockResetMeasuredSource(
    const MockResetPositionComponent& component, const MockResetMeasuredSource& source) {
  auto current = source.association.validate_current();
  if (!current.ok()) return core::Result<core::ObservedStateSnapshotDTO>::Err(current.error());
  auto sample = source.source.reader->current_state(
      source.source.descriptor.publication.query_context, component.runtime->clock->now());
  current = source.association.validate_current();
  if (!current.ok()) return core::Result<core::ObservedStateSnapshotDTO>::Err(current.error());
  return sample;
}

core::Result<void> RunMockResetPosition(
    Composition& composition, deployment::CCLCRuntimeComposition& facade,
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
  auto captured = CaptureMockResetPositionComponents(composition, facade);
  if (!captured.ok()) return Result::Err(captured.error());
  std::vector<MockResetMeasuredSource> sources;
  sources.reserve(captured.value().size());
  // Refuse every unsupported/changed arm before the first component effect.
  for (const auto& component : captured.value()) {
    auto source = CaptureMockResetMeasuredSource(component);
    if (!source.ok()) return Result::Err(ResetPositionError("reset_position_raw_source_unavailable", source.error().message));
    const auto sample = ReadMockResetMeasuredSource(component, source.value());
    if (!sample.ok()) return Result::Err(ResetPositionError("reset_position_raw_source_unavailable", sample.error().message));
    sources.push_back(std::move(source.value()));
  }
  bool effect_possible = false;
  for (std::size_t index = 0; index < captured.value().size(); ++index) {
    const auto& component = captured.value()[index];
    const auto& source = sources[index];
    const auto wake = component.handle->mock_reset_observation_wake();
    if (!wake) return Result::Err(ResetPositionError("reset_position_observation_missing", "The Mock arm observation wake is absent.", effect_possible));
    core::CCLCProgramResetPositionScopeRequest scope{
        request.authorization.program_runner_owner_epoch, request.authorization.cycle_generation,
        request.authorization.request_owner_epoch, request.authorization.request_id,
        request.authorization.request_payload_digest, component.snapshot.active_publication_receipt,
        component.snapshot.controllable_component_id, component.provider, component.domain};
    auto result = facade.WithProgramResetPositionScope(scope,
        [&](const core::CCLCProgramResetPositionAdvance& advance) -> Result {
      std::optional<core::ObservedStateSnapshotDTO> baseline;
      const auto switched = component.handle->ResetMockPosition(stop_token,
          [&](const MockResetPositionTransition& next, const MockResetPositionStepDeadline deadline) -> Result {
        // Containment remains available even after the exact scope expired.
        // It cannot reopen or extend the requested-inactivity exception.
        if (next.phase == MockResetPositionPhase::kContaining) return Result::Ok();
        auto next_core_phase = core::CCLCProgramResetPositionPhase::kSwitchingToWriter;
        switch (next.phase) {
          case MockResetPositionPhase::kSwitchingToWriter: break;
          case MockResetPositionPhase::kSettingState:
            next_core_phase = core::CCLCProgramResetPositionPhase::kSettingState; break;
          case MockResetPositionPhase::kSwitchingToJtc:
            next_core_phase = core::CCLCProgramResetPositionPhase::kSwitchingToJtc; break;
          case MockResetPositionPhase::kContaining: break;
        }
        // Production's existing owner clock and the SDK both use steady-clock
        // nanoseconds. Consume the exact SDK step-entry deadline in both; no
        // fresh budget is created after validation or dispatch.
        const auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count();
        auto advanced = advance(next_core_phase, deadline_ns);
        if (!advanced.ok()) return advanced;
        if (next.phase == MockResetPositionPhase::kSwitchingToWriter && !next.switch_acknowledged)
          effect_possible = true;  // The following switch may be sent.
        const auto validate_scope = [&] { return advance(next_core_phase, std::nullopt); };
        if (next.phase == MockResetPositionPhase::kSettingState) {
          if (!next.target_published) {
            auto sample = ReadMockResetMeasuredSource(component, source);
            if (!sample.ok()) return Result::Err(sample.error());
            baseline = std::move(sample.value());
            return Result::Ok();
          }
          if (!baseline) return Result::Err(ResetPositionError("reset_position_write_baseline_missing", "The Mock writer has no pre-publication source receipt.", true));
          const auto published_at = component.runtime->clock->now();
          auto acknowledged = WaitForMockResetObservation(*wake, stop_token, deadline, [&]() -> core::Result<bool> {
            auto valid = source.association.validate_current();
            if (!valid.ok()) return core::Result<bool>::Err(valid.error());
            auto sample = ReadMockResetMeasuredSource(component, source);
            if (!sample.ok()) return core::Result<bool>::Ok(false);
            return core::Result<bool>::Ok(sample.value().source_epoch == baseline->source_epoch &&
                ExactMockResetMeasuredTarget(sample.value(), component.target.positions,
                    baseline->sequence, baseline->source_timestamp_ns, published_at));
          }, validate_scope);
          if (!acknowledged.ok()) return acknowledged;
          const std::vector<double> zero(component.target.positions.size(), 0.0);
          const auto initialized = component.runtime->motion_state_channel->publish_commanded_initialization_and_project(
              component.target.positions, zero, component.runtime->clock->now());
          if (!initialized.applied())
            return Result::Err(ResetPositionError("reset_position_command_publication_failed", "The backend commanded initial pose could not be published and projected.", true));
          return Result::Ok();
        }
        if (next.switch_acknowledged) {
          // A positive response permits association with ordinary CM/action
          // observation. Unknown responses never reach this readback barrier.
          return WaitForMockResetObservation(*wake, stop_token, deadline, [&]() -> core::Result<bool> {
            return core::Result<bool>::Ok(next.phase == MockResetPositionPhase::kSwitchingToWriter
                ? component.handle->mock_reset_writer_active()
                : component.handle->mock_reset_successor_ready());
          }, validate_scope);
        }
        return Result::Ok();
      });
      if (switched.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
        return Result::Err(switched.error.value_or(ResetPositionError("reset_position_switch_failed", "The Mock position reset did not complete.", effect_possible)));
      return Result::Ok();
    });
    if (!result.ok()) {
      auto error = result.error();
      error.details["effect_disposition"] = effect_possible ? "unknown" : "known_no_effect";
      if (effect_possible) error.message += " Restart the Workbench before trying again; the Reset claim remains held.";
      return Result::Err(std::move(error));
    }
  }
  return Result::Ok();
}

void ConfigureProgramResetPositionAccess(ExecutionCompositionAccess& access,
                                        const core::RequestOwnerEpoch& owner_epoch,
                                        deployment::CCLCRuntimeComposition& facade) {
  // The managed cell keeps this same owner through installed-product exchange.
  // Capture its existing service lifetime, never the moving unique_ptr slot in
  // a component composition. Callers install callbacks before ingress or under
  // the existing execution-composition write exclusion.
  access.reset_robot_position_capability = [&facade](Composition& composition) {
    using Result = core::Result<ProgramResetPositionCapability>;
    try {
      const auto components = CaptureMockResetPositionComponents(composition, facade);
      if (!components.ok()) return Result::Ok({false, components.error().message});
      // Mirror the existing effect's exact source-association prerequisite
      // before selected Scene effects. This is no sample freshness, idle or
      // resource-admission test; those remain with their existing owners.
      for (const auto& component : components.value()) {
        const auto source = CaptureMockResetMeasuredSource(component);
        if (!source.ok()) return Result::Ok({false, source.error().message});
      }
      return Result::Ok({true, std::nullopt});
    } catch (const std::exception& error) {
      return Result::Ok({false, std::string("Position reset capability is unavailable: ") + error.what()});
    } catch (...) {
      return Result::Ok({false, "Position reset capability is unavailable."});
    }
  };
  access.reset_robot_position = [owner_epoch, &facade](Composition& composition,
      const ProgramResetPositionRequest& request, std::stop_token stop_token) {
    try {
      return RunMockResetPosition(composition, facade, request, owner_epoch, stop_token);
    } catch (const std::exception& error) {
      return core::Result<void>::Err(ResetPositionError("reset_position_effect_exception", error.what(), true));
    } catch (...) {
      return core::Result<void>::Err(ResetPositionError("reset_position_effect_exception", "Position reset failed unexpectedly.", true));
    }
  };
}

[[nodiscard]] RetainedNativeCCLCRuntimeConfig
WithInstalledReadinessConstruction(
    RetainedNativeCCLCRuntimeConfig config, const Composition &composition,
    const std::filesystem::path &package_root) {
  config.prepare_readiness_dependencies = [&composition, package_root] {
    return BuildInstalledCCLCReadinessDependencies(composition, package_root);
  };
  return config;
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
      const auto bindings = RetainedNativeComponentOwner(view->runtime).physical_bindings();
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

void AttachRetainedNativeCCLC(
    Composition &composition, const HostConfig &config,
    ManagedKernelApplyFenceOwner &fence_owner,
    const std::optional<core::ConfigurationFenceStatusQueryDTO> &parent_apply =
        std::nullopt,
    const std::optional<core::RequestOwnerEpoch> &quiescent_predecessor =
        std::nullopt,
    const std::function<core::Result<void>()> &restore_global_rules = {}) {
  core::IClock *owner_clock = nullptr;
  core::IScheduler *owner_scheduler = nullptr;
  Require(composition.cell_runtime && composition.component_runtime_identity(),
          "CCLC attachment requires the stable cell and an actual component owner");
  owner_clock = composition.cell_runtime->clock().get();
  owner_scheduler = composition.cell_runtime->scheduler().get();
  EnsureGripperOnlyCellPorts(composition);
  Require(composition.active_addressed_kernel() != nullptr &&
              owner_clock != nullptr && owner_scheduler != nullptr,
          "native CCLC attachment requires one stable addressed Kernel");

  auto &kernel = *composition.active_addressed_kernel();
  if (composition.cclc_runtime) {
    Require(composition.cclc_attached.load(std::memory_order_acquire),
            "constructed native CCLC is not attached");
    return;
  }
  Require(kernel.SetRouteRecoveryPending(true).ok(),
          "route recovery could not retain closed admission");
  composition.route_recovery_pending.store(true, std::memory_order_release);

  auto active_publication_reader =
      std::make_unique<component_manager::SqliteActiveBundlePublicationStore>(
          config.manager_state_root /
              std::string(
                  component_manager::kActiveBundlePublicationStoreFilename),
          core::CellId(config.cell_id));
  const auto integrity = active_publication_reader->VerifyIntegrity();
  Require(integrity.current_generation == composition.publication_generation,
          "native CCLC active publication changed during composition");
  auto active_publication =
      active_publication_reader->ReadActiveBundlePublication(
          integrity.current_generation);
  Require(
      active_publication.ok() && active_publication.value().valid() &&
          active_publication.value().request.candidate_retained_bundle_id ==
              composition.bundle->retained_bundle_id() &&
          active_publication.value()
                  .request.candidate_projection_revision.retained_bundle_id ==
              composition.projection_revision.retained_bundle_id &&
          active_publication.value()
                  .request.candidate_projection_revision.graph_revision ==
              composition.projection_revision.graph_revision &&
          active_publication.value()
                  .request.candidate_projection_revision.scene_revision
                  .scene_id ==
              composition.projection_revision.scene_revision.scene_id &&
          active_publication.value().request.candidate_graph_revision ==
              composition.bundle->value().cell_graph.graph_revision,
      "native CCLC cannot re-read the exact active product");

  // Both hosts adopt their own recorded plan while the parent Apply fence is
  // still held. A ROS plan adopts its Declared components and the routes it
  // actually published; ordinary CCLC Prepare/Enable reopens afterwards.
  if (parent_apply.has_value()) {
    fence_owner.RunWithExactHeldSuccessor(
        *parent_apply, active_publication.value(), [&] {
          core::SqliteCCLCJournal journal(
              config.execution_state_root /
                  std::string(core::kCCLCJournalFilename),
              core::CellId(config.cell_id));
          const auto plan = journal.LoadNativeApplyPlan(
              {parent_apply->owner_transaction.apply_id,
               parent_apply->owner_transaction.apply_generation});
          if (!plan.ok() || !plan.value().has_value()) {
            Fail("native CCLC has no recorded plan for this Apply");
          }
          auto facts = ReadNativeApplyFacts(composition,
                                            active_publication.value(),
                                            *plan.value());
          const auto adopted = journal.AdoptNativeApplyFacts(facts);
          if (!adopted.ok()) {
            Fail("native CCLC could not adopt exact Apply facts: " +
                 adopted.error().message);
          }
          journal.Close();
        });
  }

  auto artifact_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProductArtifactSource>(
      config.package_root);
  EnsureInstalledRetainedHostLifecycleProduct(composition, config.package_root);
  auto *const lifecycle_product_view = composition.cclc_lifecycle_product.get();

  deployment::CCLCRuntimeCompositionConfig cclc_config{
      config.execution_state_root,
      config.execution_run_root,
      core::CellId(config.cell_id),
      {core::RequestOwnerEpoch(RandomToken("cclc-owner-")), 1024U,
       kCCLCLifecycleWorkerCount, kCCLCObservationPollPeriodNs,
       30'000'000'000LL}};
  cclc_config.coordinator.quiescent_predecessor = quiescent_predecessor;
  cclc_config.preparation_failure_source = config.preparation_failure_source;
  cclc_config.lifecycle_target_driver.stream_health = host_component_stream_health();
  cclc_config.lifecycle_target_driver.read_output_drop_counts =
      [](const core::ControllableComponentId &component)
      -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    const auto counts = host_controller_launch_supervisor()
                            .component_output_counters(component.value());
    if (!counts)
      return std::nullopt;
    return std::pair{counts->dropped_forward_lines,
                     counts->dropped_forward_bytes};
  };
  cclc_config.on_incidents_pruned =
      [](const core::ComponentIncidentPruneNotice &notice) {
        host_controller_launch_supervisor().enqueue_host_log(
            "INFO manyforge CCLC closed incidents pruned: count=" +
            std::to_string(notice.count) +
            " oldest_sequence=" + std::to_string(notice.oldest_sequence) +
            " newest_sequence=" + std::to_string(notice.newest_sequence) +
            "\n");
      };
  const bool initial_bootstrap =
      std::holds_alternative<core::BootstrapPublicationTransactionDTO>(
          active_publication.value().request.transaction_ref);
  const auto bind_actual_published_outputs = [&](const deployment::CCLCRuntimeComposition &facade) {
    using Reply = core::Result<void>;
    IoProviderConstructionGenerations generations;
    std::vector<core::ComponentLifecycleCurrentSnapshotDTO> snapshots;
    for (const auto &[component, view] : IndexNativeComponentCompositions(composition)) {
      if (!view->io_provider_drivers || view->io_provider_drivers->empty()) continue;
      const auto actual = facade.ReadCurrentComponentSnapshot(component);
      if (!actual.ok()) return Reply::Err(actual.error());
      Require(actual.value().locally_valid() &&
                  actual.value().active_publication_receipt.publication_generation == composition.publication_generation &&
                  actual.value().active_publication_receipt.request.candidate_retained_bundle_id == composition.bundle->retained_bundle_id(),
              "output construction snapshot is not the actual published component");
      generations.emplace(component, actual.value().component_lifecycle_generation);
      snapshots.push_back(actual.value());
    }
    if (!snapshots.empty()) {
      const auto bound =
          BindCompositionIoOutputParticipants(composition, generations);
      if (!bound.ok())
        return bound;
      Require(composition.cell_runtime != nullptr,
              "output construction lost the stable cell");
      auto rows = NativeCellComponentRegistrations(composition);
      RetainedRuntimeParticipantResolverFactory staged_factory;
      auto resolver = NativeCellParticipantResolver(composition, rows, &staged_factory);
      const auto installed =
          composition.cell_runtime->reconfigure_runtime_product(
              std::move(rows), std::move(resolver),
              NativeCellStopSources(composition));
      if (!installed.ok())
        return installed;
      AdoptNativeCellParticipantResolverFactory(composition, std::move(staged_factory));
      for (const auto &actual : snapshots) {
        const core::ComponentCommandReadinessPublicationDTO closed{
            core::ComponentReadinessAuthorityDTO::kRestartExclusion,
            {},
            {},
            actual.controllable_component_id,
            actual.component_lifecycle_generation,
            false};
        const auto published =
            kernel.publish_runtime_provider_configuration(closed, actual, {});
        if (!published.ok())
          return Reply::Err(published.error());
      }
    }
    if (restore_global_rules) {
      // Ordinary initial Bootstrap is not successful NamedApply metadata.
      // Preserve its preexisting actual signal-service installation only when
      // it has no authored global rules; never fabricate verified_empty.
      if (initial_bootstrap &&
          !composition.compiled_waiting_global_io_rules &&
          (!composition.compiled_global_io_rules ||
           composition.compiled_global_io_rules->rules.empty())) {
        const auto installed = kernel.InstallProgramIoSignalService(
            composition.io_signal_service,
            composition.bundle->retained_bundle_id(),
            composition.bundle->value().cell_graph.graph_revision);
        if (installed.ok() && composition.io_signal_runtime)
          composition.io_signal_runtime->RequestRuleEvidenceProcessing();
        return installed;
      }
      return restore_global_rules();
    }
    return Reply::Ok();
  };
  auto cclc = [&]() -> core::Result<
      std::unique_ptr<deployment::CCLCRuntimeComposition>> {
    if (!composition.runtime) {
      auto& route = *composition.cclc_cell_route_publication_owner;
      auto& readiness = *composition.cclc_cell_command_readiness_port;
      auto current = [&kernel](const core::ControlDomainId& domain) {
        auto read = kernel.current_runtime_route_configuration(domain);
        if (!read.ok()) return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Err(read.error());
        return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
            read.value() ? std::optional(read.value()->active_configuration) : std::nullopt);
      };
      if (parent_apply || quiescent_predecessor) {
        auto created = CreateRetainedNativeCCLCRuntime(
            WithInstalledReadinessConstruction({std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
            composition.bundle, *active_publication_reader, std::move(active_publication.value()), *artifact_source,
            lifecycle_product_view->catalog_source(), lifecycle_product_view->brand_ports(),
            CellControlBindingRepresentatives(composition), kernel, fence_owner, route,
            *owner_clock, *owner_scheduler, std::move(current), lifecycle_product_view->lifecycle_snapshot_source(), &readiness);
        if (!created.ok()) return created;
        const auto started = created.value()->Start();
        if (!started.ok()) return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::Err(started.error());
        return created;
      }
      auto* const exact_route = dynamic_cast<RetainedNativeGripperRoutePublicationOwner*>(&route);
      Require(exact_route || composition.bundle->value().cell_graph.control_bindings.empty(),
              "no-motion restart has another physical route owner");
      return CreateRecoveredRetainedNativeCellCCLCRuntime(
          WithInstalledReadinessConstruction({std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
          composition.bundle, *active_publication_reader, std::move(active_publication.value()), *artifact_source,
          lifecycle_product_view->catalog_source(), lifecycle_product_view->brand_ports(),
          CellControlBindingRepresentatives(composition), kernel, fence_owner, route, readiness, kernel,
          *owner_clock, *owner_scheduler,
          [exact_route](const auto& predecessor) -> core::Result<core::RuntimeRouteConfigurationReadbackDTO> {
            if (exact_route) return exact_route->AdoptAndReadBackRouteConfigurationPredecessor(predecessor);
            return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
                {"invalid_request", "A qualified no-controller product has no physical route predecessor",
                 {{"reason", "io_provider_physical_predecessor_absent"}}});
          },
          [cell = composition.cell_runtime] { return cell->ReconcileCommandFreeOrphans(); },
          composition.cclc_cell_route_reconciler, lifecycle_product_view->lifecycle_snapshot_source(),
          bind_actual_published_outputs);
    }
    if (parent_apply || quiescent_predecessor) {
      // A live parent installation already carries its actual routes and
      // readiness. Reconstruct only the immutable facade dependencies; the
      // startup recovery overload would adopt/reconcile unrelated effects.
      auto current_configuration = [&runtime = *composition.runtime](
          const core::ControlDomainId &domain) {
        return runtime.ReadCurrentPublishedRouteConfiguration(domain);
      };
      auto created = CreateRetainedNativeCCLCRuntime(
          WithInstalledReadinessConstruction(
              {std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
          composition.bundle, *active_publication_reader,
          std::move(active_publication.value()), *artifact_source,
          lifecycle_product_view->catalog_source(),
          lifecycle_product_view->brand_ports(),
          CellControlBindingRepresentatives(composition), kernel, fence_owner,
          *composition.runtime, *owner_clock, *owner_scheduler,
          std::move(current_configuration),
          lifecycle_product_view->lifecycle_snapshot_source(),
          composition.runtime.get());
      if (!created.ok()) return created;
      const auto started = created.value()->Start();
      if (!started.ok())
        return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::Err(
            started.error());
      return created;
    }
    return CreateRetainedNativeCCLCRuntime(
        WithInstalledReadinessConstruction(
            {std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
        composition.bundle, *active_publication_reader,
        std::move(active_publication.value()), *artifact_source,
        lifecycle_product_view->catalog_source(),
        lifecycle_product_view->brand_ports(),
        CellControlBindingRepresentatives(composition), *composition.runtime,
        *composition.active_addressed_kernel(), fence_owner
        ,
        lifecycle_product_view->lifecycle_snapshot_source(),
        bind_actual_published_outputs
    );
  }();
  if (!cclc.ok()) {
    Fail("native CCLC composition failed: " + cclc.error().message);
  }
  PrepareInstalledMeasuredSourcesAfterContext(composition);


  composition.cclc_active_publication_reader =
      std::move(active_publication_reader);
  composition.cclc_artifact_source = std::move(artifact_source);
  composition.cclc_runtime = std::move(cclc.value());
  composition.cclc_attached.store(true, std::memory_order_release);
}

void DetachRetainedCCLC(Composition &composition) {
  composition.cclc_attached.store(false, std::memory_order_release);
  composition.route_recovery_pending.store(false, std::memory_order_release);
  composition.cclc_cell_route_reconciler = {};
  if (composition.runtime != nullptr) {
    composition.runtime->stop_recovery_configuration_source = {};
    composition.runtime->reconcile_controller_routes = {};
  }
  composition.cclc_runtime.reset();
  composition.cclc_cell_command_readiness_port.reset();
  composition.cclc_cell_route_publication_owner.reset();
  composition.cclc_lifecycle_product.reset();
  composition.cclc_provider_manifest_source.reset();
  composition.cclc_artifact_source.reset();
  composition.cclc_active_publication_reader.reset();
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

[[nodiscard]] core::NativeApplyParentPreparationFailureSource
MakeNativeApplyPreparationFailureSource(
    const ManagedRuntimeChildConfig &config,
    core::ResourceAddressedExecutionKernel &kernel) {
  // These roots belong to the main-scope managed child, and the stable Kernel
  // outlives every CCLC composition. No original-live effect validator is
  // reconstructed here: replacements may observe this immutable history.
  return [config, &kernel](const core::NativeApplyPlanDTO &plan)
             -> core::Result<bool> {
    const auto candidate = LoadManagedBundleAtPinnedRoot(
        config, plan.candidate_retained_bundle_id);
    if (!candidate) return core::Result<bool>::Ok(false);
    const auto manager_journal = config.manager_state_root /
        std::string(component_manager::kManagerApplyJournalFilename);
    const auto parent = component_manager::ReadNativeApplyParentStatusReadOnly(
        manager_journal, core::CellId(config.cell_id), plan, *candidate->bundle);
    if (!parent.ok()) return core::Result<bool>::Err(parent.error());
    const auto &stored = parent.value().intent.intent;
    // Read the existing full ledger entry rather than guessing the Q91 scope.
    const auto claim = stored.parent_authoring_claim
        ? stored.parent_authoring_claim
        : kernel.ReadHeldCellAdmissionClaim(
              core::CellAdmissionClaimKindDTO::kManagerApply);
    if (!claim) return core::Result<bool>::Ok(false);
    const core::CellAdmissionRecoveryReceiptQueryDTO query{
        claim->intent, *claim, std::nullopt};
    const auto receipt = kernel.GetCellAdmissionRecoveryReceipt(query);
    if (!receipt.ok()) return core::Result<bool>::Err(receipt.error());
    const auto observed =
        component_manager::ReadNativeApplyPreparationFailureReadOnly(
            manager_journal, core::CellId(config.cell_id), plan,
            *candidate->bundle, *claim, query, receipt.value());
    if (!observed.ok()) return core::Result<bool>::Err(observed.error());
    return core::Result<bool>::Ok(observed.value().has_value());
  };
}

// A borrowed compiler view, without composition/Scene/admission owners. The
// same resolver accepts this view and the actual CompositionBuildInput.
struct CandidatePhysicalBindingInput final {
  const std::shared_ptr<const deployment::RetainedBundle> &bundle;
  const deployment::VerifiedRetainedModelMap &model_map;
  const core::RuntimeEpoch &runtime_epoch;
  std::optional<core::ControllableComponentId> selected_component_id;
  const deployment::ProgramBinding *status_program_binding{};
  std::optional<core::TargetRoleId> status_motion_target_role_id{};
  std::optional<core::TargetRoleId> status_gripper_target_role_id{};
};

struct ManagedCandidateLifecycleComponentClosure final {
  core::ControllableComponentId component_id;
  std::vector<core::ExpectedControlDomainRevisionDTO> candidate_expected_domains;
  std::vector<core::ResolvedLifecycleControlDomainScopeDTO> qualified_candidate_domains;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> candidate_provider_occurrences;
  std::vector<core::BrandLifecyclePlanCatalogDTO> lifecycle_plan_catalogs;
};

struct ManagedCandidateLifecyclePlanningClosure final {
  core::ProjectionRevisionDTO candidate_projection_revision;
  core::ActiveBundlePublicationGeneration anticipated_publication_generation;
  // Compiled ActiveFacts floors/paths only, never installed or ready readback.
  std::vector<core::RuntimeParticipantBinding> compiled_physical_bindings;
  std::vector<ManagedCandidateLifecycleComponentClosure> components;
};

// Captured only by the original synchronous fresh Acquire branch. Durable
// plan/status reads on a replacement never reconstruct this live continuation.
// Its facade is borrowed from the main cell lifetime, which cannot be recreated
// until every accepted child and original managed caller has fully drained.
struct ManagedOriginalApplyBringUpCapture final {
  component_manager::PersistedManagerApplyBringUpPlan sealed_plan;
  core::CellAdmissionClaimDTO parent_claim;
  core::ActiveBundlePublicationReceiptDTO predecessor_publication;
  core::ProjectionRevisionDTO predecessor_projection_revision;
  ManagedBundleAtPinnedRoot predecessor;
  ManagedBundleAtPinnedRoot candidate;
  ManagedCandidateLifecyclePlanningClosure candidate_closure;
  deployment::CCLCRuntimeComposition *original_facade{};
};

[[nodiscard]] core::Result<ManagedCandidateLifecyclePlanningClosure>
BuildManagedCandidateLifecyclePlanningClosure(
    const ManagedRuntimeChildConfig &config,
    const ManagedBundleAtPinnedRoot &candidate,
    const core::ProjectionRevisionDTO &revision,
    const core::ActiveBundlePublicationGeneration anticipated_publication_generation) {
  using Closure = ManagedCandidateLifecyclePlanningClosure;
  try {
    Require(candidate.bundle && deployment::verify_retained_bundle_identity(*candidate.bundle) &&
                candidate.bundle->value().cell_graph.cell_id == core::CellId(config.cell_id) &&
                revision.valid() && revision.retained_bundle_id == candidate.bundle->retained_bundle_id() &&
                revision.graph_revision == candidate.bundle->value().cell_graph.graph_revision &&
                anticipated_publication_generation.nonzero() &&
                !config.runtime_unit_id.empty() && !config.runtime_epoch.empty(),
            "candidate lifecycle compiler inputs do not share the pinned product");
    const auto &bundle = *candidate.bundle;
    const auto &graph = bundle.value().cell_graph;
    const core::RuntimeUnitId runtime_unit_id(config.runtime_unit_id);
    const auto runtime_configuration =
        deployment::ros2_control_execution_runtime_contract::require_configuration(bundle, runtime_unit_id);
    // Validate host-local ownership before the resolver's ActiveFacts uses
    // this host epoch. A foreign retained unit must never receive it.
    for (const auto &component : graph.controllable_components) {
      const bool actual_io_driver = std::any_of(graph.control_domains.begin(), graph.control_domains.end(),
          [&](const auto& domain) {
            return domain.provider() && domain.provider()->controllable_component_id == component.controllable_component_id &&
                   domain.provider()->provider_runtime_unit_id == runtime_unit_id;
          });
      // A lane-free pneumatic tool (D4) has gripper rows but no controller.
      const bool lane_free_tool = std::any_of(
          runtime_configuration.gripper_target_configurations.begin(),
          runtime_configuration.gripper_target_configurations.end(), [&](const auto &row) {
            return row.controllable_component_id == component.controllable_component_id &&
                   row.actuation_kind == "pneumatic" &&
                   (row.pneumatic_io || row.output_mode == core::GripperOutputMode::kNone);
          });
      Require(std::find(component.runtime_unit_ids.begin(), component.runtime_unit_ids.end(), runtime_unit_id) !=
                  component.runtime_unit_ids.end() &&
                  (deployment::ros2_control_execution_runtime_contract::find_controller_profile(
                      runtime_configuration, component.controllable_component_id) != nullptr ||
                   actual_io_driver || lane_free_tool),
              "candidate component has no controller profile in this host runtime unit");
    }
    Require(std::all_of(graph.control_bindings.begin(), graph.control_bindings.end(),
                [&](const auto &binding) { return binding.provider_runtime_unit_id == runtime_unit_id; }),
            "candidate lifecycle provider belongs to another runtime unit");
    Closure result{revision, anticipated_publication_generation, {}, {}};
    if (graph.controllable_components.empty()) {
      Require(graph.control_bindings.empty() && graph.control_domains.empty() &&
                  bundle.value().desired_control_configurations.empty(),
              "empty candidate has nonempty lifecycle control membership");
      return core::Result<Closure>::Ok(std::move(result));
    }
    const auto &products = bundle.value().compiled_products;
    Require(products.symbol_role_maps.size() == 1U,
            "candidate lifecycle compiler requires the exact retained model map");
    const deployment::FilesystemVerifiedRetainedModelMapSource map_source(candidate.package_root);
    auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
        bundle.retained_bundle_id(), products.symbol_role_maps.front().product_id});
    Require(model_map.has_value(), "candidate retained model map is absent or invalid");
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(candidate.package_root);
    const auto inventory = deployment::compile_direct_control_program_binding_inventory(
        bundle, *model_map, catalog_store);
    const core::RuntimeEpoch runtime_epoch(config.runtime_epoch);
    std::map<core::ControllableComponentId, std::set<core::ControlDomainId>> component_domains;
    for (const auto &component : graph.controllable_components) {
      const auto [entry, inserted] = component_domains.emplace(
          component.controllable_component_id, std::set<core::ControlDomainId>{});
      Require(inserted, "candidate compiler has duplicate component membership");
      if (std::none_of(graph.control_bindings.begin(), graph.control_bindings.end(),
          [&](const auto& binding) { return binding.controllable_component_id == entry->first; }))
        continue;
      CandidatePhysicalBindingInput input{candidate.bundle, *model_map, runtime_epoch,
          entry->first, nullptr, std::nullopt, std::nullopt};
      auto physical = ResolveNativePhysicalBindings(input, inventory);
      result.compiled_physical_bindings.insert(result.compiled_physical_bindings.end(),
          std::make_move_iterator(physical.representatives.begin()),
          std::make_move_iterator(physical.representatives.end()));
    }
    for (const auto &binding : graph.control_bindings) {
      const auto component = component_domains.find(binding.controllable_component_id);
      Require(component != component_domains.end(), "candidate binding has no retained component");
      component->second.insert(binding.control_domain_id);
    }
    Require(std::all_of(result.compiled_physical_bindings.begin(), result.compiled_physical_bindings.end(),
                [](const auto &row) { return row.value().control_path.has_value(); }),
            "candidate compiler produced an unaddressed physical binding");
    std::sort(result.compiled_physical_bindings.begin(), result.compiled_physical_bindings.end(),
        [](const auto &lhs, const auto &rhs) {
          return lhs.value().control_path->control_binding_id < rhs.value().control_path->control_binding_id;
        });
    std::map<core::ControlBindingId, const core::RuntimeParticipantBinding *> compiled;
    for (const auto &binding : result.compiled_physical_bindings) {
      const auto &value = binding.value();
      Require(value.retained_bundle_id == bundle.retained_bundle_id() &&
                  value.graph_revision == graph.graph_revision && value.control_path &&
                  compiled.emplace(value.control_path->control_binding_id, &binding).second,
              "candidate compiled representatives are stale, absent or duplicated");
    }
    struct DomainClosure final {
      core::ExpectedControlDomainRevisionDTO expected;
      core::ResolvedLifecycleControlDomainScopeDTO scope;
      core::ResolvedLifecycleProviderOccurrenceDTO provider;
      core::BrandLifecyclePlanCatalogDTO catalog;
    };
    std::map<core::ControlDomainId, DomainClosure> domains;
    std::set<core::ControlBindingId> consumed;
    const deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(candidate.package_root);
    const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(candidate.package_root);
    for (const auto &desired : bundle.value().desired_control_configurations) {
      const auto domain = std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
          [&](const auto &row) { return row.control_domain_id == desired.control_domain_id; });
      Require(domain != graph.control_domains.end() && !domain->provider() &&
                  domain->control_domain_revision == desired.control_domain_revision &&
                  domain->controller().controller_topology_id == desired.controller_topology_id &&
                  domain->controller().topology_revision == desired.topology_revision,
              "candidate desired domain differs from its retained topology");
      std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
      std::set<core::ResourceId> resources;
      const core::RuntimeControlPathDTO *provider = nullptr;
      for (const auto &route : desired.binding_routes) {
        const auto representative = compiled.find(route.control_binding_id);
        Require(representative != compiled.end() && consumed.insert(route.control_binding_id).second,
                "candidate desired route has no unique compiled representative");
        const auto &value = representative->second->value();
        const auto &path = *value.control_path;
        const auto graph_binding = std::find_if(graph.control_bindings.begin(), graph.control_bindings.end(),
            [&](const auto &row) { return row.control_binding_id == route.control_binding_id; });
        Require(graph_binding != graph.control_bindings.end() &&
                    graph_binding->controllable_component_id == value.controllable_component_id &&
                    graph_binding->control_binding_revision == route.control_binding_revision &&
                    graph_binding->control_domain_id == desired.control_domain_id &&
                    graph_binding->controller_topology_id == desired.controller_topology_id &&
                    graph_binding->topology_revision == desired.topology_revision &&
                    graph_binding->command_target_id == value.command_target_id &&
                    graph_binding->capability_id == value.capability_id &&
                    graph_binding->provider_instance_id == path.provider_instance_id &&
                    graph_binding->provider_manifest_id == path.provider_manifest_id &&
                    graph_binding->provider_manifest_revision == path.provider_manifest_revision &&
                    graph_binding->provider_runtime_unit_id == path.provider_runtime_unit_id &&
                    std::find(graph_binding->legal_control_route_ids.begin(),
                              graph_binding->legal_control_route_ids.end(), route.control_route_id) !=
                        graph_binding->legal_control_route_ids.end() &&
                    path.control_domain_id == desired.control_domain_id &&
                    path.control_domain_revision == desired.control_domain_revision &&
                    path.controller_topology_id == desired.controller_topology_id &&
                    path.topology_revision == desired.topology_revision &&
                    path.control_binding_revision == route.control_binding_revision &&
                    path.control_route_id == route.control_route_id &&
                    path.provider_runtime_unit_id == runtime_unit_id &&
                    path.provider_runtime_epoch == runtime_epoch,
                "candidate compiled route differs from its retained binding/domain");
        if (provider) {
          Require(std::tie(provider->provider_instance_id, provider->provider_manifest_id,
                           provider->provider_manifest_revision, provider->provider_runtime_unit_id,
                           provider->provider_runtime_epoch) ==
                      std::tie(path.provider_instance_id, path.provider_manifest_id,
                               path.provider_manifest_revision, path.provider_runtime_unit_id,
                               path.provider_runtime_epoch),
                  "candidate domain spans multiple lifecycle provider occurrences");
        } else {
          provider = &path;
        }
        bindings.push_back({route.control_binding_id, route.control_binding_revision, route.control_route_id});
      }
      // Scope includes every domain binding/resource, including another
      // component's share. A selected-component filter would truncate it.
      std::size_t graph_binding_count = 0U;
      for (const auto &binding : graph.control_bindings) {
        if (binding.control_domain_id != desired.control_domain_id) continue;
        ++graph_binding_count;
        for (const auto &resource : binding.resource_bindings) resources.insert(resource.resource_id);
      }
      Require(provider && graph_binding_count == bindings.size() && !resources.empty(),
              "candidate desired domain does not close all retained bindings/resources");
      // Reuse the existing sealed product decoder; no synthetic topology or
      // controller catalog is accepted from an inspection request.
      static_cast<void>(deployment::load_verified_controller_lifecycle_expected_domain(
          bundle, artifacts, desired, provider->provider_runtime_unit_id, provider->provider_runtime_epoch));
      const auto manifest = manifests.get_verified({bundle.retained_bundle_id(),
          provider->provider_manifest_id, provider->provider_manifest_revision});
      Require(manifest.has_value(), "candidate retained lifecycle manifest is absent");
      auto catalog =
          make_retained_physical_ros2_control_lifecycle_catalog(manifest->value());
      if (!catalog.ok()) return core::Result<Closure>::Err(catalog.error());
      std::sort(bindings.begin(), bindings.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.control_binding_id < rhs.control_binding_id;
      });
      const core::ExpectedActiveControlConfigurationDTO absent_configuration{
          core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
      core::ExpectedControlDomainRevisionDTO expected{desired.control_domain_id,
          desired.control_domain_revision, desired.controller_topology_id, desired.topology_revision,
          absent_configuration, bindings};
      core::ResolvedLifecycleControlDomainScopeDTO scope{desired.control_domain_id,
          desired.control_domain_revision, desired.controller_topology_id, desired.topology_revision,
          std::nullopt, std::move(bindings), std::vector<core::ResourceId>(resources.begin(), resources.end())};
      core::ResolvedLifecycleProviderOccurrenceDTO occurrence{desired.control_domain_id,
          desired.control_domain_revision, provider->provider_instance_id, provider->provider_manifest_id,
          provider->provider_manifest_revision, provider->provider_runtime_unit_id,
          provider->provider_runtime_epoch, catalog.value().catalog_digest};
      Require(expected.locally_valid() && scope.locally_valid() && occurrence.locally_valid() &&
                  core::ExpectedDomainMatchesResolvedScope(expected, scope),
              "candidate lifecycle domain closure is not one valid exact value");
      Require(domains.emplace(desired.control_domain_id, DomainClosure{
          std::move(expected), std::move(scope), std::move(occurrence), std::move(catalog.value())}).second,
          "candidate has duplicate desired domains");
    }
    const auto io_catalog = deployment::ResolveRetainedIoSignalCatalog(graph);
    if (!io_catalog.ok()) return core::Result<Closure>::Err(io_catalog.error());
    for (const auto& domain : graph.control_domains) {
      const auto* driver = domain.provider();
      if (!driver) continue;
      Require(driver->provider_runtime_unit_id == runtime_unit_id &&
                  component_domains.contains(driver->controllable_component_id),
              "candidate provider domain has no exact local component/unit");
      const auto manifest = manifests.get_verified({bundle.retained_bundle_id(),
          driver->provider_manifest_id, driver->provider_manifest_revision});
      Require(manifest.has_value(), "candidate I/O lifecycle manifest is absent");
      auto catalog = core::MakeIoProviderLifecycleCatalog(manifest->value());
      if (!catalog.ok()) return core::Result<Closure>::Err(catalog.error());
      Require(catalog.value().catalog_digest == driver->lifecycle_plan_catalog_digest,
              "candidate I/O lifecycle catalog differs from its qualified domain");
      std::vector<core::ResourceId> outputs;
      for (const auto& row : io_catalog.value()) {
        if (row.runtime_unit_id == driver->provider_runtime_unit_id &&
            row.descriptor.signal_id.controllable_component_id == driver->controllable_component_id &&
            std::binary_search(driver->signal_names.begin(), driver->signal_names.end(), row.descriptor.signal_id.name) &&
            row.descriptor.output_resource)
          outputs.push_back(*row.descriptor.output_resource);
      }
      std::sort(outputs.begin(), outputs.end());
      outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
      const core::ExpectedActiveControlConfigurationDTO absent{
          core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
      core::ExpectedControlDomainRevisionDTO expected{domain.control_domain_id, domain.control_domain_revision,
          core::ExpectedProviderDomainScopeDTO{*driver, absent}};
      core::ResolvedLifecycleControlDomainScopeDTO scope{domain.control_domain_id, domain.control_domain_revision,
          core::ResolvedProviderDomainScopeDTO{*driver, runtime_epoch, std::nullopt, std::move(outputs)}};
      core::ResolvedLifecycleProviderOccurrenceDTO occurrence{domain.control_domain_id, domain.control_domain_revision,
          driver->provider_instance_id, driver->provider_manifest_id, driver->provider_manifest_revision,
          runtime_unit_id, runtime_epoch, catalog.value().catalog_digest};
      Require(expected.locally_valid() && scope.locally_valid() && occurrence.locally_valid() &&
                  core::ExpectedDomainMatchesResolvedScope(expected, scope),
              "candidate I/O lifecycle closure lacks exact provider proof");
      Require(domains.emplace(domain.control_domain_id, DomainClosure{std::move(expected), std::move(scope),
          std::move(occurrence), std::move(catalog.value())}).second, "candidate has duplicate provider domains");
      component_domains.at(driver->controllable_component_id).insert(domain.control_domain_id);
    }
    Require(consumed.size() == compiled.size() && domains.size() == graph.control_domains.size(),
            "candidate compiler has unused physical bindings or omitted domains");
    for (const auto &[component, ids] : component_domains) {
      Require(!ids.empty() && ids.size() <= core::kMaxLifecycleExpectedDomains,
              "candidate component has no bounded lifecycle domain closure");
      ManagedCandidateLifecycleComponentClosure closure{component, {}, {}, {}, {}};
      std::size_t binding_count = 0U;
      for (const auto &id : ids) {
        const auto domain = domains.find(id);
        Require(domain != domains.end(), "candidate component domain has no compiled lifecycle closure");
        const auto count = domain->second.expected.control_bindings().size();
        Require(count <= core::kMaxLifecycleExpectedBindings - binding_count,
                "candidate component binding closure exceeds the existing bound");
        binding_count += count;
        closure.candidate_expected_domains.push_back(domain->second.expected);
        closure.qualified_candidate_domains.push_back(domain->second.scope);
        closure.candidate_provider_occurrences.push_back(domain->second.provider);
        closure.lifecycle_plan_catalogs.push_back(domain->second.catalog);
      }
      result.components.push_back(std::move(closure));
    }
    return core::Result<Closure>::Ok(std::move(result));
  } catch (const std::exception &error) {
    return core::Result<Closure>::Err({"candidate_lifecycle_planning_failed", error.what(),
        {{"reason", "verified_candidate_closure_unavailable"}}});
  }
}

struct ManagedInstalledLifecycleComponentClosure final {
  std::vector<core::ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers;
  std::vector<core::BrandLifecyclePortRegistrationDTO> ports;
};

// Called only while the original live parent pins this installed Composition.
// No compiled desired configuration is substituted for missing Kernel truth.
[[nodiscard]] core::Result<ManagedInstalledLifecycleComponentClosure>
ReadInstalledManagedCandidateLifecycleComponentClosure(
    const Composition &composition,
    const ManagedCandidateLifecycleComponentClosure &qualified) {
  using Closure = ManagedInstalledLifecycleComponentClosure;
  try {
    Require(composition.cclc_lifecycle_product && composition.active_addressed_kernel() &&
                IndexNativeComponentCompositions(composition).contains(qualified.component_id) &&
                !qualified.qualified_candidate_domains.empty() &&
                qualified.qualified_candidate_domains.size() == qualified.candidate_provider_occurrences.size() &&
                qualified.qualified_candidate_domains.size() == qualified.lifecycle_plan_catalogs.size(),
            "installed lifecycle component has no actual product/Kernel closure");
    const auto physical_bindings = PhysicalControlBindings(composition);
    const auto &product = *composition.cclc_lifecycle_product;
    Closure result;
    for (std::size_t index = 0U; index < qualified.qualified_candidate_domains.size(); ++index) {
      const auto &candidate = qualified.qualified_candidate_domains[index];
      const auto port = std::find_if(product.brand_ports().begin(), product.brand_ports().end(),
          [&](const auto &row) { return row.provider_occurrence.control_domain_id == candidate.control_domain_id; });
      Require(port != product.brand_ports().end() && port->port &&
                  port->provider_occurrence == qualified.candidate_provider_occurrences[index],
              "installed lifecycle provider differs from the qualified candidate");
      const auto &provider = port->provider_occurrence;
      const auto catalog = product.catalog_source().LoadExactLifecyclePlanCatalog(
          provider.provider_manifest_id, provider.provider_manifest_revision,
          provider.lifecycle_plan_catalog_digest);
      Require(catalog.ok() && catalog.value() == qualified.lifecycle_plan_catalogs[index],
              "installed lifecycle catalog differs from the qualified candidate");
      if (candidate.provider()) {
        const auto owners = IndexNativeComponentCompositions(composition);
        const auto actual_owner = owners.find(qualified.component_id);
        Require(actual_owner != owners.end() && actual_owner->second->io_provider_drivers,
                "installed provider has no complete actual driver group");
        const auto& group = *actual_owner->second->io_provider_drivers;
        const auto driver = std::find_if(group.begin(), group.end(), [&](const auto& record) {
          return record && record->domain() == candidate && record->occurrence() == provider;
        });
        Require(driver != group.end() && (*driver)->resources_installed(),
                "installed provider domain has no exact actual driver resources");
        // Structural installed identity is independent of the map being
        // bootstrapped and of Enable/readiness. The original owner validates
        // this closure before the held-fence CLOSED map publication.
        result.domains.push_back((*driver)->domain());
        result.providers.push_back(provider);
        result.ports.push_back(*port);
        continue;
      }
      const auto current = composition.active_addressed_kernel()->current_runtime_route_configuration(
          candidate.control_domain_id);
      Require(current.ok() && current.value(),
              "installed lifecycle domain has no actual Kernel configuration");
      const auto &configuration = current.value()->active_configuration;
      Require(configuration.locally_valid() &&
                  configuration.state == core::ActiveControlConfigurationStateDTO::kReconciled &&
                  configuration.source_runtime_unit_id == provider.provider_runtime_unit_id &&
                  configuration.source_runtime_epoch == provider.provider_runtime_epoch &&
                  current.value()->readiness_rows.size() == configuration.binding_routes.size(),
              "installed lifecycle route has another provider or incomplete Stop readiness");
      std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
      std::set<core::ResourceId> resources;
      for (const auto &route : configuration.binding_routes) {
        bool found = false;
        for (const auto &physical : physical_bindings) {
          const auto &value = physical.value();
          if (!value.control_path || value.control_path->control_domain_id != candidate.control_domain_id ||
              value.control_path->control_binding_id != route.control_binding_id) continue;
          const auto &path = *value.control_path;
          Require(value.retained_bundle_id == composition.bundle->retained_bundle_id() &&
                      value.graph_revision == composition.bundle->value().cell_graph.graph_revision &&
                      path.control_domain_revision == configuration.control_domain_revision &&
                      path.controller_topology_id == configuration.controller_topology_id &&
                      path.topology_revision == configuration.topology_revision &&
                      path.control_binding_revision == route.control_binding_revision &&
                      path.control_route_id == route.control_route_id &&
                      path.activation_set_id == configuration.activation_set_id &&
                      path.active_control_configuration_id == configuration.active_control_configuration_id &&
                      path.active_control_configuration_generation == configuration.active_control_configuration_generation &&
                      path.provider_instance_id == provider.provider_instance_id &&
                      path.provider_manifest_id == provider.provider_manifest_id &&
                      path.provider_manifest_revision == provider.provider_manifest_revision &&
                      path.provider_runtime_unit_id == provider.provider_runtime_unit_id &&
                      path.provider_runtime_epoch == provider.provider_runtime_epoch,
                  "installed physical binding differs from its current Kernel route/provider");
          found = true;
          for (const auto &resource : value.resources) resources.insert(resource.resource_id);
        }
        Require(found, "installed Kernel route has no actual physical binding");
        bindings.push_back({route.control_binding_id, route.control_binding_revision, route.control_route_id});
      }
      core::ResolvedLifecycleControlDomainScopeDTO actual{
          configuration.control_domain_id, configuration.control_domain_revision,
          configuration.controller_topology_id, configuration.topology_revision, configuration,
          std::move(bindings), std::vector<core::ResourceId>(resources.begin(), resources.end())};
      auto expected = candidate;
      expected.controller().active_control_configuration = configuration;
      Require(actual.locally_valid() && actual == expected,
              "installed lifecycle domain closure differs from the qualified candidate");
      result.domains.push_back(std::move(actual));
      result.providers.push_back(provider);
      result.ports.push_back(*port);
    }
    return core::Result<Closure>::Ok(std::move(result));
  } catch (const std::exception &error) {
    return core::Result<Closure>::Err({"installed_lifecycle_closure_unavailable", error.what(),
        {{"reason", "actual_installed_product_or_route_unavailable"}}});
  }
}

// The managed cell keeps the actual facade and its immutable construction
// dependencies independently of its current physical Composition. Moving
// these owners does not rotate the namespace, rebuild a registry or change
// accepted request evidence. The pinned runtimes keep the old borrowed route
// and observation ports alive until that facade has completely stopped.
// One installed product lifetime, owned by the immutable CCLC context. The
// current/retired Composition and this pin share the same compiled ROS product.
// Product callbacks retain actual installed runtime owners, never a mutable
// pointer to whichever Composition later becomes current.
struct InstalledManagedCCLCProductLifetime final {
  std::shared_ptr<WallClock> clock;
  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtimes;
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> grippers;
  std::vector<std::shared_ptr<const ComponentRuntimePins>> component_pins;
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> projections;
  std::vector<std::shared_ptr<ControllerLaunchProcess>> processes;
  std::shared_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      provider_source;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifacts;
  std::optional<core::BuildOwnedLifecyclePlanCatalogSource> empty_catalog;
  std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>
      empty_route_publication_owner;
  std::shared_ptr<deployment::IRouteConfigurationKernelPublicationOwner> component_route_owner;
  std::shared_ptr<RetainedNativeCellCommandReadinessPort> component_readiness_owner;
  // Destroy the borrowing compiled product before its provider source.
  std::shared_ptr<RetainedHostLifecycleProduct> lifecycle_product;
};

// The same verified empty dependencies serve initial construction and later
// preparation on the existing facade. They never manufacture a physical owner.
[[nodiscard]] std::shared_ptr<InstalledManagedCCLCProductLifetime>
MakeEmptyInstalledManagedCCLCProductLifetime(
    const ManagedRuntimeChildConfig &config,
    const std::shared_ptr<const deployment::RetainedBundle> &bundle,
    const core::ActiveBundlePublicationReceiptDTO &head,
    const std::shared_ptr<WallClock> &clock) {
  Require(bundle && clock && head.valid() &&
              head.request.candidate_retained_bundle_id == bundle->retained_bundle_id() &&
              head.request.candidate_graph_revision == bundle->value().cell_graph.graph_revision &&
              bundle->value().cell_graph.cell_id == core::CellId(config.cell_id),
          "empty cell product dependencies lost their exact bundle/head");
  auto route = MakeVerifiedNoControllerRoutePublicationOwner(*bundle);
  Require(route.ok(), "empty cell CCLC product is not genuinely empty");
  auto catalog = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  Require(catalog.ok(), "empty cell lifecycle catalog cannot be constructed");
  auto lifetime = std::make_shared<InstalledManagedCCLCProductLifetime>();
  lifetime->clock = clock;
  lifetime->publication_reader =
      std::make_unique<component_manager::SqliteActiveBundlePublicationStore>(
          config.manager_state_root /
              std::string(component_manager::kActiveBundlePublicationStoreFilename),
          core::CellId(config.cell_id));
  const auto integrity = lifetime->publication_reader->VerifyIntegrity();
  const auto current = lifetime->publication_reader->ReadActiveBundlePublication(
      integrity.current_generation);
  Require(current.ok() && current.value() == head,
          "empty cell CCLC publication changed before construction");
  const auto selected = LoadManagedBundleAtPinnedRoot(config, bundle->retained_bundle_id());
  Require(selected && selected->bundle->canonical_bytes() == bundle->canonical_bytes(),
          "empty cell CCLC product is not at its pinned root");
  lifetime->artifacts = std::make_unique<
      deployment::FilesystemVerifiedRetainedProductArtifactSource>(selected->package_root);
  lifetime->empty_catalog = std::move(catalog.value());
  lifetime->empty_route_publication_owner = std::move(route.value());
  return lifetime;
}

struct ManagedCellCCLCLifetime final {
  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtime_pins;
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> gripper_pins;
  std::vector<std::shared_ptr<const ComponentRuntimePins>> component_pins;
  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      active_publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifact_source;
  std::shared_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      provider_manifest_source;
  std::optional<core::BuildOwnedLifecyclePlanCatalogSource> empty_catalog;
  std::shared_ptr<RetainedHostLifecycleProduct> lifecycle_product;
  std::shared_ptr<deployment::IRouteConfigurationKernelPublicationOwner>
      route_publication_owner;
  std::shared_ptr<RetainedNativeCellCommandReadinessPort> readiness_port;
  // Immutable construction head. Candidate installation never retargets the
  // original owner or retroactively changes its accepted request evidence.
  std::optional<core::ActiveBundlePublicationReceiptDTO> publication;
  // Original preparation for the existing installed-request reconciliation
  // lane. The facade remains the only current-context owner.
  std::shared_ptr<const deployment::CCLCRuntimeComposition::PreparedProductContext>
      pending_product;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      pending_product_request;
  std::optional<core::RequestOwnerEpoch> pending_product_owner;
  // Declared last: stop/drain the real facade before any borrowed dependency.
  std::unique_ptr<deployment::CCLCRuntimeComposition> facade;

  [[nodiscard]] core::Result<std::shared_ptr<const
      deployment::CCLCRuntimeComposition::PreparedProductContext>>
  PrepareInstalledProduct(Composition &composition,
                          const ManagedRuntimeChildConfig &config,
                          const core::ActiveBundlePublicationReceiptDTO &head) {
    Require(facade && clock && composition.component_runtime_identity() &&
                composition.cell_runtime && composition.cell_runtime->clock() == clock && head.valid() &&
                head.publication_generation == composition.publication_generation &&
                head.request.candidate_retained_bundle_id ==
                    composition.bundle->retained_bundle_id() &&
                head.request.candidate_projection_revision == composition.projection_revision,
            "CCLC product preparation lost its actual installed owner or publication");
    const auto selected = LoadManagedBundleAtPinnedRoot(
        config, composition.bundle->retained_bundle_id());
    Require(selected && selected->bundle->canonical_bytes() ==
                            composition.bundle->canonical_bytes(),
            "CCLC product preparation lost the pinned candidate package");
    EnsureInstalledRetainedHostLifecycleProduct(composition, selected->package_root);
    auto lifetime = std::make_shared<InstalledManagedCCLCProductLifetime>();
    lifetime->clock = clock;
    lifetime->provider_source = composition.cclc_provider_manifest_source;
    lifetime->lifecycle_product = composition.cclc_lifecycle_product;
    lifetime->publication_reader = std::make_unique<
        component_manager::SqliteActiveBundlePublicationStore>(
        config.manager_state_root /
            std::string(component_manager::kActiveBundlePublicationStoreFilename),
        core::CellId(config.cell_id));
    lifetime->artifacts = std::make_unique<
        deployment::FilesystemVerifiedRetainedProductArtifactSource>(selected->package_root);
    for (const auto &[id, view] : IndexNativeComponentCompositions(composition)) {
      static_cast<void>(id);
      Require(view && view->component_runtime_identity() &&
                  (!view->runtime || view->runtime->clock == clock) &&
                  (!(view->runtime || view->gripper_runtime) ||
                   ((view->controller_process || IsLocalNoOutputGripperOwner(*view)) &&
                    view->ros2_control_runtime_handle)),
              "CCLC installed product has an incomplete physical lifetime");
      if (view->runtime) lifetime->runtimes.push_back(view->runtime);
      if (view->gripper_runtime) lifetime->grippers.push_back(view->gripper_runtime);
      if (view->component_runtime_pins) lifetime->component_pins.push_back(view->component_runtime_pins);
      if (view->controller_process)
        lifetime->processes.push_back(view->controller_process);
      lifetime->projections.insert(lifetime->projections.end(),
          view->required_projection_pins.begin(), view->required_projection_pins.end());
    }
    Require((!lifetime->runtimes.empty() || !lifetime->grippers.empty() || !lifetime->component_pins.empty()) && lifetime->provider_source &&
                lifetime->lifecycle_product,
            "CCLC installed product has no genuine occurrence closure");
    const auto runtime = composition.runtime;
    deployment::CCLCRuntimeCompositionConfig context_config{
        config.execution_state_root, config.execution_run_root,
        core::CellId(config.cell_id),
        {facade->coordinator().request_owner_epoch(), 1024U,
         kCCLCLifecycleWorkerCount, kCCLCObservationPollPeriodNs,
         30'000'000'000LL}};
    // This reuses the existing native derivation's freshness/configuration
    // qualification only. It never constructs or starts another owner.
    if (!runtime) {
      EnsureGripperOnlyCellPorts(composition);
      lifetime->component_route_owner = composition.cclc_cell_route_publication_owner;
      lifetime->component_readiness_owner = composition.cclc_cell_command_readiness_port;
      auto* const kernel = composition.active_addressed_kernel();
      auto prepared = PrepareRetainedNativeCCLCProductContext(
          *facade, WithInstalledReadinessConstruction({std::move(context_config), 1'000'000'000LL},
              composition, selected->package_root), composition.bundle,
          *lifetime->publication_reader, head, *lifetime->artifacts,
          lifetime->lifecycle_product->catalog_source(), lifetime->lifecycle_product->brand_ports(),
          CellControlBindingRepresentatives(composition), *clock, *lifetime->component_route_owner, lifetime,
          [kernel](const core::ControlDomainId& domain) {
            auto current = kernel->current_runtime_route_configuration(domain);
            if (!current.ok()) return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Err(current.error());
            return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
                current.value() ? std::optional(current.value()->active_configuration) : std::nullopt);
          }, lifetime->lifecycle_product->lifecycle_snapshot_source(), lifetime->component_readiness_owner.get());
      if (prepared.ok()) PrepareInstalledMeasuredSourcesAfterContext(composition);
      return prepared;
    }
    auto prepared = PrepareRetainedNativeCCLCProductContext(
        *facade, WithInstalledReadinessConstruction(
            {std::move(context_config), 1'000'000'000LL}, composition,
            selected->package_root), composition.bundle,
        *lifetime->publication_reader, head, *lifetime->artifacts,
        lifetime->lifecycle_product->catalog_source(),
        lifetime->lifecycle_product->brand_ports(),
        CellControlBindingRepresentatives(composition), *clock, *runtime, lifetime,
        [runtime](const core::ControlDomainId &domain) {
          return runtime->ReadCurrentPublishedRouteConfiguration(domain);
        }, lifetime->lifecycle_product->lifecycle_snapshot_source(), runtime.get());
    if (prepared.ok()) PrepareInstalledMeasuredSourcesAfterContext(composition);
    return prepared;
  }

  [[nodiscard]] core::Result<std::shared_ptr<const
      deployment::CCLCRuntimeComposition::PreparedProductContext>>
  PrepareEmptyProduct(const ManagedRuntimeChildConfig &config,
                      std::shared_ptr<const deployment::RetainedBundle> bundle,
                      const core::ActiveBundlePublicationReceiptDTO &head) {
    Require(facade && clock, "empty product preparation lost its original facade");
    auto lifetime = MakeEmptyInstalledManagedCCLCProductLifetime(config, bundle, head, clock);
    return facade->PrepareProductContext({
        std::move(bundle), *lifetime->publication_reader, head, *lifetime->artifacts,
        *lifetime->empty_catalog, {}, {}, {}, {}, {}, {}, nullptr,
        *lifetime->empty_route_publication_owner, lifetime});
  }

  void TakeAttached(Composition &composition,
                    const core::ActiveBundlePublicationReceiptDTO &head) {
    Require(!facade && composition.cclc_runtime && composition.cell_runtime &&
                composition.component_runtime_identity() &&
                composition.cclc_attached.load(std::memory_order_acquire),
            "cell CCLC transfer requires one actual attached facade");
    Require(head.valid() &&
                head.publication_generation == composition.publication_generation &&
                head.request.candidate_projection_revision == composition.projection_revision &&
                head.request.candidate_retained_bundle_id == composition.bundle->retained_bundle_id(),
            "cell CCLC transfer differs from its immutable construction head");
    std::vector<std::shared_ptr<RetainedNativeRuntime>> pins;
    std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> tools;
    std::vector<std::shared_ptr<const ComponentRuntimePins>> groups;
    const auto pin = [&](const Composition& owner) {
      Require(owner.component_runtime_identity(), "cell CCLC transfer has an incomplete component dependency");
      if (owner.runtime) pins.push_back(owner.runtime);
      if (owner.gripper_runtime) tools.push_back(owner.gripper_runtime);
      if (owner.component_runtime_pins) groups.push_back(owner.component_runtime_pins);
    };
    pin(composition);
    for (const auto& peer : composition.participant_compositions) {
      Require(peer != nullptr, "cell CCLC transfer has a missing peer");
      pin(*peer);
    }
    publication = head;
    runtime_pins = std::move(pins);
    gripper_pins = std::move(tools);
    component_pins = std::move(groups);
    clock = composition.cell_runtime->clock();
    scheduler = composition.cell_runtime->scheduler();
    active_publication_reader =
        std::move(composition.cclc_active_publication_reader);
    artifact_source = std::move(composition.cclc_artifact_source);
    provider_manifest_source =
        std::move(composition.cclc_provider_manifest_source);
    lifecycle_product = std::move(composition.cclc_lifecycle_product);
    route_publication_owner =
        std::move(composition.cclc_cell_route_publication_owner);
    readiness_port = std::move(composition.cclc_cell_command_readiness_port);
    facade = std::move(composition.cclc_runtime);
  }

  void CreateEmpty(const ManagedRuntimeChildConfig &config,
                   std::shared_ptr<const deployment::RetainedBundle> bundle,
                   const core::ActiveBundlePublicationReceiptDTO &head,
                   core::ResourceAddressedExecutionKernel &kernel,
                   ManagedKernelApplyFenceOwner &fence_owner,
                   std::shared_ptr<WallClock> owner_clock,
                   std::shared_ptr<SimpleScheduler> owner_scheduler,
                   const std::optional<core::RequestOwnerEpoch> &quiescent_predecessor = {}) {
    Require(!facade && bundle && owner_clock && owner_scheduler,
            "empty cell CCLC requires the original cell dependencies");
    auto lifetime = MakeEmptyInstalledManagedCCLCProductLifetime(config, bundle, head, owner_clock);
    clock = std::move(owner_clock);
    scheduler = std::move(owner_scheduler);
    active_publication_reader = std::move(lifetime->publication_reader);
    artifact_source = std::move(lifetime->artifacts);
    empty_catalog = std::move(lifetime->empty_catalog);
    route_publication_owner = std::move(lifetime->empty_route_publication_owner);
    deployment::CCLCRuntimeCompositionConfig cclc_config{
        config.execution_state_root, config.execution_run_root,
        core::CellId(config.cell_id),
        {core::RequestOwnerEpoch(RandomToken("cclc-owner-")), 1024U,
         kCCLCLifecycleWorkerCount, kCCLCObservationPollPeriodNs,
         30'000'000'000LL}};
    cclc_config.coordinator.quiescent_predecessor = quiescent_predecessor;
    cclc_config.preparation_failure_source =
        MakeNativeApplyPreparationFailureSource(config, kernel);
    cclc_config.lifecycle_target_driver.stream_health = host_component_stream_health();
    auto created = deployment::CCLCRuntimeComposition::Create(
        std::move(cclc_config), std::move(bundle), *active_publication_reader,
        head, *artifact_source, *empty_catalog, {}, {}, {}, {}, {},
        kernel, fence_owner, *route_publication_owner, *clock, *scheduler);
    if (!created.ok()) Fail("empty cell CCLC construction failed: " + created.error().message);
    publication = head;
    facade = std::move(created.value());
    const auto started = facade->Start();
    if (!started.ok()) Fail("empty cell CCLC start failed: " + started.error().message);
  }
};

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

[[nodiscard]] bool AttachCurrentPublishedCCLCIfReady(
    Composition &composition, const ManagedRuntimeChildConfig &config,
    ManagedKernelApplyFenceOwner &fence_owner,
    const std::optional<core::ConfigurationFenceStatusQueryDTO> &parent_apply =
        std::nullopt,
    const std::function<core::Result<void>()> &restore_global_rules = {}) {
  auto &kernel = *composition.active_addressed_kernel();
  if (composition.cclc_attached.load(std::memory_order_acquire))
    return true;
  Require(kernel.SetRouteRecoveryPending(true).ok(),
          "route attachment could not close admission");
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  publication_store.Close();
  if (integrity.current_generation != composition.publication_generation) {
    return false;
  }
  Require(publication.ok() && publication.value().valid(),
          "native CCLC attachment cannot verify current publication");
  const auto active_bundle = LoadManagedBundleAtPinnedRoot(
      config, publication.value().request.candidate_retained_bundle_id);
  Require(active_bundle.has_value(),
          "native CCLC attachment cannot load its published retained bundle");
  HostConfig attachment_config;
  attachment_config.package_root = active_bundle->package_root;
  attachment_config.manager_state_root = config.manager_state_root;
  attachment_config.execution_state_root = config.execution_state_root;
  attachment_config.execution_run_root = config.execution_run_root;
  attachment_config.candidate_worker = config.candidate_worker;
  attachment_config.cell_id = config.cell_id;
  attachment_config.preparation_failure_source =
      MakeNativeApplyPreparationFailureSource(config, kernel);
  AttachRetainedNativeCCLC(composition, attachment_config, fence_owner,
                           parent_apply, std::nullopt, restore_global_rules);
  return composition.cclc_attached.load(std::memory_order_acquire);
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
    ManagedKernelAdmissionGate &admission_gate,
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
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
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
      &admission_gate,
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      true,
      {},
      {},
      config.manager_state_root,
      config.manager_run_root,
      config.logical_scene_database,
      config.program_cycle_journal,
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

[[nodiscard]] bool CurrentPublicationIsInstalledSuccessor(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  const core::ActiveBundlePublicationGeneration successor_generation(
      predecessor_generation + 1U);
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  if (integrity.current_generation ==
      request.expected_predecessor_publication_generation) {
    publication_store.Close();
    return false;
  }
  Require(integrity.current_generation == successor_generation,
          "managed composition public pointer is neither predecessor nor "
          "exact successor");
  const auto publication =
      publication_store.ReadActiveBundlePublication(successor_generation);
  publication_store.Close();
  Require(publication.ok() && publication.value().valid(),
          "managed composition successor publication cannot be verified");
  const auto *transaction =
      std::get_if<core::NamedApplyPublicationTransactionDTO>(
          &publication.value().request.transaction_ref);
  Require(transaction != nullptr && transaction->apply_id == request.apply_id &&
              transaction->apply_generation == request.apply_generation &&
              publication.value().request.candidate_projection_revision ==
                  request.candidate_projection_revision &&
              publication.value()
                      .request.expected_previous_publication_generation ==
                  std::optional<core::ActiveBundlePublicationGeneration>{
                      request.expected_predecessor_publication_generation},
          "managed composition successor belongs to another Apply");
  return true;
}

[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadExactManagedCandidateBundle(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  Require(request.valid() &&
              request.runtime_unit_id.value() == config.runtime_unit_id &&
              request.runtime_epoch.value() == config.runtime_epoch,
          "managed composition request names another runtime child");
  auto selected = LoadManagedBundleAtPinnedRoot(
      config, request.candidate_projection_revision.retained_bundle_id);
  const auto bundle = selected.has_value() ? selected->bundle : nullptr;
  const auto canonical_bytes =
      bundle != nullptr ? bundle->canonical_bytes() : std::string_view{};
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->value().cell_graph.graph_revision ==
                  request.candidate_projection_revision.graph_revision &&
              bundle->retained_bundle_id() ==
                  request.candidate_projection_revision.retained_bundle_id &&
              !canonical_bytes.empty() &&
              canonical_bytes.size() ==
                  request.retained_bundle_canonical_byte_size &&
              core::ContentDigest(
                  content_identity::sha256_digest(canonical_bytes)) ==
                  request.retained_bundle_canonical_content_digest,
          "managed selected retained bundle identity or materialization "
          "differs");
  return selected;
}

// The same actual ordinary-plan source for CLOSED staging and observation.
// Caller pins the installed composition/facade; this performs no mutation.
[[nodiscard]] std::vector<core::InstalledProviderCandidateFactsDTO>
ReadInstalledNativeProviderCandidateFacts(
    const ManagedRuntimeChildConfig &config, const ManagedBundleAtPinnedRoot &selected,
    const Composition &candidate, deployment::CCLCRuntimeComposition &facade,
    const core::NativeApplyPlanDTO &plan, const core::ConfigurationFenceScopeDTO &scope) {
  Require(candidate.bundle && candidate.cclc_lifecycle_product &&
              candidate.bundle->canonical_bytes() == selected.bundle->canonical_bytes(),
          "installed NativeApply observation lost its exact candidate product");
  const auto qualified = BuildManagedCandidateLifecyclePlanningClosure(
      config, selected, candidate.projection_revision, candidate.publication_generation);
  Require(qualified.ok(), "installed NativeApply observation has no qualified lifecycle closure");
  std::set<core::ControllableComponentId> components;
  for (const auto &domain : scope.domains) {
    const auto *sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
    if (sides && sides->candidate_provider)
      components.insert(sides->candidate_provider->controllable_component_id);
  }
  std::vector<core::InstalledProviderCandidateFactsDTO> facts;
  for (const auto &component : components) {
    const auto member = std::find_if(qualified.value().components.begin(), qualified.value().components.end(),
        [&](const auto &row) { return row.component_id == component; });
    Require(member != qualified.value().components.end(), "installed NativeApply provider component is foreign");
    const auto installed = ReadInstalledManagedCandidateLifecycleComponentClosure(candidate, *member);
    Require(installed.ok(), "installed NativeApply resources/catalog/ports differ");
    const auto actual = facade.ReadInstalledNativeApplyCandidateFacts(
        plan, *candidate.bundle, candidate.cclc_lifecycle_product->catalog_source(), component,
        installed.value().domains, installed.value().providers, installed.value().ports);
    Require(actual.ok(), "original NativeApply lifecycle owner refused installed facts");
    facts.push_back(actual.value());
  }
  return facts;
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
    ManagedKernelAdmissionGate &admission_gate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
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
              deployment::derive_apply_component_delta(*predecessor, *bundle) ==
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
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
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
      &admission_gate,
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      false,
      std::move(shared_clock),
      std::move(shared_scheduler),
      config.manager_state_root,
      config.manager_run_root,
      config.logical_scene_database,
      config.program_cycle_journal,
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

[[nodiscard]] bool IsExactRetainedNativeViewApply(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
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

void RefreshRetainedNativePublicationView(
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
      Require(composition->io_provider_drivers && !projections.contains(component_id),
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
      "manyforge.native-execution-ready.v2 " + config.runtime_unit_id + " " +
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
          config.package_root,
          core::CellId(config.cell_id),
          core::RuntimeUnitId(config.runtime_unit_id),
          core::RuntimeEpoch(config.runtime_epoch),
          core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
          [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
          },
          config.program_artifact_root,
          [&](const core::CellAdmissionRecoveryReceiptQueryDTO &query) {
            if (stable_kernel_owner == nullptr) {
              return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
                  {"selected_kernel_owner_unavailable", "cell Kernel is not installed", {}});
            }
            // The managed owner already holds the admission exclusion. This
            // existing read takes only the claim owner's mutex, preserving
            // fence -> admission -> claim order and the original Kernel life.
            return stable_kernel_owner->GetCellAdmissionRecoveryReceipt(query);
          },
          [&](const core::ControlDomainId &domain) {
            using Reply = core::Result<std::optional<
                core::RuntimeProviderDomainConfigurationReadbackDTO>>;
            if (stable_kernel_owner == nullptr)
              return Reply::Err({"selected_kernel_owner_unavailable",
                                 "cell Kernel is not installed",
                                 {}});
            // The fence owner retains admission; this getter takes only state.
            return stable_kernel_owner->current_runtime_provider_configuration(
                domain);
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
  bool prepared_managed_composition_is_empty = false;
  // A Program/Scene-only Apply keeps the exact component owners. Preparation
  // verifies the candidate product and projections without constructing
  // replacement controllers; installation refreshes only their publication
  // views beneath the Manager fence.
  std::optional<CompositionBuildInput> prepared_retained_view;
  std::shared_ptr<WallClock> detached_owner_clock;
  std::shared_ptr<SimpleScheduler> detached_owner_scheduler;
  NativeShutdownOwnerPhase scheduler_destructor_phase{
      shutdown_projection, ShutdownPhase::kSchedulerOwnerRelease,
      detached_owner_scheduler};
  // The cell Kernel's recovery resolver captures these by reference, so they
  // must outlive the cell owner and its Kernel at local-scope teardown.
  std::atomic<RetainedNativeRuntime *> stop_recovery_runtime{nullptr};
  std::atomic<core::ResourceAddressedExecutionKernel *> stop_recovery_kernel{
      nullptr};
  // The stable addressed Kernel borrows this observer from construction until
  // destruction, including intervals with no installed component. Declare
  // it before the Kernel so destruction retires the Kernel first.
  std::shared_ptr<RetainedStopSurveillanceOwner>
      stable_stop_surveillance_owner;
  NativeShutdownOwnerPhase surveillance_destructor_phase{
      shutdown_projection, ShutdownPhase::kStopSurveillanceDestruction,
      stable_stop_surveillance_owner};
  std::unique_ptr<RetainedNativeCellRuntime> stable_cell_runtime_owner;
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
  // Facade and ingress threads are destroyed before this guard; the stable
  // cell clock remains alive until after generic callbacks have quiesced.
  ManagedCellCCLCLifetime managed_cell_cclc;
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
  bootstrap_kernel_config.stop_fault_recovery_facts_resolver =
      [&](const core::StopStatusDTO &status, const std::int64_t assessed_at_ns)
      -> core::Result<core::StopFaultRecoveryFactsDTO> {
    // Execute retains this actual composition's shared access pin. Core
    // resolves these facts outside its state/admission locks; do not acquire
    // the access mutex recursively. The CCLC owner checks both durable open
    // incidents (including removed components) and its private pending state.
    auto *facade = managed_cell_cclc.facade.get();
    if (!facade && active_managed_composition)
      facade = active_managed_composition->cclc_runtime.get();
    if (!facade)
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kMissingReference)),
          "managed CCLC incident owner is unavailable",
          {{"reason", "managed_cclc_incident_owner_unavailable"}}});
    const auto clear = facade->lifecycle_target_driver().CheckNoOpenIncidents();
    if (!clear.ok())
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(clear.error());
    auto *const runtime = stop_recovery_runtime.load(std::memory_order_acquire);
    auto *const kernel = stop_recovery_kernel.load(std::memory_order_acquire);
    if (runtime == nullptr && stable_cell_runtime_owner && kernel) {
      return build_retained_cell_stop_recovery_facts(*stable_stop_surveillance_owner, *kernel,
          *stable_cell_runtime_owner->clock(), core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
          core::CellId(config.cell_id), &admission_gate, status, assessed_at_ns);
    }
    if (runtime == nullptr || kernel == nullptr) {
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kMissingReference)),
          "managed native Stop recovery owner is unavailable",
          {{"reason", "managed_native_stop_recovery_owner_unavailable"}}});
    }
    return build_retained_native_stop_recovery_facts(
        *runtime, *kernel, &admission_gate, status, assessed_at_ns);
  };
  bootstrap_kernel_config.io_output_local_resynchronization_source =
      [&](const core::RuntimeParticipantBindingDTO &binding) {
        // The only host Reset ingress retains execution_composition's shared
        // pin through Execute. Do not recursively acquire that shared_mutex.
        // Kernel calls this read outside its state/admission locks.
        auto *facade = managed_cell_cclc.facade.get();
        if (!facade && active_managed_composition)
          facade = active_managed_composition->cclc_runtime.get();
        if (!facade)
          return core::Result<core::IoOutputLocalResynchronizationProofDTO>::Err(
              {"io_output_local_resynchronization_unavailable", "Actual CCLC owner is unavailable", {}});
        return facade->coordinator().ReadIoOutputLocalResynchronization(binding);
      };
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
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.logical_scene_database, config.program_cycle_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_program_reset_status_source =
      [path = config.program_cycle_journal](const auto purpose, const auto& intent, const auto& claim) {
        return detail::ReadProgramResetStatusReadOnly(path, purpose, intent, claim);
      };
  RetainedNativeCellRuntimeInput cell_input{
      .clock = detached_owner_clock,
      .scheduler = detached_owner_scheduler,
      .operation_journal_path =
          config.execution_state_root / std::string(core::kOperationJournalFilename),
      .participant_registrations = {},
      .kernel_config = std::move(bootstrap_kernel_config),
      .stop_sources = {}};
  stable_cell_runtime_owner = std::make_unique<RetainedNativeCellRuntime>(std::move(cell_input));
  stable_stop_surveillance_owner = stable_cell_runtime_owner->stop_surveillance_owner();
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
  const auto bind_cell_runtime_to_composition =
      [&](Composition &composition) {
        bind_active_tcp_events(composition);
        for (const auto &participant : composition.participant_compositions)
          if (participant != nullptr) bind_active_tcp_events(*participant);
        composition.cell_runtime = stable_cell_runtime_owner.get();
        std::vector<std::shared_ptr<RetainedNativeGripperRuntime>>
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
        }
      };
  stop_recovery_kernel.store(stable_kernel_owner, std::memory_order_release);
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
  execution_composition.addressed_kernel = stable_kernel_owner;
  execution_composition.operation_journal = stable_operation_journal;
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

  // Main-channel ownership; child callbacks retain a copy of this immutable
  // capture rather than reading a replaceable shared continuation variable.
  std::shared_ptr<const ManagedOriginalApplyBringUpCapture>
      original_apply_bring_up_capture;
  std::optional<core::NativeApplyComponentBringUpDTO>
      original_apply_accepted_children;
  std::optional<core::NativeApplyPlanDTO> original_apply_native_plan;
  if (bootstrap_cell->bundle->value().cell_graph.controllable_components.empty()) {
    const auto bootstrap_publication = read_current_publication();
    managed_cell_cclc.CreateEmpty(
        config, bootstrap_cell->bundle, bootstrap_publication,
        *stable_kernel_owner, fence_owner, detached_owner_clock,
        detached_owner_scheduler);
    ConfigureProgramResetPositionAccess(execution_composition,
        core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
        *managed_cell_cclc.facade);
  }
  // The main managed channel serializes its direct reads. Recreation must
  // separately stop all asynchronous ingress before destroying this owner.
  const auto current_cell_cclc = [&]() -> deployment::CCLCRuntimeComposition * {
    if (managed_cell_cclc.facade) return managed_cell_cclc.facade.get();
    auto *const active = active_managed_composition.get();
    if (active && active->cclc_runtime) return active->cclc_runtime.get();
    auto *const retired = retired_managed_composition.get();
    return retired ? retired->cclc_runtime.get() : nullptr;
  };

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
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      preparing_composition_request;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      installed_under_fence_composition_request;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      installed_composition_request;
  std::vector<component_manager::ManagerRuntimeComponentReceiptDTO>
      installed_component_receipts;
  // Candidate TCP sources/rows are staged without mutating retained handles.
  std::vector<StagedRetainedTcpRebind> staged_retained_tcp_rebinds;
  std::optional<core::NamedApplyPublicationTransactionDTO> staged_retained_tcp_apply;
  std::atomic<std::shared_ptr<
      const component_manager::ManagerSelectedRuntimeCompositionRequestDTO>>
      installed_liveness_handoff;
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
      else {
        const auto &request = installed_composition_request
                                  ? installed_composition_request
                                  : installed_under_fence_composition_request;
        if (request)
          origin = core::NamedApplyPublicationTransactionDTO{
              request->apply_id, request->apply_generation};
      }
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
    if (observed.value().bring_up_plan) {
      const auto valid = component_manager::
          ValidateManagerApplyBringUpCellAdmissionSettlementReadOnly(
              database, cell, *claim, release);
      if (!valid.ok())
        return valid;
    } else {
      const auto valid =
          component_manager::ReadManagerApplyConfigurationStatusReadOnly(
              database, cell, *origin, *claim);
      if (!valid.ok())
        return Reply::Err(valid.error());
    }
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
        std::make_shared<const component_manager::
                             ManagerSelectedRuntimeCompositionRequestDTO>(
            request),
        std::memory_order_release);
  };
  std::exception_ptr managed_composition_failure;
  std::jthread current_composition_builder;
  bool current_composition_builder_started = false;
  std::jthread managed_composition_builder;
  bool managed_composition_building = false;
  std::exception_ptr selected_composition_failure;

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
    if (owner == nullptr || installed_under_fence_composition_request.has_value()) {
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
                stable_addressed_kernel() == owner &&
                !installed_under_fence_composition_request,
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
          current_composition_builder_started ||
          preparing_composition_request.has_value() ||
          installed_under_fence_composition_request.has_value() ||
          installed_composition_request.has_value()) {
        return;
      }
    }
    bool predecessor_pending = false;
    auto input = TryBuildCurrentManagedCompositionInput(
        config, projections, admission_gate, predecessor_pending);
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
          current_composition_builder_started ||
          preparing_composition_request.has_value() ||
          installed_under_fence_composition_request.has_value() ||
          installed_composition_request.has_value()) {
        return;
      }
      input->attach_addressed_owner = false;
      input->shared_clock = detached_owner_clock;
      input->ros_context_provider = host_ros_context_provider;
      input->motion_trace = host_motion_trace;
      input->shared_scheduler = detached_owner_scheduler;
      input->observation_wake = stable_observation_wake;
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
            {
              std::lock_guard lock(managed_composition_mutex);
              std::unique_lock execution_lock(execution_composition.mutex);
              Require(stable_kernel_owner != nullptr &&
                          stable_operation_journal != nullptr &&
                          detached_owner_clock != nullptr &&
                          detached_owner_scheduler != nullptr,
                      "current managed composition has no stable cell owner");
              Require(built->component_runtime_identity() &&
                          (!built->runtime || (!built->runtime->addressed_kernel &&
                           !built->runtime->operation_journal &&
                           built->runtime->clock == detached_owner_clock &&
                           built->runtime->scheduler == detached_owner_scheduler)),
                      "current managed composition duplicated the stable addressed owner");
              PrepareCompositionIoResources(*built, *stable_cell_runtime_owner,
                                            generic_io_transport, host_ros_context_provider);
              auto current_registrations = NativeCellComponentRegistrations(*built);
              RetainedRuntimeParticipantResolverFactory staged_factory;
              auto current_resolver = NativeCellParticipantResolver(*built, current_registrations, &staged_factory);
              auto current_stop_sources = NativeCellStopSources(*built);
              static_cast<void>(LoadManagedBootstrapStopProduct(config, built->bundle->retained_bundle_id(),
                  built->bundle->value().cell_graph.graph_revision));
              auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_product(
                  std::move(current_registrations), std::move(current_resolver), std::move(current_stop_sources));
              if (!reconfigured.ok()) Fail("stable Kernel rejected current runtime participants: " + reconfigured.error().message);
              AdoptNativeCellParticipantResolverFactory(*built, std::move(staged_factory));
              if (built->runtime) built->runtime->stop_surveillance_owner = stable_stop_surveillance_owner;
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
              execution_composition.replace_current(
                  active_managed_composition.get());
              wake_payload_follower();
              execution_composition.addressed_kernel =
                  active_managed_composition->active_addressed_kernel();
              stop_recovery_runtime.store(
                  active_managed_composition->runtime.get(),
                  std::memory_order_release);
              installed = active_managed_composition.get();
            }
            std::cerr << "native composition phase: stable Kernel bound\n";
            // Initial CCLC recovery is part of composing a usable runtime.  It
            // runs on the existing builder lane so the ordinary Manager
            // command channel remains responsive and cannot advertise an
            // installed composition before its exact routes are current.
            const auto routes_attached = AttachCurrentPublishedCCLCIfReady(
                *installed, config, fence_owner, std::nullopt, [&] {
                  return RestoreAppliedGlobalIoRuleCatalog(
                      execution_composition, *stable_kernel_owner,
                      config.manager_state_root, core::CellId(config.cell_id));
                });
            std::cerr << "native composition phase: control routes "
                      << (routes_attached ? "attached" : "deferred") << '\n';
            const auto attached_head = routes_attached
                ? std::optional(read_current_publication()) : std::nullopt;
            {
              std::lock_guard lock(managed_composition_mutex);
              Require(active_managed_composition.get() == installed,
                      "initial managed composition changed during CCLC "
                      "recovery");
              if (routes_attached) {
                Require(!managed_cell_cclc.facade && attached_head,
                        "initial runtime cannot construct a second cell CCLC owner");
                // Preserve managed -> execution lock order, and pin facade
                // transfer against existing Reset ingress readers.
                std::unique_lock execution_lock(execution_composition.mutex);
                managed_cell_cclc.TakeAttached(*installed, *attached_head);
                ConfigureProgramResetPositionAccess(execution_composition,
                    core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
                    *managed_cell_cclc.facade);
              }
              current_composition_building = false;
            }
          } catch (...) {
            std::lock_guard lock(managed_composition_mutex);
            managed_composition_failure = std::current_exception();
            current_composition_building = false;
          }
        });
  };

  struct OriginalApplyCandidateProof final {
    Composition *composition{};
    std::map<core::ControllableComponentId, std::shared_ptr<const IoProviderDriverGroup>> groups;
    std::vector<core::RuntimeProviderDomainConfigurationReadbackDTO> configurations;
  };
  const auto read_original_apply_child_generations =
      [&](const std::shared_ptr<const ManagedOriginalApplyBringUpCapture> &captured,
          const core::ApplyComponentBringUpAuthorityDTO &authority) {
    Require(captured && captured->original_facade && authority.valid() &&
                authority.parent_claim == captured->parent_claim &&
                authority.parent == core::ApplyComponentBringUpReferenceDTO{
                    {captured->sealed_plan.plan.apply_id, captured->sealed_plan.plan.apply_generation},
                    captured->sealed_plan.plan_digest},
            "construction proof lost its exact original parent");
    IoProviderConstructionGenerations generations;
    for (const auto &child : captured->sealed_plan.plan.children) {
      const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
          child.intent, child.candidate_projection_revision, child.candidate_retained_bundle_digest,
          child.candidate_retained_bundle_bytes, child.runtime_unit_id, child.runtime_epoch,
          child.manager_runtime_epoch, authority.parent);
      const auto actual = captured->original_facade->coordinator().ReadApplyComponentBringUpRequestResult(
          child.intent, digest);
      Require(actual.ok() && actual.value() && actual.value()->status &&
                  actual.value()->status->locally_valid() &&
                  actual.value()->status->accepted_intent == child.intent &&
                  actual.value()->status->apply_bring_up &&
                  actual.value()->status->apply_bring_up->parent_authority == authority &&
                  actual.value()->status->apply_bring_up->original_request_payload_digest == digest &&
                  actual.value()->status->status_owner_epoch == captured->original_facade->coordinator().request_owner_epoch() &&
                  !actual.value()->status->reconciliation_required &&
                  actual.value()->status->acceptance.component_lifecycle_generation.nonzero(),
              "construction proof has no original durable accepted child");
      Require(generations.emplace(child.intent.controllable_component_id,
                  actual.value()->status->acceptance.component_lifecycle_generation).second,
              "construction proof repeats an accepted component");
    }
    return generations;
  };
  // Journal reads precede all fence/composition/delivery locks. Only the
  // original live facade can supply the accepted child generation; a request
  // status or whichever map row happens to be current cannot supply it.
  const auto read_original_apply_candidate_proof =
      [&](const std::shared_ptr<const ManagedOriginalApplyBringUpCapture>& captured,
          const core::ApplyComponentBringUpAuthorityDTO& authority) {
    const auto generations = read_original_apply_child_generations(captured, authority);
    std::lock_guard composition_lock(managed_composition_mutex);
    Require(managed_cell_cclc.facade.get() == captured->original_facade &&
                active_managed_composition && installed_under_fence_composition_request &&
                installed_under_fence_composition_request->apply_id == authority.parent.owner.apply_id &&
                installed_under_fence_composition_request->apply_generation == authority.parent.owner.apply_generation &&
                active_managed_composition->bundle->canonical_bytes() == captured->candidate.bundle->canonical_bytes() &&
                active_managed_composition->projection_revision ==
                    captured->sealed_plan.plan.expected_publication.candidate_projection_revision,
            "candidate proof has no exact actual installed product");
    OriginalApplyCandidateProof result;
    result.composition = active_managed_composition.get();
    const auto owners = IndexNativeComponentCompositions(*active_managed_composition);
    for (const auto& domain : authority.held_fence_query.expected_scope.domains) {
      const auto* sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
      if (!sides || !sides->candidate_provider) continue;
      const auto& component = sides->candidate_provider->controllable_component_id;
      const auto owner = owners.find(component);
      const auto generation = generations.find(component);
      Require(owner != owners.end() && generation != generations.end() &&
                  owner->second->io_provider_drivers && !owner->second->io_provider_drivers->empty(),
              "candidate proof has no complete actual driver group/child");
      const auto group = owner->second->io_provider_drivers;
      const auto record = std::find_if(group->begin(), group->end(), [&](const auto& value) {
        return value && value->domain().control_domain_id == domain.control_domain_id;
      });
      Require(record != group->end() && (*record)->resources_installed(),
              "candidate proof has no actual installed driver resources");
      const auto current = stable_kernel_owner->current_runtime_provider_configuration(domain.control_domain_id);
      Require(current.ok() && current.value() && current.value()->locally_valid() &&
                  current.value()->domain == (*record)->domain() &&
                  current.value()->provider_occurrence == (*record)->occurrence() &&
                  current.value()->component_lifecycle_generation == generation->second &&
                  current.value()->retained_bundle_id == captured->candidate.bundle->retained_bundle_id() &&
                  current.value()->graph_revision == captured->candidate.bundle->value().cell_graph.graph_revision,
              "candidate proof differs from its actual driver/accepted child generation");
      result.groups.emplace(component, group);
      result.configurations.push_back(*current.value());
    }
    return result;
  };
  const auto revalidate_original_apply_candidate_pin = [&](const OriginalApplyCandidateProof& proof) {
    Require(active_managed_composition.get() == proof.composition,
            "original candidate composition changed before action");
    const auto owners = IndexNativeComponentCompositions(*active_managed_composition);
    for (const auto& [component, group] : proof.groups) {
      const auto owner = owners.find(component);
      Require(owner != owners.end() && owner->second->io_provider_drivers == group,
              "original candidate actual driver group changed before action");
    }
    for (const auto& expected : proof.configurations) {
      const auto current = stable_kernel_owner->current_runtime_provider_configuration(
          expected.domain.control_domain_id);
      Require(current.ok() && current.value() && current.value()->locally_valid() &&
                  current.value()->retained_bundle_id == expected.retained_bundle_id &&
                  current.value()->graph_revision == expected.graph_revision &&
                  current.value()->domain == expected.domain &&
                  current.value()->provider_occurrence == expected.provider_occurrence &&
                  current.value()->component_lifecycle_generation == expected.component_lifecycle_generation,
              "original candidate driver/accepted generation changed at the action pin");
    }
  };

  // Read actual owner-issued lifecycle facts outside composition/Kernel locks.
  // The serial managed caller borrows the stable facade; product exchange must
  // exclude these reads through its accounted current-context pin.
  // D6 (SX-365): the component has launch processes and every one is a drained
  // process group (cleanup-proven, not an exited leader). No process at all
  // (a processless I/O owner) or a local no-output owner never qualifies.
  const auto component_processes_drained = [&](const core::ControllableComponentId &id) {
    std::vector<std::shared_ptr<ControllerLaunchProcess>> processes;
    bool local_no_output = false;
    {
      std::lock_guard lock(managed_composition_mutex);
      std::shared_lock execution_lock(execution_composition.mutex);
      const auto collect = [&](Composition *root) {
        if (!root) return;
        const auto include = [&](Composition &item) {
          if (item.controllable_component_id != id) return;
          // A local no-output owner or any I/O provider (its I/O effects are
          // not proven settled) never qualifies (SX-374).
          if (IsLocalNoOutputGripperOwner(item) ||
              (item.io_provider_drivers && !item.io_provider_drivers->empty()))
            local_no_output = true;
          if (item.controller_process) processes.push_back(item.controller_process);
        };
        include(*root);
        for (auto &peer : root->participant_compositions)
          if (peer) include(*peer);
      };
      collect(active_managed_composition.get());
      collect(execution_composition.current);
    }
    return !local_no_output && !processes.empty() &&
           std::all_of(processes.begin(), processes.end(), [](const auto &process) {
             return process->cleanup_observation().state ==
                    ControllerLaunchCleanupState::kDrained;
           });
  };
  // D6: the Apply candidate is the verified canonical-empty one (its
  // Manager-verified sealed origin), from any caller.
  const auto canonical_empty_apply = [&](const component_manager::ManagerApplyIntentDTO &intent) {
    try {
      return deployment::prepared_receipt_is_canonical_empty_candidate(
          content_identity::parse_canonical_json(intent.prepared_activation.canonical_receipt_json),
          core::CellId(config.cell_id), intent.candidate_retained_bundle_id,
          intent.candidate_graph_revision);
    } catch (const std::exception &) {
      return false;
    }
  };
  const auto require_inactive_changed_components =
      [&](const deployment::ApplyComponentDeltaDTO &delta,
          const core::ActiveBundlePublicationReceiptDTO &predecessor,
          const bool canonical_empty_candidate) {
    Require(delta.valid() && predecessor.valid(),
            "configuration Apply has invalid delta/predecessor evidence");
    deployment::CCLCRuntimeComposition *facade = nullptr;
    {
      std::lock_guard lock(managed_composition_mutex);
      facade = current_cell_cclc();
      Require(facade != nullptr && !current_composition_building &&
                  !managed_composition_building,
              "configuration Apply has no stable current component owner");
    }
    for (const auto &change : delta.components) {
      if (change.disposition != deployment::ApplyComponentDisposition::kReplace &&
          change.disposition != deployment::ApplyComponentDisposition::kRemove)
        continue;
      const auto fact = facade->ReadCurrentComponentSnapshot(change.controllable_component_id);
      Require(fact.ok() && fact.value().locally_valid() &&
                  fact.value().controllable_component_id == change.controllable_component_id &&
                  fact.value().active_publication_receipt == predecessor,
              "configuration Apply cannot read the exact current component " +
                  change.controllable_component_id.value());
      // D6 (D-M4-4): a component whose provider is gone (its Disable has no
      // eligible step) still reads Active. Only an Apply of the verified
      // canonical-empty candidate, from any caller, may remove it, once every
      // launch process group is drained.
      Require(fact.value().component_lifecycle_state != core::ComponentLifecycleStateDTO::kActive ||
                  (canonical_empty_candidate &&
                   change.disposition == deployment::ApplyComponentDisposition::kRemove &&
                   component_processes_drained(change.controllable_component_id)),
              "configuration Apply cannot replace or remove Active component " +
                  change.controllable_component_id.value() + "; deactivate it first");
    }
    Require(read_current_publication() == predecessor,
            "configuration Apply predecessor changed during component inspection");
  };

  const auto query_configuration_fence_status =
      [&](const core::ConfigurationFenceStatusQueryDTO &query) {
    struct Snapshot final {
      component_manager::ManagerSelectedRuntimeCompositionRequestDTO request;
      Composition *composition;
      deployment::CCLCRuntimeComposition *facade;
      std::shared_ptr<const deployment::RetainedBundle> bundle;
      std::shared_ptr<RetainedHostLifecycleProduct> lifecycle_product;
      std::map<core::ControllableComponentId, std::shared_ptr<const IoProviderDriverGroup>> groups;
      core::ActiveBundlePublicationReceiptDTO predecessor;
      core::CellAdmissionClaimDTO parent;
      core::NativeApplyPlanDTO plan;
      std::vector<core::InstalledProviderCandidateFactsDTO> facts;
    };
    // Main-channel reads pin the same current facade/composition while using
    // its fresh plan reader. No fence/admission/Kernel lock spans source SQL.
    auto snapshot = [&]() -> std::optional<Snapshot> {
      std::lock_guard lock(managed_composition_mutex);
      if (!installed_under_fence_composition_request || !active_managed_composition ||
          selected_composition_failure || current_composition_building || managed_composition_building)
        return std::nullopt;
      const auto &request = *installed_under_fence_composition_request;
      if (query.owner_transaction != core::ApplyFenceOwnerDTO{request.apply_id, request.apply_generation} ||
          query.configuration_fence_id != request.configuration_fence_id ||
          query.configuration_fence_generation != request.configuration_fence_generation ||
          query.expected_scope != request.configuration_fence_scope ||
          query.manager_runtime_epoch != request.manager_runtime_epoch ||
          std::none_of(query.expected_scope.domains.begin(), query.expected_scope.domains.end(), [](const auto &domain) {
            const auto *sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
            return sides && sides->candidate_provider.has_value();
          }))
        return std::nullopt;
      const auto head = read_current_publication();
      if (head.publication_generation != request.expected_predecessor_publication_generation ||
          head.durable_public_pointer_content_digest != request.expected_predecessor_public_pointer_digest)
        return std::nullopt;
      auto *const facade = current_cell_cclc();
      Require(facade != nullptr, "installed NativeApply lost its original lifecycle owner");
      const auto plan = facade->LoadNativeApplyPlan({request.apply_id, request.apply_generation});
      Require(plan.ok() && plan.value(), "installed NativeApply lost its exact recorded plan");
      // The ordinary Manager reader deliberately refuses linked requests;
      // classify from the existing exact recorded plan before invoking it.
      if (plan.value()->component_bring_up) return std::nullopt;
      auto parent = stable_kernel_owner->ReadHeldCellAdmissionClaim(
          core::CellAdmissionClaimKindDTO::kManagerApply, request.apply_id.value());
      if (!parent)
        parent = stable_kernel_owner->ReadHeldCellAdmissionClaim(core::CellAdmissionClaimKindDTO::kAuthoringMutation);
      if (!parent) return std::nullopt;
      const auto original = component_manager::ReadManagerApplyConfigurationStatusReadOnly(
          config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
          core::CellId(config.cell_id), {request.apply_id, request.apply_generation}, *parent);
      Require(original.ok(), "installed NativeApply cannot verify its original Manager parent");
      const auto &stored = original.value();
      if (stored.bring_up_plan || stored.publication_receipt || stored.terminal_outcome)
        return std::nullopt; // Linked, published and terminal paths stay strict generic observations.
      const auto &intent = stored.intent.intent;
      Require(stored.fence_attempt && stored.fence_acceptance && stored.runtime_convergence_intent &&
                  intent.expected_active_projection_revision == request.predecessor_projection_revision &&
                  intent.candidate_retained_bundle_id == request.candidate_projection_revision.retained_bundle_id &&
                  intent.candidate_graph_revision == request.candidate_projection_revision.graph_revision &&
                  stored.fence_attempt->fence_intent.owner_request_context == intent.request_context &&
                  stored.fence_acceptance->accepted_fence.matches(stored.fence_attempt->fence_intent) &&
                  stored.fence_acceptance->fence_attempt_digest == stored.fence_attempt->fence_attempt_digest &&
                  stored.fence_acceptance->accepted_fence.configuration_fence_id == query.configuration_fence_id &&
                  stored.fence_acceptance->accepted_fence.configuration_fence_generation == query.configuration_fence_generation &&
                  stored.fence_acceptance->accepted_fence.captured_scope == query.expected_scope &&
                  stored.runtime_convergence_intent->runtime_convergence_intent.predecessor_publication_generation ==
                      head.publication_generation &&
                  stored.runtime_convergence_intent->runtime_convergence_intent.predecessor_public_pointer_digest ==
                      head.durable_public_pointer_content_digest,
              "installed NativeApply differs from its immutable original request/fence");
      auto *const candidate = active_managed_composition.get();
      Require(facade && candidate->bundle && candidate->cclc_lifecycle_product &&
                  candidate->projection_revision == request.candidate_projection_revision,
              "installed NativeApply lost its actual product or original lifecycle owner");
      const auto selected = LoadExactManagedCandidateBundle(config, request);
      Require(selected && selected->bundle->canonical_bytes() == candidate->bundle->canonical_bytes(),
              "installed NativeApply product no longer matches its original request");
      auto facts = ReadInstalledNativeProviderCandidateFacts(
          config, *selected, *candidate, *facade, *plan.value(), query.expected_scope);
      std::map<core::ControllableComponentId, std::shared_ptr<const IoProviderDriverGroup>> groups;
      for (const auto &[component, owner] : IndexNativeComponentCompositions(*candidate))
        if (owner->io_provider_drivers) groups.emplace(component, owner->io_provider_drivers);
      return Snapshot{request, candidate, facade, candidate->bundle, candidate->cclc_lifecycle_product,
          std::move(groups), head, *parent, *plan.value(), std::move(facts)};
    }();
    if (!snapshot) return fence_owner.QueryStatus(query);
    return fence_owner.QueryInstalledNativeApplyCandidateStatus(
        query, snapshot->predecessor, snapshot->parent, snapshot->plan, snapshot->facts, [&] {
          // Existing ledger->managed order. Compare only metadata here;
          // never recurse into this owner, a journal, or the Kernel.
          std::lock_guard lock(managed_composition_mutex);
          Require(installed_under_fence_composition_request == std::optional(snapshot->request) &&
                      active_managed_composition.get() == snapshot->composition &&
                      current_cell_cclc() == snapshot->facade &&
                      active_managed_composition->bundle == snapshot->bundle &&
                      active_managed_composition->cclc_lifecycle_product == snapshot->lifecycle_product &&
                      !selected_composition_failure && !current_composition_building && !managed_composition_building,
                  "installed NativeApply current attempt changed before observation");
          std::map<core::ControllableComponentId, std::shared_ptr<const IoProviderDriverGroup>> groups;
          const auto note = [&](const Composition &owner) {
            if (owner.io_provider_drivers)
              groups.emplace(owner.controllable_component_id, owner.io_provider_drivers);
          };
          note(*active_managed_composition);
          for (const auto &peer : active_managed_composition->participant_compositions) {
            Require(peer != nullptr, "installed NativeApply component pointer changed");
            note(*peer);
          }
          Require(groups == snapshot->groups,
                  "installed NativeApply driver group changed before observation");
        });
  };

  // F-5 timing: when the current runtime-composition packet arrived from the
  // Manager (wall clock, to compare with the Manager's send line).
  RpcTiming composition_timing;
  std::chrono::steady_clock::time_point composition_request_received{};
  std::int64_t composition_request_received_unix_ms = 0;
  const auto install_or_observe_composition =
      [&](const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
              &request)
      -> component_manager::ManagerSelectedRuntimeCompositionResultDTO {
    Require(request.valid() &&
                request.runtime_unit_id.value() == config.runtime_unit_id &&
                request.runtime_epoch.value() == config.runtime_epoch,
            "runtime-composition request names another managed child");
    bool original_configuration_finalize = false;
    bool verified_configuration_finalize = false;
    // This classifies the exact stored configuration operation's Scene target;
    // effect authority still comes from the existing held-fence checks below.
    // Linked preparation uses only its same original sealed parent/facade.
    // Actual child acceptance, installed structural source and CLOSED Kernel
    // publication below must complete before any original child continuation.
    auto configuration_parent_claim =
        stable_addressed_kernel()->ReadHeldCellAdmissionClaim(
            core::CellAdmissionClaimKindDTO::kManagerApply,
            request.apply_id.value());
    if (!configuration_parent_claim) {
      configuration_parent_claim = stable_addressed_kernel()->ReadHeldCellAdmissionClaim(
          core::CellAdmissionClaimKindDTO::kAuthoringMutation);
    }
    bool prepare_canonical_empty_candidate = false;
    if (configuration_parent_claim &&
        (configuration_parent_claim->intent.claim_kind == core::CellAdmissionClaimKindDTO::kAuthoringMutation ||
         configuration_parent_claim->intent.owner_transaction_id == request.apply_id.value())) {
      const auto journal_start = DiagnosticClock::now();
      const auto parent = component_manager::ReadManagerApplyConfigurationStatusReadOnly(
          config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
          core::CellId(config.cell_id), {request.apply_id, request.apply_generation},
          *configuration_parent_claim);
      composition_timing.journal_read += DiagnosticClock::now() - journal_start;
      if (!parent.ok()) {
        std::string detail = "runtime composition cannot verify its original Manager parent";
        if (const auto found = parent.error().details.find("reason"); found != parent.error().details.end())
          detail += "; reason=" + found->second;
        if (const auto found = parent.error().details.find("integrity_detail"); found != parent.error().details.end())
          detail += "; integrity_detail=" + found->second;
        Require(false, std::move(detail));
      }
      prepare_canonical_empty_candidate =
          canonical_empty_apply(parent.value().intent.intent) &&
          parent.value().intent.intent.candidate_retained_bundle_id ==
              request.candidate_projection_revision.retained_bundle_id &&
          parent.value().intent.intent.candidate_graph_revision ==
              request.candidate_projection_revision.graph_revision;
      if (parent.value().bring_up_plan) {
        Require(original_apply_bring_up_capture &&
                    original_apply_bring_up_capture->original_facade == current_cell_cclc() &&
                    original_apply_bring_up_capture->parent_claim == *configuration_parent_claim &&
                    original_apply_bring_up_capture->sealed_plan == *parent.value().bring_up_plan &&
                    original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
                    original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation,
                "linked runtime preparation lacks its exact original accepted child owner");
      }
      if (request.phase == component_manager::ManagerSelectedRuntimeCompositionPhase::kFinalize) {
        const auto &stored = parent.value();
        const auto &intent = stored.intent.intent;
        Require(stored.fence_attempt && stored.fence_acceptance &&
                    stored.runtime_convergence_intent && stored.latest_runtime_staged_ready &&
                    intent.apply_id == request.apply_id && intent.apply_generation == request.apply_generation &&
                    intent.expected_active_projection_revision == request.predecessor_projection_revision &&
                    intent.candidate_retained_bundle_id == request.candidate_projection_revision.retained_bundle_id &&
                    intent.candidate_graph_revision == request.candidate_projection_revision.graph_revision &&
                    stored.fence_attempt->fence_intent.owner_request_context == intent.request_context &&
                    stored.fence_attempt->fence_intent.owner_transaction ==
                        core::ApplyFenceOwnerDTO{request.apply_id, request.apply_generation} &&
                    stored.fence_acceptance->fence_attempt_digest == stored.fence_attempt->fence_attempt_digest &&
                    stored.fence_acceptance->accepted_fence.matches(stored.fence_attempt->fence_intent) &&
                    stored.fence_acceptance->accepted_fence.configuration_fence_id == request.configuration_fence_id &&
                    stored.fence_acceptance->accepted_fence.configuration_fence_generation == request.configuration_fence_generation &&
                    stored.fence_acceptance->accepted_fence.captured_scope == request.configuration_fence_scope &&
                    stored.runtime_convergence_intent->runtime_convergence_intent.predecessor_publication_generation ==
                        request.expected_predecessor_publication_generation &&
                    stored.runtime_convergence_intent->runtime_convergence_intent.predecessor_public_pointer_digest ==
                        request.expected_predecessor_public_pointer_digest &&
                    std::any_of(stored.latest_runtime_staged_ready->staged_ready.runtime_units.begin(),
                                stored.latest_runtime_staged_ready->staged_ready.runtime_units.end(),
                                [&](const auto &unit) {
                                  return unit.valid() && unit.runtime_unit_id == request.runtime_unit_id &&
                                      unit.runtime_epoch == request.runtime_epoch;
                                }),
                "preserved configuration finalization differs from its exact stored parent/fence/host");
        verified_configuration_finalize = true;
        original_configuration_finalize = !stored.bring_up_plan &&
            request.candidate_projection_revision.scene_revision ==
                request.predecessor_projection_revision.scene_revision;
      }
    }
    if (request.phase == component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare &&
        !preparing_composition_request && !installed_under_fence_composition_request &&
        (!installed_composition_request ||
         installed_composition_request->apply_id != request.apply_id ||
         installed_composition_request->apply_generation != request.apply_generation)) {
      const auto predecessor = read_current_publication();
      // A published successor is existing observation/replay, not first
      // preparation. Its original predecessor may no longer be current.
      if (predecessor.publication_generation == request.expected_predecessor_publication_generation &&
          predecessor.durable_public_pointer_content_digest == request.expected_predecessor_public_pointer_digest &&
          core::SameActiveDeployment(predecessor.request.candidate_projection_revision,
                                     request.predecessor_projection_revision))
        require_inactive_changed_components(request.component_delta, predecessor,
                                            prepare_canonical_empty_candidate);
    }
    const auto read_installed_receipts = [&] {
      const std::array<const Composition *, 1> actual_other_predecessors{
          failed_prepared_composition.get()};
      return ReadNativeInstalledComponentReceipts(
          request, active_managed_composition.get(),
          retired_managed_composition != nullptr
              ? retired_managed_composition.get()
              : (IsExactRetainedNativeViewApply(request)
                     ? active_managed_composition.get()
                     : nullptr),
          actual_other_predecessors);
    };
    const core::ConfigurationFenceStatusQueryDTO fence_query{
        {request.apply_id, request.apply_generation},
        request.configuration_fence_id,
        request.configuration_fence_generation,
        request.configuration_fence_scope,
        request.manager_runtime_epoch};
    if (request.phase ==
        component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare) {
      std::optional<
          component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
          previous_attempt;
      {
        std::lock_guard lock(managed_composition_mutex);
        if (!managed_composition_building && !current_composition_building &&
            (managed_composition_failure || selected_composition_failure))
          previous_attempt = installed_under_fence_composition_request
                                 ? installed_under_fence_composition_request
                                 : preparing_composition_request;
      }
      if (previous_attempt &&
          (previous_attempt->apply_id != request.apply_id ||
           previous_attempt->apply_generation != request.apply_generation)) {
        const auto original_claim =
            stable_addressed_kernel()->ReadHeldCellAdmissionClaim(
                core::CellAdmissionClaimKindDTO::kManagerApply,
                previous_attempt->apply_id.value());
        Require(original_claim.has_value(),
                "actual_installed_state_unknown: failed preparation has no "
                "original held Apply claim; restart the workbench container");
        const std::array original_claims{*original_claim};
        const auto terminal =
            component_manager::ValidateTerminalManagerApplyClaimsReadOnly(
                config.manager_state_root /
                    std::string(
                        component_manager::kManagerApplyJournalFilename),
                core::CellId(config.cell_id), original_claims);
        Require(terminal.ok(),
                "actual_installed_state_unknown: previous preparation remains "
                "live or unknown; restart the workbench container");
        const auto previous =
            component_manager::ReadManagerApplyConfigurationStatusReadOnly(
                config.manager_state_root /
                    std::string(
                        component_manager::kManagerApplyJournalFilename),
                core::CellId(config.cell_id),
                {previous_attempt->apply_id,
                 previous_attempt->apply_generation},
                *original_claim);
        Require(previous.ok() && !previous.value().bring_up_plan &&
                    previous.value().terminal_outcome,
                "actual_installed_state_unknown: previous preparation lacks "
                "its exact immutable terminal");
        // Fresh authority precedes retiring attempt-local markers. Neither the
        // original terminal nor its physical fence/claim is changed here.
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [] {});
        if (managed_composition_builder.joinable())
          managed_composition_builder.join();
        std::optional<CompositionBuildInput> discarded_view;
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [&] {
              std::lock_guard lock(managed_composition_mutex);
              std::unique_lock execution_lock(execution_composition.mutex);
              Require(!managed_composition_building &&
                          !current_composition_building &&
                          (installed_under_fence_composition_request ==
                               previous_attempt ||
                           preparing_composition_request == previous_attempt),
                      "actual_installed_state_unknown: previous owner changed "
                      "during finished-builder join");
              if (!previous.value()
                       .terminal_outcome->terminal_outcome.succeeded()) {
                const core::ConfigurationFenceStatusQueryDTO previous_fence{
                    {previous_attempt->apply_id,
                     previous_attempt->apply_generation},
                    previous_attempt->configuration_fence_id,
                    previous_attempt->configuration_fence_generation,
                    previous_attempt->configuration_fence_scope,
                    previous_attempt->manager_runtime_epoch};
                const auto discarded =
                    stable_addressed_kernel()->DiscardStagedGlobalIoRuleCatalog(
                        previous_fence, *original_claim);
                Require(discarded.ok(),
                        "failed global candidate lost its exact staged owner");
              }
              Require(
                  !prepared_managed_composition || !failed_prepared_composition,
                  "actual_installed_state_unknown: more than one failed "
                  "candidate owner remains; restart the workbench container");
              if (prepared_managed_composition)
                failed_prepared_composition =
                    std::move(prepared_managed_composition);
              discarded_view = std::move(prepared_retained_view);
              prepared_retained_view.reset();
              prepared_managed_composition_is_empty = false;
              preparing_composition_request.reset();
              installed_under_fence_composition_request.reset();
              managed_composition_failure = nullptr;
              selected_composition_failure = nullptr;
            });
      }
    }
    if (selected_composition_failure != nullptr)
      std::rethrow_exception(selected_composition_failure);
    const auto require_installed_candidate_configuration = [&] {
      const auto fence_status = query_configuration_fence_status(fence_query);
      Require(fence_status.matches(fence_query) &&
                  fence_status.state == core::ConfigurationFenceState::kHeld &&
                  fence_status.admission_fenced &&
                  fence_status.reconciliation_evidence.execution_host_epoch
                          .runtime_unit_id == request.runtime_unit_id &&
                  fence_status.reconciliation_evidence.execution_host_epoch
                          .runtime_epoch == request.runtime_epoch &&
                  fence_status.reconciliation_evidence
                          .active_publication_generation ==
                      request.expected_predecessor_publication_generation &&
                  fence_status.reconciliation_evidence
                          .active_public_pointer_digest ==
                      request.expected_predecessor_public_pointer_digest,
              "installed runtime composition does not reconcile to its exact "
              "durable held fence and predecessor publication");
    };
    // Initial installation retains the strict predecessor check. Only this
    // post-install handoff uses candidate rows (including proved absence),
    // still under the actual original parent and unchanged public head.
    const auto run_with_installed_candidate = [&](const std::function<void()> &effect) {
      Require(verified_configuration_finalize && configuration_parent_claim,
              "installed candidate handoff lacks its verified original Manager parent");
      Composition *installed = nullptr;
      {
        const auto lock_start = DiagnosticClock::now();
        std::lock_guard lock(managed_composition_mutex);
        composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
        Require(installed_under_fence_composition_request == std::optional(request) &&
                    !managed_composition_building && !current_composition_building,
                "installed candidate handoff lost its exact current request");
        installed = active_managed_composition.get();
      }
      std::vector<core::RuntimeProviderDomainConfigurationReadbackDTO> configurations;
      for (const auto &domain : request.configuration_fence_scope.domains) {
        const auto *sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
        if (!sides || !sides->candidate_provider) continue;
        const auto actual = stable_kernel_owner->current_runtime_provider_configuration(domain.control_domain_id);
        Require(actual.ok() && actual.value(),
                "installed candidate handoff has no actual provider row");
        configurations.push_back(*actual.value());
      }
      fence_owner.RunWithExactHeldInstalledCandidate(fence_query,
          request.expected_predecessor_publication_generation,
          request.expected_predecessor_public_pointer_digest, *configuration_parent_claim,
          request.candidate_projection_revision, configurations, [&] {
            {
              const auto lock_start = DiagnosticClock::now();
              std::lock_guard lock(managed_composition_mutex);
              composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
              Require(installed_under_fence_composition_request == std::optional(request) &&
                          active_managed_composition.get() == installed &&
                          !managed_composition_building && !current_composition_building,
                      "installed candidate changed before its pinned handoff");
            }
            effect();
          });
    };
    try_release_retired_composition();
    try_start_current_composition();
    rethrow_composition_failure();
    {
      const auto lock_start = DiagnosticClock::now();
      std::lock_guard lock(managed_composition_mutex);
      composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
      if (current_predecessor_composition_pending &&
          active_managed_composition == nullptr &&
          !current_composition_builder_started &&
          !installed_under_fence_composition_request.has_value()) {
        // The published predecessor already declares physical components,
        // but their verified Scene projections have not settled yet. Keep
        // the Manager request in preparation; a retain delta may only proceed
        // after the exact predecessor owners exist in this live Kernel.
        return {request,
                component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
                {}};
      }
    }
    if (request.phase == component_manager::
                             ManagerSelectedRuntimeCompositionPhase::kFinalize) {
      std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
          prepared_request;
      {
        const auto lock_start = DiagnosticClock::now();
        std::lock_guard lock(managed_composition_mutex);
        composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
        if (installed_under_fence_composition_request.has_value() &&
            installed_under_fence_composition_request->phase ==
                component_manager::ManagerSelectedRuntimeCompositionPhase::
                    kPrepare) {
          prepared_request = installed_under_fence_composition_request;
        }
      }
      if (prepared_request.has_value()) {
        Require(!original_configuration_finalize ||
                    request.authoritative_scene_snapshot_digest ==
                        prepared_request->authoritative_scene_snapshot_digest,
                "preserved configuration finalization changed its prepared Scene snapshot");
        auto expected = request;
        expected.phase = component_manager::
            ManagerSelectedRuntimeCompositionPhase::kPrepare;
        expected.candidate_projection_revision =
            prepared_request->candidate_projection_revision;
        expected.authoritative_scene_snapshot_digest =
            prepared_request->authoritative_scene_snapshot_digest;
        bool original_linked_finalize = false;
        {
          const auto lock_start = DiagnosticClock::now();
          std::lock_guard lock(managed_composition_mutex);
          composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
          if (original_apply_bring_up_capture &&
              original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
              original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation) {
            Require(original_apply_native_plan && original_apply_accepted_children &&
                        original_apply_native_plan->component_bring_up == original_apply_accepted_children &&
                        original_apply_native_plan->apply ==
                            core::NamedApplyPublicationTransactionDTO{request.apply_id, request.apply_generation} &&
                        original_apply_native_plan->candidate_retained_bundle_id ==
                            request.candidate_projection_revision.retained_bundle_id &&
                        original_apply_native_plan->candidate_graph_revision ==
                            request.candidate_projection_revision.graph_revision &&
                        managed_cell_cclc.facade.get() ==
                            original_apply_bring_up_capture->original_facade,
                    "original finalization lost its accepted child plan or owner");
            original_linked_finalize = true;
          }
        }
        const bool exact_prepared_apply = expected == *prepared_request &&
                    request.candidate_projection_revision.scene_revision.scene_id ==
                        prepared_request->candidate_projection_revision
                            .scene_revision.scene_id &&
                    ((original_linked_finalize || original_configuration_finalize)
                         ? request.candidate_projection_revision.scene_revision ==
                               prepared_request->candidate_projection_revision.scene_revision
                         : request.candidate_projection_revision.scene_revision.revision_id >
                               prepared_request->candidate_projection_revision.scene_revision.revision_id);
        if (!exact_prepared_apply) {
          // Bounded field names diagnose refusal without disclosing payloads or
          // changing the exact prepared-request or Scene proof predicates.
          std::string mismatches;
          const auto compare = [&](std::string_view field, const auto &actual,
                                   const auto &prepared) {
            if (actual != prepared) {
              if (!mismatches.empty()) mismatches += ",";
              mismatches += field;
            }
          };
          compare("authoritative_scene_snapshot_digest", expected.authoritative_scene_snapshot_digest,
                  prepared_request->authoritative_scene_snapshot_digest);
          compare("apply_id", expected.apply_id,
                  prepared_request->apply_id);
          compare("apply_generation", expected.apply_generation,
                  prepared_request->apply_generation);
          compare("candidate_projection_revision", expected.candidate_projection_revision,
                  prepared_request->candidate_projection_revision);
          compare("expected_predecessor_publication_generation", expected.expected_predecessor_publication_generation,
                  prepared_request->expected_predecessor_publication_generation);
          compare("expected_predecessor_public_pointer_digest", expected.expected_predecessor_public_pointer_digest,
                  prepared_request->expected_predecessor_public_pointer_digest);
          compare("retained_bundle_canonical_content_digest", expected.retained_bundle_canonical_content_digest,
                  prepared_request->retained_bundle_canonical_content_digest);
          compare("retained_bundle_canonical_byte_size", expected.retained_bundle_canonical_byte_size,
                  prepared_request->retained_bundle_canonical_byte_size);
          compare("runtime_unit_id", expected.runtime_unit_id,
                  prepared_request->runtime_unit_id);
          compare("runtime_epoch", expected.runtime_epoch,
                  prepared_request->runtime_epoch);
          compare("manager_runtime_epoch", expected.manager_runtime_epoch,
                  prepared_request->manager_runtime_epoch);
          compare("configuration_fence_id", expected.configuration_fence_id,
                  prepared_request->configuration_fence_id);
          compare("configuration_fence_generation", expected.configuration_fence_generation,
                  prepared_request->configuration_fence_generation);
          compare("configuration_fence_scope", expected.configuration_fence_scope,
                  prepared_request->configuration_fence_scope);
          compare("phase", expected.phase,
                  prepared_request->phase);
          compare("component_delta", expected.component_delta,
                  prepared_request->component_delta);
          compare("predecessor_projection_revision", expected.predecessor_projection_revision,
                  prepared_request->predecessor_projection_revision);
          Require(false,
                  "final runtime composition differs from its exact prepared Apply; "
                  "normalized_mismatching_fields=[" + mismatches + "]" +
                      "; original_configuration_finalize=" +
                      (original_configuration_finalize ? "true" : "false") +
                      "; actual_scene_revision=" +
                      std::to_string(request.candidate_projection_revision
                                         .scene_revision.revision_id) +
                      "; prepared_scene_revision=" +
                      std::to_string(prepared_request->candidate_projection_revision
                                         .scene_revision.revision_id));
        }
        require_installed_candidate_configuration();
        const auto candidate_load_start = DiagnosticClock::now();
        const auto selected_bundle =
            LoadExactManagedCandidateBundle(config, request);
        composition_timing.candidate_load += DiagnosticClock::now() - candidate_load_start;
        Require(selected_bundle.has_value(),
                "final runtime composition candidate bundle is unavailable");
        const auto &bundle = *selected_bundle->bundle;
        const auto projection_select_start = DiagnosticClock::now();
        const auto final_projections =
            bundle.value().cell_graph.controllable_components.empty()
                ? std::optional<ManagedComponentProjectionSet>{}
                : SelectManagedComponentProjections(
                      config, bundle, projections,
                      request.candidate_projection_revision, true);
        composition_timing.projection_select += DiagnosticClock::now() - projection_select_start;
        Require(bundle.value().cell_graph.controllable_components.empty() ||
                    final_projections.has_value(),
                "final runtime composition has no installed Scene projection");
        Composition *current = nullptr;
        {
          const auto lock_start = DiagnosticClock::now();
          std::lock_guard lock(managed_composition_mutex);
          composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
          Require(installed_under_fence_composition_request == prepared_request &&
                      !current_composition_building &&
                      !managed_composition_building &&
                      (bundle.value().cell_graph.controllable_components.empty() ||
                       active_managed_composition != nullptr) &&
                      (active_managed_composition == nullptr ||
                       active_managed_composition->projection_revision ==
                           prepared_request->candidate_projection_revision),
                  "prepared runtime changed before final Scene handoff");
          current = active_managed_composition.get();
          installed_under_fence_composition_request = request;
        }
        if (current != nullptr) {
          run_with_installed_candidate([&] {
                std::unique_lock execution_lock(execution_composition.mutex);
                Require(active_managed_composition.get() == current &&
                            final_projections.has_value(),
                        "native composition changed before final Scene");
                RefreshRetainedNativePublicationView(
                    *current, final_projections->by_component,
                    final_projections->required_projection_pins,
                    request.candidate_projection_revision,
                    current->publication_generation,
                    config.manager_run_root);
              });
          InstallDirectSceneFrameResolver(projections, *current);
        } else {
          // Mirror the same exact final Scene handoff for the real logical
          // product; no physical projection/owner is manufactured.
          run_with_installed_candidate([&] {
                std::unique_lock execution_lock(execution_composition.mutex);
                auto *product = execution_composition.product();
                Require(!execution_composition.current && product &&
                            product->origin_named_apply ==
                                core::NamedApplyPublicationTransactionDTO{
                                    request.apply_id, request.apply_generation} &&
                            product->projection_revision ==
                                prepared_request->candidate_projection_revision &&
                            product->bundle->retained_bundle_id() ==
                                request.candidate_projection_revision.retained_bundle_id &&
                            product->bundle->value().cell_graph.graph_revision ==
                                request.candidate_projection_revision.graph_revision,
                        "logical product changed before final Scene handoff");
                product->projection_revision = request.candidate_projection_revision;
              });
        }
      }
    }
    const bool publication_is_successor =
        CurrentPublicationIsInstalledSuccessor(config, request);
    const auto successor_bundle =
        publication_is_successor
            ? LoadExactManagedCandidateBundle(config, request)
            : std::optional<ManagedBundleAtPinnedRoot>{};
    bool reconcile_installed_candidate = false;
    bool published_installed_empty = false;
    Composition *published_installed_composition = nullptr;
    {
      const auto lock_start = DiagnosticClock::now();
      std::lock_guard lock(managed_composition_mutex);
      composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
      if (installed_composition_request.has_value()) {
        if (*installed_composition_request == request) {
          if (!publication_is_successor) {
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kInstalled,
                    installed_component_receipts};
          }
          if (current_composition_building) {
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing,
                    {}};
          }
          if (active_managed_composition == nullptr && original_configuration_finalize &&
              successor_bundle && successor_bundle->bundle->value().cell_graph.controllable_components.empty()) {
            // The exact installed empty request retains the original cell
            // owner; its postpublication context still needs reconciliation.
            current_composition_building = true;
            published_installed_empty = true;
          } else
          {
            Require(active_managed_composition != nullptr,
                    "published installed runtime has no active composition");
            current_composition_building = true;
            published_installed_composition = active_managed_composition.get();
          }
        }
      }
      if (published_installed_composition == nullptr && !published_installed_empty &&
          installed_under_fence_composition_request.has_value()) {
        Require(*installed_under_fence_composition_request == request,
                "another selected runtime composition is installed under "
                "the held fence");
        if (request.phase == component_manager::
                                 ManagerSelectedRuntimeCompositionPhase::kPrepare) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kInstalled,
                  {}};
        }
        reconcile_installed_candidate = true;
      }
      if (published_installed_composition == nullptr && !published_installed_empty &&
          !reconcile_installed_candidate) {
        if (current_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
        if (publication_is_successor) {
          const auto expected_generation =
              core::ActiveBundlePublicationGeneration(
                  request.expected_predecessor_publication_generation.value() +
                  1U);
          if (active_managed_composition == nullptr) {
            if (successor_bundle->bundle->value()
                    .cell_graph.controllable_components.empty()) {
              installed_component_receipts = read_installed_receipts();
              installed_composition_request = request;
              confirm_installed_projection_set(request);
              publish_installed_liveness_handoff(request);
              return {request,
                      component_manager::
                          ManagerSelectedRuntimeCompositionState::kInstalled,
                      installed_component_receipts};
            }
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing,
                    {}};
          }
          Require(active_managed_composition->bundle->retained_bundle_id() ==
                          request.candidate_projection_revision
                              .retained_bundle_id &&
                      active_managed_composition->projection_revision ==
                          request.candidate_projection_revision &&
                      active_managed_composition->publication_generation ==
                          expected_generation,
                  "recovered active composition differs from the published "
                  "Apply successor");
          current_composition_building = true;
          published_installed_composition =
              active_managed_composition.get();
        }
        Require(request.phase == component_manager::
                    ManagerSelectedRuntimeCompositionPhase::kPrepare,
                "final runtime composition has no exact prepared candidate");
        if (retired_managed_composition != nullptr) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
      }
    }
    if (published_installed_composition != nullptr || published_installed_empty) {
      bool attached = false;
      std::optional<core::ActiveBundlePublicationReceiptDTO> attached_head;
      try {
        // Rebind retained owners only after the exact successor is published,
        // while the parent Apply fence still blocks component motion.
        if (published_installed_composition != nullptr) {
          std::vector<StagedRetainedTcpRebind> staged;
          const core::NamedApplyPublicationTransactionDTO apply{request.apply_id, request.apply_generation};
          {
            const auto lock_start = DiagnosticClock::now();
            std::lock_guard lock(managed_composition_mutex);
            composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
            if (staged_retained_tcp_apply == apply) staged = staged_retained_tcp_rebinds;
          }
          if (!staged.empty()) {
            const auto head = read_current_publication();
            fence_owner.RunWithExactHeldSuccessor(fence_query, head, [&] {
              const auto views = IndexNativeComponentCompositions(*published_installed_composition);
              for (const auto &item : staged) {
                Require(std::any_of(views.begin(), views.end(), [&](const auto &view) {
                          return view.second->ros2_control_runtime_handle == item.handle;
                        }), "retained TCP candidate lost its exact handle");
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
                if (item.scene_projection) {
                  scene_source = [scene_projection] {
                    const auto projection = scene_projection.lock();
                    return projection ? projection->logical_snapshot() : nullptr;
                  };
                }
                const auto installed = item.handle->RebindRetainedToolContext(
                    item.rows, item.direct_binding, std::move(source), std::move(committed),
                    std::move(pose), item.real_vendor_lane, std::move(scene_source),
                    std::move(enable_source));
                if (!installed.ok()) Fail("retained TCP rebind failed: " + installed.error().message);
              }
              wake_payload_follower();
            });
            const auto lock_start = DiagnosticClock::now();
            std::lock_guard lock(managed_composition_mutex);
            composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
            Require(staged_retained_tcp_apply == apply,
                    "retained TCP candidate changed during published rebind");
            staged_retained_tcp_rebinds.clear();
            staged_retained_tcp_apply.reset();
          }
        }
        if (original_configuration_finalize) {
          const auto head = read_current_publication();
          auto *const facade = managed_cell_cclc.facade.get();
          const auto *const original_apply = std::get_if<
              core::NamedApplyPublicationTransactionDTO>(&head.request.transaction_ref);
          Require(facade && managed_cell_cclc.publication &&
                      head.valid() && original_apply &&
                      *original_apply == core::NamedApplyPublicationTransactionDTO{
                          request.apply_id, request.apply_generation},
                  "configuration handover lost its actual CCLC owner or successor");
          fence_owner.RunWithExactHeldSuccessor(fence_query, head, [&] {
            if (managed_cell_cclc.publication == head) {
              attached = true;
              return;
            }
            Require(installed_composition_request == request &&
                        managed_cell_cclc.publication->publication_generation ==
                        request.expected_predecessor_publication_generation &&
                        managed_cell_cclc.publication->durable_public_pointer_content_digest ==
                            request.expected_predecessor_public_pointer_digest,
                    "configuration handover changed its original product predecessor");
            if (!managed_cell_cclc.pending_product) {
              // Storage writes finish their operation pins before exchange.
              // Plan/adoption authority remains the original host fence above.
              const auto plan = facade->LoadNativeApplyPlan(
                  {request.apply_id, request.apply_generation});
              Require(plan.ok() && plan.value() && !plan.value()->component_bring_up,
                      "configuration handover has no exact ordinary native plan");
              const auto prepared = [&]() {
                if (published_installed_empty) {
                  Require(successor_bundle && installed_composition_request == request &&
                              plan.value()->apply == core::NamedApplyPublicationTransactionDTO{
                                  request.apply_id, request.apply_generation} &&
                              plan.value()->candidate_retained_bundle_id == head.request.candidate_retained_bundle_id &&
                              plan.value()->candidate_graph_revision == head.request.candidate_graph_revision &&
                              plan.value()->components.empty() && plan.value()->routes.empty() &&
                              plan.value()->retained_components.empty() && plan.value()->retained_domains.empty(),
                          "empty handover has no exact recorded zero-component plan");
                  // Shared preparation verifies the complete empty product and
                  // pinned package before any journal adoption is committed.
                  auto empty = managed_cell_cclc.PrepareEmptyProduct(config, successor_bundle->bundle, head);
                  if (!empty.ok()) return empty;
                  const auto adopted = facade->AdoptNativeApplyFacts({head, {}, {}});
                  if (!adopted.ok())
                    Fail("empty configuration handover could not adopt its recorded facts: " +
                         adopted.error().message);
                  return empty;
                }
                const auto adopted = facade->AdoptNativeApplyFacts(ReadNativeApplyFacts(
                    *published_installed_composition, head, *plan.value()));
                if (!adopted.ok())
                  Fail("configuration handover could not adopt current facts: " + adopted.error().message);
                return managed_cell_cclc.PrepareInstalledProduct(*published_installed_composition, config, head);
              }();
              if (!prepared.ok())
                Fail("configuration handover could not prepare its actual product: " +
                     prepared.error().message);
              // Stage all allocating metadata before installing the one
              // prepared value; a failed capture retains the original context.
              auto exact_request = request;
              auto owner = facade->coordinator().request_owner_epoch();
              managed_cell_cclc.pending_product_request = std::move(exact_request);
              managed_cell_cclc.pending_product_owner = std::move(owner);
              managed_cell_cclc.pending_product = prepared.value();
            }
            Require(managed_cell_cclc.pending_product_request == request &&
                        managed_cell_cclc.pending_product_owner &&
                        facade == managed_cell_cclc.facade.get(),
                    "configuration handover lost its original prepared identity");
            // Copy the allocating successor receipt before the context swap.
            auto successor = head;
            const auto exchanged = facade->TryExchangeProductContext(
                *managed_cell_cclc.pending_product,
                *managed_cell_cclc.pending_product_owner,
                *managed_cell_cclc.publication);
            if (!exchanged.ok())
              Fail("configuration product exchange could not be qualified: " +
                   exchanged.error().message);
            attached = exchanged.value().phase ==
                core::ComponentLifecycleProductExchangePhase::kCompleted;
            if (attached) {
              Require(exchanged.value().current_publication == head &&
                          exchanged.value().owner_epoch ==
                              *managed_cell_cclc.pending_product_owner,
                      "completed configuration product exchange changed identity");
              // NativeAdopt verified the complete installed route inventory;
              // completed exchange now selects that exact product. Clear only
              // this attachment gate, retaining the Manager claim/fence and
              // every independent Stop/fault/admission exclusion.
              if (published_installed_composition != nullptr) {
                Require(published_installed_composition->active_addressed_kernel()
                            ->SetRouteRecoveryPending(false).ok(),
                        "completed configuration exchange could not clear route recovery");
                published_installed_composition->route_recovery_pending.store(
                    false, std::memory_order_release);
                published_installed_composition->cclc_attached.store(true, std::memory_order_release);
              }
              managed_cell_cclc.publication = std::move(successor);
              managed_cell_cclc.pending_product.reset();
              managed_cell_cclc.pending_product_request.reset();
              managed_cell_cclc.pending_product_owner.reset();
            }
          });
        } else
        {
          attached = AttachCurrentPublishedCCLCIfReady(
              *published_installed_composition, config, fence_owner, fence_query);
          if (attached) attached_head = read_current_publication();
        }
        const auto lock_start = DiagnosticClock::now();
        std::lock_guard lock(managed_composition_mutex);
        composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
        Require(active_managed_composition.get() ==
                    published_installed_composition,
                "published installed runtime changed during route recovery");
        if (attached) {
          if (published_installed_empty) {
            std::unique_lock execution_lock(execution_composition.mutex);
            auto *logical = execution_composition.product();
            Require(!execution_composition.current && logical &&
                        logical->origin_named_apply ==
                            core::NamedApplyPublicationTransactionDTO{
                                request.apply_id, request.apply_generation} &&
                        logical->projection_revision == request.candidate_projection_revision,
                    "completed logical exchange lost its actual cell product");
            logical->cclc_attached.store(true, std::memory_order_release);
            logical->route_recovery_pending.store(false, std::memory_order_release);
          }
          if (!managed_cell_cclc.facade) {
            // A publication race can defer the initial attachment. Transfer
            // that same eventual owner before publishing its Reset callbacks;
            // no component-owned reference may outlive a later replacement.
            Require(attached_head.has_value(), "late cell CCLC attachment lost its exact publication");
            std::unique_lock execution_lock(execution_composition.mutex);
            managed_cell_cclc.TakeAttached(*published_installed_composition, *attached_head);
            ConfigureProgramResetPositionAccess(execution_composition,
                core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
                *managed_cell_cclc.facade);
          }
          if (installed_composition_request == request) {
            // This exact Apply already proved controller retention before
            // retiring its predecessor. Replay that receipt; the released
            // predecessor is no longer a live source of lifetime evidence.
            Require(component_manager::ComponentReceiptsMatchRequest(
                        request, installed_component_receipts),
                    "installed receipt replay differs from its exact Apply");
          } else {
            installed_component_receipts = read_installed_receipts();
          }
          installed_composition_request = request;
          confirm_installed_projection_set(request);
          publish_installed_liveness_handoff(request);
        }
        current_composition_building = false;
      } catch (...) {
        const auto lock_start = DiagnosticClock::now();
        std::lock_guard lock(managed_composition_mutex);
        composition_timing.composition_wait += DiagnosticClock::now() - lock_start;
        current_composition_building = false;
        if (original_configuration_finalize)
          selected_composition_failure = std::current_exception();
        throw;
      }
      return {
          request,
          attached
              ? component_manager::ManagerSelectedRuntimeCompositionState::
                    kInstalled
              : component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
          attached ? installed_component_receipts
                   : std::vector<component_manager::
                         ManagerRuntimeComponentReceiptDTO>{}};
    }
    if (reconcile_installed_candidate) {
      require_installed_candidate_configuration();
      // Finalization is still a private parent-Apply effect. Publish and
      // read back every required native route before reporting the selected
      // runtime installed; a later CCLC service attachment is not a route
      // readiness substitute. The Apply fence continues to block commands.
      if (active_managed_composition != nullptr) {
        run_with_installed_candidate([&] {
              const auto candidate_package = LoadManagedBundleAtPinnedRoot(
                  config, request.candidate_projection_revision
                              .retained_bundle_id);
              Require(candidate_package.has_value(),
                      "finalized native Apply bundle is unavailable");
              const deployment::FilesystemVerifiedRetainedProductArtifactSource
                  artifacts(candidate_package->package_root);
              const auto representatives =
                  CellControlBindingRepresentatives(*active_managed_composition);
              const auto &candidate = *active_managed_composition->bundle;
              // CCLC records one decision before any candidate route exists.
              // Retained facts are taken from the live Kernel only after the
              // host proves that the exact predecessor controller survived.
              const std::array<const Composition *, 1>
                  actual_other_predecessors{failed_prepared_composition.get()};
              auto plan_request = BuildNativeApplyPlanRequest(
                  request, *active_managed_composition,
                  retired_managed_composition != nullptr
                      ? retired_managed_composition.get()
                      : (IsExactRetainedNativeViewApply(request)
                             ? active_managed_composition.get()
                             : nullptr),
                  representatives, actual_other_predecessors);
              plan_request.changed_component_state =
                  core::ComponentLifecycleStateDTO::kDeclared;
              const auto plan = [&]() -> core::Result<core::NativeApplyPlanDTO> {
                {
                  std::lock_guard lock(managed_composition_mutex);
                  if (original_apply_bring_up_capture &&
                      original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
                      original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation) {
                    Require(original_apply_native_plan && original_apply_accepted_children &&
                                original_apply_native_plan->component_bring_up == original_apply_accepted_children &&
                                original_apply_native_plan->apply == plan_request.apply &&
                                original_apply_native_plan->candidate_retained_bundle_id ==
                                    plan_request.candidate_retained_bundle_id &&
                                original_apply_native_plan->candidate_graph_revision == plan_request.candidate_graph_revision &&
                                managed_cell_cclc.facade.get() ==
                                    original_apply_bring_up_capture->original_facade,
                            "original finalization lost its pre-effect accepted plan");
                    // Holding children have already advanced their genuine facts.
                    // The original pre-effect plan remains the recorded decision;
                    // finalization must not allocate again or reclassify them as Declared.
                    return core::Result<core::NativeApplyPlanDTO>::Ok(*original_apply_native_plan);
                  }
                }
                Require(managed_cell_cclc.facade != nullptr,
                        "configuration Apply lost the existing CCLC journal owner");
                return managed_cell_cclc.facade->PlanNativeApply(plan_request);
              }();
              if (!plan.ok()) {
                Fail("native CCLC could not record the Apply plan: " +
                     plan.error().message);
              }
              auto &installed = *active_managed_composition;
              EnsureGripperOnlyCellPorts(installed);
              Require(installed.runtime != nullptr ||
                          (installed.cclc_cell_route_publication_owner != nullptr &&
                           installed.cclc_cell_command_readiness_port != nullptr),
                      "installed scalar candidate has no complete CCLC route/readiness owners");
              auto &route_owner =
                  installed.cclc_cell_route_publication_owner != nullptr
                      ? *installed.cclc_cell_route_publication_owner
                      : static_cast<deployment::IRouteConfigurationKernelPublicationOwner &>(
                            *installed.runtime);
              auto &readiness_owner = installed.runtime != nullptr
                  ? static_cast<core::ICCLCComponentCommandReadinessPort &>(
                        *installed.runtime)
                  : static_cast<core::ICCLCComponentCommandReadinessPort &>(
                        *installed.cclc_cell_command_readiness_port);
              const auto initialized = InitializeRetainedNativeCandidateControlRoutes(
                  candidate, *installed.active_addressed_kernel(), route_owner,
                  readiness_owner, installed.publication_generation,
                  plan.value(), artifacts, representatives, fence_query);
              if (!initialized.ok()) {
                Fail("native CCLC candidate routes are not ready: " +
                     initialized.error().message);
              }
            });
      }
      {
        std::lock_guard lock(managed_composition_mutex);
        Require(installed_under_fence_composition_request.has_value() &&
                    *installed_under_fence_composition_request == request,
                "installed runtime composition changed during fence "
                "reconciliation");
        installed_component_receipts = read_installed_receipts();
        installed_composition_request = request;
        confirm_installed_projection_set(request);
        publish_installed_liveness_handoff(request);
        installed_under_fence_composition_request.reset();
        preparing_composition_request.reset();
      }
      try_release_retired_composition();
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kInstalled,
              installed_component_receipts};
    }

    std::unique_ptr<Composition> prepared;
    std::optional<CompositionBuildInput> retained_view_input;
    bool prepared_is_empty = false;
    bool start_builder = false;
    std::shared_ptr<WallClock> shared_clock;
    std::shared_ptr<SimpleScheduler> shared_scheduler;
    std::map<core::ControllableComponentId, RetainedNativeComponentSnapshot>
        retained_snapshots;
    std::vector<core::ControllableComponentId> actual_retained_components;
    std::vector<core::ControllableComponentId> actual_removed_components;
    std::vector<std::shared_ptr<ControllerLaunchProcess>> processes_to_retire;
    const auto actual_candidate =
        LoadExactManagedCandidateBundle(config, request);
    Require(
        actual_candidate && actual_candidate->bundle,
        "actual_installed_state_unknown: candidate product is not verified");
    {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (preparing_composition_request.has_value() &&
          *preparing_composition_request != request) {
        Fail("another selected runtime composition is still preparing");
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_retained_view.has_value()) {
        retained_view_input = std::move(prepared_retained_view);
        prepared_retained_view.reset();
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else if (!preparing_composition_request.has_value()) {
        VerifyRetainedNativePredecessors(active_managed_composition.get(),
                                         request);
        std::map<core::ControllableComponentId, const Composition *>
            actual_keepers;
        std::set<const Composition *> included_roots;
        std::set<const ControllerLaunchProcess *> included_processes;
        const std::array<const Composition *, 5> roots{
            active_managed_composition.get(), retired_managed_composition.get(),
            prepared_managed_composition.get(),
            failed_prepared_composition.get(), execution_composition.current};
        std::shared_lock execution_lock(execution_composition.mutex);
        for (const auto *root : roots) {
          if (!root || !included_roots.insert(root).second)
            continue;
          for (const auto &[id, owner] :
               IndexNativeComponentCompositions(*root)) {
            const bool owns_io = owner->io_provider_drivers && !owner->io_provider_drivers->empty();
            const bool owns_physical = owner->runtime || owner->gripper_runtime;
            Require(owner->bundle && (owns_io || owns_physical) &&
                        (!owns_physical || ((owner->controller_process || IsLocalNoOutputGripperOwner(*owner)) &&
                                            owner->ros2_control_runtime_handle)) &&
                        owner->component_runtime_identity(),
                    "actual_installed_state_unknown: component has no exact "
                    "ROS/process owner");
            const auto wanted = deployment::derive_component_runtime_identity(
                *actual_candidate->bundle, id);
            const auto installed =
                deployment::derive_component_runtime_identity(*owner->bundle,
                                                              id);
            Require(installed.has_value(),
                    "actual_installed_state_unknown: owned component source "
                    "identity is absent");
            if (!wanted)
              actual_removed_components.push_back(id);
            const bool owned_io_live = owner->io_provider_drivers &&
                !owner->io_provider_drivers->empty() &&
                std::all_of(owner->io_provider_drivers->begin(), owner->io_provider_drivers->end(),
                    [](const auto& driver) { return driver && driver->resources_installed(); });
            // A dead simulated PLC (pure I/O owner) is not a keeper either:
            // its component is rebuilt and the PLC restarted by this Apply.
            const bool launch_live = owned_launch_live(
                owner->controller_process.get(), owns_physical,
                IsLocalNoOutputGripperOwner(*owner));
            if (wanted == installed && launch_live && (!owns_io || owned_io_live)) {
              const auto [existing, inserted] =
                  actual_keepers.emplace(id, owner);
              Require(inserted ||
                          existing->second->component_runtime_identity() ==
                              owner->component_runtime_identity(),
                      "actual_installed_state_unknown: component has multiple "
                      "live target owners");
            }
          }
        }
        for (const auto &[id, owner] : actual_keepers) {
          actual_retained_components.push_back(id);
          retained_snapshots.emplace(
              id, RetainedNativeComponentSnapshot{
                      owner->runtime, owner->gripper_runtime, owner->stage,
                      owner->bundle->value()
                          .compiled_products.planning_collision_model,
                      owner->direct_control_joint_limits,
                      owner->direct_control_motion_descriptor,
                      owner->direct_control_gripper_descriptor,
                      owner->ros2_control_runtime_handle,
                      owner->controller_process, owner->io_provider_drivers,
                      owner->component_runtime_pins});
        }
        std::sort(actual_removed_components.begin(),
                  actual_removed_components.end());
        actual_removed_components.erase(
            std::unique(actual_removed_components.begin(),
                        actual_removed_components.end()),
            actual_removed_components.end());
        for (const auto *root : included_roots)
          for (const auto &[id, owner] :
               IndexNativeComponentCompositions(*root)) {
            const auto kept = actual_keepers.find(id);
            if (kept != actual_keepers.end() &&
                ((owner->component_runtime_pins &&
                  kept->second->component_runtime_pins ==
                      owner->component_runtime_pins) ||
                 (owner->controller_process
                     ? kept->second->controller_process == owner->controller_process
                     : IsLocalNoOutputGripperOwner(*owner) &&
                           IsLocalNoOutputGripperOwner(*kept->second) &&
                           kept->second->gripper_runtime == owner->gripper_runtime)))
              continue;
            Require(std::binary_search(request.configuration_fence_scope
                                           .controllable_component_ids.begin(),
                                       request.configuration_fence_scope
                                           .controllable_component_ids.end(),
                                       id),
                    "actual_installed_state_unknown: actual process retirement "
                    "is outside the accepted fence scope");
            if (owner->controller_process &&
                included_processes.insert(owner->controller_process.get()).second &&
                !SimulatedPlcSurvivesRebuild(owner->controller_process,
                                             *actual_candidate->bundle, id) &&
                !PoseSourceSurvivesRebuild(owner->controller_process,
                                           *actual_candidate->bundle,
                                           core::RuntimeUnitId(config.runtime_unit_id),
                                           id, config.execution_run_root,
                                           config.manager_run_root))
              processes_to_retire.push_back(owner->controller_process);
          }
        preparing_composition_request = request;
        managed_composition_building = true;
        start_builder = true;
        if (stable_addressed_kernel() != nullptr) {
          shared_clock = detached_owner_clock;
          shared_scheduler = detached_owner_scheduler;
        }
      }
    }

    if (start_builder) {
      // Preparation creates only closed-gate ROS endpoints. Qualify the exact
      // authority before it starts; executor activation remains fenced below.
      try {
        // Added-domain configuration facts do not exist until installation.
        // Qualify the durable predecessor authority without requiring them.
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [] {});
      } catch (...) {
        std::lock_guard lock(managed_composition_mutex);
        managed_composition_failure = std::current_exception();
        managed_composition_building = false;
        throw;
      }
      managed_composition_builder = std::jthread(
          [&, request, fence_query, shared_clock = std::move(shared_clock),
           shared_scheduler = std::move(shared_scheduler),
           retained_snapshots = std::move(retained_snapshots)
               ,
           actual_retained_components = std::move(actual_retained_components),
           actual_removed_components = std::move(actual_removed_components),
           processes_to_retire = std::move(processes_to_retire)
      ]() mutable {
            try {
              auto input = BuildManagedCompositionInput(
                  config, projections, admission_gate, request,
                  std::move(shared_clock), std::move(shared_scheduler));
              if (input.has_value()) {
                input->ros_context_provider = host_ros_context_provider;
                input->motion_trace = host_motion_trace;
                input->retained_component_snapshots =
                    std::move(retained_snapshots);
                input->observation_wake = stable_observation_wake;
              }
              // A data/setup inconsistency found while composing refuses
              // this Apply only: typed, with its cause, logged once here.
              const auto refuse = [&](const std::exception &error,
                                      const std::string &component_id = {}) {
                auto refusal =
                    host_detail::ScenarioCompositionRefusal(error, component_id);
                std::cerr << "ERROR native composition refused: apply="
                          << request.apply_id.value()
                          << (component_id.empty() ? "" : " component=")
                          << component_id << " cause=" << error.what() << '\n';
                return refusal;
              };
              // The outgoing lifecycle reader must see the exact Replace /
              // Remove decision before ensure() retires its controller.
              // These are planned floors, never installed readiness proof.
              std::vector<core::RuntimeParticipantBinding> planned_bindings;
              if (input.has_value()) {
                std::string composing_component;
                try {
                  const deployment::FilesystemVerifiedCatalogArtifactSource
                      catalogs(input->package_root);
                  const auto inventory =
                      deployment::compile_direct_control_program_binding_inventory(
                          *input->bundle, input->model_map, catalogs);
                  for (const auto &component :
                       input->bundle->value().cell_graph.controllable_components) {
                    composing_component =
                        component.controllable_component_id.value();
                    auto selected = *input;
                    selected.selected_component_id =
                        component.controllable_component_id;
                    auto physical =
                        ResolveNativePhysicalBindings(selected, inventory);
                    planned_bindings.insert(
                        planned_bindings.end(),
                        std::make_move_iterator(physical.representatives.begin()),
                        std::make_move_iterator(physical.representatives.end()));
                  }
                } catch (const std::exception &error) {
                  throw refuse(error, composing_component);
                }
              }
              const auto planned_candidate = LoadExactManagedCandidateBundle(
                  config, request);
              const auto planned_predecessor = LoadManagedBundleAtPinnedRoot(
                  config,
                  request.predecessor_projection_revision.retained_bundle_id);
              Require(planned_candidate.has_value() &&
                          planned_predecessor.has_value(),
                      "managed Apply planning lost its verified products");
              fence_owner.RunWithExactHeldPredecessor(
                  fence_query,
                  request.expected_predecessor_publication_generation,
                  request.expected_predecessor_public_pointer_digest, [&] {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(preparing_composition_request == request,
                            "managed Apply planning lost its original request");
                    VerifyRetainedNativePredecessors(
                        active_managed_composition.get(), request);
                    std::vector<core::RuntimeRouteConfigurationReadbackDTO>
                        retained_routes;
                    for (const auto &desired : planned_candidate->bundle
                                                   ->value()
                                                   .desired_control_configurations) {
                      const bool retained = std::any_of(
                          request.component_delta.components.begin(),
                          request.component_delta.components.end(),
                          [&](const auto &component) {
                            return std::binary_search(
                                       actual_retained_components.begin(),
                                       actual_retained_components.end(),
                                       component.controllable_component_id) &&
                                   std::binary_search(
                                       component.candidate_domains.begin(),
                                       component.candidate_domains.end(),
                                       desired.control_domain_id);
                          });
                      if (!retained)
                        continue;
                      const auto route = stable_addressed_kernel()
                                             ->current_runtime_route_configuration(
                                                 desired.control_domain_id);
                      Require(route.ok() && route.value().has_value(),
                              "managed Apply retained route is unavailable before preparation");
                      retained_routes.push_back(*route.value());
                    }
                    auto derived = DeriveRetainedNativeApplyPlanRequest(
                        {request.apply_id, request.apply_generation},
                        *planned_candidate->bundle,
                        planned_predecessor->bundle.get(),
                        request.component_delta, planned_bindings,
                        retained_routes,
                        core::ComponentLifecycleStateDTO::kDeclared,
                        std::span<const core::ControllableComponentId>(
                            actual_retained_components),
                        std::span<const core::ControllableComponentId>(
                            actual_removed_components));
                    if (!derived.ok())
                      Fail("managed Apply plan derivation failed: " +
                           derived.error().message);
                    Require(managed_cell_cclc.facade != nullptr,
                            "managed Apply planning lost the existing CCLC journal owner");
                    const auto recorded =
                        managed_cell_cclc.facade->PlanNativeApply(derived.value());
                    if (!recorded.ok())
                      Fail("managed CCLC could not record Apply before preparation: " +
                           recorded.error().message);
                    if (input)
                      input->retained_routes = std::move(retained_routes);
                  });
              for (const auto &process : processes_to_retire) {
                if (process->cleanup_observation().state !=
                    ControllerLaunchCleanupState::kDrained) {
                  deployment::CCLCRuntimeComposition *facade = nullptr;
                  {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(preparing_composition_request == request &&
                                managed_cell_cclc.facade,
                            "actual_installed_state_unknown: retirement lost "
                            "its original component owner");
                    facade = managed_cell_cclc.facade.get();
                  }
                  const core::ControllableComponentId component(
                      process->command().controllable_component_id);
                  const auto predecessor = read_current_publication();
                  const auto snapshot =
                      facade->ReadCurrentComponentSnapshot(component);
                  Require(snapshot.ok() && snapshot.value().locally_valid() &&
                              snapshot.value().controllable_component_id ==
                                  component &&
                              snapshot.value().active_publication_receipt ==
                                  predecessor &&
                              snapshot.value().component_lifecycle_state !=
                                  core::ComponentLifecycleStateDTO::kActive,
                          "actual_installed_state_unknown: retiring component "
                          "lacks exact inactive owner facts; restart the "
                          "workbench container");
                  fence_owner.RunWithExactHeldPredecessor(
                      fence_query,
                      request.expected_predecessor_publication_generation,
                      request.expected_predecessor_public_pointer_digest, [&] {
                        std::lock_guard lock(managed_composition_mutex);
                        std::shared_lock execution_lock(
                            execution_composition.mutex);
                        Require(preparing_composition_request == request &&
                                    managed_cell_cclc.facade.get() == facade &&
                                    read_current_publication() == predecessor,
                                "actual_installed_state_unknown: retirement's "
                                "original owner/publication changed");
                        bool still_owned = false;
                        for (const auto *root :
                             std::array<const Composition *, 5>{
                                 active_managed_composition.get(),
                                 retired_managed_composition.get(),
                                 prepared_managed_composition.get(),
                                 failed_prepared_composition.get(),
                                 execution_composition.current}) {
                          if (!root)
                            continue;
                          const auto owners =
                              IndexNativeComponentCompositions(*root);
                          const auto found = owners.find(component);
                          still_owned =
                              still_owned ||
                              (found != owners.end() &&
                               found->second->controller_process == process);
                        }
                        Require(still_owned,
                                "actual_installed_state_unknown: exact "
                                "retiring process was superseded");
                      });
                  static_cast<void>(
                      host_controller_launch_supervisor().retire_exact(
                          process));
                }
                const auto cleanup = process->cleanup_observation();
                Require(cleanup.state ==
                                ControllerLaunchCleanupState::kDrained &&
                            cleanup.receipt,
                        "actual_installed_state_unknown: old component group "
                        "has not drained; restart the workbench container");
              }
              const bool retain_view =
                  input.has_value() && IsExactRetainedNativeViewApply(request)
                  && actual_retained_components.size() ==
                         input->bundle->value()
                             .cell_graph.controllable_components.size() &&
                  active_managed_composition != nullptr
                  ;
              // Capture candidate-owned facts before installing or publishing anything.
              auto staged_tcp = input ? StageRetainedTcpRebinds(*input)
                                      : std::vector<StagedRetainedTcpRebind>{};
              // A throw here destroys the partial candidate: ~Composition
              // discards every launch it started and never adopted.
              std::unique_ptr<Composition> built;
              if (input.has_value() && !retain_view) {
                try {
                  built = BuildProductComposition(std::move(*input));
                } catch (const std::exception &error) {
                  throw refuse(error);
                }
              }
              std::lock_guard lock(managed_composition_mutex);
              prepared_managed_composition = std::move(built);
              staged_retained_tcp_rebinds = std::move(staged_tcp);
              staged_retained_tcp_apply = core::NamedApplyPublicationTransactionDTO{
                  request.apply_id, request.apply_generation};
              if (retain_view)
                prepared_retained_view = std::move(input);
              prepared_managed_composition_is_empty = !input.has_value();
              managed_composition_building = false;
            } catch (...) {
              std::lock_guard lock(managed_composition_mutex);
              managed_composition_failure = std::current_exception();
              managed_composition_building = false;
            }
          });
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kPreparing,
              {}};
    }
    if (prepared == nullptr && !prepared_is_empty &&
        !retained_view_input.has_value()) {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_retained_view.has_value()) {
        retained_view_input = std::move(prepared_retained_view);
        prepared_retained_view.reset();
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else {
        if (managed_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
        Fail("selected runtime preparation was consumed without an exact "
             "installed result; automatic replacement is forbidden");
      }
    }
    if (managed_composition_builder.joinable()) {
      managed_composition_builder.join();
    }
    // Timing diagnostics for the install below: the whole install runs inside
    // this one Manager exchange, whose budget is per installed component.
    const auto install_started = std::chrono::steady_clock::now();
    const auto install_ms = [&install_started] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - install_started)
          .count();
    };
    std::cerr << "native composition install: start apply="
              << request.apply_id.value() << " received_unix_ms="
              << composition_request_received_unix_ms << " received_to_start_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     install_started - composition_request_received)
                     .count()
              << '\n';

    std::shared_ptr<host_detail::ExecutionProduct> prepared_logical_product;
    if (prepared_is_empty) {
      Require(request.expected_predecessor_publication_generation.value() <
                  std::numeric_limits<std::uint64_t>::max(),
              "logical candidate publication generation is exhausted");
      prepared_logical_product = BuildManagedLogicalExecutionProduct(
          config, actual_candidate->bundle, actual_candidate->package_root,
          request.candidate_projection_revision,
          core::ActiveBundlePublicationGeneration(
              request.expected_predecessor_publication_generation.value() + 1U),
          {request.apply_id, request.apply_generation}, detached_owner_clock,
          detached_owner_scheduler);
    }

    IoProviderConstructionGenerations output_construction_generations;
    std::optional<core::NativeApplyPlanDTO> io_construction_plan;
    const auto construction_bundle = prepared ? prepared->bundle :
        (retained_view_input ? retained_view_input->bundle : nullptr);
    if (construction_bundle) {
      const auto rows = deployment::ResolveRetainedIoSignalCatalog(construction_bundle->value().cell_graph);
      Require(rows.ok(), "output construction catalog is invalid");
      if (std::any_of(construction_bundle->value().cell_graph.control_domains.begin(),
                      construction_bundle->value().cell_graph.control_domains.end(),
                      [](const auto &domain) { return domain.provider() != nullptr; })) {
        auto *const facade = current_cell_cclc();
        Require(facade != nullptr, "output construction lost actual CCLC journal owner");
        const auto actual = facade->LoadNativeApplyPlan({request.apply_id, request.apply_generation});
        Require(actual.ok() && actual.value() &&
                    actual.value()->candidate_retained_bundle_id == construction_bundle->retained_bundle_id() &&
                    actual.value()->candidate_graph_revision == construction_bundle->value().cell_graph.graph_revision,
                "output construction has no exact durable candidate Apply plan");
        io_construction_plan = *actual.value();
        for (const auto &component : actual.value()->components)
          Require(component.generation.nonzero() && output_construction_generations.emplace(
                      component.component_id, component.generation).second,
                  "output construction Apply plan repeats or omits a generation");
        if (original_apply_bring_up_capture &&
            original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
            original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation) {
          Require(actual.value()->component_bring_up.has_value(),
                  "linked output construction lost actual accepted child receipts");
          const auto &capture = original_apply_bring_up_capture;
          const core::ApplyComponentBringUpAuthorityDTO authority{
              actual.value()->component_bring_up->parent, capture->parent_claim,
              fence_query};
          const auto accepted = read_original_apply_child_generations(capture, authority);
          for (const auto &[component, generation] : accepted)
            output_construction_generations.insert_or_assign(component, generation);
        }
      }
    }

    const auto stage_installed_provider_candidate = [&](Composition &candidate) {
      std::set<core::ControllableComponentId> components;
      for (const auto &domain : request.configuration_fence_scope.domains) {
        const auto *sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
        if (sides && sides->candidate_provider)
          components.insert(sides->candidate_provider->controllable_component_id);
      }
      if (components.empty()) return;
      Require(io_construction_plan && configuration_parent_claim,
              "installed provider candidate has no original recorded plan/held parent");
      auto *const facade = current_cell_cclc();
      Require(facade != nullptr, "installed provider candidate lost actual lifecycle owner");
      const auto selected = LoadExactManagedCandidateBundle(config, request);
      Require(selected && selected->bundle->canonical_bytes() == candidate.bundle->canonical_bytes(),
              "installed provider candidate differs from its exact sealed product");
      EnsureInstalledRetainedHostLifecycleProduct(candidate, selected->package_root);
      const auto qualified = BuildManagedCandidateLifecyclePlanningClosure(
          config, *selected, candidate.projection_revision, candidate.publication_generation);
      Require(qualified.ok(), "installed provider candidate has no qualified lifecycle closure");
      // Pure actual source reads finish before entering Kernel publication.
      // The existing outer ledger/effect/composition pin is already held.
      std::vector<core::InstalledProviderCandidateFactsDTO> facts;
      if (!io_construction_plan->component_bring_up) {
        facts = ReadInstalledNativeProviderCandidateFacts(config, *selected, candidate,
            *facade, *io_construction_plan, request.configuration_fence_scope);
      } else {
      for (const auto &component : components) {
        const auto member = std::find_if(qualified.value().components.begin(), qualified.value().components.end(),
            [&](const auto &value) { return value.component_id == component; });
        Require(member != qualified.value().components.end(), "installed provider component is foreign");
        const auto actual = ReadInstalledManagedCandidateLifecycleComponentClosure(candidate, *member);
        Require(actual.ok(), "installed provider candidate resources/catalog/ports differ");
        const auto captured = original_apply_bring_up_capture &&
                original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
                original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation
            ? original_apply_bring_up_capture : nullptr;
        const auto *child = captured ? [&]() -> const component_manager::ManagerApplyBringUpChildRequestDTO * {
          const auto found = std::find_if(captured->sealed_plan.plan.children.begin(),
              captured->sealed_plan.plan.children.end(), [&](const auto &value) {
                return value.intent.controllable_component_id == component;
              });
          return found == captured->sealed_plan.plan.children.end() ? nullptr : &*found;
        }() : nullptr;
        auto observed = [&]() -> core::Result<core::InstalledProviderCandidateFactsDTO> {
          if (child) {
            Require(captured->original_facade == facade &&
                        captured->parent_claim == *configuration_parent_claim &&
                        captured->sealed_plan.plan.apply_id == request.apply_id &&
                        captured->sealed_plan.plan.apply_generation == request.apply_generation,
                    "installed provider child has another original owner/parent");
            const core::ApplyComponentBringUpReferenceDTO parent{
                {request.apply_id, request.apply_generation}, captured->sealed_plan.plan_digest};
            const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                child->intent, child->candidate_projection_revision, child->candidate_retained_bundle_digest,
                child->candidate_retained_bundle_bytes, child->runtime_unit_id, child->runtime_epoch,
                child->manager_runtime_epoch, parent);
            return facade->coordinator().ReadInstalledApplyCandidateFacts(child->intent, digest,
                actual.value().domains, actual.value().providers, actual.value().ports);
          }
          return facade->ReadInstalledNativeApplyCandidateFacts(*io_construction_plan, *candidate.bundle,
              candidate.cclc_lifecycle_product->catalog_source(), component,
              actual.value().domains, actual.value().providers, actual.value().ports);
        }();
        Require(observed.ok(), "original lifecycle owner refused installed candidate facts");
        facts.push_back(std::move(observed.value()));
      }
      }
      for (const auto &actual : facts) {
        bool already_installed = true;
        for (const auto &domain : actual.domains) {
          if (!domain.provider()) continue;
          const auto current = stable_kernel_owner->current_runtime_provider_configuration(domain.control_domain_id);
          const auto occurrence = std::find_if(actual.provider_occurrences.begin(), actual.provider_occurrences.end(),
              [&](const auto &value) { return value.control_domain_id == domain.control_domain_id; });
          already_installed = already_installed && current.ok() && current.value() &&
              current.value()->locally_valid() && current.value()->retained_bundle_id == actual.retained_bundle_id &&
              current.value()->graph_revision == actual.graph_revision && current.value()->domain == domain &&
              current.value()->component_lifecycle_generation == actual.component_lifecycle_generation &&
              occurrence != actual.provider_occurrences.end() && current.value()->provider_occurrence == *occurrence;
        }
        if (already_installed) continue;
        const auto staged = stable_kernel_owner->publish_installed_provider_candidate_closed(
            fence_query, *configuration_parent_claim, actual);
        Require(staged.ok(), "Kernel refused exact original installed provider CLOSED metadata");
      }
    };

    const auto unchanged_provider_fence = [&](
        const core::ResolvedLifecycleControlDomainScopeDTO &domain) {
      const auto scoped = std::find_if(
          request.configuration_fence_scope.domains.begin(),
          request.configuration_fence_scope.domains.end(),
          [&](const auto &value) {
            return value.control_domain_id == domain.control_domain_id;
          });
      const auto *sides = scoped == request.configuration_fence_scope.domains.end()
                              ? nullptr
                              : std::get_if<core::ProviderConfigurationFenceDomainDTO>(
                                    &scoped->payload);
      const auto *provider = domain.provider();
      if (!sides || !provider || !sides->predecessor_provider ||
          !sides->candidate_provider ||
          *sides->predecessor_provider != *sides->candidate_provider ||
          scoped->predecessor_control_domain_revision !=
              std::optional(domain.control_domain_revision) ||
          scoped->candidate_control_domain_revision !=
              std::optional(domain.control_domain_revision) ||
          !std::binary_search(
              request.configuration_fence_scope.controllable_component_ids.begin(),
              request.configuration_fence_scope.controllable_component_ids.end(),
              provider->provider.controllable_component_id))
        return false;
      const auto &proof = *sides->predecessor_provider;
      return proof.controllable_component_id ==
                 provider->provider.controllable_component_id &&
             proof.provider_instance_id == provider->provider.provider_instance_id &&
             proof.signal_configuration_digest ==
                 provider->provider.signal_configuration_digest &&
             proof.output_resources == domain.resource_ids();
    };
    const auto exact_predecessor = [&](
        const CompositionIoResourcePredecessor &predecessor) {
      return predecessor.bundle &&
             predecessor.publication_generation ==
                 request.expected_predecessor_publication_generation &&
             predecessor.bundle->retained_bundle_id() ==
                 request.predecessor_projection_revision.retained_bundle_id &&
             predecessor.bundle->value().cell_graph.graph_revision ==
                 request.predecessor_projection_revision.graph_revision;
    };

    // Witness actual retained generic domains independently of whole-component
    // physical retention. Mixed components may replace their physical engine
    // while keeping the SAME concrete provider DriverRecord.
    const auto retained_provider_witness = [&](const Composition &candidate,
        std::span<const CompositionIoResourcePredecessor> predecessors) {
      std::map<core::ControlDomainId, std::pair<
          core::ResolvedLifecycleControlDomainScopeDTO,
          core::ResolvedLifecycleProviderOccurrenceDTO>> paired;
      for (const auto &[component, owner] : IndexNativeComponentCompositions(candidate)) {
        static_cast<void>(component);
        if (!owner->io_provider_drivers) continue;
        for (const auto &driver : *owner->io_provider_drivers) {
          Require(driver && driver->resources_installed(),
                  "retained provider witness has no installed actual driver");
          const auto changed = std::find_if(request.configuration_fence_scope.domains.begin(),
              request.configuration_fence_scope.domains.end(), [&](const auto &scope) {
                return scope.control_domain_id == driver->domain().control_domain_id;
              });
          if (changed != request.configuration_fence_scope.domains.end()) {
            // Provider fence membership alone does not retain readiness. The
            // two sides must prove the same provider and the actual record
            // must still be owned by this exact predecessor publication.
            if (!unchanged_provider_fence(driver->domain())) continue;
          }
          bool same_actual_record = false;
          for (const auto &predecessor : predecessors) {
            if (changed != request.configuration_fence_scope.domains.end() &&
                !exact_predecessor(predecessor))
              continue;
            for (const auto &group : predecessor.driver_groups)
              if (group && std::find(group->begin(), group->end(), driver) != group->end())
                same_actual_record = true;
          }
          if (!same_actual_record) continue;
          Require(paired.emplace(driver->domain().control_domain_id,
                      std::make_pair(driver->domain(), driver->occurrence())).second,
                  "retained provider witness repeats an actual domain");
        }
      }
      std::pair<std::vector<core::ResolvedLifecycleControlDomainScopeDTO>,
                std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>> result;
      for (const auto &[id, values] : paired) {
        static_cast<void>(id);
        result.first.push_back(values.first);
        result.second.push_back(values.second);
      }
      return result;
    };

    try {
      fence_owner.RunWithExactHeldPredecessor(
        fence_query, request.expected_predecessor_publication_generation,
        request.expected_predecessor_public_pointer_digest, [&] {
          std::unique_lock execution_lock(execution_composition.mutex);
          std::cerr << "native composition install: fence and composition "
                       "lock held elapsed_ms=" << install_ms() << '\n';
          Require(retired_managed_composition == nullptr &&
                      retired_registration_engines.empty(),
                  "a predecessor runtime engine remains lifetime-pinned");
          const std::array<const Composition *, 3> io_predecessors{
              active_managed_composition.get(), retired_managed_composition.get(),
              failed_prepared_composition.get()};
          const auto io_resources = CaptureCompositionIoResourcePredecessors(io_predecessors);
          if (retained_view_input.has_value()) {
            Require(IsExactRetainedNativeViewApply(request) &&
                        active_managed_composition != nullptr &&
                        active_managed_composition->bundle != nullptr &&
                        retained_view_input->bundle != nullptr &&
                        RetainedNativeViewFollowsPredecessor(
                            active_managed_composition->bundle
                                ->retained_bundle_id(),
                            active_managed_composition->projection_revision,
                            retained_view_input->bundle->retained_bundle_id(),
                            request.predecessor_projection_revision),
                    "retained native view crossed its verified predecessor");
            auto &candidate = *active_managed_composition;
            Require(candidate.component_runtime_identity() != nullptr &&
                        candidate.active_addressed_kernel() ==
                            stable_addressed_kernel(),
                    "retained native view lost its stable command owner");
            const auto generation =
                retained_view_input->publication_generation;
            RefreshRetainedNativePublicationView(
                candidate,
                retained_view_input->component_planner_scene_projections,
                retained_view_input->required_projection_pins,
                request.candidate_projection_revision, generation,
                config.manager_run_root);
            candidate.bundle = std::move(retained_view_input->bundle);
            // Adopt this Apply's selected Program products and NamedApply
            // provenance exactly as BuildProductComposition does for a full
            // candidate. The global catalog staged below is then activated at
            // the parent claim release from this Apply's own terminal record;
            // a stale origin made that release skip activation and left the
            // predecessor catalog/service current (design rule B).
            candidate.origin_named_apply =
                retained_view_input->origin_named_apply;
            if (retained_view_input->selected_program_products) {
              const auto &selected =
                  *retained_view_input->selected_program_products;
              candidate.program = selected.program;
              candidate.program_binding = selected.binding;
              candidate.selected_program_outcome = selected.outcome;
            }
            PrepareCompositionIoResources(candidate, *stable_cell_runtime_owner,
                                          generic_io_transport, host_ros_context_provider, io_resources);
            const auto bound_outputs = BindCompositionIoOutputParticipants(candidate, output_construction_generations);
            Require(bound_outputs.ok(), "retained output construction failed");
            DetachRetainedCCLC(candidate);
            Require(candidate.active_addressed_kernel()
                        ->SetRouteRecoveryPending(true)
                        .ok(),
                    "retained native view could not close route recovery");
            candidate.route_recovery_pending.store(true,
                                                   std::memory_order_release);
            InstallDirectSceneFrameResolver(projections, candidate);
            std::vector<core::ControllableComponentId> retained_components;
            for (const auto &[component, view] : IndexNativeComponentCompositions(candidate)) {
              static_cast<void>(view);
              retained_components.push_back(component);
            }
            const auto witness = retained_provider_witness(candidate, io_resources);
            auto registrations = NativeCellComponentRegistrations(candidate);
            RetainedRuntimeParticipantResolverFactory staged_factory;
            auto resolver = NativeCellParticipantResolver(candidate, registrations, &staged_factory);
            const auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_component_set(
                std::move(registrations), std::move(resolver), NativeCellStopSources(candidate),
                candidate.bundle->retained_bundle_id(), candidate.bundle->value().cell_graph.graph_revision,
                request.configuration_fence_id, request.configuration_fence_scope, retained_components,
                {}, witness.first, witness.second,
                configuration_parent_claim ? &*configuration_parent_claim : nullptr);
            if (!reconfigured.ok()) Fail("stable Kernel rejected retained product transition: " + reconfigured.error().message);
            AdoptNativeCellParticipantResolverFactory(candidate, std::move(staged_factory));
            BindHostIoRuleEvidenceProcessor(candidate, execution_composition_owner);
            InstallCompositionIoSignals(
                candidate, *stable_cell_runtime_owner, &fence_query,
                configuration_parent_claim ? &*configuration_parent_claim
                                           : nullptr);
            stage_installed_provider_candidate(candidate);
          } else if (prepared_is_empty) {
            Require(prepared == nullptr,
                    "empty selected product has a physical composition");
            if (active_managed_composition) DetachRetainedCCLC(*active_managed_composition);
            auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_component_set(
                {}, {}, {}, request.candidate_projection_revision.retained_bundle_id,
                request.candidate_projection_revision.graph_revision,
                request.configuration_fence_id, request.configuration_fence_scope, {});
            if (!reconfigured.ok()) Fail("stable Kernel rejected the empty selected runtime: " + reconfigured.error().message);
            Require(prepared_logical_product && configuration_parent_claim,
                    "logical global candidate lost its exact Apply product");
            // Retain actual service/source ownership before fallible staging.
            execution_composition.replace_logical_product(prepared_logical_product);
            BindHostIoRuleEvidenceProcessor(*prepared_logical_product, execution_composition_owner);
            InstallCompositionIoSignals(
                *prepared_logical_product, *stable_cell_runtime_owner,
                &fence_query, &*configuration_parent_claim);
            if (active_managed_composition) {
              retired_registration_engines = snapshot_registration_engines(*active_managed_composition);
              retired_managed_composition = std::move(active_managed_composition);
            }
            Require(stable_kernel_owner->SetRouteRecoveryPending(false).ok(),
                    "empty selected runtime could not release route recovery");
          } else {
            Require(prepared != nullptr,
                    "physical selected product has no prepared composition");
            auto *const stable_owner = stable_addressed_kernel();
            if (stable_owner == nullptr) {
              attach_retained_native_addressed_owner(
                  prepared->runtime,
                  RetainedNativeAddressedOwnerConfig{
                      core::RequestOwnerEpoch("kernel-owner-" +
                                              config.runtime_epoch),
                      core::CellId(config.cell_id),
                      config.execution_state_root /
                          std::string(core::kOperationJournalFilename),
                      1024U,
                      &admission_gate,
                      prepared->runtime->motion_binding.value().graph_revision,
                      {},
                      BuildOwnerSettlementValidator(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          config.logical_scene_database,
                          config.program_cycle_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      BuildOwnerReconciliationValidator(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          config.program_cycle_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      {},
                      [path = config.program_cycle_journal](const auto purpose, const auto& intent, const auto& claim) {
                        return detail::ReadProgramResetStatusReadOnly(path, purpose, intent, claim);
                      }});
              admission_gate.BindResourceTable(
                  prepared->runtime->addressed_kernel
                      ->resource_admission_table());
            } else {
              Require(stable_cell_runtime_owner && detached_owner_clock && detached_owner_scheduler &&
                          (!prepared->runtime || (prepared->runtime->clock == detached_owner_clock &&
                           prepared->runtime->scheduler == detached_owner_scheduler)),
                      "prepared runtime crossed the stable Kernel time capabilities");
              std::optional<std::set<
                  core::IResolvedParticipantExecutionEngine *>>
                  predecessor_engine_snapshot;
              const auto retained_component_ids =
                  RetainedNativeComponentIds(request);
              auto stable_stop_observer = stable_stop_surveillance_owner;
              std::vector<RetainedStopSurveillanceSourceInput>
                  candidate_stop_sources;
              if (active_managed_composition != nullptr) {
                predecessor_engine_snapshot =
                    snapshot_registration_engines(*active_managed_composition);
                DetachRetainedCCLC(*active_managed_composition);
              }
              if (!retained_component_ids.empty()) {
                Require(active_managed_composition != nullptr &&
                            (prepared->runtime == nullptr ||
                             prepared->candidate_participant_resolver_factory),
                        "retained native Apply has no predecessor or candidate "
                        "participant resolver");
                ResealRetainedNativeCandidateOwners(
                    *prepared, *active_managed_composition, request,
                    admission_gate);
                RefreshRetainedNativeCandidateCollision(
                    *prepared, request, admission_gate);
                RefreshRetainedNativeCandidateModels(
                    *prepared, request, admission_gate,
                    prepared->runtime != nullptr
                        ? *prepared->runtime->clock
                        : *stable_cell_runtime_owner->clock());
                if (prepared->runtime != nullptr) {
                  std::vector<std::shared_ptr<RetainedNativeRuntime>> peers;
                  peers.reserve(prepared->participant_compositions.size());
                  for (const auto &component :
                       prepared->participant_compositions) {
                    Require(component != nullptr &&
                                component->component_runtime_identity() != nullptr,
                            "retained native Apply has an incomplete peer");
                    if (component->runtime != nullptr)
                      peers.push_back(component->runtime);
                  }
                  auto rejoined = rejoin_retained_native_runtime_participants(
                      prepared->runtime, std::move(peers),
                      prepared->candidate_participant_resolver_factory,
                      stable_stop_observer, candidate_stop_sources,
                      admission_gate, request.configuration_fence_id,
                      request.configuration_fence_scope);
                  if (!rejoined.ok())
                    Fail("retained native cell participant rejoin failed: " +
                         rejoined.error().message);
                  AppendNativeGripperStopSources(*prepared,
                                                 candidate_stop_sources);
                } else {
                  candidate_stop_sources = NativeCellStopSources(*prepared);
                }
                std::map<core::ControllableComponentId,
                         std::shared_ptr<RetainedPlannerSceneProjection>>
                    selected_projections;
                const auto include_projection = [&](const Composition &view) {
                  if (view.planner_scene_projection == nullptr) {
                    Require(view.runtime == nullptr &&
                                (view.gripper_runtime != nullptr ||
                                 (view.io_provider_drivers && !view.io_provider_drivers->empty())),
                            "retained motion candidate has no Scene projection");
                    return;
                  }
                  Require(selected_projections
                              .emplace(view.controllable_component_id,
                                       view.planner_scene_projection)
                              .second,
                          "retained native candidate has a duplicate Scene "
                          "projection");
                };
                include_projection(*prepared);
                for (const auto &component :
                     prepared->participant_compositions)
                  include_projection(*component);
                RefreshRetainedNativePublicationView(
                    *prepared, selected_projections,
                    prepared->required_projection_pins,
                    request.candidate_projection_revision,
                    prepared->publication_generation,
                    config.manager_run_root);
              }
              if (retained_component_ids.empty()) {
                candidate_stop_sources = NativeCellStopSources(*prepared);
              }
              PrepareCompositionIoResources(*prepared, *stable_cell_runtime_owner,
                                            generic_io_transport, host_ros_context_provider, io_resources);
              const auto bound_outputs = BindCompositionIoOutputParticipants(*prepared, output_construction_generations);
              Require(bound_outputs.ok(), "candidate output construction failed");
              auto candidate_registrations =
                  NativeCellComponentRegistrations(*prepared);
              RetainedRuntimeParticipantResolverFactory staged_factory;
              auto candidate_resolver = NativeCellParticipantResolver(
                  *prepared, candidate_registrations, &staged_factory);
              static_cast<void>(LoadManagedBootstrapStopProduct(
                  config, prepared->bundle->retained_bundle_id(),
                  prepared->bundle->value().cell_graph.graph_revision));
              const auto witness = retained_provider_witness(*prepared, io_resources);
              auto reconfigured =
                  stable_cell_runtime_owner->reconfigure_runtime_component_set(
                      std::move(candidate_registrations), std::move(candidate_resolver),
                      std::move(candidate_stop_sources),
                      request.candidate_projection_revision.retained_bundle_id,
                      request.candidate_projection_revision.graph_revision,
                      request.configuration_fence_id,
                      request.configuration_fence_scope, retained_component_ids,
                      {}, witness.first, witness.second,
                      configuration_parent_claim ? &*configuration_parent_claim : nullptr);
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the selected runtime "
                     "participants: " +
                     reconfigured.error().message);
              }
              std::cerr << "native composition install: Kernel reconfigured "
                           "elapsed_ms=" << install_ms() << '\n';
              AdoptNativeCellParticipantResolverFactory(*prepared, std::move(staged_factory));
              const auto adopt_launch = [&](Composition &component) {
                component.controller_launch_adopted = true;
              };
              adopt_launch(*prepared);
              for (auto &peer : prepared->participant_compositions)
                adopt_launch(*peer);
              if (prepared->runtime != nullptr)
                prepared->runtime->stop_surveillance_owner =
                    stable_stop_surveillance_owner;
              if (active_managed_composition) {
                Require(predecessor_engine_snapshot.has_value(), "predecessor engine snapshot is missing");
                retired_registration_engines = std::move(*predecessor_engine_snapshot);
                retired_managed_composition = std::move(active_managed_composition);
              }
            }
            // The stable Kernel and its registered engines must outlive every
            // fallible initialization below. Retain the candidate under the
            // held fence before those calls; failure must not destroy the
            // admission owner still referenced by the command surface.
            active_managed_composition = std::move(prepared);
            ApplyComponentStreamOverrides(active_managed_composition->bundle.get());
            if (active_managed_composition->runtime != nullptr)
              active_managed_composition->runtime->BindCellRuntimeOwner(
                    *stable_cell_runtime_owner, config.runtime_epoch, config.cell_id);
            bind_cell_runtime_to_composition(*active_managed_composition);
            BindHostIoRuleEvidenceProcessor(*active_managed_composition, execution_composition_owner);
            InstallCompositionIoSignals(
                *active_managed_composition, *stable_cell_runtime_owner,
                &fence_query,
                configuration_parent_claim ? &*configuration_parent_claim
                                           : nullptr);
            execution_composition.replace_current(active_managed_composition.get());
            execution_composition.addressed_kernel = stable_addressed_kernel();
            stop_recovery_runtime.store(active_managed_composition->runtime.get(),
                                        std::memory_order_release);
            auto &candidate = *active_managed_composition;
            Require(
                candidate.active_addressed_kernel()
                    ->SetRouteRecoveryPending(true)
                    .ok(),
                "selected runtime could not close route recovery admission");
            candidate.route_recovery_pending.store(true,
                                                   std::memory_order_release);
            // Program/Scene-only activation has no changed control domains.
            // There are no candidate configuration facts to stage in that case.
            if (!request.configuration_fence_scope.resources.empty()) {
              auto staged =
                candidate.active_addressed_kernel()
                    ->stage_current_runtime_configuration_facts_under_apply_fence(
                        request.configuration_fence_scope);
              if (!staged.ok()) {
                Fail("stable Kernel rejected candidate configuration staging: " +
                   staged.error().message);
              }
            }
            stage_installed_provider_candidate(candidate);
            // The candidate, stable Kernel and Stop engines are now retained
            // under this exact Apply fence. Executor start does not wait for a
            // controller or issue a home goal; observed readiness remains NRT.
            for (const auto &[component_id, view] :
                 IndexNativeComponentCompositions(candidate)) {
              const auto component_started = install_ms();
              std::cerr << "native composition install: component="
                        << component_id.value()
                        << " bring-up start elapsed_ms=" << component_started
                        << '\n';
              if (view->ros2_control_runtime_handle)
                view->ros2_control_runtime_handle->StartBackend();
              else
                Require(view->io_provider_drivers &&
                            !view->io_provider_drivers->empty() &&
                            std::all_of(view->io_provider_drivers->begin(),
                                view->io_provider_drivers->end(), [](const auto &record) {
                                  return record && record->resources_installed();
                                }),
                        "installed component has no actual transport resources");
              std::cerr << "native composition install: component="
                        << component_id.value() << " bring-up done took_ms="
                        << install_ms() - component_started << '\n';
            }
            if (original_apply_native_plan &&
                original_apply_native_plan->apply ==
                    core::NamedApplyPublicationTransactionDTO{request.apply_id, request.apply_generation}) {
              Require(original_apply_native_plan->component_bring_up &&
                          original_apply_native_plan->component_bring_up == original_apply_accepted_children &&
                          original_apply_native_plan->candidate_retained_bundle_id ==
                              candidate.bundle->retained_bundle_id(),
                      "installed original Apply lost its accepted native plan");
              const auto package = LoadExactManagedCandidateBundle(config, request);
              Require(package && package->bundle->canonical_bytes() == candidate.bundle->canonical_bytes(),
                      "installed original Apply lost its exact product artifacts");
              EnsureInstalledRetainedHostLifecycleProduct(candidate, package->package_root);
              const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(package->package_root);
              const auto representatives = CellControlBindingRepresentatives(candidate);
              EnsureGripperOnlyCellPorts(candidate);
              auto& route_owner = candidate.runtime
                  ? static_cast<deployment::IRouteConfigurationKernelPublicationOwner&>(*candidate.runtime)
                  : *candidate.cclc_cell_route_publication_owner;
              auto& readiness_owner = candidate.runtime
                  ? static_cast<core::ICCLCComponentCommandReadinessPort&>(*candidate.runtime)
                  : static_cast<core::ICCLCComponentCommandReadinessPort&>(*candidate.cclc_cell_command_readiness_port);
              const auto routes = InitializeRetainedNativeCandidateControlRoutes(
                  *candidate.bundle, *candidate.active_addressed_kernel(), route_owner,
                  readiness_owner, candidate.publication_generation, *original_apply_native_plan,
                  artifacts, representatives, fence_query);
              if (!routes.ok())
                Fail("installed original Apply routes are unavailable: " + routes.error().message);
            }
            InstallDirectSceneFrameResolver(projections, candidate);
#if !defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
            if (candidate.runtime != nullptr) {
              const auto planning_started = install_ms();
              AttachProductPickPlacePlanningOwner(candidate);
              std::cerr << "native composition install: pick-place planning "
                           "owner attached took_ms="
                        << install_ms() - planning_started << '\n';
            }
#endif
          }
          if (active_managed_composition) {
            BindHostIoRuleEvidenceProcessor(*active_managed_composition, execution_composition_owner);
            InstallCompositionIoSignals(
                *active_managed_composition, *stable_cell_runtime_owner,
                &fence_query,
                configuration_parent_claim ? &*configuration_parent_claim
                                           : nullptr);
          }
          if (active_managed_composition) {
            execution_composition.replace_current(active_managed_composition.get());
            wake_payload_follower();
          } else {
            Require(prepared_logical_product &&
                        execution_composition.product() == prepared_logical_product.get(),
                    "logical installed product changed before final selection");
          }
          execution_composition.addressed_kernel = stable_addressed_kernel();
          stop_recovery_runtime.store(
              active_managed_composition == nullptr
                  ? nullptr
                  : active_managed_composition->runtime.get(),
              std::memory_order_release);
          // Update only the successfully installed occurrence. This includes
          // the exact retained-view path; cached earlier incident facts stay
          // on their original request context inside the process owner.
          if (active_managed_composition) {
            const auto context =
                "Apply=" + request.apply_id.value() + " generation=" +
                std::to_string(request.apply_generation.value());
            const auto update_context = [&](Composition &component) {
              component.controller_request_context = context;
              if (component.controller_process)
                component.controller_process->update_request_context(context);
              // Every Apply starts without stored trajectories: a retained
              // Kernel may now serve another robot, model or Program.
              if (component.runtime) component.runtime->cache->invalidate_all();
            };
            update_context(*active_managed_composition);
            for (auto &peer :
                 active_managed_composition->participant_compositions)
              update_context(*peer);
          }
          installed_under_fence_composition_request = request;
          preparing_composition_request.reset();
          std::cerr << "native composition install: installed elapsed_ms="
                    << install_ms() << '\n';
        });
    } catch (...) {
      // Preparation was consumed. Preserve the first failure rather than
      // attempting another build on a later observation of this Apply.
      selected_composition_failure = std::current_exception();
      if (prepared != nullptr) {
        prepared->discard_unadopted_controller_launches();
        failed_prepared_composition = std::move(prepared);
      }
      throw;
    }
    require_installed_candidate_configuration();
    if (request.phase == component_manager::
                             ManagerSelectedRuntimeCompositionPhase::kPrepare) {
      return {request,
              component_manager::ManagerSelectedRuntimeCompositionState::
                  kInstalled,
              {}};
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      Require(installed_under_fence_composition_request.has_value() &&
                  *installed_under_fence_composition_request == request,
              "installed runtime composition changed before fence "
              "reconciliation completed");
      installed_component_receipts = read_installed_receipts();
      installed_composition_request = request;
      confirm_installed_projection_set(request);
      publish_installed_liveness_handoff(request);
      installed_under_fence_composition_request.reset();
    }
    try_release_retired_composition();
    return {
        request,
        component_manager::ManagerSelectedRuntimeCompositionState::kInstalled,
        installed_component_receipts};
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
                installed_request->candidate_projection_revision ==
                    current_publication->request
                        .candidate_projection_revision &&
                installed_request->runtime_unit_id ==
                    required->runtime_unit_id &&
                installed_request->runtime_epoch == required->runtime_epoch &&
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
    std::array<std::jthread *, 3> workers;
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
                              ShutdownPhase::kCurrentBuilderDrain,
                              ShutdownPhase::kManagedBuilderDrain};
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
      {&execution_surface, &current_composition_builder,
       &managed_composition_builder},
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
    // Private Apply installation can precede its public/CCLC handoff. The
    // same exclusion is independently enforced by Core activation/restore.
    if (admission_gate.has_held_configuration_fences())
      return Reply::Ok();
    std::optional<core::ActiveBundlePublicationReceiptDTO> selected;
    deployment::CCLCRuntimeComposition *facade = nullptr;
    std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO> installed_request;
    {
      std::lock_guard lock(managed_composition_mutex);
      std::shared_lock execution_lock(execution_composition.mutex);
      if (active_managed_composition || execution_composition.current ||
          preparing_composition_request ||
          installed_under_fence_composition_request || current_composition_building ||
          managed_composition_building || !managed_cell_cclc.facade ||
          !managed_cell_cclc.publication)
        return Reply::Ok();
      selected = managed_cell_cclc.publication;
      facade = managed_cell_cclc.facade.get();
      installed_request = installed_composition_request;
    }
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
         installed_request->candidate_projection_revision != selected->request.candidate_projection_revision))
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
          managed_cell_cclc.facade.get() == facade &&
          managed_cell_cclc.publication == selected &&
          installed_composition_request == installed_request &&
          !preparing_composition_request &&
          !installed_under_fence_composition_request &&
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
    std::lock_guard lock(managed_composition_mutex);
    {
      std::unique_lock execution_lock(execution_composition.mutex);
      if (active_managed_composition || execution_composition.current ||
          preparing_composition_request ||
          installed_under_fence_composition_request ||
          installed_composition_request != installed_request ||
          managed_cell_cclc.facade.get() != facade ||
          managed_cell_cclc.publication != selected ||
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
        execution_composition.replace_logical_product(logical);
        logical->cclc_attached.store(true, std::memory_order_release);
        logical->route_recovery_pending.store(true, std::memory_order_release);
        BindHostIoRuleEvidenceProcessor(*logical, execution_composition_owner);
        InstallCompositionIoSignals(*logical, *stable_cell_runtime_owner);
      }
    }
    // Keep the SAME facade pinned by the existing managed-owner mutex. The
    // shared helper reads the actual terminal proof outside composition/Core
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
    const auto released = stable_kernel_owner->SetRouteRecoveryPending(false);
    if (released.ok())
      logical->route_recovery_pending.store(false, std::memory_order_release);
    return released;
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
        // The ordinary Manager progress poll also correlates the already minted
        // CCLC incident with its existing process evidence. No new polling owner.
        try {
          struct IncidentCorrelation final {
            std::shared_ptr<ControllerLaunchProcess> process;
            std::string incident_id;
            std::string context;
            std::string summary;
            core::ControllableComponentId component_id;
            std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
            std::shared_ptr<const deployment::RetainedBundle> bundle;
            bool needs_snapshot{};
          };
          std::vector<IncidentCorrelation> correlations;
          {
            const auto lock_start = DiagnosticClock::now();
            std::lock_guard lock(managed_composition_mutex);
            timing.composition_wait += DiagnosticClock::now() - lock_start;
            auto *cell = active_managed_composition.get();
            auto *const cclc = current_cell_cclc();
            if (cell && cclc) {
              const auto snapshot = [&](Composition &component) {
                if (!component.controller_process ||
                    (component.ros2_control_runtime_handle &&
                     component.ros2_control_runtime_handle
                         ->HasControllerProcessIncidentDiagnostic()))
                  return;
                const auto view =
                    cclc->lifecycle_target_driver().ReadTargetView(
                        component.controllable_component_id);
                if (!view.ok() || !view.value().progress ||
                    !view.value().progress->incident)
                  return;
                const auto &incident = *view.value().progress->incident;
                const auto context =
                    !component.controller_request_context.empty()
                        ? component.controller_request_context
                        : "Bootstrap runtime=" + config.runtime_unit_id +
                              " epoch=" + config.runtime_epoch;
                correlations.push_back(
                    {component.controller_process, incident.incident_id, context,
                     incident.summary, component.controllable_component_id,
                     component.ros2_control_runtime_handle, component.bundle,
                     !incident.diagnostic_snapshot &&
                         incident.containment_state !=
                             core::ComponentIncidentContainmentStateDTO::
                                 kPending});
              };
              snapshot(*cell);
              for (auto &peer : cell->participant_compositions)
                snapshot(*peer);
            }
          }
          // Auxiliary IO is outside composition, CCLC/Kernel and process
          // observation locks. Operational Stop uses its separate control lane.
          for (const auto &correlation : correlations) {
            static_cast<void>(correlation.process->append_incident_correlation(
                correlation.incident_id, correlation.context,
                correlation.summary));
            if (!correlation.needs_snapshot || !correlation.handle ||
                !correlation.bundle ||
                correlation.handle->HasControllerProcessIncidentDiagnostic(
                    correlation.incident_id))
              continue;
            const auto exit = correlation.process->exit_evidence();
            if (!exit)
              continue;
            const auto snapshot = ControllerLaunchIncidentSnapshot(
                *correlation.process, *exit, *correlation.bundle,
                config.runtime_unit_id, config.runtime_epoch);
            const auto lock_start = DiagnosticClock::now();
            std::lock_guard lock(managed_composition_mutex);
            timing.composition_wait += DiagnosticClock::now() - lock_start;
            if (!active_managed_composition)
              continue;
            const auto installed =
                IndexNativeComponentCompositions(*active_managed_composition);
            const auto exact = installed.find(correlation.component_id);
            if (exact != installed.end() &&
                exact->second->controller_process == correlation.process &&
                exact->second->ros2_control_runtime_handle == correlation.handle)
              static_cast<void>(
                  correlation.handle->CacheControllerProcessIncidentDiagnostic(
                      correlation.incident_id, snapshot));
          }
        } catch (...) {
          // Diagnostic capture/write failure cannot suppress the primary fault
          // or break the Manager's ordinary status/liveness protocol.
        }
        Require(source_report_sequence !=
                    std::numeric_limits<std::uint64_t>::max(),
                "managed runtime source sequence is exhausted");
        ++source_report_sequence;
        auto source_status =
            "manyforge.native-execution-status.v2 " + config.runtime_unit_id +
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
              if constexpr (std::is_same_v<
                                T, component_manager::ManagerSelectedRuntimeCompositionRequestDTO>) {
                timing_finalize = value.phase == component_manager::
                    ManagerSelectedRuntimeCompositionPhase::kFinalize;
                if (timing_finalize) {
                  try {
                    timing_apply_id = value.apply_id.value().substr(0, 128);
                    for (auto &ch : timing_apply_id)
                      if (ch <= ' ' || ch >= 127 || ch == '=') ch = '_';
                  } catch (...) {}
                }
                TimingSpan body{composition_timing.finalize_body};
                return install_or_observe_composition(value);
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
                deployment::CCLCRuntimeComposition *lifecycle_owner = nullptr;
                struct CapturedOccurrence final {
                  core::ControllableComponentId component_id;
                  std::shared_ptr<ControllerLaunchProcess> process;
                  std::shared_ptr<const deployment::RetainedBundle> bundle;
                };
                std::vector<CapturedOccurrence> captures;
                std::vector<VerifiedLocalNoneProcessOwner> local_owners;
                std::set<const RetainedNativeGripperRuntime*> included_local_owners;
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
                         managed_composition_building ||
                         preparing_composition_request ||
                         installed_under_fence_composition_request ||
                         prepared_managed_composition ||
                         prepared_retained_view ||
                         prepared_managed_composition_is_empty ||
                         selected_composition_failure != nullptr ||
                         managed_composition_failure != nullptr;
                };
                std::array<const Composition *, 5> before{};
                {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  before = roots();
                  changing = owners_changing();
                  lifecycle_owner = current_cell_cclc();
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
                          local_owners.push_back(ReadVerifiedLocalNoneProcessOwner(*view, value));
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
                                *captured.bundle, captured.component_id)
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
                  // Observe the exact owned leader just as the existing
                  // component BringUp inspection does. These are this host's
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
                  for (const auto &component :
                       previous->bundle->value()
                           .cell_graph.controllable_components) {
                    if (!lifecycle_owner)
                      return failure(
                          "installed_component_lifecycle_unavailable",
                          "current component lifecycle owner is unavailable");
                    const auto fact =
                        lifecycle_owner->ReadCurrentComponentSnapshot(
                            component.controllable_component_id);
                    if (!fact.ok() || !fact.value().locally_valid() ||
                        fact.value().controllable_component_id !=
                            component.controllable_component_id ||
                        !same_publication(
                            fact.value().active_publication_receipt))
                      return failure(
                          "installed_component_lifecycle_unavailable",
                          "current component lifecycle does not match the "
                          "exact publication");
                    result.component_lifecycles.push_back(
                        {component.controllable_component_id,
                         fact.value().component_lifecycle_generation,
                         fact.value().component_lifecycle_state});
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
                  changing = changing || owners_changing() ||
                             roots() != before ||
                             current_cell_cclc() != lifecycle_owner;
                  for (const auto& local : local_owners) {
                    auto* const installed_kernel = stable_addressed_kernel();
                    if (!installed_kernel)
                      return failure("installed_process_inspection_scope_stale", "installed logical owner lost its stable Kernel");
                    const auto route = installed_kernel->current_runtime_route_configuration(
                        local.binding.value().control_path->control_domain_id);
                    if (local.runtime->binding().value() != local.binding.value() ||
                        !local.handle->OwnsGripper(local.runtime->backend(), local.binding) ||
                        !route.ok() || !route.value() || *route.value() != local.route)
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
                  Require(!current_composition_building && !managed_composition_building &&
                              !preparing_composition_request && !installed_under_fence_composition_request,
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
                      component_process_stop_results.size() >= core::kMaximumCCLCComponentWatermarks) {
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
              } else if constexpr (std::is_same_v<
                                       T, component_manager::ManagerComponentBringUpInspectionRequestDTO>) {
                Require(value.valid() && value.runtime_unit_id.value() == config.runtime_unit_id &&
                            value.runtime_epoch.value() == config.runtime_epoch,
                        "component inspection names another host occurrence");
                Require(manager_liveness_installed.load(std::memory_order_acquire),
                        "component inspection has no current Manager observation");
                const auto manager = manager_liveness_receiver.get()->status();
                Require(manager.state == core::ManagerLivenessStateDTO::kCurrent &&
                            !manager.command_fenced &&
                            manager.manager_runtime_epoch == value.manager_runtime_epoch &&
                            manager.receiver_runtime_unit_id == value.runtime_unit_id &&
                            manager.receiver_runtime_epoch == value.runtime_epoch,
                        "component inspection names an unavailable Manager occurrence");
                const auto require_predecessor = [&](const auto &publication) {
                  Require(publication.publication_generation == value.expected_predecessor_publication_generation &&
                              publication.durable_public_pointer_content_digest == value.expected_predecessor_public_pointer_digest &&
                              publication.request.candidate_projection_revision == value.predecessor_projection_revision,
                          "component inspection predecessor publication changed");
                };
                const auto predecessor = read_current_publication();
                require_predecessor(predecessor);
                const auto selected = LoadManagedBundleAtPinnedRoot(
                    config, value.candidate_projection_revision.retained_bundle_id);
                Require(selected && selected->bundle &&
                            selected->bundle->value().cell_graph.cell_id == core::CellId(config.cell_id) &&
                            selected->bundle->value().cell_graph.graph_revision == value.candidate_projection_revision.graph_revision &&
                            selected->bundle->canonical_bytes().size() == value.candidate_retained_bundle_bytes &&
                            core::ContentDigest(content_identity::sha256_digest(selected->bundle->canonical_bytes())) == value.candidate_retained_bundle_digest,
                        "component inspection candidate is not the exact verified bundle");
                const auto predecessor_bundle = LoadManagedBundleAtPinnedRoot(
                    config, value.predecessor_projection_revision.retained_bundle_id);
                Require(predecessor_bundle && predecessor_bundle->bundle,
                        "component inspection predecessor bundle is unavailable");
                std::optional<ManagedCandidateLifecyclePlanningClosure> candidate_planning;
                if (value.planning_component_id) {
                  Require(value.expected_predecessor_publication_generation.value() !=
                              std::numeric_limits<std::uint64_t>::max(),
                          "component planning publication generation is exhausted");
                  auto compiled = BuildManagedCandidateLifecyclePlanningClosure(
                      config, *selected, value.candidate_projection_revision,
                      core::ActiveBundlePublicationGeneration(
                          value.expected_predecessor_publication_generation.value() + 1U));
                  if (!compiled.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{compiled.error()};
                  candidate_planning = std::move(compiled.value());
                }
                deployment::CCLCRuntimeComposition *planning_owner = nullptr;
                bool planning_predecessor_present = false;
                component_manager::ManagerComponentBringUpInspectionResultDTO result{value, {}};
                std::vector<VerifiedLocalNoneProcessOwner> candidate_local_owners;
                const Composition* candidate_local_root = nullptr;
                {
                  // Borrow the existing owners only for synchronous process reads.
                  // No construction, dispatch, shutdown, or epoch mintage occurs.
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  const bool changing = current_composition_building || managed_composition_building ||
                      preparing_composition_request || installed_under_fence_composition_request ||
                      prepared_managed_composition || prepared_retained_view ||
                      prepared_managed_composition_is_empty ||
                      selected_composition_failure != nullptr || managed_composition_failure != nullptr;
                  // Changing component owners are unknown; unknown rows
                  // authorize no bring-up.
                  if (!changing && active_managed_composition)
                    Require(active_managed_composition->projection_revision == value.predecessor_projection_revision &&
                                active_managed_composition->publication_generation == value.expected_predecessor_publication_generation,
                            "component inspection owner belongs to another publication");
                  // A populated published predecessor with no installed owners
                  // is unknown, not proof that its processes are absent.
                  const auto installed = active_managed_composition
                      ? IndexNativeComponentCompositions(*active_managed_composition)
                      : std::map<core::ControllableComponentId, const Composition *>{};
                  std::set<core::ControllableComponentId> predecessor_components;
                  for (const auto &row : predecessor_bundle->bundle->value().cell_graph.controllable_components)
                    predecessor_components.insert(row.controllable_component_id);
                  for (const auto &row : selected->bundle->value().cell_graph.controllable_components) {
                    Require(std::find(row.runtime_unit_ids.begin(), row.runtime_unit_ids.end(), value.runtime_unit_id) != row.runtime_unit_ids.end(),
                            "component inspection candidate names another runtime unit");
                    auto presence = component_manager::ProcessPresenceState::kUnknown;
                    const auto current = installed.find(row.controllable_component_id);
                    if (changing) {
                      // No installed-component presence inference is valid here.
                    } else if (current == installed.end()) {
                      if (!predecessor_components.contains(row.controllable_component_id))
                        presence = component_manager::ProcessPresenceState::kAbsent;
                    } else {
                      if (current->second->controller_process) {
                        Require(current->second->controller_process->command().controllable_component_id == row.controllable_component_id.value(),
                                "component process observation has foreign attribution");
                        presence = current->second->controller_process->exited()
                            ? component_manager::ProcessPresenceState::kAbsent
                            : component_manager::ProcessPresenceState::kPresent;
                      }
                    }
                    component_manager::ManagerComponentProcessObservationDTO observed{
                        row.controllable_component_id, presence};
                    if (!changing && current != installed.end() &&
                        IsLocalNoOutputGripperOwner(*current->second)) {
                      const component_manager::ManagerComponentProcessInventoryRequestDTO scope{
                          value.predecessor_projection_revision,
                          value.expected_predecessor_publication_generation,
                          value.expected_predecessor_public_pointer_digest,
                          value.runtime_unit_id, value.runtime_epoch, value.manager_runtime_epoch};
                      auto local = ReadVerifiedLocalNoneProcessOwner(*current->second, scope);
                      candidate_local_root = active_managed_composition.get();
                      observed.process_presence = component_manager::ProcessPresenceState::kAbsent;
                      observed.process_owner_kind = local.occurrence.process_owner_kind;
                      observed.pneumatic_none_owner = local.occurrence.pneumatic_none_owner;
                      observed.installed_runtime_identity = local.occurrence.installed_runtime_identity;
                      candidate_local_owners.push_back(std::move(local));
                    }
                    result.configured_components.push_back(std::move(observed));
                  }
                  if (value.planning_component_id) {
                    Require(!changing && managed_cell_cclc.facade &&
                                managed_cell_cclc.publication == predecessor,
                            "component planning has no stable original cell owner");
                    planning_owner = managed_cell_cclc.facade.get();
                    planning_predecessor_present =
                        predecessor_components.contains(*value.planning_component_id);
                    if (!planning_predecessor_present) {
                      const auto row = std::find_if(result.configured_components.begin(),
                          result.configured_components.end(), [&](const auto &component) {
                            return component.controllable_component_id == *value.planning_component_id;
                          });
                      Require(!installed.contains(*value.planning_component_id) &&
                                  row != result.configured_components.end() &&
                                  row->process_presence == component_manager::ProcessPresenceState::kAbsent,
                              "initial component planning lacks positive predecessor inventory absence");
                    }
                  }
                }
                std::sort(result.configured_components.begin(), result.configured_components.end(),
                          [](const auto &lhs, const auto &rhs) {
                            return lhs.controllable_component_id < rhs.controllable_component_id;
                          });
                if (planning_owner) {
                  // Journal/catalog reads never run under composition or Kernel
                  // locks. The serial managed caller pins this actual facade;
                  // recreation must first exclude and drain all such callers.
                  const auto component = *value.planning_component_id;
                  const auto owner_epoch = planning_owner->coordinator().request_owner_epoch();
                  const auto generation = planning_owner->ReadApplyComponentGenerationSelection(component);
                  if (!generation.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{generation.error()};
                  std::optional<core::ComponentLifecycleCurrentSnapshotDTO> snapshot;
                  if (planning_predecessor_present) {
                    const auto current = planning_owner->ReadCurrentComponentSnapshot(component);
                    if (!current.ok())
                      return component_manager::detail::SelectedRuntimeCompositionRpcFailure{current.error()};
                    Require(generation.value().owner_qualified_predecessor &&
                                generation.value().owner_qualified_predecessor->generation ==
                                    current.value().component_lifecycle_generation &&
                                generation.value().owner_qualified_predecessor->snapshot_digest ==
                                    current.value().snapshot_digest &&
                                current.value().active_publication_receipt == predecessor,
                            "component planning snapshot differs from the owner-selected predecessor");
                    snapshot = current.value();
                  } else {
                    Require(!generation.value().owner_qualified_predecessor,
                            "initial component planning contains a foreign predecessor snapshot");
                  }
                  const auto target_view = planning_owner->lifecycle_target_driver().ReadTargetView(component);
                  if (!target_view.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{target_view.error()};
                  // This is the same absent-target startup choice used by
                  // native adoption. It is not an achieved lifecycle state.
                  const auto target = target_view.value().target
                      ? target_view.value().target->requested_state
                      : core::LifecycleTargetStateDTO::kActive;
                  const auto next = snapshot
                      ? core::NextLifecycleIntentTowardTarget(snapshot->component_lifecycle_state, target)
                      : std::optional<core::ComponentLifecycleIntentKindDTO>{
                            target == core::LifecycleTargetStateDTO::kDisabled
                                ? core::ComponentLifecycleIntentKindDTO::kDisable
                                : core::ComponentLifecycleIntentKindDTO::kPrepare};
                  Require(next && *next != core::ComponentLifecycleIntentKindDTO::kRecover,
                          "component planning has no approved next Apply bring-up action");
                  const auto closure = std::find_if(candidate_planning->components.begin(),
                      candidate_planning->components.end(), [&](const auto &row) {
                        return row.component_id == component;
                      });
                  Require(closure != candidate_planning->components.end(),
                          "component planning lacks its complete verified candidate closure");
                  const auto generation_after = planning_owner->ReadApplyComponentGenerationSelection(component);
                  const auto target_after = planning_owner->lifecycle_target_driver().ReadTargetView(component);
                  Require(generation_after.ok() && generation_after.value() == generation.value() &&
                              target_after.ok() && target_after.value().target == target_view.value().target &&
                              planning_owner->coordinator().request_owner_epoch() == owner_epoch,
                          "component planning owner facts changed during observation");
                  {
                    std::lock_guard composition_lock(managed_composition_mutex);
                    Require(managed_cell_cclc.facade.get() == planning_owner &&
                                managed_cell_cclc.publication == predecessor &&
                                !current_composition_building && !managed_composition_building &&
                                !preparing_composition_request && !installed_under_fence_composition_request,
                            "component planning original owner changed during observation");
                  }
                  result.planning_facts = component_manager::ManagerComponentBringUpPlanningFactsDTO{
                      component, owner_epoch, generation.value(), closure->candidate_expected_domains,
                      target, *next};
                }
                if (!candidate_local_owners.empty()) {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  Require(active_managed_composition.get() == candidate_local_root &&
                              !current_composition_building && !managed_composition_building &&
                              !preparing_composition_request && !installed_under_fence_composition_request,
                          "local None candidate owner changed during observation");
                  const auto installed = IndexNativeComponentCompositions(*active_managed_composition);
                  const component_manager::ManagerComponentProcessInventoryRequestDTO scope{
                      value.predecessor_projection_revision,
                      value.expected_predecessor_publication_generation,
                      value.expected_predecessor_public_pointer_digest,
                      value.runtime_unit_id, value.runtime_epoch, value.manager_runtime_epoch};
                  for (const auto& local : candidate_local_owners) {
                    const auto current = installed.find(local.occurrence.controllable_component_id);
                    Require(current != installed.end() && current->second->gripper_runtime == local.runtime &&
                                current->second->ros2_control_runtime_handle == local.handle,
                            "local None candidate lost its exact installed owner");
                    const auto readback = ReadVerifiedLocalNoneProcessOwner(*current->second, scope);
                    Require(readback.binding.value() == local.binding.value() && readback.route == local.route &&
                                readback.occurrence == local.occurrence,
                            "local None candidate binding/route changed during observation");
                  }
                }
                require_predecessor(read_current_publication());
                const auto after = manager_liveness_receiver.get()->status();
                Require(after.state == core::ManagerLivenessStateDTO::kCurrent && !after.command_fenced &&
                            after.manager_runtime_epoch == value.manager_runtime_epoch &&
                            after.receiver_runtime_epoch == value.runtime_epoch && result.valid(),
                        "component process inspection lost its current occurrence");
                return result;
              } else {
                constexpr bool read_only =
                    std::is_same_v<T, component_manager::ManagerSelectedComponentBringUpReadRequestDTO> ||
                    std::is_same_v<T, component_manager::ManagerApplyComponentBringUpReadRequestDTO>;
                const auto &original = [&]() -> const auto & {
                  if constexpr (read_only) return value.request;
                  else return value;
                }();
                using Request = std::decay_t<decltype(original)>;
                constexpr bool delegated = std::is_same_v<
                    Request, component_manager::ManagerApplyComponentBringUpRequestDTO>;
                using Response = std::conditional_t<
                    delegated, component_manager::ManagerApplyComponentBringUpResultDTO,
                    component_manager::ManagerSelectedComponentBringUpResultDTO>;
                if constexpr (delegated && !read_only) {
                  const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                      original.intent, original.candidate_projection_revision,
                      original.candidate_retained_bundle_digest, original.candidate_retained_bundle_bytes,
                      original.runtime_unit_id, original.runtime_epoch, original.manager_runtime_epoch,
                      original.parent_authority.parent);
                  deployment::CCLCRuntimeComposition *result_owner{};
                  std::shared_ptr<const ManagedOriginalApplyBringUpCapture> captured;
                  {
                    std::lock_guard lock(managed_composition_mutex);
                    result_owner = current_cell_cclc();
                    captured = original_apply_bring_up_capture;
                  }
                  Require(result_owner, "original delegated result journal is unavailable");
                  // The serial main channel pins this facade for the read. A
                  // retained result is classified before any current capture
                  // authorization, including after owner recreation or restart.
                  const auto retained = result_owner->coordinator().ReadApplyComponentBringUpRequestResult(
                      original.intent, digest);
                  if (!retained.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{retained.error()};
                  if (!retained.value())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                        core::ErrorDTO{"component_bring_up_live_authority_unavailable",
                                       "new bring-up requires the original accepted continuation", {}}};
                  Require(retained.value()->status.has_value(),
                          "delegated continuation has no actual retained child status");
                  const bool same_original_parent = captured &&
                      captured->sealed_plan.plan.apply_id == original.parent_authority.parent.owner.apply_id &&
                      captured->sealed_plan.plan.apply_generation == original.parent_authority.parent.owner.apply_generation &&
                      captured->sealed_plan.plan_digest == original.parent_authority.parent.plan_digest;
                  if (retained.value()->status->phase != core::ComponentLifecycleTransactionPhaseDTO::kAccepted ||
                      !same_original_parent) {
                    Response result{original, retained.value()};
                    Require(result.valid(), "delegated retained result differs from its request");
                    return result;
                  }
                  {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(original_apply_bring_up_capture == captured &&
                                captured->original_facade && result_owner == captured->original_facade &&
                                managed_cell_cclc.facade.get() == captured->original_facade &&
                                original_apply_accepted_children && original_apply_native_plan &&
                                original_apply_native_plan->component_bring_up == original_apply_accepted_children,
                            "new delegated continuation lacks its actual original accepted runtime");
                  }
                  const auto &plan = captured->sealed_plan.plan;
                  const auto child = std::find_if(plan.children.begin(), plan.children.end(),
                      [&](const auto &row) { return row.intent == original.intent; });
                  Require(child != plan.children.end(),
                          "delegated continuation is not a sealed original child");
                  const component_manager::ManagerApplyComponentBringUpRequestDTO expected{
                      child->intent, child->candidate_projection_revision,
                      child->candidate_retained_bundle_digest, child->candidate_retained_bundle_bytes,
                      child->runtime_unit_id, child->runtime_epoch, child->manager_runtime_epoch,
                      original.parent_authority};
                  Require(expected == original && original.parent_authority.parent_claim == captured->parent_claim &&
                              original.parent_authority.parent == core::ApplyComponentBringUpReferenceDTO{
                                  {plan.apply_id, plan.apply_generation}, captured->sealed_plan.plan_digest},
                          "delegated continuation differs from its full original request");
                  auto &coordinator = captured->original_facade->coordinator();
                  const auto qualified = std::find_if(captured->candidate_closure.components.begin(),
                      captured->candidate_closure.components.end(), [&](const auto &row) {
                        return row.component_id == original.intent.controllable_component_id;
                      });
                  Require(qualified != captured->candidate_closure.components.end(),
                          "delegated continuation lost its qualified candidate closure");
                  std::optional<ManagedInstalledLifecycleComponentClosure> installed;
                  core::ICCLCComponentCommandReadinessPort *readiness{};
                  const auto &query = original.parent_authority.held_fence_query;
                  const bool provider_scope = std::any_of(query.expected_scope.domains.begin(),
                      query.expected_scope.domains.end(), [](const auto& domain) {
                        return std::holds_alternative<core::ProviderConfigurationFenceDomainDTO>(domain.payload);
                      });
                  std::optional<OriginalApplyCandidateProof> candidate_proof;
                  if (provider_scope)
                    candidate_proof = read_original_apply_candidate_proof(captured, original.parent_authority);
                  const auto read_installed = [&] {
                    std::lock_guard lock(managed_composition_mutex);
                    if (candidate_proof) revalidate_original_apply_candidate_pin(*candidate_proof);
                    Require(managed_cell_cclc.facade.get() == captured->original_facade &&
                                !current_composition_building && !managed_composition_building &&
                                active_managed_composition && active_managed_composition->runtime &&
                                active_managed_composition->bundle->canonical_bytes() ==
                                    captured->candidate.bundle->canonical_bytes() &&
                                active_managed_composition->projection_revision == original.candidate_projection_revision &&
                                installed_under_fence_composition_request &&
                                installed_under_fence_composition_request->phase ==
                                    component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare &&
                                installed_under_fence_composition_request->apply_id == plan.apply_id &&
                                installed_under_fence_composition_request->apply_generation == plan.apply_generation,
                            "delegated continuation lacks its exact completed installation");
                    const auto actual = ReadInstalledManagedCandidateLifecycleComponentClosure(
                        *active_managed_composition, *qualified);
                    if (!actual.ok()) Fail("delegated installed closure is unavailable: " + actual.error().message);
                    installed = actual.value();
                    readiness = active_managed_composition->runtime.get();
                  };
                  if (candidate_proof) {
                    fence_owner.RunWithExactHeldOriginalApplyCandidate(query,
                        captured->predecessor_publication.publication_generation,
                        captured->predecessor_publication.durable_public_pointer_content_digest,
                        original.parent_authority, original.candidate_projection_revision,
                        candidate_proof->configurations, read_installed);
                  } else {
                    fence_owner.RunWithExactHeldPredecessor(query,
                        captured->predecessor_publication.publication_generation,
                        captured->predecessor_publication.durable_public_pointer_content_digest, read_installed);
                  }
                  Require(installed && readiness,
                          "delegated continuation did not retain its actual installed ports");
                  const auto continued = coordinator.ContinueApplyComponentBringUp(original.intent, digest,
                      installed->domains, installed->providers, installed->ports, {}, readiness);
                  if (!continued.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{continued.error()};
                  Response result{original, continued.value()};
                  Require(result.valid(), "delegated continuation result differs from its request");
                  return result;
                }
                // Original membership is consulted before current occurrence
                // authorization. Replayed evidence never enters an effect path.
                std::lock_guard lock(managed_composition_mutex);
                auto *const cclc = current_cell_cclc();
                Require(cclc != nullptr,
                        "original component result journal is unavailable");
                auto &coordinator = cclc->coordinator();
                const auto retained = [&] {
                  if constexpr (delegated) {
                    const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                        original.intent, original.candidate_projection_revision,
                        original.candidate_retained_bundle_digest,
                        original.candidate_retained_bundle_bytes,
                        original.runtime_unit_id, original.runtime_epoch,
                        original.manager_runtime_epoch, original.parent_authority.parent);
                    return coordinator.ReadApplyComponentBringUpRequestResult(original.intent, digest);
                  } else {
                    const auto digest = core::SelectedComponentBringUpSemanticRequestDigest(
                        original.intent, original.candidate_projection_revision,
                        original.candidate_retained_bundle_digest,
                        original.candidate_retained_bundle_bytes,
                        original.runtime_unit_id, original.runtime_epoch,
                        original.manager_runtime_epoch);
                    return coordinator.ReadSelectedComponentBringUpRequestResult(original.intent, digest);
                  }
                }();
                if (!retained.ok())
                  return component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                      retained.error()};
                if constexpr (!read_only) {
                  if (!retained.value())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                        core::ErrorDTO{"component_bring_up_live_authority_unavailable",
                                       "new bring-up requires the original live qualified continuation", {}}};
                }
                Response result{original, retained.value()};
                Require(result.valid(), "retained component result does not match its original request");
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
        const auto selected_result = [](const auto &result)
            -> component_manager::detail::KernelApplyFenceRpcResponse {
          if (!result.ok()) {
            return component_manager::detail::KernelApplyFenceRpcFailure{
                result.error()};
          }
          return result.value();
        };
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
                  Require(parent.ok() && !parent.value().bring_up_plan &&
                              !parent.value().terminal_outcome,
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
                        if (old.value().bring_up_plan ||
                            !old.value().fence_attempt ||
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
                return query_configuration_fence_status(value);
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpConfigurationFenceIntentDTO>) {
                return selected_result(
                    fence_owner.AcquireSelectedBringUpFence(value));
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpFenceAcquireAttemptQueryDTO>) {
                return selected_result(
                    fence_owner.ReconcileSelectedBringUpFenceAcquireAttempt(
                        value));
              } else if constexpr (
                  std::is_same_v<
                      T,
                      core::SelectedBringUpConfigurationFenceStatusQueryDTO>) {
                return selected_result(
                    fence_owner.ObserveSelectedBringUpFenceStatus(value));
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpConfigurationFenceReleaseDTO>) {
                return selected_result(
                    fence_owner.ReleaseSelectedBringUpFence(value));
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpFenceReleaseAttemptQueryDTO>) {
                return selected_result(
                    fence_owner.ReconcileSelectedBringUpFenceReleaseAttempt(
                        value));
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
                  component_manager::ReadManagerApplyBringUpStatusReadOnly(
                      database, cell, fresh->intent);
              if (!parent.ok())
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    parent.error());
              if (parent.value().bring_up_plan ||
                  !parent.value().terminal_outcome ||
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
                    component_manager::ReadManagerApplyBringUpStatusReadOnly(
                        database, cell, claim.intent);
                if (!previous.ok())
                  return core::Result<core::AuthoringAdmissionDTO>::Err(
                      previous.error());
                if (previous.value().bring_up_plan ||
                    previous.value().fence_attempt.has_value() !=
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
                const auto status = query_configuration_fence_status(query);
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
            const auto read = component_manager::ReadManagerApplyBringUpStatusReadOnly(
                config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
                core::CellId(config.cell_id), *acquire_intent);
            if (!read.ok()) {
              response = read.error();
            } else if (!read.value().bring_up_plan) {
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
                require_inactive_changed_components(
                    deployment::derive_apply_component_delta(*previous->bundle, *candidate->bundle),
                    predecessor, canonical_empty_apply(parent.intent.intent));
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
                            managed_composition_building ||
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
                              ReadManagerApplyBringUpStatusReadOnly(
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
                      },
                      // D6: only the exact canonical-empty candidate (its
                      // Manager-verified sealed origin) is admitted while Close
                      // of a latched Scenario runs.
                      [&] {
                        const auto &apply = read.value().intent.intent;
                        try {
                          return deployment::prepared_receipt_is_canonical_empty_candidate(
                              content_identity::parse_canonical_json(
                                  apply.prepared_activation.canonical_receipt_json),
                              core::CellId(config.cell_id),
                              apply.candidate_retained_bundle_id,
                              apply.candidate_graph_revision);
                        } catch (const std::exception &) {
                          return false;
                        }
                      }());
              response =
                  acquired.ok()
                      ? component_manager::detail::KernelCellAdmissionRpcResponse(
                            acquired.value())
                      : component_manager::detail::KernelCellAdmissionRpcResponse(
                            acquired.error());
            } else {
              const auto &parent = read.value();
              const auto &sealed = *parent.bring_up_plan;
              const auto &plan = sealed.plan;
              const auto predecessor = read_current_publication();
              Require(manager_liveness_installed.load(std::memory_order_acquire),
                      "Apply bring-up acquire has no current Manager observation");
              const auto manager = manager_liveness_receiver.get()->status();
              Require(
                          manager.state == core::ManagerLivenessStateDTO::kCurrent && !manager.command_fenced &&
                          manager.manager_owner_epoch == acquire_intent->owner_epoch &&
                          manager.receiver_runtime_unit_id == core::RuntimeUnitId(config.runtime_unit_id) &&
                          manager.receiver_runtime_epoch == core::RuntimeEpoch(config.runtime_epoch) &&
                          !parent.terminal_outcome && !parent.fence_attempt && !parent.fence_acceptance &&
                          !parent.private_materialization && !parent.publication_receipt &&
                          predecessor.request.candidate_projection_revision.retained_bundle_id ==
                              parent.intent.intent.expected_active_projection_revision.retained_bundle_id &&
                          predecessor.request.candidate_projection_revision.graph_revision ==
                              parent.intent.intent.expected_active_projection_revision.graph_revision &&
                          predecessor.request.candidate_projection_revision.scene_revision.scene_id ==
                              parent.intent.intent.expected_active_projection_revision.scene_revision.scene_id &&
                          predecessor.durable_public_pointer_content_digest == plan.predecessor_public_pointer_digest &&
                          predecessor.publication_generation == plan.expected_publication.expected_previous_publication_generation,
                      "Apply bring-up acquire lost its fresh original owner or predecessor");
              const auto candidate = LoadManagedBundleAtPinnedRoot(config, parent.intent.intent.candidate_retained_bundle_id);
              const auto previous = LoadManagedBundleAtPinnedRoot(config,
                  parent.intent.intent.expected_active_projection_revision.retained_bundle_id);
              Require(candidate && candidate->bundle && previous && previous->bundle && !plan.children.empty(),
                      "Apply bring-up acquire has no exact verified products");
              std::vector<core::ControllableComponentId> candidate_components;
              for (const auto &component : candidate->bundle->value().cell_graph.controllable_components)
                candidate_components.push_back(component.controllable_component_id);
              std::sort(candidate_components.begin(), candidate_components.end());
              Require(candidate_components == plan.configured_components,
                      "Apply bring-up acquire configured membership changed");
              const auto &first = plan.children.front();
              Require(predecessor.publication_generation.value() != std::numeric_limits<std::uint64_t>::max(),
                      "Apply bring-up publication generation is exhausted");
              const auto compiled = BuildManagedCandidateLifecyclePlanningClosure(config, *candidate,
                  first.candidate_projection_revision,
                  core::ActiveBundlePublicationGeneration(predecessor.publication_generation.value() + 1U));
              if (!compiled.ok()) Fail(compiled.error().message);
              deployment::CCLCRuntimeComposition *facade = nullptr;
              std::set<core::ControllableComponentId> installed_components;
              {
                const auto lock_start = DiagnosticClock::now();
                std::lock_guard composition_lock(managed_composition_mutex);
                timing.composition_wait += DiagnosticClock::now() - lock_start;
                Require(!current_composition_building && !managed_composition_building &&
                            !preparing_composition_request && !installed_under_fence_composition_request &&
                            managed_cell_cclc.facade && managed_cell_cclc.publication == predecessor,
                        "Apply bring-up acquire has no pinned original cell facade");
                facade = managed_cell_cclc.facade.get();
                Require(facade->coordinator().accepting_new_requests() &&
                            !facade->coordinator().admission_fault_closed(),
                        "Apply bring-up acquire original cell owner is closed");
                if (active_managed_composition)
                  for (const auto &[id, view] : IndexNativeComponentCompositions(*active_managed_composition)) {
                    static_cast<void>(view);
                    installed_components.insert(id);
                  }
              }
              core::ConfigurationFenceScopeDTO scope;
              std::map<core::ControlDomainId, core::ConfigurationFenceDomainScopeDTO> domains;
              std::map<core::ControlBindingId, core::ConfigurationFenceBindingScopeDTO> bindings;
              std::set<core::ResourceId> resources;
              std::vector<core::ApplyBringUpAdmissionComponentDTO> needed;
              for (const auto &child : plan.children) {
                const auto component = child.intent.controllable_component_id;
                const auto closure = std::find_if(compiled.value().components.begin(), compiled.value().components.end(),
                    [&](const auto &row) { return row.component_id == component; });
                const auto selection = facade->ReadApplyComponentGenerationSelection(component);
                Require(child.valid() && child.runtime_unit_id == manager.receiver_runtime_unit_id &&
                            child.runtime_epoch == manager.receiver_runtime_epoch && child.manager_runtime_epoch == manager.manager_runtime_epoch &&
                            child.intent.request_context.request_owner_epoch == facade->coordinator().request_owner_epoch() &&
                            child.candidate_projection_revision == first.candidate_projection_revision &&
                            child.candidate_projection_revision.retained_bundle_id == candidate->bundle->retained_bundle_id() &&
                            child.candidate_retained_bundle_bytes == candidate->bundle->canonical_bytes().size() &&
                            child.candidate_retained_bundle_digest == core::ContentDigest(content_identity::sha256_digest(candidate->bundle->canonical_bytes())) &&
                            closure != compiled.value().components.end() && child.intent.expected_control_domains == closure->candidate_expected_domains &&
                            selection.ok() && child.intent.apply_generation_selection == std::optional(selection.value()),
                        "Apply bring-up acquire changed its sealed owner selection or candidate closure");
                // Positive inventory absence, not a failed snapshot read, proves
                // this initial path. Retained/recovery offers need their genuine
                // containment join; they are never recast as initial here.
                Require(!installed_components.contains(component) &&
                            std::none_of(previous->bundle->value().cell_graph.controllable_components.begin(),
                                previous->bundle->value().cell_graph.controllable_components.end(),
                                [&](const auto &row) { return row.controllable_component_id == component; }) &&
                            !selection.value().owner_qualified_predecessor,
                        "Apply bring-up initial offer has a retained predecessor");
                core::ApplyBringUpAdmissionComponentDTO row{component,
                    core::ApplyBringUpAdmissionComponentKindDTO::kInitial, std::nullopt, {}, std::nullopt};
                scope.controllable_component_ids.push_back(component);
                for (const auto &binding : compiled.value().compiled_physical_bindings) {
                  const auto &value = binding.value();
                  if (value.controllable_component_id != component) continue;
                  row.candidate_participants.push_back(value);
                  const auto &path = *value.control_path;
                  domains.emplace(path.control_domain_id, core::ConfigurationFenceDomainScopeDTO{
                      path.control_domain_id, std::nullopt, path.control_domain_revision});
                  bindings.emplace(path.control_binding_id, core::ConfigurationFenceBindingScopeDTO{
                      path.control_binding_id, std::nullopt, path.control_binding_revision});
                  for (const auto &resource : value.resources) resources.insert(resource.resource_id);
                }
                Require(!row.candidate_participants.empty(), "Apply bring-up initial physical closure is absent");
                needed.push_back(std::move(row));
              }
              for (const auto &[id, row] : domains) { static_cast<void>(id); scope.domains.push_back(row); }
              for (const auto &[id, row] : bindings) { static_cast<void>(id); scope.bindings.push_back(row); }
              scope.resources.assign(resources.begin(), resources.end());
              const auto admission = fence_owner.ObserveAdmission({scope, manager.manager_runtime_epoch});
              Require(admission.matches({scope, manager.manager_runtime_epoch}) &&
                          admission.active_projection_revision == predecessor.request.candidate_projection_revision &&
                          admission.active_public_pointer_digest == predecessor.durable_public_pointer_content_digest &&
                          read_current_publication() == predecessor && manager_liveness_receiver.get()->status().manager_owner_epoch == manager.manager_owner_epoch,
                      "Apply bring-up acquire current observation changed");
              const core::ApplyBringUpAdmissionOfferDTO offer{{{plan.apply_id, plan.apply_generation}, sealed.plan_digest},
                  admission, plan.configured_components, std::move(needed)};
              const auto acquired = owner->AcquireApplyBringUpCellAdmissionClaim(*acquire_intent, offer);
              if (acquired.ok()) {
                Require(acquired.value().intent == *acquire_intent,
                        "Apply bring-up acquired claim differs from its original request");
                // Copy only actual original-owner facts after successful fresh
                // acquisition. Allocation uncertainty retains the durable claim;
                // it does not create a replacement/replay effect capability.
                original_apply_bring_up_capture =
                    std::make_shared<const ManagedOriginalApplyBringUpCapture>(
                        ManagedOriginalApplyBringUpCapture{
                            sealed, acquired.value(), predecessor,
                            parent.intent.intent.expected_active_projection_revision, *previous,
                            *candidate, compiled.value(), facade});
              }
              response = acquired.ok() ? component_manager::detail::KernelCellAdmissionRpcResponse(acquired.value())
                                       : component_manager::detail::KernelCellAdmissionRpcResponse(acquired.error());
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



class NativeExecutionHost final {
public:
  explicit NativeExecutionHost(HostConfig config)
      : config_(std::move(config)),
        owner_epoch_(RandomToken("execution-host-")),
        runtime_epoch_(RandomToken("native-runtime-")), admission_gate_(),
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
        fixture_clock_(std::make_shared<WallClock>()),
        fixture_scheduler_(std::make_shared<SimpleScheduler>(
            SimpleSchedulerConfig::from_environment())),
        composition_(BuildComposition(config_, owner_epoch_, runtime_epoch_,
                                      &admission_gate_, false, fixture_clock_,
                                      fixture_scheduler_, ros_context_provider_)) {
    struct FixtureStartupCleanup final {
      Composition& composition;
      std::unique_ptr<RetainedNativeCellRuntime>& cell;
      bool completed{};
      ~FixtureStartupCleanup() {
        if (!completed) {
          DetachRetainedCCLC(composition);
          cell.reset();
        }
      }
    } fixture_startup_cleanup{*composition_, fixture_cell_runtime_};
    // Use the actual component engines with the same stable cell authority as
    // the managed host. No component-local addressed Kernel/journal is made.
    auto registrations = NativeCellComponentRegistrations(*composition_);
    RetainedRuntimeParticipantResolverFactory staged_factory;
    core::ResourceAddressedExecutionKernelConfig cell_config{
        owner_epoch_, core::CellId(config_.cell_id), 1024U,
        NativeCellParticipantResolver(*composition_, registrations, &staged_factory),
        &admission_gate_, composition_->bundle->value().cell_graph.graph_revision};
    cell_config.io_rule_evidence_processing_notification =
        [](const std::shared_ptr<core::IIoSignalService>& affected) {
          if (auto runtime = std::dynamic_pointer_cast<HostIoSignalRuntime>(affected))
            runtime->RequestRuleEvidenceProcessing();
        };
    cell_config.cell_admission_claim_capacity = 1024U;
    const std::vector<core::RequiredManagerLivenessReceiverDTO> fixture_receivers;
    cell_config.manager_liveness_required_set = core::ManagerLivenessRequiredSetDTO{
        core::CellId(config_.cell_id), composition_->bundle->retained_bundle_id(),
        core::ComputeRequiredRuntimeSetDigest(fixture_receivers),
        core::RuntimeUnitId("standalone-admission-fixture.manager"),
        core::RuntimeEpoch("standalone-admission-fixture.manager-epoch"),
        core::RequestOwnerEpoch("standalone-admission-fixture.manager-owner"),
        fixture_receivers};
    // Preserve the standalone fixture's existing owner-evidence sources.
    cell_config.cell_admission_settlement_validator = BuildOwnerSettlementValidator(
        std::filesystem::path(std::string(component_manager::kManagerApplyJournalFilename)),
        {}, {}, config_.execution_state_root / std::string(core::kCCLCJournalFilename),
        core::CellId(config_.cell_id));
    cell_config.cell_admission_reconcile_validator = BuildOwnerReconciliationValidator(
        std::filesystem::path(std::string(component_manager::kManagerApplyJournalFilename)),
        {}, config_.execution_state_root / std::string(core::kCCLCJournalFilename),
        core::CellId(config_.cell_id));
    cell_config.cell_admission_program_reset_status_source =
        [](const auto purpose, const auto& intent, const auto& claim) {
          return detail::ReadProgramResetStatusReadOnly({}, purpose, intent, claim);
        };
    fixture_cell_runtime_ = std::make_unique<RetainedNativeCellRuntime>(
        RetainedNativeCellRuntimeInput{
            .clock = fixture_clock_, .scheduler = fixture_scheduler_,
            .operation_journal_path = config_.execution_state_root /
                                      std::string(core::kOperationJournalFilename),
            .participant_registrations = std::move(registrations),
            .kernel_config = std::move(cell_config),
            .stop_sources = NativeCellStopSources(*composition_),
            .stop_source_mode = RetainedStopSurveillanceEmptySourceMode::kRequireNonEmpty});
    AdoptNativeCellParticipantResolverFactory(*composition_, std::move(staged_factory));
    composition_->cell_runtime = fixture_cell_runtime_.get();
    auto& cell_kernel = fixture_cell_runtime_->addressed_kernel();
    std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> independent_grippers;
    const auto bind = [&](Composition& view) {
      view.cell_runtime = fixture_cell_runtime_.get();
      if (view.runtime) {
        view.runtime->stop_surveillance_owner = fixture_cell_runtime_->stop_surveillance_owner();
        view.runtime->BindCellRuntimeOwner(*fixture_cell_runtime_, runtime_epoch_.value(), config_.cell_id);
        view.runtime->cell_runtime_owner = fixture_cell_runtime_.get();
        Require(view.runtime->addressed_request_owner_epoch == cell_kernel.request_owner_epoch(),
                "standalone fixture runtime differs from the actual Kernel owner");
      }
      if (view.gripper_runtime) {
        view.gripper_runtime->BindCellKernel(cell_kernel);
        independent_grippers.push_back(view.gripper_runtime);
      }
    };
    bind(*composition_);
    for (const auto& peer : composition_->participant_compositions) {
      Require(peer != nullptr, "standalone fixture has a missing component owner");
      bind(*peer);
    }
    if (composition_->runtime)
      composition_->runtime->BindIndependentGripperPeers(
          composition_->runtime, std::move(independent_grippers));
    AttachProductPickPlacePlanningOwner(*composition_);
#else
        composition_(BuildComposition(config_, owner_epoch_, runtime_epoch_,
                                      &admission_gate_, true, {}, {}, ros_context_provider_)) {
#endif
    // Motion compositions bind this shared admission gate while constructing
    // their Kernel. The standalone gripper-only composition creates its
    // stable cell Kernel above, so bind its resource table explicitly before
    // the durable fence owner can observe or reconcile route publications.
    admission_gate_.BindResourceTable(
        composition_->active_addressed_kernel()->resource_admission_table());
    const auto runtime_units = composition_->host_runtime_unit_epochs();
    Require(runtime_units.size() == 1U,
            "standalone native host has no sole runtime-unit identity");
    fence_owner_ = std::make_unique<ManagedKernelApplyFenceOwner>(
        ManagedKernelApplyFenceOwnerConfig{
            config_.execution_state_root / "kernel-apply-fence.sqlite3",
            config_.manager_state_root,
            config_.package_root,
            core::CellId(config_.cell_id),
            runtime_units.front().runtime_unit_id,
            runtime_epoch_,
            owner_epoch_,
            [] {
              return std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                  .count();
            },
            {},
            {},
            [this](const core::ControlDomainId &domain) {
              return composition_->active_addressed_kernel()
                  ->current_runtime_provider_configuration(domain);
            }},
        admission_gate_);
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
    const auto fixture_readiness =
        composition_->active_addressed_kernel()
            ->UpdateCellAdmissionReadiness(
            core::CellAdmissionReadinessSnapshotDTO{
                true,
                core::ProgramCycleStatusDTO{
                    core::RequestOwnerEpoch(
                        "standalone-admission-fixture.program-runner"),
                    1U, core::ProgramCycleStateDTO::kIdleAfterReset,
                    std::nullopt, std::nullopt, std::nullopt},
                true, 0U, 0U, true, true, 0U, 0U, 0U});
    // Readiness publication is not permission to move. Retained claims and
    // faults must reach real reconciliation/ingress while admission stays
    // closed; do not require authoring eligibility before recovery can run.
    Require(fixture_readiness.ok(),
            "standalone admission fixture could not publish readiness");
#endif
    if (composition_->cell_runtime) {
      PrepareCompositionIoResources(*composition_, *composition_->cell_runtime,
                                    composition_->io_transport, ros_context_provider_);
      InstallCompositionIoSignals(*composition_, *composition_->cell_runtime);
    }
    {
      std::unique_lock lock(composition_access_.mutex);
      composition_access_.replace_current(composition_.get());
    }
    composition_access_.addressed_kernel =
        composition_->active_addressed_kernel();
    AttachRetainedNativeCCLC(
        *composition_, config_, *fence_owner_, std::nullopt, std::nullopt,
        [this] {
          return RestoreAppliedGlobalIoRuleCatalog(
              composition_access_, *composition_->active_addressed_kernel(),
              config_.manager_state_root, core::CellId(config_.cell_id));
        });
    ConfigureProgramResetPositionAccess(composition_access_, owner_epoch_,
        *composition_->cclc_runtime);
    {
      std::unique_lock lock(composition_access_.mutex);
      composition_access_.replace_current(composition_.get());
    }
    composition_access_.addressed_kernel =
        composition_->active_addressed_kernel();
    composition_access_.operation_journal = composition_->cell_runtime != nullptr
        ? &composition_->cell_runtime->operation_journal()
        : composition_->runtime == nullptr
            ? nullptr : composition_->runtime->active_operation_journal();
    // This legacy standalone owner has no Manager lease or liveness receiver.
    // Explicitly keep only its private admission seam open; managed children
    // use a separate, initially closed admission instance.
    standalone_command_admission_.OpenIfUnchanged(
        standalone_command_admission_.generation());
    command_surface_ = std::make_unique<ExecutionCommandSurface>(
        config_.execution_run_root, composition_access_,
        standalone_command_admission_, owner_epoch_);
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
    fixture_startup_cleanup.completed = true;
#endif
  }

  ~NativeExecutionHost() {
    command_surface_.reset();
    composition_access_.detach_addressed_owner();
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
    DetachRetainedCCLC(*composition_);
    // The generic I/O transport's executor fault reaches the cell
    // scheduler's sink: quiesce it before the cell removes that sink.
    if (composition_->io_transport) composition_->io_transport->quiesce();
    fixture_cell_runtime_.reset();
#endif
  }

  [[nodiscard]] int Run() {
    // Bind only after construction succeeds: constructor unwinding has no
    // command-owner destructor with which to drain scheduled Kernel calls.
    BindHostIoRuleEvidenceProcessor(*composition_, composition_access_owner_);
    if (composition_->io_signal_runtime)
      composition_->io_signal_runtime->RequestRuleEvidenceProcessing();
    return command_surface_->Run();
  }

private:
  HostConfig config_;
  core::RequestOwnerEpoch owner_epoch_;
  // The development launcher remains one process incarnation while a5.5
  // qualifies and then retires it in favor of the Manager-owned surface.
  core::RuntimeEpoch runtime_epoch_;
  ManagedKernelAdmissionGate admission_gate_;
  std::unique_ptr<ManagedKernelApplyFenceOwner> fence_owner_;
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
  std::shared_ptr<WallClock> fixture_clock_;
  std::shared_ptr<SimpleScheduler> fixture_scheduler_;
#endif
  const ros2_control_execution::Ros2TransportResources::ContextProvider
      ros_context_provider_ = MakeHostRosContextProvider();
  std::unique_ptr<Composition> composition_;
  // Retire the stable Kernel before the component engines it borrows.
  std::unique_ptr<RetainedNativeCellRuntime> fixture_cell_runtime_;
  std::shared_ptr<ExecutionCompositionAccess> composition_access_owner_ =
      std::make_shared<ExecutionCompositionAccess>();
  ExecutionCompositionAccess &composition_access_ = *composition_access_owner_;
  component_manager::detail::ManagerLivenessCommandAdmission
      standalone_command_admission_;
  std::unique_ptr<ExecutionCommandSurface> command_surface_;
};

} // namespace
} // namespace manyforge::roboplan_backend

namespace manyforge::roboplan_backend::host_detail {

int RunExecutionHostMain(int argc, char **argv) {
  try {
    manyforge::roboplan_backend::InstallSignalHandlers();
    if (argc > 1 &&
        std::string_view(argv[1]) == "--manyforge-managed-runtime-channel-fd") {
      return manyforge::roboplan_backend::RunManagedRuntimeChild(
          manyforge::roboplan_backend::ParseManagedRuntimeArguments(argc,
                                                                    argv));
    }
    manyforge::roboplan_backend::NativeExecutionHost host(
        manyforge::roboplan_backend::ParseArguments(argc, argv));
    return host.Run();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Native execution host failed with an unknown error\n";
    return 71;
  }
}

} // namespace manyforge::roboplan_backend::host_detail
