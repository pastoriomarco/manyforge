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

get_filename_component(
  _MANYFORGE_WORKSPACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
set(
  MANYFORGE_NODE_ENVIRONMENT_MANIFEST
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior/resources/node_environment_manifest.yaml")
set(
  MANYFORGE_NODE_ENVIRONMENT_CHECK
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior/scripts/generate_node_environment_manifest.py")
set(
  MANYFORGE_STRUCTURAL_PREPARATION_TOOL
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_structural_model.py")
set(
  MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_maintained_deployment_sources.py")
set(
  MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/maintained_deployment_sources.v1.json")
set(
  MANYFORGE_MAINTAINED_ASSISTANT_PROFILES
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/maintained_assistant_profiles.v1.json")
set(
  MANYFORGE_ASSISTANT_INSTRUMENT_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/assistant/assistant_instrument_registry_source.v1.json")
set(
  MANYFORGE_ASSISTANT_INSTRUMENT_GENERATOR
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_assistant_instrument_registry.py")
set(
  MANYFORGE_NODE_CATALOG_SOURCE
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior/resources/node_catalog.yaml")
set(
  MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_maintained_deployment_occurrences.py")
if(NOT EXISTS "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}" OR
   NOT EXISTS "${MANYFORGE_NODE_ENVIRONMENT_CHECK}" OR
   NOT EXISTS "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}" OR
   NOT EXISTS "${MANYFORGE_ASSISTANT_INSTRUMENT_SOURCE}" OR
   NOT EXISTS "${MANYFORGE_ASSISTANT_INSTRUMENT_GENERATOR}" OR
   NOT EXISTS "${MANYFORGE_NODE_CATALOG_SOURCE}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR}")
  message(FATAL_ERROR
    "manyforge_deployment production retention requires the node-environment "
    "closure, structural-model preparation tool, and maintained N6 source "
    "description")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior"
    "${Python3_EXECUTABLE}" "${MANYFORGE_NODE_ENVIRONMENT_CHECK}" --check
  WORKING_DIRECTORY "${_MANYFORGE_WORKSPACE_ROOT}"
  RESULT_VARIABLE MANYFORGE_NODE_ENVIRONMENT_CHECK_RESULT
  OUTPUT_VARIABLE MANYFORGE_NODE_ENVIRONMENT_CHECK_OUTPUT
  ERROR_VARIABLE MANYFORGE_NODE_ENVIRONMENT_CHECK_ERROR)
if(NOT MANYFORGE_NODE_ENVIRONMENT_CHECK_RESULT EQUAL 0)
  message(FATAL_ERROR
    "the build-owned node-environment manifest is absent, invalid, or stale:\n"
    "${MANYFORGE_NODE_ENVIRONMENT_CHECK_OUTPUT}"
    "${MANYFORGE_NODE_ENVIRONMENT_CHECK_ERROR}")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior"
    "${Python3_EXECUTABLE}" "${MANYFORGE_ASSISTANT_INSTRUMENT_GENERATOR}"
    --check
    --catalog "${MANYFORGE_NODE_CATALOG_SOURCE}"
    --assistant-profiles "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}"
    --output "${MANYFORGE_ASSISTANT_INSTRUMENT_SOURCE}"
  WORKING_DIRECTORY "${_MANYFORGE_WORKSPACE_ROOT}"
  RESULT_VARIABLE MANYFORGE_ASSISTANT_INSTRUMENT_CHECK_RESULT
  OUTPUT_VARIABLE MANYFORGE_ASSISTANT_INSTRUMENT_CHECK_OUTPUT
  ERROR_VARIABLE MANYFORGE_ASSISTANT_INSTRUMENT_CHECK_ERROR)
if(NOT MANYFORGE_ASSISTANT_INSTRUMENT_CHECK_RESULT EQUAL 0)
  message(FATAL_ERROR
    "the compiler-owned assistant instrument source is invalid or stale:\n"
    "${MANYFORGE_ASSISTANT_INSTRUMENT_CHECK_OUTPUT}"
    "${MANYFORGE_ASSISTANT_INSTRUMENT_CHECK_ERROR}")
endif()
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}"
  "${MANYFORGE_NODE_ENVIRONMENT_CHECK}"
  "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK}"
  "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
  "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}"
  "${MANYFORGE_ASSISTANT_INSTRUMENT_SOURCE}"
  "${MANYFORGE_ASSISTANT_INSTRUMENT_GENERATOR}"
  "${MANYFORGE_NODE_CATALOG_SOURCE}"
  "${MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR}")

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

file(READ "${MANYFORGE_NODE_CATALOG_SOURCE}" MANYFORGE_EMBEDDED_NODE_CATALOG)
string(FIND "${MANYFORGE_EMBEDDED_NODE_CATALOG}" ")mf_node_cat\""
  MANYFORGE_NODE_CATALOG_DELIMITER_COLLISION)
if(NOT MANYFORGE_NODE_CATALOG_DELIMITER_COLLISION EQUAL -1)
  message(FATAL_ERROR "node catalog collides with its generated-header delimiter")
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
get_filename_component(
  MANYFORGE_CORE_SOURCE_DIR
  "${MANYFORGE_CORE_INCLUDE_DIR}/.."
  ABSOLUTE)
set(
  MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE
  "${MANYFORGE_CORE_SOURCE_DIR}/src/runtime_contract_manifest_codec.cpp")
if(NOT EXISTS "${MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE}")
  message(FATAL_ERROR
    "manyforge_deployment requires the O1.4b runtime-contract codec source: "
    "${MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE}")
endif()

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
    "manyforge_deployment requires the shared content-identity source tree: "
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
    "manyforge_deployment could not resolve the shared content-identity target")
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
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.py"
  "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include/*"
  "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/src/*.cpp"
  "${MANYFORGE_CORE_INCLUDE_DIR}/*"
  "${MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE}")
list(
  REMOVE_ITEM
  MANYFORGE_COMPILER_SOURCE_INPUTS
  # These C0 transport/test artifacts are downstream of the native compiler
  # and must not feed its own build identity through the recursive glob above.
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/program_binding_lite6_native.fixture.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/program_binding_transport.v1.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_program_binding_transport.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_build_evidence.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_canonical_empty_deployment.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_closed_schema.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_structural_model_preparation.hpp.in")
list(SORT MANYFORGE_COMPILER_SOURCE_INPUTS)

set(
  MANYFORGE_COMPILER_RECIPE_INPUTS
  "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
  "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/CMakeLists.txt"
  "${MANYFORGE_NODE_ENVIRONMENT_CHECK}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_build_evidence.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_canonical_empty_deployment.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_closed_schema.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_structural_model_preparation.hpp.in")
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
           "^manyforge_core/src/(.+)\\.cpp$")
      set(logical_id
          "cpp-translation-unit:manyforge_core/${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_content_identity/include/(.+)\\.(h|hpp)$")
      set(
        logical_id
        "cpp-public-header:manyforge_content_identity/${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_content_identity/src/(.+)\\.cpp$")
      set(
        logical_id
        "cpp-translation-unit:manyforge_content_identity/${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/src/(.+)\\.cpp$")
      set(logical_id "cpp-translation-unit:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/src/(.+)\\.(h|hpp)$")
      set(logical_id "cpp-internal-header:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/tools/(.+)\\.cpp$")
      set(logical_id "compiler-tool:${CMAKE_MATCH_1}")
    elseif(relative_path MATCHES
           "^manyforge_deployment/tools/(.+)\\.py$")
      set(logical_id "compiler-python-tool:${CMAKE_MATCH_1}")
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
           "manyforge_content_identity/CMakeLists.txt")
      set(logical_id "build-recipe:manyforge_content_identity")
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

file(
  SHA256
  "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
  MANYFORGE_STRUCTURAL_PREPARATION_BUILD_SHA256)
set(
  MANYFORGE_STRUCTURAL_PREPARATION_BUILD_DIGEST
  "sha256:${MANYFORGE_STRUCTURAL_PREPARATION_BUILD_SHA256}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_structural_model_preparation.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_structural_model_preparation.hpp"
  @ONLY)

set(
  MANYFORGE_CANONICAL_EMPTY_DEPLOYMENT
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/canonical_empty/c1_build_authority.deployment.json")
file(READ
  "${MANYFORGE_CANONICAL_EMPTY_DEPLOYMENT}"
  MANYFORGE_EMBEDDED_CANONICAL_EMPTY_DEPLOYMENT)
string(
  FIND
  "${MANYFORGE_EMBEDDED_CANONICAL_EMPTY_DEPLOYMENT}"
  ")mf_canon\""
  MANYFORGE_CANONICAL_EMPTY_DELIMITER_COLLISION)
if(NOT MANYFORGE_CANONICAL_EMPTY_DELIMITER_COLLISION EQUAL -1)
  message(FATAL_ERROR
    "canonical-empty deployment collides with the fixed generated-header "
    "raw-string delimiter")
endif()
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_CANONICAL_EMPTY_DEPLOYMENT}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_canonical_empty_deployment.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_canonical_empty_deployment.hpp"
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
  kEmbeddedCommandSchema
  command_schema
  resources/closed_schema/catalog/manyforge.command_schema.v1.yaml
  mf_cmd_schema)
manyforge_embed_closed_schema(
  kEmbeddedContactPolicySchema
  contact_policy
  resources/closed_schema/catalog/manyforge.contact_policy.v1.yaml
  mf_contact)
manyforge_embed_closed_schema(
  kEmbeddedRuntimeProfileSchema
  runtime_profile
  resources/closed_schema/catalog/manyforge.runtime_profile.v1.yaml
  mf_rt_profile)
manyforge_embed_closed_schema(
  kEmbeddedRuntimePolicySchema
  runtime_policy
  resources/closed_schema/catalog/manyforge.runtime_policy.v1.yaml
  mf_rt_policy)
manyforge_embed_closed_schema(
  kEmbeddedStopEvidencePolicySchema
  stop_evidence_policy
  resources/closed_schema/catalog/manyforge.stop_evidence_policy.v2.yaml
  mf_stop_schema)
manyforge_embed_closed_schema(
  kEmbeddedEmptyRuntimeConfigurationSchema
  empty_runtime_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.empty.v1.yaml
  mf_empty_cfg)
manyforge_embed_closed_schema(
  kEmbeddedNativeExecutionRuntimeConfigurationSchema
  native_execution_runtime_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.native_execution.v1.yaml
  mf_native_cfg)
manyforge_embed_closed_schema(
  kEmbeddedRos2ControlExecutionRuntimeConfigurationSchema
  ros2_control_execution_runtime_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml
  mf_ros2_cfg)
manyforge_embed_closed_schema(
  kEmbeddedEmptyProviderContractSchema
  empty_provider_contract
  resources/closed_schema/value/manyforge.value_schema.provider_contract.empty.v1.yaml
  mf_pempty)
manyforge_embed_closed_schema(
  kEmbeddedMotionControlledStopProviderContractSchema
  motion_controlled_stop_provider_contract
  resources/closed_schema/value/manyforge.value_schema.provider_contract.motion_controlled_stop.v1.yaml
  mf_stop_params)
manyforge_embed_closed_schema(
  kEmbeddedAssistantInstrumentRegistrySource
  assistant_instrument_registry_source
  resources/assistant/assistant_instrument_registry_source.v1.json
  mf_asst_reg)

set(MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MANYFORGE_CORE_INCLUDE_DIR}>
  $<BUILD_INTERFACE:${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set(MANYFORGE_DEPLOYMENT_EXTERNAL_LIBRARIES
  EXPAT::EXPAT
  PkgConfig::LIBYAML
)

if(TARGET manyforge_core)
  set(MANYFORGE_RUNTIME_CONTRACT_CODEC_TARGET manyforge_core)
else()
  # Deployment is also qualified as a standalone CMake package. Reuse the
  # exact O1.4b Core translation unit in that package rather than inventing a
  # second runtime-contract codec or requiring the full Core link closure.
  add_library(manyforge_deployment_runtime_contract_codec STATIC
    "${MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE}")
  set_target_properties(manyforge_deployment_runtime_contract_codec
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(manyforge_deployment_runtime_contract_codec
    PUBLIC
    "${MANYFORGE_CORE_INCLUDE_DIR}"
    "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include")
  target_link_libraries(manyforge_deployment_runtime_contract_codec PUBLIC
    manyforge::content_identity)
  set(MANYFORGE_RUNTIME_CONTRACT_CODEC_TARGET
    manyforge_deployment_runtime_contract_codec)
endif()

# Canonical projection/parsing, semantic contract verification, and retained-
# bundle readback. This private substrate cannot mint retention
# authority and is deliberately free of materializer, repository-writer,
# package-generator, and admission-selection effects. The O1.4c runtime
# contract boundary reuses Core's verified runtime-contract codec, either
# through the concrete Core target or its exact standalone translation unit.
add_library(manyforge_deployment_verification_core
  src/assistant_settings_codec.cpp
  src/apply_affected_scope.cpp
  src/artifact_codec.cpp
  src/composed_model_symbol_role_map_verifier.cpp
  src/control_domain_control_internal.cpp
  src/control_domain_control_verifier.cpp
  src/control_interface_contract_codec.cpp
  src/controller_topology_codec.cpp
  src/contact_policy_codec.cpp
  src/cell_graph_codec.cpp
  src/closed_schema.cpp
  src/content_identity.cpp
  src/deployment_applicability_codec.cpp
  src/deployment_artifact_codec.cpp
  src/deployment_authoring.cpp
  src/deployment_catalog_codec.cpp
  src/model_asset_codec.cpp
  src/model_source.cpp
  src/native_execution_runtime_unit_internal.cpp
  src/ros2_control_execution_runtime_unit_internal.cpp
  src/planning_resource_map_internal.cpp
  src/posix_read_only_internal.cpp
  src/program_contract.cpp
  src/program_binding.cpp
  src/provider_manifest_codec.cpp
  src/runtime_program_binding.cpp
  src/reference_runtime_unit_verifier.cpp
  src/runtime_contract_registry.cpp
  src/verified_retained_projection.cpp
  src/runtime_profile_codec.cpp
  src/runtime_policy_codec.cpp
  src/retained_runtime_policy.cpp
  src/scenario_apply_candidate_source.cpp
  src/stop_evidence_policy_codec.cpp
  src/verified_retained_bundle.cpp
  src/verified_retained_bundle_store.cpp
)
add_library(
  manyforge::deployment_verification_core ALIAS
  manyforge_deployment_verification_core)
target_include_directories(manyforge_deployment_verification_core PUBLIC
  ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
target_include_directories(manyforge_deployment_verification_core PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_deployment_verification_core PUBLIC
  manyforge::content_identity
  ${MANYFORGE_DEPLOYMENT_EXTERNAL_LIBRARIES}
  ${MANYFORGE_RUNTIME_CONTRACT_CODEC_TARGET})
set_target_properties(
  manyforge_deployment_verification_core
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Sealing a retained controller-domain projection calls Core's independently
# qualified controller-lifecycle validator.  Keep that composition boundary
# out of the standalone Deployment verifier, which consumes Core headers but
# deliberately has no dependency on the concrete Core library.
if(TARGET manyforge_core)
  add_library(manyforge_deployment_verified_controller_lifecycle_expected_domain
    src/verified_controller_lifecycle_expected_domain.cpp)
  add_library(
    manyforge::deployment_verified_controller_lifecycle_expected_domain ALIAS
    manyforge_deployment_verified_controller_lifecycle_expected_domain)
  target_include_directories(
    manyforge_deployment_verified_controller_lifecycle_expected_domain PUBLIC
    ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
  target_include_directories(
    manyforge_deployment_verified_controller_lifecycle_expected_domain PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    manyforge_deployment_verified_controller_lifecycle_expected_domain PUBLIC
    manyforge_deployment_verification_core
    manyforge_core)
  set_target_properties(
    manyforge_deployment_verified_controller_lifecycle_expected_domain
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Active-control Stop-plan composition is a cross-owner NRT adapter. Keep it
# outside the standalone Deployment verifier: it calls Core's independently
# qualified content-addressed plan/readiness boundary and therefore exists
# only in a composition that already owns the concrete manyforge_core target.
set(_MANYFORGE_STOP_PLAN_RESOLVER_DEFAULT OFF)
if(TARGET manyforge_core)
  set(_MANYFORGE_STOP_PLAN_RESOLVER_DEFAULT ON)
endif()
option(
  MANYFORGE_BUILD_RESOLVED_STOP_BASE_PLAN_RESOLVER
  "Build the Deployment-to-Core resolved Stop base-plan composition adapter"
  ${_MANYFORGE_STOP_PLAN_RESOLVER_DEFAULT})
if(MANYFORGE_BUILD_RESOLVED_STOP_BASE_PLAN_RESOLVER)
  if(NOT TARGET manyforge_core)
    message(FATAL_ERROR
      "resolved Stop base-plan composition requires an existing "
      "manyforge_core target")
  endif()
  add_library(manyforge_deployment_resolved_stop_base_plan_resolver
    src/resolved_stop_base_plan_resolver.cpp
    src/route_configuration_publication.cpp)
  add_library(
    manyforge::deployment_resolved_stop_base_plan_resolver ALIAS
    manyforge_deployment_resolved_stop_base_plan_resolver)
  target_include_directories(
    manyforge_deployment_resolved_stop_base_plan_resolver PUBLIC
    ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
  target_include_directories(
    manyforge_deployment_resolved_stop_base_plan_resolver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    manyforge_deployment_resolved_stop_base_plan_resolver PUBLIC
    manyforge_deployment_verified_controller_lifecycle_expected_domain
    manyforge_deployment_verification_core
    manyforge_core)
  set_target_properties(
    manyforge_deployment_resolved_stop_base_plan_resolver
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Verified lifecycle/route-current snapshot composition is independently
# read-only and has no dependency on the Stop-plan repository adapter. Keep
# the target separate so minimal Manager compositions do not inherit
# unrelated storage or mutation surfaces.
set(_MANYFORGE_LIFECYCLE_CURRENT_SNAPSHOT_SOURCE_DEFAULT OFF)
if(TARGET manyforge_core)
  set(_MANYFORGE_LIFECYCLE_CURRENT_SNAPSHOT_SOURCE_DEFAULT ON)
endif()
option(
  MANYFORGE_BUILD_COMPONENT_LIFECYCLE_CURRENT_SNAPSHOT_SOURCE
  "Build the verified Deployment-to-Core lifecycle current-snapshot source"
  ${_MANYFORGE_LIFECYCLE_CURRENT_SNAPSHOT_SOURCE_DEFAULT})
if(MANYFORGE_BUILD_COMPONENT_LIFECYCLE_CURRENT_SNAPSHOT_SOURCE)
  if(NOT TARGET manyforge_core)
    message(FATAL_ERROR
      "component lifecycle current-snapshot composition requires an existing "
      "manyforge_core target")
  endif()
  add_library(manyforge_deployment_component_lifecycle_current_snapshot_source
    src/component_lifecycle_current_snapshot_source.cpp
    src/route_selection_current_snapshot_source.cpp)
  add_library(
    manyforge::deployment_component_lifecycle_current_snapshot_source ALIAS
    manyforge_deployment_component_lifecycle_current_snapshot_source)
  target_include_directories(
    manyforge_deployment_component_lifecycle_current_snapshot_source PUBLIC
    ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
  target_link_libraries(
    manyforge_deployment_component_lifecycle_current_snapshot_source PUBLIC
    manyforge_deployment_verified_controller_lifecycle_expected_domain
    manyforge_deployment_verification_core
    manyforge_core)
  set_target_properties(
    manyforge_deployment_component_lifecycle_current_snapshot_source
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Complete CCLC runtime ownership is an effectful production composition. Keep
# it outside the compiler/verifier and require every independently qualified
# Core/Deployment dependency explicitly; standalone compiler builds remain
# free of runtime journals and mutable repositories.
set(_MANYFORGE_CCLC_RUNTIME_COMPOSITION_DEFAULT OFF)
if(TARGET manyforge_core_sqlite_cclc_journal AND
   TARGET manyforge_core_filesystem_resolved_stop_base_plan_repository AND
   TARGET manyforge_deployment_resolved_stop_base_plan_resolver AND
   TARGET manyforge_deployment_component_lifecycle_current_snapshot_source)
  set(_MANYFORGE_CCLC_RUNTIME_COMPOSITION_DEFAULT ON)
endif()
option(
  MANYFORGE_BUILD_CCLC_RUNTIME_COMPOSITION
  "Build the complete Deployment-owned CCLC runtime composition"
  ${_MANYFORGE_CCLC_RUNTIME_COMPOSITION_DEFAULT})
if(MANYFORGE_BUILD_CCLC_RUNTIME_COMPOSITION)
  if(NOT TARGET manyforge_core_sqlite_cclc_journal OR
     NOT TARGET manyforge_core_filesystem_resolved_stop_base_plan_repository OR
     NOT TARGET manyforge_deployment_resolved_stop_base_plan_resolver OR
     NOT TARGET manyforge_deployment_component_lifecycle_current_snapshot_source)
    message(FATAL_ERROR
      "CCLC runtime composition requires the Core journals/repository and "
      "Deployment current-source/Stop-plan targets")
  endif()
  add_library(manyforge_deployment_cclc_runtime_composition
    src/cclc_runtime_composition.cpp
    src/cclc_open_request_server.cpp)
  add_library(
    manyforge::deployment_cclc_runtime_composition ALIAS
    manyforge_deployment_cclc_runtime_composition)
  target_include_directories(
    manyforge_deployment_cclc_runtime_composition PUBLIC
    ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
  target_link_libraries(
    manyforge_deployment_cclc_runtime_composition PUBLIC
    manyforge_deployment_component_lifecycle_current_snapshot_source
    manyforge_deployment_resolved_stop_base_plan_resolver
    manyforge_core_sqlite_cclc_journal
    manyforge_core_filesystem_resolved_stop_base_plan_repository)
  set_target_properties(
    manyforge_deployment_cclc_runtime_composition
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Full offline compiler and writer surface. The read-only admission target is
# defined separately below; producer manifests remain content-addressed
# provenance and are not interpreted as current-reader compatibility profiles.
add_library(manyforge_deployment
  src/build_manifest.cpp
  src/build_manifest_repository.cpp
  src/capability_definition_codec.cpp
  src/command_schema_codec.cpp
  src/cell_applicability.cpp
  src/cell_compiler.cpp
  src/cell_composition.cpp
  src/cell_structure.cpp
  src/component_type_codec.cpp
  src/composed_model_symbol_role_map_internal.cpp
  src/mf2_joint_name_internal.cpp
  src/native_execution_runtime_unit_emitter.cpp
  src/ros2_control_execution_runtime_unit_emitter.cpp
  src/posix_durability_internal.cpp
  src/program_binding_compiler.cpp
  src/reference_runtime_unit_internal.cpp
  src/retained_bundle.cpp
  src/retained_bundle_materializer.cpp
  src/retained_bundle_store.cpp
  src/retention_qualified_deployment.cpp
  src/scenario_catalog.cpp
  src/site_overlay_resolution.cpp
  src/structural_model_preparation.cpp
)
add_library(manyforge::deployment ALIAS manyforge_deployment)

target_include_directories(manyforge_deployment PUBLIC
  ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
target_include_directories(manyforge_deployment PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_deployment PUBLIC
  manyforge_deployment_verification_core)
set_target_properties(
  manyforge_deployment PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(
  manyforge_program_revision
  tools/program_revision.cpp)
target_include_directories(
  manyforge_program_revision PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  manyforge_program_revision PRIVATE
  manyforge::deployment_verification_core)

add_executable(
  manyforge_deployment_revision
  tools/deployment_revision.cpp)
target_include_directories(
  manyforge_deployment_revision PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  manyforge_deployment_revision PRIVATE
  manyforge::deployment_verification_core)

add_executable(
  manyforge_maintained_model_catalog
  tools/maintained_model_catalog.cpp)
target_include_directories(
  manyforge_maintained_model_catalog PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  manyforge_maintained_model_catalog PRIVATE
  manyforge::deployment)

add_executable(
  manyforge_maintained_deployment_occurrences
  tools/maintained_deployment_occurrences.cpp)
target_include_directories(
  manyforge_maintained_deployment_occurrences PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  manyforge_maintained_deployment_occurrences PRIVATE
  manyforge::deployment)

add_library(manyforge_deployment_admission
  src/canonical_empty_admission.cpp
  src/package_admission_profile.cpp
)
add_library(
  manyforge::deployment_admission ALIAS manyforge_deployment_admission)
target_include_directories(manyforge_deployment_admission PUBLIC
  ${MANYFORGE_DEPLOYMENT_PUBLIC_INCLUDE_DIRECTORIES})
target_include_directories(manyforge_deployment_admission PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_deployment_admission PRIVATE
  manyforge_deployment_verification_core)
set_target_properties(
  manyforge_deployment_admission PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(manyforge_deployment_canonical_package_generator STATIC
  src/canonical_empty_package_generator.cpp)
target_include_directories(
  manyforge_deployment_canonical_package_generator PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  manyforge_deployment_canonical_package_generator PRIVATE
  manyforge::deployment
  manyforge::deployment_admission)
target_link_libraries(
  manyforge_maintained_deployment_occurrences PRIVATE
  manyforge_deployment_canonical_package_generator)
set_target_properties(
  manyforge_deployment_canonical_package_generator
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(
  manyforge_generate_canonical_empty_package
  tools/generate_canonical_empty_package.cpp)
target_include_directories(
  manyforge_generate_canonical_empty_package PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${MANYFORGE_CORE_INCLUDE_DIR}")
target_link_libraries(
  manyforge_generate_canonical_empty_package PRIVATE
  manyforge_deployment_canonical_package_generator
  manyforge::deployment_admission)

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

  add_test(
    NAME ProgramRevisionAuthoringTool
    COMMAND
      "${CMAKE_COMMAND}"
      "-DMANYFORGE_PROGRAM_REVISION_TOOL=$<TARGET_FILE:manyforge_program_revision>"
      "-DMANYFORGE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/program_revision_tool"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/program_revision_tool.cmake")

  add_test(
    NAME ProgramRevisionLoaderRoundtrip
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "MANYFORGE_PROGRAM_REVISION_TOOL=$<TARGET_FILE:manyforge_program_revision>"
      "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider
      "${CMAKE_CURRENT_SOURCE_DIR}/test/test_program_revision_roundtrip.py")
  set_tests_properties(ProgramRevisionLoaderRoundtrip PROPERTIES TIMEOUT 60)

  add_test(
    NAME MaintainedProgramSourceRevisions
    COMMAND
      "${CMAKE_COMMAND}"
      "-DMANYFORGE_PROGRAM_REVISION_TOOL=$<TARGET_FILE:manyforge_program_revision>"
      "-DMANYFORGE_WORKSPACE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/maintained_program_source_revisions.cmake")

  add_test(
    NAME DeploymentRevisionAuthoringTool
    COMMAND
      "${CMAKE_COMMAND}"
      "-DMANYFORGE_DEPLOYMENT_REVISION_TOOL=$<TARGET_FILE:manyforge_deployment_revision>"
      "-DMANYFORGE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/deployment_revision_tool"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/deployment_revision_tool.cmake")

  add_test(
    NAME MaintainedDeploymentSourceDescription
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior"
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK}"
      --check
      --input "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
      --workspace-root "${_MANYFORGE_WORKSPACE_ROOT}")

  add_test(
    NAME MaintainedDeploymentOccurrenceSources
    COMMAND
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR}"
      --check
      --description "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
      --assistant-profiles "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}"
      --workspace-root "${_MANYFORGE_WORKSPACE_ROOT}"
      --revision-tool "$<TARGET_FILE:manyforge_deployment_revision>")
  set_tests_properties(
    MaintainedDeploymentOccurrenceSources
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

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
        "-I${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include"
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
    MANYFORGE_ASSISTANT_SETTINGS_PUBLIC_HEADER_COMPILES
    assistant_settings_public_header_compiles.cpp)
  if(NOT MANYFORGE_ASSISTANT_SETTINGS_PUBLIC_HEADER_COMPILES)
    message(FATAL_ERROR
      "The assistant-settings public-header compile control failed:\n"
      "${MANYFORGE_ASSISTANT_SETTINGS_PUBLIC_HEADER_COMPILES_OUTPUT}")
  endif()

  manyforge_try_public_header(
    MANYFORGE_VERIFIED_RETAINED_BUNDLE_PUBLIC_HEADER_COMPILES
    verified_retained_bundle_public_header_compiles.cpp)
  if(NOT MANYFORGE_VERIFIED_RETAINED_BUNDLE_PUBLIC_HEADER_COMPILES)
    message(FATAL_ERROR
      "The verified retained-bundle public-header compile control failed:\n"
      "${MANYFORGE_VERIFIED_RETAINED_BUNDLE_PUBLIC_HEADER_COMPILES_OUTPUT}")
  endif()

  manyforge_try_public_header(
    MANYFORGE_STRUCTURAL_MODEL_PREPARATION_PUBLIC_HEADER_COMPILES
    structural_model_preparation_public_header_compiles.cpp)
  if(NOT MANYFORGE_STRUCTURAL_MODEL_PREPARATION_PUBLIC_HEADER_COMPILES)
    message(FATAL_ERROR
      "The structural-model preparation public-header compile control "
      "failed:\n"
      "${MANYFORGE_STRUCTURAL_MODEL_PREPARATION_PUBLIC_HEADER_COMPILES_OUTPUT}")
  endif()

  manyforge_try_public_header(
    MANYFORGE_VERIFIED_RETAINED_BUNDLE_WRITER_API_IS_PUBLIC
    verified_retained_bundle_writer_api_is_private.cpp)
  if(MANYFORGE_VERIFIED_RETAINED_BUNDLE_WRITER_API_IS_PUBLIC)
    message(FATAL_ERROR
      "The narrow verified retained-bundle header exposes writer APIs")
  endif()

  manyforge_try_public_header(
    MANYFORGE_CANONICAL_EMPTY_ADMISSION_PUBLIC_HEADER_COMPILES
    canonical_empty_admission_public_header_compiles.cpp)
  if(NOT MANYFORGE_CANONICAL_EMPTY_ADMISSION_PUBLIC_HEADER_COMPILES)
    message(FATAL_ERROR
      "The canonical-empty admission public-header compile control failed:\n"
      "${MANYFORGE_CANONICAL_EMPTY_ADMISSION_PUBLIC_HEADER_COMPILES_OUTPUT}")
  endif()

  manyforge_try_public_header(
    MANYFORGE_CANONICAL_EMPTY_ADMISSION_PRODUCER_IS_PUBLIC
    canonical_empty_admission_producer_is_private.cpp)
  if(MANYFORGE_CANONICAL_EMPTY_ADMISSION_PRODUCER_IS_PUBLIC)
    message(FATAL_ERROR
      "canonical-empty admission producer remains callable from the public "
      "header")
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

  add_executable(test_assistant_settings_codec
    test/test_assistant_settings_codec.cpp)
  target_link_libraries(test_assistant_settings_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_assistant_settings_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_content_identity test/test_content_identity.cpp)
  target_include_directories(test_content_identity PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_content_identity PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_content_identity
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_program_binding
    test/test_program_binding.cpp)
  target_compile_definitions(test_program_binding PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_program_binding PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_binding PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_program_binding
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_scenario_catalog
    test/test_scenario_catalog.cpp)
  target_include_directories(test_scenario_catalog PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_scenario_catalog PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_scenario_catalog
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_program_contract
    test/test_program_contract.cpp)
  target_compile_definitions(test_program_contract PRIVATE
    MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/program_capture_package"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_program_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_contract PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_program_contract
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      FIXTURES_REQUIRED ProgramCapturePackage)
  add_dependencies(test_program_contract manyforge_maintained_deployment_occurrences)
  add_test(NAME ProgramCapturePackage
    COMMAND "${CMAKE_COMMAND}"
      "-DTOOL=$<TARGET_FILE:manyforge_maintained_deployment_occurrences>"
      "-DWORKSPACE=${_MANYFORGE_WORKSPACE_ROOT}"
      "-DBUILD_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/test/program_capture_package.cmake")
  set_tests_properties(ProgramCapturePackage PROPERTIES
    FIXTURES_SETUP ProgramCapturePackage TIMEOUT 120)

  add_executable(test_program_binding_compiler
    test/test_program_binding_compiler.cpp)
  target_compile_definitions(test_program_binding_compiler PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_program_binding_compiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_binding_compiler PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_program_binding_compiler
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
  target_include_directories(test_cell_structure PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_cell_structure PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_structure
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_mf2_joint_name test/test_mf2_joint_name.cpp)
  target_include_directories(test_mf2_joint_name PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_mf2_joint_name PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_mf2_joint_name
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

  add_executable(test_command_schema_codec
    test/test_command_schema_codec.cpp)
  target_compile_definitions(test_command_schema_codec PRIVATE
    MANYFORGE_DEPLOYMENT_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")
  target_link_libraries(test_command_schema_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_command_schema_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_control_interface_contract_codec
    test/test_control_interface_contract_codec.cpp)
  target_link_libraries(test_control_interface_contract_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_control_interface_contract_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_contact_policy_codec
    test/test_contact_policy_codec.cpp)
  target_link_libraries(test_contact_policy_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_contact_policy_codec
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

  set(
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  set(
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural")
  file(
    GLOB_RECURSE
    MANYFORGE_LITE6_STRUCTURAL_RESOURCE_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/meshes/*")
  file(MAKE_DIRECTORY "${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}")
  execute_process(
    COMMAND
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
      --urdf
      "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/lite6_lite_gripper.urdf"
      --srdf
      "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/lite6_lite_gripper.srdf"
      --model-asset-id
      lite6_lite_gripper
      --accepted-package-uri-prefix
      "package://manyforge_example_models/models/lite6_lite_gripper/"
      --resource-root
      "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}"
      --output-directory
      "${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    RESULT_VARIABLE MANYFORGE_LITE6_STRUCTURAL_PREPARATION_RESULT
    OUTPUT_VARIABLE MANYFORGE_LITE6_STRUCTURAL_PREPARATION_OUTPUT
    ERROR_VARIABLE MANYFORGE_LITE6_STRUCTURAL_PREPARATION_ERROR)
  if(NOT MANYFORGE_LITE6_STRUCTURAL_PREPARATION_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Lite6 structural-model qualification preparation failed:\n"
      "${MANYFORGE_LITE6_STRUCTURAL_PREPARATION_OUTPUT}"
      "${MANYFORGE_LITE6_STRUCTURAL_PREPARATION_ERROR}")
  endif()
  set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
    "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/lite6_lite_gripper.urdf"
    "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/lite6_lite_gripper.srdf"
    ${MANYFORGE_LITE6_STRUCTURAL_RESOURCE_INPUTS})

  set(
    MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/ur10e_robotiq_2f_140")
  set(
    MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ur10e_robotiq_2f_140_structural")
  file(
    GLOB_RECURSE
    MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_RESOURCE_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/meshes/*")
  file(MAKE_DIRECTORY
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT}")
  execute_process(
    COMMAND
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
      --urdf
      "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.structural.urdf"
      --srdf
      "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.structural.srdf"
      --model-asset-id
      ur10e_robotiq_2f_140
      --accepted-package-uri-prefix
      "package://manyforge_example_models/models/ur10e_robotiq_2f_140/"
      --resource-root
      "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}"
      --output-directory
      "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT}"
    RESULT_VARIABLE MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_PREPARATION_RESULT
    OUTPUT_VARIABLE MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_PREPARATION_OUTPUT
    ERROR_VARIABLE MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_PREPARATION_ERROR)
  if(NOT MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_PREPARATION_RESULT EQUAL 0)
    message(FATAL_ERROR
      "UR10e/Robotiq structural-model qualification preparation failed:\n"
      "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_PREPARATION_OUTPUT}"
      "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_PREPARATION_ERROR}")
  endif()
  set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.urdf"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.structural.urdf"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.srdf"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.structural.srdf"
    ${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_RESOURCE_INPUTS})

  set(
    MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/model_sources/ur5_roboplan")
  set(
    MANYFORGE_UR5_ROBOPLAN_HISTORICAL_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.docker/roboplan/roboplan_example_models/models/ur_robot_model")
  set(
    MANYFORGE_UR5_ROBOPLAN_RESOURCE_ROOT
    "${MANYFORGE_UR5_ROBOPLAN_HISTORICAL_SOURCE_ROOT}")
  set(
    MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ur5_roboplan_structural")
  file(
    GLOB_RECURSE
    MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_RESOURCE_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${MANYFORGE_UR5_ROBOPLAN_RESOURCE_ROOT}/meshes/*")
  file(MAKE_DIRECTORY
    "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_FIXTURE_ROOT}")
  execute_process(
    COMMAND
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
      --urdf
      "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_SOURCE_ROOT}/ur5_gripper.structural.urdf"
      --srdf
      "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_SOURCE_ROOT}/ur5_gripper.structural.srdf"
      --model-asset-id
      ur5_roboplan
      --accepted-package-uri-prefix
      "package://roboplan_example_models/models/ur_robot_model/"
      --resource-root
      "${MANYFORGE_UR5_ROBOPLAN_RESOURCE_ROOT}"
      --output-directory
      "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_FIXTURE_ROOT}"
    RESULT_VARIABLE MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_PREPARATION_RESULT
    OUTPUT_VARIABLE MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_PREPARATION_OUTPUT
    ERROR_VARIABLE MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_PREPARATION_ERROR)
  if(NOT MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_PREPARATION_RESULT EQUAL 0)
    message(FATAL_ERROR
      "UR5/RoboPlan structural-model qualification preparation failed:\n"
      "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_PREPARATION_OUTPUT}"
      "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_PREPARATION_ERROR}")
  endif()
  set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
    "${MANYFORGE_UR5_ROBOPLAN_HISTORICAL_SOURCE_ROOT}/ur5_gripper.urdf"
    "${MANYFORGE_UR5_ROBOPLAN_HISTORICAL_SOURCE_ROOT}/ur5_gripper.srdf"
    "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_SOURCE_ROOT}/ur5_gripper.structural.urdf"
    "${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_SOURCE_ROOT}/ur5_gripper.structural.srdf"
    ${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_RESOURCE_INPUTS})

  target_compile_definitions(test_cell_structure PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}"
    MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}")

  add_test(
    NAME MaintainedModelCatalogSources
    COMMAND
      "$<TARGET_FILE:manyforge_maintained_model_catalog>"
      --check
      --description "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
      --workspace-root "${_MANYFORGE_WORKSPACE_ROOT}"
      --prepared
      "lite6_lite_gripper=${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
      --prepared
      "ur10e_robotiq_2f_140=${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT}"
      --prepared
      "ur5_roboplan=${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_FIXTURE_ROOT}")

  add_test(
    NAME MaintainedDeploymentOccurrenceCompilation
    COMMAND
      "$<TARGET_FILE:manyforge_maintained_deployment_occurrences>"
      --check
      --workspace-root "${_MANYFORGE_WORKSPACE_ROOT}"
      --prepared
      "lite6_lite_gripper=${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
      --prepared
      "ur10e_robotiq_2f_140=${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT}"
      --prepared
      "ur5_roboplan=${MANYFORGE_UR5_ROBOPLAN_STRUCTURAL_FIXTURE_ROOT}")

  add_executable(test_model_source
    test/test_model_source.cpp)
  target_compile_definitions(test_model_source PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
  target_link_libraries(test_model_source PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_model_source
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  set(
    MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/structural_model_preparation")
  set(
    MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/structural_model_preparation_test")
  file(MAKE_DIRECTORY
    "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT}")
  execute_process(
    COMMAND
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
      --urdf
      "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}/source.urdf"
      --srdf
      "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}/source.srdf"
      --model-asset-id
      qualified_model
      --accepted-package-uri-prefix
      "package://qualified_models/example/"
      --resource-root
      "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}"
      --output-directory
      "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT}"
    RESULT_VARIABLE MANYFORGE_STRUCTURAL_PREPARATION_TEST_RESULT
    OUTPUT_VARIABLE MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT
    ERROR_VARIABLE MANYFORGE_STRUCTURAL_PREPARATION_TEST_ERROR)
  if(NOT MANYFORGE_STRUCTURAL_PREPARATION_TEST_RESULT EQUAL 0)
    message(FATAL_ERROR
      "structural-model provenance fixture generation failed:\n"
      "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT}"
      "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_ERROR}")
  endif()
  set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}/source.urdf"
    "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}/source.srdf"
    "${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}/meshes/tool.stl")

  add_executable(test_structural_model_preparation
    test/test_structural_model_preparation.cpp)
  target_compile_definitions(test_structural_model_preparation PRIVATE
    MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT="${MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT}"
    MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT="${MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT}")
  target_link_libraries(test_structural_model_preparation PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_structural_model_preparation
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

  add_executable(test_apply_affected_scope
    test/test_apply_affected_scope.cpp)
  target_include_directories(test_apply_affected_scope PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_apply_affected_scope PRIVATE
    manyforge_deployment_verification_core GTest::gtest_main)
  gtest_discover_tests(test_apply_affected_scope
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retained_bundle_store
    test/test_retained_bundle_store.cpp)
  target_compile_definitions(test_retained_bundle_store PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
  target_include_directories(test_retained_bundle_store PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_retained_bundle_store PRIVATE
    manyforge_deployment GTest::gtest_main)
  target_link_options(test_retained_bundle_store PRIVATE
    "-Wl,--wrap=fsync")
  gtest_discover_tests(test_retained_bundle_store
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  if(TARGET manyforge_deployment_verified_controller_lifecycle_expected_domain)
    add_executable(test_verified_controller_lifecycle_expected_domain
      test/test_verified_controller_lifecycle_expected_domain.cpp)
    target_compile_definitions(
      test_verified_controller_lifecycle_expected_domain PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
    target_include_directories(
      test_verified_controller_lifecycle_expected_domain PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_verified_controller_lifecycle_expected_domain PRIVATE
      manyforge_deployment_verified_controller_lifecycle_expected_domain
      manyforge_deployment
      GTest::gtest_main)
    gtest_discover_tests(test_verified_controller_lifecycle_expected_domain
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  endif()

  if(TARGET manyforge_deployment_resolved_stop_base_plan_resolver)
    add_executable(test_resolved_stop_base_plan_resolver
      test/test_resolved_stop_base_plan_resolver.cpp)
    target_compile_definitions(
      test_resolved_stop_base_plan_resolver PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
    target_include_directories(test_resolved_stop_base_plan_resolver PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_resolved_stop_base_plan_resolver PRIVATE
      manyforge_deployment
      manyforge_deployment_resolved_stop_base_plan_resolver
      GTest::gtest_main)
    gtest_discover_tests(test_resolved_stop_base_plan_resolver
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  endif()

  if(TARGET manyforge_deployment_component_lifecycle_current_snapshot_source)
    add_executable(test_component_lifecycle_current_snapshot_source
      test/test_component_lifecycle_current_snapshot_source.cpp)
    target_compile_definitions(
      test_component_lifecycle_current_snapshot_source PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
    target_include_directories(
      test_component_lifecycle_current_snapshot_source PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_component_lifecycle_current_snapshot_source PRIVATE
      manyforge_deployment
      manyforge_deployment_component_lifecycle_current_snapshot_source
      GTest::gtest_main)
    gtest_discover_tests(test_component_lifecycle_current_snapshot_source
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  endif()

  if(TARGET manyforge_deployment_cclc_runtime_composition)
    add_executable(test_cclc_open_request_server
      test/test_cclc_open_request_server.cpp)
    target_link_libraries(test_cclc_open_request_server PRIVATE
      manyforge_deployment_cclc_runtime_composition
      GTest::gtest_main)
    gtest_discover_tests(test_cclc_open_request_server
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

    add_executable(test_cclc_runtime_composition
      test/test_cclc_runtime_composition.cpp)
    target_compile_definitions(
      test_cclc_runtime_composition PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
    target_include_directories(test_cclc_runtime_composition PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_cclc_runtime_composition PRIVATE
      manyforge_deployment
      manyforge_deployment_cclc_runtime_composition
      GTest::gtest_main)
    gtest_discover_tests(test_cclc_runtime_composition
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
  endif()

  add_executable(test_catalog_artifact_store
    test/test_catalog_artifact_store.cpp)
  target_include_directories(test_catalog_artifact_store PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_catalog_artifact_store PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_catalog_artifact_store
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retained_bundle_materializer
    test/test_retained_bundle_materializer.cpp)
  target_compile_definitions(test_retained_bundle_materializer PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
  target_include_directories(test_retained_bundle_materializer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_retained_bundle_materializer PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_retained_bundle_materializer
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_contract_registry
    test/test_runtime_contract_registry.cpp)
  target_include_directories(test_runtime_contract_registry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_runtime_contract_registry PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_runtime_contract_registry
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_build_manifest
    test/test_build_manifest.cpp)
  target_include_directories(test_build_manifest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(test_build_manifest PRIVATE
    manyforge_deployment GTest::gtest_main)
  target_link_options(test_build_manifest PRIVATE
    "-Wl,--wrap=fsync")
  gtest_discover_tests(test_build_manifest
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retention_qualified_deployment
    test/test_retention_qualified_deployment.cpp)
  target_include_directories(test_retention_qualified_deployment PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_retention_qualified_deployment PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_retention_qualified_deployment
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(
    test_canonical_empty_admission
    test/test_canonical_empty_admission.cpp)
  target_include_directories(
    test_canonical_empty_admission PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    test_canonical_empty_admission PRIVATE
    manyforge_deployment
    manyforge_deployment_admission
    manyforge_deployment_canonical_package_generator
    GTest::gtest_main)
  gtest_discover_tests(
    test_canonical_empty_admission
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(
    test_package_admission_profile
    test/test_package_admission_profile.cpp)
  target_include_directories(
    test_package_admission_profile PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    test_package_admission_profile PRIVATE
    manyforge_deployment
    manyforge_deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(
    test_package_admission_profile
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_test(
    NAME CanonicalEmptyGeneratorCli
    COMMAND
      "${CMAKE_COMMAND}"
      "-DMANYFORGE_GENERATOR=$<TARGET_FILE:manyforge_generate_canonical_empty_package>"
      "-DMANYFORGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/canonical_empty_cli_package"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/canonical_empty_generator_cli.cmake")

  add_executable(test_runtime_policy_codec
    test/test_runtime_policy_codec.cpp)
  target_link_libraries(test_runtime_policy_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_runtime_policy_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_stop_evidence_policy_codec
    test/test_stop_evidence_policy_codec.cpp)
  target_link_libraries(test_stop_evidence_policy_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_stop_evidence_policy_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
