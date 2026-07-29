cmake_minimum_required(VERSION 3.14)
project(manyforge_deployment VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(OpenSSL REQUIRED)
find_package(double-conversion CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

set(
  MANYFORGE_CORE_INCLUDE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core/include"
  CACHE PATH
  "Path to the ROS-free manyforge_core public headers")

add_library(manyforge_deployment
  src/artifact_codec.cpp
  src/canonical_value.cpp
  src/cell_graph_codec.cpp
  src/content_identity.cpp
)
add_library(manyforge::deployment ALIAS manyforge_deployment)

target_include_directories(manyforge_deployment PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MANYFORGE_CORE_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(manyforge_deployment PUBLIC
  OpenSSL::Crypto
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

  add_executable(test_cell_graph_codec test/test_cell_graph_codec.cpp)
  target_link_libraries(test_cell_graph_codec PRIVATE
    manyforge_deployment GTest::gtest_main)
  gtest_discover_tests(test_cell_graph_codec
    PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
endif()
