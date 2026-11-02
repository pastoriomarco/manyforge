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

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_roboplan/launch_contribution.hpp>
#include <manyforge_xarm_control/xarm_io_component.hpp>

namespace manyforge::xarm_control {

/// A driver package's share folder by package name (`driver.yaml` base files
/// are `<package>/<path in its share folder>`).
using DriverShare = std::function<std::filesystem::path(const std::string &package)>;

/// What the xArm adapter reads from a model's `lanes.real` row at run time
/// (the generic part of the row is `roboplan_backend::ModelLaneRow`).
struct XArmModelLaneRow final {
  /// The robot's joint count.
  int dof{};
  /// The Enable gate's measured-versus-reported joint tolerance.
  double joint_agreement_tolerance_rad{kXArmJointAgreementToleranceRad};
};

/// An I/O member's row (CONTRACTS §16): its bank and the signals its model
/// declares.
struct XArmIoRow final {
  XArmIoBank bank{XArmIoBank::kController};
  std::vector<XArmIoSignal> signals;
};

/// The adapter folder's settings (`vendor/driver.yaml`,
/// `vendor/manyforge_settings.yaml`) the adapters run with.
struct XArmVendorSettings final {
  std::string hardware_plugin;
  std::string controllers_base;
  std::string driver_parameters_base;
  XArmIoSettings io;
  /// `reboot.controller_wait_s`.
  std::chrono::nanoseconds reboot_wait{};
  /// `reboot.probe_interval_ms`: one command-port probe's connect bound and
  /// the interval between probes.
  std::chrono::nanoseconds reboot_probe_interval{};
};

/// Parses a model's `component.yaml` text: the xArm part of its `lanes.real`
/// row (`adapter: xarm`).
[[nodiscard]] core::Result<XArmModelLaneRow> parse_xarm_lane_row(const std::string &component_yaml);

/// Parses an I/O member's row as the host builds it (YAML or JSON):
/// `{adapter: xarm, bank: controller | tool, inputs: [name], outputs: [name]}`.
/// Signal names give the bit (`CIn`/`COn` 0-7, `DIn`/`DOn` 8-15, `TIn`/`TOn`
/// 0-4).
[[nodiscard]] core::Result<XArmIoRow> parse_xarm_io_row(const std::string &row_yaml);

/// Reads the adapter folder (`<share>/manyforge_xarm_control/vendor`).
[[nodiscard]] core::Result<XArmVendorSettings>
load_xarm_vendor_settings(const std::filesystem::path &vendor_dir);

/// The controllers template of one model row on any lane's hardware (Mock and
/// Isaac rows naming `vendor: xarm`; plan "Vendor driver configuration"): the
/// vendor base (or the row's own `controllers_file`) and ManyForge's
/// controller additions, the arm claiming the hardware's command interfaces
/// (`[__command_interfaces__]`). Needs the row's `vendor_model`.
[[nodiscard]] core::Result<std::string>
make_xarm_controllers(const roboplan_backend::ModelLaneRow &row,
                      const std::filesystem::path &vendor_dir,
                      const DriverShare &driver_share);

/// The launch contribution (CONTRACTS §12, the generator's controllers
/// template) of one model row: the vendor base files (read from
/// the driver packages' share folders (`driver_share`), or
/// the row's own `controllers_file`), ManyForge's additions and the row's
/// `vendor_model`, `robot_type` and `dof`; the controllers are
/// `make_xarm_controllers`' with the hardware starting inactive. Instance values are the
/// generator's tokens (`${address}`, `${namespace}`, `${hardware_component}`);
/// each joint's `goal` and `max_deceleration_on_cancel` are the generator's.
/// `arm` false: the machine's process with its arm deactivated (CONTRACTS
/// §16; `row` is still the arm model's row, for the hardware): the same
/// hardware, starting inactive, and driver parameters, but no arm controller
/// (neither its block nor its type in the manager's), so nothing ever
/// activates the hardware and the motors stay off.
[[nodiscard]] core::Result<roboplan_backend::LaunchContribution>
make_xarm_launch_contribution(const roboplan_backend::ModelLaneRow &row,
                              const std::filesystem::path &vendor_dir,
                              const DriverShare &driver_share, bool arm);

} // namespace manyforge::xarm_control
