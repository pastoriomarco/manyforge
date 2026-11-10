cmake_minimum_required(VERSION 3.14)
project(manyforge_ros2_control_execution VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(control_msgs REQUIRED)
find_package(action_msgs REQUIRED)
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
  src/volatile_motion_trace.cpp
  src/volatile_motion_trace_file.cpp
  src/ros2_gripper_command_backend.cpp
  src/ros2_transport_resources.cpp
  src/scene_projection_worker.cpp
)

target_include_directories(manyforge_ros2_control_execution PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(manyforge_ros2_control_execution
  manyforge_core
)

target_link_libraries(manyforge_ros2_control_execution
  rclcpp::rclcpp
  rclcpp_action::rclcpp_action
  ${control_msgs_TARGETS}
  ${action_msgs_TARGETS}
  ${controller_manager_msgs_TARGETS}
  ${lifecycle_msgs_TARGETS}
  ${sensor_msgs_TARGETS}
  ${std_msgs_TARGETS}
  ${trajectory_msgs_TARGETS}
  ${builtin_interfaces_TARGETS})

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
    # Each test process runs in its own ROS domain (Fast DDS #6599);
    # same launcher as the planning backend's tests.
    find_program(MANYFORGE_RUN_RMW_ISOLATED run_rmw_isolated
      PATHS ENV AMENT_PREFIX_PATH
      PATH_SUFFIXES lib/rmw_test_fixture_implementation
      NO_DEFAULT_PATH REQUIRED)
    set(CMAKE_CROSSCOMPILING_EMULATOR
      env ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST "${MANYFORGE_RUN_RMW_ISOLATED}")
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

    # Separately compiled adapter: timing hooks never enter the product target.
    add_library(manyforge_ros2_control_leaf_timing_test_support STATIC
      src/controller_binding.cpp src/package.cpp src/ros2_control_capability.cpp
      src/ros2_control_execution_backend.cpp src/volatile_motion_trace.cpp
      src/volatile_motion_trace_file.cpp src/ros2_gripper_command_backend.cpp
      src/ros2_transport_resources.cpp src/scene_projection_worker.cpp)
    target_include_directories(manyforge_ros2_control_leaf_timing_test_support PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(manyforge_ros2_control_leaf_timing_test_support PUBLIC
      MANYFORGE_LEAF_TIMING)
    target_link_libraries(manyforge_ros2_control_leaf_timing_test_support manyforge_core)
    target_link_libraries(manyforge_ros2_control_leaf_timing_test_support
      rclcpp::rclcpp rclcpp_action::rclcpp_action
      ${control_msgs_TARGETS} ${action_msgs_TARGETS} ${controller_manager_msgs_TARGETS}
      ${lifecycle_msgs_TARGETS} ${sensor_msgs_TARGETS} ${std_msgs_TARGETS}
      ${trajectory_msgs_TARGETS} ${builtin_interfaces_TARGETS})
    add_executable(test_od55_gripper_shutdown test/test_od55_gripper_shutdown.cpp)
    target_link_libraries(test_od55_gripper_shutdown PRIVATE
      manyforge_ros2_control_leaf_timing_test_support GTest::gtest_main)
    add_test(NAME od55_gripper_shutdown
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_gripper_shutdown>
              --gtest_filter=Od55GripperShutdown.*)
    set_tests_properties(od55_gripper_shutdown PROPERTIES TIMEOUT 20)
    add_executable(test_od55_motion_leaf_timing test/test_od55_motion_leaf_timing.cpp)
    target_link_libraries(test_od55_motion_leaf_timing PRIVATE
      manyforge_ros2_control_leaf_timing_test_support GTest::gtest_main)
    add_test(NAME od55_motion_leaf_timing
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.MotionActionAndScale)
    set_tests_properties(od55_motion_leaf_timing PROPERTIES TIMEOUT 1800)
    add_test(NAME od55_motion_late_accept_race
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.CancelBeforeAcceptanceIsSentExactlyOnce)
    set_tests_properties(od55_motion_late_accept_race PROPERTIES TIMEOUT 7200)
    add_test(NAME od55_scale_leaf_deadline
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.ScaleSnapshotDeadlineExpiresWhilePublishHoldsLeaf)
    set_tests_properties(od55_scale_leaf_deadline PROPERTIES TIMEOUT 15)
    add_test(NAME od55_cancel_leaf_stamp
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.CancelStampUsesClockAfterLeafSendReturns)
    set_tests_properties(od55_cancel_leaf_stamp PROPERTIES TIMEOUT 15)
    add_test(NAME od55_goal_leaf_stop_deadline
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.StopDeadlineDoesNotInventCancelWhileGoalSendIsHeld)
    set_tests_properties(od55_goal_leaf_stop_deadline PROPERTIES TIMEOUT 15)
    add_test(NAME od55_admitted_goal_stop_deadline
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.AdmittedGoalIsCancelledOnceAfterStopDeadline)
    set_tests_properties(od55_admitted_goal_stop_deadline PROPERTIES TIMEOUT 15)
    foreach(race FencedUnsentGoalRace1000 AdmittedGoalLateCancelRace1000
                 ReplaceBeforeAdmissionStopRace1000 ReplaceAfterAdmissionStopRace1000
                 LateAcceptanceReturnsWhileStopBarrierHeld1000)
      add_test(NAME od55_${race}
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
                $<TARGET_FILE:test_od55_motion_leaf_timing>
                --gtest_filter=Od55LeafTiming.${race})
      set_tests_properties(od55_${race} PROPERTIES TIMEOUT 7200)
    endforeach()
    add_test(NAME od55_late_accept_barrier
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
              $<TARGET_FILE:test_od55_motion_leaf_timing>
              --gtest_filter=Od55LeafTiming.LateAcceptanceReturnsWhileStopBarrierHeld)
    set_tests_properties(od55_late_accept_barrier PROPERTIES TIMEOUT 15)

    add_executable(test_ros2_transport_owner test/test_ros2_transport_owner.cpp)
    target_link_libraries(test_ros2_transport_owner PRIVATE
      manyforge_ros2_control_execution GTest::gtest_main)
    gtest_discover_tests(test_ros2_transport_owner PROPERTIES TIMEOUT 10)

    add_executable(test_volatile_motion_trace test/test_volatile_motion_trace.cpp)
    target_link_libraries(test_volatile_motion_trace PRIVATE
      manyforge_ros2_control_execution GTest::gtest_main)
    gtest_discover_tests(test_volatile_motion_trace)

    add_executable(test_ros2_gripper_command_backend
      test/test_ros2_gripper_command_backend.cpp)
    target_link_libraries(test_ros2_gripper_command_backend PRIVATE
      manyforge_ros2_control_execution GTest::gtest_main)
    gtest_discover_tests(test_ros2_gripper_command_backend
      PROPERTIES TIMEOUT 180)

  endif()
endif()
