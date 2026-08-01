cmake_minimum_required(VERSION 3.14)

# manyforge_collision owns the active source-only engine lane. No target in
# this directory may depend on the removed RoboPlan factory, projection,
# singleton scene, or NativeKernelBundle.
if(NOT TARGET manyforge_collision)
  message(FATAL_ERROR "tests/cpp requires target manyforge_collision")
endif()

set(_MANYFORGE_COLLISION_QUALIFICATION_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}")

# Keep historical target names buildable so an old qualification wrapper gets
# a deterministic exit-78 source gate instead of a compiler error or, worse, a
# silently reconstructed legacy owner. EXCLUDE_FROM_ALL makes clear that these
# binaries are not current qualification.
function(manyforge_add_collision_qualification_gate target source)
  add_executable(${target} EXCLUDE_FROM_ALL "${source}")
  target_include_directories(${target} PRIVATE
    "${_MANYFORGE_COLLISION_QUALIFICATION_INCLUDE_DIR}")
endfunction()

manyforge_add_collision_qualification_gate(
  perf_collision_engine_qualification
  perf/perf_collision_engine_qualification.cpp)
manyforge_add_collision_qualification_gate(
  perf_contact_grasp_validation_nonregression
  perf/perf_contact_grasp_validation_nonregression.cpp)
manyforge_add_collision_qualification_gate(
  perf_policy_support_contact_qualification
  perf/perf_policy_support_contact_qualification.cpp)
manyforge_add_collision_qualification_gate(
  perf_runtime_collision_schedule_timing
  perf/perf_runtime_collision_schedule_qualification.cpp)
manyforge_add_collision_qualification_gate(
  perf_runtime_collision_schedule_observation
  perf/perf_runtime_collision_schedule_qualification.cpp)

if(TARGET GTest::gtest_main)
  include(GoogleTest)
  add_executable(test_collision_engine_integration
    test_collision_engine_integration.cpp)
  target_include_directories(test_collision_engine_integration PRIVATE
    "${_MANYFORGE_COLLISION_QUALIFICATION_INCLUDE_DIR}")
  target_link_libraries(test_collision_engine_integration PRIVATE
    manyforge_collision GTest::gtest_main)

  if(NOT DEFINED MANYFORGE_GTEST_TIMEOUT_SECONDS)
    set(MANYFORGE_GTEST_TIMEOUT_SECONDS 60)
  endif()
  gtest_discover_tests(test_collision_engine_integration
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
    DISCOVERY_EXTRA_ARGS --gtest_filter=*)
else()
  message(STATUS
    "GTest is unavailable; owner-neutral collision integration tests are not "
    "registered (qualification gate targets remain buildable)")
endif()
