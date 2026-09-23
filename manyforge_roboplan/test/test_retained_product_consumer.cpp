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
#include <iostream>
#include <iterator>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retained_runtime_policy.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
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

#include "content_identity_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"
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

  core::Result<std::uint64_t>
  execute(const core::ExecuteRequestDTO &request) override {
    return Require()->execute(request);
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
  core::PauseResultDTO pause() override { return Require()->pause(); }
  core::ResumeResultDTO resume(
      std::optional<std::uint64_t> expected_epoch = std::nullopt) override {
    return Require()->resume(expected_epoch);
  }
  core::ResetResultDTO reset(
      std::optional<std::uint64_t> expected_epoch = std::nullopt) override {
    return Require()->reset(expected_epoch);
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
    std::optional<core::RuntimeEpoch> provider_runtime_epoch = std::nullopt) {
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
               candidate.command_target_id == target_id;
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
            if (frame.controllable_component_id != component_id ||
                frame.frame_role_id != resolution.frame_role_id) {
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
              core::FrameRoleId("tcp"),
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
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
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
  const deployment::FilesystemVerifiedCatalogArtifactSource empty_catalog(
      package_root);
  EXPECT_FALSE(deployment::resolve_retained_stop_deadline_budget(
                   admission.retained_bundle(), empty_catalog)
                   .has_value());
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
      deployment::test::materialize_lite6_retained_consumer_fixture(
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
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
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
      materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
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
      core::StateAuthorityClass::kPredicted, std::nullopt, local_runtime_epoch);
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id,
      core::StateAuthorityClass::kPredicted, std::nullopt, local_runtime_epoch);

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
          core::ObservedStateSourceKind::kPredictedState,
          "test/direct-owner/arm", 0, 1U});
  auto gripper_channel = std::make_shared<RetainedObservedStateChannel>(
      gripper_binding, state_projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kPredictedState,
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
      {arm_channel->query_context(),
       core::ObservedStateSourceKind::kPredictedState, arm_provider},
      {gripper_channel->query_context(),
       core::ObservedStateSourceKind::kPredictedState, gripper_provider}};
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
     BootstrapStopEnvelopeFollowsDeclaredCapabilityAndExactArtifacts) {
  ScopedTemporaryRoot temporary;
  const auto passive_root = temporary.path() / "not-stoppable";
  ASSERT_TRUE(std::filesystem::create_directory(passive_root));
  const auto passive =
      deployment::test::materialize_lite6_retained_consumer_fixture(
          passive_root);
  const deployment::FilesystemVerifiedCatalogArtifactSource passive_catalog(
      passive_root);
  EXPECT_FALSE(deployment::resolve_retained_stop_deadline_budget(
      *passive.bundle, passive_catalog));

  const auto motion_root = temporary.path() / "stoppable";
  ASSERT_TRUE(std::filesystem::create_directory(motion_root));
  const auto motion = deployment::test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          motion_root);
  const deployment::FilesystemVerifiedCatalogArtifactSource motion_catalog(
      motion_root);
  const auto budget = deployment::resolve_retained_stop_deadline_budget(
      *motion.bundle, motion_catalog);
  ASSERT_TRUE(budget);
  EXPECT_GT(budget->outer_duration_ns, budget->kernel_completion_reserve_ns);
  EXPECT_GT(budget->kernel_completion_reserve_ns, 0);
  EXPECT_TRUE(budget->phases.empty());
  class MissingCatalog final : public deployment::ICatalogArtifactSource {
    std::optional<std::string>
    get_verified(const core::ContentAddressedStoreKey &,
                 const core::ContentDigest &,
                 std::optional<std::uint64_t>) const override {
      return std::nullopt;
    }
  } missing;
  EXPECT_THROW(deployment::resolve_retained_stop_deadline_budget(*motion.bundle,
                                                                 missing),
               deployment::RetainedRuntimePolicyError);
  // Even a non-stoppable capability must prove its exact policy, not treat
  // missing/tampered artifacts as an empty or passive product.
  EXPECT_THROW(deployment::resolve_retained_stop_deadline_budget(
                   *passive.bundle, missing),
               deployment::RetainedRuntimePolicyError);
}

TEST(RetainedProductCrossConsumer,
     ProductSelectedStateConfigDoesNotEvaluateNativePredictedFallback) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));

  const auto materialized = deployment::test::
      materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
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
  const auto predicted_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, target_id,
      core::StateAuthorityClass::kPredicted, std::nullopt, runtime_epoch);

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
  const auto native_default = detail::select_retained_state_channel_config(
      std::nullopt, predicted_binding);
  EXPECT_EQ(native_default.source_kind,
            core::ObservedStateSourceKind::kPredictedState);
  EXPECT_EQ(native_default.authority_generation, 1U);
}

TEST(RetainedProductCrossConsumer,
     PredictedLite6AggregateComposesButUnqualifiedParticipantsStayClosed) {
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
      materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
          package_root);
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
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto &graph = materialized.bundle->value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  const core::RuntimeEpoch local_runtime_epoch(
      "epoch." + graph.runtime_units.front().runtime_unit_id.value());
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id,
      core::StateAuthorityClass::kPredicted, std::nullopt, local_runtime_epoch);
  const core::ProgramCommandSchemaRefDTO command_schema{
      core::CommandSchemaId(std::string(core::kGripperPositionCommandSchemaId)),
      core::CommandSchemaRevision(
          std::string(core::kGripperPositionCommandSchemaRevision))};
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id,
      core::StateAuthorityClass::kPredicted, command_schema,
      local_runtime_epoch);

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
  const auto initial_scene = make_projection_runtime_shadow_snapshot(
      materialized.bundle->value().cell_graph.cell_id);
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
      RoboPlanExecutionBackendConfig{},
      SimpleSchedulerConfig{},
      10'000'000,
      {0.0, 0.0, 1.57, 0.0, 0.0, 0.0},
      core::StopDeadlineBudgetDTO{1'000'000'000, 100'000'000, {}},
      kMaintainedNativeStopSurveillancePolicy,
      {},
      {},
      {},
      {}};
  ManagedKernelAdmissionGate admission_gate;
  deployment::FilesystemCatalogArtifactStore runtime_policy_store(package_root);
  const auto runtime_policy = deployment::resolve_retained_runtime_policy(
      *materialized.bundle, runtime_policy_store);
  ASSERT_TRUE(runtime_policy.has_value());
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
      runtime_policy->value(),
      runtime_policy->reference(),
      {},
      {},
      {},
      {}};
  runtime_input.deployment_maximum_tcp_linear_speed_mps = 0.15;
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
  auto runtime = create_retained_native_runtime(std::move(runtime_input),
                                                std::move(runtime_config));

  ASSERT_NE(runtime, nullptr);
  ASSERT_NE(runtime->addressed_kernel, nullptr);
  ASSERT_NE(runtime->participant_engine, nullptr);
  ASSERT_TRUE(runtime->gripper_binding.has_value());
  const auto motion_registration = std::find_if(
      runtime->participant_registrations.begin(),
      runtime->participant_registrations.end(), [](const auto &registration) {
        return registration.binding.value().command_target_id ==
               core::CommandTargetId(
                   std::string(deployment::test::kLite6ArmTargetId));
      });
  ASSERT_NE(motion_registration, runtime->participant_registrations.end());
  ASSERT_TRUE(motion_registration->runtime_policy.has_value());
  EXPECT_EQ(motion_registration->runtime_policy->value().policy,
            runtime_policy->value());
  ASSERT_TRUE(
      motion_registration->deployment_maximum_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(
      *motion_registration->deployment_maximum_tcp_linear_speed_mps, 0.15);
  EXPECT_EQ(motion_registration->runtime_policy->value().source_artifact,
            runtime_policy->reference());
  ASSERT_TRUE(
      motion_registration->runtime_policy->value().ordinary_motion_jerk);
  EXPECT_EQ(*motion_registration->runtime_policy->value().ordinary_motion_jerk,
            jerk_profile);
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
  const auto joint_names = runtime->kinematics->group_joint_names(
      motion_value.command_target_id.value());
  ASSERT_TRUE(joint_names.ok()) << joint_names.error().message;
  std::vector<std::string> expected_joint_names;
  for (const auto &resource : motion_value.resources) {
    expected_joint_names.push_back(resource.compiled_resource_name);
  }
  EXPECT_EQ(joint_names.value(), expected_joint_names);
  const auto limits = runtime->kinematics->joint_limits(
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
  const auto tcp_pose = runtime->kinematics->forward_kinematics(
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
  const auto validation = runtime->trajectory_validator->validate_non_collision(
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
      runtime->trajectory_validator->validate_non_collision(
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
  auto over_speed_trajectory = stationary_trajectory;
  over_speed_trajectory.waypoints.back().time_from_start_s = 0.1;
  over_speed_trajectory.waypoints.back().positions_rad.front() = 1.0;
  const auto over_speed_validation =
      runtime->trajectory_validator->validate_non_collision(
          over_speed_trajectory, core::ValidationOptionsDTO{0.0, {}, {}, false},
          cancellation);
  ASSERT_FALSE(over_speed_validation.valid);
  ASSERT_TRUE(over_speed_validation.error.has_value());
  EXPECT_EQ(over_speed_validation.error->details.at("reason"),
            "trajectory_segment_velocity_limit_exceeded");
  EXPECT_NEAR(std::stod(over_speed_validation.error->details.at("limit")), 2.14,
              1e-12);
  const auto expanded =
      runtime->collision_trajectory_projector->expand_for_collision(
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
      materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
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
    EXPECT_TRUE(runtime_unit_ids
                    .insert(physical.value()
                                .control_path->provider_runtime_unit_id)
                    .second);
    for (const auto &resource : physical.value().resources) {
      ++physical_resource_count;
      EXPECT_TRUE(physical_resources.insert(resource.resource_id).second)
          << resource.resource_id.value();
    }
    physical_bindings.push_back(std::move(physical));
  }
  ASSERT_EQ(component_ids.size(), 2U);
  ASSERT_EQ(runtime_unit_ids.size(), 2U);
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
        RoboPlanExecutionBackendConfig{},
        SimpleSchedulerConfig{},
        10'000'000,
        std::vector<double>(physical.value().resources.size(), 0.0),
        core::StopDeadlineBudgetDTO{1'000'000'000, 100'000'000, {}},
        kMaintainedNativeStopSurveillancePolicy,
        {},
        {},
        {},
        {}};
    runtime_config.kernel.runtime_collision_stop_owned_by_addressed_kernel =
        true;
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
        {},
        {},
        {}};
    runtime_input.deployment_maximum_tcp_linear_speed_mps = 0.15;

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
        auto registry = std::make_shared<core::RuntimeParticipantRegistry>(
            std::move(joined_bindings));
        return [registry](const core::OperationIntentDTO &operation,
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
      primary, {peer}, std::move(joined_resolver_factory));
  attach_retained_native_addressed_owner(
      primary,
      RetainedNativeAddressedOwnerConfig{
          core::RequestOwnerEpoch("i6.joined.owner.epoch.1"),
          bundle.value().cell_graph.cell_id,
          temporary.path() / "joined-operation-journal.sqlite3",
          1024U,
          nullptr,
          bundle.value().cell_graph.graph_revision,
          core::StopDeadlineBudgetDTO{1'000'000'000, 100'000'000, {}},
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

  std::weak_ptr<RetainedNativeRuntime> primary_runtime_lifetime = primary;
  std::weak_ptr<RetainedNativeRuntime> peer_runtime_lifetime = peer;
  primary_subscription.reset();
  peer_subscription.reset();
  retained_primary_engine.reset();
  retained_peer_engine.reset();
  peer.reset();
  ASSERT_EQ(primary.use_count(), 1U);
  ASSERT_FALSE(peer_runtime_lifetime.expired());
  ASSERT_FALSE(primary_subscription_lifetime.expired());
  ASSERT_FALSE(peer_subscription_lifetime.expired());
  ASSERT_FALSE(primary_engine_lifetime.expired());
  ASSERT_FALSE(peer_engine_lifetime.expired());

  primary.reset();

  EXPECT_TRUE(primary_runtime_lifetime.expired());
  EXPECT_TRUE(peer_runtime_lifetime.expired());
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
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
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
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
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
      deployment::test::materialize_lite6_native_cclc_product_retained_consumer_fixture(
          package_root, kZeroDigest, kZeroDigest, true);
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
      {"capability_id":"tool.gripper.confirmation.operator","parameters":{},"disposition":"required"},
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
            {"runtime_mutability", CanonicalValue("immutable")},
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
  for (const auto* capability : {"tool.gripper.feedback", "tool.gripper.confirmation.operator"}) {
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
  EXPECT_EQ(*capture.value().binding, selected_program);
  // Identity-only ingress cannot supply a table or a pre-resolved Binding.
  EXPECT_FALSE(resolver.ResolveProgramCapture(capture.value()).ok());
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
        const auto& representative = representatives[alternate ? index % 2 : 0];
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
     TwoSameTypeProgramsResolveDistinctPhysicalParticipantsWithoutAliasing) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));

  constexpr std::string_view cell_id = "i6_two_lite6_program_binding_cell";
  const auto materialized = deployment::test::
      materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
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
    EXPECT_TRUE(runtime_unit_ids
                    .insert(value.runtime_unit_epochs.front().runtime_unit_id)
                    .second);
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
  EXPECT_EQ(runtime_unit_ids.size(), 2U);
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
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  const auto candidate = deployment::test::
      materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
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
            "native_projection_candidate_model_not_prepared");
  EXPECT_EQ(projection->snapshot(), predecessor_world);

  const auto prepared = projection->PrepareCandidateProjection(
      std::move(candidate_projector), request.planning_collision_model);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  const auto rebuilt = projection->RebuildProjection(request);
  ASSERT_TRUE(rebuilt.presence_valid());
  EXPECT_TRUE(
      projection->ProjectionHealth().healthy_for_target(candidate_revision));
  EXPECT_EQ(projection->collision_shared_model(), candidate_shared_model);
  EXPECT_NE(projection->snapshot(), predecessor_world);
  EXPECT_NE(projection->snapshot()->publisher_id,
            predecessor_world->publisher_id);
  const auto candidate_lease = projection->RetainProjection(
      candidate_revision,
      core::ProjectionLeaseId("command-fenced-candidate-lease"));
  ASSERT_TRUE(candidate_lease.ok()) << candidate_lease.error().message;
  const auto candidate_frame =
      projection->ResolveSceneFrame(core::SceneFrameResolutionQueryDTO{
          core::CellSceneFrameRefDTO{core::CellId(std::string(cell_id))},
          candidate_lease.value().projection_binding});
  ASSERT_FALSE(candidate_frame.ok());
  EXPECT_EQ(candidate_frame.error().details.at("reason"),
            "native_scene_frame_lease_unavailable");
  projection->ReleaseProjection(
      candidate_lease.value().projection_binding.projection_lease_id);
}

} // namespace
} // namespace manyforge::roboplan_backend
