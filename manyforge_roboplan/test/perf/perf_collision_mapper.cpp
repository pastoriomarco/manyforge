// Chunk 1 informational perf fixture (collision-ownership plan).
//
// Measures the per-call overhead of the body-id mapper's hot path:
// rebuild + N lookups. This is the only new code Chunk 1 adds to the
// collision_check call site; legacy collision_check itself is unchanged.
//
// Output: prints a single JSON line on stdout when MANYFORGE_PERF_JSON_OUT
// is set to a writable file path; otherwise prints the same JSON to stdout.
// scripts/run-collision-mapper-perf.sh compares this measurement against the
// checked-in baseline.
//
// This binary is intentionally not a gtest case (the perf measurement is
// not a pass/fail assertion in Chunk 1; the gate phases in once historical
// data calibrates a sensible threshold — see the implementation note).

#include <manyforge_roboplan/collision_body_id_mapper.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using manyforge::core::CollisionShapeDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::roboplan_backend::RoboPlanCollisionBodyIdMapper;
using manyforge::roboplan_backend::RoboPlanPlanningContext;

using Snapshot = RoboPlanPlanningContext::SceneObjectSnapshot;

Snapshot make_object(const std::string& id, bool attached) {
  Snapshot s;
  s.object_id = id;
  s.shape = CollisionShapeDTO{BoxShapeDTO{}};
  s.frame_id = "universe";
  s.attached = attached;
  return s;
}

std::vector<Snapshot> make_scene(std::size_t num_objects,
                                  std::size_t num_attached) {
  std::vector<Snapshot> objs;
  objs.reserve(num_objects);
  for (std::size_t i = 0; i < num_objects; ++i) {
    const bool attached = i < num_attached;
    objs.push_back(make_object("object_" + std::to_string(i), attached));
  }
  return objs;
}

double percentile(std::vector<double> samples, double p) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t idx =
      static_cast<std::size_t>(p * (samples.size() - 1));
  return samples[idx];
}

}  // namespace

int main() {
  // Scene size representative of the Isaac demo light scene: a handful of
  // detached fixtures and one attached object. Sized to be cheap; the
  // measurement is not noise-free at sub-microsecond scale, so we run many
  // iterations.
  constexpr std::size_t kSceneObjects = 8;
  constexpr std::size_t kAttachedObjects = 1;
  constexpr std::size_t kLookupsPerIter = 32;
  constexpr std::size_t kIterations = 50;
  constexpr std::size_t kWarmupIterations = 5;

  const auto scene = make_scene(kSceneObjects, kAttachedObjects);

  // Lookup keys: a mix of known names and one always-unknown name. The
  // mix mirrors the wrapping layer: the kernel passes raw RoboPlan body
  // names, most of which are known after the scene-object and robot-body
  // maps have been published, with unknown names still handled fail-closed.
  std::vector<std::string> lookup_keys;
  lookup_keys.reserve(kLookupsPerIter);
  for (std::size_t i = 0; i < kLookupsPerIter; ++i) {
    if (i % 4 == 3) {
      lookup_keys.push_back("unknown_link_collision_" + std::to_string(i));
    } else {
      lookup_keys.push_back("object_" + std::to_string(i % kSceneObjects));
    }
  }

  std::vector<double> rebuild_us;
  std::vector<double> lookup_us;
  rebuild_us.reserve(kIterations);
  lookup_us.reserve(kIterations);

  for (std::size_t iter = 0; iter < kWarmupIterations + kIterations; ++iter) {
    RoboPlanCollisionBodyIdMapper mapper;

    // Rebuild timing.
    auto t0 = std::chrono::steady_clock::now();
    mapper.rebuild("ur10e", scene, {});
    auto t1 = std::chrono::steady_clock::now();

    // Lookup timing: do the full lookup batch, accumulate result count to
    // prevent the optimizer from removing the calls.
    std::size_t hits = 0;
    auto t2 = std::chrono::steady_clock::now();
    for (const auto& key : lookup_keys) {
      if (mapper.lookup(key).has_value()) ++hits;
    }
    auto t3 = std::chrono::steady_clock::now();
    (void)hits;

    if (iter < kWarmupIterations) continue;

    const auto rebuild_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const auto lookup_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
    rebuild_us.push_back(static_cast<double>(rebuild_ns) / 1000.0);
    lookup_us.push_back(static_cast<double>(lookup_ns) / 1000.0);
  }

  const double rebuild_p50 = percentile(rebuild_us, 0.50);
  const double rebuild_p95 = percentile(rebuild_us, 0.95);
  const double rebuild_p99 = percentile(rebuild_us, 0.99);
  const double lookup_p50 = percentile(lookup_us, 0.50);
  const double lookup_p95 = percentile(lookup_us, 0.95);
  const double lookup_p99 = percentile(lookup_us, 0.99);

  std::ostringstream out;
  out << "{"
      << "\"schema\": \"manyforge.collision_mapper_perf.v1\","
      << "\"scene_objects\": " << kSceneObjects << ","
      << "\"attached_objects\": " << kAttachedObjects << ","
      << "\"lookups_per_iter\": " << kLookupsPerIter << ","
      << "\"iterations\": " << kIterations << ","
      << "\"warmup_iterations\": " << kWarmupIterations << ","
      << "\"rebuild_us\": {"
      << "\"p50\": " << rebuild_p50 << ","
      << "\"p95\": " << rebuild_p95 << ","
      << "\"p99\": " << rebuild_p99 << "},"
      << "\"lookup_batch_us\": {"
      << "\"p50\": " << lookup_p50 << ","
      << "\"p95\": " << lookup_p95 << ","
      << "\"p99\": " << lookup_p99 << "}"
      << "}";

  const char* out_path = std::getenv("MANYFORGE_PERF_JSON_OUT");
  if (out_path != nullptr) {
    std::ofstream f(out_path);
    if (!f) {
      std::cerr << "perf_collision_mapper: failed to open MANYFORGE_PERF_JSON_OUT="
                << out_path << "\n";
      return 2;
    }
    f << out.str() << "\n";
  }
  std::cout << out.str() << "\n";
  return 0;
}
