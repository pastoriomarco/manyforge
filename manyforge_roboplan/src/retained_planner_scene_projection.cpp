#include <algorithm>
#include <limits>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

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
    RetainedPlannerSceneProjectionConfig config, Clock clock,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver)
    : projector_(std::move(projector)),
      planning_collision_model_(std::move(planning_collision_model)),
      frame_resolver_(std::move(frame_resolver)),
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
    RetainedPlannerSceneProjectionConfig config, Clock clock,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
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
                std::move(clock), std::move(frame_resolver))));
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
    RetainedPlannerSceneProjectionConfig config, Clock clock,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
  auto result = CreateUninitialized(
      std::move(projector), std::move(planning_collision_model),
      std::move(native_runtime_binding), std::move(config), std::move(clock),
      std::move(frame_resolver));
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
      projection->projector_, initial_scene,
      core::CollisionWorldMutationSource::kDeploymentApply,
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

core::Result<void> RetainedPlannerSceneProjection::PrepareCandidateProjection(
    RetainedCollisionWorldProjector candidate_projector,
    core::RetainedProductDTO candidate_planning_collision_model,
    std::shared_ptr<const RetainedSceneFrameResolver>
        candidate_frame_resolver) {
  std::lock_guard lock(mutex_);
  const auto rejected = [](std::string message, std::string reason) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
        std::move(message), {{"reason", std::move(reason)}}});
  };
  const auto &candidate_map = candidate_projector.model_map();
  if (candidate_planning_collision_model.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      candidate_planning_collision_model.product_id.empty() ||
      candidate_planning_collision_model.product_contract_id.empty() ||
      candidate_planning_collision_model.product_contract_revision.empty() ||
      candidate_map.retained_bundle_id.empty() ||
      candidate_map.graph_revision.empty() || candidate_map.cell_id.empty() ||
      candidate_map.cell_id != projector_.model_map().cell_id) {
    return rejected("prepared native candidate model is invalid or belongs "
                    "to another cell",
                    "native_projection_candidate_model_invalid");
  }
  if (stage_record_.has_value() && !stage_record_->activated &&
      !stage_record_->discarded) {
    return rejected("a nonterminal native projection stage already owns this "
                    "runtime",
                    "native_projection_candidate_stage_active");
  }
  if (stage_record_.has_value()) {
    stage_record_.reset();
  }
  if (candidate_map.retained_bundle_id ==
          projector_.model_map().retained_bundle_id &&
      candidate_map.graph_revision == projector_.model_map().graph_revision &&
      candidate_planning_collision_model == planning_collision_model_) {
    if (candidate_frame_resolver) {
      frame_resolver_ = std::move(candidate_frame_resolver);
    }
    prepared_candidate_model_.reset();
    return core::Result<void>::Ok();
  }
  prepared_candidate_model_ = PreparedCandidateModel{
      std::move(candidate_projector),
      std::move(candidate_planning_collision_model),
      std::move(candidate_frame_resolver)};
  return core::Result<void>::Ok();
}

core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
RetainedPlannerSceneProjection::BuildPublisher(
    const RetainedCollisionWorldProjector &projector,
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
      projector, config_.publisher_id, snapshot,
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
  return RebuildProjectionLocked(request);
}

core::ProjectionCommitResultDTO
RetainedPlannerSceneProjection::RebuildProjectionLocked(
    const core::NativeProjectionRebuildDTO &request) {
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
      BuildPublisher(projector_, *request.logical_scene,
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

core::NativeProjectionBootstrapRebuildResultDTO
RetainedPlannerSceneProjection::RebuildBootstrapProjection(
    const core::NativeProjectionBootstrapRebuildDTO &request) {
  std::lock_guard lock(mutex_);
  if (!request.valid() ||
      request.native_runtime_binding != native_runtime_binding_ ||
      request.planning_collision_model != planning_collision_model_) {
    throw std::invalid_argument(
        "native bootstrap rebuild request is invalid for this runtime");
  }
  const bool already_exact =
      HealthyAt(request.canonical_empty_target) && logical_snapshot_ &&
      *logical_snapshot_ == *request.initialized_logical_scene.shared();
  const core::NativeProjectionRebuildDTO rebuild{
      request.native_runtime_binding,
      request.canonical_empty_target,
      request.planning_collision_model,
      request.initialized_logical_scene.shared(),
  };
  auto result = RebuildProjectionLocked(rebuild);
  if (result.result == core::ProjectionCommitResultStateDTO::kCommitted &&
      result.acknowledged_projection_revision ==
          std::optional<core::ProjectionRevisionDTO>{
              request.canonical_empty_target}) {
    bootstrap_record_ =
        BootstrapRecord{request.bootstrap_id, request.bootstrap_generation,
                        request.canonical_empty_target};
    core::NativeProjectionBootstrapRebuildResultDTO success{
        request.bootstrap_id,
        request.bootstrap_generation,
        request.native_runtime_binding,
        request.canonical_empty_target,
        request.planning_collision_model,
        request.initialized_logical_scene,
        already_exact
            ? core::NativeProjectionBootstrapRebuildState::kAlreadyExact
            : core::NativeProjectionBootstrapRebuildState::kRebuilt,
        request.canonical_empty_target,
        result.native_projection_generation,
        result.resulting_health.projection_content_digest,
        result.resulting_health,
        std::nullopt,
    };
    if (!success.presence_valid()) {
      throw std::logic_error(
          "native bootstrap rebuild produced an invalid success result");
    }
    return success;
  }
  core::NativeProjectionBootstrapRebuildResultDTO failed{
      request.bootstrap_id,
      request.bootstrap_generation,
      request.native_runtime_binding,
      request.canonical_empty_target,
      request.planning_collision_model,
      request.initialized_logical_scene,
      core::NativeProjectionBootstrapRebuildState::kFailedProjectionUncertain,
      std::nullopt,
      result.native_projection_generation,
      result.resulting_health.projection_content_digest,
      result.resulting_health,
      result.error.has_value()
          ? result.error
          : std::optional<core::ErrorDTO>{error(
                "projection_state_uncertain",
                "native bootstrap rebuild did not acknowledge its target",
                "native_projection_bootstrap_rebuild_unacknowledged")},
  };
  if (!failed.presence_valid()) {
    throw std::logic_error(
        "native bootstrap rebuild produced an invalid failure result");
  }
  return failed;
}

core::NativeProjectionBootstrapStatusDTO
RetainedPlannerSceneProjection::BootstrapProjectionStatus(
    const core::NativeProjectionBootstrapStatusQueryDTO &query) const {
  std::lock_guard lock(mutex_);
  if (!query.valid() ||
      query.native_runtime_binding != native_runtime_binding_) {
    throw std::invalid_argument(
        "native bootstrap status query does not match this runtime binding");
  }
  if (bootstrap_record_.has_value() &&
      bootstrap_record_->bootstrap_id == query.bootstrap_id &&
      bootstrap_record_->bootstrap_generation == query.bootstrap_generation &&
      bootstrap_record_->target == query.canonical_empty_target &&
      HealthyAt(query.canonical_empty_target)) {
    core::NativeProjectionBootstrapStatusDTO rebuilt{
        query.bootstrap_id,
        query.bootstrap_generation,
        query.native_runtime_binding,
        query.canonical_empty_target,
        core::NativeProjectionBootstrapStatusState::kRebuilt,
        query.canonical_empty_target,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        std::nullopt,
    };
    if (!rebuilt.presence_valid()) {
      throw std::logic_error(
          "native bootstrap status cannot represent rebuilt state");
    }
    return rebuilt;
  }
  if (!bootstrap_record_.has_value() &&
      health_.state == core::ProjectionHealthState::kUninitialized &&
      !health_.active_projection_revision.has_value()) {
    core::NativeProjectionBootstrapStatusDTO absent{
        query.bootstrap_id,
        query.bootstrap_generation,
        query.native_runtime_binding,
        query.canonical_empty_target,
        core::NativeProjectionBootstrapStatusState::kAbsent,
        std::nullopt,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        std::nullopt,
    };
    if (!absent.presence_valid()) {
      throw std::logic_error(
          "native bootstrap status cannot represent absent state");
    }
    return absent;
  }
  if (health_.state == core::ProjectionHealthState::kRebuilding &&
      !health_.active_projection_revision.has_value()) {
    core::NativeProjectionBootstrapStatusDTO rebuilding{
        query.bootstrap_id,
        query.bootstrap_generation,
        query.native_runtime_binding,
        query.canonical_empty_target,
        core::NativeProjectionBootstrapStatusState::kRebuilding,
        std::nullopt,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        std::nullopt,
    };
    if (!rebuilding.presence_valid()) {
      throw std::logic_error(
          "native bootstrap status cannot represent rebuilding state");
    }
    return rebuilding;
  }
  const auto status_error =
      error("projection_state_uncertain",
            "native bootstrap status cannot prove the exact requested state",
            "native_projection_bootstrap_status_uncertain");
  auto failed_health = health_;
  if (failed_health.state != core::ProjectionHealthState::kDesynchronized &&
      failed_health.state != core::ProjectionHealthState::kUnavailable) {
    failed_health.state = core::ProjectionHealthState::kUnavailable;
    failed_health.error = status_error;
  }
  core::NativeProjectionBootstrapStatusDTO failed{
      query.bootstrap_id,
      query.bootstrap_generation,
      query.native_runtime_binding,
      query.canonical_empty_target,
      core::NativeProjectionBootstrapStatusState::kFailedProjectionUncertain,
      std::nullopt,
      failed_health.native_projection_generation,
      failed_health.projection_content_digest,
      failed_health,
      status_error,
  };
  if (!failed.presence_valid()) {
    throw std::logic_error(
        "native bootstrap status cannot represent uncertain state");
  }
  return failed;
}

core::NativeProjectionStageResultDTO
RetainedPlannerSceneProjection::StageProjection(
    const core::NativeProjectionStageDTO &request) {
  std::lock_guard lock(mutex_);
  if (!request.valid()) {
    throw std::invalid_argument("native projection stage request is invalid");
  }
  const auto rejected = [&](std::string message, std::string reason) {
    core::NativeProjectionStageResultDTO result{
        request.apply_id,
        request.apply_generation,
        request.projection_stage_generation,
        request.transition_row,
        request.expected_predecessor_projection_revision,
        request.candidate_projection_revision,
        core::NativeProjectionStageResultState::kRejected,
        std::nullopt,
        health_.active_projection_revision,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        error(
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            std::move(message), std::move(reason)),
    };
    return result;
  };
  if (request.transition_row.native_runtime_binding !=
          native_runtime_binding_ ||
      !exact_scene_content(*request.frozen_logical_scene.shared())) {
    return rejected("native stage does not match its exact verified product, "
                    "runtime binding, and frozen scene",
                    "native_projection_stage_mismatch");
  }
  if (stage_record_.has_value()) {
    if (stage_record_->request == request) {
      return stage_record_->stage_result;
    }
    if (!stage_record_->activated && !stage_record_->discarded) {
      return rejected("another exact native stage already owns this runtime",
                      "native_projection_stage_conflict");
    }
  }
  if (retire_record_.has_value()) {
    if (health_.state != core::ProjectionHealthState::kUninitialized ||
        health_.active_projection_revision.has_value()) {
      return rejected("a nonterminal native retirement still owns this "
                      "runtime",
                      "native_projection_retire_stage_conflict");
    }
  }
  const bool expected_present =
      request.transition_row.expected_local_active.has_value();
  if ((expected_present &&
       !HealthyAt(request.expected_predecessor_projection_revision)) ||
      (!expected_present &&
       (health_.state != core::ProjectionHealthState::kUninitialized ||
        health_.active_projection_revision.has_value())) ||
      health_.staged_candidate.has_value()) {
    return rejected("native stage observed a different local predecessor",
                    "native_projection_stage_predecessor_mismatch");
  }
  if (health_.source_sequence == std::numeric_limits<std::uint64_t>::max()) {
    return rejected("native stage health sequence is exhausted",
                    "native_projection_stage_sequence_exhausted");
  }
  const RetainedCollisionWorldProjector *candidate_projector = &projector_;
  const core::RetainedProductDTO *candidate_product =
      &planning_collision_model_;
  std::shared_ptr<const RetainedSceneFrameResolver> candidate_frame_resolver =
      frame_resolver_;
  const auto matches_request = [&](const auto &projector,
                                   const auto &product) {
    const auto &map = projector.model_map();
    return request.planning_collision_model == product &&
           request.candidate_projection_revision.retained_bundle_id ==
               map.retained_bundle_id &&
           request.candidate_projection_revision.graph_revision ==
               map.graph_revision &&
           request.frozen_logical_scene.shared()->cell_id == map.cell_id;
  };
  if (!matches_request(*candidate_projector, *candidate_product)) {
    if (!prepared_candidate_model_.has_value() ||
        !matches_request(prepared_candidate_model_->projector,
                         prepared_candidate_model_->planning_collision_model)) {
      return rejected("native candidate model was not prepared during "
                      "staged-ready convergence",
                      "native_projection_candidate_model_not_prepared");
    }
    candidate_projector = &prepared_candidate_model_->projector;
    candidate_product =
        &prepared_candidate_model_->planning_collision_model;
    candidate_frame_resolver = prepared_candidate_model_->frame_resolver;
  }
  auto candidate =
      BuildPublisher(*candidate_projector, *request.frozen_logical_scene.shared(),
                     core::CollisionWorldMutationSource::kDeploymentApply,
                     "native_projection_stage");
  if (!candidate.ok()) {
    return rejected(candidate.error().message,
                    "native_projection_stage_publication_rejected");
  }
  const auto digest = publisher_digest(candidate.value());
  if (!digest.has_value()) {
    return rejected("staged native projection has no immutable digest",
                    "native_projection_stage_digest_missing");
  }
  auto next_health = health_;
  ++next_health.source_sequence;
  next_health.staged_candidate = core::ProjectionStagedCandidateDTO{
      request.apply_id, request.apply_generation,
      request.projection_stage_generation,
      request.candidate_projection_revision};
  next_health.error = std::nullopt;
  core::NativeProjectionStageResultDTO result{
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.transition_row,
      request.expected_predecessor_projection_revision,
      request.candidate_projection_revision,
      core::NativeProjectionStageResultState::kStaged,
      request.candidate_projection_revision,
      next_health.active_projection_revision,
      next_health.native_projection_generation,
      *digest,
      next_health,
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw std::logic_error("native stage produced an invalid success result");
  }
  StageRecord record{request,
                     *candidate_projector,
                     *candidate_product,
                     std::move(candidate_frame_resolver),
                     std::move(candidate.value()),
                     result,
                     false,
                     false};
  health_ = std::move(next_health);
  retire_record_.reset();
  stage_record_ = std::move(record);
  return result;
}

core::NativeProjectionActivationResultDTO
RetainedPlannerSceneProjection::ActivateStagedProjection(
    const core::NativeProjectionActivationDTO &request) {
  std::lock_guard lock(mutex_);
  const auto stale = [&](std::string message, std::string reason) {
    core::NativeProjectionActivationResultDTO result{
        request,
        core::NativeProjectionActivationResultState::kStale,
        std::nullopt,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        error(
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            std::move(message), std::move(reason)),
    };
    return result;
  };
  if (!request.valid() || request.transition_row.native_runtime_binding !=
                              native_runtime_binding_) {
    return stale("native activation request is invalid for this runtime",
                 "native_projection_activation_mismatch");
  }
  if (!stage_record_.has_value() ||
      stage_record_->request.apply_id != request.apply_id ||
      stage_record_->request.apply_generation != request.apply_generation ||
      stage_record_->request.projection_stage_generation !=
          request.projection_stage_generation ||
      stage_record_->request.transition_row != request.transition_row ||
      stage_record_->request.expected_predecessor_projection_revision !=
          request.expected_predecessor_projection_revision ||
      stage_record_->request.candidate_projection_revision !=
          request.candidate_projection_revision) {
    return stale("native activation has no exact staged candidate",
                 "native_projection_activation_stage_absent");
  }
  if (stage_record_->discarded) {
    return stale("native candidate stage was already discarded",
                 "native_projection_activation_stage_discarded");
  }
  if (stage_record_->activated &&
      HealthyAt(request.candidate_projection_revision)) {
    core::NativeProjectionActivationResultDTO replay{
        request,
        core::NativeProjectionActivationResultState::kActivated,
        request.candidate_projection_revision,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        std::nullopt,
    };
    return replay;
  }
  const core::ProjectionStagedCandidateDTO expected_stage{
      request.apply_id, request.apply_generation,
      request.projection_stage_generation,
      request.candidate_projection_revision};
  if (health_.staged_candidate !=
          std::optional<core::ProjectionStagedCandidateDTO>{expected_stage} ||
      health_.active_projection_revision !=
          request.transition_row.expected_local_active ||
      !stage_record_->candidate_publisher) {
    return stale("native staged state no longer matches activation",
                 "native_projection_activation_state_stale");
  }
  if (health_.source_sequence == std::numeric_limits<std::uint64_t>::max() ||
      (health_.native_projection_generation.has_value() &&
       *health_.native_projection_generation ==
           std::numeric_limits<std::uint64_t>::max())) {
    return stale("native activation counters are exhausted",
                 "native_projection_activation_counter_exhausted");
  }
  const auto digest = publisher_digest(stage_record_->candidate_publisher);
  if (!digest.has_value()) {
    return stale("native staged projection has no immutable digest",
                 "native_projection_activation_digest_missing");
  }
  auto next_health = health_;
  ++next_health.source_sequence;
  next_health.state = core::ProjectionHealthState::kHealthy;
  next_health.active_projection_revision =
      request.candidate_projection_revision;
  next_health.staged_candidate = std::nullopt;
  next_health.native_projection_generation =
      health_.native_projection_generation.has_value()
          ? std::optional<std::uint64_t>(*health_.native_projection_generation +
                                         1U)
          : std::optional<std::uint64_t>(1U);
  next_health.projection_content_digest = *digest;
  next_health.error = std::nullopt;
  logical_snapshot_ = stage_record_->request.frozen_logical_scene.shared();
  transaction_records_.clear();
  projector_ = stage_record_->candidate_projector;
  planning_collision_model_ =
      stage_record_->candidate_planning_collision_model;
  frame_resolver_ = stage_record_->candidate_frame_resolver;
  prepared_candidate_model_.reset();
  active_publisher_.store(stage_record_->candidate_publisher,
                          std::memory_order_release);
  health_ = std::move(next_health);
  stage_record_->activated = true;
  core::NativeProjectionActivationResultDTO result{
      request,
      core::NativeProjectionActivationResultState::kActivated,
      request.candidate_projection_revision,
      health_.native_projection_generation,
      health_.projection_content_digest,
      health_,
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw std::logic_error(
        "native activation produced an invalid success result");
  }
  return result;
}

core::NativeProjectionRetireResultDTO
RetainedPlannerSceneProjection::RetireProjection(
    const core::NativeProjectionRetireDTO &request) {
  std::lock_guard lock(mutex_);
  const auto stale = [&](std::string message, std::string reason) {
    return core::NativeProjectionRetireResultDTO{
        request,
        core::NativeProjectionRetireResultState::kStale,
        health_.active_projection_revision,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        error(
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            std::move(message), std::move(reason)),
    };
  };
  if (!request.valid() || request.transition_row.native_runtime_binding !=
                              native_runtime_binding_) {
    return stale("native retirement request is invalid for this runtime",
                 "native_projection_retire_mismatch");
  }
  if (retire_record_.has_value()) {
    if (retire_record_->request.apply_id == request.apply_id &&
        retire_record_->request.apply_generation == request.apply_generation &&
        retire_record_->request.projection_stage_generation ==
            request.projection_stage_generation &&
        retire_record_->request.transition_row == request.transition_row &&
        retire_record_->request.expected_predecessor_projection_revision ==
            request.expected_predecessor_projection_revision &&
        retire_record_->request.candidate_projection_revision ==
            request.candidate_projection_revision) {
      return retire_record_->result;
    }
    return stale("another exact retirement already owns this runtime",
                 "native_projection_retire_conflict");
  }
  if (!health_.active_projection_revision.has_value() &&
      health_.state == core::ProjectionHealthState::kUninitialized) {
    core::NativeProjectionRetireResultDTO result{
        request,
        core::NativeProjectionRetireResultState::kAlreadyAbsent,
        std::nullopt,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        std::nullopt,
    };
    retire_record_ = RetireRecord{request, result};
    return result;
  }
  if (!HealthyAt(request.expected_predecessor_projection_revision) ||
      (stage_record_.has_value() && !stage_record_->activated &&
       !stage_record_->discarded) ||
      !retained_lease_publishers_.empty()) {
    return stale("native retirement cannot prove an exact drained predecessor",
                 "native_projection_retire_predecessor_unavailable");
  }
  if (health_.source_sequence == std::numeric_limits<std::uint64_t>::max() ||
      !health_.native_projection_generation.has_value() ||
      *health_.native_projection_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
    return stale("native retirement counters are exhausted",
                 "native_projection_retire_counter_exhausted");
  }
  auto next_health = health_;
  ++next_health.source_sequence;
  ++*next_health.native_projection_generation;
  next_health.state = core::ProjectionHealthState::kUninitialized;
  next_health.active_projection_revision = std::nullopt;
  next_health.staged_candidate = std::nullopt;
  next_health.projection_content_digest = std::nullopt;
  next_health.error = std::nullopt;
  active_publisher_.store(nullptr, std::memory_order_release);
  logical_snapshot_.reset();
  transaction_records_.clear();
  stage_record_.reset();
  prepared_candidate_model_.reset();
  health_ = std::move(next_health);
  core::NativeProjectionRetireResultDTO result{
      request,      core::NativeProjectionRetireResultState::kRetired,
      std::nullopt, health_.native_projection_generation,
      std::nullopt, health_,
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw std::logic_error(
        "native retirement produced an invalid success result");
  }
  retire_record_ = RetireRecord{request, result};
  return result;
}

core::NativeProjectionRetireStatusDTO
RetainedPlannerSceneProjection::ProjectionRetireStatus(
    const core::NativeProjectionRetireStatusQueryDTO &query) const {
  std::lock_guard lock(mutex_);
  if (!query.valid() || query.request.transition_row.native_runtime_binding !=
                            native_runtime_binding_) {
    throw std::invalid_argument(
        "native retirement status query is invalid for this runtime");
  }
  auto state = core::NativeProjectionRetireStatusState::kPending;
  std::optional<core::ErrorDTO> status_error;
  if (retire_record_.has_value()) {
    if (retire_record_->request.apply_id != query.request.apply_id ||
        retire_record_->request.apply_generation !=
            query.request.apply_generation ||
        retire_record_->request.projection_stage_generation !=
            query.request.projection_stage_generation ||
        retire_record_->request.transition_row !=
            query.request.transition_row ||
        retire_record_->request.expected_predecessor_projection_revision !=
            query.request.expected_predecessor_projection_revision ||
        retire_record_->request.candidate_projection_revision !=
            query.request.candidate_projection_revision) {
      state = core::NativeProjectionRetireStatusState::kStale;
      status_error = error("invariant_violation",
                           "another retirement owns this native runtime",
                           "native_projection_retire_status_conflict");
    } else {
      state = retire_record_->result.state ==
                      core::NativeProjectionRetireResultState::kAlreadyAbsent
                  ? core::NativeProjectionRetireStatusState::kAlreadyAbsent
                  : core::NativeProjectionRetireStatusState::kRetired;
    }
  } else if (health_.active_projection_revision !=
             query.request.transition_row.expected_local_active) {
    state = core::NativeProjectionRetireStatusState::kStale;
    status_error = error("invariant_violation",
                         "native retirement predecessor is no longer exact",
                         "native_projection_retire_status_stale");
  }
  core::NativeProjectionRetireStatusDTO result{
      query.request,
      state,
      health_.active_projection_revision,
      health_.native_projection_generation,
      health_.projection_content_digest,
      health_,
      std::move(status_error),
  };
  if (!result.presence_valid()) {
    throw std::logic_error("native retirement status result is invalid");
  }
  return result;
}

core::NativeProjectionDiscardResultDTO
RetainedPlannerSceneProjection::DiscardStagedProjection(
    const core::NativeProjectionDiscardDTO &request) {
  std::lock_guard lock(mutex_);
  const auto failed = [&](std::string message, std::string reason) {
    return core::NativeProjectionDiscardResultDTO{
        request,
        core::NativeProjectionDiscardResultState::kFailedProjectionUncertain,
        health_.active_projection_revision,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        error("projection_state_uncertain", std::move(message),
              std::move(reason)),
    };
  };
  if (!request.valid() || request.transition_row.native_runtime_binding !=
                              native_runtime_binding_) {
    return failed("native discard request is invalid for this runtime",
                  "native_projection_discard_mismatch");
  }
  if (!stage_record_.has_value()) {
    core::NativeProjectionDiscardResultDTO result{
        request,
        core::NativeProjectionDiscardResultState::kAlreadyAbsent,
        health_.active_projection_revision,
        health_.native_projection_generation,
        health_.projection_content_digest,
        health_,
        std::nullopt,
    };
    return result;
  }
  if (stage_record_->request.apply_id != request.apply_id ||
      stage_record_->request.apply_generation != request.apply_generation ||
      stage_record_->request.projection_stage_generation !=
          request.projection_stage_generation ||
      stage_record_->request.transition_row != request.transition_row ||
      stage_record_->request.candidate_projection_revision !=
          request.candidate_projection_revision ||
      stage_record_->activated) {
    return failed("native candidate stage cannot be discarded exactly",
                  "native_projection_discard_conflict");
  }
  if (!stage_record_->discarded) {
    stage_record_->candidate_publisher.reset();
    stage_record_->discarded = true;
    health_.staged_candidate = std::nullopt;
    if (health_.source_sequence != std::numeric_limits<std::uint64_t>::max()) {
      ++health_.source_sequence;
    }
  }
  core::NativeProjectionDiscardResultDTO result{
      request,
      core::NativeProjectionDiscardResultState::kDiscarded,
      health_.active_projection_revision,
      health_.native_projection_generation,
      health_.projection_content_digest,
      health_,
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw std::logic_error("native discard produced an invalid success result");
  }
  return result;
}

core::NativeProjectionStatusDTO
RetainedPlannerSceneProjection::ProjectionStageStatus(
    const core::NativeProjectionStatusQueryDTO &query) const {
  std::lock_guard lock(mutex_);
  if (!query.valid() ||
      query.transition_row.native_runtime_binding != native_runtime_binding_) {
    throw std::invalid_argument(
        "native stage status query is invalid for this runtime");
  }
  auto state = core::NativeProjectionStatusState::kAbsent;
  std::optional<core::ProjectionRevisionDTO> candidate;
  std::optional<core::ErrorDTO> status_error;
  if (stage_record_.has_value()) {
    const bool exact =
        stage_record_->request.apply_id == query.apply_id &&
        stage_record_->request.apply_generation == query.apply_generation &&
        stage_record_->request.projection_stage_generation ==
            query.projection_stage_generation &&
        stage_record_->request.transition_row == query.transition_row;
    if (!exact) {
      state = core::NativeProjectionStatusState::kFailedProjectionUncertain;
      status_error = error("projection_state_uncertain",
                           "another stage owns this native runtime",
                           "native_projection_stage_status_conflict");
    } else if (stage_record_->discarded) {
      state = core::NativeProjectionStatusState::kDiscarded;
    } else {
      candidate = stage_record_->request.candidate_projection_revision;
      state = stage_record_->activated
                  ? core::NativeProjectionStatusState::kActivated
                  : core::NativeProjectionStatusState::kStaged;
    }
  }
  core::NativeProjectionStatusDTO result{
      query,
      state,
      std::move(candidate),
      health_.active_projection_revision,
      health_.native_projection_generation,
      health_.projection_content_digest,
      health_,
      std::move(status_error),
  };
  if (!result.presence_valid()) {
    throw std::logic_error("native stage status result is invalid");
  }
  return result;
}

core::Result<core::SceneFrameResolutionReceiptDTO>
RetainedPlannerSceneProjection::ResolveSceneFrame(
    const core::SceneFrameResolutionQueryDTO &query) const {
  std::lock_guard lock(mutex_);
  if (!query.valid() || !frame_resolver_ ||
      !HealthyAt(query.projection_binding.projection_revision) ||
      retained_lease_publishers_.find(
          query.projection_binding.projection_lease_id) ==
          retained_lease_publishers_.end()) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "scene-frame query does not name one retained healthy native "
        "projection",
        "native_scene_frame_lease_unavailable");
  }
  std::int64_t now_ns = 0;
  try {
    now_ns = clock_();
  } catch (const std::exception &exception) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "scene-frame resolution clock failed: " +
            std::string(exception.what()),
        "native_scene_frame_clock_failed");
  } catch (...) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "scene-frame resolution clock failed",
        "native_scene_frame_clock_failed");
  }
  auto resolved =
      frame_resolver_->Resolve(query, logical_snapshot_, now_ns);
  if (!resolved.ok()) {
    return core::Result<core::SceneFrameResolutionReceiptDTO>::Err(
        resolved.error());
  }
  try {
    auto receipt = core::MakeSceneFrameResolutionReceipt(resolved.value());
    if (!receipt.valid() ||
        receipt.projection_binding != query.projection_binding) {
      return projection_error<core::SceneFrameResolutionReceiptDTO>(
          "scene-frame resolver returned a foreign or invalid receipt",
          "native_scene_frame_receipt_invalid");
    }
    return core::Result<core::SceneFrameResolutionReceiptDTO>::Ok(
        std::move(receipt));
  } catch (const std::exception &exception) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "scene-frame receipt construction failed: " +
            std::string(exception.what()),
        "native_scene_frame_receipt_invalid");
  }
}

core::Result<core::CollisionPolicySnapshot>
RetainedPlannerSceneProjection::ProjectEffectiveCollisionPolicy(
    const core::LogicalSceneSnapshotDTO &logical_scene,
    const std::uint64_t effective_policy_revision_id) const {
  std::lock_guard lock(mutex_);
  if (health_.state != core::ProjectionHealthState::kHealthy ||
      !health_.active_projection_revision.has_value() ||
      !logical_snapshot_ ||
      health_.active_projection_revision->retained_bundle_id !=
          projector_.model_map().retained_bundle_id ||
      health_.active_projection_revision->graph_revision !=
          projector_.model_map().graph_revision ||
      logical_scene.cell_id != projector_.model_map().cell_id) {
    return projection_error<core::CollisionPolicySnapshot>(
        "effective policy requires one healthy exact retained projection",
        "native_collision_policy_projection_unavailable");
  }
  return projector_.project_effective_policy(logical_scene,
                                              effective_policy_revision_id);
}

std::shared_ptr<const core::LogicalSceneSnapshotDTO>
RetainedPlannerSceneProjection::logical_snapshot() const {
  std::lock_guard lock(mutex_);
  return logical_snapshot_;
}

collision::ManyForgeCollisionEngine::SharedModelPtr
RetainedPlannerSceneProjection::collision_shared_model() const {
  std::lock_guard lock(mutex_);
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
