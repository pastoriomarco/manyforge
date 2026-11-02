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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_roboplan/io_output_writer.hpp>
#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control {

/// One I/O signal an I/O member's model declares (its row's `inputs` and
/// `outputs`): only declared signals are offered, since the controller
/// answers `ret=0` even to writes on outputs that don't exist.
struct XArmIoSignal final {
  /// The signal's name in its I/O component (e.g. `CO1`, `TO0`).
  std::string name;
  core::IoSignalDirection direction{core::IoSignalDirection::kInput};
  /// Its bit in the bank's digital word, and the `ionum` of a write (CI/CO
  /// 0-7, DI/DO 8-15; TI/TO 0-4).
  int pin{};
};

/// The `xarm_io_states` settings (`vendor/manyforge_settings.yaml`, `io_states`).
struct XArmIoSettings final {
  /// The driver's publish rate of `xarm_io_states`.
  double rate_hz{100.0};
  /// The controller's tool monitoring frequency (`set_tgpio_monitor_params`).
  double tgpio_monitor_frequency_hz{100.0};
};

/// Publish periods without a sample after which the stream is stale.
inline constexpr std::int64_t kXArmIoStalePeriods = 10;

/// The stream's stale bound, computed from the settings: ten publish periods
/// (100 ms at 100 Hz).
[[nodiscard]] std::int64_t xarm_io_stale_after_ns(const XArmIoSettings &settings);

/// The plan's confirmation bound, computed from the settings:
/// 2 × (publish period + tool monitoring period) + one publish period
/// (50 ms at 100 Hz / 100 Hz).
[[nodiscard]] std::int64_t xarm_io_confirmation_bound_ns(const XArmIoSettings &settings);

/// One `xarm_msgs/IOStates` sample's digital words, stamped at receipt.
struct XArmIoStatesSample final {
  std::uint16_t cgpio_digital_inputs{};
  std::uint16_t cgpio_digital_outputs{};
  std::uint16_t tgpio_digital_inputs{};
  std::uint16_t tgpio_digital_outputs{};
  std::int64_t received_at_ns{};
};

/// The GPIO module's health and pin configuration from the rich report
/// (`xarm_cgpio_states`, `xarm_msgs/CIOState`).
struct XArmGpioModuleStatus final {
  /// 0 normal, 1 wrong, 6 communication failure.
  int state{};
  /// 0 normal, otherwise the module's error code.
  int code{};
  /// `output_conf`: each controller output's function (0 = general output).
  std::array<int, 16> output_functions{};
};

struct XArmIoComponentConfig final {
  /// The I/O member's component id.
  std::string component_id;
  XArmIoBank bank{XArmIoBank::kController};
  std::vector<XArmIoSignal> signals;
  XArmIoSettings settings;
  /// The component's lifecycle command timeout: Prepare, Recover and Enable
  /// wait this long for a fresh `xarm_io_states` sample (none: no wait).
  std::chrono::nanoseconds action_timeout{};
  /// Between reads of that wait (production: 20 ms, as the arm).
  std::function<void()> pause{};
};

/// An I/O member of an xArm machine (controller I/O or tool I/O, CONTRACTS
/// §16), on the pushed `xarm_io_states` stream its owner subscribes to (plan,
/// "xArm I/O stream"; CONTRACTS §7).
///
/// - Reading: each stream sample gives the bank's inputs (source `device`).
/// - Writing, trusting the controller: `ret=0` and `ret=1` (a controller
///   error is present, and the write still takes effect; the error is the
///   owner's fault) are success, and the reported value becomes the written
///   one at once (`manyforge_write`); other `ret` values fail typed; no answer
///   is `unknown` and is never replayed: the output and the activity stay
///   `unknown` until the stream reports the output's value.
/// - An output's reported value changes only on events: an accepted write,
///   or a change in the stream. After a write, samples still showing the old
///   value are ignored; the written value seen ends the confirmation. Not
///   seen within the confirmation bound, or a stream change to a value other
///   than the reported one, is an outside change (`outside`).
/// - A controller output with a controller function is read-only; a write is
///   refused before sending (`io_output_controller_owned`).
/// - Writes come through the output-writer interface (CONTRACTS §6, §7);
///   outputs are digital, so a number is refused (`io_signal_type_mismatch`).
/// - Prepare, Recover, Enable: a fresh stream sample within the lifecycle
///   command timeout (the stream starts with the owner's driver), else
///   `fault_stream_stale`.
/// - Health: a stale stream is `fault_stream_stale` while the owner is usable
///   (prepared, connected, not deliberately offline); controller errors (C33,
///   C19, the GPIO module's) are the owner's faults.
class XArmIoComponent final : public core::IComponentAdapter,
                              public roboplan_backend::IIoOutputWriter {
public:
  /// `owner_usable`: whether the owner is usable now (its stream is then
  /// expected).
  XArmIoComponent(XArmIoComponentConfig config, std::shared_ptr<const IXArmDriver> driver,
                  std::function<bool(std::int64_t now_ns)> owner_usable, core::IClock &clock);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

  [[nodiscard]] const std::string &component_id() const noexcept {
    return config_.component_id;
  }

  /// One stream sample: the updates of every declared signal of this
  /// component (the host publishes them as one acquisition).
  [[nodiscard]] std::vector<core::IoSignalUpdateDTO> OnIoStates(const XArmIoStatesSample &sample);
  /// One rich report's pin configuration (controller-owned outputs).
  void OnGpioModuleStatus(const XArmGpioModuleStatus &status);

  /// The observed value of every declared signal (CONTRACTS §7 `SignalSample`).
  [[nodiscard]] std::vector<core::SignalSample> Samples(std::int64_t now_ns) const;
  /// Whether an output is controller-owned now (from the latest pin
  /// configuration); tool outputs never are.
  [[nodiscard]] bool ControllerOwned(const std::string &name) const;
  /// One output write; blocks until the driver answers.
  [[nodiscard]] roboplan_backend::SetOutputResult
  SetOutput(std::string_view name, std::variant<bool, double> value,
            std::stop_token stop) override;

private:
  struct Pending final {
    bool value{};
    std::int64_t deadline_ns{};
  };
  struct Track final {
    std::optional<bool> reported;
    core::IoSignalSampleSource source{core::IoSignalSampleSource::kDevice};
    std::optional<bool> last_stream;
    std::optional<Pending> pending;
    /// The value of a write with no answer, until the stream reports.
    std::optional<bool> unknown;
  };

  [[nodiscard]] bool StreamFresh(std::int64_t now_ns) const;
  [[nodiscard]] bool OwnedLocked(const XArmIoSignal &signal) const;
  [[nodiscard]] const XArmIoSignal *Find(const std::string &name) const;
  [[nodiscard]] core::IoSignalId Id(const XArmIoSignal &signal) const;

  const XArmIoComponentConfig config_;
  const std::int64_t confirmation_bound_ns_;
  const std::int64_t stale_after_ns_;
  const std::shared_ptr<const IXArmDriver> driver_;
  const std::function<bool(std::int64_t)> owner_usable_;
  core::IClock &clock_;
  mutable std::mutex mutex_;
  core::ComponentLifecycle lifecycle_{core::ComponentLifecycle::kNotPrepared};
  std::vector<Track> tracks_;
  std::optional<std::int64_t> last_sample_at_ns_;
  std::optional<XArmGpioModuleStatus> gpio_;
  int writes_in_flight_{0};
  /// When the stream went stale while the owner was usable.
  mutable std::optional<std::int64_t> stale_since_;
};

} // namespace manyforge::xarm_control
