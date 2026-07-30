cmake_minimum_required(VERSION 3.14)
project(manyforge_deployment VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(OpenSSL REQUIRED)
find_package(double-conversion CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

function(manyforge_embed_closed_schema symbol stem relative_path delimiter)
  set(schema_path "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${schema_path}")
  file(READ "${schema_path}" MANYFORGE_EMBEDDED_SCHEMA_YAML)
  set(MANYFORGE_EMBEDDED_SCHEMA_SYMBOL "${symbol}")
  set(MANYFORGE_EMBEDDED_SCHEMA_DELIMITER "${delimiter}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_closed_schema.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_${stem}_schema.hpp"
    @ONLY)
endfunction()

manyforge_embed_closed_schema(
  kEmbeddedCapabilityDefinitionSchema
  capability_definition
  resources/closed_schema/catalog/manyforge.capability_definition.v1.yaml
  mf_cap_schema)
manyforge_embed_closed_schema(
  kEmbeddedComponentTypeSchema
  component_type
  resources/closed_schema/catalog/manyforge.component_type.v1.yaml
  mf_component)
manyforge_embed_closed_schema(
  kEmbeddedControlInterfaceContractSchema
  control_interface_contract
  resources/closed_schema/catalog/manyforge.control_interface_contract.v1.yaml
  mf_ctrl_schema)
manyforge_embed_closed_schema(
  kEmbeddedControllerTopologySchema
  controller_topology
  resources/closed_schema/catalog/manyforge.controller_topology.v1.yaml
  mf_topology)
manyforge_embed_closed_schema(
  kEmbeddedModelAssetSchema
  model_asset
  resources/closed_schema/catalog/manyforge.model_asset.v1.yaml
  mf_model_schema)
manyforge_embed_closed_schema(
  kEmbeddedProviderManifestSchema
  provider_manifest
  resources/closed_schema/catalog/manyforge.provider_manifest.v1.yaml
  mf_pmanifest)
manyforge_embed_closed_schema(
  kEmbeddedRuntimeProfileSchema
  runtime_profile
  resources/closed_schema/catalog/manyforge.runtime_profile.v1.yaml
  mf_rt_profile)
manyforge_embed_closed_schema(
  kEmbeddedStopEvidencePolicySchema
  stop_evidence_policy
  resources/closed_schema/catalog/manyforge.stop_evidence_policy.v1.yaml
  mf_stop_schema)
manyforge_embed_closed_schema(
  kEmbeddedEmptyRuntimeConfigurationSchema
  empty_runtime_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.empty.v1.yaml
  mf_empty_cfg)
manyforge_embed_closed_schema(
  kEmbeddedEmptyProviderContractSchema
  empty_provider_contract
  resources/closed_schema/value/manyforge.value_schema.provider_contract.empty.v1.yaml
  mf_pempty)

set(
  MANYFORGE_CORE_INCLUDE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core/include"
  CACHE PATH
  "Path to the ROS-free manyforge_core public headers")

add_library(manyforge_deployment
  src/artifact_codec.cpp
  src/capability_definition_codec.cpp
  src/canonical_value.cpp
  src/cell_graph_codec.cpp
  src/closed_schema.cpp
  src/component_type_codec.cpp
  src/control_interface_contract_codec.cpp
  src/controller_topology_codec.cpp
  src/content_identity.cpp
  src/model_asset_codec.cpp
  src/provider_manifest_codec.cpp
  src/runtime_profile_codec.cpp
  src/stop_evidence_policy_codec.cpp
)
add_library(manyforge::deployment ALIAS manyforge_deployment)

target_include_directories(manyforge_deployment PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MANYFORGE_CORE_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(manyforge_deployment PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_deployment PUBLIC
  OpenSSL::Crypto
  PkgConfig::LIBYAML
  PkgConfig::UTF8PROC
  double-conversion::double-conversion
)
set_target_properties(
  manyforge_deployment PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_deployment gtest cases")

  add_executable(test_artifact_codec test/test_artifact_codec.cpp)
  target_link_libraries(test_artifact_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_artifact_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_content_identity test/test_content_identity.cpp)
  target_include_directories(test_content_identity PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_content_identity PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_content_identity
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_component_type_codec
    test/test_component_type_codec.cpp)
  target_link_libraries(test_component_type_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_component_type_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cell_graph_codec test/test_cell_graph_codec.cpp)
  target_link_libraries(test_cell_graph_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_graph_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_closed_schema test/test_closed_schema.cpp)
  target_compile_definitions(test_closed_schema PRIVATE
    MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/closed_schema")
  target_link_libraries(test_closed_schema PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_closed_schema
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_control_interface_contract_codec
    test/test_control_interface_contract_codec.cpp)
  target_link_libraries(test_control_interface_contract_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_control_interface_contract_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_controller_topology_codec
    test/test_controller_topology_codec.cpp)
  target_link_libraries(test_controller_topology_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_controller_topology_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_model_asset_codec
    test/test_model_asset_codec.cpp)
  target_link_libraries(test_model_asset_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_model_asset_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_profile_codec
    test/test_runtime_profile_codec.cpp)
  target_link_libraries(test_runtime_profile_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_runtime_profile_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_provider_manifest_codec
    test/test_provider_manifest_codec.cpp)
  target_link_libraries(test_provider_manifest_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_provider_manifest_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_evidence_policy_codec
    test/test_stop_evidence_policy_codec.cpp)
  target_link_libraries(test_stop_evidence_policy_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_stop_evidence_policy_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
