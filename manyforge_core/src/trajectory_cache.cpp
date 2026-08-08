#include <algorithm>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <stdexcept>
#include <string_view>

namespace manyforge::core {
namespace {

void append_field(std::string& encoded, const std::string_view value) {
  encoded.append(std::to_string(value.size()));
  encoded.push_back(':');
  encoded.append(value);
  encoded.push_back('|');
}

[[nodiscard]] bool same_projection_lineage(const ProjectionRevisionDTO& left,
                                           const ProjectionRevisionDTO& right) noexcept {
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision &&
         left.scene_revision.scene_id == right.scene_revision.scene_id;
}

[[nodiscard]] bool same_revision_number_has_conflicting_content(
    const TrajectoryCacheRevisionEvidence& stored,
    const TrajectoryCacheRevisionEvidence& expected) noexcept {
  return (stored.projection_revision.scene_revision.revision_id ==
              expected.projection_revision.scene_revision.revision_id &&
          stored.projection_revision.scene_revision.content_hash !=
              expected.projection_revision.scene_revision.content_hash) ||
         (stored.collision_world_revision.revision_id ==
              expected.collision_world_revision.revision_id &&
          stored.collision_world_revision.content_hash !=
              expected.collision_world_revision.content_hash);
}

}  // namespace

TrajectoryCache::TrajectoryCache(CacheConfig config) : config_(config) {}

std::string TrajectoryCache::encoded_key(const TrajectoryCacheKey& key) {
  std::string encoded;
  encoded.reserve(key.program_revision.value().size() + key.program_binding_id.value().size() +
                  key.program_run_id.value().size() + key.move_id.value().size() +
                  key.controllable_component_id.value().size() +
                  key.command_target_id.value().size() + key.capability_id.value().size() + 64U);
  append_field(encoded, key.program_revision.value());
  append_field(encoded, key.program_binding_id.value());
  append_field(encoded, key.program_run_id.value());
  append_field(encoded, key.move_id.value());
  append_field(encoded, key.controllable_component_id.value());
  append_field(encoded, key.command_target_id.value());
  append_field(encoded, key.capability_id.value());
  return encoded;
}

void TrajectoryCache::store(const TrajectoryCacheKey& key, TrajectoryDTO trajectory,
                            TrajectoryCacheRevisionEvidence evidence, std::string intent_identity) {
  if (!key.valid() || !evidence.valid() || intent_identity.empty()) {
    throw std::invalid_argument(
        "trajectory cache store requires a complete slot, projection evidence, and intent");
  }
  auto shared_trajectory = std::make_shared<const TrajectoryDTO>(std::move(trajectory));
  const auto encoded = encoded_key(key);

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(encoded);
  if (it != entries_.end()) {
    it->second.key = key;
    it->second.trajectory = std::move(shared_trajectory);
    it->second.evidence = std::move(evidence);
    it->second.intent_identity = std::move(intent_identity);
    return;
  }

  while (entries_.size() >= config_.max_entries && !insertion_order_.empty()) {
    entries_.erase(insertion_order_.front());
    insertion_order_.pop_front();
  }
  entries_.emplace(encoded, Entry{key, std::move(shared_trajectory), std::move(evidence),
                                  std::move(intent_identity)});
  insertion_order_.push_back(encoded);
}

bool TrajectoryCache::contains(const TrajectoryCacheKey& key) const {
  if (!key.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.contains(encoded_key(key));
}

std::optional<TrajectoryDTO> TrajectoryCache::peek_unvalidated(
    const TrajectoryCacheKey& key) const {
  if (!key.valid()) {
    return std::nullopt;
  }
  std::shared_ptr<const TrajectoryDTO> found;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(encoded_key(key));
    if (it == entries_.end()) {
      return std::nullopt;
    }
    found = it->second.trajectory;
  }
  return *found;
}

TrajectoryCacheLookupResult TrajectoryCache::lookup_with_revision(
    const TrajectoryCacheKey& key, const TrajectoryCacheRevisionEvidence& expected_evidence,
    const std::string& expected_intent_identity) {
  if (!key.valid()) {
    return {TrajectoryCacheLookupDisposition::kMiss, std::nullopt};
  }
  if (!expected_evidence.valid()) {
    return {TrajectoryCacheLookupDisposition::kAuthoritativeRevisionUnavailable, std::nullopt};
  }

  const auto encoded = encoded_key(key);
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = entries_.find(encoded);
  if (it == entries_.end()) {
    return {TrajectoryCacheLookupDisposition::kMiss, std::nullopt};
  }
  if (!it->second.evidence.valid()) {
    erase_locked(encoded);
    return {TrajectoryCacheLookupDisposition::kRevisionMismatch, std::nullopt};
  }
  if (it->second.intent_identity != expected_intent_identity) {
    erase_locked(encoded);
    return {TrajectoryCacheLookupDisposition::kIntentMismatch, std::nullopt};
  }

  const auto& stored = it->second.evidence;
  const bool exact = stored == expected_evidence;
  const bool lineage_matches =
      same_projection_lineage(stored.projection_revision, expected_evidence.projection_revision);
  const bool scene_not_older = expected_evidence.projection_revision.scene_revision.revision_id >=
                               stored.projection_revision.scene_revision.revision_id;
  const bool collision_not_older = expected_evidence.collision_world_revision.revision_id >=
                                   stored.collision_world_revision.revision_id;
  const bool advanced = expected_evidence.projection_revision.scene_revision.revision_id >
                            stored.projection_revision.scene_revision.revision_id ||
                        expected_evidence.collision_world_revision.revision_id >
                            stored.collision_world_revision.revision_id;
  const bool revalidation_candidate =
      lineage_matches && scene_not_older && collision_not_older && advanced &&
      !same_revision_number_has_conflicting_content(stored, expected_evidence);
  if (!exact && !revalidation_candidate) {
    erase_locked(encoded);
    return {TrajectoryCacheLookupDisposition::kRevisionMismatch, std::nullopt};
  }

  auto found = it->second.trajectory;
  lock.unlock();
  return {exact ? TrajectoryCacheLookupDisposition::kHit
                : TrajectoryCacheLookupDisposition::kRevalidationCandidate,
          *found};
}

std::optional<TrajectoryCacheRevisionEvidence> TrajectoryCache::revision_of(
    const TrajectoryCacheKey& key) const {
  if (!key.valid()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(encoded_key(key));
  if (it == entries_.end() || !it->second.evidence.valid()) {
    return std::nullopt;
  }
  return it->second.evidence;
}

void TrajectoryCache::invalidate(const TrajectoryCacheKey& key) {
  if (!key.valid()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  erase_locked(encoded_key(key));
}

void TrajectoryCache::invalidate_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  insertion_order_.clear();
}

size_t TrajectoryCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

size_t TrajectoryCache::capacity() const {
  return config_.max_entries;
}

void TrajectoryCache::erase_locked(const std::string& encoded) {
  if (entries_.erase(encoded) > 0U) {
    const auto it = std::find(insertion_order_.begin(), insertion_order_.end(), encoded);
    if (it != insertion_order_.end()) {
      insertion_order_.erase(it);
    }
  }
}

}  // namespace manyforge::core
