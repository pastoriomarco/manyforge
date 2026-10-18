#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "roboplan_worker_protocol.hpp"

namespace manyforge::roboplan_backend {
namespace {

using worker_protocol::Frame;
using worker_protocol::MessageType;

constexpr std::size_t kHeaderBytes = 16U;

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

void constrain_to_parent(int argc, char **argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--parent-pid") {
    fail("usage: fake_process_candidate_worker --parent-pid PID");
  }
  pid_t expected_parent{};
  const std::string_view encoded(argv[2]);
  const auto parsed = std::from_chars(
      encoded.data(), encoded.data() + encoded.size(), expected_parent);
  if (encoded.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != encoded.data() + encoded.size() || expected_parent <= 0) {
    fail("fake worker parent PID is malformed");
  }
  if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
    fail("fake worker could not set its parent-death signal");
  }
  if (::getppid() != expected_parent) {
    fail("parent exited before fake worker containment was established");
  }
}

void read_all(int descriptor, void *output, std::size_t size) {
  auto *bytes = static_cast<char *>(output);
  std::size_t offset = 0U;
  while (offset < size) {
    const auto count = ::read(descriptor, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail("fake worker read failed");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void write_all(int descriptor, const void *input, std::size_t size) {
  const auto *bytes = static_cast<const char *>(input);
  std::size_t offset = 0U;
  while (offset < size) {
    const auto count =
        ::send(descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail("fake worker write failed");
    }
    offset += static_cast<std::size_t>(count);
  }
}

[[nodiscard]] std::uint16_t
u16(const std::array<unsigned char, kHeaderBytes> &header, std::size_t offset) {
  return static_cast<std::uint16_t>(header[offset]) |
         (static_cast<std::uint16_t>(header[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t
u32(const std::array<unsigned char, kHeaderBytes> &header, std::size_t offset) {
  std::uint32_t value = 0U;
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    value |= static_cast<std::uint32_t>(header[offset++]) << shift;
  }
  return value;
}

[[nodiscard]] std::uint64_t
u64(const std::array<unsigned char, kHeaderBytes> &header, std::size_t offset) {
  std::uint64_t value = 0U;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(header[offset++]) << shift;
  }
  return value;
}

void set_u16(std::array<unsigned char, kHeaderBytes> &header,
             std::size_t offset, std::uint16_t value) {
  header[offset] = static_cast<unsigned char>(value & 0xffU);
  header[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

template <typename Value>
void set_integer(std::array<unsigned char, kHeaderBytes> &header,
                 std::size_t offset, Value value) {
  for (unsigned shift = 0U; shift < sizeof(Value) * 8U; shift += 8U) {
    header[offset++] = static_cast<unsigned char>((value >> shift) & 0xffU);
  }
}

[[nodiscard]] Frame receive_frame(int descriptor) {
  std::array<unsigned char, kHeaderBytes> header{};
  read_all(descriptor, header.data(), header.size());
  if (u32(header, 0U) != worker_protocol::kFrameMagic ||
      u16(header, 4U) != worker_protocol::kProtocolVersion) {
    fail("fake worker received malformed frame");
  }
  const auto size = u64(header, 8U);
  if (size > 16U * 1024U * 1024U) {
    fail("fake worker payload is too large");
  }
  std::string payload(static_cast<std::size_t>(size), '\0');
  if (!payload.empty()) {
    read_all(descriptor, payload.data(), payload.size());
  }
  return {static_cast<MessageType>(u16(header, 6U)), std::move(payload)};
}

void send_frame(int descriptor, MessageType type, std::string_view payload,
                std::uint16_t version = worker_protocol::kProtocolVersion) {
  std::array<unsigned char, kHeaderBytes> header{};
  set_integer(header, 0U, worker_protocol::kFrameMagic);
  set_u16(header, 4U, version);
  set_u16(header, 6U, static_cast<std::uint16_t>(type));
  set_integer(header, 8U, static_cast<std::uint64_t>(payload.size()));
  write_all(descriptor, header.data(), header.size());
  if (!payload.empty()) {
    write_all(descriptor, payload.data(), payload.size());
  }
}

[[nodiscard]] std::vector<double>
angular_positions(const core::ControlledStateDTO &state) {
  std::vector<double> result;
  result.reserve(state.positions.size());
  for (const auto &position : state.positions) {
    const auto *angular = std::get_if<core::AngularPositionDTO>(&position);
    if (angular == nullptr) {
      fail("fake worker requires angular state");
    }
    result.push_back(angular->value_rad);
  }
  return result;
}

[[nodiscard]] core::TrajectoryDTO
trajectory(const RoboPlanCandidateRequestDTO &request) {
  const auto *goal = std::get_if<core::ResolvedJointGoalDTO>(&request.motion);
  if (goal == nullptr) {
    fail("fake worker requires a joint goal");
  }
  auto start = angular_positions(request.start_state);
  if (request.resources.size() != start.size() ||
      request.start_state.joint_names.size() != start.size() ||
      goal->resources.size() != goal->joint_names.size() ||
      goal->resources.size() != goal->positions_rad.size()) {
    fail("fake worker received inconsistent joint dimensions");
  }
  auto target = start;
  for (std::size_t goal_index = 0U; goal_index < goal->resources.size();
       ++goal_index) {
    const auto found =
        std::find(request.resources.begin(), request.resources.end(),
                  goal->resources[goal_index]);
    if (found == request.resources.end()) {
      fail("fake worker goal names an unbound resource");
    }
    const auto index = static_cast<std::size_t>(
        std::distance(request.resources.begin(), found));
    if (request.start_state.joint_names[index] !=
        goal->joint_names[goal_index]) {
      fail("fake worker goal resource and compiled joint disagree");
    }
    target[index] = goal->positions_rad[goal_index];
  }
  const std::vector<double> zeros(start.size(), 0.0);
  return core::TrajectoryDTO{
      request.resources,
      request.start_state.joint_names,
      {core::TrajectoryWaypointDTO{0.0, std::move(start), zeros, zeros},
       core::TrajectoryWaypointDTO{1.0, std::move(target), zeros, zeros}},
      // This no-jerk fixture retains the ordinary native scale slew limits;
      // it grants no extra Stop speed or coordinated-timing certificate.
      core::TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0}};
}

int run() {
  const int descriptor = worker_protocol::kProtocolDescriptor;
  const auto initialize = receive_frame(descriptor);
  if (initialize.type != MessageType::kInitialize) {
    fail("fake worker did not receive initialization");
  }
  const auto input = worker_protocol::decode_initialization(initialize.payload);
  constexpr std::string_view kCrashOncePrefix = "crash-once:";
  constexpr std::string_view kCountGenerationsPrefix = "count-generations:";
  const bool crash_once =
      input.model.native_scene_name.starts_with(kCrashOncePrefix);
  const bool count_generations =
      input.model.native_scene_name.starts_with(kCountGenerationsPrefix);
  const std::filesystem::path generation_counter(
      count_generations
          ? input.model.native_scene_name.substr(kCountGenerationsPrefix.size())
          : std::string{});
  if (count_generations) {
    if (input.model.geometries.size() != 1U ||
        input.model.allowed_collision_pairs.size() != 1U ||
        input.model.geometries.front().compiled_parent_frame != "base" ||
        !std::holds_alternative<core::BoxShapeDTO>(
            input.model.geometries.front().shape)) {
      fail("fake worker received the wrong retained projection");
    }
    const auto &pair = input.model.allowed_collision_pairs.front();
    const auto &geometry_name =
        input.model.geometries.front().native_geometry_name;
    if (!((pair.first == "forearm_link" && pair.second == geometry_name) ||
          (pair.first == geometry_name && pair.second == "forearm_link"))) {
      fail("fake worker received the wrong retained collision policy");
    }
    const int counter = ::open(generation_counter.c_str(),
                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (counter < 0) {
      fail("fake worker could not open its generation counter");
    }
    constexpr char kGeneration = 'g';
    const auto written = ::write(counter, &kGeneration, 1U);
    const auto close_result = ::close(counter);
    if (written != 1 || close_result != 0) {
      fail("fake worker could not record its generation");
    }
  }
  const char *configured_marker = std::getenv("MANYFORGE_FAKE_WORKER_MARKER");
  std::filesystem::path first_generation_marker;
  if (configured_marker != nullptr && configured_marker[0] != '\0') {
    first_generation_marker = configured_marker;
  } else if (count_generations) {
    first_generation_marker = generation_counter;
  } else if (crash_once) {
    first_generation_marker =
        input.model.native_scene_name.substr(kCrashOncePrefix.size());
  } else {
    first_generation_marker = input.model.native_scene_name;
  }
  send_frame(descriptor, MessageType::kReady, {},
             input.model.native_scene_name == "old-protocol"
                 ? worker_protocol::kProtocolVersion - 1U
                 : worker_protocol::kProtocolVersion);

  for (;;) {
    const auto frame = receive_frame(descriptor);
    if (frame.type == MessageType::kShutdown) {
      return 0;
    }
    if (frame.type != MessageType::kPlan) {
      fail("fake worker received an unsupported request");
    }
    const int marker = ::open(first_generation_marker.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (marker >= 0) {
      static_cast<void>(::close(marker));
      if (crash_once) {
        return 42;
      }
      const char *failure_gate =
          std::getenv("MANYFORGE_FAKE_WORKER_FAILURE_GATE");
      if (failure_gate != nullptr && failure_gate[0] != '\0') {
        // Test-only explicit release: exercise the real owner's asynchronous
        // planning failure without racing the initial Prepare return.
        const int gate =
            ::open(failure_gate, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (gate < 0)
          fail("fake worker could not open failure gate");
        pollfd ready{gate, POLLIN, 0};
        char release{};
        const bool released =
            ::poll(&ready, 1, 5000) == 1 && ::read(gate, &release, 1) == 1;
        static_cast<void>(::close(gate));
        if (!released)
          fail("fake worker failure gate was not released");
        send_frame(descriptor, MessageType::kPlanResult,
                   worker_protocol::encode_trajectory_result(
                       core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
                           "planning_failed",
                           "injected gated native planner failure",
                           {{"reason", "test_planner_failure"}}})));
        continue;
      }
      for (;;) {
        static_cast<void>(::pause());
      }
    }
    if (errno != EEXIST) {
      fail("fake worker could not create generation marker");
    }
    const auto request =
        worker_protocol::decode_candidate_request(frame.payload);
    send_frame(descriptor, MessageType::kPlanResult,
               worker_protocol::encode_trajectory_result(
                   core::Result<core::TrajectoryDTO>::Ok(trajectory(request))));
  }
}

} // namespace
} // namespace manyforge::roboplan_backend

int main(int argc, char **argv) {
  try {
    manyforge::roboplan_backend::constrain_to_parent(argc, argv);
    return manyforge::roboplan_backend::run();
  } catch (...) {
    return 2;
  }
}
