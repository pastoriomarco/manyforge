#pragma once

#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/xarm_digital_bank.hpp>
#include <string>
#include <string_view>

namespace manyforge::roboplan_backend::host_detail {

inline std::string XArmDigitalServiceName(std::string_view robot,
                                          XArmDigitalBank bank, bool output) {
  return controller_runtime_namespace(robot) + "/xarm/" +
         (output ? (bank == XArmDigitalBank::kTool ? "set_tgpio_digital"
                                                  : "set_cgpio_digital")
                 : (bank == XArmDigitalBank::kTool ? "get_tgpio_digital"
                                                  : "get_cgpio_digital"));
}

} // namespace manyforge::roboplan_backend::host_detail
