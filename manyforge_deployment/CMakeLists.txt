cmake_minimum_required(VERSION 3.14)
project(manyforge_deployment VERSION 0.2.0 LANGUAGES CXX)

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
find_package(ZLIB REQUIRED)
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
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior/manyforge_behavior/model_source_preparation.py")
set(
  MANYFORGE_STRUCTURAL_PREPARATION_CLIENT
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_structural_model.py")
set(
  MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_maintained_deployment_sources.py")
set(
  MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/maintained_deployment_sources.v1.json")
set(
  MANYFORGE_MAINTAINED_ASSISTANT_PROFILES
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/maintained_assistant_profiles.v2.json")
set(
  MANYFORGE_NODE_CATALOG_SOURCE
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior/resources/node_catalog.yaml")
set(
  MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_maintained_deployment_occurrences.py")
set(
  MANYFORGE_ACTIVATION_PACKAGE_PRODUCER
  "${CMAKE_CURRENT_SOURCE_DIR}/test/produce_activation_package.py")
if(NOT EXISTS "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}" OR
   NOT EXISTS "${MANYFORGE_NODE_ENVIRONMENT_CHECK}" OR
   NOT EXISTS "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}" OR
   NOT EXISTS "${MANYFORGE_STRUCTURAL_PREPARATION_CLIENT}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}" OR
   NOT EXISTS "${MANYFORGE_NODE_CATALOG_SOURCE}" OR
   NOT EXISTS "${MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR}")
  message(FATAL_ERROR
    "manyforge_deployment production retention requires the node-environment "
    "closure, structural-model preparation tool, and maintained source "
    "description")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
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
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}"
  "${MANYFORGE_NODE_ENVIRONMENT_CHECK}"
  "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK}"
  "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
  "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}"
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
  "${MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE}"
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/io_rule_stop_response.cpp"
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/io_signal_descriptor.cpp"
  "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/io_condition_evaluator.cpp")
list(
  REMOVE_ITEM
  MANYFORGE_COMPILER_SOURCE_INPUTS
  # These transport/test artifacts are downstream of the native compiler
  # and must not feed its own build identity through the recursive glob above.
# Retain the shared Program-binding fixture under its existing native name.
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/program_binding_lite6_native.fixture.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/program_binding_transport.v3.json"
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
  kEmbeddedRos2ControlExecutionRuntimeConfigurationSchema
  ros2_control_execution_runtime_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml
  mf_ros2_cfg)
manyforge_embed_closed_schema(
  kEmbeddedIoProviderComponentConfigurationSchema
  io_provider_component_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.io_provider_execution.v1.yaml
  mf_io_cfg)
manyforge_embed_closed_schema(
  kEmbeddedPoseSourceComponentConfigurationSchema
  pose_source_component_configuration
  resources/closed_schema/value/manyforge.value_schema.runtime.pose_source.v2.yaml
  mf_pose_cfg)
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
  # exact Core codec and cold timing translation units rather than inventing
  # parallel algorithms or requiring the full Kernel link closure.
  add_library(manyforge_deployment_runtime_contract_codec STATIC
    "${MANYFORGE_RUNTIME_CONTRACT_CODEC_SOURCE}"
    "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/stop_timing.cpp"
    "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/io_signal_descriptor.cpp"
    "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/io_condition_evaluator.cpp"
    "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/io_rule_stop_response.cpp"
    "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_core/src/component_lifecycle.cpp")
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

# The one Program expression engine (EXPRESSION_LANGUAGE_REFERENCE.md). It
# depends only on the standard library so the Program contract, the Python
# binding and the Program runner share it without the codec closure.
add_library(manyforge_expression src/expression.cpp)
add_library(manyforge::expression ALIAS manyforge_expression)
target_include_directories(manyforge_expression PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(
  manyforge_expression PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Canonical projection/parsing, semantic contract verification, and retained-
# bundle readback. This private substrate cannot mint retention
# authority and is deliberately free of materializer, repository-writer,
# package-generator, and admission-selection effects. The O1.4c runtime
# contract boundary reuses Core's verified runtime-contract codec, either
# through the concrete Core target or its exact standalone translation unit.
add_library(manyforge_deployment_verification_core
  src/assistant_settings_codec.cpp
  src/apply_affected_scope.cpp
  src/apply_component_delta.cpp
  src/component_spec.cpp
  src/artifact_codec.cpp
  src/composed_model_symbol_role_map_verifier.cpp
  src/control_domain_control_internal.cpp
  src/control_domain_control_verifier.cpp
  src/control_interface_contract_codec.cpp
  src/controller_topology_codec.cpp
  src/contact_policy_codec.cpp
  src/cell_graph_codec.cpp
  src/cell_kinematic_index.cpp
  src/closed_schema.cpp
  src/content_identity.cpp
  src/deployment_applicability_codec.cpp
  src/deployment_artifact_codec.cpp
  src/deployment_authoring.cpp
  src/deployment_catalog_codec.cpp
  src/model_asset_codec.cpp
  src/model_source.cpp
  src/execution_runtime_contract.cpp
  src/execution_host_unit_internal.cpp
  src/native_component_configuration.cpp
  src/ordinary_motion_jerk.cpp
  src/initial_scene_projection.cpp
  src/io_signal_catalog.cpp
  src/io_provider_domain.cpp
  src/ros2_control_execution_runtime_unit_internal.cpp
  src/planning_resource_map_internal.cpp
  src/posix_read_only_internal.cpp
  src/prepared_activation_input.cpp
  src/program_binding_outcome.cpp
  src/program_contract.cpp
  src/program_reference_facts.cpp
  src/program_reference_probe_internal.cpp
  src/program_reference_validator.cpp
  src/program_value_path.cpp
  src/program_binding.cpp
  src/program_io_condition_compiler.cpp
  src/retained_io_policy_compiler.cpp
  src/provider_manifest_codec.cpp
  src/capability_definition_codec.cpp
  src/command_schema_codec.cpp
  src/component_type_codec.cpp
  src/runtime_program_binding.cpp
  src/reference_runtime_unit_verifier.cpp
  src/runtime_contract_registry.cpp
  src/verified_retained_projection.cpp
  src/runtime_profile_codec.cpp
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
  manyforge::expression
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
    src/resolved_stop_base_plan_resolver.cpp)
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

# The pure retained policy compiler shares the existing verification owner.
# Preserve both historical target names without a second translation unit.
add_library(manyforge_deployment_program_io_condition_compiler ALIAS
  manyforge_deployment_verification_core)
add_library(manyforge::deployment_program_io_condition_compiler ALIAS
  manyforge_deployment_verification_core)

# Full offline compiler and writer surface. The read-only admission target is
# defined separately below; producer manifests remain content-addressed
# provenance and are not interpreted as current-reader compatibility profiles.
add_library(manyforge_deployment
  src/activation_content_collection.cpp
  src/activation_io_provider_internal.cpp
  src/activation_pose_source_internal.cpp
  src/activation_component_profile_internal.cpp
  src/activation_deployment_compiler_internal.cpp
  src/activation_deployment_intent_internal.cpp
  src/activation_model_source_closure_internal.cpp
  src/activation_prepared_model_closure_internal.cpp
  src/activation_prepared_model_internal.cpp
  src/build_manifest.cpp
  src/build_manifest_repository.cpp
  src/cell_applicability.cpp
  src/cell_compiler.cpp
  src/cell_composition.cpp
  src/cell_structure.cpp
  src/component_static_frames.cpp
  src/composed_model_symbol_role_map_internal.cpp
  src/mf2_joint_name_internal.cpp
  src/ros2_control_execution_runtime_unit_emitter.cpp
  src/posix_durability_internal.cpp
  src/program_binding_compiler.cpp
  src/prepared_activation_repository.cpp
  src/program_io_stop_response_validation.cpp
  src/global_io_rule_qualification.cpp
  src/reference_runtime_unit_internal.cpp
  src/retained_bundle.cpp
  src/retained_bundle_materializer.cpp
  src/retained_bundle_store.cpp
  src/retention_qualified_deployment.cpp
  src/scenario_program_admission.cpp
  src/site_overlay_resolution.cpp
  src/stop_deceleration_coherence_internal.cpp
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
target_link_libraries(manyforge_deployment PRIVATE ZLIB::ZLIB)
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
add_library(manyforge_preparation_support STATIC
  src/activation_materialization_internal.cpp
  src/prepared_activation_candidate.cpp
  src/prepared_selector_facts.cpp)
set_target_properties(manyforge_preparation_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(manyforge_preparation_support PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(manyforge_preparation_support PRIVATE
  manyforge::deployment
  manyforge_deployment_canonical_package_generator)

add_executable(manyforge_prepare_activation
  tools/prepare_activation.cpp)
target_include_directories(manyforge_prepare_activation PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(manyforge_prepare_activation PRIVATE
  manyforge_preparation_support
  manyforge::deployment)
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
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_CHECK}"
      --check
      --input "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
      --workspace-root "${_MANYFORGE_WORKSPACE_ROOT}")

  add_test(
    NAME MaintainedDeploymentOccurrenceSources
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_MAINTAINED_DEPLOYMENT_OCCURRENCE_GENERATOR}"
      --check
      --description "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
      --assistant-profiles "${MANYFORGE_MAINTAINED_ASSISTANT_PROFILES}"
      --workspace-root "${_MANYFORGE_WORKSPACE_ROOT}")
  set_tests_properties(
    MaintainedDeploymentOccurrenceSources
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_test(
    NAME DualRobotScenarioSemantics
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider
      "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior/tests/test_dual_robot_scenario.py")
  set_tests_properties(
    DualRobotScenarioSemantics
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

  add_executable(test_io_provider_domain test/test_io_provider_domain.cpp)
  target_compile_definitions(test_io_provider_domain PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_io_provider_domain PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_io_provider_domain PRIVATE
    manyforge_deployment manyforge_deployment_canonical_package_generator GTest::gtest_main)
  gtest_discover_tests(test_io_provider_domain PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_io_provider test/test_activation_io_provider.cpp)
  target_compile_definitions(test_activation_io_provider PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_activation_io_provider PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_io_provider PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_io_provider PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_pose_source test/test_activation_pose_source.cpp)
  target_compile_definitions(test_activation_pose_source PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_activation_pose_source PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_pose_source PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_pose_source PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_attachments test/test_activation_attachments.cpp)
  target_include_directories(test_activation_attachments PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_attachments PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_attachments PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_ai_model test/test_activation_ai_model.cpp)
  target_compile_definitions(test_activation_ai_model PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_activation_ai_model PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_ai_model PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_ai_model PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_component_lane test/test_activation_component_lane.cpp)
  target_compile_definitions(test_activation_component_lane PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_activation_component_lane PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_component_lane PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_component_lane
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_machine test/test_activation_machine.cpp)
  target_compile_definitions(test_activation_machine PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_activation_machine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_machine PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_machine
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_sensor test/test_activation_sensor.cpp)
  target_compile_definitions(test_activation_sensor PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_activation_sensor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_sensor PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_sensor
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_io_signal_catalog test/test_io_signal_catalog.cpp)
  target_include_directories(test_io_signal_catalog PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_io_signal_catalog PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_io_signal_catalog
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

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
  if(NOT TARGET manyforge_core)
    target_sources(test_program_binding PRIVATE
      "${MANYFORGE_CORE_SOURCE_DIR}/src/runtime_participant_binding.cpp")
  endif()
  target_compile_definitions(test_program_binding PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_program_binding PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_binding PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_program_binding
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  if(TARGET manyforge_deployment_program_io_condition_compiler)
    add_executable(test_program_io_condition_compiler
      test/test_program_io_condition_compiler.cpp
      test/test_retained_io_policy_compiler.cpp)
    if(NOT TARGET manyforge_core)
      # The standalone test directly exercises the Core Stop profile codec.
      # Include its translation-unit dependencies without changing production libraries.
      target_sources(test_program_io_condition_compiler PRIVATE
        "${MANYFORGE_CORE_SOURCE_DIR}/src/prepared_participant_stop_plan_codec.cpp"
        "${MANYFORGE_CORE_SOURCE_DIR}/src/resolved_stop_base_plan_codec.cpp"
        "${MANYFORGE_CORE_SOURCE_DIR}/src/stop_profile_mapper.cpp"
        "${MANYFORGE_CORE_SOURCE_DIR}/src/interrupt_structured_value.cpp"
        "${MANYFORGE_CORE_SOURCE_DIR}/src/execution_stop_support.cpp")
    endif()
    target_compile_definitions(test_program_io_condition_compiler PRIVATE
      MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}"
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/program_capture_package"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
    target_include_directories(test_program_io_condition_compiler PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_program_io_condition_compiler PRIVATE
      manyforge_preparation_support
      manyforge_deployment_program_io_condition_compiler manyforge_deployment GTest::gtest_main)
    gtest_discover_tests(test_program_io_condition_compiler
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
        FIXTURES_REQUIRED ProgramCapturePackage)

    add_executable(test_program_io_stop_response_validation
      test/test_program_io_stop_response_validation.cpp)
    target_compile_definitions(test_program_io_stop_response_validation PRIVATE
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/program_capture_package"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
    target_include_directories(test_program_io_stop_response_validation PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_program_io_stop_response_validation PRIVATE
      manyforge_deployment_program_io_condition_compiler manyforge_deployment GTest::gtest_main)
    gtest_discover_tests(test_program_io_stop_response_validation
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
        FIXTURES_REQUIRED ProgramCapturePackage)
  endif()

  add_executable(test_program_contract
    test/test_program_contract.cpp)
  target_compile_definitions(test_program_contract PRIVATE
    MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/program_capture_package"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper"
    MANYFORGE_VALUE_SOURCE_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/resources/expression/program_value_source_conformance.v1.yaml"
    MANYFORGE_POSE_VALUE_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/resources/expression/program_pose_value_conformance.v1.yaml")
  target_include_directories(test_program_contract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_contract PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_program_contract
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      FIXTURES_REQUIRED ProgramCapturePackage)
  # The prepared activation of the Lite6 pick-and-place Scenario under the cell
  # the capture tests read, produced by the ordinary activation preparer.
  add_test(NAME ProgramCapturePackage
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "${Python3_EXECUTABLE}" "${MANYFORGE_ACTIVATION_PACKAGE_PRODUCER}"
      --tool "$<TARGET_FILE:manyforge_prepare_activation>"
      --workspace "${_MANYFORGE_WORKSPACE_ROOT}"
      --package-root "${CMAKE_CURRENT_BINARY_DIR}/program_capture_package"
      --scenario "examples/mock_lite6_pick_and_place.scenario=lite6_pick_place_cell")
  set_tests_properties(ProgramCapturePackage PROPERTIES
    FIXTURES_SETUP ProgramCapturePackage TIMEOUT 120)

  add_executable(test_program_binding_compiler
    test/test_program_binding_compiler.cpp)
  target_compile_definitions(test_program_binding_compiler PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_program_binding_compiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_binding_compiler PRIVATE
    manyforge_deployment GTest::gtest_main)
  target_link_libraries(test_program_binding_compiler PRIVATE
    manyforge_deployment_canonical_package_generator manyforge_preparation_support)
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
  target_include_directories(test_cell_applicability PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(test_cell_applicability PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
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

  add_executable(test_closed_schema
    test/test_closed_schema.cpp)
  target_compile_definitions(test_closed_schema PRIVATE
    MANYFORGE_CLOSED_SCHEMA_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/closed_schema"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
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
  target_compile_definitions(test_deployment_artifact_codec PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
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

  function(manyforge_prepare_maintained_model
      family_id label urdf_path srdf_path package_prefix
      resource_root output_variable)
    # One spelling per source file: a graph that also adds another package
    # naming the same model (aggregate) must not list it twice.
    get_filename_component(urdf_path "${urdf_path}" REALPATH)
    get_filename_component(srdf_path "${srdf_path}" REALPATH)
    get_filename_component(resource_root "${resource_root}" REALPATH)
    set(fixture_root
      "${CMAKE_CURRENT_BINARY_DIR}/generated/${family_id}_structural")
    file(GLOB_RECURSE resource_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false
      "${resource_root}/meshes/*")
    file(MAKE_DIRECTORY "${fixture_root}")
    execute_process(
      COMMAND
        "${Python3_EXECUTABLE}"
        "${MANYFORGE_STRUCTURAL_PREPARATION_CLIENT}"
        --urdf "${urdf_path}"
        --srdf "${srdf_path}"
        --model-asset-id "${family_id}"
        --accepted-package-uri-prefix "${package_prefix}"
        --resource-root "${resource_root}"
        --output-directory "${fixture_root}"
      RESULT_VARIABLE preparation_result
      OUTPUT_VARIABLE preparation_output
      ERROR_VARIABLE preparation_error)
    if(NOT preparation_result EQUAL 0)
      message(FATAL_ERROR
        "${label} structural-model qualification preparation failed:\n"
        "${preparation_output}${preparation_error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${MANYFORGE_STRUCTURAL_PREPARATION_TOOL}"
      "${MANYFORGE_STRUCTURAL_PREPARATION_CLIENT}"
      "${urdf_path}"
      "${srdf_path}"
      ${resource_inputs}
      ${ARGN})
    # Build-time consumers re-read these sources beside the prepared receipt.
    set(MANYFORGE_MAINTAINED_MODEL_SOURCE_INPUTS
      ${MANYFORGE_MAINTAINED_MODEL_SOURCE_INPUTS}
      "${urdf_path}"
      "${srdf_path}"
      ${resource_inputs}
      ${ARGN}
      PARENT_SCOPE)
    set(${output_variable} "${fixture_root}" PARENT_SCOPE)
  endfunction()

  # One preparation per model family row of the maintained description,
  # so a family added as data needs no CMake edit. URDF/SRDF and the resource
  # root come from the row's `source`; distinct historical files are extra
  # inputs. A family whose resources may come from an installed package names
  # a sentinel file here: the local root is used only when it holds that file,
  # otherwise the package named by the accepted `package://` prefix.
  # The strict family-row loaders accept no extra field, so it stays here.
  set(MANYFORGE_INSTALLED_MODEL_SENTINEL_ur5_roboplan "meshes/visual/base.dae")
  file(READ "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}"
    _maintained_description)
  string(JSON _family_count LENGTH "${_maintained_description}" model_families)
  math(EXPR _family_last "${_family_count} - 1")
  set(MANYFORGE_MAINTAINED_FAMILY_IDS "")
  set(MANYFORGE_MAINTAINED_PREPARED_ARGUMENTS "")
  set(MANYFORGE_MAINTAINED_PREPARATION_MANIFESTS "")
  foreach(_family_index RANGE ${_family_last})
    string(JSON _family_id GET "${_maintained_description}"
      model_families ${_family_index} model_family_id)
    foreach(_field urdf_path srdf_path historical_urdf_path historical_srdf_path
        resource_root accepted_package_uri_prefix)
      string(JSON _family_${_field} GET "${_maintained_description}"
        model_families ${_family_index} source ${_field})
    endforeach()
    set(_family_resource_root
      "${_MANYFORGE_WORKSPACE_ROOT}/${_family_resource_root}")
    set(_family_sentinel "${MANYFORGE_INSTALLED_MODEL_SENTINEL_${_family_id}}")
    if(_family_sentinel AND
       NOT EXISTS "${_family_resource_root}/${_family_sentinel}")
      string(REGEX MATCH "^package://([^/]+)/(.*)/$" _package_match
        "${_family_accepted_package_uri_prefix}")
      set(_package_name "${CMAKE_MATCH_1}")
      set(_package_path "${CMAKE_MATCH_2}")
      find_package(${_package_name} CONFIG REQUIRED)
      get_filename_component(_family_installed_root
        "${${_package_name}_DIR}/../../../share/${_package_name}/${_package_path}"
        REALPATH)
      foreach(_field historical_urdf_path historical_srdf_path)
        string(REPLACE "${_family_resource_root}" "${_family_installed_root}"
          _family_${_field}
          "${_MANYFORGE_WORKSPACE_ROOT}/${_family_${_field}}")
      endforeach()
      set(_family_resource_root "${_family_installed_root}")
      if(NOT EXISTS "${_family_resource_root}/${_family_sentinel}")
        message(FATAL_ERROR
          "model family ${_family_id} requires the installed example model resources")
      endif()
    else()
      foreach(_field historical_urdf_path historical_srdf_path)
        set(_family_${_field}
          "${_MANYFORGE_WORKSPACE_ROOT}/${_family_${_field}}")
      endforeach()
    endif()
    set(_family_extra_inputs "")
    foreach(_kind urdf srdf)
      set(_family_${_kind}_path
        "${_MANYFORGE_WORKSPACE_ROOT}/${_family_${_kind}_path}")
      if(NOT EXISTS "${_family_historical_${_kind}_path}")
        message(FATAL_ERROR
          "model family ${_family_id} requires ${_family_historical_${_kind}_path}")
      endif()
      if(NOT _family_historical_${_kind}_path STREQUAL _family_${_kind}_path)
        list(APPEND _family_extra_inputs "${_family_historical_${_kind}_path}")
      endif()
    endforeach()
    manyforge_prepare_maintained_model(
      ${_family_id} "${_family_id}"
      "${_family_urdf_path}"
      "${_family_srdf_path}"
      "${_family_accepted_package_uri_prefix}"
      "${_family_resource_root}"
      MANYFORGE_STRUCTURAL_FIXTURE_ROOT_${_family_id}
      ${_family_extra_inputs})
    list(APPEND MANYFORGE_MAINTAINED_FAMILY_IDS ${_family_id})
    list(APPEND MANYFORGE_MAINTAINED_PREPARED_ARGUMENTS --prepared
      "${_family_id}=${MANYFORGE_STRUCTURAL_FIXTURE_ROOT_${_family_id}}")
    list(APPEND MANYFORGE_MAINTAINED_PREPARATION_MANIFESTS
      "${MANYFORGE_STRUCTURAL_FIXTURE_ROOT_${_family_id}}/preparation-manifest.json")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MANYFORGE_MAINTAINED_DEPLOYMENT_SOURCE_DESCRIPTION}")
  # The fixtures several tests name directly: the combined example models
  # (kept for their meshes), prepared for structural tests only. They are no
  # maintained family; the split Lite6 roots are this root's
  # `generated/<asset>_structural` siblings.
  set(MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  set(MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/ur10e_robotiq_2f_140")
  manyforge_prepare_maintained_model(
    lite6_lite_gripper "lite6_lite_gripper"
    "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/lite6_lite_gripper.urdf"
    "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}/lite6_lite_gripper.srdf"
    "package://manyforge_example_models/models/lite6_lite_gripper/"
    "${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT)
  manyforge_prepare_maintained_model(
    ur10e_robotiq_2f_140 "ur10e_robotiq_2f_140"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.structural.urdf"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}/ur10e_robotiq_2f_140.structural.srdf"
    "package://manyforge_example_models/models/ur10e_robotiq_2f_140/"
    "${MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_SOURCE_ROOT}"
    MANYFORGE_UR10E_ROBOTIQ_STRUCTURAL_FIXTURE_ROOT)

  # Process qualification needs the exact cell-scoped empty predecessor
  # before it can exercise ordinary project-folder activation. This generated
  # tree is used only as Manager bootstrap input; Composer discovers the
  # repository Scenario projects directly and never treats it as a catalog.
  set(MANYFORGE_DUAL_ROBOT_SCENARIO_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/dual-robot-scenario-fixture")
  set(MANYFORGE_DUAL_ROBOT_SCENARIO_PACKAGE_ROOT
    "${MANYFORGE_DUAL_ROBOT_SCENARIO_FIXTURE_ROOT}/retained-package")
  set(MANYFORGE_DUAL_ROBOT_SCENARIO_STAMP
    "${MANYFORGE_DUAL_ROBOT_SCENARIO_FIXTURE_ROOT}/materialized.stamp")
  file(GLOB_RECURSE MANYFORGE_DUAL_ROBOT_SCENARIO_INPUTS
    CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/*"
    "${_MANYFORGE_WORKSPACE_ROOT}/examples/mock_dual_arm_shared_group.scenario/*")
  add_custom_command(
    OUTPUT "${MANYFORGE_DUAL_ROBOT_SCENARIO_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_DUAL_ROBOT_SCENARIO_FIXTURE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_DUAL_ROBOT_SCENARIO_PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "${Python3_EXECUTABLE}" "${MANYFORGE_ACTIVATION_PACKAGE_PRODUCER}"
      --tool "$<TARGET_FILE:manyforge_prepare_activation>"
      --workspace "${_MANYFORGE_WORKSPACE_ROOT}"
      --package-root "${MANYFORGE_DUAL_ROBOT_SCENARIO_PACKAGE_ROOT}"
      --publish-canonical-empty
      --scenario "examples/mock_dual_arm_shared_group.scenario=dual_robot_wall_cell"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${MANYFORGE_DUAL_ROBOT_SCENARIO_STAMP}"
    DEPENDS
      manyforge_prepare_activation
      "${MANYFORGE_ACTIVATION_PACKAGE_PRODUCER}"
      ${MANYFORGE_DUAL_ROBOT_SCENARIO_INPUTS}
      "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}"
      "${MANYFORGE_NODE_CATALOG_SOURCE}"
      ${MANYFORGE_MAINTAINED_MODEL_SOURCE_INPUTS}
    VERBATIM)
  # Part of the default test build, like the other process fixtures: the
  # process tests consume this tree directly and must never see a stale one.
  add_custom_target(manyforge_dual_robot_cell_bootstrap ALL
    DEPENDS "${MANYFORGE_DUAL_ROBOT_SCENARIO_STAMP}")

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
      ${MANYFORGE_MAINTAINED_PREPARED_ARGUMENTS})

  # Programs with expressions validate through the manyforge_headless_py
  # engine binding, which this graph does not build: the caller's PYTHONPATH
  # (scripts/test-all.sh setup_expression_binding) is kept after the sources.
  function(manyforge_add_prepare_activation_test test_name scenario)
    add_test(
      NAME "${test_name}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/prepare_activation_tool.py"
        --tool "$<TARGET_FILE:manyforge_prepare_activation>"
        --workspace "${_MANYFORGE_WORKSPACE_ROOT}"
        --scenario "${scenario}"
        ${ARGN})
    set_tests_properties("${test_name}" PROPERTIES TIMEOUT 180
      ENVIRONMENT_MODIFICATION
        "PYTHONPATH=path_list_prepend:${_MANYFORGE_WORKSPACE_ROOT};PYTHONPATH=path_list_prepend:${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior")
  endfunction()

  manyforge_add_prepare_activation_test(
    PrepareActivationSingleDocument
    "examples/mock_lite6_pick_and_place.scenario")
  manyforge_add_prepare_activation_test(
    PrepareActivationNoProgram
    "examples/mock_lite6_pick_and_place.scenario"
    --no-program)
  manyforge_add_prepare_activation_test(
    PrepareActivationMovingTakeoverBudget
    "examples/mock_lite6_pick_and_place.scenario"
    --takeover-budget)
  manyforge_add_prepare_activation_test(
    PrepareActivationRuntimeIdentity
    "examples/mock_lite6_pick_and_place.scenario"
    --runtime-identity)
  manyforge_add_prepare_activation_test(
    PrepareActivationDualDocument
    "examples/mock_dual_arm_shared_group.scenario")
  # UF850 on Mock, with the xArm gripper mounted as its own
  # tool component.
  manyforge_add_prepare_activation_test(
    PrepareActivationUf850MockSplit
    "examples/mock_uf850_xarm_gripper.scenario")
  manyforge_add_prepare_activation_test(
    PrepareActivationAssignmentRejections
    "examples/mock_dual_arm_shared_group.scenario"
    --negative-matrix)
  manyforge_add_prepare_activation_test(
    PrepareActivationSourceClosureRejections
    "examples/mock_dual_arm_shared_group.scenario"
    --source-closure-rejections)
  manyforge_add_prepare_activation_test(
    PrepareActivationIntentRejections
    "examples/mock_dual_arm_shared_group.scenario"
    --intent-rejections)
  manyforge_add_prepare_activation_test(
    PrepareActivationStopLimitNames
    "examples/mock_lite6_pick_and_place.scenario"
    --stop-limit-names)
  manyforge_add_prepare_activation_test(
    PrepareActivationRealMeasured
    "examples/real_lite6_arm_observation.scenario"
    --real-measured)
  manyforge_add_prepare_activation_test(
    PrepareActivationRos2Control
    "examples/sim_ur10e_pick_and_place.scenario"
    --ros2-control)
  # The mounted lane-free pneumatic tool of the split Scenario.
  manyforge_add_prepare_activation_test(
    PrepareActivationToolSplit
    "examples/mock_lite6_pneumatic_pick_and_place.scenario"
    --tool-split)
  manyforge_add_prepare_activation_test(
    PrepareActivationVisionBinPick
    "examples/mock_ur5_vision_bin_pick.scenario")
  manyforge_add_prepare_activation_test(
    PrepareActivationVisionPosePick
    "examples/mock_ur5_vision_pose_pick.scenario")

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
      "${MANYFORGE_STRUCTURAL_PREPARATION_CLIENT}"
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

  add_executable(test_program_binding_outcome
    test/test_program_binding_outcome.cpp)
  target_link_libraries(test_program_binding_outcome PRIVATE
    manyforge_deployment_verification_core GTest::gtest_main)
  gtest_discover_tests(test_program_binding_outcome
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_scenario_activation_empty_map
    test/test_scenario_activation_empty_map.cpp)
  target_compile_definitions(test_scenario_activation_empty_map PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}")
  target_include_directories(test_scenario_activation_empty_map PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_scenario_activation_empty_map PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_scenario_activation_empty_map
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_activation_catalog_presence
    test/test_activation_catalog_presence.cpp)
  target_compile_definitions(test_activation_catalog_presence PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_activation_catalog_presence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_catalog_presence PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_catalog_presence
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_prepared_selector_facts test/test_prepared_selector_facts.cpp)
  target_compile_definitions(test_prepared_selector_facts PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_WORKSPACE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
  target_include_directories(test_prepared_selector_facts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_prepared_selector_facts PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_prepared_selector_facts
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_apply_affected_scope
    test/test_apply_affected_scope.cpp)
  target_include_directories(test_apply_affected_scope PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_apply_affected_scope PRIVATE
    manyforge_deployment_verification_core GTest::gtest_main)
  gtest_discover_tests(test_apply_affected_scope
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_apply_component_delta
    test/test_apply_component_delta.cpp)
  target_compile_definitions(test_apply_component_delta PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
  target_include_directories(test_apply_component_delta PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_apply_component_delta PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_apply_component_delta
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  # Every example Scenario's Deployment compiled
  # into its sealed retained bundle (no Program), for the component spec
  # tests: parity with the earlier hand-list identity and attachment edits.
  set(MANYFORGE_EXAMPLE_BUNDLE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/example-bundles")
  set(MANYFORGE_EXAMPLE_BUNDLE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/example-bundles.stamp")
  file(GLOB_RECURSE MANYFORGE_EXAMPLE_SCENARIO_INPUTS
    CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${_MANYFORGE_WORKSPACE_ROOT}/examples/*.scenario/*")
  add_custom_command(
    OUTPUT "${MANYFORGE_EXAMPLE_BUNDLE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT}"
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/produce_example_bundles.py"
      --tool "$<TARGET_FILE:manyforge_prepare_activation>"
      --workspace "${_MANYFORGE_WORKSPACE_ROOT}"
      --output "${MANYFORGE_EXAMPLE_BUNDLE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${MANYFORGE_EXAMPLE_BUNDLE_STAMP}"
    DEPENDS
      manyforge_prepare_activation
      "${CMAKE_CURRENT_SOURCE_DIR}/test/produce_example_bundles.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/prepare_activation_tool.py"
      ${MANYFORGE_EXAMPLE_SCENARIO_INPUTS}
      "${MANYFORGE_NODE_ENVIRONMENT_MANIFEST}"
      "${MANYFORGE_NODE_CATALOG_SOURCE}"
      ${MANYFORGE_MAINTAINED_MODEL_SOURCE_INPUTS}
    VERBATIM)
  add_custom_target(manyforge_example_bundles ALL
    DEPENDS "${MANYFORGE_EXAMPLE_BUNDLE_STAMP}")

  add_executable(test_component_spec
    test/test_component_spec.cpp)
  add_dependencies(test_component_spec manyforge_example_bundles)
  target_compile_definitions(test_component_spec PRIVATE
    MANYFORGE_EXAMPLE_BUNDLE_ROOT="${MANYFORGE_EXAMPLE_BUNDLE_ROOT}"
    MANYFORGE_HOST_CONFIGURATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/resources/closed_schema/value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml")
  target_include_directories(test_component_spec PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_component_spec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_component_spec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  # The compiled launch description and joint lists, their parity
  # with the host-built ones on every example (the examples' retained
  # packages are kept under example-bundles/packages), and the static frames.
  add_executable(test_component_description
    test/test_component_description.cpp)
  add_dependencies(test_component_description manyforge_example_bundles)
  target_compile_definitions(test_component_description PRIVATE
    MANYFORGE_EXAMPLE_BUNDLE_ROOT="${MANYFORGE_EXAMPLE_BUNDLE_ROOT}")
  target_include_directories(test_component_description PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_component_description PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_component_description
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  # The cell kinematic index and its equivalence oracle against verbatim
  # copies of the earlier helpers it replaces.
  add_executable(test_cell_kinematic_index
    test/test_cell_kinematic_index.cpp)
  target_link_libraries(test_cell_kinematic_index PRIVATE
    manyforge_deployment_verification_core GTest::gtest_main)
  gtest_discover_tests(test_cell_kinematic_index
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_retained_bundle_store
    test/test_retained_bundle_store.cpp)
  target_compile_definitions(test_retained_bundle_store PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
  target_include_directories(test_retained_bundle_store PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_retained_bundle_store PRIVATE
    manyforge_deployment manyforge_deployment_canonical_package_generator
    GTest::gtest_main)
  # The test defines fsync to inject directory-fsync faults.
  target_link_libraries(test_retained_bundle_store PRIVATE ${CMAKE_DL_LIBS})
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

  add_executable(test_activation_content_collection
    test/test_activation_content_collection.cpp)
  target_compile_definitions(test_activation_content_collection PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT}")
  target_include_directories(test_activation_content_collection PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_activation_content_collection PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_activation_content_collection
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

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

  add_executable(test_stop_deceleration_coherence
    test/test_stop_deceleration_coherence.cpp)
  target_include_directories(test_stop_deceleration_coherence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_stop_deceleration_coherence PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_stop_deceleration_coherence
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_ordinary_motion_jerk test/test_ordinary_motion_jerk.cpp)
  target_link_libraries(test_ordinary_motion_jerk PRIVATE manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_ordinary_motion_jerk
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_build_manifest
    test/test_build_manifest.cpp)
  target_include_directories(test_build_manifest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(test_build_manifest PRIVATE
    manyforge_deployment GTest::gtest_main)
  # The test defines fsync to inject directory-fsync faults.
  target_link_libraries(test_build_manifest PRIVATE ${CMAKE_DL_LIBS})
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
      "-DMANYFORGE_ROS_PROFILE=${CMAKE_CURRENT_SOURCE_DIR}/resources/maintained/catalog/runtime_profile/manyforge.ros2_control_execution_runtime.json"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/canonical_empty_generator_cli.cmake")

  add_executable(test_stop_evidence_policy_codec
    test/test_stop_evidence_policy_codec.cpp)
  target_link_libraries(test_stop_evidence_policy_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_stop_evidence_policy_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_prepared_catalog_source test/test_prepared_catalog_source.cpp)
  target_include_directories(test_prepared_catalog_source PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_prepared_catalog_source PRIVATE
    manyforge_preparation_support manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_prepared_catalog_source
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_program_value_path test/test_program_value_path.cpp)
  target_compile_definitions(test_program_value_path PRIVATE
    MANYFORGE_VALUE_PATH_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/resources/expression/value_path_conformance.v1.yaml")
  target_link_libraries(test_program_value_path PRIVATE
    manyforge_deployment PkgConfig::LIBYAML GTest::gtest_main)
  gtest_discover_tests(test_program_value_path
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  add_executable(test_expression test/test_expression.cpp)
  target_compile_definitions(test_expression PRIVATE
    MANYFORGE_EXPRESSION_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/resources/expression/conformance.v1.yaml")
  target_link_libraries(test_expression PRIVATE
    manyforge_expression PkgConfig::LIBYAML GTest::gtest_main)
  gtest_discover_tests(test_expression
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
