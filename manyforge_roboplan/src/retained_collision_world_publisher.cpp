#include <manyforge_roboplan/retained_collision_world_publisher.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/logical_scene_identity.hpp>

namespace manyforge::roboplan_backend {
namespace {

template <typename T>
core::Result<T> publication_error(std::string message) {
  return core::Result<T>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {},
  });
}

bool initial_mutation_source(
    const core::CollisionWorldMutationSource source) noexcept {
  return source == core::CollisionWorldMutationSource::kSceneInitialize ||
         source == core::CollisionWorldMutationSource::kDeploymentApply;
}

std::vector<core::CollisionWorldBodyFreshness> body_freshness(
    const core::CollisionWorldSnapshot& world) {
  std::vector<core::CollisionWorldBodyFreshness> result;
  for (const auto& body : world.bodies) {
    if (!body.source_id.has_value() &&
        !body.source_timestamp_ns.has_value() &&
        !body.source_freshness_budget_ns.has_value()) {
      continue;
    }
    result.push_back({
        body.body_id,
        body.source_id,
        body.source_timestamp_ns,
        body.source_freshness_budget_ns,
    });
  }
  return result;
}

std::vector<core::SceneSnapshotLiveObject> live_objects(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    const core::CollisionWorldSnapshot& world) {
  std::vector<core::SceneSnapshotLiveObject> result;
  result.reserve(logical_scene.objects.size());
  for (const auto& object : logical_scene.objects) {
    core::SceneSnapshotLiveObject live;
    live.object_id = object.scene_resource_id.value();
    const std::string body_prefix =
        "scene/" + object.scene_resource_id.value() + "/shape/";
    for (const auto& body : world.bodies) {
      if (body.body_id.starts_with(body_prefix)) {
        live.collision_body_ids.push_back(body.body_id);
        live.body_content_hashes.emplace(body.body_id,
                                         body.body_content_hash);
      }
    }
    live.object_revision_token =
        logical_scene.revision.content_hash.value() + "/" + live.object_id;
    if (const auto* fixed =
            std::get_if<core::CellFixedSceneObjectPlacementDTO>(
                &object.placement)) {
      live.pose = fixed->pose_in_cell;
      live.declared_role = core::ObservedSceneObjectRole::kStaticObstacle;
    } else {
      live.declared_role = core::ObservedSceneObjectRole::kAttachedObject;
    }
    if (object.provider_evidence.has_value()) {
      const auto& evidence = *object.provider_evidence;
      live.source_id = evidence.provider_instance_id.value() + "@" +
                       evidence.provider_source_epoch;
      live.source_timestamp_ns = evidence.source_timestamp_ns;
      live.source_freshness_budget_ns =
          evidence.source_freshness_budget_ns;
      // Leave the declared role contextual: the operation may identify this
      // exact object as its observed target; otherwise the Kernel treats it as
      // a snapshot obstacle and still enforces its source freshness.
      live.declared_role.reset();
    }
    result.push_back(std::move(live));
  }
  return result;
}

}  // namespace

RetainedCollisionWorldPublisher::RetainedCollisionWorldPublisher(
    RetainedCollisionWorldProjector projector,
    std::string publisher_id,
    const std::size_t ledger_capacity)
    : projector_(std::move(projector)),
      publisher_id_(std::move(publisher_id)),
      ledger_capacity_(ledger_capacity) {}

core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
RetainedCollisionWorldPublisher::create(
    RetainedCollisionWorldProjector projector,
    std::string publisher_id,
    const core::LogicalSceneSnapshotDTO& initial_scene,
    RetainedCollisionWorldPublicationContext initial_context,
    const std::size_t ledger_capacity) {
  if (publisher_id.empty()) {
    return publication_error<
        std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "collision-world publisher id must be non-empty");
  }
  if (ledger_capacity == 0U || ledger_capacity > 4096U) {
    return publication_error<
        std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "collision-world ledger capacity is outside the supported bound");
  }
  if (!initial_mutation_source(initial_context.mutation_source)) {
    return publication_error<
        std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "first collision-world publication must initialize a scene or "
        "deployment");
  }

  auto publisher = std::shared_ptr<RetainedCollisionWorldPublisher>(
      new RetainedCollisionWorldPublisher(
          std::move(projector), std::move(publisher_id), ledger_capacity));
  auto initialized =
      publisher->publish(initial_scene, std::move(initial_context));
  if (!initialized.ok()) {
    return publication_error<
        std::shared_ptr<RetainedCollisionWorldPublisher>>(
        initialized.error().message);
  }
  return core::Result<
      std::shared_ptr<RetainedCollisionWorldPublisher>>::Ok(
      std::move(publisher));
}

core::Result<core::CollisionWorldRevision>
RetainedCollisionWorldPublisher::publish(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    RetainedCollisionWorldPublicationContext context) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return publish_locked(logical_scene, std::move(context));
}

core::Result<core::CollisionWorldRevision>
RetainedCollisionWorldPublisher::publish_locked(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    RetainedCollisionWorldPublicationContext context) {
  if (!context.attribution.locally_valid() || context.source_label.empty() ||
      context.observed_timestamp_ns < 0 ||
      (context.correlation_id.has_value() &&
       context.correlation_id->empty())) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world publication provenance is incomplete or invalid");
  }
  if (world_revision_id_ == 0U &&
      !initial_mutation_source(context.mutation_source)) {
    return publication_error<core::CollisionWorldRevision>(
        "first collision-world publication has an invalid mutation source");
  }
  if (world_revision_id_ != 0U &&
      context.mutation_source ==
          core::CollisionWorldMutationSource::kSceneInitialize) {
    return publication_error<core::CollisionWorldRevision>(
        "scene initialization cannot follow an existing publication");
  }
  if (world_revision_id_ == std::numeric_limits<std::uint64_t>::max() ||
      owner_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
      runtime_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world publication counter is exhausted");
  }

  core::ContentDigest logical_content{std::string{}};
  try {
    logical_content =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(
            logical_scene);
  } catch (const std::exception& error) {
    return publication_error<core::CollisionWorldRevision>(
        "logical-scene content cannot be verified: " +
        std::string(error.what()));
  }
  if (logical_content != logical_scene.revision.content_hash) {
    return publication_error<core::CollisionWorldRevision>(
        "logical-scene content identity is not exact");
  }

  if (last_scene_identity_.has_value()) {
    const auto& previous = last_scene_identity_->revision;
    const auto& next = logical_scene.revision;
    if (previous.scene_id == next.scene_id) {
      if (next.revision_id <= previous.revision_id) {
        return publication_error<core::CollisionWorldRevision>(
            "logical-scene revision did not advance monotonically");
      }
    } else if (context.mutation_source !=
               core::CollisionWorldMutationSource::kDeploymentApply) {
      return publication_error<core::CollisionWorldRevision>(
          "logical-scene lineage changed outside deployment Apply");
    }
  }

  if (policy_revision_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-policy publication counter is exhausted");
  }
  const std::uint64_t proposed_policy_revision = policy_revision_id_ + 1U;
  auto policy = projector_.project_effective_policy(
      logical_scene, proposed_policy_revision);
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

  const std::uint64_t proposed_world_revision = world_revision_id_ + 1U;
  auto world = projector_.project(
      logical_scene, proposed_world_revision,
      context.observed_timestamp_ns, publisher_id_);
  if (!world.ok()) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world projection failed: " + world.error().message);
  }

  auto world_ptr = std::make_shared<const core::CollisionWorldSnapshot>(
      std::move(world.value()));
  core::CollisionWorldPolicySnapshotBundle coherent{
      world_ptr,
      std::move(policy.value()),
      body_freshness(*world_ptr),
  };
  auto runtime = core::build_runtime_collision_query_bundle(
      std::move(coherent), runtime_epoch_ + 1U);
  if (!runtime || !runtime->world || !runtime->policy ||
      !runtime->policy_index || !runtime->lookup_order_valid ||
      !runtime->distance_selection_order_valid) {
    return publication_error<core::CollisionWorldRevision>(
        "collision runtime bundle preparation failed closed");
  }

  std::set<core::CollisionBodyId> accepted_body_ids;
  if (auto current = runtime_bundle_.load(std::memory_order_acquire);
      current && current->world) {
    for (const auto& body : current->world->bodies) {
      accepted_body_ids.insert(body.body_id);
    }
  }
  for (const auto& body : runtime->world->bodies) {
    accepted_body_ids.insert(body.body_id);
  }
  for (const auto& affected : context.affected_body_ids) {
    if (affected.empty() || !accepted_body_ids.contains(affected)) {
      return publication_error<core::CollisionWorldRevision>(
          "collision-world ledger names an unknown affected body");
    }
  }
  std::sort(context.affected_body_ids.begin(),
            context.affected_body_ids.end());
  context.affected_body_ids.erase(
      std::unique(context.affected_body_ids.begin(),
                  context.affected_body_ids.end()),
      context.affected_body_ids.end());

  const std::uint64_t previous_revision_id = world_revision_id_;
  const std::uint64_t next_owner_sequence = owner_sequence_ + 1U;
  auto published_revision = runtime->world->revision;
  core::CollisionWorldLeaseInventory next_inventory;
  next_inventory.projection_binding = core::ProjectionLeaseBindingDTO{
      core::ProjectionRevisionDTO{
          projector_.model_map().retained_bundle_id,
          projector_.model_map().graph_revision,
          logical_scene.revision},
      core::ProjectionLeaseId{publisher_id_ + ".current"}};
  next_inventory.captured_at_ns = context.observed_timestamp_ns;
  next_inventory.bundle = core::CollisionWorldPolicySnapshotBundle{
      runtime->world, *runtime->policy, runtime->body_freshness};
  next_inventory.topology_epoch = logical_scene.topology_epoch;
  next_inventory.policy_epoch = logical_scene.policy_epoch;
  next_inventory.live_objects = live_objects(logical_scene, *runtime->world);
  auto next_scene_identity = logical_scene.identity();
  std::string next_policy_content_hash =
      policy_changed ? runtime->policy->revision.content_hash
                     : policy_content_hash_;
  core::CollisionWorldLedgerEntry entry;
  entry.revision = published_revision;
  entry.previous_revision_id = previous_revision_id;
  entry.mutation_source = context.mutation_source;
  entry.affected_body_ids = std::move(context.affected_body_ids);
  entry.owner_sequence = next_owner_sequence;
  entry.observed_timestamp_ns = context.observed_timestamp_ns;
  entry.attribution = std::move(context.attribution);
  entry.source_label = std::move(context.source_label);
  entry.correlation_id = std::move(context.correlation_id);
  auto next_ledger = ledger_;
  next_ledger.push_back(std::move(entry));
  while (next_ledger.size() > ledger_capacity_) {
    next_ledger.pop_front();
  }

  if (!retire_and_swap_locked(std::move(runtime))) {
    return publication_error<core::CollisionWorldRevision>(
        "collision runtime bundle retirement is backpressured");
  }

  world_revision_id_ = proposed_world_revision;
  if (policy_changed) {
    policy_revision_id_ = proposed_policy_revision;
  }
  policy_content_hash_ = std::move(next_policy_content_hash);
  ++runtime_epoch_;
  owner_sequence_ = next_owner_sequence;
  last_scene_identity_ = std::move(next_scene_identity);
  current_inventory_ = std::move(next_inventory);
  ledger_.swap(next_ledger);
  return core::Result<core::CollisionWorldRevision>::Ok(
      std::move(published_revision));
}

bool RetainedCollisionWorldPublisher::retire_and_swap_locked(
    core::RuntimeCollisionQueryBundlePtr next) {
  for (auto& retired : retired_runtime_bundles_) {
    if (retired && retired.use_count() == 1) {
      retired.reset();
    }
  }

  auto current = runtime_bundle_.load(std::memory_order_acquire);
  auto available = retired_runtime_bundles_.end();
  if (current) {
    available = std::find(retired_runtime_bundles_.begin(),
                          retired_runtime_bundles_.end(), nullptr);
    if (available == retired_runtime_bundles_.end()) {
      return false;
    }
  }

  auto previous = runtime_bundle_.exchange(
      std::move(next), std::memory_order_acq_rel);
  current.reset();
  if (previous && previous.use_count() > 1) {
    *available = std::move(previous);
  }
  return true;
}

std::vector<core::CollisionWorldLedgerEntry>
RetainedCollisionWorldPublisher::recent_ledger(const std::size_t limit) const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const std::size_t count =
      limit == 0U ? ledger_.size() : std::min(limit, ledger_.size());
  std::vector<core::CollisionWorldLedgerEntry> result;
  result.reserve(count);
  for (auto entry = ledger_.rbegin();
       entry != ledger_.rend() && result.size() < count; ++entry) {
    result.push_back(*entry);
  }
  return result;
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
      bundle->body_freshness,
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
  if (projection_lease_id.empty()) {
    if (next_lease_id_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    projection_lease_id = core::ProjectionLeaseId{
        publisher_id_ + ".lease." + std::to_string(next_lease_id_++)};
  }
  if (const auto existing = retained_inventories_.find(projection_lease_id);
      existing != retained_inventories_.end()) {
    return existing->second;
  }
  if (retained_inventories_.size() >= kRuntimeRetirementCapacity) {
    return std::nullopt;
  }
  auto retained = *current_inventory_;
  retained.projection_binding.projection_lease_id = projection_lease_id;
  retained_inventory_order_.push_back(projection_lease_id);
  retained_inventories_.emplace(projection_lease_id, retained);
  return retained;
}

void RetainedCollisionWorldPublisher::release_scene_snapshot_lease_inventory(
    const core::ProjectionLeaseId& projection_lease_id) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  retained_inventories_.erase(projection_lease_id);
  retained_inventory_order_.erase(
      std::remove(retained_inventory_order_.begin(),
                  retained_inventory_order_.end(), projection_lease_id),
      retained_inventory_order_.end());
}

core::CollisionWorldLeaseRetentionStats
RetainedCollisionWorldPublisher::scene_snapshot_lease_retention_stats() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  core::CollisionWorldLeaseRetentionStats stats;
  stats.retained_count = retained_inventories_.size();
  stats.retained_capacity = kRuntimeRetirementCapacity;
  stats.retained_ids_oldest_first.assign(retained_inventory_order_.begin(),
                                         retained_inventory_order_.end());
  return stats;
}

core::RuntimeCollisionQueryBundlePtr
RetainedCollisionWorldPublisher::published_runtime_collision_query_bundle()
    const {
  return runtime_bundle_.load(std::memory_order_acquire);
}

}  // namespace manyforge::roboplan_backend
