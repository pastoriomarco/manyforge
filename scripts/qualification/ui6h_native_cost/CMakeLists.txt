cmake_minimum_required(VERSION 3.18)
project(ui6h_native_cost LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MANYFORGE_COST_SOURCE_ROOT "" CACHE PATH "Exact current or archived source root")
option(MANYFORGE_COST_CURRENT "Use current component-model snapshot contract" ON)
find_package(pinocchio REQUIRED)
find_package(Threads REQUIRED)
add_library(ui6h_collision_engine STATIC
  ${MANYFORGE_COST_SOURCE_ROOT}/manyforge_collision/src/manyforge_collision_engine.cpp)
target_include_directories(ui6h_collision_engine PUBLIC
  ${MANYFORGE_COST_SOURCE_ROOT}/manyforge_collision/include
  ${MANYFORGE_COST_SOURCE_ROOT}/manyforge_core/include)
target_link_libraries(ui6h_collision_engine PUBLIC pinocchio::pinocchio Threads::Threads)
target_compile_definitions(ui6h_collision_engine PUBLIC COAL_DISABLE_HPP_FCL_WARNINGS)
add_executable(ui6h_collision_cost ../ui6h_collision_cost.cpp)
target_link_libraries(ui6h_collision_cost PRIVATE ui6h_collision_engine)
if(MANYFORGE_COST_CURRENT)
  target_compile_definitions(ui6h_collision_cost PRIVATE MANYFORGE_COST_CURRENT)
  target_include_directories(ui6h_collision_cost PRIVATE
    ${MANYFORGE_COST_SOURCE_ROOT}/manyforge_roboplan/include)
  target_sources(ui6h_collision_cost PRIVATE
    ${MANYFORGE_COST_SOURCE_ROOT}/manyforge_core/src/runtime_collision_monitor.cpp
    ${MANYFORGE_COST_SOURCE_ROOT}/manyforge_roboplan/src/native_runtime_support.cpp)
  find_package(GTest REQUIRED)
  add_executable(ui6h_collision_regression
    ${MANYFORGE_COST_SOURCE_ROOT}/tests/cpp/test_collision_engine_integration.cpp)
  target_link_libraries(ui6h_collision_regression PRIVATE ui6h_collision_engine GTest::gtest_main)
endif()
