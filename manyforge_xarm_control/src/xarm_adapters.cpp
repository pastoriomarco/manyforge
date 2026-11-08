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

#include <manyforge_xarm_control/xarm_adapters.hpp>

#include <chrono>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace manyforge::xarm_control {

bool xarm_controller_reachable(const std::string &address,
                               const std::chrono::nanoseconds timeout) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *found = nullptr;
  if (::getaddrinfo(address.c_str(), "502", &hints, &found) != 0 || found == nullptr)
    return false;
  bool reachable = false;
  const int fd = ::socket(found->ai_family, found->ai_socktype | SOCK_NONBLOCK, 0);
  if (fd >= 0) {
    if (::connect(fd, found->ai_addr, found->ai_addrlen) == 0) {
      reachable = true;
    } else {
      pollfd poll_fd{fd, POLLOUT, 0};
      int error = 0;
      socklen_t length = sizeof(error);
      reachable = ::poll(&poll_fd, 1,
                         static_cast<int>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(timeout)
                                 .count())) == 1 &&
                  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
    }
    ::close(fd);
  }
  ::freeaddrinfo(found);
  return reachable;
}

XArmMachine make_xarm_machine(rclcpp::Node &node, core::IClock &clock,
                              XArmMachineInputs inputs, const XArmRosOwner *ros_owner) {
  XArmMachine made;
  made.robot_states =
      std::make_shared<XArmRobotStatesTransport>(node, inputs.driver_namespace, clock, ros_owner);
  made.driver = std::make_shared<XArmDriverServices>(node, inputs.driver_namespace, ros_owner);
  made.owner = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
      made.robot_states, made.driver, std::move(inputs.lane), std::move(inputs.restart_driver),
      [address = inputs.address, probe = inputs.settings.reboot_probe_interval] {
        return xarm_controller_reachable(address, probe);
      },
      [](const std::chrono::nanoseconds duration) { std::this_thread::sleep_for(duration); },
      &clock, std::move(inputs.io_sink), inputs.settings.reboot_wait,
      inputs.settings.reboot_probe_interval, inputs.lifecycle_command_timeout,
      std::move(inputs.launch_settling)});
  made.io_states =
      std::make_shared<XArmIoStatesTransport>(node, inputs.driver_namespace, clock, made.owner, ros_owner);
  return made;
}

} // namespace manyforge::xarm_control
