cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
option(BUILD_TESTING "Build unit tests" ON)

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
  add_subdirectory(
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
    EXCLUDE_FROM_ALL)
  cmake_policy(POP)
  set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
  unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
  unset(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL)
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
  src/retained_planner_scene_projection.cpp)
target_include_directories(manyforge_roboplan_collision_projection PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_collision_projection PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_collision_projection PUBLIC
  manyforge_collision
  manyforge_core)

add_library(manyforge_roboplan_native_state_projection
  src/retained_kinematic_state_projector.cpp
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
  src/native_runtime_support.cpp)
target_include_directories(manyforge_roboplan_native_runtime_support PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_runtime_support
  PUBLIC manyforge_core
  PRIVATE Threads::Threads)

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
  src/retained_native_runtime.cpp)
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
  manyforge_core_sqlite_operation_journal)

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

# Selected composition root for the first complete addressed native path.
# Its same-user Unix socket is a bounded private transport consumed by the
# headless API; it is not a second public execution contract.
add_executable(manyforge_native_execution_host
  src/native_execution_host_main.cpp)
target_compile_definitions(manyforge_native_execution_host PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_native_execution_host PRIVATE
  manyforge_roboplan_native_runtime
  manyforge_roboplan_process_candidate_client
  manyforge_roboplan_retained_consumer
  manyforge_component_manager
  manyforge_deployment
  Threads::Threads)
install(TARGETS manyforge_native_execution_host
  RUNTIME DESTINATION bin)

message(STATUS
  "manyforge_roboplan I6 cutover: retained-product projection, addressed "
  "native state, focused planning, execution/gripper adapters, runtime "
  "composition, and process-contained candidate computation are built; "
  "the bounded N3 headless/Python callers are available, while full Program "
  "execution, event collection, ROS adapters, and product workflow lanes "
  "remain unavailable until their I6 gates")

# Tests
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    find_package(roboplan_example_models REQUIRED)

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

    add_executable(test_retained_product_consumer
      test/test_retained_product_consumer.cpp)
    add_dependencies(
      test_retained_product_consumer
      generate_canonical_empty_product_package
      generate_lite6_retained_consumer_product)
    target_compile_definitions(
      test_retained_product_consumer PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT="${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
      MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT="${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}")
    target_include_directories(test_retained_product_consumer PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test")
    target_link_libraries(test_retained_product_consumer PRIVATE
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_collision_projection
      manyforge_roboplan_native_state_projection
      manyforge_roboplan_native_runtime
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
      manyforge_native_execution_host)
    target_compile_definitions(
      test_retained_native_runtime_unit_adapter PRIVATE
      MANYFORGE_I6_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
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
          "PYTHONOPTIMIZE=1;PYTHONPATH=${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior:${_MANYFORGE_WORKSPACE_ROOT};LD_LIBRARY_PATH=${_LIB_PATHS};MANYFORGE_NATIVE_EXECUTION_HOST=$<TARGET_FILE:manyforge_native_execution_host>;MANYFORGE_ROBOPLAN_CANDIDATE_WORKER=$<TARGET_FILE:manyforge_roboplan_candidate_worker>;MANYFORGE_ROBOPLAN_FAKE_WORKER=$<TARGET_FILE:fake_process_candidate_worker>;MANYFORGE_I6_NATIVE_FIXTURE_ROOT=${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
        TIMEOUT 60)

  endif()
endif()
