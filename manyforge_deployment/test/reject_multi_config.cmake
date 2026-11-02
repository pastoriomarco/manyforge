if(NOT DEFINED MANYFORGE_TEST_SOURCE_DIR OR
   NOT DEFINED MANYFORGE_TEST_BINARY_DIR)
  message(FATAL_ERROR "multi-config rejection test is missing its paths")
endif()

file(REMOVE_RECURSE "${MANYFORGE_TEST_BINARY_DIR}")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${MANYFORGE_TEST_SOURCE_DIR}"
    -B "${MANYFORGE_TEST_BINARY_DIR}"
    -G "Ninja Multi-Config"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(configure_result EQUAL 0)
  message(FATAL_ERROR
    "build-qualified compiler provenance accepted a multi-config generator")
endif()
string(CONCAT configure_log "${configure_output}" "${configure_error}")
string(
  FIND
  "${configure_log}"
  "build-qualified compiler provenance requires a single-configuration"
  expected_failure)
if(expected_failure EQUAL -1)
  message(FATAL_ERROR
    "multi-config configure failed for an unexpected reason:\n"
    "${configure_log}")
endif()
