// Offline matched geometry/query probe. Both revisions consume the SAME model
// files and numeric poses; only the historical/current snapshot adapters
// differ. It never creates a Kernel, controller, runtime scene owner or
// hardware lane.
#include <coal/shape/geometric_shapes.h>

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <pinocchio/parsers/srdf.hpp>
#include <pinocchio/parsers/urdf.hpp>
#ifdef MANYFORGE_COST_CURRENT
#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
namespace core = manyforge::core;
using Engine = manyforge::collision::ManyForgeCollisionEngine;
using Clock = std::chrono::steady_clock;
// Count standard C++ heap allocation separately from timing; not an assertion
// about direct malloc/aligned allocation or controller real-time guarantees.
thread_local bool counting = false;
thread_local std::size_t allocations = 0;
void* allocate(std::size_t size) {
  if (counting) ++allocations;
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
core::Pose3DTO pose(const pinocchio::SE3& value) {
  const Eigen::Quaterniond q(value.rotation());
  return {{value.translation().x(), value.translation().y(), value.translation().z()},
          {q.x(), q.y(), q.z(), q.w()}};
}
template <class Function>
void measure(const std::string& name, std::size_t count, Function work) {
  std::vector<double> costs(count);
  for (std::size_t i = 0; i < 20; ++i) work();
  allocations = 0;
  counting = true;
  for (std::size_t i = 0; i < count; ++i) work();
  counting = false;
  const auto counted = allocations;
  for (auto& cost : costs) {
    const auto start = Clock::now();
    work();
    cost = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  }
  std::sort(costs.begin(), costs.end());
  std::cout << "{\"operation\":\"" << name << "\",\"samples\":" << count
            << ",\"allocations_per_call\":" << double(counted) / count
            << ",\"p95_us\":" << costs[count * 95 / 100]
            << ",\"p99_us\":" << costs[count * 99 / 100] << ",\"max_us\":" << costs.back() << "}\n";
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

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  require(argc == 2, "provide the one frozen model workspace root");
  const std::filesystem::path root(argv[1]);
  const auto directory = root / "manyforge_example_models/models/ur10e_robotiq_2f_140";
  const auto urdf = directory / "ur10e_robotiq_2f_140.urdf";
  const auto srdf = directory / "ur10e_robotiq_2f_140.srdf";
  const std::vector<std::filesystem::path> packages{root};
  const auto start = Clock::now();
  const auto shared = Engine::load_shared_model("matched-ur10e", urdf, srdf, packages, "arm");
  std::cout << std::setprecision(12) << "{\"model_load_us\":"
            << std::chrono::duration<double, std::micro>(Clock::now() - start).count() << "}\n";
  Engine engine(shared, Engine::make_shared_scene_cache(shared));
  // The untimed independent inventory supplies exact frame/geometry
  // descriptors.
  pinocchio::Model model;
  pinocchio::GeometryModel geometry;
  pinocchio::urdf::buildModel(urdf.string(), model, false, true);
  pinocchio::urdf::buildGeom(model, urdf.string(), pinocchio::COLLISION, geometry,
                             std::vector<std::string>{root.string()});
  geometry.addAllCollisionPairs();
  pinocchio::srdf::removeCollisionPairs(model, geometry, srdf.string());
  core::CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "matched-offline-probe";
  snapshot.revision.revision_id = 1;
  snapshot.revision.content_hash = "matched-geometry-1";
  snapshot.contains_mesh_geometry = true;
  std::map<std::string, std::string> labels;
#ifdef MANYFORGE_COST_CURRENT
  std::vector<manyforge::collision::RetainedComponentCollisionBodyProjectionInput> bodies;
  std::set<std::string> frames;
  for (const auto& object : geometry.geometryObjects) {
    const auto& frame = model.frames[object.parentFrame].name;
    if (frames.insert(frame).second)
      bodies.push_back({core::ControllableComponentId("matched"), core::ModelInstanceId("model"),
                        core::ModelRevision("model-1"), core::CollisionBodyRoleId(frame), 0,
                        frame});
  }
  const std::vector<manyforge::collision::RetainedComponentModelFrameProjectionInput> model_frames{
      {core::ControllableComponentId("matched"), core::ModelInstanceId("model"),
       core::ModelRevision("model-1"), core::FrameRoleId("world"), "base_link"}};
  const auto projected =
      Engine::project_retained_component_collision(shared, "base_link", bodies, model_frames);
  if (!projected.ok()) throw std::runtime_error(projected.error().message);
  snapshot.bodies = projected.value().component_bodies;
  snapshot.component_model_frames = projected.value().component_model_frames;
  for (const auto& body : snapshot.bodies) labels[body.body_id] = body.frame_id;
#else
  std::map<std::string, unsigned> indices;
  for (const auto& object : geometry.geometryObjects) {
    const auto& frame = model.frames[object.parentFrame];
    core::CollisionWorldBody body;
    body.body_id =
        "robot/matched/link/" + frame.name + "/collision/" + std::to_string(indices[frame.name]++);
    body.owner_kind = core::CollisionOwnerKind::kRobotLink;
    body.frame_id = "world";
    core::CollisionGeometryDescriptor descriptor;
    descriptor.body_id = body.body_id;
    descriptor.owner_kind = body.owner_kind;
    if (const auto* box = dynamic_cast<const coal::Box*>(object.geometry.get())) {
      descriptor.geometry_kind = core::CollisionGeometryKind::kBox;
      descriptor.box_dimensions_m =
          core::Vector3DTO{2 * box->halfSide.x(), 2 * box->halfSide.y(), 2 * box->halfSide.z()};
    } else {
      descriptor.geometry_kind = core::CollisionGeometryKind::kTriangleMesh;
      require(!object.meshPath.empty(), "frozen dataset has unsupported non-mesh geometry");
      descriptor.source_uri = object.meshPath;
      descriptor.mesh_scale =
          core::Vector3DTO{object.meshScale.x(), object.meshScale.y(), object.meshScale.z()};
    }
    descriptor.local_pose = pose(frame.placement.inverse() * object.placement);
    body.geometries.push_back(descriptor);
    labels[body.body_id] = frame.name;
    snapshot.bodies.push_back(body);
  }
  snapshot.robot_model_frames.push_back({"matched", "base_link", {}});
#endif
  core::CollisionWorldBody fixture;
  fixture.body_id = "scene/matched-obstacle/shape/0";
  fixture.frame_id = "world";
#ifdef MANYFORGE_COST_CURRENT
  fixture.owner_kind = core::CollisionOwnerKind::kSceneResource;
#else
  fixture.owner_kind = core::CollisionOwnerKind::kFixture;
#endif
  core::CollisionGeometryDescriptor box;
  box.body_id = fixture.body_id;
  box.owner_kind = fixture.owner_kind;
  box.geometry_kind = core::CollisionGeometryKind::kBox;
  box.box_dimensions_m = core::Vector3DTO{0.2, 0.2, 0.2};
  fixture.geometries.push_back(box);
  fixture.pose.position_m.x_m = 10;
  labels[fixture.body_id] = "obstacle";
  snapshot.bodies.push_back(fixture);
  snapshot.body_count = snapshot.bodies.size();
  snapshot.geometry_count = geometry.geometryObjects.size() + 1;
  std::cout << "{\"robot_geometry_count\":" << geometry.geometryObjects.size()
            << ",\"base_pair_count\":" << geometry.collisionPairs.size() << "}\n";
  const std::vector<std::string> names{"shoulder_pan_joint", "shoulder_lift_joint",
                                       "elbow_joint",        "wrist_1_joint",
                                       "wrist_2_joint",      "wrist_3_joint"};
  std::vector<double> positions(6, 0);
  require(engine.prepare_runtime_query(names) == core::RuntimeCollisionQueryPrepareStatus::kReady,
          "runtime joint preparation failed");
  const auto query = [&](core::CollisionDistanceRequestMode mode) {
    return engine.check_runtime(snapshot, {positions, 0, 50'000'000, mode, {}});
  };
  for (int profile = 0; profile < 3; ++profile) {
    const std::string name = profile == 0 ? "home" : profile == 1 ? "rest" : "collision";
    if (profile == 1) positions = {0, -0.785, 0.785, 0, 1.57, 0};
    if (profile == 2) {
      positions.assign(6, 0);
      snapshot.bodies.back().pose.position_m.x_m = 0;
      snapshot.bodies.back().geometries.front().box_dimensions_m = core::Vector3DTO{4, 4, 4};
    }
    ++snapshot.revision.revision_id;
    snapshot.revision.content_hash = "initial-" + name;
    core::CollisionQuery initial;
    initial.query_id = "initial-" + name;
    initial.collision_world_revision = snapshot.revision;
    initial.robot_state.joint_names = names;
    initial.robot_state.positions_rad = positions;
    const auto initial_start = Clock::now();
    const auto initialized = engine.check(snapshot, initial, core::CollisionPolicySnapshot{},
                                          core::CollisionCancellationToken{});
    if (initialized.engine_status != core::CollisionEngineStatus::kOk) {
      for (const auto& [key, value] : initialized.raw_diagnostics)
        std::cerr << key << '=' << value << '\n';
      throw std::runtime_error("initial query/preparation failed");
    }
    std::cout << "{\"initial_profile\":\"" << name << "\",\"prepare_and_query_us\":"
              << std::chrono::duration<double, std::micro>(Clock::now() - initial_start).count()
              << "}\n";
    measure(name + "_revision_prepare", 20, [&] {
      ++snapshot.revision.revision_id;
      snapshot.revision.content_hash =
          "matched-geometry-" + std::to_string(snapshot.revision.revision_id);
      require(engine.prepare_runtime_snapshot(snapshot) ==
                  core::RuntimeCollisionQueryPrepareStatus::kReady,
              "runtime snapshot preparation failed");
    });
    for (const auto mode : {core::CollisionDistanceRequestMode::kNone,
                            core::CollisionDistanceRequestMode::kAllCollidingPairs}) {
      const auto value = query(mode);
      require(value.status == (profile == 2 ? core::RuntimeCollisionQueryStatus::kCollision
                                            : core::RuntimeCollisionQueryStatus::kClear),
              "unexpected collision decision");
      const auto counters = engine.internal_last_query_mechanism_counters();
      std::cout << "{\"profile\":\"" << name << "\",\"distance_mode\":" << int(mode)
                << ",\"active_pairs\":" << counters.active_collision_pair_count
                << ",\"collision_pairs\":" << value.collision_pairs.size()
                << ",\"distance_calls\":" << counters.distance_api_call_count << "}\n";
      std::map<std::string, std::optional<double>> evidence;
      for (const auto& pair : value.collision_pairs) {
        auto a = labels.at(pair.pair->body_a), b = labels.at(pair.pair->body_b);
        if (b < a) std::swap(a, b);
        evidence[a + "|" + b] = std::nullopt;
      }
      for (const auto& pair : value.clearance_pairs) {
        auto a = labels.at(pair.pair->body_a), b = labels.at(pair.pair->body_b);
        if (b < a) std::swap(a, b);
        evidence[a + "|" + b] = pair.signed_clearance_m;
      }
      std::cout << "{\"physical_pair_evidence\":{";
      bool first = true;
      for (const auto& [pair, distance] : evidence) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << std::quoted(pair) << ':';
        if (distance)
          std::cout << *distance;
        else
          std::cout << "null";
      }
      std::cout << "}}\n";
      measure(
          name + (mode == core::CollisionDistanceRequestMode::kNone ? "_warm" : "_distance_warm"),
          500, [&] { require(query(mode).status == value.status, "unstable collision decision"); });
    }
  }
#ifdef MANYFORGE_COST_CURRENT
  // A short real production-scheduler run, separate from the offline cost and
  // allocation passes. No Stop/safety interference or full-Kernel claim.
  snapshot.bodies.back() = fixture;
  ++snapshot.revision.revision_id;
  snapshot.revision.content_hash = "schedule";
  require(
      engine.prepare_runtime_snapshot(snapshot) == core::RuntimeCollisionQueryPrepareStatus::kReady,
      "schedule snapshot preparation failed");
  manyforge::roboplan_backend::WallClock clock;
  manyforge::roboplan_backend::SimpleScheduler scheduler;
  std::mutex mutex;
  std::condition_variable done;
  std::atomic<unsigned> ticks{0};
  core::RuntimeCollisionMonitor monitor(
      clock, scheduler, {50'000'000},
      [&] {
        require(query(core::CollisionDistanceRequestMode::kNone).status ==
                    core::RuntimeCollisionQueryStatus::kClear,
                "scheduled collision query failed");
        if (++ticks == 40) {
          std::lock_guard lock(mutex);
          done.notify_one();
        }
        return core::Result<void>::Ok();
      },
      [&] { return ticks.load() < 40; });
  monitor.start();
  {
    std::unique_lock lock(mutex);
    require(done.wait_for(lock, std::chrono::seconds(4), [&] { return ticks.load() == 40; }),
            "bounded scheduler qualification expired");
  }
  monitor.stop(true);
  const auto metrics = monitor.timing_metrics();
  std::cout << "{\"period_ns\":50000000,\"callbacks\":" << metrics.callbacks_started
            << ",\"jitter_max_ns\":" << metrics.scheduled_to_start_jitter_max_ns
            << ",\"callback_max_ns\":" << metrics.callback_duration_max_ns
            << ",\"missed_periods\":" << metrics.missed_periods << "}\n";
  require(metrics.missed_periods == 0, "collision monitor missed a period");
#endif
}
