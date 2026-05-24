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
  src/collision_policy_evaluator.cpp
  src/collision_validation_coordinator.cpp
  src/parallel_collision_validation_worker_pool.cpp
  src/collision_world_snapshot_builder.cpp
  src/interrupt_manager.cpp
  src/execution_kernel.cpp
)
target_include_directories(manyforge_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# The static library is linked into both standalone test executables AND
# the pybind11 shared module (manyforge_core_py). Shared modules require
# position-independent code from every object they consume; build the
# static lib with PIC so a single .a serves both link paths.
set_target_properties(manyforge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Testing
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_core gtest cases")

  add_executable(test_version test/test_version.cpp)
  target_link_libraries(test_version PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_version
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_contracts_compile test/test_contracts_compile.cpp)
  target_link_libraries(test_contracts_compile PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_contracts_compile
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_fake_clock test/test_fake_clock.cpp)
  target_link_libraries(test_fake_clock PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_fake_clock
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manual_scheduler test/test_manual_scheduler.cpp)
  target_link_libraries(test_manual_scheduler PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_manual_scheduler
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_state_machine test/test_state_machine.cpp)
  target_link_libraries(test_state_machine PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_state_machine
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_policy test/test_stop_policy.cpp)
  target_link_libraries(test_stop_policy PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_policy
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cache test/test_cache.cpp)
  target_link_libraries(test_cache PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_cache
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_collision_policy test/test_collision_policy.cpp)
  target_link_libraries(test_collision_policy PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_policy
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_collision_policy_evaluator
    test/test_collision_policy_evaluator.cpp)
  target_link_libraries(test_collision_policy_evaluator PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_policy_evaluator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_collision_validation_coordinator
    test/test_collision_validation_coordinator.cpp)
  target_link_libraries(test_collision_validation_coordinator PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_validation_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_parallel_collision_validation_worker_pool
    test/test_parallel_collision_validation_worker_pool.cpp)
  target_link_libraries(test_parallel_collision_validation_worker_pool PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_parallel_collision_validation_worker_pool
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_collision_engine_mismatch_ledger
    test/test_collision_engine_mismatch_ledger.cpp)
  target_link_libraries(test_collision_engine_mismatch_ledger PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_engine_mismatch_ledger
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_collision_failure_archive
    test/test_collision_failure_archive.cpp)
  target_link_libraries(test_collision_failure_archive PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_failure_archive
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_content_hash test/test_content_hash.cpp)
  target_link_libraries(test_content_hash PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_content_hash
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_collision_world_snapshot
    test/test_collision_world_snapshot.cpp)
  target_link_libraries(test_collision_world_snapshot PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_world_snapshot
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_interrupt_manager
    test/test_interrupt_manager.cpp)
  target_link_libraries(test_interrupt_manager PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_interrupt_manager
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_kernel test/test_execution_kernel.cpp)
  target_link_libraries(test_execution_kernel PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_kernel
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  # ADR-0016 streaming-session contract tests (kernel lifecycle + state
  # transitions + event emission against a streaming-capable mock backend).
  add_executable(test_streaming_session test/test_streaming_session.cpp)
  target_link_libraries(test_streaming_session PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_streaming_session
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
