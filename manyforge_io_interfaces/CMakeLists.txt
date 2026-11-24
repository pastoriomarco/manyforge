# Copyright 2026 Marco Pastorio
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.14)
project(manyforge_io_interfaces VERSION 0.4.0)

# Buildable alone (e.g. as a PLC bridge's custom-interface workspace).
find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/IoBankState.msg"
  "msg/IoBankCommand.msg"
)

if(BUILD_TESTING)
  find_package(ament_cmake_copyright REQUIRED)
  find_package(ament_cmake_lint_cmake REQUIRED)
  ament_copyright()
  ament_lint_cmake()
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
