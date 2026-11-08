#pragma once

#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_roboplan/roboplan_candidate_computer.hpp>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace manyforge::roboplan_backend {

struct RoboPlanWorkerModelDTO;

struct RoboPlanCandidateProjectionDTO final {
  core::ProjectionLeaseBindingDTO projection_binding;
  core::CollisionWorldSnapshotPtr world;
  std::shared_ptr<const core::CollisionPolicySnapshot> policy;
  /// Planning-only allowance for this operation's resolved grasp pairs.
  /// Coordinator and runtime contact constraints remain authoritative.
  std::optional<std::string> contact_policy_id{};
  /// The Kernel's clearance margins for this operation. They do not change
  /// the selected worker generation; they are sent with each plan request.
  std::shared_ptr<const core::CollisionClearanceMargins> clearance_margins{};
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

  // The optional qualified target is internal phase evidence for preview only.
  [[nodiscard]] virtual core::Result<core::TrajectoryDTO> compute_with_target(
      const RoboPlanCandidateRequestDTO& request,
      std::optional<core::ControlledStateDTO>* target) {
    if (target) target->reset();
    if (request.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled)
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          "unsupported_capability", "Diagnostic planning is unavailable", {}});
    return compute(request);
  }

  [[nodiscard]] virtual core::Result<std::vector<double>> solve_ik(
      const core::Pose3DTO& goal, const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame, const core::ControlledStateDTO& seed_state,
      double timeout_s) = 0;

  [[nodiscard]] virtual core::Result<std::vector<double>> solve_ik_diagnostic(
      const core::Pose3DTO&, const core::ResolvedFrameDTO&,
      const core::ResolvedFrameDTO&, const core::ControlledStateDTO&, double,
      const std::optional<core::TolerancesDTO>&) {
    return core::Result<std::vector<double>>::Err(core::ErrorDTO{
        "unsupported_capability", "Diagnostic IK is unavailable", {}});
  }

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

  /// Replace the cell planning model a retained-projection worker is built
  /// from. The worker is retired and the projection selection cleared, so no
  /// later request can plan before its exact projection is reselected against
  /// the replacement model. Fixed-model clients have no replaceable model.
  [[nodiscard]] virtual core::Result<void> replace_retained_model(
      const RoboPlanWorkerModelDTO& model);

  /// Replace the planner settings (seed, collision-check step; cell context)
  /// a retained-projection worker is started with. A worker started with
  /// other settings is retired; the next request starts one with these.
  /// Fixed test clients keep their own.
  [[nodiscard]] virtual core::Result<void> replace_retained_settings(
      const RoboPlanCandidateComputerConfig&) {
    return core::Result<void>::Ok();
  }

  /// Non-blocking cancellation of only the currently active request.
  virtual void request_cancel() noexcept = 0;
};

struct RoboPlanPlannerBackendConfig final {
  /// Used only when IkRequestDTO::timeout_s is <= 0.
  double default_single_shot_ik_timeout_s{0.05};
};

/// Resource-addressed IPlannerBackend adapter for RoboPlan 0.8.
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
  [[nodiscard]] core::ProjectionLeaseBindingDTO projection_binding() const {
    std::shared_lock lock(projection_mutex_);
    return projection_binding_;
  }

  /// Re-anchor an unchanged planner/controller to the candidate Scene product
  /// under the Manager's Apply fence. An in-flight plan/IK query finishes on
  /// its predecessor lease before the new lease becomes visible.
  [[nodiscard]] core::Result<void> RebindProjection(
      const core::ProjectionLeaseBindingDTO& expected_predecessor,
      core::ProjectionLeaseBindingDTO candidate);

  /// Replace the cell planning model of this unchanged planner under the
  /// Manager's Apply fence, when a retained controller's candidate product
  /// carries another cell planning model. An in-flight plan/IK query finishes
  /// on its predecessor model before the replacement becomes visible.
  [[nodiscard]] core::Result<void> ReplaceRetainedModel(
      const RoboPlanWorkerModelDTO& model);

  /// Replace the planner settings of this unchanged planner under the
  /// Manager's Apply fence (they are cell context: a retained component
  /// takes the candidate's). An in-flight query finishes on the old ones.
  [[nodiscard]] core::Result<void> ReplaceRetainedSettings(
      const RoboPlanCandidateComputerConfig& settings);

  [[nodiscard]] const RoboPlanCandidateBindingDTO& candidate_binding() const noexcept {
    return candidate_binding_;
  }

 private:
  mutable std::shared_mutex projection_mutex_;
  core::ProjectionLeaseBindingDTO projection_binding_;
  RoboPlanCandidateBindingDTO candidate_binding_;
  std::unique_ptr<IRoboPlanCandidateClient> candidate_client_;
  RoboPlanPlannerBackendConfig config_;
};

}  // namespace manyforge::roboplan_backend
