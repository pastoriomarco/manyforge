#include <algorithm>
#include <limits>
#include <manyforge_core/execution_collision_support.hpp>
#include <map>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <set>
#include <string>
#include <utility>
#include <variant>
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

bool initial_mutation_source(
    const core::CollisionWorldMutationSource source) noexcept {
  return source == core::CollisionWorldMutationSource::kSceneInitialize ||
         source == core::CollisionWorldMutationSource::kDeploymentApply;
}

core::ErrorDTO freshness_refusal(std::string message, std::string reason) {
  return core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}},
  };
}

bool same_evidence_origin(const core::SceneObjectProviderEvidenceDTO &lhs,
                          const core::SceneObjectProviderEvidenceDTO &rhs) {
  return lhs.provider_instance_id == rhs.provider_instance_id &&
         lhs.provider_manifest_id == rhs.provider_manifest_id &&
         lhs.provider_manifest_revision == rhs.provider_manifest_revision &&
         lhs.provider_source_epoch == rhs.provider_source_epoch &&
         lhs.source_freshness_budget_ns == rhs.source_freshness_budget_ns;
}

const core::SceneObjectSnapshotDTO *
find_scene_object(const core::LogicalSceneSnapshotDTO &scene,
                  const std::string &object_id) {
  const auto found = std::find_if(
      scene.objects.begin(), scene.objects.end(), [&](const auto &object) {
        return object.scene_resource_id.value() == object_id;
      });
  return found == scene.objects.end() ? nullptr : &*found;
}

/// Replaces one object's freshness in a sidecar and live-object list using
/// the same conventions as project_body_freshness/project_live_objects:
/// body source_id is the provider instance, live source_id is
/// "<instance>@<source epoch>", and the age origin is received_at_ns.
/// The sidecar is left sorted and unique by body id. Returns false when the
/// object has no projected collision bodies.
bool apply_object_freshness(
    std::vector<core::CollisionWorldBodyFreshness> &body_freshness,
    std::vector<core::SceneSnapshotLiveObject> &live_objects,
    const std::string &object_id,
    const core::SceneObjectProviderEvidenceDTO &evidence) {
  const auto live = std::find_if(
      live_objects.begin(), live_objects.end(),
      [&](const auto &value) { return value.object_id == object_id; });
  if (live == live_objects.end() || live->collision_body_ids.empty()) {
    return false;
  }
  for (const auto &body_id : live->collision_body_ids) {
    const auto entry = std::find_if(
        body_freshness.begin(), body_freshness.end(),
        [&](const auto &value) { return value.body_id == body_id; });
    core::CollisionWorldBodyFreshness replacement{
        body_id, evidence.provider_instance_id.value(), evidence.received_at_ns,
        evidence.source_freshness_budget_ns};
    if (entry == body_freshness.end()) {
      body_freshness.push_back(std::move(replacement));
    } else {
      *entry = std::move(replacement);
    }
  }
  std::sort(body_freshness.begin(), body_freshness.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.body_id < rhs.body_id;
            });
  body_freshness.erase(
      std::unique(body_freshness.begin(), body_freshness.end(),
                  [](const auto &lhs, const auto &rhs) {
                    return lhs.body_id == rhs.body_id;
                  }),
      body_freshness.end());
  live->source_id = evidence.provider_instance_id.value() + "@" +
                    evidence.provider_source_epoch;
  live->source_timestamp_ns = evidence.received_at_ns;
  live->source_freshness_budget_ns = evidence.source_freshness_budget_ns;
  return true;
}

} // namespace

std::optional<core::ErrorDTO> ValidateProviderFreshnessAgainstCommittedScene(
    const core::LogicalSceneSnapshotDTO &committed_scene,
    const core::ProviderFreshnessConfirmationDTO &confirmation) {
  if (!confirmation.valid()) {
    return freshness_refusal("provider freshness confirmation is incomplete",
                             "provider_freshness_invalid");
  }
  if (committed_scene.revision !=
      confirmation.expected_projection_revision.scene_revision) {
    return freshness_refusal(
        "provider freshness confirmation names another scene revision",
        "provider_freshness_revision_mismatch");
  }
  const auto *object = find_scene_object(
      committed_scene, confirmation.scene_resource_id.value());
  if (object == nullptr) {
    return freshness_refusal(
        "provider freshness confirmation names an absent scene object",
        "provider_freshness_object_absent");
  }
  const auto *fixed =
      std::get_if<core::CellFixedSceneObjectPlacementDTO>(&object->placement);
  const bool attached = std::any_of(
      committed_scene.carried_object_attachments.begin(),
      committed_scene.carried_object_attachments.end(),
      [&](const auto &attachment) {
        return attachment.scene_resource_id == confirmation.scene_resource_id;
      });
  if (fixed == nullptr || attached ||
      fixed->cell_id != committed_scene.cell_id) {
    return freshness_refusal(
        "provider freshness confirmation requires an unattached cell-fixed "
        "object",
        "provider_freshness_object_not_cell_fixed");
  }
  if (!(fixed->pose_in_cell == confirmation.pose_in_cell)) {
    return freshness_refusal(
        "provider freshness confirmation pose differs from the committed pose",
        "provider_freshness_pose_mismatch");
  }
  if (!object->provider_evidence.has_value() ||
      !same_evidence_origin(*object->provider_evidence,
                            confirmation.evidence)) {
    return freshness_refusal(
        "provider freshness confirmation differs from the committed evidence "
        "origin or freshness budget",
        "provider_freshness_origin_mismatch");
  }
  if (confirmation.evidence.provider_source_sequence <=
          object->provider_evidence->provider_source_sequence ||
      confirmation.evidence.received_at_ns <
          object->provider_evidence->received_at_ns) {
    return freshness_refusal(
        "provider freshness confirmation is not newer than the committed "
        "evidence",
        "provider_freshness_not_newer");
  }
  return std::nullopt;
}

RetainedCollisionWorldPublisher::RetainedCollisionWorldPublisher(
    RetainedCollisionWorldProjector projector, std::string publisher_id,
    Clock clock, const std::size_t ledger_capacity)
    : projector_(std::move(projector)), publisher_id_(std::move(publisher_id)),
      clock_(std::move(clock)), ledger_capacity_(ledger_capacity) {}

core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>
RetainedCollisionWorldPublisher::create(
    RetainedCollisionWorldProjector projector, std::string publisher_id,
    const core::LogicalSceneSnapshotDTO &initial_scene,
    RetainedCollisionWorldPublicationContext initial_context, Clock clock,
    const std::size_t ledger_capacity) {
  if (publisher_id.empty() || !clock) {
    return publication_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "collision-world publisher id and lease clock must be available");
  }
  if (ledger_capacity == 0U || ledger_capacity > 4096U) {
    return publication_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "collision-world ledger capacity is outside the supported bound");
  }
  if (!initial_mutation_source(initial_context.mutation_source)) {
    return publication_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        "first collision-world publication must initialize a scene or "
        "deployment");
  }

  auto publisher = std::shared_ptr<RetainedCollisionWorldPublisher>(
      new RetainedCollisionWorldPublisher(std::move(projector),
                                          std::move(publisher_id),
                                          std::move(clock), ledger_capacity));
  auto initialized =
      publisher->publish(initial_scene, std::move(initial_context));
  if (!initialized.ok()) {
    return publication_error<std::shared_ptr<RetainedCollisionWorldPublisher>>(
        initialized.error().message);
  }
  return core::Result<std::shared_ptr<RetainedCollisionWorldPublisher>>::Ok(
      std::move(publisher));
}

core::Result<core::CollisionWorldRevision>
RetainedCollisionWorldPublisher::publish(
    const core::LogicalSceneSnapshotDTO &logical_scene,
    RetainedCollisionWorldPublicationContext context) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return publish_locked(logical_scene, std::move(context));
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
RetainedCollisionWorldPublisher::publish_locked(
    const core::LogicalSceneSnapshotDTO &logical_scene,
    RetainedCollisionWorldPublicationContext context) {
  if (!context.attribution.locally_valid() || context.source_label.empty() ||
      context.observed_timestamp_ns < 0 ||
      (context.correlation_id.has_value() && context.correlation_id->empty())) {
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
  if (collision_world_revision_sequence_ ==
          std::numeric_limits<std::uint64_t>::max() ||
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
  } catch (const std::exception &error) {
    return publication_error<core::CollisionWorldRevision>(
        "logical-scene content cannot be verified: " +
        std::string(error.what()));
  }
  if (logical_content != logical_scene.revision.content_hash) {
    return publication_error<core::CollisionWorldRevision>(
        "logical-scene content identity is not exact");
  }

  if (last_scene_identity_.has_value()) {
    const auto &previous = last_scene_identity_->revision;
    const auto &next = logical_scene.revision;
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
                                  context.observed_timestamp_ns, publisher_id_);
  if (!world.ok()) {
    return publication_error<core::CollisionWorldRevision>(
        "collision-world projection failed: " + world.error().message);
  }

  auto world_ptr = std::make_shared<const core::CollisionWorldSnapshot>(
      std::move(world.value()));
  const core::ProjectionRevisionDTO projection_revision{
      projector_.model_map().retained_bundle_id,
      projector_.model_map().graph_revision, logical_scene.revision};
  auto logical_scene_ptr =
      std::make_shared<const core::LogicalSceneSnapshotDTO>(logical_scene);
  // Freshness overrides are a derived cache of committed evidence: keep one
  // only while its object's committed evidence and placement are exactly the
  // ones it was confirmed against, and re-apply it over the projection.
  auto body_freshness = projector_.project_body_freshness(*world_ptr);
  auto live_objects = projector_.project_live_objects(logical_scene, *world_ptr);
  std::map<std::string, FreshnessOverride> next_overrides;
  for (const auto &[object_id, override_value] : freshness_overrides_) {
    const auto *object = find_scene_object(logical_scene, object_id);
    const bool attached = std::any_of(
        logical_scene.carried_object_attachments.begin(),
        logical_scene.carried_object_attachments.end(),
        [&](const auto &attachment) {
          return attachment.scene_resource_id.value() == object_id;
        });
    if (object == nullptr || attached ||
        !object->provider_evidence.has_value() ||
        !(*object->provider_evidence == override_value.committed_evidence) ||
        !(object->placement == override_value.committed_placement)) {
      continue;
    }
    if (apply_object_freshness(body_freshness, live_objects, object_id,
                               override_value.confirmed_evidence)) {
      next_overrides.emplace(object_id, override_value);
    }
  }
  core::CollisionWorldPolicySnapshotBundle coherent{
      world_ptr,
      std::move(policy.value()),
      std::move(body_freshness),
  };
  auto runtime = core::build_runtime_collision_query_bundle(
      std::move(coherent), runtime_epoch_ + 1U, projection_revision);
  if (!runtime || !runtime->world || !runtime->policy ||
      !runtime->policy_index || !runtime->lookup_order_valid ||
      !runtime->distance_selection_order_valid) {
    return publication_error<core::CollisionWorldRevision>(
        "collision runtime bundle preparation failed closed");
  }

  std::set<core::CollisionBodyId> accepted_body_ids;
  if (auto current = runtime_bundle_.load(std::memory_order_acquire);
      current && current->world) {
    for (const auto &body : current->world->bodies) {
      accepted_body_ids.insert(body.body_id);
    }
  }
  for (const auto &body : runtime->world->bodies) {
    accepted_body_ids.insert(body.body_id);
  }
  for (const auto &affected : context.affected_body_ids) {
    if (affected.empty() || !accepted_body_ids.contains(affected)) {
      return publication_error<core::CollisionWorldRevision>(
          "collision-world ledger names an unknown affected body");
    }
  }
  std::sort(context.affected_body_ids.begin(), context.affected_body_ids.end());
  context.affected_body_ids.erase(std::unique(context.affected_body_ids.begin(),
                                              context.affected_body_ids.end()),
                                  context.affected_body_ids.end());

  const std::uint64_t previous_revision_id = world_revision_id_;
  const std::uint64_t next_owner_sequence = owner_sequence_ + 1U;
  auto published_revision = runtime->world->revision;
  core::CollisionWorldLeaseInventory next_inventory;
  next_inventory.projection_binding = core::ProjectionLeaseBindingDTO{
      projection_revision, core::ProjectionLeaseId{publisher_id_ + ".current"}};
  next_inventory.logical_scene = std::move(logical_scene_ptr);
  next_inventory.captured_at_ns = context.observed_timestamp_ns;
  next_inventory.bundle = core::CollisionWorldPolicySnapshotBundle{
      runtime->world, *runtime->policy, runtime->body_freshness};
  next_inventory.topology_epoch = logical_scene.topology_epoch;
  next_inventory.policy_epoch = logical_scene.policy_epoch;
  next_inventory.live_objects = std::move(live_objects);
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

  collision_world_revision_sequence_ = proposed_world_revision;
  world_revision_id_ = proposed_world_revision;
  if (policy_changed) {
    policy_revision_id_ = proposed_policy_revision;
  }
  policy_content_hash_ = std::move(next_policy_content_hash);
  ++runtime_epoch_;
  owner_sequence_ = next_owner_sequence;
  last_scene_identity_ = std::move(next_scene_identity);
  current_inventory_ = std::move(next_inventory);
  freshness_overrides_.swap(next_overrides);
  ledger_.swap(next_ledger);
  return core::Result<core::CollisionWorldRevision>::Ok(
      std::move(published_revision));
}

core::Result<std::uint64_t> RetainedCollisionWorldPublisher::publish_freshness(
    const core::ProviderFreshnessConfirmationDTO &confirmation) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const auto refuse = [](core::ErrorDTO value) {
    return core::Result<std::uint64_t>::Err(std::move(value));
  };
  auto current = runtime_bundle_.load(std::memory_order_acquire);
  if (!current || !current->world || !current->policy ||
      !current->policy_index || !current_inventory_.has_value() ||
      !current_inventory_->logical_scene) {
    return refuse(freshness_refusal(
        "collision-world publisher has no current publication",
        "provider_freshness_publication_unavailable"));
  }
  if (current_inventory_->projection_binding.projection_revision !=
          confirmation.expected_projection_revision ||
      current->projection_revision != confirmation.expected_projection_revision) {
    return refuse(freshness_refusal(
        "provider freshness confirmation names another projection revision",
        "provider_freshness_revision_mismatch"));
  }
  const auto &committed_scene = *current_inventory_->logical_scene;
  if (auto refusal = ValidateProviderFreshnessAgainstCommittedScene(
          committed_scene, confirmation);
      refusal.has_value()) {
    return refuse(std::move(*refusal));
  }
  const auto &object_id = confirmation.scene_resource_id.value();
  const auto *object = find_scene_object(committed_scene, object_id);
  if (const auto existing = freshness_overrides_.find(object_id);
      existing != freshness_overrides_.end() &&
      existing->second.committed_evidence == *object->provider_evidence &&
      (confirmation.evidence.provider_source_sequence <=
           existing->second.confirmed_evidence.provider_source_sequence ||
       confirmation.evidence.received_at_ns <
           existing->second.confirmed_evidence.received_at_ns)) {
    return refuse(freshness_refusal(
        "provider freshness confirmation is not newer than the last accepted "
        "confirmation",
        "provider_freshness_not_newer"));
  }
  if (runtime_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    return refuse(freshness_refusal(
        "collision runtime epoch is exhausted",
        "provider_freshness_epoch_exhausted"));
  }

  auto next_inventory = *current_inventory_;
  auto next_freshness = current->body_freshness;
  if (!apply_object_freshness(next_freshness, next_inventory.live_objects,
                              object_id, confirmation.evidence)) {
    return refuse(freshness_refusal(
        "provider freshness confirmation names an object without projected "
        "collision bodies",
        "provider_freshness_object_unprojected"));
  }
  auto next_bundle = std::make_shared<core::RuntimeCollisionQueryBundle>();
  next_bundle->projection_revision = current->projection_revision;
  next_bundle->world = current->world;
  next_bundle->policy = current->policy;
  next_bundle->policy_index = current->policy_index;
  next_bundle->selected_distance_pairs = current->selected_distance_pairs;
  next_bundle->contact_grasp_selected_distance_pairs =
      current->contact_grasp_selected_distance_pairs;
  next_bundle->distance_selection_order_valid =
      current->distance_selection_order_valid;
  next_bundle->body_freshness = next_freshness;
  next_bundle->lookup_order_valid = core::runtime_collision_lookup_order_is_valid(
      *next_bundle->world, next_bundle->body_freshness);
  next_bundle->epoch = runtime_epoch_ + 1U;
  if (!next_bundle->lookup_order_valid ||
      !next_bundle->distance_selection_order_valid) {
    return refuse(freshness_refusal(
        "freshness-only runtime bundle preparation failed closed",
        "provider_freshness_bundle_invalid"));
  }
  next_inventory.bundle.body_freshness = std::move(next_freshness);
  auto next_overrides = freshness_overrides_;
  next_overrides.insert_or_assign(
      object_id, FreshnessOverride{*object->provider_evidence,
                                   object->placement, confirmation.evidence});
  const auto next_epoch = next_bundle->epoch;

  if (!retire_and_swap_locked(std::move(next_bundle))) {
    return refuse(freshness_refusal(
        "collision runtime bundle retirement is backpressured",
        "provider_freshness_backpressured"));
  }
  runtime_epoch_ = next_epoch;
  current_inventory_ = std::move(next_inventory);
  freshness_overrides_.swap(next_overrides);
  return core::Result<std::uint64_t>::Ok(next_epoch);
}

bool RetainedCollisionWorldPublisher::retire_and_swap_locked(
    core::RuntimeCollisionQueryBundlePtr next) {
  for (auto &retired : retired_runtime_bundles_) {
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

  auto previous =
      runtime_bundle_.exchange(std::move(next), std::memory_order_acq_rel);
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
  if (const auto existing = retained_inventories_.find(projection_lease_id);
      !projection_lease_id.empty() && existing != retained_inventories_.end()) {
    return existing->second;
  }
  std::int64_t captured_at_ns = 0;
  try {
    captured_at_ns = clock_();
  } catch (...) {
    return std::nullopt;
  }
  return retain_scene_snapshot_lease_inventory_locked(
      std::move(projection_lease_id), captured_at_ns);
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedCollisionWorldPublisher::retain_scene_snapshot_lease_inventory_at(
    core::ProjectionLeaseId projection_lease_id,
    const std::int64_t captured_at_ns) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return retain_scene_snapshot_lease_inventory_locked(
      std::move(projection_lease_id), captured_at_ns);
}

std::optional<core::CollisionWorldLeaseInventory>
RetainedCollisionWorldPublisher::retain_scene_snapshot_lease_inventory_locked(
    core::ProjectionLeaseId projection_lease_id,
    const std::int64_t captured_at_ns) {
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
  if (captured_at_ns < 0 ||
      retained_inventories_.size() >= kRuntimeRetirementCapacity) {
    return std::nullopt;
  }
  auto retained = *current_inventory_;
  retained.projection_binding.projection_lease_id = projection_lease_id;
  retained.captured_at_ns = captured_at_ns;
  retained_inventory_order_.push_back(projection_lease_id);
  retained_inventories_.emplace(projection_lease_id, retained);
  return retained;
}

void RetainedCollisionWorldPublisher::release_scene_snapshot_lease_inventory(
    const core::ProjectionLeaseId &projection_lease_id) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  retained_inventories_.erase(projection_lease_id);
  retained_inventory_order_.erase(std::remove(retained_inventory_order_.begin(),
                                              retained_inventory_order_.end(),
                                              projection_lease_id),
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

} // namespace manyforge::roboplan_backend
