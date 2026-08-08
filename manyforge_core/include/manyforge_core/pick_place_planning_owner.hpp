#pragma once

#include <array>
#include <cstddef>
#include <manyforge_core/pick_place_sequence.hpp>
#include <manyforge_core/ports/iaddressed_motion_preview.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/ports/iscene_candidate_preview.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxPickPlaceSequenceIdBytes = 96U;

/// Public, portable subset of deterministic candidate synthesis. Translation
/// offsets are expressed in the selected arm's logical planning-base frame,
/// unless the corresponding candidate-TCP flag is true. Resolved frame poses
/// never enter this caller-owned value.
struct PickCandidateSynthesisIntentDTO final {
  PickOffsetEnvelopeDTO pick_offset_envelope{};
  PickOffsetEnvelopeDTO approach_offset_envelope{{0.0, 0.0, 0.05},
                                                  {0.0, 0.0, 0.1}, {}, {}};
  bool pick_offset_uses_candidate_tcp{};
  bool approach_offset_uses_candidate_tcp{};
  std::array<double, 3U> base_tool_orientation_rpy_deg{180.0, 0.0,
                                                       0.0};
  double translation_step_m{0.01};
  double rotation_step_deg{15.0};
  bool force_vertical_pick{};
  std::optional<double> min_tcp_z_m;
  std::size_t maximum_candidate_count{4096U};
  PickCandidateSearchStrategyDTO search_strategy{
      PickCandidateSearchStrategyDTO::kFirstFeasible};
};

/// Caller-owned pick-planning intent. All four operations remain ordinary
/// public intents. The owner replaces only the three motion payloads with the
/// selected deterministic plan when it returns a sequence request.
struct PickSequencePlanningIntentDTO final {
  std::string sequence_id;
  SceneResourceId selected_object_id{std::string{}};
  RuntimeObjectId selected_runtime_object_id{std::string{}};
  FrameRoleId carrier_frame_role_id{std::string{}};
  PickCandidateSynthesisIntentDTO synthesis;
  OperationIntentDTO approach_operation;
  OperationIntentDTO descend_operation;
  OperationIntentDTO tool_operation;
  OperationIntentDTO retreat_operation;
};

/// Caller-owned inverse sequence. Place poses/paths are ordinary portable
/// motion payloads; the owner resolves the carried attachment and candidate
/// detach privately.
struct PlaceSequencePlanningIntentDTO final {
  std::string sequence_id;
  SceneResourceId selected_object_id{std::string{}};
  FrameRoleId carrier_frame_role_id{std::string{}};
  OperationIntentDTO approach_operation;
  OperationIntentDTO descend_operation;
  OperationIntentDTO tool_operation;
  OperationIntentDTO retreat_operation;
};

enum class PickPlacePlanningStatusDTO {
  kPlanned,
  kNoFeasibleCandidate,
};

struct PickPlacePlanningResultDTO final {
  PickPlacePlanningStatusDTO status{
      PickPlacePlanningStatusDTO::kNoFeasibleCandidate};
  SceneResourceId selected_object_id{std::string{}};
  std::size_t attempted_candidate_count{};
  std::vector<PickCandidateReportDTO> reports;
  std::optional<PickPlaceSequenceRequestDTO> sequence_request;
};

/// Stateless addressed planning owner. It resolves the exact arm/tool
/// Program Bindings, current scene projection, authoritative participant
/// state, carrier frame and grasp body roles before invoking the N5d-2
/// candidate and sequence owners. No method dispatches, journals, commits a
/// scene transition, or publishes runtime state.
class AddressedPickPlacePlanningOwner final {
 public:
  AddressedPickPlacePlanningOwner(
      IAddressedMotionPreviewOwner& motion,
      ISceneAuthority& scene,
      ISceneCandidatePreview& scene_candidates,
      IPlannerSceneProjection& projection)
      : motion_(motion),
        scene_(scene),
        scene_candidates_(scene_candidates),
        projection_(projection) {}

  [[nodiscard]] Result<PickPlacePlanningResultDTO> PlanPick(
      PickSequencePlanningIntentDTO intent,
      std::stop_token stop_token = {});

  [[nodiscard]] Result<PickPlacePlanningResultDTO> PlanPlace(
      PlaceSequencePlanningIntentDTO intent,
      std::stop_token stop_token = {});

 private:
  IAddressedMotionPreviewOwner& motion_;
  ISceneAuthority& scene_;
  ISceneCandidatePreview& scene_candidates_;
  IPlannerSceneProjection& projection_;
};

[[nodiscard]] constexpr std::string_view to_string(
    PickPlacePlanningStatusDTO status) noexcept {
  switch (status) {
    case PickPlacePlanningStatusDTO::kPlanned:
      return "planned";
    case PickPlacePlanningStatusDTO::kNoFeasibleCandidate:
      return "no_feasible_candidate";
  }
  return {};
}

}  // namespace manyforge::core
