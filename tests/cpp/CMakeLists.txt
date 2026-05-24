cmake_minimum_required(VERSION 3.14)

# Cross-package C++ integration tests. These targets intentionally live outside
# both production modules: they depend on manyforge_roboplan for snapshot
# publication / RoboPlan shadow comparison and on manyforge_collision for the
# ManyForge-owned engine under qualification.

if(NOT TARGET manyforge_roboplan)
  message(FATAL_ERROR "tests/cpp requires target manyforge_roboplan")
endif()
if(NOT TARGET manyforge_collision)
  message(FATAL_ERROR "tests/cpp requires target manyforge_collision")
endif()
if(NOT TARGET GTest::gtest_main)
  message(FATAL_ERROR "tests/cpp requires GTest::gtest_main")
endif()

include(GoogleTest)

add_executable(test_collision_engine_integration
  test_collision_engine_integration.cpp)
target_link_libraries(test_collision_engine_integration PRIVATE
  manyforge_roboplan manyforge_collision GTest::gtest_main)

# Qualification probe for the scene-aware manyforge_collision engine. Not a
# ctest case: invoked by scripts/run-collision-engine-qualification.sh so
# CI / operators can archive the JSON evidence separately from unit-test
# pass/fail output.
add_executable(perf_collision_engine_qualification
  perf/perf_collision_engine_qualification.cpp)
target_link_libraries(perf_collision_engine_qualification PRIVATE
  manyforge_roboplan manyforge_collision)

gtest_discover_tests(test_collision_engine_integration
  PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${MANYFORGE_INTEGRATION_TEST_LIB_PATHS}"
    TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
  DISCOVERY_EXTRA_ARGS --gtest_filter=*)
