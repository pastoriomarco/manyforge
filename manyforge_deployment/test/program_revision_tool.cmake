if(NOT DEFINED MANYFORGE_PROGRAM_REVISION_TOOL OR
   NOT DEFINED MANYFORGE_TEST_BINARY_DIR)
  message(FATAL_ERROR "program revision test inputs are missing")
endif()

file(REMOVE_RECURSE "${MANYFORGE_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${MANYFORGE_TEST_BINARY_DIR}")
set(program "${MANYFORGE_TEST_BINARY_DIR}/program.yaml")
file(WRITE "${program}" [=[api_version: manyforge.program.v1
program_id: revision_tool_test
program_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
program_profile: composable-cell-v1
name: Revision tool test
catalog: {api_version: manyforge.node_catalog.v2, content_version: 2.0.0}
component_roles: {robot: {}}
target_roles: {}
contact_policies:
  - api_version: manyforge.contact_policy.v1
    policy_id: revision_tool_policy
    version: sha256:0000000000000000000000000000000000000000000000000000000000000000
    mode: contact_grasp
    sensing_mode: geometry_only
    default_unmatched_response: hard_stop
    zones: []
tree: {node_id: sequence, instance_id: root, parameters: {}, children: []}
]=])

execute_process(
  COMMAND "${MANYFORGE_PROGRAM_REVISION_TOOL}" --check --input "${program}"
  RESULT_VARIABLE stale_result
  OUTPUT_QUIET
  ERROR_QUIET)
if(stale_result EQUAL 0)
  message(FATAL_ERROR "zero placeholder unexpectedly passed revision check")
endif()

execute_process(
  COMMAND "${MANYFORGE_PROGRAM_REVISION_TOOL}" --update --input "${program}"
  RESULT_VARIABLE update_result
  OUTPUT_VARIABLE expected_revision
  ERROR_VARIABLE update_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT update_result EQUAL 0)
  message(FATAL_ERROR "revision update failed: ${update_error}")
endif()
string(LENGTH "${expected_revision}" expected_revision_length)
if(NOT expected_revision MATCHES "^sha256:[0-9a-f]+$" OR
   NOT expected_revision_length EQUAL 71)
  message(FATAL_ERROR "revision update returned an invalid digest")
endif()

execute_process(
  COMMAND "${MANYFORGE_PROGRAM_REVISION_TOOL}" --check --input "${program}"
  RESULT_VARIABLE check_result
  OUTPUT_VARIABLE checked_revision
  ERROR_VARIABLE check_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT check_result EQUAL 0)
  message(FATAL_ERROR "updated revision did not pass: ${check_error}")
endif()
if(NOT checked_revision STREQUAL expected_revision)
  message(FATAL_ERROR "update/check revisions differ")
endif()

file(READ "${program}" updated_program)
string(REGEX MATCHALL "program_revision:" revision_rows "${updated_program}")
list(LENGTH revision_rows revision_row_count)
if(NOT revision_row_count EQUAL 1)
  message(FATAL_ERROR "revision update did not preserve one authored field")
endif()
string(REGEX MATCHALL "    version: sha256:[0-9a-f]+" policy_revision_rows
       "${updated_program}")
list(LENGTH policy_revision_rows policy_revision_row_count)
if(NOT policy_revision_row_count EQUAL 1)
  message(FATAL_ERROR "revision update did not preserve one contact-policy field")
endif()
