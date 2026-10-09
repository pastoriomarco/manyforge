# The shared PreparedMotion builder and the sealed participant/trajectory
# codecs it decodes, compiled once. Core links this target instead of
# compiling these sources; a cross-process receiver (the ros2_control
# controller plugin) links the same target without the Core graph:
#
#   if(NOT TARGET manyforge_prepared_motion)
#     add_subdirectory(<manyforge>/manyforge_core/prepared_motion
#                      ${CMAKE_CURRENT_BINARY_DIR}/manyforge_prepared_motion
#                      EXCLUDE_FROM_ALL)
#   endif()
#   target_link_libraries(<target> PRIVATE manyforge::prepared_motion)
#
# Dependencies: manyforge_motion_numerics (sibling subdirectory) and
# manyforge_content_identity (OpenSSL, double-conversion, utf8proc), both
# added here when absent. Header-only Core types come from ../include.
cmake_minimum_required(VERSION 3.14)
project(manyforge_prepared_motion LANGUAGES CXX)

get_filename_component(_MANYFORGE_PREPARED_MOTION_CORE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(NOT TARGET manyforge_motion_numerics)
  add_subdirectory("${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/motion_numerics"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_motion_numerics" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET manyforge_content_identity)
  if(NOT MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR)
    get_filename_component(MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
      "${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/../manyforge_content_identity" ABSOLUTE)
  endif()
  add_subdirectory("${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_content_identity" EXCLUDE_FROM_ALL)
endif()

add_library(manyforge_prepared_motion STATIC
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/execution/prepared_motion_builder.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/prepared_operation_participant_codec.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/prepared_participant_stop_plan_codec.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/prepared_motion_evidence_codec.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/prepared_planning_codec.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/resolved_stop_base_plan_codec.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/resolved_stop_base_plan_decoder.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/runtime_participant_binding.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/runtime_participant_stop_readiness.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/controller_lifecycle.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/stop_profile_mapper.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/stop_timing.cpp
  ${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/src/interrupt_structured_value.cpp
)
add_library(manyforge::prepared_motion ALIAS manyforge_prepared_motion)
target_include_directories(manyforge_prepared_motion PUBLIC
  $<BUILD_INTERFACE:${_MANYFORGE_PREPARED_MOTION_CORE_DIR}/include>)
target_link_libraries(manyforge_prepared_motion
  PUBLIC manyforge::motion_numerics
  PRIVATE manyforge::content_identity)
target_compile_features(manyforge_prepared_motion PUBLIC cxx_std_20)
# One builder result in every process: no compiler-chosen FMA contraction.
target_compile_options(manyforge_prepared_motion PRIVATE
  -Wall -Wextra -Werror -pedantic -ffp-contract=off)
set_target_properties(manyforge_prepared_motion PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
unset(_MANYFORGE_PREPARED_MOTION_CORE_DIR)
