// Copyright 2026 Flexin Group SRL
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

#include <manyforge_core/ports/iio_signal_ingress.hpp>
#include <memory>
#include <optional>
#include <rclcpp/node.hpp>
#include <rclcpp/qos.hpp>
#include <string>

namespace manyforge::roboplan_backend {

/// One resolved std_msgs/Bool input on an existing host ROS node. The host
/// exclusively assigns this source and installs its unique incarnation first.
struct RosBooleanInputBinding final {
  core::IoSignalId signal_id;
  core::InterruptSourceId source_id;
  core::IoSourceIncarnation incarnation;
  std::string topic;
};

struct RosBooleanInputStatus final {
  bool accepting;
  std::uint64_t last_ingress_source_sequence;
  std::uint64_t last_enqueued_source_sequence;
  std::optional<core::IoIngressLossReason> last_loss;
};

/// Uses the existing host's shared bounded ingress directly. Its short critical
/// section is not a lock-free or real-time guarantee. The host alone drains and
/// evaluates acquisitions; original trusted ingress timestamps are preserved.
/// Translation only: no node/executor/thread, queue, timer, heartbeat, scripts,
/// Stop, journal or output authority. DDS buffering/unreported loss cannot
/// prove captured transitions. Time is first application callback ingress, not
/// device acquisition age. Use the same IClock as Core; shutdown and quiesce
/// the host's executor before releasing that clock. One source/topic is this
/// initial slice, not a restriction on the shared I/O model.
class RosBooleanInputProvider final {
public:
  static core::Result<std::unique_ptr<RosBooleanInputProvider>> Create(
      rclcpp::Node::SharedPtr node,
      std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
      RosBooleanInputBinding binding,
      const core::IoSignalDescriptorDTO &descriptor, const rclcpp::QoS &qos,
      std::optional<std::uint64_t> prepared_driver_generation = std::nullopt);
  ~RosBooleanInputProvider();
  RosBooleanInputProvider(const RosBooleanInputProvider &) = delete;
  RosBooleanInputProvider &operator=(const RosBooleanInputProvider &) = delete;

  RosBooleanInputStatus status() const;
  /// Real host/lifecycle loss with an exact ingress fence; delayed
  /// notifications cannot overtake a newer ingress sample. No graph-discovery
  /// liveness policy.
  bool MarkUnavailable(const core::IoSourceIncarnation &expected_incarnation,
                       std::uint64_t expected_ingress_source_sequence);
  /// Original Apply/domain owner only. The actual driver remains installed;
  /// old buffered callbacks retain their old immutable source incarnation.
  core::Result<void>
  RebindIngress(std::shared_ptr<core::IIoSignalIngress> ingress,
                core::IoSourceIncarnation incarnation,
                std::uint64_t driver_observation_generation);
  /// Stops accepting callbacks and enqueues an ordered disconnect fact once.
  /// Reversible gates use the actual driver owner's observation generation.
  bool Pause(std::uint64_t driver_observation_generation);
  bool Resume(std::uint64_t driver_observation_generation);
  void Shutdown();

private:
  struct State;
  struct Entities;
  RosBooleanInputProvider(std::shared_ptr<State> state,
                          std::unique_ptr<Entities> entities);
  std::shared_ptr<State> state_;
  std::unique_ptr<Entities> entities_;
};

} // namespace manyforge::roboplan_backend
