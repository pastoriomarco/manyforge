#include <algorithm>
#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#endif
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <stdexcept>
#include <string_view>
#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
#include <thread>
#endif
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

/// Projection content identity: the collision world is derived from the
/// logical scene over one fixed retained model, so the scene content names it.
core::ContentDigest
projection_digest(const core::LogicalSceneSnapshotDTO &scene) {
  std::string preimage("manyforge.native-projection-content.v2");
  preimage.push_back('\0');
  preimage.append(scene.revision.content_hash.value());
  return core::ContentDigest(
      manyforge::content_identity::sha256_digest(preimage));
}

#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
// Process-fixture synchronization at the actual serialized construction
// boundary. This function and both call sites are absent from ordinary hosts.
// The finite deadline fails closed rather than leaving a fixture child holding
// the projection mutex when its test process cannot publish a release marker.
bool projection_construction_test_operation_selected(
    std::string_view selected_operations, const std::string_view operation) {
  while (!selected_operations.empty()) {
    const auto separator = selected_operations.find(',');
    if (selected_operations.substr(0, separator) == operation) {
      return true;
    }
    if (separator == std::string_view::npos) {
      return false;
    }
    selected_operations.remove_prefix(separator + 1U);
  }
  return false;
}

void projection_construction_test_boundary(
    const std::string_view operation,
    const core::ProjectionExecutionHostBindingDTO &binding) {
  const char *const root =
      std::getenv("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR");
  if (root == nullptr || *root == '\0') {
    return;
  }
  const auto control_root = std::filesystem::path(root);
  {
    static std::mutex trace_mutex;
    std::lock_guard trace_lock(trace_mutex);
    std::ofstream trace(control_root / "trace.log",
                        std::ios::binary | std::ios::app);
    if (!trace) {
      throw std::runtime_error(
          "native projection construction trace could not be opened");
    }
    trace << "execute " << operation << ' '
          << binding.provider_binding_id.value() << '\n';
    if (!trace) {
      throw std::runtime_error(
          "native projection construction trace could not be written");
    }
  }
  const char *const held =
      std::getenv("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD");
  if (held != nullptr && projection_construction_test_operation_selected(
                             held, operation)) {
    std::ofstream entered(
        control_root / (std::string(operation) + ".entered"),
        std::ios::binary | std::ios::trunc);
    if (!entered) {
      throw std::runtime_error(
          "native projection construction barrier could not be entered");
    }
    entered << binding.provider_binding_id.value() << '\n';
    entered.close();
    constexpr auto kDiagnosticDeadline = std::chrono::seconds(30);
    const auto deadline = std::chrono::steady_clock::now() +
                          kDiagnosticDeadline;
    const auto release =
        control_root / (std::string(operation) + ".release");
    while (!std::filesystem::exists(release) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    if (!std::filesystem::exists(release)) {
      throw std::runtime_error(
          "native projection construction barrier exceeded its fixed "
          "30-second diagnostic deadline");
    }
  }
  const char *const failed =
      std::getenv("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL");
  if (failed != nullptr && std::string_view(failed) == operation) {
    throw std::runtime_error("injected native projection construction failure");
  }
}
#endif

} // namespace

RetainedPlannerSceneProjection::RetainedPlannerSceneProjection(
    RetainedCollisionWorldProjector projector,
    core::RetainedProductDTO planning_collision_model,
    core::ProjectionExecutionHostBindingDTO execution_host_binding,
    RetainedPlannerSceneProjectionConfig config, Clock clock,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver)
    : projector_(std::move(projector)),
      planning_collision_model_(std::move(planning_collision_model)),
      frame_resolver_(std::move(frame_resolver)),
      execution_host_binding_(std::move(execution_host_binding)),
      config_(std::move(config)), clock_(std::move(clock)),
      health_{execution_host_binding_,
              1U,
              core::ProjectionHealthState::kUninitialized,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt} {
  build_worker_ = std::thread([this] { RunCollisionWorldBuilds(); });
}

RetainedPlannerSceneProjection::~RetainedPlannerSceneProjection() {
  stopping_.store(true, std::memory_order_release);
  build_wake_.fetch_add(1U, std::memory_order_acq_rel);
  build_wake_.notify_one();
  if (build_worker_.joinable()) {
    build_worker_.join();
  }
}

void RetainedPlannerSceneProjection::RunCollisionWorldBuilds() {
  std::uint64_t seen = 0U;
  for (;;) {
    build_wake_.wait(seen, std::memory_order_acquire);
    seen = build_wake_.load(std::memory_order_acquire);
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    std::lock_guard lock(mutex_);
    BuildCollisionWorldLocked();
  }
}

void RetainedPlannerSceneProjection::BuildCollisionWorldLocked() {
  const auto change = scene_change_.load(std::memory_order_acquire);
  if (built_change_.load(std::memory_order_acquire) == change ||
      health_.state != core::ProjectionHealthState::kHealthy ||
      !logical_snapshot_ || !health_.active_projection_revision) {
    return;
  }
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  if (!publisher) {
    return;
  }
  std::optional<std::string> failure;
  try {
    const auto observed_at = clock_();
    const auto published =
        publisher->publish(*logical_snapshot_, std::max<std::int64_t>(observed_at, 0));
    if (!published.ok()) {
      failure = published.error().message;
    }
  } catch (const std::exception &exception) {
    failure = exception.what();
  } catch (...) {
    failure = "collision-world build ended without a result";
  }
  if (failure.has_value()) {
    static_cast<void>(Desynchronize(
        std::nullopt, *health_.active_projection_revision,
        error("projection_state_uncertain",
              "collision-world build failed: " + *failure,
              "native_projection_collision_world_build_failed")));
    return;
  }
  built_change_.store(change, std::memory_order_release);
}

void RetainedPlannerSceneProjection::RefreshCollisionWorld() const {
  if (built_change_.load(std::memory_order_acquire) ==
      scene_change_.load(std::memory_order_acquire)) {
    return;
  }
  // The cache is logically const: rebuilding it never changes the scene.
  auto &self = const_cast<RetainedPlannerSceneProjection &>(*this);
  std::lock_guard lock(self.mutex_);
  self.BuildCollisionWorldLocked();
}

void RetainedPlannerSceneProjection::RequestCollisionWorldBuild()
    const noexcept {
  const auto change = scene_change_.load(std::memory_order_acquire);
  if (built_change_.load(std::memory_order_acquire) == change ||
      requested_change_.exchange(change, std::memory_order_acq_rel) ==
          change) {
    return;
  }
  build_wake_.fetch_add(1U, std::memory_order_acq_rel);
  build_wake_.notify_one();
}

core::Result<std::shared_ptr<RetainedPlannerSceneProjection>>
RetainedPlannerSceneProjection::CreateUninitialized(
    RetainedCollisionWorldProjector projector,
    core::RetainedProductDTO planning_collision_model,
    core::ProjectionExecutionHostBindingDTO execution_host_binding,
    RetainedPlannerSceneProjectionConfig config, Clock clock,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
  if (planning_collision_model.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      planning_collision_model.product_id.empty() ||
      planning_collision_model.product_contract_id.empty() ||
      planning_collision_model.product_contract_revision.empty() ||
      !execution_host_binding.valid() || config.publisher_id.empty() ||
      config.transaction_replay_capacity == 0U ||
      config.transaction_replay_capacity > 4096U || !clock) {
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
                std::move(execution_host_binding), std::move(config),
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
    core::ProjectionExecutionHostBindingDTO execution_host_binding,
    core::LogicalSceneSnapshotDTO initial_scene,
    RetainedPlannerSceneProjectionConfig config, Clock clock,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
  auto result = CreateUninitialized(
      std::move(projector), std::move(planning_collision_model),
      std::move(execution_host_binding), std::move(config), std::move(clock),
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
  auto publisher =
      projection->BuildPublisher(projection->projector_, initial_scene);
  if (!publisher.ok()) {
    return projection_error<std::shared_ptr<RetainedPlannerSceneProjection>>(
        publisher.error().message,
        "native_projection_initial_publication_failed");
  }
  const auto digest = projection_digest(initial_scene);
  auto snapshot = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      std::move(initial_scene));
  projection->logical_snapshot_ = std::move(snapshot);
  projection->health_ = {
      projection->execution_host_binding_,
      2U,
      core::ProjectionHealthState::kHealthy,
      revision,
      1U,
      digest,
      std::nullopt,
  };
  projection->active_publisher_.store(std::move(publisher.value()),
                                      std::memory_order_release);
  return result;
}

core::Result<void> RetainedPlannerSceneProjection::InstallActiveFrameResolver(
    const core::ProjectionRevisionDTO &expected_active_revision,
    std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
  std::lock_guard lock(mutex_);
  if (!expected_active_revision.valid() || !frame_resolver ||
      !frame_resolver->matches_structural_revision(
          expected_active_revision.retained_bundle_id,
          expected_active_revision.graph_revision)) {
    return projection_error<void>(
        "active native frame resolver is absent or belongs to another "
        "retained product",
        "native_projection_active_frame_resolver_invalid");
  }
  // The resolver is structural; any revision of the same Scene is current.
  if (!health_.healthy_for_identity(expected_active_revision) ||
      !logical_snapshot_) {
    return projection_error<void>(
        "active native frame resolver does not name the exact active "
        "projection revision",
        "native_projection_active_frame_resolver_stale");
  }
  frame_resolver_ = std::move(frame_resolver);
  return core::Result<void>::Ok();
}

core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
RetainedPlannerSceneProjection::BuildPublisher(
    const RetainedCollisionWorldProjector &projector,
    const core::LogicalSceneSnapshotDTO &snapshot) const {
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
  if (next_publisher_incarnation_ ==
      std::numeric_limits<std::uint64_t>::max()) {
    return projection_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "native projection publisher incarnations are exhausted",
        "native_projection_publisher_incarnation_exhausted");
  }
  const auto publisher_id = config_.publisher_id + ".incarnation." +
                            std::to_string(next_publisher_incarnation_++);
  return RetainedCollisionWorldPublisher::create(projector, publisher_id,
                                                 snapshot, observed_at, clock_);
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
  if (next_records.size() >= config_.transaction_replay_capacity) {
    next_records.erase(next_records.begin());
  }
  next_records.emplace(transaction.transaction_sequence,
                       TransactionRecord{transaction, success});

  if (!no_native_change) {
    // The collision world is rebuilt lazily by its next consumer.
    next_health.projection_content_digest =
        projection_digest(*candidate_snapshot);
    success.resulting_health = next_health;
    next_records.at(transaction.transaction_sequence).result = success;
  }

  logical_snapshot_ = std::move(candidate_snapshot);
  health_ = std::move(next_health);
  transaction_records_.swap(next_records);
  if (!no_native_change) {
    scene_change_.fetch_add(1U, std::memory_order_acq_rel);
  }
  return success;
}

core::Result<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::RetainProjection(
    const core::ProjectionRevisionDTO &projection_revision,
    core::ProjectionLeaseId projection_lease_id) {
  std::lock_guard lock(mutex_);
  return RetainProjectionLocked(projection_revision,
                                std::move(projection_lease_id));
}

core::Result<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::RetainProjectionLocked(
    const core::ProjectionRevisionDTO &projection_revision,
    core::ProjectionLeaseId projection_lease_id) {
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
    if (retained->second.projection_binding.projection_revision ==
        projection_revision) {
      return core::Result<core::CollisionWorldLeaseInventory>::Ok(
          retained->second);
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
  if (retained_lease_publishers_.size() >= kMaxRetainedProjectionLeases) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease retention reached its fixed capacity",
        "native_projection_lease_capacity_exhausted");
  }
  BuildCollisionWorldLocked();
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  if (!publisher) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection publisher is unavailable",
        "native_projection_lease_unavailable");
  }
  std::int64_t captured_at_ns = 0;
  try {
    captured_at_ns = clock_();
  } catch (const std::exception &exception) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease clock failed: " +
            std::string(exception.what()),
        "native_projection_lease_clock_failed");
  } catch (...) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease clock failed",
        "native_projection_lease_clock_failed");
  }
  if (captured_at_ns < 0) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection lease clock returned a negative timestamp",
        "native_projection_lease_clock_invalid");
  }
  auto inventory = publisher->current_scene_snapshot_lease_inventory();
  if (!inventory.has_value() || !inventory->logical_scene ||
      inventory->projection_binding.projection_revision !=
          projection_revision) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "native projection publisher could not retain the exact revision",
        "native_projection_lease_unavailable");
  }
  inventory->projection_binding.projection_lease_id = projection_lease_id;
  inventory->captured_at_ns = captured_at_ns;
  const auto [inserted, was_inserted] =
      retained_lease_publishers_.emplace(projection_lease_id, *inventory);
  if (!was_inserted) {
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
  return core::Result<core::CollisionWorldLeaseInventory>::Ok(
      std::move(*inventory));
}

core::Result<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::RetainCurrentProjection(core::ProjectionLeaseId lease_id) {
  std::lock_guard lock(mutex_);
  if (!health_.active_projection_revision) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "current native planning projection is unavailable", "native_projection_lease_unavailable");
  }
  return RetainProjectionLocked(*health_.active_projection_revision, std::move(lease_id));
}

std::shared_ptr<const core::LogicalSceneSnapshotDTO>
RetainedPlannerSceneProjection::RetainedScene(const core::ProjectionLeaseBindingDTO &binding) const {
  const auto retained = retained_lease_publishers_.find(binding.projection_lease_id);
  if (!binding.valid() || health_.state != core::ProjectionHealthState::kHealthy ||
      !health_.active_projection_revision || !logical_snapshot_ ||
      !core::ProjectionIsSameOrForwardInLineage(*health_.active_projection_revision,
                                               binding.projection_revision) ||
      retained == retained_lease_publishers_.end() || !retained->second.logical_scene ||
      retained->second.logical_scene->revision != binding.projection_revision.scene_revision) {
    return nullptr;
  }
  return retained->second.logical_scene;
}

void RetainedPlannerSceneProjection::ReleaseProjection(
    const core::ProjectionLeaseId &projection_lease_id) {
  std::lock_guard lock(mutex_);
  const auto retained = retained_lease_publishers_.find(projection_lease_id);
  if (retained == retained_lease_publishers_.end()) {
    return;
  }
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
  return RebuildProjectionLocked(request,
                                 RebuildConstructionContext::kOrdinary);
}

core::ProjectionCommitResultDTO
RetainedPlannerSceneProjection::RebuildProjectionLocked(
    const core::NativeProjectionRebuildDTO &request,
    [[maybe_unused]] const RebuildConstructionContext construction_context) {
  const auto invalid = [&](std::string message, std::string reason) {
    return RollbackProven(std::nullopt, request.target_projection_revision,
                          error(std::string(core::to_string(
                                    core::ErrorCode::kInvariantViolation)),
                                std::move(message), std::move(reason)));
  };
  if (!request.valid() ||
      request.expected_execution_host_binding != execution_host_binding_ ||
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
    if (request.maintenance_successor.has_value() &&
        (!maintenance_successor_.has_value() ||
         *maintenance_successor_ != request)) {
      return invalid("maintenance successor replay changed its exact Apply "
                     "identity", "native_projection_maintenance_conflict");
    }
    return {std::nullopt,
            request.target_projection_revision,
            core::ProjectionCommitResultStateDTO::kCommitted,
            request.target_projection_revision,
            health_.native_projection_generation,
            false,
            health_,
            std::nullopt};
  }
  if (request.maintenance_successor.has_value()) {
    if (!HealthyAt(*request.maintenance_predecessor) ||
        request.target_projection_revision.scene_revision.revision_id <=
            request.maintenance_predecessor->scene_revision.revision_id ||
        health_.source_sequence >
            std::numeric_limits<std::uint64_t>::max() - 1U ||
        (health_.native_projection_generation.has_value() &&
         *health_.native_projection_generation ==
             std::numeric_limits<std::uint64_t>::max())) {
      return invalid("maintenance successor has no exact healthy fenced "
                     "predecessor", "native_projection_maintenance_stale");
    }
    auto publisher = active_publisher_.load(std::memory_order_acquire);
    if (!publisher) {
      return invalid("maintenance publisher is unavailable",
                     "native_projection_maintenance_unavailable");
    }
    try {
      const auto observed_at = clock_();
      if (observed_at < 0) {
        return invalid("maintenance clock returned a negative timestamp",
                       "native_projection_maintenance_clock_invalid");
      }
      const auto published =
          publisher->publish(*request.logical_scene, observed_at);
      if (!published.ok()) {
        return Desynchronize(
            std::nullopt, request.target_projection_revision,
            error("projection_state_uncertain", published.error().message,
                  "native_projection_maintenance_publication_uncertain"));
      }
      ++health_.source_sequence;
      health_.active_projection_revision = request.target_projection_revision;
      health_.native_projection_generation =
          health_.native_projection_generation.value_or(0U) + 1U;
      health_.projection_content_digest =
          projection_digest(*request.logical_scene);
      built_change_.store(scene_change_.load(std::memory_order_acquire),
                          std::memory_order_release);
      logical_snapshot_ = request.logical_scene;
      transaction_records_.clear();
      maintenance_successor_ = request;
      return {std::nullopt, request.target_projection_revision,
              core::ProjectionCommitResultStateDTO::kCommitted,
              request.target_projection_revision,
              health_.native_projection_generation, false, health_,
              std::nullopt};
    } catch (const std::exception &exception) {
      return Desynchronize(
          std::nullopt, request.target_projection_revision,
          error("projection_state_uncertain", exception.what(),
                "native_projection_maintenance_effect_uncertain"));
    } catch (...) {
      return Desynchronize(
          std::nullopt, request.target_projection_revision,
          error("projection_state_uncertain",
                "maintenance publication ended without a result",
                "native_projection_maintenance_effect_uncertain"));
    }
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
  health_.error = std::nullopt;
  active_publisher_.store(nullptr, std::memory_order_release);
#if defined(MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY)
  projection_construction_test_boundary(
      construction_context == RebuildConstructionContext::kBootstrap
          ? "bootstrap_rebuild"
          : "rebuild",
      execution_host_binding_);
#endif
  auto rebuilt = BuildPublisher(projector_, *request.logical_scene);
  if (!rebuilt.ok()) {
    return Desynchronize(std::nullopt, request.target_projection_revision,
                         error("projection_state_uncertain",
                               rebuilt.error().message,
                               "native_projection_rebuild_failed"));
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
  next_health.projection_content_digest =
      projection_digest(*request.logical_scene);
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
  built_change_.store(scene_change_.load(std::memory_order_acquire),
                      std::memory_order_release);
  return success;
}

core::NativeProjectionBootstrapRebuildResultDTO
RetainedPlannerSceneProjection::RebuildBootstrapProjection(
    const core::NativeProjectionBootstrapRebuildDTO &request) {
  std::lock_guard lock(mutex_);
  if (!request.valid() ||
      request.execution_host_binding != execution_host_binding_ ||
      request.planning_collision_model != planning_collision_model_) {
    throw std::invalid_argument(
        "native bootstrap rebuild request is invalid for this runtime");
  }
  const bool already_exact =
      HealthyAt(request.canonical_empty_target) && logical_snapshot_ &&
      *logical_snapshot_ == *request.initialized_logical_scene.shared();
  const core::NativeProjectionRebuildDTO rebuild{
      request.execution_host_binding,
      request.canonical_empty_target,
      request.planning_collision_model,
      request.initialized_logical_scene.shared(),
  };
  auto result = RebuildProjectionLocked(
      rebuild, RebuildConstructionContext::kBootstrap);
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
        request.execution_host_binding,
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
      request.execution_host_binding,
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
      query.execution_host_binding != execution_host_binding_) {
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
        query.execution_host_binding,
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
        query.execution_host_binding,
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
        query.execution_host_binding,
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
      query.execution_host_binding,
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

core::Result<core::SceneFrameResolutionReceiptDTO>
RetainedPlannerSceneProjection::ResolveSceneFrame(
    const core::SceneFrameResolutionQueryDTO &query) const {
  std::lock_guard lock(mutex_);
  const auto retained_scene = RetainedScene(query.projection_binding);
  if (!query.valid() || !frame_resolver_ || !retained_scene) {
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
        "scene-frame resolution clock failed: " + std::string(exception.what()),
        "native_scene_frame_clock_failed");
  } catch (...) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "scene-frame resolution clock failed",
        "native_scene_frame_clock_failed");
  }
  auto resolved = frame_resolver_->Resolve(query, retained_scene, now_ns);
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

core::Result<core::SceneFrameResolutionReceiptDTO>
RetainedPlannerSceneProjection::ResolveCandidateSceneFrame(
    const core::CandidateSceneFrameResolutionQueryDTO &query) const {
  std::lock_guard lock(mutex_);
  const auto retained_scene = RetainedScene(query.projection_binding);
  if (!query.valid() || !frame_resolver_ || !retained_scene) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "candidate scene-frame query does not name one retained healthy "
        "native projection",
        "native_candidate_scene_frame_lease_unavailable");
  }
  std::int64_t now_ns = 0;
  try {
    now_ns = clock_();
  } catch (const std::exception &exception) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "candidate scene-frame resolution clock failed: " +
            std::string(exception.what()),
        "native_candidate_scene_frame_clock_failed");
  } catch (...) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "candidate scene-frame resolution clock failed",
        "native_candidate_scene_frame_clock_failed");
  }
  auto resolved =
      frame_resolver_->ResolveCandidate(query, retained_scene, now_ns);
  if (!resolved.ok()) {
    return core::Result<core::SceneFrameResolutionReceiptDTO>::Err(
        resolved.error());
  }
  try {
    auto receipt = core::MakeSceneFrameResolutionReceipt(resolved.value());
    if (!receipt.valid() ||
        receipt.projection_binding != query.projection_binding ||
        !receipt.predicted_state_digest.has_value()) {
      return projection_error<core::SceneFrameResolutionReceiptDTO>(
          "candidate scene-frame resolver returned foreign, observed, or "
          "invalid evidence",
          "native_candidate_scene_frame_receipt_invalid");
    }
    return core::Result<core::SceneFrameResolutionReceiptDTO>::Ok(
        std::move(receipt));
  } catch (const std::exception &exception) {
    return projection_error<core::SceneFrameResolutionReceiptDTO>(
        "candidate scene-frame receipt construction failed: " +
            std::string(exception.what()),
        "native_candidate_scene_frame_receipt_invalid");
  }
}

core::Result<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::ProjectSceneCandidate(
    const core::SceneCandidatePreviewDTO &candidate,
    const core::ProjectionLeaseBindingDTO &predecessor_binding,
    core::ProjectionLeaseId candidate_lease_id) const {
  std::lock_guard lock(mutex_);
  if (!candidate.valid() || !predecessor_binding.valid() ||
      candidate_lease_id.empty() ||
      candidate.prior_projection_revision !=
          predecessor_binding.projection_revision ||
      !RetainedScene(predecessor_binding)) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene candidate does not descend from one retained healthy native "
        "projection",
        "native_scene_candidate_predecessor_unavailable");
  }

  std::int64_t now_ns = 0;
  try {
    now_ns = clock_();
  } catch (const std::exception &exception) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene-candidate projection clock failed: " +
            std::string(exception.what()),
        "native_scene_candidate_clock_failed");
  } catch (...) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene-candidate projection clock failed",
        "native_scene_candidate_clock_failed");
  }

  auto policy = projector_.project_effective_policy(
      *candidate.logical_scene,
      candidate.effective_collision_policy.revision.revision_id);
  if (!policy.ok() || policy.value() != candidate.effective_collision_policy) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene candidate effective policy differs from the exact retained "
        "projector",
        "native_scene_candidate_policy_mismatch");
  }
  const std::string publisher_id =
      "candidate-" +
      candidate.proposed_projection_revision.scene_revision.content_hash.value()
          .substr(0U, 23U) +
      "-" + candidate.request_digest.value().substr(7U, 8U);
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  if (!publisher) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene-candidate collision revision owner became unavailable",
        "native_scene_candidate_revision_owner_unavailable");
  }
  auto collision_world_revision =
      publisher->reserve_private_collision_world_revision();
  if (!collision_world_revision.ok()) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene-candidate collision revision reservation failed: " +
            collision_world_revision.error().message,
        "native_scene_candidate_revision_reservation_failed");
  }
  auto world = projector_.project(
      *candidate.logical_scene,
      collision_world_revision.value(), now_ns,
      publisher_id);
  if (!world.ok()) {
    return projection_error<core::CollisionWorldLeaseInventory>(
        "scene-candidate collision projection failed: " + world.error().message,
        "native_scene_candidate_projection_failed");
  }
  auto world_snapshot = std::make_shared<const core::CollisionWorldSnapshot>(
      std::move(world.value()));
  core::CollisionWorldLeaseInventory inventory;
  inventory.projection_binding = {candidate.proposed_projection_revision,
                                  std::move(candidate_lease_id)};
  inventory.logical_scene = candidate.logical_scene;
  inventory.captured_at_ns = now_ns;
  inventory.bundle = {world_snapshot, std::move(policy.value())};
  return core::Result<core::CollisionWorldLeaseInventory>::Ok(
      std::move(inventory));
}

core::Result<core::CollisionPolicySnapshot>
RetainedPlannerSceneProjection::ProjectEffectiveCollisionPolicy(
    const core::LogicalSceneSnapshotDTO &logical_scene,
    const std::uint64_t effective_policy_revision_id) const {
  std::lock_guard lock(mutex_);
  if (health_.state != core::ProjectionHealthState::kHealthy ||
      !health_.active_projection_revision.has_value() || !logical_snapshot_ ||
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
  RefreshCollisionWorld();
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->snapshot() : nullptr;
}

core::CollisionWorldRevision
RetainedPlannerSceneProjection::published_collision_world_revision() const {
  RefreshCollisionWorld();
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->published_collision_world_revision()
                   : core::CollisionWorldRevision{};
}

std::optional<core::CollisionPolicySnapshot>
RetainedPlannerSceneProjection::collision_policy_snapshot() const {
  RefreshCollisionWorld();
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->collision_policy_snapshot() : std::nullopt;
}

core::CollisionWorldPolicySnapshotBundle
RetainedPlannerSceneProjection::collision_world_policy_snapshot_bundle() const {
  RefreshCollisionWorld();
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->collision_world_policy_snapshot_bundle()
                   : core::CollisionWorldPolicySnapshotBundle{};
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::current_scene_snapshot_lease_inventory() const {
  RefreshCollisionWorld();
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  return publisher ? publisher->current_scene_snapshot_lease_inventory()
                   : std::nullopt;
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedPlannerSceneProjection::retain_scene_snapshot_lease_inventory(
    core::ProjectionLeaseId projection_lease_id) {
  std::lock_guard lock(mutex_);
  if (!health_.active_projection_revision.has_value() ||
      projection_lease_id.empty()) {
    return std::nullopt;
  }
  auto retained = RetainProjectionLocked(*health_.active_projection_revision,
                                        std::move(projection_lease_id));
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
  result.retained_capacity = kMaxRetainedProjectionLeases;
  result.retained_ids_oldest_first.reserve(retained_lease_order_.size());
  for (const auto &lease_id : retained_lease_order_) {
    result.retained_ids_oldest_first.push_back(lease_id);
  }
  return result;
}

core::RuntimeCollisionQueryBundlePtr
RetainedPlannerSceneProjection::published_runtime_collision_query_bundle()
    const {
  // Runtime-monitor path: keep the previous world until the off-real-time
  // build of a changed scene is published.
  auto publisher = active_publisher_.load(std::memory_order_acquire);
  auto bundle = publisher ? publisher->published_runtime_collision_query_bundle()
                          : nullptr;
  RequestCollisionWorldBuild();
  return bundle;
}

} // namespace manyforge::roboplan_backend
