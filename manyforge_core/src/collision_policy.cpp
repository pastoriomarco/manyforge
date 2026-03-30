#include <manyforge_core/collision/collision_policy.hpp>

namespace manyforge::core {

CollisionPolicy::CollisionPolicy(StopPolicy& stop_policy,
                                 CacheInvalidationCallback invalidate_cache,
                                 CollisionEventCallback on_collision)
    : stop_policy_(stop_policy),
      invalidate_cache_(std::move(invalidate_cache)),
      on_collision_(std::move(on_collision)) {}

void CollisionPolicy::set_response(CollisionSeverity severity,
                                   CollisionResponse response) {
  std::lock_guard<std::mutex> lock(mutex_);
  responses_[severity] = response;
}

CollisionResponse CollisionPolicy::get_response(
    CollisionSeverity severity) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = responses_.find(severity);
  if (it != responses_.end()) {
    return it->second;
  }
  return default_response(severity);
}

Result<void> CollisionPolicy::handle_collision(const CollisionEvent& event) {
  CollisionResponse response = get_response(event.severity);

  if (response.should_stop) {
    auto result = (response.stop_type == StopType::kSoft)
        ? stop_policy_.request_soft_stop(response.deceleration_ns)
        : stop_policy_.request_hard_stop();
    if (!result.ok()) {
      return result;
    }
  }

  if (response.invalidate_cache && invalidate_cache_) {
    invalidate_cache_();
  }

  if (on_collision_) {
    on_collision_(event, response);
  }

  return Result<void>::Ok();
}

Result<void> CollisionPolicy::pre_execute_check(const CollisionEvent* event) {
  if (event == nullptr) {
    return Result<void>::Ok();
  }
  return Result<void>::Err(ErrorCode::kCollisionDetected);
}

CollisionResponse CollisionPolicy::default_response(
    CollisionSeverity severity) const {
  switch (severity) {
    case CollisionSeverity::kWarning:
      return {false, StopType::kSoft, 0, false, ErrorCode::kCollisionDetected};
    case CollisionSeverity::kSoftStop:
      return {true, StopType::kSoft, 100'000'000, true,
              ErrorCode::kCollisionDetected};
    case CollisionSeverity::kHardStop:
      return {true, StopType::kHard, 0, true,
              ErrorCode::kCollisionDetected};
  }
  return {true, StopType::kHard, 0, true, ErrorCode::kCollisionDetected};
}

}  // namespace manyforge::core
