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

#include <manyforge_core/io_source_ingress_sender.hpp>
#include <memory>
#include <rclcpp/node.hpp>
#include <rclcpp/qos.hpp>
#include <variant>

namespace manyforge::roboplan_backend {

struct UrDigitalInputSelector final {
  std::uint8_t pin;
};
struct RosGpioBooleanInputSelector final {
  std::string group;
  std::string interface_name;
};
using RosBooleanInputSelector =
    std::variant<UrDigitalInputSelector, RosGpioBooleanInputSelector>;
struct RosMappedBooleanInputSignal final {
  core::IoSignalDescriptorDTO descriptor;
  RosBooleanInputSelector selector;
};
struct RosMappedBooleanInputBinding final {
  core::InterruptSourceId source_id;
  core::IoSourceIncarnation incarnation;
  std::string topic;
  std::vector<RosMappedBooleanInputSignal> signals;
};

/// One typed ROS source, one atomic configured-signal acquisition. UR reads
/// digital_in_states by exact pin; GPIO reads exact group/interface with finite
/// 0/1 values. Missing/ambiguous channels are invalid without a borrowed value.
/// Receipt is actual callback ingress, never a hardware acquisition timestamp.
/// The existing host owns node/executor/clock/ingress and lifecycle
/// generations. Host serializes lifecycle transitions; quiesce executor before
/// clock release.
class RosMappedBooleanInputProvider final {
public:
  static core::Result<std::unique_ptr<RosMappedBooleanInputProvider>> Create(
      rclcpp::Node::SharedPtr node,
      std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
      RosMappedBooleanInputBinding binding, const rclcpp::QoS &qos,
      std::optional<std::uint64_t> prepared_driver_generation = std::nullopt);
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
  bool Resume(std::uint64_t driver_observation_generation);
  void Shutdown();

private:
  struct State;
  struct Entities;
  RosMappedBooleanInputProvider(std::shared_ptr<State> state,
                                std::unique_ptr<Entities> entities);
  std::shared_ptr<State> state_;
  std::unique_ptr<Entities> entities_;
};
} // namespace manyforge::roboplan_backend
