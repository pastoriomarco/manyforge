cmake_minimum_required(VERSION 3.14)
project(manyforge_ros2_control_execution VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(control_msgs REQUIRED)
find_package(controller_manager_msgs REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(trajectory_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)

if(NOT TARGET manyforge_core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core
                   ${CMAKE_CURRENT_BINARY_DIR}/manyforge_core)
endif()

add_library(manyforge_ros2_control_execution
  src/controller_binding.cpp
  src/package.cpp
  src/ros2_control_capability.cpp
  src/ros2_control_execution_backend.cpp
  src/scene_projection_worker.cpp
)

target_include_directories(manyforge_ros2_control_execution PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(manyforge_ros2_control_execution
  manyforge_core
)

ament_target_dependencies(manyforge_ros2_control_execution
  rclcpp
  rclcpp_action
  control_msgs
  controller_manager_msgs
  lifecycle_msgs
  sensor_msgs
  std_msgs
  trajectory_msgs
  builtin_interfaces)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME ros2_control_execution_boundary
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/test_boundary.py
            ${CMAKE_CURRENT_SOURCE_DIR})

  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    add_executable(test_ros2_control_execution_package
      test/test_package.cpp)
    target_link_libraries(test_ros2_control_execution_package PRIVATE
      manyforge_ros2_control_execution GTest::gtest_main)
    gtest_discover_tests(test_ros2_control_execution_package)

    add_executable(test_ros2_capability
      test/test_ros2_capability.cpp)
    target_link_libraries(test_ros2_capability PRIVATE
      manyforge_ros2_control_execution GTest::gtest_main)
    gtest_discover_tests(test_ros2_capability)

    add_executable(test_ros2_control_execution_backend
      test/test_ros2_control_execution_backend.cpp)
    target_link_libraries(test_ros2_control_execution_backend PRIVATE
      manyforge_ros2_control_execution GTest::gtest_main)
    gtest_discover_tests(test_ros2_control_execution_backend)

    # WS8 hot-path qualification probe. This executable is intentionally not a
    # ctest case: unit tests prove behavior, while
    # scripts/run-ros2-control-hot-path-qualification.sh runs this probe in a
    # release build and emits archival JSON allocation/latency evidence.
    add_executable(perf_ros2_control_hot_paths
      test/perf/perf_ros2_control_hot_paths.cpp)
    target_link_libraries(perf_ros2_control_hot_paths PRIVATE
      manyforge_ros2_control_execution)
  endif()
endif()
