#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/program_reference.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::core {

/// A component's default TCP (Deployment `default_tcp`, compiled into its cell
/// graph row): one TCP role of one of its command targets. Role names are
/// unique per target only, so the target is part of the value.
struct ComponentDefaultTcpDTO {
  CommandTargetId command_target_id;
  FrameRoleId frame_role_id;

  friend bool operator==(const ComponentDefaultTcpDTO&,
                         const ComponentDefaultTcpDTO&) = default;
};

/// One TCP, named by its owner: component + target + frame role
/// (`lite_gripper / gripper / tcp`). There is no alias: every frame has one
/// name. A dotted spelling is for display only.
struct TcpEntryDTO {
  ControllableComponentId component_id;
  CommandTargetId target_id;
  FrameRoleId frame_role_id;

  friend bool operator==(const TcpEntryDTO&, const TcpEntryDTO&) = default;
};

/// Selector form 1: the robot's Active TCP.
struct CurrentTcpDTO final {
  friend bool operator==(const CurrentTcpDTO&, const CurrentTcpDTO&) = default;
};

/// Selector form 2: the default TCP of the exact direct component target.
struct DefaultTcpOfTargetDTO final {
  ProgramTargetReferenceDTO target;

  friend bool operator==(const DefaultTcpOfTargetDTO&, const DefaultTcpOfTargetDTO&) = default;
};

/// Authored TCP selector. The target and frame can belong to different
/// components when a mounted tool supplies the frame.
struct TcpEntryReferenceDTO final {
  ProgramTargetReferenceDTO target;
  ProgramFrameReferenceDTO frame;
  friend bool operator==(const TcpEntryReferenceDTO&, const TcpEntryReferenceDTO&) = default;
};

[[nodiscard]] inline bool valid_tcp_entry_reference(const TcpEntryReferenceDTO& entry) noexcept {
  return !entry.target.component_id.empty() && !entry.target.command_target_id.empty() &&
         !entry.frame.component_id.empty() && !entry.frame.frame_id.empty();
}

/// Active TCP: the TCP selector of a move or a Set TCP. Forms 2
/// and 3 set the Active TCP to what they resolve to; a Set TCP never takes
/// form 1.
using TcpSelectorDTO = std::variant<CurrentTcpDTO, DefaultTcpOfTargetDTO, TcpEntryReferenceDTO>;

[[nodiscard]] inline bool valid_tcp_entry(const TcpEntryDTO& entry) noexcept {
  return !entry.component_id.empty() && !entry.target_id.empty() &&
         !entry.frame_role_id.empty();
}

[[nodiscard]] inline bool valid_tcp_selector(const TcpSelectorDTO& selector) noexcept {
  if (const auto* target = std::get_if<DefaultTcpOfTargetDTO>(&selector))
    return !target->target.component_id.empty() && !target->target.command_target_id.empty();
  if (const auto* entry = std::get_if<TcpEntryReferenceDTO>(&selector))
    return valid_tcp_entry_reference(*entry);
  return true;
}

/// The run-time check `carries(robot, entry owner)` failed. Refused
/// before any effect, for an entry move, a Set TCP and a Robot-control
/// selection alike.
[[nodiscard]] inline ErrorDTO tcp_tool_not_mounted_error(const ControllableComponentId& robot,
                                                         const TcpEntryDTO& entry) {
  return ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                  "tool not mounted on " + robot.value(),
                  {{"reason", "tcp_tool_not_mounted"},
                   {"robot", robot.value()},
                   {"tool", entry.component_id.value()}}};
}

/// Set TCP named an entry (from a key) that is not in the robot's TCP list.
[[nodiscard]] inline ErrorDTO active_tcp_unknown_error(const ControllableComponentId& robot,
                                                       const TcpEntryDTO& entry) {
  return ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                  "TCP not on " + robot.value(),
                  {{"reason", "active_tcp_unknown"},
                   {"robot", robot.value()},
                   {"component_id", entry.component_id.value()},
                   {"target_id", entry.target_id.value()},
                   {"frame_role_id", entry.frame_role_id.value()}}};
}

[[nodiscard]] inline ErrorDTO tcp_tool_not_mounted_error(
    const ControllableComponentId& robot, const TcpEntryReferenceDTO& entry) {
  return ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                  "tool not mounted on " + robot.value(),
                  {{"reason", "tcp_tool_not_mounted"}, {"robot", robot.value()},
                   {"tool", entry.frame.component_id.value()}}};
}

[[nodiscard]] inline ErrorDTO active_tcp_unknown_error(
    const ControllableComponentId& robot, const TcpEntryReferenceDTO& entry) {
  return ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                  "TCP not on " + robot.value(),
                  {{"reason", "active_tcp_unknown"}, {"robot", robot.value()},
                   {"component_id", entry.target.component_id.value()},
                   {"command_target_id", entry.target.command_target_id.value()},
                   {"frame_component_id", entry.frame.component_id.value()},
                   {"frame_id", entry.frame.frame_id.value()}}};
}

/// Active TCP: what made a confirmed Active TCP change. A Set TCP
/// or a TCP-changing move of a Program, Robot control's Set TCP, or a reset
/// to the End-effector TCP.
enum class ActiveTcpChangeSource : std::uint8_t { kProgram, kRobotControl, kReset };

[[nodiscard]] constexpr std::string_view to_string(ActiveTcpChangeSource source) noexcept {
  switch (source) {
    case ActiveTcpChangeSource::kProgram:
      return "program";
    case ActiveTcpChangeSource::kRobotControl:
      return "robot_control";
    case ActiveTcpChangeSource::kReset:
      return "reset";
  }
  return "reset";
}

/// Active TCP: one confirmed change of a robot's Active TCP,
/// the `active_tcp_changed` event of the host's ordered events stream. The
/// robot owner appends it under its own lock, so the stream order is the
/// commit order. `entry` absent: no End-effector TCP.
struct ActiveTcpChangeDTO final {
  ControllableComponentId component_id;
  std::optional<TcpEntryReferenceDTO> previous;
  std::optional<TcpEntryReferenceDTO> entry;
  ActiveTcpChangeSource source{ActiveTcpChangeSource::kReset};
  /// The controller took a new TCP offset in this change (the vendor
  /// adapter sent it); false on Mock/Isaac, with the checkbox off, and while
  /// the controller could not take it (the next Enable re-syncs).
  bool offset_sent{};
  /// Order of Active TCP values (with the Kernel owner epoch of the stream
  /// that carries them): the robot runtime owner's registration generation
  /// (new and greater at every install or restart within one Kernel owner
  /// epoch) and its sequence after this change (from 0 at install, +1 per
  /// change). Results, reads and events carry `{owner_epoch,
  /// registration_generation, sequence}`; a reader keeps the greatest.
  std::uint64_t registration_generation{};
  std::uint64_t sequence{};
  std::int64_t occurred_at_ns{};

  friend bool operator==(const ActiveTcpChangeDTO&, const ActiveTcpChangeDTO&) = default;
};

}  // namespace manyforge::core
