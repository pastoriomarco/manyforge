cmake_minimum_required(VERSION 3.14)
project(manyforge_component_manager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 3.37 REQUIRED)

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
  MANYFORGE_MANAGER_PERSISTENCE_SCHEMA
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/001_component_manager_foundation.sql")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_MANAGER_PERSISTENCE_SCHEMA}")
file(READ
  "${MANYFORGE_MANAGER_PERSISTENCE_SCHEMA}"
  MANYFORGE_MANAGER_PERSISTENCE_SCHEMA_SQL)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_persistence_schema.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_persistence_schema.hpp"
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

add_library(manyforge_component_manager
  src/active_bundle_publication_store.cpp
  src/persistence.cpp
  src/runtime_observation.cpp
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
)
set_target_properties(
  manyforge_component_manager PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_component_manager gtest cases")

  add_executable(test_runtime_observation
    test/test_runtime_observation.cpp)
  target_compile_definitions(test_runtime_observation PRIVATE
    MANYFORGE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(test_runtime_observation PRIVATE
    manyforge_component_manager GTest::gtest_main)
  gtest_discover_tests(test_runtime_observation
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_persistence
    test/test_persistence.cpp)
  target_compile_definitions(test_persistence PRIVATE
    MANYFORGE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(test_persistence PRIVATE
    manyforge_component_manager GTest::gtest_main)
  gtest_discover_tests(test_persistence
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_active_bundle_publication_store
    test/test_active_bundle_publication_store.cpp)
  target_link_libraries(test_active_bundle_publication_store PRIVATE
    manyforge_component_manager GTest::gtest_main)
  gtest_discover_tests(test_active_bundle_publication_store
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
