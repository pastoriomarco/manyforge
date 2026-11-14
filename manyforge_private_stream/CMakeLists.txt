cmake_minimum_required(VERSION 3.14)
project(manyforge_private_stream VERSION 0.2.1 LANGUAGES CXX)

# Server side shared by ManyForge's private SEQPACKET streams: descriptor
# holder, send with deadline, error frame, peer check and subscriber pool.
# Transport, not core: no ROS and no manyforge_core.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(Threads REQUIRED)

set(
  MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_content_identity"
  CACHE PATH
  "Path to the shared ManyForge RFC 8785/content-identity library")
get_filename_component(
  MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR
  "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}"
  ABSOLUTE)
if(NOT TARGET manyforge_content_identity)
  add_subdirectory(
    "${MANYFORGE_CONTENT_IDENTITY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_content_identity"
    EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET manyforge::content_identity)
  message(FATAL_ERROR
    "manyforge_private_stream could not resolve the shared content-identity target")
endif()

add_library(manyforge_private_stream INTERFACE)
add_library(manyforge::private_stream ALIAS manyforge_private_stream)
target_include_directories(manyforge_private_stream INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(manyforge_private_stream INTERFACE cxx_std_20)
target_link_libraries(manyforge_private_stream INTERFACE
  manyforge::content_identity
  Threads::Threads)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MANYFORGE_PRIVATE_STREAM_TESTING_DEFAULT ON)
else()
  set(MANYFORGE_PRIVATE_STREAM_TESTING_DEFAULT OFF)
endif()
option(
  MANYFORGE_PRIVATE_STREAM_BUILD_TESTING
  "Build standalone private-stream tests"
  ${MANYFORGE_PRIVATE_STREAM_TESTING_DEFAULT})

if(MANYFORGE_PRIVATE_STREAM_BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(test_private_stream_server
    test/test_private_stream_server.cpp)
  target_link_libraries(test_private_stream_server PRIVATE
    manyforge::private_stream
    GTest::gtest_main)
  gtest_discover_tests(test_private_stream_server
    PROPERTIES TIMEOUT 60)
endif()
