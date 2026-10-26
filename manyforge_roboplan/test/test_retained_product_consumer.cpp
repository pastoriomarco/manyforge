#include "sealed_program_execution_capture.hpp"
#include <manyforge_core/types/program_io_condition.hpp>
#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/testing/fake_execution_port.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
#include "host_io_provider_installation.hpp"
#include "host_io_signal_runtime.hpp"
#include "host_io_output_channels.hpp"
#include "io_provider_lifecycle_product.hpp"
#include "maintained_activation_fixture.hpp"
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_deployment/component_lifecycle_current_snapshot_source.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include "../src/retained_ros2_control_lifecycle_plans_internal.hpp"
#endif

#include "content_identity_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "io_retained_fixture.hpp"
#include "managed_kernel_apply_fence_owner.hpp"
#include "mf2_joint_name_internal.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include "retained_bundle_internal.hpp"
#include "retained_native_runtime_internal.hpp"

namespace manyforge::roboplan_backend {
namespace {

using collision::ManyForgeCollisionEngine;
constexpr std::string_view kCanonicalCellFrame =
    "mf1_cell_e228ede37638a8d03080644d418a75c681930a4b5b3f6b0bbde2603974dab681";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[nodiscard]] std::vector<RoboPlanJointLimitInput>
retained_joint_limit_rows(std::string_view urdf,
                          std::optional<std::string_view> srdf) {
  const auto documents = deployment::parse_model_documents_for_projection(
      urdf, srdf);
  std::vector<RoboPlanJointLimitInput> result;
  for (const auto& joint : documents.urdf.joints) {
    if (joint.type == deployment::UrdfJointType::kFixed ||
        joint.type == deployment::UrdfJointType::kFloating ||
        joint.type == deployment::UrdfJointType::kPlanar ||
        joint.mimic.has_value()) {
      continue;
    }
    if (!joint.limit.has_value() || !std::isfinite(joint.limit->velocity) ||
        joint.limit->velocity <= 0.0) {
      throw std::runtime_error("test retained joint has no velocity limit");
    }
    RoboPlanJointLimitInput row;
    row.joint_name = joint.name;
    row.maximum_velocity = {joint.limit->velocity};
    if (joint.type == deployment::UrdfJointType::kRevolute ||
        joint.type == deployment::UrdfJointType::kPrismatic) {
      if (!joint.limit->authored_lower.has_value() ||
          !joint.limit->authored_upper.has_value()) {
        throw std::runtime_error("test retained joint has incomplete bounds");
      }
      row.minimum_position = std::vector<double>{joint.limit->lower};
      row.maximum_position = std::vector<double>{joint.limit->upper};
    }
    if (joint.limit->acceleration.has_value()) {
      row.maximum_acceleration =
          std::vector<double>{*joint.limit->acceleration};
    }
    result.push_back(std::move(row));
  }
  std::sort(result.begin(), result.end(), [](const auto& left,
                                             const auto& right) {
    return left.joint_name < right.joint_name;
  });
  return result;
}

[[nodiscard]] std::vector<RoboPlanJointLimitInput>
retained_joint_limit_rows(
    const deployment::VerifiedRetainedPlanningModel& retained) {
  return retained_joint_limit_rows(
      retained.urdf_bytes(),
      retained.srdf_bytes().empty()
          ? std::optional<std::string_view>{}
          : std::optional<std::string_view>{retained.srdf_bytes()});
}

static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProductArtifact>);
static_assert(
    !std::is_aggregate_v<deployment::VerifiedRetainedProductArtifact>);
static_assert(
    !std::is_default_constructible_v<deployment::VerifiedRetainedModelMap>);
static_assert(!std::is_aggregate_v<deployment::VerifiedRetainedModelMap>);
static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProjectionInput>);
static_assert(
    !std::is_aggregate_v<deployment::VerifiedRetainedProjectionInput>);

struct SubscriptionObservationSnapshot final {
  std::size_t active_subscriptions{};
  std::size_t subscriptions{};
  std::size_t unsubscriptions{};
  std::size_t observer_destructions{};
  std::size_t destructions_with_active_subscription{};
  std::size_t peer_back_pointer_observations{};
  std::size_t uncleared_peer_back_pointers{};
};

/// External observation state for joined-engine teardown. It owns neither an
/// observer nor an underlying engine, so keeping this ledger after root reset
/// cannot extend any production lifetime under test.
class SubscriptionObservationLedger final {
public:
  void Subscribed(const std::string &observer,
                  const std::uint64_t handle) {
    std::lock_guard lock(mutex_);
    if (!active_.emplace(observer, handle).second) {
      throw std::logic_error(
          "real participant returned a duplicate subscription handle");
    }
    ++subscriptions_;
  }

  void Unsubscribed(const std::string &observer,
                    const std::uint64_t handle) {
    std::lock_guard lock(mutex_);
    if (active_.erase({observer, handle}) == 1U) {
      ++unsubscriptions_;
    }
  }

  void ObserverDestroyed(const std::string &observer,
                         const bool observes_peer,
                         const bool peer_back_pointers_cleared) {
    std::lock_guard lock(mutex_);
    ++observer_destructions_;
    if (std::any_of(active_.begin(), active_.end(), [&](const auto &entry) {
          return entry.first == observer;
        })) {
      ++destructions_with_active_subscription_;
    }
    if (observes_peer) {
      ++peer_back_pointer_observations_;
      if (!peer_back_pointers_cleared) {
        ++uncleared_peer_back_pointers_;
      }
    }
  }

  [[nodiscard]] SubscriptionObservationSnapshot Snapshot() const {
    std::lock_guard lock(mutex_);
    return {active_.size(),
            subscriptions_,
            unsubscriptions_,
            observer_destructions_,
            destructions_with_active_subscription_,
            peer_back_pointer_observations_,
            uncleared_peer_back_pointers_};
  }

private:
  mutable std::mutex mutex_;
  std::set<std::pair<std::string, std::uint64_t>> active_;
  std::size_t subscriptions_{};
  std::size_t unsubscriptions_{};
  std::size_t observer_destructions_{};
  std::size_t destructions_with_active_subscription_{};
  std::size_t peer_back_pointer_observations_{};
  std::size_t uncleared_peer_back_pointers_{};
};

/// Runtime-owned observer that delegates to and owns one real participant
/// engine. Only the non-owning addressed registration points at this wrapper;
/// an aliasing participant_engine shared_ptr below makes the runtime itself the
/// wrapper owner without changing the production runtime shape.
class SubscriptionObservedParticipantEngine final
    : public core::IResolvedParticipantExecutionEngine {
public:
  SubscriptionObservedParticipantEngine(
      std::shared_ptr<core::IResolvedParticipantExecutionEngine> engine,
      std::shared_ptr<SubscriptionObservationLedger> ledger,
      std::string observer,
      std::function<bool()> peer_back_pointers_cleared = {})
      : engine_(std::move(engine)), ledger_(std::move(ledger)),
        observer_(std::move(observer)),
        peer_back_pointers_cleared_(
            std::move(peer_back_pointers_cleared)) {
    if (engine_ == nullptr || ledger_ == nullptr || observer_.empty()) {
      throw std::invalid_argument("observed participant engine is absent");
    }
  }

  ~SubscriptionObservedParticipantEngine() override {
    ledger_->ObserverDestroyed(
        observer_, static_cast<bool>(peer_back_pointers_cleared_),
        !peer_back_pointers_cleared_ || peer_back_pointers_cleared_());
  }

  core::AddressedParticipantStopFenceDisposition
  fence_addressed_participant_stop(
      const std::uint64_t execution_id) noexcept override {
    return engine_->fence_addressed_participant_stop(execution_id);
  }
  core::Result<core::AddressedParticipantStopSettlementDTO>
  settle_addressed_participant_stop(
      const core::AddressedParticipantStopSettlementRequestDTO &request)
      override {
    return Require()->settle_addressed_participant_stop(request);
  }
  core::ResetResultDTO reset_after_certified_containment(
      const std::uint64_t execution_id,
      std::optional<std::uint64_t> expected_epoch = std::nullopt) override {
    return Require()->reset_after_certified_containment(execution_id,
                                                        expected_epoch);
  }
  core::AcknowledgeCompletedResultDTO acknowledge_completed(
      const std::uint64_t execution_id,
      std::optional<std::uint64_t> expected_epoch = std::nullopt) override {
    return Require()->acknowledge_completed(execution_id, expected_epoch);
  }
  core::Result<core::ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const core::ResolvedRuntimeScaleCommandDTO &command) override {
    return Require()->set_runtime_scale(command);
  }
  core::Result<core::ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const core::OperationId &operation_id,
      const core::ControlBindingId &control_binding_id) const override {
    return Require()->runtime_scale_status(operation_id, control_binding_id);
  }
  core::GripperCommandResultDTO command_gripper(
      const core::GripperCommandRequestDTO &request) override {
    return Require()->command_gripper(request);
  }
  core::Result<void> validate_gripper_command(
      const core::GripperCommandRequestDTO &request) const override {
    return Require()->validate_gripper_command(request);
  }
  core::GripperCommandStatusDTO
  get_gripper_status(const std::string &gripper_id) const override {
    return Require()->get_gripper_status(gripper_id);
  }
  core::Result<std::uint64_t>
  stop_gripper(const std::string &gripper_id,
               const std::uint64_t expected_command_id,
               const std::uint64_t expected_generation) override {
    return Require()->stop_gripper(gripper_id, expected_command_id,
                                   expected_generation);
  }
  core::Result<void>
  reset_gripper(const std::string &gripper_id,
                const std::uint64_t expected_command_id,
                const std::uint64_t expected_generation) override {
    return Require()->reset_gripper(gripper_id, expected_command_id,
                                    expected_generation);
  }
  core::ExecutionStatus get_status() const override {
    return Require()->get_status();
  }
  core::KernelRecoveryAssessmentDTO
  get_recovery_assessment() const override {
    return Require()->get_recovery_assessment();
  }
  core::KernelRecoveryActionResultDTO apply_recovery_action(
      const core::KernelRecoveryActionRequestDTO &request) override {
    return Require()->apply_recovery_action(request);
  }

  core::SubscriptionHandle
  subscribe_events(core::EventCallback callback) override {
    const auto engine = Require();
    const auto handle = engine->subscribe_events(std::move(callback));
    try {
      ledger_->Subscribed(observer_, handle.id);
    } catch (...) {
      engine->unsubscribe_events(handle);
      throw;
    }
    return handle;
  }
  void unsubscribe_events(const core::SubscriptionHandle handle) override {
    Require()->unsubscribe_events(handle);
    ledger_->Unsubscribed(observer_, handle.id);
  }

private:
  [[nodiscard]] std::shared_ptr<core::IResolvedParticipantExecutionEngine>
  Require() const {
    return engine_;
  }

  std::shared_ptr<core::IResolvedParticipantExecutionEngine> engine_;
  std::shared_ptr<SubscriptionObservationLedger> ledger_;
  std::string observer_;
  std::function<bool()> peer_back_pointers_cleared_;
};

[[nodiscard]] core::LogicalSceneSnapshotRoot
make_projection_runtime_shadow_snapshot(const core::CellId &cell_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  auto snapshot = std::make_shared<core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          core::SceneRevision{
              core::logical_scene_identity::MintSceneId({
                  cell_id,
                  core::BootstrapId("i3_projection_runtime_shadow"),
                  core::BootstrapGeneration(1U),
              }),
              1U, core::ContentDigest(std::string(kZeroDigest))},
          cell_id,
          0U,
          0U,
          0U,
          std::move(policy),
          {},
          {},
      });
  snapshot->revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(*snapshot);
  if (!snapshot->locally_valid()) {
    throw std::logic_error(
        "test-only projection runtime-shadow snapshot is invalid");
  }
  return core::LogicalSceneSnapshotRoot(std::move(snapshot));
}

[[nodiscard]] core::LogicalSceneSnapshotDTO make_collision_scene_snapshot(
    const core::CellId &cell_id, const std::uint64_t scene_revision_id,
    const core::SceneResourceId &scene_resource_id, const double x_m,
    const double extent_m,
    std::optional<core::SceneObjectProviderEvidenceDTO> provider_evidence =
        std::nullopt) {
  auto base = make_projection_runtime_shadow_snapshot(cell_id);
  core::LogicalSceneSnapshotDTO snapshot = *base.shared();
  snapshot.revision.revision_id = scene_revision_id;
  snapshot.transaction_sequence = scene_revision_id - 1U;
  snapshot.topology_epoch = scene_revision_id - 1U;
  snapshot.objects.push_back(core::SceneObjectSnapshotDTO{
      scene_resource_id,
      {core::BoxShapeDTO{{extent_m, extent_m, extent_m}}},
      core::CellFixedSceneObjectPlacementDTO{
          cell_id,
          {{x_m, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
      },
      std::move(provider_evidence),
  });
  snapshot.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
  if (!snapshot.locally_valid()) {
    throw std::logic_error("test collision logical-scene snapshot is invalid");
  }
  return snapshot;
}

class ScopedTemporaryRoot final {
public:
  ScopedTemporaryRoot() {
    std::filesystem::path pattern = std::filesystem::temp_directory_path() /
                                    "manyforge-lite6-retained-consumer-XXXXXX";
    std::string owned_pattern = pattern.string();
    char *created = ::mkdtemp(owned_pattern.data());
    if (created == nullptr) {
      throw std::runtime_error(
          "cannot create retained Lite6 consumer test root");
    }
    path_ = created;
  }

  ~ScopedTemporaryRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedTemporaryRoot(const ScopedTemporaryRoot &) = delete;
  ScopedTemporaryRoot &operator=(const ScopedTemporaryRoot &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class ScopedCurrentDirectory final {
public:
  explicit ScopedCurrentDirectory(const std::filesystem::path &replacement)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(replacement);
  }

  ~ScopedCurrentDirectory() {
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
  }

  ScopedCurrentDirectory(const ScopedCurrentDirectory &) = delete;
  ScopedCurrentDirectory &operator=(const ScopedCurrentDirectory &) = delete;

private:
  std::filesystem::path previous_;
};

[[nodiscard]] deployment::VerifiedRetainedPlanningModel
load_lite6_planning_model(const std::filesystem::path &package_root,
                          const deployment::RetainedPlanningModelQuery &query) {
  const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
      package_root);
  auto model = source.get_verified(query);
  if (!model.has_value()) {
    throw std::logic_error(
        "durable Lite6 planning model was absent on exact point read");
  }
  return std::move(*model);
}

[[nodiscard]] deployment::VerifiedRetainedModelMap
load_lite6_model_map(const std::filesystem::path &package_root,
                     const deployment::RetainedModelMapQuery &query) {
  const deployment::FilesystemVerifiedRetainedModelMapSource source(
      package_root);
  auto model_map = source.get_verified(query);
  if (!model_map.has_value()) {
    throw std::logic_error(
        "durable Lite6 model map was absent on exact point read");
  }
  return std::move(*model_map);
}

template <typename Range, typename Predicate>
[[nodiscard]] const typename Range::value_type &
require_fixture_one(const Range &values, Predicate predicate,
                    std::string_view description) {
  const auto found = std::find_if(values.begin(), values.end(), predicate);
  if (found == values.end() ||
      std::find_if(std::next(found), values.end(), predicate) != values.end()) {
    throw std::logic_error("retained fixture has no sole " +
                           std::string(description));
  }
  return *found;
}

[[nodiscard]] core::RuntimeParticipantBinding make_fixture_runtime_participant(
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const core::ControllableComponentId &component_id,
    const core::CommandTargetId &target_id,
    std::optional<core::StateAuthorityClass> state_authority_override =
        std::nullopt,
    std::optional<core::ProgramCommandSchemaRefDTO> command_schema =
        std::nullopt,
    std::optional<core::RuntimeEpoch> provider_runtime_epoch = std::nullopt,
    std::optional<core::CapabilityId> selected_capability = std::nullopt) {
  const auto &graph = bundle.value().cell_graph;
  const auto &component = require_fixture_one(
      graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id;
      },
      "controllable component");
  const auto &control_binding = require_fixture_one(
      graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id &&
               candidate.command_target_id == target_id &&
               (!selected_capability ||
                candidate.capability_id == *selected_capability);
      },
      "component target control binding");
  const auto &control_domain = require_fixture_one(
      graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id == control_binding.control_domain_id;
      },
      "control domain");
  const auto &desired = require_fixture_one(
      bundle.value().desired_control_configurations,
      [&](const auto &candidate) {
        return candidate.control_domain_id ==
                   control_domain.control_domain_id &&
               candidate.control_domain_revision ==
                   control_domain.control_domain_revision;
      },
      "desired control configuration");
  const auto &desired_route = require_fixture_one(
      desired.binding_routes,
      [&](const auto &candidate) {
        return candidate.control_binding_id ==
                   control_binding.control_binding_id &&
               candidate.control_binding_revision ==
                   control_binding.control_binding_revision;
      },
      "desired control route");
  const auto *target = model_map.find_target(component_id, target_id);
  const auto *coordinate_map = model_map.find_binding(
      component_id, target_id, control_binding.control_binding_id);
  if (target == nullptr || coordinate_map == nullptr) {
    throw std::logic_error(
        "retained fixture target has no exact model coordinate map");
  }

  const auto model_instance =
      [&](const core::ModelInstanceId &id) -> const core::ModelInstanceDTO & {
    return require_fixture_one(
        graph.model_instances,
        [&](const auto &candidate) {
          return candidate.model_instance_id == id;
        },
        "model instance");
  };

  std::vector<core::ProgramModelRoleBindingDTO> model_bindings;
  for (const auto &binding : component.model_role_bindings) {
    const auto &model = model_instance(binding.model_instance_id);
    model_bindings.push_back({binding.model_role_id, binding.model_instance_id,
                              model.model_revision});
  }

  std::vector<core::RuntimeModelGroupBindingDTO> model_groups;
  for (const auto &group : target->model_groups) {
    model_groups.push_back({group.model_group_role_id, group.model_instance_id,
                            group.compiled_group_name,
                            group.expanded_compiled_joint_names});
  }

  std::vector<core::ProgramFrameRoleBindingDTO> frame_bindings;
  for (const auto &resolution : target->frames) {
    std::visit(
        [&](const auto &frame) {
          using Frame = std::decay_t<decltype(frame)>;
          if constexpr (std::is_same_v<
                            Frame,
                            deployment::CompositionComponentModelFrame>) {
            // A target role may alias a component frame role in the qualified map.
            if (frame.controllable_component_id != component_id) {
              throw std::logic_error(
                  "retained fixture target frame ownership differs");
            }
            frame_bindings.push_back(
                {resolution.frame_role_id, frame.model_instance_id,
                 frame.model_revision, frame.compiled_frame_name});
          }
        },
        resolution.frame);
  }

  std::vector<core::RuntimeResourceBindingDTO> resources;
  for (const auto &binding : control_binding.resource_bindings) {
    const auto &row = require_fixture_one(
        coordinate_map->resources,
        [&](const auto &candidate) {
          return candidate.resource_id == binding.resource_id;
        },
        "resource coordinate row");
    if (row.resource_role_id != binding.resource_role_id ||
        row.coordinates.empty() || row.coordinates.front().derived_mimic) {
      throw std::logic_error(
          "retained fixture direct resource coordinate differs");
    }
    const auto &coordinate = row.coordinates.front();
    std::optional<core::CoordinateKind> kind;
    if (coordinate.coordinate_kind ==
            deployment::UrdfCoordinateKind::kAngular &&
        coordinate.unit == deployment::CompositionCoordinateUnit::kRadians) {
      kind = core::CoordinateKind::kAngular;
    } else if (coordinate.coordinate_kind ==
                   deployment::UrdfCoordinateKind::kLinear &&
               coordinate.unit ==
                   deployment::CompositionCoordinateUnit::kMeters) {
      kind = core::CoordinateKind::kLinear;
    }
    if (!kind.has_value()) {
      throw std::logic_error(
          "retained fixture resource has no supported canonical unit");
    }
    resources.push_back({binding.resource_role_id, binding.resource_id,
                         coordinate.source_joint_name,
                         coordinate.compiled_joint_name, *kind});
  }

  std::vector<core::ProgramCollisionBodyRoleBindingDTO> collision_bodies;
  for (const auto &body : target->collision_bodies) {
    collision_bodies.push_back(
        {body.collision_body_role_id, body.model_instance_id,
         model_instance(body.model_instance_id).model_revision,
         body.compiled_link_names});
  }

  std::set<core::RuntimeUnitId> runtime_unit_ids(
      component.runtime_unit_ids.begin(), component.runtime_unit_ids.end());
  runtime_unit_ids.insert(control_domain.runtime_unit_ids.begin(),
                          control_domain.runtime_unit_ids.end());
  runtime_unit_ids.insert(control_binding.provider_runtime_unit_id);

  std::optional<core::RuntimeScaleRouteQualificationDTO>
      runtime_scale_qualification;
  const auto qualification = std::find_if(
      control_binding.runtime_scale_route_qualifications.begin(),
      control_binding.runtime_scale_route_qualifications.end(),
      [&](const auto &candidate) {
        return candidate.control_route_id == desired_route.control_route_id;
      });
  if (qualification !=
      control_binding.runtime_scale_route_qualifications.end()) {
    runtime_scale_qualification = *qualification;
  }

  std::vector<core::RuntimeCapabilityProviderPathDTO> supporting_providers;
  if (control_binding.capability_id ==
      core::CapabilityId("execution.trajectory.execute")) {
    const auto &planner = require_fixture_one(
        bundle.value().capability_provider_bindings,
        [&](const auto &candidate) {
          const auto *target =
              std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
                  &candidate.target);
          return candidate.capability_id ==
                     core::CapabilityId("planning.motion.joint_goal") &&
                 target != nullptr &&
                 target->controllable_component_id == component_id &&
                 target->command_target_id == target_id;
        },
        "joint-planner capability provider");
    std::set<core::RuntimeUnitId> planner_runtime_units;
    for (const auto &endpoint : planner.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      if (runtime_unit == nullptr) {
        throw std::logic_error("planner endpoint source kind and value differ");
      }
      planner_runtime_units.insert(*runtime_unit);
    }
    if (planner_runtime_units.size() != 1U) {
      throw std::logic_error(
          "planner capability provider has no exact runtime-unit owner");
    }
    const auto planner_runtime_unit = *planner_runtime_units.begin();
    runtime_unit_ids.insert(planner_runtime_unit);
    supporting_providers.push_back(
        {planner.capability_id, planner.capability_definition_revision,
         planner.capability_provider_binding_id,
         planner.capability_provider_binding_revision,
         planner.provider_instance_id, planner.provider_manifest_id,
         planner.provider_manifest_revision, planner_runtime_unit,
         core::RuntimeEpoch("epoch." + planner_runtime_unit.value())});
  }

  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  for (const auto &runtime_unit_id : runtime_unit_ids) {
    runtime_epochs.push_back(
        {runtime_unit_id,
         core::RuntimeEpoch("epoch." + runtime_unit_id.value())});
  }

  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("i6.aggregate.state.fixture"),
      core::ProgramRevision(std::string(kZeroDigest)),
      core::ProgramBindingId("i6.aggregate.state.binding"),
      bundle.retained_bundle_id(),
      graph.graph_revision,
      core::ComponentRoleId("robot"),
      core::TargetRoleId(target_id.value()),
      component.controllable_component_id,
      component.component_type_id,
      component.component_type_revision,
      target_id,
      control_binding.capability_id,
      control_binding.capability_definition_revision,
      std::move(command_schema),
      std::move(model_bindings),
      std::move(model_groups),
      std::move(frame_bindings),
      std::move(resources),
      std::move(collision_bodies),
      core::RuntimeControlPathDTO{
          control_domain.control_domain_id,
          control_domain.control_domain_revision,
          control_binding.control_binding_id,
          control_binding.control_binding_revision,
          control_binding.control_interface_contract_id,
          control_binding.control_interface_revision,
          control_binding.controller_topology_id,
          control_binding.topology_revision,
          desired_route.control_route_id,
          desired.activation_set_id,
          core::ActiveControlConfigurationId("active.native"),
          core::ActiveControlConfigurationGeneration(1U),
          control_binding.provider_instance_id,
          control_binding.provider_manifest_id,
          control_binding.provider_manifest_revision,
          control_binding.provider_runtime_unit_id,
          provider_runtime_epoch.value_or(core::RuntimeEpoch(
              "epoch." + control_binding.provider_runtime_unit_id.value())),
          control_binding.state_authority_id,
          state_authority_override.value_or(
              control_binding.state_authority_class),
          control_binding.stop_evidence_policy_id,
          control_binding.stop_evidence_policy_revision,
          control_binding.containment_granularity,
          std::move(runtime_scale_qualification),
      },
      std::nullopt,
      std::move(supporting_providers),
      std::move(runtime_epochs),
  };
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error("retained fixture runtime participant failed: " +
                           sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::RuntimeParticipantBinding with_program_binding_identity(
    const core::RuntimeParticipantBinding &physical,
    const deployment::ProgramBinding &program_binding) {
  const auto &binding = program_binding.value();
  if (binding.component_role_bindings.size() != 1U ||
      binding.target_role_bindings.size() != 1U ||
      binding.component_role_bindings.front().controllable_component_id !=
          physical.value().controllable_component_id ||
      binding.target_role_bindings.front().command_target_id !=
          physical.value().command_target_id) {
    throw std::logic_error(
        "direct-control Program Binding crosses its physical participant");
  }
  auto value = physical.value();
  value.program_id = binding.program_id;
  value.program_revision = binding.program_revision;
  value.program_binding_id = program_binding.program_binding_id();
  value.component_role_id =
      binding.component_role_bindings.front().component_role_id;
  value.target_role_id = binding.target_role_bindings.front().target_role_id;
  value.frame_bindings =
      binding.target_role_bindings.front().frame_role_bindings;
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(
        "direct-control physical participant identity failed: " +
        sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::ResolvedExecutionBindingDTO make_fixture_execution_binding(
    const core::RuntimeParticipantBinding &runtime,
    const core::FrameRoleId &base_role = core::FrameRoleId("base"),
    const core::FrameRoleId &tcp_role = core::FrameRoleId("tcp")) {
  const auto &value = runtime.value();
  if (!value.control_path.has_value() || value.model_groups.size() != 1U ||
      value.supporting_capability_provider_paths.size() != 1U) {
    throw std::logic_error(
        "fixture runtime participant has no sole motion/control path");
  }
  const auto &control = *value.control_path;
  const auto &planner = value.supporting_capability_provider_paths.front();
  const auto &group = value.model_groups.front();
  const auto base = require_fixture_one(
      value.frame_bindings,
      [&](const auto &candidate) {
        return candidate.frame_role_id == base_role;
      },
      "base frame binding");
  const auto tcp = require_fixture_one(
      value.frame_bindings,
      [&](const auto &candidate) {
        return candidate.frame_role_id == tcp_role;
      },
      "tcp frame binding");
  const auto model_revision = [&](const core::ModelInstanceId &model_id) {
    return require_fixture_one(
               value.model_bindings,
               [&](const auto &candidate) {
                 return candidate.model_instance_id == model_id;
               },
               "execution model binding")
        .model_revision;
  };
  std::vector<core::PlannerModelBindingDTO> models;
  for (const auto &model : value.model_bindings) {
    models.push_back({model.model_instance_id, model.model_revision});
  }
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  std::vector<core::PlannerRuntimeUnitBindingDTO> runtime_units;
  for (const auto &runtime_unit : value.runtime_unit_epochs) {
    runtime_units.push_back(
        {runtime_unit.runtime_unit_id, runtime_unit.runtime_epoch});
  }
  return core::ResolvedExecutionBindingDTO{
      core::OperationId("operation:reference-resolution"),
      core::OperationParticipantId("participant:reference-resolution"),
      core::ControllerGoalId("goal:reference-resolution"),
      core::ControllerGoalGeneration(1U),
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      core::ProgramRunId("run:reference-resolution"),
      value.controllable_component_id,
      value.component_type_id,
      value.component_type_revision,
      value.command_target_id,
      value.capability_id,
      value.capability_definition_revision,
      core::PlannerCapabilityBindingDTO{
          planner.capability_id, planner.capability_definition_revision,
          planner.capability_provider_binding_id,
          planner.capability_provider_binding_revision,
          planner.provider_instance_id, planner.provider_manifest_id,
          planner.provider_manifest_revision, planner.provider_runtime_unit_id,
          planner.provider_runtime_epoch},
      std::move(models),
      core::PlannerModelGroupBindingDTO{
          group.model_group_role_id, group.model_instance_id,
          model_revision(group.model_instance_id), group.compiled_group_name,
          std::move(compiled_joint_names)},
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      control.control_interface_contract_id,
      control.control_interface_revision,
      control.controller_topology_id,
      control.topology_revision,
      control.control_route_id,
      control.activation_set_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation,
      control.provider_instance_id,
      control.provider_manifest_id,
      control.provider_manifest_revision,
      control.provider_runtime_epoch,
      std::move(runtime_units),
      std::move(resources),
      core::ResolvedComponentModelFrameDTO{
          value.controllable_component_id, base.model_instance_id,
          base.frame_role_id, base.compiled_frame_name, base.model_revision},
      core::ResolvedComponentModelFrameDTO{
          value.controllable_component_id, tcp.model_instance_id,
          tcp.frame_role_id, tcp.compiled_frame_name, tcp.model_revision},
      {},
      {},
      control.runtime_scale_qualification,
      value.retained_bundle_id,
      value.graph_revision,
      value.target_role_id,
      value.frame_bindings,
      value.collision_body_bindings};
}

[[nodiscard]] core::ObservedStateSnapshotDTO
make_fixture_state(const core::ObservedStateQueryContextDTO &context,
                   const std::vector<double> &positions_si,
                   std::uint64_t sequence, std::int64_t timestamp_ns) {
  if (positions_si.size() != context.coordinate_kinds.size()) {
    throw std::logic_error("fixture state dimension differs from context");
  }
  std::vector<core::CoordinatePositionDTO> positions;
  positions.reserve(positions_si.size());
  for (std::size_t index = 0U; index < positions_si.size(); ++index) {
    if (context.coordinate_kinds[index] == core::CoordinateKind::kAngular) {
      positions.emplace_back(core::AngularPositionDTO{positions_si[index]});
    } else {
      positions.emplace_back(core::LinearPositionDTO{positions_si[index]});
    }
  }
  core::ObservedStateSourceKind source_kind =
      core::ObservedStateSourceKind::kJointState;
  if (context.state_authority_class == core::StateAuthorityClass::kCommanded) {
    source_kind = core::ObservedStateSourceKind::kCommandedState;
  } else if (context.state_authority_class ==
             core::StateAuthorityClass::kPredicted) {
    source_kind = core::ObservedStateSourceKind::kPredictedState;
  }
  return {
      context.controllable_component_id,
      context.command_target_id,
      context.control_domain_id,
      context.control_domain_revision,
      context.control_binding_id,
      context.control_binding_revision,
      context.provider_instance_id,
      context.provider_manifest_id,
      context.provider_manifest_revision,
      context.controller_topology_id,
      context.topology_revision,
      context.control_route_id,
      context.active_control_configuration_id,
      context.active_control_configuration_generation,
      context.resources,
      sequence,
      context.joint_names,
      std::move(positions),
      std::nullopt,
      "fixture.state." + context.command_target_id.value(),
      context.runtime_epoch,
      source_kind,
      timestamp_ns,
      timestamp_ns,
      context.state_authority_id,
      context.state_authority_class,
      1U,
      context.freshness_budget_ns,
      true,
  };
}

[[nodiscard]] RoboPlanCandidateBindingDTO
make_candidate_binding(const core::RuntimeParticipantBinding &runtime) {
  const auto &value = runtime.value();
  if (value.model_groups.size() != 1U) {
    throw std::logic_error("fixture motion target has no sole model group");
  }
  const auto &group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &candidate) {
        return candidate.model_instance_id == group.model_instance_id;
      });
  if (model == value.model_bindings.end()) {
    throw std::logic_error("fixture motion group has no model revision");
  }
  std::vector<core::ResourceId> resources;
  std::vector<std::string> joint_names;
  resources.reserve(value.resources.size());
  joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    joint_names.push_back(resource.compiled_resource_name);
  }
  std::vector<core::ResolvedFrameDTO> frames;
  frames.reserve(value.frame_bindings.size());
  for (const auto &frame : value.frame_bindings) {
    frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id,
        frame.frame_role_id, frame.compiled_frame_name, frame.model_revision});
  }
  return RoboPlanCandidateBindingDTO{
      value.controllable_component_id, value.command_target_id,
      group.model_instance_id,         model->model_revision,
      group.compiled_group_name,       std::move(resources),
      std::move(joint_names),          std::move(frames)};
}

[[nodiscard]] core::OperationIntentDTO
make_motion_operation(const core::RuntimeParticipantBinding &binding,
                      const core::RequestOwnerEpoch &owner_epoch,
                      const core::SceneRevision &scene_revision) {
  const auto &value = binding.value();
  std::vector<std::string> source_joint_names;
  source_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    if (resource.coordinate_kind != core::CoordinateKind::kAngular ||
        !resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      throw std::logic_error(
          "fixture motion target has no source angular joint");
    }
    source_joint_names.push_back(*resource.source_joint_name);
  }
  std::vector<double> goal_positions(value.resources.size(), 0.0);
  if (!goal_positions.empty()) {
    goal_positions.front() = 0.05;
  }
  return core::OperationIntentDTO{
      core::PublicRequestContextDTO{core::RequestId("request.native.motion.1"),
                                    owner_epoch, "retained_product_consumer",
                                    std::nullopt},
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      core::ProgramRunId("run.native.motion.1"),
      value.graph_revision,
      value.retained_bundle_id,
      scene_revision,
      {core::OperationParticipantIntentDTO{
          value.component_role_id, value.target_role_id, value.capability_id,
          std::nullopt,
          core::MotionParticipantIntentDTO{
              core::MoveId("move.native.retained-projection"),
              core::FrameRoleId("base"),
              core::MotionEndpointReferenceDTO{value.target_role_id,
                                               core::FrameRoleId("tcp")},
              core::TolerancesDTO{1.0e-4, 5.0e-3, 1.0e-2},
              core::SpeedProfileDTO{0.5, 0.5, std::nullopt},
              core::TimeoutsDTO{3.0, 5.0},
              core::CacheMode::kDefault,
              core::PlanningProfileDTO::kFirstFeasible,
              core::MotionTargetModeDTO::kAbsolute,
              std::nullopt,
              std::nullopt,
              core::JointGoalDTO{std::move(source_joint_names),
                                 std::move(goal_positions)},
              std::nullopt,
              {},
              {}}}},
      core::OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] core::OperationIntentDTO
make_gripper_operation(const core::RuntimeParticipantBinding &binding,
                       const core::RequestOwnerEpoch &owner_epoch) {
  const auto &value = binding.value();
  return core::OperationIntentDTO{
      core::PublicRequestContextDTO{core::RequestId("request.native.gripper.1"),
                                    owner_epoch, "retained_product_consumer",
                                    std::nullopt},
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      core::ProgramRunId("run.native.gripper.1"),
      value.graph_revision,
      value.retained_bundle_id,
      std::nullopt,
      {core::OperationParticipantIntentDTO{
          value.component_role_id, value.target_role_id, value.capability_id,
          std::nullopt,
          core::CapabilityCommandIntentDTO{
              core::CommandSchemaId(
                  std::string(core::kGripperPositionCommandSchemaId)),
              core::CommandSchemaRevision(
                  std::string(core::kGripperPositionCommandSchemaRevision)),
              core::GripperPositionCommandValueDTO{
                  core::LinearPositionDTO{0.004}, std::nullopt},
              1.0,
              {core::CommandDurationOptionDTO{
                  core::CommandOptionId("command_duration_s"), 0.0}}}}},
      core::OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] core::ConfigurationFenceScopeDTO make_runtime_fence_scope(
    const std::vector<const core::RuntimeParticipantBinding *> &bindings) {
  std::set<core::ControllableComponentId> components;
  std::map<core::ControlDomainId, core::ControlDomainRevision> domains;
  std::map<core::ControlBindingId, core::ControlBindingRevision>
      control_bindings;
  std::set<core::ResourceId> resources;
  for (const auto *binding : bindings) {
    if (binding == nullptr || !binding->value().control_path.has_value()) {
      throw std::logic_error("runtime fence fixture has no exact control path");
    }
    const auto &value = binding->value();
    const auto &control = *value.control_path;
    components.insert(value.controllable_component_id);
    const auto [domain, inserted_domain] = domains.emplace(
        control.control_domain_id, control.control_domain_revision);
    if (!inserted_domain && domain->second != control.control_domain_revision) {
      throw std::logic_error(
          "runtime fence fixture has conflicting domain revisions");
    }
    const auto [control_binding, inserted_binding] = control_bindings.emplace(
        control.control_binding_id, control.control_binding_revision);
    if (!inserted_binding &&
        control_binding->second != control.control_binding_revision) {
      throw std::logic_error(
          "runtime fence fixture has conflicting binding revisions");
    }
    for (const auto &resource : value.resources) {
      resources.insert(resource.resource_id);
    }
  }

  std::vector<core::ConfigurationFenceDomainScopeDTO> domain_scope;
  domain_scope.reserve(domains.size());
  for (const auto &[domain_id, revision] : domains) {
    domain_scope.push_back({domain_id, revision, revision});
  }
  std::vector<core::ConfigurationFenceBindingScopeDTO> binding_scope;
  binding_scope.reserve(control_bindings.size());
  for (const auto &[binding_id, revision] : control_bindings) {
    binding_scope.push_back({binding_id, revision, revision});
  }
  core::ConfigurationFenceScopeDTO scope{{components.begin(), components.end()},
                                         std::move(domain_scope),
                                         std::move(binding_scope),
                                         {resources.begin(), resources.end()}};
  if (!scope.valid()) {
    throw std::logic_error("runtime fence fixture scope is invalid");
  }
  return scope;
}

void expect_exact_planning_reopen(
    const deployment::VerifiedRetainedPlanningModel &first,
    const deployment::VerifiedRetainedPlanningModel &second) {
  EXPECT_EQ(first.query(), second.query());
  EXPECT_EQ(first.urdf_bytes(), second.urdf_bytes());
  EXPECT_EQ(first.srdf_bytes(), second.srdf_bytes());
  ASSERT_EQ(first.resources().size(), second.resources().size());
  for (std::size_t index = 0U; index < first.resources().size(); ++index) {
    const auto &left = first.resources()[index];
    const auto &right = second.resources()[index];
    EXPECT_EQ(left.logical_uri(), right.logical_uri());
    EXPECT_EQ(left.media_type(), right.media_type());
    EXPECT_EQ(left.byte_size(), right.byte_size());
    EXPECT_EQ(left.content_digest(), right.content_digest());
    EXPECT_EQ(left.content_addressed_store_key(),
              right.content_addressed_store_key());
    EXPECT_EQ(left.bytes(), right.bytes());
  }
}

struct Lite6SceneFacts final {
  std::size_t joint_count;
  std::size_t configuration_size;
  std::size_t velocity_size;
  std::size_t frame_count;
  std::size_t geometry_count;
  std::size_t collision_pair_count;
  std::vector<std::string> independent_joint_names;
  std::vector<std::string> mimic_expanded_joint_names;

  friend bool operator==(const Lite6SceneFacts &,
                         const Lite6SceneFacts &) = default;
};

[[nodiscard]] Lite6SceneFacts
require_meaningful_lite6_scene(const roboplan::Scene &scene) {
  const auto &model = scene.getModel();
  const auto independent = scene.getJointNames();
  const auto with_mimics = scene.getJointNamesWithMimics();
  const auto &collision_model = scene.getCollisionModel();

  EXPECT_GT(model.njoints, 1);
  EXPECT_GE(model.nq, 7);
  EXPECT_GE(model.nv, 7);
  EXPECT_GE(model.nframes, 10);
  EXPECT_GE(independent.size(), 7U);
  EXPECT_GT(with_mimics.size(), independent.size());
  EXPECT_GE(collision_model.geometryObjects.size(), 10U);
  EXPECT_FALSE(collision_model.collisionPairs.empty());
  EXPECT_EQ(scene.getCurrentJointPositions().size(), model.nq);
  EXPECT_NO_THROW(
      scene.computeCollisionDistances(scene.getCurrentJointPositions()));

  return {
      static_cast<std::size_t>(model.njoints),
      static_cast<std::size_t>(model.nq),
      static_cast<std::size_t>(model.nv),
      static_cast<std::size_t>(model.nframes),
      collision_model.geometryObjects.size(),
      collision_model.collisionPairs.size(),
      independent,
      with_mimics,
  };
}

[[nodiscard]] std::filesystem::path
staged_resource_path(const RetainedPlanningModelStage &stage,
                     std::string_view logical_uri) {
  constexpr std::string_view kAssetPrefix = "asset://";
  if (!logical_uri.starts_with(kAssetPrefix)) {
    throw std::logic_error("verified planning resource is not an asset URI");
  }
  return stage.staging_root() /
         std::string(logical_uri.substr(kAssetPrefix.size()));
}

[[nodiscard]] RetainedPlanningModelInput prepared_model_input(
    std::string urdf, std::string srdf,
    const std::filesystem::path& source_root,
    std::string_view exact_asset_prefix) {
  constexpr std::string_view kFilenamePrefix = "filename=\"";
  if (!exact_asset_prefix.starts_with("asset://") ||
      !exact_asset_prefix.ends_with('/')) {
    throw std::logic_error("test asset prefix is not exact and bounded");
  }

  std::set<std::string, std::less<>> logical_uris;
  std::size_t cursor = 0U;
  while ((cursor = urdf.find(kFilenamePrefix, cursor)) != std::string::npos) {
    const auto value_start = cursor + kFilenamePrefix.size();
    const auto value_end = urdf.find('"', value_start);
    if (value_end == std::string::npos) {
      throw std::logic_error("prepared test URDF has an unterminated filename");
    }
    std::string logical_uri = urdf.substr(value_start, value_end - value_start);
    if (!std::string_view(logical_uri).starts_with(exact_asset_prefix)) {
      throw std::logic_error(
          "prepared test URDF escaped its closure-local asset prefix");
    }
    logical_uris.insert(std::move(logical_uri));
    cursor = value_end + 1U;
  }
  if (logical_uris.empty()) {
    throw std::logic_error("prepared test URDF has no retained resources");
  }

  RetainedPlanningModelInput result{std::move(urdf), std::move(srdf), {}};
  result.resources.reserve(logical_uris.size());
  for (const auto& logical_uri : logical_uris) {
    const std::string relative = logical_uri.substr(exact_asset_prefix.size());
    if (relative.empty() || relative.starts_with('/') ||
        relative.find("..") != std::string::npos) {
      throw std::logic_error("prepared test resource path is not bounded");
    }
    const auto extension = std::filesystem::path(relative).extension();
    const std::string media_type =
        extension == ".stl"   ? "model/stl"
        : extension == ".dae" ? "model/vnd.collada+xml"
                              : throw std::logic_error(
                                    "prepared test resource type is unsupported");
    std::string bytes = deployment::test::read_lite6_fixture_file(
        source_root / relative);
    const std::string digest = deployment::sha256_digest(bytes);
    result.resources.push_back(RetainedPlanningResourceInput{
        logical_uri, media_type, static_cast<std::uint64_t>(bytes.size()),
        digest,
        "sha256/" + digest.substr(std::string_view("sha256:").size()),
        std::move(bytes)});
  }
  return result;
}

[[nodiscard]] std::map<std::string, std::string, std::less<>>
load_joint_name_map(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("cannot open RoboPlan import joint map");
  }
  std::map<std::string, std::string, std::less<>> result;
  std::string line;
  while (std::getline(stream, line)) {
    const auto separator = line.find('\t');
    if (separator == std::string::npos || separator == 0U ||
        separator + 1U == line.size() ||
        !result.emplace(line.substr(0U, separator),
                        line.substr(separator + 1U))
             .second) {
      throw std::runtime_error("RoboPlan import joint map is malformed");
    }
  }
  if (result.empty()) {
    throw std::runtime_error("RoboPlan import joint map is empty");
  }
  return result;
}

[[nodiscard]] std::string
compiled_symbol(const deployment::VerifiedRetainedModelMap &model_map,
                deployment::OccurrenceSymbolKind kind,
                std::string_view source_name,
                std::string_view scope_name = {}) {
  const auto found =
      std::find_if(model_map.symbols().begin(), model_map.symbols().end(),
                   [&](const deployment::OccurrenceSymbolRewrite &rewrite) {
                     return rewrite.source.model_instance_id.value() ==
                                "lite6_fixture_instance" &&
                            rewrite.source.kind == kind &&
                            rewrite.source.source_name == source_name &&
                            rewrite.source.scope_name == scope_name;
                   });
  if (found == model_map.symbols().end()) {
    throw std::logic_error(
        "retained Lite6 model map is missing source symbol " +
        std::string(source_name));
  }
  return found->compiled_name;
}

void expect_total_retained_model_map_semantics(
    const deployment::VerifiedRetainedModelMap &model_map) {
  using SourceKey = std::tuple<std::string, deployment::OccurrenceSymbolKind,
                               std::string, std::string>;
  std::set<SourceKey> source_keys;
  std::set<std::string, std::less<>> compiled_names;
  for (const auto &rewrite : model_map.symbols()) {
    EXPECT_TRUE(source_keys
                    .emplace(rewrite.source.model_instance_id.value(),
                             rewrite.source.kind, rewrite.source.scope_name,
                             rewrite.source.source_name)
                    .second);
    EXPECT_TRUE(compiled_names.emplace(rewrite.compiled_name).second);
  }
  ASSERT_EQ(source_keys.size(), model_map.symbols().size());
  ASSERT_EQ(compiled_names.size(), model_map.symbols().size());

  for (const auto &reference : model_map.references()) {
    const auto target =
        std::find_if(model_map.symbols().begin(), model_map.symbols().end(),
                     [&](const deployment::OccurrenceSymbolRewrite &candidate) {
                       return candidate.source == reference.source_target;
                     });
    ASSERT_NE(target, model_map.symbols().end());
    EXPECT_EQ(target->compiled_name, reference.compiled_target_name);
  }

  std::set<std::pair<std::string, std::size_t>> coordinate_keys;
  for (const auto &coordinate : model_map.coordinates()) {
    EXPECT_TRUE(coordinate_keys
                    .emplace(coordinate.model_instance_id.value(),
                             coordinate.source_joint_index)
                    .second);
    EXPECT_EQ(coordinate.compiled_joint_name,
              compiled_symbol(model_map,
                              deployment::OccurrenceSymbolKind::kJoint,
                              coordinate.source_joint_name));
  }
  EXPECT_EQ(coordinate_keys.size(), model_map.coordinates().size());
}

void expect_exact_retained_planning_resources(
    const deployment::VerifiedRetainedPlanningModel &retained) {
  std::set<std::string, std::less<>> retained_uris;
  for (const auto &resource : retained.resources()) {
    EXPECT_TRUE(
        retained_uris.emplace(std::string(resource.logical_uri())).second);
    EXPECT_EQ(resource.byte_size(), resource.bytes().size());
    EXPECT_EQ(resource.content_digest().value(),
              deployment::sha256_digest(resource.bytes()));
    EXPECT_EQ(resource.content_addressed_store_key().value(),
              "sha256/" + resource.content_digest().value().substr(
                              std::string_view("sha256:").size()));
  }
}

[[nodiscard]] double mimic_expanded_position(const roboplan::Scene &scene,
                                             std::string_view joint_name) {
  const auto names = scene.getJointNamesWithMimics();
  const auto values = scene.getCurrentJointPositionsWithMimics();
  const auto found = std::find(names.begin(), names.end(), joint_name);
  if (found == names.end()) {
    throw std::logic_error("RoboPlan mimic-expanded state is missing " +
                           std::string(joint_name));
  }
  return values[static_cast<Eigen::Index>(std::distance(names.begin(), found))];
}

void expect_exact_retained_and_roboplan_mimic_semantics(
    const deployment::VerifiedRetainedModelMap &model_map,
    roboplan::Scene &scene) {
  for (std::size_t index = 0U; index < model_map.coordinates().size();
       ++index) {
    const auto &coordinate = model_map.coordinates()[index];
    EXPECT_EQ(coordinate.model_instance_id.value(), "lite6_fixture_instance");
    EXPECT_EQ(coordinate.source_joint_index, index);
    EXPECT_EQ(coordinate.compiled_joint_name,
              compiled_symbol(model_map,
                              deployment::OccurrenceSymbolKind::kJoint,
                              coordinate.source_joint_name));
    if (coordinate.mimic_source_compiled_joint_name.has_value()) {
      const auto master = std::find_if(
          model_map.coordinates().begin(), model_map.coordinates().end(),
          [&](const deployment::CompositionModelCoordinate &candidate) {
            return candidate.model_instance_id ==
                       coordinate.model_instance_id &&
                   candidate.compiled_joint_name ==
                       *coordinate.mimic_source_compiled_joint_name;
          });
      ASSERT_NE(master, model_map.coordinates().end());
      EXPECT_NE(master->source_joint_name, coordinate.source_joint_name);
    }
  }

  const std::string master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                      "right_finger_joint");
  const std::string follower = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kJoint, "left_finger_joint");
  const std::string gripper_group = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kSrdfGroup, "lite_gripper");
  EXPECT_LT(master, follower);
  const auto follower_coordinate = std::find_if(
      model_map.coordinates().begin(), model_map.coordinates().end(),
      [&follower](const deployment::CompositionModelCoordinate &candidate) {
        return candidate.compiled_joint_name == follower;
      });
  ASSERT_NE(follower_coordinate, model_map.coordinates().end());
  ASSERT_TRUE(
      follower_coordinate->mimic_source_compiled_joint_name.has_value());
  EXPECT_EQ(*follower_coordinate->mimic_source_compiled_joint_name, master);
  EXPECT_DOUBLE_EQ(follower_coordinate->mimic_multiplier, 1.0);
  EXPECT_DOUBLE_EQ(follower_coordinate->mimic_offset, 0.0);

  const auto group = scene.getJointGroupInfo(gripper_group);
  ASSERT_TRUE(group.has_value()) << group.error();
  EXPECT_EQ(group->joint_names, (std::vector<std::string>{follower, master}));
  EXPECT_EQ(static_cast<std::size_t>(group->q_indices.size()), 1U);
  const auto independent = scene.getJointNames();
  const auto expanded = scene.getJointNamesWithMimics();
  std::vector<std::string> expected_independent;
  for (int index = 1; index <= 6; ++index) {
    expected_independent.push_back(
        compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                        "joint" + std::to_string(index)));
  }
  expected_independent.push_back(master);
  auto expected_expanded = expected_independent;
  expected_expanded.push_back(follower);
  EXPECT_EQ(scene.getModel().nq, 7);
  EXPECT_EQ(scene.getModel().nv, 7);
  EXPECT_EQ(independent.size(), 7U);
  EXPECT_EQ(expanded.size(), 8U);
  EXPECT_EQ(independent, expected_independent);
  EXPECT_EQ(expanded, expected_expanded);
  EXPECT_EQ(std::count(independent.begin(), independent.end(), master), 1);
  EXPECT_EQ(std::count(independent.begin(), independent.end(), follower), 0);
  EXPECT_EQ(std::count(expanded.begin(), expanded.end(), master), 1);
  EXPECT_EQ(std::count(expanded.begin(), expanded.end(), follower), 1);

  auto positions = scene.getCurrentJointPositions();
  const auto master_id = scene.getModel().getJointId(master);
  ASSERT_LT(master_id, scene.getModel().idx_qs.size());
  positions[scene.getModel().idx_qs[master_id]] = 0.004;
  scene.setJointPositions(positions);
  EXPECT_NEAR(mimic_expanded_position(scene, master), 0.004, 1e-12);
  EXPECT_NEAR(mimic_expanded_position(scene, follower), 0.004, 1e-12);
}

[[nodiscard]] deployment::UrdfJointIr
synthetic_joint(std::string name, std::string parent_link,
                std::string child_link, deployment::UrdfJointType type,
                std::optional<std::string> mimic_master = std::nullopt) {
  deployment::UrdfJointIr joint{};
  joint.name = std::move(name);
  joint.parent_link_name = std::move(parent_link);
  joint.child_link_name = std::move(child_link);
  joint.type = type;
  joint.coordinate_kind = type == deployment::UrdfJointType::kFixed
                              ? deployment::UrdfCoordinateKind::kNone
                              : deployment::UrdfCoordinateKind::kLinear;
  if (mimic_master.has_value()) {
    joint.mimic = deployment::UrdfMimicIr{
        .joint_name = std::move(*mimic_master),
        .authored_multiplier = 1.0,
        .authored_offset = 0.0,
        .multiplier = 1.0,
        .offset = 0.0,
    };
  }
  return joint;
}

[[nodiscard]] std::string synthetic_mf2_name(std::size_t rank,
                                             std::string_view source_name) {
  return deployment::detail::make_mf2_generated_joint_name(
      rank, {
                {"fixture", "pinocchio_cross_branch"},
                {"source", source_name},
            });
}

[[nodiscard]] deployment::RetainedProductArtifactQuery artifact_query(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    std::string_view role) {
  const auto &product = admission.retained_bundle()
                            .value()
                            .compiled_products.planning_collision_model;
  return {admission.retained_bundle_id(), product.product_id,
          product.product_kind, core::ArtifactRoleId(std::string(role))};
}

[[nodiscard]] deployment::VerifiedRetainedProductArtifact require_artifact(
    const deployment::IVerifiedRetainedProductArtifactSource &source,
    const deployment::RetainedProductArtifactQuery &query) {
  std::optional<deployment::VerifiedRetainedProductArtifact> artifact =
      source.get_verified(query);
  if (!artifact.has_value()) {
    throw std::runtime_error(
        "canonical-empty retained product artifact is absent");
  }
  return std::move(*artifact);
}

void expect_canonical_empty_scene(const roboplan::Scene &scene) {
  ASSERT_EQ(scene.getModel().njoints, 1);
  ASSERT_EQ(scene.getModel().names.size(), 1U);
  EXPECT_EQ(scene.getModel().names.front(), "universe");
  ASSERT_EQ(scene.getModel().nframes, 2);
  const auto cell_frame_id =
      scene.getModel().getFrameId(std::string(kCanonicalCellFrame));
  ASSERT_EQ(cell_frame_id, 1U);
  const auto &cell_frame = scene.getModel().frames[cell_frame_id];
  EXPECT_EQ(cell_frame.parentJoint, 0U);
  EXPECT_TRUE(cell_frame.placement.toHomogeneousMatrix().isApprox(
      Eigen::Matrix4d::Identity()));
  EXPECT_EQ(scene.getModel().nq, 0);
  EXPECT_EQ(scene.getModel().nv, 0);
  EXPECT_TRUE(scene.getJointNames().empty());
  EXPECT_TRUE(scene.getJointNamesWithMimics().empty());
  EXPECT_EQ(scene.getCurrentJointPositions().size(), 0);
  EXPECT_TRUE(scene.getCollisionModel().geometryObjects.empty());
  EXPECT_TRUE(scene.getCollisionModel().collisionPairs.empty());

  const Eigen::VectorXd empty_configuration(0);
  EXPECT_FALSE(scene.hasCollisions(empty_configuration));
  EXPECT_NO_THROW(scene.computeCollisionDistances(empty_configuration));
  EXPECT_TRUE(scene.getCollisionData().distanceResults.empty());
}

TEST(RetainedProductCrossConsumer,
     CompilerEmittedSymbolAndBodyLookupPerformanceStaysBounded) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));

  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  ASSERT_EQ(
      materialized.bundle->value().compiled_products.symbol_role_maps.size(),
      1U);
  const auto &product =
      materialized.bundle->value().compiled_products.symbol_role_maps.front();
  const auto model_map = load_lite6_model_map(
      package_root,
      {materialized.bundle->retained_bundle_id(), product.product_id});
  ASSERT_FALSE(model_map.symbols().empty());
  ASSERT_FALSE(model_map.model_collision_bodies().empty());

  constexpr std::size_t kWarmupIterations = 16U;
  constexpr std::size_t kIterations = 100U;
  constexpr std::size_t kSymbolLookupsPerIteration = 32U;
  constexpr std::size_t kBodyLookupsPerIteration = 32U;
  const auto lookup_batch = [&model_map]() {
    std::size_t hits = 0U;
    for (std::size_t index = 0U; index < kSymbolLookupsPerIteration; ++index) {
      const auto &query =
          model_map.symbols()[index % model_map.symbols().size()].source;
      const auto found = std::find_if(
          model_map.symbols().begin(), model_map.symbols().end(),
          [&query](const deployment::OccurrenceSymbolRewrite &candidate) {
            return candidate.source == query;
          });
      hits += found != model_map.symbols().end() ? 1U : 0U;
    }
    for (std::size_t index = 0U; index < kBodyLookupsPerIteration; ++index) {
      const auto &query =
          model_map.model_collision_bodies()
              [index % model_map.model_collision_bodies().size()];
      const auto found = std::find_if(
          model_map.model_collision_bodies().begin(),
          model_map.model_collision_bodies().end(),
          [&query](const deployment::CompositionModelCollisionBodyResolution
                       &candidate) {
            return candidate.model_instance_id == query.model_instance_id &&
                   candidate.collision_body_role_id ==
                       query.collision_body_role_id;
          });
      hits += found != model_map.model_collision_bodies().end() ? 1U : 0U;
    }
    return hits;
  };

  std::size_t observed_hits = 0U;
  for (std::size_t iteration = 0U; iteration < kWarmupIterations; ++iteration) {
    observed_hits += lookup_batch();
  }
  std::vector<double> samples_us;
  samples_us.reserve(kIterations);
  for (std::size_t iteration = 0U; iteration < kIterations; ++iteration) {
    const auto started_at = std::chrono::steady_clock::now();
    observed_hits += lookup_batch();
    samples_us.push_back(std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - started_at)
                             .count());
  }
  EXPECT_EQ(observed_hits,
            (kWarmupIterations + kIterations) *
                (kSymbolLookupsPerIteration + kBodyLookupsPerIteration));

  const auto percentile = [&samples_us](const double ratio) {
    auto sorted = samples_us;
    std::sort(sorted.begin(), sorted.end());
    return sorted[static_cast<std::size_t>(
        ratio * static_cast<double>(sorted.size() - 1U))];
  };
  const double p50_us = percentile(0.50);
  const double p95_us = percentile(0.95);
  const double p99_us = percentile(0.99);

  // The deleted mapper's benchmark was informational. The current route has
  // no runtime map rebuild: it point-reads the immutable compiler-emitted map.
  // Five milliseconds is a deliberately broad regression tripwire, not an
  // attempt to compare unlike FNV and retained-product workloads.
  EXPECT_LT(p99_us, 5000.0);
  std::cout << "{\"schema\":\"manyforge.i6.retained_model_lookup_perf.v1\","
            << "\"runtime_map_rebuilds\":0,"
            << "\"symbol_lookups_per_iteration\":" << kSymbolLookupsPerIteration
            << ","
            << "\"body_lookups_per_iteration\":" << kBodyLookupsPerIteration
            << ",\"iterations\":" << kIterations << ",\"lookup_batch_us\":{"
            << "\"p50\":" << p50_us << ",\"p95\":" << p95_us
            << ",\"p99\":" << p99_us << "}}\n";
}

TEST(RetainedProductRoboPlanConsumer,
     Mf2CrossBranchTraversalImportsDeeperMasterBeforeFollower) {
  deployment::UrdfDocumentIr source;
  source.robot_name = "mf2_cross_branch";
  source.joints = {
      synthetic_joint("a_follower_branch", "root", "follower_branch",
                      deployment::UrdfJointType::kFixed),
      synthetic_joint("follower", "follower_branch", "follower_tip",
                      deployment::UrdfJointType::kPrismatic, "master"),
      synthetic_joint("z_master_branch", "root", "master_branch",
                      deployment::UrdfJointType::kFixed),
      synthetic_joint("master", "master_branch", "master_tip",
                      deployment::UrdfJointType::kPrismatic),
  };
  const auto rank_rows =
      deployment::detail::dependency_ordered_joint_local_ranks(source);
  const std::map<std::string, std::size_t, std::less<>> ranks(rank_rows.begin(),
                                                              rank_rows.end());
  ASSERT_EQ(ranks.at("z_master_branch"), 0U);
  ASSERT_EQ(ranks.at("a_follower_branch"), 1U);
  const std::string master_branch =
      synthetic_mf2_name(ranks.at("z_master_branch"), "z_master_branch");
  const std::string follower_branch =
      synthetic_mf2_name(ranks.at("a_follower_branch"), "a_follower_branch");
  const std::string master = synthetic_mf2_name(ranks.at("master"), "master");
  const std::string follower =
      synthetic_mf2_name(ranks.at("follower"), "follower");
  ASSERT_LT(master_branch, follower_branch);

  const std::string urdf =
      "<robot name=\"mf2_cross_branch\">"
      "<link name=\"root\"/>"
      "<link name=\"follower_branch\"/>"
      "<joint name=\"" +
      follower_branch +
      "\" type=\"fixed\"><parent link=\"root\"/><child "
      "link=\"follower_branch\"/></joint>"
      "<link name=\"follower_tip\"/>"
      "<joint name=\"" +
      follower +
      "\" type=\"prismatic\"><parent link=\"follower_branch\"/><child "
      "link=\"follower_tip\"/><axis xyz=\"1 0 0\"/><limit lower=\"-1\" "
      "upper=\"1\" effort=\"1\" velocity=\"1\"/><mimic joint=\"" +
      master +
      "\" multiplier=\"1\" offset=\"0\"/></joint>"
      "<link name=\"master_branch\"/>"
      "<joint name=\"" +
      master_branch +
      "\" type=\"fixed\"><parent link=\"root\"/><child "
      "link=\"master_branch\"/></joint>"
      "<link name=\"master_tip\"/>"
      "<joint name=\"" +
      master +
      "\" type=\"prismatic\"><parent link=\"master_branch\"/><child "
      "link=\"master_tip\"/><axis xyz=\"1 0 0\"/><limit lower=\"-1\" "
      "upper=\"1\" effort=\"1\" velocity=\"1\"/></joint>"
      "</robot>";
  const std::string srdf =
      "<robot name=\"mf2_cross_branch\"><group name=\"motion\">"
      "<joint name=\"" +
      follower + "\"/><joint name=\"" + master + "\"/></group></robot>";

  const auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "mf2_cross_branch", urdf, srdf, {},
      retained_joint_limit_rows(urdf, std::string_view(srdf))});
  EXPECT_EQ(scene->getModel().nq, 1);
  EXPECT_EQ(scene->getModel().nv, 1);
  EXPECT_EQ(scene->getJointNames(), (std::vector<std::string>{master}));
  EXPECT_EQ(scene->getJointNamesWithMimics(),
            (std::vector<std::string>{master, follower}));
  const auto group = scene->getJointGroupInfo("motion");
  ASSERT_TRUE(group.has_value()) << group.error();
  EXPECT_EQ(group->joint_names, (std::vector<std::string>{follower, master}));
  EXPECT_EQ(static_cast<std::size_t>(group->q_indices.size()), 1U);
  auto positions = scene->getCurrentJointPositions();
  ASSERT_EQ(positions.size(), 1);
  positions[0] = 0.25;
  scene->setJointPositions(positions);
  EXPECT_NEAR(mimic_expanded_position(*scene, master), 0.25, 1e-12);
  EXPECT_NEAR(mimic_expanded_position(*scene, follower), 0.25, 1e-12);
}

TEST(RetainedProductRoboPlanConsumer,
     MaintainedUr5eFactoryPreservesNamesAndRadianLimits) {
  const std::filesystem::path model_root(
      MANYFORGE_UR5E_STRUCTURAL_SOURCE_ROOT);
  const std::filesystem::path fixture_root(
      MANYFORGE_UR5E_STRUCTURAL_FIXTURE_ROOT);
  const std::string verified_urdf =
      roboplan::loadTextFile(fixture_root / "model.urdf");
  const std::string verified_srdf =
      roboplan::loadTextFile(fixture_root / "model.srdf");
  const auto limits = retained_joint_limit_rows(verified_urdf, verified_srdf);
  ASSERT_EQ(limits.size(), 7U);
  const auto joint_names = load_joint_name_map(
      MANYFORGE_UR5E_ROBOPLAN_IMPORT_JOINT_MAP);

  ScopedTemporaryRoot temporary;
  const auto staging_parent = temporary.path() / "staging";
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  auto stage = RetainedPlanningModelStage::create(
      prepared_model_input(verified_urdf, verified_srdf, model_root,
                           "asset://ur5e_robotiq_2f_85/"),
      staging_parent);
  EXPECT_NE(stage.loader_urdf_bytes().find("package://ur5e_robotiq_2f_85/"),
            std::string_view::npos);
  EXPECT_EQ(stage.loader_urdf_bytes().find("asset://"),
            std::string_view::npos);
  ASSERT_EQ(stage.package_paths().size(), 1U);
  EXPECT_EQ(stage.package_paths().front(), stage.staging_root());

  const auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_ur5e", std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(), limits});
  const std::vector<std::string> arm_names{
      "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
      "wrist_1_joint",     "wrist_2_joint",       "wrist_3_joint"};
  std::vector<std::string> imported_arm_names;
  imported_arm_names.reserve(arm_names.size());
  std::transform(arm_names.begin(), arm_names.end(),
                 std::back_inserter(imported_arm_names),
                 [&joint_names](const auto& name) {
                   return joint_names.at(name);
                 });
  const auto arm = scene->getJointGroupInfo("ur_manipulator");
  ASSERT_TRUE(arm.has_value()) << arm.error();
  EXPECT_EQ(arm->joint_names, imported_arm_names);

  for (const auto& name : arm_names) {
    const auto joint = scene->getJointInfo(joint_names.at(name));
    ASSERT_TRUE(joint.has_value()) << name;
    ASSERT_EQ(joint->limits.max_velocity.size(), 1);
    EXPECT_DOUBLE_EQ(joint->limits.max_velocity[0], std::numbers::pi) << name;
  }
  const auto shoulder =
      scene->getJointInfo(joint_names.at("shoulder_pan_joint"));
  ASSERT_TRUE(shoulder.has_value());
  EXPECT_DOUBLE_EQ(shoulder->limits.min_position[0], -2.0 * std::numbers::pi);
  EXPECT_DOUBLE_EQ(shoulder->limits.max_position[0], 2.0 * std::numbers::pi);
  const auto elbow = scene->getJointInfo(joint_names.at("elbow_joint"));
  ASSERT_TRUE(elbow.has_value());
  EXPECT_DOUBLE_EQ(elbow->limits.min_position[0], -std::numbers::pi);
  EXPECT_DOUBLE_EQ(elbow->limits.max_position[0], std::numbers::pi);

  const auto gripper = scene->getJointInfo(
      joint_names.at("robotiq_85_left_knuckle_joint"));
  ASSERT_TRUE(gripper.has_value());
  EXPECT_DOUBLE_EQ(gripper->limits.min_position[0], 0.0);
  EXPECT_DOUBLE_EQ(gripper->limits.max_position[0], 0.8);
  EXPECT_DOUBLE_EQ(gripper->limits.max_velocity[0], 0.5);
  const auto gripper_group =
      scene->getJointGroupInfo("robotiq_2f_85_gripper");
  ASSERT_TRUE(gripper_group.has_value()) << gripper_group.error();
  EXPECT_EQ(gripper_group->joint_names.size(), 6U);
  EXPECT_EQ(gripper_group->q_indices.size(), 1U);
}

TEST(RetainedProductRoboPlanConsumer,
     CanonicalEmptyExactBytesYieldDeterministicPlanningCollisionScene) {
  const std::filesystem::path package_root(
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT);
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource
      admission_source(package_root);
  const auto admission = admission_source.load_verified();
  const deployment::FilesystemVerifiedRetainedProductArtifactSource
      product_source(package_root);

  const auto urdf_query = artifact_query(admission, "urdf");
  const auto srdf_query = artifact_query(admission, "srdf");
  const auto urdf = require_artifact(product_source, urdf_query);
  const auto srdf = require_artifact(product_source, srdf_query);
  EXPECT_EQ(urdf.query(), urdf_query);
  EXPECT_EQ(srdf.query(), srdf_query);
  EXPECT_EQ(urdf.product_contract_id(), srdf.product_contract_id());
  EXPECT_EQ(urdf.product_contract_revision(), srdf.product_contract_revision());
  EXPECT_EQ(urdf.artifact_ref().media_type, "application/xml");
  EXPECT_EQ(srdf.artifact_ref().media_type, "application/xml");

  const auto first_scene = make_roboplan_scene(RoboPlanSceneInput{
      admission.cell_id().value(), std::string(urdf.bytes()),
      std::string(srdf.bytes()), {},
      retained_joint_limit_rows(urdf.bytes(), srdf.bytes())});
  expect_canonical_empty_scene(*first_scene);

  const deployment::FilesystemCanonicalEmptyRetainedBundleSource
      reopened_admission_source(package_root);
  const auto reopened_admission = reopened_admission_source.load_verified();
  const deployment::FilesystemVerifiedRetainedProductArtifactSource
      reopened_product_source(package_root);
  const auto reopened_urdf = require_artifact(
      reopened_product_source, artifact_query(reopened_admission, "urdf"));
  const auto reopened_srdf = require_artifact(
      reopened_product_source, artifact_query(reopened_admission, "srdf"));

  EXPECT_EQ(reopened_admission.retained_bundle_id(),
            admission.retained_bundle_id());
  EXPECT_EQ(reopened_urdf.query(), urdf.query());
  EXPECT_EQ(reopened_srdf.query(), srdf.query());
  EXPECT_EQ(reopened_urdf.product_contract_id(), urdf.product_contract_id());
  EXPECT_EQ(reopened_srdf.product_contract_id(), srdf.product_contract_id());
  EXPECT_EQ(reopened_urdf.product_contract_revision(),
            urdf.product_contract_revision());
  EXPECT_EQ(reopened_srdf.product_contract_revision(),
            srdf.product_contract_revision());
  EXPECT_EQ(reopened_urdf.artifact_ref(), urdf.artifact_ref());
  EXPECT_EQ(reopened_srdf.artifact_ref(), srdf.artifact_ref());
  EXPECT_EQ(reopened_urdf.bytes(), urdf.bytes());
  EXPECT_EQ(reopened_srdf.bytes(), srdf.bytes());

  const auto reopened_scene = make_roboplan_scene(RoboPlanSceneInput{
      reopened_admission.cell_id().value(),
      std::string(reopened_urdf.bytes()),
      std::string(reopened_srdf.bytes()), {},
      retained_joint_limit_rows(reopened_urdf.bytes(),
                                reopened_srdf.bytes())});
  expect_canonical_empty_scene(*reopened_scene);
  EXPECT_EQ(reopened_scene->getModel().nframes,
            first_scene->getModel().nframes);
  EXPECT_EQ(reopened_scene->getCollisionModel().geometryObjects.size(),
            first_scene->getCollisionModel().geometryObjects.size());
}

TEST(RetainedProductRoboPlanConsumer,
     DurableLite6ReopensStagesAndBuildsDeterministicNonEmptyScene) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized =
      deployment::test::materialize_lite6_ros2_control_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  ASSERT_EQ(materialized.bundle->value().provenance.generator_records.size(),
            1U);
  EXPECT_EQ(materialized.bundle->value().provenance.generator_records.front(),
            materialized.input.preparation.generator_record());
  const auto &planning =
      materialized.bundle->value().compiled_products.planning_collision_model;
  const deployment::RetainedPlanningModelQuery query{
      materialized.bundle->retained_bundle_id(), planning.product_id};
  ASSERT_EQ(
      materialized.bundle->value().compiled_products.symbol_role_maps.size(),
      1U);
  const auto &symbol_product =
      materialized.bundle->value().compiled_products.symbol_role_maps.front();
  const deployment::RetainedModelMapQuery model_map_query{
      materialized.bundle->retained_bundle_id(), symbol_product.product_id};

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      projection_source(package_root);
  const auto first_projection = deployment::verify_retained_projection_input(
      *materialized.bundle,
      make_projection_runtime_shadow_snapshot(
          materialized.bundle->value().cell_graph.cell_id),
      projection_source);
  const auto &first = first_projection.planning_model();
  EXPECT_EQ(first_projection.projection_revision(),
            (core::ProjectionRevisionDTO{
                materialized.bundle->retained_bundle_id(),
                materialized.bundle->value().cell_graph.graph_revision,
                first_projection.logical_scene().shared()->revision}));
  const auto model_map = load_lite6_model_map(package_root, model_map_query);
  expect_total_retained_model_map_semantics(model_map);
  ASSERT_NO_THROW(model_map.require_same_bundle_as(first));
  ASSERT_EQ(model_map.query().retained_bundle_id,
            first.query().retained_bundle_id);
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto *arm_binding =
      model_map.find_binding(component_id, command_target_id);
  const auto *arm_target =
      model_map.find_target(component_id, command_target_id);
  ASSERT_NE(arm_binding, nullptr);
  ASSERT_NE(arm_target, nullptr);
  ASSERT_FALSE(arm_target->model_groups.empty());
  ASSERT_EQ(arm_binding->resources.size(), 6U);
  ASSERT_FALSE(first.resources().empty());
  expect_exact_retained_planning_resources(first);
  const std::filesystem::path generated_fixture_root(
      MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT);
  EXPECT_EQ(first.urdf_bytes(),
            deployment::test::read_lite6_fixture_file(generated_fixture_root /
                                                      "retained-model.urdf"));
  EXPECT_EQ(first.srdf_bytes(),
            deployment::test::read_lite6_fixture_file(generated_fixture_root /
                                                      "retained-model.srdf"));

  // This unrelated valid CAS blob must never become an implicit model
  // resource merely because it is present under the package root.
  const deployment::StoredRawArtifact ambient_blob = [&]() {
    deployment::FilesystemRawArtifactStore raw_store(package_root);
    return raw_store.put_if_absent("unreferenced ambient retained bytes");
  }();
  for (const auto &resource : first.resources()) {
    EXPECT_NE(resource.content_digest(), ambient_blob.content_digest);
    EXPECT_NE(resource.bytes(), "unreferenced ambient retained bytes");
  }

  const auto reopened_projection = deployment::verify_retained_projection_input(
      *materialized.bundle,
      make_projection_runtime_shadow_snapshot(
          materialized.bundle->value().cell_graph.cell_id),
      projection_source);
  const auto &reopened = reopened_projection.planning_model();
  EXPECT_EQ(reopened_projection.projection_revision(),
            first_projection.projection_revision());
  EXPECT_EQ(reopened_projection.logical_scene().shared()->identity(),
            first_projection.logical_scene().shared()->identity());
  EXPECT_TRUE(
      core::IsCanonicalEmptyCollisionPolicy(reopened_projection.logical_scene()
                                                .shared()
                                                ->configured_collision_policy));
  EXPECT_TRUE(reopened_projection.logical_scene().shared()->objects.empty());
  EXPECT_TRUE(reopened_projection.logical_scene()
                  .shared()
                  ->carried_object_attachments.empty());
  expect_exact_planning_reopen(first, reopened);

  std::filesystem::path first_stage_root;
  std::string first_loader_urdf;
  Lite6SceneFacts first_scene_facts{};
  {
    auto stage =
        RetainedPlanningModelStage::create_verified(first, staging_parent);
    first_stage_root = stage.staging_root();
    first_loader_urdf = stage.loader_urdf_bytes();
    ASSERT_EQ(stage.package_paths().size(), 1U);
    EXPECT_EQ(stage.package_paths().front(), stage.staging_root());
    EXPECT_EQ(stage.staging_root().parent_path(), staging_parent);
    EXPECT_EQ(stage.loader_urdf_bytes().find("asset://"),
              std::string_view::npos);
    EXPECT_EQ(stage.loader_urdf_bytes().find("file://"),
              std::string_view::npos);
    EXPECT_EQ(stage.loader_urdf_bytes().find("model://"),
              std::string_view::npos);
    EXPECT_EQ(
        stage.loader_urdf_bytes().find("package://manyforge_example_models/"),
        std::string_view::npos);
    for (const auto &resource : first.resources()) {
      EXPECT_TRUE(std::filesystem::is_regular_file(
          staged_resource_path(stage, resource.logical_uri())));
    }

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    try {
      const auto joint_limits = retained_joint_limit_rows(first);
      auto scene = make_roboplan_scene(RoboPlanSceneInput{
          "retained_lite6", std::string(stage.loader_urdf_bytes()),
          std::string(stage.srdf_bytes()), stage.package_paths(),
          joint_limits});
      first_scene_facts = require_meaningful_lite6_scene(*scene);
      std::size_t imported_acceleration_limit_count = 0U;
      for (const auto& row : joint_limits) {
        if (!row.maximum_acceleration.has_value()) {
          continue;
        }
        const auto imported = scene->getJointInfo(row.joint_name);
        ASSERT_TRUE(imported.has_value()) << row.joint_name;
        ASSERT_EQ(imported->limits.max_acceleration.size(), 1);
        EXPECT_DOUBLE_EQ(imported->limits.max_acceleration[0], 10.0)
            << row.joint_name;
        ++imported_acceleration_limit_count;
      }
      EXPECT_EQ(imported_acceleration_limit_count, 6U);
      expect_exact_retained_and_roboplan_mimic_semantics(model_map, *scene);

      const std::string master =
          compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                          "right_finger_joint");
      auto positions = scene->getCurrentJointPositions();
      const auto master_id = scene->getModel().getJointId(master);
      ASSERT_LT(master_id, scene->getModel().idx_qs.size());
      positions[scene->getModel().idx_qs[master_id]] = 0.004;
      scene->setJointPositions(positions);
      EXPECT_NEAR(mimic_expanded_position(*scene, master), 0.004, 1e-12);
    } catch (const std::exception &error) {
      ADD_FAILURE() << error.what() << "\n" << first.urdf_bytes();
      return;
    }
  }
  EXPECT_FALSE(std::filesystem::exists(first_stage_root));

  std::filesystem::path reopened_stage_root;
  Lite6SceneFacts reopened_scene_facts{};
  {
    auto stage =
        RetainedPlanningModelStage::create_verified(reopened, staging_parent);
    reopened_stage_root = stage.staging_root();
    EXPECT_NE(reopened_stage_root, first_stage_root);
    EXPECT_EQ(stage.loader_urdf_bytes(), first_loader_urdf);

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    const auto scene = make_roboplan_scene(RoboPlanSceneInput{
        "retained_lite6", std::string(stage.loader_urdf_bytes()),
        std::string(stage.srdf_bytes()), stage.package_paths(),
        retained_joint_limit_rows(reopened)});
    reopened_scene_facts = require_meaningful_lite6_scene(*scene);
  }
  EXPECT_EQ(reopened_scene_facts, first_scene_facts);
  EXPECT_FALSE(std::filesystem::exists(reopened_stage_root));

  // Removing a selected collision mesh must make a fresh real Scene load fail.
  // If Pinocchio/RoboPlan searched an ambient ROS/package location, this
  // could incorrectly keep succeeding.
  std::filesystem::path incomplete_stage_root;
  {
    auto stage =
        RetainedPlanningModelStage::create_verified(reopened, staging_parent);
    incomplete_stage_root = stage.staging_root();
    const auto selected = std::find_if(
        reopened.resources().begin(), reopened.resources().end(),
        [&stage](const deployment::VerifiedRetainedPlanningResource &resource) {
          const std::string package_uri =
              "package://" + std::string(resource.logical_uri().substr(
                                 std::string_view("asset://").size()));
          const auto urdf = stage.loader_urdf_bytes();
          std::size_t position = 0U;
          while ((position = urdf.find(package_uri, position)) !=
                 std::string_view::npos) {
            const auto collision_start = urdf.rfind("<collision", position);
            const auto collision_end = urdf.rfind("</collision>", position);
            if (collision_start != std::string_view::npos &&
                (collision_end == std::string_view::npos ||
                 collision_start > collision_end)) {
              return true;
            }
            position += package_uri.size();
          }
          return false;
        });
    ASSERT_NE(selected, reopened.resources().end());
    const std::filesystem::path selected_path =
        staged_resource_path(stage, selected->logical_uri());
    const std::filesystem::path hidden_path =
        selected_path.string() + ".missing";
    ASSERT_NO_THROW(std::filesystem::rename(selected_path, hidden_path));

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    EXPECT_ANY_THROW(static_cast<void>(make_roboplan_scene(RoboPlanSceneInput{
        "retained_lite6_missing_resource",
        std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
        stage.package_paths(), retained_joint_limit_rows(reopened)})));
  }
  EXPECT_FALSE(std::filesystem::exists(incomplete_stage_root));
  EXPECT_TRUE(std::filesystem::is_empty(staging_parent));
}

TEST(RetainedProductCrossConsumer,
     AggregateArmAndGripperStateProjectsExactUnitsMimicsAndFk) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const deployment::RetainedPlanningModelQuery planning_query{
      materialized.bundle->retained_bundle_id(),
      products.planning_collision_model.product_id};
  const deployment::RetainedModelMapQuery map_query{
      materialized.bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id};
  const auto retained = load_lite6_planning_model(package_root, planning_query);
  const auto model_map = load_lite6_model_map(package_root, map_query);

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id);
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_lite6_state_projection", std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(),
      retained_joint_limit_rows(retained)});
  auto scene_mutex = std::make_shared<std::mutex>();
  auto projector = RetainedKinematicStateProjector::create_verified(
      scene, scene_mutex, *materialized.bundle, model_map, retained,
      std::vector<core::RuntimeParticipantBinding>{arm_binding,
                                                   gripper_binding},
      1'000'000'000);

  const auto contexts = projector.query_contexts();
  ASSERT_EQ(contexts.size(), 2U);
  const auto arm_context =
      std::find_if(contexts.begin(), contexts.end(), [&](const auto &context) {
        return context.command_target_id == arm_target_id;
      });
  const auto gripper_context =
      std::find_if(contexts.begin(), contexts.end(), [&](const auto &context) {
        return context.command_target_id == gripper_target_id;
      });
  ASSERT_NE(arm_context, contexts.end());
  ASSERT_NE(gripper_context, contexts.end());
  ASSERT_EQ(arm_context->coordinate_kinds.size(), 6U);
  EXPECT_TRUE(std::all_of(
      arm_context->coordinate_kinds.begin(),
      arm_context->coordinate_kinds.end(),
      [](const auto kind) { return kind == core::CoordinateKind::kAngular; }));
  ASSERT_EQ(gripper_context->coordinate_kinds.size(), 1U);
  EXPECT_EQ(gripper_context->coordinate_kinds.front(),
            core::CoordinateKind::kLinear);

  const auto *arm_target = model_map.find_target(component_id, arm_target_id);
  ASSERT_NE(arm_target, nullptr);
  const auto arm_tcp = std::find_if(
      arm_target->frames.begin(), arm_target->frames.end(),
      [](const auto &frame) { return frame.frame_role_id.value() == "tcp"; });
  ASSERT_NE(arm_tcp, arm_target->frames.end());
  std::string arm_tcp_name;
  std::visit(
      [&](const auto &frame) {
        if constexpr (requires { frame.compiled_frame_name; }) {
          arm_tcp_name = frame.compiled_frame_name;
        }
      },
      arm_tcp->frame);
  ASSERT_FALSE(arm_tcp_name.empty());
  const Eigen::Matrix4d zero_tcp =
      scene->forwardKinematics(scene->getCurrentJointPositions(), arm_tcp_name,
                               model_map.cell_frame().compiled_frame_name);

  const std::vector<double> arm_positions{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  const auto arm_state =
      make_fixture_state(*arm_context, arm_positions, 1U, 100);
  const auto arm_result = projector.apply(arm_state, 100);
  ASSERT_TRUE(arm_result.applied())
      << (arm_result.error.has_value() ? arm_result.error->message : "");
  const Eigen::Matrix4d moved_tcp =
      scene->forwardKinematics(scene->getCurrentJointPositions(), arm_tcp_name,
                               model_map.cell_frame().compiled_frame_name);
  EXPECT_FALSE(moved_tcp.isApprox(zero_tcp, 1e-10));

  const auto gripper_state =
      make_fixture_state(*gripper_context, {0.004}, 1U, 110);
  const auto gripper_result = projector.apply(gripper_state, 110);
  ASSERT_TRUE(gripper_result.applied())
      << (gripper_result.error.has_value() ? gripper_result.error->message
                                           : "");
  const Eigen::Matrix4d after_gripper_tcp =
      scene->forwardKinematics(scene->getCurrentJointPositions(), arm_tcp_name,
                               model_map.cell_frame().compiled_frame_name);
  EXPECT_TRUE(after_gripper_tcp.isApprox(moved_tcp, 1e-12));

  const auto arm_read = projector.current_state(
      arm_context->control_binding_id, arm_context->control_binding_revision);
  ASSERT_TRUE(arm_read.ok()) << arm_read.error().message;
  EXPECT_EQ(arm_read.value().positions.size(), arm_positions.size());
  for (std::size_t index = 0U; index < arm_positions.size(); ++index) {
    const auto *angular = std::get_if<core::AngularPositionDTO>(
        &arm_read.value().positions[index]);
    ASSERT_NE(angular, nullptr);
    EXPECT_NEAR(angular->value_rad, arm_positions[index], 1e-12);
  }
  const auto gripper_read =
      projector.current_state(gripper_context->control_binding_id,
                              gripper_context->control_binding_revision);
  ASSERT_TRUE(gripper_read.ok()) << gripper_read.error().message;
  ASSERT_EQ(gripper_read.value().positions.size(), 1U);
  const auto *linear = std::get_if<core::LinearPositionDTO>(
      &gripper_read.value().positions.front());
  ASSERT_NE(linear, nullptr);
  EXPECT_NEAR(linear->value_m, 0.004, 1e-12);

  const auto coordinates = projector.model_coordinate_state();
  ASSERT_TRUE(coordinates.ok()) << coordinates.error().message;
  const std::string gripper_master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                      "right_finger_joint");
  const std::string gripper_follower = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kJoint, "left_finger_joint");
  for (const auto &name : {gripper_master, gripper_follower}) {
    const auto found =
        std::find_if(coordinates.value().begin(), coordinates.value().end(),
                     [&](const auto &coordinate) {
                       return coordinate.compiled_joint_name == name;
                     });
    ASSERT_NE(found, coordinates.value().end());
    const auto *position =
        std::get_if<core::LinearPositionDTO>(&found->position);
    ASSERT_NE(position, nullptr);
    EXPECT_NEAR(position->value_m, 0.004, 1e-12);
  }

  auto foreign = gripper_state;
  foreign.control_binding_revision = arm_context->control_binding_revision;
  const auto rejected = projector.apply(foreign, 120);
  EXPECT_EQ(rejected.status, RetainedStateProjectionStatus::kRejected);
  const auto unchanged =
      projector.current_state(gripper_context->control_binding_id,
                              gripper_context->control_binding_revision);
  ASSERT_TRUE(unchanged.ok()) << unchanged.error().message;
  EXPECT_EQ(unchanged.value(), gripper_read.value());
}

TEST(RetainedProductCrossConsumer,
     PneumaticCommandedFeedbackAliasUsesConstructionOnlyAbsence) {
  ScopedTemporaryRoot temporary;
  const auto package_root = temporary.path() / "retained-package";
  const auto staging_parent = temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  const auto materialized = deployment::test::
      materialize_lite6_retained_consumer_fixture_from(
          package_root,
          deployment::test::make_lite6_controllable_retention_fixture(
              false, false, 1U, true, true, {}, false, true, "resource", true,
              {}, {}, true, true, false, std::nullopt, false,
              deployment::test::kLite6ArmTargetId, std::nullopt, std::nullopt,
              {}, true, {}, "manyforge.provider.ros2_control", false, true,
              nullptr, nullptr, false, false, "pneumatic", 0.8, "commanded"));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto &products = bundle.value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, {bundle.retained_bundle_id(),
                     products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, {bundle.retained_bundle_id(),
                     products.symbol_role_maps.front().product_id});
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const core::RuntimeEpoch epoch("pneumatic-alias-frame-epoch");
  const auto arm = make_fixture_runtime_participant(
      bundle, model_map, component_id, arm_target_id, std::nullopt,
      std::nullopt, epoch);
  const auto command = make_fixture_runtime_participant(
      bundle, model_map, component_id, gripper_target_id, std::nullopt,
      std::nullopt, epoch, core::CapabilityId("tool.gripper.command"));
  const auto feedback = make_fixture_runtime_participant(
      bundle, model_map, component_id, gripper_target_id, std::nullopt,
      std::nullopt, epoch, core::CapabilityId("tool.gripper.feedback"));
  ASSERT_TRUE(command.value().control_path.has_value());
  ASSERT_TRUE(feedback.value().control_path.has_value());
  const auto runtime_id = command.value().control_path->provider_runtime_unit_id;
  EXPECT_EQ(feedback.value().control_path->provider_runtime_unit_id, runtime_id);
  const auto configuration = deployment::ros2_control_execution_runtime_contract::
      require_component_configuration(bundle, runtime_id, component_id);
  const auto &configured = require_fixture_one(
      configuration.gripper_target_configurations,
      [&](const auto &row) {
        return row.controllable_component_id == component_id &&
               row.command_target_id == gripper_target_id;
      }, "actual pneumatic command configuration");
  ASSERT_EQ(configured.actuation_kind, "pneumatic");
  ASSERT_EQ(configured.output_mode, core::GripperOutputMode::kCommanded);
  const auto command_context = core::make_observed_state_query_context(command, 0);
  const auto feedback_context = core::make_observed_state_query_context(feedback, 0);
  ASSERT_TRUE(command_context.ok()) << command_context.error().message;
  ASSERT_TRUE(feedback_context.ok()) << feedback_context.error().message;
  ASSERT_NE(command_context.value().control_binding_id,
            feedback_context.value().control_binding_id);
  EXPECT_EQ(command_context.value().resources, feedback_context.value().resources);
  EXPECT_EQ(command_context.value().joint_names, feedback_context.value().joint_names);
  EXPECT_EQ(command_context.value().coordinate_kinds,
            feedback_context.value().coordinate_kinds);
  EXPECT_EQ(command_context.value().control_route_id,
            feedback_context.value().control_route_id);

  auto stage = RetainedPlanningModelStage::create_verified(retained, staging_parent);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "pneumatic_alias_frame_state", std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(),
      retained_joint_limit_rows(retained)});
  auto projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, std::make_shared<std::mutex>(), bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{arm}, 0));
  auto arm_channel = std::make_shared<RetainedObservedStateChannel>(
      arm, projector, RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kCommandedState,
          "test/pneumatic-alias/actual-arm", 0, 1U});
  ASSERT_TRUE(arm_channel->initialize_from_projection(10).applied());
  const auto *arm_target = model_map.find_target(component_id, arm_target_id);
  ASSERT_NE(arm_target, nullptr);
  ASSERT_FALSE(arm_target->model_groups.empty());
  auto collision_model = ManyForgeCollisionEngine::load_shared_model(
      "pneumatic_alias_frame_state", stage.loader_urdf_path(), stage.srdf_path(),
      stage.package_paths(), arm_target->model_groups.front().compiled_group_name);
  const RetainedSceneFrameStateSourceInput arm_source{
      arm_channel->query_context(), arm_channel->source_kind(),
      std::shared_ptr<const core::IObservedStateProvider>(
          arm_channel, arm_channel->addressed_publication())};
  // No gripper channel, coordinate value or publication is supplied.
  const RetainedSceneFrameStateSourceInput absence{
      command_context.value(), core::ObservedStateSourceKind::kCommandedState,
      nullptr, RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic};

  // Reverse only traversal of genuine verified rows. All identities, retained
  // paths and bundle checks still come from the same sealed model map.
  struct ReversedMapTraversal final {
    const deployment::VerifiedRetainedModelMap &verified;
    std::vector<deployment::CompositionControlBindingCoordinateMap> rows;
    const auto &query() const { return verified.query(); }
    const auto &cell_frame() const { return verified.cell_frame(); }
    const auto &targets() const { return verified.targets(); }
    const auto &kinematic_frame_paths() const {
      return verified.kinematic_frame_paths();
    }
    const auto &control_bindings() const { return rows; }
    void require_same_bundle_as(
        const deployment::VerifiedRetainedPlanningModel &planning) const {
      verified.require_same_bundle_as(planning);
    }
  } reversed{model_map, model_map.control_bindings()};
  std::reverse(reversed.rows.begin(), reversed.rows.end());
  const auto command_map = std::find_if(
      reversed.rows.begin(), reversed.rows.end(), [&](const auto &row) {
        return row.control_binding_id == command_context.value().control_binding_id;
      });
  const auto feedback_map = std::find_if(
      reversed.rows.begin(), reversed.rows.end(), [&](const auto &row) {
        return row.control_binding_id == feedback_context.value().control_binding_id;
      });
  ASSERT_NE(command_map, reversed.rows.end());
  ASSERT_NE(feedback_map, reversed.rows.end());
  ASSERT_LT(feedback_map, command_map);
  const auto create = [&](const auto &traversal,
                          std::vector<RetainedSceneFrameStateSourceInput> sources) {
    return RetainedSceneFrameResolver::create_verified(
        collision_model, bundle, traversal, retained, runtime_id, epoch,
        std::move(sources));
  };
  auto resolver = create(model_map, {arm_source, absence});
  auto reversed_resolver = create(reversed, {arm_source, absence});
  ASSERT_NE(resolver, nullptr);
  ASSERT_NE(reversed_resolver, nullptr);
  const auto logical_scene = make_projection_runtime_shadow_snapshot(
      bundle.value().cell_graph.cell_id);
  const core::ProjectionLeaseBindingDTO lease{
      {bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
       logical_scene.shared()->revision},
      core::ProjectionLeaseId("pneumatic-alias-frame-lease")};
  const core::SceneFrameResolutionQueryDTO query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("tcp")}, lease};
  const auto scene_before = scene->getCurrentJointPositions();
  const auto resolved = resolver->Resolve(query, logical_scene.shared(), 10);
  const auto reversed_resolved = reversed_resolver->Resolve(
      query, logical_scene.shared(), 10);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  ASSERT_TRUE(reversed_resolved.ok()) << reversed_resolved.error().message;
  EXPECT_EQ(reversed_resolved.value(), resolved.value());
  for (const auto &state : resolved.value().kinematic_state_path_view.resource_states) {
    EXPECT_NE(state.resource_id, command.value().resources.front().resource_id);
  }
  EXPECT_TRUE(scene->getCurrentJointPositions().isApprox(scene_before));
  const std::vector<double> moved{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  ASSERT_TRUE(arm_channel->publish_state(
      moved, std::vector<double>(moved.size(), 0.0), 20, 20).applied());
  const auto after = resolver->Resolve(query, logical_scene.shared(), 20);
  const auto reversed_after = reversed_resolver->Resolve(
      query, logical_scene.shared(), 20);
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_TRUE(reversed_after.ok()) << reversed_after.error().message;
  EXPECT_EQ(reversed_after.value(), after.value());
  EXPECT_FALSE(after.value().frame_pose_in_cell == resolved.value().frame_pose_in_cell);
  EXPECT_TRUE(scene->getCurrentJointPositions().isApprox(scene_before));

  const auto rejects_both_orders = [&](const auto &sources) {
    EXPECT_THROW(static_cast<void>(create(model_map, sources)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(create(reversed, sources)), std::invalid_argument);
  };
  rejects_both_orders(std::vector<RetainedSceneFrameStateSourceInput>{arm_source});
  rejects_both_orders(std::vector<RetainedSceneFrameStateSourceInput>{absence});
  const auto reject_marker = [&](auto marker) {
    rejects_both_orders(std::vector<RetainedSceneFrameStateSourceInput>{
        arm_source, std::move(marker)});
  };
  auto invalid = absence;
  invalid.coordinate_authority =
      RetainedSceneFrameCoordinateAuthority::kRequiredPublication;
  reject_marker(invalid);
  invalid = absence;
  invalid.coordinate_authority = static_cast<RetainedSceneFrameCoordinateAuthority>(999);
  reject_marker(invalid);
  invalid = absence;
  invalid.context.provider_instance_id = core::ProviderInstanceId("foreign-owner");
  reject_marker(invalid);
  invalid = absence;
  invalid.context.runtime_epoch = core::RuntimeEpoch("foreign-epoch");
  reject_marker(invalid);
  invalid = absence;
  invalid.context.control_route_id = core::ControlRouteId("foreign-route");
  reject_marker(invalid);
  invalid = absence;
  invalid.context.controllable_component_id = core::ControllableComponentId("foreign-component");
  reject_marker(invalid);
  invalid = absence;
  invalid.context.resources.front() = core::ResourceId("foreign-resource");
  reject_marker(invalid);
  invalid = absence;
  invalid.context.control_binding_id = core::ControlBindingId("unused-command");
  reject_marker(invalid);
  invalid = absence;
  invalid.context = feedback_context.value();
  reject_marker(invalid);
  invalid = absence;
  invalid.context = arm_source.context;
  reject_marker(invalid);
  invalid = absence;
  invalid.provider = arm_source.provider;
  reject_marker(invalid);
  rejects_both_orders(std::vector<RetainedSceneFrameStateSourceInput>{
      arm_source, absence, absence});
  auto unused = absence;
  unused.context.control_binding_id = core::ControlBindingId("unused-command");
  rejects_both_orders(std::vector<RetainedSceneFrameStateSourceInput>{
      arm_source, absence, unused});
  EXPECT_THROW(static_cast<void>(RetainedSceneFrameResolver::create(
                   collision_model, RetainedSceneFrameResolverInput{
                       bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
                       model_map.cell_frame().cell_id,
                       model_map.cell_frame().compiled_frame_name,
                       model_map.kinematic_frame_paths(), {}, {}, {absence}, 10'000'000})),
               std::invalid_argument);
}

TEST(
    RetainedProductCrossConsumer,
    SceneFrameResolverReadsDirectOrExternalOwnerPublicationNotMutablePlannerState) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_motion_arm_gripper_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.symbol_role_maps.front().product_id});

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto &component = require_fixture_one(
      materialized.bundle->value().cell_graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id;
      },
      "scene-frame controllable component");
  const auto &arm_graph_binding = require_fixture_one(
      materialized.bundle->value().cell_graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id &&
               candidate.command_target_id == arm_target_id;
      },
      "scene-frame arm control binding");
  const auto &control_domain = require_fixture_one(
      materialized.bundle->value().cell_graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id ==
               arm_graph_binding.control_domain_id;
      },
      "scene-frame control domain");
  std::set<core::RuntimeUnitId> local_runtime_units(
      component.runtime_unit_ids.begin(), component.runtime_unit_ids.end());
  local_runtime_units.insert(control_domain.runtime_unit_ids.begin(),
                             control_domain.runtime_unit_ids.end());
  ASSERT_EQ(local_runtime_units.size(), 1U);
  const auto local_runtime_unit_id = *local_runtime_units.begin();
  const core::RuntimeEpoch local_runtime_epoch(
      "scene-frame-direct-owner-epoch");
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id,
      std::nullopt, std::nullopt, local_runtime_epoch);
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id,
      std::nullopt, std::nullopt, local_runtime_epoch);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_lite6_direct_frame_state",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), retained_joint_limit_rows(retained)});
  auto scene_mutex = std::make_shared<std::mutex>();
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *materialized.bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{arm_binding,
                                                       gripper_binding},
          0));
  auto arm_channel = std::make_shared<RetainedObservedStateChannel>(
      arm_binding, state_projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kCommandedState,
          "test/direct-owner/arm", 0, 1U});
  auto gripper_channel = std::make_shared<RetainedObservedStateChannel>(
      gripper_binding, state_projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kCommandedState,
          "test/direct-owner/gripper", 0, 1U});
  ASSERT_TRUE(arm_channel->initialize_from_projection(10).applied());
  ASSERT_TRUE(gripper_channel->initialize_from_projection(10).applied());

  const auto *arm_target = model_map.find_target(component_id, arm_target_id);
  ASSERT_NE(arm_target, nullptr);
  ASSERT_FALSE(arm_target->model_groups.empty());
  auto collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_direct_frame_state", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(),
      arm_target->model_groups.front().compiled_group_name);

  EXPECT_THROW(static_cast<void>(RetainedSceneFrameResolver::create_verified(
                   collision_model, *materialized.bundle, model_map, retained,
                   local_runtime_unit_id, local_runtime_epoch, {})),
               std::invalid_argument);

  const std::shared_ptr<const core::IObservedStateProvider> arm_provider(
      arm_channel, arm_channel->addressed_publication());
  const std::shared_ptr<const core::IObservedStateProvider> gripper_provider(
      gripper_channel, gripper_channel->addressed_publication());
  const std::vector<RetainedSceneFrameStateSourceInput> provided_state_sources{
      {arm_channel->query_context(), arm_channel->source_kind(), arm_provider},
      {gripper_channel->query_context(), gripper_channel->source_kind(), gripper_provider}};
  auto resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, *materialized.bundle, model_map, retained,
      local_runtime_unit_id, local_runtime_epoch, provided_state_sources);
  ASSERT_NE(resolver, nullptr);

  auto external_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, *materialized.bundle, model_map, retained,
      core::RuntimeUnitId("scene-frame-external-consumer"),
      core::RuntimeEpoch("scene-frame-external-receiver-epoch"),
      provided_state_sources);
  ASSERT_NE(external_resolver, nullptr);

  const auto logical_scene = make_projection_runtime_shadow_snapshot(
      materialized.bundle->value().cell_graph.cell_id);
  const core::ProjectionLeaseBindingDTO projection_binding{
      {materialized.bundle->retained_bundle_id(),
       materialized.bundle->value().cell_graph.graph_revision,
       logical_scene.shared()->revision},
      core::ProjectionLeaseId("direct-owner-frame-lease")};
  const core::SceneFrameResolutionQueryDTO query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("tcp")},
      projection_binding};
  const auto neutral = resolver->Resolve(query, logical_scene.shared(), 10);
  ASSERT_TRUE(neutral.ok()) << neutral.error().message;
  const auto *gripper_target =
      model_map.find_target(component_id, gripper_target_id);
  ASSERT_NE(gripper_target, nullptr);
  const auto gripper_tcp = std::find_if(
      gripper_target->frames.begin(), gripper_target->frames.end(),
      [](const auto &frame) { return frame.frame_role_id.value() == "tcp"; });
  ASSERT_NE(gripper_tcp, gripper_target->frames.end());
  const auto *gripper_model_tcp =
      std::get_if<deployment::CompositionComponentModelFrame>(
          &gripper_tcp->frame);
  ASSERT_NE(gripper_model_tcp, nullptr);
  const auto resolved_gripper_tcp = resolver->Resolve(
      core::SceneFrameResolutionQueryDTO{
          core::TargetCarrierSceneFrameRefDTO{component_id, gripper_target_id,
                                              core::FrameRoleId("tcp")},
          projection_binding},
      logical_scene.shared(), 10);
  ASSERT_TRUE(resolved_gripper_tcp.ok())
      << resolved_gripper_tcp.error().message;
  const auto *resolved_gripper_model_tcp =
      std::get_if<core::ResolvedComponentModelFrameDTO>(
          &resolved_gripper_tcp.value().resolved_frame);
  ASSERT_NE(resolved_gripper_model_tcp, nullptr);
  EXPECT_EQ(resolved_gripper_model_tcp->frame_role_id,
            gripper_model_tcp->frame_role_id);
  EXPECT_EQ(resolved_gripper_model_tcp->compiled_frame_name,
            gripper_model_tcp->compiled_frame_name);
  const auto external_neutral =
      external_resolver->Resolve(query, logical_scene.shared(), 10);
  ASSERT_TRUE(external_neutral.ok()) << external_neutral.error().message;
  EXPECT_EQ(external_neutral.value(), neutral.value());

  // The genuine ROS mixed fixture explicitly selects pneumatic actuation.
  // Its seeded gripper channel remains owned, but is not FK authority.
  ASSERT_TRUE(gripper_binding.value().control_path.has_value());
  const auto pneumatic_configuration = deployment::
      ros2_control_execution_runtime_contract::require_component_configuration(
          *materialized.bundle,
          gripper_binding.value().control_path->provider_runtime_unit_id,
          component_id);
  const auto &pneumatic_row = require_fixture_one(
      pneumatic_configuration.gripper_target_configurations,
      [&](const auto &row) {
        return row.controllable_component_id == component_id &&
               row.command_target_id == gripper_target_id;
      },
      "exact pneumatic scene-frame target configuration");
  ASSERT_EQ(pneumatic_row.actuation_kind, "pneumatic");
  ASSERT_EQ(pneumatic_row.output_mode, core::GripperOutputMode::kCommanded);
  const RetainedSceneFrameStateSourceInput unavailable_gripper{
      gripper_channel->query_context(), gripper_channel->source_kind(), nullptr,
      RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic};
  const auto with_gripper_source = [&](
      RetainedSceneFrameStateSourceInput source) {
    return RetainedSceneFrameResolver::create_verified(
        collision_model, *materialized.bundle, model_map, retained,
        local_runtime_unit_id, local_runtime_epoch,
        {provided_state_sources.front(), std::move(source)});
  };
  const Eigen::VectorXd scene_before_absence = scene->getCurrentJointPositions();
  auto pneumatic_resolver = with_gripper_source(unavailable_gripper);
  ASSERT_NE(pneumatic_resolver, nullptr);
  const auto pneumatic_tcp = pneumatic_resolver->Resolve(
      query, logical_scene.shared(), 10);
  ASSERT_TRUE(pneumatic_tcp.ok()) << pneumatic_tcp.error().message;
  EXPECT_EQ(pneumatic_tcp.value(), neutral.value());
  const auto pneumatic_gripper_tcp = pneumatic_resolver->Resolve(
      core::SceneFrameResolutionQueryDTO{
          core::TargetCarrierSceneFrameRefDTO{component_id, gripper_target_id,
                                              core::FrameRoleId("tcp")},
          projection_binding},
      logical_scene.shared(), 10);
  ASSERT_TRUE(pneumatic_gripper_tcp.ok())
      << pneumatic_gripper_tcp.error().message;
  EXPECT_EQ(pneumatic_gripper_tcp.value(), resolved_gripper_tcp.value());
  EXPECT_TRUE(scene->getCurrentJointPositions().isApprox(scene_before_absence));

  // Unmarked missing state still refuses, including legacy three-field inputs.
  const RetainedSceneFrameStateSourceInput default_missing_gripper{
      unavailable_gripper.context, unavailable_gripper.publication_source,
      nullptr};
  EXPECT_EQ(default_missing_gripper.coordinate_authority,
            RetainedSceneFrameCoordinateAuthority::kRequiredPublication);
  EXPECT_THROW(static_cast<void>(with_gripper_source(default_missing_gripper)),
               std::invalid_argument);
  auto invalid_absence = unavailable_gripper;
  invalid_absence.provider = gripper_provider;
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  invalid_absence = unavailable_gripper;
  invalid_absence.coordinate_authority =
      static_cast<RetainedSceneFrameCoordinateAuthority>(999);
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  invalid_absence = unavailable_gripper;
  invalid_absence.context.runtime_epoch = core::RuntimeEpoch("foreign-epoch");
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  invalid_absence = unavailable_gripper;
  invalid_absence.context.control_route_id = core::ControlRouteId("foreign-route");
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  invalid_absence = unavailable_gripper;
  invalid_absence.context.controllable_component_id =
      core::ControllableComponentId("foreign-component");
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  invalid_absence = unavailable_gripper;
  invalid_absence.publication_source = core::ObservedStateSourceKind::kJointState;
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  invalid_absence = unavailable_gripper;
  ASSERT_FALSE(invalid_absence.context.coordinate_kinds.empty());
  invalid_absence.context.coordinate_kinds.front() =
      unavailable_gripper.context.coordinate_kinds.front() ==
              core::CoordinateKind::kLinear
          ? core::CoordinateKind::kAngular
          : core::CoordinateKind::kLinear;
  ASSERT_NE(invalid_absence.context.coordinate_kinds,
            unavailable_gripper.context.coordinate_kinds);
  EXPECT_THROW(static_cast<void>(with_gripper_source(invalid_absence)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(RetainedSceneFrameResolver::create_verified(
                   collision_model, *materialized.bundle, model_map, retained,
                   local_runtime_unit_id, local_runtime_epoch,
                   {provided_state_sources.front(), unavailable_gripper,
                    unavailable_gripper})),
               std::invalid_argument);
  auto unavailable_arm = provided_state_sources.front();
  unavailable_arm.provider.reset();
  unavailable_arm.coordinate_authority =
      RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic;
  EXPECT_THROW(static_cast<void>(RetainedSceneFrameResolver::create_verified(
                   collision_model, *materialized.bundle, model_map, retained,
                   local_runtime_unit_id, local_runtime_epoch,
                   {unavailable_arm, unavailable_gripper})),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(RetainedSceneFrameResolver::create_verified(
                   collision_model, *materialized.bundle, model_map, retained,
                   local_runtime_unit_id, local_runtime_epoch,
                   {unavailable_gripper})),
               std::invalid_argument);

  const Eigen::VectorXd planner_state_before =
      scene->getCurrentJointPositions();
  const std::vector<double> moved_positions{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  const std::vector<double> zero_velocities(moved_positions.size(), 0.0);
  const auto published =
      arm_channel->publish_state(moved_positions, zero_velocities, 20, 20);
  ASSERT_TRUE(published.applied())
      << (published.error.has_value() ? published.error->message : "");
  EXPECT_TRUE(scene->getCurrentJointPositions().isApprox(planner_state_before));

  const auto moved = resolver->Resolve(query, logical_scene.shared(), 20);
  ASSERT_TRUE(moved.ok()) << moved.error().message;
  const auto pneumatic_moved =
      pneumatic_resolver->Resolve(query, logical_scene.shared(), 20);
  ASSERT_TRUE(pneumatic_moved.ok()) << pneumatic_moved.error().message;
  EXPECT_EQ(pneumatic_moved.value(), moved.value());
  const auto external_moved =
      external_resolver->Resolve(query, logical_scene.shared(), 20);
  ASSERT_TRUE(external_moved.ok()) << external_moved.error().message;
  EXPECT_EQ(external_moved.value(), moved.value());
  EXPECT_FALSE(moved.value().frame_pose_in_cell ==
               neutral.value().frame_pose_in_cell);
  EXPECT_NE(moved.value().resolution_evidence_digest,
            neutral.value().resolution_evidence_digest);
  ASSERT_EQ(moved.value().kinematic_state_path_view.model_states.size(), 1U);
  EXPECT_FALSE(moved.value().kinematic_state_path_view.resource_states.empty());

  const auto exact_binding = make_fixture_execution_binding(arm_binding);
  auto start_publication = arm_channel->addressed_publication()->publication(
      exact_binding.control_binding_id, exact_binding.control_binding_revision);
  ASSERT_TRUE(start_publication.ok()) << start_publication.error().message;
  const auto &start_snapshot = start_publication.value().snapshot;
  core::ControlledStateDTO controlled_start{
      exact_binding.controllable_component_id,
      exact_binding.command_target_id,
      exact_binding.control_domain_id,
      exact_binding.control_domain_revision,
      exact_binding.control_binding_id,
      exact_binding.control_binding_revision,
      exact_binding.controller_topology_id,
      exact_binding.topology_revision,
      exact_binding.control_route_id,
      exact_binding.active_control_configuration_id,
      exact_binding.active_control_configuration_generation,
      start_snapshot.resources,
      start_snapshot.joint_names,
      start_snapshot.positions,
      start_snapshot.velocities};
  auto predicted_terminal = controlled_start;
  ASSERT_FALSE(predicted_terminal.positions.empty());
  predicted_terminal.positions.front() = core::AngularPositionDTO{0.35};
  const core::CandidateSceneFrameResolutionQueryDTO candidate_query{
      query.selector, projection_binding, predicted_terminal};
  const auto predicted =
      resolver->ResolveCandidate(candidate_query, logical_scene.shared(), 20);
  ASSERT_TRUE(predicted.ok()) << predicted.error().message;
  ASSERT_TRUE(predicted.value().predicted_state_digest.has_value());
  EXPECT_EQ(*predicted.value().predicted_state_digest,
            core::ComputeCandidateStateEvidenceDigest(predicted_terminal));
  EXPECT_EQ(predicted.value().kinematic_state_path_view.view_digest,
            moved.value().kinematic_state_path_view.view_digest);
  EXPECT_FALSE(predicted.value().frame_pose_in_cell ==
               moved.value().frame_pose_in_cell);
  const auto unchanged_after_candidate =
      resolver->Resolve(query, logical_scene.shared(), 20);
  ASSERT_TRUE(unchanged_after_candidate.ok())
      << unchanged_after_candidate.error().message;
  EXPECT_EQ(unchanged_after_candidate.value(), moved.value());

  auto foreign_predicted = predicted_terminal;
  foreign_predicted.control_binding_id =
      core::ControlBindingId("foreign-binding");
  const auto rejected_foreign = resolver->ResolveCandidate(
      core::CandidateSceneFrameResolutionQueryDTO{
          query.selector, projection_binding, foreign_predicted},
      logical_scene.shared(), 20);
  ASSERT_FALSE(rejected_foreign.ok());
  EXPECT_EQ(rejected_foreign.error().details.at("reason"),
            "candidate_state_binding_unavailable");
  // The predicted state is a request input, not the selected frame: the
  // failure names no reference (spec 210 §4.4).
  EXPECT_EQ(rejected_foreign.error().details.count("reference_type"), 0U);
  EXPECT_EQ(rejected_foreign.error().details.count("reference_id"), 0U);
  auto reference_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 2U,
      core::SceneResourceId("reference-object"), 0.4, 0.05);
  auto reference_scene_ptr =
      std::make_shared<const core::LogicalSceneSnapshotDTO>(reference_scene);
  const core::ProjectionLeaseBindingDTO reference_projection{
      {materialized.bundle->retained_bundle_id(),
       materialized.bundle->value().cell_graph.graph_revision,
       reference_scene.revision},
      core::ProjectionLeaseId("program-reference-resolution-lease")};
  const core::ReferenceDTO tcp_reference(core::ReferenceDTO::Type::kFrameRole,
                                         "tcp");
  ASSERT_FALSE(exact_binding.collision_body_role_bindings.empty());
  const auto &body_role = exact_binding.collision_body_role_bindings.front();
  ASSERT_GT(body_role.compiled_collision_body_names.size(), 1U);
  const core::ReferenceDTO body_reference(
      core::ReferenceDTO::Type::kComponentBodyRole,
      body_role.collision_body_role_id.value());
  const core::ReferenceDTO scene_reference(
      core::ReferenceDTO::Type::kSceneResource, "reference-object");
  const core::ReferenceDTO runtime_reference(
      core::ReferenceDTO::Type::kRuntimeObject, "selected-object");
  const core::Pose3DTO inline_pose{{0.1, -0.2, 0.3}, {0.0, 0.0, 0.0, 1.0}};
  const core::ReferenceDTO inline_reference(inline_pose);
  const std::vector<double> later_positions{0.1, -0.6, 0.5, 0.2, 1.2, -0.1};
  ASSERT_TRUE(
      arm_channel->publish_state(later_positions, zero_velocities, 30, 30)
          .applied());
  auto newer_publication = arm_channel->addressed_publication()->publication(
      exact_binding.control_binding_id, exact_binding.control_binding_revision);
  ASSERT_TRUE(newer_publication.ok()) << newer_publication.error().message;
  EXPECT_GT(newer_publication.value().publication_sequence,
            start_publication.value().publication_sequence);
  EXPECT_EQ(newer_publication.value().snapshot.received_timestamp_ns, 30);
  core::ProgramFrameResolutionCaptureDTO capture{
      exact_binding,
      controlled_start,
      start_publication.value(),
      reference_projection,
      reference_scene_ptr,
      {{core::RuntimeObjectId("selected-object"),
        core::SceneResourceId("reference-object")}},
      {tcp_reference, body_reference, scene_reference, runtime_reference,
       inline_reference},
      20};
  auto captured = resolver->capture_program_frame_resolution_context(capture);
  ASSERT_TRUE(captured.ok()) << captured.error().message;
  ASSERT_TRUE(captured.value().valid());
  const auto captured_view = std::find_if(
      captured.value().kinematic_state_path_views.begin(),
      captured.value().kinematic_state_path_views.end(),
      [](const auto &view) { return !view.resource_states.empty(); });
  ASSERT_NE(captured_view, captured.value().kinematic_state_path_views.end());
  for (const auto &view : captured.value().kinematic_state_path_views) {
    for (const auto &resource : view.resource_states) {
      EXPECT_EQ(resource.publication_received_timestamp_ns, 20);
      EXPECT_EQ(resource.publication_sequence,
                start_publication.value().publication_sequence);
    }
  }

  const auto resolved_tcp =
      resolver->resolve_reference_pose(tcp_reference, captured.value());
  const auto resolved_body =
      resolver->resolve_reference_pose(body_reference, captured.value());
  const auto resolved_scene =
      resolver->resolve_reference_pose(scene_reference, captured.value());
  const auto resolved_runtime =
      resolver->resolve_reference_pose(runtime_reference, captured.value());
  const auto resolved_inline =
      resolver->resolve_reference_pose(inline_reference, captured.value());
  ASSERT_TRUE(resolved_tcp.ok()) << resolved_tcp.error().message;
  ASSERT_FALSE(resolved_body.ok());
  EXPECT_EQ(resolved_body.error().details.at("reason"),
            "program_component_body_role_spatially_ambiguous");
  ASSERT_TRUE(resolved_scene.ok()) << resolved_scene.error().message;
  ASSERT_TRUE(resolved_runtime.ok()) << resolved_runtime.error().message;
  ASSERT_TRUE(resolved_inline.ok()) << resolved_inline.error().message;
  EXPECT_EQ(resolved_runtime.value(), resolved_scene.value());
  EXPECT_EQ(resolved_inline.value(), inline_pose);

  const core::SceneFrameResolutionQueryDTO base_query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("base")},
      reference_projection};
  const core::SceneFrameResolutionQueryDTO tcp_query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("tcp")},
      reference_projection};
  const auto base_pose = resolver->Resolve(base_query, reference_scene_ptr, 20);
  const auto tcp_pose = resolver->Resolve(tcp_query, reference_scene_ptr, 20);
  ASSERT_TRUE(base_pose.ok()) << base_pose.error().message;
  ASSERT_TRUE(tcp_pose.ok()) << tcp_pose.error().message;
  const auto expected_tcp = core::multiply_poses(
      core::inverse_pose(base_pose.value().frame_pose_in_cell),
      tcp_pose.value().frame_pose_in_cell);
  const auto expected_scene = core::multiply_poses(
      core::inverse_pose(base_pose.value().frame_pose_in_cell),
      std::get<core::CellFixedSceneObjectPlacementDTO>(
          reference_scene.objects.front().placement)
          .pose_in_cell);
  const auto expect_pose_near = [](const core::Pose3DTO &actual,
                                   const core::Pose3DTO &expected) {
    EXPECT_NEAR(actual.position_m.x_m, expected.position_m.x_m, 1e-12);
    EXPECT_NEAR(actual.position_m.y_m, expected.position_m.y_m, 1e-12);
    EXPECT_NEAR(actual.position_m.z_m, expected.position_m.z_m, 1e-12);
    EXPECT_NEAR(actual.orientation.x, expected.orientation.x, 1e-12);
    EXPECT_NEAR(actual.orientation.y, expected.orientation.y, 1e-12);
    EXPECT_NEAR(actual.orientation.z, expected.orientation.z, 1e-12);
    EXPECT_NEAR(actual.orientation.w, expected.orientation.w, 1e-12);
  };
  expect_pose_near(resolved_tcp.value(), expected_tcp);
  expect_pose_near(resolved_scene.value(), expected_scene);

  const auto frozen_tcp =
      resolver->resolve_reference_pose(tcp_reference, captured.value());
  ASSERT_TRUE(frozen_tcp.ok()) << frozen_tcp.error().message;
  EXPECT_EQ(frozen_tcp.value(), resolved_tcp.value());

  auto conflicting_capture = capture;
  conflicting_capture.observed_start_publication = newer_publication.value();
  conflicting_capture.captured_at_ns = 30;
  const auto conflict = resolver->capture_program_frame_resolution_context(
      std::move(conflicting_capture));
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"),
            "program_reference_start_state_mismatch");
}

TEST(RetainedProductCrossConsumer,
     CellSceneFrameResolverUsesEveryComponentStateSource) {
  ScopedTemporaryRoot temporary;
  const auto package_root = temporary.path() / "retained-package";
  const auto staging_parent = temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  constexpr std::string_view cell_id = "i6_cell_frame_resolver_cell";
  const auto materialized = deployment::test::
      materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto &products = bundle.value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root,
      {bundle.retained_bundle_id(),
       products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root,
      {bundle.retained_bundle_id(),
       products.symbol_role_maps.front().product_id});

  const core::ControllableComponentId root_component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::ControllableComponentId peer_component_id(
      deployment::test::lite6_controllable_fixture_detail::indexed_id(
          deployment::test::kLite6ControllableComponentId, 1U));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::RuntimeEpoch root_epoch("cell-frame-root-owner-epoch");
  const auto root_binding = make_fixture_runtime_participant(
      bundle, model_map, root_component_id, arm_target_id, std::nullopt,
      std::nullopt, root_epoch);
  const auto peer_binding = make_fixture_runtime_participant(
      bundle, model_map, peer_component_id, arm_target_id, std::nullopt,
      std::nullopt, root_epoch);

  const auto &root_component = require_fixture_one(
      bundle.value().cell_graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == root_component_id;
      },
      "root scene-frame component");
  const auto &root_graph_binding = require_fixture_one(
      bundle.value().cell_graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == root_component_id &&
               candidate.command_target_id == arm_target_id;
      },
      "root scene-frame control binding");
  const auto &root_domain = require_fixture_one(
      bundle.value().cell_graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id ==
               root_graph_binding.control_domain_id;
      },
      "root scene-frame control domain");
  std::set<core::RuntimeUnitId> root_runtime_units(
      root_component.runtime_unit_ids.begin(),
      root_component.runtime_unit_ids.end());
  root_runtime_units.insert(root_domain.runtime_unit_ids.begin(),
                            root_domain.runtime_unit_ids.end());
  ASSERT_EQ(root_runtime_units.size(), 1U);
  const auto root_runtime_unit_id = *root_runtime_units.begin();

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_two_lite6_cell_frame_state",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), retained_joint_limit_rows(retained)});
  auto scene_mutex = std::make_shared<std::mutex>();
  auto projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{root_binding,
                                                       peer_binding},
          0));
  auto make_arm_channel = [&](const core::RuntimeParticipantBinding &binding,
                              std::string_view channel_name) {
    return std::make_shared<RetainedObservedStateChannel>(
        binding, projector,
        RetainedObservedStateChannelConfig{
            core::ObservedStateSourceKind::kCommandedState,
            "test/cell-frame/" + std::string(channel_name), 0, 1U});
  };
  auto root_channel = make_arm_channel(root_binding, "root-arm");
  auto peer_channel = make_arm_channel(peer_binding, "peer-arm");
  ASSERT_TRUE(root_channel->initialize_from_projection(10).applied());
  ASSERT_TRUE(peer_channel->initialize_from_projection(10).applied());

  const auto *root_target = model_map.find_target(root_component_id,
                                                  arm_target_id);
  ASSERT_NE(root_target, nullptr);
  ASSERT_FALSE(root_target->model_groups.empty());
  auto collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_two_lite6_cell_frame_state", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(),
      root_target->model_groups.front().compiled_group_name);
  const auto source_for = [](const auto &channel) {
    return RetainedSceneFrameStateSourceInput{
        channel->query_context(), channel->source_kind(),
        std::shared_ptr<const core::IObservedStateProvider>(
            channel, channel->addressed_publication())};
  };
  const auto root_source = source_for(root_channel);
  const auto peer_source = source_for(peer_channel);
  const auto root_only_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, bundle, model_map, retained, root_runtime_unit_id,
      root_epoch, {root_source}, root_component_id);
  const auto cell_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, bundle, model_map, retained, root_runtime_unit_id,
      root_epoch, {root_source, peer_source});

  const auto logical_scene = make_projection_runtime_shadow_snapshot(
      bundle.value().cell_graph.cell_id);
  const core::ProjectionLeaseBindingDTO projection_binding{
      {bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
       logical_scene.shared()->revision},
      core::ProjectionLeaseId("cell-frame-resolver-lease")};
  const auto query_for = [&](const core::ControllableComponentId
                                 &component_id) {
    return core::SceneFrameResolutionQueryDTO{
        core::TargetCarrierSceneFrameRefDTO{
            component_id, arm_target_id, core::FrameRoleId("tcp")},
        projection_binding};
  };
  for (const auto &component_id : {root_component_id, peer_component_id}) {
    const auto resolved = cell_resolver->Resolve(
        query_for(component_id), logical_scene.shared(), 10);
    ASSERT_TRUE(resolved.ok()) << resolved.error().message;
    const auto *model_frame =
        std::get_if<core::ResolvedComponentModelFrameDTO>(
            &resolved.value().resolved_frame);
    ASSERT_NE(model_frame, nullptr);
    EXPECT_EQ(model_frame->controllable_component_id, component_id);
  }

  const auto root_frame = root_only_resolver->Resolve(
      query_for(root_component_id), logical_scene.shared(), 10);
  ASSERT_TRUE(root_frame.ok()) << root_frame.error().message;
  const auto peer_without_source = root_only_resolver->Resolve(
      query_for(peer_component_id), logical_scene.shared(), 10);
  ASSERT_FALSE(peer_without_source.ok());
  EXPECT_EQ(peer_without_source.error().details.at("reason"),
            "kinematic_state_source_unavailable");
}

TEST(RetainedProductCrossConsumer,
     CellResolverResolvesPeerRobotFramesAndPeerCarriedObjectsAtAdmission) {
  ScopedTemporaryRoot temporary;
  const auto package_root = temporary.path() / "retained-package";
  const auto staging_parent = temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  constexpr std::string_view cell_id = "i6_peer_reference_cell";
  const auto materialized = deployment::test::
      materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto &products = bundle.value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root,
      {bundle.retained_bundle_id(),
       products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root,
      {bundle.retained_bundle_id(),
       products.symbol_role_maps.front().product_id});

  const core::ControllableComponentId root_component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::ControllableComponentId peer_component_id(
      deployment::test::lite6_controllable_fixture_detail::indexed_id(
          deployment::test::kLite6ControllableComponentId, 1U));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::RuntimeEpoch root_epoch("peer-reference-root-owner-epoch");
  const auto root_binding = make_fixture_runtime_participant(
      bundle, model_map, root_component_id, arm_target_id, std::nullopt,
      std::nullopt, root_epoch);
  const auto peer_binding = make_fixture_runtime_participant(
      bundle, model_map, peer_component_id, arm_target_id, std::nullopt,
      std::nullopt, root_epoch);
  const auto &root_component = require_fixture_one(
      bundle.value().cell_graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == root_component_id;
      },
      "root peer-reference component");
  const auto &root_graph_binding = require_fixture_one(
      bundle.value().cell_graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == root_component_id &&
               candidate.command_target_id == arm_target_id;
      },
      "root peer-reference control binding");
  const auto &root_domain = require_fixture_one(
      bundle.value().cell_graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id ==
               root_graph_binding.control_domain_id;
      },
      "root peer-reference control domain");
  std::set<core::RuntimeUnitId> root_runtime_units(
      root_component.runtime_unit_ids.begin(),
      root_component.runtime_unit_ids.end());
  root_runtime_units.insert(root_domain.runtime_unit_ids.begin(),
                            root_domain.runtime_unit_ids.end());
  ASSERT_EQ(root_runtime_units.size(), 1U);
  const auto root_runtime_unit_id = *root_runtime_units.begin();

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_two_lite6_peer_reference_state",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), retained_joint_limit_rows(retained)});
  auto scene_mutex = std::make_shared<std::mutex>();
  auto projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{root_binding}, 0));
  auto root_channel = std::make_shared<RetainedObservedStateChannel>(
      root_binding, projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kCommandedState,
          "test/peer-reference/root-arm", 0, 1U});
  // The peer source is owned by its own projector with a positive freshness
  // budget so admission-time staleness of the other robot is observable.
  constexpr std::int64_t kPeerFreshnessBudgetNs = 5;
  auto peer_scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_two_lite6_peer_reference_peer_state",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), retained_joint_limit_rows(retained)});
  auto peer_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          peer_scene, std::make_shared<std::mutex>(), bundle, model_map,
          retained, std::vector<core::RuntimeParticipantBinding>{peer_binding},
          kPeerFreshnessBudgetNs));
  auto peer_channel = std::make_shared<RetainedObservedStateChannel>(
      peer_binding, peer_projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kCommandedState,
          "test/peer-reference/peer-arm", kPeerFreshnessBudgetNs, 1U});
  ASSERT_TRUE(root_channel->initialize_from_projection(10).applied());
  ASSERT_TRUE(peer_channel->initialize_from_projection(10).applied());

  const auto *root_target =
      model_map.find_target(root_component_id, arm_target_id);
  ASSERT_NE(root_target, nullptr);
  ASSERT_FALSE(root_target->model_groups.empty());
  auto collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_two_lite6_peer_reference_state", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(),
      root_target->model_groups.front().compiled_group_name);
  const auto source_for = [](const auto &channel) {
    return RetainedSceneFrameStateSourceInput{
        channel->query_context(), channel->source_kind(),
        std::shared_ptr<const core::IObservedStateProvider>(
            channel, channel->addressed_publication())};
  };
  const auto root_only_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, bundle, model_map, retained, root_runtime_unit_id,
      root_epoch, {source_for(root_channel)}, root_component_id);
  const auto cell_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, bundle, model_map, retained, root_runtime_unit_id,
      root_epoch, {source_for(root_channel), source_for(peer_channel)});

  // A Scene object carried by the peer TCP, expressed through its carrier.
  const auto *peer_target =
      model_map.find_target(peer_component_id, arm_target_id);
  ASSERT_NE(peer_target, nullptr);
  const auto peer_tcp_row = std::find_if(
      peer_target->frames.begin(), peer_target->frames.end(),
      [](const auto &frame) { return frame.frame_role_id.value() == "tcp"; });
  ASSERT_NE(peer_tcp_row, peer_target->frames.end());
  const auto *peer_tcp_model =
      std::get_if<deployment::CompositionComponentModelFrame>(
          &peer_tcp_row->frame);
  ASSERT_NE(peer_tcp_model, nullptr);
  const core::CarriedObjectAttachmentId attachment_id("peer-carried-attachment");
  const core::CarriedObjectAttachmentRevision attachment_revision{
      std::string(kZeroDigest)};
  const core::Pose3DTO pose_in_carrier{{0.0, 0.0, 0.1}, {0.0, 0.0, 0.0, 1.0}};
  auto logical_scene = *make_projection_runtime_shadow_snapshot(
                            bundle.value().cell_graph.cell_id)
                            .shared();
  logical_scene.revision.revision_id = 2U;
  logical_scene.transaction_sequence = 1U;
  logical_scene.topology_epoch = 1U;
  logical_scene.objects.push_back(core::SceneObjectSnapshotDTO{
      core::SceneResourceId("peer-carried"),
      {core::BoxShapeDTO{{0.02, 0.02, 0.02}}},
      core::CarriedSceneObjectPlacementDTO{attachment_id, attachment_revision},
      std::nullopt});
  logical_scene.carried_object_attachments.push_back(
      core::CarriedObjectAttachmentDTO{
          attachment_id,
          attachment_revision,
          core::SceneResourceId("peer-carried"),
          peer_component_id,
          arm_target_id,
          core::ComponentSceneFrameRefDTO{peer_component_id,
                                          peer_tcp_model->model_instance_id,
                                          peer_tcp_model->frame_role_id},
          pose_in_carrier,
          {}});
  const auto sealed_attachment_revision =
      core::logical_scene_identity::ComputeCarriedObjectAttachmentRevision(
          logical_scene.carried_object_attachments.front());
  logical_scene.carried_object_attachments.front()
      .carried_object_attachment_revision = sealed_attachment_revision;
  std::get<core::CarriedSceneObjectPlacementDTO>(
      logical_scene.objects.front().placement)
      .carried_object_attachment_revision = sealed_attachment_revision;
  logical_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          logical_scene);
  ASSERT_TRUE(logical_scene.locally_valid());
  const auto scene_ptr =
      std::make_shared<const core::LogicalSceneSnapshotDTO>(logical_scene);
  const core::ProjectionLeaseBindingDTO projection_binding{
      {bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
       logical_scene.revision},
      core::ProjectionLeaseId("peer-reference-lease")};

  // Both robots move away from the neutral projection before admission.
  const std::vector<double> zero_velocities(6U, 0.0);
  const std::vector<double> root_positions{0.2, -0.4, 0.6, 0.0, 1.0, 0.0};
  const std::vector<double> peer_positions{-0.3, -0.785, 0.785,
                                           0.1,  1.57,   0.2};
  ASSERT_TRUE(
      root_channel->publish_state(root_positions, zero_velocities, 20, 20)
          .applied());
  ASSERT_TRUE(
      peer_channel->publish_state(peer_positions, zero_velocities, 20, 20)
          .applied());

  const auto exact_binding = make_fixture_execution_binding(root_binding);
  const auto controlled_start_at = [&](const std::int64_t) {
    auto publication = root_channel->addressed_publication()->publication(
        exact_binding.control_binding_id,
        exact_binding.control_binding_revision);
    EXPECT_TRUE(publication.ok());
    const auto &snapshot = publication.value().snapshot;
    return std::pair{
        core::ControlledStateDTO{
            exact_binding.controllable_component_id,
            exact_binding.command_target_id,
            exact_binding.control_domain_id,
            exact_binding.control_domain_revision,
            exact_binding.control_binding_id,
            exact_binding.control_binding_revision,
            exact_binding.controller_topology_id,
            exact_binding.topology_revision,
            exact_binding.control_route_id,
            exact_binding.active_control_configuration_id,
            exact_binding.active_control_configuration_generation,
            snapshot.resources, snapshot.joint_names, snapshot.positions,
            snapshot.velocities},
        publication.value()};
  };
  const core::ReferenceTargetDTO peer_robot{core::TargetRoleId("peer_arm"),
                                            peer_component_id, arm_target_id};
  const core::ReferenceDTO peer_tcp(core::FrameRoleId("tcp"), peer_robot);
  const core::ReferenceDTO own_tcp(core::ReferenceDTO::Type::kFrameRole, "tcp");
  const core::ReferenceDTO carried(core::ReferenceDTO::Type::kSceneResource,
                                   "peer-carried");
  const core::ReferenceDTO cell(core::ReferenceDTO::Type::kCellFrame, "");
  const auto capture_with = [&](const RetainedSceneFrameResolver &resolver,
                                std::vector<core::ReferenceDTO> references,
                                const std::int64_t captured_at_ns) {
    auto [start, publication] = controlled_start_at(captured_at_ns);
    return resolver.capture_program_frame_resolution_context(
        core::ProgramFrameResolutionCaptureDTO{
            exact_binding, std::move(start), std::move(publication),
            projection_binding, scene_ptr, {}, std::move(references),
            captured_at_ns});
  };

  auto captured = capture_with(*cell_resolver,
                               {own_tcp, peer_tcp, carried, cell}, 20);
  ASSERT_TRUE(captured.ok()) << captured.error().message;
  const auto resolve_cell_frame = [&](const core::ControllableComponentId &id,
                                      const char *role,
                                      const std::int64_t now_ns) {
    auto resolved = cell_resolver->Resolve(
        core::SceneFrameResolutionQueryDTO{
            core::TargetCarrierSceneFrameRefDTO{id, arm_target_id,
                                                core::FrameRoleId(role)},
            projection_binding},
        scene_ptr, now_ns);
    EXPECT_TRUE(resolved.ok()) << resolved.error().message;
    return resolved.value().frame_pose_in_cell;
  };
  const auto root_base = resolve_cell_frame(root_component_id, "base", 20);
  const auto peer_tcp_in_cell = resolve_cell_frame(peer_component_id, "tcp", 20);
  const auto in_root_base = [&](const core::Pose3DTO &pose_in_cell) {
    return core::multiply_poses(core::inverse_pose(root_base), pose_in_cell);
  };
  const auto expect_pose_near = [](const core::Pose3DTO &actual,
                                   const core::Pose3DTO &expected) {
    EXPECT_NEAR(actual.position_m.x_m, expected.position_m.x_m, 1e-12);
    EXPECT_NEAR(actual.position_m.y_m, expected.position_m.y_m, 1e-12);
    EXPECT_NEAR(actual.position_m.z_m, expected.position_m.z_m, 1e-12);
    EXPECT_NEAR(std::abs(actual.orientation.x * expected.orientation.x +
                         actual.orientation.y * expected.orientation.y +
                         actual.orientation.z * expected.orientation.z +
                         actual.orientation.w * expected.orientation.w),
                1.0, 1e-12);
  };

  const auto resolved_peer =
      cell_resolver->resolve_reference_pose(peer_tcp, captured.value());
  const auto resolved_own =
      cell_resolver->resolve_reference_pose(own_tcp, captured.value());
  const auto resolved_carried =
      cell_resolver->resolve_reference_pose(carried, captured.value());
  const auto resolved_cell =
      cell_resolver->resolve_reference_pose(cell, captured.value());
  ASSERT_TRUE(resolved_peer.ok()) << resolved_peer.error().message;
  ASSERT_TRUE(resolved_own.ok()) << resolved_own.error().message;
  ASSERT_TRUE(resolved_carried.ok()) << resolved_carried.error().message;
  ASSERT_TRUE(resolved_cell.ok()) << resolved_cell.error().message;
  expect_pose_near(resolved_peer.value(), in_root_base(peer_tcp_in_cell));
  expect_pose_near(resolved_carried.value(),
                   in_root_base(core::multiply_poses(peer_tcp_in_cell,
                                                     pose_in_carrier)));
  expect_pose_near(resolved_cell.value(), core::inverse_pose(root_base));
  // The own-robot "tcp" and the peer "tcp" are different frames.
  EXPECT_GT(std::abs(resolved_own.value().position_m.x_m -
                     resolved_peer.value().position_m.x_m) +
                std::abs(resolved_own.value().position_m.y_m -
                         resolved_peer.value().position_m.y_m),
            1e-3);

  // Admission sampling: later peer motion does not change the captured
  // answer, while a new admission samples the new peer state.
  const std::vector<double> later_peer_positions{0.4, -0.2, 0.3,
                                                 -0.1, 0.9, 0.0};
  ASSERT_TRUE(peer_channel
                  ->publish_state(later_peer_positions, zero_velocities, 30,
                                  30)
                  .applied());
  const auto frozen_peer =
      cell_resolver->resolve_reference_pose(peer_tcp, captured.value());
  ASSERT_TRUE(frozen_peer.ok()) << frozen_peer.error().message;
  EXPECT_EQ(frozen_peer.value(), resolved_peer.value());
  const auto frozen_carried =
      cell_resolver->resolve_reference_pose(carried, captured.value());
  ASSERT_TRUE(frozen_carried.ok()) << frozen_carried.error().message;
  EXPECT_EQ(frozen_carried.value(), resolved_carried.value());
  auto recaptured = capture_with(*cell_resolver, {peer_tcp}, 30);
  ASSERT_TRUE(recaptured.ok()) << recaptured.error().message;
  const auto moved_peer =
      cell_resolver->resolve_reference_pose(peer_tcp, recaptured.value());
  ASSERT_TRUE(moved_peer.ok()) << moved_peer.error().message;
  EXPECT_FALSE(moved_peer.value() == resolved_peer.value());

  // Stale other-robot state fails at admission, before any motion exists.
  const auto stale = capture_with(*cell_resolver, {peer_tcp}, 40);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"),
            "observed_state_unavailable_at_evaluation_time");

  // A robot without a state source in this resolver is refused by name.
  const auto not_local = capture_with(*root_only_resolver, {peer_tcp}, 30);
  ASSERT_FALSE(not_local.ok());
  EXPECT_EQ(not_local.error().details.at("reason"),
            "program_reference_robot_not_local");
  // So is an object carried by such a robot.
  const auto carried_not_local =
      capture_with(*root_only_resolver, {carried}, 30);
  ASSERT_FALSE(carried_not_local.ok());
  EXPECT_EQ(carried_not_local.error().details.at("reason"),
            "program_reference_robot_not_local");
  const auto missing_frame = capture_with(
      *cell_resolver,
      {core::ReferenceDTO(core::FrameRoleId("no-such-frame"), peer_robot)}, 30);
  ASSERT_FALSE(missing_frame.ok());
  EXPECT_EQ(missing_frame.error().details.at("reason"),
            "program_reference_robot_frame_missing");

  // Every missing or ambiguous reference names the reference that failed
  // (spec 210 §5.3); a robot-qualified frame also keeps its target role.
  const auto expect_named = [](const core::ErrorDTO &error, const char *code,
                               const char *reference_type,
                               const char *reference_id,
                               const char *target_role_id) {
    EXPECT_EQ(error.code, code);
    EXPECT_EQ(error.details.at("reference_type"), reference_type);
    EXPECT_EQ(error.details.at("reference_id"), reference_id);
    if (target_role_id == nullptr) {
      EXPECT_EQ(error.details.count("target_role_id"), 0U);
    } else {
      EXPECT_EQ(error.details.at("target_role_id"), target_role_id);
    }
  };
  expect_named(not_local.error(), "missing_reference", "frame_role", "tcp",
               "peer_arm");
  expect_named(carried_not_local.error(), "missing_reference",
               "scene_resource", "peer-carried", nullptr);
  expect_named(missing_frame.error(), "missing_reference", "frame_role",
               "no-such-frame", "peer_arm");
  const core::ReferenceDTO absent_resource(
      core::ReferenceDTO::Type::kSceneResource, "no-such-resource");
  const auto missing_resource =
      capture_with(*cell_resolver, {absent_resource}, 30);
  ASSERT_FALSE(missing_resource.ok());
  EXPECT_EQ(missing_resource.error().details.at("reason"),
            "program_scene_resource_missing");
  expect_named(missing_resource.error(), "missing_reference", "scene_resource",
               "no-such-resource", nullptr);
  const auto resolved_missing_resource =
      cell_resolver->resolve_reference_pose(absent_resource, captured.value());
  ASSERT_FALSE(resolved_missing_resource.ok());
  EXPECT_EQ(resolved_missing_resource.error().details.at("reason"),
            "program_scene_resource_missing");
  expect_named(resolved_missing_resource.error(), "missing_reference",
               "scene_resource", "no-such-resource", nullptr);
  const auto missing_own_role = capture_with(
      *cell_resolver,
      {core::ReferenceDTO(core::ReferenceDTO::Type::kFrameRole,
                          "no-such-own-frame")},
      30);
  ASSERT_FALSE(missing_own_role.ok());
  EXPECT_EQ(missing_own_role.error().details.at("reason"),
            "program_frame_role_missing");
  expect_named(missing_own_role.error(), "missing_reference", "frame_role",
               "no-such-own-frame", nullptr);

  // An error names exactly one reference. When the moving robot's own base
  // frame fails while another robot's frame is requested, the details name
  // that base with the moving robot's own target role and take no field of
  // the requested peer reference.
  const auto *base_frame = std::get_if<core::ResolvedComponentModelFrameDTO>(
      &exact_binding.base_frame);
  ASSERT_NE(base_frame, nullptr);
  ASSERT_FALSE(exact_binding.target_role_id.empty());
  ASSERT_NE(exact_binding.target_role_id.value(), "peer_arm");
  auto without_base = captured.value();
  const auto base_path =
      std::find_if(without_base.kinematic_paths.begin(),
                   without_base.kinematic_paths.end(), [&](const auto &path) {
                     return path.terminal_frame == exact_binding.base_frame;
                   });
  ASSERT_NE(base_path, without_base.kinematic_paths.end());
  const auto base_index =
      std::distance(without_base.kinematic_paths.begin(), base_path);
  without_base.kinematic_paths.erase(base_path);
  without_base.kinematic_state_path_views.erase(
      without_base.kinematic_state_path_views.begin() + base_index);
  ASSERT_TRUE(without_base.valid());
  for (const auto &requested : {peer_tcp, own_tcp, carried}) {
    const auto base_failed =
        cell_resolver->resolve_reference_pose(requested, without_base);
    ASSERT_FALSE(base_failed.ok());
    EXPECT_EQ(base_failed.error().details.at("reason"),
              "program_reference_frame_view_missing");
    expect_named(base_failed.error(), "missing_reference", "frame_role",
                 base_frame->frame_role_id.value().c_str(),
                 exact_binding.target_role_id.value().c_str());
  }

  // A failing runtime-object binding of the capture is named by its key.
  const auto capture_with_objects =
      [&](std::vector<core::RuntimeObjectReferenceBindingDTO> objects) {
        auto [start, publication] = controlled_start_at(30);
        return cell_resolver->capture_program_frame_resolution_context(
            core::ProgramFrameResolutionCaptureDTO{exact_binding,
                                                   std::move(start),
                                                   std::move(publication),
                                                   projection_binding,
                                                   scene_ptr,
                                                   std::move(objects),
                                                   {},
                                                   30});
      };
  const auto stale_object =
      capture_with_objects({{core::RuntimeObjectId("stale-object"),
                             core::SceneResourceId("no-such-resource")}});
  ASSERT_FALSE(stale_object.ok());
  EXPECT_EQ(stale_object.error().details.at("reason"),
            "runtime_object_binding_invalid");
  expect_named(stale_object.error(), "missing_reference", "runtime_object",
               "stale-object", nullptr);
  const auto duplicated_object =
      capture_with_objects({{core::RuntimeObjectId("held-object"),
                             core::SceneResourceId("peer-carried")},
                            {core::RuntimeObjectId("held-object"),
                             core::SceneResourceId("peer-carried")}});
  ASSERT_FALSE(duplicated_object.ok());
  EXPECT_EQ(duplicated_object.error().details.at("reason"),
            "runtime_object_binding_invalid");
  expect_named(duplicated_object.error(), "missing_reference", "runtime_object",
               "held-object", nullptr);
  // A binding without a key names nothing.
  const auto keyless_object = capture_with_objects(
      {{core::RuntimeObjectId(""), core::SceneResourceId("peer-carried")}});
  ASSERT_FALSE(keyless_object.ok());
  EXPECT_EQ(keyless_object.error().details.at("reason"),
            "runtime_object_binding_invalid");
  EXPECT_EQ(keyless_object.error().details.count("reference_type"), 0U);
  EXPECT_EQ(keyless_object.error().details.count("reference_id"), 0U);

  // A failure about the whole capture or context names no reference, even
  // when raised while one requested reference is being resolved.
  const auto expect_unnamed = [](const core::ErrorDTO &error) {
    EXPECT_EQ(error.details.count("reference_type"), 0U);
    EXPECT_EQ(error.details.count("reference_id"), 0U);
    EXPECT_EQ(error.details.count("target_role_id"), 0U);
  };
  auto invalid_context = captured.value();
  invalid_context.captured_at_ns = -1;
  ASSERT_FALSE(invalid_context.valid());
  const auto context_mismatch =
      cell_resolver->resolve_reference_pose(peer_tcp, invalid_context);
  ASSERT_FALSE(context_mismatch.ok());
  EXPECT_EQ(context_mismatch.error().details.at("reason"),
            "program_reference_context_mismatch");
  expect_unnamed(context_mismatch.error());
  {
    auto [start, publication] = controlled_start_at(30);
    const auto identity_mismatch =
        cell_resolver->capture_program_frame_resolution_context(
            core::ProgramFrameResolutionCaptureDTO{exact_binding,
                                                   std::move(start),
                                                   std::move(publication),
                                                   projection_binding,
                                                   scene_ptr,
                                                   {},
                                                   {peer_tcp},
                                                   -1});
    ASSERT_FALSE(identity_mismatch.ok());
    EXPECT_EQ(identity_mismatch.error().details.at("reason"),
              "program_reference_capture_identity_mismatch");
    expect_unnamed(identity_mismatch.error());
  }
  const auto query_mismatch = cell_resolver->Resolve(
      core::SceneFrameResolutionQueryDTO{
          core::TargetCarrierSceneFrameRefDTO{peer_component_id, arm_target_id,
                                              core::FrameRoleId("tcp")},
          projection_binding},
      make_projection_runtime_shadow_snapshot(bundle.value().cell_graph.cell_id)
          .shared(),
      30);
  ASSERT_FALSE(query_mismatch.ok());
  EXPECT_EQ(query_mismatch.error().details.at("reason"),
            "scene_frame_query_mismatch");
  expect_unnamed(query_mismatch.error());
  {
    // A capture failure met while one requested reference's frame path is
    // read is about the capture, not that reference: the own TCP path reads
    // a root publication newer than the selected start, so the frame path
    // conflicts with the start. Naming is opt-in, so it names nothing.
    auto [selected_start, selected_publication] = controlled_start_at(30);
    ASSERT_TRUE(
        root_channel->publish_state(root_positions, zero_velocities, 30, 30)
            .applied());
    const auto capture_from_selected_start =
        [&](std::vector<core::ReferenceDTO> references) {
          return cell_resolver->capture_program_frame_resolution_context(
              core::ProgramFrameResolutionCaptureDTO{exact_binding,
                                                     selected_start,
                                                     selected_publication,
                                                     projection_binding,
                                                     scene_ptr,
                                                     {},
                                                     std::move(references),
                                                     30});
        };
    // The base frame alone agrees with the selected start; the conflict is
    // met only on the requested reference's path.
    const auto base_only = capture_from_selected_start({});
    ASSERT_TRUE(base_only.ok()) << base_only.error().message;
    const auto conflict = capture_from_selected_start({own_tcp});
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "missing_reference");
    EXPECT_EQ(conflict.error().details.at("reason"),
              "program_reference_state_capture_conflict");
    expect_unnamed(conflict.error());
  }

  // Pose transform between frames of different robots and Scene objects.
  const core::Pose3DTO probe{{0.05, -0.02, 0.03}, {0.0, 0.0, 0.0, 1.0}};
  const auto transform = cell_resolver->ResolveFrameTransform(
      RetainedSceneFrameTransformQuery{
          probe, carried,
          core::ReferenceDTO(core::FrameRoleId("base"),
                             core::ReferenceTargetDTO{
                                 core::TargetRoleId("root_arm"),
                                 root_component_id, arm_target_id}),
          projection_binding},
      scene_ptr, 30);
  ASSERT_TRUE(transform.ok()) << transform.error().message;
  const auto peer_tcp_at_30 = resolve_cell_frame(peer_component_id, "tcp", 30);
  const auto root_base_at_30 = resolve_cell_frame(root_component_id, "base", 30);
  expect_pose_near(
      transform.value().pose_in_target_frame,
      core::multiply_poses(
          core::inverse_pose(root_base_at_30),
          core::multiply_poses(
              core::multiply_poses(peer_tcp_at_30, pose_in_carrier), probe)));
  EXPECT_EQ(transform.value().resolution_evidence_digests.size(), 2U);
  const auto to_cell = cell_resolver->ResolveFrameTransform(
      RetainedSceneFrameTransformQuery{probe, cell, cell, projection_binding},
      scene_ptr, 30);
  ASSERT_TRUE(to_cell.ok()) << to_cell.error().message;
  expect_pose_near(to_cell.value().pose_in_target_frame, probe);
  EXPECT_TRUE(to_cell.value().resolution_evidence_digests.empty());
  const auto unqualified = cell_resolver->ResolveFrameTransform(
      RetainedSceneFrameTransformQuery{probe, own_tcp, cell,
                                       projection_binding},
      scene_ptr, 30);
  ASSERT_FALSE(unqualified.ok());
  EXPECT_EQ(unqualified.error().details.at("reason"),
            "frame_transform_reference_unsupported");
  expect_named(unqualified.error(), "invalid_request", "frame_role", "tcp",
               nullptr);
  const auto transform_not_local = root_only_resolver->ResolveFrameTransform(
      RetainedSceneFrameTransformQuery{probe, peer_tcp, cell,
                                       projection_binding},
      scene_ptr, 30);
  ASSERT_FALSE(transform_not_local.ok());
  EXPECT_EQ(transform_not_local.error().details.at("reason"),
            "program_reference_robot_not_local");
  expect_named(transform_not_local.error(), "missing_reference", "frame_role",
               "tcp", "peer_arm");
  const auto transform_stale = cell_resolver->ResolveFrameTransform(
      RetainedSceneFrameTransformQuery{probe, peer_tcp, cell,
                                       projection_binding},
      scene_ptr, 40);
  ASSERT_FALSE(transform_stale.ok());
  EXPECT_EQ(transform_stale.error().details.at("reason"),
            "observed_state_unavailable_at_evaluation_time");
  const auto carried_transform_not_local =
      root_only_resolver->ResolveFrameTransform(
          RetainedSceneFrameTransformQuery{probe, carried, cell,
                                           projection_binding},
          scene_ptr, 30);
  ASSERT_FALSE(carried_transform_not_local.ok());
  EXPECT_EQ(carried_transform_not_local.error().details.at("reason"),
            "program_reference_robot_not_local");
  expect_named(carried_transform_not_local.error(), "missing_reference",
               "scene_resource", "peer-carried", nullptr);

  // One coherent capture: the peer source publishes a new state right after
  // its first read. Source (carried object) and target (its carrier TCP)
  // cross the same peer path, so a coherent answer is exactly
  // `pose_in_carrier * probe` whatever the robot state, and the peer source
  // is read once.
  class ScriptedPublications final : public core::IObservedStateProvider {
  public:
    ScriptedPublications(core::ObservedStatePublicationDTO first,
                         core::ObservedStatePublicationDTO later)
        : first_(std::move(first)), later_(std::move(later)) {}
    [[nodiscard]] core::Result<core::ObservedStateSnapshotDTO>
    current_state(const core::ObservedStateQueryContextDTO &,
                  std::int64_t) const override {
      return core::Result<core::ObservedStateSnapshotDTO>::Err(
          core::ErrorDTO{"unsupported", "unsupported", {}});
    }
    [[nodiscard]] core::Result<core::ObservedStatePublicationDTO>
    publication(const core::ControlBindingId &id,
                const core::ControlBindingRevision &revision) const override {
      return publication_at(id, revision, 0);
    }
    [[nodiscard]] core::Result<core::ObservedStatePublicationDTO>
    publication_at(const core::ControlBindingId &,
                   const core::ControlBindingRevision &,
                   std::int64_t) const override {
      ++reads_;
      return core::Result<core::ObservedStatePublicationDTO>::Ok(
          reads_ == 1U ? first_ : later_);
    }
    [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

  private:
    core::ObservedStatePublicationDTO first_;
    core::ObservedStatePublicationDTO later_;
    mutable std::size_t reads_{};
  };
  const auto peer_context = peer_channel->query_context();
  const auto first_peer = peer_channel->addressed_publication()->publication(
      peer_context.control_binding_id, peer_context.control_binding_revision);
  ASSERT_TRUE(first_peer.ok());
  const std::vector<double> drifted_peer_positions{-0.6, -0.1, 0.9,
                                                   0.3,  1.2,  -0.4};
  ASSERT_TRUE(peer_channel
                  ->publish_state(drifted_peer_positions, zero_velocities, 31,
                                  31)
                  .applied());
  const auto drifted_peer = peer_channel->addressed_publication()->publication(
      peer_context.control_binding_id, peer_context.control_binding_revision);
  ASSERT_TRUE(drifted_peer.ok());
  ASSERT_FALSE(drifted_peer.value().snapshot.positions ==
               first_peer.value().snapshot.positions);
  const auto scripted = std::make_shared<ScriptedPublications>(
      first_peer.value(), drifted_peer.value());
  const auto scripted_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, bundle, model_map, retained, root_runtime_unit_id,
      root_epoch,
      {source_for(root_channel),
       RetainedSceneFrameStateSourceInput{
           peer_context, peer_channel->source_kind(), scripted}});
  const auto coherent = scripted_resolver->ResolveFrameTransform(
      RetainedSceneFrameTransformQuery{probe, carried, peer_tcp,
                                       projection_binding},
      scene_ptr, 32);
  ASSERT_TRUE(coherent.ok()) << coherent.error().message;
  EXPECT_EQ(scripted->reads(), 1U);
  expect_pose_near(coherent.value().pose_in_target_frame,
                   core::multiply_poses(pose_in_carrier, probe));
  EXPECT_EQ(coherent.value().source_frame_in_cell,
            core::multiply_poses(coherent.value().target_frame_in_cell,
                                 pose_in_carrier));

  // One capture holds the retained receiver-time skew budget across robots:
  // two individually fresh publications received further apart than the
  // budget are never composed, at admission or in a frame transform.
  constexpr std::int64_t kSkewBudgetNs = 10'000'000;
  const core::ReferenceDTO root_tcp(
      core::FrameRoleId("tcp"),
      core::ReferenceTargetDTO{core::TargetRoleId("root_arm"),
                               root_component_id, arm_target_id});
  const auto publish_pair = [&](const std::int64_t root_at_ns,
                                const std::int64_t peer_at_ns) {
    ASSERT_TRUE(root_channel
                    ->publish_state(root_positions, zero_velocities,
                                    root_at_ns, root_at_ns)
                    .applied());
    ASSERT_TRUE(peer_channel
                    ->publish_state(peer_positions, zero_velocities,
                                    peer_at_ns, peer_at_ns)
                    .applied());
  };
  const auto cross_robot_transform = [&](const std::int64_t now_ns) {
    return cell_resolver->ResolveFrameTransform(
        RetainedSceneFrameTransformQuery{probe, peer_tcp, root_tcp,
                                         projection_binding},
        scene_ptr, now_ns);
  };
  constexpr std::int64_t kSkewedRootNs = 1'000;
  constexpr std::int64_t kSkewedPeerNs = kSkewedRootNs + kSkewBudgetNs + 1;
  publish_pair(kSkewedRootNs, kSkewedPeerNs);
  for (const auto &component_id : {root_component_id, peer_component_id}) {
    const auto individually = cell_resolver->Resolve(
        core::SceneFrameResolutionQueryDTO{
            core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                                core::FrameRoleId("tcp")},
            projection_binding},
        scene_ptr, kSkewedPeerNs);
    ASSERT_TRUE(individually.ok()) << individually.error().message;
  }
  const auto skewed_capture = capture_with(*cell_resolver, {peer_tcp},
                                           kSkewedPeerNs);
  ASSERT_FALSE(skewed_capture.ok());
  EXPECT_EQ(skewed_capture.error().details.at("reason"),
            "kinematic_receiver_time_skew_exceeded");
  const auto skewed_transform = cross_robot_transform(kSkewedPeerNs);
  ASSERT_FALSE(skewed_transform.ok());
  EXPECT_EQ(skewed_transform.error().details.at("reason"),
            "kinematic_receiver_time_skew_exceeded");

  constexpr std::int64_t kBudgetRootNs = 20'000'000;
  constexpr std::int64_t kBudgetPeerNs = kBudgetRootNs + kSkewBudgetNs;
  publish_pair(kBudgetRootNs, kBudgetPeerNs);
  const auto within_capture = capture_with(*cell_resolver, {peer_tcp},
                                           kBudgetPeerNs);
  ASSERT_TRUE(within_capture.ok()) << within_capture.error().message;
  const auto within_transform = cross_robot_transform(kBudgetPeerNs);
  ASSERT_TRUE(within_transform.ok()) << within_transform.error().message;
  EXPECT_EQ(within_transform.value().resolution_evidence_digests.size(), 2U);
}

TEST(RetainedProductCrossConsumer,
     ProductSelectedStateConfigRequiresExplicitSelection) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));

  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_motion_arm_gripper_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto model_map = load_lite6_model_map(
      package_root, deployment::RetainedModelMapQuery{
                        materialized.bundle->retained_bundle_id(),
                        products.symbol_role_maps.front().product_id});
  const auto &graph = materialized.bundle->value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  const core::RuntimeEpoch runtime_epoch(
      "epoch." + graph.runtime_units.front().runtime_unit_id.value());
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto commanded_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, target_id,
      core::StateAuthorityClass::kCommanded, std::nullopt, runtime_epoch);

  const RetainedObservedStateChannelConfig selected{
      core::ObservedStateSourceKind::kCommandedState,
      "test/product-selected-commanded-state", 0, 7U};
  const auto resolved = detail::select_retained_state_channel_config(
      std::optional<RetainedObservedStateChannelConfig>{selected},
      commanded_binding);
  EXPECT_EQ(resolved.source_kind,
            core::ObservedStateSourceKind::kCommandedState);
  EXPECT_EQ(resolved.source_id, selected.source_id);
  EXPECT_EQ(resolved.authority_generation, 7U);

  EXPECT_THROW(static_cast<void>(detail::select_retained_state_channel_config(
                   std::nullopt, commanded_binding)),
               std::invalid_argument);
}

enum class Lite6AggregateCheck {
  kCompose,
  kMovingValidation,
  kResealInventory,
  kRetainedRouteGenerationReseal,
  kRefreshCellModel,
  kDeploymentFaultStopProfile,
  kDeploymentFastFaultStopProfile,
  kDeploymentFaultStopProfileWithExtraJoints,
  kDeploymentFaultStopProfileWithOnlyExtraSettleOverrides,
  kDeploymentFaultStopProfileWithSparseSettleOverrides,
  kDeploymentFaultStopProfileMissingArmJoint,
  kDeploymentFaultStopProfileWithoutArmJoints,
  kExecutionPortParticipant,
  kMissingSelectedBackend,
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  kRosOrdinaryJerkRejection,
#endif
  kSameRoutePredecessorQualification,
  kMeasuredPublication,
  kResolverRebuildFailures,
  kStopResponseFollowsRouteRebind,
  kQualifiedEndpointPlannerFrames,
  kMovingBudgetPublication,
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  kMixedOutputPublication,
  kGeometricHostInstallation,
#endif
  kMeasuredOptionalDomainRace,
  kMeasuredNullOptionalDomainRace
};

#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
void select_geometric_contact_station(
    deployment::CanonicalValue::Object &root) {
  using deployment::CanonicalValue;
  const auto components =
      std::find_if(root.begin(), root.end(),
                   [](const auto &row) { return row.first == "components"; });
  if (components == root.end())
    throw std::logic_error("maintained geometric fixture has no components");
  auto rows = components->second.as_array();
  std::erase_if(rows, [](const auto &row) {
    const auto *id = row.find_object_member("component_id");
    return id == nullptr || (id->as_string() != "lite6_robot" &&
                             id->as_string() != "station_io");
  });
  const auto station =
      std::find_if(rows.begin(), rows.end(), [](const auto &row) {
        const auto *id = row.find_object_member("component_id");
        return id != nullptr && id->as_string() == "station_io";
      });
  if (station == rows.end())
    throw std::logic_error("maintained geometric fixture has no station I/O");
  auto station_fields = station->as_object();
  auto configuration = std::find_if(
      station_fields.begin(), station_fields.end(),
      [](const auto &row) { return row.first == "configuration"; });
  auto configuration_fields = configuration->second.as_object();
  auto runtime_configuration = std::find_if(
      configuration_fields.begin(), configuration_fields.end(),
      [](const auto &row) { return row.first == "runtime_configuration"; });
  auto runtime_fields = runtime_configuration->second.as_object();
  auto signals =
      std::find_if(runtime_fields.begin(), runtime_fields.end(),
                   [](const auto &row) { return row.first == "io_signals"; });
  signals->second = deployment::parse_strict_json_model_yaml(R"([{
    "command_target_id":"io","controllable_component_id":"station_io",
    "direction":"input",
    "freshness":{"mode":"sample_age","maximum_age_ns":5000000000},
    "provider":{"profile":"geometric_contact","ingress_depth":4,
      "endpoint":{"controllable_component_id":"lite6_robot",
        "command_target_id":"gripper","frame_role_id":"tcp"},
      "target_scene_resource_id":"geometric_target"},
    "resource_role_id":"probe_contact","signal_name":"probe_contact",
    "source_id":"station_io.probe_contact",
    "transition_capability":"sampled_only","update_mode":"sampled",
    "value_type":"bool"
  }])")
                        .root();
  runtime_configuration->second = CanonicalValue(std::move(runtime_fields));
  configuration->second = CanonicalValue(std::move(configuration_fields));
  *station = CanonicalValue(std::move(station_fields));
  components->second = CanonicalValue(std::move(rows));
}
#endif

void check_refreshed_cell_model(
    RetainedNativeRuntime &runtime, ManagedKernelAdmissionGate &admission_gate,
    const RetainedPlanningModelStage &stage,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::VerifiedRetainedPlanningModel &retained,
    const core::RuntimeParticipantBinding &arm_binding,
    const core::RuntimeParticipantBinding &gripper_binding,
    const std::string &tcp_frame);

// The selected Core constructor cases never submit a gripper command.
// The second participant supplies only the required owned interfaces;
// unexpected command/Stop execution fails the fixture instead of simulating law.
class UnusedSelectedGripperBackend final
    : public core::IGripperCommandBackend,
      public core::IControlDomainExecutionPort {
public:
  UnusedSelectedGripperBackend(core::RuntimeParticipantBinding binding,
      std::shared_ptr<IRetainedObservedStateChannel> channel)
      : binding_(std::move(binding)), channel_(std::move(channel)) {}

  core::Result<void> RebindRoute(core::RuntimeParticipantBinding binding,
      std::shared_ptr<IRetainedObservedStateChannel> channel) {
    if (!channel || binding.value().controllable_component_id !=
                        binding_.value().controllable_component_id ||
        binding.value().command_target_id != binding_.value().command_target_id)
      return core::Result<void>::Err({"test_gripper_route_invalid", "selected gripper owner differs", {}});
    binding_ = std::move(binding);
    channel_ = std::move(channel);
    return core::Result<void>::Ok();
  }

  core::GripperCommandResultDTO command_gripper(
      const core::GripperCommandRequestDTO &) override {
    throw std::logic_error("selected-owner fixture does not execute gripper commands");
  }
  core::GripperCommandStatusDTO get_gripper_status(
      const std::string &gripper_id) const override {
    if (gripper_id != binding_.value().command_target_id.value() || !channel_)
      throw std::logic_error("selected-owner fixture has no matching gripper state owner");
    const auto context = channel_->query_context();
    const auto expected = core::make_observed_state_query_context(
        binding_, context.freshness_budget_ns);
    if (!expected.ok() || context != expected.value() ||
        context.resources.size() != 1U ||
        context.coordinate_kinds.front() != core::CoordinateKind::kLinear)
      throw std::logic_error("selected-owner fixture gripper channel differs from its binding");

    // This fixture has accepted no command: Idle and command counters zero
    // describe its own history, independently of route readiness or presence.
    core::GripperCommandStatusDTO status;
    status.state = core::GripperCommandState::kIdle;
    status.gripper_id = gripper_id;
    const auto state = channel_->current_controlled_state();
    if (!state.ok()) {
      status.error = state.error();
      return status;
    }
    if (state.value().controllable_component_id != context.controllable_component_id ||
        state.value().command_target_id != context.command_target_id ||
        state.value().control_binding_id != context.control_binding_id ||
        state.value().control_binding_revision != context.control_binding_revision ||
        state.value().active_control_configuration_id != context.active_control_configuration_id ||
        state.value().active_control_configuration_generation !=
            context.active_control_configuration_generation ||
        state.value().resources != context.resources ||
        state.value().joint_names != context.joint_names ||
        state.value().positions.size() != 1U ||
        !std::holds_alternative<core::LinearPositionDTO>(state.value().positions.front()))
      throw std::logic_error("selected-owner fixture gripper positions differ from its channel");
    const auto position = std::get<core::LinearPositionDTO>(
        state.value().positions.front()).value_m;
    status.current_position = position;
    status.commanded_position = position;
    status.observed_position = position;
    return status;
  }
  core::GripperCommandResultDTO mark_gripper_grasp_lost(
      const std::string &, std::uint64_t, std::uint64_t,
      const std::string &, const std::string &, const std::string &) override {
    throw std::logic_error("selected-owner fixture does not mutate grasp evidence");
  }
  void invalidate_gripper_commands(core::GripperInvalidationReason) noexcept override {}
  void authorize_gripper_commands() noexcept override {}
  core::ParticipantStopStageResultDTO TranslateStopStage(
      const core::ResolvedParticipantStopStageDTO &,
      const core::RemainingStopBudgetDTO &, std::stop_token) override {
    throw std::logic_error("selected-owner fixture does not execute gripper Stop");
  }
private:
  core::RuntimeParticipantBinding binding_;
  std::shared_ptr<IRetainedObservedStateChannel> channel_;
};

// Focused Core seam: the selected product supplies bindings and state, while
// FakeExecutionPort exposes ownership/rebind behavior without simulator law.
[[nodiscard]] RetainedRuntimeBackendFactory selected_core_test_backend_factory(
    core::ExecutionPortParticipant *&port_participant,
    bool require_gripper = false) {
  return [&port_participant, require_gripper](
            const core::RuntimeParticipantBinding &motion,
            const std::optional<core::RuntimeParticipantBinding> &gripper,
            const std::shared_ptr<RetainedObservedStateChannel> &motion_channel,
            const std::shared_ptr<RetainedObservedStateChannel> &gripper_channel,
            core::IClock &clock, core::IScheduler &scheduler) {
          auto port = std::make_shared<core::testing::FakeExecutionPort>();
          core::ExecutionLaneFactsDTO facts;
          for (const auto &resource : motion.value().resources)
            facts.joint_names.push_back(resource.compiled_resource_name);
          facts.supports_scale = true;
          facts.state_rate_hz = 100.0;
          facts.controller_update_rate_hz = 150.0;
          port->SetLaneFacts(facts);
          core::ExecutionPortParticipantConfig config;
          config.stop.trip_authority = motion.value().control_path->state_authority_class;
          config.stop.stop_traj_delta_s = 0.05;
          config.stop.sample_period_s = 0.01;
          config.stop.replacement_start_allowance_s = 0.02;
          config.stop.hard_stop_completion_timeout_ns = 500'000'000;
          config.late_cycle_margin_s = 0.015;
          auto participant = std::make_shared<core::ExecutionPortParticipant>(
              motion, port, clock, scheduler,
              [](core::TimePoint, const std::stop_token &) {},
              core::ExecutionPortParticipantStateAuthority{
                  &motion_channel->publication().numeric_publication(),
                  [] { return core::Result<void>::Ok(); }},
              std::move(config));
          port_participant = participant.get();
          if (require_gripper) {
            EXPECT_TRUE(gripper.has_value());
            EXPECT_NE(gripper_channel, nullptr);
          }
          auto gripper_backend = gripper.has_value()
              ? std::make_shared<UnusedSelectedGripperBackend>(*gripper, gripper_channel)
              : nullptr;
          RetainedRuntimeBackendSet selected;
          selected.execution = participant;
          selected.gripper = gripper_backend;
          selected.gripper_stop_execution_port = gripper_backend.get();
          selected.rebind_motion_route =
              [participant](
                  core::RuntimeParticipantBinding binding,
                  std::vector<core::RuntimeParticipantBinding> aliases) {
                return participant->RebindRoute(std::move(binding), std::move(aliases));
              };
          if (gripper_backend) {
            selected.rebind_gripper_route =
                [gripper_backend](core::RuntimeParticipantBinding binding,
                    std::shared_ptr<IRetainedObservedStateChannel> channel) {
                  return gripper_backend->RebindRoute(std::move(binding), std::move(channel));
                };
          }
          return selected;
        };
}

[[nodiscard]] RetainedObservedStateChannelConfig selected_core_test_state_config(
    const core::RuntimeParticipantBinding &binding) {
  return {binding.value().control_path->state_authority_class == core::StateAuthorityClass::kPredicted
              ? core::ObservedStateSourceKind::kPredictedState
              : core::ObservedStateSourceKind::kCommandedState,
          "test/selected-core-state", 0, 1U};
}

void check_selected_core_lite6_aggregate(const Lite6AggregateCheck check) {
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  const bool mixed_output_check =
      check == Lite6AggregateCheck::kMixedOutputPublication;
  const bool geometric_host_check =
      check == Lite6AggregateCheck::kGeometricHostInstallation;
  const bool ros_jerk_rejection =
      check == Lite6AggregateCheck::kRosOrdinaryJerkRejection;
#else
  const bool mixed_output_check = false;
  const bool geometric_host_check = false;
  const bool ros_jerk_rejection = false;
#endif
  const bool maintained_product_check =
      mixed_output_check || geometric_host_check;
  const bool reseal_capability_inventory =
      check == Lite6AggregateCheck::kResealInventory;
  const bool authored_fault_check =
      check == Lite6AggregateCheck::kDeploymentFaultStopProfile ||
      check == Lite6AggregateCheck::kDeploymentFastFaultStopProfile ||
      check ==
          Lite6AggregateCheck::kDeploymentFaultStopProfileWithExtraJoints ||
      check == Lite6AggregateCheck::
                   kDeploymentFaultStopProfileWithOnlyExtraSettleOverrides ||
      check == Lite6AggregateCheck::
                   kDeploymentFaultStopProfileWithSparseSettleOverrides ||
      check ==
          Lite6AggregateCheck::kDeploymentFaultStopProfileMissingArmJoint ||
      check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithoutArmJoints;
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  struct SelectedBundle {
    std::shared_ptr<const deployment::RetainedBundle> bundle;
  };
  const auto materialized = [&]() -> SelectedBundle {
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
    if (geometric_host_check)
      return {deployment::test::MaterializeMaintainedInterruptDeployment(
                  package_root,
                  deployment::parse_strict_json_model_yaml(
                      R"({"interrupt_rules":[]})")
                      .root(),
                  "examples/r5_trial_e_signal.scenario",
                  select_geometric_contact_station)
                  .bundle};
    if (mixed_output_check)
      return {deployment::test::MaterializeMaintainedInterruptDeployment(
                  package_root,
                  deployment::parse_strict_json_model_yaml(
                      R"({"interrupt_rules":[]})")
                      .root(),
                  "examples/r5_trial_e_signal.scenario")
                  .bundle};
    if (ros_jerk_rejection) {
      const auto plans = detail::ros2_control_lifecycle_plans_internal::
          retained_ros2_control_lifecycle_plans();
      const auto semantic =
          core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
              core::ProviderManifestId("manyforge.provider.ros2_control"),
              plans);
      if (!semantic.ok())
        throw std::logic_error(semantic.error().message);
      return {
          deployment::test::
              materialize_lite6_ros2_control_cclc_product_retained_consumer_fixture(
                  package_root, semantic.value().value())
                  .bundle};
    }
#endif
    return {
        deployment::test::
            materialize_lite6_ros2_control_motion_arm_gripper_retained_consumer_fixture(
                package_root)
                .bundle};
  }();
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, deployment::RetainedPlanningModelQuery{
                        materialized.bundle->retained_bundle_id(),
                        products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, deployment::RetainedModelMapQuery{
                        materialized.bundle->retained_bundle_id(),
                        products.symbol_role_maps.front().product_id});

  const core::ControllableComponentId component_id(
      maintained_product_check
          ? "lite6_robot"
          : std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto &graph = materialized.bundle->value().cell_graph;
  if (!maintained_product_check) {
    ASSERT_EQ(graph.runtime_units.size(), 1U);
  }
  const auto &arm_control = require_fixture_one(
      graph.control_bindings,
      [&](const auto &row) {
        return row.controllable_component_id == component_id &&
               row.command_target_id == arm_target_id &&
               row.capability_id ==
                   core::CapabilityId("execution.trajectory.execute");
      },
      "actual arm execution control binding");
  const core::RuntimeEpoch local_runtime_epoch(
      "epoch." + (maintained_product_check
                      ? arm_control.provider_runtime_unit_id
                      : graph.runtime_units.front().runtime_unit_id)
                     .value());
  const auto state_authority = core::StateAuthorityClass::kCommanded;
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id,
      state_authority, std::nullopt, local_runtime_epoch,
      maintained_product_check ? std::optional(arm_control.capability_id)
                               : std::nullopt);
  const core::ProgramCommandSchemaRefDTO command_schema{
      core::CommandSchemaId(std::string(core::kGripperPositionCommandSchemaId)),
      core::CommandSchemaRevision(
          std::string(core::kGripperPositionCommandSchemaRevision))};
  auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id,
      state_authority, command_schema, local_runtime_epoch,
      maintained_product_check
          ? std::optional(core::CapabilityId("tool.gripper.command"))
          : std::nullopt);
  if (geometric_host_check) {
    deployment::FilesystemCatalogArtifactStore artifacts(package_root);
    const auto direct =
        deployment::compile_direct_control_program_binding_inventory(
            *materialized.bundle, model_map, artifacts);
    const auto direct_gripper = require_fixture_one(
        direct.gripper,
        [&](const auto &binding) {
          return binding != nullptr &&
                 std::any_of(
                     binding->value().target_role_bindings.begin(),
                     binding->value().target_role_bindings.end(),
                     [&](const auto &target) {
                       return target.controllable_component_id == component_id &&
                              target.command_target_id == gripper_target_id;
                     });
        },
        "actual direct gripper Program Binding");
    gripper_binding =
        with_program_binding_identity(gripper_binding, *direct_gripper);
  }

  if (check == Lite6AggregateCheck::kQualifiedEndpointPlannerFrames) {
    const auto *tool = model_map.find_target(component_id, gripper_target_id);
    ASSERT_NE(tool, nullptr);
    // This fixture deliberately maps both physical targets' TCP to the same
    // modeled frame. The full physical inventory therefore exercises exact
    // deduplication, rather than providing a sibling-only frame by itself.
    const auto full_legacy = retained_motion_planner_frames(arm_binding);
    EXPECT_EQ(retained_motion_planner_frames(arm_binding, &model_map),
              full_legacy);

    // Compile an actual Program whose arm declares only its base while its
    // sibling tool declares the TCP. The selected arm inventory is produced by
    // the sealed compiler/runtime path, not by editing a verified binding.
    auto program = deployment::parse_strict_json_model_yaml(R"({
      "api_version":"manyforge.program.v2",
      "program_id":"qualified_endpoint_inventory",
      "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "program_profile":"composable-cell-v1",
      "name":"Qualified endpoint inventory",
      "component_roles":{"robot":{}},
      "target_roles":{
        "arm":{"component_role":"robot",
          "required_capabilities":[{"capability_id":"execution.trajectory.execute","parameters":{},"disposition":"required"}],
          "required_frame_roles":["base"],"required_resource_roles":["joints"],
          "required_collision_body_roles":[],"required_model_roles":["arm_model"]},
        "tool":{"component_role":"robot",
          "required_capabilities":[{"capability_id":"tool.gripper.command","parameters":{},"disposition":"required"}],
          "required_frame_roles":["tcp"],"required_resource_roles":["fingers"],
          "required_collision_body_roles":[],"required_model_roles":["arm_model"]}},
      "tree":{"node_id":"sequence","instance_id":"root","parameters":{},"children":[]}
    })")
                       .root();
    auto program_members = program.as_object();
    program_members.emplace_back(
        "catalog",
        deployment::CanonicalValue(deployment::CanonicalValue::Object{
            {"api_version",
             deployment::CanonicalValue("manyforge.node_catalog.v2")},
            {"content_version",
             deployment::CanonicalValue(
                 materialized.bundle->value()
                     .catalogs.node_environment.content_version)}}));
    program = deployment::normalize_program_authoring(
        deployment::CanonicalValue(std::move(program_members)));
    const auto program_revision = deployment::detail::compute_content_identity(
        program,
        {"program-revision", "manyforge.program.v2", "program_revision"});
    program_members = program.as_object();
    for (auto &[name, value] : program_members)
      if (name == "program_revision")
        value = deployment::CanonicalValue(program_revision);
    const auto contract = deployment::verify_program_binding_contract(
        deployment::canonical_json(
            deployment::CanonicalValue(std::move(program_members))),
        *materialized.bundle);
    deployment::FilesystemCatalogArtifactStore catalog(package_root);
    const auto compiled = deployment::compile_program_binding(
        *contract,
        {{{core::ComponentRoleId("robot"), component_id}},
         {{core::TargetRoleId("arm"), arm_target_id},
          {core::TargetRoleId("tool"), gripper_target_id}}},
        *materialized.bundle, model_map, catalog);
    ASSERT_NE(compiled, nullptr);
    const auto selected_target = deployment::resolve_runtime_program_target(
        *compiled, core::TargetRoleId("arm"),
        core::CapabilityId("execution.trajectory.execute"),
        *materialized.bundle, model_map);
    const auto &physical = *arm_binding.value().control_path;
    auto selected = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            selected_target,
            {physical.control_route_id,
             physical.active_control_configuration_id,
             physical.active_control_configuration_generation,
             arm_binding.value().runtime_unit_epochs,
             {{physical.provider_instance_id, physical.provider_runtime_unit_id,
               physical.provider_runtime_epoch}}}));
    ASSERT_TRUE(selected.ok()) << selected.error().message;
    ASSERT_EQ(selected.value().value().frame_bindings.size(), 1U);
    EXPECT_EQ(selected.value().value().frame_bindings.front().frame_role_id,
              core::FrameRoleId("base"));
    const auto legacy = retained_motion_planner_frames(selected.value());
    EXPECT_EQ(legacy,
              make_candidate_binding(selected.value()).permitted_frames);
    const auto frames =
        retained_motion_planner_frames(selected.value(), &model_map);
    EXPECT_EQ(frames,
              retained_motion_planner_frames(selected.value(), &model_map));
    std::size_t sibling_only = 0U;
    for (const auto &exposed : tool->frames) {
      const auto *frame =
          std::get_if<deployment::CompositionComponentModelFrame>(
              &exposed.frame);
      ASSERT_NE(frame, nullptr);
      const core::ResolvedFrameDTO exact = core::ResolvedComponentModelFrameDTO{
          frame->controllable_component_id, frame->model_instance_id,
          exposed.frame_role_id, frame->compiled_frame_name,
          frame->model_revision};
      EXPECT_EQ(std::count(frames.begin(), frames.end(), exact), 1);
      if (std::find(legacy.begin(), legacy.end(), exact) == legacy.end())
        ++sibling_only;
    }
    EXPECT_GT(sibling_only, 0U);
    for (const auto &frame : frames) {
      const auto *model =
          std::get_if<core::ResolvedComponentModelFrameDTO>(&frame);
      ASSERT_NE(model, nullptr);
      EXPECT_EQ(model->controllable_component_id, component_id);
      EXPECT_EQ(model->model_instance_id,
                arm_binding.value().model_groups.front().model_instance_id);
      EXPECT_EQ(model->model_revision,
                arm_binding.value().model_bindings.front().model_revision);
      EXPECT_EQ(std::count(frames.begin(), frames.end(), frame), 1);
    }
    auto wrong = arm_binding.value();
    wrong.retained_bundle_id = core::RetainedBundleId("other-product");
    const auto wrong_product =
        core::verify_runtime_participant_binding(std::move(wrong));
    ASSERT_TRUE(wrong_product.ok());
    EXPECT_THROW(
        retained_motion_planner_frames(wrong_product.value(), &model_map),
        std::invalid_argument);
    return;
  }

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  const auto joint_limits = retained_joint_limit_rows(retained);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "retained_lite6_native_composition",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), joint_limits});
  auto scene_mutex = std::make_shared<std::mutex>();
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *materialized.bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{arm_binding,
                                                       gripper_binding},
          0));

  const auto &arm_group = arm_binding.value().model_groups.front();
  auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_native_composition", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(), arm_group.compiled_group_name);
  auto collision_projector = RetainedCollisionWorldProjector::create_verified(
      shared_collision_model, *materialized.bundle, model_map, retained);
  auto initial_scene = make_projection_runtime_shadow_snapshot(
      materialized.bundle->value().cell_graph.cell_id);
  if (geometric_host_check) {
    initial_scene = core::LogicalSceneSnapshotRoot(
        std::make_shared<const core::LogicalSceneSnapshotDTO>(
            make_collision_scene_snapshot(
                materialized.bundle->value().cell_graph.cell_id, 2U,
                core::SceneResourceId("geometric_target"), 0.0, 10.0)));
  }
  ASSERT_FALSE(arm_binding.value().runtime_unit_epochs.empty());
  const auto local_runtime = arm_binding.value().runtime_unit_epochs.front();
  const core::ProjectionNativeRuntimeBindingDTO native_binding{
      core::CapabilityProviderBindingId("lite6.runtime.projection"),
      core::CapabilityProviderBindingRevision("lite6-runtime-projection-v1"),
      local_runtime.runtime_unit_id, local_runtime.runtime_epoch};
  std::int64_t projection_clock =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      std::move(collision_projector), products.planning_collision_model,
      native_binding, *initial_scene.shared(),
      RetainedPlannerSceneProjectionConfig{"retained-lite6-native-runtime", 32U,
                                           32U},
      [&projection_clock] { return projection_clock++; });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  auto projection = std::move(projection_result.value());
  const auto inventory = projection->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(inventory.has_value());

  const auto candidate_binding = make_candidate_binding(arm_binding);
  ProcessContainedCandidateClientConfig candidate_config;
  candidate_config.worker_executable = MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH;
  candidate_config.require_retained_projection = true;
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding,
      std::make_unique<ProcessContainedRoboPlanCandidateClient>(
          RoboPlanWorkerModelDTO{"retained_lite6_native_worker",
                                 stage.loader_urdf_path(),
                                 stage.srdf_path(),
                                 stage.package_paths(),
                                 joint_limits,
                                 {},
                                 {}},
          candidate_binding, RoboPlanCandidateComputerConfig{.rng_seed = 7U},
          std::move(candidate_config)));
  const core::RequestOwnerEpoch owner_epoch("i6.native.runtime.epoch.1");
  RetainedNativeRuntimeConfig runtime_config{
      owner_epoch,
      materialized.bundle->value().cell_graph.cell_id,
      temporary.path() / std::string(core::kOperationJournalFilename),
      1024U,
      core::CacheConfig{},
      core::ExecutionKernelConfig{},
      SimpleSchedulerConfig{},
      10'000'000,
      {0.0, 0.0, 1.57, 0.0, 0.0, 0.0},
      kMaintainedNativeStopSurveillancePolicy,
      {},
      {},
      {},
      {},
      {}};
  ManagedKernelAdmissionGate admission_gate;
  auto runtime_input = RetainedNativeRuntimeInput{
      arm_binding,
      gripper_binding,
      {},
      {},
      {},
      {},
      scene,
      scene_mutex,
      state_projector,
      projection,
      shared_collision_model,
      std::move(planner),
      [&](const auto &channels) {
        std::vector<RetainedSceneFrameStateSourceInput> state_sources;
        state_sources.reserve(channels.size());
        for (const auto &channel : channels) {
          state_sources.push_back(RetainedSceneFrameStateSourceInput{
              channel->query_context(), channel->source_kind(),
              std::shared_ptr<const core::IObservedStateProvider>(
                  channel, channel->addressed_publication())});
        }
        return RetainedSceneFrameResolver::create_verified(
            shared_collision_model, *materialized.bundle, model_map, retained,
            local_runtime.runtime_unit_id, local_runtime.runtime_epoch,
            std::move(state_sources));
      },
      {},
      {},
      &admission_gate,
      {},
      {},
      {},
      {}};
  if (mixed_output_check) {
    // Include every actual control-facing route, including shared feedback,
    // reset and Stop capabilities of the same physical backend owner.
    for (const auto& control : graph.control_bindings) {
      if (control.controllable_component_id != component_id) continue;
      ASSERT_TRUE(control.command_target_id == arm_target_id ||
                  control.command_target_id == gripper_target_id);
      if (control.control_binding_id == arm_binding.value().control_path->control_binding_id) {
        runtime_input.control_binding_representatives.push_back(arm_binding);
      } else if (control.control_binding_id == gripper_binding.value().control_path->control_binding_id) {
        runtime_input.control_binding_representatives.push_back(gripper_binding);
      } else {
        runtime_input.control_binding_representatives.push_back(make_fixture_runtime_participant(
            *materialized.bundle, model_map, component_id, control.command_target_id,
            state_authority, {}, local_runtime_epoch, control.capability_id));
      }
    }
  }
  runtime_input.deployment_maximum_tcp_linear_speed_mps = 0.15;
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  if (ros_jerk_rejection) {
    core::OrdinaryMotionJerkPolicy jerk_profile{true, false, {}};
    for (const auto &resource : arm_binding.value().resources)
      jerk_profile.limits.emplace(*resource.source_joint_name,
                                  core::AngularJerkDTO{50.0});
    runtime_input.ordinary_motion_jerk =
        deployment::native_execution_runtime_contract::OrdinaryMotionJerkSettings{
            true, 1.0, {}};
    for (const auto &[joint, limit] : jerk_profile.limits)
      runtime_input.ordinary_motion_jerk->joint_limits.push_back(
          {joint, std::get<core::AngularJerkDTO>(limit).value_rad_s3, {}});
    EXPECT_EQ(graph.runtime_units.front().runtime_profile_id.value(),
              "manyforge.ros2_control_execution_runtime");
    ASSERT_TRUE(arm_binding.value().control_path.has_value());
    EXPECT_EQ(arm_binding.value().control_path->state_authority_class,
              core::StateAuthorityClass::kCommanded);
    ASSERT_TRUE(runtime_input.ordinary_motion_jerk.has_value());
    EXPECT_TRUE(runtime_input.ordinary_motion_jerk->enabled);

    RetainedRos2ControlBackendConfig ros_config;
    ros_config.node_name = "manyforge_jerk_rejection_fixture";
    ros_config.node_namespace = "/manyforge/" + component_id.value();
    ros_config.executor_thread_count = 1U;
    ros_config.participant.stop.trip_authority =
        arm_binding.value().control_path->state_authority_class;
    ros_config.observed_state_authority_profile_id = "manyforge.commanded_simulation";
    ros_config.defer_ros_start = true;
    for (const auto& resource : arm_binding.value().resources) {
      ASSERT_TRUE(resource.source_joint_name.has_value());
      ros_config.arm.adapter.lane_facts.joint_names.push_back(*resource.source_joint_name);
    }
    runtime_input.motion_state_channel_config = retained_ros2_control_motion_state_config(ros_config);
    runtime_input.gripper_state_channel_config =
        retained_ros2_control_gripper_state_config(gripper_binding);
    auto ros_factory = make_retained_ros2_control_backend_factory(std::move(ros_config));
    ASSERT_TRUE(static_cast<bool>(ros_factory));
    unsigned factory_calls = 0U;
    runtime_input.backend_factory =
        [real_factory = std::move(ros_factory), &factory_calls](
            const core::RuntimeParticipantBinding& motion,
            const std::optional<core::RuntimeParticipantBinding>& gripper,
            const std::shared_ptr<RetainedObservedStateChannel>& motion_channel,
            const std::shared_ptr<RetainedObservedStateChannel>& gripper_channel,
            core::IClock& clock, core::IScheduler& scheduler) {
          ++factory_calls;
          return real_factory(motion, gripper, motion_channel, gripper_channel, clock, scheduler);
        };
    try {
      static_cast<void>(create_retained_native_runtime(
          std::move(runtime_input), std::move(runtime_config)));
      ADD_FAILURE() << "selected ROS ordinary jerk must refuse before backend construction";
    } catch (const std::invalid_argument& error) {
      EXPECT_STREQ(error.what(), "ordinary jerk profile requires the native execution backend");
    }
    EXPECT_EQ(factory_calls, 0U);
    return;
  }
#endif
  if (reseal_capability_inventory) {
    // This synthetic DTO fixture adds a real control-facing feedback capability
    // to the same controller. It tests owner classification, not graph/compiler
    // qualification; the mixed-Apply process test covers that product boundary.
    auto observed = arm_binding.value();
    const auto *feedback = core::find_builtin_capability_definition(
        "execution.feedback.state_snapshots");
    ASSERT_NE(feedback, nullptr);
    observed.capability_id =
        core::CapabilityId("execution.feedback.state_snapshots");
    observed.capability_definition_revision =
        core::CapabilityDefinitionRevision(
            std::string(feedback->capability_definition_revision));
    observed.control_path->control_binding_id =
        core::ControlBindingId("arm.feedback.control-binding");
    observed.control_path->runtime_scale_qualification.reset();
    auto representative =
        core::verify_runtime_participant_binding(std::move(observed));
    ASSERT_TRUE(representative.ok()) << representative.error().message;
    ASSERT_TRUE(retained_bindings_share_backend_owner(
        arm_binding, representative.value()));
    runtime_input.control_binding_representatives =
        {arm_binding, gripper_binding, representative.value()};
  }
  // Q37: a Deployment-authored fault profile (as the ROS host reads it) is
  // the motion target's fault profile; only its load-time defaults resolve.
  core::MotionInterruptStopProfileDTO authored_fault;
  authored_fault.profile_id = "fault_spring_back_hold";
  authored_fault.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  if (check == Lite6AggregateCheck::kDeploymentFastFaultStopProfile) {
    authored_fault.profile_id = "fault_spring_back_fast";
    authored_fault.stop_mode = core::MotionInterruptStopMode::kSpringBackFast;
  }
  authored_fault.jerk_limits_enabled = true;
  for (const auto &resource : arm_binding.value().resources)
    authored_fault.per_joint_max_deceleration.emplace(
        *resource.source_joint_name,
        core::CoordinateAccelerationDTO{core::AngularAccelerationDTO{7.0}});
  auto expected_arm_fault_limits = authored_fault.per_joint_max_deceleration;
  if (authored_fault_check) {
    if (check != Lite6AggregateCheck::kDeploymentFaultStopProfile &&
        check != Lite6AggregateCheck::kDeploymentFastFaultStopProfile) {
      // The actual UR10e component profile has these six additional gripper
      // joints. This owning constructor fixture uses its real Lite6 arm
      // binding; it does not relabel that model as UR10e.
      for (const auto *joint : {"finger_joint", "left_inner_finger_joint",
                                "left_inner_knuckle_joint", "right_inner_finger_joint",
                                "right_inner_knuckle_joint", "right_outer_knuckle_joint"}) {
        authored_fault.per_joint_max_deceleration.emplace(
            joint, core::AngularAccelerationDTO{13.0});
      }
    }
    if (check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithExtraJoints ||
        check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithOnlyExtraSettleOverrides ||
        check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithSparseSettleOverrides) {
      authored_fault.per_joint_settle_velocity_threshold.emplace();
      for (const auto &[joint, _] : authored_fault.per_joint_max_deceleration) {
        const bool arm = expected_arm_fault_limits.contains(joint);
        authored_fault.per_joint_max_jerk.emplace(
            joint, core::AngularJerkDTO{arm ? 11.0 : 23.0});
        if (!arm || check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithExtraJoints ||
            (check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithSparseSettleOverrides &&
             joint == *arm_binding.value().resources.front().source_joint_name))
          authored_fault.per_joint_settle_velocity_threshold->emplace(
              joint, core::AngularVelocityDTO{arm ? 0.005 : 0.004});
      }
    }
    if (check == Lite6AggregateCheck::kDeploymentFaultStopProfileMissingArmJoint) {
      const auto missing = *arm_binding.value().resources.front().source_joint_name;
      authored_fault.per_joint_max_deceleration.erase(missing);
      expected_arm_fault_limits.erase(missing);
    } else if (check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithoutArmJoints) {
      for (const auto &resource : arm_binding.value().resources)
        authored_fault.per_joint_max_deceleration.erase(*resource.source_joint_name);
      expected_arm_fault_limits.clear();
    }
    runtime_input.fault_stop_profile_from_deployment = true;
    runtime_input.deployment_fault_stop_profile = authored_fault;
  }
  const auto original_authored_fault = authored_fault;
  // All constructor cases select the existing Core execution-port seam.
  core::ExecutionPortParticipant *port_participant = nullptr;
  runtime_input.motion_state_channel_config = selected_core_test_state_config(arm_binding);
  runtime_input.gripper_state_channel_config = selected_core_test_state_config(gripper_binding);
  if (authored_fault_check) {
    // These constructor checks need the same independently loaded/reloaded
    // Stop authority as the existing measured-publication fixture below.
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(package_root);
    core::FilesystemResolvedStopBasePlanRepository plans(temporary.path());
    const auto &path = *arm_binding.value().control_path;
    const auto desired = std::find_if(
        materialized.bundle->value().desired_control_configurations.begin(),
        materialized.bundle->value().desired_control_configurations.end(),
        [&](const auto &row) { return row.control_domain_id == path.control_domain_id; });
    ASSERT_NE(desired, materialized.bundle->value().desired_control_configurations.end());
    std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
    for (const auto &row : desired->binding_routes)
      routes.push_back({row.control_binding_id, row.control_binding_revision, row.control_route_id});
    const auto generation = path.active_control_configuration_generation;
    core::ActiveControlConfigurationDTO configuration{
        path.active_control_configuration_id, path.control_domain_id,
        path.control_domain_revision, std::move(routes), desired->activation_set_id,
        path.controller_topology_id, path.topology_revision,
        path.provider_runtime_unit_id, path.provider_runtime_epoch, generation.value(),
        {}, generation, core::ActiveControlConfigurationStateDTO::kReconciled};
    for (const auto &resource : arm_binding.value().resources)
      configuration.kinematic_resource_source_projections.push_back(
          {resource.resource_id, {{path.control_binding_id, path.control_binding_revision}},
           path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
           path.state_authority_id, path.state_authority_class,
           runtime_input.motion_state_channel_config->source_kind});
    std::sort(configuration.kinematic_resource_source_projections.begin(),
              configuration.kinematic_resource_source_projections.end(),
              [](const auto &left, const auto &right) {
                return left.resource_id < right.resource_id;
              });
    ASSERT_TRUE(configuration.locally_valid());
    const auto expected = deployment::load_verified_controller_lifecycle_expected_domain(
        *materialized.bundle, artifacts, *desired,
        configuration.source_runtime_unit_id, configuration.source_runtime_epoch);
    const auto plan = deployment::resolve_verified_stop_base_plan(
        *materialized.bundle, artifacts, expected, configuration, arm_binding);
    auto readiness = deployment::store_and_verify_runtime_participant_stop_readiness(
        *materialized.bundle, plan, core::ComponentLifecycleGeneration(1U),
        expected, arm_binding, plans);
    ASSERT_TRUE(readiness.ok()) << readiness.error().message;
    runtime_input.motion_stop_readiness = std::move(readiness.value());
  }
  if (check == Lite6AggregateCheck::kMissingSelectedBackend) {
    try {
      static_cast<void>(create_retained_native_runtime(
          std::move(runtime_input), std::move(runtime_config)));
      ADD_FAILURE() << "retained runtime requires an explicit selected execution backend";
    } catch (const std::invalid_argument &error) {
      EXPECT_STREQ(error.what(),
                   "retained runtime execution adapter set is incomplete or owns a second state authority");
    }
    EXPECT_EQ(port_participant, nullptr);
    return;
  }
  runtime_input.backend_factory = selected_core_test_backend_factory(port_participant, true);
  unsigned rebinds = 0, enabled = 0, retired = 0, reentrant_reads = 0;
  const bool moving_budget_check =
      check == Lite6AggregateCheck::kMovingBudgetPublication;
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  if (maintained_product_check) {
    runtime_input.participant_resolver_factory = [package_root,
                                                  bundle = materialized.bundle,
                                                  model_map](auto composed) {
      auto resolver = std::make_shared<ProductBackedRuntimeParticipantResolver>(
          package_root, bundle, model_map, std::move(composed),
          std::vector<core::ProgramBindingId>{});
      return RetainedRuntimeParticipantResolver{
          [resolver](const auto &operation, auto index) {
            return resolver->ResolveExecutionParticipant(operation, index);
          }};
    };
  }
#endif
  const auto configured_collision_monitor_period_ns =
      runtime_config.kernel.collision_monitor_period_ns;
  const auto configured_immediate_query_budget_ns =
      runtime_config.kernel.runtime_collision_immediate_query_budget_ns;
  const auto configured_dynamic_body_max_age_cap_ns =
      runtime_config.kernel.runtime_dynamic_body_max_age_cap_ns;
  auto runtime =
      (moving_budget_check || mixed_output_check || geometric_host_check)
          ? create_retained_native_runtime_participants(
                std::move(runtime_input), std::move(runtime_config))
          : create_retained_native_runtime(std::move(runtime_input),
                                           std::move(runtime_config));
  if (geometric_host_check) {
    runtime->collision_monitor_period_ns =
        configured_collision_monitor_period_ns;
    runtime->runtime_collision_immediate_query_budget_ns =
        configured_immediate_query_budget_ns;
    runtime->runtime_dynamic_body_max_age_cap_ns =
        configured_dynamic_body_max_age_cap_ns > 0
            ? std::optional<core::Duration>(
                  configured_dynamic_body_max_age_cap_ns)
            : std::nullopt;
  }
  // Match managed composition: one stable cell Kernel, subordinate engines,
  // and an already sealed cell policy in its current registration snapshot.
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  std::shared_ptr<host_detail::IoProviderDriverRecord> mixed_driver;
  std::shared_ptr<std::vector<host_detail::HostIoOutputChannelEntry>>
      mixed_channels;
  std::function<void()> dispatch_mixed_output;
  std::optional<core::ComponentLifecycleGeneration> mixed_arm_generation;
#endif
  std::unique_ptr<RetainedNativeCellRuntime> budget_cell;
  if (moving_budget_check || mixed_output_check || geometric_host_check) {
    auto rows = runtime->participant_registrations;
    if (moving_budget_check)
      for (auto &row : rows) {
        row.motion_takeover_preparation_budget_s =
            row.binding.value().capability_id.value() ==
                    "execution.trajectory.execute"
                ? std::optional<double>(3.5)
                : std::nullopt;
      }
    if (moving_budget_check)
      runtime->participant_resolver_factory = [](auto) {
        return RetainedRuntimeParticipantResolver{
            [](const core::OperationIntentDTO &, std::size_t) {
              return core::Result<RetainedResolvedParticipant>::Err(
                  {"unused_route_fixture",
                   "route fixture dispatches no operations",
                   {}});
            }};
      };
    const auto resolver = maintained_product_check
                              ? runtime->participant_resolver
                              : make_retained_cell_kernel_participant_resolver(
                                    runtime->participant_resolver_factory(
                                        {runtime->motion_binding}),
                                    rows);
    RetainedNativeCellRuntimeInput cell_input{
        runtime->clock,
        runtime->scheduler,
        temporary.path() / "budget-cell.sqlite3",
        std::move(rows),
        core::ResourceAddressedExecutionKernelConfig{
            owner_epoch, materialized.bundle->value().cell_graph.cell_id, 1024U,
            resolver, &admission_gate,
            materialized.bundle->value().cell_graph.graph_revision},
        runtime->stop_surveillance_owner->SnapshotSources()};
    budget_cell =
        std::make_unique<RetainedNativeCellRuntime>(std::move(cell_input));
    runtime->cell_runtime_owner = budget_cell.get();
    runtime->stop_surveillance_owner = budget_cell->stop_surveillance_owner();
    runtime->BindCellRuntimeOwner(
        *budget_cell, "budget-cell",
        materialized.bundle->value().cell_graph.cell_id.value());
  }
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
  if (geometric_host_check) {
    const auto take = []<class T>(core::Result<T> value) {
      if (!value.ok())
        throw std::logic_error(value.error().code + ": " +
                               value.error().message);
      return std::move(value.value());
    };
    const core::ControllableComponentId station("station_io");
    const auto catalog =
        take(deployment::ResolveRetainedIoSignalCatalog(graph));
    ASSERT_EQ(catalog.size(), 1U);
    const auto &signal = catalog.front();
    EXPECT_EQ(signal.provider.profile, "geometric_contact");
    ASSERT_TRUE(signal.provider.endpoint);
    EXPECT_EQ(signal.provider.endpoint->controllable_component_id,
              component_id);
    EXPECT_EQ(signal.provider.endpoint->command_target_id, gripper_target_id);
    EXPECT_EQ(signal.provider.endpoint->frame_role_id,
              core::FrameRoleId("tcp"));
    const auto &domain = require_fixture_one(
        graph.control_domains,
        [&](const auto &row) {
          return row.provider() &&
                 row.provider()->controllable_component_id == station;
        },
        "actual geometric provider domain");
    const auto &provider = *domain.provider();
    deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(
        package_root);
    const auto manifest = manifests.get_verified(
        {materialized.bundle->retained_bundle_id(),
         provider.provider_manifest_id, provider.provider_manifest_revision});
    ASSERT_TRUE(manifest);
    const core::RuntimeEpoch station_epoch("geometric.actual.station.driver");
    auto driver = take(host_detail::IoProviderDriverRecord::Create(
        {domain.control_domain_id, domain.control_domain_revision,
         core::ResolvedProviderDomainScopeDTO{provider, station_epoch, {}, {}}},
        {domain.control_domain_id, domain.control_domain_revision,
         provider.provider_instance_id, provider.provider_manifest_id,
         provider.provider_manifest_revision, provider.provider_runtime_unit_id,
         station_epoch, provider.lifecycle_plan_catalog_digest},
        manifest->value(), runtime->clock, 1'000'000'000LL));
    auto drivers = std::make_shared<const host_detail::IoProviderDriverGroup>(
        host_detail::IoProviderDriverGroup{driver});

    const core::ProjectionRevisionDTO product_projection{
        materialized.bundle->retained_bundle_id(), graph.graph_revision,
        initial_scene.shared()->revision};
    constexpr std::uint64_t kPublicationGeneration = 1U;
    auto composition = std::make_unique<host_detail::Composition>(
        component_id, materialized.bundle, nullptr, nullptr, nullptr, nullptr,
        runtime->motion_binding, runtime->gripper_binding, std::nullopt,
        deployment::CanonicalValue(deployment::CanonicalValue::Object{}),
        deployment::CanonicalValue(deployment::CanonicalValue::Object{}),
        nullptr, runtime, product_projection,
        core::ActiveBundlePublicationGeneration(kPublicationGeneration),
        std::nullopt, std::nullopt);
    composition->cell_runtime = budget_cell.get();
    composition->target_resolver =
        std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, materialized.bundle, model_map,
            std::vector<core::RuntimeParticipantBinding>{
                runtime->motion_binding, *runtime->gripper_binding},
            std::vector<core::ProgramBindingId>{});
    deployment::FilesystemCatalogArtifactStore artifacts(package_root);
    composition->direct_control_io_program_bindings =
        deployment::compile_direct_control_program_binding_inventory(
            *materialized.bundle, model_map, artifacts)
            .io;
    auto station_view = std::make_unique<host_detail::Composition>(
        station, materialized.bundle, nullptr, nullptr, nullptr, nullptr,
        std::nullopt, std::nullopt, std::nullopt,
        deployment::CanonicalValue(deployment::CanonicalValue::Object{}),
        deployment::CanonicalValue(deployment::CanonicalValue::Object{}),
        nullptr, nullptr, product_projection,
        core::ActiveBundlePublicationGeneration(kPublicationGeneration),
        std::nullopt, std::nullopt);
    station_view->cell_runtime = budget_cell.get();
    station_view->io_provider_drivers = drivers;
    station_view->component_runtime_pins =
        std::make_shared<const host_detail::ComponentRuntimePins>(
            host_detail::ComponentRuntimePins{nullptr, nullptr, drivers});
    composition->participant_compositions.push_back(std::move(station_view));

    const auto prepared = host_detail::PrepareCompositionIoProviderResources(
        *composition, *budget_cell, {}, {});
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    ASSERT_NE(composition->io_signal_runtime, nullptr);
    const auto &installations =
        composition->io_signal_runtime->driver_input_installations();
    ASSERT_EQ(installations.size(), 1U);
    ASSERT_EQ(installations.begin()->second.size(), 1U);
    const auto &geometric =
        installations.begin()->second.front().geometric_contact;
    ASSERT_TRUE(geometric);
    EXPECT_EQ(geometric->endpoint.command_target_id, gripper_target_id);
    ASSERT_EQ(geometric->endpoint.contact_body_bindings.size(), 1U);
    EXPECT_EQ(geometric->endpoint.contact_body_bindings.front()
                  .collision_body_role_id,
              core::CollisionBodyRoleId("gripper_fingers"));
    EXPECT_EQ(geometric->observed_states.size(), 2U);
    const auto query =
        std::dynamic_pointer_cast<collision::ManyForgeCollisionEngine>(
            geometric->collision_engine);
    ASSERT_NE(query, nullptr);
    EXPECT_TRUE(query->runtime_query_covers_full_configuration());

    composition->io_signal_runtime->Start();
    composition->io_signal_service = composition->io_signal_runtime->service();
    std::uint64_t lifecycle_sequence = 0U;
    const auto step = [&](std::string_view primitive) {
      const auto sequence = ++lifecycle_sequence;
      const auto snapshot = take(driver->ReadLifecycleSnapshot(
          domain.control_domain_id, runtime->clock->now()));
      const auto &actual = driver->occurrence();
      const core::BrandLifecyclePrimitiveIntentDTO intent{
          core::LifecycleTransactionId("geometric.driver." +
                                       std::string(primitive) + "." +
                                       std::to_string(sequence)),
          core::LifecycleTransactionGeneration(sequence),
          station,
          actual.control_domain_id,
          actual.control_domain_revision,
          actual.provider_instance_id,
          actual.provider_manifest_id,
          actual.provider_manifest_revision,
          actual.provider_runtime_unit_id,
          actual.provider_runtime_epoch,
          core::LifecyclePrimitiveId("io.provider." + std::string(primitive)),
          core::LifecyclePrimitiveAttemptId("geometric.attempt." +
                                            std::string(primitive) + "." +
                                            std::to_string(sequence)),
          snapshot.source_generation,
          1'000'000'000LL};
      const auto outcome = driver->Dispatch(intent, {});
      ASSERT_EQ(outcome.known,
                core::LifecyclePrimitiveKnownResultDTO::kSucceeded)
          << (outcome.error ? outcome.error->message : "missing result");
    };
    const core::IoSignalId signal_id{station, "probe_contact"};
    EXPECT_EQ(take(composition->io_signal_service->ReadSignal(signal_id))
                  .availability,
              core::IoSignalAvailability::kMissing);
    step("prepare");
    step("enable");
    ASSERT_TRUE(composition->io_signal_runtime->DrainPending().ok());
    const auto contact =
        take(composition->io_signal_service->ReadSignal(signal_id));
    ASSERT_EQ(contact.availability, core::IoSignalAvailability::kFresh);
    ASSERT_TRUE(contact.latest_sample);
    // Compare the contained level; bool conversion only tests optional presence.
    EXPECT_EQ(contact.latest_sample->publication.value,
              std::optional<bool>(true));
    const auto initial_bundle =
        geometric->collision_publisher
            ->published_runtime_collision_query_bundle();
    ASSERT_NE(initial_bundle, nullptr);
    ASSERT_NE(initial_bundle->world, nullptr);
    const auto target_body_id = core::make_scene_collision_body_id(
        core::SceneResourceId("geometric_target"), 0U);
    const auto &initial_target = require_fixture_one(
        initial_bundle->world->bodies,
        [&](const auto &body) { return body.body_id == target_body_id; },
        "initial geometric target collision body");
    EXPECT_DOUBLE_EQ(initial_target.pose.position_m.x_m, 0.0);
    const auto initial_world_revision = initial_bundle->world->revision;
    const auto initial_prepared_scene = query->internal_prepared_scene_identity();
    ASSERT_NE(initial_prepared_scene, nullptr);
    const auto initial_scene_build_count =
        query->internal_prepared_scene_build_count();
    const auto initial_source_sequence =
        contact.latest_sample->publication.source_sequence;

    step("disable");
    auto separated_scene = *projection->logical_snapshot();
    ASSERT_EQ(separated_scene.objects.size(), 1U);
    const auto prior_revision = product_projection;
    separated_scene.objects.front().placement =
        core::CellFixedSceneObjectPlacementDTO{
            graph.cell_id, {{100.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}};
    ++separated_scene.transaction_sequence;
    ++separated_scene.topology_epoch;
    ++separated_scene.revision.revision_id;
    separated_scene.revision.content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(
            separated_scene);
    const core::ProjectionRevisionDTO separated_revision{
        materialized.bundle->retained_bundle_id(), graph.graph_revision,
        separated_scene.revision};
    const core::SceneTransactionDTO separation{
        core::RequestId("geometric-separate-target"),
        {},
        separated_scene.transaction_sequence,
        prior_revision,
        separated_revision,
        core::ContentDigest(std::string(kZeroDigest)),
        core::ResolvedUpsertSceneObjectDTO{separated_scene.objects.front()},
        core::ContentDigest(std::string(kZeroDigest))};
    const auto moved = projection->ApplyTransaction(separation);
    ASSERT_EQ(moved.result, core::ProjectionCommitResultStateDTO::kCommitted);
    ASSERT_NE(projection->logical_snapshot(), nullptr);
    EXPECT_EQ(projection->logical_snapshot()->revision,
              separated_scene.revision);
    const auto &logical_target = require_fixture_one(
        projection->logical_snapshot()->objects,
        [&](const auto &object) {
          return object.scene_resource_id ==
                 core::SceneResourceId("geometric_target");
        },
        "moved logical geometric target");
    const auto *logical_fixed =
        std::get_if<core::CellFixedSceneObjectPlacementDTO>(
            &logical_target.placement);
    ASSERT_NE(logical_fixed, nullptr);
    EXPECT_DOUBLE_EQ(logical_fixed->pose_in_cell.position_m.x_m, 100.0);
    const auto separated_bundle =
        geometric->collision_publisher
            ->published_runtime_collision_query_bundle();
    ASSERT_NE(separated_bundle, nullptr);
    ASSERT_NE(separated_bundle->world, nullptr);
    EXPECT_NE(separated_bundle, initial_bundle);
    EXPECT_GT(separated_bundle->world->revision.revision_id,
              initial_world_revision.revision_id);
    EXPECT_NE(separated_bundle->world->revision.content_hash,
              initial_world_revision.content_hash);
    const auto &separated_target = require_fixture_one(
        separated_bundle->world->bodies,
        [&](const auto &body) { return body.body_id == target_body_id; },
        "moved geometric target collision body");
    EXPECT_DOUBLE_EQ(separated_target.pose.position_m.x_m, 100.0);
    step("prepare");
    step("enable");
    ASSERT_TRUE(composition->io_signal_runtime->DrainPending().ok());
    const auto separated =
        take(composition->io_signal_service->ReadSignal(signal_id));
    ASSERT_EQ(separated.availability, core::IoSignalAvailability::kFresh);
    ASSERT_TRUE(separated.latest_sample);
    EXPECT_GT(separated.latest_sample->publication.source_sequence,
              initial_source_sequence);
    EXPECT_NE(query->internal_prepared_scene_identity(),
              initial_prepared_scene);
    EXPECT_GT(query->internal_prepared_scene_build_count(),
              initial_scene_build_count);
    EXPECT_EQ(separated.latest_sample->publication.value,
              std::optional<bool>(false));
    composition->io_signal_runtime->Shutdown();
    driver->Shutdown();
    return;
  }

  if (mixed_output_check) {
    // Same genuine maintained arm + model-free station product as the process
    // trial. The current snapshot supplies the construction generation; no
    // component Active state or Kernel output admission is fabricated here.
    const auto take = []<class T>(core::Result<T> value) {
      if (!value.ok()) {
        const auto &error = value.error();
        const auto reason = error.details.find("reason");
        throw std::logic_error(error.code + ": " + error.message +
                               (reason == error.details.end()
                                    ? std::string{}
                                    : " [" + reason->second + "]"));
      }
      return std::move(value.value());
    };
    const core::ControllableComponentId station("station_io");
    const auto catalog = take(deployment::ResolveRetainedIoSignalCatalog(graph));
    const auto& domain = require_fixture_one(graph.control_domains, [&](const auto& row) {
      return row.provider() && row.provider()->controllable_component_id == station;
    }, "actual station provider domain");
    const auto& provider = *domain.provider();
    deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(package_root);
    const auto manifest = manifests.get_verified({materialized.bundle->retained_bundle_id(),
        provider.provider_manifest_id, provider.provider_manifest_revision});
    ASSERT_TRUE(manifest);
    std::vector<core::ResourceId> resources;
    std::vector<core::IoSignalDescriptorDTO> descriptors;
    std::map<core::InterruptSourceId, std::vector<deployment::RetainedIoSignalConfiguration>> input_rows;
    for (const auto& row : catalog) {
      if (row.descriptor.signal_id.controllable_component_id != station) continue;
      if (row.descriptor.output_resource) resources.push_back(*row.descriptor.output_resource);
      else {
        descriptors.push_back(row.descriptor);
        input_rows[row.descriptor.source_id].push_back(row);
      }
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    ASSERT_FALSE(resources.empty());
    const core::RuntimeEpoch station_epoch("mixed.actual.station.driver");
    const core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
        domain.control_domain_id, domain.control_domain_revision, provider.provider_instance_id,
        provider.provider_manifest_id, provider.provider_manifest_revision,
        provider.provider_runtime_unit_id, station_epoch, provider.lifecycle_plan_catalog_digest};
    mixed_driver = take(host_detail::IoProviderDriverRecord::Create(
        {domain.control_domain_id, domain.control_domain_revision,
         core::ResolvedProviderDomainScopeDTO{provider, station_epoch, {}, resources}},
        occurrence, manifest->value(), runtime->clock, 1'000'000'000LL));

    component_manager::SqliteActiveBundlePublicationStore publications(
        temporary.path() / "mixed-active.sqlite3", graph.cell_id);
    const core::ProjectionRevisionDTO product_projection{
        materialized.bundle->retained_bundle_id(), graph.graph_revision, initial_scene.shared()->revision};
    const auto publication = publications.PublishActiveBundle(
        {core::BootstrapPublicationTransactionDTO{core::BootstrapId("mixed.bootstrap"),
                                                core::BootstrapGeneration(1U)},
         product_projection, materialized.bundle->retained_bundle_id(), graph.graph_revision, {},
         core::ContentDigest(deployment::sha256_digest(materialized.bundle->canonical_bytes()))});
    core::SqliteCCLCJournal lifecycle_journal(
        temporary.path() / "mixed-cclc.sqlite3", graph.cell_id, 8U);
    ASSERT_TRUE(lifecycle_journal.BeginOwnerEpoch(
        core::RequestOwnerEpoch("mixed.cclc.owner"), runtime->clock->now(), 32U).ok());
    std::vector<core::BuildOwnedLifecyclePlanCatalogEntryDTO> lifecycle_entries{
        {manifest->value(), mixed_driver->catalog()}};
    std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> occurrences{mixed_driver->occurrence()};
    for (const auto& registration : runtime->participant_registrations) {
      const auto& path = *registration.binding.value().control_path;
      if (std::any_of(occurrences.begin(), occurrences.end(), [&](const auto& row) {
            return row.control_domain_id == path.control_domain_id;
          })) continue;
      const auto physical_manifest = manifests.get_verified({materialized.bundle->retained_bundle_id(),
          path.provider_manifest_id, path.provider_manifest_revision});
      ASSERT_TRUE(physical_manifest);
      auto physical_catalog = take(make_retained_ros2_control_lifecycle_catalog(physical_manifest->value()));
      occurrences.push_back({path.control_domain_id, path.control_domain_revision, path.provider_instance_id,
          path.provider_manifest_id, path.provider_manifest_revision, path.provider_runtime_unit_id,
          path.provider_runtime_epoch, physical_catalog.catalog_digest});
      if (std::none_of(lifecycle_entries.begin(), lifecycle_entries.end(), [&](const auto& row) {
            return row.provider_manifest.provider_manifest_id == path.provider_manifest_id &&
                row.provider_manifest.provider_manifest_revision == path.provider_manifest_revision;
          })) lifecycle_entries.push_back({physical_manifest->value(), std::move(physical_catalog)});
    }
    auto lifecycle_catalog = take(core::BuildOwnedLifecyclePlanCatalogSource::Make(std::move(lifecycle_entries)));
    deployment::VerifiedComponentLifecycleCurrentSnapshotSource current_source(
        *materialized.bundle, publications, publication, lifecycle_journal,
        lifecycle_catalog, std::move(occurrences));
    const auto actual_arm = take(current_source.ReadCurrentComponentSnapshot(component_id));
    ASSERT_EQ(actual_arm.component_lifecycle_state, core::ComponentLifecycleStateDTO::kDeclared);
    mixed_arm_generation = actual_arm.component_lifecycle_generation;
    const auto actual = take(current_source.ReadCurrentComponentSnapshot(station));
    ASSERT_EQ(actual.component_lifecycle_state, core::ComponentLifecycleStateDTO::kDeclared);
    ASSERT_EQ(actual.domains.size(), 1U);
    ASSERT_EQ(actual.domains.front(), mixed_driver->domain());
    ASSERT_EQ(actual.provider_occurrences.front(), mixed_driver->occurrence());

    std::vector<core::InterruptSourceId> synthetic_sources;
    for (const auto& [source, rows] : input_rows)
      if (rows.front().provider.profile == "mock") synthetic_sources.push_back(source);
    std::shared_ptr<core::IoSignalSubsystem> inputs(take(core::IoSignalSubsystem::Create(
        {owner_epoch, 1U, descriptors, std::move(synthetic_sources)},
        {input_rows.size(), descriptors.size(), 16U, 8U, 512U, 1024U}, *runtime->clock)));
    std::map<core::InterruptSourceId, core::IoSourceIncarnation> incarnations;
    for (const auto& [source, rows] : input_rows) {
      const core::IoSourceIncarnation incarnation("mixed." + source.value());
      ASSERT_TRUE(inputs->InstallSource(source, incarnation, {}).ok());
      incarnations.emplace(source, incarnation);
    }
    std::shared_ptr<core::IoSignalIngress> ingress(take(core::IoSignalIngress::Create(inputs)));
    std::vector<host_detail::IoProviderInputSourceInstallation> installed_inputs;
    for (const auto& [source, rows] : input_rows)
      installed_inputs.push_back(
          {rows, {}, ingress, incarnations.at(source), std::nullopt});
    std::vector<host_detail::HostIoOutputAliasInstallation> aliases;
    {
      auto delivery = mixed_driver->AcquireDelivery();
      for (const auto& row : catalog)
        if (row.descriptor.signal_id.controllable_component_id == station && row.descriptor.output_resource)
          aliases.push_back({row, mixed_driver->occurrence(),
              mixed_driver->driver_observation_generation_locked(), mixed_driver,
              publication.publication_generation});
    }
    mixed_channels = std::make_shared<std::vector<host_detail::HostIoOutputChannelEntry>>(
        take(host_detail::CreateHostIoOutputChannels(std::move(aliases), {}, *runtime->clock)));
    std::vector<host_detail::IoProviderOutputChannelInstallation> installed_outputs;
    for (const auto& channel : *mixed_channels) {
      std::vector<deployment::RetainedIoSignalConfiguration> rows;
      for (const auto& row : catalog)
        if (row.descriptor.signal_id.controllable_component_id == station &&
            row.descriptor.output_resource == channel.resource_id) rows.push_back(row);
      installed_outputs.push_back({channel.resource_id, std::move(rows), channel.provider});
    }
    {
      auto delivery = mixed_driver->AcquireDelivery();
      ASSERT_TRUE(mixed_driver->InstallResourcesLocked(
          std::move(installed_inputs), std::move(installed_outputs), {}).ok());
    }
    for (const auto* primitive : {"prepare", "enable"}) {
      SCOPED_TRACE(primitive);
      const auto before = take(mixed_driver->ReadLifecycleSnapshot(
          domain.control_domain_id, runtime->clock->now()));
      const auto& selected = mixed_driver->occurrence();
      const core::BrandLifecyclePrimitiveIntentDTO intent{
          core::LifecycleTransactionId("mixed.driver." + std::string(primitive)),
          core::LifecycleTransactionGeneration(1U), station, domain.control_domain_id,
          selected.control_domain_revision, selected.provider_instance_id, selected.provider_manifest_id,
          selected.provider_manifest_revision, selected.provider_runtime_unit_id,
          selected.provider_runtime_epoch, core::LifecyclePrimitiveId("io.provider." + std::string(primitive)),
          core::LifecyclePrimitiveAttemptId("mixed.attempt." + std::string(primitive)),
          before.source_generation, 1'000'000'000LL};
      ASSERT_TRUE(intent.locally_valid());
      const auto outcome = mixed_driver->Dispatch(intent, {});
      ASSERT_EQ(outcome.known, core::LifecyclePrimitiveKnownResultDTO::kSucceeded)
          << (outcome.error ? outcome.error->code + ": " + outcome.error->message
                            : "no lifecycle error returned");
    }
    EXPECT_EQ(take(current_source.ReadCurrentComponentSnapshot(station)).component_lifecycle_state,
              core::ComponentLifecycleStateDTO::kDeclared);

    deployment::FilesystemCatalogArtifactStore artifacts(package_root);
    const auto direct = deployment::compile_direct_control_program_binding_inventory(
        *materialized.bundle, model_map, artifacts);
    const auto binding = require_fixture_one(direct.io, [&](const auto& row) {
      return std::any_of(row->value().target_role_bindings.begin(), row->value().target_role_bindings.end(),
          [&](const auto& target) { return target.controllable_component_id == station; });
    }, "genuine station direct output binding");
    const auto& target = require_fixture_one(binding->value().target_role_bindings,
        [&](const auto& row) { return row.controllable_component_id == station; }, "station target");
    const auto sealed_target = deployment::resolve_runtime_program_target(
        *binding, target.target_role_id, core::CapabilityId("io.gpio.set_output"),
        *materialized.bundle, model_map);
    const core::RuntimeProviderDomainConfigurationReadbackDTO closed{
        materialized.bundle->retained_bundle_id(), graph.graph_revision,
        actual.component_lifecycle_generation, actual.domains.front(), actual.provider_occurrences.front(),
        false, 0U, 0U};
    auto complete = budget_cell->participant_registrations();
    for (const auto& role : target.signal_role_bindings) {
      if (role.direction != core::IoSignalDirection::kOutput) continue;
      auto output = take(core::verify_runtime_participant_binding(
          deployment::build_runtime_io_output_binding_dto(
              sealed_target, role.resource_role_id, *materialized.bundle, closed)));
      const auto& channel = require_fixture_one(*mixed_channels, [&](const auto& row) {
        return row.resource_id == *output.value().io_output_binding->signal.output_resource;
      }, "actual station output channel engine");
      complete.push_back({.binding = std::move(output), .stop_readiness = {},
          .retained_default_stop_limits = {}, .stop_node_category = core::MotionInterruptNodeCategory::kGeneric,
          .engine = channel.engine.get(), .stop_execution_port = nullptr, .participant_policy = {},
          .prepared_motion_engine = channel.engine.get(), .default_operator_stop_profile = {},
          .default_fault_stop_profile = {}, .deployment_maximum_tcp_linear_speed_mps = {},
          .intrinsic_direct_control = true, .direct_control_joint_limits = {}});
    }
    RetainedRuntimeParticipantResolverFactory current_factory =
        [package_root, bundle = materialized.bundle, model_map, direct_id = binding->program_binding_id()]
        (auto composed) {
          auto resolver = std::make_shared<ProductBackedRuntimeParticipantResolver>(
              package_root, bundle, model_map, std::move(composed),
              std::vector<core::ProgramBindingId>{direct_id});
          return RetainedRuntimeParticipantResolver{
              [resolver](const auto& operation, auto index) {
                return resolver->ResolveExecutionParticipant(operation, index);
              }};
        };
    std::vector<core::RuntimeParticipantBinding> all_bindings;
    for (const auto& row : complete) all_bindings.push_back(row.binding);
    auto resolver = make_retained_cell_kernel_participant_resolver(
        current_factory(std::move(all_bindings)), complete);
    ASSERT_TRUE(budget_cell->addressed_kernel().SetRouteRecoveryPending(true).ok());
    ASSERT_TRUE(budget_cell->reconfigure_runtime_product(
        std::move(complete), std::move(resolver), budget_cell->stop_surveillance_owner()->SnapshotSources()).ok());
    // The current immutable product factory is adopted only after the same
    // successful stable-cell publication, as in the actual host caller.
    runtime->AdoptVerifiedParticipantResolverFactory(std::move(current_factory));
    ASSERT_TRUE(budget_cell->addressed_kernel().SetRouteRecoveryPending(false).ok());
    auto* const stable = budget_cell.get();
    dispatch_mixed_output = [stable, binding, driver = mixed_driver, channels = mixed_channels,
        expected_generation = actual.component_lifecycle_generation, sequence = 0U]() mutable {
      const auto& selected = require_fixture_one(stable->participant_registrations(), [&](const auto& row) {
        return row.binding.value().io_output_binding &&
            row.binding.value().controllable_component_id == core::ControllableComponentId("station_io") &&
            row.binding.value().io_output_binding->resource_role_id == core::ResourceRoleId("lamp");
      }, "retained genuine lamp registration");
      EXPECT_EQ(selected.binding.value().io_output_binding->component_lifecycle_generation, expected_generation);
      const auto& target = binding->value().target_role_bindings.front();
      const auto schema = core::builtin_io_signal_write_command_schema();
      const core::CapabilityCommandIntentDTO command{schema.command_schema_id, schema.command_schema_revision,
          core::IoSignalWriteCommandValueDTO{core::ResourceRoleId("lamp"), (++sequence % 2U) != 0U}, {}, {}};
      const core::OperationIntentDTO operation{
          {core::RequestId("mixed.output." + std::to_string(sequence)), stable->addressed_kernel().request_owner_epoch(), "test", {}},
          binding->value().program_id, binding->value().program_revision, binding->program_binding_id(),
          core::ProgramRunId("mixed.output.session"), selected.binding.value().graph_revision,
          selected.binding.value().retained_bundle_id, {},
          {{target.component_role_id, target.target_role_id, core::CapabilityId("io.gpio.set_output"), {}, command, {}}}};
      const auto resolved = stable->participant_resolver()(operation, 0U);
      ASSERT_TRUE(resolved.ok()) << resolved.error().message;
      ASSERT_EQ(resolved.value().engine, selected.engine);
      ASSERT_EQ(resolved.value().prepared_motion_engine, selected.prepared_motion_engine);
      EXPECT_EQ(resolved.value().binding.value().io_output_binding, selected.binding.value().io_output_binding);
      ASSERT_FALSE(resolved.value().binding.value().control_path);
      const auto prepared = core::MakePreparedIoOutputParticipant(
          core::OperationId("mixed.write." + std::to_string(sequence)), core::OperationRevision(1U),
          core::OperationParticipantId("lamp"), operation.program_run_id, command, resolved.value().binding);
      ASSERT_TRUE(prepared.ok()) << prepared.error().message;
      auto participant = std::make_shared<const core::PreparedOperationParticipantDTO>(prepared.value());
      const auto& output = std::get<core::PreparedIoOutputCommandDTO>(participant->payload);
      auto* const engine = resolved.value().prepared_motion_engine;
      const auto prepared_id = engine->PrepareCapabilityCommand({participant, resolved.value().binding,
          core::ResolvedPreparedIoOutputCommandAuthorityDTO{output.signal_id, output.output_resource,
              output.command_value.value, participant->provider()->lifecycle_provider, output.completion_policy}});
      ASSERT_TRUE(prepared_id.ok()) << prepared_id.error().message;
      const auto sent = engine->DispatchPreparedCapabilityCommand(prepared_id.value(), participant->prepared_contract_digest,
          core::ControllerGoalId("mixed.goal." + std::to_string(sequence)), core::ControllerGoalGeneration(1U));
      ASSERT_TRUE(sent.ok()) << sent.error().message;
      ASSERT_TRUE(sent.value().accepted);
      const auto observed = engine->ObservePreparedCapabilityCommand(prepared_id.value());
      ASSERT_TRUE(observed.ok());
      ASSERT_TRUE(observed.value().output_observation);
      EXPECT_EQ(observed.value().output_observation->evidence, core::IoOutputEvidence::kSourceAccepted);
      EXPECT_EQ(observed.value().output_observation->identity.component_lifecycle_generation, expected_generation);
      EXPECT_EQ(observed.value().output_observation->identity.provider, driver->occurrence());
      EXPECT_TRUE(resolved.value().engine->acknowledge_completed(prepared_id.value()).accepted);
      // Keep the actual leaf/engine and driver alive through the dispatch.
      EXPECT_FALSE(channels->empty());
    };
  }
#endif
  if (check == Lite6AggregateCheck::kExecutionPortParticipant) {
    ASSERT_NE(runtime, nullptr);
    ASSERT_NE(port_participant, nullptr);
    EXPECT_EQ(runtime->execution.get(), port_participant);
    std::size_t motion_registrations = 0U;
    for (const auto &registration : runtime->participant_registrations) {
      if (registration.binding.value().command_target_id != arm_target_id) continue;
      ++motion_registrations;
      // The participant Kernel drives it; its Stop stages reach the helper.
      EXPECT_EQ(registration.engine, runtime->participant_engine.get());
      EXPECT_EQ(registration.prepared_motion_engine, runtime->participant_engine.get());
      EXPECT_EQ(registration.stop_execution_port,
                static_cast<core::IControlDomainExecutionPort *>(port_participant));
    }
    EXPECT_GE(motion_registrations, 1U);
    return;
  }

  ASSERT_NE(runtime, nullptr);
  ASSERT_NE(runtime->active_addressed_kernel(), nullptr);
  ASSERT_NE(runtime->participant_engine, nullptr);
  ASSERT_TRUE(runtime->gripper_binding.has_value());
  if (check == Lite6AggregateCheck::kMeasuredPublication ||
      check == Lite6AggregateCheck::kResolverRebuildFailures ||
      check == Lite6AggregateCheck::kStopResponseFollowsRouteRebind ||
      check == Lite6AggregateCheck::kRetainedRouteGenerationReseal ||
      moving_budget_check || mixed_output_check ||
      check == Lite6AggregateCheck::kMeasuredOptionalDomainRace ||
      check == Lite6AggregateCheck::kMeasuredNullOptionalDomainRace) {
    // Use the verified ROS product and actual Kernel with the focused Core port.
    // Stop readiness is produced by the existing sealed repository boundary.
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(package_root);
    core::FilesystemResolvedStopBasePlanRepository plans(temporary.path());
    const auto make_configuration = [&](const core::RuntimeParticipantBinding &binding,
                                        core::ActiveControlConfigurationGeneration generation) {
      const auto &path = *binding.value().control_path;
      const auto desired = std::find_if(
          materialized.bundle->value().desired_control_configurations.begin(),
          materialized.bundle->value().desired_control_configurations.end(),
          [&](const auto &row) { return row.control_domain_id == path.control_domain_id; });
      if (desired == materialized.bundle->value().desired_control_configurations.end())
        throw std::runtime_error("actual fixture desired domain missing");
      std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
      for (const auto &row : desired->binding_routes)
        routes.push_back({row.control_binding_id, row.control_binding_revision, row.control_route_id});
      core::ActiveControlConfigurationDTO result{
          path.active_control_configuration_id, path.control_domain_id,
          path.control_domain_revision, std::move(routes), desired->activation_set_id,
          path.controller_topology_id, path.topology_revision,
          path.provider_runtime_unit_id, path.provider_runtime_epoch, generation.value(),
          {}, generation, core::ActiveControlConfigurationStateDTO::kReconciled};
      for (const auto &representative : runtime->control_binding_representatives) {
        const auto &selected = *representative.value().control_path;
        if (selected.control_domain_id != path.control_domain_id) continue;
        const auto channel = retained_bindings_share_backend_owner(runtime->motion_binding, representative)
                                 ? runtime->motion_state_channel : runtime->gripper_state_channel;
        for (const auto &resource : representative.value().resources) {
          core::KinematicResourceSourceProjectionDTO projection{
              resource.resource_id, {{selected.control_binding_id, selected.control_binding_revision}},
              selected.provider_instance_id, selected.provider_manifest_id,
              selected.provider_manifest_revision, selected.state_authority_id,
              selected.state_authority_class, channel->source_kind()};
          auto prior = std::find_if(result.kinematic_resource_source_projections.begin(),
              result.kinematic_resource_source_projections.end(), [&](const auto& row) {
                return row.resource_id == resource.resource_id;
              });
          if (prior == result.kinematic_resource_source_projections.end()) {
            result.kinematic_resource_source_projections.push_back(std::move(projection));
          } else {
            auto same_owner = projection;
            same_owner.contributing_control_bindings = prior->contributing_control_bindings;
            if (same_owner != *prior)
              throw std::logic_error("actual fixture resource has conflicting source owners");
            prior->contributing_control_bindings.push_back(projection.contributing_control_bindings.front());
          }
        }
      }
      std::sort(result.kinematic_resource_source_projections.begin(),
                result.kinematic_resource_source_projections.end(),
                [](const auto &a, const auto &b) { return a.resource_id < b.resource_id; });
      for (auto& projection : result.kinematic_resource_source_projections)
        std::sort(projection.contributing_control_bindings.begin(),
            projection.contributing_control_bindings.end(), [](const auto& a, const auto& b) {
              return std::tie(a.control_binding_id, a.control_binding_revision) <
                     std::tie(b.control_binding_id, b.control_binding_revision);
            });
      if (!result.locally_valid())
        throw std::logic_error("actual fixture route configuration is not canonical");
      return result;
    };
    const auto ready = [&](const core::ActiveControlConfigurationDTO &configuration) {
      const auto desired = std::find_if(
          materialized.bundle->value().desired_control_configurations.begin(),
          materialized.bundle->value().desired_control_configurations.end(),
          [&](const auto &row) { return row.control_domain_id == configuration.control_domain_id; });
      if (desired == materialized.bundle->value().desired_control_configurations.end())
        throw std::runtime_error("actual fixture desired domain missing");
      const auto expected = deployment::load_verified_controller_lifecycle_expected_domain(
          *materialized.bundle, artifacts, *desired,
          configuration.source_runtime_unit_id, configuration.source_runtime_epoch);
      std::vector<core::RuntimeParticipantBinding> bindings;
      for (const auto &binding : runtime->control_binding_representatives)
        if (binding.value().control_path->control_domain_id == configuration.control_domain_id)
          bindings.push_back(binding);
      const std::vector<core::RouteSelectionComponentExpectationDTO> components{
          {component_id, mixed_output_check
              ? runtime->active_addressed_kernel()->component_command_readiness(component_id).value().component_lifecycle_generation.value()
              : core::ComponentLifecycleGeneration(1U), mixed_output_check
              ? core::ComponentLifecycleStateDTO::kDeclared
              : core::ComponentLifecycleStateDTO::kPrepared}};
      return deployment::resolve_store_and_verify_route_readiness_batch(
          *materialized.bundle, artifacts, expected, configuration, components, bindings, plans);
    };
    const auto assert_budget = [&]() {
      if (!moving_budget_check)
        return;
      const auto &runtime_rows =
          std::as_const(runtime->participant_registrations);
      for (const auto *rows :
           {&runtime_rows, &budget_cell->participant_registrations()}) {
        ASSERT_FALSE(rows->empty());
        for (const auto &row : *rows) {
          const bool motion = row.binding.value().capability_id.value() ==
                              "execution.trajectory.execute";
          EXPECT_EQ(row.motion_takeover_preparation_budget_s,
                    motion ? std::optional<double>(3.5) : std::nullopt);
        }
      }
    };
#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
    if (mixed_output_check) {
      dispatch_mixed_output();
      ASSERT_TRUE(mixed_arm_generation);
      const core::ComponentCommandReadinessPublicationDTO publication{
          core::ComponentReadinessAuthorityDTO::kRestartExclusion, {}, {}, component_id,
          *mixed_arm_generation, false};
      EXPECT_FALSE(runtime->PublishComponentCommandReadiness(publication).ok());
      const auto initial = make_configuration(runtime->motion_binding,
          runtime->motion_binding.value().control_path->active_control_configuration_generation);
      // Unchanged predecessor adoption is allowed while drained. A valid
      // foreign owner revision must still refuse without touching IO.
      auto foreign_predecessor = initial;
      foreign_predecessor.control_domain_revision = core::ControlDomainRevision("foreign.domain.revision");
      ASSERT_TRUE(foreign_predecessor.locally_valid());
      const auto refused = runtime->AdoptAndReadBackRouteConfigurationPredecessor(foreign_predecessor);
      ASSERT_FALSE(refused.ok());
      EXPECT_EQ(refused.error().details.at("reason"), "route_predecessor_rebind_mismatch");
      dispatch_mixed_output();
      ASSERT_TRUE(budget_cell->addressed_kernel().SetRouteRecoveryPending(true).ok());
      const auto readiness = runtime->PublishComponentCommandReadiness(publication);
      ASSERT_TRUE(readiness.ok()) << readiness.error().message;
      EXPECT_FALSE(readiness.value().command_eligible);
      EXPECT_EQ(readiness.value().component_lifecycle_generation, publication.component_lifecycle_generation);
      dispatch_mixed_output();
      const auto adopted = runtime->AdoptAndReadBackRouteConfigurationPredecessor(initial);
      ASSERT_TRUE(adopted.ok()) << adopted.error().message;
      EXPECT_EQ(adopted.value().active_configuration, initial);
      EXPECT_FALSE(adopted.value().operation_admission_open);
      dispatch_mixed_output();
      const auto successor = make_configuration(runtime->motion_binding,
          core::ActiveControlConfigurationGeneration(initial.active_control_configuration_generation.value() + 1U));
      auto successor_ready = ready(successor);
      ASSERT_TRUE(successor_ready.ok()) << successor_ready.error().message;
      const auto published = runtime->PublishAndReadBackRouteConfiguration(successor, successor_ready.value());
      ASSERT_TRUE(published.ok()) << published.error().message;
      EXPECT_EQ(published.value().active_configuration, successor);
      dispatch_mixed_output();
      mixed_driver->Shutdown();
      return;
    }
#endif
    assert_budget();
    const auto initial = make_configuration(runtime->motion_binding,
        runtime->motion_binding.value().control_path->active_control_configuration_generation);
    ASSERT_TRUE(initial.locally_valid());
    auto initial_ready = ready(initial);
    ASSERT_TRUE(initial_ready.ok()) << initial_ready.error().message;
    auto installed = runtime->PublishAndReadBackRouteConfiguration(initial, std::move(initial_ready.value()));
    ASSERT_TRUE(installed.ok()) << installed.error().message;
    EXPECT_EQ(installed.value().active_configuration, initial);
    if (check == Lite6AggregateCheck::kRetainedRouteGenerationReseal) {
      // This lower-level regression uses the real retained runtime route
      // publication and strict product reseal. It does not exercise the host
      // Apply call site; that remains a separate private integration gate.
      const auto generation_one_motion = arm_binding;
      const auto generation_one_gripper = gripper_binding;
      const auto generation_one_representatives =
          runtime->control_binding_representatives;
      const auto generation_one_registrations = runtime->participant_registrations;
      ASSERT_TRUE(generation_one_motion.value().control_path->runtime_scale_qualification);
      EXPECT_EQ(generation_one_motion.value().control_path
                    ->active_control_configuration_generation,
                core::ActiveControlConfigurationGeneration(1U));

      const auto arm_successor = make_configuration(
          runtime->motion_binding,
          core::ActiveControlConfigurationGeneration(2U));
      const auto arm_successor_ready = ready(arm_successor);
      ASSERT_TRUE(arm_successor_ready.ok()) << arm_successor_ready.error().message;
      const auto arm_published = runtime->PublishAndReadBackRouteConfiguration(
          arm_successor, arm_successor_ready.value());
      ASSERT_TRUE(arm_published.ok()) << arm_published.error().message;
      EXPECT_EQ(arm_published.value().active_configuration, arm_successor);
      ASSERT_EQ(runtime->motion_binding.value().control_path
                    ->active_control_configuration_generation,
                arm_successor.active_control_configuration_generation);

      ASSERT_TRUE(runtime->gripper_binding.has_value());
      const auto gripper_successor = make_configuration(
          *runtime->gripper_binding,
          core::ActiveControlConfigurationGeneration(2U));
      const auto gripper_successor_ready = ready(gripper_successor);
      ASSERT_TRUE(gripper_successor_ready.ok())
          << gripper_successor_ready.error().message;
      const auto gripper_published = runtime->PublishAndReadBackRouteConfiguration(
          gripper_successor, gripper_successor_ready.value());
      ASSERT_TRUE(gripper_published.ok()) << gripper_published.error().message;
      EXPECT_EQ(gripper_published.value().active_configuration,
                gripper_successor);
      ASSERT_EQ(runtime->gripper_binding->value().control_path
                    ->active_control_configuration_generation,
                gripper_successor.active_control_configuration_generation);

      const auto route_for = [&](const core::RuntimeParticipantBinding &binding)
          -> const core::ActiveControlConfigurationDTO & {
        return binding.value().control_path->control_domain_id ==
                       arm_successor.control_domain_id
                   ? arm_successor
                   : gripper_successor;
      };

      auto unreconciled = arm_successor;
      unreconciled.state = core::ActiveControlConfigurationStateDTO::
          kExternalOrUnknownUnreconciled;
      const auto unreconciled_result =
          detail::rebase_runtime_binding_to_predecessor(
              generation_one_motion, unreconciled);
      ASSERT_FALSE(unreconciled_result.ok());
      EXPECT_EQ(unreconciled_result.error().details.at("reason"),
                "route_predecessor_rebind_input_invalid");
      EXPECT_EQ(unreconciled_result.error().message,
                "durable route predecessor rebind has no exact control path");

      auto wrong_revision = arm_successor;
      const auto motion_route = std::find_if(
          wrong_revision.binding_routes.begin(),
          wrong_revision.binding_routes.end(), [&](const auto &row) {
            return row.control_binding_id ==
                   generation_one_motion.value().control_path->control_binding_id;
          });
      ASSERT_NE(motion_route, wrong_revision.binding_routes.end());
      motion_route->control_binding_revision =
          core::ControlBindingRevision("foreign.binding.revision");
      for (auto &projection : wrong_revision.kinematic_resource_source_projections) {
        for (auto &contributor : projection.contributing_control_bindings) {
          if (contributor.control_binding_id == motion_route->control_binding_id)
            contributor.control_binding_revision =
                motion_route->control_binding_revision;
        }
      }
      ASSERT_TRUE(wrong_revision.locally_valid());
      const auto wrong_revision_result =
          detail::rebase_runtime_binding_to_predecessor(
              generation_one_motion, wrong_revision);
      ASSERT_FALSE(wrong_revision_result.ok());
      EXPECT_EQ(wrong_revision_result.error().details.at("reason"),
                "route_predecessor_rebind_mismatch");
      EXPECT_EQ(wrong_revision_result.error().message,
                "durable route predecessor crosses its retained runtime binding");

      const auto rebase = [&](const core::RuntimeParticipantBinding &binding) {
        return detail::rebase_runtime_binding_to_predecessor(
            binding, route_for(binding));
      };
      const auto candidate_motion = rebase(generation_one_motion);
      ASSERT_TRUE(candidate_motion.ok()) << candidate_motion.error().message;
      const auto candidate_gripper = rebase(generation_one_gripper);
      ASSERT_TRUE(candidate_gripper.ok()) << candidate_gripper.error().message;
      EXPECT_EQ(candidate_motion.value().value().control_path
                    ->active_control_configuration_generation,
                arm_successor.active_control_configuration_generation);
      EXPECT_EQ(candidate_motion.value().value().control_path
                    ->runtime_scale_qualification,
                generation_one_motion.value().control_path
                    ->runtime_scale_qualification);
      // A newly compiled binding with no retained route facts stays at the
      // compiler's generation 1; only explicit authoritative retained-route
      // rebasing moves the existing candidate to generation 2.
      EXPECT_EQ(generation_one_motion.value().control_path
                    ->active_control_configuration_generation,
                core::ActiveControlConfigurationGeneration(1U));

      std::vector<core::RuntimeParticipantBinding> candidate_representatives;
      candidate_representatives.reserve(generation_one_representatives.size());
      for (const auto &representative : generation_one_representatives) {
        const auto rebased = rebase(representative);
        ASSERT_TRUE(rebased.ok()) << rebased.error().message;
        EXPECT_EQ(rebased.value().value().runtime_unit_epochs,
                  representative.value().runtime_unit_epochs);
        EXPECT_EQ(rebased.value().value().control_path,
                  runtime->control_binding_representatives.at(
                      candidate_representatives.size()).value().control_path);
        candidate_representatives.push_back(rebased.value());
      }
      auto candidate_registrations = runtime->participant_registrations;
      for (auto &candidate : candidate_registrations) {
        const auto source = std::find_if(
            generation_one_registrations.begin(),
            generation_one_registrations.end(), [&](const auto &row) {
              return row.binding.value().control_path->control_binding_id ==
                     candidate.binding.value().control_path->control_binding_id;
            });
        ASSERT_NE(source, generation_one_registrations.end());
        const auto predecessor_binding = candidate.binding;
        const bool is_motion_registration =
            source->binding.value().control_path->control_binding_id ==
            generation_one_motion.value().control_path->control_binding_id;
        if (is_motion_registration) {
          ASSERT_TRUE(candidate.stop_readiness.has_value());
        }
        if (candidate.stop_readiness.has_value()) {
          const auto &route = route_for(source->binding);
          const auto desired = std::find_if(
              materialized.bundle->value().desired_control_configurations.begin(),
              materialized.bundle->value().desired_control_configurations.end(),
              [&](const auto &row) {
                return row.control_domain_id == route.control_domain_id;
              });
          ASSERT_NE(desired,
                    materialized.bundle->value().desired_control_configurations.end());
          const auto expected_domain =
              deployment::load_verified_controller_lifecycle_expected_domain(
                  *materialized.bundle, artifacts, *desired,
                  route.source_runtime_unit_id, route.source_runtime_epoch);
          const auto stale_readiness =
              core::rebind_runtime_participant_stop_readiness_product(
                  *candidate.stop_readiness, predecessor_binding,
                  source->binding, expected_domain.sealed());
          ASSERT_FALSE(stale_readiness.ok());
          EXPECT_EQ(stale_readiness.error().details.at("reason"),
                    "runtime_participant_stop_product_rebind_changes_controller");
        }
        const auto rebound = rebase(source->binding);
        ASSERT_TRUE(rebound.ok()) << rebound.error().message;
        if (candidate.stop_readiness.has_value()) {
          const auto &route = route_for(source->binding);
          const auto desired = std::find_if(
              materialized.bundle->value().desired_control_configurations.begin(),
              materialized.bundle->value().desired_control_configurations.end(),
              [&](const auto &row) {
                return row.control_domain_id == route.control_domain_id;
              });
          ASSERT_NE(desired,
                    materialized.bundle->value().desired_control_configurations.end());
          const auto expected_domain =
              deployment::load_verified_controller_lifecycle_expected_domain(
                  *materialized.bundle, artifacts, *desired,
                  route.source_runtime_unit_id, route.source_runtime_epoch);
          const auto rebound_readiness =
              core::rebind_runtime_participant_stop_readiness_product(
                  *candidate.stop_readiness, predecessor_binding,
                  rebound.value(), expected_domain.sealed());
          ASSERT_TRUE(rebound_readiness.ok()) << rebound_readiness.error().message;
          candidate.stop_readiness = rebound_readiness.value();
        }
        candidate.binding = rebound.value();
      }

      const core::ConfigurationFenceId fence_id(
          "retained-route-generation.reseal.apply");
      const core::ConfigurationFenceScopeDTO scope{};
      admission_gate.RestoreFence(fence_id, scope);
      const auto reseal = [&](
          const core::RuntimeParticipantBinding &motion,
          const std::optional<core::RuntimeParticipantBinding> &gripper,
          const std::vector<core::RuntimeParticipantBinding> &representatives,
          auto candidates) {
        return reseal_retained_native_runtime_for_apply(
            *runtime,
            RetainedNativeRuntimeResealInput{
                motion, gripper, {}, {},
                representatives, std::move(candidates), {}, nullptr,
                runtime->reference_resolver, fence_id, scope, admission_gate});
      };
      const auto unreconciled_candidate =
          reseal(generation_one_motion, generation_one_gripper,
                 generation_one_representatives, generation_one_registrations);
      ASSERT_FALSE(unreconciled_candidate.ok());
      EXPECT_EQ(unreconciled_candidate.error().details.at("reason"),
                "native_product_reseal_physical_identity_changed");

      const auto accepted =
          reseal(candidate_motion.value(), candidate_gripper.value(),
                 candidate_representatives, candidate_registrations);
      ASSERT_TRUE(accepted.ok()) << accepted.error().message;
      EXPECT_EQ(runtime->motion_binding.value(), candidate_motion.value().value());
      ASSERT_TRUE(runtime->gripper_binding.has_value());
      EXPECT_EQ(runtime->gripper_binding->value(), candidate_gripper.value().value());
      ASSERT_TRUE(runtime->motion_binding.value().control_path
                      ->runtime_scale_qualification.has_value());
      EXPECT_EQ(*runtime->motion_binding.value().control_path
                     ->runtime_scale_qualification,
                *generation_one_motion.value().control_path
                     ->runtime_scale_qualification);
      return;
    }
    if (check == Lite6AggregateCheck::kResolverRebuildFailures) {
      auto *kernel = runtime->active_addressed_kernel();
      ASSERT_TRUE(kernel->SetRouteRecoveryPending(true).ok());
      const auto physical_before = runtime->motion_binding.value();
      const auto channel_before =
          runtime->motion_state_channel->binding().value();
      const auto factory_before = runtime->participant_resolver_factory;
      const auto resolver_before = runtime->participant_resolver;
      const auto actual_rebind = runtime->rebind_motion_route;
      std::size_t rebind_count = 0U;
      runtime->rebind_motion_route = [&](auto binding, auto aliases) {
        ++rebind_count;
        return actual_rebind(std::move(binding), std::move(aliases));
      };
      const auto successor = make_configuration(
          runtime->motion_binding,
          core::ActiveControlConfigurationGeneration(
              initial.active_control_configuration_generation.value() + 1U));
      const auto successor_ready = ready(successor);
      ASSERT_TRUE(successor_ready.ok()) << successor_ready.error().message;
      struct Expected final {
        const char *failed_reason;
        const char *failed_message;
        const char *missing_reason;
        const char *missing_message;
        const char *thrown_reason;
      };
      const Expected expected[]{
          {"route_publication_resolver_factory_failed",
           "native participant resolver factory returned no resolver",
           "route_publication_resolver_factory_missing",
           "dynamic native participant resolution cannot retain stale physical "
           "facts",
           "route_publication_native_composition_failed"},
          {"component_readiness_resolver_factory_failed",
           "native participant resolver factory returned no resolver",
           "component_readiness_resolver_factory_missing",
           "dynamic native participant resolution cannot retain stale "
           "readiness",
           "component_readiness_native_composition_failed"},
          {"route_predecessor_resolver_factory_failed",
           "native predecessor resolver factory returned no resolver",
           "route_predecessor_resolver_factory_missing",
           "native predecessor adoption cannot retain a stale resolver",
           "route_predecessor_adoption_failed"}};
      for (int purpose = 0; purpose < 3; ++purpose) {
        for (int failure = 0; failure < 3; ++failure) {
          SCOPED_TRACE(purpose);
          SCOPED_TRACE(failure);
          std::size_t factory_calls = 0U;
          // Presence of the installed resolver is what makes losing a factory
          // an error. This sentinel must never resolve or dispatch an
          // operation.
          runtime->participant_resolver = [](const auto &, std::size_t) {
            ADD_FAILURE() << "failed rebuild used the installed resolver";
            return core::Result<core::RuntimeParticipantExecutionRegistration>::
                Err(core::ErrorCode::kInternalError);
          };
          runtime->participant_resolver_factory =
              failure == 1
                  ? RetainedRuntimeParticipantResolverFactory{}
                  : RetainedRuntimeParticipantResolverFactory{[&](auto) {
                      ++factory_calls;
                      if (failure == 2)
                        throw std::runtime_error("factory throw witness");
                      return RetainedRuntimeParticipantResolver{};
                    }};
          const auto expect_refusal = [&](const auto &result) {
            ASSERT_FALSE(result.ok());
            EXPECT_EQ(result.error().code,
                      core::to_string(core::ErrorCode::kInvariantViolation));
            EXPECT_EQ(result.error().details.at("reason"),
                      failure == 0   ? expected[purpose].failed_reason
                      : failure == 1 ? expected[purpose].missing_reason
                                     : expected[purpose].thrown_reason);
            EXPECT_EQ(result.error().message,
                      failure == 0   ? expected[purpose].failed_message
                      : failure == 1 ? expected[purpose].missing_message
                                     : "factory throw witness");
          };
          if (purpose == 0) {
            expect_refusal(runtime->PublishAndReadBackRouteConfiguration(
                successor, successor_ready.value()));
          } else if (purpose == 1) {
            expect_refusal(runtime->PublishComponentCommandReadiness(
                {core::ComponentReadinessAuthorityDTO::kRestartExclusion,
                 {},
                 {},
                 component_id,
                 core::ComponentLifecycleGeneration(2U),
                 false}));
          } else {
            expect_refusal(
                runtime->AdoptAndReadBackRouteConfigurationPredecessor(
                    initial));
          }
          EXPECT_EQ(factory_calls, failure == 1 ? 0U : 1U);
          EXPECT_EQ(rebind_count, 0U);
          EXPECT_EQ(runtime->motion_binding.value(), physical_before);
          EXPECT_EQ(runtime->motion_state_channel->binding().value(),
                    channel_before);
          const auto readback = kernel->runtime_route_configuration_readback(
              initial.control_domain_id,
              initial.active_control_configuration_id,
              initial.active_control_configuration_generation);
          ASSERT_TRUE(readback.ok());
          EXPECT_EQ(readback.value(), installed.value());
        }
      }
      runtime->participant_resolver_factory = factory_before;
      runtime->participant_resolver = resolver_before;
      runtime->rebind_motion_route = actual_rebind;
      return;
    }
    if (check == Lite6AggregateCheck::kStopResponseFollowsRouteRebind) {
      // The composition keeps this construction-time copy as its
      // direct-control binding and composes its first resolver from it.
      const auto composed = runtime->CurrentPhysicalBindings();
      ASSERT_EQ(composed.size(), 2U);
      ASSERT_EQ(composed.front().value(), runtime->motion_binding.value());
      ASSERT_EQ(composed.back().value(), runtime->gripper_binding->value());
      const auto composed_resolver =
          std::make_shared<ProductBackedRuntimeParticipantResolver>(
              package_root, materialized.bundle, model_map, composed,
              std::vector<core::ProgramBindingId>{});
      const auto outside = [](const auto &reply) {
        return !reply.ok() && reply.error().code == "invalid_request" &&
               reply.error().message ==
                   "Stop metadata is outside the exact composed binding";
      };
      const auto route_of = [](const deployment::CanonicalValue &value) {
        const auto *route = value.find_object_member("control_route_id");
        return route != nullptr ? route->as_string() : std::string{};
      };
      // Without a rebind the current rows are the composed rows.
      const auto before =
          composed_resolver->ReadQualifiedStopResponse(composed.front(), composed);
      ASSERT_TRUE(before.ok()) << before.error().message;
      EXPECT_EQ(route_of(before.value()),
                composed.front().value().control_path->control_route_id.value());

      // Route publication rebinds the runtime to a successor generation.
      runtime->prepare_measured_sources = [](const auto &, const auto &, const auto &) {
        return core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>::Ok(nullptr);
      };
      const auto successor = make_configuration(runtime->motion_binding,
          core::ActiveControlConfigurationGeneration(
              initial.active_control_configuration_generation.value() + 1U));
      auto successor_ready = ready(successor);
      ASSERT_TRUE(successor_ready.ok()) << successor_ready.error().message;
      const auto published = runtime->PublishAndReadBackRouteConfiguration(
          successor, successor_ready.value());
      ASSERT_TRUE(published.ok()) << published.error().message;
      const auto current = runtime->CurrentPhysicalBindings();
      ASSERT_EQ(current.size(), composed.size());
      ASSERT_EQ(current.front().value(), runtime->motion_binding.value());
      EXPECT_EQ(current.front()
                    .value()
                    .control_path->active_control_configuration_generation,
                successor.active_control_configuration_generation);
      // Only the control path moved; the target identity is unchanged.
      ASSERT_NE(current.front().value(), composed.front().value());
      EXPECT_EQ(current.front().value().command_target_id,
                composed.front().value().command_target_id);
      EXPECT_EQ(current.front().value().capability_id,
                composed.front().value().capability_id);
      EXPECT_EQ(current.front().value().resources,
                composed.front().value().resources);

      // A Program-only Apply recomposes the resolver from current
      // registrations; the stale composition copy is then outside it.
      std::vector<core::RuntimeParticipantBinding> registered;
      for (const auto &row : runtime->participant_registrations)
        registered.push_back(row.binding);
      const auto recomposed =
          composed_resolver->WithComposedParticipants(std::move(registered));
      EXPECT_TRUE(outside(
          recomposed->ReadQualifiedStopResponse(composed.front(), current)));
      // The current runtime binding is readable whether or not the resolver
      // was recomposed after the rebind.
      for (const auto *resolver : {composed_resolver.get(), recomposed.get()}) {
        const auto reply =
            resolver->ReadQualifiedStopResponse(current.front(), current);
        ASSERT_TRUE(reply.ok()) << reply.error().message;
        EXPECT_EQ(route_of(reply.value()),
                  current.front().value().control_path->control_route_id.value());
      }
      // Exact identity stays mandatory: a binding off the current rows, or
      // no current rows at all, is refused.
      auto foreign = current.front().value();
      foreign.control_path->active_control_configuration_generation =
          core::ActiveControlConfigurationGeneration(
              successor.active_control_configuration_generation.value() + 7U);
      const auto foreign_binding =
          core::verify_runtime_participant_binding(std::move(foreign));
      ASSERT_TRUE(foreign_binding.ok()) << foreign_binding.error().message;
      EXPECT_TRUE(outside(recomposed->ReadQualifiedStopResponse(
          foreign_binding.value(), current)));
      EXPECT_TRUE(outside(
          recomposed->ReadQualifiedStopResponse(current.front(), {})));

      // A later activation after the recomposition moves the runtime again;
      // the recomposed resolver's rows are now stale but the read follows.
      const auto later = make_configuration(runtime->motion_binding,
          core::ActiveControlConfigurationGeneration(
              successor.active_control_configuration_generation.value() + 1U));
      auto later_ready = ready(later);
      ASSERT_TRUE(later_ready.ok()) << later_ready.error().message;
      const auto republished = runtime->PublishAndReadBackRouteConfiguration(
          later, later_ready.value());
      ASSERT_TRUE(republished.ok()) << republished.error().message;
      const auto latest = runtime->CurrentPhysicalBindings();
      ASSERT_EQ(latest.size(), composed.size());
      EXPECT_EQ(latest.front()
                    .value()
                    .control_path->active_control_configuration_generation,
                later.active_control_configuration_generation);
      ASSERT_NE(latest.front().value(), current.front().value());
      EXPECT_TRUE(outside(
          recomposed->ReadQualifiedStopResponse(current.front(), latest)));
      for (const auto *resolver : {composed_resolver.get(), recomposed.get()}) {
        const auto reply =
            resolver->ReadQualifiedStopResponse(latest.front(), latest);
        ASSERT_TRUE(reply.ok()) << reply.error().message;
        EXPECT_EQ(route_of(reply.value()),
                  latest.front().value().control_path->control_route_id.value());
      }
      return;
    }
    assert_budget();
    const auto absent =
        runtime->active_addressed_kernel()->current_runtime_route_configuration(
            runtime->gripper_binding->value().control_path->control_domain_id);
    ASSERT_TRUE(absent.ok());
    ASSERT_FALSE(absent.value());
    const auto actual_rebind = runtime->rebind_motion_route;
    runtime->rebind_motion_route = [&, actual_rebind](auto binding, auto aliases) {
      ++rebinds;
      return actual_rebind(std::move(binding), std::move(aliases));
    };
    auto successor = make_configuration(runtime->motion_binding,
        core::ActiveControlConfigurationGeneration(initial.active_control_configuration_generation.value() + 1U));
    auto successor_ready = ready(successor);
    ASSERT_TRUE(successor_ready.ok()) << successor_ready.error().message;
    const auto physical_before = runtime->motion_binding.value();
    const auto channel_before = runtime->motion_state_channel->binding().value();
    using PreparedResult = core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>;
    if (check != Lite6AggregateCheck::kMeasuredPublication) {
      const auto other = make_configuration(*runtime->gripper_binding,
          runtime->gripper_binding->value().control_path->active_control_configuration_generation);
      auto other_ready = ready(other);
      ASSERT_TRUE(other_ready.ok()) << other_ready.error().message;
      bool outer = true;
      runtime->prepare_measured_sources = [&](const auto &motion, const auto &, const auto &) {
        if (!outer) return PreparedResult::Ok(nullptr);
        outer = false;
        const auto nested = runtime->PublishAndReadBackRouteConfiguration(other, other_ready.value());
        EXPECT_TRUE(nested.ok()) << (nested.ok() ? "" : nested.error().message);
        if (check == Lite6AggregateCheck::kMeasuredNullOptionalDomainRace)
          return PreparedResult::Ok(nullptr);
        return PreparedResult::Ok(std::make_shared<RetainedPreparedRobotMeasuredSources>(
            RetainedPreparedRobotMeasuredSources{{motion.value().controllable_component_id, {}}, [&] { ++enabled; }}));
      };
      const auto raced = runtime->PublishAndReadBackRouteConfiguration(successor, successor_ready.value());
      ASSERT_FALSE(raced.ok());
      EXPECT_EQ(raced.error().details.at("reason"), "measured_source_association_changed");
      EXPECT_EQ(rebinds, 0U);
      EXPECT_EQ(enabled, 0U);
      EXPECT_EQ(runtime->motion_binding.value(), physical_before);
      const auto still_current =
          runtime->active_addressed_kernel()
              ->current_runtime_route_configuration(initial.control_domain_id);
      ASSERT_TRUE(still_current.ok());
      ASSERT_TRUE(still_current.value());
      EXPECT_EQ(still_current.value()->active_configuration, initial);
      const auto appeared =
          runtime->active_addressed_kernel()
              ->current_runtime_route_configuration(other.control_domain_id);
      ASSERT_TRUE(appeared.ok());
      ASSERT_TRUE(appeared.value());
      EXPECT_EQ(appeared.value()->active_configuration, other);
      return;
    }
    runtime->prepare_measured_sources = [&](const auto &, const auto &, const auto &) {
      EXPECT_EQ(runtime->motion_binding.value(), physical_before);
      EXPECT_EQ(runtime->motion_state_channel->binding().value(), channel_before);
      EXPECT_EQ(rebinds, 0U);
      return PreparedResult::Err({"test_association_invalid", "exact source association refused", {}});
    };
    const auto rejected = runtime->PublishAndReadBackRouteConfiguration(successor, successor_ready.value());
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "test_association_invalid");
    EXPECT_EQ(rebinds, 0U);
    EXPECT_EQ(runtime->motion_binding.value(), physical_before);
    EXPECT_EQ(runtime->motion_state_channel->binding().value(), channel_before);
    const auto failed_readback =
        runtime->active_addressed_kernel()->current_runtime_route_configuration(
            initial.control_domain_id);
    ASSERT_TRUE(failed_readback.ok());
    ASSERT_TRUE(failed_readback.value());
    EXPECT_EQ(failed_readback.value()->active_configuration, initial);
    struct ProducerLease final {
      std::function<void()> retire;
      ~ProducerLease() { retire(); }
    };
    std::weak_ptr<const RetainedPreparedRobotMeasuredSources> committed;
    runtime->prepare_measured_sources = [&](const auto &motion, const auto &, const auto &configurations) {
      EXPECT_EQ(runtime->motion_binding.value(), physical_before);
      EXPECT_EQ(runtime->motion_state_channel->binding().value(), channel_before);
      EXPECT_EQ(rebinds, 0U);
      EXPECT_EQ(configurations.size(), 1U); // the genuinely absent tool domain stays absent
      auto lease = std::make_shared<ProducerLease>();
      lease->retire = [&, weak_runtime = std::weak_ptr<RetainedNativeRuntime>(runtime)] {
        ++retired;
        auto live = weak_runtime.lock();
        if (!live) return;
        // Reenter the actual route exclusion after retirement. The configured
        // nil preparer has no side effects and cannot recursively own a lease.
        const auto unavailable = live->PrepareCurrentMeasuredSources();
        EXPECT_FALSE(unavailable.ok());
        if (!unavailable.ok()) {
          EXPECT_EQ(unavailable.error().details.at("reason"), "measured_prepared_sources_missing");
        }
        ++reentrant_reads;
      };
      auto prepared = std::make_shared<RetainedPreparedRobotMeasuredSources>(
          RetainedPreparedRobotMeasuredSources{{motion.value().controllable_component_id, {}},
              [&, lease, successor] {
                ++enabled;
                const auto current = runtime->active_addressed_kernel()
                                         ->current_runtime_route_configuration(
                                             successor.control_domain_id);
                EXPECT_TRUE(current.ok());
                if (current.ok() && current.value())
                  EXPECT_EQ(current.value()->active_configuration, successor);
                else ADD_FAILURE() << "Enable preceded exact Kernel publication";
                EXPECT_EQ(runtime->motion_binding.value().control_path->active_control_configuration_generation,
                          successor.active_control_configuration_generation);
              }});
      committed = prepared;
      return PreparedResult::Ok(prepared);
    };
    const auto published = runtime->PublishAndReadBackRouteConfiguration(successor, successor_ready.value());
    ASSERT_TRUE(published.ok()) << published.error().message;
    EXPECT_EQ(published.value().active_configuration, successor);
    assert_budget();
    EXPECT_EQ(enabled, 1U);
    EXPECT_EQ(rebinds, 1U);
    EXPECT_FALSE(committed.expired());
    EXPECT_EQ(retired, 0U);
    runtime->prepare_measured_sources = [](const auto &, const auto &, const auto &) { return PreparedResult::Ok(nullptr); };
    auto nil_configuration = make_configuration(runtime->motion_binding,
        core::ActiveControlConfigurationGeneration(successor.active_control_configuration_generation.value() + 1U));
    auto nil_ready = ready(nil_configuration);
    ASSERT_TRUE(nil_ready.ok()) << nil_ready.error().message;
    const auto nil_published = runtime->PublishAndReadBackRouteConfiguration(nil_configuration, nil_ready.value());
    ASSERT_TRUE(nil_published.ok()) << nil_published.error().message;
    EXPECT_EQ(nil_published.value().active_configuration, nil_configuration);
    EXPECT_EQ(runtime->motion_state_channel->binding().value(), runtime->motion_binding.value());
    EXPECT_EQ(enabled, 1U);
    EXPECT_EQ(rebinds, 2U);
    EXPECT_TRUE(committed.expired());
    EXPECT_EQ(retired, 1U);
    EXPECT_EQ(reentrant_reads, 1U);
    assert_budget();
    if (moving_budget_check) {
      ASSERT_TRUE(
          budget_cell->addressed_kernel().SetRouteRecoveryPending(true).ok());
      const auto readiness = runtime->PublishComponentCommandReadiness(
          {core::ComponentReadinessAuthorityDTO::kRestartExclusion,
           std::nullopt, std::nullopt, component_id,
           core::ComponentLifecycleGeneration(2U), false});
      ASSERT_TRUE(readiness.ok()) << readiness.error().message;
      assert_budget();
    }
    return;
  }
  if (check == Lite6AggregateCheck::kSameRoutePredecessorQualification) {
    const auto before = runtime->motion_binding;
    const auto &path = *before.value().control_path;
    ASSERT_TRUE(path.runtime_scale_qualification.has_value());
    const auto qualification = *path.runtime_scale_qualification;
    const auto execution = runtime->execution;
    const auto channel = runtime->motion_state_channel;
    core::ActiveControlConfigurationDTO predecessor{
        path.active_control_configuration_id,
        path.control_domain_id,
        path.control_domain_revision,
        {{path.control_binding_id, path.control_binding_revision,
          path.control_route_id}},
        path.activation_set_id,
        path.controller_topology_id,
        path.topology_revision,
        path.provider_runtime_unit_id,
        path.provider_runtime_epoch,
        1,
        {},
        path.active_control_configuration_generation,
        core::ActiveControlConfigurationStateDTO::kReconciled};
    for (const auto &resource : before.value().resources) {
      predecessor.kinematic_resource_source_projections.push_back(
          {resource.resource_id,
           {{path.control_binding_id, path.control_binding_revision}},
           path.provider_instance_id, path.provider_manifest_id,
           path.provider_manifest_revision, path.state_authority_id,
           path.state_authority_class,
           channel->source_kind()});
    }
    ASSERT_TRUE(runtime->addressed_kernel->SetRouteRecoveryPending(true).ok());
    auto foreign = predecessor;
    foreign.binding_routes.front().control_binding_revision =
        core::ControlBindingRevision("foreign-binding-revision");
    EXPECT_FALSE(runtime->AdoptAndReadBackRouteConfigurationPredecessor(foreign).ok());
    EXPECT_EQ(runtime->motion_binding.value(), before.value());
    EXPECT_EQ(channel->binding().value(), before.value());
    const auto adopted =
        runtime->AdoptAndReadBackRouteConfigurationPredecessor(predecessor);
    ASSERT_TRUE(adopted.ok()) << adopted.error().message;
    EXPECT_EQ(adopted.value().active_configuration, predecessor);
    EXPECT_FALSE(adopted.value().operation_admission_open);
    EXPECT_TRUE(adopted.value().readiness_rows.empty());
    ASSERT_TRUE(runtime->motion_binding.value().control_path->runtime_scale_qualification);
    EXPECT_EQ(*runtime->motion_binding.value().control_path->runtime_scale_qualification,
              qualification);
    EXPECT_EQ(runtime->motion_binding.value(), before.value());
    EXPECT_EQ(channel->binding().value(), before.value());
    EXPECT_EQ(runtime->execution, execution);
    EXPECT_EQ(runtime->motion_state_channel, channel);
    return;
  }
  if (reseal_capability_inventory) {
    ASSERT_EQ(runtime->control_binding_representatives.size(), 3U);
    ASSERT_EQ(runtime->participant_registrations.size(), 3U);
    const auto motion_engine = runtime->participant_engine;
    const auto gripper_engine = runtime->gripper_participant_engine;
    const auto motion_channel = runtime->motion_state_channel;
    const auto gripper_channel = runtime->gripper_state_channel;
    const auto registrations = runtime->participant_registrations;
    const auto secondary = std::find_if(
        registrations.begin(), registrations.end(), [](const auto &row) {
          return row.binding.value().capability_id ==
                 core::CapabilityId("execution.feedback.state_snapshots");
        });
    ASSERT_NE(secondary, registrations.end());
    ASSERT_EQ(secondary->engine, motion_engine.get());
    const auto secondary_index = static_cast<std::size_t>(
        std::distance(registrations.begin(), secondary));
    const core::ConfigurationFenceId fence_id("same-controller.reseal.apply");
    const core::ConfigurationFenceScopeDTO scope{};
    admission_gate.RestoreFence(fence_id, scope);
    const auto reseal = [&](auto candidate_registrations) {
      return reseal_retained_native_runtime_for_apply(
          *runtime,
          RetainedNativeRuntimeResealInput{
              arm_binding, gripper_binding, {}, {},
              runtime->control_binding_representatives,
              std::move(candidate_registrations), {}, nullptr,
              runtime->reference_resolver, fence_id, scope, admission_gate});
    };

    auto changed_engine = registrations;
    changed_engine[secondary_index].engine = runtime->participant_engine_for(false);
    const auto bad_candidate = reseal(std::move(changed_engine));
    ASSERT_FALSE(bad_candidate.ok());
    EXPECT_EQ(bad_candidate.error().details.at("reason"),
              "native_product_reseal_registration_identity_changed");
    runtime->participant_registrations.erase(
        runtime->participant_registrations.begin() + secondary_index);
    const auto missing_predecessor = reseal(registrations);
    runtime->participant_registrations = registrations;
    ASSERT_FALSE(missing_predecessor.ok());
    EXPECT_EQ(missing_predecessor.error().details.at("reason"),
              "native_product_reseal_registration_identity_changed");

    const auto accepted = reseal(registrations);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    EXPECT_EQ(runtime->participant_engine, motion_engine);
    EXPECT_EQ(runtime->gripper_participant_engine, gripper_engine);
    EXPECT_EQ(runtime->motion_state_channel, motion_channel);
    EXPECT_EQ(runtime->gripper_state_channel, gripper_channel);
    EXPECT_EQ(runtime->motion_binding.value(), arm_binding.value());
    EXPECT_EQ(runtime->gripper_binding->value(), gripper_binding.value());
    ASSERT_EQ(runtime->participant_registrations.size(), registrations.size());
    for (std::size_t index = 0U; index < registrations.size(); ++index) {
      const auto &current = runtime->participant_registrations[index];
      EXPECT_EQ(current.binding.value(), registrations[index].binding.value());
      EXPECT_EQ(current.engine, registrations[index].engine);
      EXPECT_EQ(current.stop_execution_port,
                registrations[index].stop_execution_port);
    }
    return;
  }
  const auto motion_registration = std::find_if(
      runtime->participant_registrations.begin(),
      runtime->participant_registrations.end(), [](const auto &registration) {
        return registration.binding.value().command_target_id ==
               core::CommandTargetId(
                   std::string(deployment::test::kLite6ArmTargetId));
      });
  ASSERT_NE(motion_registration, runtime->participant_registrations.end());
  if (authored_fault_check) {
    auto expected_fault = authored_fault;
    expected_fault.per_joint_max_deceleration = expected_arm_fault_limits;
    if (check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithExtraJoints ||
        check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithOnlyExtraSettleOverrides ||
        check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithSparseSettleOverrides) {
      expected_fault.per_joint_max_jerk.clear();
      for (const auto &resource : arm_binding.value().resources)
        expected_fault.per_joint_max_jerk.emplace(*resource.source_joint_name,
                                                 core::AngularJerkDTO{11.0});
      if (check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithExtraJoints) {
        expected_fault.per_joint_settle_velocity_threshold.emplace();
        expected_fault.per_joint_settle_velocity_threshold->clear();
        for (const auto &resource : arm_binding.value().resources)
          expected_fault.per_joint_settle_velocity_threshold->emplace(
              *resource.source_joint_name, core::AngularVelocityDTO{0.005});
      } else if (check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithSparseSettleOverrides) {
        expected_fault.per_joint_settle_velocity_threshold.emplace();
        expected_fault.per_joint_settle_velocity_threshold->clear();
        expected_fault.per_joint_settle_velocity_threshold->emplace(
            *arm_binding.value().resources.front().source_joint_name,
            core::AngularVelocityDTO{0.005});
      } else {
        expected_fault.per_joint_settle_velocity_threshold.reset();
      }
      ASSERT_EQ(authored_fault.per_joint_max_deceleration.size(), 12U);
      ASSERT_EQ(authored_fault.per_joint_max_jerk.size(), 12U);
    }
    expected_fault.deceleration_time_s = core::kDefaultDecelerationTimeS;
    EXPECT_TRUE(runtime->motion_fault_stop_profile_from_deployment);
    EXPECT_EQ(runtime->motion_fault_stop_profile,
              std::optional<core::MotionInterruptStopProfileDTO>(expected_fault));
    EXPECT_EQ(motion_registration->default_fault_stop_profile,
              std::optional<core::MotionInterruptStopProfileDTO>(expected_fault));
    EXPECT_EQ(authored_fault, original_authored_fault);
    ASSERT_NE(port_participant, nullptr); // Missing limits did not add an Apply-time refusal.
    ASSERT_TRUE(motion_registration->default_operator_stop_profile);
    ASSERT_TRUE(motion_registration->stop_readiness);
    const auto &nominal = motion_registration->stop_readiness->value()
                              .resolved_stop_base_plan.escalation_stages.front();
    ASSERT_TRUE(nominal.default_contract_parameters);
    const auto decoded_operator = core::map_motion_controlled_stop_contract_parameters_to_profile(
        *nominal.default_contract_parameters, arm_binding.value().resources, "operator_soft_on_path");
    ASSERT_TRUE(decoded_operator.ok()) << decoded_operator.error().message;
    const auto expected_operator =
        core::resolve_motion_interrupt_stop_jerk_limits(decoded_operator.value());
    ASSERT_TRUE(expected_operator.ok()) << expected_operator.error().message;
    EXPECT_EQ(*motion_registration->default_operator_stop_profile, expected_operator.value());
    EXPECT_NE(motion_registration->default_operator_stop_profile->per_joint_max_deceleration,
              motion_registration->default_fault_stop_profile->per_joint_max_deceleration);
    const core::PreparedParticipantStopPlanContextDTO context{
        core::OperationId("fault-projection.operation"), core::OperationRevision(1U),
        core::OperationParticipantId("fault-projection.participant"),
        core::ProgramId("fault-projection.program"), core::ProgramRevision(std::string(kZeroDigest)),
        core::ProgramBindingId(std::string(kZeroDigest)), core::ProgramRunId("fault-projection.run"),
        motion_registration->default_fault_stop_profile,
        core::ParticipantStopProfilePurposeDTO::kFault, std::nullopt,
        motion_registration->stop_node_category};
    const auto plan = core::MakePreparedParticipantStopPlan(
        motion_registration->stop_readiness->value().resolved_stop_base_plan, context,
        arm_binding.value().resources, motion_registration->retained_default_stop_limits);
    const bool missing =
        check == Lite6AggregateCheck::kDeploymentFaultStopProfileMissingArmJoint ||
        check == Lite6AggregateCheck::kDeploymentFaultStopProfileWithoutArmJoints;
    if (missing) {
      ASSERT_FALSE(plan.ok());
      EXPECT_EQ(plan.error().details.at("reason"),
                check == Lite6AggregateCheck::kDeploymentFaultStopProfileMissingArmJoint
                    ? "motion_interrupt_stop_deceleration_coverage_mismatch"
                    : "soft_on_path/spring_back_hold requires a non-empty per_joint_max_deceleration");
    } else {
      ASSERT_TRUE(plan.ok()) << plan.error().message;
      EXPECT_EQ(plan.value().participant_stop_profile,
                motion_registration->default_fault_stop_profile);
      EXPECT_TRUE(core::VerifyPreparedParticipantStopPlan(
          motion_registration->stop_readiness->value().resolved_stop_base_plan, plan.value(),
          arm_binding.value().resources, motion_registration->retained_default_stop_limits,
          motion_registration->stop_node_category).ok());
      ASSERT_TRUE(plan.value().stage_commands.front().validated_contract_parameters);
      const auto decoded = core::map_motion_controlled_stop_contract_parameters_to_profile(
          *plan.value().stage_commands.front().validated_contract_parameters,
          arm_binding.value().resources, "observed_fault_profile");
      ASSERT_TRUE(decoded.ok()) << decoded.error().message;
      ASSERT_TRUE(decoded.value().per_joint_settle_velocity_threshold);
      for (const auto &resource : arm_binding.value().resources) {
        const auto retained = std::find_if(
            motion_registration->retained_default_stop_limits.begin(),
            motion_registration->retained_default_stop_limits.end(), [&](const auto &limit) {
              return limit.resource_id == resource.resource_id;
            });
        ASSERT_NE(retained, motion_registration->retained_default_stop_limits.end());
        auto expected_threshold = retained->settle_velocity_threshold;
        if (expected_fault.per_joint_settle_velocity_threshold) {
          const auto override_it = expected_fault.per_joint_settle_velocity_threshold->find(
              *resource.source_joint_name);
          if (override_it != expected_fault.per_joint_settle_velocity_threshold->end())
            expected_threshold = override_it->second;
        }
        EXPECT_EQ(decoded.value().per_joint_settle_velocity_threshold->at(
                      *resource.source_joint_name), expected_threshold);
      }
      ASSERT_EQ(expected_fault.per_joint_max_deceleration.size(), 6U);
      for (const auto &[_, value] : expected_fault.per_joint_max_deceleration)
        EXPECT_EQ(value, core::CoordinateAccelerationDTO{core::AngularAccelerationDTO{7.0}});
    }
    return;
  }
  EXPECT_FALSE(runtime->motion_fault_stop_profile_from_deployment);
  ASSERT_TRUE(motion_registration->participant_policy.has_value());
  ASSERT_TRUE(
      motion_registration->deployment_maximum_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(
      *motion_registration->deployment_maximum_tcp_linear_speed_mps, 0.15);
  ASSERT_NE(runtime->motion_state_channel, nullptr);
  ASSERT_NE(runtime->gripper_state_channel, nullptr);
  const auto initial_motion_state =
      runtime->motion_state_channel->current_controlled_state();
  ASSERT_TRUE(initial_motion_state.ok())
      << initial_motion_state.error().message;
  ASSERT_EQ(initial_motion_state.value().positions.size(), 6U);
  const std::vector<double> expected_initial_motion_positions{0.0, 0.0, 1.57,
                                                              0.0, 0.0, 0.0};
  for (std::size_t index = 0U; index < expected_initial_motion_positions.size();
       ++index) {
    ASSERT_TRUE(std::holds_alternative<core::AngularPositionDTO>(
        initial_motion_state.value().positions[index]));
    EXPECT_DOUBLE_EQ(std::get<core::AngularPositionDTO>(
                         initial_motion_state.value().positions[index])
                         .value_rad,
                     expected_initial_motion_positions[index]);
  }
  auto direct_frame_resolver =
      std::dynamic_pointer_cast<RetainedSceneFrameResolver>(
          runtime->reference_resolver);
  ASSERT_NE(direct_frame_resolver, nullptr);
  const auto installed_frame_resolver = projection->InstallActiveFrameResolver(
      inventory->projection_binding.projection_revision, direct_frame_resolver);
  ASSERT_TRUE(installed_frame_resolver.ok())
      << installed_frame_resolver.error().message;
  auto foreign_revision = inventory->projection_binding.projection_revision;
  foreign_revision.graph_revision = core::GraphRevision("foreign-graph");
  const auto foreign_frame_resolver = projection->InstallActiveFrameResolver(
      foreign_revision, direct_frame_resolver);
  ASSERT_FALSE(foreign_frame_resolver.ok());
  EXPECT_EQ(foreign_frame_resolver.error().details.at("reason"),
            "native_projection_active_frame_resolver_invalid");
  admission_gate.BindResourceTable(
      runtime->addressed_kernel->resource_admission_table());
  const auto physical_scope = make_runtime_fence_scope(
      {&runtime->motion_binding, &*runtime->gripper_binding});
  EXPECT_EQ(runtime->collision_publisher.get(), projection.get());
  const auto arm_context = runtime->motion_state_channel->query_context();
  EXPECT_TRUE(std::all_of(
      arm_context.coordinate_kinds.begin(), arm_context.coordinate_kinds.end(),
      [](const auto kind) { return kind == core::CoordinateKind::kAngular; }));
  ASSERT_EQ(
      runtime->gripper_state_channel->query_context().coordinate_kinds.size(),
      1U);
  EXPECT_EQ(
      runtime->gripper_state_channel->query_context().coordinate_kinds.front(),
      core::CoordinateKind::kLinear);

  const auto &motion_value = runtime->motion_binding.value();
  const auto joint_names = runtime->kinematic_model->group_joint_names(
      motion_value.command_target_id.value());
  ASSERT_TRUE(joint_names.ok()) << joint_names.error().message;
  std::vector<std::string> expected_joint_names;
  for (const auto &resource : motion_value.resources) {
    expected_joint_names.push_back(resource.compiled_resource_name);
  }
  EXPECT_EQ(joint_names.value(), expected_joint_names);
  const auto limits = runtime->kinematic_model->joint_limits(
      motion_value.controllable_component_id.value(),
      motion_value.command_target_id.value());
  ASSERT_TRUE(limits.ok()) << limits.error().message;
  EXPECT_EQ(limits.value().joint_names, joint_names.value());
  ASSERT_EQ(limits.value().position_lower_rad.size(),
            joint_names.value().size());
  ASSERT_EQ(limits.value().position_upper_rad.size(),
            joint_names.value().size());
  ASSERT_EQ(limits.value().velocity_max_rad_s.size(),
            joint_names.value().size());
  EXPECT_TRUE(
      std::all_of(limits.value().velocity_max_rad_s.begin(),
                  limits.value().velocity_max_rad_s.end(),
                  [](double value) { return std::abs(value - 2.14) < 1e-12; }));
  ASSERT_EQ(limits.value().acceleration_max_rad_s2.size(),
            joint_names.value().size());
  EXPECT_TRUE(
      std::all_of(limits.value().acceleration_max_rad_s2.begin(),
                  limits.value().acceleration_max_rad_s2.end(),
                  [](double value) { return std::abs(value - 10.0) < 1e-12; }));
  const auto base_frame = require_fixture_one(
      motion_value.frame_bindings,
      [](const auto &frame) { return frame.frame_role_id.value() == "base"; },
      "motion base frame");
  const auto tcp_frame = require_fixture_one(
      motion_value.frame_bindings,
      [](const auto &frame) { return frame.frame_role_id.value() == "tcp"; },
      "motion tcp frame");
  const core::RobotStateDTO zero_arm_state{
      joint_names.value(),
      std::vector<double>(joint_names.value().size(), 0.0),
      {}};
  const auto tcp_pose = runtime->kinematic_model->forward_kinematics(
      core::FkRequestDTO{zero_arm_state, tcp_frame.compiled_frame_name,
                         base_frame.compiled_frame_name});
  ASSERT_TRUE(tcp_pose.ok()) << tcp_pose.error().message;
  std::vector<core::ResourceId> motion_resources;
  for (const auto &resource : motion_value.resources) {
    motion_resources.push_back(resource.resource_id);
  }
  const core::TrajectoryDTO stationary_trajectory{
      std::move(motion_resources),
      joint_names.value(),
      {core::TrajectoryWaypointDTO{
           0.0, std::vector<double>(joint_names.value().size(), 0.0), {}, {}},
       core::TrajectoryWaypointDTO{
           1.0, std::vector<double>(joint_names.value().size(), 0.0), {}, {}}}};
  core::CollisionCancellationToken cancellation;
  const auto validation = runtime->kinematic_model->validate_non_collision(
      stationary_trajectory, core::ValidationOptionsDTO{0.0, {}, {}, false},
      cancellation);
  ASSERT_TRUE(validation.valid)
      << (validation.error.has_value() ? validation.error->message : "");
  ASSERT_TRUE(validation.trajectory_metrics.has_value());
  EXPECT_DOUBLE_EQ(validation.trajectory_metrics->trajectory_duration_s, 1.0);
  EXPECT_EQ(validation.trajectory_metrics->peak_abs_joint_velocity_rad_s,
            std::vector<double>(joint_names.value().size(), 0.0));
  EXPECT_TRUE(validation.trajectory_metrics->peak_abs_joint_acceleration_rad_s2
                  .empty());
  EXPECT_FALSE(
      validation.trajectory_metrics->peak_tcp_linear_speed_mps.has_value());

  auto measured_trajectory = stationary_trajectory;
  measured_trajectory.waypoints = {
      core::TrajectoryWaypointDTO{
          0.0, std::vector<double>(joint_names.value().size(), 0.0),
          std::vector<double>(joint_names.value().size(), 0.0),
          std::vector<double>(joint_names.value().size(), 0.0)},
      core::TrajectoryWaypointDTO{
          0.5, std::vector<double>(joint_names.value().size(), 0.0),
          std::vector<double>(joint_names.value().size(), 0.0),
          std::vector<double>(joint_names.value().size(), 0.0)},
      core::TrajectoryWaypointDTO{
          1.0, std::vector<double>(joint_names.value().size(), 0.0),
          std::vector<double>(joint_names.value().size(), 0.0),
          std::vector<double>(joint_names.value().size(), 0.0)}};
  measured_trajectory.waypoints[1].positions_rad[0] = 0.01;
  measured_trajectory.waypoints[2].positions_rad[0] = 0.02;
  measured_trajectory.waypoints[1].velocities_rad_s[0] = 0.02;
  measured_trajectory.waypoints[2].velocities_rad_s[0] = 0.02;
  measured_trajectory.waypoints[1].accelerations_rad_s2[0] = 0.03;
  const auto measured_validation =
      runtime->kinematic_model->validate_non_collision(
          measured_trajectory,
          core::ValidationOptionsDTO{0.15, tcp_frame.compiled_frame_name,
                                     base_frame.compiled_frame_name, false},
          cancellation);
  ASSERT_TRUE(measured_validation.valid)
      << (measured_validation.error.has_value()
              ? measured_validation.error->message
              : "");
  ASSERT_TRUE(measured_validation.trajectory_metrics.has_value());
  const auto &measured_metrics = *measured_validation.trajectory_metrics;
  EXPECT_DOUBLE_EQ(measured_metrics.trajectory_duration_s, 1.0);
  ASSERT_EQ(measured_metrics.peak_abs_joint_velocity_rad_s.size(),
            joint_names.value().size());
  EXPECT_NEAR(measured_metrics.peak_abs_joint_velocity_rad_s[0], 0.02, 1.0e-12);
  ASSERT_EQ(measured_metrics.peak_abs_joint_acceleration_rad_s2.size(),
            joint_names.value().size());
  EXPECT_NEAR(measured_metrics.peak_abs_joint_acceleration_rad_s2[0], 0.03,
              1.0e-12);
  ASSERT_TRUE(measured_metrics.peak_tcp_linear_speed_mps.has_value());
  EXPECT_GT(*measured_metrics.peak_tcp_linear_speed_mps, 0.0);
  EXPECT_LE(*measured_metrics.peak_tcp_linear_speed_mps, 0.15);
  if (check == Lite6AggregateCheck::kMovingValidation) {
    core::ValidationOptionsDTO moving_options{
        1.0, tcp_frame.compiled_frame_name, base_frame.compiled_frame_name,
        false};
    moving_options.retained_prefix =
        core::RetainedTrajectoryPrefixValidationOptionsDTO{
            0.5, 1.0, tcp_frame.compiled_frame_name,
            base_frame.compiled_frame_name};
    const auto validate = [&](const core::TrajectoryDTO &trajectory,
                              const core::ValidationOptionsDTO &options) {
      return runtime->kinematic_model->validate_non_collision(
          trajectory, options, cancellation);
    };
    auto seam_trajectory = measured_trajectory;
    for (auto &point : seam_trajectory.waypoints) {
      std::fill(point.velocities_rad_s.begin(), point.velocities_rad_s.end(),
                0.0);
      std::fill(point.accelerations_rad_s2.begin(),
                point.accelerations_rad_s2.end(), 0.0);
    }
    seam_trajectory.waypoints[1].positions_rad[0] = 0.0;
    const auto ordinary = validate(
        seam_trajectory,
        core::ValidationOptionsDTO{1.0, tcp_frame.compiled_frame_name,
                                   base_frame.compiled_frame_name, false});
    const auto moving = validate(seam_trajectory, moving_options);
    ASSERT_TRUE(ordinary.valid && moving.valid);
    EXPECT_FALSE(ordinary.moving_replacement);
    EXPECT_EQ(moving.trajectory_metrics, ordinary.trajectory_metrics);
    ASSERT_TRUE(moving.moving_replacement);
    const auto &prefix = moving.moving_replacement->unchanged_prefix;
    const auto &future = moving.moving_replacement->changed_future;
    EXPECT_DOUBLE_EQ(prefix.trajectory_duration_s, 0.5);
    EXPECT_DOUBLE_EQ(future.trajectory_duration_s, 0.5);
    ASSERT_TRUE(prefix.peak_tcp_linear_speed_mps &&
                future.peak_tcp_linear_speed_mps);
    EXPECT_DOUBLE_EQ(*moving.trajectory_metrics->peak_tcp_linear_speed_mps,
                     std::max(*prefix.peak_tcp_linear_speed_mps,
                              *future.peak_tcp_linear_speed_mps));
    ASSERT_EQ(prefix.peak_abs_joint_acceleration_rad_s2.size(),
              joint_names.value().size());
    ASSERT_EQ(future.peak_abs_joint_acceleration_rad_s2.size(),
              joint_names.value().size());
    EXPECT_DOUBLE_EQ(prefix.peak_abs_joint_acceleration_rad_s2[0], 0.0);
    EXPECT_NEAR(future.peak_abs_joint_acceleration_rad_s2[0], 0.08, 1e-12);
    for (std::size_t joint = 0; joint < joint_names.value().size(); ++joint) {
      EXPECT_DOUBLE_EQ(
          moving.trajectory_metrics->peak_abs_joint_velocity_rad_s[joint],
          std::max(prefix.peak_abs_joint_velocity_rad_s[joint],
                   future.peak_abs_joint_velocity_rad_s[joint]));
      EXPECT_DOUBLE_EQ(
          moving.trajectory_metrics->peak_abs_joint_acceleration_rad_s2[joint],
          std::max(prefix.peak_abs_joint_acceleration_rad_s2[joint],
                   future.peak_abs_joint_acceleration_rad_s2[joint]));
    }
    const auto shared_knot = validate(measured_trajectory, moving_options);
    ASSERT_TRUE(shared_knot.valid && shared_knot.moving_replacement);
    EXPECT_NEAR(shared_knot.moving_replacement->unchanged_prefix
                    .peak_abs_joint_acceleration_rad_s2[0],
                0.03, 1e-12);
    EXPECT_NEAR(shared_knot.moving_replacement->changed_future
                    .peak_abs_joint_acceleration_rad_s2[0],
                0.03, 1e-12);

    auto fast_prefix = seam_trajectory;
    fast_prefix.waypoints[1].positions_rad[0] = 0.01;
    fast_prefix.waypoints[2].positions_rad[0] = 0.01;
    auto slower_future_options = moving_options;
    slower_future_options.max_tcp_linear_speed_mps =
        *measured_metrics.peak_tcp_linear_speed_mps * 0.1;
    const auto slower_future = validate(fast_prefix, slower_future_options);
    ASSERT_TRUE(slower_future.valid && slower_future.moving_replacement);
    EXPECT_GT(*slower_future.moving_replacement->unchanged_prefix
                   .peak_tcp_linear_speed_mps,
              slower_future_options.max_tcp_linear_speed_mps);
    EXPECT_DOUBLE_EQ(*slower_future.moving_replacement->changed_future
                          .peak_tcp_linear_speed_mps,
                     0.0);
    auto source_too_slow = slower_future_options;
    source_too_slow.retained_prefix->max_tcp_linear_speed_mps = 1e-6;
    const auto refused_source = validate(fast_prefix, source_too_slow);
    ASSERT_FALSE(refused_source.valid);
    ASSERT_TRUE(refused_source.error);
    EXPECT_EQ(refused_source.error->details.at("reason"),
              "tcp_linear_speed_limit_exceeded");
    EXPECT_EQ(refused_source.error->details.at("tcp_frame"),
              tcp_frame.compiled_frame_name);
    auto future_too_slow = moving_options;
    future_too_slow.max_tcp_linear_speed_mps = 1e-6;
    const auto refused_future = validate(seam_trajectory, future_too_slow);
    ASSERT_FALSE(refused_future.valid);
    ASSERT_TRUE(refused_future.error);
    EXPECT_EQ(refused_future.error->details.at("reason"),
              "tcp_linear_speed_limit_exceeded");
    EXPECT_EQ(refused_future.error->details.at("segment_index"), "1");

    // The seam must be re-evaluated in the new frame pair, not subtracted
    // from the source's TCP coordinates. A frame relative to itself is fixed.
    auto different_tcp = moving_options;
    different_tcp.tcp_frame = base_frame.compiled_frame_name;
    different_tcp.max_tcp_linear_speed_mps = 1e-6;
    const auto changed_tcp = validate(measured_trajectory, different_tcp);
    ASSERT_TRUE(changed_tcp.valid && changed_tcp.moving_replacement);
    EXPECT_DOUBLE_EQ(*changed_tcp.moving_replacement->changed_future
                          .peak_tcp_linear_speed_mps,
                     0.0);
    auto different_base = moving_options;
    different_base.retained_prefix->base_frame = tcp_frame.compiled_frame_name;
    const auto changed_base = validate(measured_trajectory, different_base);
    ASSERT_TRUE(changed_base.valid && changed_base.moving_replacement);
    EXPECT_NEAR(*changed_base.moving_replacement->unchanged_prefix
                     .peak_tcp_linear_speed_mps,
                0.0, 1e-12);
    EXPECT_GT(*changed_base.moving_replacement->changed_future
                   .peak_tcp_linear_speed_mps,
              0.0);

    auto excessive_seam_acceleration = seam_trajectory;
    excessive_seam_acceleration.waypoints[1].time_from_start_s = 0.1;
    excessive_seam_acceleration.waypoints[2].time_from_start_s = 0.2;
    excessive_seam_acceleration.waypoints[2].positions_rad[0] = 0.19;
    auto short_options = moving_options;
    short_options.retained_prefix->end_time_s = 0.1;
    const auto refused_acceleration =
        validate(excessive_seam_acceleration, short_options);
    ASSERT_FALSE(refused_acceleration.valid);
    ASSERT_TRUE(refused_acceleration.error);
    EXPECT_EQ(refused_acceleration.error->details.at("reason"),
              "trajectory_segment_acceleration_limit_exceeded");

    auto missing_boundary = moving_options;
    missing_boundary.retained_prefix->end_time_s = 0.4;
    const auto missing = validate(seam_trajectory, missing_boundary);
    ASSERT_FALSE(missing.valid);
    ASSERT_TRUE(missing.error);
    EXPECT_EQ(missing.error->details.at("reason"),
              "trajectory_retained_prefix_boundary_missing");
    for (const double boundary :
         {0.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
      auto invalid = moving_options;
      invalid.retained_prefix->end_time_s = boundary;
      EXPECT_FALSE(validate(seam_trajectory, invalid).valid);
    }
    auto invalid_cap = moving_options;
    invalid_cap.retained_prefix->max_tcp_linear_speed_mps = 0.0;
    EXPECT_FALSE(validate(seam_trajectory, invalid_cap).valid);
    auto invalid_frames = moving_options;
    invalid_frames.retained_prefix->tcp_frame.clear();
    EXPECT_FALSE(validate(seam_trajectory, invalid_frames).valid);
    return;
  }
  auto over_speed_trajectory = stationary_trajectory;
  over_speed_trajectory.waypoints.back().time_from_start_s = 0.1;
  over_speed_trajectory.waypoints.back().positions_rad.front() = 1.0;
  const auto over_speed_validation =
      runtime->kinematic_model->validate_non_collision(
          over_speed_trajectory, core::ValidationOptionsDTO{0.0, {}, {}, false},
          cancellation);
  ASSERT_FALSE(over_speed_validation.valid);
  ASSERT_TRUE(over_speed_validation.error.has_value());
  EXPECT_EQ(over_speed_validation.error->details.at("reason"),
            "trajectory_segment_velocity_limit_exceeded");
  EXPECT_NEAR(std::stod(over_speed_validation.error->details.at("limit")), 2.14,
              1e-12);
  const auto expanded =
      runtime->kinematic_model->expand_for_collision(
          stationary_trajectory, zero_arm_state);
  ASSERT_TRUE(expanded.ok()) << expanded.error().message;
  ASSERT_EQ(expanded.value().waypoints.size(), 2U);
  EXPECT_EQ(expanded.value().waypoints.front().positions_rad.size(),
            expanded.value().joint_names.size());

  const auto motion_accepted =
      runtime->addressed_kernel->execute(make_motion_operation(
          arm_binding, owner_epoch,
          inventory->projection_binding.projection_revision.scene_revision));
  ASSERT_FALSE(motion_accepted.ok());
  EXPECT_EQ(motion_accepted.error().details.at("reason"),
            "runtime_route_publication_required");

  const auto gripper_accepted = runtime->addressed_kernel->execute(
      make_gripper_operation(gripper_binding, owner_epoch));
  ASSERT_FALSE(gripper_accepted.ok());
  EXPECT_EQ(gripper_accepted.error().details.at("reason"),
            "runtime_route_publication_required");

  const auto idle = admission_gate.Observe(physical_scope);
  EXPECT_EQ(idle.affected_queued_operation_count, 0U);
  EXPECT_EQ(idle.affected_active_operation_count, 0U);
  EXPECT_EQ(runtime->participant_engine->get_metrics().executions_completed,
            0U);
  if (check == Lite6AggregateCheck::kRefreshCellModel)
    check_refreshed_cell_model(*runtime, admission_gate, stage,
                               *materialized.bundle, model_map, retained,
                               arm_binding, gripper_binding,
                               tcp_frame.compiled_frame_name);
}

// The retained controller's whole-cell model changes around it: its root is
// moved in the cell and a peer joint is added ahead of it, shifting every one
// of its model coordinates. The controller's own model facts are unchanged.
void check_refreshed_cell_model(
    RetainedNativeRuntime &runtime, ManagedKernelAdmissionGate &admission_gate,
    const RetainedPlanningModelStage &stage,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::VerifiedRetainedPlanningModel &retained,
    const core::RuntimeParticipantBinding &arm_binding,
    const core::RuntimeParticipantBinding &gripper_binding,
    const std::string &tcp_frame) {
  const std::string predecessor_urdf(stage.loader_urdf_bytes());
  const std::string srdf(stage.srdf_bytes());
  const auto cell_start = predecessor_urdf.find("<link name=\"mf1_cell_");
  ASSERT_NE(cell_start, std::string::npos);
  const auto cell_name_start = cell_start + std::string("<link name=\"").size();
  const auto cell_link = predecessor_urdf.substr(
      cell_name_start, predecessor_urdf.find('"', cell_name_start) -
                           cell_name_start);
  const auto moved_root = [&](std::string urdf) {
    const auto parent =
        urdf.find("<parent link=\"" + cell_link + "\"/>");
    const auto joint_open = urdf.rfind("<joint ", parent);
    const auto joint_body = urdf.find('>', joint_open) + 1U;
    urdf.insert(joint_body, "<origin xyz=\"0.3 -0.2 0.1\"/>");
    return urdf;
  };
  const auto with_peer = [&](std::string urdf) {
    const auto first_joint = urdf.find("<joint ");
    urdf.insert(first_joint,
                "<link name=\"peer_link\"><collision><geometry><sphere "
                "radius=\"0.05\"/></geometry></collision></link>"
                "<joint name=\"aaa_peer_joint\" type=\"revolute\"><origin "
                "xyz=\"1 0 0\"/><parent link=\"" +
                    cell_link +
                    "\"/><child link=\"peer_link\"/><axis xyz=\"0 0 1\"/>"
                    "<limit lower=\"-1\" upper=\"1\" effort=\"1\" "
                    "velocity=\"1\" acceleration=\"1\"/></joint>");
    return urdf;
  };
  const auto make_candidate =
      [&](const std::string &urdf,
          std::vector<RoboPlanJointLimitInput> limits,
          std::vector<core::RuntimeParticipantBinding> projected) {
        auto scene = make_roboplan_scene(RoboPlanSceneInput{
            "retained_lite6_candidate_cell", urdf, srdf, stage.package_paths(),
            std::move(limits)});
        auto scene_mutex = std::make_shared<std::mutex>();
        auto projector = std::make_shared<RetainedKinematicStateProjector>(
            RetainedKinematicStateProjector::create_verified(
                scene, scene_mutex, bundle, model_map, retained,
                std::move(projected), 0));
        return make_retained_motion_model_generation(
            arm_binding, std::move(scene), std::move(scene_mutex),
            std::move(projector));
      };
  const auto candidate_urdf = with_peer(moved_root(predecessor_urdf));
  // Limits are read from the closure-local retained URDF, edited identically.
  const std::string retained_urdf(retained.urdf_bytes());
  const auto limits_of = [&](const std::string &urdf) {
    return retained_joint_limit_rows(
        urdf, retained.srdf_bytes().empty()
                  ? std::optional<std::string_view>{}
                  : std::optional<std::string_view>{retained.srdf_bytes()});
  };
  const auto candidate_limits =
      limits_of(with_peer(moved_root(retained_urdf)));
  const std::vector<core::RuntimeParticipantBinding> both{arm_binding,
                                                          gripper_binding};
  const auto candidate = make_candidate(candidate_urdf, candidate_limits, both);

  const auto predecessor = runtime.kinematic_model->current_generation();
  const auto predecessor_projector =
      runtime.motion_state_channel->kinematic_projector();
  ASSERT_EQ(predecessor_projector, predecessor->state_projector);
  const auto &motion = arm_binding.value();
  const auto joint_names =
      runtime.kinematic_model->group_joint_names(motion.command_target_id.value());
  ASSERT_TRUE(joint_names.ok());
  const core::RobotStateDTO zero_state{
      joint_names.value(), std::vector<double>(joint_names.value().size(), 0.0),
      {}};
  const auto world_tcp_before = runtime.kinematic_model->forward_kinematics(
      core::FkRequestDTO{zero_state, tcp_frame, ""});
  ASSERT_TRUE(world_tcp_before.ok()) << world_tcp_before.error().message;

  const core::ConfigurationFenceId fence_id("cell-model.refresh.apply");
  const core::ConfigurationFenceScopeDTO scope{};
  const auto refresh = [&](auto generation) {
    return refresh_retained_native_runtime_model_for_apply(
        runtime, RetainedNativeRuntimeModelRefreshInput{
                     std::move(generation), fence_id, scope, admission_gate});
  };
  const auto unfenced = refresh(candidate);
  ASSERT_FALSE(unfenced.ok());
  EXPECT_EQ(unfenced.error().details.at("reason"),
            "native_model_refresh_fence_not_held");
  EXPECT_EQ(runtime.kinematic_model->current_generation(), predecessor);
  admission_gate.RestoreFence(fence_id, scope);

  // A candidate that changes the controller's own kinematic target is not a
  // retain; it is refused before any consumer changes.
  auto slower_limits = candidate_limits;
  for (auto &row : slower_limits)
    if (row.joint_name == joint_names.value().front())
      row.maximum_velocity = {1.0};
  const auto changed = refresh(make_candidate(candidate_urdf, slower_limits, both));
  ASSERT_FALSE(changed.ok());
  EXPECT_EQ(changed.error().details.at("reason"),
            "native_model_refresh_controller_model_changed");
  EXPECT_EQ(runtime.kinematic_model->current_generation(), predecessor);
  EXPECT_EQ(runtime.motion_state_channel->kinematic_projector(),
            predecessor_projector);

  const auto refreshed = refresh(candidate);
  ASSERT_TRUE(refreshed.ok()) << refreshed.error().message;
  EXPECT_EQ(runtime.kinematic_model->current_generation(), candidate);
  EXPECT_EQ(runtime.motion_state_channel->kinematic_projector(),
            candidate->state_projector);
  EXPECT_EQ(runtime.gripper_state_channel->kinematic_projector(),
            candidate->state_projector);
  EXPECT_TRUE(admission_gate.exact_fence_is_held(fence_id, scope));

  // World-frame FK follows the moved root exactly.
  const auto world_tcp_after = runtime.kinematic_model->forward_kinematics(
      core::FkRequestDTO{zero_state, tcp_frame, ""});
  ASSERT_TRUE(world_tcp_after.ok()) << world_tcp_after.error().message;
  EXPECT_NEAR(world_tcp_after.value().position_m.x_m -
                  world_tcp_before.value().position_m.x_m, 0.3, 1e-9);
  EXPECT_NEAR(world_tcp_after.value().position_m.y_m -
                  world_tcp_before.value().position_m.y_m, -0.2, 1e-9);
  EXPECT_NEAR(world_tcp_after.value().position_m.z_m -
                  world_tcp_before.value().position_m.z_m, 0.1, 1e-9);

  // Observations project into the candidate scene at the candidate's own
  // coordinate indices: the arm and the model-backed gripper.
  const std::vector<double> arm_positions{0.1, -0.2, 1.2, 0.3, -0.4, 0.5};
  const auto now = runtime.clock->now();
  ASSERT_TRUE(runtime.motion_state_channel
                  ->publish_and_project(arm_positions,
                                        std::vector<double>(6U, 0.0), now, now)
                  .applied());
  ASSERT_TRUE(runtime.gripper_state_channel
                  ->publish_and_project(std::vector<double>{0.004},
                                        std::vector<double>{0.0}, now, now)
                  .applied());
  const auto arm_context = runtime.motion_state_channel->query_context();
  const auto arm_state = candidate->state_projector->current_state(
      arm_context.control_binding_id, arm_context.control_binding_revision);
  ASSERT_TRUE(arm_state.ok()) << arm_state.error().message;
  for (std::size_t index = 0U; index < arm_positions.size(); ++index)
    EXPECT_DOUBLE_EQ(std::get<core::AngularPositionDTO>(
                         arm_state.value().positions[index]).value_rad,
                     arm_positions[index]);
  const auto gripper_context = runtime.gripper_state_channel->query_context();
  const auto gripper_state = candidate->state_projector->current_state(
      gripper_context.control_binding_id,
      gripper_context.control_binding_revision);
  ASSERT_TRUE(gripper_state.ok()) << gripper_state.error().message;
  EXPECT_DOUBLE_EQ(std::get<core::LinearPositionDTO>(
                       gripper_state.value().positions.front()).value_m,
                   0.004);

  // Collision expansion names every candidate coordinate and places the
  // target's values by name, including the added peer.
  const core::TrajectoryDTO trajectory{
      motion.resources.empty() ? std::vector<core::ResourceId>{}
                               : [&] {
                                   std::vector<core::ResourceId> ids;
                                   for (const auto &resource : motion.resources)
                                     ids.push_back(resource.resource_id);
                                   return ids;
                                 }(),
      joint_names.value(),
      {core::TrajectoryWaypointDTO{0.0, arm_positions, {}, {}}}};
  const auto expanded = runtime.kinematic_model->expand_for_collision(
      trajectory, core::RobotStateDTO{joint_names.value(), arm_positions, {}});
  ASSERT_TRUE(expanded.ok()) << expanded.error().message;
  const auto &names = expanded.value().joint_names;
  EXPECT_EQ(std::count(names.begin(), names.end(), "aaa_peer_joint"), 1);
  for (std::size_t index = 0U; index < joint_names.value().size(); ++index) {
    const auto found =
        std::find(names.begin(), names.end(), joint_names.value()[index]);
    ASSERT_NE(found, names.end());
    EXPECT_DOUBLE_EQ(expanded.value().waypoints.front().positions_rad
                         [static_cast<std::size_t>(found - names.begin())],
                     arm_positions[index]);
  }

  // Peer removal: the next candidate drops the peer again. A candidate whose
  // projector omits the gripper target fails after the arm consumers were
  // installed; the failure is returned as is, nothing is restored and the
  // Apply fence keeps admission closed.
  const auto removed_urdf = moved_root(predecessor_urdf);
  const auto removed = make_candidate(
      removed_urdf,
      limits_of(moved_root(retained_urdf)),
      {arm_binding});
  const auto partial = refresh(removed);
  ASSERT_FALSE(partial.ok());
  EXPECT_EQ(partial.error().details.at("reason"),
            "retained_state_projector_target_mismatch");
  EXPECT_EQ(runtime.kinematic_model->current_generation(), removed);
  EXPECT_EQ(runtime.gripper_state_channel->kinematic_projector(),
            candidate->state_projector);
  EXPECT_TRUE(admission_gate.exact_fence_is_held(fence_id, scope));
}

TEST(RetainedProductCrossConsumer,
     SelectedCoreLite6AggregateComposesButUnqualifiedParticipantsStayClosed) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kCompose);
}

TEST(RetainedProductCrossConsumer,
     MovingValidationPreservesJointSeamAndUsesEachTcpFrameAndLimit) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kMovingValidation);
}

TEST(RetainedProductCrossConsumer,
     MissingSelectedBackendRefusesInsteadOfConstructingASimulator) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kMissingSelectedBackend);
}

TEST(RetainedProductCrossConsumer,
     SameRoutePredecessorAdoptionPreservesExactScaleQualificationWithoutOpeningReadiness) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kSameRoutePredecessorQualification);
}

TEST(RetainedNativeMeasuredRoutePublication,
     MovingBudgetSurvivesRouteRebuildAndReadinessPublication) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kMovingBudgetPublication);
}

#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
TEST(RetainedNativeMeasuredRoutePublication,
     ActualMixedOutputResolvesAndDispatchesAcrossArmReadinessAndRouteAdoption) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kMixedOutputPublication);
}

TEST(RetainedProductCrossConsumer,
     ActualGeometricContactInstallsMaintainedGripperEndpointThroughHostIo) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kGeometricHostInstallation);
}
#endif

TEST(RetainedProductCrossConsumer,
     QualifiedEndpointPlannerFramesUseTheSealedSameModelInventory) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kQualifiedEndpointPlannerFrames);
}

TEST(RetainedNativeMeasuredRoutePublication,
     ResolverRebuildRefusalsKeepExactErrorsAndDoNotRebindOrPublish) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kResolverRebuildFailures);
}

TEST(RetainedNativeMeasuredRoutePublication,
     StopResponseReadsTheCurrentBindingAfterRouteRebind) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kStopResponseFollowsRouteRebind);
}

TEST(RetainedNativeMeasuredRoutePublication, ExactStagePrecedesRebindAndRealThenNullCommitPreserveKernelPublication) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kMeasuredPublication);
}
TEST(RetainedNativeMeasuredRoutePublication, RealStageRefusesOptionalDomainAppearanceBeforePhysicalEffects) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kMeasuredOptionalDomainRace);
}
TEST(RetainedNativeMeasuredRoutePublication, NullStageRefusesOptionalDomainAppearanceBeforePhysicalEffects) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kMeasuredNullOptionalDomainRace);
}

TEST(RetainedProductCrossConsumer,
     RetainedControllerRefreshesEveryModelConsumerWithTheCellModel) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kRefreshCellModel);
}

TEST(RetainedProductCrossConsumer,
     SameControllerCapabilityInventoryResealPreservesOwnersAndRejectsChangedRegistration) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kResealInventory);
}

TEST(RetainedProductCrossConsumer,
     RetainedRouteGenerationRebasePreservesQualificationThroughStrictReseal) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kRetainedRouteGenerationReseal);
}

TEST(RetainedProductCrossConsumer,
     ExecutionPortParticipantRegistersThroughTheRetainedBackendSeam) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kExecutionPortParticipant);
}

#ifdef MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY
TEST(RetainedProductCrossConsumer,
     SelectedRosOrdinaryJerkRefusesBeforeBackendFactoryConstruction) {
  check_selected_core_lite6_aggregate(Lite6AggregateCheck::kRosOrdinaryJerkRejection);
}
#endif

TEST(RetainedProductCrossConsumer,
     DeploymentAuthoredFaultStopProfileIsTheMotionFaultProfile) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFaultStopProfile);
}

TEST(RetainedProductCrossConsumer,
     DeploymentAuthoredFastFaultProfileSurvivesRegistrationAndStopPreparation) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFastFaultStopProfile);
}

TEST(RetainedProductCrossConsumer,
     ComponentFaultStopProfileProjectsExtraJointsOntoActualArmResources) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFaultStopProfileWithExtraJoints);
}

TEST(RetainedProductCrossConsumer,
     ComponentFaultStopProfileWithOnlyExtraSettleOverridesUsesRetainedDefaults) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFaultStopProfileWithOnlyExtraSettleOverrides);
}

TEST(RetainedProductCrossConsumer,
     ComponentFaultStopProfileKeepsSparseSettleOverridesAndOtherRetainedDefaults) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFaultStopProfileWithSparseSettleOverrides);
}

TEST(RetainedProductCrossConsumer,
     ComponentFaultStopProfileMissingArmJointStillRefusesAtPrepare) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFaultStopProfileMissingArmJoint);
}

TEST(RetainedProductCrossConsumer,
     ComponentFaultStopProfileWithoutArmJointsStillRefusesAtPrepare) {
  check_selected_core_lite6_aggregate(
      Lite6AggregateCheck::kDeploymentFaultStopProfileWithoutArmJoints);
}

TEST(RetainedProductCrossConsumer,
     RootOwnedJoinedTwoLite6TeardownRetiresKernelBeforePeerEngines) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  constexpr std::string_view cell_id = "i6_joined_teardown_cell";
  const auto materialized = deployment::test::
      materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto &graph = bundle.value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.controllable_components.size(), 2U);
  const auto &shared_runtime_unit_id = graph.runtime_units.front().runtime_unit_id;
  for (const auto &component : graph.controllable_components) {
    ASSERT_EQ(component.runtime_unit_ids.size(), 1U);
    EXPECT_EQ(component.runtime_unit_ids.front(), shared_runtime_unit_id);
  }
  // Q68: both selected ROS components belong to this exact shared host.
  // Cross-unit isolation remains NOT_RUN/R7 after native simulator retirement.
  const auto &products = bundle.value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root,
      {bundle.retained_bundle_id(),
       products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root,
      {bundle.retained_bundle_id(),
       products.symbol_role_maps.front().product_id});
  deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
  const auto program_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          bundle, model_map, catalog_store);
  ASSERT_EQ(program_inventory.motion.size(), 2U);
  ASSERT_TRUE(program_inventory.gripper.empty());

  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  physical_bindings.reserve(program_inventory.motion.size());
  std::set<core::ResourceId> physical_resources;
  std::set<core::ControllableComponentId> component_ids;
  std::set<core::RuntimeUnitId> runtime_unit_ids;
  std::size_t physical_resource_count = 0U;
  for (const auto &program_binding : program_inventory.motion) {
    ASSERT_NE(program_binding, nullptr);
    ASSERT_EQ(program_binding->value().component_role_bindings.size(), 1U);
    const auto component_id = program_binding->value()
                                  .component_role_bindings.front()
                                  .controllable_component_id;
    auto physical = with_program_binding_identity(
        make_fixture_runtime_participant(
            bundle, model_map, component_id,
            core::CommandTargetId(
                std::string(deployment::test::kLite6ArmTargetId))),
        *program_binding);
    ASSERT_TRUE(physical.value().control_path.has_value());
    EXPECT_TRUE(component_ids.insert(component_id).second);
    runtime_unit_ids.insert(
        physical.value().control_path->provider_runtime_unit_id);
    for (const auto &resource : physical.value().resources) {
      ++physical_resource_count;
      EXPECT_TRUE(physical_resources.insert(resource.resource_id).second)
          << resource.resource_id.value();
    }
    physical_bindings.push_back(std::move(physical));
  }
  ASSERT_EQ(component_ids.size(), 2U);
  ASSERT_EQ(runtime_unit_ids.size(), 1U);
  EXPECT_EQ(*runtime_unit_ids.begin(), shared_runtime_unit_id);
  ASSERT_EQ(physical_resources.size(), physical_resource_count);

  auto model_stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  const auto joint_limits = retained_joint_limit_rows(retained);
  const auto initial_scene = make_projection_runtime_shadow_snapshot(
      bundle.value().cell_graph.cell_id);
  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtimes;
  runtimes.reserve(physical_bindings.size());
  for (std::size_t index = 0U; index < physical_bindings.size(); ++index) {
    const auto &physical = physical_bindings[index];
    const auto runtime_name =
        "retained_lite6_joined_teardown_" + std::to_string(index + 1U);
    auto scene = make_roboplan_scene(RoboPlanSceneInput{
        runtime_name, std::string(model_stage.loader_urdf_bytes()),
        std::string(model_stage.srdf_bytes()), model_stage.package_paths(),
        joint_limits});
    auto scene_mutex = std::make_shared<std::mutex>();
    auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
        RetainedKinematicStateProjector::create_verified(
            scene, scene_mutex, bundle, model_map, retained,
            std::vector<core::RuntimeParticipantBinding>{physical}, 0));

    ASSERT_EQ(physical.value().model_groups.size(), 1U);
    auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
        runtime_name, model_stage.loader_urdf_path(), model_stage.srdf_path(),
        model_stage.package_paths(),
        physical.value().model_groups.front().compiled_group_name);
    auto collision_projector =
        RetainedCollisionWorldProjector::create_verified(
            shared_collision_model, bundle, model_map, retained);
    const auto &control = *physical.value().control_path;
    const core::ProjectionNativeRuntimeBindingDTO native_binding{
        core::CapabilityProviderBindingId("lite6.joined.projection." +
                                          std::to_string(index + 1U)),
        core::CapabilityProviderBindingRevision(
            "lite6-joined-projection-v1"),
        control.provider_runtime_unit_id, control.provider_runtime_epoch};
    auto projection_clock = std::make_shared<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
        std::move(collision_projector), products.planning_collision_model,
        native_binding, *initial_scene.shared(),
        RetainedPlannerSceneProjectionConfig{runtime_name, 32U, 32U},
        [projection_clock] { return (*projection_clock)++; });
    ASSERT_TRUE(projection_result.ok())
        << projection_result.error().message;
    auto projection = std::move(projection_result.value());
    const auto projection_inventory =
        projection->current_scene_snapshot_lease_inventory();
    ASSERT_TRUE(projection_inventory.has_value());

    const auto candidate_binding = make_candidate_binding(physical);
    ProcessContainedCandidateClientConfig candidate_config;
    candidate_config.worker_executable =
        MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH;
    candidate_config.require_retained_projection = true;
    auto planner = std::make_unique<RoboPlanPlannerBackend>(
        projection_inventory->projection_binding, candidate_binding,
        std::make_unique<ProcessContainedRoboPlanCandidateClient>(
            RoboPlanWorkerModelDTO{runtime_name + "_worker",
                                   model_stage.loader_urdf_path(),
                                   model_stage.srdf_path(),
                                   model_stage.package_paths(), joint_limits,
                                   {}, {}},
            candidate_binding,
            RoboPlanCandidateComputerConfig{
                .rng_seed = static_cast<std::uint64_t>(17U + index)},
            std::move(candidate_config)));
    RetainedNativeRuntimeConfig runtime_config{
        core::RequestOwnerEpoch("i6.joined.prejoin." +
                                std::to_string(index + 1U)),
        bundle.value().cell_graph.cell_id,
        temporary.path() /
            ("unused-joined-" + std::to_string(index + 1U) + ".sqlite3"),
        1024U,
        core::CacheConfig{},
        core::ExecutionKernelConfig{},
        SimpleSchedulerConfig{},
        10'000'000,
        std::vector<double>(physical.value().resources.size(), 0.0),
          kMaintainedNativeStopSurveillancePolicy,
        {},
        {},
        {},
        {},
        {}};
    const auto local_runtime_unit_id = control.provider_runtime_unit_id;
    const auto local_runtime_epoch = control.provider_runtime_epoch;
    const auto local_component_id = physical.value().controllable_component_id;
    auto runtime_input = RetainedNativeRuntimeInput{
        physical,
        std::nullopt,
        {},
        {},
        {},
        {},
        scene,
        scene_mutex,
        state_projector,
        projection,
        shared_collision_model,
        std::move(planner),
        [shared_collision_model, bundle_root = materialized.bundle, &model_map,
         &retained, local_runtime_unit_id, local_runtime_epoch,
         local_component_id](const auto &channels) {
          std::vector<RetainedSceneFrameStateSourceInput> state_sources;
          state_sources.reserve(channels.size());
          for (const auto &channel : channels) {
            state_sources.push_back(RetainedSceneFrameStateSourceInput{
                channel->query_context(), channel->source_kind(),
                std::shared_ptr<const core::IObservedStateProvider>(
                    channel, channel->addressed_publication())});
          }
          return RetainedSceneFrameResolver::create_verified(
              shared_collision_model, *bundle_root, model_map, retained,
              local_runtime_unit_id, local_runtime_epoch,
              std::move(state_sources), local_component_id);
        },
        {},
        {},
        nullptr,
        {},
        {},
        {},
        {}};
    runtime_input.deployment_maximum_tcp_linear_speed_mps = 0.15;
    core::ExecutionPortParticipant *selected_participant = nullptr;
    runtime_input.motion_state_channel_config = selected_core_test_state_config(physical);
    runtime_input.backend_factory = selected_core_test_backend_factory(selected_participant);

    std::shared_ptr<RetainedNativeRuntime> runtime;
    if (runtimes.empty()) {
      runtime = create_retained_native_runtime_participants(
          std::move(runtime_input), std::move(runtime_config));
    } else {
      runtime = create_retained_native_runtime_participants(
          std::move(runtime_input), std::move(runtime_config),
          runtimes.front()->clock, runtimes.front()->scheduler);
    }
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(runtime->motion_participant_policy.has_value());
    EXPECT_FALSE(
        runtime->motion_participant_policy->value().ordinary_motion_jerk);
    ASSERT_NE(runtime->participant_engine, nullptr);
    ASSERT_NE(runtime->motion_state_channel, nullptr);
    ASSERT_NE(runtime->reference_resolver, nullptr);
    EXPECT_EQ(runtime->motion_binding.value(), physical.value());
    EXPECT_EQ(runtime->motion_state_channel->binding().value(),
              physical.value());
    EXPECT_FALSE(runtime->gripper_binding.has_value());
    EXPECT_EQ(runtime->gripper_state_channel, nullptr);
    runtimes.push_back(std::move(runtime));
  }

  ASSERT_EQ(runtimes.size(), 2U);
  auto primary = std::move(runtimes[0]);
  auto peer = std::move(runtimes[1]);
  runtimes.clear();
  ASSERT_EQ(primary->clock, peer->clock);
  ASSERT_EQ(primary->scheduler, peer->scheduler);

  // Product reseal must not be used to disguise a physical model change as a
  // retained controller. Reject that mismatch before touching either state
  // channel or the backend route.
  auto changed_model_dto = primary->motion_binding.value();
  changed_model_dto.component_type_revision =
      core::ComponentTypeRevision("foreign-retained-controller-revision");
  auto changed_model_binding = core::verify_runtime_participant_binding(
      std::move(changed_model_dto));
  ASSERT_TRUE(changed_model_binding.ok())
      << changed_model_binding.error().message;
  auto changed_model_registrations = primary->participant_registrations;
  ASSERT_EQ(changed_model_registrations.size(), 1U);
  changed_model_registrations.front().binding = changed_model_binding.value();
  ManagedKernelAdmissionGate reseal_gate;
  const core::ConfigurationFenceId reseal_fence_id(
      "i6.joined.invalid-retained-reseal.apply");
  const core::ConfigurationFenceScopeDTO empty_reseal_scope{};
  reseal_gate.RestoreFence(reseal_fence_id, empty_reseal_scope);
  const auto unchanged_state_binding = primary->motion_state_channel->binding();
  const auto invalid_reseal = reseal_retained_native_runtime_for_apply(
      *primary,
      RetainedNativeRuntimeResealInput{
          changed_model_binding.value(), std::nullopt, {}, {},
          {changed_model_binding.value()},
          std::move(changed_model_registrations), {}, nullptr,
          primary->reference_resolver,
          reseal_fence_id, empty_reseal_scope, reseal_gate});
  ASSERT_FALSE(invalid_reseal.ok());
  EXPECT_EQ(invalid_reseal.error().details.at("reason"),
            "native_product_reseal_physical_identity_changed");
  EXPECT_EQ(primary->motion_binding.value(), physical_bindings.front().value());
  EXPECT_EQ(primary->motion_state_channel->binding().value(),
            unchanged_state_binding.value());

  auto subscription_ledger =
      std::make_shared<SubscriptionObservationLedger>();
  auto retained_primary_engine = primary->participant_engine;
  auto retained_peer_engine = peer->participant_engine;
  std::weak_ptr<core::ExecutionKernel> primary_engine_lifetime =
      retained_primary_engine;
  std::weak_ptr<core::ExecutionKernel> peer_engine_lifetime =
      retained_peer_engine;
  auto primary_subscription =
      std::make_shared<SubscriptionObservedParticipantEngine>(
          retained_primary_engine, subscription_ledger, "primary");
  auto *const peer_observation = peer.get();
  auto peer_subscription =
      std::make_shared<SubscriptionObservedParticipantEngine>(
          retained_peer_engine, subscription_ledger, "peer",
          [peer_observation] {
            return peer_observation->cell_addressed_kernel == nullptr &&
                   peer_observation->cell_runtime_root == nullptr;
          });
  std::weak_ptr<SubscriptionObservedParticipantEngine>
      primary_subscription_lifetime = primary_subscription;
  std::weak_ptr<SubscriptionObservedParticipantEngine>
      peer_subscription_lifetime = peer_subscription;

  primary->participant_engine = std::shared_ptr<core::ExecutionKernel>(
      primary_subscription, retained_primary_engine.get());
  peer->participant_engine = std::shared_ptr<core::ExecutionKernel>(
      peer_subscription, retained_peer_engine.get());
  std::size_t primary_observed_registrations = 0U;
  for (auto &registration : primary->participant_registrations) {
    if (registration.engine == retained_primary_engine.get()) {
      registration.engine = primary_subscription.get();
      ++primary_observed_registrations;
    }
  }
  std::size_t peer_observed_registrations = 0U;
  for (auto &registration : peer->participant_registrations) {
    if (registration.engine == retained_peer_engine.get()) {
      registration.engine = peer_subscription.get();
      ++peer_observed_registrations;
    }
  }
  ASSERT_EQ(primary_observed_registrations, 1U);
  ASSERT_EQ(peer_observed_registrations, 1U);

  RetainedRuntimeParticipantResolverFactory joined_resolver_factory =
      [](std::vector<core::RuntimeParticipantBinding> joined_bindings)
      -> RetainedRuntimeParticipantResolver {
    for (auto &binding : joined_bindings) {
      if (binding.value().capability_id !=
          core::CapabilityId("execution.trajectory.execute"))
        continue;
      auto value = binding.value();
      const auto tcp =
          std::find_if(value.frame_bindings.begin(), value.frame_bindings.end(),
                       [](const auto &row) {
                         return row.frame_role_id == core::FrameRoleId("tcp");
                       });
      if (tcp == value.frame_bindings.end())
        throw std::logic_error("joined motion fixture has no exact TCP frame");
      value.motion_endpoint = core::RuntimeMotionEndpointBindingDTO{
          value.target_role_id, value.command_target_id, *tcp, {}};
      auto captured =
          core::verify_runtime_participant_binding(std::move(value));
      if (!captured.ok())
        throw std::logic_error(captured.error().message);
      binding = std::move(captured.value());
    }
    auto registry = std::make_shared<core::RuntimeParticipantRegistry>(
        std::move(joined_bindings));
    return
        [registry](const core::OperationIntentDTO &operation,
                   const std::size_t participant_index)
            -> core::Result<RetainedResolvedParticipant> {
          auto binding = registry->resolve(operation, participant_index);
          if (!binding.ok()) {
            return core::Result<RetainedResolvedParticipant>::Err(
                binding.error());
          }
          return core::Result<RetainedResolvedParticipant>::Ok(
              {std::move(binding.value()), false});
        };
  };
  join_retained_native_runtime_participants(
      primary, {peer}, joined_resolver_factory);
  attach_retained_native_addressed_owner(
      primary,
      RetainedNativeAddressedOwnerConfig{
          core::RequestOwnerEpoch("i6.joined.owner.epoch.1"),
          bundle.value().cell_graph.cell_id,
          temporary.path() / "joined-operation-journal.sqlite3",
          1024U,
          nullptr,
          bundle.value().cell_graph.graph_revision,
              {},
          {},
          {},
          {},
          {}});

  ASSERT_NE(primary->addressed_kernel, nullptr);
  EXPECT_TRUE(primary->addressed_kernel->participant_engine_in_use(
      primary_subscription.get()));
  EXPECT_TRUE(primary->addressed_kernel->participant_engine_in_use(
      peer_subscription.get()));
  const auto subscribed = subscription_ledger->Snapshot();
  ASSERT_EQ(subscribed.active_subscriptions, 2U);
  ASSERT_EQ(subscribed.subscriptions, 2U);
  ASSERT_EQ(subscribed.unsubscriptions, 0U);
  ASSERT_EQ(peer->cell_addressed_kernel, primary->addressed_kernel.get());
  ASSERT_EQ(peer->cell_runtime_root, primary.get());

  // Re-sealing a joined peer excludes every route reader of that peer for
  // its whole duration. Those readers serialize on the cell root's mutex, so
  // the re-seal must hold the root's mutex, not the peer's own: a reader
  // started while the backend route is being re-bound may not complete until
  // the re-seal returns.
  {
    ManagedKernelAdmissionGate peer_reseal_gate;
    const core::ConfigurationFenceId peer_reseal_fence_id(
        "i6.joined.peer-reseal.apply");
    const core::ConfigurationFenceScopeDTO peer_reseal_scope{};
    peer_reseal_gate.RestoreFence(peer_reseal_fence_id, peer_reseal_scope);
    // This fixture observes the peer engine through a subscription wrapper;
    // re-seal resolves registrations against the retained engine itself.
    const auto observed_registrations = peer->participant_registrations;
    for (auto &registration : peer->participant_registrations) {
      if (registration.engine == peer_subscription.get())
        registration.engine = peer->participant_engine_for(true);
    }
    const auto retained_rebind = peer->rebind_motion_route;
    ASSERT_TRUE(retained_rebind);
    std::optional<std::future_status> reader_during_reseal;
    std::future<bool> reader;
    peer->rebind_motion_route =
        [&](core::RuntimeParticipantBinding binding,
            std::vector<core::RuntimeParticipantBinding> aliases) {
          // A reader that only needs the route lock: the peer has no
          // gripper, so it answers stale as soon as it holds the lock.
          reader = std::async(std::launch::async, [&] {
            return !peer->ReadGripperStatus(peer->motion_binding).ok();
          });
          reader_during_reseal =
              reader.wait_for(std::chrono::milliseconds(200));
          return retained_rebind(std::move(binding), std::move(aliases));
        };
    const auto resealed = reseal_retained_native_runtime_for_apply(
        *peer,
        RetainedNativeRuntimeResealInput{
            peer->motion_binding, std::nullopt, {}, {},
            peer->control_binding_representatives,
            peer->participant_registrations, {}, nullptr,
            peer->reference_resolver, peer_reseal_fence_id, peer_reseal_scope,
            peer_reseal_gate});
    peer->rebind_motion_route = retained_rebind;
    peer->participant_registrations = observed_registrations;
    ASSERT_TRUE(resealed.ok()) << resealed.error().message;
    ASSERT_TRUE(reader_during_reseal.has_value());
    EXPECT_EQ(*reader_during_reseal, std::future_status::timeout);
    ASSERT_TRUE(reader.valid());
    EXPECT_TRUE(reader.get());
  }

  auto stable_stop_observer = primary->stop_surveillance_owner;
  ASSERT_NE(stable_stop_observer, nullptr);
  std::weak_ptr<RetainedStopSurveillanceOwner> stable_observer_lifetime =
      stable_stop_observer;
  ManagedKernelAdmissionGate rejoin_gate;
  const auto rejoin_scope = make_runtime_fence_scope(
      {&primary->motion_binding, &peer->motion_binding});
  const core::ConfigurationFenceId rejoin_fence_id(
      "i6.joined.root-replacement.apply");
  rejoin_gate.RestoreFence(rejoin_fence_id, rejoin_scope);
  ASSERT_TRUE(primary->stop_surveillance_policy.has_value());
  const auto original_surveillance_policy = primary->stop_surveillance_policy;
  {
    ++primary->stop_surveillance_policy->sample_cadence_ns;
    std::vector<RetainedStopSurveillanceSourceInput> rejected_sources;
    const auto rejected = rejoin_retained_native_runtime_participants(
        peer, {primary}, joined_resolver_factory, stable_stop_observer,
        rejected_sources, rejoin_gate, rejoin_fence_id, rejoin_scope);
    primary->stop_surveillance_policy = original_surveillance_policy;
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"),
              "native_participant_rejoin_stop_policy_mismatch");
    EXPECT_TRUE(rejected_sources.empty());
  }
  std::vector<RetainedStopSurveillanceSourceInput> root_swapped_sources;
  const auto root_swapped = rejoin_retained_native_runtime_participants(
      peer, {primary}, joined_resolver_factory, stable_stop_observer,
      root_swapped_sources, rejoin_gate, rejoin_fence_id, rejoin_scope);
  ASSERT_TRUE(root_swapped.ok()) << root_swapped.error().message;
  ASSERT_EQ(root_swapped_sources.size(), 2U);
  EXPECT_EQ(peer->participant_peers.size(), 1U);
  EXPECT_EQ(peer->participant_peers.front(), primary);
  EXPECT_EQ(peer->stop_surveillance_owner, stable_stop_observer);
  EXPECT_EQ(primary->stop_surveillance_owner, stable_stop_observer);
  EXPECT_EQ(primary->cell_runtime_root, peer.get());
  EXPECT_EQ(peer->cell_runtime_root, peer.get());
  EXPECT_EQ(peer->cell_addressed_kernel, primary->addressed_kernel.get());

  // Rejoin can succeed before the host's scoped Core participant install.
  // If that later step fails, the host retains the private prepared
  // composition while admission stays closed. Model that ownership boundary:
  // the candidate root must keep both the old Kernel owner and its exact Stop
  // observer alive, even after the caller drops its predecessor references.
  std::weak_ptr<RetainedNativeRuntime> failed_candidate_lifetime = peer;
  std::weak_ptr<RetainedNativeRuntime> failed_predecessor_lifetime = primary;
  auto failed_prepared_composition = peer;
  peer.reset();
  primary.reset();
  stable_stop_observer.reset();
  ASSERT_FALSE(failed_candidate_lifetime.expired());
  ASSERT_FALSE(failed_predecessor_lifetime.expired());
  ASSERT_FALSE(stable_observer_lifetime.expired());
  ASSERT_EQ(failed_prepared_composition->participant_peers.size(), 1U);
  auto *const failed_predecessor =
      failed_prepared_composition->participant_peers.front().get();
  ASSERT_NE(failed_predecessor, nullptr);
  EXPECT_EQ(failed_predecessor->cell_runtime_root,
            failed_prepared_composition.get());
  ASSERT_NE(failed_prepared_composition->cell_addressed_kernel, nullptr);
  EXPECT_EQ(failed_prepared_composition->cell_addressed_kernel,
            failed_predecessor->addressed_kernel.get());
  EXPECT_EQ(failed_prepared_composition->stop_surveillance_owner,
            failed_predecessor->stop_surveillance_owner);

  // The failed candidate is still available for diagnosis/authorized
  // restoration. Reacquire the exact retained owners before modeling the next
  // successful Apply that removes one cell peer.
  peer = failed_prepared_composition;
  primary = peer->participant_peers.front();
  stable_stop_observer = peer->stop_surveillance_owner;
  failed_prepared_composition.reset();

  // A later Apply can remove the former root entirely. Rejoin accepts zero
  // peers and returns a one-component Stop table, but must leave the stable
  // observer on its predecessor table until the host's Kernel commit hook.
  std::vector<RetainedStopSurveillanceSourceInput> single_component_sources;
  const auto single_component = rejoin_retained_native_runtime_participants(
      peer, {}, joined_resolver_factory, stable_stop_observer,
      single_component_sources, rejoin_gate, rejoin_fence_id, rejoin_scope);
  ASSERT_TRUE(single_component.ok()) << single_component.error().message;
  ASSERT_EQ(single_component_sources.size(), 1U);
  EXPECT_EQ(single_component_sources.front().binding.value(),
            peer->motion_binding.value());
  EXPECT_TRUE(peer->participant_peers.empty());
  EXPECT_EQ(peer->stop_surveillance_owner, stable_stop_observer);
  peer->addressed_kernel = std::move(primary->addressed_kernel);
  peer->operation_journal = std::move(primary->operation_journal);
  primary->stop_surveillance_owner.reset();
  primary->cell_addressed_kernel = nullptr;
  primary->cell_runtime_root = nullptr;
  EXPECT_FALSE(stable_observer_lifetime.expired());

  std::weak_ptr<RetainedNativeRuntime> primary_runtime_lifetime = primary;
  std::weak_ptr<RetainedNativeRuntime> peer_runtime_lifetime = peer;
  primary_subscription.reset();
  peer_subscription.reset();
  retained_primary_engine.reset();
  retained_peer_engine.reset();
  ASSERT_EQ(primary.use_count(), 1U);
  ASSERT_FALSE(peer_runtime_lifetime.expired());
  ASSERT_FALSE(primary_subscription_lifetime.expired());
  ASSERT_FALSE(peer_subscription_lifetime.expired());
  ASSERT_FALSE(primary_engine_lifetime.expired());
  ASSERT_FALSE(peer_engine_lifetime.expired());
  stable_stop_observer.reset();

  peer.reset();

  EXPECT_TRUE(peer_runtime_lifetime.expired());
  EXPECT_TRUE(stable_observer_lifetime.expired());
  EXPECT_FALSE(primary_runtime_lifetime.expired());
  primary.reset();

  EXPECT_TRUE(primary_runtime_lifetime.expired());
  EXPECT_TRUE(primary_subscription_lifetime.expired());
  EXPECT_TRUE(peer_subscription_lifetime.expired());
  EXPECT_TRUE(primary_engine_lifetime.expired());
  EXPECT_TRUE(peer_engine_lifetime.expired());
  const auto retired = subscription_ledger->Snapshot();
  EXPECT_EQ(retired.active_subscriptions, 0U);
  EXPECT_EQ(retired.subscriptions, 2U);
  EXPECT_EQ(retired.unsubscriptions, 2U);
  EXPECT_EQ(retired.observer_destructions, 2U);
  EXPECT_EQ(retired.destructions_with_active_subscription, 0U);
  EXPECT_EQ(retired.peer_back_pointer_observations, 1U);
  EXPECT_EQ(retired.uncleared_peer_back_pointers, 0U);
}

TEST(RetainedProductCrossConsumer,
     DurableLite6ModelPreservesAuthoritativeCollisionDecisions) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  const deployment::RetainedPlanningModelQuery planning_query{
      materialized.bundle->retained_bundle_id(),
      products.planning_collision_model.product_id};
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const deployment::RetainedModelMapQuery map_query{
      materialized.bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id};
  const auto retained = load_lite6_planning_model(package_root, planning_query);
  const auto model_map = load_lite6_model_map(package_root, map_query);
  model_map.require_same_bundle_as(retained);

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto *arm_binding =
      model_map.find_binding(component_id, command_target_id);
  const auto *arm_target =
      model_map.find_target(component_id, command_target_id);
  ASSERT_NE(arm_binding, nullptr);
  ASSERT_NE(arm_target, nullptr);
  const auto arm_group = std::find_if(
      arm_target->model_groups.begin(), arm_target->model_groups.end(),
      [](const deployment::CompositionTargetGroupResolution &group) {
        return group.model_group_role_id.value() == "planning_group";
      });
  ASSERT_NE(arm_group, arm_target->model_groups.end());
  std::vector<std::string> arm_joint_names;
  for (const auto &resource : arm_binding->resources) {
    ASSERT_EQ(resource.coordinates.size(), 1U);
    EXPECT_FALSE(resource.coordinates.front().derived_mimic);
    arm_joint_names.push_back(resource.coordinates.front().compiled_joint_name);
  }
  ASSERT_EQ(arm_joint_names.size(), 6U);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  const auto planning_scene_owner = make_roboplan_scene(RoboPlanSceneInput{
      "retained_lite6_cross_consumer",
      std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(),
      retained_joint_limit_rows(retained)});
  auto& planning_scene = *planning_scene_owner;
  auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_cross_consumer", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(), arm_group->compiled_group_name);
  ManyForgeCollisionEngine collision_engine(
      shared_collision_model, ManyForgeCollisionEngine::make_shared_scene_cache(
                                  shared_collision_model));
  const auto collision_projector =
      RetainedCollisionWorldProjector::create_verified(
          shared_collision_model, *materialized.bundle, model_map, retained);

  auto overlapping_role_map = collision_projector.model_map();
  ASSERT_FALSE(overlapping_role_map.component_bodies.empty());
  const auto existing_body = overlapping_role_map.component_bodies.front();
  auto alias_body = existing_body;
  alias_body.collision_body_role_id =
      core::CollisionBodyRoleId("zz_overlapping_collision_alias");
  overlapping_role_map.component_bodies.push_back(alias_body);
  const auto aliased_projector = RetainedCollisionWorldProjector::create(
      shared_collision_model, std::move(overlapping_role_map));
  const auto semantic_alias = core::make_component_collision_body_id(
      alias_body.controllable_component_id, alias_body.model_instance_id,
      alias_body.collision_body_role_id, alias_body.collision_index);
  const auto physical_body = core::make_component_collision_body_id(
      existing_body.controllable_component_id, existing_body.model_instance_id,
      existing_body.collision_body_role_id, existing_body.collision_index);
  ASSERT_EQ(aliased_projector.component_projection().component_bodies.size(),
            collision_projector.component_projection().component_bodies.size());
  ASSERT_EQ(aliased_projector.component_body_aliases().count(semantic_alias),
            1U);
  EXPECT_EQ(aliased_projector.component_body_aliases().at(semantic_alias),
            physical_body);

  const core::SceneResourceId scene_resource_id("retained-model-obstacle");
  const std::string scene_body_id = "scene/retained-model-obstacle/shape/0";
  std::set<core::CollisionBodyId> component_body_ids;
  std::set<std::string> projected_collision_frames;
  std::size_t projected_geometry_count = 0U;
  for (const auto &body :
       collision_projector.component_projection().component_bodies) {
    ASSERT_EQ(body.owner_kind, core::CollisionOwnerKind::kComponentModelBody);
    ASSERT_FALSE(body.geometries.empty());
    projected_geometry_count += body.geometries.size();
    ASSERT_TRUE(component_body_ids.insert(body.body_id).second);
    ASSERT_TRUE(projected_collision_frames.insert(body.frame_id).second);
  }

  std::set<std::string> retained_collision_frames;
  for (const auto &geometry :
       planning_scene.getCollisionModel().geometryObjects) {
    ASSERT_LT(geometry.parentFrame, planning_scene.getModel().frames.size());
    retained_collision_frames.insert(
        planning_scene.getModel().frames[geometry.parentFrame].name);
  }
  EXPECT_EQ(projected_collision_frames, retained_collision_frames);
  EXPECT_EQ(projected_geometry_count,
            planning_scene.getCollisionModel().geometryObjects.size());

  const auto make_snapshot = [&](const std::uint64_t revision_id,
                                 const double fixture_x_m,
                                 const double fixture_extent_m) {
    const auto logical_scene = make_collision_scene_snapshot(
        materialized.bundle->value().cell_graph.cell_id, revision_id + 1U,
        scene_resource_id, fixture_x_m, fixture_extent_m);
    auto built = collision_projector.project(logical_scene, revision_id, 0,
                                             "retained-lite6-cross-consumer");
    if (!built.ok()) {
      throw std::logic_error(
          "cannot construct retained-model parity snapshot: " +
          built.error().message);
    }
    return built.value();
  };

  const Eigen::Vector4d color(0.2, 0.4, 0.8, 1.0);
  const std::string fixture_name = "retained_model_world_fixture";
  const auto add_world_fixture = [&](const double x_m, const double extent_m) {
    Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
    pose(0, 3) = x_m;
    const auto added = planning_scene.addBoxGeometry(
        fixture_name, "universe", roboplan::Box(extent_m, extent_m, extent_m),
        pose, color);
    ASSERT_TRUE(added.has_value()) << added.error();
  };
  add_world_fixture(10.0, 0.2);
  auto snapshot = make_snapshot(1U, 10.0, 0.2);

  const auto compare_configuration = [&](std::string query_id,
                                         const std::vector<std::string>& names,
                                         const std::vector<double>& values,
                                         const bool expected_collision,
                                         const bool expected_scene_contact = true) {
    ASSERT_EQ(names.size(), values.size());
    Eigen::VectorXd planning_positions =
        Eigen::VectorXd::Zero(planning_scene.getModel().nq);
    for (std::size_t index = 0U; index < names.size(); ++index) {
      const auto joint_id = planning_scene.getModel().getJointId(names[index]);
      ASSERT_LT(joint_id, planning_scene.getModel().idx_qs.size());
      planning_positions[planning_scene.getModel().idx_qs[joint_id]] =
          values[index];
    }
    const bool planning_collision =
        planning_scene.hasCollisions(planning_positions);

    core::CollisionQuery query;
    query.query_id = std::move(query_id);
    query.collision_world_revision = snapshot.revision;
    query.robot_state.joint_names = names;
    query.robot_state.positions_rad = values;
    const auto result =
        collision_engine.check(snapshot, query, core::CollisionPolicySnapshot{},
                               core::CollisionCancellationToken{});
    std::string diagnostic_text;
    for (const auto &[key, value] : result.raw_diagnostics) {
      diagnostic_text += key + "=" + value + ";";
    }
    EXPECT_EQ(result.engine_status, core::CollisionEngineStatus::kOk)
        << diagnostic_text;
    EXPECT_EQ(result.in_collision, planning_collision);
    EXPECT_EQ(result.decision, planning_collision
                                   ? core::CollisionDecisionResult::kFail
                                   : core::CollisionDecisionResult::kPass);
    EXPECT_EQ(planning_collision, expected_collision);
    if (expected_collision) {
      ASSERT_FALSE(result.normalized_pairs.empty());
      bool saw_scene_contact = false;
      for (const auto &pair : result.normalized_pairs) {
        const bool body_a_known = pair.body_a == scene_body_id ||
                                  component_body_ids.count(pair.body_a) == 1U;
        const bool body_b_known = pair.body_b == scene_body_id ||
                                  component_body_ids.count(pair.body_b) == 1U;
        EXPECT_TRUE(body_a_known);
        EXPECT_TRUE(body_b_known);
        saw_scene_contact = saw_scene_contact || pair.body_a == scene_body_id ||
                            pair.body_b == scene_body_id;
      }
      EXPECT_EQ(saw_scene_contact, expected_scene_contact);
    } else {
      EXPECT_TRUE(result.normalized_pairs.empty());
    }
  };

  const std::vector<double> home(arm_joint_names.size(), 0.0);
  const std::vector<double> historical_rest{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  compare_configuration("lite6-home", arm_joint_names, home, false);
  compare_configuration("lite6-historical-rest", arm_joint_names,
                        historical_rest, false);

  // With the sole scene fixture ten metres away, this must be a real
  // SRDF-enabled component/component collision, not robot/world coverage.
  const std::vector<double> self_collision{0.0, -2.5, 0.0, 0.0, 2.0, 0.0};
  compare_configuration("lite6-true-self-collision", arm_joint_names, self_collision, true, false);
  ASSERT_EQ(collision_engine.prepare_runtime_query(arm_joint_names),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  ASSERT_EQ(collision_engine.prepare_runtime_snapshot(snapshot),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  const auto runtime_self_collision = collision_engine.check_runtime(
      snapshot, {self_collision, 0, 50'000'000, core::CollisionDistanceRequestMode::kNone, {}});
  ASSERT_EQ(runtime_self_collision.engine_status, core::CollisionEngineStatus::kOk);
  EXPECT_EQ(runtime_self_collision.status, core::RuntimeCollisionQueryStatus::kCollision);
  ASSERT_FALSE(runtime_self_collision.collision_pairs.empty());
  for (const auto& observation : runtime_self_collision.collision_pairs) {
    ASSERT_NE(observation.pair, nullptr);
    EXPECT_EQ(component_body_ids.count(observation.pair->body_a), 1U);
    EXPECT_EQ(component_body_ids.count(observation.pair->body_b), 1U);
  }

  const std::string gripper_master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                      "right_finger_joint");
  compare_configuration("lite6-open-gripper", {gripper_master}, {0.004}, false);

  const auto removed = planning_scene.removeGeometry(fixture_name);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  add_world_fixture(0.0, 4.0);
  snapshot = make_snapshot(2U, 0.0, 4.0);
  compare_configuration("lite6-positive-collision", arm_joint_names, home,
                        true);

  // A gripper-only query must keep the cross-branch mimic finger while
  // excluding pairs between bodies that cannot move in this subgroup.
  // The arm-group engine above would mask a missing mimic dependency because
  // both finger branches also inherit the moving arm's ancestry.
  expect_exact_retained_and_roboplan_mimic_semantics(model_map, planning_scene);
  const std::string gripper_follower =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint, "left_finger_joint");
  const std::string gripper_group =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kSrdfGroup, "lite_gripper");
  const std::set<std::string> finger_frames{
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kLink,
                      "lite_gripper_left_finger"),
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kLink,
                      "lite_gripper_right_finger"),
  };
  using NamedCollisionPair = std::pair<std::string, std::string>;
  std::set<NamedCollisionPair> expected_finger_world_pairs;
  for (const auto& body : collision_projector.component_projection().component_bodies) {
    if (finger_frames.count(body.frame_id) == 1U) {
      expected_finger_world_pairs.insert(
          {std::min(body.body_id, scene_body_id), std::max(body.body_id, scene_body_id)});
    }
  }
  ASSERT_EQ(expected_finger_world_pairs.size(), 2U);
  std::size_t expected_gripper_pair_count = 0;
  const auto& reference_geometry = planning_scene.getCollisionModel();
  for (const auto& pair : reference_geometry.collisionPairs) {
    const auto& first = reference_geometry.geometryObjects[pair.first];
    const auto& second = reference_geometry.geometryObjects[pair.second];
    const auto& frames = planning_scene.getModel().frames;
    ASSERT_LT(first.parentFrame, frames.size());
    ASSERT_LT(second.parentFrame, frames.size());
    if (finger_frames.count(frames[first.parentFrame].name) == 1U ||
        finger_frames.count(frames[second.parentFrame].name) == 1U) {
      ++expected_gripper_pair_count;
    }
  }
  ASSERT_GT(expected_gripper_pair_count, 0U);
  ASSERT_LT(expected_gripper_pair_count, reference_geometry.collisionPairs.size());
  auto gripper_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_gripper_cross_consumer", stage.loader_urdf_path(), stage.srdf_path(),
      stage.package_paths(), gripper_group);
  ManyForgeCollisionEngine gripper_collision_engine(
      gripper_collision_model,
      ManyForgeCollisionEngine::make_shared_scene_cache(gripper_collision_model));
  const std::vector<std::string> gripper_joint_names{gripper_master};
  ASSERT_EQ(gripper_collision_engine.prepare_runtime_query(gripper_joint_names),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  ASSERT_EQ(gripper_collision_engine.prepare_runtime_snapshot(snapshot),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  for (const double opening : {0.0, 0.004}) {
    SCOPED_TRACE("gripper opening " + std::to_string(opening));
    auto positions = Eigen::VectorXd::Zero(planning_scene.getModel().nq).eval();
    const auto master_id = planning_scene.getModel().getJointId(gripper_master);
    positions[planning_scene.getModel().idx_qs[master_id]] = opening;
    planning_scene.setJointPositions(positions);
    EXPECT_NEAR(mimic_expanded_position(planning_scene, gripper_master), opening, 1e-12);
    EXPECT_NEAR(mimic_expanded_position(planning_scene, gripper_follower), opening, 1e-12);
    core::CollisionQuery query;
    query.query_id = "lite6-gripper-subgroup";
    query.collision_world_revision = snapshot.revision;
    query.robot_state.joint_names = gripper_joint_names;
    query.robot_state.positions_rad = {opening};
    const auto ordinary = gripper_collision_engine.check(
        snapshot, query, core::CollisionPolicySnapshot{}, core::CollisionCancellationToken{});
    ASSERT_EQ(ordinary.engine_status, core::CollisionEngineStatus::kOk);
    EXPECT_TRUE(ordinary.in_collision);
    std::set<NamedCollisionPair> ordinary_pairs;
    for (const auto& pair : ordinary.normalized_pairs) {
      ordinary_pairs.emplace(pair.body_a, pair.body_b);
    }
    EXPECT_EQ(ordinary_pairs, expected_finger_world_pairs);
    const auto runtime =
        gripper_collision_engine.check_runtime(snapshot, {query.robot_state.positions_rad,
                                                          0,
                                                          50'000'000,
                                                          core::CollisionDistanceRequestMode::kNone,
                                                          {}});
    ASSERT_EQ(runtime.engine_status, core::CollisionEngineStatus::kOk);
    EXPECT_EQ(runtime.status, core::RuntimeCollisionQueryStatus::kCollision);
    std::set<NamedCollisionPair> runtime_pairs;
    for (const auto& observation : runtime.collision_pairs) {
      ASSERT_NE(observation.pair, nullptr);
      runtime_pairs.emplace(observation.pair->body_a, observation.pair->body_b);
    }
    EXPECT_EQ(runtime_pairs, expected_finger_world_pairs);
    EXPECT_EQ(gripper_collision_engine.internal_last_query_mechanism_counters()
                  .active_collision_pair_count,
              expected_gripper_pair_count);
  }

  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto *gripper_target =
      model_map.find_target(component_id, gripper_target_id);
  ASSERT_NE(gripper_target, nullptr);
  const auto carrier_frame = std::find_if(
      gripper_target->frames.begin(), gripper_target->frames.end(),
      [](const deployment::CompositionTargetFrameResolution &frame) {
        return frame.frame_role_id.value() == "tcp";
      });
  ASSERT_NE(carrier_frame, gripper_target->frames.end());
  const auto *carrier = std::get_if<deployment::CompositionComponentModelFrame>(
      &carrier_frame->frame);
  ASSERT_NE(carrier, nullptr);

  const auto collision_target = std::find_if(
      collision_projector.model_map().targets.begin(),
      collision_projector.model_map().targets.end(),
      [&](const RetainedCollisionModelMapInput::Target &candidate) {
        return candidate.controllable_component_id == component_id &&
               candidate.command_target_id == gripper_target_id;
      });
  ASSERT_NE(collision_target, collision_projector.model_map().targets.end());
  ASSERT_FALSE(collision_target->bodies.empty());
  auto exact_carrier_candidates = collision_target->bodies;
  auto exact_carrier_body = collision_target->bodies.front();
  exact_carrier_body.collision_body_role_id =
      core::CollisionBodyRoleId("exact_carrier_collision");
  exact_carrier_body.collision_index = 0U;
  exact_carrier_body.compiled_frame_name = carrier->compiled_frame_name;
  exact_carrier_candidates.push_back(exact_carrier_body);
  auto exact_carrier_alias = exact_carrier_body;
  exact_carrier_alias.collision_body_role_id =
      core::CollisionBodyRoleId("zz_exact_carrier_alias");
  exact_carrier_candidates.push_back(exact_carrier_alias);
  const auto exact_carrier_parent =
      ManyForgeCollisionEngine::resolve_retained_carried_object_parent(
          shared_collision_model, carrier->compiled_frame_name,
          exact_carrier_candidates);
  ASSERT_TRUE(exact_carrier_parent.ok())
      << exact_carrier_parent.error().message;
  EXPECT_EQ(exact_carrier_parent.value().compiled_parent_frame_name,
            carrier->compiled_frame_name);
  EXPECT_EQ(exact_carrier_parent.value().parent_body_id,
            core::make_component_collision_body_id(
                exact_carrier_body.controllable_component_id,
                exact_carrier_body.model_instance_id,
                exact_carrier_body.collision_body_role_id,
                exact_carrier_body.collision_index));

  core::LogicalSceneSnapshotDTO carried_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 4U,
      core::SceneResourceId("carried-part"), 0.0, 0.02);
  carried_scene.objects.front().support_contacts = {
      core::SupportContactDTO{.support_object_id = "support-table"},
  };
  carried_scene.objects.push_back(core::SceneObjectSnapshotDTO{
      core::SceneResourceId("support-table"),
      {core::BoxShapeDTO{{0.5, 0.5, 0.05}}},
      core::CellFixedSceneObjectPlacementDTO{
          materialized.bundle->value().cell_graph.cell_id,
          {{10.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
      },
      std::nullopt,
      {},
  });
  carried_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          carried_scene);
  auto detached_policy_result =
      collision_projector.project_effective_policy(carried_scene, 2U);
  ASSERT_TRUE(detached_policy_result.ok())
      << detached_policy_result.error().message;
  EXPECT_TRUE(detached_policy_result.value().support_contacts.empty());
  EXPECT_NE(
      std::find(
          detached_policy_result.value().allowed_static_static_pairs.begin(),
          detached_policy_result.value().allowed_static_static_pairs.end(),
          (core::CollisionPairId{"scene/carried-part/shape/0",
                                 "scene/support-table/shape/0"})),
      detached_policy_result.value().allowed_static_static_pairs.end());
  EXPECT_TRUE(std::any_of(
      detached_policy_result.value().allowed_static_static_pairs.begin(),
      detached_policy_result.value().allowed_static_static_pairs.end(),
      [&](const auto &pair) {
        const bool names_fixed_scene =
            pair.body_a == "scene/carried-part/shape/0" ||
            pair.body_b == "scene/carried-part/shape/0";
        const auto &other = pair.body_a == "scene/carried-part/shape/0"
                                ? pair.body_b
                                : pair.body_a;
        return names_fixed_scene && component_body_ids.count(other) == 1U;
      }));
  core::CarriedObjectAttachmentDTO attachment{
      core::CarriedObjectAttachmentId("carried-part-attachment"),
      core::CarriedObjectAttachmentRevision(std::string(kZeroDigest)),
      core::SceneResourceId("carried-part"),
      component_id,
      gripper_target_id,
      core::ComponentSceneFrameRefDTO{
          component_id,
          carrier->model_instance_id,
          core::FrameRoleId("tcp"),
      },
      {{0.0, 0.0, 0.04}, {0.0, 0.0, 0.0, 1.0}},
      {core::CollisionBodyRoleId("gripper_collision")},
  };
  attachment.carried_object_attachment_revision =
      core::logical_scene_identity::ComputeCarriedObjectAttachmentRevision(
          attachment);
  carried_scene.objects.front().placement =
      core::CarriedSceneObjectPlacementDTO{
          attachment.carried_object_attachment_id,
          attachment.carried_object_attachment_revision,
      };
  carried_scene.carried_object_attachments = {attachment};
  carried_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          carried_scene);
  ASSERT_TRUE(carried_scene.locally_valid());

  auto carried_world_result = collision_projector.project(
      carried_scene, 3U, 0, "retained-lite6-cross-consumer");
  ASSERT_TRUE(carried_world_result.ok())
      << carried_world_result.error().message;
  const auto &carried_world = carried_world_result.value();
  const auto carried_body =
      std::find_if(carried_world.bodies.begin(), carried_world.bodies.end(),
                   [](const core::CollisionWorldBody &body) {
                     return body.body_id == "scene/carried-part/shape/0";
                   });
  ASSERT_NE(carried_body, carried_world.bodies.end());
  ASSERT_TRUE(carried_body->parent_body_id.has_value());
  EXPECT_EQ(component_body_ids.count(*carried_body->parent_body_id), 1U);
  EXPECT_NE(carried_body->frame_id,
            collision_projector.model_map().compiled_cell_frame_name);

  // A `continuous` pose source keeps provider evidence on a carried object:
  // it projects exactly like the carried object, never as a fresh obstacle.
  core::LogicalSceneSnapshotDTO observed_carried_scene = carried_scene;
  observed_carried_scene.objects.front().provider_evidence =
      core::SceneObjectProviderEvidenceDTO{
          core::ProviderInstanceId("isaac.pose.observer"),
          core::ProviderManifestId("manyforge.provider.logical_scene"),
          core::ProviderManifestRevision("sha256:" + std::string(64U, 'b')),
          "sha256:" + std::string(64U, 'a'),
          7U,
          1'000,
          std::nullopt,
          10'000'000'000};
  observed_carried_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          observed_carried_scene);
  auto observed_carried_world = collision_projector.project(
      observed_carried_scene, 3U, 0, "retained-lite6-cross-consumer");
  ASSERT_TRUE(observed_carried_world.ok())
      << observed_carried_world.error().message;
  const auto observed_body = std::find_if(
      observed_carried_world.value().bodies.begin(),
      observed_carried_world.value().bodies.end(),
      [](const core::CollisionWorldBody &body) {
        return body.body_id == "scene/carried-part/shape/0";
      });
  ASSERT_NE(observed_body, observed_carried_world.value().bodies.end());
  EXPECT_EQ(observed_body->parent_body_id, carried_body->parent_body_id);
  EXPECT_EQ(observed_body->pose, carried_body->pose);
  EXPECT_FALSE(observed_body->source_freshness_budget_ns.has_value());
  const auto observed_live = collision_projector.project_live_objects(
      observed_carried_scene, observed_carried_world.value());
  ASSERT_EQ(observed_live.size(), carried_scene.objects.size());
  EXPECT_FALSE(observed_live.front().source_freshness_budget_ns.has_value());
  EXPECT_FALSE(observed_live.front().source_timestamp_ns.has_value());

  auto carried_policy_result =
      collision_projector.project_effective_policy(carried_scene, 2U);
  ASSERT_TRUE(carried_policy_result.ok())
      << carried_policy_result.error().message;
  const auto &carried_policy = carried_policy_result.value();
  ASSERT_EQ(
      carried_policy.allowed_touch_link_pairs.size(),
      gripper_target->collision_bodies.front().compiled_link_names.size());
  for (const auto &pair : carried_policy.allowed_touch_link_pairs) {
    EXPECT_TRUE(pair.body_a == carried_body->body_id ||
                pair.body_b == carried_body->body_id);
  }
  ASSERT_EQ(carried_policy.support_contacts.size(), 1U);
  EXPECT_EQ(carried_policy.support_contacts.front().pair,
            (core::CollisionPairId{"scene/carried-part/shape/0",
                                   "scene/support-table/shape/0"}));

  core::CollisionQuery carried_query;
  carried_query.query_id = "lite6-carried-object-ingestion";
  carried_query.collision_world_revision = carried_world.revision;
  carried_query.robot_state.joint_names = arm_joint_names;
  carried_query.robot_state.positions_rad = home;
  const auto carried_result =
      collision_engine.check(carried_world, carried_query, carried_policy,
                             core::CollisionCancellationToken{});
  EXPECT_EQ(carried_result.engine_status, core::CollisionEngineStatus::kOk);

  core::LogicalSceneSnapshotDTO no_touch_scene = carried_scene;
  no_touch_scene.carried_object_attachments.front().touch_body_roles.clear();
  no_touch_scene.carried_object_attachments.front()
      .carried_object_attachment_revision =
      core::logical_scene_identity::ComputeCarriedObjectAttachmentRevision(
          no_touch_scene.carried_object_attachments.front());
  auto &no_touch_placement = std::get<core::CarriedSceneObjectPlacementDTO>(
      no_touch_scene.objects.front().placement);
  no_touch_placement.carried_object_attachment_revision =
      no_touch_scene.carried_object_attachments.front()
          .carried_object_attachment_revision;
  no_touch_scene.revision.revision_id = 5U;
  no_touch_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          no_touch_scene);
  auto no_touch_world = collision_projector.project(
      no_touch_scene, 4U, 0, "retained-lite6-cross-consumer");
  ASSERT_TRUE(no_touch_world.ok()) << no_touch_world.error().message;
  EXPECT_NE(no_touch_world.value().revision.content_hash,
            carried_world.revision.content_hash);

  const auto initial_publish_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 2U,
      core::SceneResourceId("carried-part"), 0.0, 0.02);
  std::int64_t publisher_lease_clock = 6'000'000'010;
  auto publisher_result = RetainedCollisionWorldPublisher::create(
      collision_projector, "retained-lite6-publisher", initial_publish_scene,
      RetainedCollisionWorldPublicationContext{
          core::CollisionWorldMutationSource::kDeploymentApply,
          {},
          10,
          {},
          "retained_product_consumer",
          std::nullopt,
      },
      [&publisher_lease_clock] { return publisher_lease_clock++; });
  ASSERT_TRUE(publisher_result.ok()) << publisher_result.error().message;
  const auto publisher = publisher_result.value();
  const auto initial_world = publisher->snapshot();
  ASSERT_NE(initial_world, nullptr);
  EXPECT_EQ(initial_world, publisher->snapshot());
  EXPECT_EQ(initial_world->revision.revision_id, 1U);
  EXPECT_FALSE(initial_world->bodies.back().parent_body_id.has_value());
  const auto initial_inventory =
      publisher->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(initial_inventory.has_value());
  ASSERT_TRUE(initial_inventory->projection_binding.valid());
  EXPECT_EQ(initial_inventory->projection_binding.projection_revision
                .retained_bundle_id,
            materialized.bundle->retained_bundle_id());
  EXPECT_EQ(
      initial_inventory->projection_binding.projection_revision.graph_revision,
      materialized.bundle->value().cell_graph.graph_revision);
  EXPECT_EQ(initial_inventory->bundle.world, initial_world);

  const core::ProjectionLeaseId operation_lease("operation.native.lease.1");
  const auto retained_inventory =
      publisher->retain_scene_snapshot_lease_inventory(operation_lease);
  ASSERT_TRUE(retained_inventory.has_value());
  EXPECT_EQ(retained_inventory->projection_binding.projection_lease_id,
            operation_lease);
  EXPECT_EQ(retained_inventory->captured_at_ns, 6'000'000'010);
  EXPECT_EQ(retained_inventory->bundle.world, initial_world);
  auto retention_stats = publisher->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(retention_stats.retained_count, 1U);
  ASSERT_EQ(retention_stats.retained_ids_oldest_first.size(), 1U);
  EXPECT_EQ(retention_stats.retained_ids_oldest_first.front(), operation_lease);

  auto attached_publication = publisher->publish(
      carried_scene,
      RetainedCollisionWorldPublicationContext{
          core::CollisionWorldMutationSource::kCarriedObjectAttach,
          {core::CollisionBodyId("scene/carried-part/shape/0")},
          20,
          {},
          "scene_attach_carried",
          std::string("attach-correlation"),
      });
  ASSERT_TRUE(attached_publication.ok())
      << attached_publication.error().message;
  EXPECT_EQ(attached_publication.value().revision_id, 2U);
  EXPECT_EQ(initial_world->revision.revision_id, 1U);
  const auto advanced_inventory =
      publisher->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(advanced_inventory.has_value());
  EXPECT_NE(advanced_inventory->bundle.world, initial_world);
  EXPECT_EQ(advanced_inventory->bundle.world->revision.revision_id, 2U);
  const auto repeated_retention =
      publisher->retain_scene_snapshot_lease_inventory(operation_lease);
  ASSERT_TRUE(repeated_retention.has_value());
  EXPECT_EQ(repeated_retention->projection_binding,
            retained_inventory->projection_binding);
  EXPECT_EQ(repeated_retention->bundle.world, initial_world);
  const auto published_runtime =
      publisher->published_runtime_collision_query_bundle();
  ASSERT_NE(published_runtime, nullptr);
  ASSERT_NE(published_runtime->world, nullptr);
  ASSERT_NE(published_runtime->policy, nullptr);
  EXPECT_TRUE(published_runtime->lookup_order_valid);
  EXPECT_TRUE(published_runtime->distance_selection_order_valid);
  EXPECT_EQ(published_runtime->policy->allowed_touch_link_pairs,
            carried_policy.allowed_touch_link_pairs);

  const auto ledger = publisher->recent_ledger();
  ASSERT_EQ(ledger.size(), 2U);
  EXPECT_EQ(ledger[0].revision.revision_id, 2U);
  EXPECT_EQ(ledger[0].previous_revision_id, 1U);
  EXPECT_EQ(ledger[0].mutation_source,
            core::CollisionWorldMutationSource::kCarriedObjectAttach);
  EXPECT_EQ(ledger[0].owner_sequence, 2U);
  EXPECT_EQ(ledger[1].revision.revision_id, 1U);
  EXPECT_EQ(ledger[1].previous_revision_id, 0U);

  auto stale_publication = publisher->publish(
      carried_scene, RetainedCollisionWorldPublicationContext{
                         core::CollisionWorldMutationSource::kSceneEdit,
                         {},
                         30,
                         {},
                         "stale_scene_replay",
                         std::nullopt,
                     });
  EXPECT_FALSE(stale_publication.ok());
  EXPECT_EQ(publisher->published_collision_world_revision().revision_id, 2U);
  EXPECT_EQ(publisher->recent_ledger().size(), 2U);

  publisher->release_scene_snapshot_lease_inventory(operation_lease);
  publisher->release_scene_snapshot_lease_inventory(operation_lease);
  retention_stats = publisher->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(retention_stats.retained_count, 0U);
  EXPECT_TRUE(retention_stats.retained_ids_oldest_first.empty());
}

TEST(RetainedProductCrossConsumer,
     PlannerSceneProjectionCommitsLeasesAndRebuildsExactSnapshots) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.symbol_role_maps.front().product_id});
  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto *target = model_map.find_target(component_id, command_target_id);
  ASSERT_NE(target, nullptr);
  ASSERT_FALSE(target->model_groups.empty());
  auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_projection_state_machine", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(),
      target->model_groups.front().compiled_group_name);
  const auto collision_projector =
      RetainedCollisionWorldProjector::create_verified(
          shared_collision_model, *materialized.bundle, model_map, retained);

  constexpr std::int64_t kReceiverTimestampNs = 4'000'000'000;
  constexpr std::int64_t kRawSourceTimestampNs = 1'250'000'000;
  const auto initial_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 2U,
      core::SceneResourceId("first-object"), 10.0, 0.1,
      core::SceneObjectProviderEvidenceDTO{
          core::ProviderInstanceId("isaac.pose.observer"),
          core::ProviderManifestId("manyforge.provider.logical_scene"),
          core::ProviderManifestRevision("sha256:"
                                         "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                                         "bbbbbbbbbbbbbbbbbbbbbbbbbbb"),
          "sha256:"
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          7U, kReceiverTimestampNs, kRawSourceTimestampNs, 10'000'000'000});
  const core::ProjectionRevisionDTO initial_revision{
      materialized.bundle->retained_bundle_id(),
      materialized.bundle->value().cell_graph.graph_revision,
      initial_scene.revision};
  const core::ProjectionNativeRuntimeBindingDTO native_binding{
      core::CapabilityProviderBindingId("lite6_joint_planner_binding"),
      core::CapabilityProviderBindingRevision("planner-binding-revision"),
      core::RuntimeUnitId("native-execution-host"),
      core::RuntimeEpoch("native-execution-host-epoch-1")};
  std::int64_t clock_value = kReceiverTimestampNs + 10;
  bool fail_clock = false;
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      collision_projector, products.planning_collision_model, native_binding,
      initial_scene,
      RetainedPlannerSceneProjectionConfig{
          "retained-lite6-transactional-projection", 2U, 8U},
      [&clock_value, &fail_clock] {
        if (fail_clock) {
          throw std::runtime_error("injected projection clock failure");
        }
        return clock_value++;
      });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  auto projection = projection_result.value();
  ASSERT_TRUE(
      projection->ProjectionHealth().healthy_for_target(initial_revision));
  const auto publication_inventory =
      projection->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(publication_inventory.has_value());
  EXPECT_EQ(publication_inventory->captured_at_ns, kReceiverTimestampNs + 10);
  ASSERT_EQ(publication_inventory->bundle.body_freshness.size(), 1U);
  EXPECT_EQ(
      publication_inventory->bundle.body_freshness.front().source_timestamp_ns,
      kReceiverTimestampNs);
  ASSERT_EQ(publication_inventory->live_objects.size(), 1U);
  EXPECT_EQ(publication_inventory->live_objects.front().source_timestamp_ns,
            kReceiverTimestampNs);
  EXPECT_NE(publication_inventory->live_objects.front().source_timestamp_ns,
            kRawSourceTimestampNs);

  const core::ProjectionLeaseId predecessor_lease(
      "z.operation.predecessor.oldest");
  clock_value = 6'000'000'010;
  auto retained_predecessor =
      projection->RetainProjection(initial_revision, predecessor_lease);
  ASSERT_TRUE(retained_predecessor.ok())
      << retained_predecessor.error().message;
  EXPECT_EQ(retained_predecessor.value().captured_at_ns, 6'000'000'010);
  ASSERT_EQ(retained_predecessor.value().live_objects.size(),
            publication_inventory->live_objects.size());
  for (std::size_t index = 0;
       index < retained_predecessor.value().live_objects.size(); ++index) {
    EXPECT_EQ(
        retained_predecessor.value().live_objects[index].source_timestamp_ns,
        publication_inventory->live_objects[index].source_timestamp_ns);
  }
  const auto predecessor_world = retained_predecessor.value().bundle.world;
  ASSERT_NE(predecessor_world, nullptr);
  clock_value = 9'000'000'010;
  const auto replayed_predecessor =
      projection->RetainProjection(initial_revision, predecessor_lease);
  ASSERT_TRUE(replayed_predecessor.ok())
      << replayed_predecessor.error().message;
  EXPECT_EQ(replayed_predecessor.value().captured_at_ns, 6'000'000'010);
  EXPECT_EQ(replayed_predecessor.value().bundle.world, predecessor_world);
  const core::ProjectionLeaseId second_predecessor_lease(
      "a.operation.predecessor.newer");
  auto retained_second_predecessor =
      projection->RetainProjection(initial_revision, second_predecessor_lease);
  ASSERT_TRUE(retained_second_predecessor.ok())
      << retained_second_predecessor.error().message;
  EXPECT_EQ(retained_second_predecessor.value().captured_at_ns, 9'000'000'010);
  fail_clock = true;
  const auto failed_clock_lease = projection->RetainProjection(
      initial_revision, core::ProjectionLeaseId("clock-failure-lease"));
  fail_clock = false;
  ASSERT_FALSE(failed_clock_lease.ok());
  EXPECT_EQ(failed_clock_lease.error().details.at("reason"),
            "native_projection_lease_clock_failed");
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats()
                .retained_ids_oldest_first,
            (std::vector<core::ProjectionLeaseId>{predecessor_lease,
                                                  second_predecessor_lease}));

  core::SceneObjectSnapshotDTO second_object{
      core::SceneResourceId("second-object"),
      {core::SphereShapeDTO{0.05}},
      core::CellFixedSceneObjectPlacementDTO{
          initial_scene.cell_id, {{9.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}},
      std::nullopt,
      {}};
  auto candidate_scene = initial_scene;
  candidate_scene.transaction_sequence = 2U;
  candidate_scene.topology_epoch = 2U;
  candidate_scene.objects.push_back(second_object);
  candidate_scene.revision.revision_id = 3U;
  candidate_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          candidate_scene);
  const core::ProjectionRevisionDTO candidate_revision{
      initial_revision.retained_bundle_id, initial_revision.graph_revision,
      candidate_scene.revision};
  const core::SceneTransactionDTO transaction{
      core::RequestId("projection-add-second-object"),
      {},
      2U,
      initial_revision,
      candidate_revision,
      core::ContentDigest(std::string(kZeroDigest)),
      core::ResolvedUpsertSceneObjectDTO{second_object},
      core::ContentDigest(std::string(kZeroDigest))};

  const auto candidate_policy = projection->ProjectEffectiveCollisionPolicy(
      candidate_scene, candidate_scene.revision.revision_id);
  ASSERT_TRUE(candidate_policy.ok()) << candidate_policy.error().message;
  const core::SceneCandidatePreviewDTO candidate_preview{
      core::ContentDigest(std::string(kZeroDigest)),
      initial_revision,
      candidate_revision,
      std::make_shared<const core::LogicalSceneSnapshotDTO>(candidate_scene),
      core::ResolvedUpsertSceneObjectDTO{second_object},
      candidate_policy.value(),
      core::SceneUpdateStatusDTO::kGeometryChanged,
      core::ContentDigest(std::string(kZeroDigest))};
  ASSERT_TRUE(candidate_preview.valid());
  const auto private_candidate = projection->ProjectSceneCandidate(
      candidate_preview, retained_predecessor.value().projection_binding,
      core::ProjectionLeaseId("pick-candidate.attach"));
  ASSERT_TRUE(private_candidate.ok()) << private_candidate.error().message;
  EXPECT_EQ(private_candidate.value().projection_binding.projection_revision,
            candidate_revision);
  EXPECT_EQ(private_candidate.value().logical_scene,
            candidate_preview.logical_scene);
  ASSERT_NE(private_candidate.value().bundle.world, nullptr);
  EXPECT_NE(private_candidate.value().bundle.world, predecessor_world);
  EXPECT_GT(private_candidate.value().bundle.world->revision.revision_id,
            predecessor_world->revision.revision_id);
  EXPECT_EQ(private_candidate.value().bundle.policy, candidate_policy.value());
  EXPECT_EQ(private_candidate.value().live_objects.size(), 2U);
  EXPECT_EQ(*projection->logical_snapshot(), initial_scene);
  EXPECT_EQ(projection->snapshot(), predecessor_world);

  const auto repeated_private_candidate = projection->ProjectSceneCandidate(
      candidate_preview, retained_predecessor.value().projection_binding,
      core::ProjectionLeaseId("pick-candidate.attach.repeated"));
  ASSERT_TRUE(repeated_private_candidate.ok())
      << repeated_private_candidate.error().message;
  ASSERT_NE(repeated_private_candidate.value().bundle.world, nullptr);
  EXPECT_GT(
      repeated_private_candidate.value().bundle.world->revision.revision_id,
      private_candidate.value().bundle.world->revision.revision_id);
  EXPECT_EQ(
      repeated_private_candidate.value().bundle.world->revision.content_hash,
      private_candidate.value().bundle.world->revision.content_hash);

  const auto committed = projection->ApplyTransaction(transaction);
  ASSERT_TRUE(committed.presence_valid());
  EXPECT_EQ(committed.result, core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_TRUE(
      committed.resulting_health.healthy_for_target(candidate_revision));
  ASSERT_NE(projection->logical_snapshot(), nullptr);
  EXPECT_EQ(*projection->logical_snapshot(), candidate_scene);
  const auto after_live_update = projection->ProjectSceneCandidate(
      candidate_preview, retained_predecessor.value().projection_binding,
      core::ProjectionLeaseId("pick-candidate.after-live-update"));
  ASSERT_TRUE(after_live_update.ok()) << after_live_update.error().message;
  EXPECT_EQ(after_live_update.value().logical_scene, candidate_preview.logical_scene);
  const auto current_planning = projection->RetainCurrentProjection(
      core::ProjectionLeaseId("current-planning.after-live-update"));
  ASSERT_TRUE(current_planning.ok()) << current_planning.error().message;
  EXPECT_EQ(current_planning.value().projection_binding.projection_revision, candidate_revision);
  projection->ReleaseProjection(current_planning.value().projection_binding.projection_lease_id);
  const auto current_world = projection->snapshot();
  ASSERT_NE(current_world, nullptr);
  EXPECT_NE(current_world, predecessor_world);
  EXPECT_GT(
      current_world->revision.revision_id,
      repeated_private_candidate.value().bundle.world->revision.revision_id);
  EXPECT_EQ(retained_predecessor.value().bundle.world, predecessor_world);

  const auto replay = projection->ApplyTransaction(transaction);
  EXPECT_EQ(replay.transaction_sequence, committed.transaction_sequence);
  EXPECT_EQ(replay.acknowledged_projection_revision,
            committed.acknowledged_projection_revision);
  EXPECT_EQ(replay.native_projection_generation,
            committed.native_projection_generation);
  EXPECT_EQ(projection->snapshot(), current_world);

  const auto refused_healthy_rebuild =
      projection->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, initial_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              initial_scene)});
  EXPECT_EQ(refused_healthy_rebuild.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(
      projection->ProjectionHealth().healthy_for_target(candidate_revision));
  EXPECT_EQ(projection->snapshot(), current_world);

  auto conflicting = transaction;
  conflicting.request_id = core::RequestId("conflicting-reuse");
  const auto conflict = projection->ApplyTransaction(conflicting);
  EXPECT_EQ(conflict.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(conflict.rollback_proven);
  EXPECT_EQ(projection->snapshot(), current_world);

  auto policy_scene = candidate_scene;
  policy_scene.transaction_sequence = 3U;
  policy_scene.policy_epoch = 1U;
  policy_scene.configured_collision_policy.policy_id = "reduced-diagnostics";
  policy_scene.configured_collision_policy.require_full_diagnostics = false;
  ++policy_scene.configured_collision_policy.revision.revision_id;
  policy_scene.configured_collision_policy.revision.content_hash =
      core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
          policy_scene.configured_collision_policy)
          .value();
  policy_scene.revision.revision_id = 4U;
  policy_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          policy_scene);
  const core::ProjectionRevisionDTO policy_revision{
      candidate_revision.retained_bundle_id, candidate_revision.graph_revision,
      policy_scene.revision};
  auto mismatched_effective = policy_scene.configured_collision_policy;
  mismatched_effective.require_full_diagnostics = true;
  mismatched_effective.revision.content_hash =
      core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
          mismatched_effective)
          .value();
  const core::SceneTransactionDTO mismatched_policy_transaction{
      core::RequestId("projection-mismatched-effective-policy"),
      {},
      3U,
      candidate_revision,
      policy_revision,
      core::ContentDigest(std::string(kZeroDigest)),
      core::ResolvedCollisionPolicyDTO{policy_scene.configured_collision_policy,
                                       std::move(mismatched_effective)},
      std::nullopt};
  const auto rejected_policy =
      projection->ApplyTransaction(mismatched_policy_transaction);
  EXPECT_EQ(rejected_policy.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(rejected_policy.rollback_proven);
  EXPECT_EQ(projection->snapshot(), current_world);
  EXPECT_EQ(*projection->logical_snapshot(), candidate_scene);

  auto uncertain_scene = candidate_scene;
  auto third_object = second_object;
  third_object.scene_resource_id = core::SceneResourceId("third-object");
  uncertain_scene.objects.push_back(third_object);
  uncertain_scene.transaction_sequence = 3U;
  uncertain_scene.topology_epoch = 3U;
  uncertain_scene.revision.revision_id = 4U;
  uncertain_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          uncertain_scene);
  const core::ProjectionRevisionDTO uncertain_revision{
      candidate_revision.retained_bundle_id, candidate_revision.graph_revision,
      uncertain_scene.revision};
  const core::SceneTransactionDTO uncertain_transaction{
      core::RequestId("projection-injected-uncertain-effect"),
      {},
      3U,
      candidate_revision,
      uncertain_revision,
      core::ContentDigest(std::string(kZeroDigest)),
      core::ResolvedUpsertSceneObjectDTO{third_object},
      core::ContentDigest(std::string(kZeroDigest))};
  fail_clock = true;
  const auto uncertain = projection->ApplyTransaction(uncertain_transaction);
  fail_clock = false;
  EXPECT_EQ(uncertain.result,
            core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain);
  EXPECT_EQ(projection->ProjectionHealth().state,
            core::ProjectionHealthState::kDesynchronized);
  EXPECT_EQ(projection->snapshot(), nullptr);
  EXPECT_FALSE(projection->collision_policy_snapshot().has_value());
  EXPECT_FALSE(
      projection->current_scene_snapshot_lease_inventory().has_value());
  EXPECT_EQ(projection->published_runtime_collision_query_bundle(), nullptr);
  EXPECT_FALSE(
      projection->RetainProjection(initial_revision, predecessor_lease).ok());

  const auto recovered =
      projection->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  ASSERT_TRUE(recovered.presence_valid());
  EXPECT_TRUE(
      recovered.resulting_health.healthy_for_target(candidate_revision));
  std::optional<core::SceneTransactionDTO> oldest_retained_transaction;
  for (std::uint64_t sequence = 3U; sequence <= 5U; ++sequence) {
    const core::SceneTransactionDTO no_op{
        core::RequestId("projection-bounded-replay-" +
                        std::to_string(sequence)),
        {},
        sequence,
        candidate_revision,
        candidate_revision,
        core::ContentDigest(std::string(kZeroDigest)),
        core::ResolvedUpsertSceneObjectDTO{second_object},
        core::ContentDigest(std::string(kZeroDigest))};
    const auto result = projection->ApplyTransaction(no_op);
    ASSERT_TRUE(result.presence_valid());
    EXPECT_EQ(result.result, core::ProjectionCommitResultStateDTO::kCommitted);
    if (sequence == 3U) {
      oldest_retained_transaction = no_op;
      const auto replayed = projection->ApplyTransaction(no_op);
      EXPECT_EQ(replayed.transaction_sequence, sequence);
      EXPECT_EQ(replayed.result,
                core::ProjectionCommitResultStateDTO::kCommitted);
    }
  }
  ASSERT_TRUE(oldest_retained_transaction.has_value());
  const auto compacted_replay =
      projection->ApplyTransaction(*oldest_retained_transaction);
  EXPECT_EQ(compacted_replay.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  ASSERT_TRUE(compacted_replay.error.has_value());
  EXPECT_EQ(compacted_replay.error->details.at("reason"),
            "native_projection_resolved_delta_mismatch");
  ASSERT_NE(projection->logical_snapshot(), nullptr);
  EXPECT_EQ(projection->logical_snapshot()->transaction_sequence, 5U);
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats().retained_count,
            2U);
  const auto recovered_predecessor =
      projection->RetainProjection(initial_revision, predecessor_lease);
  ASSERT_TRUE(recovered_predecessor.ok())
      << recovered_predecessor.error().message;
  EXPECT_EQ(recovered_predecessor.value().bundle.world, predecessor_world);

  projection->ReleaseProjection(predecessor_lease);
  projection->ReleaseProjection(second_predecessor_lease);
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats().retained_count,
            0U);

  std::int64_t rebuild_clock = 100;
  auto uninitialized_result =
      RetainedPlannerSceneProjection::CreateUninitialized(
          collision_projector, products.planning_collision_model,
          native_binding,
          RetainedPlannerSceneProjectionConfig{
              "retained-lite6-rebuilt-projection", 8U, 8U},
          [&rebuild_clock] { return rebuild_clock++; });
  ASSERT_TRUE(uninitialized_result.ok())
      << uninitialized_result.error().message;
  auto uninitialized = uninitialized_result.value();
  EXPECT_EQ(uninitialized->ProjectionHealth().state,
            core::ProjectionHealthState::kUninitialized);
  EXPECT_EQ(uninitialized->snapshot(), nullptr);
  EXPECT_FALSE(uninitialized->collision_policy_snapshot().has_value());
  EXPECT_FALSE(
      uninitialized->current_scene_snapshot_lease_inventory().has_value());
  EXPECT_EQ(uninitialized->published_runtime_collision_query_bundle(), nullptr);
  EXPECT_FALSE(
      uninitialized
          ->RetainProjection(candidate_revision,
                             core::ProjectionLeaseId("uninitialized-lease"))
          .ok());
  const auto rebuilt =
      uninitialized->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  ASSERT_TRUE(rebuilt.presence_valid());
  EXPECT_FALSE(rebuilt.transaction_sequence.has_value());
  EXPECT_TRUE(rebuilt.resulting_health.healthy_for_target(candidate_revision));
  ASSERT_NE(uninitialized->snapshot(), nullptr);
  EXPECT_EQ(*uninitialized->logical_snapshot(), candidate_scene);
  const auto rebuilt_generation = rebuilt.native_projection_generation;
  const auto rebuilt_world = uninitialized->snapshot();
  const auto rebuilt_replay =
      uninitialized->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  ASSERT_TRUE(rebuilt_replay.presence_valid());
  EXPECT_EQ(rebuilt_replay.native_projection_generation, rebuilt_generation);
  EXPECT_EQ(uninitialized->snapshot(), rebuilt_world);

  auto mismatched_product = products.planning_collision_model;
  mismatched_product.product_id = core::ProductId("another-product");
  const auto rejected_rebuild =
      uninitialized->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, std::move(mismatched_product),
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  EXPECT_EQ(rejected_rebuild.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(rejected_rebuild.rollback_proven);
  EXPECT_TRUE(
      uninitialized->ProjectionHealth().healthy_for_target(candidate_revision));

  std::int64_t bootstrap_clock = 200;
  auto bootstrap_projection_result =
      RetainedPlannerSceneProjection::CreateUninitialized(
          collision_projector, products.planning_collision_model,
          native_binding,
          RetainedPlannerSceneProjectionConfig{
              "retained-lite6-bootstrap-projection", 8U, 8U},
          [&bootstrap_clock] { return bootstrap_clock++; });
  ASSERT_TRUE(bootstrap_projection_result.ok())
      << bootstrap_projection_result.error().message;
  auto bootstrap_projection = bootstrap_projection_result.value();
  const core::NativeProjectionBootstrapStatusQueryDTO bootstrap_query{
      core::BootstrapId("native-bootstrap"), core::BootstrapGeneration(3U),
      native_binding, candidate_revision};
  const auto bootstrap_absent =
      bootstrap_projection->BootstrapProjectionStatus(bootstrap_query);
  EXPECT_EQ(bootstrap_absent.state,
            core::NativeProjectionBootstrapStatusState::kAbsent);
  const core::NativeProjectionBootstrapRebuildDTO bootstrap_request{
      bootstrap_query.bootstrap_id,
      bootstrap_query.bootstrap_generation,
      native_binding,
      candidate_revision,
      products.planning_collision_model,
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene))};
  const auto bootstrap_rebuilt =
      bootstrap_projection->RebuildBootstrapProjection(bootstrap_request);
  ASSERT_TRUE(bootstrap_rebuilt.presence_valid());
  EXPECT_EQ(bootstrap_rebuilt.state,
            core::NativeProjectionBootstrapRebuildState::kRebuilt);
  EXPECT_TRUE(bootstrap_rebuilt.resulting_health.healthy_for_target(
      candidate_revision));
  const auto bootstrap_replay =
      bootstrap_projection->RebuildBootstrapProjection(bootstrap_request);
  EXPECT_EQ(bootstrap_replay.state,
            core::NativeProjectionBootstrapRebuildState::kAlreadyExact);
  EXPECT_EQ(bootstrap_replay.native_projection_generation,
            bootstrap_rebuilt.native_projection_generation);
  const auto bootstrap_status =
      bootstrap_projection->BootstrapProjectionStatus(bootstrap_query);
  EXPECT_EQ(bootstrap_status.state,
            core::NativeProjectionBootstrapStatusState::kRebuilt);

  const core::ProjectionNativeRuntimeBindingDTO added_binding{
      core::CapabilityProviderBindingId("lite6_added_planner_binding"),
      core::CapabilityProviderBindingRevision("added-binding-revision"),
      core::RuntimeUnitId("added-native-execution-host"),
      core::RuntimeEpoch("added-native-execution-host-epoch-1")};
  std::int64_t added_clock = 300;
  auto added_projection_result =
      RetainedPlannerSceneProjection::CreateUninitialized(
          collision_projector, products.planning_collision_model, added_binding,
          RetainedPlannerSceneProjectionConfig{
              "retained-lite6-added-projection", 8U, 8U},
          [&added_clock] { return added_clock++; });
  ASSERT_TRUE(added_projection_result.ok())
      << added_projection_result.error().message;
  auto added_projection = added_projection_result.value();
  const core::NativeProjectionRebuildDTO added_rebuild{
      added_binding, candidate_revision, products.planning_collision_model,
      std::make_shared<const core::LogicalSceneSnapshotDTO>(candidate_scene)};
  const auto added = added_projection->RebuildProjection(added_rebuild);
  ASSERT_TRUE(added.presence_valid());
  EXPECT_TRUE(added_projection->ProjectionHealth().healthy_for_target(
      candidate_revision));
  EXPECT_NE(added_projection->snapshot(), nullptr);
  const auto added_replay = added_projection->RebuildProjection(added_rebuild);
  EXPECT_EQ(added_replay.native_projection_generation,
            added.native_projection_generation);

  // The Program baseline advances the Scene revision under the same held
  // Apply. The existing publisher, model, and composition must stay alive.
  auto maintenance_scene = candidate_scene;
  ++maintenance_scene.revision.revision_id;
  const core::ProjectionRevisionDTO maintenance_revision{
      candidate_revision.retained_bundle_id,
      candidate_revision.graph_revision, maintenance_scene.revision};
  const core::NativeProjectionRebuildDTO maintenance_request{
      added_binding, maintenance_revision, products.planning_collision_model,
      std::make_shared<const core::LogicalSceneSnapshotDTO>(maintenance_scene),
      std::pair{core::ApplyId("apply-initial-scene"), core::ApplyGeneration(1U)},
      candidate_revision};
  const auto maintenance = added_projection->RebuildProjection(maintenance_request);
  ASSERT_TRUE(maintenance.presence_valid());
  EXPECT_EQ(maintenance.result,
            core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_TRUE(maintenance.resulting_health.healthy_for_target(
      maintenance_revision));
  const auto maintenance_replay =
      added_projection->RebuildProjection(maintenance_request);
  EXPECT_EQ(maintenance_replay.native_projection_generation,
            maintenance.native_projection_generation);
  auto foreign_apply = maintenance_request;
  foreign_apply.maintenance_successor =
      std::pair{core::ApplyId("other-apply"), core::ApplyGeneration(1U)};
  EXPECT_EQ(added_projection->RebuildProjection(foreign_apply).result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
}

TEST(RetainedProductCrossConsumer, ObservationSelectionMemoKeepsLiveChecksAndExactAlternatingKeys) {
  using deployment::CanonicalValue;
  // Opt-in source-cost qualification reuses this real verified repository and
  // resolver. Ordinary correctness/race tests keep their small fixture.
  const bool maximum_program = std::getenv("MANYFORGE_QUALIFY_PROGRAM_BYTES") != nullptr;
  ScopedTemporaryRoot temporary;
  const auto package_root = temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  const auto materialized =
      deployment::test::materialize_lite6_ros2_control_cclc_product_retained_consumer_fixture(
          package_root, kZeroDigest, true);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto& bundle = *materialized.bundle;
  const auto model_map = load_lite6_model_map(
      package_root, {bundle.retained_bundle_id(),
                     bundle.value().compiled_products.symbol_role_maps.front().product_id});
  deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
  deployment::FilesystemProgramRepository programs(package_root);
  deployment::FilesystemProgramBindingRepository bindings(package_root);

  const auto target_role = deployment::parse_strict_json_model_yaml(R"({
    "component_role":"robot",
    "required_capabilities":[
      {"capability_id":"tool.gripper.feedback","parameters":{},"disposition":"required"}
    ],
    "required_frame_roles":["tcp"],
    "required_resource_roles":["fingers"],
    "required_collision_body_roles":["gripper_collision"],
    "required_model_roles":["arm_model"]
  })")
                               .root();
  std::vector<std::shared_ptr<const deployment::ProgramBinding>> selected;
  for (const auto* program_id : {"observation.memo.a", "observation.memo.b"}) {
    auto root = deployment::normalize_program_authoring(CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.program.v2")},
        {"program_id", CanonicalValue(program_id)},
        {"program_revision", CanonicalValue(std::string(kZeroDigest))},
        {"program_profile", CanonicalValue("composable-cell-v1")},
        {"name", CanonicalValue("Observation memo fixture")},
        {"catalog",
         CanonicalValue(CanonicalValue::Object{
             {"api_version", CanonicalValue("manyforge.node_catalog.v2")},
             {"content_version",
              CanonicalValue(bundle.value().catalogs.node_environment.content_version)}})},
        {"component_roles", CanonicalValue(CanonicalValue::Object{
                                {"robot", CanonicalValue(CanonicalValue::Object{})}})},
        {"target_roles",
         CanonicalValue(CanonicalValue::Object{{"first", target_role}, {"second", target_role}})},
        {"tree", CanonicalValue(CanonicalValue::Object{
                     {"node_id", CanonicalValue("timer")},
                     {"instance_id", CanonicalValue("root")},
                     {"parameters",
                      CanonicalValue(CanonicalValue::Object{{"duration_s", CanonicalValue(1.0)}})},
                     {"children", CanonicalValue(CanonicalValue::Array{})}})},
    }));
    if (maximum_program) {
      // Three-byte NFC text reaches the C++ 16-MiB encoded boundary while
      // remaining below Python's 8-Mi-character aggregate and 1-M-character
      // per-value bounds. This is a portable Program, not an oversized DTO.
      const auto scalar = [](std::size_t bytes) {
        std::string value;
        value.reserve(bytes);
        for (std::size_t index = 0; index < bytes / 3; ++index) value += "€";
        value.append(bytes % 3, 'x');
        return value;
      };
      CanonicalValue::Array parameters;
      for (unsigned index = 0; index < 16; ++index) {
        parameters.emplace_back(CanonicalValue::Object{
            {"parameter_id", CanonicalValue("value" + std::to_string(10U + index))},
            {"type", CanonicalValue("string")},
            {"default", CanonicalValue(scalar(deployment::kMaxArtifactScalarBytes))}});
      }
      auto members = root.as_object();
      members.emplace_back("parameters", CanonicalValue(parameters));
      const auto excess = deployment::canonical_json(CanonicalValue(members)).size() -
                          deployment::kMaxArtifactEncodedBytes;
      ASSERT_LT(excess, deployment::kMaxArtifactScalarBytes);
      auto last = parameters.back().as_object();
      for (auto& [key, value] : last)
        if (key == "default")
          value = CanonicalValue(scalar(deployment::kMaxArtifactScalarBytes - excess));
      parameters.back() = CanonicalValue(std::move(last));
      members.back().second = CanonicalValue(std::move(parameters));
      root = CanonicalValue(std::move(members));
      ASSERT_EQ(deployment::canonical_json(root).size(), deployment::kMaxArtifactEncodedBytes);
    }
    const auto revision = deployment::detail::compute_content_identity(
        root, {"program-revision", "manyforge.program.v2", "program_revision"});
    auto members = root.as_object();
    for (auto& [name, value] : members)
      if (name == "program_revision") value = CanonicalValue(revision);
    const auto canonical_program = deployment::canonical_json(CanonicalValue(std::move(members)));
    const auto program = deployment::verify_program_binding_contract(canonical_program, bundle);
    if (maximum_program) {
      if (const auto* output = std::getenv("MANYFORGE_QUALIFY_PROGRAM_OUTPUT")) {
        std::ofstream stream(output);
        stream << canonical_program;
        ASSERT_TRUE(stream.good());
      }
    }
    ASSERT_TRUE(programs.put_if_absent(program, bundle));
    auto binding = deployment::compile_program_binding(
        *program,
        {{{core::ComponentRoleId("robot"), core::ControllableComponentId(std::string(
                                               deployment::test::kLite6ControllableComponentId))}},
         {{core::TargetRoleId("first"),
           core::CommandTargetId(std::string(deployment::test::kLite6GripperTargetId))},
          {core::TargetRoleId("second"),
           core::CommandTargetId(std::string(deployment::test::kLite6GripperTargetId))}}},
        bundle, model_map, catalog_store);
    ASSERT_TRUE(bindings.put_if_absent(binding, bundle, model_map));
    selected.push_back(std::move(binding));
  }

  std::vector<core::RuntimeParticipantBinding> representatives;
  for (const auto* capability : {"tool.gripper.feedback"}) {
    const auto target = deployment::resolve_runtime_program_target(
        *selected.front(), core::TargetRoleId("first"), core::CapabilityId(capability), bundle,
        model_map);
    ASSERT_TRUE(target.control_binding());
    ASSERT_TRUE(target.control_domain());
    ASSERT_TRUE(target.desired_route());
    const auto& control = *target.control_binding();
    std::set<core::RuntimeUnitId> runtime_units(target.component().runtime_unit_ids.begin(),
                                                target.component().runtime_unit_ids.end());
    runtime_units.insert(target.control_domain()->runtime_unit_ids.begin(),
                         target.control_domain()->runtime_unit_ids.end());
    runtime_units.insert(control.provider_runtime_unit_id);
    std::vector<core::RuntimeUnitEpochBindingDTO> epochs;
    for (const auto& id : runtime_units)
      epochs.push_back({id, core::RuntimeEpoch("epoch." + id.value())});
    auto binding =
        core::verify_runtime_participant_binding(deployment::build_runtime_participant_binding_dto(
            target, {*target.desired_route(),
                     core::ActiveControlConfigurationId("active.native"),
                     core::ActiveControlConfigurationGeneration(1U),
                     std::move(epochs),
                     {{control.provider_instance_id, control.provider_runtime_unit_id,
                       core::RuntimeEpoch("epoch." + control.provider_runtime_unit_id.value())}}}));
    ASSERT_TRUE(binding.ok()) << binding.error().message;
    representatives.push_back(std::move(binding.value()));
  }
  const auto physical = representatives.front();
  ProductBackedRuntimeParticipantResolver resolver(
      package_root, materialized.bundle, model_map, representatives, {});
  const auto &selected_program = selected.front()->value();
  const core::ProgramRunExecutionCaptureDTO capture_request{
      core::RequestOwnerEpoch("kernel.capture"),
      core::RequestOwnerEpoch("runner.capture"),
      core::CellAdmissionClaimGeneration(1U),
      core::ProgramRunId("run.capture"),
      selected_program.program_id,
      selected_program.program_revision,
      selected_program.program_binding_id,
      selected_program.retained_bundle_id,
      selected_program.graph_revision,
      {}};
  const auto capture = resolver.ResolveProgramCapture(capture_request);
  ASSERT_TRUE(capture.ok()) << capture.error().message;
  ASSERT_TRUE(capture.value().table);
  ASSERT_TRUE(capture.value().binding);
  ASSERT_TRUE(capture.value().blackboard_key_declarations);
  ASSERT_TRUE(capture.value().io_conditions);
  EXPECT_TRUE(capture.value().io_conditions->program_node_conditions.empty());
  EXPECT_TRUE(capture.value()
                  .io_conditions->referenced_program_blackboard_keys.empty());
  auto injected_conditions = capture_request;
  injected_conditions.io_conditions =
      std::make_shared<const core::ProgramIoConditionProjectionDTO>();
  EXPECT_FALSE(resolver.ResolveProgramCapture(injected_conditions).ok());
  EXPECT_EQ(*capture.value().binding, selected_program);
  // Identity-only ingress cannot supply a table or a pre-resolved Binding.
  EXPECT_FALSE(resolver.ResolveProgramCapture(capture.value()).ok());
  auto injected_declarations = capture_request;
  injected_declarations.blackboard_key_declarations = std::make_shared<
      const std::vector<core::ProgramBlackboardKeyDeclarationDTO>>(
      std::vector<core::ProgramBlackboardKeyDeclarationDTO>{
          {"caller.grant", "bool"}});
  EXPECT_FALSE(resolver.ResolveProgramCapture(injected_declarations).ok());
  const auto query =
      [&](const auto &binding_id, const auto &target_id,
          const auto &capability_id, const auto &source,
          std::span<const core::RuntimeParticipantBinding> current) {
        return resolver.ResolveObservationTarget(
            binding_id, target_id, capability_id, source, current);
      };
  const auto binding_id = selected.front()->program_binding_id();
  const core::TargetRoleId target_id("first");
  const core::CapabilityId capability_id("tool.gripper.feedback");
  if (maximum_program) {
    for (const auto* profile : {"cold", "same_key", "alternating_key"}) {
      std::vector<double> samples;
      for (unsigned index = 0; index < (std::string_view(profile) == "cold" ? 1U : 20U); ++index) {
        const bool alternate = std::string_view(profile) == "alternating_key";
        const auto& representative = representatives.front();
        const auto begun = std::chrono::steady_clock::now();
        const auto result =
            query(binding_id, alternate && index % 2 ? core::TargetRoleId("second") : target_id,
                  representative.value().capability_id, physical, representatives);
        samples.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun)
                .count());
        ASSERT_TRUE(result.ok()) << result.error().message;
      }
      std::sort(samples.begin(), samples.end());
      std::cout << "{\"program_bytes\":" << deployment::kMaxArtifactEncodedBytes
                << ",\"profile\":\"" << profile << "\",\"samples\":" << samples.size()
                << ",\"p95_ms\":"
                << samples[std::min(samples.size() - 1, samples.size() * 95 / 100)]
                << ",\"max_ms\":" << samples.back() << "}" << std::endl;
    }
    return;
  }
  for (int hit = 0; hit < 2; ++hit) {
    const auto result = query(binding_id, target_id, capability_id, physical, representatives);
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().observation_binding.value(), physical.value());
  }

  // An unknown route supplies no current representative. A warm immutable
  // selection must not turn it, or a changed owner, into a valid live read.
  EXPECT_FALSE(query(binding_id, target_id, capability_id, physical, {}).ok());
  const auto reject_changed = [&](auto change) {
    auto changed = representatives;
    auto value = changed.front().value();
    change(value);
    auto sealed = core::verify_runtime_participant_binding(std::move(value));
    ASSERT_TRUE(sealed.ok()) << sealed.error().message;
    changed.front() = std::move(sealed.value());
    EXPECT_FALSE(query(binding_id, target_id, capability_id, physical, changed).ok());
  };
  reject_changed([](auto& value) {
    value.control_path->control_route_id = core::ControlRouteId("changed.route");
  });
  reject_changed([](auto& value) {
    value.control_path->provider_instance_id = core::ProviderInstanceId("changed.provider");
  });
  reject_changed([](auto& value) {
    value.control_path->active_control_configuration_generation =
        core::ActiveControlConfigurationGeneration(2U);
  });
  auto newer = representatives;
  auto newer_value = newer.front().value();
  auto& path = *newer_value.control_path;
  path.active_control_configuration_generation = core::ActiveControlConfigurationGeneration(2U);
  path.provider_runtime_epoch = core::RuntimeEpoch("epoch.replacement");
  for (auto& epoch : newer_value.runtime_unit_epochs)
    if (epoch.runtime_unit_id == path.provider_runtime_unit_id)
      epoch.runtime_epoch = path.provider_runtime_epoch;
  auto newer_binding = core::verify_runtime_participant_binding(std::move(newer_value));
  ASSERT_TRUE(newer_binding.ok()) << newer_binding.error().message;
  newer.front() = std::move(newer_binding.value());
  EXPECT_FALSE(query(binding_id, target_id, capability_id, physical, newer).ok());
  const auto fresh = query(binding_id, target_id, capability_id, newer.front(), newer);
  ASSERT_TRUE(fresh.ok()) << fresh.error().message;
  EXPECT_EQ(fresh.value().observation_binding.value(), newer.front().value());

  const auto check_key = [&](const auto& binding, const char* role, const auto& representative) {
    const auto& capability = representative.value().capability_id;
    const auto result = query(binding->program_binding_id(), core::TargetRoleId(role), capability,
                              physical, representatives);
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().target.program_binding_id(), binding->program_binding_id());
    EXPECT_EQ(result.value().target.target_role().target_role_id, core::TargetRoleId(role));
    EXPECT_EQ(result.value().target.capability().capability_id, capability);
    EXPECT_EQ(result.value().observation_binding.value(), representative.value());
  };
  // Exercise each key axis independently before racing replacement; returned
  // values must remain pinned even when the capacity-one selection changes.
  for (const auto& binding : selected)
    for (const auto* role : {"first", "second"})
      for (const auto& representative : representatives) check_key(binding, role, representative);
  std::barrier rendezvous(2);
  const auto concurrent_reads = [&](std::size_t offset) {
    for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
      rendezvous.arrive_and_wait();
      check_key(selected[(iteration + offset) % selected.size()],
                iteration % 2U == 0U ? "first" : "second",
                representatives[(iteration / 2U + offset) % representatives.size()]);
    }
  };
  std::jthread first(concurrent_reads, 0U);
  std::jthread second(concurrent_reads, 1U);
  first.join();
  second.join();
}

TEST(RetainedProductCrossConsumer,
     IntrinsicEndpointRetainsItsExactAssociatedBodyRows) {
  ScopedTemporaryRoot temporary;
  const auto package_root = temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  const auto materialized = deployment::test::
      materialize_lite6_ros2_control_observed_endpoint_retained_consumer_fixture(
          package_root, false, true);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto model_map = load_lite6_model_map(
      package_root,
      {bundle.retained_bundle_id(),
       bundle.value().compiled_products.symbol_role_maps.front().product_id});
  deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          bundle, model_map, catalog_store);
  ASSERT_EQ(inventory.motion.size(), 1U);
  const auto &program_binding = *inventory.motion.front();
  const auto &target = program_binding.value().target_role_bindings.front();
  const auto physical = with_program_binding_identity(
      make_fixture_runtime_participant(
          bundle, model_map, target.controllable_component_id,
          target.command_target_id, std::nullopt, std::nullopt, std::nullopt,
          core::CapabilityId("execution.trajectory.execute")),
      program_binding);
  const auto tcp =
      std::find_if(target.frame_role_bindings.begin(),
                   target.frame_role_bindings.end(), [](const auto &frame) {
                     return frame.frame_role_id == core::FrameRoleId("tcp");
                   });
  ASSERT_NE(tcp, target.frame_role_bindings.end());
  ASSERT_EQ(tcp->contact_body_role_ids.size(), 1U);
  ProductBackedRuntimeParticipantResolver resolver(
      package_root, materialized.bundle, model_map, {physical},
      {program_binding.program_binding_id()});
  const auto scene = make_projection_runtime_shadow_snapshot(
      bundle.value().cell_graph.cell_id);
  const auto operation = make_motion_operation(
      physical, core::RequestOwnerEpoch("kernel.endpoint"),
      scene.shared()->revision);
  const auto resolved = resolver.Resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  const auto &endpoint = resolved.value().value().motion_endpoint;
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint->frame, *tcp);
  ASSERT_EQ(endpoint->contact_body_bindings.size(), 1U);
  const auto &body = endpoint->contact_body_bindings.front();
  EXPECT_EQ(body.controllable_component_id,
            physical.value().controllable_component_id);
  EXPECT_EQ(body.model_instance_id, tcp->model_instance_id);
  EXPECT_EQ(body.model_revision, tcp->model_revision);
  EXPECT_EQ(body.collision_body_role_id, tcp->contact_body_role_ids.front());
  EXPECT_FALSE(body.collision_body_ids.empty());
  EXPECT_EQ(resolved.value().value().resources, physical.value().resources);
}

TEST(RetainedProductCrossConsumer,
     TwoSameTypeProgramsResolveDistinctPhysicalParticipantsWithoutAliasing) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));

  constexpr std::string_view cell_id = "i6_two_lite6_program_binding_cell";
  const auto materialized = deployment::test::
      materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto &graph = bundle.value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.controllable_components.size(), 2U);
  const auto &shared_runtime_unit_id = graph.runtime_units.front().runtime_unit_id;
  for (const auto &component : graph.controllable_components) {
    ASSERT_EQ(component.runtime_unit_ids.size(), 1U);
    EXPECT_EQ(component.runtime_unit_ids.front(), shared_runtime_unit_id);
  }
  // Q68: both selected ROS components belong to this exact shared host.
  // Cross-unit isolation remains NOT_RUN/R7 after native simulator retirement.
  ASSERT_EQ(bundle.value().compiled_products.symbol_role_maps.size(), 1U);
  const auto model_map = load_lite6_model_map(
      package_root,
      {bundle.retained_bundle_id(),
       bundle.value().compiled_products.symbol_role_maps.front().product_id});
  deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          bundle, model_map, catalog_store);
  ASSERT_EQ(inventory.motion.size(), 2U);
  EXPECT_TRUE(inventory.gripper.empty());

  std::set<core::ProgramBindingId> program_binding_ids;
  std::set<core::ControllableComponentId> component_ids;
  std::set<core::ModelInstanceId> model_instance_ids;
  std::set<core::RuntimeUnitId> runtime_unit_ids;
  std::set<core::ResourceId> all_resources;
  std::set<std::string> all_compiled_resources;
  std::vector<std::vector<std::string>> source_joint_orders;
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  physical_bindings.reserve(inventory.motion.size());
  for (const auto &program_binding : inventory.motion) {
    ASSERT_NE(program_binding, nullptr);
    ASSERT_EQ(program_binding->value().component_role_bindings.size(), 1U);
    ASSERT_EQ(program_binding->value().target_role_bindings.size(), 1U);
    const auto component_id = program_binding->value()
                                  .component_role_bindings.front()
                                  .controllable_component_id;
    EXPECT_TRUE(
        program_binding_ids.insert(program_binding->program_binding_id())
            .second);
    EXPECT_TRUE(component_ids.insert(component_id).second);
    auto physical = with_program_binding_identity(
        make_fixture_runtime_participant(
            bundle, model_map, component_id,
            core::CommandTargetId(
                std::string(deployment::test::kLite6ArmTargetId))),
        *program_binding);
    const auto &value = physical.value();
    ASSERT_EQ(value.model_bindings.size(), 1U);
    EXPECT_TRUE(model_instance_ids
                    .insert(value.model_bindings.front().model_instance_id)
                    .second);
    ASSERT_EQ(value.runtime_unit_epochs.size(), 1U);
    runtime_unit_ids.insert(value.runtime_unit_epochs.front().runtime_unit_id);
    std::vector<std::string> source_joint_order;
    source_joint_order.reserve(value.resources.size());
    for (const auto &resource : value.resources) {
      ASSERT_TRUE(resource.source_joint_name.has_value());
      source_joint_order.push_back(*resource.source_joint_name);
      EXPECT_TRUE(all_resources.insert(resource.resource_id).second);
      EXPECT_TRUE(all_compiled_resources.insert(resource.compiled_resource_name)
                      .second);
    }
    source_joint_orders.push_back(std::move(source_joint_order));
    physical_bindings.push_back(std::move(physical));
  }
  EXPECT_EQ(program_binding_ids.size(), 2U);
  EXPECT_EQ(component_ids.size(), 2U);
  EXPECT_EQ(model_instance_ids.size(), 2U);
  ASSERT_EQ(runtime_unit_ids.size(), 1U);
  EXPECT_EQ(*runtime_unit_ids.begin(), shared_runtime_unit_id);
  EXPECT_EQ(all_resources.size(), 12U);
  EXPECT_EQ(all_compiled_resources.size(), 12U);
  ASSERT_EQ(source_joint_orders.size(), 2U);
  EXPECT_EQ(source_joint_orders[0], source_joint_orders[1]);

  ProductBackedRuntimeParticipantResolver resolver(
      package_root, materialized.bundle,
      load_lite6_model_map(package_root,
                           {bundle.retained_bundle_id(),
                            bundle.value()
                                .compiled_products.symbol_role_maps.front()
                                .product_id}),
      physical_bindings,
      {program_binding_ids.begin(), program_binding_ids.end()});
  // The control readmodel qualifies sealed selectors against the actual
  // product and physical prototypes, without a separate alias inventory.
  for (std::size_t index = 0U; index < inventory.motion.size(); ++index) {
    const auto &selected = *inventory.motion[index];
    const auto &role = selected.value().target_role_bindings.front();
    const auto bound = resolver.ResolveBoundTarget(
        selected, role.target_role_id,
        core::CapabilityId("execution.trajectory.execute"));
    ASSERT_TRUE(bound.ok()) << bound.error().message;
    EXPECT_EQ(bound.value().program_binding_id(), selected.program_binding_id());
    EXPECT_EQ(bound.value().target_role().component_role_id, role.component_role_id);
    EXPECT_EQ(bound.value().target_role().target_role_id, role.target_role_id);
    EXPECT_EQ(bound.value().component().controllable_component_id,
              physical_bindings[index].value().controllable_component_id);
    EXPECT_EQ(bound.value().target_role().command_target_id,
              physical_bindings[index].value().command_target_id);
    EXPECT_NE(bound.value().component().controllable_component_id,
              physical_bindings[(index + 1U) % physical_bindings.size()]
                  .value().controllable_component_id);
    EXPECT_FALSE(resolver.ResolveBoundTarget(
        selected, core::TargetRoleId("missing.role"),
        core::CapabilityId("execution.trajectory.execute")).ok());
    EXPECT_FALSE(resolver.ResolveBoundTarget(
        selected, role.target_role_id,
        core::CapabilityId("tool.gripper.command")).ok());
  }
  const auto scene = make_projection_runtime_shadow_snapshot(
      core::CellId(std::string(cell_id)));
  for (std::size_t index = 0U; index < physical_bindings.size(); ++index) {
    auto operation = make_motion_operation(
        physical_bindings[index], core::RequestOwnerEpoch("kernel.epoch.two"),
        scene.shared()->revision);
    operation.request_context.request_id =
        core::RequestId("request.two-lite6." + std::to_string(index + 1U));
    ASSERT_TRUE(resolver.ValidateExecutionOrigin(operation).ok());
    auto forged = operation;
    forged.program_id = core::ProgramId("forged-direct-program");
    EXPECT_FALSE(resolver.ValidateExecutionOrigin(forged).ok());
    EXPECT_FALSE(resolver.Resolve(forged, 0U).ok());
    const auto resolved = resolver.Resolve(operation, 0U);
    ASSERT_TRUE(resolved.ok()) << resolved.error().message;
    EXPECT_EQ(resolved.value().value().program_binding_id,
              physical_bindings[index].value().program_binding_id);
    EXPECT_EQ(resolved.value().value().controllable_component_id,
              physical_bindings[index].value().controllable_component_id);
    EXPECT_EQ(resolved.value().value().resources,
              physical_bindings[index].value().resources);
    const auto &endpoint = resolved.value().value().motion_endpoint;
    ASSERT_TRUE(endpoint);
    EXPECT_EQ(endpoint->target_role_id,
              physical_bindings[index].value().target_role_id);
    EXPECT_EQ(endpoint->command_target_id,
              physical_bindings[index].value().command_target_id);
    EXPECT_EQ(endpoint->frame.frame_role_id, core::FrameRoleId("tcp"));
    auto crossed_endpoint = operation;
    std::get<core::MotionParticipantIntentDTO>(
        crossed_endpoint.participants.front().payload)
        .tcp_endpoint.target_role_id =
        core::TargetRoleId("missing.endpoint.owner");
    EXPECT_FALSE(resolver.Resolve(crossed_endpoint, 0U).ok());
    crossed_endpoint = operation;
    std::get<core::MotionParticipantIntentDTO>(
        crossed_endpoint.participants.front().payload)
        .tcp_endpoint.frame_role_id =
        core::FrameRoleId("missing.endpoint.frame");
    EXPECT_FALSE(resolver.Resolve(crossed_endpoint, 0U).ok());
    const auto &physical = physical_bindings[index];
    const auto &control = *physical.value().control_path;
    const auto context = core::make_observed_state_query_context(physical, 100);
    ASSERT_TRUE(context.ok());
    auto sample = make_fixture_state(
        context.value(),
        std::vector<double>(physical.value().resources.size(), 0.0), 1, 10);
    core::ActiveControlConfigurationDTO configuration{
        control.active_control_configuration_id,
        control.control_domain_id,
        control.control_domain_revision,
        {{control.control_binding_id, control.control_binding_revision,
          control.control_route_id}},
        control.activation_set_id,
        control.controller_topology_id,
        control.topology_revision,
        control.provider_runtime_unit_id,
        control.provider_runtime_epoch,
        1,
        {},
        control.active_control_configuration_generation,
        core::ActiveControlConfigurationStateDTO::kReconciled};
    for (const auto &resource : physical.value().resources) {
      configuration.kinematic_resource_source_projections.push_back(
          {resource.resource_id,
           {{control.control_binding_id, control.control_binding_revision}},
           control.provider_instance_id,
           control.provider_manifest_id,
           control.provider_manifest_revision,
           control.state_authority_id,
           control.state_authority_class,
           sample.source_kind});
    }
    ASSERT_TRUE(
        target_observation_source_is_current(physical, sample, configuration));
    auto changed_configuration = configuration;
    changed_configuration.state = core::ActiveControlConfigurationStateDTO::
        kExternalOrUnknownUnreconciled;
    EXPECT_FALSE(target_observation_source_is_current(physical, sample,
                                                      changed_configuration));
    changed_configuration = configuration;
    changed_configuration.active_control_configuration_generation =
        core::ActiveControlConfigurationGeneration(2);
    EXPECT_FALSE(target_observation_source_is_current(physical, sample,
                                                      changed_configuration));
    changed_configuration = configuration;
    changed_configuration.binding_routes.clear();
    EXPECT_FALSE(target_observation_source_is_current(physical, sample,
                                                      changed_configuration));
    changed_configuration = configuration;
    changed_configuration.kinematic_resource_source_projections.front()
        .publication_source = core::ObservedStateSourceKind::kActionFeedback;
    EXPECT_FALSE(target_observation_source_is_current(physical, sample,
                                                      changed_configuration));
    auto changed_sample = sample;
    changed_sample.provider_instance_id =
        core::ProviderInstanceId("different.provider");
    EXPECT_FALSE(target_observation_source_is_current(physical, changed_sample,
                                                      configuration));
    changed_sample = sample;
    changed_sample.source_epoch = core::RuntimeEpoch("different.epoch");
    EXPECT_FALSE(target_observation_source_is_current(physical, changed_sample,
                                                      configuration));
    // A second capability binding may read this source only when the current
    // owner explicitly groups both bindings. Never relabel the sample.
    changed_sample = sample;
    changed_sample.control_binding_id =
        core::ControlBindingId("source.binding");
    EXPECT_FALSE(target_observation_source_is_current(physical, changed_sample,
                                                      configuration));
    configuration.binding_routes.push_back(
        {changed_sample.control_binding_id,
         changed_sample.control_binding_revision,
         changed_sample.control_route_id});
    for (auto &group : configuration.kinematic_resource_source_projections)
      group.contributing_control_bindings.push_back(
          {changed_sample.control_binding_id,
           changed_sample.control_binding_revision});
    EXPECT_TRUE(target_observation_source_is_current(physical, changed_sample,
                                                     configuration));
    const auto &frames = inventory.motion[index]
                             ->value()
                             .target_role_bindings.front()
                             .frame_role_bindings;
    ASSERT_FALSE(frames.empty());
    for (const auto &frame : frames) {
      EXPECT_TRUE(
          resolver.FrameObservationCovered(frame, physical_bindings[index]));
      if (frame.frame_role_id.value() == "tcp") {
        EXPECT_FALSE(resolver.FrameObservationCovered(
            frame, physical_bindings[(index + 1U) % physical_bindings.size()]));
      }
    }
  }

  auto unknown = make_motion_operation(
      physical_bindings.front(), core::RequestOwnerEpoch("kernel.epoch.two"),
      scene.shared()->revision);
  unknown.program_binding_id = core::ProgramBindingId("unknown-binding");
  const auto rejected = resolver.Resolve(unknown, 0U);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_program_binding_unavailable");
}

TEST(
    RetainedProductCrossConsumer,
    PlannerSceneProjectionRebuildsWithoutFabricatedFrameState) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  constexpr std::string_view cell_id = "i6_projection_transition_cell";
  const auto predecessor = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  const auto candidate = deployment::test::
      materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(predecessor.bundle, nullptr);
  ASSERT_NE(candidate.bundle, nullptr);
  ASSERT_NE(predecessor.bundle->retained_bundle_id(),
            candidate.bundle->retained_bundle_id());

  const auto load_projector =
      [&](const deployment::test::DurableLite6RetainedConsumerFixture &fixture,
          std::string model_name) {
        const auto &products = fixture.bundle->value().compiled_products;
        EXPECT_EQ(products.symbol_role_maps.size(), 1U);
        const auto retained = load_lite6_planning_model(
            package_root, {fixture.bundle->retained_bundle_id(),
                           products.planning_collision_model.product_id});
        const auto model_map = load_lite6_model_map(
            package_root, {fixture.bundle->retained_bundle_id(),
                           products.symbol_role_maps.front().product_id});
        auto model_stage = RetainedPlanningModelStage::create_verified(
            retained, staging_parent);
        const auto *target = model_map.find_target(
            core::ControllableComponentId(
                std::string(deployment::test::kLite6ControllableComponentId)),
            core::CommandTargetId(
                std::string(deployment::test::kLite6ArmTargetId)));
        EXPECT_NE(target, nullptr);
        if (target == nullptr || target->model_groups.empty()) {
          throw std::logic_error(
              "cross-bundle fixture has no Lite6 arm model group");
        }
        auto shared_model = ManyForgeCollisionEngine::load_shared_model(
            std::move(model_name), model_stage.loader_urdf_path(),
            model_stage.srdf_path(), model_stage.package_paths(),
            target->model_groups.front().compiled_group_name);
        return std::pair{
            RetainedCollisionWorldProjector::create_verified(
                shared_model, *fixture.bundle, model_map, retained),
            std::move(shared_model)};
      };

  auto [predecessor_projector, predecessor_shared_model] =
      load_projector(predecessor, "i6_projection_predecessor");
  auto [candidate_projector, candidate_shared_model] =
      load_projector(candidate, "i6_projection_candidate");
  const auto initial_scene = make_collision_scene_snapshot(
      core::CellId(std::string(cell_id)), 2U,
      core::SceneResourceId("cross-bundle-object"), 2.0, 0.1);
  const core::ProjectionRevisionDTO predecessor_revision{
      predecessor.bundle->retained_bundle_id(),
      predecessor.bundle->value().cell_graph.graph_revision,
      initial_scene.revision};
  const core::ProjectionRevisionDTO candidate_revision{
      candidate.bundle->retained_bundle_id(),
      candidate.bundle->value().cell_graph.graph_revision,
      initial_scene.revision};
  const core::ProjectionNativeRuntimeBindingDTO binding{
      core::CapabilityProviderBindingId("cross_bundle_planner_binding"),
      core::CapabilityProviderBindingRevision("cross-bundle-binding-revision"),
      core::RuntimeUnitId("cross_bundle_native_execution"),
      core::RuntimeEpoch("cross-bundle-native-epoch-1")};
  std::int64_t clock = 400;
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      std::move(predecessor_projector),
      predecessor.bundle->value().compiled_products.planning_collision_model,
      binding, initial_scene,
      RetainedPlannerSceneProjectionConfig{
          "cross-bundle-transactional-projection", 8U, 8U},
      [&clock] { return clock++; });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  auto projection = std::move(projection_result.value());
  EXPECT_EQ(projection->collision_shared_model(), predecessor_shared_model);
  const auto predecessor_lease = projection->RetainProjection(
      predecessor_revision,
      core::ProjectionLeaseId("command-fenced-predecessor-lease"));
  ASSERT_TRUE(predecessor_lease.ok()) << predecessor_lease.error().message;
  const auto predecessor_frame =
      projection->ResolveSceneFrame(core::SceneFrameResolutionQueryDTO{
          core::CellSceneFrameRefDTO{core::CellId(std::string(cell_id))},
          predecessor_lease.value().projection_binding});
  ASSERT_FALSE(predecessor_frame.ok());
  EXPECT_EQ(predecessor_frame.error().details.at("reason"),
            "native_scene_frame_lease_unavailable");
  projection->ReleaseProjection(
      predecessor_lease.value().projection_binding.projection_lease_id);
  const auto predecessor_world = projection->snapshot();
  ASSERT_NE(predecessor_world, nullptr);

  const core::NativeProjectionRebuildDTO request{
      binding, candidate_revision,
      candidate.bundle->value().compiled_products.planning_collision_model,
      std::make_shared<const core::LogicalSceneSnapshotDTO>(initial_scene)};
  const auto unprepared = projection->RebuildProjection(request);
  EXPECT_EQ(unprepared.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  ASSERT_TRUE(unprepared.error.has_value());
  EXPECT_EQ(unprepared.error->details.at("reason"),
            "native_projection_rebuild_mismatch");
  EXPECT_EQ(projection->snapshot(), predecessor_world);
  EXPECT_TRUE(
      projection->ProjectionHealth().healthy_for_target(predecessor_revision));

  // A different retained model is installed only after the old computational
  // projection is retired; it is never swapped into a healthy projection.
  projection.reset();
  auto candidate_result = RetainedPlannerSceneProjection::CreateInitialized(
      std::move(candidate_projector), request.planning_collision_model,
      binding, initial_scene,
      RetainedPlannerSceneProjectionConfig{
          "cross-bundle-candidate-projection", 8U, 8U},
      [&clock] { return clock++; });
  ASSERT_TRUE(candidate_result.ok()) << candidate_result.error().message;
  auto candidate_projection = std::move(candidate_result.value());
  EXPECT_TRUE(candidate_projection->ProjectionHealth().healthy_for_target(
      candidate_revision));
  EXPECT_EQ(candidate_projection->collision_shared_model(),
            candidate_shared_model);
  EXPECT_NE(candidate_projection->snapshot(), predecessor_world);
  EXPECT_NE(candidate_projection->snapshot()->publisher_id,
            predecessor_world->publisher_id);
  const auto candidate_lease = candidate_projection->RetainProjection(
      candidate_revision,
      core::ProjectionLeaseId("command-fenced-candidate-lease"));
  ASSERT_TRUE(candidate_lease.ok()) << candidate_lease.error().message;
  const auto candidate_frame =
      candidate_projection->ResolveSceneFrame(core::SceneFrameResolutionQueryDTO{
          core::CellSceneFrameRefDTO{core::CellId(std::string(cell_id))},
          candidate_lease.value().projection_binding});
  ASSERT_FALSE(candidate_frame.ok());
  EXPECT_EQ(candidate_frame.error().details.at("reason"),
            "native_scene_frame_lease_unavailable");
  candidate_projection->ReleaseProjection(
      candidate_lease.value().projection_binding.projection_lease_id);
}

/// Exact Lite6 retained collision projector for provider-freshness tests.
struct ProviderFreshnessFixture final {
  ProviderFreshnessFixture() {
    const auto package_root = temporary.path() / "retained-package";
    const auto staging_parent = temporary.path() / "private-stages";
    std::filesystem::create_directory(package_root);
    std::filesystem::create_directory(staging_parent);
    bundle = deployment::test::
                 materialize_lite6_ros2_control_arm_gripper_aggregate_retained_consumer_fixture(
                     package_root)
                     .bundle;
    if (bundle == nullptr) {
      throw std::logic_error("Lite6 freshness fixture has no retained bundle");
    }
    const auto &products = bundle->value().compiled_products;
    planning_collision_model = products.planning_collision_model;
    const auto retained = load_lite6_planning_model(
        package_root, {bundle->retained_bundle_id(),
                       products.planning_collision_model.product_id});
    const auto model_map = load_lite6_model_map(
        package_root, {bundle->retained_bundle_id(),
                       products.symbol_role_maps.front().product_id});
    auto stage =
        RetainedPlanningModelStage::create_verified(retained, staging_parent);
    const auto *target = model_map.find_target(
        core::ControllableComponentId(
            std::string(deployment::test::kLite6ControllableComponentId)),
        core::CommandTargetId(std::string(deployment::test::kLite6ArmTargetId)));
    if (target == nullptr || target->model_groups.empty()) {
      throw std::logic_error("Lite6 freshness fixture has no arm group");
    }
    auto shared_model = ManyForgeCollisionEngine::load_shared_model(
        "retained_lite6_provider_freshness", stage.loader_urdf_path(),
        stage.srdf_path(), stage.package_paths(),
        target->model_groups.front().compiled_group_name);
    projector.emplace(RetainedCollisionWorldProjector::create_verified(
        shared_model, *bundle, model_map, retained));
  }

  [[nodiscard]] const core::CellId &cell_id() const {
    return bundle->value().cell_graph.cell_id;
  }

  [[nodiscard]] core::ProjectionRevisionDTO
  revision(const core::LogicalSceneSnapshotDTO &scene) const {
    return {bundle->retained_bundle_id(),
            bundle->value().cell_graph.graph_revision, scene.revision};
  }

  ScopedTemporaryRoot temporary;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  core::RetainedProductDTO planning_collision_model{
      core::ProductId(""), core::RetainedProductKind::kPlanningCollisionModel,
      core::ProductContractId(""), core::ProductContractRevision(""), {}};
  std::optional<RetainedCollisionWorldProjector> projector;
};

constexpr std::string_view kObservedObject = "a-observed";
constexpr std::string_view kOtherObject = "b-other";

[[nodiscard]] core::SceneObjectProviderEvidenceDTO
freshness_evidence(const std::string &instance, const std::uint64_t sequence,
                   const std::int64_t received_at_ns) {
  return core::SceneObjectProviderEvidenceDTO{
      core::ProviderInstanceId(instance),
      core::ProviderManifestId("manyforge.provider.logical_scene"),
      core::ProviderManifestRevision("sha256:" + std::string(64U, 'b')),
      "sha256:" + std::string(64U, 'a'),
      sequence,
      received_at_ns,
      1'250'000'000,
      10'000'000'000};
}

struct FreshnessSceneObject final {
  std::string_view object_id;
  double x_m;
  std::optional<core::SceneObjectProviderEvidenceDTO> evidence;
};

[[nodiscard]] core::Pose3DTO freshness_pose(const double x_m) {
  return {{x_m, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
}

/// Objects must be supplied in scene-resource-id order.
[[nodiscard]] core::LogicalSceneSnapshotDTO
make_freshness_scene(const core::CellId &cell_id,
                     const std::uint64_t scene_revision_id,
                     const std::vector<FreshnessSceneObject> &objects) {
  auto base = make_projection_runtime_shadow_snapshot(cell_id);
  core::LogicalSceneSnapshotDTO snapshot = *base.shared();
  snapshot.revision.revision_id = scene_revision_id;
  snapshot.transaction_sequence = scene_revision_id - 1U;
  snapshot.topology_epoch = scene_revision_id - 1U;
  for (const auto &object : objects) {
    snapshot.objects.push_back(core::SceneObjectSnapshotDTO{
        core::SceneResourceId(std::string(object.object_id)),
        {core::BoxShapeDTO{{0.1, 0.1, 0.1}}},
        core::CellFixedSceneObjectPlacementDTO{cell_id,
                                               freshness_pose(object.x_m)},
        object.evidence,
    });
  }
  snapshot.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
  if (!snapshot.locally_valid()) {
    throw std::logic_error("test freshness logical-scene snapshot is invalid");
  }
  return snapshot;
}

[[nodiscard]] core::ProviderFreshnessConfirmationDTO
freshness_confirmation(const core::ProjectionRevisionDTO &revision,
                       const double x_m,
                       core::SceneObjectProviderEvidenceDTO evidence) {
  return {revision, core::SceneResourceId(std::string(kObservedObject)),
          freshness_pose(x_m), std::move(evidence)};
}

[[nodiscard]] bool observed_body(const std::string &body_id) {
  return body_id.starts_with("scene/" + std::string(kObservedObject) +
                             "/shape/");
}

/// Freshness of the observed object in a sidecar; every one of its bodies
/// must carry the same timestamp.
[[nodiscard]] std::optional<std::int64_t> observed_sidecar_timestamp(
    const std::vector<core::CollisionWorldBodyFreshness> &sidecar) {
  std::optional<std::int64_t> result;
  bool seen = false;
  for (const auto &entry : sidecar) {
    if (!observed_body(entry.body_id)) {
      continue;
    }
    if (seen && entry.source_timestamp_ns != result) {
      throw std::logic_error("observed bodies disagree on freshness");
    }
    seen = true;
    result = entry.source_timestamp_ns;
  }
  return result;
}

[[nodiscard]] const core::SceneSnapshotLiveObject &
live_object(const core::CollisionWorldLeaseInventory &inventory,
            const std::string_view object_id) {
  for (const auto &live : inventory.live_objects) {
    if (live.object_id == object_id) {
      return live;
    }
  }
  throw std::logic_error("lease inventory has no such live object");
}

[[nodiscard]] RetainedCollisionWorldPublicationContext
freshness_commit_context(const std::int64_t observed_at_ns) {
  return RetainedCollisionWorldPublicationContext{
      core::CollisionWorldMutationSource::kPoseSourceApply,
      {},
      observed_at_ns,
      {},
      "provider_freshness_test_commit",
      std::nullopt};
}

TEST(RetainedProductCrossConsumer,
     ProviderFreshnessPublishesSidecarOnlyAndKeepsLeasesImmutable) {
  ProviderFreshnessFixture fixture;
  const auto committed = freshness_evidence("isaac.pose.observer", 7U,
                                            4'000'000'000);
  const auto other = freshness_evidence("isaac.other.observer", 3U,
                                        3'000'000'000);
  const auto initial = make_freshness_scene(
      fixture.cell_id(), 2U,
      {{kObservedObject, 10.0, committed}, {kOtherObject, 12.0, other}});
  std::int64_t clock_value = 4'500'000'000;
  auto publisher_result = RetainedCollisionWorldPublisher::create(
      *fixture.projector, "retained-lite6-freshness-publisher", initial,
      RetainedCollisionWorldPublicationContext{
          core::CollisionWorldMutationSource::kDeploymentApply,
          {},
          10,
          {},
          "provider_freshness_initialize",
          std::nullopt},
      [&clock_value] { return clock_value++; });
  ASSERT_TRUE(publisher_result.ok()) << publisher_result.error().message;
  const auto publisher = publisher_result.value();
  const auto revision = fixture.revision(initial);

  const core::ProjectionLeaseId earlier_lease("operation.before.freshness");
  const auto earlier = publisher->retain_scene_snapshot_lease_inventory(
      earlier_lease);
  ASSERT_TRUE(earlier.has_value());
  const auto before = publisher->published_runtime_collision_query_bundle();
  ASSERT_NE(before, nullptr);
  ASSERT_EQ(observed_sidecar_timestamp(before->body_freshness),
            std::optional<std::int64_t>(4'000'000'000));
  const auto world_revision = publisher->published_collision_world_revision();
  const auto ledger_size = publisher->recent_ledger().size();

  auto confirmed = committed;
  confirmed.provider_source_sequence = 8U;
  confirmed.received_at_ns = 5'000'000'000;
  confirmed.source_timestamp_ns = 1'500'000'000;
  const auto applied = publisher->publish_freshness(
      freshness_confirmation(revision, 10.0, confirmed));
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_EQ(applied.value(), before->epoch + 1U);

  const auto after = publisher->published_runtime_collision_query_bundle();
  ASSERT_NE(after, nullptr);
  EXPECT_NE(after, before);
  EXPECT_EQ(after->epoch, before->epoch + 1U);
  EXPECT_EQ(after->world, before->world);
  EXPECT_EQ(after->policy, before->policy);
  EXPECT_EQ(after->policy_index, before->policy_index);
  EXPECT_EQ(after->projection_revision, before->projection_revision);
  EXPECT_EQ(after->selected_distance_pairs, before->selected_distance_pairs);
  EXPECT_TRUE(after->lookup_order_valid);
  EXPECT_TRUE(after->distance_selection_order_valid);
  ASSERT_EQ(after->body_freshness.size(), before->body_freshness.size());
  std::size_t observed_entries = 0U;
  for (std::size_t index = 0U; index < after->body_freshness.size(); ++index) {
    const auto &entry = after->body_freshness[index];
    const auto &prior = before->body_freshness[index];
    ASSERT_EQ(entry.body_id, prior.body_id);
    if (index > 0U) {
      EXPECT_LT(after->body_freshness[index - 1U].body_id, entry.body_id);
    }
    if (observed_body(entry.body_id)) {
      ++observed_entries;
      EXPECT_EQ(entry.source_timestamp_ns,
                std::optional<std::int64_t>(5'000'000'000));
      EXPECT_EQ(entry.source_id, std::optional<std::string>(
                                     "isaac.pose.observer"));
      EXPECT_EQ(entry.source_freshness_budget_ns,
                committed.source_freshness_budget_ns);
    } else {
      EXPECT_EQ(entry.source_timestamp_ns, prior.source_timestamp_ns);
      EXPECT_EQ(entry.source_id, prior.source_id);
    }
  }
  EXPECT_GT(observed_entries, 0U);
  // Freshness-only: no world, revision or ledger change.
  EXPECT_EQ(publisher->published_collision_world_revision().revision_id,
            world_revision.revision_id);
  EXPECT_EQ(publisher->published_collision_world_revision().content_hash,
            world_revision.content_hash);
  EXPECT_EQ(publisher->recent_ledger().size(), ledger_size);

  const auto current = publisher->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(current->bundle.world, before->world);
  EXPECT_EQ(observed_sidecar_timestamp(current->bundle.body_freshness),
            std::optional<std::int64_t>(5'000'000'000));
  EXPECT_EQ(live_object(*current, kObservedObject).source_timestamp_ns,
            std::optional<std::int64_t>(5'000'000'000));
  EXPECT_EQ(live_object(*current, kObservedObject).source_id,
            std::optional<std::string>(
                "isaac.pose.observer@sha256:" + std::string(64U, 'a')));
  EXPECT_EQ(live_object(*current, kOtherObject).source_timestamp_ns,
            std::optional<std::int64_t>(3'000'000'000));
  // The committed logical scene is unchanged.
  EXPECT_EQ(*current->logical_scene, initial);

  // An already-retained lease is an immutable copy.
  const auto earlier_again =
      publisher->retain_scene_snapshot_lease_inventory(earlier_lease);
  ASSERT_TRUE(earlier_again.has_value());
  EXPECT_EQ(observed_sidecar_timestamp(earlier_again->bundle.body_freshness),
            std::optional<std::int64_t>(4'000'000'000));
  EXPECT_EQ(live_object(*earlier_again, kObservedObject).source_timestamp_ns,
            std::optional<std::int64_t>(4'000'000'000));
  // A lease retained afterwards captures the confirmation.
  const auto later = publisher->retain_scene_snapshot_lease_inventory(
      core::ProjectionLeaseId("operation.after.freshness"));
  ASSERT_TRUE(later.has_value());
  EXPECT_EQ(live_object(*later, kObservedObject).source_timestamp_ns,
            std::optional<std::int64_t>(5'000'000'000));

  // Replays, older sequences, mismatched pose/origin/revision are refused
  // without any publication.
  const auto refused_reason =
      [&](const core::ProviderFreshnessConfirmationDTO &confirmation) {
        const auto result = publisher->publish_freshness(confirmation);
        EXPECT_FALSE(result.ok());
        return result.ok() ? std::string("accepted")
                           : result.error().details.at("reason");
      };
  auto replay = confirmed;
  replay.received_at_ns = 5'500'000'000;
  EXPECT_EQ(refused_reason(freshness_confirmation(revision, 10.0, replay)),
            "provider_freshness_not_newer");
  auto older = committed;
  older.provider_source_sequence = 6U;
  EXPECT_EQ(refused_reason(freshness_confirmation(revision, 10.0, older)),
            "provider_freshness_not_newer");
  auto newer = confirmed;
  newer.provider_source_sequence = 9U;
  newer.received_at_ns = 6'000'000'000;
  EXPECT_EQ(refused_reason(freshness_confirmation(revision, 10.25, newer)),
            "provider_freshness_pose_mismatch");
  auto foreign_epoch = newer;
  foreign_epoch.provider_source_epoch = "sha256:" + std::string(64U, 'f');
  EXPECT_EQ(
      refused_reason(freshness_confirmation(revision, 10.0, foreign_epoch)),
      "provider_freshness_origin_mismatch");
  auto other_budget = newer;
  other_budget.source_freshness_budget_ns = 5'000'000'000;
  EXPECT_EQ(
      refused_reason(freshness_confirmation(revision, 10.0, other_budget)),
      "provider_freshness_origin_mismatch");
  auto stale_revision = revision;
  stale_revision.scene_revision.revision_id = 1U;
  EXPECT_EQ(
      refused_reason(freshness_confirmation(stale_revision, 10.0, newer)),
      "provider_freshness_revision_mismatch");
  auto other_object = freshness_confirmation(revision, 12.0, newer);
  other_object.scene_resource_id =
      core::SceneResourceId(std::string(kOtherObject));
  EXPECT_EQ(refused_reason(other_object),
            "provider_freshness_origin_mismatch");
  EXPECT_EQ(publisher->published_runtime_collision_query_bundle(), after);

  const auto accepted_newer = publisher->publish_freshness(
      freshness_confirmation(revision, 10.0, newer));
  ASSERT_TRUE(accepted_newer.ok()) << accepted_newer.error().message;
  EXPECT_EQ(accepted_newer.value(), after->epoch + 1U);
  EXPECT_EQ(observed_sidecar_timestamp(
                publisher->published_runtime_collision_query_bundle()
                    ->body_freshness),
            std::optional<std::int64_t>(6'000'000'000));

  publisher->release_scene_snapshot_lease_inventory(earlier_lease);
  publisher->release_scene_snapshot_lease_inventory(
      core::ProjectionLeaseId("operation.after.freshness"));
}

TEST(RetainedProductCrossConsumer,
     ProviderFreshnessOverrideFollowsExactCommittedEvidence) {
  ProviderFreshnessFixture fixture;
  const auto committed = freshness_evidence("isaac.pose.observer", 7U,
                                            4'000'000'000);
  const auto other = freshness_evidence("isaac.other.observer", 3U,
                                        3'000'000'000);
  const auto cell = fixture.cell_id();
  std::uint64_t scene_revision = 2U;
  auto scene = make_freshness_scene(
      cell, scene_revision,
      {{kObservedObject, 10.0, committed}, {kOtherObject, 12.0, other}});
  std::int64_t clock_value = 4'500'000'000;
  auto publisher_result = RetainedCollisionWorldPublisher::create(
      *fixture.projector, "retained-lite6-freshness-override", scene,
      RetainedCollisionWorldPublicationContext{
          core::CollisionWorldMutationSource::kDeploymentApply,
          {},
          10,
          {},
          "provider_freshness_initialize",
          std::nullopt},
      [&clock_value] { return clock_value++; });
  ASSERT_TRUE(publisher_result.ok()) << publisher_result.error().message;
  const auto publisher = publisher_result.value();
  const auto commit =
      [&](const std::vector<FreshnessSceneObject> &objects) {
        scene = make_freshness_scene(cell, ++scene_revision, objects);
        const auto published = publisher->publish(
            scene, freshness_commit_context(
                       static_cast<std::int64_t>(scene_revision) * 100));
        ASSERT_TRUE(published.ok()) << published.error().message;
      };
  const auto observed_now = [&] {
    return observed_sidecar_timestamp(
        publisher->published_runtime_collision_query_bundle()->body_freshness);
  };
  const auto observed_live_now = [&] {
    return live_object(*publisher->current_scene_snapshot_lease_inventory(),
                       kObservedObject)
        .source_timestamp_ns;
  };
  const auto confirm = [&](const double x_m, const std::uint64_t sequence,
                           const std::int64_t received_at_ns,
                           const core::SceneObjectProviderEvidenceDTO &base) {
    auto evidence = base;
    evidence.provider_source_sequence = sequence;
    evidence.received_at_ns = received_at_ns;
    return publisher->publish_freshness(
        freshness_confirmation(fixture.revision(scene), x_m, evidence));
  };

  ASSERT_TRUE(confirm(10.0, 8U, 5'000'000'000, committed).ok());
  ASSERT_EQ(observed_now(), std::optional<std::int64_t>(5'000'000'000));

  // An unrelated commit (another object moves) re-applies the override while
  // the observed object's committed evidence and placement are exact.
  commit({{kObservedObject, 10.0, committed}, {kOtherObject, 12.5, other}});
  EXPECT_EQ(observed_now(), std::optional<std::int64_t>(5'000'000'000));
  EXPECT_EQ(observed_live_now(), std::optional<std::int64_t>(5'000'000'000));
  // The override still orders later confirmations.
  const auto replay = confirm(10.0, 8U, 5'100'000'000, committed);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().details.at("reason"),
            "provider_freshness_not_newer");

  // A newer committed evidence for the object discards the override, even
  // when the committed receive time is older than the confirmed one: the
  // owner's committed evidence is authoritative.
  auto newer_committed = committed;
  newer_committed.provider_source_sequence = 9U;
  newer_committed.received_at_ns = 4'500'000'000;
  commit({{kObservedObject, 10.0, newer_committed},
          {kOtherObject, 12.5, other}});
  EXPECT_EQ(observed_now(), std::optional<std::int64_t>(4'500'000'000));
  EXPECT_EQ(observed_live_now(), std::optional<std::int64_t>(4'500'000'000));
  // The next ordering domain is the new committed evidence.
  EXPECT_FALSE(confirm(10.0, 9U, 7'000'000'000, newer_committed).ok());
  ASSERT_TRUE(confirm(10.0, 10U, 7'000'000'000, newer_committed).ok());
  EXPECT_EQ(observed_now(), std::optional<std::int64_t>(7'000'000'000));

  // A committed pose change with otherwise identical evidence discards it.
  commit({{kObservedObject, 10.5, newer_committed},
          {kOtherObject, 12.5, other}});
  EXPECT_EQ(observed_now(), std::optional<std::int64_t>(4'500'000'000));

  // Object removal discards it; re-adding the identical object does not
  // resurrect a stale confirmation.
  ASSERT_TRUE(confirm(10.5, 11U, 8'000'000'000, newer_committed).ok());
  EXPECT_EQ(observed_now(), std::optional<std::int64_t>(8'000'000'000));
  commit({{kOtherObject, 12.5, other}});
  EXPECT_FALSE(observed_now().has_value());
  commit({{kObservedObject, 10.5, newer_committed},
          {kOtherObject, 12.5, other}});
  EXPECT_EQ(observed_now(), std::optional<std::int64_t>(4'500'000'000));
  EXPECT_EQ(observed_live_now(), std::optional<std::int64_t>(4'500'000'000));
}

TEST(RetainedProductCrossConsumer,
     ProviderFreshnessProjectionRequiresExactHealthyCommittedObject) {
  ProviderFreshnessFixture fixture;
  const auto committed = freshness_evidence("isaac.pose.observer", 7U,
                                            4'000'000'000);
  const auto initial = make_freshness_scene(
      fixture.cell_id(), 2U,
      {{kObservedObject, 10.0, committed},
       {kOtherObject, 12.0,
        freshness_evidence("isaac.other.observer", 3U, 3'000'000'000)}});
  const auto revision = fixture.revision(initial);
  std::int64_t clock_value = 4'500'000'000;
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      *fixture.projector, fixture.planning_collision_model,
      core::ProjectionNativeRuntimeBindingDTO{
          core::CapabilityProviderBindingId("lite6_joint_planner_binding"),
          core::CapabilityProviderBindingRevision("planner-binding-revision"),
          core::RuntimeUnitId("native-execution-host"),
          core::RuntimeEpoch("native-execution-host-epoch-1")},
      initial,
      RetainedPlannerSceneProjectionConfig{
          "retained-lite6-freshness-projection", 4U, 8U},
      [&clock_value] { return clock_value++; });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  const auto projection = projection_result.value();
  const auto health_before = projection->ProjectionHealth();
  const auto bundle_before =
      projection->published_runtime_collision_query_bundle();
  ASSERT_NE(bundle_before, nullptr);

  const auto refused_reason =
      [&](const core::ProviderFreshnessConfirmationDTO &confirmation) {
        const auto result = projection->ApplyProviderFreshness(confirmation);
        EXPECT_FALSE(result.ok());
        return result.ok() ? std::string("accepted")
                           : result.error().details.at("reason");
      };
  auto newer = committed;
  newer.provider_source_sequence = 8U;
  newer.received_at_ns = 5'000'000'000;

  auto foreign_revision = revision;
  foreign_revision.scene_revision.content_hash =
      core::ContentDigest("sha256:" + std::string(64U, 'e'));
  EXPECT_EQ(
      refused_reason(freshness_confirmation(foreign_revision, 10.0, newer)),
      "provider_freshness_projection_unavailable");
  auto foreign_graph = revision;
  foreign_graph.graph_revision = core::GraphRevision("another-graph");
  EXPECT_EQ(refused_reason(freshness_confirmation(foreign_graph, 10.0, newer)),
            "provider_freshness_projection_unavailable");
  EXPECT_EQ(refused_reason(freshness_confirmation(revision, 9.0, newer)),
            "provider_freshness_pose_mismatch");
  auto foreign_instance = newer;
  foreign_instance.provider_instance_id =
      core::ProviderInstanceId("another.observer");
  EXPECT_EQ(
      refused_reason(freshness_confirmation(revision, 10.0, foreign_instance)),
      "provider_freshness_origin_mismatch");
  auto foreign_manifest = newer;
  foreign_manifest.provider_manifest_revision =
      core::ProviderManifestRevision("sha256:" + std::string(64U, 'd'));
  EXPECT_EQ(
      refused_reason(freshness_confirmation(revision, 10.0, foreign_manifest)),
      "provider_freshness_origin_mismatch");
  auto older = committed;
  older.provider_source_sequence = 6U;
  EXPECT_EQ(refused_reason(freshness_confirmation(revision, 10.0, older)),
            "provider_freshness_not_newer");
  EXPECT_EQ(refused_reason(freshness_confirmation(revision, 10.0, committed)),
            "provider_freshness_not_newer");
  auto earlier_receipt = newer;
  earlier_receipt.received_at_ns = 3'000'000'000;
  EXPECT_EQ(
      refused_reason(freshness_confirmation(revision, 10.0, earlier_receipt)),
      "provider_freshness_not_newer");
  auto absent = freshness_confirmation(revision, 10.0, newer);
  absent.scene_resource_id = core::SceneResourceId("absent-object");
  EXPECT_EQ(refused_reason(absent), "provider_freshness_object_absent");
  EXPECT_EQ(projection->published_runtime_collision_query_bundle(),
            bundle_before);

  const auto applied = projection->ApplyProviderFreshness(
      freshness_confirmation(revision, 10.0, newer));
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  const auto health_after = projection->ProjectionHealth();
  EXPECT_TRUE(health_after.healthy_for_target(revision));
  EXPECT_EQ(health_after.source_sequence, health_before.source_sequence);
  EXPECT_EQ(health_after.native_projection_generation,
            health_before.native_projection_generation);
  EXPECT_EQ(health_after.projection_content_digest,
            health_before.projection_content_digest);
  EXPECT_EQ(*projection->logical_snapshot(), initial);
  const auto bundle_after =
      projection->published_runtime_collision_query_bundle();
  ASSERT_NE(bundle_after, nullptr);
  EXPECT_EQ(bundle_after->epoch, bundle_before->epoch + 1U);
  EXPECT_EQ(bundle_after->world, bundle_before->world);
  EXPECT_EQ(observed_sidecar_timestamp(bundle_after->body_freshness),
            std::optional<std::int64_t>(5'000'000'000));
  const auto lease = projection->RetainProjection(
      revision, core::ProjectionLeaseId("freshness.projection.lease"));
  ASSERT_TRUE(lease.ok()) << lease.error().message;
  EXPECT_EQ(live_object(lease.value(), kObservedObject).source_timestamp_ns,
            std::optional<std::int64_t>(5'000'000'000));
  projection->ReleaseProjection(
      core::ProjectionLeaseId("freshness.projection.lease"));
}

namespace io_resolver_tests {
using V = deployment::CanonicalValue;
V member(V root, std::string_view name, V value) {
  auto fields = root.as_object();
  for (auto &[key, selected] : fields)
    if (key == name) {
      selected = std::move(value);
      return V(std::move(fields));
    }
  fields.emplace_back(std::string(name), std::move(value));
  return V(std::move(fields));
}

class Fixture final {
public:
  explicit Fixture(bool output = false, bool second = false, bool ros = false) {
    const auto retained =
        deployment::test::materialize_boolean_io_retained_fixture(
            package.path(), !output, output, ros, second);
    bundle = retained.bundle;
    deployment::FilesystemVerifiedRetainedModelMapSource maps(package.path());
    map = maps.get_verified({bundle->retained_bundle_id(),
                             bundle->value()
                                 .compiled_products.symbol_role_maps.front()
                                 .product_id});
    if (!map)
      throw std::logic_error(
          "IO resolver fixture has no real retained model map");
    deployment::FilesystemCatalogArtifactStore catalog(package.path());
    if (output) {
      const auto direct =
          deployment::compile_direct_control_program_binding_inventory(
              *bundle, *map, catalog);
      if (direct.io.size() != 1U)
        throw std::logic_error(
            "IO resolver fixture has no exact direct target");
      direct_binding = direct.io.front();
    }
    auto root = deployment::parse_strict_json_model_yaml(R"({
      "api_version":"manyforge.program.v2","program_id":"resolver_io_authored",
      "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "program_profile":"composable-cell-v1","name":"Resolver IO fixture",
      "catalog":{"api_version":"manyforge.node_catalog.v2","content_version":"0.0.0"},
      "component_roles":{"device":{}},
      "target_roles":{"signals":{"component_role":"device","required_capabilities":[],
        "required_frame_roles":[],"required_resource_roles":[],"required_collision_body_roles":[],"required_model_roles":[]}},
      "blackboard":{"namespaces":[],"keys":[]},
      "tree":{"node_id":"io.check_input","instance_id":"check","parameters":{"target_role":"signals","resource_role":"ready","expected_value":true},"children":[]}
    })")
                    .root();
    root = member(
        root, "catalog",
        member(*root.find_object_member("catalog"), "content_version",
               V(bundle->value().catalogs.node_environment.content_version)));
    auto roles = *root.find_object_member("target_roles");
    auto target = *roles.find_object_member("signals");
    target = member(target, "required_capabilities",
                    V(V::Array{V(V::Object{
                        {"capability_id", V(output ? "io.gpio.set_output"
                                                   : "io.gpio.get_input")},
                        {"parameters", V(V::Object{})},
                        {"disposition", V("required")}})}));
    V::Array resources{V(output ? "lamp" : "ready")};
    if (second)
      resources.emplace_back("lamp_other");
    root = member(root, "target_roles",
                  member(roles, "signals",
                         member(target, "required_resource_roles",
                                V(std::move(resources)))));
    if (output) {
      V::Array children;
      for (const auto &role :
           second ? std::vector<std::string>{"lamp", "lamp_other"}
                  : std::vector<std::string>{"lamp"})
        children.emplace_back(
            V::Object{{"node_id", V("io.set_output")},
                      {"instance_id", V("write." + role)},
                      {"parameters", V(V::Object{{"target_role", V("signals")},
                                                 {"resource_role", V(role)},
                                                 {"value", V(true)}})},
                      {"children", V(V::Array{})}});
      root = member(root, "tree",
                    V(V::Object{{"node_id", V("sequence")},
                                {"instance_id", V("root")},
                                {"parameters", V(V::Object{})},
                                {"children", V(std::move(children))}}));
    }
    root = deployment::normalize_program_authoring(root);
    const auto revision = deployment::detail::compute_content_identity(
        root, {"program-revision", "manyforge.program.v2", "program_revision"});
    root = member(root, "program_revision", V(revision));
    program = deployment::verify_program_binding_contract(
        deployment::canonical_json(root), *bundle);
    authored_binding = deployment::compile_program_binding(
        *program,
        {{{core::ComponentRoleId("device"),
           core::ControllableComponentId("device.io")}},
         {{core::TargetRoleId("signals"), core::CommandTargetId("gpio")}}},
        *bundle, *map, catalog);
    deployment::FilesystemProgramRepository programs(artifacts.path());
    deployment::FilesystemProgramBindingRepository bindings(artifacts.path());
    if (!programs.put_if_absent(program, *bundle) ||
        !bindings.put_if_absent(authored_binding, *bundle, *map))
      throw std::logic_error(
          "IO resolver fixture could not publish exact authored artifacts");
  }

  core::RuntimeParticipantBinding
  Prototype(std::string role, std::uint64_t generation = 3U,
            bool authored = false) const {
    const auto &graph = bundle->value().cell_graph;
    if (graph.control_domains.size() != 1U ||
        !graph.control_domains.front().provider())
      throw std::logic_error(
          "IO resolver fixture has no qualified provider domain");
    const auto &domain = graph.control_domains.front();
    const auto &provider = *domain.provider();
    const auto rows = deployment::ResolveRetainedIoSignalCatalog(graph);
    if (!rows.ok())
      throw std::logic_error(rows.error().message);
    std::vector<core::ResourceId> resources;
    for (const auto &row : rows.value())
      if (row.descriptor.output_resource)
        resources.push_back(*row.descriptor.output_resource);
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()),
                    resources.end());
    const core::RuntimeEpoch epoch("actual.fixture.provider");
    const core::RuntimeProviderDomainConfigurationReadbackDTO closed{
        bundle->retained_bundle_id(),
        graph.graph_revision,
        core::ComponentLifecycleGeneration(generation),
        {domain.control_domain_id, domain.control_domain_revision,
         core::ResolvedProviderDomainScopeDTO{provider, epoch, std::nullopt,
                                              resources}},
        {domain.control_domain_id, domain.control_domain_revision,
         provider.provider_instance_id, provider.provider_manifest_id,
         provider.provider_manifest_revision, provider.provider_runtime_unit_id,
         epoch, provider.lifecycle_plan_catalog_digest},
        false,
        0U,
        0U};
    const auto &identity = authored ? *authored_binding : *direct_binding;
    const auto target = deployment::resolve_runtime_program_target(
        identity, identity.value().target_role_bindings.front().target_role_id,
        core::CapabilityId("io.gpio.set_output"), *bundle, *map);
    auto made = core::verify_runtime_participant_binding(
        deployment::build_runtime_io_output_binding_dto(
            target, core::ResourceRoleId(std::move(role)), *bundle, closed));
    if (!made.ok())
      throw std::logic_error(made.error().message);
    return std::move(made.value());
  }

  core::OperationIntentDTO
  Operation(const core::RuntimeParticipantBinding &prototype, std::string role,
            bool direct) const {
    const auto &value = prototype.value();
    const auto &identity = direct ? *direct_binding : *authored_binding;
    const auto &target = identity.value().target_role_bindings.front();
    const auto schema = core::builtin_io_signal_write_command_schema();
    return {{core::RequestId("request.resolver." + role),
             core::RequestOwnerEpoch("kernel"),
             "test",
             {}},
            identity.value().program_id,
            identity.value().program_revision,
            identity.program_binding_id(),
            core::ProgramRunId("run.resolver"),
            value.graph_revision,
            value.retained_bundle_id,
            std::nullopt,
            {{target.component_role_id,
              target.target_role_id,
              core::CapabilityId("io.gpio.set_output"),
              {},
              core::CapabilityCommandIntentDTO{
                  schema.command_schema_id,
                  schema.command_schema_revision,
                  core::IoSignalWriteCommandValueDTO{core::ResourceRoleId(role),
                                                     true},
                  std::nullopt,
                  {}},
              direct ? std::nullopt
                     : std::optional(core::ProgramNodeExecutionScopeDTO{
                           core::ProgramNodeInstanceId("write." + role),
                           "attempt.1"})}}};
  }

  ScopedTemporaryRoot package, artifacts;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::optional<deployment::VerifiedRetainedModelMap> map;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> authored_binding,
      direct_binding;
};

TEST(RetainedProductConsumerTest,
     IoResolverCapturesQualifiedProviderOnlyProductWithoutExecutionBindings) {
  Fixture fixture;
  ProductBackedRuntimeParticipantResolver resolver(
      fixture.package.path(), fixture.bundle, *fixture.map, {}, {},
      fixture.artifacts.path());
  const auto &value = fixture.authored_binding->value();
  const core::ProgramRunExecutionCaptureDTO request{
      core::RequestOwnerEpoch("kernel"),
      core::RequestOwnerEpoch("runner"),
      core::CellAdmissionClaimGeneration(1U),
      core::ProgramRunId("run"),
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      value.retained_bundle_id,
      value.graph_revision,
      {}};
  const auto capture = resolver.ResolveProgramCapture(request);
  ASSERT_TRUE(capture.ok()) << capture.error().message;
  ASSERT_TRUE(capture.value().table);
  ASSERT_TRUE(capture.value().binding);
  const auto target = resolver.ResolveBoundTarget(
      *fixture.authored_binding, core::TargetRoleId("signals"),
      core::CapabilityId("io.gpio.get_input"));
  ASSERT_TRUE(target.ok()) << target.error().message;
  EXPECT_FALSE(target.value().control_binding());
  EXPECT_FALSE(target.value().control_domain());
  const auto selected = resolver.ResolveOperationTarget(
      value.program_binding_id, core::TargetRoleId("signals"));
  ASSERT_TRUE(selected.ok());
  ASSERT_EQ(selected.value().signal_role_bindings.size(), 1U);
  EXPECT_EQ(selected.value().signal_role_bindings.front().resource_role_id,
            core::ResourceRoleId("ready"));
  auto injected = request;
  injected.binding = capture.value().binding;
  EXPECT_FALSE(resolver.ResolveProgramCapture(std::move(injected)).ok());
}

TEST(RetainedProductConsumerTest,
     IoResolverSelectsActualChannelRoleAndRebindPreservesArtifactRoots) {
  Fixture fixture(true, true);
  auto first = fixture.Prototype("lamp");
  auto other = fixture.Prototype("lamp_other");
  ASSERT_NE(*first.value().io_output_binding->signal.output_resource,
            *other.value().io_output_binding->signal.output_resource);
  ProductBackedRuntimeParticipantResolver empty(
      fixture.package.path(), fixture.bundle, *fixture.map, {}, {},
      fixture.artifacts.path());
  auto current = empty.WithComposedParticipants(
      {first, other}, {fixture.direct_binding->program_binding_id()});
  for (const bool direct : {false, true}) {
    for (const auto &row :
         std::vector<core::RuntimeParticipantBinding>{first, other}) {
      const auto role = row.value().io_output_binding->resource_role_id.value();
      auto request = fixture.Operation(row, role, direct);
      EXPECT_TRUE(current->ValidateExecutionOrigin(request).ok());
      const auto resolved = current->ResolveExecutionParticipant(request, 0U);
      ASSERT_TRUE(resolved.ok()) << resolved.error().message;
      EXPECT_EQ(resolved.value().intrinsic_direct_control, direct);
      EXPECT_EQ(resolved.value().binding.value().io_output_binding,
                row.value().io_output_binding);
      EXPECT_FALSE(resolved.value().binding.value().control_path);
      EXPECT_TRUE(resolved.value().binding.value().resources.empty());
      EXPECT_FALSE(empty.Resolve(request, 0U).ok());
      std::get<core::IoSignalWriteCommandValueDTO>(
          std::get<core::CapabilityCommandIntentDTO>(
              request.participants.front().payload)
              .command_value)
          .resource_role_id = core::ResourceRoleId("missing");
      EXPECT_FALSE(current->Resolve(request, 0U).ok());
    }
  }
  auto request = fixture.Operation(first, "lamp", false);
  auto resealed = empty.WithComposedParticipants(
      {fixture.Prototype("lamp", 7U), fixture.Prototype("lamp_other", 7U)},
      {fixture.direct_binding->program_binding_id()});
  const auto old = current->Resolve(request, 0U);
  const auto newer = resealed->Resolve(request, 0U);
  ASSERT_TRUE(old.ok());
  ASSERT_TRUE(newer.ok());
  EXPECT_EQ(
      old.value().value().io_output_binding->component_lifecycle_generation,
      core::ComponentLifecycleGeneration(3U));
  EXPECT_EQ(
      newer.value().value().io_output_binding->component_lifecycle_generation,
      core::ComponentLifecycleGeneration(7U));
  const auto bound = resealed->ResolveBoundTarget(
      *fixture.authored_binding, core::TargetRoleId("signals"),
      core::CapabilityId("io.gpio.set_output"));
  ASSERT_TRUE(bound.ok());
  EXPECT_EQ(bound.value().program_binding_id(),
            fixture.authored_binding->program_binding_id());
  request.participants.front().execution_scope.reset();
  EXPECT_FALSE(resealed->ValidateExecutionOrigin(request).ok());
}

TEST(RetainedProductConsumerTest,
     IoResolverPrefersExactProgramOutputAliasOverDirectChannelPrototype) {
  Fixture fixture(true, true);
  const auto direct = fixture.Prototype("lamp");
  const auto authored = fixture.Prototype("lamp", 3U, true);
  const auto other_direct = fixture.Prototype("lamp_other");
  const auto other_authored = fixture.Prototype("lamp_other", 3U, true);
  ASSERT_NE(direct.value().program_binding_id,
            authored.value().program_binding_id);
  ASSERT_EQ(direct.value().io_output_binding,
            authored.value().io_output_binding);
  ProductBackedRuntimeParticipantResolver resolver(
      fixture.package.path(), fixture.bundle, *fixture.map,
      {direct, authored, other_direct, other_authored},
      {fixture.direct_binding->program_binding_id()}, fixture.artifacts.path());
  for (const bool intrinsic : {false, true}) {
    for (const auto &row :
         std::vector<core::RuntimeParticipantBinding>{direct, other_direct}) {
      const auto role = row.value().io_output_binding->resource_role_id.value();
      const auto request = fixture.Operation(row, role, intrinsic);
      ASSERT_TRUE(resolver.ValidateExecutionOrigin(request).ok());
      const auto resolved = resolver.ResolveExecutionParticipant(request, 0U);
      ASSERT_TRUE(resolved.ok()) << resolved.error().message;
      EXPECT_EQ(resolved.value().binding.value().program_binding_id,
                request.program_binding_id);
      EXPECT_EQ(resolved.value().binding.value().io_output_binding,
                row.value().io_output_binding);
      EXPECT_EQ(resolved.value().intrinsic_direct_control, intrinsic);
    }
  }
  const auto duplicate =
      resolver.WithComposedParticipants({direct, authored, authored});
  EXPECT_FALSE(
      duplicate->Resolve(fixture.Operation(direct, "lamp", false), 0U).ok());
  EXPECT_TRUE(
      duplicate->Resolve(fixture.Operation(direct, "lamp", true), 0U).ok());
}

TEST(RetainedProductConsumerTest,
     IoResolverRebindRefusesForeignProductAndAmbiguousActualChannel) {
  Fixture fixture(true);
  Fixture foreign(true, false, true);
  const auto first = fixture.Prototype("lamp");
  ProductBackedRuntimeParticipantResolver resolver(
      fixture.package.path(), fixture.bundle, *fixture.map, {first},
      {fixture.direct_binding->program_binding_id()}, fixture.artifacts.path());
  EXPECT_THROW(resolver.WithComposedParticipants({foreign.Prototype("lamp")}),
               std::invalid_argument);
  auto duplicated = resolver.WithComposedParticipants({first, first});
  EXPECT_FALSE(
      duplicated->Resolve(fixture.Operation(first, "lamp", false), 0U).ok());
  EXPECT_FALSE(
      duplicated->Resolve(fixture.Operation(first, "lamp", true), 0U).ok());
  auto wrong = fixture.Operation(first, "lamp", true);
  wrong.program_id = core::ProgramId("invented.direct.identity");
  EXPECT_FALSE(resolver.ValidateExecutionOrigin(wrong).ok());
  EXPECT_FALSE(resolver.Resolve(wrong, 0U).ok());
  EXPECT_FALSE(resolver
                   .ResolveBoundTarget(*foreign.authored_binding,
                                       core::TargetRoleId("signals"),
                                       core::CapabilityId("io.gpio.set_output"))
                   .ok());
}

TEST(RetainedProductConsumerTest,
     SealedCaptureRetainsActualSourceAliasesWithoutAResolver) {
  Fixture fixture;
  const auto &value = fixture.authored_binding->value();
  const core::ProgramRunExecutionCaptureDTO request{
      core::RequestOwnerEpoch("kernel.sealed.capture"),
      core::RequestOwnerEpoch("runner.sealed.capture"),
      core::CellAdmissionClaimGeneration(1U),
      core::ProgramRunId("run.sealed.capture"),
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      value.retained_bundle_id,
      value.graph_revision,
      {}};
  const auto *table = &fixture.program->execution_table();
  const auto *declarations = &fixture.program->blackboard_key_declarations();
  const auto *binding = &fixture.authored_binding->value();
  const std::weak_ptr<const deployment::ProgramBindingContract>
      program_lifetime = fixture.program;
  const std::weak_ptr<const deployment::ProgramBinding> binding_lifetime =
      fixture.authored_binding;
  {
    const auto capture = detail::ResolveSealedProgramCapture(
        request, fixture.program, fixture.authored_binding, *fixture.bundle);
    ASSERT_TRUE(capture.ok()) << capture.error().message;
    ASSERT_TRUE(capture.value().table);
    ASSERT_TRUE(capture.value().binding);
    ASSERT_TRUE(capture.value().blackboard_key_declarations);
    ASSERT_TRUE(capture.value().io_conditions);
    EXPECT_EQ(capture.value().table.get(), table);
    EXPECT_EQ(capture.value().binding.get(), binding);
    EXPECT_EQ(capture.value().blackboard_key_declarations.get(), declarations);
    EXPECT_EQ(capture.value().request_owner_epoch, request.request_owner_epoch);
    EXPECT_EQ(capture.value().program_runner_owner_epoch,
              request.program_runner_owner_epoch);
    EXPECT_EQ(capture.value().claim_generation, request.claim_generation);
    EXPECT_EQ(capture.value().program_run_id, request.program_run_id);
    EXPECT_TRUE(capture.value().io_conditions->program_node_conditions.empty());
    EXPECT_TRUE(capture.value()
                    .io_conditions->referenced_program_blackboard_keys.empty());
    // The aliases, rather than a resolver or another product registry, retain
    // the exact sealed source objects after their original pins are released.
    fixture.program.reset();
    fixture.authored_binding.reset();
    EXPECT_FALSE(program_lifetime.expired());
    EXPECT_FALSE(binding_lifetime.expired());
    EXPECT_EQ(capture.value().table.get(), table);
    EXPECT_EQ(capture.value().binding.get(), binding);
  }
  EXPECT_TRUE(program_lifetime.expired());
  EXPECT_TRUE(binding_lifetime.expired());
}

TEST(RetainedProductConsumerTest,
     SealedCaptureAndResolverShareIdentityAndInjectionRefusals) {
  Fixture fixture;
  ProductBackedRuntimeParticipantResolver resolver(
      fixture.package.path(), fixture.bundle, *fixture.map, {}, {},
      fixture.artifacts.path());
  const auto &value = fixture.authored_binding->value();
  const core::ProgramRunExecutionCaptureDTO request{
      core::RequestOwnerEpoch("kernel.sealed.capture"),
      core::RequestOwnerEpoch("runner.sealed.capture"),
      core::CellAdmissionClaimGeneration(1U),
      core::ProgramRunId("run.sealed.capture"),
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      value.retained_bundle_id,
      value.graph_revision,
      {}};
  const auto direct = detail::ResolveSealedProgramCapture(
      request, fixture.program, fixture.authored_binding, *fixture.bundle);
  const auto delegated = resolver.ResolveProgramCapture(request);
  ASSERT_TRUE(direct.ok()) << direct.error().message;
  ASSERT_TRUE(delegated.ok()) << delegated.error().message;
  EXPECT_EQ(*direct.value().table, *delegated.value().table);
  EXPECT_EQ(*direct.value().binding, *delegated.value().binding);
  EXPECT_EQ(*direct.value().blackboard_key_declarations,
            *delegated.value().blackboard_key_declarations);
  EXPECT_EQ(direct.value().io_conditions->program_node_conditions.size(),
            delegated.value().io_conditions->program_node_conditions.size());
  EXPECT_EQ(
      direct.value().io_conditions->referenced_program_blackboard_keys,
      delegated.value().io_conditions->referenced_program_blackboard_keys);
  std::vector<core::ProgramRunExecutionCaptureDTO> injected(4U, request);
  injected[0].table = direct.value().table;
  injected[1].binding = direct.value().binding;
  injected[2].blackboard_key_declarations =
      direct.value().blackboard_key_declarations;
  injected[3].io_conditions = direct.value().io_conditions;
  for (const auto &candidate : injected) {
    const auto pure = detail::ResolveSealedProgramCapture(
        candidate, fixture.program, fixture.authored_binding, *fixture.bundle);
    const auto loaded = resolver.ResolveProgramCapture(candidate);
    ASSERT_FALSE(pure.ok());
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(pure.error().code, loaded.error().code);
    EXPECT_EQ(pure.error().message, loaded.error().message);
    EXPECT_EQ(pure.error().details, loaded.error().details);
    EXPECT_EQ(pure.error().details.at("reason"),
              "program_capture_product_mismatch");
  }
  auto foreign = request;
  foreign.graph_revision = core::GraphRevision("foreign.graph");
  const auto bad_product = detail::ResolveSealedProgramCapture(
      foreign, fixture.program, fixture.authored_binding, *fixture.bundle);
  ASSERT_FALSE(bad_product.ok());
  EXPECT_EQ(bad_product.error().details.at("reason"),
            "program_capture_product_mismatch");
  auto wrong_binding = request;
  wrong_binding.program_binding_id = core::ProgramBindingId("foreign.binding");
  const auto bad_binding = detail::ResolveSealedProgramCapture(
      wrong_binding, fixture.program, fixture.authored_binding,
      *fixture.bundle);
  ASSERT_FALSE(bad_binding.ok());
  EXPECT_EQ(bad_binding.error().details.at("reason"),
            "program_capture_binding_unavailable");
  const auto missing_binding = detail::ResolveSealedProgramCapture(
      request, fixture.program, nullptr, *fixture.bundle);
  ASSERT_FALSE(missing_binding.ok());
  EXPECT_EQ(missing_binding.error().details.at("reason"),
            "program_capture_binding_unavailable");
  const auto missing_program = detail::ResolveSealedProgramCapture(
      request, nullptr, fixture.authored_binding, *fixture.bundle);
  ASSERT_FALSE(missing_program.ok());
  EXPECT_EQ(missing_program.error().details.at("reason"),
            "program_capture_source_unavailable");
  Fixture other(true);
  const auto wrong_program = detail::ResolveSealedProgramCapture(
      request, other.program, fixture.authored_binding, *fixture.bundle);
  ASSERT_FALSE(wrong_program.ok());
  EXPECT_EQ(wrong_program.error().details.at("reason"),
            "program_capture_source_unavailable");
}

} // namespace io_resolver_tests

} // namespace
} // namespace manyforge::roboplan_backend
