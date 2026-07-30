cmake_minimum_required(VERSION 3.18)
project(manyforge_e0_binding_proof LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Python 3.12 EXACT COMPONENTS Interpreter Development.Module REQUIRED)
execute_process(
  COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
  OUTPUT_VARIABLE pybind11_ROOT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
find_package(pybind11 CONFIG REQUIRED)
execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_VARIABLE nanobind_ROOT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
find_package(nanobind CONFIG REQUIRED)

set(MANYFORGE_SOURCE_DIR "" CACHE PATH "ManyForge source tree at the pinned proof commit")
set(E0_BINDING "both" CACHE STRING "pybind, nanobind, or both")
option(
  E0_BUILD_NANOBIND_PEER
  "Build a second nanobind module for bounded coexistence qualification"
  OFF)
if(NOT EXISTS "${MANYFORGE_SOURCE_DIR}/manyforge_deployment/CMakeLists.txt")
  message(FATAL_ERROR "MANYFORGE_SOURCE_DIR must name a ManyForge source tree")
endif()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(
  "${MANYFORGE_SOURCE_DIR}/manyforge_deployment"
  "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment")

if(E0_BINDING STREQUAL "both" OR E0_BINDING STREQUAL "pybind")
  pybind11_add_module(e0_pybind MODULE pybind_module.cpp)
  target_link_libraries(e0_pybind PRIVATE manyforge_deployment)
  target_include_directories(e0_pybind PRIVATE
    "${MANYFORGE_SOURCE_DIR}/manyforge_core/include")
  target_compile_options(e0_pybind PRIVATE
    -Wall -Wextra -Werror -pedantic)
  install(TARGETS e0_pybind LIBRARY DESTINATION .)
endif()

if(E0_BINDING STREQUAL "both" OR E0_BINDING STREQUAL "nanobind")
  nanobind_add_module(e0_nanobind NB_STATIC NOMINSIZE nanobind_module.cpp)
  target_link_libraries(e0_nanobind PRIVATE manyforge_deployment)
  target_include_directories(e0_nanobind PRIVATE
    "${MANYFORGE_SOURCE_DIR}/manyforge_core/include")
  target_compile_options(e0_nanobind PRIVATE
    -Wall -Wextra -Werror -pedantic)

  install(TARGETS e0_nanobind LIBRARY DESTINATION .)

  if(E0_BUILD_NANOBIND_PEER)
    nanobind_add_module(
      e0_nanobind_roboplan_peer NB_STATIC NOMINSIZE
      nanobind_roboplan_peer.cpp)
    target_include_directories(e0_nanobind_roboplan_peer PRIVATE
      "${MANYFORGE_SOURCE_DIR}/manyforge_core/include")
    target_compile_options(e0_nanobind_roboplan_peer PRIVATE
      -Wall -Wextra -Werror -pedantic)
  endif()
endif()
