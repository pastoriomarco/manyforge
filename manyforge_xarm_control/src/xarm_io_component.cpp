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

#include <manyforge_xarm_control/xarm_io_component.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace manyforge::xarm_control {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentLifecycle;
using core::IoSignalSampleSource;

namespace {

constexpr std::string_view kStreamId = "io_states";

[[nodiscard]] std::int64_t period_ns(const double hz) {
  return static_cast<std::int64_t>(std::llround(1e9 / hz));
}

[[nodiscard]] roboplan_backend::SetOutputResult write_failed(std::string code, std::string message,
                                               const ActionOutcome outcome =
                                                   ActionOutcome::kFailed,
                                               std::optional<std::string> vendor = std::nullopt) {
  return {outcome, core::ActionError{std::move(code), std::move(message), std::move(vendor),
                                     std::nullopt}};
}

[[nodiscard]] core::ActionResult action_result(const ComponentAction action,
                                               const ActionOutcome outcome,
                                               std::optional<core::ActionError> error = {}) {
  core::ActionResult made;
  made.action = action;
  made.outcome = outcome;
  made.error = std::move(error);
  return made;
}

} // namespace

std::int64_t xarm_io_confirmation_bound_ns(const XArmIoSettings &settings) {
  const auto publish = period_ns(settings.rate_hz);
  return 2 * (publish + period_ns(settings.tgpio_monitor_frequency_hz)) + publish;
}

std::int64_t xarm_io_stale_after_ns(const XArmIoSettings &settings) {
  return kXArmIoStalePeriods * period_ns(settings.rate_hz);
}

XArmIoComponent::XArmIoComponent(XArmIoComponentConfig config,
                                 std::shared_ptr<const IXArmDriver> driver,
                                 std::function<bool(std::int64_t)> owner_usable,
                                 core::IClock &clock)
    : config_(std::move(config)),
      confirmation_bound_ns_(xarm_io_confirmation_bound_ns(config_.settings)),
      stale_after_ns_(xarm_io_stale_after_ns(config_.settings)),
      driver_(std::move(driver)), owner_usable_(std::move(owner_usable)), clock_(clock),
      tracks_(config_.signals.size()) {
  if (!driver_ || !owner_usable_) throw std::invalid_argument("the xArm I/O needs its owner");
  if (!(config_.settings.rate_hz > 0.0) || !(config_.settings.tgpio_monitor_frequency_hz > 0.0))
    throw std::invalid_argument("the xArm I/O stream settings must be positive");
  for (const auto &signal : config_.signals) {
    if (signal.pin < 0 || signal.pin > 15)
      throw std::invalid_argument("an xArm I/O pin is a bit of a 16-bit word: " + signal.name);
  }
}

const XArmIoSignal *XArmIoComponent::Find(const std::string &name) const {
  const auto found = std::find_if(config_.signals.begin(), config_.signals.end(),
                                  [&](const auto &signal) { return signal.name == name; });
  return found == config_.signals.end() ? nullptr : &*found;
}

core::IoSignalId XArmIoComponent::Id(const XArmIoSignal &signal) const {
  return {core::ControllableComponentId(config_.component_id), signal.name};
}

bool XArmIoComponent::StreamFresh(const std::int64_t now_ns) const {
  return last_sample_at_ns_ && now_ns - *last_sample_at_ns_ <= stale_after_ns_;
}

bool XArmIoComponent::OwnedLocked(const XArmIoSignal &signal) const {
  return config_.bank == XArmIoBank::kController &&
         signal.direction == core::IoSignalDirection::kOutput && gpio_ &&
         gpio_->output_functions[static_cast<std::size_t>(signal.pin)] != 0;
}

bool XArmIoComponent::ControllerOwned(const std::string &name) const {
  const auto *signal = Find(name);
  std::lock_guard lock(mutex_);
  return signal != nullptr && OwnedLocked(*signal);
}

std::vector<core::IoSignalUpdateDTO> XArmIoComponent::OnIoStates(const XArmIoStatesSample &sample) {
  const bool tool = config_.bank == XArmIoBank::kTool;
  const auto inputs = tool ? sample.tgpio_digital_inputs : sample.cgpio_digital_inputs;
  const auto outputs = tool ? sample.tgpio_digital_outputs : sample.cgpio_digital_outputs;
  std::vector<core::IoSignalUpdateDTO> updates;
  std::lock_guard lock(mutex_);
  last_sample_at_ns_ = sample.received_at_ns;
  for (std::size_t index = 0U; index < config_.signals.size(); ++index) {
    const auto &signal = config_.signals[index];
    auto &track = tracks_[index];
    const bool output = signal.direction == core::IoSignalDirection::kOutput;
    const bool value = (((output ? outputs : inputs) >> signal.pin) & 1U) != 0U;
    if (!output) {
      track.reported = value;
      track.source = IoSignalSampleSource::kDevice;
    } else if (track.unknown) {
      // A write with no answer: the stream reports whether it took effect.
      if (value != track.reported) {
        track.reported = value;
        track.source = value == *track.unknown ? IoSignalSampleSource::kManyforgeWrite
                                               : IoSignalSampleSource::kDevice;
      }
      track.unknown.reset();
    } else if (track.pending) {
      if (value == track.pending->value) {
        // Our write seen: confirmed, nothing more happens.
        track.pending.reset();
      } else if (sample.received_at_ns > track.pending->deadline_ns) {
        // Not seen within the bound: an outside write raced ours.
        track.pending.reset();
        track.reported = value;
        track.source = IoSignalSampleSource::kOutside;
      }
      // Otherwise a sample still showing the old value: ignored.
    } else if (!track.reported) {
      track.reported = value;
      track.source = IoSignalSampleSource::kDevice;
    } else if (value != *track.reported && (!track.last_stream || value != *track.last_stream)) {
      track.reported = value;
      track.source = IoSignalSampleSource::kOutside;
    }
    if (output) track.last_stream = value;
    updates.push_back({Id(signal), core::IoSampleValidity::kValid, track.reported, std::nullopt,
                       track.source});
  }
  return updates;
}

void XArmIoComponent::OnGpioModuleStatus(const XArmGpioModuleStatus &status) {
  std::lock_guard lock(mutex_);
  gpio_ = status;
}

std::vector<core::SignalSample> XArmIoComponent::Samples(const std::int64_t now_ns) const {
  std::vector<core::SignalSample> samples;
  std::lock_guard lock(mutex_);
  const bool fresh = StreamFresh(now_ns);
  for (std::size_t index = 0U; index < config_.signals.size(); ++index) {
    const auto &track = tracks_[index];
    core::SignalSample sample{Id(config_.signals[index]), std::nullopt, track.source, 0, fresh};
    if (track.reported) sample.value = *track.reported;
    if (last_sample_at_ns_) sample.age_ns = now_ns - *last_sample_at_ns_;
    samples.push_back(std::move(sample));
  }
  return samples;
}

core::ComponentState XArmIoComponent::State(const std::int64_t now_ns) const {
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  const bool owner_usable = owner_usable_(now_ns);
  std::lock_guard lock(mutex_);
  core::ComponentStream stream{std::string(kStreamId), core::StreamFreshness::kNone, std::nullopt};
  if (last_sample_at_ns_) {
    stream.freshness =
        StreamFresh(now_ns) ? core::StreamFreshness::kFresh : core::StreamFreshness::kStale;
    stream.age_ns = now_ns - *last_sample_at_ns_;
  }
  state.streams.push_back(stream);
  state.lifecycle = lifecycle_;
  const bool unknown = std::any_of(tracks_.begin(), tracks_.end(),
                                   [](const Track &track) { return track.unknown.has_value(); });
  state.activity = writes_in_flight_ > 0 ? core::ComponentActivity::kBusy
                   : unknown             ? core::ComponentActivity::kUnknown
                                         : core::ComponentActivity::kIdle;
  // Its own stream only while the owner is usable; otherwise the core shows
  // the member offline through its owner.
  if (lifecycle_ == ComponentLifecycle::kNotPrepared || !owner_usable ||
      stream.freshness == core::StreamFreshness::kFresh) {
    stale_since_.reset();
    return state;
  }
  if (!stale_since_) stale_since_ = now_ns;
  state.lifecycle = ComponentLifecycle::kFault;
  state.fault = core::Fault{core::FaultCode::kStreamStale,
                            "the robot's I/O stream (xarm_io_states) stopped",
                            true,
                            core::FaultOperatorAction::kRecover,
                            std::nullopt,
                            std::nullopt,
                            {},
                            *stale_since_};
  return state;
}

std::vector<core::Capability> XArmIoComponent::capabilities() const {
  return {core::Capability::kIoManualWrite, core::Capability::kIoOutputReadback};
}

core::ActionResult XArmIoComponent::Run(const ComponentAction action, const std::stop_token stop) {
  if (action == ComponentAction::kPrepare || action == ComponentAction::kRecover ||
      action == ComponentAction::kEnable) {
    // The stream comes through the owner, whose driver may have just
    // started: a fresh sample within the lifecycle command timeout.
    const auto deadline = clock_.now() + config_.action_timeout.count();
    const auto fresh = [&] {
      std::lock_guard lock(mutex_);
      return StreamFresh(clock_.now());
    };
    while (!fresh() && !stop.stop_requested() && clock_.now() < deadline) {
      if (config_.pause)
        config_.pause();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  std::lock_guard lock(mutex_);
  switch (action) {
  case ComponentAction::kPrepare:
  case ComponentAction::kRecover:
  case ComponentAction::kEnable:
    if (!StreamFresh(clock_.now()))
      return action_result(action, ActionOutcome::kFailed,
                           core::ActionError{"fault_stream_stale",
                                             "the robot's I/O stream (xarm_io_states) isn't "
                                             "arriving",
                                             std::nullopt, std::nullopt});
    lifecycle_ = action == ComponentAction::kEnable ? ComponentLifecycle::kEnabled
                                                    : ComponentLifecycle::kPrepared;
    return action_result(action, ActionOutcome::kSucceeded);
  case ComponentAction::kDisable:
    if (lifecycle_ == ComponentLifecycle::kEnabled) lifecycle_ = ComponentLifecycle::kPrepared;
    return action_result(action, ActionOutcome::kSucceeded);
  case ComponentAction::kResetPosition:
  case ComponentAction::kPowerOff:
  case ComponentAction::kReboot:
  case ComponentAction::kSimReset: break;
  }
  return action_result(action, ActionOutcome::kFailed,
                       core::ActionError{"component_action_unsupported",
                                         "the xArm I/O doesn't support this action",
                                         std::nullopt, std::nullopt});
}

roboplan_backend::SetOutputResult
XArmIoComponent::SetOutput(const std::string_view name,
                           const std::variant<bool, double> requested,
                           const std::stop_token stop) {
  const auto *signal = Find(std::string(name));
  if (signal == nullptr || signal->direction != core::IoSignalDirection::kOutput)
    return write_failed("io_signal_unknown", "'" + std::string(name) +
                                                 "' is not an output this I/O member's model "
                                                 "declares");
  if (!std::holds_alternative<bool>(requested))
    return write_failed("io_signal_type_mismatch",
                        std::string(name) + " is a digital output; its value is true or false");
  const bool value = std::get<bool>(requested);
  {
    std::lock_guard lock(mutex_);
    if (OwnedLocked(*signal))
      return write_failed("io_output_controller_owned",
                          std::string(name) + " has a controller function and is read-only");
    if (lifecycle_ != ComponentLifecycle::kEnabled)
      return write_failed("io_output_not_submitted",
                          "the I/O component is not enabled; nothing was sent");
    ++writes_in_flight_;
  }
  const auto call = driver_->SetDigitalOutput(config_.bank, signal->pin, value,
                                              kXArmServiceResponseTimeout, stop);
  const auto at = clock_.now();
  std::lock_guard lock(mutex_);
  --writes_in_flight_;
  auto &track = tracks_[static_cast<std::size_t>(signal - config_.signals.data())];
  // `ret=1`: a controller error is present and the write still takes effect;
  // the error is the owner's fault.
  if (call.outcome == XArmServiceCallOutcome::kSucceeded || call.vendor_return == 1) {
    // Trust the controller: the reported value is the written one at once.
    track.reported = value;
    track.source = IoSignalSampleSource::kManyforgeWrite;
    track.pending = Pending{value, at + confirmation_bound_ns_};
    track.unknown.reset();
    return {ActionOutcome::kSucceeded, std::nullopt};
  }
  if (call.outcome == XArmServiceCallOutcome::kNotSent)
    return write_failed(call.reason == "xarm_service_cancelled"
                            ? "component_action_interrupted_by_stop"
                            : "io_output_not_submitted",
                        "the output write was not sent (" + call.reason + ")");
  if (call.vendor_return)
    return write_failed("io_output_write_failed", "the robot controller refused the write",
                        ActionOutcome::kFailed, "ret=" + std::to_string(*call.vendor_return));
  // Sent, no answer: unknown until the stream reports, never replayed.
  track.pending.reset();
  track.unknown = value;
  return write_failed(XArmCallUnanswered(call.reason)
                          ? std::string(kRobotControllerUnresponsiveCode)
                          : std::string("component_action_interrupted_by_stop"),
                      "the output write's outcome is unknown (" + call.reason + ")",
                      ActionOutcome::kUnknown);
}

} // namespace manyforge::xarm_control
