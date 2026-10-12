# Copyright 2026 ManyForge contributors
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.14)
project(manyforge_perception_provider VERSION 0.1.0)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(composition_interfaces REQUIRED)
find_package(message_filters REQUIRED)
find_package(manyforge_perception_interfaces REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(vision_msgs REQUIRED)
find_package(isaac_ros_tensor_msgs REQUIRED)
find_package(OpenSSL REQUIRED)
add_library(provider_model src/provider_model.cpp)
target_compile_features(provider_model PUBLIC cxx_std_17)
target_include_directories(provider_model PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(provider_model PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(provider_model PRIVATE -Wall -Wextra -Wpedantic)

add_library(native_translation src/native_translation.cpp)
target_compile_features(native_translation PUBLIC cxx_std_17)
target_include_directories(native_translation PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(native_translation PUBLIC provider_model
  ${manyforge_perception_interfaces_TARGETS} ${sensor_msgs_TARGETS}
  ${vision_msgs_TARGETS} ${isaac_ros_tensor_msgs_TARGETS}
  PRIVATE OpenSSL::Crypto)
set_target_properties(native_translation PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(native_translation PRIVATE -Wall -Wextra -Wpedantic)

add_executable(perception_test_client src/test_client.cpp)
target_compile_features(perception_test_client PRIVATE cxx_std_17)
target_link_libraries(perception_test_client PRIVATE
  rclcpp::rclcpp rclcpp_action::rclcpp_action ${manyforge_perception_interfaces_TARGETS})
target_compile_options(perception_test_client PRIVATE -Wall -Wextra -Wpedantic)

add_library(instance_pose_sequencer SHARED src/instance_pose_sequencer.cpp)
target_compile_features(instance_pose_sequencer PRIVATE cxx_std_17)
target_link_libraries(instance_pose_sequencer PRIVATE native_translation
  rclcpp::rclcpp rclcpp_action::rclcpp_action rclcpp_components::component
  message_filters::message_filters ${composition_interfaces_TARGETS})
target_compile_options(instance_pose_sequencer PRIVATE -Wall -Wextra -Wpedantic)
rclcpp_components_register_nodes(instance_pose_sequencer
  "manyforge::perception::InstancePoseSequencer")

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_cppcheck REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  find_package(launch_testing_ament_cmake REQUIRED)
  list(APPEND AMENT_LINT_AUTO_EXCLUDE ament_cmake_cppcheck)
  ament_cppcheck(LIBRARIES googletest
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
  ament_lint_auto_find_test_dependencies()
  ament_add_gtest(test_provider_model test/test_provider_model.cpp)
  target_link_libraries(test_provider_model provider_model)
  ament_add_gtest(test_native_translation test/test_native_translation.cpp)
  target_link_libraries(test_native_translation native_translation)
  add_launch_test(test/test_action_contract.py TIMEOUT 90)
endif()

install(TARGETS provider_model native_translation EXPORT export_provider_model
  ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
install(TARGETS perception_test_client DESTINATION lib/${PROJECT_NAME})
install(TARGETS instance_pose_sequencer LIBRARY DESTINATION lib)
install(DIRECTORY launch config DESTINATION share/${PROJECT_NAME})
ament_export_targets(export_provider_model HAS_LIBRARY_TARGET)
ament_export_dependencies(
  manyforge_perception_interfaces sensor_msgs vision_msgs isaac_ros_tensor_msgs OpenSSL)
ament_package()
