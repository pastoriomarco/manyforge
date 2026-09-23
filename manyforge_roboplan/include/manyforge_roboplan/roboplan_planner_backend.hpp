#pragma once

#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_roboplan/roboplan_candidate_computer.hpp>
#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {

struct RoboPlanCandidateProjectionDTO final {
  core::ProjectionLeaseBindingDTO projection_binding;
  core::CollisionWorldSnapshotPtr world;
  std::shared_ptr<const core::CollisionPolicySnapshot> policy;
};

/// Private candidate-computation transport used by the addressed planner
/// adapter. The production implementation is a supervised worker process;
/// focused tests may provide an in-memory implementation. No public caller can
/// select a model, generated group, or worker through this surface.
class IRoboPlanCandidateClient {
 public:
  virtual ~IRoboPlanCandidateClient() = default;

  [[nodiscard]] virtual const RoboPlanCandidateBindingDTO& binding() const noexcept = 0;

  [[nodiscard]] virtual core::Result<core::TrajectoryDTO> compute(
      const RoboPlanCandidateRequestDTO& request) = 0;

  [[nodiscard]] virtual core::Result<std::vector<double>> solve_ik(
      const core::Pose3DTO& goal, const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame, const core::ControlledStateDTO& seed_state,
      double timeout_s) = 0;

  /// True only for production clients whose worker scene is selected from the
  /// exact Kernel-retained projection before planning or IK.
  [[nodiscard]] virtual bool requires_retained_projection() const noexcept { return false; }

  /// Select one immutable retained projection. Fixed-model test clients keep
  /// this no-op; a client reporting `true` above must replace its worker
  /// generation whenever the selected identity changes.
  [[nodiscard]] virtual core::Result<void> select_retained_projection(
      const RoboPlanCandidateProjectionDTO&) {
    return core::Result<void>::Ok();
  }

  /// Non-blocking cancellation of only the currently active request.
  virtual void request_cancel() noexcept = 0;
};

struct RoboPlanPlannerBackendConfig final {
  /// Used only when IkRequestDTO::timeout_s is <= 0.
  double default_single_shot_ik_timeout_s{0.05};
};

/// Resource-addressed IPlannerBackend adapter for RoboPlan 0.7.
///
/// This boundary validates the complete Kernel-issued participant/projection
/// identity, strips it to the exact private candidate contract, and restores
/// only portable resources and frame identities in the result. Opaque native
/// planning runs behind IRoboPlanCandidateClient so Stop can terminate a
/// wedged worker without depending on RoboPlan cooperative cancellation.
class RoboPlanPlannerBackend final : public core::IPlannerBackend {
 public:
  RoboPlanPlannerBackend(core::ProjectionLeaseBindingDTO projection_binding,
                         RoboPlanCandidateBindingDTO candidate_binding,
                         std::unique_ptr<IRoboPlanCandidateClient> candidate_client,
                         RoboPlanPlannerBackendConfig config = {});

  RoboPlanPlannerBackend(const RoboPlanPlannerBackend&) = delete;
  RoboPlanPlannerBackend& operator=(const RoboPlanPlannerBackend&) = delete;

  [[nodiscard]] core::PlanResultDTO plan(const core::PlanRequestDTO& request) override;
  void request_cancel() override;
  [[nodiscard]] core::IkResultDTO solve_ik(const core::IkRequestDTO& request) override;

  /// Construction-time structural/scene-lineage anchor. Individual planning
  /// calls carry their own exact current projection lease; the adapter must
  /// not freeze the logical-scene revision at process startup.
  [[nodiscard]] const core::ProjectionLeaseBindingDTO& projection_binding() const noexcept {
    return projection_binding_;
  }

  [[nodiscard]] const RoboPlanCandidateBindingDTO& candidate_binding() const noexcept {
    return candidate_binding_;
  }

 private:
  core::ProjectionLeaseBindingDTO projection_binding_;
  RoboPlanCandidateBindingDTO candidate_binding_;
  std::unique_ptr<IRoboPlanCandidateClient> candidate_client_;
  RoboPlanPlannerBackendConfig config_;
};

}  // namespace manyforge::roboplan_backend
