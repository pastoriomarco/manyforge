// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0

// The vendor-neutral I/O bank adapter family `ros_io_bank` (OD-327): one
// library, registered in the ament resource index
// `manyforge_component_adapters`. An owner-less model-form I/O device whose
// `lanes.real` row names it is a bank of up to 64 inputs and 64 outputs on
// two topics under `/<component_id>/io_bank` (`manyforge_io_interfaces`).
// Its device (a PLC bridge, `sim_plc`, any ROS node) runs outside the host:
// no launch contribution.

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/io_bank_component.hpp>

#include "ros_io_bank_link.hpp"

namespace {

using manyforge::roboplan_backend::ComponentAdapterContext;
using manyforge::roboplan_backend::ComponentAdapters;

/// Between reads of the lifecycle actions' wait for a fresh sample.
constexpr std::chrono::milliseconds kIoBankPollInterval{20};

ComponentAdapters make_io_bank(const ComponentAdapterContext &context) {
  using namespace manyforge::roboplan_backend;
  if (!context.node || !context.clock)
    throw std::invalid_argument("an I/O bank needs the host's node and clock");
  if (context.owner || !context.role.empty())
    throw std::invalid_argument("an I/O bank is an owner-less component");
  const auto &id = context.component_id.value();
  auto config = io_bank_config_from_row(id, context.lane_row);
  config.action_timeout = context.lifecycle_command_timeout;
  config.pause = [] { std::this_thread::sleep_for(kIoBankPollInterval); };
  auto component = std::make_shared<IoBankComponent>(std::move(config), *context.clock);
  // The state callback: the component's bounded update under its mutex,
  // then the host's I/O sink after it is released.
  auto link = std::make_shared<RosIoBankLink>(
      context.node, id, context.clock,
      [weak = std::weak_ptr(component), sink = context.io_updates,
       id](const IoBankStateSample &sample) {
        const auto held = weak.lock();
        if (!held) return;
        const auto updates = held->OnState(sample);
        if (sink && !updates.empty()) sink(id, updates, sample.received_at_ns);
      },
      context.ros_entity_owner);
  component->Attach(link);
  return {component, nullptr, std::nullopt, std::nullopt,
          context.ros_entity_owner ? link : nullptr};
}

constexpr manyforge::roboplan_backend::ComponentAdapterFamily kFamily{
    manyforge::roboplan_backend::kIoBankAdapterFamily, &make_io_bank, nullptr, nullptr};

} // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}
