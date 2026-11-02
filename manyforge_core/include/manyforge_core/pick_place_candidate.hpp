#pragma once

#include <array>
#include <cstddef>
#include <manyforge_core/ports/iaddressed_motion_preview.hpp>
#include <manyforge_core/ports/iscene_candidate_preview.hpp>
#include <manyforge_core/ports/iscene_candidate_projection.hpp>
#include <manyforge_core/types/motion_preview.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

enum class PickCandidateSearchStrategyDTO {
  kFirstFeasible,
  kBestCost,
};

struct PickOffsetEnvelopeDTO final {
  std::array<double, 3U> position_min_m{};
  std::array<double, 3U> position_max_m{};
  std::array<double, 3U> orientation_rpy_min_deg{};
  std::array<double, 3U> orientation_rpy_max_deg{};
};

/// Translation basis for one portable offset envelope. A resolved basis is
/// captured from immutable Program/frame evidence before search; the
/// candidate-TCP form intentionally follows each synthesized TCP heading.
struct PickOffsetBasisDTO final {
  bool use_candidate_tcp_orientation{};
  QuaternionDTO resolved_orientation_in_base{};
};

struct PickCandidateSynthesisConfigDTO final {
  PickOffsetEnvelopeDTO pick_offset_envelope{};
  PickOffsetEnvelopeDTO approach_offset_envelope{{0.0, 0.0, 0.05}, {0.0, 0.0, 0.1}, {}, {}};
  PickOffsetBasisDTO pick_offset_basis{};
  PickOffsetBasisDTO approach_offset_basis{};
  std::array<double, 3U> base_tool_orientation_rpy_deg{180.0, 0.0, 0.0};
  double translation_step_m{0.01};
  double rotation_step_deg{15.0};
  bool force_vertical_pick{};
  std::optional<double> min_tcp_z_m;
  std::size_t maximum_candidate_count{4096U};
  PickCandidateSearchStrategyDTO search_strategy{PickCandidateSearchStrategyDTO::kFirstFeasible};
};

struct PickPoseCandidateDTO final {
  std::size_t candidate_index{};
  Pose3DTO approach_pose;
  Pose3DTO pick_pose;
  Pose3DTO retreat_pose;
  CartesianPathDTO descend_path;
  CartesianPathDTO retreat_path;
  bool min_tcp_z_applied{};
};

/// Bounded deterministic grid translated from the maintained pre-plan pick
/// semantics. It stores the bounded pick and approach offset sets and
/// materializes only one pair at a time, avoiding a full Cartesian-product
/// candidate array.
class PickCandidateGrid final {
 public:
  struct OffsetSample final {
    std::array<double, 3U> position;
    std::array<double, 3U> orientation_rpy_deg;
  };

  [[nodiscard]] static Result<PickCandidateGrid> Create(
      SceneObjectSnapshotDTO selected_object, Pose3DTO object_pose_in_motion_base,
      PickCandidateSynthesisConfigDTO config, CartesianPathDTO descend_path_template = {},
      CartesianPathDTO retreat_path_template = {});

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] Result<PickPoseCandidateDTO> candidate(std::size_t zero_based_index) const;
  [[nodiscard]] const SceneResourceId& selected_object_id() const noexcept {
    return selected_object_.scene_resource_id;
  }
  [[nodiscard]] PickCandidateSearchStrategyDTO search_strategy() const noexcept {
    return config_.search_strategy;
  }

 private:
  PickCandidateGrid(SceneObjectSnapshotDTO selected_object, Pose3DTO object_pose_in_motion_base,
                    PickCandidateSynthesisConfigDTO config, CartesianPathDTO descend_path_template,
                    CartesianPathDTO retreat_path_template, std::vector<OffsetSample> pick_offsets,
                    std::vector<OffsetSample> approach_offsets);

  SceneObjectSnapshotDTO selected_object_;
  Pose3DTO object_pose_in_motion_base_;
  PickCandidateSynthesisConfigDTO config_;
  CartesianPathDTO descend_path_template_;
  CartesianPathDTO retreat_path_template_;
  std::vector<OffsetSample> pick_offsets_;
  std::vector<OffsetSample> approach_offsets_;
};

enum class PickCandidateStageDTO {
  kApproach,
  kDescend,
  kCandidateAttach,
  kCandidateDetach,
  kRetreat,
};

struct PickCandidateStageReportDTO final {
  PickCandidateStageDTO stage{PickCandidateStageDTO::kApproach};
  bool succeeded{};
  std::optional<ErrorDTO> error;
  double trajectory_duration_s{};
  std::size_t trajectory_waypoint_count{};
};

struct PickCandidateReportDTO final {
  std::size_t candidate_index{};
  std::vector<PickCandidateStageReportDTO> stages;
};

struct PlannedPickCandidateDTO final {
  PickPoseCandidateDTO candidate;
  MotionPreviewResultDTO approach_preview;
  MotionPreviewResultDTO descend_preview;
  SceneTransactionRequestDTO attach_request;
  SceneCandidatePreviewDTO attached_scene;
  MotionPreviewResultDTO retreat_preview;
  double planning_score{};
};

struct PickCandidateSearchOutcomeDTO final {
  std::optional<PlannedPickCandidateDTO> selected;
  std::size_t attempted_candidate_count{};
  std::vector<PickCandidateReportDTO> reports;
  std::optional<PickPoseCandidateDTO> last_attempted_candidate;
  bool stopped_on_invariant_failure{};
};

struct PickCandidatePlanningRequestDTO final {
  PickCandidateGrid grid;
  OperationIntentDTO approach_operation;
  OperationIntentDTO descend_operation;
  OperationIntentDTO retreat_operation;
  RobotStateDTO start_state;
  CollisionWorldLeaseInventory predecessor_projection;
  ResolvedSceneCandidatePreviewRequestDTO attach_preview_request;
  TargetCarrierSceneFrameRefDTO carrier_frame_selector;
  std::string candidate_lease_scope;
};

/// Owner-private pick candidate loop. It shares Kernel planning and
/// validation, then uses the scene owner's reducer and the exact native
/// projector for the carried retreat. It never dispatches or commits effects.
class AddressedPickCandidatePlanner final {
 public:
  AddressedPickCandidatePlanner(IAddressedMotionPreviewSession& motion,
                                ISceneCandidatePreview& scene,
                                ISceneCandidateProjection& projection)
      : motion_(motion), scene_(scene), projection_(projection) {}

  [[nodiscard]] Result<PickCandidateSearchOutcomeDTO> Search(
      PickCandidatePlanningRequestDTO request, std::stop_token stop_token = {});

 private:
  IAddressedMotionPreviewSession& motion_;
  ISceneCandidatePreview& scene_;
  ISceneCandidateProjection& projection_;
};

struct PlacePlanningRequestDTO final {
  SceneResourceId selected_object_id;
  OperationIntentDTO approach_operation;
  OperationIntentDTO descend_operation;
  OperationIntentDTO retreat_operation;
  RobotStateDTO start_state;
  CollisionWorldLeaseInventory predecessor_projection;
  ResolvedSceneCandidatePreviewRequestDTO detach_preview_request;
  TargetCarrierSceneFrameRefDTO carrier_frame_selector;
  std::string candidate_lease_scope;
};

struct PlannedPlaceDTO final {
  MotionSpecDTO approach_motion;
  MotionSpecDTO descend_motion;
  MotionSpecDTO retreat_motion;
  MotionPreviewResultDTO approach_preview;
  MotionPreviewResultDTO descend_preview;
  SceneTransactionRequestDTO detach_request;
  SceneCandidatePreviewDTO detached_scene;
  MotionPreviewResultDTO retreat_preview;
};

struct PlacePlanningOutcomeDTO final {
  std::optional<PlannedPlaceDTO> selected;
  PickCandidateReportDTO report;
  bool stopped_on_invariant_failure{};
};

/// Deterministic inverse of the pick phase model. The object remains carried
/// through approach/descend, is detached only in an owner-private successor at
/// the predicted release state, and retreats against that detached successor.
class AddressedPlacePlanner final {
 public:
  AddressedPlacePlanner(IAddressedMotionPreviewSession& motion,
                        ISceneCandidatePreview& scene,
                        ISceneCandidateProjection& projection)
      : motion_(motion), scene_(scene), projection_(projection) {}

  [[nodiscard]] Result<PlacePlanningOutcomeDTO> Plan(PlacePlanningRequestDTO request,
                                                     std::stop_token stop_token = {});

 private:
  IAddressedMotionPreviewSession& motion_;
  ISceneCandidatePreview& scene_;
  ISceneCandidateProjection& projection_;
};

[[nodiscard]] constexpr std::string_view to_string(PickCandidateStageDTO stage) noexcept {
  switch (stage) {
    case PickCandidateStageDTO::kApproach:
      return "approach";
    case PickCandidateStageDTO::kDescend:
      return "descend";
    case PickCandidateStageDTO::kCandidateAttach:
      return "candidate_attach";
    case PickCandidateStageDTO::kCandidateDetach:
      return "candidate_detach";
    case PickCandidateStageDTO::kRetreat:
      return "retreat";
  }
  return {};
}

}  // namespace manyforge::core
