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

# Step 18 matched contact-grasp validation timing. The same source is copied
# unchanged into the detached 643fe9c lane; only this compile definition marks
# the current binary as able to read the new direct mechanism counters after
# (and outside) each timing window.
add_executable(perf_contact_grasp_validation_nonregression
  perf/perf_contact_grasp_validation_nonregression.cpp)
target_compile_definitions(perf_contact_grasp_validation_nonregression PRIVATE
  MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE=1)
target_link_libraries(perf_contact_grasp_validation_nonregression PRIVATE
  manyforge_roboplan manyforge_collision)

# Step 18 same-geometry policy-level support-contact benchmark. Unlike the
# contact-grasp non-regression cell, this exercises support policy evaluation
# through the production coordinator/worker-pool path and compares all policy
# modes against the pinned all-active-distance implementation.
add_executable(perf_policy_support_contact_qualification
  perf/perf_policy_support_contact_qualification.cpp)
target_compile_definitions(perf_policy_support_contact_qualification PRIVATE
  MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE=1)
target_link_libraries(perf_policy_support_contact_qualification PRIVATE
  manyforge_roboplan manyforge_collision)

# Step 18 scheduled-runtime qualification probes. The timing binary deliberately
# excludes allocation interposition and mechanism reads; the observation binary
# trades perturbation for owner-attributed allocation and direct engine-counter
# evidence. Neither target is a ctest case: the Docker runner archives their
# JSON alongside the pinned before/after qualification inputs.
add_executable(perf_runtime_collision_schedule_timing
  perf/perf_runtime_collision_schedule_qualification.cpp)
target_compile_definitions(perf_runtime_collision_schedule_timing PRIVATE
  MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION=0)
target_link_libraries(perf_runtime_collision_schedule_timing PRIVATE
  manyforge_roboplan manyforge_collision)

add_executable(perf_runtime_collision_schedule_observation
  perf/perf_runtime_collision_schedule_qualification.cpp)
target_compile_definitions(perf_runtime_collision_schedule_observation PRIVATE
  MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION=1)
target_compile_options(perf_runtime_collision_schedule_observation PRIVATE
  -Wno-mismatched-new-delete)
target_link_libraries(perf_runtime_collision_schedule_observation PRIVATE
  manyforge_roboplan manyforge_collision)

gtest_discover_tests(test_collision_engine_integration
  PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${MANYFORGE_INTEGRATION_TEST_LIB_PATHS}"
    TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
  DISCOVERY_EXTRA_ARGS --gtest_filter=*)
