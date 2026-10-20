#include "roboplan_worker_protocol.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <tl/expected.hpp>

#include "manyforge_roboplan/roboplan_scene_factory.hpp"
#include <manyforge_collision/scene_asset_root.hpp>

namespace manyforge::roboplan_backend {
namespace {

using worker_protocol::Frame;
using worker_protocol::MessageType;

constexpr std::size_t kHeaderBytes = 16U;
constexpr std::size_t kMaximumFrameBytes = 256U * 1024U * 1024U;

/// The loaded scene-mesh bytes differ from the collision model's pinned
/// digest. Reported with the exact kSceneMeshContentChangedDetail.
class SceneMeshContentChanged final : public std::runtime_error {
 public:
  SceneMeshContentChanged()
      : std::runtime_error(
            std::string(worker_protocol::kSceneMeshContentChangedDetail)) {}
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error("RoboPlan candidate worker: " +
                           std::move(message));
}

void read_all(int descriptor, void* output, std::size_t size) {
  auto* bytes = static_cast<char*>(output);
  std::size_t offset = 0U;
  while (offset < size) {
    const auto count = ::read(descriptor, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      fail("read failed: " + std::string(std::strerror(errno)));
    }
    if (count == 0) {
      fail("protocol peer closed");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void write_all(int descriptor, const void* input, std::size_t size) {
  const auto* bytes = static_cast<const char*>(input);
  std::size_t offset = 0U;
  while (offset < size) {
    const auto count =
        ::send(descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      fail("write failed: " + std::string(std::strerror(errno)));
    }
    if (count == 0) {
      fail("protocol write made no progress");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void put_u16(std::array<unsigned char, kHeaderBytes>& header,
             std::size_t offset, std::uint16_t value) {
  header[offset] = static_cast<unsigned char>(value & 0xffU);
  header[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put_u32(std::array<unsigned char, kHeaderBytes>& header,
             std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    header[offset++] = static_cast<unsigned char>((value >> shift) & 0xffU);
  }
}

void put_u64(std::array<unsigned char, kHeaderBytes>& header,
             std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    header[offset++] = static_cast<unsigned char>((value >> shift) & 0xffU);
  }
}

[[nodiscard]] std::uint16_t get_u16(
    const std::array<unsigned char, kHeaderBytes>& header,
    std::size_t offset) {
  return static_cast<std::uint16_t>(header[offset]) |
         (static_cast<std::uint16_t>(header[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t get_u32(
    const std::array<unsigned char, kHeaderBytes>& header,
    std::size_t offset) {
  std::uint32_t value = 0U;
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    value |= static_cast<std::uint32_t>(header[offset++]) << shift;
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(
    const std::array<unsigned char, kHeaderBytes>& header,
    std::size_t offset) {
  std::uint64_t value = 0U;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(header[offset++]) << shift;
  }
  return value;
}

[[nodiscard]] Frame receive_frame(int descriptor) {
  std::array<unsigned char, kHeaderBytes> header{};
  read_all(descriptor, header.data(), header.size());
  if (get_u32(header, 0U) != worker_protocol::kFrameMagic ||
      get_u16(header, 4U) != worker_protocol::kProtocolVersion) {
    fail("frame header has an unsupported magic or version");
  }
  const auto raw_type = get_u16(header, 6U);
  if (raw_type < static_cast<std::uint16_t>(MessageType::kInitialize) ||
      raw_type > static_cast<std::uint16_t>(MessageType::kProtocolError)) {
    fail("frame type is invalid");
  }
  const auto payload_size = get_u64(header, 8U);
  if (payload_size > kMaximumFrameBytes ||
      payload_size > std::numeric_limits<std::size_t>::max()) {
    fail("frame payload exceeds its byte bound");
  }
  std::string payload(static_cast<std::size_t>(payload_size), '\0');
  if (!payload.empty()) {
    read_all(descriptor, payload.data(), payload.size());
  }
  return {static_cast<MessageType>(raw_type), std::move(payload)};
}

void send_frame(int descriptor, MessageType type, std::string_view payload) {
  if (payload.size() > kMaximumFrameBytes) {
    fail("outgoing frame payload exceeds its byte bound");
  }
  std::array<unsigned char, kHeaderBytes> header{};
  put_u32(header, 0U, worker_protocol::kFrameMagic);
  put_u16(header, 4U, worker_protocol::kProtocolVersion);
  put_u16(header, 6U, static_cast<std::uint16_t>(type));
  put_u64(header, 8U, payload.size());
  write_all(descriptor, header.data(), header.size());
  if (!payload.empty()) {
    write_all(descriptor, payload.data(), payload.size());
  }
}

[[nodiscard]] Eigen::Matrix4d transform(const core::Pose3DTO& pose) {
  const auto& q = pose.orientation;
  if (!std::isfinite(pose.position_m.x_m) ||
      !std::isfinite(pose.position_m.y_m) ||
      !std::isfinite(pose.position_m.z_m) || !std::isfinite(q.x) ||
      !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) {
    fail("geometry pose is non-finite");
  }
  Eigen::Quaterniond orientation(q.w, q.x, q.y, q.z);
  const double scale = orientation.coeffs().cwiseAbs().maxCoeff();
  if (!std::isfinite(scale) || scale <= 0.0) {
    fail("geometry pose has a zero-norm orientation");
  }
  orientation.coeffs() /= scale;
  orientation.normalize();
  Eigen::Matrix4d value = Eigen::Matrix4d::Identity();
  value.block<3, 3>(0, 0) = orientation.toRotationMatrix();
  value(0, 3) = pose.position_m.x_m;
  value(1, 3) = pose.position_m.y_m;
  value(2, 3) = pose.position_m.z_m;
  return value;
}

[[nodiscard]] bool finite_positive(double value) {
  return std::isfinite(value) && value > 0.0;
}

void add_geometry(roboplan::Scene& scene,
                  const RoboPlanWorkerGeometryDTO& geometry,
                  const std::string& parent_frame) {
  if (geometry.native_geometry_name.empty() ||
      geometry.compiled_parent_frame.empty() || parent_frame.empty()) {
    fail("geometry identity is empty");
  }
  const Eigen::Vector4d color(0.55, 0.55, 0.55, 1.0);
  auto pose = transform(geometry.pose_in_parent);
  const auto result = std::visit(
      [&](const auto& shape) -> tl::expected<void, std::string> {
        using Shape = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Shape, core::BoxShapeDTO>) {
          if (!finite_positive(shape.dimensions_m.x_m) ||
              !finite_positive(shape.dimensions_m.y_m) ||
              !finite_positive(shape.dimensions_m.z_m)) {
            return tl::unexpected("box dimensions are invalid");
          }
          return scene.addBoxGeometry(
              geometry.native_geometry_name, parent_frame,
              roboplan::Box(shape.dimensions_m.x_m, shape.dimensions_m.y_m,
                            shape.dimensions_m.z_m),
              pose, color);
        } else if constexpr (std::is_same_v<Shape, core::SphereShapeDTO>) {
          if (!finite_positive(shape.radius_m)) {
            return tl::unexpected("sphere radius is invalid");
          }
          return scene.addSphereGeometry(
              geometry.native_geometry_name, parent_frame,
              roboplan::Sphere(shape.radius_m), pose, color);
        } else if constexpr (std::is_same_v<Shape,
                                             core::CylinderShapeDTO>) {
          if (!finite_positive(shape.radius_m) ||
              !finite_positive(shape.height_m)) {
            return tl::unexpected("cylinder dimensions are invalid");
          }
          return scene.addCylinderGeometry(
              geometry.native_geometry_name, parent_frame,
              roboplan::Cylinder(shape.radius_m, shape.height_m), pose,
              color);
        } else {
          if (shape.resource_uri.empty() ||
              !finite_positive(shape.scale.x_m) ||
              !finite_positive(shape.scale.y_m) ||
              !finite_positive(shape.scale.z_m) ||
              geometry.resolved_mesh_path.has_value() ==
                  geometry.scene_mesh_content_digest.has_value()) {
            return tl::unexpected("mesh resource is invalid or unavailable");
          }
          const Eigen::Vector3d scale(shape.scale.x_m, shape.scale.y_m,
                                      shape.scale.z_m);
          if (geometry.resolved_mesh_path.has_value()) {
            // Model-local asset:// mesh staged by the retained model stage.
            if (!geometry.resolved_mesh_path->is_absolute() ||
                !std::filesystem::is_regular_file(
                    *geometry.resolved_mesh_path)) {
              return tl::unexpected("mesh resource is invalid or unavailable");
            }
            return scene.addMeshGeometry(
                geometry.native_geometry_name, parent_frame,
                roboplan::Mesh(*geometry.resolved_mesh_path, scale), pose,
                color);
          }
          // Relative Program scene mesh: open it below this worker's own
          // scene-asset root descriptor, keep the validated file open, hash
          // exactly those bytes, and load only through /proc/self/fd/<fd>.
          std::string error;
          const auto opened = collision::OpenedSceneMesh::open(
              worker_protocol::kSceneAssetRootDescriptor, shape.resource_uri,
              error);
          if (!opened.has_value()) {
            return tl::unexpected("scene mesh is unavailable: " + error);
          }
          if (opened->content_digest() !=
              *geometry.scene_mesh_content_digest) {
            throw SceneMeshContentChanged();
          }
          return scene.addMeshGeometry(
              geometry.native_geometry_name, parent_frame,
              roboplan::Mesh(opened->loader_path(), scale), pose, color);
        }
      },
      geometry.shape);
  if (!result.has_value()) {
    fail("could not add resolved scene geometry: " + result.error());
  }
}

[[nodiscard]] std::string staging_frame_for(
    roboplan::Scene& scene, const RoboPlanWorkerModelDTO& model) {
  const auto reserved = [&](const std::string& frame_name) {
    const auto is_geometry_name = std::any_of(
        model.geometries.begin(), model.geometries.end(),
        [&](const auto& geometry) {
          return geometry.native_geometry_name == frame_name;
        });
    if (is_geometry_name) {
      return true;
    }
    return std::any_of(
        model.allowed_collision_pairs.begin(),
        model.allowed_collision_pairs.end(), [&](const auto& pair) {
          return pair.first == frame_name || pair.second == frame_name;
        });
  };
  for (const auto& frame : scene.getModel().frames) {
    if (frame.name.empty() || reserved(frame.name)) {
      continue;
    }
    const auto geometry_ids = scene.getCollisionGeometryIds(frame.name);
    if (geometry_ids.has_value() && geometry_ids->empty()) {
      return frame.name;
    }
  }
  fail("retained projection has no collision-free geometry staging frame");
}

[[nodiscard]] std::shared_ptr<roboplan::Scene> make_scene(
    const RoboPlanWorkerModelDTO& model) {
  if (model.native_scene_name.empty() || !model.urdf_path.is_absolute() ||
      !model.srdf_path.is_absolute() ||
      !std::filesystem::is_regular_file(model.urdf_path) ||
      !std::filesystem::is_regular_file(model.srdf_path) ||
      model.package_paths.empty()) {
    fail("retained model paths are incomplete");
  }
  for (const auto& path : model.package_paths) {
    if (!path.is_absolute() || !std::filesystem::is_directory(path)) {
      fail("retained package path is invalid");
    }
  }
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      model.native_scene_name, roboplan::loadTextFile(model.urdf_path),
      roboplan::loadTextFile(model.srdf_path), model.package_paths,
      model.joint_limits});
  std::string staging_frame;
  if (!model.geometries.empty()) {
    staging_frame = staging_frame_for(*scene, model);
    for (const auto& geometry : model.geometries) {
      add_geometry(*scene, geometry, staging_frame);
    }
  }
  for (const auto& pair : model.allowed_collision_pairs) {
    if (pair.first.empty() || pair.second.empty() ||
        pair.second <= pair.first) {
      fail("resolved allowed-collision pair is not canonical");
    }
    const auto result = scene->setCollisions(pair.first, pair.second, false);
    if (!result.has_value()) {
      fail("could not apply resolved collision policy: " + result.error());
    }
  }
  for (const auto& geometry : model.geometries) {
    const auto frame_id = scene->getFrameId(geometry.compiled_parent_frame);
    if (!frame_id.has_value() ||
        frame_id.value() >= scene->getModel().frames.size()) {
      fail("resolved scene geometry has no exact compiled parent frame");
    }
    // RoboPlan records GeometryObject::placement in the selected frame's
    // parent-joint coordinates even though its API accepts a frame name.
    // Retained ManyForge poses are relative to that named frame, so preserve
    // the semantic frame by applying its fixed parent-joint transform here.
    Eigen::Matrix4d pose = scene->getModel().frames[frame_id.value()]
                               .placement.toHomogeneousMatrix() *
                           transform(geometry.pose_in_parent);
    const auto result = scene->updateGeometryPlacement(
        geometry.native_geometry_name, geometry.compiled_parent_frame, pose);
    if (!result.has_value()) {
      fail("could not place resolved scene geometry: " + result.error());
    }
  }
  return scene;
}

void constrain_process(pid_t expected_parent) {
  if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
    fail("PR_SET_PDEATHSIG failed: " + std::string(std::strerror(errno)));
  }
  if (::getppid() != expected_parent) {
    fail("parent exited before worker containment was established");
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    fail("PR_SET_NO_NEW_PRIVS failed: " + std::string(std::strerror(errno)));
  }
  rlimit core_limit{0U, 0U};
  if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
    fail("could not disable worker core dumps");
  }
}

[[nodiscard]] pid_t parse_parent_pid(int argc, char** argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--parent-pid") {
    fail("usage: manyforge_roboplan_candidate_worker --parent-pid PID");
  }
  std::string_view text(argv[2]);
  if (text.empty()) {
    fail("parent PID is empty");
  }
  std::uint64_t value = 0U;
  for (char character : text) {
    if (character < '0' || character > '9') {
      fail("parent PID is malformed");
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (static_cast<std::uint64_t>(
                     std::numeric_limits<pid_t>::max()) - digit) /
                    10U) {
      fail("parent PID is out of range");
    }
    value = value * 10U + digit;
  }
  if (value == 0U) {
    fail("parent PID is zero");
  }
  return static_cast<pid_t>(value);
}

int run_worker(int descriptor) {
  const auto initialize = receive_frame(descriptor);
  if (initialize.type != MessageType::kInitialize) {
    fail("first message is not initialization");
  }
  auto input = worker_protocol::decode_initialization(initialize.payload);
  auto scene = make_scene(input.model);
  RoboPlanCandidateComputer computer(scene, std::move(input.binding),
                                     input.candidate_config);
  send_frame(descriptor, MessageType::kReady, {});

  for (;;) {
    auto request = receive_frame(descriptor);
    switch (request.type) {
      case MessageType::kPlan: {
        auto candidate =
            worker_protocol::decode_candidate_request(request.payload);
        RoboPlanCandidateControl control;
        if (candidate.moving_replacement) {
          const double remaining =
              candidate.moving_replacement->remaining_preparation_budget_s;
          if (!std::isfinite(remaining) || remaining <= 0.0 ||
              remaining > std::chrono::duration<double>(std::chrono::hours(24))
                              .count()) {
            fail("moving replacement preparation budget is invalid");
          }
          const auto deadline =
              std::chrono::steady_clock::now() +
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(remaining));
          control.deadline_expired = [deadline] {
            return std::chrono::steady_clock::now() >= deadline;
          };
        }
        auto result = computer.compute(candidate, control);
        if (control.deadline_expired && control.deadline_expired()) {
          core::ErrorDTO error{"planning_timeout",
                               "Moving replacement preparation timed out", {}};
          if (!result.ok()) {
            const auto &underlying = result.error();
            error.details = underlying.details;
            error.details.try_emplace("underlying_error_code", underlying.code);
            error.details.try_emplace("underlying_error_message", underlying.message);
            if (const auto reason = underlying.details.find("reason");
                reason != underlying.details.end()) {
              error.details.try_emplace("underlying_error_reason", reason->second);
            }
          }
          error.details["reason"] = "planning_timeout";
          result = core::Result<core::TrajectoryDTO>::Err(std::move(error));
        }
        send_frame(descriptor, MessageType::kPlanResult,
                   worker_protocol::encode_trajectory_result(result));
        break;
      }
      case MessageType::kIk: {
        auto candidate = worker_protocol::decode_ik_request(request.payload);
        auto result = computer.solve_ik(
            candidate.goal, candidate.base_frame, candidate.tcp_frame,
            candidate.seed_state, candidate.timeout_s);
        send_frame(descriptor, MessageType::kIkResult,
                   worker_protocol::encode_ik_result(result));
        break;
      }
      case MessageType::kShutdown:
        if (!request.payload.empty()) {
          fail("shutdown message has a payload");
        }
        return 0;
      default:
        fail("received a message invalid for initialized state");
    }
  }
}

}  // namespace
}  // namespace manyforge::roboplan_backend

int main(int argc, char** argv) {
  using namespace manyforge::roboplan_backend;
  const int descriptor = worker_protocol::kProtocolDescriptor;
  try {
    const auto parent = parse_parent_pid(argc, argv);
    constrain_process(parent);
    return run_worker(descriptor);
  } catch (const SceneMeshContentChanged& error) {
    try {
      send_frame(descriptor, worker_protocol::MessageType::kProtocolError,
                 worker_protocol::encode_protocol_error(
                     worker_protocol::kSceneMeshContentChangedDetail));
    } catch (...) {
    }
    std::cerr << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    try {
      send_frame(descriptor, worker_protocol::MessageType::kProtocolError,
                 worker_protocol::encode_protocol_error(error.what()));
    } catch (...) {
    }
    std::cerr << error.what() << '\n';
    return 2;
  }
}
