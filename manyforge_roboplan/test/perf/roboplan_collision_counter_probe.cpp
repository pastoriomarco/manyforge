#include <roboplan/core/scene.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>

namespace {

std::atomic<std::uint64_t> g_query_count{0};
std::atomic<std::uint64_t> g_query_duration_ns{0};

using RealHasCollisions = bool (*)(
    const roboplan::Scene*, const Eigen::VectorXd&, bool);

RealHasCollisions resolve_real_has_collisions() noexcept {
  constexpr const char* kSymbol =
      "_ZNK8roboplan5Scene13hasCollisionsERKN5Eigen6MatrixIdLin1ELi1ELi0ELin1ELi1EEEb";
  if (void* symbol = dlsym(RTLD_NEXT, kSymbol); symbol != nullptr) {
    return reinterpret_cast<RealHasCollisions>(symbol);
  }

  // Python imports extension modules with RTLD_LOCAL by default. In that
  // layout libroboplan is outside RTLD_NEXT's global search scope even though
  // the interposed call originates from the extension. Acquire its existing
  // object explicitly and retain the handle for process lifetime so the
  // returned function pointer cannot outlive the library.
  void* const roboplan =
      dlopen("libroboplan.so", RTLD_NOW | RTLD_NOLOAD);
  if (roboplan == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<RealHasCollisions>(dlsym(roboplan, kSymbol));
}

void record_duration(
    const std::chrono::steady_clock::time_point started_at) noexcept {
  const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started_at)
                            .count();
  g_query_count.fetch_add(1, std::memory_order_relaxed);
  g_query_duration_ns.fetch_add(
      static_cast<std::uint64_t>(duration), std::memory_order_relaxed);
}

}  // namespace

extern "C" bool manyforge_roboplan_collision_probe_snapshot(
    std::uint64_t* query_count,
    std::uint64_t* query_duration_ns) noexcept {
  if (query_count == nullptr || query_duration_ns == nullptr) {
    return false;
  }
  *query_count = g_query_count.load(std::memory_order_relaxed);
  *query_duration_ns =
      g_query_duration_ns.load(std::memory_order_relaxed);
  return true;
}

extern "C" void manyforge_roboplan_collision_probe_reset() noexcept {
  g_query_count.store(0, std::memory_order_relaxed);
  g_query_duration_ns.store(0, std::memory_order_relaxed);
}

namespace roboplan {

bool Scene::hasCollisions(const Eigen::VectorXd& q, const bool debug) const {
  static const RealHasCollisions real = resolve_real_has_collisions();
  if (real == nullptr) {
    std::abort();
  }
  const auto started_at = std::chrono::steady_clock::now();
  try {
    const bool result = real(this, q, debug);
    record_duration(started_at);
    return result;
  } catch (...) {
    record_duration(started_at);
    throw;
  }
}

}  // namespace roboplan
