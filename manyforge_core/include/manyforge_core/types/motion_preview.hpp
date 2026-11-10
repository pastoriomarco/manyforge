#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/tcp_entry.hpp>

namespace manyforge::core {

/// Exact owner read used to resolve a direct-control preview endpoint.
enum class PreviewTcpCaptureSource { kActiveTcp, kJointFlangeFallback, kExplicitEntry };
struct PreviewTcpCapture final {
  TcpEntryReferenceDTO entry;
  std::uint64_t registration_generation;
  std::uint64_t sequence;
  PreviewTcpCaptureSource source;
};

// Owner-private, non-authorizing sampled evidence for a manual preview.
enum class MotionPreviewContactDisposition { kAllowed, kRefused, kUnknown };
enum class MotionPreviewSampleStatus { kChecked, kUnknown };

struct MotionPreviewContact {
  std::size_t pair_index{};
  MotionPreviewContactDisposition disposition{MotionPreviewContactDisposition::kUnknown};
};

struct MotionPreviewPair {
  std::string body_a;
  std::string body_b;
};

struct MotionPreviewClearanceViolation {
  std::size_t pair_index{};
  double distance_m{};
  double required_clearance_m{};
};

struct MotionPreviewSample {
  double time_from_start_s{};
  std::vector<double> positions_rad;
  std::vector<MotionPreviewContact> contacts;
  MotionPreviewSampleStatus status{MotionPreviewSampleStatus::kUnknown};
  std::vector<MotionPreviewClearanceViolation> clearance_violations;
};

struct MotionPathPreview {
  // Exact compiled direct-control binding order; the native serializer maps it
  // to the source-local joint names after validating the binding.
  std::vector<std::string> joint_names;
  std::vector<MotionPreviewPair> pairs;
  std::vector<MotionPreviewSample> samples;
  std::string diagnostic_kind{"validated_path"};
};

/// Immutable result of one non-dispatching plan/validate pass.
///
/// This is an owner-private C++ value, not a public request surface. The
/// trajectory and terminal state are retained so deterministic multi-leg
/// candidate search can chain the exact output of one leg into the next
/// without sampling mutable robot state between legs.
struct MotionPreviewResultDTO final {
  TrajectoryDTO trajectory;
  RobotStateDTO start_state;
  RobotStateDTO final_state;
  /// Exact typed terminal state for the selected execution binding. This is
  /// predicted candidate evidence, not an observed-state publication.
  ControlledStateDTO final_controlled_state;
  Pose3DTO final_tcp_pose;
  ProjectionLeaseBindingDTO projection_binding;
  CollisionWorldRevision collision_world_revision;
  std::size_t dynamically_discovered_start_contact_count{};
  std::optional<PreviewTcpCapture> resolved_tcp{};
};

}  // namespace manyforge::core
