#include <manyforge_core/collision/collision_policy.hpp>

namespace manyforge::core {

CollisionPolicy::CollisionPolicy(StopPolicy& stop_policy,
                                 CacheInvalidationCallback invalidate_cache,
                                 CollisionEventCallback on_collision)
    : stop_policy_(stop_policy),
      invalidate_cache_(std::move(invalidate_cache)),
      on_collision_(std::move(on_collision)) {}

void CollisionPolicy::set_response(CollisionResponse response) {
  std::lock_guard<std::mutex> lock(mutex_);
  response_ = response;
}

CollisionResponse CollisionPolicy::get_response() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return response_;
}

Result<void> CollisionPolicy::handle_collision(const CollisionCheckResultDTO& check) {
  if (!check.in_collision) {
    return Result<void>::Ok();
  }

  CollisionResponse response = get_response();

  auto result = (response.stop_type == StopType::kSoft)
      ? stop_policy_.request_soft_stop(response.deceleration_ns)
      : stop_policy_.request_hard_stop();
  if (!result.ok()) return result;

  if (response.invalidate_cache && invalidate_cache_) {
    invalidate_cache_();
  }

  if (on_collision_) {
    on_collision_(check);
  }

  return Result<void>::Ok();
}

}  // namespace manyforge::core
