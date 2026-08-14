cmake_minimum_required(VERSION 3.14)
project(manyforge_lite6_product VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Werror -pedantic)

set(
  MANYFORGE_CORE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core"
  CACHE PATH
  "Path to the ROS-independent ManyForge Core source tree")
if(NOT TARGET manyforge_core)
  if(NOT EXISTS "${MANYFORGE_CORE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "manyforge_lite6_product requires manyforge_core")
  endif()
  set(_MANYFORGE_LITE6_BUILD_TESTING "${BUILD_TESTING}")
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_CORE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
    EXCLUDE_FROM_ALL)
  set(BUILD_TESTING "${_MANYFORGE_LITE6_BUILD_TESTING}")
  unset(_MANYFORGE_LITE6_BUILD_TESTING)
endif()

add_library(manyforge_lite6_product STATIC
  src/lifecycle_profile.cpp
)
add_library(manyforge::lite6_product ALIAS manyforge_lite6_product)
target_include_directories(manyforge_lite6_product PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(manyforge_lite6_product PUBLIC manyforge_core)

