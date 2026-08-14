cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 3.37 REQUIRED)
find_package(Threads REQUIRED)
option(BUILD_TESTING "Build unit tests" ON)
option(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME
  "Build the retained ros2_control/Isaac execution product" OFF)

# I6 restores the selected native planning dependency as an ordinary product
# target. Discover cmeel's prefix before resolving Pinocchio/RoboPlan so the
# same pinned installation is used in test and non-test builds.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c
          "import site, pathlib; print(';'.join(str(pathlib.Path(p) / 'cmeel.prefix') for p in site.getsitepackages()))"
  OUTPUT_VARIABLE _CMEEL_PREFIXES
  OUTPUT_STRIP_TRAILING_WHITESPACE)
foreach(_prefix ${_CMEEL_PREFIXES})
  if(EXISTS "${_prefix}/include/boost/version.hpp")
    set(BOOST_ROOT "${_prefix}")
    set(BOOST_INCLUDEDIR "${_prefix}/include")
    set(BOOST_LIBRARYDIR "${_prefix}/lib")
    break()
  endif()
endforeach()
set(COAL_DISABLE_HPP_FCL_WARNINGS ON)
find_package(Eigen3 REQUIRED)
find_package(pinocchio REQUIRED)
find_package(roboplan REQUIRED)
find_package(roboplan_rrt REQUIRED)
find_package(roboplan_simple_ik REQUIRED)
find_package(roboplan_toppra REQUIRED)
find_package(roboplan_cartesian_planning REQUIRED)

get_filename_component(
  _MANYFORGE_REPOSITORY_SOURCE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  REALPATH)

# The wall clock and scheduler are factory-neutral implementations of Core's
# time ports.  Build Core without its package-local tests so this bounded
# support library remains compiled in ordinary builds without restoring a
# Kernel factory, event collector, planner, execution backend, or entrypoint.
if(NOT TARGET manyforge_core)
  set(_MANYFORGE_ROBOPLAN_BUILD_TESTING "${BUILD_TESTING}")
  cmake_policy(PUSH)
  cmake_policy(SET CMP0077 NEW)
  set(BUILD_TESTING OFF)
  set(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL ON)
  set(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL ON)
  set(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY ON)
  add_subdirectory(
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
    EXCLUDE_FROM_ALL)
  cmake_policy(POP)
  set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
  unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
  unset(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL)
  unset(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL)
  unset(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY)
endif()

if(NOT TARGET manyforge_lite6_product)
  add_subdirectory(
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_lite6_product"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_lite6_product"
    EXCLUDE_FROM_ALL)
endif()

# The I6 native projection boundary consumes the ManyForge-owned collision
# model directly. Build it without its package-local integration suite here;
# cumulative collision qualification remains owned by the dedicated collision
# image/lane.
if(NOT TARGET manyforge_collision)
  set(_MANYFORGE_ROBOPLAN_BUILD_TESTING "${BUILD_TESTING}")
  cmake_policy(PUSH)
  cmake_policy(SET CMP0077 NEW)
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_collision"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_collision")
  cmake_policy(POP)
  set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
  unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
endif()

# The selected N3 host is a read-only consumer of I3 retained products and
# I4's durable active-bundle publication. Embed those production libraries
# without duplicating either package's independent test tree; the host does
# not acquire compiler or Component Manager mutation authority.
if(NOT TARGET manyforge_component_manager)
  set(_MANYFORGE_ROBOPLAN_BUILD_TESTING "${BUILD_TESTING}")
  set(MANYFORGE_BUILD_CCLC_RUNTIME_COMPOSITION ON)
  cmake_policy(PUSH)
  cmake_policy(SET CMP0077 NEW)
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_component_manager"
    EXCLUDE_FROM_ALL)
  cmake_policy(POP)
  set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
  unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
  unset(MANYFORGE_BUILD_CCLC_RUNTIME_COMPOSITION)
endif()
if(NOT TARGET manyforge_component_manager OR
   NOT TARGET manyforge_deployment)
  message(FATAL_ERROR
    "I6 native execution requires retained-product verification and the "
    "active-bundle publication reader")
endif()

# I3 keeps only the bounded, file-only bridge required to qualify exact
# retained planning products against RoboPlan/Pinocchio. This target owns no
# runtime scene truth, Kernel, execution backend, ROS node, or Python module.
add_library(manyforge_roboplan_retained_consumer
  src/retained_planning_model_stage.cpp)

target_include_directories(manyforge_roboplan_retained_consumer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(manyforge_roboplan_retained_consumer
  PUBLIC OpenSSL::Crypto)

add_library(manyforge_roboplan_collision_projection
  src/retained_collision_world_projector.cpp
  src/retained_collision_world_publisher.cpp
  src/retained_planner_scene_projection.cpp
  src/retained_scene_frame_resolver.cpp)
target_include_directories(manyforge_roboplan_collision_projection PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_collision_projection PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_collision_projection PUBLIC
  manyforge_collision
  manyforge_core
  PRIVATE manyforge::content_identity)

add_library(manyforge_roboplan_native_state_projection
  src/retained_kinematic_state_projector.cpp
  src/retained_execution_state_sink.cpp
  src/retained_observed_state_channel.cpp)
target_include_directories(manyforge_roboplan_native_state_projection PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_native_state_projection PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_native_state_projection PUBLIC
  manyforge_core
  Eigen3::Eigen
  roboplan::roboplan)

add_library(manyforge_roboplan_native_runtime_support
  src/native_runtime_support.cpp
  src/retained_externally_managed_lifecycle_ports.cpp
  src/retained_native_controller_lifecycle_port.cpp
  src/retained_native_brand_lifecycle_port.cpp)
target_include_directories(manyforge_roboplan_native_runtime_support PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_runtime_support
  PUBLIC manyforge_core
  PRIVATE Threads::Threads)

add_library(manyforge_roboplan_native_lifecycle_product
  src/retained_native_lifecycle_product.cpp)
target_include_directories(manyforge_roboplan_native_lifecycle_product PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_lifecycle_product PUBLIC
  manyforge_roboplan_native_runtime_support
  manyforge_lite6_product
  manyforge_deployment)

add_library(manyforge_roboplan_native_gripper
  src/retained_gripper_command_backend.cpp)
target_include_directories(manyforge_roboplan_native_gripper PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_gripper PUBLIC
  manyforge_roboplan_native_state_projection
  manyforge_roboplan_native_runtime_support)

add_library(manyforge_roboplan_native_focused_adapters
  src/retained_focused_adapters.cpp)
target_include_directories(
  manyforge_roboplan_native_focused_adapters PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(
  manyforge_roboplan_native_focused_adapters PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(
  manyforge_roboplan_native_focused_adapters PUBLIC
  manyforge_roboplan_native_state_projection
  pinocchio::pinocchio
  roboplan::roboplan)

add_library(manyforge_roboplan_native_execution
  src/roboplan_execution_backend.cpp)
target_include_directories(manyforge_roboplan_native_execution PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_execution PUBLIC
  manyforge_roboplan_native_state_projection
  manyforge_roboplan_native_runtime_support)

# The safety/fidelity algorithms were retained and qualified before the native
# owner returned. I6 promotes the same implementations into the production
# planning boundary; no duplicate test-only copy remains.
add_library(manyforge_roboplan_planning_contracts STATIC
  src/cartesian_planning_contract.cpp
  src/roboplan_cartesian_trajectory_dynamics.cpp
  src/joint_cap_contract.cpp)
target_include_directories(manyforge_roboplan_planning_contracts PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_planning_contracts PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_planning_contracts PUBLIC
  manyforge_core
  Eigen3::Eigen
  pinocchio::pinocchio
  roboplan::roboplan)

add_library(manyforge_roboplan_native_candidate
  src/roboplan_candidate_computer.cpp)
target_include_directories(manyforge_roboplan_native_candidate PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_native_candidate PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_native_candidate
  PUBLIC
    manyforge_core
    roboplan::roboplan
  PRIVATE
    manyforge_roboplan_planning_contracts
    roboplan_rrt::roboplan_rrt
    roboplan_simple_ik::roboplan_simple_ik
    roboplan_toppra::roboplan_toppra
    roboplan_cartesian_planning::roboplan_cartesian_planning)

add_library(manyforge_roboplan_planner_backend
  src/roboplan_planner_backend.cpp)
target_include_directories(manyforge_roboplan_planner_backend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_planner_backend PUBLIC
  manyforge_roboplan_native_candidate)

add_library(manyforge_roboplan_native_runtime
  src/retained_native_runtime.cpp
  src/retained_stop_surveillance_owner.cpp)
target_include_directories(manyforge_roboplan_native_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_native_runtime PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_native_runtime PUBLIC
  manyforge_roboplan_native_execution
  manyforge_roboplan_native_focused_adapters
  manyforge_roboplan_native_gripper
  manyforge_roboplan_planner_backend
  manyforge_roboplan_collision_projection
  manyforge_collision
  manyforge_core
  manyforge_core_sqlite_operation_journal
  manyforge::deployment_resolved_stop_base_plan_resolver
  PRIVATE manyforge::content_identity)

if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
  if(NOT TARGET manyforge_ros2_control_execution)
    set(_MANYFORGE_ROBOPLAN_BUILD_TESTING "${BUILD_TESTING}")
    cmake_policy(PUSH)
    cmake_policy(SET CMP0077 NEW)
    set(BUILD_TESTING OFF)
    add_subdirectory(
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_ros2_control_execution"
      "${CMAKE_CURRENT_BINARY_DIR}/manyforge_ros2_control_execution"
      EXCLUDE_FROM_ALL)
    cmake_policy(POP)
    set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
    unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
  endif()
  add_library(manyforge_roboplan_ros2_control_runtime
    src/retained_isaac_ros2_control_profile.cpp
    src/retained_ros2_control_admission_owner.cpp
    src/retained_ros2_control_backend_factory.cpp
    src/retained_ros2_control_lifecycle_product.cpp
    src/retained_ros2_control_runtime_handle.cpp)
  target_include_directories(manyforge_roboplan_ros2_control_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_roboplan_ros2_control_runtime PUBLIC
    manyforge_roboplan_native_runtime
    manyforge_ros2_control_execution
    manyforge::content_identity)
endif()

if(NOT TARGET manyforge_deployment_cclc_runtime_composition)
  message(FATAL_ERROR
    "retained native CCLC composition requires the Deployment runtime owner")
endif()
add_library(manyforge_roboplan_native_cclc_runtime
  src/retained_native_cclc_runtime.cpp)
target_include_directories(manyforge_roboplan_native_cclc_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_cclc_runtime PUBLIC
  manyforge_roboplan_native_runtime
  manyforge_roboplan_native_runtime_support
  manyforge_roboplan_native_lifecycle_product
  manyforge::deployment_cclc_runtime_composition)

add_library(manyforge_roboplan_worker_protocol STATIC
  src/roboplan_worker_protocol.cpp)
target_include_directories(manyforge_roboplan_worker_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(manyforge_roboplan_worker_protocol PUBLIC
  manyforge_roboplan_planner_backend)

add_library(manyforge_roboplan_process_candidate_client
  src/process_contained_candidate_client.cpp)
target_include_directories(manyforge_roboplan_process_candidate_client
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(manyforge_roboplan_process_candidate_client
  PUBLIC
    manyforge_roboplan_planner_backend
  PRIVATE
    manyforge_roboplan_worker_protocol
    Threads::Threads)

add_executable(manyforge_roboplan_candidate_worker
  src/roboplan_candidate_worker_main.cpp)
target_include_directories(manyforge_roboplan_candidate_worker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(manyforge_roboplan_candidate_worker PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_candidate_worker PRIVATE
  manyforge_roboplan_worker_protocol
  manyforge_roboplan_native_candidate)

add_library(manyforge_roboplan_managed_kernel_apply_fence
  src/managed_kernel_apply_fence_owner.cpp)
target_include_directories(manyforge_roboplan_managed_kernel_apply_fence
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
target_link_libraries(manyforge_roboplan_managed_kernel_apply_fence
  PUBLIC manyforge_core
  PRIVATE
    manyforge_component_manager
    manyforge::content_identity
    manyforge_deployment
    manyforge::persistence
    SQLite::SQLite3)

# Selected composition root for the first complete addressed native path.
# Its same-user Unix socket is a bounded private transport consumed by the
# headless API; it is not a second public execution contract.
add_executable(manyforge_native_execution_host
  src/product_backed_runtime_participant_resolver.cpp
  src/native_execution_host_main.cpp)
target_include_directories(manyforge_native_execution_host PRIVATE
  "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
target_compile_definitions(manyforge_native_execution_host PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_native_execution_host PRIVATE
  manyforge_roboplan_managed_kernel_apply_fence
  manyforge_roboplan_native_cclc_runtime
  manyforge_roboplan_native_runtime
  manyforge_roboplan_process_candidate_client
  manyforge_roboplan_retained_consumer
  manyforge_component_manager
  manyforge::content_identity
  manyforge::scene_value_codec
  manyforge_deployment
  manyforge::persistence
  SQLite::SQLite3
  Threads::Threads)
install(TARGETS
  manyforge_native_execution_host
  manyforge_roboplan_candidate_worker
  RUNTIME DESTINATION bin)

if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
  # I9 keeps the native and ros2_control implementations as separate runtime
  # products.  They share the retained host protocol and CCLC composition but
  # never select an execution backend through an in-process runtime switch.
  add_executable(manyforge_ros2_control_execution_host
    src/product_backed_runtime_participant_resolver.cpp
    src/native_execution_host_main.cpp)
  target_include_directories(manyforge_ros2_control_execution_host PRIVATE
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
  target_compile_definitions(manyforge_ros2_control_execution_host PRIVATE
    COAL_DISABLE_HPP_FCL_WARNINGS
    MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=1)
  target_link_libraries(manyforge_ros2_control_execution_host PRIVATE
    manyforge_roboplan_managed_kernel_apply_fence
    manyforge_roboplan_native_cclc_runtime
    manyforge_roboplan_ros2_control_runtime
    manyforge_roboplan_process_candidate_client
    manyforge_roboplan_retained_consumer
    manyforge_component_manager
    manyforge::content_identity
    manyforge::scene_value_codec
    manyforge_deployment
    manyforge::persistence
    SQLite::SQLite3
    Threads::Threads)
  install(TARGETS manyforge_ros2_control_execution_host
    RUNTIME DESTINATION bin)
  message(STATUS
    "manyforge_roboplan retained runtime: native execution remains available "
    "and the I9 ros2_control adapter is a separate installed runtime product")
else()
  message(STATUS
    "manyforge_roboplan retained runtime: native execution is built; the I9 "
    "ros2_control adapter seam is not selected in this build")
endif()

# Tests
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    find_package(roboplan_example_models REQUIRED)

    # The explicit Stop qualification target consumes exact target-scoped
    # source/build closures. Request both File API objects during configure;
    # neither query runs the qualifier nor changes ordinary product targets.
    set(_MANYFORGE_CMAKE_FILE_API_QUERY_ROOT
      "${CMAKE_BINARY_DIR}/.cmake/api/v1/query")
    file(MAKE_DIRECTORY "${_MANYFORGE_CMAKE_FILE_API_QUERY_ROOT}")
    file(WRITE
      "${_MANYFORGE_CMAKE_FILE_API_QUERY_ROOT}/codemodel-v2" "")
    file(WRITE
      "${_MANYFORGE_CMAKE_FILE_API_QUERY_ROOT}/toolchains-v1" "")

    # Test-only variant of the exact native transport host. It joins the real
    # addressed C++ pick/place planning owner to deterministic subordinate
    # ports so the Python/headless/Behavior/Composer process path can be
    # qualified without introducing a second production scene or runtime
    # owner. The installed host above never compiles this fixture.
    add_executable(manyforge_native_execution_host_pick_place_fixture
      src/product_backed_runtime_participant_resolver.cpp
      src/native_execution_host_main.cpp
      test/focused_pick_place_planning_fixture.cpp)
    target_include_directories(
      manyforge_native_execution_host_pick_place_fixture PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_compile_definitions(
      manyforge_native_execution_host_pick_place_fixture PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS
      MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE=1)
    target_link_libraries(
      manyforge_native_execution_host_pick_place_fixture PRIVATE
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_roboplan_native_cclc_runtime
      manyforge_roboplan_native_runtime
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_retained_consumer
      manyforge_component_manager
      manyforge::content_identity
      manyforge::scene_value_codec
      manyforge_deployment
      manyforge::persistence
      SQLite::SQLite3
      Threads::Threads)

    if(NOT TARGET manyforge_collision)
      message(FATAL_ERROR
        "I6 retained collision projection requires the authoritative "
        "collision consumer target")
    endif()

    set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
        "Per-test timeout for manyforge_roboplan gtest cases")

    add_executable(test_native_runtime_support
      test/test_native_runtime_support.cpp)
    target_link_libraries(test_native_runtime_support PRIVATE
      manyforge_roboplan_native_runtime_support GTest::gtest_main)

    # Bounded qualification-only evidence capture. It is deliberately not a
    # production target and supplies no planner, factory, scene, execution, or
    # gripper implementation.
    add_library(manyforge_roboplan_planning_evidence STATIC
      src/roboplan_planning_qualification.cpp)
    target_include_directories(manyforge_roboplan_planning_evidence PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(manyforge_roboplan_planning_evidence
      PUBLIC manyforge_core
      PRIVATE Threads::Threads)

    add_library(manyforge_roboplan_planner_options_contract INTERFACE)
    target_include_directories(
      manyforge_roboplan_planner_options_contract INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
    target_link_libraries(
      manyforge_roboplan_planner_options_contract INTERFACE
      manyforge_core
      roboplan::roboplan
      roboplan_rrt::roboplan_rrt
      roboplan_simple_ik::roboplan_simple_ik)

    add_executable(test_roboplan_planning_qualification
      test/test_roboplan_planning_qualification.cpp)
    target_include_directories(test_roboplan_planning_qualification PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
    target_link_libraries(test_roboplan_planning_qualification PRIVATE
      manyforge_roboplan_planning_evidence GTest::gtest_main)

    add_executable(test_roboplan_planner_options
      test/test_roboplan_planner_options.cpp)
    target_link_libraries(test_roboplan_planner_options PRIVATE
      manyforge_roboplan_planner_options_contract GTest::gtest_main)

    add_executable(test_cartesian_planning_contract
      test/test_cartesian_planning_contract.cpp)
    target_compile_definitions(test_cartesian_planning_contract PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS)
    target_link_libraries(test_cartesian_planning_contract PRIVATE
      manyforge_roboplan_planning_contracts GTest::gtest_main)

    add_executable(test_joint_cap_contract
      test/test_joint_cap_contract.cpp)
    target_link_libraries(test_joint_cap_contract PRIVATE
      manyforge_roboplan_planning_contracts GTest::gtest_main)

    add_executable(test_roboplan_candidate_computer
      test/test_roboplan_candidate_computer.cpp)
    target_compile_definitions(test_roboplan_candidate_computer PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS)
    target_link_libraries(test_roboplan_candidate_computer PRIVATE
      manyforge_roboplan_native_candidate
      roboplan_example_models::roboplan_example_models
      GTest::gtest_main)

    add_executable(test_roboplan_planner_backend
      test/test_roboplan_planner_backend.cpp)
    target_include_directories(test_roboplan_planner_backend PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
    target_link_libraries(test_roboplan_planner_backend PRIVATE
      manyforge_roboplan_planner_backend GTest::gtest_main)

    add_executable(test_retained_native_backends
      test/test_retained_native_backends.cpp)
    target_link_libraries(test_retained_native_backends PRIVATE
      manyforge_roboplan_native_execution
      manyforge_roboplan_native_gripper
      GTest::gtest_main)

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      add_executable(test_retained_ros2_control_backend_factory
        test/test_retained_ros2_control_backend_factory.cpp)
      target_link_libraries(test_retained_ros2_control_backend_factory PRIVATE
        manyforge_roboplan_ros2_control_runtime
        GTest::gtest_main)
      gtest_discover_tests(test_retained_ros2_control_backend_factory)
    endif()

    add_executable(test_retained_stop_surveillance_owner
      test/test_retained_stop_surveillance_owner.cpp)
    target_include_directories(test_retained_stop_surveillance_owner PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_link_libraries(test_retained_stop_surveillance_owner PRIVATE
      manyforge_roboplan_native_runtime GTest::gtest_main)

    add_executable(test_retained_native_controller_lifecycle_port
      test/test_retained_native_controller_lifecycle_port.cpp)
    target_link_libraries(test_retained_native_controller_lifecycle_port PRIVATE
      manyforge_roboplan_native_runtime_support GTest::gtest_main)

    add_executable(test_retained_native_cclc_runtime
      test/test_retained_native_cclc_runtime.cpp)
    target_link_libraries(test_retained_native_cclc_runtime PRIVATE
      manyforge_roboplan_native_cclc_runtime
      manyforge_component_manager
      manyforge_deployment
      GTest::gtest_main)

    add_executable(test_native_stop_qualification_seams
      test/test_native_stop_qualification_seams.cpp)
    target_include_directories(test_native_stop_qualification_seams PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_link_libraries(test_native_stop_qualification_seams PRIVATE
      manyforge_roboplan_native_execution
      GTest::gtest_main)

    # The runner links the exact production native backend and production
    # StopControl scheduler. Its private probe can only observe/block nominal
    # stages; the software-hard issue path itself has no qualification branch.
    # EXCLUDE_FROM_ALL keeps evidence generation explicit and outside ordinary
    # build, install, retained compilation, and materialization paths.
    add_executable(manyforge_native_stop_qualification_runner
      EXCLUDE_FROM_ALL
      test/run_native_stop_qualification.cpp)
    target_include_directories(
      manyforge_native_stop_qualification_runner PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/test"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src")
    target_compile_definitions(
      manyforge_native_stop_qualification_runner PRIVATE
      MANYFORGE_NATIVE_STOP_QUALIFICATION_REPETITIONS=16
      MANYFORGE_NATIVE_STOP_ISSUE_BOUND_NS=50000000
      MANYFORGE_NATIVE_STOP_NOMINAL_STAGE_BUDGET_NS=500000000)
    target_link_libraries(
      manyforge_native_stop_qualification_runner PRIVATE
      manyforge_roboplan_native_execution
      manyforge_roboplan_native_runtime_support
      manyforge_deployment
      manyforge_core
      Threads::Threads)

    set(_MANYFORGE_NATIVE_STOP_QUALIFICATION_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/qualification/native_stop")
    set(_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE
      "${_MANYFORGE_NATIVE_STOP_QUALIFICATION_ROOT}/implementation-build.json")
    set(_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT
      "${_MANYFORGE_NATIVE_STOP_QUALIFICATION_ROOT}/runner-contract.json")
    set(_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE
      "${_MANYFORGE_NATIVE_STOP_QUALIFICATION_ROOT}/implementation-evidence.json")
    set(_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE
      "${_MANYFORGE_NATIVE_STOP_QUALIFICATION_ROOT}/substitution-evidence.json")
    set(_MANYFORGE_NATIVE_STOP_CLOSURE_GENERATOR
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_native_stop_build_closure.py")
    set(_MANYFORGE_NATIVE_STOP_CASE_CONTRACT
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/contracts/native_stop_qualification_case_contract.v1.json")
    set(_MANYFORGE_NATIVE_STOP_SEMANTIC_INPUTS
      "${CMAKE_CURRENT_SOURCE_DIR}/include/manyforge_roboplan/roboplan_execution_backend.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/roboplan_execution_backend.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/native_stop_qualification_probe.hpp"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/include/manyforge_core/types/stop_control.hpp"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/include/manyforge_core/execution_stop_support.hpp"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/src/execution_stop_support.cpp"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/include/manyforge_deployment/native_execution_runtime_contract.hpp"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/closed_schema/value/manyforge.value_schema.provider_contract.empty.v1.yaml"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/closed_schema/value/manyforge.value_schema.provider_contract.motion_controlled_stop.v1.yaml")
    set(_MANYFORGE_NATIVE_STOP_SEMANTIC_ARGUMENTS)
    foreach(_semantic_input IN LISTS _MANYFORGE_NATIVE_STOP_SEMANTIC_INPUTS)
      list(APPEND _MANYFORGE_NATIVE_STOP_SEMANTIC_ARGUMENTS
        --semantic-input "${_semantic_input}")
    endforeach()
    add_custom_command(
      OUTPUT
        "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_NATIVE_STOP_QUALIFICATION_ROOT}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${_MANYFORGE_NATIVE_STOP_CLOSURE_GENERATOR}"
        --source-root "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
        --build-root "${CMAKE_BINARY_DIR}"
        --implementation-target manyforge_native_execution_host
        --implementation-id
          manyforge.native_execution_host
        --installed-target-id manyforge_native_execution_host
        --installed-target "$<TARGET_FILE:manyforge_native_execution_host>"
        --runner-target manyforge_native_stop_qualification_runner
        --runner-executable
          "$<TARGET_FILE:manyforge_native_stop_qualification_runner>"
        --case-contract "${_MANYFORGE_NATIVE_STOP_CASE_CONTRACT}"
        --runner-source-input
          "${CMAKE_CURRENT_SOURCE_DIR}/test/native_stop_qualification_fixture.hpp"
        --runner-source-input
          "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src/stop_qualification_evidence_internal.hpp"
        ${_MANYFORGE_NATIVE_STOP_SEMANTIC_ARGUMENTS}
        --implementation-output "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        --runner-output "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
      DEPENDS
        manyforge_native_execution_host
        manyforge_native_stop_qualification_runner
        "${_MANYFORGE_NATIVE_STOP_CLOSURE_GENERATOR}"
        "${_MANYFORGE_NATIVE_STOP_CASE_CONTRACT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/native_stop_qualification_fixture.hpp"
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src/stop_qualification_evidence_internal.hpp"
        ${_MANYFORGE_NATIVE_STOP_SEMANTIC_INPUTS}
      COMMAND_EXPAND_LISTS
      VERBATIM)
    add_custom_target(manyforge_native_stop_qualification_closure
      DEPENDS
        "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}")
    add_custom_command(
      OUTPUT
        "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}"
      COMMAND
        "$<TARGET_FILE:manyforge_native_stop_qualification_runner>"
        --implementation-build "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        --runner-contract "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
        --implementation-output
          "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        --substitution-output
          "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}"
      DEPENDS
        manyforge_native_stop_qualification_runner
        "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
      VERBATIM)
    add_custom_target(manyforge_run_native_stop_qualification
      DEPENDS
        "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}")

    add_test(
      NAME native_stop_build_closure_generator
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_generate_native_stop_build_closure.py")
    set_tests_properties(native_stop_build_closure_generator PROPERTIES
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
      TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

    add_executable(test_managed_kernel_admission_gate
      test/test_managed_kernel_admission_gate.cpp)
    target_include_directories(test_managed_kernel_admission_gate PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_managed_kernel_admission_gate PRIVATE
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge::core_sqlite_operation_journal
      manyforge_component_manager
      manyforge_deployment
      GTest::gtest_main)

    add_executable(fake_process_candidate_worker
      test/fake_process_candidate_worker.cpp)
    target_include_directories(fake_process_candidate_worker PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fake_process_candidate_worker PRIVATE
      manyforge_roboplan_worker_protocol)

    add_executable(test_process_contained_candidate_client
      test/test_process_contained_candidate_client.cpp)
    add_dependencies(test_process_contained_candidate_client
      manyforge_roboplan_candidate_worker
      fake_process_candidate_worker)
    target_compile_definitions(test_process_contained_candidate_client PRIVATE
      MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH="$<TARGET_FILE:manyforge_roboplan_candidate_worker>"
      MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH="$<TARGET_FILE:fake_process_candidate_worker>")
    target_link_libraries(test_process_contained_candidate_client PRIVATE
      manyforge_roboplan_process_candidate_client
      roboplan_example_models::roboplan_example_models
      Threads::Threads
      GTest::gtest_main)

    add_executable(test_roboplan_worker_protocol
      test/test_roboplan_worker_protocol.cpp)
    target_include_directories(test_roboplan_worker_protocol PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_roboplan_worker_protocol PRIVATE
      manyforge_roboplan_worker_protocol GTest::gtest_main)

    add_executable(test_retained_planning_model_stage
      test/test_retained_planning_model_stage.cpp)
    target_link_libraries(test_retained_planning_model_stage PRIVATE
      manyforge_roboplan_retained_consumer GTest::gtest_main)

    add_executable(test_roboplan_scene_generation
      test/test_roboplan_scene_generation.cpp)
    target_compile_definitions(test_roboplan_scene_generation PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS)
    target_link_libraries(test_roboplan_scene_generation PRIVATE
      roboplan::roboplan
      roboplan_example_models::roboplan_example_models
      Threads::Threads
      GTest::gtest_main)

    # I3 consumer-equivalence integration: embed the production deployment
    # compiler without its package-local tests, generate the ordinary sealed
    # canonical-empty package and a prepared non-empty Lite6 source fixture,
    # then point-read exact retained planning inputs into real RoboPlan
    # Scenes. This is test-only composition; production RoboPlan code does not
    # acquire a deployment/compiler dependency.
    if(NOT TARGET manyforge_deployment)
      set(_MANYFORGE_ROBOPLAN_BUILD_TESTING "${BUILD_TESTING}")
      cmake_policy(PUSH)
      cmake_policy(SET CMP0077 NEW)
      set(BUILD_TESTING OFF)
      add_subdirectory(
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment"
        "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment")
      cmake_policy(POP)
      set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
      unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
    endif()
    if(NOT TARGET manyforge_deployment OR
       NOT TARGET manyforge_deployment_admission OR
       NOT TARGET manyforge_generate_canonical_empty_package)
      message(FATAL_ERROR
        "I3 retained-product consumer qualification requires the "
        "compiler, focused admission library, and canonical-empty package "
        "generator")
    endif()

    set(_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE
      "${CMAKE_CURRENT_BINARY_DIR}/canonical_empty_product_package")
    set(_MANYFORGE_CANONICAL_EMPTY_PRODUCT_ADMISSION
      "${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}/canonical-empty-admission.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_ADMISSION}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}"
      COMMAND
        "$<TARGET_FILE:manyforge_generate_canonical_empty_package>"
        --output-root
        "${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}"
      DEPENDS manyforge_generate_canonical_empty_package
      VERBATIM)
    add_custom_target(
      generate_canonical_empty_product_package
      DEPENDS "${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_ADMISSION}")

    if(NOT Python3_Interpreter_FOUND)
      message(FATAL_ERROR
        "I3 non-empty RoboPlan consumer qualification requires Python")
    endif()
    find_program(_MANYFORGE_URDFDOM_CHECK_URDF_EXECUTABLE
      NAMES check_urdf)
    if(NOT _MANYFORGE_URDFDOM_CHECK_URDF_EXECUTABLE)
      message(FATAL_ERROR
        "I3 independent retained-model qualification requires urdfdom check_urdf")
    endif()
    set(_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_example_models/models/lite6_lite_gripper")
    set(_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_structural_consumer_fixture")
    set(_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/tools/prepare_structural_model.py")
    get_filename_component(
      _MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL
      "${_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL}"
      REALPATH)
    file(GLOB_RECURSE _MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS
      CONFIGURE_DEPENDS
      LIST_DIRECTORIES false
      "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}/meshes/*")
    file(MAKE_DIRECTORY "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}")
    execute_process(
      COMMAND
        "${Python3_EXECUTABLE}"
        "${_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL}"
        --urdf
        "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}/lite6_lite_gripper.urdf"
        --srdf
        "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}/lite6_lite_gripper.srdf"
        --model-asset-id
        lite6_lite_gripper
        --accepted-package-uri-prefix
        "package://manyforge_example_models/models/lite6_lite_gripper/"
        --resource-root
        "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
        --output-directory
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      RESULT_VARIABLE _MANYFORGE_ROBOPLAN_LITE6_PREPARATION_RESULT
      OUTPUT_VARIABLE _MANYFORGE_ROBOPLAN_LITE6_PREPARATION_OUTPUT
      ERROR_VARIABLE _MANYFORGE_ROBOPLAN_LITE6_PREPARATION_ERROR)
    if(NOT _MANYFORGE_ROBOPLAN_LITE6_PREPARATION_RESULT EQUAL 0)
      message(FATAL_ERROR
        "I3 non-empty RoboPlan consumer fixture preparation failed:\n"
        "${_MANYFORGE_ROBOPLAN_LITE6_PREPARATION_OUTPUT}"
        "${_MANYFORGE_ROBOPLAN_LITE6_PREPARATION_ERROR}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL}"
      "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}/lite6_lite_gripper.urdf"
      "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}/lite6_lite_gripper.srdf"
      ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS})

    # Deployment owns this deterministic test-only generator. It publishes
    # exact retained bytes and compiler maps, without importing any RoboPlan,
    # collision-engine, Composer, or Pinocchio consumer API.
    add_executable(generate_lite6_retained_consumer_fixture
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test/generate_lite6_retained_consumer_fixture.cpp")
    target_compile_definitions(
      generate_lite6_retained_consumer_fixture PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
    target_include_directories(
      generate_lite6_retained_consumer_fixture PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test")
    target_link_libraries(
      generate_lite6_retained_consumer_fixture PRIVATE
      manyforge_deployment)

    set(_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_retained_consumer_fixture")
    set(_MANYFORGE_LITE6_RETAINED_CONSUMER_MANIFEST
      "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_LITE6_RETAINED_CONSUMER_MANIFEST}"
      BYPRODUCTS
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}/retained-model.urdf"
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}/retained-model.srdf"
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}/retained-control.json"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_retained_consumer_fixture>"
        --output-root
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
      DEPENDS
        generate_lite6_retained_consumer_fixture
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(
      generate_lite6_retained_consumer_product
      DEPENDS "${_MANYFORGE_LITE6_RETAINED_CONSUMER_MANIFEST}")

    # N3 process qualification needs one ordinary retained bundle, the exact
    # compiled Program Binding selected for it, and I4's durable active
    # publication. The generator remains test-only; the production host can
    # consume any independently produced package with the same contracts.
    add_executable(generate_lite6_native_execution_fixture_tool
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test/generate_lite6_native_execution_fixture.cpp")
    target_compile_definitions(
      generate_lite6_native_execution_fixture_tool PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
    target_include_directories(
      generate_lite6_native_execution_fixture_tool PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test")
    target_link_libraries(
      generate_lite6_native_execution_fixture_tool PRIVATE
      manyforge_deployment
      manyforge_component_manager
      manyforge_lite6_product
      manyforge_core)

    set(_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_execution_fixture")
    set(_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_MANIFEST
      "${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root
        "${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_execution_fixture ALL
      DEPENDS "${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_MANIFEST}")
    add_dependencies(test_managed_kernel_admission_gate
      generate_lite6_native_execution_fixture)
    target_compile_definitions(test_managed_kernel_admission_gate PRIVATE
      MANYFORGE_I6_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}")

    # L0.1 reuses the same native package generator through an uninstalled,
    # explicitly non-authoritative build-identity fixture seam. This exercises
    # the conditional product artifact and Manager launch check without pulling
    # the explicit offline Stop qualification runner into an ordinary build.
    set(_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_build_identified_native_execution_fixture")
    set(_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_MANIFEST
      "${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT
        "${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root
        "${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_ROOT}"
        --test-build-identity-target
        "$<TARGET_FILE:manyforge_native_execution_host>"
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        manyforge_native_execution_host
      VERBATIM)
    add_custom_target(generate_lite6_build_identified_native_execution_fixture
      DEPENDS
        "${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_MANIFEST}")

    # Explicit L0.1 evidence lane. Building this target runs the real bounded
    # Stop qualifier once, then feeds its exact four canonical outputs plus
    # the two executable byte subjects through the production retained
    # materialize/store/full-readback path. It is intentionally neither ALL
    # nor CTest and no ordinary fixture or test depends on it.
    set(_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_stop_qualified_native_execution_fixture")
    set(_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_MANIFEST
      "${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT}"
        --stop-implementation-build
          "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        --stop-installed-target
          "$<TARGET_FILE:manyforge_native_execution_host>"
        --stop-runner-contract
          "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
        --stop-runner-executable
          "$<TARGET_FILE:manyforge_native_stop_qualification_runner>"
        --stop-implementation-evidence
          "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        --stop-substitution-evidence
          "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}"
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        manyforge_native_execution_host
        manyforge_native_stop_qualification_runner
        "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
        "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(
      manyforge_materialize_stop_qualified_native_execution_fixture
      DEPENDS "${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_MANIFEST}")
    target_compile_definitions(test_retained_native_cclc_runtime PRIVATE
      MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT="${_MANYFORGE_I7_STOP_QUALIFIED_FIXTURE_ROOT}")
    add_dependencies(test_retained_native_cclc_runtime
      manyforge_materialize_stop_qualified_native_execution_fixture)

    # L0.7 production-process fixture. It retains the same executable and
    # Stop qualification evidence as the focused L0.1 packet, while adding
    # the complete arm+gripper lifecycle catalogs, runnable Program products,
    # and two-domain route configuration consumed by the generic native host.
    set(_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_cclc_product_fixture")
    set(_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_MANIFEST
      "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT}"
        --fixture-kind cclc-product
        --stop-implementation-build
          "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        --stop-installed-target
          "$<TARGET_FILE:manyforge_native_execution_host>"
        --stop-runner-contract
          "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
        --stop-runner-executable
          "$<TARGET_FILE:manyforge_native_stop_qualification_runner>"
        --stop-implementation-evidence
          "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        --stop-substitution-evidence
          "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}"
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        manyforge_native_execution_host
        manyforge_native_stop_qualification_runner
        "${_MANYFORGE_NATIVE_STOP_BUILD_CLOSURE}"
        "${_MANYFORGE_NATIVE_STOP_RUNNER_CONTRACT}"
        "${_MANYFORGE_NATIVE_STOP_IMPLEMENTATION_EVIDENCE}"
        "${_MANYFORGE_NATIVE_STOP_SUBSTITUTION_EVIDENCE}"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_cclc_product_fixture ALL
      DEPENDS "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_MANIFEST}")

    add_executable(test_retained_product_consumer
      src/product_backed_runtime_participant_resolver.cpp
      test/test_retained_product_consumer.cpp)
    add_dependencies(
      test_retained_product_consumer
      generate_canonical_empty_product_package
      generate_lite6_retained_consumer_product
      manyforge_roboplan_candidate_worker)
    target_compile_definitions(
      test_retained_product_consumer PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT="${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
      MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT="${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
      MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH="$<TARGET_FILE:manyforge_roboplan_candidate_worker>")
    target_include_directories(test_retained_product_consumer PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test")
    target_link_libraries(test_retained_product_consumer PRIVATE
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_collision_projection
      manyforge_roboplan_native_state_projection
      manyforge_roboplan_native_runtime
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_collision
      manyforge_core
      manyforge_deployment
      manyforge_deployment_admission
      roboplan::roboplan
      GTest::gtest_main)

    add_executable(test_retained_native_runtime_unit_adapter
      test/test_retained_native_runtime_unit_adapter.cpp)
    add_dependencies(test_retained_native_runtime_unit_adapter
      generate_lite6_native_execution_fixture
      generate_lite6_build_identified_native_execution_fixture
      manyforge_native_execution_host
      manyforge_roboplan_candidate_worker)
    target_compile_definitions(
      test_retained_native_runtime_unit_adapter PRIVATE
      MANYFORGE_I6_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT="$<TARGET_FILE_DIR:manyforge_native_execution_host>")
    target_link_libraries(test_retained_native_runtime_unit_adapter PRIVATE
      manyforge_component_manager
      manyforge_deployment_verification_core
      GTest::gtest_main)

    # Pinocchio/coal/roboplan .so files live in cmeel.prefix/lib.
    # Discover the active Python site-packages roots rather than hardcoding a
    # Python minor version into the test runtime path.
    set(_LIB_PATHS "$ENV{LD_LIBRARY_PATH}")
    execute_process(
      COMMAND python3 -c
              "import site, pathlib; print(';'.join(str(pathlib.Path(p) / 'cmeel.prefix/lib') for p in site.getsitepackages()))"
      OUTPUT_VARIABLE _CMEEL_LIB_DIRS
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    foreach(_dir
        ${_CMEEL_LIB_DIRS}
        "/usr/local/lib")
      if(IS_DIRECTORY "${_dir}")
        string(APPEND _LIB_PATHS ":${_dir}")
      endif()
    endforeach()

    get_filename_component(
      _MANYFORGE_WORKSPACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)

    # The qualification runner removes its ambient loader environment before
    # CTest execution. Register every RoboPlan GTest with the exact library
    # closure derived above instead of relying on inherited process state.
    gtest_discover_tests(test_native_runtime_support
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_roboplan_planning_qualification
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_planning_model_stage
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_product_consumer
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_native_runtime_unit_adapter
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_roboplan_scene_generation
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_roboplan_planner_options
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_cartesian_planning_contract
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_joint_cap_contract
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_roboplan_candidate_computer
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_roboplan_planner_backend
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_native_backends
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_stop_surveillance_owner
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_native_controller_lifecycle_port
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_native_cclc_runtime
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_native_stop_qualification_seams
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_managed_kernel_admission_gate
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_process_contained_candidate_client
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_roboplan_worker_protocol
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    add_test(
      NAME retained_compiled_visual_fk_composer_consumer
      COMMAND
        "${Python3_EXECUTABLE}"
        "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_composer/tests/qualify_retained_compiled_visual_fk.py"
        --fixture-root
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}")
    set_tests_properties(
      retained_compiled_visual_fk_composer_consumer
      PROPERTIES
        ENVIRONMENT
          "PYTHONOPTIMIZE=1;PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT};LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    add_test(
      NAME retained_compiled_model_independent_standard_parser
      COMMAND
        "${Python3_EXECUTABLE}"
        "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_deployment/test/qualify_retained_model_standard_parsers.py"
        --fixture-root
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
        --urdfdom-checker
        "${_MANYFORGE_URDFDOM_CHECK_URDF_EXECUTABLE}")
    set_tests_properties(
      retained_compiled_model_independent_standard_parser
      PROPERTIES
        ENVIRONMENT
          "PYTHONOPTIMIZE=1;PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT};LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    add_test(
      NAME native_addressed_execution_process
      COMMAND
        "${Python3_EXECUTABLE}" -m pytest
        "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_roboplan/test/test_native_execution_path.py")
    set_tests_properties(
      native_addressed_execution_process
      PROPERTIES
        ENVIRONMENT
          "PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT};LD_LIBRARY_PATH=${_LIB_PATHS};MANYFORGE_NATIVE_EXECUTION_HOST=$<TARGET_FILE:manyforge_native_execution_host>;MANYFORGE_NATIVE_PICK_PLACE_FIXTURE_HOST=$<TARGET_FILE:manyforge_native_execution_host_pick_place_fixture>;MANYFORGE_ROBOPLAN_CANDIDATE_WORKER=$<TARGET_FILE:manyforge_roboplan_candidate_worker>;MANYFORGE_ROBOPLAN_FAKE_WORKER=$<TARGET_FILE:fake_process_candidate_worker>;MANYFORGE_I6_NATIVE_FIXTURE_ROOT=${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT}"
        TIMEOUT 60)

  endif()
endif()
