cmake_minimum_required(VERSION 3.14)
project(manyforge_component_manager VERSION 0.1.0 LANGUAGES CXX)

cmake_policy(SET CMP0077 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 3.37 REQUIRED)

set(
  MANYFORGE_PERSISTENCE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_persistence"
  CACHE PATH
  "Path to the manyforge_persistence source tree")
if(NOT TARGET manyforge::persistence)
  if(NOT EXISTS
     "${MANYFORGE_PERSISTENCE_SOURCE_DIR}/include/manyforge_persistence/sqlite_read_only_preflight.hpp")
    message(FATAL_ERROR
      "manyforge_component_manager requires the manyforge_persistence "
      "mechanical storage-primitives target")
  endif()
  add_subdirectory(
    "${MANYFORGE_PERSISTENCE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_persistence"
    EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET manyforge::persistence)
  message(FATAL_ERROR
    "manyforge_component_manager could not resolve manyforge_persistence")
endif()

set(
  MANYFORGE_CORE_INCLUDE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core/include"
  CACHE PATH
  "Path to the manyforge_core public headers")
if(NOT EXISTS
   "${MANYFORGE_CORE_INCLUDE_DIR}/manyforge_core/ports/iactive_bundle_publication_store.hpp")
  message(FATAL_ERROR
    "manyforge_component_manager requires the manyforge_core publication "
    "port headers")
endif()

set(
  MANYFORGE_DEPLOYMENT_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment"
  CACHE PATH
  "Path to the manyforge_deployment source tree")
if(NOT TARGET manyforge_deployment_admission)
  if(TARGET manyforge_deployment)
    message(FATAL_ERROR
      "manyforge_component_manager found a partial deployment target set "
      "without the focused admission target")
  endif()
  if(NOT EXISTS
     "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/include/manyforge_deployment/canonical_empty_admission.hpp")
    message(FATAL_ERROR
      "manyforge_component_manager requires the manyforge_deployment "
      "canonical-empty admission target")
  endif()
  set(_MANYFORGE_MANAGER_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_MANAGER_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_MANAGER_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  # Manager owns its focused tests. The embedded deployment target supplies
  # production admission/generation code without duplicating deployment's
  # independent test suite in this standalone CTest tree.
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_MANAGER_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_MANAGER_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge::deployment_admission)
  message(FATAL_ERROR
    "manyforge_component_manager could not resolve the focused "
    "manyforge_deployment admission target")
endif()

set(
  MANYFORGE_SCENE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_scene"
  CACHE PATH
  "Path to the manyforge_scene source tree for the value-only wire codec")
if(NOT TARGET manyforge::scene_value_codec)
  if(NOT EXISTS
     "${MANYFORGE_SCENE_SOURCE_DIR}/include/manyforge_scene/logical_scene_value_codec.hpp")
    message(FATAL_ERROR
      "manyforge_component_manager requires the logical-scene value codec")
  endif()
  set(_MANYFORGE_MANAGER_SCENE_BUILD_TESTING "${BUILD_TESTING}")
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_SCENE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_scene"
    EXCLUDE_FROM_ALL)
  set(BUILD_TESTING "${_MANYFORGE_MANAGER_SCENE_BUILD_TESTING}")
  unset(_MANYFORGE_MANAGER_SCENE_BUILD_TESTING)
endif()
if(NOT TARGET manyforge::scene_value_codec)
  message(FATAL_ERROR
    "manyforge_component_manager could not resolve the logical-scene value "
    "codec")
endif()

set(
  MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/001_manager_bootstrap_journal.sql")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA}")
file(READ
  "${MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA}"
  MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA_SQL)
string(
  FIND
  "${MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA_SQL}"
  ")mf_mbjs\""
  MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA_DELIMITER_COLLISION)
if(NOT MANYFORGE_MANAGER_BOOTSTRAP_JOURNAL_SCHEMA_DELIMITER_COLLISION
   EQUAL -1)
  message(FATAL_ERROR
    "manager bootstrap-journal schema collides with its generated-header "
    "raw-string delimiter")
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_manager_bootstrap_journal_schema.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_manager_bootstrap_journal_schema.hpp"
  @ONLY)

set(
  MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/002_active_bundle_publication_store.sql")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA}")
file(READ
  "${MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA}"
  MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA_SQL)
string(
  FIND
  "${MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA_SQL}"
  ")mf_abps\""
  MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA_DELIMITER_COLLISION)
if(NOT MANYFORGE_ACTIVE_BUNDLE_PUBLICATION_SCHEMA_DELIMITER_COLLISION
   EQUAL -1)
  message(FATAL_ERROR
    "active-bundle publication schema collides with its generated-header "
    "raw-string delimiter")
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_active_bundle_publication_schema.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_active_bundle_publication_schema.hpp"
  @ONLY)

set(
  MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/003_manager_apply_journal.sql")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA}")
file(READ
  "${MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA}"
  MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA_SQL)
string(
  FIND
  "${MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA_SQL}"
  ")mf_majs\""
  MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA_DELIMITER_COLLISION)
if(NOT MANYFORGE_MANAGER_APPLY_JOURNAL_SCHEMA_DELIMITER_COLLISION
   EQUAL -1)
  message(FATAL_ERROR
    "manager Apply-journal schema collides with its generated-header "
    "raw-string delimiter")
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_manager_apply_journal_schema.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_manager_apply_journal_schema.hpp"
  @ONLY)

add_library(manyforge_component_manager
  src/active_bundle_publication_store.cpp
  src/apply_completion.cpp
  src/canonical_empty_bootstrap_coordinator.cpp
  src/durable_store_set.cpp
  src/kernel_apply_fence_rpc_internal.cpp
  src/kernel_cell_admission_rpc_internal.cpp
  src/manager_apply_fence_coordinator.cpp
  src/manager_apply_fence_release_coordinator.cpp
  src/manager_apply_journal.cpp
  src/manager_liveness_rpc_internal.cpp
  src/manager_named_apply_coordinator.cpp
  src/manager_active_runtime_coordinator.cpp
  src/manager_planned_maintenance_coordinator.cpp
  src/manager_runtime_convergence_coordinator.cpp
  src/manager_scene_activation_coordinator.cpp
  src/manager_scene_stage_coordinator.cpp
  src/manager_selected_bundle_transition_coordinator.cpp
  src/manager_selected_runtime_composition_coordinator.cpp
  src/native_projection_rpc_internal.cpp
  src/native_projection_transition_internal.cpp
  src/manager_bootstrap_journal.cpp
  src/native_execution_runtime_unit_admission.cpp
  src/reference_runtime_unit_admission.cpp
  src/retained_native_runtime_unit_adapter.cpp
  src/retained_posix_runtime_unit_adapter.cpp
  src/runtime_convergence.cpp
  src/runtime_observation.cpp
  src/runtime_unit_adapter_router.cpp
  src/scene_staging.cpp
  src/selected_bundle_transition.cpp
  src/selected_runtime_composition_rpc_internal.cpp
  src/sqlite_store_startup_internal.cpp
  src/manager_state_marker_internal.cpp
)
add_library(
  manyforge::component_manager ALIAS manyforge_component_manager)
target_include_directories(manyforge_component_manager PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MANYFORGE_CORE_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(manyforge_component_manager PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_component_manager PRIVATE
  OpenSSL::Crypto
  SQLite::SQLite3
  manyforge::deployment_admission
  manyforge::deployment_verification_core
  manyforge::persistence
  manyforge::scene_value_codec
)
get_target_property(
  MANYFORGE_COMPONENT_MANAGER_DIRECT_LINKS
  manyforge_component_manager
  LINK_LIBRARIES)
if(NOT "manyforge::deployment_admission" IN_LIST
       MANYFORGE_COMPONENT_MANAGER_DIRECT_LINKS)
  message(FATAL_ERROR
    "manyforge_component_manager lost its focused admission dependency")
endif()
if(NOT "manyforge::deployment_verification_core" IN_LIST
       MANYFORGE_COMPONENT_MANAGER_DIRECT_LINKS)
  message(FATAL_ERROR
    "manyforge_component_manager lost its read-neutral Apply-scope "
    "derivation dependency")
endif()
if(NOT "manyforge::persistence" IN_LIST
       MANYFORGE_COMPONENT_MANAGER_DIRECT_LINKS)
  message(FATAL_ERROR
    "manyforge_component_manager lost its mechanical persistence dependency")
endif()
if("manyforge::deployment" IN_LIST MANYFORGE_COMPONENT_MANAGER_DIRECT_LINKS OR
   "manyforge_deployment" IN_LIST MANYFORGE_COMPONENT_MANAGER_DIRECT_LINKS)
  message(FATAL_ERROR
    "manyforge_component_manager must not directly link the full deployment "
    "compiler target")
endif()
set_target_properties(
  manyforge_component_manager PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_component_manager gtest cases")

  set(
    MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/canonical-empty-test-package")
  set(
    MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_MARKER
    "${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}/canonical-empty-admission.json")
  add_custom_command(
    OUTPUT "${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}"
    COMMAND
      "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "$<TARGET_FILE:manyforge_generate_canonical_empty_package>"
      --output-root
      "${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    DEPENDS manyforge_generate_canonical_empty_package
    VERBATIM)
  add_custom_target(
    manyforge_manager_test_canonical_empty_package
    DEPENDS "${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}")

  add_executable(test_runtime_observation
    test/test_runtime_observation.cpp)
  target_compile_definitions(test_runtime_observation PRIVATE
    MANYFORGE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(test_runtime_observation PRIVATE
    manyforge_component_manager GTest::gtest_main)
  gtest_discover_tests(test_runtime_observation
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  # Test-only join: generate one real retained runtime product through the
  # offline compiler, then admit it through the production focused reader.
  # The Component Manager library itself remains free of the full compiler.
  add_executable(test_reference_runtime_unit_admission
    test/test_reference_runtime_unit_admission.cpp)
  target_include_directories(test_reference_runtime_unit_admission PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_link_libraries(test_reference_runtime_unit_admission PRIVATE
    manyforge_component_manager
    manyforge::deployment
    GTest::gtest_main)
  gtest_discover_tests(test_reference_runtime_unit_admission
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_sqlite_store_startup_internal
    test/test_sqlite_store_startup_internal.cpp)
  target_include_directories(test_sqlite_store_startup_internal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_sqlite_store_startup_internal PRIVATE
    manyforge_component_manager GTest::gtest_main)
  gtest_discover_tests(test_sqlite_store_startup_internal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_bootstrap_journal
    test/test_manager_bootstrap_journal.cpp)
  add_dependencies(
    test_manager_bootstrap_journal
    manyforge_manager_test_canonical_empty_package)
  target_compile_definitions(test_manager_bootstrap_journal PRIVATE
    MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(test_manager_bootstrap_journal PRIVATE
    manyforge_component_manager
    manyforge::deployment_admission
    GTest::gtest_main
    SQLite::SQLite3)
  set(
    MANYFORGE_MANAGER_ADMISSION_LINK_MAP
    "${CMAKE_CURRENT_BINARY_DIR}/test_manager_bootstrap_journal.map")
  target_link_options(test_manager_bootstrap_journal PRIVATE
    "-Wl,-Map,${MANYFORGE_MANAGER_ADMISSION_LINK_MAP}")
  gtest_discover_tests(test_manager_bootstrap_journal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  add_test(
    NAME ManagerAdmissionLinkBoundary
    COMMAND
      "${CMAKE_COMMAND}"
      "-DMANYFORGE_LINK_MAP=${MANYFORGE_MANAGER_ADMISSION_LINK_MAP}"
      "-DMANYFORGE_BINARY=$<TARGET_FILE:test_manager_bootstrap_journal>"
      "-DMANYFORGE_NM=${CMAKE_NM}"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/verify_admission_link_boundary.cmake")

  add_executable(test_active_bundle_publication_store
    test/test_active_bundle_publication_store.cpp)
  target_link_libraries(test_active_bundle_publication_store PRIVATE
    manyforge_component_manager GTest::gtest_main)
  gtest_discover_tests(test_active_bundle_publication_store
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_apply_journal
    test/test_manager_apply_journal.cpp)
  target_link_libraries(test_manager_apply_journal PRIVATE
    manyforge_component_manager
    GTest::gtest_main
    SQLite::SQLite3)
  gtest_discover_tests(test_manager_apply_journal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_apply_fence_coordinator
    test/test_manager_apply_fence_coordinator.cpp)
  target_include_directories(test_manager_apply_fence_coordinator PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_link_libraries(test_manager_apply_fence_coordinator PRIVATE
    manyforge_component_manager
    manyforge::deployment
    GTest::gtest_main)
  gtest_discover_tests(test_manager_apply_fence_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_planned_maintenance_coordinator
    test/test_manager_planned_maintenance_coordinator.cpp)
  target_link_libraries(test_manager_planned_maintenance_coordinator PRIVATE
    manyforge_component_manager
    GTest::gtest_main)
  gtest_discover_tests(test_manager_planned_maintenance_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_runtime_convergence_coordinator
    test/test_manager_runtime_convergence_coordinator.cpp)
  target_include_directories(
    test_manager_runtime_convergence_coordinator PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_link_libraries(test_manager_runtime_convergence_coordinator PRIVATE
    manyforge_component_manager
    manyforge::deployment
    GTest::gtest_main)
  gtest_discover_tests(test_manager_runtime_convergence_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_selected_runtime_composition_rpc_internal
    test/test_selected_runtime_composition_rpc_internal.cpp)
  target_include_directories(
    test_selected_runtime_composition_rpc_internal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_selected_runtime_composition_rpc_internal PRIVATE
    manyforge_component_manager
    GTest::gtest_main)
  gtest_discover_tests(test_selected_runtime_composition_rpc_internal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_liveness_rpc_internal
    test/test_manager_liveness_rpc_internal.cpp)
  target_include_directories(
    test_manager_liveness_rpc_internal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_manager_liveness_rpc_internal PRIVATE
    manyforge_component_manager
    manyforge::content_identity
    GTest::gtest_main)
  gtest_discover_tests(test_manager_liveness_rpc_internal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_kernel_cell_admission_rpc_internal
    test/test_kernel_cell_admission_rpc_internal.cpp)
  target_include_directories(
    test_kernel_cell_admission_rpc_internal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_kernel_cell_admission_rpc_internal PRIVATE
    manyforge_component_manager
    GTest::gtest_main)
  gtest_discover_tests(test_kernel_cell_admission_rpc_internal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_kernel_apply_fence_rpc_internal
    test/test_kernel_apply_fence_rpc_internal.cpp)
  target_include_directories(
    test_kernel_apply_fence_rpc_internal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/include")
  target_link_libraries(test_kernel_apply_fence_rpc_internal PRIVATE
    manyforge_component_manager
    GTest::gtest_main)
  gtest_discover_tests(test_kernel_apply_fence_rpc_internal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_native_projection_transition_internal
    test/test_native_projection_transition_internal.cpp)
  target_include_directories(
    test_native_projection_transition_internal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_native_projection_transition_internal PRIVATE
    manyforge_component_manager
    GTest::gtest_main)
  gtest_discover_tests(test_native_projection_transition_internal
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_active_runtime_coordinator
    test/test_manager_active_runtime_coordinator.cpp)
  target_link_libraries(test_manager_active_runtime_coordinator PRIVATE
    manyforge_component_manager
    manyforge::deployment_verification_core
    GTest::gtest_main)
  gtest_discover_tests(test_manager_active_runtime_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retained_posix_runtime_unit_adapter
    test/test_retained_posix_runtime_unit_adapter.cpp)
  target_include_directories(
    test_retained_posix_runtime_unit_adapter PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_link_libraries(test_retained_posix_runtime_unit_adapter PRIVATE
    manyforge_component_manager
    manyforge::deployment
    GTest::gtest_main)
  gtest_discover_tests(test_retained_posix_runtime_unit_adapter
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_canonical_empty_bootstrap_coordinator
    test/test_canonical_empty_bootstrap_coordinator.cpp)
  add_dependencies(
    test_canonical_empty_bootstrap_coordinator
    manyforge_manager_test_canonical_empty_package)
  target_compile_definitions(
    test_canonical_empty_bootstrap_coordinator PRIVATE
    MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(
    test_canonical_empty_bootstrap_coordinator PRIVATE
    manyforge_component_manager
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_canonical_empty_bootstrap_coordinator
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_canonical_empty_bootstrap_advance
    test/test_canonical_empty_bootstrap_advance.cpp)
  add_dependencies(
    test_canonical_empty_bootstrap_advance
    manyforge_manager_test_canonical_empty_package)
  target_compile_definitions(
    test_canonical_empty_bootstrap_advance PRIVATE
    MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(
    test_canonical_empty_bootstrap_advance PRIVATE
    manyforge_component_manager
    manyforge::deployment_admission
    OpenSSL::Crypto
    GTest::gtest_main)
  gtest_discover_tests(test_canonical_empty_bootstrap_advance
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
