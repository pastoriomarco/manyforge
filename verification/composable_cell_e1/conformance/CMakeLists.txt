cmake_minimum_required(VERSION 3.14)
project(manyforge_e1_roboplan_conformance LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Eigen3 REQUIRED)
find_package(GTest REQUIRED)
find_package(roboplan REQUIRED)
find_package(roboplan_cartesian_planning REQUIRED)
find_package(roboplan_example_models REQUIRED)
find_package(roboplan_rrt REQUIRED)
find_package(roboplan_simple_ik REQUIRED)
find_package(roboplan_toppra REQUIRED)

get_filename_component(
  MANYFORGE_REPOSITORY_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
  REALPATH)

set(BUILD_TESTING OFF)
add_subdirectory(
  "${MANYFORGE_REPOSITORY_ROOT}/manyforge_core"
  "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
  EXCLUDE_FROM_ALL)

add_library(manyforge_e1_planner_conformance_support STATIC
  "${MANYFORGE_REPOSITORY_ROOT}/manyforge_core/test_support/src/planner_backend_conformance.cpp")
target_include_directories(manyforge_e1_planner_conformance_support PUBLIC
  "${MANYFORGE_REPOSITORY_ROOT}/manyforge_core/test_support/include")
target_link_libraries(manyforge_e1_planner_conformance_support PUBLIC
  manyforge_core)

add_executable(test_e1_roboplan_candidate_conformance
  test_e1_roboplan_candidate_conformance.cpp)
target_compile_options(test_e1_roboplan_candidate_conformance PRIVATE
  -Wall -Wextra -Werror -pedantic)
target_compile_definitions(test_e1_roboplan_candidate_conformance PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(test_e1_roboplan_candidate_conformance PRIVATE
  manyforge_e1_planner_conformance_support
  manyforge::content_identity
  Eigen3::Eigen
  roboplan::roboplan
  roboplan_cartesian_planning::roboplan_cartesian_planning
  roboplan_example_models::roboplan_example_models
  roboplan_rrt::roboplan_rrt
  roboplan_simple_ik::roboplan_simple_ik
  roboplan_toppra::roboplan_toppra
  GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(test_e1_roboplan_candidate_conformance
  PROPERTIES TIMEOUT 30)
