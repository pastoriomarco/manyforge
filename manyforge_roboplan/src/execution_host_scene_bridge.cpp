#include "execution_host_scene_bridge.hpp"
#include "execution_host_commands.hpp"
#include "execution_host_posix.hpp"
#include "execution_host_responses.hpp"
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <manyforge_scene/scene_candidate_preview.hpp>
#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

namespace scene = manyforge::scene;

ManagerSceneAuthorityProxy::ManagerSceneAuthorityProxy(
    std::filesystem::path manager_run_root,
    std::shared_ptr<core::IPlannerSceneProjection> projection)
    : manager_run_root_(std::move(manager_run_root)), projection_(std::move(projection)) {}

[[nodiscard]] core::SceneCommitResultDTO
  ManagerSceneAuthorityProxy::Commit(const core::SceneTransactionRequestDTO &request) {
    const auto encoded = scene::EncodeCanonicalSceneTransactionRequest(request);
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_commit")},
            {"transaction",
             CanonicalCodecValue(encoded, "scene transaction request")},
        }));
    RequireExactObjectSize(response, 10U, "Manager scene commit response");
    Require(Text(response, "request_id") ==
                    request.request_context.request_id.value() &&
                Text(response, "request_owner_epoch") ==
                    request.request_context.request_owner_epoch.value(),
            "Manager scene commit response changed request identity");
    std::optional<core::ErrorDTO> error;
    const auto &encoded_error = Member(response, "error");
    if (!std::holds_alternative<std::nullptr_t>(encoded_error.storage())) {
      error = DecodeManagerError(encoded_error);
    }
    std::optional<std::uint64_t> transaction_sequence;
    const auto &encoded_sequence = Member(response, "transaction_sequence");
    if (!std::holds_alternative<std::nullptr_t>(encoded_sequence.storage())) {
      const auto *number = std::get_if<double>(&encoded_sequence.storage());
      Require(number != nullptr && *number >= 1.0 &&
                  *number <= kMaximumSafeInteger &&
                  std::floor(*number) == *number,
              "Manager scene transaction sequence is invalid");
      transaction_sequence = static_cast<std::uint64_t>(*number);
    }
    std::optional<core::ProjectionRevisionDTO> committed_projection;
    const auto &encoded_projection =
        Member(response, "committed_projection_revision");
    if (!std::holds_alternative<std::nullptr_t>(encoded_projection.storage())) {
      committed_projection = DecodeProjectionRevision(encoded_projection);
    }
    std::optional<core::SceneUpdateStatusDTO> update_status;
    const auto &encoded_update = Member(response, "update_status");
    if (!std::holds_alternative<std::nullptr_t>(encoded_update.storage())) {
      update_status = DecodeSceneUpdateStatus(encoded_update);
    }
    core::SceneCommitResultDTO result{
        request.request_context.request_id,
        DecodeRequestReplayStatus(response, "replay_status"),
        Boolean(response, "ok"),
        std::move(error),
        transaction_sequence,
        std::move(committed_projection),
        DecodeSceneProjectionCommitStatus(response, "projection_status"),
        update_status};
    Require(result.presence_valid(),
            "Manager scene commit response is internally inconsistent");
    if (result.success) {
      Require(
          result.committed_projection_revision->retained_bundle_id ==
                  request.expected_projection_revision.retained_bundle_id &&
              result.committed_projection_revision->graph_revision ==
                  request.expected_projection_revision.graph_revision &&
              result.committed_projection_revision->scene_revision.scene_id ==
                  request.expected_projection_revision.scene_revision.scene_id,
          "Manager scene commit changed structural or scene-lineage identity");
    }
    return result;
  }

[[nodiscard]] core::Result<core::SceneCandidatePreviewDTO>
ManagerSceneAuthorityProxy::PreviewResolvedSceneCandidate(
    const core::ResolvedSceneCandidatePreviewRequestDTO &request,
    const core::CollisionWorldLeaseInventory &predecessor) const {
  if (!projection_ || !predecessor.logical_scene) {
    return core::Result<core::SceneCandidatePreviewDTO>::Err(
        {"live_scene_snapshot_unavailable", "Candidate planning projection is unavailable", {}});
  }
  return scene::BuildResolvedSceneCandidatePreview(
      request, predecessor.projection_binding.projection_revision,
      predecessor.logical_scene, predecessor.logical_scene->transaction_sequence + 1U,
      [&](const core::LogicalSceneSnapshotDTO &candidate) {
        return projection_->ProjectEffectiveCollisionPolicy(candidate, candidate.revision.revision_id);
      });
}

[[nodiscard]] core::RequestOwnerEpoch
  ManagerSceneAuthorityProxy::CurrentRequestOwnerEpoch() const {
    return Read().request_owner_epoch;
  }

[[nodiscard]] core::ProjectionRevisionDTO
  ManagerSceneAuthorityProxy::CurrentProjectionRevision() const {
    return Read().projection_revision;
  }

[[nodiscard]] core::ProjectionRevisionDTO
  ManagerSceneAuthorityProxy::CurrentProjectionRevision(const std::stop_token stop_token) const {
    return Read(stop_token).projection_revision;
  }

[[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  ManagerSceneAuthorityProxy::LogicalSnapshot() const {
    return Read().logical_scene;
  }

[[nodiscard]] std::vector<core::SceneObjectSnapshotDTO>
  ManagerSceneAuthorityProxy::GetSceneObjects() const {
    return Read().logical_scene->objects;
  }

[[nodiscard]] core::SceneObjectStateDTO ManagerSceneAuthorityProxy::GetObjectState(
      const core::SceneResourceId &scene_resource_id) const {
    Require(!scene_resource_id.empty() &&
                scene_resource_id.value().size() <= 256U,
            "scene resource identity is invalid");
    const auto snapshot = Read().logical_scene;
    const auto found = std::lower_bound(
        snapshot->objects.begin(), snapshot->objects.end(), scene_resource_id,
        [](const core::SceneObjectSnapshotDTO &object,
           const core::SceneResourceId &id) {
          return object.scene_resource_id < id;
        });
    if (found == snapshot->objects.end() ||
        found->scene_resource_id != scene_resource_id) {
      return {scene_resource_id, false, false, std::nullopt, std::nullopt};
    }
    const auto *carried =
        std::get_if<core::CarriedSceneObjectPlacementDTO>(&found->placement);
    return {scene_resource_id, true, carried != nullptr,
            carried == nullptr ? std::nullopt
                               : std::optional<core::CarriedObjectAttachmentId>(
                                     carried->carried_object_attachment_id),
            found->provider_evidence};
  }

[[nodiscard]] core::Result<core::Pose3DTO>
  ManagerSceneAuthorityProxy::GetObjectPose(const core::SceneResourceId &scene_resource_id) const {
    Require(!scene_resource_id.empty() &&
                scene_resource_id.value().size() <= 256U,
            "scene resource identity is invalid");
    const auto current = Read();
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_pose_query")},
            {"expected_projection_revision",
             ProjectionRevisionValue(current.projection_revision)},
            {"scene_resource_ids",
             CanonicalValue(CanonicalValue::Array{
                 CanonicalValue(scene_resource_id.value())})},
        }));
    if (!Boolean(response, "ok")) {
      return core::Result<core::Pose3DTO>::Err(
          DecodeManagerError(Member(response, "error")));
    }
    RequireExactObjectSize(response, 4U, "Manager scene pose response");
    Require(core::ProjectionIsSameOrForwardInLineage(
                DecodeProjectionRevision(Member(response, "projection_revision")),
                current.projection_revision),
            "Manager scene pose response changed projection identity");
    const auto &poses = Array(response, "poses");
    Require(poses.size() == 1U, "Manager scene pose response is incomplete");
    const auto &row = poses.front();
    RequireExactObjectSize(row, 2U, "Manager scene pose row");
    Require(Text(row, "scene_resource_id") == scene_resource_id.value(),
            "Manager scene pose response changed resource identity");
    const auto &pose = Member(row, "pose_in_cell");
    RequireExactObjectSize(pose, 2U, "Manager scene pose");
    const auto &position = Member(pose, "position_m");
    RequireExactObjectSize(position, 3U, "Manager scene pose position");
    const auto &orientation = Array(pose, "orientation_xyzw");
    Require(orientation.size() == 4U,
            "Manager scene pose orientation is incomplete");
    const auto coordinate = [](const CanonicalValue &value,
                               const std::string_view label) {
      const auto *number = std::get_if<double>(&value.storage());
      Require(number != nullptr && std::isfinite(*number),
              std::string(label) + " must be finite");
      return *number;
    };
    core::Pose3DTO result{
        {Number(position, "x"), Number(position, "y"), Number(position, "z")},
        {coordinate(orientation[0], "orientation x"),
         coordinate(orientation[1], "orientation y"),
         coordinate(orientation[2], "orientation z"),
         coordinate(orientation[3], "orientation w")}};
    return core::Result<core::Pose3DTO>::Ok(std::move(result));
  }

[[nodiscard]] ManagerSceneRead
  ManagerSceneAuthorityProxy::Read(const std::stop_token stop_token) const {
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_owner_read")},
        }),
        stop_token);
    if (!Boolean(response, "ok")) {
      const auto error = DecodeManagerError(Member(response, "error"));
      Fail("Manager scene read failed: " + error.code + ": " + error.message);
    }
    RequireExactObjectSize(response, 5U, "Manager scene owner response");
    auto snapshot = std::make_shared<const core::LogicalSceneSnapshotDTO>(
        scene::DecodeLogicalSceneSnapshot(content_identity::canonical_json(
            Member(response, "logical_scene"))));
    auto projection =
        DecodeProjectionRevision(Member(response, "projection_revision"));
    Require(snapshot->revision == projection.scene_revision,
            "Manager scene owner response crossed a projection revision");
    return {core::RequestOwnerEpoch(Text(response, "request_owner_epoch")),
            std::move(projection), std::move(snapshot)};
  }

ManagerPlanningSceneSynchronizer::ManagerPlanningSceneSynchronizer(
      std::filesystem::path manager_run_root,
      core::RetainedBundleId retained_bundle_id,
      core::GraphRevision graph_revision,
      const core::ActiveBundlePublicationGeneration publication_generation,
      std::shared_ptr<core::ICollisionWorldSnapshotPublisher>
          collision_publisher)
      : manager_run_root_(std::move(manager_run_root)),
        retained_bundle_id_(std::move(retained_bundle_id)),
        graph_revision_(std::move(graph_revision)),
        publication_generation_(publication_generation),
        collision_publisher_(std::move(collision_publisher)) {
    Require(
        !manager_run_root_.empty() &&
            core::IsExactLowercaseSha256Digest(retained_bundle_id_.value()) &&
            core::IsExactLowercaseSha256Digest(graph_revision_.value()) &&
            publication_generation_.nonzero() &&
            collision_publisher_ != nullptr,
        "Manager planning-scene synchronizer identity is invalid");
  }

[[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  ManagerPlanningSceneSynchronizer::SynchronizeBeforePlanning() {
    try {
        const auto response = ExchangeManagerCommand(
            manager_run_root_,
            CanonicalValue(CanonicalValue::Object{
                {"api_version",
                 CanonicalValue(std::string(kManagerCommandApiVersion))},
                {"command",
                 CanonicalValue("scene_runtime_latch_before_planning")},
                {"expected_runtime_identity", RuntimeIdentityValue()},
            }));
        if (!Boolean(response, "ok")) {
          auto error = DecodeManagerError(Member(response, "error"));
          return core::Result<core::ProjectionRevisionDTO>::Err(
              std::move(error));
        }
        RequireExactObjectSize(response, 10U,
                               "Manager before-planning latch response");
        RequireRuntimeIdentity(Member(response, "runtime_identity"));
        const auto initial = DecodeProjectionRevision(
            Member(response, "initial_projection_revision"));
        const auto projection =
            DecodeProjectionRevision(Member(response, "projection_revision"));
        const auto latched = Counter(response, "latched_observation_count", 0U);
        const auto committed =
            Counter(response, "committed_observation_count", 0U);
        const auto already_applied =
            Counter(response, "already_applied_observation_count", 0U);
        const auto skipped_attached =
            Counter(response, "skipped_attached_observation_count", 0U);
        static_cast<void>(
            Counter(response, "scene_runtime_source_generation", 1U));
        Require(core::ProjectionIsSameOrForwardInLineage(projection, initial) &&
                    projection.retained_bundle_id == retained_bundle_id_ &&
                    projection.graph_revision == graph_revision_ &&
                    latched == committed + already_applied + skipped_attached,
                "Manager before-planning latch response crossed its request");
        for (std::size_t convergence_attempt = 0U;
             convergence_attempt < kPlanningSceneSynchronizationAttempts;
             ++convergence_attempt) {
          const auto converged =
              collision_publisher_->current_scene_snapshot_lease_inventory();
          if (converged.has_value() &&
              core::ProjectionIsSameOrForwardInLineage(
                  converged->projection_binding.projection_revision, projection)) {
            return core::Result<core::ProjectionRevisionDTO>::Ok(projection);
          }
          if (convergence_attempt + 1U <
              kPlanningSceneSynchronizationAttempts) {
            std::this_thread::sleep_for(
                kPlanningSceneSynchronizationRetryDelay);
          }
        }
        return Failure(
            "planning_scene_projection_not_converged",
            "local native projection did not converge to the Manager latch");
    } catch (const std::exception &error) {
      return Failure("planning_scene_synchronization_failed", error.what());
    } catch (...) {
      return Failure("planning_scene_synchronization_failed",
                     "unknown Manager planning-scene synchronization failure");
    }
  }

[[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  ManagerPlanningSceneSynchronizer::Failure(std::string code, std::string message) const {
    return core::Result<core::ProjectionRevisionDTO>::Err(
        core::ErrorDTO{std::move(code),
                       std::move(message),
                       {{"reason", "before_planning_scene_latch_failed"}}});
  }

[[nodiscard]] CanonicalValue ManagerPlanningSceneSynchronizer::RuntimeIdentityValue() const {
    const auto generation =
        static_cast<double>(publication_generation_.value());
    return CanonicalValue(CanonicalValue::Object{
        {"deployment_generation", CanonicalValue(generation)},
        {"graph_revision", CanonicalValue(graph_revision_.value())},
        {"program_generation", CanonicalValue(0.0)},
        {"retained_bundle_id", CanonicalValue(retained_bundle_id_.value())},
        {"runtime_generation", CanonicalValue(generation)},
    });
  }

void ManagerPlanningSceneSynchronizer::RequireRuntimeIdentity(const CanonicalValue &value) const {
    RequireExactObjectSize(value, 5U, "Manager scene-runtime latch identity");
    Require(Text(value, "retained_bundle_id") == retained_bundle_id_.value() &&
                Text(value, "graph_revision") == graph_revision_.value() &&
                Counter(value, "deployment_generation", 1U) ==
                    publication_generation_.value() &&
                Counter(value, "program_generation", 0U) == 0U &&
                Counter(value, "runtime_generation", 1U) ==
                    publication_generation_.value(),
            "Manager scene-runtime latch identity changed");
  }
} // namespace manyforge::roboplan_backend::host_detail
