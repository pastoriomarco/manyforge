#include <atomic>
#include <manyforge_core/time/stuck_drain_wait.hpp>

namespace manyforge::core {
namespace {
std::atomic<const char*> first_stuck_drain{nullptr};
}  // namespace

const char* FirstStuckDrainName() noexcept {
  return first_stuck_drain.load(std::memory_order_acquire);
}

void detail::RecordStuckDrain(const char* drain) noexcept {
  const char* empty = nullptr;
  (void)first_stuck_drain.compare_exchange_strong(empty, drain, std::memory_order_release,
                                                  std::memory_order_relaxed);
}

}  // namespace manyforge::core
