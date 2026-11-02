#pragma once

#include <manyforge_core/types/trajectory_dto.hpp>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>

namespace manyforge::core {

struct PreparedExecutionStart {
  bool cache_hit = false;
  bool cache_miss = false;
  bool cache_invalidated = false;
  std::string cache_invalidation_reason;
  TrajectoryDTO trajectory;
  /// Planning attempt of a cached trajectory (its validation step).
  std::uint32_t cache_planning_attempt_index = 0;
};

class WorkerStartGate {
 public:
  void release();
  bool wait(std::stop_token st);

 private:
  std::mutex mutex_;
  std::condition_variable_any cv_;
  bool released_ = false;
};

}  // namespace manyforge::core
