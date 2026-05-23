#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/types/collision.hpp>

namespace manyforge::roboplan_backend {

/// Direct (diagnostic-compare) collision query engine (Chunk 4 of the
/// collision-ownership plan).
///
/// Owns its own roboplan::Scene instance — a fully independent Pinocchio
/// Model + Coal geometry pipeline — so two DirectCollisionQueryEngine
/// instances can be queried concurrently from different worker threads
/// without sharing mutable state and without contending on any other
/// RoboPlanPlanningContext's scene_mutex_.
///
  /// Chunk 4 scope: robot-model-only diagnostic prototype. The engine does
  /// not yet consume CollisionWorldSnapshot scene-object descriptors; that
  /// lands before any authoritative use. Required diagnostics beyond binary
  /// collision (distance, contact points, penetration depth, body-pair
  /// identity) surface as `engine_status = kDiagnosticIncomplete` and
  /// `decision = kInconclusiveFail`, never as a false pass. The engine runs
  /// in **diagnostic compare mode** only; its results are reported alongside
  /// the RoboPlan legacy result in CollisionResult, never as the authoritative
  /// kernel input. Chunk 8's qualification gate is what would promote it.
class DirectCollisionQueryEngine {
 public:
  /// Construct an independent collision query engine.
  ///
  /// @param model_name Logical model identifier (e.g. "ur5_gripper").
  /// @param urdf_path Path to the URDF file.
  /// @param srdf_path Path to the SRDF file.
  /// @param package_paths Directories used by URDF resource resolution.
  /// @param group_name Joint group name used for joint-state vectors.
  DirectCollisionQueryEngine(
      const std::string& model_name,
      const std::filesystem::path& urdf_path,
      const std::filesystem::path& srdf_path,
      const std::vector<std::filesystem::path>& package_paths,
      std::string group_name);

  /// Convenience factory mirroring the create_native_kernel helper:
  /// resolves URDF/SRDF + package paths from the bundled
  /// roboplan_example_models for the given model_name.
  static std::shared_ptr<DirectCollisionQueryEngine> create_from_example_model(
      const std::string& model_name = "ur5_gripper",
      const std::string& group_name = "arm");

  /// Run a collision query against this engine's independent scene.
  ///
  /// Thread-safety: callers may invoke check() concurrently on
  /// *different* DirectCollisionQueryEngine instances without
  /// synchronization. Concurrent calls on the *same* instance serialize
  /// on the engine's internal mutex.
  ///
  /// Unsupported required capabilities surface as
  /// `engine_status = kDiagnosticIncomplete` and `decision =
  /// kInconclusiveFail`, never as a false `kPass`.
  core::CollisionResult check(const core::CollisionQuery& query);

  /// Engine identity for traceability + mismatch records. Stable for
  /// the lifetime of the engine.
  const std::string& engine_id() const { return engine_id_; }
  const std::string& engine_version() const { return engine_version_; }

  /// Test helper: returns a stable pointer to this engine's internal
  /// roboplan::Scene. Used by unit tests that need to assert two engine
  /// instances do NOT share a backing scene.
  const void* internal_scene_identity() const {
    return static_cast<const void*>(scene_.get());
  }

 private:
  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::string engine_id_;
  std::string engine_version_;
  /// Internal mutex serializes concurrent check() calls on the same
  /// instance. Separate instances do not share this mutex.
  mutable std::mutex scene_mutex_;
};

}  // namespace manyforge::roboplan_backend
