// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/tool_context.hpp>
#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control {

/// One TCP offset send including the re-arm it needs (the
/// controller answers `set_tcp_offset` by leaving the ready state, live
/// 2026-10-28): confirmed, or the typed error and whether a sent step may
/// still take effect.
struct XArmTcpOffsetSent final {
  bool confirmed{};
  std::optional<core::ErrorDTO> error;
  std::optional<std::array<double, 6>> ready_offset{};
};

/// What bounds one tool-context step's calls. At Enable the
/// sends are part of the lifecycle step: each call is allowed min(the call
/// cap, the step's remaining time) and follows the step's cancellation.
/// Outside Enable (not a lifecycle step): no deadline, the fixed call cap.
struct XArmToolContextBound final {
  std::optional<std::chrono::steady_clock::time_point> step_deadline;
  std::stop_token cancellation;
};

/// The controller's TCP offset
/// matches the wanted one within these (as sent: mm, then radians).
inline constexpr double kXArmTcpOffsetToleranceMm = 0.01;
inline constexpr double kXArmTcpOffsetToleranceRad = 1e-4;

/// Active TCP: the xArm side of the event-driven tool
/// context. It keeps no copy of what the controller has; each step sends what
/// its event changed, and calls are serialized.
/// - A TCP change or reset: `set_tcp_offset`, its re-arm (`set_state(0)`, a
///   fresh ready report: the controller drops to state 5 after the offset),
///   then the post-send
///   check: the fresh report after the re-arm must show the sent
///   offset within the tolerances, otherwise `tcp_offset_not_applied`.
///   No report is read before the send.
/// - A payload change: `set_tcp_load` only, no offset and no re-arm (the
///   controller stays ready), at once, also while moving (to verify
///   live). No readback.
/// - Enable (the re-sync): the fresh reported offset is read
///   once; the Active TCP's offset (zero without one) is sent only when the
///   report differs, with the post-send check; then the payload, always. No
///   fresh report: `xarm_status_unavailable`, nothing sent.
/// - The host selects Keep for changes with sync off and Zero at Enable.
/// A failure after a send carries `effect` (`possible`: a sent call may still
/// act; `observed`: the robot was re-armed): the component fault.
class XArmToolContextSender final {
public:
  using SendTcpOffset =
      std::function<XArmTcpOffsetSent(const core::Pose3DTO &, const XArmToolContextBound &)>;
  using SendTcpLoad = std::function<XArmServiceCallResult(
      double, const std::array<double, 3> &, const XArmToolContextBound &)>;
  /// The controller's reported offset (`xarm_tcp_offset` units) from the
  /// latest fresh report; none without one. Read at Enable.
  using ReportedTcpOffset = std::function<std::optional<std::array<double, 6>>()>;

  XArmToolContextSender(SendTcpOffset send_offset, SendTcpLoad send_load,
                        ReportedTcpOffset reported_offset);

  /// Each optional value requests a send; absent means Keep. At Enable the
  /// requested offset is compared with the fresh report before sending.
  /// True means an offset was sent and the robot re-armed.
  [[nodiscard]] core::Result<bool> Apply(const std::optional<core::Pose3DTO> &offset,
                                         const std::optional<core::ToolContextPointMassDTO> &payload,
                                         bool at_enable,
                                         const XArmToolContextBound &bound = {});

private:
  const SendTcpOffset send_offset_;
  const SendTcpLoad send_load_;
  const ReportedTcpOffset reported_offset_;
  // No lock: the sender holds no mutable state, and every Apply comes from
  // the arm's tool-context hook, which the runtime handle calls only under
  // its `switch_mutex_` (one apply at a time per arm).
};

} // namespace manyforge::xarm_control
