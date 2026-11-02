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

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control::testing {

/// A deterministic `robot_states`.
class FakeStatus final : public IXArmStatusSource {
public:
  [[nodiscard]] XArmObservedState observe() const override {
    std::lock_guard lock(mutex);
    return report;
  }
  void Set(XArmObservedState next) {
    std::lock_guard lock(mutex);
    report = std::move(next);
  }
  mutable std::mutex mutex;
  XArmObservedState report;
};

/// A driver that records each call and answers from scripted results.
class FakeDriver final : public IXArmDriver {
public:
  [[nodiscard]] XArmServiceCallResult CleanError(std::chrono::nanoseconds,
                                                 std::stop_token) const override {
    return Record("clean_error", clean_error);
  }
  [[nodiscard]] XArmServiceCallResult CleanWarn(std::chrono::nanoseconds,
                                                std::stop_token) const override {
    return Record("clean_warn", clean_warn);
  }
  /// Answers from `err_warn_reads` in order while any are left, then `err_warn`.
  [[nodiscard]] XArmErrWarnRead GetErrWarnCode(std::chrono::nanoseconds,
                                               std::stop_token) const override {
    static_cast<void>(Record("get_err_warn_code", {XArmServiceCallOutcome::kSucceeded, "", 0}));
    std::lock_guard lock(mutex);
    if (reads_answered < err_warn_reads.size()) return err_warn_reads[reads_answered++];
    return err_warn;
  }
  [[nodiscard]] XArmServiceCallResult SetState(int state, std::chrono::nanoseconds,
                                               std::stop_token) const override {
    return Record("set_state(" + std::to_string(state) + ")", set_state);
  }
  [[nodiscard]] XArmServiceCallResult SetTcpOffset(const core::Pose3DTO &,
                                                   std::chrono::nanoseconds,
                                                   std::stop_token) const override {
    return Record("set_tcp_offset", set_tcp_offset);
  }
  [[nodiscard]] XArmServiceCallResult SetTcpLoad(double, const std::array<double, 3> &,
                                                 std::chrono::nanoseconds,
                                                 std::stop_token) const override {
    return Record("set_tcp_load", ok);
  }
  [[nodiscard]] XArmServiceCallResult SetDigitalOutput(XArmIoBank bank, int ionum, bool value,
                                                       std::chrono::nanoseconds,
                                                       std::stop_token) const override {
    return Record(std::string(bank == XArmIoBank::kTool ? "set_tgpio_digital("
                                                        : "set_cgpio_digital(") +
                      std::to_string(ionum) + "," + (value ? "1" : "0") + ")",
                  set_digital);
  }
  [[nodiscard]] XArmServiceCallResult SystemControl(int value, std::chrono::nanoseconds,
                                                    std::stop_token) const override {
    return Record("system_control(" + std::to_string(value) + ")", system_control);
  }

  [[nodiscard]] std::vector<std::string> Calls() const {
    std::lock_guard lock(mutex);
    return calls;
  }

  static inline const XArmServiceCallResult ok{XArmServiceCallOutcome::kSucceeded, "", 0};
  XArmServiceCallResult clean_error{ok};
  XArmErrWarnRead err_warn{ok, 0, 0};
  std::vector<XArmErrWarnRead> err_warn_reads;
  XArmServiceCallResult clean_warn{ok};
  XArmServiceCallResult set_state{ok};
  XArmServiceCallResult set_tcp_offset{ok};
  XArmServiceCallResult set_digital{ok};
  XArmServiceCallResult system_control{ok};

private:
  XArmServiceCallResult Record(std::string call, XArmServiceCallResult answer) const {
    std::lock_guard lock(mutex);
    calls.push_back(std::move(call));
    return answer;
  }
  mutable std::mutex mutex;
  mutable std::vector<std::string> calls;
  mutable std::size_t reads_answered{0};
};

/// A ros2_control lane (or a member adapter): records its actions, answers
/// from a script.
class FakeLane final : public core::IComponentAdapter {
public:
  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override {
    auto made = state;
    made.observed_at_ns = now_ns;
    return made;
  }
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action, std::stop_token) override {
    actions.push_back(action);
    if (on_run) on_run(action);
    core::ActionResult made;
    made.action = action;
    made.outcome = outcome;
    if (outcome != core::ActionOutcome::kSucceeded)
      made.error = core::ActionError{"lane_failed", "the lane failed", {}, {}};
    if (outcome == core::ActionOutcome::kSucceeded && action == core::ComponentAction::kEnable)
      state.lifecycle = core::ComponentLifecycle::kEnabled;
    // The arm controller switched off.
    if (outcome == core::ActionOutcome::kSucceeded && action == core::ComponentAction::kDisable &&
        state.lifecycle == core::ComponentLifecycle::kEnabled)
      state.lifecycle = core::ComponentLifecycle::kPrepared;
    return made;
  }
  [[nodiscard]] std::vector<core::Capability> capabilities() const override { return {}; }

  core::ComponentState state = [] {
    core::ComponentState made;
    made.lifecycle = core::ComponentLifecycle::kPrepared;
    made.activity = core::ComponentActivity::kBusy;
    // The joint state broadcaster's samples, just received.
    made.streams = {{"measured_joints", core::StreamFreshness::kFresh, 0}};
    return made;
  }();
  core::ActionOutcome outcome{core::ActionOutcome::kSucceeded};
  std::vector<core::ComponentAction> actions;
  std::function<void(core::ComponentAction)> on_run;
};

/// A `robot_states` report at `at`: by default error-free, joint-servo mode,
/// ready, all six motors on.
[[nodiscard]] inline XArmObservedState report(const std::int64_t at, const int error = 0,
                                              const int mode = kXArmJointServoMode,
                                              const int state = kXArmReadyState,
                                              const int motors = 0x3F) {
  XArmObservedState made;
  made.received_timestamp_ns = at;
  made.error_code = error;
  made.warning_code = 0;
  made.mode = mode;
  made.state = state;
  made.motion_enable_mask = motors;
  made.tcp_offset = {0, 0, 0, 0, 0, 0};
  made.joint_angles_rad = {0.1, -0.2, 0.3, 0.0, 0.5, 0.6};
  return made;
}

} // namespace manyforge::xarm_control::testing
