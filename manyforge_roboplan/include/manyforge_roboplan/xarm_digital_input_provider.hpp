// Copyright 2026 Marco Pastorio
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <manyforge_core/io_source_ingress_sender.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_roboplan/xarm_digital_bank.hpp>
#include <memory>
#include <rclcpp/node.hpp>
#include <vector>

namespace manyforge::roboplan_backend {

struct XArmDigitalInputSignal final {
  core::IoSignalDescriptorDTO descriptor;
  std::uint8_t pin;
};

struct XArmDigitalInputBinding final {
  core::InterruptSourceId source_id;
  core::IoSourceIncarnation incarnation;
  std::string service;
  XArmDigitalBank bank;
  std::vector<XArmDigitalInputSignal> signals;
};

/// A sampled xArm bank on the existing ROS executor and host scheduler.
/// Each request covers all signals in the bank; it cannot make an old result
/// fresh on a later source incarnation. The scheduler never waits on ROS.
class XArmDigitalInputProvider final {
public:
  static core::Result<std::unique_ptr<XArmDigitalInputProvider>> Create(
      rclcpp::Node::SharedPtr node,
      std::shared_ptr<core::IIoSignalIngress> ingress,
      std::shared_ptr<core::IClock> clock,
      std::shared_ptr<core::IScheduler> scheduler, XArmDigitalInputBinding binding,
      std::optional<std::uint64_t> prepared_driver_generation = std::nullopt);
  ~XArmDigitalInputProvider();
  XArmDigitalInputProvider(const XArmDigitalInputProvider &) = delete;
  XArmDigitalInputProvider &operator=(const XArmDigitalInputProvider &) = delete;

  bool Pause(std::uint64_t generation);
  bool Resume(std::uint64_t generation);
  core::Result<void> RebindIngress(
      std::shared_ptr<core::IIoSignalIngress> ingress,
      core::IoSourceIncarnation incarnation, std::uint64_t generation);
  void Shutdown();

private:
  struct State;
  explicit XArmDigitalInputProvider(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
