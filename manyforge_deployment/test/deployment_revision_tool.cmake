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

# Migrate an actual maintained contract, not a second synthetic schema. Only
# an explicit migration flag may supply an omitted jerk choice.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
     current_stop_source)
string(REGEX REPLACE "\"jerk_limits_enabled\"[ \t\r\n]*:[ \t\r\n]*(true|false)[ \t\r\n]*,"
       "" legacy_stop_source "${current_stop_source}")
if(legacy_stop_source STREQUAL current_stop_source)
  message(FATAL_ERROR "maintained migration fixture contains no explicit Stop choice")
endif()
set(legacy_path "${MANYFORGE_TEST_BINARY_DIR}/legacy-stop.yaml")
foreach(choice IN ITEMS on off)
  file(WRITE "${legacy_path}" "${legacy_stop_source}")
  execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --update --input "${legacy_path}"
    RESULT_VARIABLE omitted_result OUTPUT_QUIET ERROR_QUIET)
  if(omitted_result EQUAL 0)
    message(FATAL_ERROR "ordinary update silently migrated an omitted jerk choice")
  endif()
  execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --print --input "${legacy_path}"
    --migrate-controlled-stop-jerk "${choice}"
    RESULT_VARIABLE print_result OUTPUT_VARIABLE migration_revision ERROR_VARIABLE migration_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  file(READ "${legacy_path}" untouched)
  if(NOT print_result EQUAL 0 OR NOT untouched STREQUAL legacy_stop_source)
    message(FATAL_ERROR "migration preview failed or changed the input: ${migration_error}")
  endif()
  execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --update --input "${legacy_path}"
    --migrate-controlled-stop-jerk "${choice}"
    RESULT_VARIABLE migration_result OUTPUT_VARIABLE updated_revision ERROR_VARIABLE migration_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT migration_result EQUAL 0 OR NOT updated_revision STREQUAL migration_revision)
    message(FATAL_ERROR "explicit Stop migration failed: ${migration_error}")
  endif()
  execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --check --input "${legacy_path}"
    RESULT_VARIABLE check_result OUTPUT_QUIET ERROR_VARIABLE check_error)
  if(NOT check_result EQUAL 0)
    message(FATAL_ERROR "migrated artifact did not independently verify: ${check_error}")
  endif()
  file(READ "${legacy_path}" migrated)
  string(JSON row_count LENGTH "${migrated}" policies stop_stage_parameters)
  math(EXPR last_row "${row_count} - 1")
  foreach(index RANGE 0 ${last_row})
    string(JSON schema GET "${migrated}" policies stop_stage_parameters ${index}
      provider_contract_ref contract_schema_id)
    if(schema STREQUAL "manyforge.value_schema.provider_contract.motion_controlled_stop.v1")
      string(JSON enabled GET "${migrated}" policies stop_stage_parameters ${index}
        default_contract_parameters jerk_limits_enabled)
      if((choice STREQUAL "on" AND NOT enabled) OR (choice STREQUAL "off" AND enabled))
        message(FATAL_ERROR "migration did not preserve the explicit operator choice")
      endif()
    endif()
  endforeach()
  execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --update --input "${legacy_path}"
    --migrate-controlled-stop-jerk invalid RESULT_VARIABLE invalid_result OUTPUT_QUIET ERROR_QUIET)
  file(READ "${legacy_path}" after_invalid)
  if(invalid_result EQUAL 0 OR NOT after_invalid STREQUAL migrated)
    message(FATAL_ERROR "invalid migration choice changed the source")
  endif()
endforeach()

# The same CLI imports explicit angular MoveIt limits without activating them.
# Ordinary-motion jerk is a native-runtime field: since the maintained offline
# examples moved to the mock ROS lane (Q63-Q66), the import is exercised on the
# frozen pre-cutover native Lite 6 deployment until the native executor goes.
file(READ "${CMAKE_CURRENT_LIST_DIR}/fixtures/native_execution/lite6_lite_gripper_pick_and_place.native.deployment.yaml"
     current_stop_source)
set(imported_path "${MANYFORGE_TEST_BINARY_DIR}/imported-jerk.yaml")
set(moveit_path "${MANYFORGE_TEST_BINARY_DIR}/joint_limits.yaml")
file(WRITE "${imported_path}" "${current_stop_source}")
file(WRITE "${moveit_path}" "joint_limits:\n  joint1: {has_jerk_limits: true, max_jerk: 12}\n")
string(JSON runtime_id GET "${current_stop_source}" graph runtime_units 0 runtime_unit_id)
string(JSON component_id GET "${current_stop_source}" graph runtime_units 0 configuration
  motion_target_limits 0 controllable_component_id)
string(JSON target_id GET "${current_stop_source}" graph runtime_units 0 configuration
  motion_target_limits 0 command_target_id)
set(import_args --import-moveit-jerk "${moveit_path}"
  --source-id robot_moveit_config/config/joint_limits.yaml
  --runtime-unit "${runtime_id}" --component "${component_id}" --target "${target_id}"
  --joints joint1,joint2,joint3,joint4,joint5,joint6)
execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --print --input "${imported_path}"
  ${import_args} RESULT_VARIABLE preview_result OUTPUT_VARIABLE preview_revision ERROR_VARIABLE import_error)
file(READ "${imported_path}" unchanged)
if(NOT preview_result EQUAL 0 OR NOT unchanged STREQUAL current_stop_source)
  message(FATAL_ERROR "MoveIt import preview failed or wrote the source: ${import_error}")
endif()
execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --update --input "${imported_path}"
  ${import_args} RESULT_VARIABLE import_result OUTPUT_VARIABLE import_revision ERROR_VARIABLE import_error)
if(NOT import_result EQUAL 0 OR NOT import_revision STREQUAL preview_revision)
  message(FATAL_ERROR "MoveIt import did not match its preview: ${import_error}")
endif()
execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --check --input "${imported_path}"
  RESULT_VARIABLE import_check_result OUTPUT_QUIET ERROR_VARIABLE import_error)
if(NOT import_check_result EQUAL 0)
  message(FATAL_ERROR "MoveIt import did not independently verify: ${import_error}")
endif()
file(READ "${imported_path}" imported_source)
string(JSON enabled GET "${imported_source}" graph runtime_units 0 configuration
  motion_target_limits 0 ordinary_motion_jerk enabled)
string(JSON maximum GET "${imported_source}" graph runtime_units 0 configuration
  motion_target_limits 0 ordinary_motion_jerk joint_limits 0 imported maximum_rad_s3)
if(enabled OR NOT maximum EQUAL 12)
  message(FATAL_ERROR "MoveIt import activated shaping or changed its units/value")
endif()
file(WRITE "${moveit_path}" "joint_limits:\n  joint1: {has_jerk_limits: true, max_jerk: -1}\n")
execute_process(COMMAND "${MANYFORGE_DEPLOYMENT_REVISION_TOOL}" --update --input "${imported_path}"
  ${import_args} RESULT_VARIABLE invalid_result OUTPUT_QUIET ERROR_QUIET)
file(READ "${imported_path}" after_invalid)
if(invalid_result EQUAL 0 OR NOT after_invalid STREQUAL imported_source)
  message(FATAL_ERROR "invalid MoveIt import changed the deployment")
endif()
