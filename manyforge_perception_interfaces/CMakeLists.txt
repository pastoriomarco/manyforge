# Copyright 2026 Marco Pastorio
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.14)
project(manyforge_perception_interfaces VERSION 0.2.1)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(std_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "action/EstimateInstances.action"
  "msg/Region.msg"
  "msg/Detection.msg"
  "msg/Instance.msg"
  "msg/JobTiming.msg"
  DEPENDENCIES builtin_interfaces std_msgs geometry_msgs
)

if(BUILD_TESTING)
  find_package(ament_cmake_copyright REQUIRED)
  find_package(ament_cmake_lint_cmake REQUIRED)
  ament_copyright()
  ament_lint_cmake()
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
