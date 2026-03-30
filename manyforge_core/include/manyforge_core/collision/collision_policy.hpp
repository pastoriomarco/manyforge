#pragma once

#include <functional>
#include <map>
#include <mutex>

#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/types/collision_event.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Configured response for a given collision severity.
/// ADR-0009: collision response policy is explicit and configurable.
struct CollisionResponse {
  bool should_stop = true;
  StopType stop_type = StopType::kHard;
  Duration deceleration_ns = 100'000'000;  ///< 100ms default for soft stops.
  bool invalidate_cache = true;
  ErrorCode error_code = ErrorCode::kCollisionDetected;
};

/// Policy layer for collision response.
/// ADR-0009: on collision -> apply configured stop mode, invalidate cache,
/// emit event.
class CollisionPolicy {
 public:
  using CacheInvalidationCallback = std::function<void()>;
  using CollisionEventCallback =
      std::function<void(const CollisionEvent&, const CollisionResponse&)>;

  /// @param stop_policy The stop policy to drive on collision.
  /// @param invalidate_cache Called to invalidate the trajectory cache.
  /// @param on_collision Called after handling each collision event.
  explicit CollisionPolicy(
      StopPolicy& stop_policy,
      CacheInvalidationCallback invalidate_cache = nullptr,
      CollisionEventCallback on_collision = nullptr);

  /// Set the response for a given severity level.
  void set_response(CollisionSeverity severity, CollisionResponse response);

  /// Get the configured response for a given severity level.
  CollisionResponse get_response(CollisionSeverity severity) const;

  /// Handle a collision event. Applies the configured response policy:
  /// stops if configured, invalidates cache if configured, fires callback.
  /// @return Ok on success (or no-stop warning), Err if stop transition fails.
  Result<void> handle_collision(const CollisionEvent& event);

  /// Pre-execution collision check. If a collision is present, blocks
  /// execution by returning an error.
  /// @param event Pointer to a collision event, or nullptr if none.
  /// @return Ok if no collision, Err(kCollisionDetected) if collision present.
  Result<void> pre_execute_check(const CollisionEvent* event);

 private:
  CollisionResponse default_response(CollisionSeverity severity) const;

  StopPolicy& stop_policy_;
  CacheInvalidationCallback invalidate_cache_;
  CollisionEventCallback on_collision_;
  mutable std::mutex mutex_;
  std::map<CollisionSeverity, CollisionResponse> responses_;
};

}  // namespace manyforge::core
