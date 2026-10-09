# Shared real-time motion numerics: the motion step, the runtime-scale
# governor and the trajectory sampler, compiled once. Consumable on its own
# (e.g. by a ros2_control controller plugin) without the Core graph:
#
#   if(NOT TARGET manyforge_motion_numerics)
#     add_subdirectory(<manyforge>/manyforge_core/motion_numerics
#                      ${CMAKE_CURRENT_BINARY_DIR}/manyforge_motion_numerics
#                      EXCLUDE_FROM_ALL)
#   endif()
#   target_link_libraries(<target> PRIVATE manyforge::motion_numerics)
#
# Depends only on the C++ standard library and the header-only Core type
# headers under ../include. No find_package; keep it that way.
cmake_minimum_required(VERSION 3.14)
project(manyforge_motion_numerics LANGUAGES CXX)

get_filename_component(_MANYFORGE_MOTION_NUMERICS_CORE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(manyforge_motion_numerics STATIC
  ${_MANYFORGE_MOTION_NUMERICS_CORE_DIR}/src/execution/motion_step.cpp
  ${_MANYFORGE_MOTION_NUMERICS_CORE_DIR}/src/runtime_scale_governor.cpp
  ${_MANYFORGE_MOTION_NUMERICS_CORE_DIR}/src/trajectory_sampler.cpp
)
add_library(manyforge::motion_numerics ALIAS manyforge_motion_numerics)
target_include_directories(manyforge_motion_numerics PUBLIC
  $<BUILD_INTERFACE:${_MANYFORGE_MOTION_NUMERICS_CORE_DIR}/include>)
target_compile_features(manyforge_motion_numerics PUBLIC cxx_std_20)
target_compile_options(manyforge_motion_numerics PRIVATE -Wall -Wextra -Werror -pedantic)
set_target_properties(manyforge_motion_numerics PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
unset(_MANYFORGE_MOTION_NUMERICS_CORE_DIR)
