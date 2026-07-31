if(NOT DEFINED MANYFORGE_PROFILE_EMITTER OR
   NOT EXISTS "${MANYFORGE_PROFILE_EMITTER}")
  message(FATAL_ERROR "profile emitter is absent")
endif()
if(NOT DEFINED MANYFORGE_GENERATED_PROFILE OR
   NOT EXISTS "${MANYFORGE_GENERATED_PROFILE}")
  message(FATAL_ERROR "build-generated admission profile is absent")
endif()
if(NOT DEFINED MANYFORGE_TEST_ROOT OR
   MANYFORGE_TEST_ROOT STREQUAL "")
  message(FATAL_ERROR "profile determinism test root is absent")
endif()

file(MAKE_DIRECTORY "${MANYFORGE_TEST_ROOT}")
set(first "${MANYFORGE_TEST_ROOT}/first.hpp")
set(second "${MANYFORGE_TEST_ROOT}/second.hpp")
file(REMOVE "${first}" "${second}")

foreach(output IN ITEMS "${first}" "${second}")
  execute_process(
    COMMAND "${MANYFORGE_PROFILE_EMITTER}" --output "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "profile emitter failed for ${output}: ${stdout}${stderr}")
  endif()
endforeach()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E compare_files
    "${first}" "${second}"
  RESULT_VARIABLE repeated_compare)
if(NOT repeated_compare EQUAL 0)
  message(FATAL_ERROR "two profile emissions differ")
endif()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E compare_files
    "${first}" "${MANYFORGE_GENERATED_PROFILE}"
  RESULT_VARIABLE generated_compare)
if(NOT generated_compare EQUAL 0)
  message(FATAL_ERROR
    "fresh profile emission differs from the build-generated header")
endif()
