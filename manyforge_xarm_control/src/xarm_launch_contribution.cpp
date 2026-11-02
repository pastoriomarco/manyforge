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

#include <manyforge_xarm_control/xarm_launch_contribution.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace manyforge::xarm_control {
namespace {

constexpr std::string_view kInvalid = "adapter_configuration_invalid";

[[nodiscard]] core::ErrorDTO invalid(const std::string &what) {
  return {std::string(kInvalid), "xArm adapter configuration: " + what, {}};
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

/// `CI3` → 3, `DO2` → 10, `TO1` → 1: the bit of the bank's digital word.
[[nodiscard]] XArmIoSignal io_signal(const std::string &name, const XArmIoBank bank,
                                     const core::IoSignalDirection direction) {
  static const std::regex kController("^([CD])([IO])([0-7])$");
  static const std::regex kTool("^T([IO])([0-4])$");
  std::smatch match;
  const bool output = direction == core::IoSignalDirection::kOutput;
  if (bank == XArmIoBank::kController && std::regex_match(name, match, kController) &&
      (match[2] == "O") == output)
    return {name, direction, (match[1] == "D" ? 8 : 0) + std::stoi(match[3])};
  if (bank == XArmIoBank::kTool && std::regex_match(name, match, kTool) &&
      (match[1] == "O") == output)
    return {name, direction, std::stoi(match[2])};
  throw std::invalid_argument("I/O signal '" + name + "' is not a signal of its bank");
}

[[nodiscard]] std::vector<XArmIoSignal> io_bank(const YAML::Node &node, const XArmIoBank bank) {
  std::vector<XArmIoSignal> signals;
  for (const auto &[key, direction] :
       {std::pair{"inputs", core::IoSignalDirection::kInput},
        std::pair{"outputs", core::IoSignalDirection::kOutput}}) {
    for (const auto &name : node[key])
      signals.push_back(io_signal(name.as<std::string>(), bank, direction));
  }
  return signals;
}

/// Deep merge: maps merge key by key, anything else is replaced.
void merge(YAML::Node base, const YAML::Node &addition) {
  for (const auto &entry : addition) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap() && base[key] && base[key].IsMap())
      merge(base[key], entry.second);
    else
      base[key] = YAML::Clone(entry.second);
  }
}

[[nodiscard]] std::string replace_all(std::string text, const std::string &from,
                                      const std::string &to) {
  for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}

[[nodiscard]] std::string emit(const YAML::Node &node) {
  YAML::Emitter out;
  out.SetNullFormat(YAML::LowerNull);
  out << node;
  return std::string(out.c_str()) + "\n";
}

} // namespace

core::Result<XArmModelLaneRow> parse_xarm_lane_row(const std::string &component_yaml) {
  try {
    const auto real = YAML::Load(component_yaml)["lanes"]["real"];
    if (!real || !real.IsMap() || !real["adapter"] || real["adapter"].as<std::string>() != "xarm")
      return core::Result<XArmModelLaneRow>::Err(invalid("no lanes.real row with adapter xarm"));
    XArmModelLaneRow row;
    row.dof = real["dof"].as<int>();
    if (real["joint_agreement_tolerance_rad"])
      row.joint_agreement_tolerance_rad = real["joint_agreement_tolerance_rad"].as<double>();
    if (row.dof < 1 || row.dof > 7 || !(row.joint_agreement_tolerance_rad > 0.0) ||
        !std::isfinite(row.joint_agreement_tolerance_rad))
      return core::Result<XArmModelLaneRow>::Err(
          invalid("lanes.real needs 1-7 joints and a positive joint agreement tolerance"));
    return core::Result<XArmModelLaneRow>::Ok(std::move(row));
  } catch (const std::exception &error) {
    return core::Result<XArmModelLaneRow>::Err(invalid(error.what()));
  }
}

core::Result<XArmIoRow> parse_xarm_io_row(const std::string &row_yaml) {
  try {
    const auto row = YAML::Load(row_yaml);
    if (!row.IsMap() || !row["adapter"] || row["adapter"].as<std::string>() != "xarm")
      return core::Result<XArmIoRow>::Err(invalid("an I/O row needs adapter xarm"));
    const auto bank = row["bank"].as<std::string>();
    if (bank != "controller" && bank != "tool")
      return core::Result<XArmIoRow>::Err(invalid("an I/O row's bank is controller or tool"));
    XArmIoRow made;
    made.bank = bank == "tool" ? XArmIoBank::kTool : XArmIoBank::kController;
    made.signals = io_bank(row, made.bank);
    return core::Result<XArmIoRow>::Ok(std::move(made));
  } catch (const std::exception &error) {
    return core::Result<XArmIoRow>::Err(invalid(error.what()));
  }
}

core::Result<XArmVendorSettings> load_xarm_vendor_settings(const std::filesystem::path &vendor_dir) {
  try {
    const auto driver = YAML::LoadFile((vendor_dir / "driver.yaml").string());
    const auto settings = YAML::LoadFile((vendor_dir / "manyforge_settings.yaml").string());
    XArmVendorSettings loaded;
    loaded.hardware_plugin = driver["hardware_plugin"].as<std::string>();
    loaded.controllers_base = driver["base_files"]["controllers"].as<std::string>();
    loaded.driver_parameters_base = driver["base_files"]["driver_parameters"].as<std::string>();
    const auto io = settings["driver"]["io_states"];
    loaded.io.rate_hz = io["rate"].as<double>();
    loaded.io.tgpio_monitor_frequency_hz = io["tgpio_monitor_frequency"].as<double>();
    loaded.reboot_wait = std::chrono::seconds(settings["reboot"]["controller_wait_s"].as<int>());
    loaded.reboot_probe_interval =
        std::chrono::milliseconds(settings["reboot"]["probe_interval_ms"].as<int>());
    return core::Result<XArmVendorSettings>::Ok(std::move(loaded));
  } catch (const std::exception &error) {
    return core::Result<XArmVendorSettings>::Err(invalid(error.what()));
  }
}

namespace {

/// The model row's adapter key (the vendor keys sit on every lane's row of an
/// xArm model).
/// A `driver.yaml` base file: `<package>/<path>` in that package's share folder.
[[nodiscard]] std::filesystem::path driver_file(const DriverShare &driver_share,
                                                const std::string &base) {
  const auto slash = base.find('/');
  return driver_share(base.substr(0, slash)) / base.substr(slash + 1);
}

[[nodiscard]] std::string row_key(const roboplan_backend::ModelLaneRow &row,
                                  const std::string_view name) {
  const auto found = row.adapter_keys.find(name);
  if (found == row.adapter_keys.end())
    throw std::invalid_argument("the lane row has no " + std::string(name));
  return found->second;
}

/// The vendor base (or the model's own file) with ManyForge's controller
/// additions, as the generator's template; the arm claims the lane
/// hardware's command interfaces (`[__command_interfaces__]`).
[[nodiscard]] YAML::Node controllers_template(const roboplan_backend::ModelLaneRow &row,
                                              const XArmVendorSettings &vendor,
                                              const YAML::Node &settings,
                                              const DriverShare &driver_share) {
  const auto base_path =
      row.controllers_file
          ? *row.controllers_file
          : driver_file(driver_share, replace_all(vendor.controllers_base, "{model}",
                                                  row_key(row, "vendor_model")));
  const auto base = YAML::Load(read_file(base_path));
  if (!base["controller_manager"] || !base[row.controller])
    throw std::invalid_argument(base_path.string() + " has no controller_manager or " +
                                row.controller);
  const auto additions = settings["controllers"];

  YAML::Node manager_block = YAML::Clone(base["controller_manager"]);
  merge(manager_block, additions["controller_manager"]);
  YAML::Node manager(YAML::NodeType::Map);
  for (const auto &entry : manager_block["ros__parameters"]) {
    const auto name = entry.first.as<std::string>();
    manager[name == row.controller ? "__controller__" : name] = entry.second;
  }

  YAML::Node arm = YAML::Clone(base[row.controller]);
  merge(arm, additions["arm_controller"]);
  auto parameters = arm["ros__parameters"];
  std::vector<std::string> joints;
  for (const auto &joint : parameters["joints"]) joints.push_back(joint.as<std::string>());
  YAML::Node joint_list(YAML::NodeType::Sequence);
  joint_list.push_back("__joint__");
  joint_list.SetStyle(YAML::EmitterStyle::Flow);
  parameters["joints"] = joint_list;
  YAML::Node command_interfaces(YAML::NodeType::Sequence);
  command_interfaces.push_back("__command_interfaces__");
  command_interfaces.SetStyle(YAML::EmitterStyle::Flow);
  parameters["command_interfaces"] = command_interfaces;
  // One joint constraint line: the vendor's per-joint entries without its
  // `goal` (the row's arrival tolerance, set by the generator) and the
  // Deployment's Stop deceleration.
  YAML::Node constraints(YAML::NodeType::Map);
  YAML::Node joint(YAML::NodeType::Map);
  for (const auto &entry : parameters["constraints"]) {
    const auto name = entry.first.as<std::string>();
    if (std::find(joints.begin(), joints.end(), name) == joints.end()) {
      constraints[name] = entry.second;
    } else if (joint.size() == 0U && entry.second.IsMap()) {
      for (const auto &field : entry.second)
        if (field.first.as<std::string>() != "goal")
          joint[field.first.as<std::string>()] = field.second;
    }
  }
  joint["max_deceleration_on_cancel"] = YAML::Node(YAML::NodeType::Null);
  joint.SetStyle(YAML::EmitterStyle::Flow);
  constraints["__joint__"] = joint;
  parameters["constraints"] = constraints;

  YAML::Node controllers(YAML::NodeType::Map);
  controllers["/**/controller_manager"]["ros__parameters"] = manager;
  controllers["/**/__controller__"] = arm;
  return controllers;
}

} // namespace

core::Result<std::string> make_xarm_controllers(const roboplan_backend::ModelLaneRow &row,
                                                const std::filesystem::path &vendor_dir,
                                                const DriverShare &driver_share) {
  using Made = core::Result<std::string>;
  try {
    const auto vendor = load_xarm_vendor_settings(vendor_dir);
    if (!vendor.ok()) return Made::Err(vendor.error());
    const auto settings = YAML::LoadFile((vendor_dir / "manyforge_settings.yaml").string());
    return Made::Ok(
        emit(controllers_template(row, vendor.value(), settings, driver_share)));
  } catch (const std::exception &error) {
    return Made::Err(invalid(error.what()));
  }
}

core::Result<roboplan_backend::LaunchContribution>
make_xarm_launch_contribution(const roboplan_backend::ModelLaneRow &row,
                              const std::filesystem::path &vendor_dir,
                              const DriverShare &driver_share, const bool arm) {
  using Made = core::Result<roboplan_backend::LaunchContribution>;
  try {
    const auto vendor = load_xarm_vendor_settings(vendor_dir);
    if (!vendor.ok()) return Made::Err(vendor.error());
    const auto settings = YAML::LoadFile((vendor_dir / "manyforge_settings.yaml").string());
    roboplan_backend::LaunchContribution made;

    // Hardware: ManyForge's parameters plus the model's joint count and type.
    made.hardware.plugin = vendor.value().hardware_plugin;
    for (const auto &entry : settings["hardware"]["parameters"])
      made.hardware.parameters.emplace(entry.first.as<std::string>(),
                                       entry.second.as<std::string>());
    made.hardware.parameters.emplace("dof", row_key(row, "dof"));
    made.hardware.parameters.emplace("robot_type", row_key(row, "robot_type"));
    made.hardware.joint_command_interfaces =
        settings["hardware"]["joint_command_interfaces"].as<std::vector<std::string>>();

    // Controllers: the vendor's, with this hardware's initial state. With
    // the arm deactivated (CONTRACTS §16) the manager loads no arm
    // controller: only the manager block, without the arm controller's type.
    auto controllers = controllers_template(row, vendor.value(), settings, driver_share);
    if (!arm) {
      controllers.remove("/**/__controller__");
      controllers["/**/controller_manager"]["ros__parameters"].remove("__controller__");
    }
    if (settings["hardware"]["initial_state"].as<std::string>() == "inactive") {
      YAML::Node inactive(YAML::NodeType::Sequence);
      inactive.push_back(std::string(roboplan_backend::kLaunchHardwareComponentToken));
      inactive.SetStyle(YAML::EmitterStyle::Flow);
      controllers["/**/controller_manager"]["ros__parameters"]["hardware_components_initial_state"]
                 ["inactive"] = inactive;
    }
    made.controllers_yaml = emit(controllers);

    // Driver parameters: the vendor base with only the listed services on and
    // the I/O stream settings.
    auto driver = YAML::Load(
        read_file(driver_file(driver_share, vendor.value().driver_parameters_base)))["ufactory_driver"];
    auto services = driver["ros__parameters"]["services"];
    std::vector<std::string> names;
    for (const auto &entry : services) names.push_back(entry.first.as<std::string>());
    for (const auto &name : names) services[name] = false;
    for (const auto &name : settings["driver"]["services"])
      services[name.as<std::string>()] = true;
    driver["ros__parameters"]["io_states"] = YAML::Clone(settings["driver"]["io_states"]);
    YAML::Node driver_parameters(YAML::NodeType::Map);
    driver_parameters["/**/ufactory_driver"] = driver;
    made.driver_parameters_yaml = emit(driver_parameters);

    for (const auto &remap : settings["remappings"])
      made.remappings.push_back({remap["from"].as<std::string>(), remap["to"].as<std::string>()});
    return Made::Ok(std::move(made));
  } catch (const std::exception &error) {
    return Made::Err(invalid(error.what()));
  }
}

} // namespace manyforge::xarm_control
