#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <manyforge_core/cell_model_state.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <manyforge_roboplan/roboplan_scene_factory.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// One already-resolved geometry row for a worker-owned projection. Logical
/// frame/resource resolution occurs before this private adapter boundary.
struct RoboPlanWorkerGeometryDTO final {
  std::string native_geometry_name;
  std::string compiled_parent_frame;
  core::CollisionShapeDTO shape;
  core::Pose3DTO pose_in_parent;
  /// Present only for a model-local asset:// MeshShapeDTO. This is a
  /// Manager-private, verified, staged file; public scene callers never
  /// provide a filesystem path. A Program scene MeshShapeDTO instead carries
  /// its captured `content_digest` (`sha256:<hex>`) in the shape: the worker
  /// reads `raw-artifacts/sha256/<hex>.blob` below its scene content root
  /// descriptor, verifies the bytes against it, and never opens the URI.
  std::optional<std::filesystem::path> resolved_mesh_path;
};

struct RoboPlanWorkerModelDTO final {
  std::string native_scene_name;
  std::filesystem::path urdf_path;
  std::filesystem::path srdf_path;
  std::vector<std::filesystem::path> package_paths;
  std::vector<RoboPlanJointLimitInput> joint_limits;
  std::vector<RoboPlanWorkerGeometryDTO> geometries;
  /// Native body-name pairs disabled only after collision-policy resolution.
  std::vector<std::pair<std::string, std::string>> allowed_collision_pairs;
};

struct ProcessContainedCandidateClientConfig final {
  std::filesystem::path worker_executable;
  std::chrono::milliseconds startup_timeout{10'000};
  std::chrono::milliseconds request_transport_grace{250};
  std::chrono::milliseconds shutdown_timeout{500};
  std::size_t maximum_request_bytes{4U * 1024U * 1024U};
  std::size_t maximum_response_bytes{256U * 1024U * 1024U};
  /// Production mode: rebuild the worker-owned scene from each exact retained
  /// projection generation selected by the Kernel. Fixed-model tests leave
  /// this false.
  bool require_retained_projection{false};
  /// Directory descriptor of the activation root (Manager-opened, separate
  /// from the retained package root): the scene content root. Program scene
  /// meshes are read only from its raw-artifact store by their captured
  /// digest, inside the worker, which receives a duplicate as its reserved
  /// FD 9. Unset: Program scene meshes fail closed. Model-local asset://
  /// values continue to resolve only through the retained planning-model
  /// stage.
  std::optional<int> scene_content_root_fd;
  /// When set, this inherited directory descriptor is the retained package
  /// root of a managed child. The worker receives it as FD 6; it is never a
  /// scene-mesh root.
  std::optional<int> retained_package_root_fd;
  /// The cell's observed-state sources. Every plan and IK request carries the
  /// positions of the cell model's coordinates outside this planner's group
  /// as observed at that moment, read with `clock` (the clock the samples
  /// are stamped with). A request is refused with that component's stream
  /// condition when its state is stale or absent. Both unset: the worker
  /// keeps those coordinates as its scene has them.
  std::shared_ptr<const core::CellModelStateHandle> cell_model_state{};
  std::shared_ptr<const core::IClock> clock{};
};

/// Long-lived RoboPlan worker client.
///
/// The child is created with posix_spawn/exec, owns one exact scene, and
/// processes one request at a time. request_cancel() publishes no callback and
/// takes no planner mutex: it sends SIGKILL through a request-pinned pidfd.
/// The next request recreates the same exact worker generation.
class ProcessContainedRoboPlanCandidateClient final : public IRoboPlanCandidateClient {
 public:
  ProcessContainedRoboPlanCandidateClient(RoboPlanWorkerModelDTO model,
                                          RoboPlanCandidateBindingDTO binding,
                                          RoboPlanCandidateComputerConfig candidate_config,
                                          ProcessContainedCandidateClientConfig config);
  ~ProcessContainedRoboPlanCandidateClient() override;

  ProcessContainedRoboPlanCandidateClient(const ProcessContainedRoboPlanCandidateClient&) = delete;
  ProcessContainedRoboPlanCandidateClient& operator=(
      const ProcessContainedRoboPlanCandidateClient&) = delete;

  [[nodiscard]] const RoboPlanCandidateBindingDTO& binding() const noexcept override;
  [[nodiscard]] core::Result<core::TrajectoryDTO> compute(
      const RoboPlanCandidateRequestDTO& request) override;
  [[nodiscard]] core::Result<core::TrajectoryDTO> compute_with_target(
      const RoboPlanCandidateRequestDTO& request,
      std::optional<core::ControlledStateDTO>* target) override;
  [[nodiscard]] core::Result<std::vector<double>> solve_ik(
      const core::Pose3DTO& goal, const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame, const core::ControlledStateDTO& seed_state,
      double timeout_s) override;
  [[nodiscard]] core::Result<std::vector<double>> solve_ik_diagnostic(
      const core::Pose3DTO& goal, const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame, const core::ControlledStateDTO& seed_state,
      double timeout_s, const std::optional<core::TolerancesDTO>& tolerances) override;
  [[nodiscard]] bool requires_retained_projection() const noexcept override;
  [[nodiscard]] core::Result<void> select_retained_projection(
      const RoboPlanCandidateProjectionDTO& projection) override;
  [[nodiscard]] core::Result<void> replace_retained_model(
      const RoboPlanWorkerModelDTO& model) override;
  [[nodiscard]] core::Result<void> replace_retained_settings(
      const RoboPlanCandidateComputerConfig& settings) override;
  void request_cancel() noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::roboplan_backend
