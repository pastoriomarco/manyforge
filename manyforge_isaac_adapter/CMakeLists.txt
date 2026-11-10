cmake_minimum_required(VERSION 3.14)
project(manyforge_isaac_adapter VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(yaml-cpp REQUIRED)

# The Isaac Sim lane adapter: its lane folder, launch
# contribution, ros2_control execution configuration and adapter family. It
# builds on the execution host's ros2_control lane runtime, so it is built
# inside the RoboPlan execution host graph.
if(NOT TARGET manyforge_roboplan_ros2_control_runtime)
  message(FATAL_ERROR
    "manyforge_isaac_adapter is built inside the RoboPlan execution host graph "
    "(manyforge_roboplan with MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)")
endif()

add_library(${PROJECT_NAME}
  src/isaac_backend_config.cpp
  src/isaac_lane.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(${PROJECT_NAME}
  PUBLIC manyforge_roboplan_ros2_control_runtime
  PRIVATE ament_index_cpp::ament_index_cpp yaml-cpp)

add_library(manyforge_isaac_adapter_family SHARED
  src/isaac_adapter_family.cpp)
target_link_libraries(manyforge_isaac_adapter_family PRIVATE
  ${PROJECT_NAME}
  manyforge_roboplan_ros2_control_runtime)
manyforge_register_component_adapter_family(manyforge_isaac_adapter_family isaac)

# The lane folder: driver.yaml, manyforge_settings.yaml,
# controllers template, gripper bridges, README.md.
install(DIRECTORY lane DESTINATION share/${PROJECT_NAME})
manyforge_stage_component_adapter_data(${PROJECT_NAME} lane)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_isaac_lane test/test_isaac_lane.cpp)
  target_link_libraries(test_isaac_lane ${PROJECT_NAME} yaml-cpp)
  target_compile_definitions(test_isaac_lane PRIVATE
    MANYFORGE_ISAAC_LANE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lane")

  ament_add_gtest(test_isaac_adapter_family test/test_isaac_adapter_family.cpp)
  target_link_libraries(test_isaac_adapter_family
    ${PROJECT_NAME} manyforge_roboplan_ros2_control_runtime)
  target_compile_definitions(test_isaac_adapter_family PRIVATE
    MANYFORGE_ISAAC_ADAPTER_FAMILY_LIBRARY="$<TARGET_FILE:manyforge_isaac_adapter_family>")
  add_dependencies(test_isaac_adapter_family manyforge_isaac_adapter_family)
endif()

ament_package()
