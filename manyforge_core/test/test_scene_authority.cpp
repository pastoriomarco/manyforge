#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/scene_authority.hpp>

namespace manyforge::core {
namespace {

ErrorDTO fake_error(std::string reason) {
  return ErrorDTO{to_string(ErrorCode::kInternalError),
                  "fake projection failure", {{"reason", std::move(reason)}}};
}

class FakeProjection final : public IPlannerSceneProjection,
                             public ICollisionWorldSnapshotPublisher {
 public:
  enum class NextCommit {
    kSuccessGeometry,
    kSuccessFreshness,
    kSuccessNoOp,
    kFailedRolledBack,
    kFailedUncertain,
    kSuccessPublicationMismatch,
    kSuccessDuplicateObject,
    kSuccessConfiguredPolicyMismatch,
  };

  FakeProjection() {
    SceneRobotPlacementDTO placement;
    placement.robot_id = "robot0";
    placement.pose_in_world.orientation.w = 1.0;
    robot_placements_.push_back(std::move(placement));
    publish_locked({1, "fake-1"});
  }

  SceneProjectionCommitResult apply_transaction(
      const SceneTransactionDTO& transaction,
      const SceneRevisionDTO& proposed_revision) override {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      std::unique_lock<std::mutex> gate_lock(apply_gate_mutex_);
      if (block_next_apply_) {
        apply_blocked_ = true;
        apply_gate_cv_.notify_all();
        apply_gate_cv_.wait(gate_lock, [&] { return release_blocked_apply_; });
        block_next_apply_ = false;
        apply_blocked_ = false;
        release_blocked_apply_ = false;
      }
    }
    ++apply_calls;
    SceneProjectionCommitResult result;
    result.acknowledged_revision = revision_;
    if (transaction.expected_revision.revision_id != revision_.revision_id ||
        transaction.expected_revision.content_hash != revision_.content_hash) {
      result.edit.error = fake_error("fake_expected_revision_mismatch");
      return result;
    }
    if (next_commit == NextCommit::kFailedRolledBack ||
        next_commit == NextCommit::kFailedUncertain) {
      result.edit.error = fake_error("fake_commit_failed");
      result.rollback_proven = next_commit == NextCommit::kFailedRolledBack;
      next_commit = NextCommit::kSuccessGeometry;
      return result;
    }

    result.edit.success = true;
    if (next_commit == NextCommit::kSuccessFreshness) {
      result.edit.update_status = "freshness_only";
      result.edit.collision_world_freshness_changed = true;
      result.acknowledged_revision = revision_;
    } else if (next_commit == NextCommit::kSuccessNoOp) {
      result.edit.update_status = "no_op";
      result.acknowledged_revision = revision_;
    } else {
      apply_operation_locked(transaction.operation);
      if (next_commit == NextCommit::kSuccessConfiguredPolicyMismatch) {
        configured_policy_.policy_id = "projection_wrong_policy";
      }
      SceneRevisionDTO next = proposed_revision;
      next.content_hash = "fake-" + std::to_string(next.revision_id);
      if (next_commit == NextCommit::kSuccessPublicationMismatch) {
        result.acknowledged_revision = next;
      } else {
        publish_locked(next);
        result.acknowledged_revision = revision_;
      }
      result.edit.update_status = "geometry_changed";
      result.edit.collision_world_revision_changed = true;
      if (next_commit == NextCommit::kSuccessDuplicateObject &&
          !objects_.empty()) {
        objects_.push_back(objects_.front());
      }
    }
    next_commit = NextCommit::kSuccessGeometry;
    return result;
  }

  Result<CollisionWorldLeaseInventory> retain_projection(
      const SceneRevisionDTO& revision,
      const std::string& retention_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++retain_calls;
    if (revision.revision_id != revision_.revision_id ||
        revision.content_hash != revision_.content_hash) {
      return Result<CollisionWorldLeaseInventory>::Err(
          fake_error("fake_retention_revision_mismatch"));
    }
    retained_ids.insert(retention_id);
    auto inventory = inventory_;
    inventory.retention_id = retention_id;
    return Result<CollisionWorldLeaseInventory>::Ok(std::move(inventory));
  }

  void release_projection(const std::string& retention_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++release_calls;
    retained_ids.erase(retention_id);
  }

  SceneProjectionHealthDTO projection_health() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_;
  }

  SceneProjectionCommitResult rebuild_projection(
      const LogicalSceneSnapshot& snapshot,
      const SceneRevisionDTO& proposed_revision) override {
    std::lock_guard<std::mutex> lock(mutex_);
    robot_placements_ = snapshot.robot_placements;
    objects_ = snapshot.objects;
    configured_policy_ = snapshot.configured_collision_policy;
    publish_locked(proposed_revision);
    inventory_.topology_epoch += rebuild_topology_epoch_delta;
    inventory_.policy_epoch += rebuild_policy_epoch_delta;
    health_ = {SceneProjectionHealth::kReady, revision_, {}};
    SceneProjectionCommitResult result;
    result.edit.success = true;
    result.edit.update_status = "geometry_changed";
    result.edit.collision_world_revision_changed = true;
    result.acknowledged_revision = revision_;
    return result;
  }

  std::vector<SceneObjectSnapshotDTO> scene_objects() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return objects_;
  }

  std::vector<SceneRobotPlacementDTO> robot_placements() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return robot_placements_;
  }

  CollisionPolicySnapshot configured_collision_policy() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return configured_policy_;
  }

  Result<Pose3DTO> object_pose(const std::string& object_id,
                              const std::string&,
                              const SceneRevisionDTO& expected_revision)
      const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_revision.revision_id != revision_.revision_id ||
        expected_revision.content_hash != revision_.content_hash) {
      return Result<Pose3DTO>::Err(
          fake_error("fake_object_pose_revision_mismatch"));
    }
    const auto object = std::find_if(
        objects_.begin(), objects_.end(), [&](const auto& candidate) {
          return candidate.object_id == object_id;
        });
    if (object == objects_.end()) {
      return Result<Pose3DTO>::Err(fake_error("fake_object_missing"));
    }
    return Result<Pose3DTO>::Ok(object->pose_in_universe);
  }

  CollisionWorldSnapshotPtr snapshot() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return world_;
  }

  CollisionWorldRevision published_collision_world_revision()
      const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
  }

  std::optional<CollisionPolicySnapshot> collision_policy_snapshot()
      const override {
    return CollisionPolicySnapshot{};
  }

  CollisionWorldPolicySnapshotBundle collision_world_policy_snapshot_bundle()
      const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return bundle_;
  }

  std::optional<CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return inventory_;
  }

  RuntimeCollisionQueryBundlePtr published_runtime_collision_query_bundle()
      const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_;
  }

  void block_next_apply() {
    std::lock_guard<std::mutex> lock(apply_gate_mutex_);
    block_next_apply_ = true;
  }

  void wait_until_apply_blocked() {
    std::unique_lock<std::mutex> lock(apply_gate_mutex_);
    apply_gate_cv_.wait(lock, [&] { return apply_blocked_; });
  }

  void release_blocked_apply() {
    std::lock_guard<std::mutex> lock(apply_gate_mutex_);
    release_blocked_apply_ = true;
    apply_gate_cv_.notify_all();
  }

  void mutate_projection_object_without_publication(std::string object_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    SceneObjectSnapshotDTO object;
    object.object_id = std::move(object_id);
    object.frame_id = "universe";
    objects_.push_back(std::move(object));
  }

  NextCommit next_commit = NextCommit::kSuccessGeometry;
  std::uint64_t rebuild_topology_epoch_delta = 0;
  std::uint64_t rebuild_policy_epoch_delta = 0;
  std::atomic<std::uint64_t> apply_calls{0};
  std::atomic<std::uint64_t> retain_calls{0};
  std::atomic<std::uint64_t> release_calls{0};
  std::set<std::string> retained_ids;

 private:
  std::mutex apply_gate_mutex_;
  std::condition_variable apply_gate_cv_;
  bool block_next_apply_ = false;
  bool apply_blocked_ = false;
  bool release_blocked_apply_ = false;
  void apply_operation_locked(const SceneOperationDTO& operation) {
    std::visit(
        [&](const auto& request) {
          using Request = std::decay_t<decltype(request)>;
          if constexpr (std::is_same_v<Request, AddObjectRequestDTO>) {
            SceneObjectSnapshotDTO object;
            object.object_id = request.object_id;
            object.shape = request.shape;
            object.pose_in_frame = request.pose;
            object.pose_in_universe = request.pose;
            object.frame_id = request.frame_id;
            object.support_contacts = request.support_contacts;
            objects_.push_back(std::move(object));
          } else if constexpr (std::is_same_v<Request,
                                              RemoveObjectRequestDTO>) {
            std::erase_if(objects_, [&](const auto& object) {
              return object.object_id == request.object_id;
            });
          } else if constexpr (std::is_same_v<
                                   Request, UpdateObjectPoseRequestDTO>) {
            const auto object = std::find_if(
                objects_.begin(), objects_.end(), [&](const auto& candidate) {
                  return candidate.object_id == request.object_id;
                });
            if (object != objects_.end()) {
              object->pose_in_frame = request.pose;
              object->pose_in_universe = request.pose;
              object->frame_id = request.frame_id;
              object->attached = request.frame_id != "universe";
            }
          } else if constexpr (std::is_same_v<Request,
                                              AttachObjectRequestDTO>) {
            const auto object = std::find_if(
                objects_.begin(), objects_.end(), [&](const auto& candidate) {
                  return candidate.object_id == request.object_id;
                });
            if (object != objects_.end()) {
              object->frame_id = request.frame_id;
              object->attached = true;
              object->touch_links = request.touch_links;
            }
          } else if constexpr (std::is_same_v<Request,
                                              DetachObjectRequestDTO>) {
            const auto object = std::find_if(
                objects_.begin(), objects_.end(), [&](const auto& candidate) {
                  return candidate.object_id == request.object_id;
                });
            if (object != objects_.end()) {
              object->frame_id = request.frame_id;
              object->attached = false;
              object->touch_links.clear();
            }
          } else if constexpr (std::is_same_v<
                                   Request,
                                   SetRobotBasePoseRequestDTO>) {
            const auto placement = std::find_if(
                robot_placements_.begin(), robot_placements_.end(),
                [&](const auto& candidate) {
                  return candidate.robot_id == request.robot_id;
                });
            if (placement != robot_placements_.end()) {
              placement->pose_in_world = request.pose_in_world;
            }
          } else if constexpr (std::is_same_v<
                                   Request,
                                   SetCollisionPolicyRequestDTO>) {
            configured_policy_ = request.policy;
          }
        },
        operation);
  }

  void publish_locked(SceneRevisionDTO revision) {
    revision_ = std::move(revision);
    auto world = std::make_shared<CollisionWorldSnapshot>();
    world->publisher_id = "fake_projection";
    world->revision = revision_;
    world_ = world;
    bundle_ = CollisionWorldPolicySnapshotBundle{world_, configured_policy_};
    inventory_ = {};
    inventory_.bundle = bundle_;
    auto runtime = std::make_shared<RuntimeCollisionQueryBundle>();
    runtime->world = world_;
    runtime->policy =
        std::make_shared<CollisionPolicySnapshot>(configured_policy_);
    runtime->distance_selection_order_valid = true;
    runtime->lookup_order_valid = true;
    runtime->epoch = revision_.revision_id;
    runtime_ = std::move(runtime);
    health_ = {SceneProjectionHealth::kReady, revision_, {}};
  }

  mutable std::mutex mutex_;
  SceneRevisionDTO revision_;
  std::vector<SceneRobotPlacementDTO> robot_placements_;
  std::vector<SceneObjectSnapshotDTO> objects_;
  CollisionWorldSnapshotPtr world_;
  CollisionWorldPolicySnapshotBundle bundle_;
  CollisionWorldLeaseInventory inventory_;
  RuntimeCollisionQueryBundlePtr runtime_;
  SceneProjectionHealthDTO health_;
  CollisionPolicySnapshot configured_policy_;
};

AddObjectRequestDTO add_box(std::string id) {
  AddObjectRequestDTO request;
  request.object_id = std::move(id);
  request.shape = BoxShapeDTO{{0.1, 0.2, 0.3}};
  request.frame_id = "universe";
  request.pose.orientation.w = 1.0;
  return request;
}

TEST(SceneAuthorityTest, PublishesOneCoherentInitialRevision) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);

  EXPECT_EQ(authority.current_revision().revision_id, 1U);
  ASSERT_NE(authority.snapshot(), nullptr);
  EXPECT_EQ(authority.snapshot()->revision.revision_id, 1U);
  ASSERT_TRUE(authority.current_scene_snapshot_lease_inventory().has_value());
  EXPECT_EQ(authority.published_runtime_collision_query_bundle()
                ->world->revision.revision_id,
            1U);
  const auto status = authority.authority_status();
  EXPECT_EQ(status.revision.revision_id, 1U);
  EXPECT_EQ(status.transaction_sequence, 0U);
  EXPECT_EQ(status.admission_epoch, 1U);
}

TEST(SceneAuthorityTest, CommitsMonotonicRevisionAndLogicalObject) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);

  const auto result = authority.add_object(add_box("box"));

  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.collision_world_revision_changed);
  EXPECT_EQ(authority.current_revision().revision_id, 2U);
  const auto objects = authority.scene_objects();
  ASSERT_EQ(objects.size(), 1U);
  EXPECT_EQ(objects.front().object_id, "box");
  const auto state = authority.object_state("box");
  EXPECT_TRUE(state.exists);
  EXPECT_EQ(state.frame_id, "universe");
  EXPECT_EQ(authority.authority_status().admission_epoch, 2U);
}

TEST(SceneAuthorityTest,
     LiveObservedPoseRevisionDoesNotInvalidateRecoveryAdmission) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  ASSERT_TRUE(authority.add_object(add_box("tracked")).success);
  const auto epoch_before = authority.authority_status().admission_epoch;

  UpdateObjectPoseRequestDTO observed;
  observed.object_id = "tracked";
  observed.frame_id = "universe";
  observed.pose.position_m.x_m = 0.25;
  observed.pose.orientation.w = 1.0;
  observed.source_id = "foundationpose:tracked";
  observed.source_timestamp_ns = 42;
  observed.source_freshness_budget_ns = 1000000000;

  const auto result = authority.update_object_pose(observed);

  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.collision_world_revision_changed);
  EXPECT_EQ(authority.authority_status().admission_epoch, epoch_before);
}

TEST(SceneAuthorityTest,
     AuthoredPoseRevisionInvalidatesRecoveryAdmission) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  ASSERT_TRUE(authority.add_object(add_box("authored")).success);
  const auto epoch_before = authority.authority_status().admission_epoch;

  UpdateObjectPoseRequestDTO authored;
  authored.object_id = "authored";
  authored.frame_id = "universe";
  authored.pose.position_m.y_m = 0.5;
  authored.pose.orientation.w = 1.0;

  const auto result = authority.update_object_pose(authored);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(authority.authority_status().admission_epoch, epoch_before + 1);
}

TEST(SceneAuthorityTest,
     AttachmentPreconditionRejectsBeforeProjectionWithoutMutation) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  ASSERT_TRUE(authority.add_object(add_box("tracked")).success);

  AttachObjectRequestDTO attach;
  attach.object_id = "tracked";
  attach.frame_id = "tool0";
  attach.touch_links = {"finger"};
  ASSERT_TRUE(authority.attach_object(attach).success);
  ASSERT_TRUE(authority.object_state("tracked").attached);

  const auto revision_before = authority.current_revision();
  const auto projection_calls_before = projection.apply_calls.load();
  UpdateObjectPoseRequestDTO stale_observation;
  stale_observation.object_id = "tracked";
  stale_observation.frame_id = "universe";
  stale_observation.pose.position_m.x_m = 0.5;
  stale_observation.pose.orientation.w = 1.0;
  stale_observation.expected_attached = false;

  const auto result = authority.update_object_pose(stale_observation);

  ASSERT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorSceneObjectStateConflict);
  EXPECT_EQ(result.error->details.at("reason"),
            "scene_object_attachment_precondition_failed");
  EXPECT_EQ(result.error->details.at("current_attached"), "true");
  EXPECT_EQ(result.error->details.at("current_frame_id"), "tool0");
  EXPECT_EQ(projection.apply_calls.load(), projection_calls_before);
  EXPECT_EQ(authority.current_revision().revision_id,
            revision_before.revision_id);
  EXPECT_EQ(authority.current_revision().content_hash,
            revision_before.content_hash);
  const auto state = authority.object_state("tracked");
  EXPECT_TRUE(state.attached);
  EXPECT_EQ(state.frame_id, "tool0");
}

TEST(SceneAuthorityTest,
     ExplicitLiveTopologyTransactionRetainsRecoveryAdmissionEpoch) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  const auto epoch_before = authority.authority_status().admission_epoch;
  SceneTransactionDTO transaction;
  transaction.expected_revision = authority.current_revision();
  transaction.operation = add_box("perception_obstacle");
  transaction.authority_source = "perception_world";
  transaction.recovery_admission_impact =
      SceneRecoveryAdmissionImpact::kLiveObservation;

  const auto result = authority.commit(std::move(transaction));

  ASSERT_TRUE(result.success);
  EXPECT_EQ(authority.authority_status().admission_epoch, epoch_before);
}

TEST(SceneAuthorityTest, NoOpDoesNotAdvanceRecoveryAdmissionEpoch) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.next_commit = FakeProjection::NextCommit::kSuccessNoOp;
  const auto epoch_before = authority.authority_status().admission_epoch;

  ASSERT_TRUE(authority.add_object(add_box("ignored")).success);

  EXPECT_EQ(authority.authority_status().admission_epoch, epoch_before);
}

TEST(SceneAuthorityTest, CommitsRobotPlacementThroughLogicalTransaction) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  SetRobotBasePoseRequestDTO request;
  request.robot_id = "robot0";
  request.pose_in_world.position_m.x_m = 1.25;
  request.pose_in_world.orientation.w = 1.0;

  const auto result = authority.set_robot_base_pose(request);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(authority.current_revision().revision_id, 2U);
  const auto placements = authority.robot_placements();
  ASSERT_EQ(placements.size(), 1U);
  EXPECT_EQ(placements.front().robot_id, "robot0");
  EXPECT_DOUBLE_EQ(placements.front().pose_in_world.position_m.x_m, 1.25);
}

TEST(SceneAuthorityTest,
     CommitsConfiguredCollisionPolicyAndRebuildPreservesIt) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  SetCollisionPolicyRequestDTO request;
  request.policy.policy_id = "qualified_policy";
  request.policy.revision = {17, "configured-policy-17"};

  ASSERT_TRUE(authority.set_collision_policy(request).success);
  auto logical = authority.logical_snapshot();
  ASSERT_NE(logical, nullptr);
  EXPECT_EQ(logical->configured_collision_policy.policy_id,
            "qualified_policy");
  EXPECT_EQ(logical->configured_collision_policy.revision.revision_id, 17U);

  projection.next_commit = FakeProjection::NextCommit::kFailedUncertain;
  ASSERT_FALSE(authority.add_object(add_box("uncertain")).success);
  const auto rebuilt = authority.rebuild_projection();

  ASSERT_TRUE(rebuilt.ok());
  EXPECT_EQ(projection.configured_collision_policy().policy_id,
            "qualified_policy");
  logical = authority.logical_snapshot();
  ASSERT_NE(logical, nullptr);
  EXPECT_EQ(logical->configured_collision_policy.revision.content_hash,
            "configured-policy-17");
}

TEST(SceneAuthorityTest,
     RejectsProjectionConfiguredPolicyMismatchBeforePublishingCommit) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  SetCollisionPolicyRequestDTO request;
  request.policy.policy_id = "authority_policy";
  request.policy.revision = {23, "authority-policy-23"};
  projection.next_commit =
      FakeProjection::NextCommit::kSuccessConfiguredPolicyMismatch;

  const auto result = authority.set_collision_policy(request);

  EXPECT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "scene_projection_configured_policy_mismatch");
  const auto logical = authority.logical_snapshot();
  ASSERT_NE(logical, nullptr);
  EXPECT_EQ(logical->configured_collision_policy.policy_id, "free_space");
  EXPECT_EQ(authority.snapshot(), nullptr);
}

TEST(SceneAuthorityTest, RejectsStaleExpectedRevisionWithoutProjectionCall) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  SceneTransactionDTO transaction;
  transaction.expected_revision = {99, "stale"};
  transaction.operation = add_box("box");

  const auto result = authority.commit(std::move(transaction));

  EXPECT_FALSE(result.success);
  EXPECT_EQ(projection.apply_calls.load(), 0U);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "scene_transaction_revision_mismatch");
}

TEST(SceneAuthorityTest,
     ConcurrentConvenienceEditsBindRevisionUnderWriterLock) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.block_next_apply();

  SceneEditResultDTO first;
  SceneEditResultDTO second;
  std::atomic<bool> second_started{false};
  std::thread first_writer([&] {
    first = authority.add_object(add_box("first"));
  });
  projection.wait_until_apply_blocked();
  std::thread second_writer([&] {
    second_started.store(true, std::memory_order_release);
    second = authority.add_object(add_box("second"));
  });
  while (!second_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Give the second writer an opportunity to reach revision binding while the
  // first projection commit is deliberately held open. Correct code blocks it
  // before that read; the old read-then-lock implementation bound revision 1.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  projection.release_blocked_apply();

  first_writer.join();
  second_writer.join();

  ASSERT_TRUE(first.success);
  ASSERT_TRUE(second.success)
      << (second.error ? second.error->message : "missing error");
  EXPECT_EQ(authority.current_revision().revision_id, 3U);
  EXPECT_EQ(projection.apply_calls.load(), 2U);
}

TEST(SceneAuthorityTest, RolledBackFailureDoesNotPoisonAuthority) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.next_commit = FakeProjection::NextCommit::kFailedRolledBack;

  EXPECT_FALSE(authority.add_object(add_box("first")).success);
  EXPECT_TRUE(authority.add_object(add_box("second")).success);
}

TEST(SceneAuthorityTest, UncertainFailurePoisonsAuthorityFailClosed) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.next_commit = FakeProjection::NextCommit::kFailedUncertain;

  EXPECT_FALSE(authority.add_object(add_box("first")).success);
  const auto second = authority.add_object(add_box("second"));

  EXPECT_FALSE(second.success);
  ASSERT_TRUE(second.error.has_value());
  EXPECT_EQ(second.error->details.at("reason"),
            "scene_projection_desynchronized");
  EXPECT_EQ(projection.apply_calls.load(), 1U);
}

TEST(SceneAuthorityTest, ExplicitRebuildRestoresCommittedProjection) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  ASSERT_TRUE(authority.add_object(add_box("committed")).success);
  projection.next_commit = FakeProjection::NextCommit::kFailedUncertain;
  ASSERT_FALSE(authority.add_object(add_box("rejected")).success);

  const auto rebuilt = authority.rebuild_projection();

  ASSERT_TRUE(rebuilt.ok());
  EXPECT_EQ(rebuilt.value().revision_id, 2U);
  EXPECT_TRUE(authority.add_object(add_box("after_rebuild")).success);
  const auto objects = authority.scene_objects();
  EXPECT_EQ(objects.size(), 2U);
  EXPECT_EQ(objects.front().object_id, "after_rebuild");
  EXPECT_EQ(objects.back().object_id, "committed");
}

TEST(SceneAuthorityTest,
     RebuildEpochMismatchCannotReplaceAuthoritativeLogicalSnapshot) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  const auto before = authority.logical_snapshot();
  ASSERT_NE(before, nullptr);
  projection.next_commit = FakeProjection::NextCommit::kFailedUncertain;
  ASSERT_FALSE(authority.add_object(add_box("uncertain")).success);
  projection.rebuild_topology_epoch_delta = 1;

  const auto rebuilt = authority.rebuild_projection();

  EXPECT_FALSE(rebuilt.ok());
  EXPECT_EQ(rebuilt.error().details.at("reason"),
            "scene_projection_epoch_mismatch");
  const auto after = authority.logical_snapshot();
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after.get(), before.get());
  EXPECT_EQ(after->topology_epoch, before->topology_epoch);
  EXPECT_EQ(authority.snapshot(), nullptr);
}

TEST(SceneAuthorityTest, PublicationMismatchPoisonsAuthorityFailClosed) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.next_commit =
      FakeProjection::NextCommit::kSuccessPublicationMismatch;

  const auto result = authority.add_object(add_box("box"));

  EXPECT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "scene_projection_collision_publication_mismatch");
  EXPECT_FALSE(authority.add_object(add_box("second")).success);
  EXPECT_EQ(authority.snapshot(), nullptr);
  EXPECT_EQ(authority.published_collision_world_revision().revision_id, 0U);
  EXPECT_FALSE(authority.collision_policy_snapshot().has_value());
  EXPECT_EQ(authority.collision_world_policy_snapshot_bundle().world, nullptr);
  EXPECT_FALSE(authority.current_scene_snapshot_lease_inventory().has_value());
  EXPECT_FALSE(authority.retain_scene_snapshot_lease_inventory("blocked")
                   .has_value());
  EXPECT_EQ(authority.published_runtime_collision_query_bundle(), nullptr);
}

TEST(SceneAuthorityTest, DuplicateLogicalIdentityPoisonsAuthority) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.next_commit = FakeProjection::NextCommit::kSuccessDuplicateObject;

  const auto result = authority.add_object(add_box("box"));

  EXPECT_FALSE(result.success);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "scene_projection_duplicate_object");
}

TEST(SceneAuthorityTest, LogicalReadsIgnoreUncommittedProjectionMutation) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.mutate_projection_object_without_publication("uncommitted");

  EXPECT_TRUE(authority.scene_objects().empty());
  EXPECT_FALSE(authority.object_state("uncommitted").exists);
}

TEST(SceneAuthorityTest, FreshnessAndNoOpDoNotAdvanceLogicalRevision) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  projection.next_commit = FakeProjection::NextCommit::kSuccessFreshness;
  EXPECT_TRUE(authority.update_object_pose({}).success);
  EXPECT_EQ(authority.current_revision().revision_id, 1U);

  projection.next_commit = FakeProjection::NextCommit::kSuccessNoOp;
  EXPECT_TRUE(authority.update_object_pose({}).success);
  EXPECT_EQ(authority.current_revision().revision_id, 1U);
}

TEST(SceneAuthorityTest, RetentionIsBoundedIdempotentAndReleasable) {
  FakeProjection projection;
  SceneAuthority authority(
      projection, projection,
      SceneAuthorityConfig{.world_frame = "universe",
                           .retained_lease_capacity = 2});

  const auto first =
      authority.retain_scene_snapshot_lease_inventory("first");
  const auto same = authority.retain_scene_snapshot_lease_inventory("first");
  const auto second =
      authority.retain_scene_snapshot_lease_inventory("second");
  const auto overflow =
      authority.retain_scene_snapshot_lease_inventory("third");

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(same.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(overflow.has_value());
  EXPECT_EQ(projection.retain_calls.load(), 2U);
  EXPECT_EQ(authority.scene_snapshot_lease_retention_stats().retained_count,
            2U);

  authority.release_scene_snapshot_lease_inventory("first");
  EXPECT_EQ(authority.scene_snapshot_lease_retention_stats().retained_count,
            1U);
  EXPECT_EQ(projection.release_calls.load(), 1U);
  EXPECT_TRUE(
      authority.retain_scene_snapshot_lease_inventory("third").has_value());
}

TEST(SceneAuthorityTest, ProjectionRebuildPreservesAuthorityOwnedLease) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  const auto retained =
      authority.retain_scene_snapshot_lease_inventory("active-plan");
  ASSERT_TRUE(retained.has_value());

  projection.next_commit = FakeProjection::NextCommit::kFailedUncertain;
  ASSERT_FALSE(authority.add_object(add_box("uncertain")).success);
  ASSERT_TRUE(authority.rebuild_projection().ok());

  EXPECT_TRUE(projection.retained_ids.contains("active-plan"));
  EXPECT_EQ(authority.scene_snapshot_lease_retention_stats().retained_count,
            1U);
  authority.release_scene_snapshot_lease_inventory("active-plan");
  EXPECT_FALSE(projection.retained_ids.contains("active-plan"));
}

TEST(SceneAuthorityTest, ConcurrentGeneratedRetentionIdsAreUnique) {
  FakeProjection projection;
  SceneAuthority authority(
      projection, projection,
      SceneAuthorityConfig{.world_frame = "universe",
                           .retained_lease_capacity = 8});
  std::mutex results_mutex;
  std::vector<std::string> ids;
  std::vector<std::jthread> workers;
  for (int index = 0; index < 8; ++index) {
    workers.emplace_back([&] {
      auto retained = authority.retain_scene_snapshot_lease_inventory();
      ASSERT_TRUE(retained.has_value());
      std::lock_guard<std::mutex> lock(results_mutex);
      ids.push_back(retained->retention_id);
    });
  }
  workers.clear();

  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids.size(), 8U);
  EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}

TEST(SceneAuthorityTest, GeneratedRetentionIdSkipsCallerOwnedId) {
  FakeProjection projection;
  SceneAuthority authority(projection, projection);
  const auto caller =
      authority.retain_scene_snapshot_lease_inventory("scene_lease_1");
  ASSERT_TRUE(caller.has_value());

  const auto generated = authority.retain_scene_snapshot_lease_inventory();

  ASSERT_TRUE(generated.has_value());
  EXPECT_EQ(generated->retention_id, "scene_lease_2");
  EXPECT_NE(generated->retention_id, caller->retention_id);
  EXPECT_EQ(authority.scene_snapshot_lease_retention_stats().retained_count,
            2U);
}

}  // namespace
}  // namespace manyforge::core
