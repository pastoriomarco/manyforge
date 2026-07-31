cmake_minimum_required(VERSION 3.14)
project(manyforge_scene VERSION 0.1.0 LANGUAGES CXX)

cmake_policy(SET CMP0077 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

option(BUILD_TESTING "Build unit tests" ON)

find_package(SQLite3 3.37 REQUIRED)

set(
  MANYFORGE_CORE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core"
  CACHE PATH
  "Path to the manyforge_core source tree")
if(NOT TARGET manyforge_core)
  if(NOT EXISTS
     "${MANYFORGE_CORE_SOURCE_DIR}/include/manyforge_core/logical_scene_identity.hpp")
    message(FATAL_ERROR
      "manyforge_scene requires the public manyforge_core logical-scene "
      "identity codec")
  endif()
  set(_MANYFORGE_SCENE_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_SCENE_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_SCENE_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_CORE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_SCENE_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_SCENE_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge_core)
  message(FATAL_ERROR "manyforge_scene could not resolve manyforge_core")
endif()

set(
  MANYFORGE_PERSISTENCE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_persistence"
  CACHE PATH
  "Path to the manyforge_persistence source tree")
if(NOT TARGET manyforge::persistence)
  if(NOT EXISTS
     "${MANYFORGE_PERSISTENCE_SOURCE_DIR}/include/manyforge_persistence/sqlite_read_only_preflight.hpp")
    message(FATAL_ERROR
      "manyforge_scene requires the manyforge_persistence mechanical "
      "SQLite preflight")
  endif()
  add_subdirectory(
    "${MANYFORGE_PERSISTENCE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_persistence"
    EXCLUDE_FROM_ALL)
endif()

set(
  MANYFORGE_DEPLOYMENT_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment"
  CACHE PATH
  "Path to the manyforge_deployment source tree")
if(NOT TARGET manyforge::deployment_admission)
  if(TARGET manyforge_deployment)
    message(FATAL_ERROR
      "manyforge_scene found a partial deployment target set without the "
      "focused sealed-admission target")
  endif()
  if(NOT EXISTS
     "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/include/manyforge_deployment/canonical_empty_admission.hpp")
    message(FATAL_ERROR
      "manyforge_scene requires the sealed manyforge_deployment "
      "canonical-empty admission boundary")
  endif()
  set(_MANYFORGE_SCENE_DEPLOYMENT_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_SCENE_DEPLOYMENT_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_SCENE_DEPLOYMENT_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_SCENE_DEPLOYMENT_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_SCENE_DEPLOYMENT_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge::deployment_admission)
  message(FATAL_ERROR
    "manyforge_scene could not resolve the focused deployment admission "
    "target")
endif()
if(NOT TARGET manyforge::persistence)
  message(FATAL_ERROR
    "manyforge_scene could not resolve manyforge_persistence")
endif()

set(
  MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/001_canonical_empty_scene_store.sql")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA}")
file(READ
  "${MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA}"
  MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA_SQL)
string(
  FIND
  "${MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA_SQL}"
  ")mf_sces\""
  MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA_DELIMITER_COLLISION)
if(NOT MANYFORGE_CANONICAL_EMPTY_SCENE_SCHEMA_DELIMITER_COLLISION EQUAL -1)
  message(FATAL_ERROR
    "canonical-empty scene schema collides with its generated-header "
    "raw-string delimiter")
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_canonical_empty_scene_schema.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_canonical_empty_scene_schema.hpp"
  @ONLY)

add_library(manyforge_scene
  src/canonical_empty_scene_owner.cpp)
add_library(manyforge::scene ALIAS manyforge_scene)
target_include_directories(manyforge_scene PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(manyforge_scene PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_scene PUBLIC manyforge_core)
target_link_libraries(manyforge_scene PRIVATE
  SQLite::SQLite3
  manyforge::content_identity
  manyforge::deployment_admission
  manyforge::persistence)
set_target_properties(manyforge_scene PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_SCENE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_scene gtest cases")

  add_executable(test_canonical_empty_scene_owner
    test/test_canonical_empty_scene_owner.cpp)
  set(
    MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/canonical-empty-test-package")
  set(
    MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_MARKER
    "${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}/canonical-empty-admission.json")
  add_custom_command(
    OUTPUT "${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}"
    COMMAND
      "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "$<TARGET_FILE:manyforge_generate_canonical_empty_package>"
      --output-root
      "${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    DEPENDS manyforge_generate_canonical_empty_package
    VERBATIM)
  add_custom_target(
    manyforge_scene_test_canonical_empty_package
    DEPENDS "${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}")
  add_dependencies(
    test_canonical_empty_scene_owner
    manyforge_scene_test_canonical_empty_package)
  target_compile_definitions(test_canonical_empty_scene_owner PRIVATE
    MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(test_canonical_empty_scene_owner PRIVATE
    manyforge_scene
    manyforge::deployment_admission
    SQLite::SQLite3
    GTest::gtest_main)
  gtest_discover_tests(test_canonical_empty_scene_owner
    PROPERTIES TIMEOUT ${MANYFORGE_SCENE_GTEST_TIMEOUT_SECONDS})

  set(
    MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_component_manager"
    CACHE PATH
    "Path to the manyforge_component_manager source tree for integration proof")
  if(NOT TARGET manyforge::component_manager)
    if(NOT EXISTS
       "${MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR}/include/manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp")
      message(FATAL_ERROR
        "manyforge_scene integration proof requires the Component Manager "
        "canonical-empty coordinator")
    endif()
    set(_MANYFORGE_SCENE_MANAGER_BUILD_TESTING "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    add_subdirectory(
      "${MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}/manyforge_component_manager"
      EXCLUDE_FROM_ALL)
    set(BUILD_TESTING "${_MANYFORGE_SCENE_MANAGER_BUILD_TESTING}")
  endif()
  if(NOT TARGET manyforge::component_manager)
    message(FATAL_ERROR
      "manyforge_scene could not resolve the Component Manager integration "
      "target")
  endif()

  add_executable(test_canonical_empty_scene_manager_integration
    test/test_canonical_empty_scene_manager_integration.cpp)
  add_dependencies(
    test_canonical_empty_scene_manager_integration
    manyforge_scene_test_canonical_empty_package)
  target_compile_definitions(
    test_canonical_empty_scene_manager_integration PRIVATE
    MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(
    test_canonical_empty_scene_manager_integration PRIVATE
    manyforge_scene
    manyforge::component_manager
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_canonical_empty_scene_manager_integration
    PROPERTIES TIMEOUT ${MANYFORGE_SCENE_GTEST_TIMEOUT_SECONDS})
endif()
