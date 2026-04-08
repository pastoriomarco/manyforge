cmake_minimum_required(VERSION 3.14)
project(manyforge_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

# Core library
add_library(manyforge_core
  src/state_machine.cpp
  src/stop_policy.cpp
  src/trajectory_cache.cpp
  src/collision_policy.cpp
  src/execution_kernel.cpp
)
target_include_directories(manyforge_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Testing
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(test_version test/test_version.cpp)
  target_link_libraries(test_version PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_version)

  add_executable(test_contracts_compile test/test_contracts_compile.cpp)
  target_link_libraries(test_contracts_compile PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_contracts_compile)

  add_executable(test_fake_clock test/test_fake_clock.cpp)
  target_link_libraries(test_fake_clock PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_fake_clock)

  add_executable(test_manual_scheduler test/test_manual_scheduler.cpp)
  target_link_libraries(test_manual_scheduler PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_manual_scheduler)

  add_executable(test_state_machine test/test_state_machine.cpp)
  target_link_libraries(test_state_machine PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_state_machine)

  add_executable(test_stop_policy test/test_stop_policy.cpp)
  target_link_libraries(test_stop_policy PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_policy)

  add_executable(test_cache test/test_cache.cpp)
  target_link_libraries(test_cache PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_cache)

  add_executable(test_collision_policy test/test_collision_policy.cpp)
  target_link_libraries(test_collision_policy PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_policy)

  add_executable(test_execution_kernel test/test_execution_kernel.cpp)
  target_link_libraries(test_execution_kernel PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_kernel)
endif()
