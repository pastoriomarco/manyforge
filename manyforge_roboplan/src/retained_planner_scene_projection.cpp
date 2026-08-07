#include <manyforge_roboplan/retained_planner_scene_projection.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>

namespace manyforge::roboplan_backend {
namespace {

template <typename T>
core::Result<T> projection_error(std::string message, std::string reason) {
  return core::Result<T>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}},
  });
}

core::ErrorDTO error(std::string code, std::string message,
                     std::string reason) {
  return {std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

bool exact_scene_content(const core::LogicalSceneSnapshotDTO &scene) {
  try {
    return core::logical_scene_identity::ComputeLogicalSceneContentHash(
               scene) == scene.revision.content_hash;
  } catch (...) {
    return false;
  }
}

core::CollisionWorldMutationSource
mutation_source(const core::ResolvedSceneOperationDTO &operation) {
  return std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::ResolvedAttachCarriedObjectDTO>) {
          return core::CollisionWorldMutationSource::kCarriedObjectAttach;
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedDetachCarriedObjectDTO>) {
          return core::CollisionWorldMutationSource::kCarriedObjectDetach;
        } else if constexpr (std::is_same_v<T,
                                            core::ResolvedCollisionPolicyDTO>) {
          return core::CollisionWorldMutationSource::kPolicyUpdate;
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedUpsertSceneObjectDTO>) {
          return value.object.provider_evidence.has_value()
                     ? core::CollisionWorldMutationSource::kPoseSourceApply
                     : core::CollisionWorldMutationSource::kSceneEdit;
        } else {
          return core::CollisionWorldMutationSource::kSceneEdit;
        }
      },
      operation);
}

std::optional<core::ContentDigest> publisher_digest(
    const std::shared_ptr<RetainedCollisionWorldPublisher> &publisher) {
  if (!publisher) {
    return std::nullopt;
  }
  const auto revision = publisher->published_collision_world_revision();
  if (revision.revision_id == 0U || revision.content_hash.empty()) {
    return std::nullopt;
  }
  return core::ContentDigest(revision.content_hash);
}

} // namespace

RetainedPlannerSceneProjection::RetainedPlannerSceneProjection(
    RetainedCollisionWorldProjector projector,
    core::RetainedProductDTO planning_collision_model,
    core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
    RetainedPlannerSceneProjectionConfig config, Clock clock)
    : projector_(std::move(projector)),
      planning_collision_model_(std::move(planning_collision_model)),
      native_runtime_binding_(std::move(native_runtime_binding)),
      config_(std::move(config)), clock_(std::move(clock)),
      health_{native_runtime_binding_,
              1U,
              core::ProjectionHealthState::kUninitialized,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt} {}

core::Result<std::shared_ptr<RetainedPlannerSceneProjection>>
RetainedPlannerSceneProjection::CreateUninitialized(
    RetainedCollisionWorldProjector projector,
    core::RetainedProductDTO planning_collision_model,
    core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
    RetainedPlannerSceneProjectionConfig config, Clock clock) {
  if (planning_collision_model.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      planning_collision_model.product_id.empty() ||
      planning_collision_model.product_contract_id.empty() ||
      planning_collision_model.product_contract_revision.empty() ||
      !native_runtime_binding.valid() || config.publisher_id.empty() ||
      config.transaction_replay_capacity == 0U ||
      config.transaction_replay_capacity > 4096U ||
      config.collision_ledger_capacity == 0U ||
      config.collision_ledger_capacity > 4096U || !clock) {
    return projection_error<std::shared_ptr<RetainedPlannerSceneProjection>>(
        "retained planner projection configuration is incomplete or "
        "unbounded",
        "native_projection_configuration_invalid");
  }
  try {
    return core::Result<std::shared_ptr<RetainedPlannerSceneProjection>>::Ok(
        std::shared_ptr<RetainedPlannerSceneProjection>(
            new RetainedPlannerSceneProjection(
                std::move(projector), std::move(planning_collision_model),
                std::move(native_runtime_binding), std::move(config),
                std::move(clock))));
  } catch (const std::exception &exception) {
    return projection_error<std::shared_ptr<RetainedPlannerSceneProjection>>(
        "retained planner projection construction failed: " +
            std::string(exception.what()),
        "native_projection_construction_failed");
  }
}

core::Result<std::shared_ptr<RetainedPlannerSceneProjection>>
RetainedPlannerSceneProjection::CreateInitialized(
    RetainedCollisionWorldProjector projector,
    core::RetainedProductDTO planning_collision_model,
    core::ProjectionNativeRuntimeBindingDTO native_runtime_binding,
    core::LogicalSceneSnapshotDTO initial_scene,
    RetainedPlannerSceneProjectionConfig config, Clock clock) {
  auto result = CreateUninitialized(
      std::move(projector), std::move(planning_collision_model),
      std::move(native_runtime_binding), std::move(config), std::move(clock));
  if (!result.ok()) {
    return result;
  }
  auto projection = result.value();
  const auto &map = projection->projector_.model_map();
  const core::ProjectionRevisionDTO revision{
      map.retained_bundle_id, map.graph_revision, initial_scene.revision};
  if (!initial_scene.locally_valid() || !exact_scene_content(initial_scene) ||
      initial_scene.cell_id != map.cell_id) {
    return projection_error<std::shared_ptr<RetainedPlannerSceneProjection>>(
        "initial logical scene does not match the exact retained projection",
        "native_projection_initial_scene_mismatch");
  }
  auto publisher = projection->BuildPublisher(
      initial_scene, core::CollisionWorldMutationSource::kDeploymentApply,
      "native_projection_initialize");
  if (!publisher.ok()) {
    return projection_error<std::shared_ptr<RetainedPlannerSceneProjection>>(
        publisher.error().message,
        "native_projection_initial_publication_failed");
  }
  auto snapshot = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      std::move(initial_scene));
  const auto digest = publisher_digest(publisher.value());
  if (!digest.has_value()) {
    return projection_error<std::shared_ptr<RetainedPlannerSceneProjection>>(
        "initial collision projection has no immutable content digest",
        "native_projection_initial_digest_missing");
  }
  projection->logical_snapshot_ = std::move(snapshot);
  projection->health_ = {
      projection->native_runtime_binding_,
      2U,
      core::ProjectionHealthState::kHealthy,
      revision,
      std::nullopt,
      1U,
      *digest,
      std::nullopt,
  };
  projection->active_publisher_.store(std::move(publisher.value()),
                                      std::memory_order_release);
  return result;
}

core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
RetainedPlannerSceneProjection::BuildPublisher(
    const core::LogicalSceneSnapshotDTO &snapshot,
    const core::CollisionWorldMutationSource source,
    std::string source_label) const {
  std::int64_t observed_at = 0;
  try {
    observed_at = clock_();
  } catch (const std::exception &exception) {
    return projection_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "native projection clock failed: " + std::string(exception.what()),
        "native_projection_clock_failed");
  } catch (...) {
    return projection_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "native projection clock failed", "native_projection_clock_failed");
  }
  if (observed_at < 0) {
    return projection_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "native projection clock returned a negative timestamp",
        "native_projection_clock_invalid");
  }
  return RetainedCollisionWorldPublisher::create(
      projector_, config_.publisher_id, snapshot,
      RetainedCollisionWorldPublicationContext{
          source, {}, observed_at, {}, std::move(source_label), std::nullopt},
      config_.collision_ledger_capacity);
}

bool RetainedPlannerSceneProjection::HealthyAt(
    const core::ProjectionRevisionDTO &revision) const noexcept {
  return health_.healthy_for_target(revision) && logical_snapshot_ &&
         logical_snapshot_->revision == revision.scene_revision &&
         active_publisher_.load(std::memory_order_acquire) != nullptr;
}

core::ProjectionCommitResultDTO RetainedPlannerSceneProjection::RollbackProven(
    const std::optional<std::uint64_t> transaction_sequence,
    const core::ProjectionRevisionDTO &proposed, core::ErrorDTO value) {
  if (health_.source_sequence != std::numeric_limits<std::uint64_t>::max()) {
    ++health_.source_sequence;
  }
  return {transaction_sequence,
          proposed,
          core::ProjectionCommitResultStateDTO::kRejectedRollbackProven,
          health_.active_projection_revision,
          health_.native_projection_generation,
          true,
          health_,
          std::move(value)};
}

core::ProjectionCommitResultDTO RetainedPlannerSceneProjection::Desynchronize(
    const std::optional<std::uint64_t> transaction_sequence,
    const core::ProjectionRevisionDTO &proposed, core::ErrorDTO value) {
  if (health_.source_sequence != std::numeric_limits<std::uint64_t>::max()) {
    ++health_.source_sequence;
  }
  health_.state = core::ProjectionHealthState::kDesynchronized;
  health_.staged_candidate = std::nullopt;
  health_.error = value;
  active_publisher_.store(nullptr, std::memory_order_release);
  return {transaction_sequence,
          proposed,
          core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
          std::nullopt,
          health_.native_projection_generation,
          false,
          health_,
          std::move(value)};
}

core::ProjectionCommitResultDTO
RetainedPlannerSceneProjection::ApplyTransaction(
    const core::SceneTransactionDTO &transaction) {
  std::lock_guard lock(mutex_);
  const auto invalid = [&](std::string message, std::string reason) {
    return RollbackProven(transaction.transaction_sequence,
                          transaction.proposed_projection_revision,
                          error(std::string(core::to_string(
                                    core::ErrorCode::kInvariantViolation)),
                                std::move(message), std::move(reason)));
  };
  if (!transaction.proposed_projection_revision.valid()) {
    return invalid("native projection received an invalid proposed revision",
                   "native_projection_transaction_invalid");
  }
  if (const auto replay =
          transaction_records_.find(transaction.transaction_sequence);
      replay != transaction_records_.end()) {
    if (replay->second.transaction == transaction) {
      return replay->second.result;
    }
    return invalid("native projection transaction sequence was reused with "
                   "different normalized intent",
                   "native_projection_transaction_conflict");
  }
  if (!HealthyAt(transaction.prior_projection_revision)) {
    return invalid("native projection is not healthy at the exact "
                   "transaction predecessor",
                   "native_projection_predecessor_unavailable");
  }
  if (transaction_records_.size() >= config_.transaction_replay_capacity) {
    return invalid("native projection transaction history reached its "
                   "bounded pre-compaction capacity",
                   "native_projection_transaction_history_full");
  }

  const auto candidate =
      core::ApplyResolvedSceneTransaction(*logical_snapshot_, transaction);
  if (!candidate.ok()) {
    return invalid(candidate.error().message,
                   "native_projection_resolved_delta_mismatch");
  }

  auto candidate_snapshot =
      std::make_shared<const core::LogicalSceneSnapshotDTO>(candidate.value());
  if (const auto *policy = std::get_if<core::ResolvedCollisionPolicyDTO>(
          &transaction.resolved_operation)) {
    const auto derived = projector_.project_effective_policy(
        *candidate_snapshot, policy->effective_policy.revision.revision_id);
    if (!derived.ok() || derived.value() != policy->effective_policy) {
      return invalid(derived.ok()
                         ? "owner-supplied effective collision policy differs "
                           "from the exact retained projection"
                         : derived.error().message,
                     "native_projection_effective_policy_mismatch");
    }
  }
  auto next_health = health_;
  if (next_health.source_sequence ==
      std::numeric_limits<std::uint64_t>::max()) {
    return invalid("native projection health sequence is exhausted",
                   "native_projection_sequence_exhausted");
  }
  ++next_health.source_sequence;
  next_health.active_projection_revision =
      transaction.proposed_projection_revision;
  next_health.error = std::nullopt;

  const bool no_native_change = transaction.proposed_projection_revision ==
                                transaction.prior_projection_revision;
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  if (!publisher) {
    return invalid("native projection publisher became unavailable",
                   "native_projection_publisher_unavailable");
  }

  if (!no_native_change) {
    if (!next_health.native_projection_generation.has_value() ||
        *next_health.native_projection_generation ==
            std::numeric_limits<std::uint64_t>::max()) {
      return invalid("native projection generation is exhausted",
                     "native_projection_generation_exhausted");
    }
    ++*next_health.native_projection_generation;
  }

  core::ProjectionCommitResultDTO success{
      transaction.transaction_sequence,
      transaction.proposed_projection_revision,
      core::ProjectionCommitResultStateDTO::kCommitted,
      transaction.proposed_projection_revision,
      next_health.native_projection_generation,
      false,
      next_health,
      std::nullopt,
  };
  auto next_records = transaction_records_;
  next_records.emplace(transaction.transaction_sequence,
                       TransactionRecord{transaction, success});

  if (!no_native_change) {
    std::int64_t observed_at = 0;
    try {
      observed_at = clock_();
      if (observed_at < 0) {
        return invalid("native projection clock returned a negative "
                       "transaction timestamp",
                       "native_projection_clock_invalid");
      }
      const auto published = publisher->publish(
          *candidate_snapshot,
          RetainedCollisionWorldPublicationContext{
              mutation_source(transaction.resolved_operation),
              {},
              observed_at,
              transaction.attribution,
              "logical_scene_transaction",
              transaction.request_id.value(),
          });
      if (!published.ok()) {
        return invalid(published.error().message,
                       "native_projection_publication_rejected");
      }
    } catch (const std::exception &exception) {
      return Desynchronize(
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          error("projection_state_uncertain",
                "native projection transaction effect is uncertain: " +
                    std::string(exception.what()),
                "native_projection_effect_uncertain"));
    } catch (...) {
      return Desynchronize(
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          error("projection_state_uncertain",
                "native projection transaction effect is uncertain",
                "native_projection_effect_uncertain"));
    }
    const auto digest = publisher_digest(publisher);
    if (!digest.has_value()) {
      return Desynchronize(
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          error("projection_state_uncertain",
                "committed native projection has no immutable digest",
                "native_projection_digest_missing"));
    }
    next_health.projection_content_digest = *digest;
    success.resulting_health = next_health;
    success.native_projection_generation =
        next_health.native_projection_generation;
    next_records.at(transaction.transaction_sequence).result = success;
  }

  logical_snapshot_ = std::move(candidate_snapshot);
  health_ = std::move(next_health);
  transaction_records_.swap(next_records);
  return success;
}

core::Result<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::RetainProjection(
    const core::ProjectionRevisionDTO &projection_revision,
    core::ProjectionLeaseId projection_lease_id) {
  std::lock_guard lock(mutex_);
  if (projection_lease_id.empty()) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease does not name the exact healthy active "
        "revision",
        "native_projection_lease_unavailable");
  }
  if (const auto retained =
          retained_lease_publishers_.find(projection_lease_id);
      retained != retained_lease_publishers_.end()) {
    if (health_.state != core::ProjectionHealthState::kHealthy) {
      return projection_error<core::CollisionWorldLeaseInventory>(
          "native projection lease retry is unavailable while projection "
          "health is closed",
          "native_projection_lease_unavailable");
    }
    const auto inventory =
        retained->second->retain_scene_snapshot_lease_inventory(
            projection_lease_id);
    if (inventory.has_value() &&
        inventory->projection_binding.projection_revision ==
            projection_revision) {
      return core::Result<core::CollisionWorldLeaseInventory>::Ok(*inventory);
    }
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease ID is already bound to another revision",
        "native_projection_lease_conflict");
  }
  if (!HealthyAt(projection_revision)) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease does not name the exact healthy active "
        "revision",
        "native_projection_lease_unavailable");
  }
  if (retained_lease_publishers_.size() >=
      RetainedCollisionWorldPublisher::kRuntimeRetirementCapacity) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease retention reached its fixed capacity",
        "native_projection_lease_capacity_exhausted");
  }
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  if (!publisher) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection publisher is unavailable",
        "native_projection_lease_unavailable");
  }
  auto inventory =
      publisher->retain_scene_snapshot_lease_inventory(projection_lease_id);
  if (!inventory.has_value() ||
      inventory->projection_binding.projection_revision !=
          projection_revision) {
    if (inventory.has_value()) {
      publisher->release_scene_snapshot_lease_inventory(projection_lease_id);
    }
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection publisher could not retain the exact revision",
        "native_projection_lease_unavailable");
  }
  try {
    const auto [inserted, was_inserted] =
        retained_lease_publishers_.emplace(projection_lease_id, publisher);
    if (!was_inserted) {
      publisher->release_scene_snapshot_lease_inventory(projection_lease_id);
      return projection_error<core::CollisionWorldLeaseInventory>(
          "native projection lease ID could not be retained uniquely",
          "native_projection_lease_conflict");
    }
    try {
      retained_lease_order_.push_back(projection_lease_id);
    } catch (...) {
      retained_lease_publishers_.erase(inserted);
      throw;
    }
  } catch (...) {
    publisher->release_scene_snapshot_lease_inventory(projection_lease_id);
    throw;
  }
  return core::Result<core::CollisionWorldLeaseInventory>::Ok(
      std::move(*inventory));
}

void RetainedPlannerSceneProjection::ReleaseProjection(
    const core::ProjectionLeaseId &projection_lease_id) {
  std::lock_guard lock(mutex_);
  const auto retained = retained_lease_publishers_.find(projection_lease_id);
  if (retained == retained_lease_publishers_.end()) {
    return;
  }
  retained->second->release_scene_snapshot_lease_inventory(projection_lease_id);
  retained_lease_publishers_.erase(retained);
  retained_lease_order_.erase(std::remove(retained_lease_order_.begin(),
                                          retained_lease_order_.end(),
                                          projection_lease_id),
                              retained_lease_order_.end());
}

core::ProjectionHealthDTO
RetainedPlannerSceneProjection::ProjectionHealth() const {
  std::lock_guard lock(mutex_);
  return health_;
}

core::ProjectionCommitResultDTO
RetainedPlannerSceneProjection::RebuildProjection(
    const core::NativeProjectionRebuildDTO &request) {
  std::lock_guard lock(mutex_);
  const auto invalid = [&](std::string message, std::string reason) {
    return RollbackProven(std::nullopt, request.target_projection_revision,
                          error(std::string(core::to_string(
                                    core::ErrorCode::kInvariantViolation)),
                                std::move(message), std::move(reason)));
  };
  if (!request.valid() ||
      request.expected_native_runtime_binding != native_runtime_binding_ ||
      request.planning_collision_model != planning_collision_model_ ||
      request.target_projection_revision.retained_bundle_id !=
          projector_.model_map().retained_bundle_id ||
      request.target_projection_revision.graph_revision !=
          projector_.model_map().graph_revision ||
      request.logical_scene->cell_id != projector_.model_map().cell_id ||
      !exact_scene_content(*request.logical_scene)) {
    return invalid("native projection rebuild input does not match its exact "
                   "verified retained product and logical snapshot",
                   "native_projection_rebuild_mismatch");
  }
  if (HealthyAt(request.target_projection_revision) &&
      *logical_snapshot_ == *request.logical_scene) {
    return {std::nullopt,
            request.target_projection_revision,
            core::ProjectionCommitResultStateDTO::kCommitted,
            request.target_projection_revision,
            health_.native_projection_generation,
            false,
            health_,
            std::nullopt};
  }
  if (health_.state == core::ProjectionHealthState::kHealthy) {
    return invalid("recovery cannot replace a healthy native projection at "
                   "a different target",
                   "native_projection_rebuild_not_required");
  }
  if (health_.source_sequence >
          std::numeric_limits<std::uint64_t>::max() - 2U ||
      (health_.native_projection_generation.has_value() &&
       *health_.native_projection_generation ==
           std::numeric_limits<std::uint64_t>::max())) {
    return invalid("native projection rebuild counters are exhausted",
                   "native_projection_rebuild_counter_exhausted");
  }

  ++health_.source_sequence;
  health_.state = core::ProjectionHealthState::kRebuilding;
  health_.active_projection_revision = std::nullopt;
  health_.staged_candidate = std::nullopt;
  health_.error = std::nullopt;
  active_publisher_.store(nullptr, std::memory_order_release);

  auto rebuilt =
      BuildPublisher(*request.logical_scene,
                     core::CollisionWorldMutationSource::kDeploymentApply,
                     "native_projection_rebuild");
  if (!rebuilt.ok()) {
    return Desynchronize(std::nullopt, request.target_projection_revision,
                         error("projection_state_uncertain",
                               rebuilt.error().message,
                               "native_projection_rebuild_failed"));
  }
  const auto digest = publisher_digest(rebuilt.value());
  if (!digest.has_value()) {
    return Desynchronize(
        std::nullopt, request.target_projection_revision,
        error("projection_state_uncertain",
              "rebuilt native projection has no immutable digest",
              "native_projection_rebuild_digest_missing"));
  }

  auto next_snapshot = request.logical_scene;
  auto next_health = health_;
  ++next_health.source_sequence;
  next_health.state = core::ProjectionHealthState::kHealthy;
  next_health.active_projection_revision = request.target_projection_revision;
  next_health.native_projection_generation =
      health_.native_projection_generation.has_value()
          ? std::optional<std::uint64_t>(*health_.native_projection_generation +
                                         1U)
          : std::optional<std::uint64_t>(1U);
  next_health.projection_content_digest = *digest;
  next_health.error = std::nullopt;
  core::ProjectionCommitResultDTO success{
      std::nullopt,
      request.target_projection_revision,
      core::ProjectionCommitResultStateDTO::kCommitted,
      request.target_projection_revision,
      next_health.native_projection_generation,
      false,
      next_health,
      std::nullopt,
  };
  logical_snapshot_ = std::move(next_snapshot);
  transaction_records_.clear();
  health_ = std::move(next_health);
  active_publisher_.store(std::move(rebuilt.value()),
                          std::memory_order_release);
  return success;
}

std::shared_ptr<const core::LogicalSceneSnapshotDTO>
RetainedPlannerSceneProjection::logical_snapshot() const {
  std::lock_guard lock(mutex_);
  return logical_snapshot_;
}

const collision::ManyForgeCollisionEngine::SharedModelPtr &
RetainedPlannerSceneProjection::collision_shared_model() const noexcept {
  return projector_.shared_model();
}

core::CollisionWorldSnapshotPtr
RetainedPlannerSceneProjection::snapshot() const {
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->snapshot() : nullptr;
}

core::CollisionWorldRevision
RetainedPlannerSceneProjection::published_collision_world_revision() const {
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->published_collision_world_revision()
                   : core::CollisionWorldRevision{};
}

std::optional<core::CollisionPolicySnapshot>
RetainedPlannerSceneProjection::collision_policy_snapshot() const {
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->collision_policy_snapshot() : std::nullopt;
}

core::CollisionWorldPolicySnapshotBundle
RetainedPlannerSceneProjection::collision_world_policy_snapshot_bundle() const {
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->collision_world_policy_snapshot_bundle()
                   : core::CollisionWorldPolicySnapshotBundle{};
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::current_scene_snapshot_lease_inventory() const {
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->current_scene_snapshot_lease_inventory()
                   : std::nullopt;
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::retain_scene_snapshot_lease_inventory(
    core::ProjectionLeaseId projection_lease_id) {
  core::ProjectionRevisionDTO revision;
  {
    std::lock_guard lock(mutex_);
    if (!health_.active_projection_revision.has_value()) {
      return std::nullopt;
    }
    revision = *health_.active_projection_revision;
  }
  if (projection_lease_id.empty()) {
    return std::nullopt;
  }
  auto retained = RetainProjection(revision, std::move(projection_lease_id));
  return retained.ok() ? std::optional<core::CollisionWorldLeaseInventory>(
                             std::move(retained.value()))
                       : std::nullopt;
}

void RetainedPlannerSceneProjection::release_scene_snapshot_lease_inventory(
    const core::ProjectionLeaseId &projection_lease_id) {
  ReleaseProjection(projection_lease_id);
}

core::CollisionWorldLeaseRetentionStats
RetainedPlannerSceneProjection::scene_snapshot_lease_retention_stats() const {
  std::lock_guard lock(mutex_);
  core::CollisionWorldLeaseRetentionStats result;
  result.retained_count = retained_lease_publishers_.size();
  result.retained_capacity =
      RetainedCollisionWorldPublisher::kRuntimeRetirementCapacity;
  result.retained_ids_oldest_first.reserve(retained_lease_order_.size());
  for (const auto &lease_id : retained_lease_order_) {
    result.retained_ids_oldest_first.push_back(lease_id);
  }
  return result;
}

core::RuntimeCollisionQueryBundlePtr
RetainedPlannerSceneProjection::published_runtime_collision_query_bundle()
    const {
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->published_runtime_collision_query_bundle()
                   : nullptr;
}

} // namespace manyforge::roboplan_backend
