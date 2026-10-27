#include <algorithm>
#include <limits>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

template <typename T> core::Result<T> publication_error(std::string message) {
  return core::Result<T>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {},
  });
}

} // namespace

RetainedCollisionWorldPublisher::RetainedCollisionWorldPublisher(
    RetainedCollisionWorldProjector projector, std::string publisher_id,
    Clock clock)
    : projector_(std::move(projector)), publisher_id_(std::move(publisher_id)),
      clock_(std::move(clock)) {}

core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
RetainedCollisionWorldPublisher::create(
    RetainedCollisionWorldProjector projector, std::string publisher_id,
    const core::LogicalSceneSnapshotDTO &initial_scene,
    const std::int64_t observed_timestamp_ns, Clock clock) {
  if (publisher_id.empty() || !clock) {
    return publication_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "collision-world publisher id and clock must be available");
  }

  auto publisher = std::shared_ptr<RetainedCollisionWorldPublisher>(
      new RetainedCollisionWorldPublisher(
          std::move(projector), std::move(publisher_id), std::move(clock)));
  auto initialized = publisher->publish(initial_scene, observed_timestamp_ns);
  if (!initialized.ok()) {
    return publication_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        initialized.error().message);
  }
  return core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>::Ok(
      std::move(publisher));
}

core::Result<std::uint64_t>
RetainedCollisionWorldPublisher::reserve_private_collision_world_revision() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (collision_world_revision_sequence_ ==
      std::numeric_limits<std::uint64_t>::max()) {
    return publication_error<std::uint64_t>(
        "collision-world revision sequence is exhausted");
  }
  return core::Result<std::uint64_t>::Ok(
      ++collision_world_revision_sequence_);
}

core::Result<core::CollisionWorldRevision>
RetainedCollisionWorldPublisher::publish(
    const core::LogicalSceneSnapshotDTO &logical_scene,
    const std::int64_t observed_timestamp_ns) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (observed_timestamp_ns < 0) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world publication timestamp is negative");
  }
  if (collision_world_revision_sequence_ ==
          std::numeric_limits<std::uint64_t>::max() ||
      policy_revision_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world publication counter is exhausted");
  }

  const std::uint64_t proposed_policy_revision = policy_revision_id_ + 1U;
  auto policy = projector_.project_effective_policy(logical_scene,
                                                    proposed_policy_revision);
  if (!policy.ok()) {
    return publication_error<core::CollisionWorldRevision>(
        "effective collision-policy projection failed: " +
        policy.error().message);
  }
  const bool policy_changed =
      policy_revision_id_ == 0U ||
      policy.value().revision.content_hash != policy_content_hash_;
  if (!policy_changed) {
    policy.value().revision.revision_id = policy_revision_id_;
  }

  const std::uint64_t proposed_world_revision =
      collision_world_revision_sequence_ + 1U;
  auto world = projector_.project(logical_scene, proposed_world_revision,
                                  observed_timestamp_ns, publisher_id_);
  if (!world.ok()) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world projection failed: " + world.error().message);
  }

  auto world_ptr = std::make_shared<const core::CollisionWorldSnapshot>(
      std::move(world.value()));
  const core::ProjectionRevisionDTO projection_revision{
      projector_.model_map().retained_bundle_id,
      projector_.model_map().graph_revision, logical_scene.revision};
  auto runtime = core::build_runtime_collision_query_bundle(
      core::CollisionWorldPolicySnapshotBundle{world_ptr,
                                               std::move(policy.value())},
      projection_revision);
  if (!runtime || !runtime->world || !runtime->policy ||
      !runtime->policy_index || !runtime->lookup_order_valid ||
      !runtime->distance_selection_order_valid) {
    return publication_error<core::CollisionWorldRevision>(
        "collision runtime bundle preparation failed closed");
  }

  core::CollisionWorldLeaseInventory next_inventory;
  next_inventory.projection_binding = core::ProjectionLeaseBindingDTO{
      projection_revision, core::ProjectionLeaseId{publisher_id_ + ".current"}};
  next_inventory.logical_scene =
      std::make_shared<const core::LogicalSceneSnapshotDTO>(logical_scene);
  next_inventory.captured_at_ns = observed_timestamp_ns;
  next_inventory.bundle =
      core::CollisionWorldPolicySnapshotBundle{runtime->world, *runtime->policy};
  auto published_revision = runtime->world->revision;
  if (policy_changed) {
    policy_revision_id_ = proposed_policy_revision;
    policy_content_hash_ = runtime->policy->revision.content_hash;
  }

  swap_locked(std::move(runtime));
  collision_world_revision_sequence_ = proposed_world_revision;
  current_inventory_ = std::move(next_inventory);
  return core::Result<core::CollisionWorldRevision>::Ok(
      std::move(published_revision));
}

void RetainedCollisionWorldPublisher::swap_locked(
    core::RuntimeCollisionQueryBundlePtr next) {
  std::erase_if(retired_runtime_bundles_,
                [](const auto &retired) { return retired.use_count() == 1; });
  auto previous =
      runtime_bundle_.exchange(std::move(next), std::memory_order_acq_rel);
  if (previous && previous.use_count() > 1) {
    retired_runtime_bundles_.push_back(std::move(previous));
  }
}

core::CollisionWorldSnapshotPtr
RetainedCollisionWorldPublisher::snapshot() const {
  auto bundle = runtime_bundle_.load(std::memory_order_acquire);
  return bundle ? bundle->world : nullptr;
}

core::CollisionWorldRevision
RetainedCollisionWorldPublisher::published_collision_world_revision() const {
  auto bundle = runtime_bundle_.load(std::memory_order_acquire);
  return bundle && bundle->world ? bundle->world->revision
                                 : core::CollisionWorldRevision{};
}

std::optional<core::CollisionPolicySnapshot>
RetainedCollisionWorldPublisher::collision_policy_snapshot() const {
  auto bundle = runtime_bundle_.load(std::memory_order_acquire);
  return bundle && bundle->policy
             ? std::optional<core::CollisionPolicySnapshot>(*bundle->policy)
             : std::nullopt;
}

core::CollisionWorldPolicySnapshotBundle
RetainedCollisionWorldPublisher::collision_world_policy_snapshot_bundle()
    const {
  auto bundle = runtime_bundle_.load(std::memory_order_acquire);
  if (!bundle) {
    return {};
  }
  return {
      bundle->world,
      bundle->policy
          ? std::optional<core::CollisionPolicySnapshot>(*bundle->policy)
          : std::nullopt,
  };
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedCollisionWorldPublisher::current_scene_snapshot_lease_inventory()
    const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return current_inventory_;
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedCollisionWorldPublisher::retain_scene_snapshot_lease_inventory(
    core::ProjectionLeaseId projection_lease_id) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!current_inventory_.has_value()) {
    return std::nullopt;
  }
  std::int64_t captured_at_ns = 0;
  try {
    captured_at_ns = clock_();
  } catch (...) {
    return std::nullopt;
  }
  if (captured_at_ns < 0) {
    return std::nullopt;
  }
  if (projection_lease_id.empty()) {
    projection_lease_id = core::ProjectionLeaseId{
        publisher_id_ + ".lease." + std::to_string(next_lease_id_++)};
  }
  auto retained = *current_inventory_;
  retained.projection_binding.projection_lease_id =
      std::move(projection_lease_id);
  retained.captured_at_ns = captured_at_ns;
  return retained;
}

core::RuntimeCollisionQueryBundlePtr
RetainedCollisionWorldPublisher::published_runtime_collision_query_bundle()
    const {
  return runtime_bundle_.load(std::memory_order_acquire);
}

} // namespace manyforge::roboplan_backend
