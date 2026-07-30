cmake_minimum_required(VERSION 3.14)
project(manyforge_deployment VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR
    "build-qualified compiler provenance requires a single-configuration "
    "generator; one configure tree cannot represent several binary "
    "configurations with one compiler-build manifest")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(OpenSSL REQUIRED)
find_package(EXPAT REQUIRED)
find_package(double-conversion CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

set(
  MANYFORGE_NODE_ENVIRONMENT_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_environment_manifest.yaml")
set(
  MANYFORGE_NODE_ENVIRONMENT_CHECK
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/scripts/generate_node_environment_manifest.py")
if(NOT EXISTS "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}" OR
   NOT EXISTS "${MANYFORGE_NODE_ENVIRONMENT_CHECK}")
  message(FATAL_ERROR
    "manyforge_deployment production retention requires the sole "
    "manyforge_behavior node-environment manifest and its freshness checker")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior"
    "${Python3_EXECUTABLE}" "${MANYFORGE_NODE_ENVIRONMENT_CHECK}" --check
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
  RESULT_VARIABLE MANYFORGE_NODE_ENVIRONMENT_CHECK_RESULT
  OUTPUT_VARIABLE MANYFORGE_NODE_ENVIRONMENT_CHECK_OUTPUT
  ERROR_VARIABLE MANYFORGE_NODE_ENVIRONMENT_CHECK_ERROR)
if(NOT MANYFORGE_NODE_ENVIRONMENT_CHECK_RESULT EQUAL 0)
  message(FATAL_ERROR
    "the build-owned node-environment manifest is absent, invalid, or stale:\n"
    "${MANYFORGE_NODE_ENVIRONMENT_CHECK_OUTPUT}"
    "${MANYFORGE_NODE_ENVIRONMENT_CHECK_ERROR}")
endif()
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}"
  "${MANYFORGE_NODE_ENVIRONMENT_CHECK}")

file(READ
  "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}"
  MANYFORGE_EMBEDDED_NODE_ENVIRONMENT_MANIFEST)
string(
  FIND
  "${MANYFORGE_EMBEDDED_NODE_ENVIRONMENT_MANIFEST}"
  ")mf_node_env\""
  MANYFORGE_NODE_ENVIRONMENT_DELIMITER_COLLISION)
if(NOT MANYFORGE_NODE_ENVIRONMENT_DELIMITER_COLLISION EQUAL -1)
  message(FATAL_ERROR
    "node-environment manifest collides with the fixed generated-header "
    "raw-string delimiter")
endif()

set(
  MANYFORGE_CORE_INCLUDE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core/include"
  CACHE PATH
  "Path to the ROS-free manyforge_core public headers")
get_filename_component(
  MANYFORGE_CORE_INCLUDE_DIR
  "${MANYFORGE_CORE_INCLUDE_DIR}"
  ABSOLUTE)
if(NOT IS_DIRECTORY "${MANYFORGE_CORE_INCLUDE_DIR}")
  message(FATAL_ERROR
    "the configured manyforge_core include root does not exist: "
    "${MANYFORGE_CORE_INCLUDE_DIR}")
endif()

file(
  GLOB_RECURSE
  MANYFORGE_COMPILER_SOURCE_INPUTS
  CONFIGURE_DEPENDS
  LIST_DIRECTORIES false
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${MANYFORGE_CORE_INCLUDE_DIR}/*")
list(
  REMOVE_ITEM
  MANYFORGE_COMPILER_SOURCE_INPUTS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_build_evidence.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_closed_schema.hpp.in")
list(SORT MANYFORGE_COMPILER_SOURCE_INPUTS)

set(
  MANYFORGE_COMPILER_RECIPE_INPUTS
  "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
  "${MANYFORGE_NODE_ENVIRONMENT_CHECK}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_build_evidence.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_closed_schema.hpp.in")
list(SORT MANYFORGE_COMPILER_RECIPE_INPUTS)

function(
  manyforge_build_input_id
  output_id
  relative_path
  input_class)
  if(input_class STREQUAL "source")
    if(relative_path MATCHES
       "^manyforge_deployment/include/(.+)\\.(h|hpp)$")
      set(logical_id "cpp-public-header:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_core/include/(.+)\\.(h|hpp)$")
      set(logical_id "cpp-public-header:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/src/(.+)\\.cpp$")
      set(logical_id "cpp-translation-unit:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/src/(.+)\\.(h|hpp)$")
      set(logical_id "cpp-internal-header:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/resources/(.+)$")
      set(logical_id "compiler-resource:${CMAKE_MATCH_1}")
    else()
      message(FATAL_ERROR
        "compiler source input has no closed logical-ID mapping: "
        "${relative_path}")
    endif()
  elseif(input_class STREQUAL "recipe")
    if(relative_path STREQUAL
       "manyforge_deployment/CMakeLists.txt")
      set(logical_id "build-recipe:manyforge_deployment")
    elseif(relative_path STREQUAL
           "manyforge_behavior/scripts/generate_node_environment_manifest.py")
      set(
        logical_id
        "build-recipe:node-environment-manifest-freshness-check")
    elseif(relative_path MATCHES
           "^manyforge_deployment/src/(.+)\\.hpp\\.in$")
      set(logical_id "build-template:${CMAKE_MATCH_1}")
    else()
      message(FATAL_ERROR
        "compiler recipe input has no closed logical-ID mapping: "
        "${relative_path}")
    endif()
  else()
    message(FATAL_ERROR
      "unknown compiler build-evidence input class: ${input_class}")
  endif()
  string(REPLACE "/" "." logical_id "${logical_id}")
  string(TOLOWER "${logical_id}" logical_id)
  set(${output_id} "${logical_id}" PARENT_SCOPE)
endfunction()

function(
  manyforge_build_evidence_rows
  output_rows
  output_count
  input_paths
  input_class)
  set(indexed_rows "")
  foreach(input_path IN LISTS ${input_paths})
    if(NOT EXISTS "${input_path}")
      message(FATAL_ERROR
        "compiler build-evidence input disappeared: ${input_path}")
    endif()
    file(
      RELATIVE_PATH core_relative_path
      "${MANYFORGE_CORE_INCLUDE_DIR}"
      "${input_path}")
    if(input_class STREQUAL "source" AND
       NOT IS_ABSOLUTE "${core_relative_path}" AND
       NOT core_relative_path MATCHES "^\\.\\./")
      set(
        relative_path
        "manyforge_core/include/${core_relative_path}")
    else()
      file(
        RELATIVE_PATH relative_path
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${input_path}")
    endif()
    manyforge_build_input_id(
      logical_id "${relative_path}" "${input_class}")
    file(SHA256 "${input_path}" content_digest)
    list(APPEND indexed_rows
      "${logical_id}!sha256:${content_digest}")
    set_property(
      DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input_path}")
  endforeach()
  list(SORT indexed_rows)
  list(LENGTH indexed_rows count)
  set(previous_id "")
  set(rows "")
  foreach(indexed_row IN LISTS indexed_rows)
    string(FIND "${indexed_row}" "!" separator)
    string(SUBSTRING "${indexed_row}" 0 ${separator} logical_id)
    math(EXPR digest_offset "${separator} + 1")
    string(SUBSTRING "${indexed_row}" ${digest_offset} -1 content_digest)
    if("${previous_id}" STREQUAL "${logical_id}")
      message(FATAL_ERROR
        "compiler build-evidence logical ID collision: ${logical_id}")
    endif()
    set(previous_id "${logical_id}")
    string(APPEND rows
      "    BuildEvidenceRow{\"${logical_id}\", "
      "\"${content_digest}\"},\n")
  endforeach()
  set(${output_rows} "${rows}" PARENT_SCOPE)
  set(${output_count} "${count}" PARENT_SCOPE)
endfunction()

manyforge_build_evidence_rows(
  MANYFORGE_COMPILER_SOURCE_ROWS
  MANYFORGE_COMPILER_SOURCE_COUNT
  MANYFORGE_COMPILER_SOURCE_INPUTS
  source)
manyforge_build_evidence_rows(
  MANYFORGE_COMPILER_RECIPE_ROWS
  MANYFORGE_COMPILER_RECIPE_COUNT
  MANYFORGE_COMPILER_RECIPE_INPUTS
  recipe)

if(CMAKE_BUILD_TYPE)
  set(MANYFORGE_COMPILER_BUILD_PROFILE "${CMAKE_BUILD_TYPE}")
else()
  set(MANYFORGE_COMPILER_BUILD_PROFILE "unspecified")
endif()
set(
  MANYFORGE_COMPILER_BUILD_TARGET
  "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
set(MANYFORGE_COMPILER_PACKAGE_VERSION "${PROJECT_VERSION}")
set(MANYFORGE_COMPILER_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
set(MANYFORGE_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(MANYFORGE_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(MANYFORGE_CMAKE_VERSION "${CMAKE_VERSION}")
set(MANYFORGE_OPENSSL_VERSION "${OPENSSL_VERSION}")
set(MANYFORGE_EXPAT_VERSION "${EXPAT_VERSION_STRING}")
set(MANYFORGE_UTF8PROC_VERSION "${UTF8PROC_VERSION}")
set(MANYFORGE_LIBYAML_VERSION "${LIBYAML_VERSION}")
set(
  MANYFORGE_DOUBLE_CONVERSION_VERSION
  "${double-conversion_VERSION}")
if(NOT MANYFORGE_DOUBLE_CONVERSION_VERSION)
  message(FATAL_ERROR
    "the selected double-conversion CMake package does not expose "
    "double-conversion_VERSION; build-qualified compiler provenance "
    "requires a versioned package config")
endif()

# Hash every caller/toolchain-mutable C++ and linker flag input without
# embedding paths, secrets, or command strings in the semantic manifest.
# Fixed target options/definitions are covered by the CMake recipe digest;
# compiler defaults are bound by compiler ID/version, target, and profile.
string(TOUPPER "${CMAKE_BUILD_TYPE}" MANYFORGE_BUILD_TYPE_UPPER)
set(MANYFORGE_EFFECTIVE_BUILD_CONFIGURATION "")
function(manyforge_append_build_configuration key value)
  string(LENGTH "${value}" value_length)
  string(APPEND MANYFORGE_EFFECTIVE_BUILD_CONFIGURATION
    "${key}:${value_length}:${value}\n")
  set(
    MANYFORGE_EFFECTIVE_BUILD_CONFIGURATION
    "${MANYFORGE_EFFECTIVE_BUILD_CONFIGURATION}"
    PARENT_SCOPE)
endfunction()
manyforge_append_build_configuration(
  "cmake_cxx_flags" "${CMAKE_CXX_FLAGS}")
manyforge_append_build_configuration(
  "cmake_cxx_flags_profile"
  "${CMAKE_CXX_FLAGS_${MANYFORGE_BUILD_TYPE_UPPER}}")
manyforge_append_build_configuration(
  "cmake_exe_linker_flags" "${CMAKE_EXE_LINKER_FLAGS}")
manyforge_append_build_configuration(
  "cmake_exe_linker_flags_profile"
  "${CMAKE_EXE_LINKER_FLAGS_${MANYFORGE_BUILD_TYPE_UPPER}}")
manyforge_append_build_configuration(
  "cmake_shared_linker_flags" "${CMAKE_SHARED_LINKER_FLAGS}")
manyforge_append_build_configuration(
  "cmake_shared_linker_flags_profile"
  "${CMAKE_SHARED_LINKER_FLAGS_${MANYFORGE_BUILD_TYPE_UPPER}}")
manyforge_append_build_configuration(
  "cmake_static_linker_flags" "${CMAKE_STATIC_LINKER_FLAGS}")
manyforge_append_build_configuration(
  "cmake_static_linker_flags_profile"
  "${CMAKE_STATIC_LINKER_FLAGS_${MANYFORGE_BUILD_TYPE_UPPER}}")
manyforge_append_build_configuration(
  "cmake_interprocedural_optimization"
  "${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
manyforge_append_build_configuration(
  "cmake_interprocedural_optimization_profile"
  "${CMAKE_INTERPROCEDURAL_OPTIMIZATION_${MANYFORGE_BUILD_TYPE_UPPER}}")
string(
  SHA256
  MANYFORGE_BUILD_CONFIGURATION_SHA256
  "${MANYFORGE_EFFECTIVE_BUILD_CONFIGURATION}")
set(
  MANYFORGE_BUILD_CONFIGURATION_DIGEST
  "sha256:${MANYFORGE_BUILD_CONFIGURATION_SHA256}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_build_evidence.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_build_evidence.hpp"
  @ONLY)

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

add_library(manyforge_deployment
  src/artifact_codec.cpp
  src/build_manifest.cpp
  src/build_manifest_repository.cpp
  src/capability_definition_codec.cpp
  src/canonical_value.cpp
  src/cell_applicability.cpp
  src/cell_compiler.cpp
  src/cell_composition.cpp
  src/cell_graph_codec.cpp
  src/cell_structure.cpp
  src/closed_schema.cpp
  src/component_type_codec.cpp
  src/control_interface_contract_codec.cpp
  src/controller_topology_codec.cpp
  src/content_identity.cpp
  src/deployment_applicability_codec.cpp
  src/deployment_artifact_codec.cpp
  src/deployment_catalog_codec.cpp
  src/model_asset_codec.cpp
  src/model_source.cpp
  src/provider_manifest_codec.cpp
  src/retained_bundle.cpp
  src/retention_qualified_deployment.cpp
  src/runtime_profile_codec.cpp
  src/site_overlay_resolution.cpp
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
  EXPAT::EXPAT
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
  add_test(
    NAME BuildManifestAuthorityRejectsMultiConfigGenerator
    COMMAND
      "${CMAKE_COMMAND}"
      "-DMANYFORGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DMANYFORGE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/multi_config_rejection"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/reject_multi_config.cmake")

  function(manyforge_try_public_header result_variable source_file)
    unset(${result_variable} CACHE)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(
      ${result_variable}
      "${CMAKE_CURRENT_BINARY_DIR}/compile_surface/${result_variable}"
      SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test/compile_surface/${source_file}"
      COMPILE_DEFINITIONS
        "-I${CMAKE_CURRENT_SOURCE_DIR}/include"
        "-I${MANYFORGE_CORE_INCLUDE_DIR}"
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
      OUTPUT_VARIABLE compile_output)
    set(${result_variable} "${${result_variable}}" PARENT_SCOPE)
    set(${result_variable}_OUTPUT "${compile_output}" PARENT_SCOPE)
  endfunction()

  manyforge_try_public_header(
    MANYFORGE_RETAINED_BUNDLE_PUBLIC_HEADER_COMPILES
    retained_bundle_public_header_compiles.cpp)
  if(NOT MANYFORGE_RETAINED_BUNDLE_PUBLIC_HEADER_COMPILES)
    message(FATAL_ERROR
      "The retained-bundle public-header compile control failed:\n"
      "${MANYFORGE_RETAINED_BUNDLE_PUBLIC_HEADER_COMPILES_OUTPUT}")
  endif()

  manyforge_try_public_header(
    MANYFORGE_RETAINED_ARTIFACT_INPUT_IS_PUBLIC
    retained_artifact_input_is_private.cpp)
  if(MANYFORGE_RETAINED_ARTIFACT_INPUT_IS_PUBLIC)
    message(FATAL_ERROR
      "RetainedArtifactInput remains nameable from the public header")
  endif()

  manyforge_try_public_header(
    MANYFORGE_BUILD_AND_RETAIN_PRODUCT_IS_PUBLIC
    build_and_retain_product_is_private.cpp)
  if(MANYFORGE_BUILD_AND_RETAIN_PRODUCT_IS_PUBLIC)
    message(FATAL_ERROR
      "build_and_retain_product remains callable from the public header")
  endif()

  manyforge_try_public_header(
    MANYFORGE_RETAINED_PRODUCT_PROJECTION_IS_PUBLIC
    retained_product_projection_is_private.cpp)
  if(MANYFORGE_RETAINED_PRODUCT_PROJECTION_IS_PUBLIC)
    message(FATAL_ERROR
      "retained_product_projection remains callable from the public header")
  endif()

  manyforge_try_public_header(
    MANYFORGE_RETAINED_PRODUCT_OUTPUT_DIGEST_IS_PUBLIC
    retained_product_output_digest_is_private.cpp)
  if(MANYFORGE_RETAINED_PRODUCT_OUTPUT_DIGEST_IS_PUBLIC)
    message(FATAL_ERROR
      "retained_product_output_digest remains callable from the public header")
  endif()

  manyforge_try_public_header(
    MANYFORGE_BUILD_QUALIFIED_RETENTION_CONTEXT_IS_PUBLIC
    build_qualified_retention_context_is_private.cpp)
  if(MANYFORGE_BUILD_QUALIFIED_RETENTION_CONTEXT_IS_PUBLIC)
    message(FATAL_ERROR
      "BuildQualifiedRetentionContext remains nameable from the public header")
  endif()

  manyforge_try_public_header(
    MANYFORGE_RETAINED_BUNDLE_MATERIALIZER_IS_PUBLIC
    retained_bundle_materializer_is_private.cpp)
  if(MANYFORGE_RETAINED_BUNDLE_MATERIALIZER_IS_PUBLIC)
    message(FATAL_ERROR
      "RetainedBundleMaterializer remains callable from the public header")
  endif()

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

  add_executable(test_cell_structure test/test_cell_structure.cpp)
  target_link_libraries(test_cell_structure PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_structure
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cell_applicability
    test/test_cell_applicability.cpp)
  target_link_libraries(test_cell_applicability PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_applicability
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cell_applicability_budget
    test/test_cell_applicability_budget.cpp)
  target_include_directories(test_cell_applicability_budget PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_cell_applicability_budget PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_applicability_budget
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_cell_compiler
    test/test_cell_compiler.cpp)
  target_include_directories(test_cell_compiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_cell_compiler PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_compiler
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_closed_schema test/test_closed_schema.cpp)
  target_compile_definitions(test_closed_schema PRIVATE
    MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/closed_schema")
  target_include_directories(test_closed_schema PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
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

  add_executable(test_deployment_applicability_codec
    test/test_deployment_applicability_codec.cpp)
  target_link_libraries(test_deployment_applicability_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_deployment_applicability_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_deployment_artifact_codec
    test/test_deployment_artifact_codec.cpp)
  target_include_directories(test_deployment_artifact_codec PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_deployment_artifact_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_deployment_artifact_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_deployment_catalog_codec
    test/test_deployment_catalog_codec.cpp)
  target_include_directories(test_deployment_catalog_codec PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_deployment_catalog_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_deployment_catalog_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_site_overlay_resolution
    test/test_site_overlay_resolution.cpp)
  target_link_libraries(test_site_overlay_resolution PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_site_overlay_resolution
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_model_asset_codec
    test/test_model_asset_codec.cpp)
  target_link_libraries(test_model_asset_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_model_asset_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_model_source
    test/test_model_source.cpp)
  target_link_libraries(test_model_source PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_model_source
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

  add_executable(test_retained_bundle
    test/test_retained_bundle.cpp)
  target_compile_definitions(test_retained_bundle PRIVATE
    MANYFORGE_RETAINED_BUNDLE_PUBLIC_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/include/manyforge_deployment/retained_bundle.hpp")
  target_include_directories(test_retained_bundle PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_retained_bundle PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_retained_bundle
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retained_bundle_materializer
    test/test_retained_bundle_materializer.cpp)
  target_include_directories(test_retained_bundle_materializer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_retained_bundle_materializer PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_retained_bundle_materializer
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_build_manifest
    test/test_build_manifest.cpp)
  target_include_directories(test_build_manifest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(test_build_manifest PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_build_manifest
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retention_qualified_deployment
    test/test_retention_qualified_deployment.cpp)
  target_link_libraries(test_retention_qualified_deployment PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_retention_qualified_deployment
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_evidence_policy_codec
    test/test_stop_evidence_policy_codec.cpp)
  target_link_libraries(test_stop_evidence_policy_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_stop_evidence_policy_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
