#pragma once

#include <chrono>
#include <filesystem>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
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
  /// Required only for MeshShapeDTO. This is a Manager-private, verified,
  /// staged file; public scene callers never provide a filesystem path.
  std::optional<std::filesystem::path> resolved_mesh_path;
};

struct RoboPlanWorkerModelDTO final {
  std::string native_scene_name;
  std::filesystem::path urdf_path;
  std::filesystem::path srdf_path;
  std::vector<std::filesystem::path> package_paths;
  std::optional<std::filesystem::path> yaml_config_path;
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
  [[nodiscard]] core::Result<std::vector<double>> solve_ik(
      const core::Pose3DTO& goal, const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame, const core::ControlledStateDTO& seed_state,
      double timeout_s) override;
  [[nodiscard]] bool requires_retained_projection() const noexcept override;
  [[nodiscard]] core::Result<void> select_retained_projection(
      const RoboPlanCandidateProjectionDTO& projection) override;
  void request_cancel() noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::roboplan_backend
