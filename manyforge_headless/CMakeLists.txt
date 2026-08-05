cmake_minimum_required(VERSION 3.14)
project(manyforge_headless VERSION 0.1.0 LANGUAGES CXX)

cmake_policy(SET CMP0077 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(SQLite3 3.37 REQUIRED)
find_package(Threads REQUIRED)

set(
  MANYFORGE_DEPLOYMENT_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment"
  CACHE PATH
  "Path to the manyforge_deployment source tree")
if(NOT TARGET manyforge::deployment_verification_core)
  if(NOT EXISTS
     "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/include/manyforge_deployment/artifact_codec.hpp")
    message(FATAL_ERROR
      "manyforge_headless requires the deployment strict JSON and retained "
      "verification substrate")
  endif()
  set(_MANYFORGE_HEADLESS_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_HEADLESS_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_HEADLESS_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_HEADLESS_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_HEADLESS_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge::deployment_verification_core OR
   NOT TARGET manyforge::content_identity)
  message(FATAL_ERROR
    "manyforge_headless could not resolve its strict codec dependencies")
endif()

add_library(manyforge_headless
  src/operator_alarm_catalog.cpp
  src/operator_alarm_metrics.cpp
  src/operator_alarm_owner.cpp
  src/operator_alarm_reducer.cpp
  src/runtime_snapshot.cpp
  src/scene_runtime_state_owner.cpp
  src/source_snapshot.cpp)
add_library(manyforge::headless ALIAS manyforge_headless)
target_include_directories(manyforge_headless PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_headless PUBLIC
  manyforge::content_identity
  manyforge::deployment_verification_core)
set_target_properties(manyforge_headless PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(manyforge_node_environment_projection
  src/node_environment_projection.cpp)
add_library(manyforge::node_environment_projection ALIAS
  manyforge_node_environment_projection)
target_include_directories(manyforge_node_environment_projection PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_node_environment_projection PUBLIC
  manyforge::content_identity
  manyforge::deployment_verification_core)
set_target_properties(manyforge_node_environment_projection PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

add_library(manyforge_event_historian
  src/event_historian.cpp
  src/headless_api_core.cpp
  src/sqlite_event_history_store.cpp)
add_library(manyforge::event_historian ALIAS manyforge_event_historian)
target_include_directories(manyforge_event_historian PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_event_historian PUBLIC
  manyforge_headless
  manyforge_node_environment_projection
  SQLite::SQLite3
  Threads::Threads)
set_target_properties(manyforge_event_historian PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

option(MANYFORGE_HEADLESS_BUILD_PYTHON
       "Build the thin pybind11 headless API bridge" OFF)
if(MANYFORGE_HEADLESS_BUILD_PYTHON)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(manyforge_headless_py NO_EXTRAS python/bindings.cpp)
  target_link_libraries(manyforge_headless_py PRIVATE
    manyforge_event_historian
    pybind11::module
    pybind11::headers)
  install(TARGETS manyforge_headless_py
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .)
endif()

set(
  MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_component_manager"
  CACHE PATH
  "Path to the manyforge_component_manager source tree")
if(NOT TARGET manyforge::component_manager)
  if(NOT EXISTS
     "${MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR}/include/manyforge_component_manager/manager_active_runtime_coordinator.hpp")
    message(FATAL_ERROR
      "manyforge_headless owner publications require the Component Manager")
  endif()
  set(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_component_manager"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()

set(
  MANYFORGE_SCENE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_scene"
  CACHE PATH
  "Path to the manyforge_scene source tree")
if(NOT TARGET manyforge::scene)
  if(NOT EXISTS
     "${MANYFORGE_SCENE_SOURCE_DIR}/include/manyforge_scene/canonical_empty_scene_owner.hpp")
    message(FATAL_ERROR
      "manyforge_headless owner publications require the logical-scene owner")
  endif()
  set(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_SCENE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_scene"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge::component_manager OR NOT TARGET manyforge::scene)
  message(FATAL_ERROR
    "manyforge_headless could not resolve its source-owner dependencies")
endif()

add_library(manyforge_headless_owner_publication
  src/owner_publication.cpp)
add_library(manyforge::headless_owner_publication ALIAS
  manyforge_headless_owner_publication)
target_include_directories(manyforge_headless_owner_publication PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_headless_owner_publication PUBLIC
  manyforge_headless
  manyforge::component_manager
  manyforge::scene
  manyforge::deployment_verification_core)
set_target_properties(manyforge_headless_owner_publication PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

add_executable(manyforge_manager_daemon src/manager_daemon.cpp)
target_link_libraries(manyforge_manager_daemon PRIVATE
  manyforge_headless_owner_publication
  manyforge::deployment_admission)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_headless gtest cases")

  add_executable(test_source_snapshot test/test_source_snapshot.cpp)
  target_link_libraries(test_source_snapshot PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_source_snapshot
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_snapshot test/test_runtime_snapshot.cpp)
  target_link_libraries(test_runtime_snapshot PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_runtime_snapshot
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_scene_runtime_state_owner
    test/test_scene_runtime_state_owner.cpp)
  target_link_libraries(test_scene_runtime_state_owner PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_scene_runtime_state_owner
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_operator_alarm_catalog
    test/test_operator_alarm_catalog.cpp)
  target_compile_definitions(test_operator_alarm_catalog PRIVATE
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml")
  target_link_libraries(test_operator_alarm_catalog PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_operator_alarm_catalog
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_operator_alarm_owner
    test/test_operator_alarm_owner.cpp)
  target_compile_definitions(test_operator_alarm_owner PRIVATE
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml")
  target_link_libraries(test_operator_alarm_owner PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_operator_alarm_owner
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(operator_alarm_performance_probe
    test/operator_alarm_performance_probe.cpp)
  target_link_libraries(operator_alarm_performance_probe PRIVATE
    manyforge_headless)
  add_test(NAME operator_alarm_performance_probe
    COMMAND operator_alarm_performance_probe
      --catalog
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml"
      --iterations 10000)
  set_tests_properties(operator_alarm_performance_probe PROPERTIES
    TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_event_historian test/test_event_historian.cpp)
  target_link_libraries(test_event_historian PRIVATE
    manyforge_event_historian GTest::gtest_main)
  gtest_discover_tests(test_event_historian
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  set(
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/canonical-empty-test-package")
  set(
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_MARKER
    "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}/canonical-empty-admission.json")
  add_custom_command(
    OUTPUT "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}"
    COMMAND
      "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "$<TARGET_FILE:manyforge_generate_canonical_empty_package>"
      --output-root
      "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    DEPENDS manyforge_generate_canonical_empty_package
    VERBATIM)
  add_custom_target(manyforge_headless_test_canonical_empty_package
    DEPENDS "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}")

  add_executable(test_headless_api_core test/test_headless_api_core.cpp)
  add_dependencies(test_headless_api_core
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_headless_api_core PRIVATE
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_NODE_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_catalog.yaml")
  target_link_libraries(test_headless_api_core PRIVATE
    manyforge_event_historian
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_headless_api_core
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_owner_publication test/test_owner_publication.cpp)
  add_dependencies(test_owner_publication
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_owner_publication PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(test_owner_publication PRIVATE
    manyforge_headless_owner_publication
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_owner_publication
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_node_environment_projection
    test/test_node_environment_projection.cpp)
  add_dependencies(test_node_environment_projection
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_node_environment_projection PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_NODE_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_catalog.yaml")
  target_link_libraries(test_node_environment_projection PRIVATE
    manyforge_node_environment_projection
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_node_environment_projection
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_daemon_process
    test/test_manager_daemon_process.cpp)
  add_dependencies(test_manager_daemon_process
    manyforge_manager_daemon
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_manager_daemon_process PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON="$<TARGET_FILE:manyforge_manager_daemon>")
  target_link_libraries(test_manager_daemon_process PRIVATE
    manyforge_headless_owner_publication
    GTest::gtest_main)
  gtest_discover_tests(test_manager_daemon_process
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})
endif()
