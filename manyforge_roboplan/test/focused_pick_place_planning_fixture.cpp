#include "focused_pick_place_planning_fixture.hpp"

#include <algorithm>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend::process_fixture {
namespace {

using namespace manyforge::core;

constexpr char kDigest3[] =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr char kDigest4[] =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";
constexpr char kDigest5[] =
    "sha256:5555555555555555555555555555555555555555555555555555555555555555";

[[nodiscard]] SceneObjectSnapshotDTO
BoxObject(std::string id, SceneObjectPlacementDTO placement) {
  return {SceneResourceId(std::move(id)),
          {BoxShapeDTO{Vector3DTO{0.2, 0.3, 0.4}}},
          std::move(placement),
          std::nullopt,
          {}};
}

[[nodiscard]] CollisionWorldLeaseInventory CombinedProjection() {
  auto binding = testing::make_test_projection_binding();
  auto logical_scene = *testing::make_test_logical_scene(binding);
  const CarriedObjectAttachmentId attachment_id("attachment:carried-workpiece");
  const CarriedObjectAttachmentRevision attachment_revision(
      "attachment-revision:1");
  logical_scene.objects = {
      BoxObject(
          "carried-workpiece",
          CarriedSceneObjectPlacementDTO{attachment_id, attachment_revision}),
      BoxObject("workpiece", CellFixedSceneObjectPlacementDTO{
                                 CellId("cell:test"), Pose3DTO{}})};
  logical_scene.carried_object_attachments = {CarriedObjectAttachmentDTO{
      attachment_id,
      attachment_revision,
      SceneResourceId("carried-workpiece"),
      ControllableComponentId("component:test"),
      CommandTargetId("target:test"),
      ComponentSceneFrameRefDTO{ControllableComponentId("component:test"),
                                ModelInstanceId("model:test"),
                                FrameRoleId("frame-role:tcp")},
      Pose3DTO{{0.0, 0.0, 0.2}, {}},
      {CollisionBodyRoleId("collision-body-role:tool")}}};
  auto world = std::make_shared<CollisionWorldSnapshot>();
  world->publisher_id = "pick-place-process-fixture";
  world->revision = {1U, kDigest3};
  return {
      binding,
      std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene)),
      10,
      {std::move(world),
       testing::make_test_logical_scene(binding)->configured_collision_policy}};
}

[[nodiscard]] RuntimeParticipantBindingDTO ArmBinding() {
  std::vector<ProgramFrameBindingDTO> frames{
      {{ControllableComponentId("component:test"), FrameRoleId("frame-role:base")}, ModelInstanceId("model:test"),
       ModelRevision("model-revision:test"), "base"},
      {{ControllableComponentId("component:test"), FrameRoleId("frame-role:tcp")}, ModelInstanceId("model:test"),
       ModelRevision("model-revision:test"), "tcp"}};
  std::vector<ProgramCollisionBodyBindingDTO> collision_bodies{
      {{ControllableComponentId("component:test"), CollisionBodyRoleId("collision-body-role:tool")},
       ModelInstanceId("model:test"),
       ModelRevision("model-revision:test"),
       {"tcp"}}};
  std::vector<ProgramContactGraspBindingDTO> contact_grasps{
      {ContactPolicyId("pick.contact"),
       ContactPolicyRevision("contact-policy-revision:1"),
       RuntimeObjectId("selected_object"),
       {ProgramContactGraspBodyBindingDTO{
           {ControllableComponentId("component:test"), CollisionBodyRoleId("collision-body-role:tool")},
           ModelRoleId("tool"),
           ModelInstanceId("model:test"),
           ModelRevision("model-revision:test"),
           {CollisionBodyId("body:tool")}}},
       ContactGraspEnvelopeDTO{0.001, 0.8}}};
  return {ProgramId("program:test"),
          ProgramRevision("program-revision:test"),
          ProgramBindingId("program-binding:test"),
          RetainedBundleId("bundle:test"),
          GraphRevision("graph:test"),
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
}

[[nodiscard]] RuntimeParticipantBindingDTO CarrierBinding() {
  auto value = ArmBinding();
  value.capability_id = CapabilityId("tool.gripper.command");
  return value;
}

class MotionPreviewSession final : public IAddressedMotionPreviewSession {
public:
  Result<RuntimeParticipantBindingDTO>
  resolve_binding(const OperationIntentDTO &operation) const override {
    if (operation.participants.size() != 1U) {
      return Result<RuntimeParticipantBindingDTO>::Err(
          ErrorDTO{"invalid_request", "fixture requires one participant", {}});
    }
    return Result<RuntimeParticipantBindingDTO>::Ok(
        std::holds_alternative<CapabilityCommandIntentDTO>(
            operation.participants.front().payload)
            ? CarrierBinding()
            : ArmBinding());
  }

  Result<ExecuteRequestDTO>
  prepare_motion(const OperationIntentDTO &operation) override {
    if (operation.participants.size() != 1U) {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture requires one participant", {}});
    }
    const auto *motion = std::get_if<MotionParticipantIntentDTO>(
        &operation.participants.front().payload);
    if (motion == nullptr) {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture requires motion", {}});
    }
    auto request = testing::make_test_execute_request(
        {"joint1", "joint2"}, operation.program_run_id.value());
    request.move_id = motion->move_id.value();
    request.tolerances = motion->tolerances;
    request.speed_profile = motion->speed_profile;
    request.timeouts = motion->timeouts;
    request.cache_mode = motion->cache_mode;
    request.planner = motion->planner;
    request.target_mode = motion->target_mode;
    request.target_reference = motion->target_reference;
    request.offset_reference = motion->offset_reference;
    request.goal_frame_reference = motion->goal_frame_reference;
    request.contact_policy_id = motion->contact_policy_id.has_value()
                                    ? motion->contact_policy_id->value()
                                    : std::string{};
    request.runtime_object_bindings = motion->runtime_object_bindings;
    if (const auto *pose = std::get_if<PoseGoalDTO>(&motion->motion)) {
      request.motion = *pose;
    } else if (const auto *path =
                   std::get_if<CartesianPathDTO>(&motion->motion)) {
      request.motion = *path;
    } else {
      return Result<ExecuteRequestDTO>::Err(
          ErrorDTO{"invalid_request", "fixture motion is unsupported", {}});
    }
    if (motion->contact_policy_id.has_value() &&
        !motion->runtime_object_bindings.empty()) {
      request.resolved_contact_grasp = ResolvedContactGraspBindingDTO{
          *motion->contact_policy_id,
          ContactPolicyRevision("contact-policy-revision:1"),
          motion->runtime_object_bindings.front().runtime_object_id,
          motion->runtime_object_bindings.front().scene_resource_id,
          {CollisionBodyId("body:tool")},
          ContactGraspEnvelopeDTO{0.001, 0.8}};
    }
    return Result<ExecuteRequestDTO>::Ok(std::move(request));
  }

  Result<RobotStateDTO> current_state() const override {
    return Result<RobotStateDTO>::Ok(
        RobotStateDTO{{"joint1", "joint2"}, {0.0, 0.0}, {}});
  }

  Result<std::vector<Pose3DTO>> resolve_reference_poses(
      const OperationIntentDTO &, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory &, const RobotStateDTO &,
      std::vector<RuntimeObjectReferenceBindingDTO>) override {
    std::vector<Pose3DTO> resolved;
    resolved.reserve(references.size());
    for (const auto &reference : references) {
      resolved.push_back(reference.pose.value_or(Pose3DTO{
          {},
          QuaternionDTO{0.0, 0.0, 0.7071067811865475, 0.7071067811865476}}));
    }
    return Result<std::vector<Pose3DTO>>::Ok(std::move(resolved));
  }

  Result<MotionPreviewResultDTO>
  preview_motion(OperationIntentDTO operation,
                 CollisionWorldLeaseInventory projection_inventory,
                 RobotStateDTO start_state, std::uint32_t,
                 std::stop_token,
                 std::optional<MotionPathPreview>* path_preview) override {
    if (path_preview != nullptr) path_preview->reset();
    auto prepared = prepare_motion(operation);
    if (!prepared.ok()) {
      return Result<MotionPreviewResultDTO>::Err(prepared.error());
    }
    const auto ordinal = ++preview_count_;
    auto request = std::move(prepared.value());
    if (request.move_id.find("fixture_no_feasible") != std::string::npos) {
      return Result<MotionPreviewResultDTO>::Err(
          ErrorDTO{"planning_failed",
                   "fixture candidate is intentionally infeasible",
                   {{"failure_stage", "planning"},
                    {"reason", "fixture_no_feasible_candidate"}}});
    }
    std::vector<double> final_positions(start_state.positions_rad.size(),
                                        static_cast<double>(ordinal));
    RobotStateDTO final_state{start_state.joint_names, final_positions, {}};
    TrajectoryDTO trajectory{
        request.binding.resources,
        start_state.joint_names,
        {{0.0, start_state.positions_rad, {}, {}},
         {static_cast<double>(ordinal), final_positions, {}, {}}}};
    return Result<MotionPreviewResultDTO>::Ok(MotionPreviewResultDTO{
        std::move(trajectory), std::move(start_state), final_state,
        testing::make_test_controlled_state(request.binding, final_positions),
        Pose3DTO{{0.0, 0.0, static_cast<double>(ordinal)}, {}},
        projection_inventory.projection_binding,
        projection_inventory.bundle.world->revision, 0U});
  }

private:
  std::size_t preview_count_{};
};

class MotionPreviewOwner final : public IAddressedMotionPreviewOwner {
public:
  Result<std::unique_ptr<IAddressedMotionPreviewSession>>
  begin_motion_preview_session(const OperationIntentDTO &) override {
    std::unique_ptr<IAddressedMotionPreviewSession> session =
        std::make_unique<MotionPreviewSession>();
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Ok(
        std::move(session));
  }
};

class SceneOwner final : public ISceneCandidatePreview, public ISceneAuthority {
public:
  explicit SceneOwner(CollisionWorldLeaseInventory predecessor)
      : predecessor_(std::move(predecessor)) {}

  Result<SceneCandidatePreviewDTO> PreviewResolvedSceneCandidate(
      const ResolvedSceneCandidatePreviewRequestDTO &request,
      const CollisionWorldLeaseInventory&) const override {
    const auto *attach =
        std::get_if<AttachCarriedObjectRequestDTO>(&request.request.operation);
    const auto *detach =
        std::get_if<DetachCarriedObjectRequestDTO>(&request.request.operation);
    if ((attach == nullptr) == (detach == nullptr) ||
        !request.frame_resolution.has_value()) {
      return Result<SceneCandidatePreviewDTO>::Err(
          ErrorDTO{"invalid_request", "fixture candidate is incomplete", {}});
    }

    auto logical_scene = *predecessor_.logical_scene;
    const auto selected_id =
        attach != nullptr ? attach->scene_resource_id : [&]() {
          const auto found =
              std::find_if(logical_scene.carried_object_attachments.begin(),
                           logical_scene.carried_object_attachments.end(),
                           [&](const auto &candidate) {
                             return candidate.carried_object_attachment_id ==
                                    detach->carried_object_attachment_id;
                           });
          return found == logical_scene.carried_object_attachments.end()
                     ? SceneResourceId(std::string{})
                     : found->scene_resource_id;
        }();
    const auto selected = std::lower_bound(
        logical_scene.objects.begin(), logical_scene.objects.end(), selected_id,
        [](const auto &candidate, const auto &id) {
          return candidate.scene_resource_id < id;
        });
    if (selected == logical_scene.objects.end() ||
        selected->scene_resource_id != selected_id) {
      return Result<SceneCandidatePreviewDTO>::Err(ErrorDTO{
          "missing_reference", "fixture selected object is absent", {}});
    }

    auto proposed = predecessor_.projection_binding.projection_revision;
    ++proposed.scene_revision.revision_id;
    proposed.scene_revision.content_hash = ContentDigest(kDigest4);
    logical_scene.revision = proposed.scene_revision;
    ++logical_scene.transaction_sequence;
    ++logical_scene.topology_epoch;
    std::optional<ResolvedSceneOperationDTO> resolved_operation;
    if (attach != nullptr) {
      const CarriedObjectAttachmentRevision attachment_revision(
          "attachment-revision:planned");
      selected->placement = CarriedSceneObjectPlacementDTO{
          attach->carried_object_attachment_id, attachment_revision};
      const auto &resolved = std::get<ResolvedComponentModelFrameDTO>(
          request.frame_resolution->resolved_frame);
      CarriedObjectAttachmentDTO attachment{
          attach->carried_object_attachment_id,
          attachment_revision,
          attach->scene_resource_id,
          attach->controllable_component_id,
          attach->command_target_id,
          ComponentSceneFrameRefDTO{attach->controllable_component_id,
                                    resolved.model_instance_id,
                                    attach->frame_role_id},
          Pose3DTO{},
          attach->touch_body_roles};
      logical_scene.carried_object_attachments.push_back(attachment);
      std::sort(logical_scene.carried_object_attachments.begin(),
                logical_scene.carried_object_attachments.end(),
                [](const auto &left, const auto &right) {
                  return left.carried_object_attachment_id <
                         right.carried_object_attachment_id;
                });
      resolved_operation.emplace(
          ResolvedAttachCarriedObjectDTO{*selected, std::move(attachment)});
    } else {
      const auto attachment =
          std::find_if(logical_scene.carried_object_attachments.begin(),
                       logical_scene.carried_object_attachments.end(),
                       [&](const auto &candidate) {
                         return candidate.carried_object_attachment_id ==
                                detach->carried_object_attachment_id;
                       });
      if (attachment == logical_scene.carried_object_attachments.end()) {
        return Result<SceneCandidatePreviewDTO>::Err(ErrorDTO{
            "missing_reference", "fixture carried attachment is absent", {}});
      }
      const auto attachment_revision =
          attachment->carried_object_attachment_revision;
      selected->placement =
          CellFixedSceneObjectPlacementDTO{logical_scene.cell_id, detach->pose};
      logical_scene.carried_object_attachments.erase(attachment);
      resolved_operation.emplace(ResolvedDetachCarriedObjectDTO{
          *selected, detach->carried_object_attachment_id,
          attachment_revision});
    }
    auto snapshot = std::make_shared<const LogicalSceneSnapshotDTO>(
        std::move(logical_scene));
    SceneCandidatePreviewDTO candidate{
        ContentDigest(kDigest3),
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
    last_preview_ = candidate;
    return Result<SceneCandidatePreviewDTO>::Ok(std::move(candidate));
  }

  SceneCommitResultDTO
  Commit(const SceneTransactionRequestDTO &request) override {
    if (!last_preview_.has_value() ||
        request.expected_projection_revision !=
            predecessor_.projection_binding.projection_revision ||
        last_preview_->prior_projection_revision !=
            request.expected_projection_revision) {
      return {request.request_context.request_id,
              RequestReplayStatusDTO::kAcceptedNew,
              false,
              ErrorDTO{"scene_commit_failed",
                       "fixture scene preview is stale",
                       {{"reason", "fixture_scene_preview_stale"}}},
              std::nullopt,
              std::nullopt,
              SceneProjectionCommitStatusDTO::kFailedProjectionUncertain,
              std::nullopt};
    }
    predecessor_.projection_binding.projection_revision =
        last_preview_->proposed_projection_revision;
    predecessor_.logical_scene = last_preview_->logical_scene;
    predecessor_.bundle.policy = last_preview_->effective_collision_policy;
    const auto committed = last_preview_->proposed_projection_revision;
    last_preview_.reset();
    return {request.request_context.request_id,
            RequestReplayStatusDTO::kAcceptedNew,
            true,
            std::nullopt,
            1U,
            committed,
            SceneProjectionCommitStatusDTO::kCommitted,
            SceneUpdateStatusDTO::kGeometryChanged};
  }

  RequestOwnerEpoch CurrentRequestOwnerEpoch() const override {
    return RequestOwnerEpoch("scene.owner.epoch:test");
  }

  ProjectionRevisionDTO CurrentProjectionRevision() const override {
    return predecessor_.projection_binding.projection_revision;
  }

  std::shared_ptr<const LogicalSceneSnapshotDTO>
  LogicalSnapshot() const override {
    return predecessor_.logical_scene;
  }

  std::vector<SceneObjectSnapshotDTO> GetSceneObjects() const override {
    return predecessor_.logical_scene->objects;
  }

  SceneObjectStateDTO GetObjectState(const SceneResourceId &id) const override {
    const auto found =
        std::lower_bound(predecessor_.logical_scene->objects.begin(),
                         predecessor_.logical_scene->objects.end(), id,
                         [](const auto &candidate, const auto &selected) {
                           return candidate.scene_resource_id < selected;
                         });
    if (found == predecessor_.logical_scene->objects.end() ||
        found->scene_resource_id != id) {
      return {id, false, false, std::nullopt, std::nullopt};
    }
    const auto *carried =
        std::get_if<CarriedSceneObjectPlacementDTO>(&found->placement);
    return {
        id, true, carried != nullptr,
        carried == nullptr
            ? std::optional<CarriedObjectAttachmentId>{}
            : std::optional<
                  CarriedObjectAttachmentId>{carried
                                                 ->carried_object_attachment_id},
        std::nullopt};
  }

  Result<Pose3DTO> GetObjectPose(const SceneResourceId &id) const override {
    const auto found =
        std::lower_bound(predecessor_.logical_scene->objects.begin(),
                         predecessor_.logical_scene->objects.end(), id,
                         [](const auto &candidate, const auto &selected) {
                           return candidate.scene_resource_id < selected;
                         });
    if (found == predecessor_.logical_scene->objects.end() ||
        found->scene_resource_id != id) {
      return Result<Pose3DTO>::Err(
          ErrorDTO{"object_not_found", "fixture object is absent", {}});
    }
    if (const auto *fixed =
            std::get_if<CellFixedSceneObjectPlacementDTO>(&found->placement)) {
      return Result<Pose3DTO>::Ok(fixed->pose_in_cell);
    }
    return Result<Pose3DTO>::Ok(Pose3DTO{});
  }

private:
  mutable CollisionWorldLeaseInventory predecessor_;
  mutable std::optional<SceneCandidatePreviewDTO> last_preview_;
};

class SceneProjection final : public IPlannerSceneProjection {
public:
  explicit SceneProjection(CollisionWorldLeaseInventory predecessor)
      : predecessor_(std::move(predecessor)) {}

  ProjectionCommitResultDTO
  ApplyTransaction(const SceneTransactionDTO &) override {
    throw std::logic_error("fixture projection commit is unavailable");
  }

  Result<CollisionWorldLeaseInventory>
  RetainProjection(const ProjectionRevisionDTO &revision,
                   ProjectionLeaseId lease_id) override {
    if (revision != predecessor_.projection_binding.projection_revision) {
      return Result<CollisionWorldLeaseInventory>::Err(
          ErrorDTO{"stale_revision", "fixture projection is stale", {}});
    }
    auto retained = predecessor_;
    retained.projection_binding.projection_lease_id = std::move(lease_id);
    retained_lease_ = retained.projection_binding.projection_lease_id;
    return Result<CollisionWorldLeaseInventory>::Ok(std::move(retained));
  }

  Result<CollisionWorldLeaseInventory>
  RetainCurrentProjection(ProjectionLeaseId lease_id) override {
    return RetainProjection(predecessor_.projection_binding.projection_revision,
                            std::move(lease_id));
  }

  void ReleaseProjection(const ProjectionLeaseId &lease_id) override {
    if (retained_lease_ == std::optional(lease_id)) {
      retained_lease_.reset();
    }
  }

  ProjectionHealthDTO ProjectionHealth() const override {
    throw std::logic_error("fixture projection health is unavailable");
  }
  ProjectionCommitResultDTO
  RebuildProjection(const NativeProjectionRebuildDTO &) override {
    throw std::logic_error("fixture projection rebuild is unavailable");
  }
  NativeProjectionBootstrapRebuildResultDTO RebuildBootstrapProjection(
      const NativeProjectionBootstrapRebuildDTO &) override {
    throw std::logic_error("fixture bootstrap projection is unavailable");
  }
  NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const NativeProjectionBootstrapStatusQueryDTO &) const override {
    throw std::logic_error("fixture bootstrap status is unavailable");
  }
  Result<SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(const SceneFrameResolutionQueryDTO &query) const override {
    if (!query.valid() ||
        !std::holds_alternative<TargetCarrierSceneFrameRefDTO>(
            query.selector)) {
      return Result<SceneFrameResolutionReceiptDTO>::Err(
          ErrorDTO{"invalid_request", "fixture frame query is invalid", {}});
    }
    const auto &selector =
        std::get<TargetCarrierSceneFrameRefDTO>(query.selector);
    return Result<SceneFrameResolutionReceiptDTO>::Ok(
        SceneFrameResolutionReceiptDTO{
            ResolvedComponentModelFrameDTO{
                selector.controllable_component_id,
                ModelInstanceId("model:test"), selector.frame_role_id,
                selector.frame_role_id == FrameRoleId("frame-role:base")
                    ? "base"
                    : "tcp",
                ModelRevision("model-revision:test")},
            Pose3DTO{},
            {},
            query.projection_binding,
            std::nullopt,
            ContentDigest(kDigest5)});
  }

  Result<SceneFrameResolutionReceiptDTO> ResolveCandidateSceneFrame(
      const CandidateSceneFrameResolutionQueryDTO &query) const override {
    if (!std::holds_alternative<TargetCarrierSceneFrameRefDTO>(
            query.selector)) {
      return Result<SceneFrameResolutionReceiptDTO>::Err(ErrorDTO{
          "invalid_request", "fixture candidate frame query is invalid", {}});
    }
    const auto &selector =
        std::get<TargetCarrierSceneFrameRefDTO>(query.selector);
    return Result<SceneFrameResolutionReceiptDTO>::Ok(
        SceneFrameResolutionReceiptDTO{
            ResolvedComponentModelFrameDTO{
                selector.controllable_component_id,
                ModelInstanceId("model:test"), selector.frame_role_id, "tcp",
                ModelRevision("model-revision:test")},
            Pose3DTO{},
            {},
            query.projection_binding,
            ContentDigest(kDigest4),
            ContentDigest(kDigest5)});
  }

  Result<CollisionWorldLeaseInventory>
  ProjectSceneCandidate(const SceneCandidatePreviewDTO &candidate,
                        const ProjectionLeaseBindingDTO &predecessor_binding,
                        ProjectionLeaseId candidate_lease_id) const override {
    if (!candidate.valid() ||
        predecessor_binding.projection_revision !=
            predecessor_.projection_binding.projection_revision) {
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

private:
  CollisionWorldLeaseInventory predecessor_;
  std::optional<ProjectionLeaseId> retained_lease_;
};

struct Ports final {
  Ports()
      : predecessor(CombinedProjection()), scene(predecessor),
        projection(predecessor) {}

  CollisionWorldLeaseInventory predecessor;
  MotionPreviewOwner motion;
  SceneOwner scene;
  SceneProjection projection;
};

} // namespace

FocusedPickPlacePlanningFixture MakeFocusedPickPlacePlanningFixture() {
  auto ports = std::make_shared<Ports>();
  auto owner = std::make_unique<AddressedPickPlacePlanningOwner>(
      ports->motion, ports->scene, ports->scene, ports->projection);
  return {std::move(ports), std::move(owner)};
}

} // namespace manyforge::roboplan_backend::process_fixture
