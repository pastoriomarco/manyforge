// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_roboplan/io_bank_tracker.hpp>
#include <manyforge_roboplan/io_output_writer.hpp>

namespace manyforge::roboplan_backend {

/// `IoBankCommand.values` (`manyforge_io_interfaces`).
inline constexpr std::uint8_t kIoBankOutputOff = 0U;
inline constexpr std::uint8_t kIoBankOutputOn = 1U;
inline constexpr std::uint8_t kIoBankOutputKeep = 2U;

/// `IoBankState.applied_result` values (`manyforge_io_interfaces`).
inline constexpr std::uint8_t kIoBankResultUnset = 0U;
inline constexpr std::uint8_t kIoBankResultAccepted = 1U;
inline constexpr std::uint8_t kIoBankResultRefused = 2U;

/// State periods without a fresh sample after which the bank's stream is
/// stale (the xArm's rule).
inline constexpr std::int64_t kIoBankStalePeriods = 10;
/// K: fresh device samples after a write's send without its decision, after
/// which the write ends `unknown` (OD-327 D8; to be measured, T-7).
inline constexpr std::uint32_t kIoBankUnansweredSamples = 40U;

/// The stale bound: ten state periods (50 ms at 200 Hz).
[[nodiscard]] std::int64_t io_bank_stale_after_ns(double rate_hz);

/// One `IoBankState` sample, stamped at receipt.
struct IoBankStateSample final {
  std::uint32_t cycle{};
  std::uint32_t applied_seq{};
  std::uint8_t applied_result{};
  std::uint16_t device_code{};
  /// Bit `i` = `inputs[i]` / `outputs[i]`.
  std::uint64_t inputs{};
  std::uint64_t outputs{};
  /// Bit `i` = `refused[i]`: the outputs of `applied_seq` the device refused.
  std::uint64_t refused{};
  std::int64_t received_at_ns{};
};

/// One `IoBankCommand`: OFF/ON for the outputs it sets, KEEP for the others.
struct IoBankCommandMessage final {
  std::uint32_t seq{};
  std::array<std::uint8_t, 64> values{};
};

/// One finished command, for the link's debug log (latency measurement).
struct IoBankWriteRecord final {
  std::uint32_t seq{};
  /// The outputs it set.
  std::size_t outputs{};
  std::int64_t sent_at_ns{};
  /// Receipt time of the sample that decided it; none when it ended unknown.
  std::optional<std::int64_t> decided_at_ns;
  /// Succeeded: every output accepted; failed: any refused; unknown.
  core::ActionOutcome outcome{core::ActionOutcome::kUnknown};
};

/// The bank's transport (`RosIoBankLink`; a fake in tests).
class IIoBankLink {
public:
  virtual ~IIoBankLink() = default;
  /// Publishes one command; false when nothing was sent.
  [[nodiscard]] virtual bool Send(const IoBankCommandMessage &command) = 0;
  /// One finished command (called on a writer's thread, no lock held).
  virtual void Record(const IoBankWriteRecord &record) = 0;
};

struct IoBankComponentConfig final {
  std::string component_id;
  std::vector<IoBankSignal> signals;
  /// The device's state rate (the model's `rate_hz`).
  double rate_hz{200.0};
  /// Prepare, Recover and Enable wait this long for a fresh sample.
  std::chrono::nanoseconds action_timeout{};
  /// Between reads of that wait (production: 20 ms).
  std::function<void()> pause{};
};

/// The `ros_io_bank` family id (a model's `lanes.real.adapter`).
inline constexpr std::string_view kIoBankAdapterFamily = "ros_io_bank";

/// The family's check of the row the host hands it (OD-327 D6/D20):
/// `{adapter: ros_io_bank, rate_hz, inputs: [name], outputs: [name],
/// indexes: {name: index}}`. Every signal needs an index 0..63, unique per
/// direction; `rate_hz` is positive. Throws `std::invalid_argument`.
[[nodiscard]] IoBankComponentConfig
io_bank_config_from_row(const std::string &component_id,
                        const content_identity::CanonicalValue &lane_row);

/// A device's I/O bank over the `ros_io_bank` protocol (an owner-less
/// model-form I/O device, OD-327). ROS-free; `RosIoBankLink` carries it.
///
/// - Reading: a sample whose `cycle` equals the previous one is not fresh;
///   a `cycle` that goes back starts a new epoch (device restart): a pending
///   write ends `unknown` and every value is reported afresh as `device`.
///   Fresh samples go to the shared tracker (`IoBankTracker`, position =
///   this component's sample count).
/// - Writing: one command in flight per bank. Writes that arrive meanwhile
///   wait in arrival order; the next command merges them, at most one per
///   output (a second write to an output already in it waits for the
///   following command, so order holds and pulses survive). Nothing is
///   dropped or skipped. The oldest waiting write's caller sends the
///   command, once. It is decided by the first fresh sample with
///   `applied_seq == seq`: with `ACCEPTED` or `REFUSED`, each write whose
///   output is not in `refused` succeeds (the sample's `outputs` is the
///   read-back), each refused one fails `io_output_source_rejected` with the
///   device's code; any other result is `unknown` for every write in it.
///   Without it, every write in it ends `unknown`
///   (`io_output_acceptance_unknown`) when the stream goes stale, after K
///   fresh samples or at a device restart. A Stop before the send removes
///   only that caller's write; after the send it completes. A late
///   acceptance of an unknown command resolves its outputs as its effect;
///   nothing is resent. Only declared outputs are ever non-KEEP.
/// - `seq`: from the first fresh sample of each epoch, `applied_seq + 1`,
///   then one more per command, never equal to the last `applied_seq` seen.
/// - Prepare, Recover, Enable: a fresh sample within the lifecycle command
///   time, else `fault_stream_stale`. Health: a stale stream is
///   `fault_stream_stale` (`vendor_code: cycle_stopped` while samples still
///   arrive with a frozen `cycle`).
class IoBankComponent final : public core::IComponentAdapter, public IIoOutputWriter {
public:
  IoBankComponent(IoBankComponentConfig config, core::IClock &clock);

  /// The transport, attached once after construction (its callback holds
  /// this component).
  void Attach(std::shared_ptr<IIoBankLink> link);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

  [[nodiscard]] const std::string &component_id() const noexcept {
    return config_.component_id;
  }

  /// One state sample: the updates of every signal (empty when the sample
  /// isn't fresh). Bounded memory work under the mutex; waits on nothing.
  [[nodiscard]] std::vector<core::IoSignalUpdateDTO> OnState(const IoBankStateSample &sample);

  [[nodiscard]] std::vector<core::SignalSample> Samples(std::int64_t now_ns) const;

  [[nodiscard]] SetOutputResult SetOutput(std::string_view name,
                                          std::variant<bool, double> value,
                                          std::stop_token stop) override;

  /// The writes waiting for a command (observation for tests and status).
  [[nodiscard]] std::size_t WaitingWrites() const;

private:
  /// One caller's write; shared by its caller and the command carrying it.
  struct Write final {
    std::size_t signal{};
    bool value{};
    std::optional<SetOutputResult> result;
  };
  struct Command final {
    std::uint32_t seq{};
    std::vector<std::shared_ptr<Write>> writes;
    std::uint32_t samples_seen{};
    bool ended{};
    std::optional<std::int64_t> decided_at_ns;
    core::ActionOutcome outcome{core::ActionOutcome::kUnknown};
  };

  void NotifyLocked();
  void EndLocked(Command &command, std::optional<std::int64_t> decided_at_ns);
  void UnknownLocked(const std::shared_ptr<Command> &command, const std::string &why);
  [[nodiscard]] std::uint32_t NextSeqLocked();

  const IoBankComponentConfig config_;
  core::IClock &clock_;
  mutable std::mutex mutex_;
  /// Notified when a command ends or a waiting write leaves (any waiter may
  /// then send the next command or return); stop-aware for waiting writes.
  std::condition_variable_any changed_;
  /// Counts those notifications (a waiter's wake-up condition).
  std::uint64_t changes_{};
  std::shared_ptr<IIoBankLink> link_;
  core::ComponentLifecycle lifecycle_{core::ComponentLifecycle::kNotPrepared};
  IoBankTracker tracker_;
  /// This component's fresh-sample count (the tracker's position).
  std::int64_t position_{};
  std::optional<std::uint32_t> last_cycle_;
  std::uint32_t last_applied_seq_{};
  /// Receipt of the last sample, fresh or a repeat.
  std::optional<std::int64_t> last_any_sample_at_ns_;
  /// The next `seq`; none until the first fresh sample of an epoch.
  std::optional<std::uint32_t> next_seq_;
  /// Waiting writes in arrival order, and the command in flight.
  std::deque<std::shared_ptr<Write>> waiting_;
  std::shared_ptr<Command> in_flight_;
  /// The last command that ended unknown, for a late acceptance.
  std::shared_ptr<Command> late_;
  mutable std::optional<std::int64_t> stale_since_;
};

} // namespace manyforge::roboplan_backend
