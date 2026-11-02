cmake_minimum_required(VERSION 3.14)
project(manyforge_content_identity VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(OpenSSL REQUIRED)
find_package(double-conversion CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)

add_library(manyforge_content_identity
  src/canonical_value.cpp
  src/content_identity.cpp
)
add_library(manyforge::content_identity ALIAS manyforge_content_identity)
target_include_directories(manyforge_content_identity PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(manyforge_content_identity PUBLIC
  OpenSSL::Crypto
  PkgConfig::UTF8PROC
  double-conversion::double-conversion
)
set_target_properties(
  manyforge_content_identity PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MANYFORGE_CONTENT_IDENTITY_TESTING_DEFAULT ON)
else()
  set(MANYFORGE_CONTENT_IDENTITY_TESTING_DEFAULT OFF)
endif()
option(
  MANYFORGE_CONTENT_IDENTITY_BUILD_TESTING
  "Build standalone content-identity tests"
  ${MANYFORGE_CONTENT_IDENTITY_TESTING_DEFAULT})

if(MANYFORGE_CONTENT_IDENTITY_BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(test_manyforge_content_identity
    test/test_content_identity.cpp)
  target_link_libraries(test_manyforge_content_identity PRIVATE
    manyforge::content_identity
    GTest::gtest_main)
  gtest_discover_tests(test_manyforge_content_identity
    PROPERTIES TIMEOUT 60)
endif()
