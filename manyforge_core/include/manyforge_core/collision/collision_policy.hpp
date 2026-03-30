#pragma once

#include <functional>
#include <mutex>

#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/validation.hpp>

namespace manyforge::core {

/// Configurable collision response (spec 230 §6.3).
/// Collision is binary — in_collision or not. No severity levels.
struct CollisionResponse {
  StopType stop_type = StopType::kHard;
  Duration deceleration_ns = 0;
  bool invalidate_cache = true;
};

/// Policy layer for collision response.
/// On collision: trigger stop + cache invalidation + emit error.
class CollisionPolicy {
 public:
  using CacheInvalidationCallback = std::function<void()>;
  using CollisionEventCallback = std::function<void(const CollisionCheckResultDTO&)>;

  explicit CollisionPolicy(
      StopPolicy& stop_policy,
      CacheInvalidationCallback invalidate_cache = nullptr,
      CollisionEventCallback on_collision = nullptr);

  void set_response(CollisionResponse response);
  CollisionResponse get_response() const;

  /// Handle runtime collision detection (during execution monitoring).
  /// If in_collision: triggers stop + cache invalidation + callback.
  /// If not in_collision: no-op, returns Ok.
  Result<void> handle_collision(const CollisionCheckResultDTO& check);

  /// Pre-execution collision gate.
  /// If in_collision: returns Err(kValidationCollision).
  /// If not: returns Ok.
  Result<void> pre_execute_check(const CollisionCheckResultDTO& check);

 private:
  StopPolicy& stop_policy_;
  CacheInvalidationCallback invalidate_cache_;
  CollisionEventCallback on_collision_;
  mutable std::mutex mutex_;
  CollisionResponse response_;
};

}  // namespace manyforge::core
