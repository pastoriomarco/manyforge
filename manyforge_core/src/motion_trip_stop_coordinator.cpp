#include <manyforge_core/motion_trip_stop_coordinator.hpp>

namespace manyforge::core {

void MotionTripStopCoordinator::begin(std::stop_source stop_source) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_source_ = std::move(stop_source);
    collision_detected_.store(false, std::memory_order_release);
  }
  active_.store(true, std::memory_order_release);
}

void MotionTripStopCoordinator::deactivate() {
  active_.store(false, std::memory_order_release);
}

MotionTripStopCoordinator::FinishResult MotionTripStopCoordinator::finish() {
  deactivate();
  std::lock_guard<std::mutex> lock(mutex_);
  FinishResult result{
      collision_detected_.load(std::memory_order_acquire),
  };
  stop_source_.reset();
  return result;
}

void MotionTripStopCoordinator::reset() {
  active_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mutex_);
  collision_detected_.store(false, std::memory_order_release);
  stop_source_.reset();
}

bool MotionTripStopCoordinator::active() const {
  return active_.load(std::memory_order_acquire);
}

bool MotionTripStopCoordinator::collision_detected() const {
  return collision_detected_.load(std::memory_order_acquire);
}

bool MotionTripStopCoordinator::request_stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stop_source_.has_value()) {
    return false;
  }
  stop_source_->request_stop();
  return true;
}

bool MotionTripStopCoordinator::signal_collision_and_cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  collision_detected_.store(true, std::memory_order_release);
  if (!stop_source_.has_value()) {
    return false;
  }
  stop_source_->request_stop();
  return true;
}

}  // namespace manyforge::core
