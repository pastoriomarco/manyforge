#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <manyforge_core/pick_place_candidate.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr char kDigest3[] =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr char kDigest4[] =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";
constexpr char kDigest5[] =
    "sha256:5555555555555555555555555555555555555555555555555555555555555555";

[[nodiscard]] QuaternionDTO yaw_quaternion(const double radians) {
  return {0.0, 0.0, std::sin(radians / 2.0), std::cos(radians / 2.0)};
}

[[nodiscard]] SceneObjectSnapshotDTO box_object(const Vector3DTO dimensions = {0.2, 0.3, 0.4}) {
  return {SceneResourceId("workpiece"),
          {BoxShapeDTO{dimensions}},
          CellFixedSceneObjectPlacementDTO{CellId("cell:test"), Pose3DTO{}},
          std::nullopt,
          {}};
}

[[nodiscard]] PickCandidateSynthesisConfigDTO config() {
  PickCandidateSynthesisConfigDTO value;
  value.translation_step_m = 0.01;
  value.rotation_step_deg = 15.0;
  value.maximum_candidate_count = 128U;
  return value;
}

[[nodiscard]] CollisionWorldLeaseInventory predecessor_projection() {
  auto binding = testing::make_test_projection_binding();
  auto logical_scene = *testing::make_test_logical_scene(binding);
  logical_scene.objects.push_back(box_object());
  auto world = std::make_shared<CollisionWorldSnapshot>();
  world->publisher_id = "pick-candidate-test";
  world->revision = {1U, kDigest3};
  return {binding,
          std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene)),
          10,
          {std::move(world),
           testing::make_test_logical_scene(binding)->configured_collision_policy}};
}

[[nodiscard]] CollisionWorldLeaseInventory successor_projection(
    CollisionWorldLeaseInventory predecessor) {
  auto revision = predecessor.projection_binding.projection_revision;
  ++revision.scene_revision.revision_id;
  revision.scene_revision.content_hash = ContentDigest(kDigest5);
  predecessor.projection_binding.projection_revision = revision;
  auto logical_scene = *predecessor.logical_scene;
  logical_scene.revision = revision.scene_revision;
  predecessor.logical_scene =
      std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene));
  auto world = std::make_shared<CollisionWorldSnapshot>(*predecessor.bundle.world);
  world->revision = {2U, kDigest5};
  predecessor.bundle.world = std::move(world);
  return predecessor;
}

[[nodiscard]] CollisionWorldLeaseInventory carried_predecessor_projection() {
  auto predecessor = predecessor_projection();
  auto logical_scene = *predecessor.logical_scene;
  const CarriedObjectAttachmentId attachment_id("attachment:workpiece");
  const CarriedObjectAttachmentRevision attachment_revision("attachment-revision:1");
  logical_scene.objects.front().placement =
      CarriedSceneObjectPlacementDTO{attachment_id, attachment_revision};
  logical_scene.carried_object_attachments = {CarriedObjectAttachmentDTO{
      attachment_id,
      attachment_revision,
      SceneResourceId("workpiece"),
      ControllableComponentId("component:test"),
      CommandTargetId("target:test"),
      ComponentSceneFrameRefDTO{ControllableComponentId("component:test"),
                                ModelInstanceId("model:test"), FrameRoleId("frame-role:tcp")},
      Pose3DTO{{0.0, 0.0, 0.2}, {}},
      {CollisionBodyRoleId("collision-body-role:tool")}}};
  predecessor.logical_scene =
      std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene));
  return predecessor;
}

[[nodiscard]] RuntimeParticipantBindingDTO arm_runtime_binding() {
  std::vector<ProgramFrameRoleBindingDTO> frames{
      ProgramFrameRoleBindingDTO{FrameRoleId("frame-role:base"), ModelInstanceId("model:test"),
                                 ModelRevision("model-revision:test"), "base"},
      ProgramFrameRoleBindingDTO{FrameRoleId("frame-role:tcp"), ModelInstanceId("model:test"),
                                 ModelRevision("model-revision:test"), "tcp"}};
  std::vector<ProgramCollisionBodyRoleBindingDTO> collision_bodies{
      ProgramCollisionBodyRoleBindingDTO{CollisionBodyRoleId("collision-body-role:tool"),
                                         ModelInstanceId("model:test"),
                                         ModelRevision("model-revision:test"),
                                         {"tcp"}}};
  std::vector<ProgramContactGraspBindingDTO> contact_grasps{ProgramContactGraspBindingDTO{
      ContactPolicyId("pick.contact"),
      ContactPolicyRevision("contact-policy-revision:1"),
      RuntimeObjectId("selected_object"),
      {ProgramContactGraspBodyBindingDTO{ComponentRoleId("component-role:tool"),
                                         TargetRoleId("target-role:tool"),
                                         ControllableComponentId("component:test"),
                                         CommandTargetId("target:test"),
                                         ModelRoleId("tool"),
                                         CollisionBodyRoleId("collision-body-role:tool"),
                                         ModelInstanceId("model:test"),
                                         ModelRevision("model-revision:test"),
                                         {CollisionBodyId("body:tool")}}},
      ContactGraspEnvelopeDTO{0.001, 0.8}}};
  auto value = RuntimeParticipantBindingDTO{ProgramId("program:test"),
                                      ProgramRevision("program-revision:test"),
                                      ProgramBindingId("program-binding:test"),
                                      RetainedBundleId("bundle:test"),
                                      GraphRevision("graph:test"),
                                      ComponentRoleId("component-role:arm"),
                                      TargetRoleId("target-role:test"),
                                      ControllableComponentId("component:test"),
                                      ComponentTypeId("component-type:test"),
                                      ComponentTypeRevision("component-type-revision:test"),
                                      CommandTargetId("target:test"),
                                      CapabilityId("execution.trajectory.execute"),
                                      CapabilityDefinitionRevision("capability-revision:test"),
                                      std::nullopt,
                                      {},
                                      {},
                                      std::move(frames),
                                      {},
                                      std::move(collision_bodies),
                                      std::nullopt,
                                      std::nullopt,
                                      {},
                                      {},
                                      std::move(contact_grasps)};
  value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
      value.target_role_id, value.command_target_id, value.frame_bindings.back(), {}};
  return value;
}

[[nodiscard]] RuntimeParticipantBindingDTO carrier_runtime_binding() {
  auto value = arm_runtime_binding();
  value.component_role_id = ComponentRoleId("component-role:tool");
  value.target_role_id = TargetRoleId("target-role:tool");
  value.capability_id = CapabilityId("tool.gripper.command");
  value.motion_endpoint.reset();
  return value;
}

class FakeMotionPreview final : public IAddressedMotionPreviewSession {
 public:
  explicit FakeMotionPreview(const bool fail_first = false,
                             std::size_t* reference_resolution_count = nullptr,
                             const RuntimeParticipantBindingDTO* motion_binding = nullptr,
                             const RuntimeParticipantBindingDTO* carrier_binding = nullptr)
      : fail_first_with_start_collision(fail_first),
        reference_resolution_count_(reference_resolution_count),
        motion_binding_(motion_binding),
        carrier_binding_(carrier_binding) {}

  Result<RuntimeParticipantBindingDTO> resolve_binding(
      const OperationIntentDTO& operation) const override {
    if (operation.participants.size() != 1U) {
      return Result<RuntimeParticipantBindingDTO>::Err(
          ErrorDTO{"invalid_request", "fixture requires one participant", {}});
    }
    return Result<RuntimeParticipantBindingDTO>::Ok(
        std::holds_alternative<CapabilityCommandIntentDTO>(operation.participants.front().payload)
            ? (carrier_binding_ ? *carrier_binding_ : carrier_runtime_binding())
            : (motion_binding_ ? *motion_binding_ : arm_runtime_binding()));
  }

  Result<ExecuteRequestDTO> prepare_motion(const OperationIntentDTO& operation) override {
    if (operation.participants.size() != 1U) {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture requires one participant", {}});
    }
    const auto* motion =
        std::get_if<MotionParticipantIntentDTO>(&operation.participants.front().payload);
    if (motion == nullptr) {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture requires motion", {}});
    }
    auto request =
        testing::make_test_execute_request({"joint1", "joint2"}, operation.program_run_id.value());
    const auto binding = motion_binding_ ? *motion_binding_ : arm_runtime_binding();
    if (!binding.motion_endpoint) {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture motion endpoint capture is absent", {}});
    }
    {
      const auto& endpoint = *binding.motion_endpoint;
      if (!tcp_selector_admits(motion->tcp_endpoint, endpoint, std::nullopt))
        return Result<ExecuteRequestDTO>::Err(
            ErrorDTO{"invalid_request", "fixture endpoint capture differs from motion", {}});
      request.binding.tcp_frame = ResolvedComponentModelFrameDTO{
          binding.controllable_component_id, endpoint.frame.model_instance_id,
          endpoint.frame.frame_role_id, endpoint.frame.compiled_frame_name,
          endpoint.frame.model_revision};
    }
    request.move_id = motion->move_id.value();
    request.tolerances = motion->tolerances;
    request.speed_profile = motion->speed_profile;
    request.timeouts = motion->timeouts;
    request.cache_mode = motion->cache_mode;
    request.planner = motion->planner;
    request.target_mode = motion->target_mode;
    request.target_reference = motion->target_reference;
    request.offset_reference = motion->offset_reference;
    request.contact_policy_id =
        motion->contact_policy_id.has_value() ? motion->contact_policy_id->value() : std::string{};
    request.runtime_object_bindings = motion->runtime_object_bindings;
    if (const auto* pose = std::get_if<PoseGoalDTO>(&motion->motion)) {
      request.motion = *pose;
    } else if (const auto* path = std::get_if<CartesianPathDTO>(&motion->motion)) {
      request.motion = *path;
    } else {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture motion is unsupported", {}});
    }
    if (motion->contact_policy_id.has_value() && !motion->runtime_object_bindings.empty()) {
      request.resolved_contact_grasp =
          ResolvedContactGraspBindingDTO{*motion->contact_policy_id,
                                         ContactPolicyRevision("contact-policy-revision:1"),
                                         motion->runtime_object_bindings.front().runtime_object_id,
                                         motion->runtime_object_bindings.front().scene_resource_id,
                                         {CollisionBodyId("body:tool")},
                                         ContactGraspEnvelopeDTO{0.001, 0.8}};
    }
    return Result<ExecuteRequestDTO>::Ok(std::move(request));
  }

  Result<RobotStateDTO> current_state() const override {
    return Result<RobotStateDTO>::Ok(RobotStateDTO{{"joint1", "joint2"}, {0.0, 0.0}, {}});
  }

  Result<std::vector<Pose3DTO>> resolve_reference_poses(
      const OperationIntentDTO&, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory&, const RobotStateDTO&,
      std::vector<RuntimeObjectReferenceBindingDTO>) override {
    if (reference_resolution_count_ != nullptr) {
      ++*reference_resolution_count_;
    }
    std::vector<Pose3DTO> resolved;
    resolved.reserve(references.size());
    for (const auto& reference : references) {
      resolved.push_back(reference.pose.value_or(
          Pose3DTO{{}, QuaternionDTO{0.0, 0.0, 0.7071067811865475, 0.7071067811865476}}));
    }
    return Result<std::vector<Pose3DTO>>::Ok(std::move(resolved));
  }

  Result<MotionPreviewResultDTO> preview_motion(OperationIntentDTO operation,
                                                CollisionWorldLeaseInventory projection_inventory,
                                                RobotStateDTO start_state,
                                                const std::uint32_t planning_attempt_index,
                                                std::stop_token) override {
    if (!operation.expected_scene_revision.has_value() ||
        *operation.expected_scene_revision !=
            projection_inventory.projection_binding.projection_revision.scene_revision) {
      return Result<MotionPreviewResultDTO>::Err(
          ErrorDTO{"scene_changed_during_validation",
                   "fixture preview intent does not name its exact projection",
                   {{"reason", "motion_preview_scene_revision_mismatch"}}});
    }
    auto prepared = prepare_motion(operation);
    if (!prepared.ok()) {
      return Result<MotionPreviewResultDTO>::Err(prepared.error());
    }
    auto request = std::move(prepared.value());
    requests.push_back(request);
    start_states.push_back(start_state);
    projection_bindings.push_back(projection_inventory.projection_binding);
    expected_scene_revisions.push_back(*operation.expected_scene_revision);
    planning_attempt_indices.push_back(planning_attempt_index);
    if (fail_first_with_start_collision && requests.size() == 1U) {
      return Result<MotionPreviewResultDTO>::Err(ErrorDTO{
          "collision", "fixture start state is in collision", {{"failure_stage", "start_state"}}});
    }

    std::vector<double> final_positions(start_state.positions_rad.size(),
                                        static_cast<double>(requests.size()));
    RobotStateDTO final_state{start_state.joint_names, final_positions, {}};
    TrajectoryDTO trajectory{request.binding.resources,
                             start_state.joint_names,
                             {{0.0, start_state.positions_rad, {}, {}},
                              {static_cast<double>(requests.size()), final_positions, {}, {}}}};
    return Result<MotionPreviewResultDTO>::Ok(MotionPreviewResultDTO{
        std::move(trajectory), std::move(start_state), final_state,
        testing::make_test_controlled_state(request.binding, final_positions),
        Pose3DTO{{0.0, 0.0, static_cast<double>(requests.size())}, {}},
        projection_inventory.projection_binding, projection_inventory.bundle.world->revision, 0U});
  }

  bool fail_first_with_start_collision{};
  std::vector<ExecuteRequestDTO> requests;
  std::vector<RobotStateDTO> start_states;
  std::vector<ProjectionLeaseBindingDTO> projection_bindings;
  std::vector<SceneRevision> expected_scene_revisions;
  std::vector<std::uint32_t> planning_attempt_indices;

 private:
  std::size_t* reference_resolution_count_{};
  const RuntimeParticipantBindingDTO* motion_binding_{};
  const RuntimeParticipantBindingDTO* carrier_binding_{};
};

class FakeMotionPreviewOwner final : public IAddressedMotionPreviewOwner {
 public:
  Result<std::unique_ptr<IAddressedMotionPreviewSession>> begin_motion_preview_session(
      const OperationIntentDTO&) override {
    ++begin_count;
    std::unique_ptr<IAddressedMotionPreviewSession> session = std::make_unique<FakeMotionPreview>(
        fail_first_with_start_collision, &reference_resolution_count,
        motion_binding ? &*motion_binding : nullptr, carrier_binding ? &*carrier_binding : nullptr);
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Ok(std::move(session));
  }

  std::size_t begin_count{};
  bool fail_first_with_start_collision{};
  std::size_t reference_resolution_count{};
  std::optional<RuntimeParticipantBindingDTO> motion_binding;
  std::optional<RuntimeParticipantBindingDTO> carrier_binding;
};

[[nodiscard]] OperationIntentDTO motion_operation(
    std::string request_id, std::string move_id, MotionSpecDTO motion,
    const std::optional<ContactPolicyId>& contact_policy = std::nullopt,
    std::vector<RuntimeObjectReferenceBindingDTO> runtime_objects = {}) {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch("kernel.epoch:test"), "pick_candidate_test",
                              std::nullopt},
      ProgramId("program:test"),
      ProgramRevision("program-revision:test"),
      ProgramBindingId("program-binding:test"),
      ProgramRunId("run:test"),
      GraphRevision("graph:test"),
      RetainedBundleId("bundle:test"),
      testing::make_test_projection_binding().projection_revision.scene_revision,
      {OperationParticipantIntentDTO{ComponentRoleId("component-role:arm"),
                                     TargetRoleId("target-role:test"),
                                     CapabilityId("execution.trajectory.execute"), std::nullopt,
                                     MotionParticipantIntentDTO{MoveId(std::move(move_id)),
                                                                FrameRoleId("frame-role:base"),
                                                                CurrentTcpDTO{},
                                                                TolerancesDTO{},
                                                                SpeedProfileDTO{},
                                                                TimeoutsDTO{1.0, 1.0},
                                                                CacheMode::kDefault,
                                                                PlannerOverrideDTO{},
                                                                MotionTargetModeDTO::kAbsolute,
                                                                std::nullopt,
                                                                std::nullopt,
                                                                std::move(motion),
                                                                contact_policy,
                                                                {},
                                                                {},
                                                                std::move(runtime_objects)}}},
      OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] OperationIntentDTO tool_operation(std::string request_id, double position_m) {
  auto operation = motion_operation(std::move(request_id), "unused", PoseGoalDTO{Pose3DTO{}});
  operation.participants.front() = OperationParticipantIntentDTO{
      ComponentRoleId("component-role:tool"), TargetRoleId("target-role:tool"),
      CapabilityId("tool.gripper.command"), std::nullopt,
      CapabilityCommandIntentDTO{
          CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
          CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
          GripperPositionCommandValueDTO{LinearPositionDTO{position_m}, LinearEffortDTO{8.0}},
          2.0,
          {}}};
  return operation;
}

class FakeSceneCandidatePreview final : public ISceneCandidatePreview, public ISceneAuthority {
 public:
  explicit FakeSceneCandidatePreview(CollisionWorldLeaseInventory predecessor)
      : predecessor_(std::move(predecessor)) {}

  Result<SceneCandidatePreviewDTO> PreviewResolvedSceneCandidate(
      const ResolvedSceneCandidatePreviewRequestDTO& request,
      const CollisionWorldLeaseInventory&) const override {
    ++call_count;
    last_request = request;
    const auto* attach = std::get_if<AttachCarriedObjectRequestDTO>(&request.request.operation);
    const auto* detach = std::get_if<DetachCarriedObjectRequestDTO>(&request.request.operation);
    if ((attach == nullptr) == (detach == nullptr) || !request.frame_resolution.has_value()) {
      return Result<SceneCandidatePreviewDTO>::Err(
          ErrorDTO{"invalid_request", "fixture candidate is incomplete", {}});
    }

    auto logical_scene = *predecessor_.logical_scene;
    auto proposed = predecessor_.projection_binding.projection_revision;
    ++proposed.scene_revision.revision_id;
    proposed.scene_revision.content_hash = ContentDigest(kDigest4);
    logical_scene.revision = proposed.scene_revision;
    ++logical_scene.transaction_sequence;
    ++logical_scene.topology_epoch;
    auto& selected = logical_scene.objects.front();
    std::optional<ResolvedSceneOperationDTO> resolved_operation;
    if (attach != nullptr) {
      const CarriedObjectAttachmentRevision attachment_revision("attachment-revision:1");
      selected.placement =
          CarriedSceneObjectPlacementDTO{attach->carried_object_attachment_id, attachment_revision};
      const auto& resolved =
          std::get<ResolvedComponentModelFrameDTO>(request.frame_resolution->resolved_frame);
      CarriedObjectAttachmentDTO attachment{
          attach->carried_object_attachment_id,
          attachment_revision,
          attach->scene_resource_id,
          attach->controllable_component_id,
          attach->command_target_id,
          ComponentSceneFrameRefDTO{attach->controllable_component_id, resolved.model_instance_id,
                                    attach->frame_role_id},
          Pose3DTO{},
          attach->touch_body_roles};
      logical_scene.carried_object_attachments = {attachment};
      resolved_operation.emplace(ResolvedAttachCarriedObjectDTO{selected, std::move(attachment)});
    } else {
      const auto attachment = logical_scene.carried_object_attachments.front();
      selected.placement = CellFixedSceneObjectPlacementDTO{logical_scene.cell_id, Pose3DTO{}};
      logical_scene.carried_object_attachments.clear();
      resolved_operation.emplace(
          ResolvedDetachCarriedObjectDTO{selected, detach->carried_object_attachment_id,
                                         attachment.carried_object_attachment_revision});
    }
    auto snapshot = std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene));
    SceneCandidatePreviewDTO candidate{ContentDigest(kDigest3),
                                       predecessor_.projection_binding.projection_revision,
                                       proposed,
                                       std::move(snapshot),
                                       std::move(*resolved_operation),
                                       predecessor_.bundle.policy.value(),
                                       SceneUpdateStatusDTO::kGeometryChanged,
                                       ContentDigest(kDigest5)};
    if (!candidate.valid()) {
      return Result<SceneCandidatePreviewDTO>::Err(
          ErrorDTO{"invariant_violation", "fixture candidate is invalid", {}});
    }
    return Result<SceneCandidatePreviewDTO>::Ok(std::move(candidate));
  }

  SceneCommitResultDTO Commit(const SceneTransactionRequestDTO&) override {
    ++commit_count;
    throw std::logic_error("fixture scene commit must not be called");
  }

  RequestOwnerEpoch CurrentRequestOwnerEpoch() const override {
    return RequestOwnerEpoch("scene.owner.epoch:test");
  }

  ProjectionRevisionDTO CurrentProjectionRevision() const override {
    return predecessor_.projection_binding.projection_revision;
  }

  std::shared_ptr<const LogicalSceneSnapshotDTO> LogicalSnapshot() const override {
    return predecessor_.logical_scene;
  }

  std::vector<SceneObjectSnapshotDTO> GetSceneObjects() const override {
    return predecessor_.logical_scene->objects;
  }

  SceneObjectStateDTO GetObjectState(const SceneResourceId& id) const override {
    const auto found = std::find_if(
        predecessor_.logical_scene->objects.begin(), predecessor_.logical_scene->objects.end(),
        [&](const auto& object) { return object.scene_resource_id == id; });
    if (found == predecessor_.logical_scene->objects.end()) {
      return {id, false, false, std::nullopt, std::nullopt};
    }
    const auto* carried = std::get_if<CarriedSceneObjectPlacementDTO>(&found->placement);
    return {id, true, carried != nullptr,
            carried == nullptr
                ? std::optional<CarriedObjectAttachmentId>{}
                : std::optional<CarriedObjectAttachmentId>{carried->carried_object_attachment_id},
            std::nullopt};
  }

  Result<Pose3DTO> GetObjectPose(const SceneResourceId& id) const override {
    const auto found = std::find_if(
        predecessor_.logical_scene->objects.begin(), predecessor_.logical_scene->objects.end(),
        [&](const auto& object) { return object.scene_resource_id == id; });
    if (found == predecessor_.logical_scene->objects.end()) {
      return Result<Pose3DTO>::Err(ErrorDTO{"object_not_found", "fixture object is absent", {}});
    }
    if (const auto* fixed = std::get_if<CellFixedSceneObjectPlacementDTO>(&found->placement)) {
      return Result<Pose3DTO>::Ok(fixed->pose_in_cell);
    }
    return Result<Pose3DTO>::Ok(Pose3DTO{});
  }

  void SetPredecessor(CollisionWorldLeaseInventory predecessor) {
    predecessor_ = std::move(predecessor);
  }

  mutable std::size_t call_count{};
  std::size_t commit_count{};
  mutable std::optional<ResolvedSceneCandidatePreviewRequestDTO> last_request;

 private:
  CollisionWorldLeaseInventory predecessor_;
};

class FakeSceneCandidateProjection final : public IPlannerSceneProjection {
 public:
  explicit FakeSceneCandidateProjection(CollisionWorldLeaseInventory predecessor)
      : predecessor_(std::move(predecessor)) {}

  ProjectionCommitResultDTO ApplyTransaction(const SceneTransactionDTO&) override {
    throw std::logic_error("fixture projection commit is unavailable");
  }

  Result<CollisionWorldLeaseInventory> RetainProjection(const ProjectionRevisionDTO& revision,
                                                        ProjectionLeaseId lease_id) override {
    ++retain_call_count;
    if (revision != predecessor_.projection_binding.projection_revision) {
      return Result<CollisionWorldLeaseInventory>::Err(
          ErrorDTO{"stale_revision", "fixture projection is stale", {}});
    }
    auto retained = predecessor_;
    retained.projection_binding.projection_lease_id = std::move(lease_id);
    retained_lease = retained.projection_binding.projection_lease_id;
    return Result<CollisionWorldLeaseInventory>::Ok(std::move(retained));
  }

  Result<CollisionWorldLeaseInventory> RetainCurrentProjection(
      ProjectionLeaseId lease_id) override {
    return RetainProjection(predecessor_.projection_binding.projection_revision,
                            std::move(lease_id));
  }

  void ReleaseProjection(const ProjectionLeaseId& lease_id) override {
    ++release_call_count;
    if (retained_lease == std::optional(lease_id)) {
      retained_lease.reset();
    }
  }

  ProjectionHealthDTO ProjectionHealth() const override {
    throw std::logic_error("fixture projection health is unavailable");
  }

  ProjectionCommitResultDTO RebuildProjection(const NativeProjectionRebuildDTO&) override {
    throw std::logic_error("fixture projection rebuild is unavailable");
  }

  NativeProjectionBootstrapRebuildResultDTO RebuildBootstrapProjection(
      const NativeProjectionBootstrapRebuildDTO&) override {
    throw std::logic_error("fixture bootstrap projection is unavailable");
  }

  NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const NativeProjectionBootstrapStatusQueryDTO&) const override {
    throw std::logic_error("fixture bootstrap status is unavailable");
  }

  Result<SceneFrameResolutionReceiptDTO> ResolveSceneFrame(
      const SceneFrameResolutionQueryDTO& query) const override {
    ++current_frame_call_count;
    if (!query.valid()) {
      return Result<SceneFrameResolutionReceiptDTO>::Err(
          ErrorDTO{"invalid_request", "fixture frame query is invalid", {}});
    }
    const auto& selector = std::get<TargetCarrierSceneFrameRefDTO>(query.selector);
    return Result<SceneFrameResolutionReceiptDTO>::Ok(SceneFrameResolutionReceiptDTO{
        ResolvedComponentModelFrameDTO{
            selector.controllable_component_id, ModelInstanceId("model:test"),
            selector.frame_role_id,
            selector.frame_role_id == FrameRoleId("frame-role:base") ? "base" : "tcp",
            ModelRevision("model-revision:test")},
        Pose3DTO{},
        {},
        query.projection_binding,
        std::nullopt,
        ContentDigest(kDigest5)});
  }

  Result<SceneFrameResolutionReceiptDTO> ResolveCandidateSceneFrame(
      const CandidateSceneFrameResolutionQueryDTO& query) const override {
    ++frame_call_count;
    last_frame_query = query;
    const auto& selector = std::get<TargetCarrierSceneFrameRefDTO>(query.selector);
    return Result<SceneFrameResolutionReceiptDTO>::Ok(SceneFrameResolutionReceiptDTO{
        ResolvedComponentModelFrameDTO{selector.controllable_component_id,
                                       ModelInstanceId("model:test"), selector.frame_role_id, "tcp",
                                       ModelRevision("model-revision:test")},
        Pose3DTO{},
        {},
        query.projection_binding,
        ContentDigest(kDigest4),
        ContentDigest(kDigest5)});
  }

  Result<CollisionWorldLeaseInventory> ProjectSceneCandidate(
      const SceneCandidatePreviewDTO& candidate,
      const ProjectionLeaseBindingDTO& predecessor_binding,
      ProjectionLeaseId candidate_lease_id) const override {
    ++projection_call_count;
    last_predecessor_binding = predecessor_binding;
    last_candidate_lease_id = candidate_lease_id;
    if (!candidate.valid()) {
      return Result<CollisionWorldLeaseInventory>::Err(
          ErrorDTO{"invalid_request", "fixture candidate is invalid", {}});
    }
    auto projected = predecessor_;
    projected.projection_binding = {candidate.proposed_projection_revision,
                                    std::move(candidate_lease_id)};
    projected.logical_scene = candidate.logical_scene;
    projected.bundle.policy = candidate.effective_collision_policy;
    return Result<CollisionWorldLeaseInventory>::Ok(std::move(projected));
  }

  void SetPredecessor(CollisionWorldLeaseInventory predecessor) {
    predecessor_ = std::move(predecessor);
  }

  mutable std::size_t frame_call_count{};
  mutable std::size_t current_frame_call_count{};
  mutable std::size_t projection_call_count{};
  std::size_t retain_call_count{};
  std::size_t release_call_count{};
  std::optional<ProjectionLeaseId> retained_lease;
  mutable std::optional<CandidateSceneFrameResolutionQueryDTO> last_frame_query;
  mutable std::optional<ProjectionLeaseBindingDTO> last_predecessor_binding;
  mutable std::optional<ProjectionLeaseId> last_candidate_lease_id;

 private:
  CollisionWorldLeaseInventory predecessor_;
};

class ReplacingPlanningSceneSynchronizer final : public IPlanningSceneSynchronizer {
 public:
  ReplacingPlanningSceneSynchronizer(FakeSceneCandidatePreview& scene,
                                     FakeSceneCandidateProjection& projection,
                                     CollisionWorldLeaseInventory successor,
                                     std::optional<ProjectionRevisionDTO> returned_floor =
                                         std::nullopt)
      : scene_(scene), projection_(projection), successor_(std::move(successor)),
        returned_floor_(std::move(returned_floor)) {}

  Result<ProjectionRevisionDTO> SynchronizeBeforePlanning() override {
    ++call_count;
    scene_.SetPredecessor(successor_);
    projection_.SetPredecessor(successor_);
    return Result<ProjectionRevisionDTO>::Ok(
        returned_floor_.value_or(successor_.projection_binding.projection_revision));
  }

  std::size_t call_count{};

 private:
  FakeSceneCandidatePreview& scene_;
  FakeSceneCandidateProjection& projection_;
  CollisionWorldLeaseInventory successor_;
  std::optional<ProjectionRevisionDTO> returned_floor_;
};

[[nodiscard]] PickCandidatePlanningRequestDTO pick_request(
    PickCandidateGrid grid, CollisionWorldLeaseInventory predecessor) {
  auto approach = motion_operation("pick.approach", "approach", PoseGoalDTO{Pose3DTO{}});
  auto descend = motion_operation(
      "pick.descend", "descend", CartesianPathDTO{}, ContactPolicyId("pick.contact"),
      {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                        SceneResourceId("workpiece")}});
  auto retreat = motion_operation("pick.retreat", "retreat", CartesianPathDTO{});
  const TargetCarrierSceneFrameRefDTO carrier{ControllableComponentId("component:test"),
                                              CommandTargetId("target:test"),
                                              FrameRoleId("frame-role:tcp")};
  return PickCandidatePlanningRequestDTO{
      std::move(grid),
      std::move(approach),
      std::move(descend),
      std::move(retreat),
      RobotStateDTO{{"joint1", "joint2"}, {0.0, 0.0}, {}},
      predecessor,
      ResolvedSceneCandidatePreviewRequestDTO{
          SceneTransactionRequestDTO{
              PublicRequestContextDTO{RequestId("pick.candidate.attach"),
                                      RequestOwnerEpoch("pick.owner.epoch"), "pick_candidate_test",
                                      std::nullopt},
              {},
              predecessor.projection_binding.projection_revision,
              AttachCarriedObjectRequestDTO{CarriedObjectAttachmentId("attachment:workpiece"),
                                            SceneResourceId("workpiece"),
                                            carrier.controllable_component_id,
                                            carrier.command_target_id,
                                            carrier.frame_role_id,
                                            {CollisionBodyRoleId("collision-body-role:tool")},
                                            {}},
              std::nullopt},
          std::nullopt},
      carrier,
      "pick.candidate"};
}

[[nodiscard]] PlacePlanningRequestDTO place_request(CollisionWorldLeaseInventory predecessor) {
  auto approach = motion_operation("place.approach", "place.approach",
                                   PoseGoalDTO{Pose3DTO{{0.0, 0.0, 0.3}, {}}});
  auto descend = motion_operation(
      "place.descend", "place.descend",
      CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.3}, {}}, Pose3DTO{{0.0, 0.0, 0.2}, {}}}});
  auto retreat = motion_operation(
      "place.retreat", "place.retreat",
      CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.2}, {}}, Pose3DTO{{0.0, 0.0, 0.3}, {}}}});
  const TargetCarrierSceneFrameRefDTO carrier{ControllableComponentId("component:test"),
                                              CommandTargetId("target:test"),
                                              FrameRoleId("frame-role:tcp")};
  return PlacePlanningRequestDTO{
      SceneResourceId("workpiece"),
      std::move(approach),
      std::move(descend),
      std::move(retreat),
      RobotStateDTO{{"joint1", "joint2"}, {0.0, 0.0}, {}},
      predecessor,
      ResolvedSceneCandidatePreviewRequestDTO{
          SceneTransactionRequestDTO{PublicRequestContextDTO{RequestId("place.candidate.detach"),
                                                             RequestOwnerEpoch("place.owner.epoch"),
                                                             "place_candidate_test", std::nullopt},
                                     {},
                                     predecessor.projection_binding.projection_revision,
                                     DetachCarriedObjectRequestDTO{
                                         CarriedObjectAttachmentId("attachment:workpiece"),
                                         CarriedObjectAttachmentRevision("attachment-revision:1"),
                                         Pose3DTO{}, CellSceneFrameRefDTO{CellId("cell:test")}},
                                     std::nullopt},
          std::nullopt},
      carrier,
      "place.candidate"};
}

TEST(PickCandidateGrid, PreservesDeterministicPickThenApproachOrdering) {
  auto value = config();
  value.pick_offset_envelope.position_min_m = {-0.01, 0.0, 0.0};
  value.pick_offset_envelope.position_max_m = {0.01, 0.0, 0.0};
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.06};
  const Pose3DTO object_pose{{1.0, 2.0, 0.5}, {0.0, 0.0, 0.0, 1.0}};

  auto grid = PickCandidateGrid::Create(box_object(), object_pose, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_EQ(grid.value().size(), 6U);
  auto first = grid.value().candidate(0U);
  auto second = grid.value().candidate(1U);
  auto third = grid.value().candidate(2U);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(third.ok());
  EXPECT_EQ(first.value().candidate_index, 1U);
  EXPECT_NEAR(first.value().pick_pose.position_m.x_m, 0.99, 1e-12);
  EXPECT_NEAR(first.value().pick_pose.position_m.z_m, 0.7, 1e-12);
  EXPECT_NEAR(first.value().approach_pose.position_m.z_m, 0.75, 1e-12);
  EXPECT_NEAR(second.value().pick_pose.position_m.x_m, 0.99, 1e-12);
  EXPECT_NEAR(second.value().approach_pose.position_m.z_m, 0.76, 1e-12);
  EXPECT_NEAR(third.value().pick_pose.position_m.x_m, 1.0, 1e-12);
  ASSERT_EQ(first.value().descend_path.waypoints.size(), 2U);
  ASSERT_EQ(first.value().retreat_path.waypoints.size(), 2U);
  EXPECT_EQ(first.value().descend_path.waypoints.front(), first.value().approach_pose);
  EXPECT_EQ(first.value().descend_path.waypoints.back(), first.value().pick_pose);
  EXPECT_EQ(first.value().retreat_path.waypoints.front(), first.value().pick_pose);
  EXPECT_EQ(first.value().retreat_path.waypoints.back(), first.value().retreat_pose);
}

TEST(PickCandidateGrid, DefaultApproachEnvelopePreservesHistoricalFiveToTenCentimetres) {
  auto value = config();

  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_EQ(grid.value().size(), 6U);
  auto first = grid.value().candidate(0U);
  auto last = grid.value().candidate(5U);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(last.ok());
  EXPECT_NEAR(first.value().approach_pose.position_m.z_m - first.value().pick_pose.position_m.z_m,
              0.05, 1e-12);
  EXPECT_NEAR(last.value().approach_pose.position_m.z_m - last.value().pick_pose.position_m.z_m,
              0.10, 1e-12);
}

TEST(PickCandidateGrid, VerticalPickPreservesObjectHeadingAndIgnoresRpyGrid) {
  auto value = config();
  value.force_vertical_pick = true;
  value.pick_offset_envelope.orientation_rpy_min_deg = {-90.0, -45.0, -30.0};
  value.pick_offset_envelope.orientation_rpy_max_deg = {90.0, 45.0, 30.0};
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  const Pose3DTO object_pose{{0.0, 0.0, 0.4}, yaw_quaternion(kPi / 3.0)};

  auto grid = PickCandidateGrid::Create(box_object(), object_pose, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_EQ(grid.value().size(), 1U);
  auto candidate = grid.value().candidate(0U);
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  const auto& orientation = candidate.value().pick_pose.orientation;
  const auto rotate = [&](const std::array<double, 3U>& vector) {
    const double xx = orientation.x * orientation.x;
    const double yy = orientation.y * orientation.y;
    const double zz = orientation.z * orientation.z;
    const double xy = orientation.x * orientation.y;
    const double xz = orientation.x * orientation.z;
    const double yz = orientation.y * orientation.z;
    const double wx = orientation.w * orientation.x;
    const double wy = orientation.w * orientation.y;
    const double wz = orientation.w * orientation.z;
    return std::array<double, 3U>{(1.0 - 2.0 * (yy + zz)) * vector[0] +
                                      2.0 * (xy - wz) * vector[1] + 2.0 * (xz + wy) * vector[2],
                                  2.0 * (xy + wz) * vector[0] +
                                      (1.0 - 2.0 * (xx + zz)) * vector[1] +
                                      2.0 * (yz - wx) * vector[2],
                                  2.0 * (xz - wy) * vector[0] + 2.0 * (yz + wx) * vector[1] +
                                      (1.0 - 2.0 * (xx + yy)) * vector[2]};
  };
  const auto tool_x = rotate({1.0, 0.0, 0.0});
  const auto tool_z = rotate({0.0, 0.0, 1.0});
  EXPECT_NEAR(tool_x[0], std::cos(kPi / 3.0), 1e-12);
  EXPECT_NEAR(tool_x[1], std::sin(kPi / 3.0), 1e-12);
  EXPECT_NEAR(tool_x[2], 0.0, 1e-12);
  EXPECT_NEAR(tool_z[0], 0.0, 1e-12);
  EXPECT_NEAR(tool_z[1], 0.0, 1e-12);
  EXPECT_NEAR(tool_z[2], -1.0, 1e-12);
  EXPECT_EQ(candidate.value().approach_pose.orientation, candidate.value().pick_pose.orientation);
}

TEST(PickCandidateGrid, AppliesMinimumTcpHeightToEveryLinkedPose) {
  auto value = config();
  value.min_tcp_z_m = 0.8;
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};

  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{{0.0, 0.0, 0.1}, {}}, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  auto candidate = grid.value().candidate(0U);
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  EXPECT_TRUE(candidate.value().min_tcp_z_applied);
  EXPECT_DOUBLE_EQ(candidate.value().pick_pose.position_m.z_m, 0.8);
  EXPECT_DOUBLE_EQ(candidate.value().approach_pose.position_m.z_m, 0.85);
  EXPECT_DOUBLE_EQ(candidate.value().descend_path.waypoints.front().position_m.z_m, 0.85);
  EXPECT_DOUBLE_EQ(candidate.value().descend_path.waypoints.back().position_m.z_m, 0.8);
}

TEST(PickCandidateGrid, UsesTheDeclaredResolvedOrCandidateTcpOffsetBasis) {
  auto resolved = config();
  resolved.base_tool_orientation_rpy_deg = {0.0, 0.0, 0.0};
  resolved.pick_offset_envelope.position_min_m = {0.1, 0.0, 0.0};
  resolved.pick_offset_envelope.position_max_m = {0.1, 0.0, 0.0};
  resolved.approach_offset_envelope.position_min_m = {};
  resolved.approach_offset_envelope.position_max_m = {};
  resolved.pick_offset_basis.resolved_orientation_in_base = yaw_quaternion(kPi / 2.0);
  auto candidate_tcp = resolved;
  candidate_tcp.pick_offset_basis.use_candidate_tcp_orientation = true;

  auto resolved_grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, resolved);
  auto candidate_grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, candidate_tcp);

  ASSERT_TRUE(resolved_grid.ok()) << resolved_grid.error().message;
  ASSERT_TRUE(candidate_grid.ok()) << candidate_grid.error().message;
  auto resolved_pose = resolved_grid.value().candidate(0U);
  auto candidate_pose = candidate_grid.value().candidate(0U);
  ASSERT_TRUE(resolved_pose.ok());
  ASSERT_TRUE(candidate_pose.ok());
  EXPECT_NEAR(resolved_pose.value().pick_pose.position_m.x_m, 0.0, 1e-12);
  EXPECT_NEAR(resolved_pose.value().pick_pose.position_m.y_m, 0.1, 1e-12);
  EXPECT_NEAR(candidate_pose.value().pick_pose.position_m.x_m, 0.1, 1e-12);
  EXPECT_NEAR(candidate_pose.value().pick_pose.position_m.y_m, 0.0, 1e-12);
}

TEST(PickCandidateGrid, RejectsInvalidOrOversizedGridsBeforePlanning) {
  auto reversed = config();
  reversed.pick_offset_envelope.position_min_m = {1.0, 0.0, 0.0};
  reversed.pick_offset_envelope.position_max_m = {0.0, 0.0, 0.0};
  auto invalid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, reversed);
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().details.at("reason"), "pick_candidate_grid_axis_reversed");

  auto oversized = config();
  oversized.maximum_candidate_count = 100U;
  oversized.pick_offset_envelope.position_max_m = {0.1, 0.1, 0.0};
  oversized.approach_offset_envelope.position_min_m = {};
  oversized.approach_offset_envelope.position_max_m = {};
  auto rejected = PickCandidateGrid::Create(box_object(), Pose3DTO{}, oversized);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "pick_candidate_count_exceeded");
}

TEST(AddressedPickCandidatePlanner,
     ChainsExactStatesAndUsesContactOnlyForDescendAndAttachedRetreat) {
  auto predecessor = predecessor_projection();
  FakeMotionPreview motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickCandidatePlanner planner(motion, scene, projection);
  auto value = config();
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, value);
  ASSERT_TRUE(grid.ok()) << grid.error().message;

  auto result = planner.Search(pick_request(std::move(grid.value()), std::move(predecessor)));

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().selected.has_value());
  EXPECT_EQ(result.value().attempted_candidate_count, 1U);
  ASSERT_EQ(motion.requests.size(), 3U);
  EXPECT_FALSE(motion.requests[0].resolved_contact_grasp.has_value());
  EXPECT_TRUE(motion.requests[0].contact_policy_id.empty());
  EXPECT_TRUE(motion.requests[1].resolved_contact_grasp.has_value());
  EXPECT_EQ(motion.requests[1].contact_policy_id, "pick.contact");
  EXPECT_FALSE(motion.requests[2].resolved_contact_grasp.has_value());
  EXPECT_TRUE(motion.requests[2].contact_policy_id.empty());
  EXPECT_EQ(motion.start_states[0].positions_rad, std::vector<double>({0.0, 0.0}));
  EXPECT_EQ(motion.start_states[1].positions_rad, std::vector<double>({1.0, 1.0}));
  EXPECT_EQ(motion.start_states[2].positions_rad, std::vector<double>({2.0, 2.0}));
  EXPECT_EQ(motion.projection_bindings[0], motion.projection_bindings[1]);
  EXPECT_NE(motion.projection_bindings[1], motion.projection_bindings[2]);
  ASSERT_EQ(motion.expected_scene_revisions.size(), 3U);
  EXPECT_EQ(motion.expected_scene_revisions[2],
            motion.projection_bindings[2].projection_revision.scene_revision);
  EXPECT_EQ(scene.call_count, 1U);
  ASSERT_TRUE(scene.last_request.has_value());
  EXPECT_TRUE(scene.last_request->frame_resolution.has_value());
  EXPECT_EQ(projection.frame_call_count, 1U);
  EXPECT_EQ(projection.projection_call_count, 1U);
  ASSERT_TRUE(projection.last_frame_query.has_value());
  EXPECT_EQ(projection.last_frame_query->predicted_state.positions,
            testing::make_test_controlled_state(motion.requests[1].binding, {2.0, 2.0}).positions);
}

TEST(AddressedPickCandidatePlanner, StopsTheGridOnRoboPlanStartStateCollisionEvidence) {
  auto predecessor = predecessor_projection();
  FakeMotionPreview motion;
  motion.fail_first_with_start_collision = true;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickCandidatePlanner planner(motion, scene, projection);
  auto value = config();
  value.pick_offset_envelope.position_min_m = {-0.01, 0.0, 0.0};
  value.pick_offset_envelope.position_max_m = {0.01, 0.0, 0.0};
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, value);
  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_GT(grid.value().size(), 1U);

  auto result = planner.Search(pick_request(std::move(grid.value()), std::move(predecessor)));

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(result.value().selected.has_value());
  EXPECT_TRUE(result.value().stopped_on_invariant_failure);
  EXPECT_EQ(result.value().attempted_candidate_count, 1U);
  EXPECT_EQ(motion.requests.size(), 1U);
  EXPECT_EQ(scene.call_count, 0U);
  EXPECT_EQ(projection.frame_call_count, 0U);
  EXPECT_EQ(projection.projection_call_count, 0U);
}

TEST(AddressedPlacePlanner,
     KeepsTheObjectCarriedUntilDetachThenRetreatsAgainstTheDetachedSuccessor) {
  auto predecessor = carried_predecessor_projection();
  FakeMotionPreview motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPlacePlanner planner(motion, scene, projection);

  auto result = planner.Plan(place_request(std::move(predecessor)));

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().selected.has_value());
  ASSERT_EQ(motion.requests.size(), 3U);
  EXPECT_EQ(motion.start_states[0].positions_rad, std::vector<double>({0.0, 0.0}));
  EXPECT_EQ(motion.start_states[1].positions_rad, std::vector<double>({1.0, 1.0}));
  EXPECT_EQ(motion.start_states[2].positions_rad, std::vector<double>({2.0, 2.0}));
  EXPECT_EQ(motion.projection_bindings[0], motion.projection_bindings[1]);
  EXPECT_NE(motion.projection_bindings[1], motion.projection_bindings[2]);
  ASSERT_EQ(motion.expected_scene_revisions.size(), 3U);
  EXPECT_EQ(motion.expected_scene_revisions[2],
            motion.projection_bindings[2].projection_revision.scene_revision);
  EXPECT_EQ(scene.call_count, 1U);
  ASSERT_TRUE(scene.last_request.has_value());
  ASSERT_TRUE(scene.last_request->frame_resolution.has_value());
  const auto* detach =
      std::get_if<DetachCarriedObjectRequestDTO>(&scene.last_request->request.operation);
  ASSERT_NE(detach, nullptr);
  EXPECT_TRUE(std::holds_alternative<ComponentSceneFrameRefDTO>(detach->frame_ref));
  const Pose3DTO expected_release_pose{{0.0, 0.0, 0.2}, {}};
  EXPECT_EQ(detach->pose, expected_release_pose);
  EXPECT_EQ(projection.frame_call_count, 1U);
  EXPECT_EQ(projection.projection_call_count, 1U);
  ASSERT_TRUE(projection.last_frame_query.has_value());
  EXPECT_EQ(projection.last_frame_query->predicted_state.positions,
            testing::make_test_controlled_state(motion.requests[1].binding, {2.0, 2.0}).positions);
  EXPECT_TRUE(std::holds_alternative<PoseGoalDTO>(result.value().selected->approach_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().selected->descend_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().selected->retreat_motion));
}

TEST(AddressedPickPlacePlanningOwner,
     ResolvesPublicPickIntentWithoutCommittingOrDispatchingEffects) {
  auto predecessor = predecessor_projection();
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  auto synthesis = PickCandidateSynthesisIntentDTO{};
  synthesis.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  synthesis.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto approach = motion_operation("public.pick.approach", "approach", PoseGoalDTO{Pose3DTO{}});
  auto descend = motion_operation(
      "public.pick.descend", "descend", CartesianPathDTO{}, ContactPolicyId("pick.contact"),
      {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                        SceneResourceId("workpiece")}});
  auto retreat = motion_operation("public.pick.retreat", "retreat", CartesianPathDTO{});
  auto tool = tool_operation("public.pick.tool", 0.0);
  // Callers name no Scene revision; planning uses the latest Scene.
  for (auto* const operation : {&approach, &descend, &tool, &retreat}) {
    operation->expected_scene_revision = std::nullopt;
  }

  const PickSequencePlanningIntentDTO request{"public-pick-1",
                                              SceneResourceId("workpiece"),
                                              RuntimeObjectId("selected_object"),
                                              FrameRoleId("frame-role:tcp"),
                                              synthesis,
                                              std::move(approach),
                                              std::move(descend),
                                              std::move(tool),
                                              std::move(retreat)};
  auto result = owner.PlanPick(request);
  auto repeated = owner.PlanPick(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(repeated.ok()) << repeated.error().message;
  EXPECT_EQ(result.value().status, PickPlacePlanningStatusDTO::kPlanned);
  EXPECT_EQ(repeated.value().status, PickPlacePlanningStatusDTO::kPlanned);
  ASSERT_TRUE(result.value().plan.has_value());
  ASSERT_TRUE(repeated.value().plan.has_value());
  EXPECT_EQ(result.value().attempted_candidate_count, 1U);
  EXPECT_EQ(motion.begin_count, 2U);
  EXPECT_EQ(scene.commit_count, 0U);
  EXPECT_EQ(scene.call_count, 2U);
  EXPECT_EQ(projection.retain_call_count, 2U);
  EXPECT_EQ(projection.release_call_count, 2U);
  EXPECT_EQ(projection.current_frame_call_count, 4U);
  EXPECT_EQ(result.value().plan->approach_motion, repeated.value().plan->approach_motion);
  EXPECT_EQ(result.value().plan->descend_motion, repeated.value().plan->descend_motion);
  EXPECT_EQ(result.value().plan->retreat_motion, repeated.value().plan->retreat_motion);
  EXPECT_EQ(result.value().plan->predecessor_projection_revision,
            repeated.value().plan->predecessor_projection_revision);
  EXPECT_EQ(result.value().plan->preview_successor_projection_revision,
            repeated.value().plan->preview_successor_projection_revision);
  EXPECT_EQ(result.value().plan->selected_object_id.value(), "workpiece");
  EXPECT_EQ(result.value().plan->kind, PickPlaceExecutionKindDTO::kPick);
  EXPECT_TRUE(std::holds_alternative<PoseGoalDTO>(result.value().plan->approach_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().plan->descend_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().plan->retreat_motion));
}

TEST(AddressedPickPlacePlanningOwner, QualifiedSiblingTcpKeepsTheExplicitCarrierFrameAndOwner) {
  // The endpoint's portable alias and modeled TCP differ from the tool operation's
  // carrier alias/frame. Only the exact physical carrier component/target/model
  // may authorize attachment; the TCP is never substituted for that frame.
  for (int carrier_identity = 0; carrier_identity < 3; ++carrier_identity) {
    SCOPED_TRACE(carrier_identity);
    auto predecessor = predecessor_projection();
    FakeMotionPreviewOwner motion;
    motion.motion_binding = arm_runtime_binding();
    auto& arm = *motion.motion_binding;
    auto endpoint_bodies = arm.contact_grasp_bindings.front().allowed_body_bindings;
    endpoint_bodies.front().target_role_id = TargetRoleId("target-role:tool_tcp_alias");
    arm.motion_endpoint =
        RuntimeMotionEndpointBindingDTO{TargetRoleId("target-role:tool_tcp_alias"),
                                        CommandTargetId("target:test"),
                                        {FrameRoleId("frame-role:fingertip"),
                                         ModelInstanceId("model:test"),
                                         ModelRevision("model-revision:test"),
                                         "fingertip",
                                         {CollisionBodyRoleId("collision-body-role:tool")}},
                                        std::move(endpoint_bodies)};
    arm.contact_grasp_bindings.front().allowed_body_bindings.clear();
    arm.contact_grasp_bindings.front().bodies_from_motion_endpoint = true;
    motion.carrier_binding = carrier_runtime_binding();
    if (carrier_identity == 1)
      motion.carrier_binding->controllable_component_id =
          ControllableComponentId("other-component");
    if (carrier_identity == 2)
      motion.carrier_binding->command_target_id = CommandTargetId("other-target");
    FakeSceneCandidatePreview scene(predecessor);
    FakeSceneCandidateProjection projection(predecessor);
    AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
    const auto qualified = [&](OperationIntentDTO operation) {
      auto& payload = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
      payload.tcp_endpoint = tcp_entry_of(*arm.motion_endpoint, arm.controllable_component_id);
      return operation;
    };
    const PickSequencePlanningIntentDTO request{
        "qualified-endpoint-pick",
        SceneResourceId("workpiece"),
        RuntimeObjectId("selected_object"),
        FrameRoleId("frame-role:tcp"),
        PickCandidateSynthesisIntentDTO{},
        qualified(motion_operation("qualified.approach", "approach", PoseGoalDTO{Pose3DTO{}})),
        qualified(motion_operation(
            "qualified.descend", "descend", CartesianPathDTO{}, ContactPolicyId("pick.contact"),
            {{RuntimeObjectId("selected_object"), SceneResourceId("workpiece")}})),
        tool_operation("qualified.tool", 0.0),
        qualified(motion_operation("qualified.retreat", "retreat", CartesianPathDTO{}))};
    const auto result = owner.PlanPick(request);
    if (carrier_identity != 0) {
      ASSERT_FALSE(result.ok());
      EXPECT_EQ(result.error().details.at("reason"), "pick_carrier_touch_roles_unresolved");
      EXPECT_EQ(projection.retain_call_count, 1U);
      EXPECT_EQ(projection.release_call_count, 1U);
      EXPECT_FALSE(projection.retained_lease.has_value());
      EXPECT_EQ(scene.call_count, 0U);
    } else {
      ASSERT_TRUE(result.ok()) << result.error().message;
      EXPECT_EQ(result.value().status, PickPlacePlanningStatusDTO::kPlanned);
      ASSERT_TRUE(scene.last_request);
      const auto& attach =
          std::get<AttachCarriedObjectRequestDTO>(scene.last_request->request.operation);
      EXPECT_EQ(attach.controllable_component_id, ControllableComponentId("component:test"));
      EXPECT_EQ(attach.command_target_id, CommandTargetId("target:test"));
      EXPECT_EQ(attach.frame_role_id, request.carrier_frame_role_id);
      EXPECT_NE(attach.frame_role_id, arm.motion_endpoint->frame.frame_role_id);
      EXPECT_EQ(
          attach.touch_body_roles,
          (std::vector<CollisionBodyRoleId>{CollisionBodyRoleId("collision-body-role:tool")}));
      ASSERT_TRUE(scene.last_request->frame_resolution);
      const auto& frame = std::get<ResolvedComponentModelFrameDTO>(
          scene.last_request->frame_resolution->resolved_frame);
      EXPECT_EQ(frame.frame_role_id, request.carrier_frame_role_id);
      EXPECT_EQ(frame.compiled_frame_name, "tcp");
    }
    EXPECT_EQ(scene.commit_count, 0U);
  }
}

TEST(AddressedPickPlacePlanningOwner, AcceptsNewerSameLineageProjectionBeyondCaptureFloorForPick) {
  auto predecessor = predecessor_projection();
  auto successor = successor_projection(predecessor);
  const auto capture_floor = predecessor.projection_binding.projection_revision;
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  ReplacingPlanningSceneSynchronizer synchronizer(scene, projection, successor, capture_floor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection, &synchronizer);
  auto synthesis = PickCandidateSynthesisIntentDTO{};
  synthesis.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  synthesis.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto approach =
      motion_operation("synchronized.pick.approach", "approach", PoseGoalDTO{Pose3DTO{}});
  auto descend = motion_operation(
      "synchronized.pick.descend", "descend", CartesianPathDTO{}, ContactPolicyId("pick.contact"),
      {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                        SceneResourceId("workpiece")}});
  auto tool = tool_operation("synchronized.pick.tool", 0.0);
  auto retreat = motion_operation("synchronized.pick.retreat", "retreat", CartesianPathDTO{});
  const PickSequencePlanningIntentDTO request{"synchronized-pick-1",
                                              SceneResourceId("workpiece"),
                                              RuntimeObjectId("selected_object"),
                                              FrameRoleId("frame-role:tcp"),
                                              synthesis,
                                              std::move(approach),
                                              std::move(descend),
                                              std::move(tool),
                                              std::move(retreat)};

  const auto result = owner.PlanPick(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().plan.has_value());
  EXPECT_EQ(synchronizer.call_count, 1U);
  const auto& planned = *result.value().plan;
  EXPECT_EQ(planned.predecessor_projection_revision,
            successor.projection_binding.projection_revision);
  EXPECT_EQ(planned.predecessor_projection_revision.scene_revision.revision_id,
            capture_floor.scene_revision.revision_id + 1U);
  ASSERT_TRUE(scene.last_request);
  EXPECT_EQ(scene.last_request->request.expected_projection_revision,
            successor.projection_binding.projection_revision);
  EXPECT_EQ(planned.preview_successor_projection_revision.scene_revision.revision_id,
            successor.projection_binding.projection_revision.scene_revision.revision_id + 1U);
  EXPECT_EQ(planned.preview_successor_projection_revision.scene_revision.content_hash,
            ContentDigest(kDigest4));
}

TEST(AddressedPickPlacePlanningOwner, PreservesPortableAndCandidateTcpOffsetReferenceSemantics) {
  auto predecessor = predecessor_projection();
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  auto synthesis = PickCandidateSynthesisIntentDTO{};
  synthesis.pick_offset_envelope.position_min_m = {0.02, 0.0, 0.0};
  synthesis.pick_offset_envelope.position_max_m = {0.02, 0.0, 0.0};
  synthesis.approach_offset_envelope.position_min_m = {0.05, 0.0, 0.0};
  synthesis.approach_offset_envelope.position_max_m = {0.05, 0.0, 0.0};
  synthesis.pick_offset_reference = ReferenceDTO(ReferenceDTO::Type::kSceneResource, "workpiece");
  synthesis.approach_offset_reference =
      ReferenceDTO(ReferenceDTO::Type::kFrameRole, "frame-role:tcp");
  auto result = owner.PlanPick(PickSequencePlanningIntentDTO{
      "public-pick-reference", SceneResourceId("workpiece"), RuntimeObjectId("selected_object"),
      FrameRoleId("frame-role:tcp"), synthesis,
      motion_operation("reference.approach", "approach", PoseGoalDTO{Pose3DTO{}}),
      motion_operation("reference.descend", "descend", CartesianPathDTO{},
                       ContactPolicyId("pick.contact"),
                       {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                                         SceneResourceId("workpiece")}}),
      tool_operation("reference.tool", 0.0),
      motion_operation("reference.retreat", "retreat", CartesianPathDTO{})});

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(motion.reference_resolution_count, 1U);
  ASSERT_TRUE(result.value().last_attempted_pick_candidate.has_value());
  const auto& candidate = *result.value().last_attempted_pick_candidate;
  EXPECT_NEAR(candidate.pick_pose.position_m.x_m, 0.0, 1.0e-9);
  EXPECT_NEAR(candidate.pick_pose.position_m.y_m, 0.02, 1.0e-9);
  EXPECT_NEAR(candidate.pick_pose.position_m.z_m, 0.2, 1.0e-9);
  EXPECT_NEAR(candidate.approach_pose.position_m.x_m, 0.05, 1.0e-9);
  EXPECT_NEAR(candidate.approach_pose.position_m.y_m, 0.02, 1.0e-9);

  synthesis.pick_offset_reference = ReferenceDTO(ReferenceDTO::Type::kFrameRole, "frame-role:tcp");
  synthesis.approach_offset_reference =
      ReferenceDTO(ReferenceDTO::Type::kComponentBodyRole, "body-role:wrist");
  auto inverse_result = owner.PlanPick(PickSequencePlanningIntentDTO{
      "public-pick-reference-inverse", SceneResourceId("workpiece"),
      RuntimeObjectId("selected_object"), FrameRoleId("frame-role:tcp"), synthesis,
      motion_operation("reference.inverse.approach", "approach", PoseGoalDTO{Pose3DTO{}}),
      motion_operation("reference.inverse.descend", "descend", CartesianPathDTO{},
                       ContactPolicyId("pick.contact"),
                       {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                                         SceneResourceId("workpiece")}}),
      tool_operation("reference.inverse.tool", 0.0),
      motion_operation("reference.inverse.retreat", "retreat", CartesianPathDTO{})});

  ASSERT_TRUE(inverse_result.ok()) << inverse_result.error().message;
  EXPECT_EQ(motion.reference_resolution_count, 2U);
  ASSERT_TRUE(inverse_result.value().last_attempted_pick_candidate.has_value());
  const auto& inverse_candidate = *inverse_result.value().last_attempted_pick_candidate;
  EXPECT_NEAR(inverse_candidate.pick_pose.position_m.x_m, 0.02, 1.0e-9);
  EXPECT_NEAR(inverse_candidate.pick_pose.position_m.y_m, 0.0, 1.0e-9);
  EXPECT_NEAR(inverse_candidate.approach_pose.position_m.x_m, 0.02, 1.0e-9);
  EXPECT_NEAR(inverse_candidate.approach_pose.position_m.y_m, 0.05, 1.0e-9);
}

// C-P7 (G7): an offset whose reference is the move's TCP entry keeps the
// candidate TCP's orientation also when the entry belongs to a tool mounted on
// the arm (before: only the arm's own component matched, so a tool TCP
// reference was resolved like any frame and took the start orientation). A
// different entry of the same tool is an ordinary reference.
TEST(AddressedPickPlacePlanningOwner, OffsetReferenceEqualToAToolTcpEntryUsesTheCandidateTcp) {
  auto predecessor = predecessor_projection();
  FakeMotionPreviewOwner motion;
  motion.motion_binding = arm_runtime_binding();
  auto& arm = *motion.motion_binding;
  auto endpoint_bodies = arm.contact_grasp_bindings.front().allowed_body_bindings;
  endpoint_bodies.front().target_role_id = TargetRoleId("target-role:gripper");
  arm.motion_endpoint =
      RuntimeMotionEndpointBindingDTO{TargetRoleId("target-role:gripper"),
                                      CommandTargetId("target:test"),
                                      {FrameRoleId("frame-role:fingertip"),
                                       ModelInstanceId("model:test"),
                                       ModelRevision("model-revision:test"),
                                       "fingertip",
                                       {CollisionBodyRoleId("collision-body-role:tool")}},
                                      std::move(endpoint_bodies)};
  arm.contact_grasp_bindings.front().allowed_body_bindings.clear();
  arm.contact_grasp_bindings.front().bodies_from_motion_endpoint = true;
  motion.carrier_binding = carrier_runtime_binding();
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  const auto qualified = [&](OperationIntentDTO operation) {
    auto& payload = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
    payload.tcp_endpoint = tcp_entry_of(*arm.motion_endpoint, arm.controllable_component_id);
    return operation;
  };
  const auto request = [&](const std::string& id, PickCandidateSynthesisIntentDTO synthesis) {
    return PickSequencePlanningIntentDTO{
        id,
        SceneResourceId("workpiece"),
        RuntimeObjectId("selected_object"),
        FrameRoleId("frame-role:tcp"),
        std::move(synthesis),
        qualified(motion_operation(id + ".approach", "approach", PoseGoalDTO{Pose3DTO{}})),
        qualified(motion_operation(
            id + ".descend", "descend", CartesianPathDTO{}, ContactPolicyId("pick.contact"),
            {{RuntimeObjectId("selected_object"), SceneResourceId("workpiece")}})),
        tool_operation(id + ".tool", 0.0),
        qualified(motion_operation(id + ".retreat", "retreat", CartesianPathDTO{}))};
  };
  // The tool's entry, named by its own component (not the arm's).
  const ReferenceTargetDTO tool_entry_owner{TargetRoleId("target-role:gripper"),
                                            ControllableComponentId("component:gripper"),
                                            CommandTargetId("target:test")};
  ASSERT_NE(tool_entry_owner.controllable_component_id, arm.controllable_component_id);
  auto synthesis = PickCandidateSynthesisIntentDTO{};
  synthesis.pick_offset_reference =
      ReferenceDTO(FrameRoleId("frame-role:fingertip"), tool_entry_owner);
  const auto candidate = owner.PlanPick(request("tool-entry-offset", synthesis));
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  EXPECT_EQ(motion.reference_resolution_count, 0U);

  synthesis.pick_offset_reference = ReferenceDTO(FrameRoleId("frame-role:base"), tool_entry_owner);
  const auto other_entry = owner.PlanPick(request("other-entry-offset", synthesis));
  ASSERT_TRUE(other_entry.ok()) << other_entry.error().message;
  EXPECT_EQ(motion.reference_resolution_count, 1U);
}

TEST(AddressedPickPlacePlanningOwner, AcceptsAnyFrameOffsetReferenceAndNamesAnUnboundObject) {
  auto predecessor = predecessor_projection();
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  const auto request = [](const std::string& id, PickCandidateSynthesisIntentDTO synthesis) {
    return PickSequencePlanningIntentDTO{
        id, SceneResourceId("workpiece"), RuntimeObjectId("selected_object"),
        FrameRoleId("frame-role:tcp"), std::move(synthesis),
        motion_operation(id + ".approach", "approach", PoseGoalDTO{Pose3DTO{}}),
        motion_operation(id + ".descend", "descend", CartesianPathDTO{},
                         ContactPolicyId("pick.contact"),
                         {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                                           SceneResourceId("workpiece")}}),
        tool_operation(id + ".tool", 0.0),
        motion_operation(id + ".retreat", "retreat", CartesianPathDTO{})};
  };

  // The cell frame and another robot's frame are ordinary offset references:
  // both reach the one reference resolution like motion references.
  auto synthesis = PickCandidateSynthesisIntentDTO{};
  synthesis.pick_offset_reference = ReferenceDTO(ReferenceDTO::Type::kCellFrame, "");
  synthesis.approach_offset_reference =
      ReferenceDTO(FrameRoleId("frame-role:tcp"),
                   ReferenceTargetDTO{TargetRoleId("observer_arm"),
                                      ControllableComponentId("observer.component"),
                                      CommandTargetId("observer.command")});
  const auto accepted = owner.PlanPick(request("any-frame-offsets", synthesis));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(motion.reference_resolution_count, 1U);

  // A runtime object other than the selected one is refused by name.
  synthesis.pick_offset_reference =
      ReferenceDTO(ReferenceDTO::Type::kRuntimeObject, "other_object");
  synthesis.approach_offset_reference.reset();
  const auto unbound = owner.PlanPick(request("unbound-offset-object", synthesis));
  ASSERT_FALSE(unbound.ok());
  EXPECT_EQ(unbound.error().code, "missing_reference");
  EXPECT_EQ(unbound.error().details.at("reason"), "pick_offset_runtime_object_unbound");
  EXPECT_EQ(unbound.error().details.at("reference_type"), "runtime_object");
  EXPECT_EQ(unbound.error().details.at("reference_id"), "other_object");
  EXPECT_EQ(motion.reference_resolution_count, 1U);
}

TEST(AddressedPickPlacePlanningOwner, NamesAnUnresolvedCarrierFrameWithTheToolTargetRole) {
  const auto expect_carrier_named = [](const ErrorDTO& error, const char* reason) {
    EXPECT_EQ(error.code, "missing_reference");
    EXPECT_EQ(error.details.at("reason"), reason);
    EXPECT_EQ(error.details.at("reference_type"), "frame_role");
    EXPECT_EQ(error.details.at("reference_id"), "frame-role:absent");
    EXPECT_EQ(error.details.at("target_role_id"), "target-role:tool");
  };
  {
    auto predecessor = predecessor_projection();
    FakeMotionPreviewOwner motion;
    FakeSceneCandidatePreview scene(predecessor);
    FakeSceneCandidateProjection projection(predecessor);
    AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
    const auto pick = owner.PlanPick(PickSequencePlanningIntentDTO{
        "absent-carrier-pick", SceneResourceId("workpiece"), RuntimeObjectId("selected_object"),
        FrameRoleId("frame-role:absent"), PickCandidateSynthesisIntentDTO{},
        motion_operation("absent-carrier-pick.approach", "approach", PoseGoalDTO{Pose3DTO{}}),
        motion_operation("absent-carrier-pick.descend", "descend", CartesianPathDTO{},
                         ContactPolicyId("pick.contact"),
                         {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                                           SceneResourceId("workpiece")}}),
        tool_operation("absent-carrier-pick.tool", 0.0),
        motion_operation("absent-carrier-pick.retreat", "retreat", CartesianPathDTO{})});
    ASSERT_FALSE(pick.ok());
    expect_carrier_named(pick.error(), "pick_carrier_frame_unresolved");
  }
  {
    auto predecessor = carried_predecessor_projection();
    FakeMotionPreviewOwner motion;
    FakeSceneCandidatePreview scene(predecessor);
    FakeSceneCandidateProjection projection(predecessor);
    AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
    const auto place = owner.PlanPlace(PlaceSequencePlanningIntentDTO{
        "absent-carrier-place", SceneResourceId("workpiece"), FrameRoleId("frame-role:absent"),
        motion_operation("absent-carrier-place.approach", "place.approach",
                         PoseGoalDTO{Pose3DTO{{0.0, 0.0, 0.3}, {}}}),
        motion_operation(
            "absent-carrier-place.descend", "place.descend",
            CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.3}, {}}, Pose3DTO{{0.0, 0.0, 0.2}, {}}}}),
        tool_operation("absent-carrier-place.tool", 0.04),
        motion_operation(
            "absent-carrier-place.retreat", "place.retreat",
            CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.2}, {}}, Pose3DTO{{0.0, 0.0, 0.3}, {}}}})});
    ASSERT_FALSE(place.ok());
    expect_carrier_named(place.error(), "place_carrier_frame_unresolved");
  }
}

TEST(AddressedPickPlacePlanningOwner, NamesTheRuntimeObjectAndPolicyOfAnUnresolvedContactGrasp) {
  const auto plan = [](const std::string& id, const std::string& runtime_object_id,
                       const std::string& contact_policy_id, bool release = false) {
    auto predecessor = predecessor_projection();
    FakeMotionPreviewOwner motion;
    if (release) {
      motion.motion_binding = arm_runtime_binding();
      motion.motion_binding->contact_grasp_bindings.front().envelope = ContactReleaseEnvelopeDTO{};
    }
    FakeSceneCandidatePreview scene(predecessor);
    FakeSceneCandidateProjection projection(predecessor);
    AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
    return owner.PlanPick(PickSequencePlanningIntentDTO{
        id, SceneResourceId("workpiece"), RuntimeObjectId(runtime_object_id),
        FrameRoleId("frame-role:tcp"), PickCandidateSynthesisIntentDTO{},
        motion_operation(id + ".approach", "approach", PoseGoalDTO{Pose3DTO{}}),
        motion_operation(id + ".descend", "descend", CartesianPathDTO{},
                         ContactPolicyId(contact_policy_id),
                         {RuntimeObjectReferenceBindingDTO{RuntimeObjectId(runtime_object_id),
                                                           SceneResourceId("workpiece")}}),
        tool_operation(id + ".tool", 0.0),
        motion_operation(id + ".retreat", "retreat", CartesianPathDTO{})});
  };
  const auto expect_named = [](const ErrorDTO& error, const char* runtime_object_id,
                               const char* contact_policy_id) {
    EXPECT_EQ(error.code, "missing_reference");
    EXPECT_EQ(error.details.at("reason"), "pick_contact_grasp_template_unresolved");
    EXPECT_EQ(error.details.at("reference_type"), "runtime_object");
    EXPECT_EQ(error.details.at("reference_id"), runtime_object_id);
    EXPECT_EQ(error.details.at("contact_policy_id"), contact_policy_id);
    EXPECT_FALSE(error.details.contains("target_role_id"));
  };

  // The Program binds no contact-grasp policy with the descend's ID.
  const auto missing_policy = plan("absent-policy-pick", "selected_object", "pick.absent");
  ASSERT_FALSE(missing_policy.ok());
  expect_named(missing_policy.error(), "selected_object", "pick.absent");
  EXPECT_NE(missing_policy.error().message.find("absent"), std::string::npos)
      << missing_policy.error().message;

  // The policy exists but is bound to another runtime object than the selected one.
  const auto mismatch = plan("mismatched-object-pick", "other_object", "pick.contact");
  ASSERT_FALSE(mismatch.ok());
  expect_named(mismatch.error(), "other_object", "pick.contact");
  EXPECT_NE(mismatch.error().message.find("other than the selected"), std::string::npos)
      << mismatch.error().message;

  const auto release = plan("release-policy-pick", "selected_object", "pick.contact", true);
  ASSERT_FALSE(release.ok());
  expect_named(release.error(), "selected_object", "pick.contact");
}

TEST(AddressedPickPlacePlanningOwner, FailedPlanningRetainsOneBoundedLinkedMotionDiagnostic) {
  auto predecessor = predecessor_projection();
  FakeMotionPreviewOwner motion;
  motion.fail_first_with_start_collision = true;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  auto synthesis = PickCandidateSynthesisIntentDTO{};
  synthesis.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  synthesis.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto result = owner.PlanPick(PickSequencePlanningIntentDTO{
      "public-pick-failed", SceneResourceId("workpiece"), RuntimeObjectId("selected_object"),
      FrameRoleId("frame-role:tcp"), synthesis,
      motion_operation("failed.approach", "approach", PoseGoalDTO{Pose3DTO{}}),
      motion_operation("failed.descend", "descend", CartesianPathDTO{},
                       ContactPolicyId("pick.contact"),
                       {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                                         SceneResourceId("workpiece")}}),
      tool_operation("failed.tool", 0.0),
      motion_operation("failed.retreat", "retreat", CartesianPathDTO{})});

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().status, PickPlacePlanningStatusDTO::kNoFeasibleCandidate);
  EXPECT_FALSE(result.value().plan.has_value());
  ASSERT_TRUE(result.value().last_attempted_pick_candidate.has_value());
  const auto& candidate = *result.value().last_attempted_pick_candidate;
  ASSERT_EQ(candidate.descend_path.waypoints.size(), 2U);
  ASSERT_EQ(candidate.retreat_path.waypoints.size(), 2U);
  EXPECT_EQ(candidate.descend_path.waypoints.front(), candidate.approach_pose);
  EXPECT_EQ(candidate.descend_path.waypoints.back(), candidate.pick_pose);
  EXPECT_EQ(candidate.retreat_path.waypoints.front(), candidate.pick_pose);
  EXPECT_EQ(candidate.retreat_path.waypoints.back(), candidate.retreat_pose);
  ASSERT_EQ(result.value().reports.size(), 1U);
  ASSERT_EQ(result.value().reports.front().stages.size(), 1U);
  EXPECT_FALSE(result.value().reports.front().stages.front().succeeded);
}

TEST(AddressedPickPlacePlanningOwner, RejectsInvalidOrCancelledPickBeforeRetainingAProjection) {
  auto predecessor = predecessor_projection();
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  const auto make_intent = [&]() {
    return PickSequencePlanningIntentDTO{
        "public-pick-2",
        SceneResourceId("workpiece"),
        RuntimeObjectId("selected_object"),
        FrameRoleId("frame-role:tcp"),
        {},
        motion_operation("public.pick2.approach", "approach", PoseGoalDTO{Pose3DTO{}}),
        motion_operation("public.pick2.descend", "descend", CartesianPathDTO{},
                         ContactPolicyId("pick.contact"),
                         {RuntimeObjectReferenceBindingDTO{RuntimeObjectId("selected_object"),
                                                           SceneResourceId("workpiece")}}),
        tool_operation("public.pick2.tool", 0.0),
        motion_operation("public.pick2.retreat", "retreat", CartesianPathDTO{})};
  };
  auto oversized = make_intent();
  oversized.sequence_id = std::string(kMaxPickPlaceSequenceIdBytes + 1U, 'x');
  auto oversized_result = owner.PlanPick(std::move(oversized));
  ASSERT_FALSE(oversized_result.ok());
  EXPECT_EQ(oversized_result.error().details.at("reason"), "pick_place_public_intent_invalid");
  EXPECT_EQ(motion.begin_count, 0U);
  EXPECT_EQ(projection.retain_call_count, 0U);

  std::stop_source cancellation;
  cancellation.request_stop();
  auto cancelled = owner.PlanPick(make_intent(), cancellation.get_token());
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().details.at("reason"), "pick_planning_cancelled");
  EXPECT_EQ(motion.begin_count, 0U);
  EXPECT_EQ(projection.retain_call_count, 0U);
}

TEST(AddressedPickPlacePlanningOwner, ResolvesThePublicInversePlaceWithoutCommittingEffects) {
  auto predecessor = carried_predecessor_projection();
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection);
  auto approach = motion_operation("public.place.approach", "place.approach",
                                   PoseGoalDTO{Pose3DTO{{0.0, 0.0, 0.3}, {}}});
  auto descend = motion_operation(
      "public.place.descend", "place.descend",
      CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.3}, {}}, Pose3DTO{{0.0, 0.0, 0.2}, {}}}});
  auto retreat = motion_operation(
      "public.place.retreat", "place.retreat",
      CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.2}, {}}, Pose3DTO{{0.0, 0.0, 0.3}, {}}}});

  auto result = owner.PlanPlace(PlaceSequencePlanningIntentDTO{
      "public-place-1", SceneResourceId("workpiece"), FrameRoleId("frame-role:tcp"),
      std::move(approach), std::move(descend), tool_operation("public.place.tool", 0.04),
      std::move(retreat)});

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().status, PickPlacePlanningStatusDTO::kPlanned);
  ASSERT_TRUE(result.value().plan.has_value());
  EXPECT_EQ(scene.commit_count, 0U);
  EXPECT_EQ(projection.retain_call_count, 1U);
  EXPECT_EQ(projection.release_call_count, 1U);
  EXPECT_EQ(result.value().plan->kind, PickPlaceExecutionKindDTO::kPlace);
  EXPECT_EQ(result.value().plan->selected_object_id.value(), "workpiece");
  EXPECT_TRUE(std::holds_alternative<PoseGoalDTO>(result.value().plan->approach_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().plan->descend_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().plan->retreat_motion));
}

TEST(AddressedPickPlacePlanningOwner, AcceptsNewerSameLineageProjectionBeyondCaptureFloorForPlace) {
  auto predecessor = carried_predecessor_projection();
  auto successor = successor_projection(predecessor);
  const auto capture_floor = predecessor.projection_binding.projection_revision;
  FakeMotionPreviewOwner motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  ReplacingPlanningSceneSynchronizer synchronizer(scene, projection, successor, capture_floor);
  AddressedPickPlacePlanningOwner owner(motion, scene, scene, projection, &synchronizer);
  auto approach = motion_operation("floor.place.approach", "place.approach",
                                  PoseGoalDTO{Pose3DTO{{0.0, 0.0, 0.3}, {}}});
  auto descend = motion_operation(
      "floor.place.descend", "place.descend",
      CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.3}, {}}, Pose3DTO{{0.0, 0.0, 0.2}, {}}}});
  auto retreat = motion_operation(
      "floor.place.retreat", "place.retreat",
      CartesianPathDTO{.waypoints = {Pose3DTO{{0.0, 0.0, 0.2}, {}}, Pose3DTO{{0.0, 0.0, 0.3}, {}}}});

  const auto result = owner.PlanPlace(PlaceSequencePlanningIntentDTO{
      "floor-place-1", SceneResourceId("workpiece"), FrameRoleId("frame-role:tcp"),
      std::move(approach), std::move(descend), tool_operation("floor.place.tool", 0.04),
      std::move(retreat)});

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().plan.has_value());
  EXPECT_EQ(result.value().status, PickPlacePlanningStatusDTO::kPlanned);
  EXPECT_EQ(synchronizer.call_count, 1U);
  const auto& planned = *result.value().plan;
  EXPECT_EQ(planned.kind, PickPlaceExecutionKindDTO::kPlace);
  EXPECT_EQ(planned.predecessor_projection_revision,
            successor.projection_binding.projection_revision);
  EXPECT_EQ(planned.predecessor_projection_revision.scene_revision.revision_id,
            capture_floor.scene_revision.revision_id + 1U);
  ASSERT_TRUE(scene.last_request);
  EXPECT_EQ(scene.last_request->request.expected_projection_revision,
            successor.projection_binding.projection_revision);
  EXPECT_EQ(planned.preview_successor_projection_revision.scene_revision.revision_id,
            successor.projection_binding.projection_revision.scene_revision.revision_id + 1U);
  EXPECT_EQ(scene.commit_count, 0U);
  EXPECT_EQ(projection.retain_call_count, 1U);
  EXPECT_EQ(projection.release_call_count, 1U);
}

}  // namespace
}  // namespace manyforge::core
