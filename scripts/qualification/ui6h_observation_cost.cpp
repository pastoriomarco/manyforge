// Matched offline publication probe: compile this same file against each
// revision's observed_robot_state_publication.cpp/header. No runtime owner,
// controller, collision or browser performance claim is implied.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool count_allocations = false;
std::size_t allocations = 0;
void* allocate(std::size_t size) {
  if (count_allocations) ++allocations;
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
}  // namespace
void* operator new(std::size_t size) {
  return allocate(size);
}
void* operator new[](std::size_t size) {
  return allocate(size);
}
void operator delete(void* value) noexcept {
  std::free(value);
}
void operator delete[](void* value) noexcept {
  std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept {
  std::free(value);
}

template <class Function>
void measure(const char* name, std::size_t joints, Function work) {
  constexpr std::size_t count = 10'000;
  std::vector<double> costs(count);
  for (std::size_t i = 0; i < 100; ++i) work();
  // Allocation counting is a separate pass, not part of latency timing.
  allocations = 0;
  count_allocations = true;
  for (std::size_t i = 0; i < count; ++i) work();
  count_allocations = false;
  const auto counted = allocations;
  for (auto& cost : costs) {
    const auto begin = std::chrono::steady_clock::now();
    work();
    cost =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
  }
  std::sort(costs.begin(), costs.end());
  std::cout << "{\"operation\":\"" << name << "\",\"joints\":" << joints << ",\"samples\":" << count
            << ",\"allocations_per_call\":" << static_cast<double>(counted) / count
            << ",\"p95_ns\":" << costs[count * 95 / 100]
            << ",\"p99_ns\":" << costs[count * 99 / 100] << ",\"max_ns\":" << costs.back() << "}\n";
}

int main() {
  using namespace manyforge::core;
  for (const std::size_t count : {6U, 256U}) {
    std::vector<std::string> names;
    std::vector<double> positions(count), velocities(count);
    for (std::size_t i = 0; i < count; ++i) {
      names.push_back("joint" + std::to_string(i + 1));
      positions[i] = static_cast<double>(i) / count;
      velocities[i] = 0.1;
    }
    ObservedRobotStatePublication publication(
        names, {{ObservedRobotStateSourceKind::kJointState, "matched-source",
                 ObservedRobotStateAuthorityProfile::kMeasured, 100'000'000}});
    std::int64_t now = 31LL * 365 * 24 * 60 * 60 * 1'000'000'000;
    measure("publish", count, [&] {
      ++now;
      const auto result = publication.publish(ObservedRobotStateSourceKind::kJointState, names,
                                              positions, velocities, now, now);
      if (!result.published || !result.complete || !result.velocities_complete)
        throw std::runtime_error("publication was not complete");
    });
    measure("acquire", count, [&] {
      const auto view = publication.acquire(now);
      if (!view || !view.complete() || view.positions_rad().size() != count ||
          view.positions_rad().back() != positions.back())
        throw std::runtime_error("acquisition changed the dataset");
    });
    measure("snapshot", count, [&] {
      const auto view = publication.snapshot(now);
      if (!view || !view->complete || view->positions_rad != positions)
        throw std::runtime_error("snapshot changed the dataset");
    });
#ifdef MANYFORGE_PROBE_CURRENT_AS_OF
    measure("acquire_as_of_current_only", count, [&] {
      const auto view = publication.acquire_as_of(now);
      if (!view || !view.complete() || view.positions_rad().size() != count)
        throw std::runtime_error("as-of acquisition failed");
    });
#endif
  }
}
