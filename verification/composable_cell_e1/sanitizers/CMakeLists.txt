cmake_minimum_required(VERSION 3.22)
project(manyforge_e1_roboplan_sanitizers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(ROBOPLAN_SOURCE_ROOT "" CACHE PATH "Exact patched RoboPlan source root")
set(MANYFORGE_E1_SANITIZER "asan_ubsan" CACHE STRING
    "Focused sanitizer: asan_ubsan or tsan")
option(MANYFORGE_E1_BUILD_RRT "Build and run the focused RRT corpus" ON)
option(MANYFORGE_E1_BUILD_SIMPLE_IK
       "Build and run the focused Simple IK corpus" ON)

if(NOT IS_DIRECTORY "${ROBOPLAN_SOURCE_ROOT}/roboplan")
  message(FATAL_ERROR "ROBOPLAN_SOURCE_ROOT must name the exact source tree")
endif()

find_package(Eigen3 REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(console_bridge REQUIRED)
find_package(pinocchio REQUIRED)
find_package(roboplan_example_models REQUIRED)
find_package(yaml-cpp REQUIRED)

if(MANYFORGE_E1_SANITIZER STREQUAL "asan_ubsan")
  set(_E1_COMPILE_OPTIONS
      -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
  set(_E1_LINK_OPTIONS -fsanitize=address,undefined)
  # Pinocchio/Coal are supplied as uninstrumented binaries. LeakSanitizer
  # enters a recursive fatal-signal loop while tearing down the collision
  # corpus, so this lane claims ASan/UBSan only. Process/RSS leak and recovery
  # bounds are covered independently by the planner-worker containment probe.
  set(_E1_TEST_ENVIRONMENT
      "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1"
      "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
elseif(MANYFORGE_E1_SANITIZER STREQUAL "tsan")
  set(_E1_COMPILE_OPTIONS
      -O1 -g1 -fsanitize=thread -fno-omit-frame-pointer)
  set(_E1_LINK_OPTIONS -fsanitize=thread)
  set(_E1_TEST_ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
else()
  message(FATAL_ERROR "MANYFORGE_E1_SANITIZER must be asan_ubsan or tsan")
endif()

function(e1_instrument target)
  # Pinocchio is supplied as an uninstrumented binary and was compiled with
  # Eigen's glibc-aligned allocator path. ASan changes Eigen's automatic
  # allocator selection unless this ABI setting is carried across the shared
  # library boundary, which makes an instrumented destructor misread memory
  # allocated by Pinocchio.
  target_compile_definitions(${target} PRIVATE
    EIGEN_MALLOC_ALREADY_ALIGNED=1)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic ${_E1_COMPILE_OPTIONS})
  target_link_options(${target} PRIVATE ${_E1_LINK_OPTIONS})
endfunction()

set(_ROBOPLAN_CORE "${ROBOPLAN_SOURCE_ROOT}/roboplan")
add_library(e1_roboplan_core SHARED
  "${_ROBOPLAN_CORE}/include/tinyxml2/tinyxml2.cpp"
  "${_ROBOPLAN_CORE}/src/core/collision_context.cpp"
  "${_ROBOPLAN_CORE}/src/core/path_utils.cpp"
  "${_ROBOPLAN_CORE}/src/core/pose_utils.cpp"
  "${_ROBOPLAN_CORE}/src/core/scene.cpp"
  "${_ROBOPLAN_CORE}/src/core/scene_utils.cpp"
  "${_ROBOPLAN_CORE}/src/core/types.cpp")
target_include_directories(e1_roboplan_core PUBLIC
  "${_ROBOPLAN_CORE}/include")
target_link_libraries(e1_roboplan_core PUBLIC
  Eigen3::Eigen
  console_bridge::console_bridge
  pinocchio::pinocchio)
if(yaml-cpp_VERSION VERSION_LESS "0.8")
  target_link_libraries(e1_roboplan_core PRIVATE yaml-cpp)
else()
  target_link_libraries(e1_roboplan_core PRIVATE yaml-cpp::yaml-cpp)
endif()
e1_instrument(e1_roboplan_core)

add_executable(e1_test_native_lifetime
  "${CMAKE_CURRENT_SOURCE_DIR}/../native_lifetime/test_e1_roboplan_native_lifetime.cpp")
target_compile_definitions(e1_test_native_lifetime PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(e1_test_native_lifetime PRIVATE
  e1_roboplan_core
  GTest::gtest_main
  Threads::Threads
  roboplan_example_models::roboplan_example_models)
e1_instrument(e1_test_native_lifetime)

if(MANYFORGE_E1_BUILD_RRT)
  set(_ROBOPLAN_RRT "${ROBOPLAN_SOURCE_ROOT}/roboplan_rrt")
  add_library(e1_roboplan_rrt SHARED
    "${_ROBOPLAN_RRT}/src/constraints.cpp"
    "${_ROBOPLAN_RRT}/src/rrt.cpp")
  target_include_directories(e1_roboplan_rrt PUBLIC
    "${_ROBOPLAN_RRT}/include")
  target_link_libraries(e1_roboplan_rrt PUBLIC e1_roboplan_core)
  e1_instrument(e1_roboplan_rrt)

  add_executable(e1_test_rrt
    "${_ROBOPLAN_RRT}/test/test_rrt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_e1_dynotree_state_contract.cpp")
  target_link_libraries(e1_test_rrt PRIVATE
    e1_roboplan_rrt
    GTest::gtest_main
    roboplan_example_models::roboplan_example_models)
  e1_instrument(e1_test_rrt)

endif()

if(MANYFORGE_E1_BUILD_SIMPLE_IK)
  set(_ROBOPLAN_SIMPLE_IK "${ROBOPLAN_SOURCE_ROOT}/roboplan_simple_ik")
  add_library(e1_roboplan_simple_ik SHARED
    "${_ROBOPLAN_SIMPLE_IK}/src/simple_ik.cpp")
  target_include_directories(e1_roboplan_simple_ik PUBLIC
    "${_ROBOPLAN_SIMPLE_IK}/include")
  target_link_libraries(e1_roboplan_simple_ik PUBLIC e1_roboplan_core)
  e1_instrument(e1_roboplan_simple_ik)

  add_executable(e1_test_simple_ik
    "${_ROBOPLAN_SIMPLE_IK}/test/test_simple_ik.cpp")
  target_link_libraries(e1_test_simple_ik PRIVATE
    e1_roboplan_simple_ik
    GTest::gtest_main
    roboplan_example_models::roboplan_example_models)
  e1_instrument(e1_test_simple_ik)
endif()

enable_testing()
add_test(NAME e1_sanitized_native_lifetime COMMAND e1_test_native_lifetime)
set_tests_properties(e1_sanitized_native_lifetime PROPERTIES TIMEOUT 120)
if(MANYFORGE_E1_BUILD_RRT)
  add_test(NAME e1_sanitized_rrt COMMAND e1_test_rrt)
  set_tests_properties(e1_sanitized_rrt PROPERTIES TIMEOUT 180)
endif()
if(MANYFORGE_E1_BUILD_SIMPLE_IK)
  add_test(NAME e1_sanitized_simple_ik COMMAND e1_test_simple_ik)
  set_tests_properties(e1_sanitized_simple_ik PROPERTIES TIMEOUT 120)
endif()

set_tests_properties(e1_sanitized_native_lifetime PROPERTIES
  ENVIRONMENT "${_E1_TEST_ENVIRONMENT}")
if(MANYFORGE_E1_BUILD_RRT)
  set_tests_properties(e1_sanitized_rrt PROPERTIES
    ENVIRONMENT "${_E1_TEST_ENVIRONMENT}")
endif()
if(MANYFORGE_E1_BUILD_SIMPLE_IK)
  set_tests_properties(e1_sanitized_simple_ik PROPERTIES
    ENVIRONMENT "${_E1_TEST_ENVIRONMENT}")
endif()
