cmake_minimum_required(VERSION 3.14)
project(manyforge_persistence VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(SQLite3 3.37 REQUIRED)

add_library(manyforge_persistence
  src/sqlite_read_only_preflight.cpp
)
add_library(manyforge::persistence ALIAS manyforge_persistence)
target_include_directories(manyforge_persistence PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(manyforge_persistence PUBLIC SQLite::SQLite3)
set_target_properties(
  manyforge_persistence PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MANYFORGE_PERSISTENCE_TESTING_DEFAULT ON)
else()
  set(MANYFORGE_PERSISTENCE_TESTING_DEFAULT OFF)
endif()
option(
  MANYFORGE_PERSISTENCE_BUILD_TESTING
  "Build standalone persistence-primitive tests"
  ${MANYFORGE_PERSISTENCE_TESTING_DEFAULT})

if(MANYFORGE_PERSISTENCE_BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(test_manyforge_persistence
    test/test_sqlite_read_only_preflight.cpp)
  target_link_libraries(test_manyforge_persistence PRIVATE
    manyforge::persistence
    GTest::gtest_main)
  gtest_discover_tests(test_manyforge_persistence
    PROPERTIES TIMEOUT 60)
endif()
