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
                            std::string intent_identity) {
  if (!key.valid() || intent_identity.empty()) {
    throw std::invalid_argument("trajectory cache store requires a complete slot and intent");
  }
  auto shared_trajectory = std::make_shared<const TrajectoryDTO>(std::move(trajectory));
  const auto encoded = encoded_key(key);

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(encoded);
  if (it != entries_.end()) {
    it->second.key = key;
    it->second.trajectory = std::move(shared_trajectory);
    it->second.intent_identity = std::move(intent_identity);
    return;
  }

  while (entries_.size() >= config_.max_entries && !insertion_order_.empty()) {
    entries_.erase(insertion_order_.front());
    insertion_order_.pop_front();
  }
  entries_.emplace(encoded, Entry{key, std::move(shared_trajectory), std::move(intent_identity)});
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

TrajectoryCacheLookupResult TrajectoryCache::lookup(const TrajectoryCacheKey& key,
                                                    const std::string& expected_intent_identity) {
  if (!key.valid()) {
    return {TrajectoryCacheLookupDisposition::kMiss, std::nullopt};
  }

  const auto encoded = encoded_key(key);
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = entries_.find(encoded);
  if (it == entries_.end()) {
    return {TrajectoryCacheLookupDisposition::kMiss, std::nullopt};
  }
  if (it->second.intent_identity != expected_intent_identity) {
    erase_locked(encoded);
    return {TrajectoryCacheLookupDisposition::kIntentMismatch, std::nullopt};
  }

  auto found = it->second.trajectory;
  lock.unlock();
  return {TrajectoryCacheLookupDisposition::kFound, *found};
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
