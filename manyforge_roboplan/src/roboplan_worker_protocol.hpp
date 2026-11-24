#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_roboplan/process_contained_candidate_client.hpp>

namespace manyforge::roboplan_backend::worker_protocol {

inline constexpr std::uint32_t kFrameMagic = 0x4d465750U;  // MFWP
/// v16: scene meshes are read by their captured content digest from the
/// scene content root (OD-335). v17: the mesh shape carries that digest
/// (its one carrier); the geometry row no longer does.
inline constexpr std::uint16_t kProtocolVersion = 17U;
inline constexpr int kProtocolDescriptor = 3;
/// Worker-reserved duplicate of the scene content (activation) root directory.
inline constexpr int kSceneContentRootDescriptor = 9;
/// Prefix of the protocol-error detail of a worker that could not read a
/// captured scene mesh (missing blob or bytes differing from the digest).
inline constexpr std::string_view kSceneMeshUnavailableDetailPrefix =
    "scene_mesh_unavailable: ";

enum class MessageType : std::uint16_t {
  kInitialize = 1U,
  kReady = 2U,
  kPlan = 3U,
  kPlanResult = 4U,
  kIk = 5U,
  kIkResult = 6U,
  kShutdown = 7U,
  kProtocolError = 8U,
};

struct Frame final {
  MessageType type;
  std::string payload;
};

struct WorkerInitializationDTO final {
  RoboPlanWorkerModelDTO model;
  RoboPlanCandidateBindingDTO binding;
  RoboPlanCandidateComputerConfig candidate_config;
};

struct WorkerIkRequestDTO final {
  core::Pose3DTO goal;
  core::ResolvedFrameDTO base_frame;
  core::ResolvedFrameDTO tcp_frame;
  core::ControlledStateDTO seed_state;
  double timeout_s{};
  /// Native body-name pairs (`first < second`) the move's contact rules
  /// allow; the worker deactivates them for this solve.
  std::vector<std::pair<std::string, std::string>> contact_allowed_pairs{};
  core::PlannerCollisionMode collision_mode{core::PlannerCollisionMode::kEnforced};
  std::optional<std::int64_t> diagnostic_deadline_ns{};
  std::optional<core::TolerancesDTO> diagnostic_tolerances{};
  /// As RoboPlanCandidateRequestDTO::other_coordinates.
  RoboPlanOtherCoordinatesDTO other_coordinates{};
};

[[nodiscard]] std::string encode_initialization(
    const WorkerInitializationDTO& value);
[[nodiscard]] WorkerInitializationDTO decode_initialization(
    std::string_view bytes);

// Optional absolute cap is private same-host steady-clock transport metadata;
// it is omitted with the planning budget and never becomes planning authority.
[[nodiscard]] std::string encode_candidate_request(
    const RoboPlanCandidateRequestDTO& value,
    std::optional<std::int64_t> planning_deadline_ns = std::nullopt);
[[nodiscard]] RoboPlanCandidateRequestDTO decode_candidate_request(
    std::string_view bytes,
    std::optional<std::int64_t>* planning_deadline_ns = nullptr);

[[nodiscard]] std::string encode_trajectory_result(
    const core::Result<core::TrajectoryDTO>& value,
    const std::optional<core::ControlledStateDTO>& qualified_target = std::nullopt);
[[nodiscard]] core::Result<core::TrajectoryDTO> decode_trajectory_result(
    std::string_view bytes,
    std::optional<core::ControlledStateDTO>* qualified_target = nullptr);

[[nodiscard]] std::string encode_ik_request(
    const WorkerIkRequestDTO& value);
[[nodiscard]] WorkerIkRequestDTO decode_ik_request(std::string_view bytes);

[[nodiscard]] std::string encode_ik_result(
    const core::Result<std::vector<double>>& value);
[[nodiscard]] core::Result<std::vector<double>> decode_ik_result(
    std::string_view bytes);

[[nodiscard]] std::string encode_protocol_error(std::string_view detail);
[[nodiscard]] std::string decode_protocol_error(std::string_view bytes);

}  // namespace manyforge::roboplan_backend::worker_protocol
