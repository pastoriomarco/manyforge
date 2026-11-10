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

#include <atomic>
#include <limits>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/ports/iio_signal_ingress.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace manyforge::core {

struct IoSourceIngressSenderStatus final {
  bool accepting;
  std::uint64_t last_ingress_source_sequence;
  std::uint64_t last_enqueued_source_sequence;
  std::optional<IoIngressLossReason> last_loss;
};

struct IoSourceIngressAcquisitionReceipt final {
  TimePoint received_at_ns;
  std::uint64_t driver_observation_generation;
  IoSourceIncarnation callback_incarnation;
};

/// Shared adapter-local handoff for the two ROS boolean input providers.
/// Host supplies an exclusive installed source/incarnation, nonnull shared
/// ingress and its IClock. No queue, evaluator, source-currentness or lifecycle
/// authority lives here. Capture receipt before decoding/enqueue; quiesce all
/// selected callbacks before releasing the clock. Ingress may briefly lock.
class IoSourceIngressSender final {
 public:
  IoSourceIngressSender(std::shared_ptr<IIoSignalIngress> ingress,
                        std::shared_ptr<IClock> clock,
                        InterruptSourceId source, IoSourceIncarnation incarnation,
                        std::optional<std::uint64_t> prepared_driver_generation = std::nullopt)
      : ingress_(std::move(ingress)),
        clock_(std::move(clock)),
        source_(std::move(source)),
        incarnation_(std::move(incarnation)),
        accepting_(!prepared_driver_generation),
        enabled_(!prepared_driver_generation),
        driver_generation_(prepared_driver_generation.value_or(0U)) {
    if (!clock_)
      throw std::invalid_argument("I/O ingress sender requires its shared clock");
  }

  IoSourceIngressAcquisitionReceipt CaptureAcquisition(
      const IoSourceIncarnation& callback_incarnation,
      std::uint64_t subscription_generation) const {
    // The subscription captures its actual owner-issued generation immutably.
    // Even an old buffered callback first entering after Resume cannot adopt
    // the new channel's token. Stamp before decoding or enqueue work.
    return {clock_->now(), subscription_generation, callback_incarnation};
  }

  void Receive(std::vector<IoSignalUpdateDTO> updates, IoSourceIngressAcquisitionReceipt receipt) {
    std::lock_guard lock(mutex_);
    // IO-1: every message is an arrival on the device's `inputs` stream,
    // also while its delivery is paused.
    if (arrivals_) arrivals_->RecordArrival();
    if (!accepting_ || receipt.driver_observation_generation != driver_generation_.load() ||
        receipt.callback_incarnation != incarnation_)
      return;
    const auto received_at_ns = receipt.received_at_ns;
    if (ingress_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      Lose(IoIngressLossReason::kSequenceExhausted, received_at_ns);
      accepting_ = false;
      return;
    }
    const IoSourceAcquisitionDTO acquisition{source_, incarnation_, ++ingress_sequence_,
                                             received_at_ns, std::move(updates)};
    if (ingress_->TryEnqueue(acquisition)) {
      enqueued_sequence_ = ingress_sequence_;
    } else {
      // Valid-current capacity refusal already retained its shared loss
      // barrier. Invalid/stale refusal cannot change the current source. Never
      // notify twice.
      last_loss_ = IoIngressLossReason::kHostIngressRejected;
    }
  }

  void NotifyLoss(IoIngressLossReason reason, IoSourceIngressAcquisitionReceipt receipt,
                  std::optional<std::uint64_t> middleware_lost = std::nullopt) {
    std::lock_guard lock(mutex_);
    if (accepting_ && receipt.driver_observation_generation == driver_generation_.load() &&
        receipt.callback_incarnation == incarnation_)
      Lose(reason, receipt.received_at_ns, middleware_lost);
  }

  bool MarkUnavailable(const IoSourceIncarnation& expected_incarnation,
                       std::uint64_t expected_ingress_source_sequence) {
    std::lock_guard lock(mutex_);
    if (!accepting_ || expected_incarnation != incarnation_ ||
        expected_ingress_source_sequence != ingress_sequence_)
      return false;
    Lose(IoIngressLossReason::kDisconnected, clock_->now());
    return true;
  }

  /// IO-1: the device's `inputs` stream (absent: none declared).
  void SetArrivalRecorder(std::shared_ptr<StreamRecorder> arrivals) {
    std::lock_guard lock(mutex_);
    arrivals_ = std::move(arrivals);
  }

  /// Rebind delivery of the same actual retained driver under the existing
  /// Apply/domain lock. The host supplies the new source incarnation and its
  /// actual driver observation generation. No driver lifecycle state is copied.
  bool RebindIngress(std::shared_ptr<IIoSignalIngress> ingress, IoSourceIncarnation incarnation,
                     std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto current = driver_generation_.load();
    if (retired_ || !ingress || incarnation.empty() || generation == 0U) return false;
    if (incarnation == incarnation_) return ingress == ingress_ && generation == current;
    if (generation <= current) return false;
    if (accepting_) Lose(IoIngressLossReason::kDisconnected, clock_->now());
    ingress_ = std::move(ingress);
    incarnation_ = std::move(incarnation);
    ingress_sequence_ = 0U;
    enqueued_sequence_ = 0U;
    last_loss_.reset();
    accepting_ = enabled_;
    // Publish the successor token only after its source binding is complete.
    // Old callback receipts cannot enter after this critical section releases.
    driver_generation_.store(generation, std::memory_order_release);
    return true;
  }

  /// The existing driver owner supplies its actual observation generation;
  /// this helper never mints a lifecycle/source epoch or resets sequence.
  bool Pause(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto current = driver_generation_.load();
    if (retired_ || generation == 0U || generation < current) return false;
    if (generation == current) return !enabled_;
    const bool was_accepting = accepting_;
    accepting_ = false;
    enabled_ = false;
    driver_generation_.store(generation, std::memory_order_release);
    if (was_accepting) Lose(IoIngressLossReason::kDisconnected, clock_->now());
    return true;
  }

  bool Resume(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto current = driver_generation_.load();
    if (retired_ || generation == 0U || generation < current ||
        ingress_sequence_ == std::numeric_limits<std::uint64_t>::max())
      return false;
    if (generation == current) return accepting_;
    driver_generation_.store(generation, std::memory_order_release);
    accepting_ = true;
    enabled_ = true;
    return true;
  }

  /// Roll back one failed owner-issued Resume without minting another driver
  /// generation. This is not a lifecycle command: only the exact current
  /// generation may close its own accepting gate.
  bool AbortResume(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (retired_ || generation == 0U ||
        generation != driver_generation_.load())
      return false;
    if (!enabled_)
      return true;
    const bool was_accepting = accepting_;
    accepting_ = false;
    enabled_ = false;
    if (was_accepting)
      Lose(IoIngressLossReason::kDisconnected, clock_->now());
    return true;
  }

  void Shutdown() {
    std::lock_guard lock(mutex_);
    retired_ = true;
    enabled_ = false;
    if (!accepting_) return;
    const auto received_at_ns = clock_->now();
    accepting_ = false;
    Lose(IoIngressLossReason::kDisconnected, received_at_ns);
  }

  IoSourceIngressSenderStatus status() const {
    std::lock_guard lock(mutex_);
    return {accepting_, ingress_sequence_, enqueued_sequence_, last_loss_};
  }

 private:
  void Lose(IoIngressLossReason reason, TimePoint received_at_ns,
            std::optional<std::uint64_t> middleware_lost = std::nullopt) {
    last_loss_ = reason;
    ingress_->NotifyLoss(
        {source_, incarnation_, received_at_ns, ingress_sequence_, reason, middleware_lost});
  }

  std::shared_ptr<IIoSignalIngress> ingress_;
  std::shared_ptr<IClock> clock_;
  const InterruptSourceId source_;
  IoSourceIncarnation incarnation_;
  mutable std::mutex mutex_;
  bool accepting_ = true;
  bool enabled_ = true;
  bool retired_ = false;
  std::atomic<std::uint64_t> driver_generation_{0U};
  std::uint64_t ingress_sequence_ = 0;
  std::uint64_t enqueued_sequence_ = 0;
  std::optional<IoIngressLossReason> last_loss_;
  std::shared_ptr<StreamRecorder> arrivals_;
};

}  // namespace manyforge::core
