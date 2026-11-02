if(NOT DEFINED MANYFORGE_DEPLOYMENT_REVISION_TOOL OR
   NOT DEFINED MANYFORGE_TEST_BINARY_DIR)
  message(FATAL_ERROR "deployment revision test inputs are missing")
endif()

file(REMOVE_RECURSE "${MANYFORGE_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${MANYFORGE_TEST_BINARY_DIR}")
set(deployment "${MANYFORGE_TEST_BINARY_DIR}/deployment.yaml")
file(WRITE "${deployment}" [=[api_version: manyforge.deployment.v1
deployment_artifact_id: revision_tool_test
deployment_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
cell_id: revision_tool_cell
name: Revision tool test
catalogs: {records: []}
graph:
  runtime_units: []
  model_instances: []
  controllable_components: []
  control_domains:
    - control_domain_id: arm_domain
      control_domain_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
      runtime_unit_ids: []
      control_coordinator_endpoint_role: coordinator
      controller_topology_id: arm_topology
      topology_revision: sha256:1111111111111111111111111111111111111111111111111111111111111111
      containment_granularity: control_domain
  control_bindings: []
  component_attachments: []
desired_control_configurations: []
capability_provider_bindings: []
]=])

execute_process(
  COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --check --input "${deployment}"
  RESULT_VARIABLE stale_result
  OUTPUT_QUIET
  ERROR_QUIET)
if(stale_result EQUAL 0)
  message(FATAL_ERROR "zero placeholders unexpectedly passed revision check")
endif()

execute_process(
  COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --update --input "${deployment}"
  RESULT_VARIABLE update_result
  OUTPUT_VARIABLE expected_revision
  ERROR_VARIABLE update_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT update_result EQUAL 0)
  message(FATAL_ERROR "deployment revision update failed: ${update_error}")
endif()
string(LENGTH "${expected_revision}" expected_revision_length)
if(NOT expected_revision MATCHES "^sha256:[0-9a-f]+$" OR
   NOT expected_revision_length EQUAL 71)
  message(FATAL_ERROR "deployment revision update returned an invalid digest")
endif()

execute_process(
  COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --check --input "${deployment}"
  RESULT_VARIABLE check_result
  OUTPUT_VARIABLE checked_revision
  ERROR_VARIABLE check_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT check_result EQUAL 0)
  message(FATAL_ERROR "updated deployment revision did not pass: ${check_error}")
endif()
if(NOT checked_revision STREQUAL expected_revision)
  message(FATAL_ERROR "deployment update/check revisions differ")
endif()

file(READ "${deployment}" updated_deployment)
string(REGEX MATCHALL "\"deployment_revision\": \"sha256:[0-9a-f]+\""
       deployment_revision_rows "${updated_deployment}")
list(LENGTH deployment_revision_rows deployment_revision_row_count)
if(NOT deployment_revision_row_count EQUAL 1)
  message(FATAL_ERROR "update did not preserve one deployment revision")
endif()
string(REGEX MATCHALL "\"control_domain_revision\": \"sha256:[0-9a-f]+\""
       domain_revision_rows "${updated_deployment}")
list(LENGTH domain_revision_rows domain_revision_row_count)
if(NOT domain_revision_row_count EQUAL 1)
  message(FATAL_ERROR "update did not seal one nested control-domain revision")
endif()

file(APPEND "${deployment}" "\n")
execute_process(
  COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --check --input "${deployment}"
  RESULT_VARIABLE whitespace_result
  OUTPUT_VARIABLE whitespace_revision
  ERROR_VARIABLE whitespace_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT whitespace_result EQUAL 0 OR
   NOT whitespace_revision STREQUAL expected_revision)
  message(FATAL_ERROR
    "source whitespace unexpectedly changed semantic identity: ${whitespace_error}")
endif()


