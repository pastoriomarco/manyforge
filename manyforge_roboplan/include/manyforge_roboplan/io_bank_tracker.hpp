// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/types/io_signal.hpp>

namespace manyforge::roboplan_backend {

/// One signal of an I/O bank: its name in its component, its direction and
/// its bit in the bank's words (0..63).
struct IoBankSignal final {
  std::string name;
  core::IoSignalDirection direction{core::IoSignalDirection::kInput};
  int index{};
};

/// The read-back of one I/O bank, shared by every I/O adapter whose device
/// reports the whole bank per sample (the xArm's `xarm_io_states`, the
/// `ros_io_bank` family's state). ROS-free, no clock, no lock: the component
/// that owns it holds its own mutex around every call.
///
/// - Inputs: each sample's value (source `device`).
/// - Outputs: the reported value changes only on events. An accepted write
///   reports the written value at once (`manyforge_write`); afterwards a
///   sample still showing the old value at a position up to `old_until` is
///   ignored, the written value seen ends the confirmation, and another value
///   after `old_until` is an outside change (`outside`). Without a write in
///   progress, a change in the samples is an outside change. A write with no
///   answer leaves the output `unknown` until the next sample reports it.
/// - `position` grows with each sample: the xArm passes the receipt time,
///   the bank its own sample count.
class IoBankTracker final {
public:
  IoBankTracker(std::string component_id, std::vector<IoBankSignal> signals,
                const std::int64_t stale_after_ns)
      : component_id_(std::move(component_id)), signals_(std::move(signals)),
        stale_after_ns_(stale_after_ns), tracks_(signals_.size()) {
    for (const auto &signal : signals_)
      if (signal.index < 0 || signal.index > 63)
        throw std::invalid_argument("an I/O bank signal's index is 0..63: " + signal.name);
  }

  /// One device sample: bit `i` of each word is the signal with index `i`.
  /// The updates of every signal (the host publishes them as one acquisition).
  [[nodiscard]] std::vector<core::IoSignalUpdateDTO>
  OnSample(const std::uint64_t inputs, const std::uint64_t outputs,
           const std::int64_t received_at_ns, const std::int64_t position) {
    using Source = core::IoSignalSampleSource;
    std::vector<core::IoSignalUpdateDTO> updates;
    updates.reserve(signals_.size());
    last_sample_at_ns_ = received_at_ns;
    for (std::size_t index = 0U; index < signals_.size(); ++index) {
      const auto &signal = signals_[index];
      auto &track = tracks_[index];
      const bool output = signal.direction == core::IoSignalDirection::kOutput;
      const bool value = (((output ? outputs : inputs) >> signal.index) & 1U) != 0U;
      if (!output) {
        track.reported = value;
        track.source = Source::kDevice;
      } else if (track.held) {
        // A sent write not decided yet: its samples are ignored.
      } else if (track.unknown) {
        // A write with no answer: the sample reports whether it took effect.
        if (value != track.reported) {
          track.reported = value;
          track.source = value == *track.unknown ? Source::kManyforgeWrite : Source::kDevice;
        }
        track.unknown.reset();
      } else if (track.pending) {
        if (value == track.pending->value) {
          // The write seen: confirmed, nothing more happens.
          track.pending.reset();
        } else if (position > track.pending->old_until) {
          // Not seen in time: an outside write raced it.
          track.pending.reset();
          track.reported = value;
          track.source = Source::kOutside;
        }
        // Otherwise a sample still showing the old value: ignored.
      } else if (!track.reported) {
        track.reported = value;
        track.source = Source::kDevice;
      } else if (value != *track.reported && (!track.last_stream || value != *track.last_stream)) {
        track.reported = value;
        track.source = Source::kOutside;
      }
      if (output && !track.held) track.last_stream = value;
      updates.push_back({Id(signal), core::IoSampleValidity::kValid, track.reported,
                         std::nullopt, track.source});
    }
    return updates;
  }

  /// An accepted write: reported at once; samples at positions up to
  /// `old_until` still showing the old value are ignored.
  void WriteAccepted(const std::size_t signal, const bool value, const std::int64_t old_until) {
    auto &track = tracks_.at(signal);
    track.held = false;
    track.reported = value;
    track.source = core::IoSignalSampleSource::kManyforgeWrite;
    track.pending = Pending{value, old_until};
    track.unknown.reset();
  }

  /// A sent write with no answer: unknown until the next sample.
  void WriteUnanswered(const std::size_t signal, const bool value) {
    auto &track = tracks_.at(signal);
    track.held = false;
    track.pending.reset();
    track.unknown = value;
  }

  /// A sent write whose decision arrives in a later sample: until it is
  /// decided, the output's samples are ignored.
  void Hold(const std::size_t signal) { tracks_.at(signal).held = true; }
  /// A held write refused by the device: its samples count again.
  void Release(const std::size_t signal) { tracks_.at(signal).held = false; }

  /// The device restarted: every value is reported afresh from the device.
  void NewEpoch() { tracks_.assign(signals_.size(), Track{}); }

  [[nodiscard]] bool Fresh(const std::int64_t now_ns) const {
    return last_sample_at_ns_ && now_ns - *last_sample_at_ns_ <= stale_after_ns_;
  }
  [[nodiscard]] const std::optional<std::int64_t> &LastSampleAt() const noexcept {
    return last_sample_at_ns_;
  }
  [[nodiscard]] std::int64_t StaleAfterNs() const noexcept { return stale_after_ns_; }
  [[nodiscard]] bool AnyUnknown() const {
    return std::any_of(tracks_.begin(), tracks_.end(),
                       [](const Track &track) { return track.unknown.has_value(); });
  }

  /// The observed value of every signal.
  [[nodiscard]] std::vector<core::SignalSample> Samples(const std::int64_t now_ns) const {
    std::vector<core::SignalSample> samples;
    samples.reserve(signals_.size());
    const bool fresh = Fresh(now_ns);
    for (std::size_t index = 0U; index < signals_.size(); ++index) {
      const auto &track = tracks_[index];
      core::SignalSample sample{Id(signals_[index]), std::nullopt, track.source, 0, fresh};
      if (track.reported) sample.value = *track.reported;
      if (last_sample_at_ns_) sample.age_ns = now_ns - *last_sample_at_ns_;
      samples.push_back(std::move(sample));
    }
    return samples;
  }

  [[nodiscard]] const std::vector<IoBankSignal> &signals() const noexcept { return signals_; }
  /// The position of the signal named `name` in `signals()`, or none.
  [[nodiscard]] std::optional<std::size_t> Find(const std::string_view name) const {
    const auto found = std::find_if(signals_.begin(), signals_.end(),
                                    [&](const auto &signal) { return signal.name == name; });
    if (found == signals_.end()) return std::nullopt;
    return static_cast<std::size_t>(found - signals_.begin());
  }
  [[nodiscard]] core::IoSignalId Id(const IoBankSignal &signal) const {
    return {core::ControllableComponentId(component_id_), signal.name};
  }

private:
  struct Pending final {
    bool value{};
    std::int64_t old_until{};
  };
  struct Track final {
    std::optional<bool> reported;
    core::IoSignalSampleSource source{core::IoSignalSampleSource::kDevice};
    std::optional<bool> last_stream;
    std::optional<Pending> pending;
    /// The value of a write with no answer, until a sample reports.
    std::optional<bool> unknown;
    bool held{};
  };

  std::string component_id_;
  std::vector<IoBankSignal> signals_;
  std::int64_t stale_after_ns_;
  std::vector<Track> tracks_;
  std::optional<std::int64_t> last_sample_at_ns_;
};

} // namespace manyforge::roboplan_backend
