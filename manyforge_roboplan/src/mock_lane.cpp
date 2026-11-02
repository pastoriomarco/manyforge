#include <manyforge_roboplan/mock_lane.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include <manyforge_roboplan/controller_launch_artifacts.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw ControllerLaunchArtifactError("Mock lane: " + message);
}

} // namespace

LaunchContribution mock_launch_contribution(const ModelLaneRow &row, const bool arm) {
  if (row.adapter != kMockLaneAdapter) {
    fail("the row's adapter is '" + row.adapter + "', not mock");
  }
  // A vendor's model runs the vendor's controllers (`vendor`); any other
  // model names its own file.
  std::string controllers;
  if (row.vendor_controllers_yaml.has_value()) {
    controllers = *row.vendor_controllers_yaml;
  } else {
    if (!row.controllers_file.has_value()) {
      fail("the Mock row names neither a vendor nor a controllers_file");
    }
    std::ifstream stream(*row.controllers_file, std::ios::binary);
    if (!stream) fail("cannot read " + row.controllers_file->string());
    controllers.assign(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
  }
  LaunchContribution contribution;
  // lane/manyforge_settings.yaml `hardware`.
  contribution.hardware.plugin = "mock_components/GenericSystem";
  contribution.hardware.parameters = {{"calculate_dynamics", "true"}};
  contribution.hardware.joint_command_interfaces = {"position"};
  contribution.controllers_yaml = arm ? with_reset_position_writer(controllers)
                                      : std::move(controllers);
  return contribution;
}

} // namespace manyforge::roboplan_backend
