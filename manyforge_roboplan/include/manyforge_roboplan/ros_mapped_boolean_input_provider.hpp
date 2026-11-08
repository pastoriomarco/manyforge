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

#include <manyforge_core/io_source_ingress_sender.hpp>
#include <memory>
#include <optional>
#include <rclcpp/node.hpp>
#include <rclcpp/qos.hpp>

namespace manyforge::ros2_control_execution { class Ros2TransportResources; }

namespace manyforge::roboplan_backend {

struct RosGpioBooleanInputSelector final {
  std::string group;
  std::string interface_name;
};
struct RosMappedBooleanInputSignal final {
  core::IoSignalDescriptorDTO descriptor;
  RosGpioBooleanInputSelector selector;
};
struct RosMappedBooleanInputBinding final {
  core::InterruptSourceId source_id;
  core::IoSourceIncarnation incarnation;
  std::string topic;
  std::vector<RosMappedBooleanInputSignal> signals;
};

/// One typed ROS source, one atomic configured-signal acquisition. GPIO reads
/// exact group/interface with finite 0/1 values. Missing/ambiguous channels are invalid without a borrowed value.
/// Receipt is actual callback ingress, never a hardware acquisition timestamp.
/// The existing host owns node/executor/clock/ingress and lifecycle
/// generations. Host serializes lifecycle transitions; quiesce executor before
/// clock release.
class RosMappedBooleanInputProvider final {
public:
  static core::Result<std::unique_ptr<RosMappedBooleanInputProvider>> Create(
      rclcpp::Node::SharedPtr node,
      std::shared_ptr<core::IIoSignalIngress> ingress,
      std::shared_ptr<core::IClock> clock,
      RosMappedBooleanInputBinding binding, const rclcpp::QoS &qos,
      std::optional<std::uint64_t> prepared_driver_generation = std::nullopt,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner = {});
  // Lifecycle authority captures under the delivery barrier, stages outside
  // that barrier, then verifies Matches and commits under the same barrier.
  // Activate only after every consumer and the owner slot are committed.
  struct TransportRecoverySnapshot;
  struct TransportRecoveryStage;
  [[nodiscard]] std::shared_ptr<const TransportRecoverySnapshot>
  CaptureTransportRecovery() const;
  [[nodiscard]] bool MatchesTransportRecovery(
      const TransportRecoverySnapshot &snapshot) const noexcept;
  [[nodiscard]] static core::Result<std::shared_ptr<TransportRecoveryStage>>
  StageTransportRecovery(
      std::shared_ptr<const TransportRecoverySnapshot> snapshot,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner);
  void CommitTransportRecovery(TransportRecoveryStage &stage) noexcept;
  static void ActivateTransportRecovery(TransportRecoveryStage &stage) noexcept;
  ~RosMappedBooleanInputProvider();
  RosMappedBooleanInputProvider(const RosMappedBooleanInputProvider &) = delete;
  RosMappedBooleanInputProvider &
  operator=(const RosMappedBooleanInputProvider &) = delete;
  core::IoSourceIngressSenderStatus status() const;
  bool MarkUnavailable(const core::IoSourceIncarnation &expected_incarnation,
                       std::uint64_t expected_ingress_source_sequence);
  core::Result<void>
  RebindIngress(std::shared_ptr<core::IIoSignalIngress> ingress,
                core::IoSourceIncarnation incarnation,
                std::uint64_t driver_observation_generation);
  bool Pause(std::uint64_t driver_observation_generation);
  /// A faulted or closing ROS owner throws its typed refusal before
  /// creating a replacement subscription.
  bool Resume(std::uint64_t driver_observation_generation);
  /// IO-1: every received message records an arrival (none: no stream).
  void SetArrivalRecorder(std::shared_ptr<core::StreamRecorder> arrivals);
  /// Compatible publishers observed for the current subscription.
  std::optional<std::size_t> matched_publishers() const noexcept;
  void Shutdown();

private:
  struct State;
  struct Entities;
  RosMappedBooleanInputProvider(std::shared_ptr<State> state,
                                std::shared_ptr<Entities> entities);
  std::shared_ptr<State> state_;
  std::shared_ptr<Entities> entities_;
};
} // namespace manyforge::roboplan_backend
