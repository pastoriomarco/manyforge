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


#include <manyforge_xarm_control/xarm_tool_context.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <utility>

namespace manyforge::xarm_control {
namespace {

using Offset = std::array<double, 6>;

[[nodiscard]] core::Result<bool> refused(core::ErrorDTO error) {
  return core::Result<bool>::Err(std::move(error));
}

// A sent call's effect: `observed` once this step re-armed the robot.
void mark_effect(core::ErrorDTO &error, const bool rearmed) {
  if (rearmed) error.details.insert_or_assign("effect", "observed");
}

[[nodiscard]] core::ErrorDTO load_not_applied(const XArmServiceCallResult &call,
                                              const bool rearmed) {
  // A sent call with no answer in time is the robot controller
  // unresponsive (the component fault); any other failure is the typed
  // refusal, with the vendor reason recorded.
  const bool unanswered = call.outcome == XArmServiceCallOutcome::kUnknown &&
                          XArmCallUnanswered(call.reason);
  core::ErrorDTO error{
      unanswered ? std::string(kRobotControllerUnresponsiveCode)
                 : std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
      "the robot controller did not confirm the payload",
      {{"reason", "tool_context_not_applied"},
       {"vendor_reason", call.reason},
       {"tool_context_field", "tcp_load"}}};
  if (call.outcome == XArmServiceCallOutcome::kNotSent &&
      call.reason == "component_lifecycle_deadline_expired") {
    error.code = "component_lifecycle_deadline_expired";
    error.message = "the lifecycle deadline expired before dispatch";
    error.details.insert_or_assign("reason", "deadline_exhausted");
  }
  if (call.vendor_return)
    error.details.emplace("vendor_return", std::to_string(*call.vendor_return));
  if (rearmed) mark_effect(error, true);
  else if (call.outcome == XArmServiceCallOutcome::kUnknown)
    error.details.insert_or_assign("effect", "possible");
  return error;
}

// Within the tolerances (a reported offset is a float); angles the shorter
// way round.
[[nodiscard]] bool offset_matches(const Offset &reported, const Offset &wanted,
                                  const bool require_finite = false) {
  for (std::size_t index = 0U; index < 6U; ++index) {
    const double difference =
        index < 3U ? std::abs(wanted[index] - reported[index])
                   : std::abs(std::remainder(wanted[index] - reported[index],
                                             2.0 * std::numbers::pi));
    if ((require_finite && !std::isfinite(difference)) ||
        difference > (index < 3U ? kXArmTcpOffsetToleranceMm : kXArmTcpOffsetToleranceRad))
      return false;
  }
  return true;
}

} // namespace

XArmToolContextSender::XArmToolContextSender(SendTcpOffset send_offset,
                                             SendTcpLoad send_load,
                                             ReportedTcpOffset reported_offset)
    : send_offset_(std::move(send_offset)), send_load_(std::move(send_load)),
      reported_offset_(std::move(reported_offset)) {}

core::Result<bool>
XArmToolContextSender::Apply(const std::optional<core::Pose3DTO> &offset,
                             const std::optional<core::ToolContextPointMassDTO> &payload,
                             const bool at_enable, const XArmToolContextBound &bound) {
  // Serialized upstream (see the header); no lock across the driver calls.
  bool rearmed = false;
  if (offset) {
    const core::Pose3DTO &sent = *offset;
    const Offset wanted = xarm_tcp_offset(sent);
    bool send = true;
    if (at_enable) {
      const auto reported = reported_offset_();
      if (!reported)
        return refused(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
            "the robot controller's status report is missing or out of date",
            {{"reason", "xarm_status_unavailable"}, {"effect", "not_started"}}});
      // If the host crashes after an asynchronous offset
      // send, a matching report at the new host's Enable can skip this send
      // before the old request lands. The maintainer accepted that residual.
      send = !offset_matches(*reported, wanted);
    }
    if (send) {
      // The offset, `set_state(0)` and a fresh ready report (the controller
      // drops to state 5 after `set_tcp_offset`), each call once.
      auto sent_result = send_offset_(sent, bound);
      if (!sent_result.confirmed)
        return refused(sent_result.error.value_or(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
            "the robot controller did not confirm the TCP offset",
            {{"reason", "tool_context_not_applied"},
             {"tool_context_field", "tcp_offset"},
             {"effect", "possible"}}}));
      rearmed = true;
      // The report after the re-arm shows the sent offset.
      if (!sent_result.ready_offset || !offset_matches(*sent_result.ready_offset, wanted, true)) {
        core::ErrorDTO error{
            "tcp_offset_not_applied",
            "the robot controller did not apply the TCP offset",
            {{"reason", "tcp_offset_not_applied"}, {"tool_context_field", "tcp_offset"},
             {"vendor", "xarm"}}};
        mark_effect(error, rearmed);
        return refused(std::move(error));
      }
    }
  }
  // The payload: `set_tcp_load` only, which keeps the controller ready
  // (refused in state 5, hence after the offset's re-arm).
  if (payload) {
    const auto call = send_load_(payload->mass_kg, payload->position_in_flange_m, bound);
    if (call.outcome != XArmServiceCallOutcome::kSucceeded)
      return refused(load_not_applied(call, rearmed));
  }
  return core::Result<bool>::Ok(rearmed);
}

} // namespace manyforge::xarm_control
