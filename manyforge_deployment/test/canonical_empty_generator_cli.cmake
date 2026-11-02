if(NOT DEFINED MANYFORGE_GENERATOR OR
   NOT DEFINED MANYFORGE_TEST_ROOT)
  message(FATAL_ERROR
    "canonical-empty CLI test requires MANYFORGE_GENERATOR and "
    "MANYFORGE_TEST_ROOT")
endif()

function(expect_generator_failure label)
  execute_process(
    COMMAND "${MANYFORGE_GENERATOR}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(result EQUAL 0)
    message(FATAL_ERROR
      "${label}: canonical-empty generator unexpectedly accepted arguments; "
      "stdout=${output}; stderr=${error}")
  endif()
endfunction()

file(REMOVE_RECURSE "${MANYFORGE_TEST_ROOT}")
file(MAKE_DIRECTORY "${MANYFORGE_TEST_ROOT}")

expect_generator_failure("missing arguments")
expect_generator_failure(
  "positional output root"
  "${MANYFORGE_TEST_ROOT}")
expect_generator_failure(
  "missing output-root value"
  "--output-root")
expect_generator_failure(
  "unknown option"
  "--other-root" "${MANYFORGE_TEST_ROOT}")
expect_generator_failure(
  "duplicate output root"
  "--output-root" "${MANYFORGE_TEST_ROOT}"
  "--output-root" "${MANYFORGE_TEST_ROOT}")
expect_generator_failure(
  "trailing semantic argument"
  "--output-root" "${MANYFORGE_TEST_ROOT}" "unexpected")

execute_process(
  COMMAND
    "${MANYFORGE_GENERATOR}"
    "--output-root" "${MANYFORGE_TEST_ROOT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "exact --output-root PATH invocation failed; "
    "stdout=${output}; stderr=${error}")
endif()
if(NOT EXISTS
   "${MANYFORGE_TEST_ROOT}/canonical-empty-admission.json")
  message(FATAL_ERROR
    "successful generator invocation omitted the commit record")
endif()

# The exact same invocation is an idempotent replay.
execute_process(
  COMMAND
    "${MANYFORGE_GENERATOR}"
    "--output-root" "${MANYFORGE_TEST_ROOT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "exact idempotent CLI replay failed; "
    "stdout=${output}; stderr=${error}")
endif()

file(MAKE_DIRECTORY "${MANYFORGE_TEST_ROOT}/ros")
expect_generator_failure("invalid ROS runtime identity"
  --output-root "${MANYFORGE_TEST_ROOT}/ros"
  --cell-id "../foreign" --ros-runtime-profile "${MANYFORGE_ROS_PROFILE}")
set(ros_args --output-root "${MANYFORGE_TEST_ROOT}/ros"
  --cell-id ros_test_runtime --ros-runtime-profile "${MANYFORGE_ROS_PROFILE}")
foreach(attempt RANGE 1 2)
  execute_process(COMMAND "${MANYFORGE_GENERATOR}" ${ros_args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "ROS bootstrap ${attempt} failed: ${error}")
  endif()
  if(attempt EQUAL 1)
    set(first_ros_output "${output}")
  elseif(NOT output STREQUAL first_ros_output)
    message(FATAL_ERROR "ROS bootstrap replay changed its identity")
  endif()
endforeach()
string(JSON ros_cell GET "${output}" cell_id)
if(NOT ros_cell STREQUAL "ros_test_runtime")
  message(FATAL_ERROR "ROS bootstrap ignored the configured runtime identity")
endif()
if(NOT EXISTS "${MANYFORGE_TEST_ROOT}/ros/canonical-empty-admission.ros_test_runtime.json")
  message(FATAL_ERROR "ROS bootstrap omitted its exact admission")
endif()

file(REMOVE_RECURSE "${MANYFORGE_TEST_ROOT}")
