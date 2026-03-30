#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

/// Configuration for the trajectory cache.
/// ADR-0008: bounded by size + TTL.
struct CacheConfig {
  /// Maximum number of entries in the cache.
  size_t max_entries = 100;

  /// Time-to-live for cache entries in nanoseconds. 0 = no TTL.
  Duration ttl_ns = 0;
};

/// Cache key per ADR-0008: request + start state + scene + planner.
struct CacheKey {
  /// Hash of the movement request (goal pose, constraints, etc).
  uint64_t request_hash = 0;

  /// Hash of the robot's start state (joint positions).
  uint64_t start_state_hash = 0;

  /// Scene version at planning time.
  uint64_t scene_version = 0;

  /// Planner backend identifier.
  std::string planner_backend_id;

  /// Hash of planner parameters (velocities, tolerances, etc).
  uint64_t planner_params_hash = 0;

  bool operator==(const CacheKey& other) const {
    return request_hash == other.request_hash &&
           start_state_hash == other.start_state_hash &&
           scene_version == other.scene_version &&
           planner_backend_id == other.planner_backend_id &&
           planner_params_hash == other.planner_params_hash;
  }
};

/// Hash function for CacheKey, enabling use in unordered containers.
struct CacheKeyHash {
  size_t operator()(const CacheKey& key) const {
    size_t h = std::hash<uint64_t>{}(key.request_hash);
    h ^= std::hash<uint64_t>{}(key.start_state_hash) +
         0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint64_t>{}(key.scene_version) +
         0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::string>{}(key.planner_backend_id) +
         0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint64_t>{}(key.planner_params_hash) +
         0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

}  // namespace manyforge::core
