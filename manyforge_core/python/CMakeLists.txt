cmake_minimum_required(VERSION 3.14)
project(manyforge_core_py VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

# Find pybind11
find_package(pybind11 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(EXPAT REQUIRED)
find_package(double-conversion CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

# manyforge_core directory (passed from command line)
set(MANYFORGE_CORE_DIR "${MANYFORGE_CORE_DIR}" CACHE PATH "Path to manyforge_core build")
set(MANYFORGE_CORE_INCLUDE_DIR "${MANYFORGE_CORE_INCLUDE_DIR}" CACHE PATH "Path to manyforge_core headers")
set(
  MANYFORGE_DEPLOYMENT_DIR
  "${MANYFORGE_DEPLOYMENT_DIR}"
  CACHE PATH
  "Path to manyforge_deployment build")
set(
  MANYFORGE_DEPLOYMENT_INCLUDE_DIR
  "${MANYFORGE_DEPLOYMENT_INCLUDE_DIR}"
  CACHE PATH
  "Path to manyforge_deployment headers")
set(
  MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../../manyforge_content_identity"
  CACHE PATH
  "Path to the shared ManyForge content-identity source tree")
get_filename_component(
  MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
  "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}"
  ABSOLUTE)
if(NOT EXISTS
   "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include/manyforge_content_identity/content_identity.hpp")
  message(FATAL_ERROR
    "manyforge_core_py requires the shared content-identity source tree: "
    "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}")
endif()

# Add manyforge_core headers (try explicit include dir, then fall back to build-dir/../include)
if(MANYFORGE_CORE_INCLUDE_DIR)
  include_directories(${MANYFORGE_CORE_INCLUDE_DIR})
else()
  include_directories(${MANYFORGE_CORE_DIR}/../include)
endif()

add_library(manyforge_deployment_archive STATIC IMPORTED)
set_target_properties(manyforge_deployment_archive PROPERTIES
  IMPORTED_LOCATION
    "${MANYFORGE_DEPLOYMENT_DIR}/libmanyforge_deployment.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${MANYFORGE_DEPLOYMENT_INCLUDE_DIR}"
)

add_library(manyforge_content_identity_archive STATIC IMPORTED)
set_target_properties(manyforge_content_identity_archive PROPERTIES
  IMPORTED_LOCATION
    "${MANYFORGE_CORE_DIR}/manyforge_content_identity/libmanyforge_content_identity.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}/include"
)

add_library(manyforge_deployment_verification_core_archive STATIC IMPORTED)
set_target_properties(
  manyforge_deployment_verification_core_archive PROPERTIES
  IMPORTED_LOCATION
    "${MANYFORGE_DEPLOYMENT_DIR}/libmanyforge_deployment_verification_core.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${MANYFORGE_DEPLOYMENT_INCLUDE_DIR}"
)
target_link_libraries(
  manyforge_deployment_verification_core_archive INTERFACE
  manyforge_content_identity_archive
)
target_link_libraries(manyforge_deployment_archive INTERFACE
  manyforge_deployment_verification_core_archive
  manyforge_content_identity_archive
  OpenSSL::Crypto
  EXPAT::EXPAT
  PkgConfig::LIBYAML
  PkgConfig::UTF8PROC
  double-conversion::double-conversion
)

# Python bindings module
pybind11_add_module(manyforge_core_py NO_EXTRAS
  bindings.cpp
  bindings_catalog.cpp
  bindings_cell_graph.cpp
  bindings_command_schema.cpp
  bindings_component_type.cpp
  bindings_coordinate_state.cpp
  bindings_deployment.cpp
  bindings_model_compiler.cpp
  bindings_strong_identity.cpp
)

# Link against the manyforge_core library
target_link_libraries(manyforge_core_py PRIVATE
  ${MANYFORGE_CORE_DIR}/libmanyforge_core.a
  manyforge_deployment_archive
  pybind11::module
  pybind11::headers
)

target_include_directories(manyforge_core_py PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Install the Python module
install(TARGETS manyforge_core_py
  LIBRARY DESTINATION .
  RUNTIME DESTINATION .
)
