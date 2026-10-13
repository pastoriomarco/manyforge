cmake_minimum_required(VERSION 3.14)
project(manyforge_ros_integration LANGUAGES CXX)
cmake_policy(SET CMP0077 NEW)

# ROS dependencies introduce this ON default later in the graph. Own it before
# creating Core/Deployment targets so the first and subsequent configurations
# agree on their library types; an explicit caller selection still wins.
option(BUILD_SHARED_LIBS "Build the integration graph's shared libraries" ON)

# One graph keeps package-local tests while RoboPlan reuses the production
# targets. It does not replace the packages' independent sanitizer lanes.
option(BUILD_TESTING "Build the integration graph's package tests" ON)
if(BUILD_TESTING)
  enable_testing()
endif()
get_filename_component(MANYFORGE_INTEGRATION_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../../.." REALPATH)
set(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL ON)
set(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL ON)
set(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY ON)
set(MANYFORGE_BUILD_CCLC_RUNTIME_COMPOSITION ON)
set(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME ON)

add_subdirectory("${MANYFORGE_INTEGRATION_SOURCE_DIR}/manyforge_core" core)
add_subdirectory("${MANYFORGE_INTEGRATION_SOURCE_DIR}/manyforge_deployment" deployment)
add_subdirectory("${MANYFORGE_INTEGRATION_SOURCE_DIR}/manyforge_ros2_control_execution" adapter)
add_subdirectory("${MANYFORGE_INTEGRATION_SOURCE_DIR}/manyforge_roboplan" roboplan)
# Preserve the runtime helper's established host/worker artifact locations.
set_target_properties(manyforge_ros2_control_execution_host manyforge_native_execution_host
  manyforge_roboplan_candidate_worker
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# The existing Headless process fixture resolves this test host beside the
# native host and candidate worker. It exists only in BUILD_TESTING graphs.
if(TARGET manyforge_native_execution_host_pick_place_planning_fixture)
  set_target_properties(manyforge_native_execution_host_pick_place_planning_fixture
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# Report-only ROS hot-path measurement, invoked explicitly after cutover.
# EXCLUDE_FROM_ALL keeps measurement out of ordinary builds and tests.
add_executable(perf_ros2_control_hot_paths EXCLUDE_FROM_ALL
  "${MANYFORGE_INTEGRATION_SOURCE_DIR}/manyforge_ros2_control_execution/test/perf/perf_ros2_control_hot_paths.cpp")
target_compile_features(perf_ros2_control_hot_paths PRIVATE cxx_std_20)
target_include_directories(perf_ros2_control_hot_paths PRIVATE
  "${MANYFORGE_INTEGRATION_SOURCE_DIR}/manyforge_roboplan/test")
target_link_libraries(perf_ros2_control_hot_paths PRIVATE
  manyforge_roboplan_ros2_control_runtime manyforge_roboplan_native_runtime_support)
