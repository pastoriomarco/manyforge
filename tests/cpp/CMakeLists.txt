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

# Mesh depth (OD-363). Its benchmark is a disabled test:
#   test_mesh_depth --gtest_also_run_disabled_tests --gtest_filter='*Benchmark*'
add_executable(test_mesh_depth test_mesh_depth.cpp)
target_link_libraries(test_mesh_depth PRIVATE
  manyforge_collision GTest::gtest_main)
if(TARGET manyforge_mesh_file_topology)
  target_link_libraries(test_mesh_depth PRIVATE manyforge_mesh_file_topology)
  target_compile_definitions(test_mesh_depth PRIVATE MANYFORGE_MESH_FILE_TOPOLOGY=1)
endif()

if(NOT DEFINED MANYFORGE_GTEST_TIMEOUT_SECONDS)
  set(MANYFORGE_GTEST_TIMEOUT_SECONDS 60)
endif()
gtest_discover_tests(test_collision_engine_integration
  PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
gtest_discover_tests(test_mesh_depth
  PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
