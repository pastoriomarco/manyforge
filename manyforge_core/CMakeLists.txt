cmake_minimum_required(VERSION 3.14)
project(manyforge_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

set(
  MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_content_identity"
  CACHE PATH
  "Path to the shared ManyForge RFC 8785/content-identity library")
get_filename_component(
  MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
  "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}"
  ABSOLUTE)
if(NOT EXISTS
   "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include/manyforge_content_identity/content_identity.hpp")
  message(FATAL_ERROR
    "manyforge_core requires the shared content-identity source tree: "
    "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}")
endif()
if(NOT TARGET manyforge_content_identity)
  add_subdirectory(
    "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_content_identity"
    EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET manyforge::content_identity)
  message(FATAL_ERROR
    "manyforge_core could not resolve the shared content-identity target")
endif()

# Core library
add_library(manyforge_core
  src/addressed_observed_state_publication.cpp
  src/build_owned_lifecycle_plan_catalog_source.cpp
  src/component_lifecycle.cpp
  src/component_lifecycle_snapshot.cpp
  src/component_lifecycle_admission.cpp
  src/component_lifecycle_coordinator.cpp
  src/cclc_open_request.cpp
  src/component_lifecycle_journal_codec.cpp
  src/component_lifecycle_plan_catalog_codec.cpp
  src/component_lifecycle_plan_evaluator.cpp
  src/fixed_cclc_port_registry.cpp
  src/runtime_contract_manifest_codec.cpp
  src/controller_lifecycle.cpp
  src/controller_lifecycle_evaluator.cpp
  src/cclc_domain_reservation_table.cpp
  src/coordinate_state.cpp
  src/state_machine.cpp
  src/terminal_committer.cpp
  src/error_code_mapping.cpp
  src/emergency_stop_acceptance_reserve.cpp
  src/bounded_operation_control_executor.cpp
  src/bounded_stop_control_executor.cpp
  src/bounded_stop_stage_attempt_executor.cpp
  src/execution_admission_support.cpp
  src/execution_collision_support.cpp
  src/execution_correlation_tracker.cpp
  src/execution_gripper_support.cpp
  src/execution_interrupt_support.cpp
  src/execution_recovery_support.cpp
  src/execution_snapshot_publisher.cpp
  src/execution_stop_support.cpp
  src/execution_worker_signals.cpp
  src/external_observed_state_provider.cpp
  src/kernel_event_publisher.cpp
  src/kernel_flight_recorder.cpp
  src/kernel_event_factories.cpp
  src/kinematic_state_view_provider.cpp
  src/motion_trip_stop_coordinator.cpp
  src/motion_target_resolution.cpp
  src/operator_stop_worker.cpp
  src/operation_intent.cpp
  src/participant_stop_stage.cpp
  src/pick_place_candidate.cpp
  src/pick_place_planning_owner.cpp
  src/pick_place_sequence.cpp
  src/observed_robot_state_publication.cpp
  src/observed_state.cpp
  src/execution_progress_publication.cpp
  src/trajectory_progress_projection.cpp
  src/observed_robot_state_provider.cpp
  src/pose_math.cpp
  src/resource_addressed_execution_kernel.cpp
  src/resolved_stop_base_plan_codec.cpp
  src/resolved_stop_base_plan_decoder.cpp
  src/prepared_participant_stop_plan_codec.cpp
  src/prepared_planning_codec.cpp
  src/prepared_motion_evidence_codec.cpp
  src/prepared_operation_participant_codec.cpp
  src/prepared_execution.cpp
  src/route_selection.cpp
  src/route_selection_owner.cpp
  src/resource_slot_table.cpp
  src/runtime_collision_monitor.cpp
  src/runtime_participant_binding.cpp
  src/runtime_participant_policy.cpp
  src/runtime_participant_stop_readiness.cpp
  src/runtime_participant_registry.cpp
  src/runtime_collision_query_session.cpp
  src/runtime_collision_lookahead_session.cpp
  src/scene_snapshot_lease.cpp
  src/scene_frame_resolution.cpp
  src/stop_intent.cpp
  src/stop_scope_resolver.cpp
  src/stop_fault_recovery.cpp
  src/orphan_resource_reconciliation.cpp
  src/stop_surveillance.cpp
  src/stop_terminal_reducer.cpp
  src/stop_terminal_state.cpp
  src/stop_journal_projection_codec.cpp
  src/stop_policy.cpp
  src/trajectory_cache.cpp
  src/trajectory_cache_intent.cpp
  src/trajectory_cache_compatibility.cpp
  src/trajectory_sampler.cpp
  src/collision_policy_evaluator.cpp
  src/collision_validation_coordinator.cpp
  src/ordered_contact_validation.cpp
  src/parallel_collision_validation_worker_pool.cpp
  src/collision_world_snapshot_builder.cpp
  src/logical_scene_identity_internal.cpp
  src/logical_scene_projection.cpp
  src/interrupt_manager.cpp
  src/soft_on_path_stop_planner.cpp
  src/stop_profile_mapper.cpp
  src/execution_kernel.cpp
)
target_include_directories(manyforge_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(manyforge_core PRIVATE manyforge::content_identity)

# The static library is linked into both standalone test executables AND
# the pybind11 shared module (manyforge_core_py). Shared modules require
# position-independent code from every object they consume; build the
# static lib with PIC so a single .a serves both link paths.
set_target_properties(manyforge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The durable operation and CCLC journals are independent optional concrete
# adapters. Keep both outside the pure Core target so DTO/compiler consumers do
# not acquire a persistence dependency. A standalone Core build enables both;
# composed owners explicitly select only the adapter they use.
set(_MANYFORGE_CORE_SQLITE_JOURNAL_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_MANYFORGE_CORE_SQLITE_JOURNAL_DEFAULT ON)
endif()
option(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL
       "Build the SQLite-backed durable operation journal"
       ${_MANYFORGE_CORE_SQLITE_JOURNAL_DEFAULT})
option(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL
       "Build the SQLite-backed durable CCLC journal"
       ${_MANYFORGE_CORE_SQLITE_JOURNAL_DEFAULT})
unset(_MANYFORGE_CORE_SQLITE_JOURNAL_DEFAULT)
if(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL OR
   MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL)
  find_package(SQLite3 3.37 REQUIRED)
endif()
if(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL)
  add_library(manyforge_core_sqlite_operation_journal
    src/sqlite_operation_journal.cpp)
  add_library(manyforge::core_sqlite_operation_journal ALIAS
    manyforge_core_sqlite_operation_journal)
  target_include_directories(manyforge_core_sqlite_operation_journal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_core_sqlite_operation_journal
    PUBLIC manyforge_core
    PRIVATE SQLite::SQLite3 manyforge::content_identity)
  set_target_properties(manyforge_core_sqlite_operation_journal PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

if(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL)
  add_library(manyforge_core_sqlite_cclc_journal
    src/sqlite_cclc_journal.cpp)
  add_library(manyforge::core_sqlite_cclc_journal ALIAS
    manyforge_core_sqlite_cclc_journal)
  target_include_directories(manyforge_core_sqlite_cclc_journal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_core_sqlite_cclc_journal
    PUBLIC manyforge_core
    PRIVATE SQLite::SQLite3 manyforge::content_identity)
  set_target_properties(manyforge_core_sqlite_cclc_journal PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

# The resolved Stop base-plan repository is another independent optional
# concrete adapter. Keep durable filesystem effects outside the pure Core
# target and let each composed owner select the adapter explicitly.
set(_MANYFORGE_CORE_STOP_PLAN_REPOSITORY_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_MANYFORGE_CORE_STOP_PLAN_REPOSITORY_DEFAULT ON)
endif()
option(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY
       "Build the POSIX durable resolved Stop base-plan repository"
       ${_MANYFORGE_CORE_STOP_PLAN_REPOSITORY_DEFAULT})
unset(_MANYFORGE_CORE_STOP_PLAN_REPOSITORY_DEFAULT)
if(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY)
  add_library(manyforge_core_filesystem_resolved_stop_base_plan_repository
    src/filesystem_resolved_stop_base_plan_repository.cpp)
  add_library(manyforge::core_filesystem_resolved_stop_base_plan_repository
    ALIAS manyforge_core_filesystem_resolved_stop_base_plan_repository)
  target_include_directories(
    manyforge_core_filesystem_resolved_stop_base_plan_repository PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(
    manyforge_core_filesystem_resolved_stop_base_plan_repository
    PUBLIC manyforge_core
    PRIVATE manyforge::content_identity)
  set_target_properties(
    manyforge_core_filesystem_resolved_stop_base_plan_repository PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

# Testing
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/test_support/include)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_core gtest cases")

  # Planner-neutral conformance support. This target intentionally has no
  # GTest or concrete-planner dependency so any adapter test package can reuse
  # it without linking that planner into manyforge_core.
  add_library(manyforge_planner_conformance_test_support
    test_support/src/planner_backend_conformance.cpp)
  add_library(manyforge::planner_conformance_test_support ALIAS
    manyforge_planner_conformance_test_support)
  target_include_directories(manyforge_planner_conformance_test_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test_support/include>)
  target_link_libraries(manyforge_planner_conformance_test_support PUBLIC
    manyforge_core)

  add_executable(test_planner_backend_conformance
    test/test_planner_backend_conformance.cpp)
  target_link_libraries(test_planner_backend_conformance PRIVATE
    manyforge_planner_conformance_test_support GTest::gtest_main)
  gtest_discover_tests(test_planner_backend_conformance
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_version test/test_version.cpp)
  target_link_libraries(test_version PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_version
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_kinematic_state_view_provider
    test/test_kinematic_state_view_provider.cpp)
  target_link_libraries(test_kinematic_state_view_provider PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_kinematic_state_view_provider
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_external_observed_state_provider
    test/test_external_observed_state_provider.cpp)
  target_link_libraries(test_external_observed_state_provider PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_external_observed_state_provider
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_contracts_compile test/test_contracts_compile.cpp)
  target_link_libraries(test_contracts_compile PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_contracts_compile
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_composable_cell_contracts
    test/test_composable_cell_contracts.cpp)
  target_link_libraries(test_composable_cell_contracts PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_composable_cell_contracts
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_active_control_configuration
    test/test_active_control_configuration.cpp)
  target_link_libraries(test_active_control_configuration PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_active_control_configuration
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_component_lifecycle
    test/test_component_lifecycle.cpp)
  target_link_libraries(test_component_lifecycle PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_component_lifecycle
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_component_lifecycle_admission
    test/test_component_lifecycle_admission.cpp)
  target_link_libraries(test_component_lifecycle_admission PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_component_lifecycle_admission
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cclc_open_request
    test/test_cclc_open_request.cpp)
  target_link_libraries(test_cclc_open_request PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_cclc_open_request
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_component_lifecycle_plan_evaluator
    test/test_component_lifecycle_plan_evaluator.cpp)
  target_link_libraries(test_component_lifecycle_plan_evaluator PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_component_lifecycle_plan_evaluator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_controller_lifecycle_evaluator
    test/test_controller_lifecycle_evaluator.cpp)
  target_link_libraries(test_controller_lifecycle_evaluator PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_controller_lifecycle_evaluator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_fixed_cclc_port_registry
    test/test_fixed_cclc_port_registry.cpp)
  target_link_libraries(test_fixed_cclc_port_registry PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_fixed_cclc_port_registry
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_route_selection
    test/test_route_selection.cpp)
  target_link_libraries(test_route_selection PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_route_selection
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_component_lifecycle_plan_catalog_codec
    test/test_component_lifecycle_plan_catalog_codec.cpp)
  target_link_libraries(test_component_lifecycle_plan_catalog_codec PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_component_lifecycle_plan_catalog_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_contract_manifest_codec
    test/test_runtime_contract_manifest_codec.cpp)
  target_link_libraries(test_runtime_contract_manifest_codec PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_contract_manifest_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cclc_domain_reservation_table
    test/test_cclc_domain_reservation_table.cpp)
  target_link_libraries(test_cclc_domain_reservation_table PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_cclc_domain_reservation_table
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_control
    test/test_stop_control.cpp)
  target_link_libraries(test_stop_control PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_control
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_profile_mapper
    test/test_stop_profile_mapper.cpp)
  target_link_libraries(test_stop_profile_mapper PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_profile_mapper
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_coordinate_state
    test/test_coordinate_state.cpp)
  target_link_libraries(test_coordinate_state PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_coordinate_state
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_addressed_observed_state_publication
    test/test_addressed_observed_state_publication.cpp)
  target_link_libraries(test_addressed_observed_state_publication PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_addressed_observed_state_publication
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_operation_intent
    test/test_operation_intent.cpp)
  target_link_libraries(test_operation_intent PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_operation_intent
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_resource_slot_table
    test/test_resource_slot_table.cpp)
  target_link_libraries(test_resource_slot_table PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_resource_slot_table
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_emergency_stop_acceptance_reserve
    test/test_emergency_stop_acceptance_reserve.cpp)
  target_link_libraries(test_emergency_stop_acceptance_reserve PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_emergency_stop_acceptance_reserve
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_bounded_operation_control_executor
    test/test_bounded_operation_control_executor.cpp)
  target_link_libraries(test_bounded_operation_control_executor PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_bounded_operation_control_executor
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_bounded_stop_control_executor
    test/test_bounded_stop_control_executor.cpp)
  target_link_libraries(test_bounded_stop_control_executor PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_bounded_stop_control_executor
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_bounded_stop_stage_attempt_executor
    test/test_bounded_stop_stage_attempt_executor.cpp)
  target_link_libraries(test_bounded_stop_stage_attempt_executor PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_bounded_stop_stage_attempt_executor
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_intent
    test/test_stop_intent.cpp)
  target_link_libraries(test_stop_intent PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_intent
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_scope_resolver
    test/test_stop_scope_resolver.cpp)
  target_link_libraries(test_stop_scope_resolver PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_scope_resolver
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_participant_stop_stage
    test/test_participant_stop_stage.cpp)
  target_link_libraries(test_participant_stop_stage PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_participant_stop_stage
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_terminal_reducer
    test/test_stop_terminal_reducer.cpp)
  target_link_libraries(test_stop_terminal_reducer PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_stop_terminal_reducer
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_terminal_state
    test/test_stop_terminal_state.cpp)
  target_link_libraries(test_stop_terminal_state PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_stop_terminal_state
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_surveillance
    test/test_stop_surveillance.cpp)
  target_link_libraries(test_stop_surveillance PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_stop_surveillance
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_fault_recovery
    test/test_stop_fault_recovery.cpp)
  target_link_libraries(test_stop_fault_recovery PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_stop_fault_recovery
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_journal_projection_codec
    test/test_stop_journal_projection_codec.cpp)
  target_link_libraries(test_stop_journal_projection_codec PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_stop_journal_projection_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_resolved_stop_base_plan_codec
    test/test_resolved_stop_base_plan_codec.cpp)
  target_link_libraries(test_resolved_stop_base_plan_codec PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_resolved_stop_base_plan_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_prepared_planning_codec
    test/test_prepared_planning_codec.cpp)
  target_link_libraries(test_prepared_planning_codec PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_prepared_planning_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_prepared_motion_evidence_codec
    test/test_prepared_motion_evidence_codec.cpp)
  target_link_libraries(test_prepared_motion_evidence_codec PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_prepared_motion_evidence_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_prepared_operation_participant_codec
    test/test_prepared_operation_participant_codec.cpp)
  target_link_libraries(test_prepared_operation_participant_codec PRIVATE
    manyforge_core manyforge::content_identity GTest::gtest_main)
  gtest_discover_tests(test_prepared_operation_participant_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_prepared_execution
    test/test_prepared_execution.cpp)
  target_link_libraries(test_prepared_execution PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_prepared_execution
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  if(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY)
    add_executable(test_filesystem_resolved_stop_base_plan_repository
      test/test_filesystem_resolved_stop_base_plan_repository.cpp)
    target_link_libraries(
      test_filesystem_resolved_stop_base_plan_repository PRIVATE
      manyforge_core_filesystem_resolved_stop_base_plan_repository
      GTest::gtest_main)
    gtest_discover_tests(test_filesystem_resolved_stop_base_plan_repository
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  endif()

  add_executable(test_resource_addressed_execution_kernel
    test/test_resource_addressed_execution_kernel.cpp)
  target_link_libraries(test_resource_addressed_execution_kernel PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_resource_addressed_execution_kernel
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  if(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL)
    add_executable(test_sqlite_operation_journal
      test/test_sqlite_operation_journal.cpp)
    target_link_libraries(test_sqlite_operation_journal PRIVATE
      manyforge_core_sqlite_operation_journal GTest::gtest_main)
    gtest_discover_tests(test_sqlite_operation_journal
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  endif()

  if(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL)
    add_executable(test_sqlite_cclc_journal
      test/test_sqlite_cclc_journal.cpp)
    target_link_libraries(test_sqlite_cclc_journal PRIVATE
      manyforge_core_sqlite_cclc_journal GTest::gtest_main)
    gtest_discover_tests(test_sqlite_cclc_journal
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

    add_executable(test_component_lifecycle_coordinator
      test/test_component_lifecycle_coordinator.cpp)
    target_link_libraries(test_component_lifecycle_coordinator PRIVATE
      manyforge_core_sqlite_cclc_journal GTest::gtest_main)
    gtest_discover_tests(test_component_lifecycle_coordinator
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

    add_executable(test_route_selection_coordinator
      test/test_route_selection_coordinator.cpp)
    target_link_libraries(test_route_selection_coordinator PRIVATE
      manyforge_core_sqlite_cclc_journal GTest::gtest_main)
    gtest_discover_tests(test_route_selection_coordinator
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  endif()

  add_executable(test_runtime_participant_binding
    test/test_runtime_participant_binding.cpp)
  target_link_libraries(test_runtime_participant_binding PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_participant_binding
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_participant_stop_readiness
    test/test_runtime_participant_stop_readiness.cpp)
  target_link_libraries(test_runtime_participant_stop_readiness PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_participant_stop_readiness
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_participant_policy
    test/test_runtime_participant_policy.cpp)
  target_link_libraries(test_runtime_participant_policy PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_participant_policy
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_participant_registry
    test/test_runtime_participant_registry.cpp)
  target_link_libraries(test_runtime_participant_registry PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_participant_registry
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_component_manager_ports
    test/test_component_manager_ports.cpp)
  target_link_libraries(test_component_manager_ports PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_component_manager_ports
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_logical_scene_snapshot
    test/test_logical_scene_snapshot.cpp)
  target_link_libraries(test_logical_scene_snapshot PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_logical_scene_snapshot
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_logical_scene_identity
    test/test_logical_scene_identity.cpp)
  target_link_libraries(test_logical_scene_identity PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_logical_scene_identity
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_logical_scene_projection
    test/test_logical_scene_projection.cpp)
  target_link_libraries(test_logical_scene_projection PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_logical_scene_projection
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_state_sink test/test_execution_state_sink.cpp)
  target_link_libraries(test_execution_state_sink PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_state_sink
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

  add_executable(test_runtime_collision_monitor
    test/test_runtime_collision_monitor.cpp)
  target_link_libraries(test_runtime_collision_monitor PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_collision_monitor
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_collision_query_session
    test/test_runtime_collision_query_session.cpp)
  target_link_libraries(test_runtime_collision_query_session PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_collision_query_session
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_progress_publication
    test/test_execution_progress_publication.cpp)
  target_link_libraries(test_execution_progress_publication PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_progress_publication
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_trajectory_progress_projection
    test/test_trajectory_progress_projection.cpp)
  target_link_libraries(test_trajectory_progress_projection PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_trajectory_progress_projection
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_collision_lookahead_session
    test/test_runtime_collision_lookahead_session.cpp)
  target_link_libraries(test_runtime_collision_lookahead_session PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_runtime_collision_lookahead_session
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_motion_trip_stop_coordinator
    test/test_motion_trip_stop_coordinator.cpp)
  target_link_libraries(test_motion_trip_stop_coordinator PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_motion_trip_stop_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_worker_signals
    test/test_execution_worker_signals.cpp)
  target_link_libraries(test_execution_worker_signals PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_worker_signals
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_correlation_tracker
    test/test_execution_correlation_tracker.cpp)
  target_link_libraries(test_execution_correlation_tracker PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_correlation_tracker
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_snapshot_publisher
    test/test_execution_snapshot_publisher.cpp)
  target_link_libraries(test_execution_snapshot_publisher PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_snapshot_publisher
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_error_code_mapping
    test/test_error_code_mapping.cpp)
  target_link_libraries(test_error_code_mapping PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_error_code_mapping
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_kernel_event_factories
    test/test_kernel_event_factories.cpp)
  target_link_libraries(test_kernel_event_factories PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_kernel_event_factories
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_kernel_flight_recorder
    test/test_kernel_flight_recorder.cpp)
  target_link_libraries(test_kernel_flight_recorder PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_kernel_flight_recorder
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_kernel_event_publisher
    test/test_kernel_event_publisher.cpp)
  target_link_libraries(test_kernel_event_publisher PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_kernel_event_publisher
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_pose_math
    test/test_pose_math.cpp)
  target_link_libraries(test_pose_math PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_pose_math
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_pick_place_candidate
    test/test_pick_place_candidate.cpp)
  target_link_libraries(test_pick_place_candidate PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_pick_place_candidate
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_pick_place_sequence
    test/test_pick_place_sequence.cpp)
  target_link_libraries(test_pick_place_sequence PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_pick_place_sequence
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_motion_target_resolution
    test/test_motion_target_resolution.cpp)
  target_link_libraries(test_motion_target_resolution PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_motion_target_resolution
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_operator_stop_worker
    test/test_operator_stop_worker.cpp)
  target_link_libraries(test_operator_stop_worker PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_operator_stop_worker
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_observed_robot_state_publication
    test/test_observed_robot_state_publication.cpp)
  target_link_libraries(test_observed_robot_state_publication PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_observed_robot_state_publication
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cache test/test_cache.cpp)
  target_link_libraries(test_cache PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_cache
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_trajectory_cache_compatibility
    test/test_trajectory_cache_compatibility.cpp)
  target_link_libraries(test_trajectory_cache_compatibility PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_trajectory_cache_compatibility
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_trajectory_sampler test/test_trajectory_sampler.cpp)
  target_link_libraries(test_trajectory_sampler PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_trajectory_sampler
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_trajectory_sampler_parity
    test/test_trajectory_sampler_parity.cpp)
  target_link_libraries(test_trajectory_sampler_parity
    PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_trajectory_sampler_parity
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

  add_executable(test_collision_resource_manager
    test/test_collision_resource_manager.cpp)
  target_link_libraries(test_collision_resource_manager PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_collision_resource_manager
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_execution_recovery_support
    test/test_execution_recovery_support.cpp)
  target_link_libraries(test_execution_recovery_support PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_recovery_support
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_parallel_collision_validation_worker_pool
    test/test_parallel_collision_validation_worker_pool.cpp)
  target_link_libraries(test_parallel_collision_validation_worker_pool PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_parallel_collision_validation_worker_pool
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_ordered_contact_validation
    test/test_ordered_contact_validation.cpp)
  target_link_libraries(test_ordered_contact_validation PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_ordered_contact_validation
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

  add_executable(test_scene_snapshot_lease
    test/test_scene_snapshot_lease.cpp)
  target_link_libraries(test_scene_snapshot_lease PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_scene_snapshot_lease
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_interrupt_manager
    test/test_interrupt_manager.cpp)
  target_link_libraries(test_interrupt_manager PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_interrupt_manager
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_soft_on_path_stop_planner
    test/test_soft_on_path_stop_planner.cpp)
  target_link_libraries(test_soft_on_path_stop_planner PRIVATE
    manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_soft_on_path_stop_planner
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  # Phase 4.5G qualification probe. This executable is intentionally not a
  # ctest case: unit tests prove correctness invariants, while
  # scripts/run-soft-on-path-planner-qualification.sh runs this probe in a
  # release build and emits archival JSON latency evidence.
  add_executable(perf_soft_on_path_planner
    test/perf/perf_soft_on_path_planner.cpp)
  target_link_libraries(perf_soft_on_path_planner PRIVATE manyforge_core)

  add_executable(perf_kernel_event_pipeline
    test/perf/perf_kernel_event_pipeline.cpp)
  target_link_libraries(perf_kernel_event_pipeline PRIVATE manyforge_core)

  add_executable(test_execution_kernel test/test_execution_kernel.cpp)
  target_link_libraries(test_execution_kernel PRIVATE manyforge_core GTest::gtest_main)
  gtest_discover_tests(test_execution_kernel
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

endif()
