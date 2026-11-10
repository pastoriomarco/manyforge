cmake_minimum_required(VERSION 3.14)

# manyforge_collision engine integration tests. Built by
# manyforge_collision/CMakeLists.txt when BUILD_TESTING is on.
if(NOT TARGET manyforge_collision)
  message(FATAL_ERROR "tests/cpp requires target manyforge_collision")
endif()

find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(test_collision_engine_integration
  test_collision_engine_integration.cpp)
target_include_directories(test_collision_engine_integration PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_collision_engine_integration PRIVATE
  manyforge_collision GTest::gtest_main)

if(NOT DEFINED MANYFORGE_GTEST_TIMEOUT_SECONDS)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS 60)
endif()
gtest_discover_tests(test_collision_engine_integration
  PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
