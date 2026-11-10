cmake_minimum_required(VERSION 3.14)
project(manyforge_collision VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTING "Build unit tests" ON)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
add_compile_definitions(COAL_DISABLE_HPP_FCL_WARNINGS)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Coal / hpp-fcl currently emits deprecated-copy and hpp-fcl compatibility
# warnings through Pinocchio's public headers. Keep these suppressions local to
# the collision module and remove them once the pinned Coal/Pinocchio stack no
# longer emits those warnings under -Wall -Wextra -Werror.

find_package(Git QUIET)
set(_MANYFORGE_COLLISION_GIT_SHA "unknown")
set(_MANYFORGE_COLLISION_GIT_DIRTY "")
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}/.. rev-parse --short=12 HEAD
    OUTPUT_VARIABLE _MANYFORGE_COLLISION_GIT_SHA
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _MANYFORGE_COLLISION_GIT_SHA)
    set(_MANYFORGE_COLLISION_GIT_SHA "unknown")
  endif()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}/.. status --porcelain
    OUTPUT_VARIABLE _MANYFORGE_COLLISION_GIT_STATUS
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_MANYFORGE_COLLISION_GIT_STATUS)
    set(_MANYFORGE_COLLISION_GIT_DIRTY "-dirty")
  endif()
endif()
set(MANYFORGE_COLLISION_ENGINE_VERSION_OVERRIDE "" CACHE STRING
    "Explicit manyforge_collision engine version override")
if(MANYFORGE_COLLISION_ENGINE_VERSION_OVERRIDE)
  set(MANYFORGE_COLLISION_ENGINE_VERSION
      "${MANYFORGE_COLLISION_ENGINE_VERSION_OVERRIDE}")
else()
  # This must not be a cached derived value: reused container build volumes
  # are reconfigured across commits and must never retain a stale engine id.
  set(MANYFORGE_COLLISION_ENGINE_VERSION
      "${PROJECT_VERSION}+${_MANYFORGE_COLLISION_GIT_SHA}${_MANYFORGE_COLLISION_GIT_DIRTY}")
endif()

execute_process(
  COMMAND python3 -c
          "import site, pathlib; print(';'.join(str(pathlib.Path(p) / 'cmeel.prefix') for p in site.getsitepackages()))"
  OUTPUT_VARIABLE _CMEEL_PREFIXES
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

foreach(_prefix ${_CMEEL_PREFIXES})
  if(EXISTS "${_prefix}/include/boost/version.hpp")
    set(BOOST_ROOT "${_prefix}")
    set(BOOST_INCLUDEDIR "${_prefix}/include")
    set(BOOST_LIBRARYDIR "${_prefix}/lib")
    break()
  endif()
endforeach()

find_package(Eigen3 REQUIRED)
find_package(pinocchio REQUIRED)
find_package(OpenSSL REQUIRED)

# Core's own tests belong to the manyforge_core lane, not this one.
if(NOT TARGET manyforge_core)
  set(_MANYFORGE_COLLISION_BUILD_TESTING "${BUILD_TESTING}")
  set(BUILD_TESTING OFF)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core
                   ${CMAKE_CURRENT_BINARY_DIR}/manyforge_core
                   EXCLUDE_FROM_ALL)
  set(BUILD_TESTING "${_MANYFORGE_COLLISION_BUILD_TESTING}")
  unset(_MANYFORGE_COLLISION_BUILD_TESTING)
endif()

add_library(manyforge_collision
  src/manyforge_collision_engine.cpp
)

target_include_directories(manyforge_collision PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(manyforge_collision
  manyforge_core
  Eigen3::Eigen
  pinocchio::pinocchio
  OpenSSL::Crypto
)

target_compile_definitions(manyforge_collision PRIVATE
  MANYFORGE_COLLISION_ENGINE_VERSION="${MANYFORGE_COLLISION_ENGINE_VERSION}")

if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/collision_engine_integration_tests")
  get_filename_component(_MANYFORGE_COLLISION_TEST_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
  target_compile_definitions(test_collision_engine_integration PRIVATE
    MANYFORGE_COLLISION_TEST_SOURCE_ROOT="${_MANYFORGE_COLLISION_TEST_SOURCE_ROOT}")
endif()
