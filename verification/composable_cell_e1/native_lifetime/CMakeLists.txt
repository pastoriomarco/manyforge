cmake_minimum_required(VERSION 3.14)
project(manyforge_e1_roboplan_native_lifetime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)

find_package(Eigen3 REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(roboplan REQUIRED)
find_package(roboplan_example_models REQUIRED)

enable_testing()
include(GoogleTest)

add_executable(
  test_e1_roboplan_native_lifetime
  test_e1_roboplan_native_lifetime.cpp)
target_compile_definitions(
  test_e1_roboplan_native_lifetime
  PRIVATE COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(
  test_e1_roboplan_native_lifetime
  PRIVATE
    Eigen3::Eigen
    GTest::gtest_main
    Threads::Threads
    roboplan::roboplan
    roboplan_example_models::roboplan_example_models)

gtest_discover_tests(
  test_e1_roboplan_native_lifetime
  PROPERTIES TIMEOUT 60)
