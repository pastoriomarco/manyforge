cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dependencies
find_package(Eigen3 REQUIRED)
find_package(pinocchio REQUIRED)
find_package(roboplan REQUIRED)
find_package(roboplan_rrt REQUIRED)
find_package(roboplan_toppra REQUIRED)
find_package(roboplan_simple_ik REQUIRED)
find_package(roboplan_example_models REQUIRED)

# manyforge_core (sibling directory)
if(NOT TARGET manyforge_core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core
                   ${CMAKE_CURRENT_BINARY_DIR}/manyforge_core)
endif()

# Native backend library
add_library(manyforge_roboplan
  src/roboplan_planner_backend.cpp
  src/roboplan_planning_context.cpp
  src/roboplan_execution_backend.cpp
  src/native_kernel_factory.cpp
)

target_include_directories(manyforge_roboplan PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(manyforge_roboplan PUBLIC
  manyforge_core
  Eigen3::Eigen
  pinocchio::pinocchio
  roboplan::roboplan
  roboplan_rrt::roboplan_rrt
  roboplan_toppra::roboplan_toppra
  roboplan_simple_ik::roboplan_simple_ik
  roboplan_example_models::roboplan_example_models
)

# pybind11 module
find_package(pybind11 QUIET)
if(pybind11_FOUND)
  pybind11_add_module(manyforge_roboplan_py python/bindings.cpp)
  target_link_libraries(manyforge_roboplan_py PRIVATE manyforge_roboplan)
endif()

# Tests
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)

    add_executable(test_native_backends test/test_native_backends.cpp)
    target_link_libraries(test_native_backends PRIVATE
      manyforge_roboplan GTest::gtest_main)

    # Pinocchio/coal/roboplan .so files live in cmeel.prefix/lib
    set(_LIB_PATHS "$ENV{LD_LIBRARY_PATH}")
    foreach(_dir
        "/usr/local/lib/python3.11/dist-packages/cmeel.prefix/lib"
        "/usr/local/lib")
      if(IS_DIRECTORY "${_dir}")
        string(APPEND _LIB_PATHS ":${_dir}")
      endif()
    endforeach()

    gtest_discover_tests(test_native_backends
      PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)
  endif()
endif()
