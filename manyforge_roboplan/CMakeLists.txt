cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.1.0 LANGUAGES CXX)
set(COAL_DISABLE_HPP_FCL_WARNINGS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
add_compile_definitions(COAL_DISABLE_HPP_FCL_WARNINGS)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(FetchContent)

# Discover the active cmeel prefix dynamically so Python minor-version changes
# do not leak into CMake package resolution.
execute_process(
  COMMAND python3 -c
          "import site, pathlib; print(';'.join(str(pathlib.Path(p) / 'cmeel.prefix') for p in site.getsitepackages()))"
  OUTPUT_VARIABLE _CMEEL_PREFIXES
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

foreach(_prefix ${_CMEEL_PREFIXES})
  if(EXISTS "${_prefix}/include/boost/version.hpp")
    set(BOOST_ROOT "${_prefix}")
    set(BOOST_INCLUDEDIR "${_prefix}/include")
    set(BOOST_LIBRARYDIR "${_prefix}/lib")
    break()
  endif()
endforeach()

# Dependencies
find_package(Eigen3 REQUIRED)
find_package(pinocchio REQUIRED)
find_package(roboplan REQUIRED)
find_package(roboplan_rrt REQUIRED)
find_package(roboplan_toppra REQUIRED)
find_package(roboplan_simple_ik REQUIRED)
find_package(roboplan_example_models REQUIRED)
find_package(fmt CONFIG QUIET)

option(MANYFORGE_ENABLE_QUALIFICATION_PROBES
  "Build opt-in process-local qualification instrumentation" OFF)
option(BUILD_TESTING "Build unit tests" ON)

option(MANYFORGE_ENABLE_ROS2_CONTROL "Build ros2_control execution backend" OFF)
set(_MANYFORGE_ROS2_CONTROL_SOURCES "")
if(MANYFORGE_ENABLE_ROS2_CONTROL)
  find_package(ament_cmake REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(rclcpp_action REQUIRED)
  find_package(control_msgs REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(trajectory_msgs REQUIRED)
  find_package(builtin_interfaces REQUIRED)
  list(APPEND _MANYFORGE_ROS2_CONTROL_SOURCES
    src/ros2_control_kernel_factory.cpp)
endif()

if(NOT fmt_FOUND)
  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.0.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(fmt)
endif()

# manyforge_core (sibling directory)
if(NOT TARGET manyforge_core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core
                   ${CMAKE_CURRENT_BINARY_DIR}/manyforge_core)
endif()

# Planner-neutral ros2_control execution boundary. This sibling package is
# included from the RoboPlan composition build for now, but it must remain free
# of RoboPlan/Pinocchio/collision dependencies.
if(MANYFORGE_ENABLE_ROS2_CONTROL AND NOT TARGET manyforge_ros2_control_execution)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_ros2_control_execution
                   ${CMAKE_CURRENT_BINARY_DIR}/manyforge_ros2_control_execution)
endif()

# manyforge_collision is a neutral ManyForge-owned module. This package links
# it only at the composition boundary (NativeKernelFactory) so kernels created
# through the RoboPlan adapter can receive the authoritative ManyForge
# collision-validation pipeline. RoboPlan remains the planner/snapshot
# publisher; validation semantics stay behind core-owned ports.
if(NOT TARGET manyforge_collision)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_collision
                   ${CMAKE_CURRENT_BINARY_DIR}/manyforge_collision)
endif()

# Native backend library
add_library(manyforge_roboplan
  src/roboplan_planner_backend.cpp
  src/roboplan_projection_backend.cpp
  src/roboplan_focused_adapters.cpp
  src/roboplan_execution_backend.cpp
  src/roboplan_execution_state_sink.cpp
  src/roboplan_gripper_command_backend.cpp
  src/roboplan_planning_qualification.cpp
  src/native_kernel_factory.cpp
  src/collision_body_id_mapper.cpp
  ${_MANYFORGE_ROS2_CONTROL_SOURCES}
)

target_include_directories(manyforge_roboplan PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(manyforge_roboplan
  manyforge_core
  Eigen3::Eigen
  pinocchio::pinocchio
  roboplan::roboplan
  roboplan_rrt::roboplan_rrt
  roboplan_toppra::roboplan_toppra
  roboplan_simple_ik::roboplan_simple_ik
  roboplan_example_models::roboplan_example_models
  manyforge_collision
  fmt::fmt
)

if(MANYFORGE_ENABLE_ROS2_CONTROL)
  target_link_libraries(manyforge_roboplan
    manyforge_ros2_control_execution)
  target_compile_definitions(manyforge_roboplan PUBLIC MANYFORGE_ROS2_CONTROL_ENABLED)
  ament_target_dependencies(manyforge_roboplan
    rclcpp
    rclcpp_action
    control_msgs
    sensor_msgs
    trajectory_msgs
    builtin_interfaces)
endif()

# pybind11 module
set(PYBIND11_FINDPYTHON ON)
find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
find_package(pybind11 QUIET)
if(pybind11_FOUND AND Python3_FOUND AND Python3_Development.Module_FOUND)
  pybind11_add_module(manyforge_roboplan_py python/bindings.cpp)
  target_link_libraries(manyforge_roboplan_py PRIVATE manyforge_roboplan)
endif()

# The collision counter is never linked into a production target. A live RP5
# qualification process may preload it explicitly; normal runtime builds leave
# this option disabled and therefore carry no query interception overhead.
if(BUILD_TESTING OR MANYFORGE_ENABLE_QUALIFICATION_PROBES)
  add_library(roboplan_collision_counter_probe SHARED
    test/perf/roboplan_collision_counter_probe.cpp)
  target_link_libraries(roboplan_collision_counter_probe PRIVATE
    Eigen3::Eigen pinocchio::pinocchio ${CMAKE_DL_LIBS})
  set_target_properties(roboplan_collision_counter_probe PROPERTIES
    PREFIX "lib")
endif()

# Tests
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
        "Per-test timeout for manyforge_roboplan gtest cases")

    add_executable(test_native_backends test/test_native_backends.cpp)
    target_link_libraries(test_native_backends PRIVATE
      manyforge_roboplan GTest::gtest_main)

    add_executable(test_collision_body_id_mapper
      test/test_collision_body_id_mapper.cpp)
    target_link_libraries(test_collision_body_id_mapper PRIVATE
      manyforge_roboplan GTest::gtest_main)

    # Chunk 2: collision-world publisher + ledger integration tests against
    # a real RoboPlan scene. The Chunk 1 mapper unit tests above are
    # standalone; this suite needs the full scene + NativeKernelFactory.
    add_executable(test_collision_world_publisher
      test/test_collision_world_publisher.cpp)
    target_link_libraries(test_collision_world_publisher PRIVATE
      manyforge_roboplan GTest::gtest_main)

    add_executable(test_roboplan_execution_state_sink
      test/test_roboplan_execution_state_sink.cpp)
    target_link_libraries(test_roboplan_execution_state_sink PRIVATE
      manyforge_roboplan GTest::gtest_main)

    add_executable(test_roboplan_gripper_command_backend
      test/test_roboplan_gripper_command_backend.cpp)
    target_link_libraries(test_roboplan_gripper_command_backend PRIVATE
      manyforge_roboplan GTest::gtest_main)

    # Informational perf measurement for the collision-ownership Chunk 1
    # mapper. Not a ctest case: invoked by scripts/run-collision-mapper-perf.sh
    # and reports JSON to stdout / MANYFORGE_PERF_JSON_OUT. Phase 1 is
    # informational only; a future RoboPlan-aware CI lane can promote it.
    add_executable(perf_collision_mapper test/perf/perf_collision_mapper.cpp)
    target_link_libraries(perf_collision_mapper PRIVATE manyforge_roboplan)

    add_executable(perf_roboplan_planning_pipeline
      test/perf/perf_roboplan_planning_pipeline.cpp)
    target_link_libraries(perf_roboplan_planning_pipeline PRIVATE
      manyforge_roboplan ${CMAKE_DL_LIBS})
    # The benchmark deliberately pairs global operator new with malloc/free to
    # count allocations. GCC's whole-program warning cannot infer that pair.
    target_compile_options(perf_roboplan_planning_pipeline PRIVATE
      -Wno-mismatched-new-delete)

    add_executable(perf_collision_strategy
      ../manyforge_collision/test/perf/perf_collision_strategy.cpp)
    target_link_libraries(perf_collision_strategy PRIVATE
      manyforge_roboplan manyforge_collision
      roboplan_example_models::roboplan_example_models)
    target_compile_options(perf_collision_strategy PRIVATE
      -Wno-mismatched-new-delete)

    # Pinocchio/coal/roboplan .so files live in cmeel.prefix/lib.
    # Discover the active Python site-packages roots rather than hardcoding a
    # Python minor version into the test runtime path.
    set(_LIB_PATHS "$ENV{LD_LIBRARY_PATH}")
    execute_process(
      COMMAND python3 -c
              "import site, pathlib; print(';'.join(str(pathlib.Path(p) / 'cmeel.prefix/lib') for p in site.getsitepackages()))"
      OUTPUT_VARIABLE _CMEEL_LIB_DIRS
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    foreach(_dir
        ${_CMEEL_LIB_DIRS}
        "/usr/local/lib")
      if(IS_DIRECTORY "${_dir}")
        string(APPEND _LIB_PATHS ":${_dir}")
      endif()
    endforeach()

    gtest_discover_tests(test_native_backends
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)

    gtest_discover_tests(test_collision_body_id_mapper
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)

    gtest_discover_tests(test_collision_world_publisher
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)

    gtest_discover_tests(test_roboplan_execution_state_sink
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)

    gtest_discover_tests(test_roboplan_gripper_command_backend
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)

    set(MANYFORGE_INTEGRATION_TEST_LIB_PATHS "${_LIB_PATHS}")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests/cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/manyforge_integration_tests)
  endif()
endif()
