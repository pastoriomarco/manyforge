#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_roboplan/process_contained_candidate_client.hpp>

namespace manyforge::roboplan_backend::worker_protocol {

inline constexpr std::uint32_t kFrameMagic = 0x4d465750U;  // MFWP
inline constexpr std::uint16_t kProtocolVersion = 7U;
inline constexpr int kProtocolDescriptor = 3;

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
};

[[nodiscard]] std::string encode_initialization(
    const WorkerInitializationDTO& value);
[[nodiscard]] WorkerInitializationDTO decode_initialization(
    std::string_view bytes);

[[nodiscard]] std::string encode_candidate_request(
    const RoboPlanCandidateRequestDTO& value);
[[nodiscard]] RoboPlanCandidateRequestDTO decode_candidate_request(
    std::string_view bytes);

[[nodiscard]] std::string encode_trajectory_result(
    const core::Result<core::TrajectoryDTO>& value);
[[nodiscard]] core::Result<core::TrajectoryDTO> decode_trajectory_result(
    std::string_view bytes);

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
