cmake_minimum_required(VERSION 3.14)
project(manyforge_headless VERSION 0.2.0 LANGUAGES CXX)

cmake_policy(SET CMP0077 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(SQLite3 3.37 REQUIRED)
find_package(Threads REQUIRED)

set(
  MANYFORGE_DEPLOYMENT_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment"
  CACHE PATH
  "Path to the manyforge_deployment source tree")
get_filename_component(
  MANYFORGE_DEPLOYMENT_SOURCE_DIR
  "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}"
  REALPATH)
if(NOT TARGET manyforge::deployment_verification_core)
  if(NOT EXISTS
     "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/include/manyforge_deployment/artifact_codec.hpp")
    message(FATAL_ERROR
      "manyforge_headless requires the deployment strict JSON and retained "
      "verification substrate")
  endif()
  set(_MANYFORGE_HEADLESS_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_HEADLESS_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_HEADLESS_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_HEADLESS_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_HEADLESS_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge::deployment_verification_core OR
   NOT TARGET manyforge::content_identity)
  message(FATAL_ERROR
    "manyforge_headless could not resolve its strict codec dependencies")
endif()

add_library(manyforge_headless
  src/manager_package_generation.cpp
  src/operator_alarm_catalog.cpp
  src/operator_alarm_metrics.cpp
  src/operator_alarm_owner.cpp
  src/operator_alarm_reducer.cpp
  src/runtime_snapshot.cpp
  src/scene_runtime_state_owner.cpp
  src/source_snapshot.cpp)
add_library(manyforge::headless ALIAS manyforge_headless)
target_include_directories(manyforge_headless PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_headless PUBLIC
  manyforge::content_identity
  manyforge::deployment_verification_core)
set_target_properties(manyforge_headless PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(manyforge_node_environment_projection
  src/assistant_mode_projection.cpp
  src/node_environment_projection.cpp
  src/source_model_projection.cpp)
add_library(manyforge::node_environment_projection ALIAS
  manyforge_node_environment_projection)
target_include_directories(manyforge_node_environment_projection PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_node_environment_projection PUBLIC
  manyforge::content_identity
  manyforge::deployment_verification_core)
set_target_properties(manyforge_node_environment_projection PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

add_library(manyforge_event_historian
  src/event_historian.cpp
  src/headless_api_core.cpp
  src/sqlite_event_history_store.cpp)
add_library(manyforge::event_historian ALIAS manyforge_event_historian)
target_include_directories(manyforge_event_historian PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_event_historian PUBLIC
  manyforge_headless
  manyforge_node_environment_projection
  manyforge::deployment
  SQLite::SQLite3
  Threads::Threads)
set_target_properties(manyforge_event_historian PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

option(MANYFORGE_HEADLESS_BUILD_PYTHON
       "Build the thin pybind11 headless API bridge" OFF)
if(MANYFORGE_HEADLESS_BUILD_PYTHON)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(manyforge_headless_py NO_EXTRAS python/bindings.cpp)
  target_link_libraries(manyforge_headless_py PRIVATE
    manyforge_event_historian
    manyforge::expression
    pybind11::module
    pybind11::headers)
  install(TARGETS manyforge_headless_py
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .)
endif()

set(
  MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_component_manager"
  CACHE PATH
  "Path to the manyforge_component_manager source tree")
if(NOT TARGET manyforge::component_manager)
  if(NOT EXISTS
     "${MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR}/include/manyforge_component_manager/manager_active_runtime_coordinator.hpp")
    message(FATAL_ERROR
      "manyforge_headless owner publications require the Component Manager")
  endif()
  set(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_COMPONENT_MANAGER_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_component_manager"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_HEADLESS_MANAGER_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()

set(
  MANYFORGE_SCENE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_scene"
  CACHE PATH
  "Path to the manyforge_scene source tree")
if(NOT TARGET manyforge::scene)
  if(NOT EXISTS
     "${MANYFORGE_SCENE_SOURCE_DIR}/include/manyforge_scene/logical_scene_owner.hpp")
    message(FATAL_ERROR
      "manyforge_headless owner publications require the logical-scene owner")
  endif()
  set(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING_WAS_DEFINED FALSE)
  if(DEFINED BUILD_TESTING)
    set(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING_WAS_DEFINED TRUE)
    set(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING "${BUILD_TESTING}")
  endif()
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_SCENE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_scene"
    EXCLUDE_FROM_ALL)
  if(_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING_WAS_DEFINED)
    set(BUILD_TESTING "${_MANYFORGE_HEADLESS_SCENE_BUILD_TESTING}")
  else()
    unset(BUILD_TESTING)
  endif()
endif()
if(NOT TARGET manyforge::component_manager OR NOT TARGET manyforge::scene)
  message(FATAL_ERROR
    "manyforge_headless could not resolve its source-owner dependencies")
endif()

# Deployment may have been added before Scene materialized the existing Core
# target. Its standalone codec branch must resolve provider-domain validation
# through that same Core implementation once the Headless graph supplies it.
if(TARGET manyforge_deployment_runtime_contract_codec)
  target_link_libraries(manyforge_deployment_runtime_contract_codec PUBLIC
    manyforge_core)
endif()

add_library(manyforge_headless_owner_publication
  src/owner_publication.cpp
  src/scene_runtime_pose_projector.cpp)
add_library(manyforge::headless_owner_publication ALIAS
  manyforge_headless_owner_publication)
target_include_directories(manyforge_headless_owner_publication PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_headless_owner_publication PUBLIC
  manyforge_headless
  manyforge::component_manager
  manyforge::scene
  manyforge::deployment_verification_core)
set_target_properties(manyforge_headless_owner_publication PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

add_executable(manyforge_manager_daemon src/manager_daemon.cpp)
target_link_libraries(manyforge_manager_daemon PRIVATE
  manyforge_headless_owner_publication
  manyforge::deployment_admission
  # Apply acceptance recompiles the stored prepared Program outcome.
  manyforge::deployment)

option(BUILD_TESTING "Build unit tests" ON)
set(
  MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST
  ""
  CACHE FILEPATH
  "Optional selected ROS execution host for the real Manager Apply process test")
set(MANYFORGE_HEADLESS_TEST_ROS_SETUP "" CACHE FILEPATH
  "Chained built ROS overlay setup.bash for enabled Manager Python process tests")
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)
  set(MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
      "Per-test timeout for manyforge_headless gtest cases")

  add_executable(test_manager_package_generation
    test/test_manager_package_generation.cpp)
  target_link_libraries(test_manager_package_generation PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_manager_package_generation
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_source_snapshot test/test_source_snapshot.cpp)
  target_link_libraries(test_source_snapshot PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_source_snapshot
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_runtime_snapshot test/test_runtime_snapshot.cpp)
  target_link_libraries(test_runtime_snapshot PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_runtime_snapshot
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_scene_runtime_state_owner
    test/test_scene_runtime_state_owner.cpp)
  target_link_libraries(test_scene_runtime_state_owner PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_scene_runtime_state_owner
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_scene_admission_reconciliation
    test/test_scene_admission_reconciliation.cpp)
  target_link_libraries(test_scene_admission_reconciliation PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_scene_admission_reconciliation
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_response_transport
    test/test_manager_response_transport.cpp)
  target_link_libraries(test_manager_response_transport PRIVATE
    GTest::gtest_main)
  gtest_discover_tests(test_manager_response_transport
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_scene_runtime_pose_projector
    test/test_scene_runtime_pose_projector.cpp)
  target_link_libraries(test_scene_runtime_pose_projector PRIVATE
    manyforge_headless_owner_publication GTest::gtest_main)
  gtest_discover_tests(test_scene_runtime_pose_projector
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_operator_alarm_catalog
    test/test_operator_alarm_catalog.cpp)
  target_compile_definitions(test_operator_alarm_catalog PRIVATE
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml")
  target_link_libraries(test_operator_alarm_catalog PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_operator_alarm_catalog
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_operator_alarm_owner
    test/test_operator_alarm_owner.cpp)
  target_compile_definitions(test_operator_alarm_owner PRIVATE
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml")
  target_link_libraries(test_operator_alarm_owner PRIVATE
    manyforge_headless GTest::gtest_main)
  gtest_discover_tests(test_operator_alarm_owner
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(operator_alarm_performance_probe
    test/operator_alarm_performance_probe.cpp)
  target_link_libraries(operator_alarm_performance_probe PRIVATE
    manyforge_headless)
  add_test(NAME operator_alarm_performance_probe
    COMMAND operator_alarm_performance_probe
      --catalog
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml"
      --iterations 10000)
  set_tests_properties(operator_alarm_performance_probe PROPERTIES
    TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_event_historian test/test_event_historian.cpp)
  add_executable(historian_performance_probe test/historian_performance_probe.cpp)
  target_link_libraries(historian_performance_probe PRIVATE manyforge_event_historian)
  target_link_libraries(test_event_historian PRIVATE
    manyforge_event_historian GTest::gtest_main)
  gtest_discover_tests(test_event_historian
    PROPERTIES
      RUN_SERIAL TRUE
      TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/test/event_historian_ctest_properties.cmake")

  set(
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/canonical-empty-test-package")
  set(
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_MARKER
    "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}/canonical-empty-admission.json")
  add_custom_command(
    OUTPUT "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}"
    COMMAND
      "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    COMMAND
      "$<TARGET_FILE:manyforge_generate_canonical_empty_package>"
      --output-root
      "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    DEPENDS manyforge_generate_canonical_empty_package
    VERBATIM)
  add_custom_target(manyforge_headless_test_canonical_empty_package
    DEPENDS "${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_MARKER}")

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  get_filename_component(
    MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper"
    REALPATH)
  set(
    MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lite6_lite_gripper_structural")
  file(
    GLOB_RECURSE
    MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}/meshes/*")
  file(MAKE_DIRECTORY "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}")
  execute_process(
    COMMAND
      "${Python3_EXECUTABLE}"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/tools/prepare_structural_model.py"
      --urdf
      "${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}/lite6_lite_gripper.urdf"
      --srdf
      "${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}/lite6_lite_gripper.srdf"
      --model-asset-id
      lite6_lite_gripper
      --accepted-package-uri-prefix
      "package://manyforge_example_models/models/lite6_lite_gripper/"
      --resource-root
      "${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}"
      --output-directory
      "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}"
    RESULT_VARIABLE MANYFORGE_HEADLESS_TEST_LITE6_PREPARATION_RESULT
    OUTPUT_VARIABLE MANYFORGE_HEADLESS_TEST_LITE6_PREPARATION_OUTPUT
    ERROR_VARIABLE MANYFORGE_HEADLESS_TEST_LITE6_PREPARATION_ERROR)
  if(NOT MANYFORGE_HEADLESS_TEST_LITE6_PREPARATION_RESULT EQUAL 0)
    message(FATAL_ERROR
      "headless assistant fixture preparation failed:\n"
      "${MANYFORGE_HEADLESS_TEST_LITE6_PREPARATION_OUTPUT}"
      "${MANYFORGE_HEADLESS_TEST_LITE6_PREPARATION_ERROR}")
  endif()
  set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/tools/prepare_structural_model.py"
    "${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}/lite6_lite_gripper.urdf"
    "${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}/lite6_lite_gripper.srdf"
    ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS})

  add_executable(manyforge_generate_headless_assistant_test_package
    test/generate_assistant_authoring_package.cpp)
  target_compile_definitions(
    manyforge_generate_headless_assistant_test_package PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}")
  target_include_directories(
    manyforge_generate_headless_assistant_test_package PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_link_libraries(
    manyforge_generate_headless_assistant_test_package PRIVATE
    manyforge::deployment)

  set(
    MANYFORGE_HEADLESS_TEST_ASSISTANT_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/assistant-authoring-test-fixture")
  set(
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT
    "${MANYFORGE_HEADLESS_TEST_ASSISTANT_FIXTURE_ROOT}/retained-package")
  set(
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE
    "${MANYFORGE_HEADLESS_TEST_ASSISTANT_FIXTURE_ROOT}/retained-bundle-id.txt")
  add_custom_command(
    OUTPUT "${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    COMMAND
      "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_FIXTURE_ROOT}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_FIXTURE_ROOT}"
    COMMAND
      "$<TARGET_FILE:manyforge_generate_headless_assistant_test_package>"
      --package-root
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
      --bundle-id-output
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    DEPENDS
      manyforge_generate_headless_assistant_test_package
      "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.urdf"
      "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.srdf"
      ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
    VERBATIM)
  add_custom_target(manyforge_headless_test_assistant_package
    DEPENDS "${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}")

  add_executable(manyforge_generate_headless_alarm_registry_test_package
    test/generate_alarm_registry_package.cpp)
  target_compile_definitions(
    manyforge_generate_headless_alarm_registry_test_package PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}")
  target_include_directories(
    manyforge_generate_headless_alarm_registry_test_package PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_link_libraries(
    manyforge_generate_headless_alarm_registry_test_package PRIVATE
    manyforge::deployment
    manyforge_deployment_canonical_package_generator)

  set(
    MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_FIXTURE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/alarm-registry-test-fixture")
  set(
    MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT
    "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_FIXTURE_ROOT}/retained-package")
  set(
    MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE
    "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_FIXTURE_ROOT}/retained-bundle-ids.txt")
  add_custom_command(
    OUTPUT "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE}"
    COMMAND
      "${CMAKE_COMMAND}" -E remove_directory
      "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_FIXTURE_ROOT}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_FIXTURE_ROOT}"
    COMMAND
      "$<TARGET_FILE:manyforge_generate_headless_alarm_registry_test_package>"
      --package-root
      "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT}"
      --cell-id
      c1_build_authority_cell
      --bundle-ids-output
      "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE}"
    DEPENDS
      manyforge_generate_headless_alarm_registry_test_package
      "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.urdf"
      "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.srdf"
      ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
    VERBATIM)
  add_custom_target(manyforge_headless_test_alarm_registry_package
    DEPENDS "${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE}")

  if(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
    # Tests that start a ROS stack run in their own ROS domain (Fast DDS
    # #6599); same launcher as manyforge_roboplan's tests.
    find_program(MANYFORGE_RUN_RMW_ISOLATED run_rmw_isolated
      PATHS ENV AMENT_PREFIX_PATH
      PATH_SUFFIXES lib/rmw_test_fixture_implementation
      NO_DEFAULT_PATH REQUIRED)
    set(MANYFORGE_HEADLESS_ROS_TEST_LAUNCHER
      env ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST "${MANYFORGE_RUN_RMW_ISOLATED}")
    if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}")
      message(FATAL_ERROR
        "the selected execution host for the Manager Apply process "
        "test does not exist: ${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}")
    endif()
    get_filename_component(
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}"
      REALPATH)
    get_filename_component(
      _MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_TARGET_ROOT
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}"
      DIRECTORY)
    set(
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER
      "${_MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_TARGET_ROOT}/manyforge_roboplan_candidate_worker")
    if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER}")
      message(FATAL_ERROR
        "the build-owned native candidate worker for the Manager Apply "
        "process test does not exist: "
        "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER}")
    endif()
    set(
      MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST
      "${_MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_TARGET_ROOT}/manyforge_ros2_control_execution_host_pick_place_planning_fixture")
    if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST}")
      message(FATAL_ERROR
        "the compatible replacement execution host does not exist: "
        "${MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST}")
    endif()
    # Manager Apply packages are generated from example Scenarios through the
    # normal activation path. Each model component reads its prepared root as
    # a `generated/<asset>_structural` sibling of the combined Lite6 root.
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_MODEL_FAMILIES
      lite6 lite_gripper robotiq_2f_85 robotiq_2f_140 ur10e)
    set(MANYFORGE_HEADLESS_TEST_MAINTAINED_DESCRIPTION
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/resources/maintained/maintained_deployment_sources.v1.json")
    file(READ "${MANYFORGE_HEADLESS_TEST_MAINTAINED_DESCRIPTION}"
      _manyforge_headless_maintained_description)
    string(JSON _manyforge_headless_family_count LENGTH
      "${_manyforge_headless_maintained_description}" model_families)
    math(EXPR _manyforge_headless_family_last "${_manyforge_headless_family_count} - 1")
    get_filename_component(_manyforge_headless_workspace
      "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
    set(MANYFORGE_HEADLESS_TEST_MAINTAINED_MODEL_INPUTS
      "${MANYFORGE_HEADLESS_TEST_MAINTAINED_DESCRIPTION}")
    foreach(_family_index RANGE ${_manyforge_headless_family_last})
      string(JSON _family_id GET "${_manyforge_headless_maintained_description}"
        model_families ${_family_index} model_family_id)
      if(NOT _family_id IN_LIST MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_MODEL_FAMILIES)
        continue()
      endif()
      foreach(_field urdf_path srdf_path resource_root accepted_package_uri_prefix)
        string(JSON _family_${_field} GET "${_manyforge_headless_maintained_description}"
          model_families ${_family_index} source ${_field})
      endforeach()
      set(_family_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/generated/${_family_id}_structural")
      file(GLOB_RECURSE _family_resource_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${_manyforge_headless_workspace}/${_family_resource_root}/meshes/*")
      file(MAKE_DIRECTORY "${_family_fixture_root}")
      execute_process(
        COMMAND "${Python3_EXECUTABLE}"
          "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/tools/prepare_structural_model.py"
          --urdf "${_manyforge_headless_workspace}/${_family_urdf_path}"
          --srdf "${_manyforge_headless_workspace}/${_family_srdf_path}"
          --model-asset-id "${_family_id}"
          --accepted-package-uri-prefix "${_family_accepted_package_uri_prefix}"
          --resource-root "${_manyforge_headless_workspace}/${_family_resource_root}"
          --output-directory "${_family_fixture_root}"
        RESULT_VARIABLE _family_preparation_result
        OUTPUT_VARIABLE _family_preparation_output
        ERROR_VARIABLE _family_preparation_error)
      if(NOT _family_preparation_result EQUAL 0)
        message(FATAL_ERROR
          "headless Manager Apply model preparation failed for ${_family_id}:\n"
          "${_family_preparation_output}${_family_preparation_error}")
      endif()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/tools/prepare_structural_model.py"
        "${_manyforge_headless_workspace}/${_family_urdf_path}"
        "${_manyforge_headless_workspace}/${_family_srdf_path}"
        ${_family_resource_inputs})
      list(APPEND MANYFORGE_HEADLESS_TEST_MAINTAINED_MODEL_INPUTS
        "${_family_fixture_root}/preparation-manifest.json"
        "${_family_fixture_root}/model.urdf"
        "${_family_fixture_root}/model.srdf"
        ${_family_resource_inputs})
    endforeach()
    foreach(_scenario mock_lite6_pick_and_place mock_ur10e_assistant_scene_authoring
        mock_ur5_semantic_object_pick)
      list(APPEND MANYFORGE_HEADLESS_TEST_MAINTAINED_MODEL_INPUTS
        "${_manyforge_headless_workspace}/examples/${_scenario}.scenario/deployment.yaml"
        "${_manyforge_headless_workspace}/examples/${_scenario}.scenario/program.yaml")
    endforeach()
    # Close is the Apply of this empty intent.
    list(APPEND MANYFORGE_HEADLESS_TEST_MAINTAINED_MODEL_INPUTS
      "${_manyforge_headless_workspace}/manyforge_composer/backend/resources/Untitled.scenario/deployment.yaml")

    add_executable(manyforge_generate_headless_manager_apply_test_package
      test/generate_manager_apply_package.cpp)
    target_compile_definitions(
      manyforge_generate_headless_manager_apply_test_package PRIVATE
      MANYFORGE_WORKSPACE_ROOT="${_manyforge_headless_workspace}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}")
    target_include_directories(
      manyforge_generate_headless_manager_apply_test_package PRIVATE
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
    target_link_libraries(
      manyforge_generate_headless_manager_apply_test_package PRIVATE
      manyforge_preparation_support
      manyforge::deployment
      manyforge::deployment_admission
      manyforge_deployment_canonical_package_generator)

    # One generated package per profile: <fixture root>/retained-package and
    # <fixture root>/metadata.json.
    function(manyforge_headless_manager_apply_package target fixture_root profile)
      add_custom_command(
        OUTPUT "${fixture_root}/metadata.json"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${fixture_root}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture_root}"
        COMMAND "$<TARGET_FILE:manyforge_generate_headless_manager_apply_test_package>"
          --package-root "${fixture_root}/retained-package"
          --metadata "${fixture_root}/metadata.json"
          --profile ${profile}
        DEPENDS manyforge_generate_headless_manager_apply_test_package
          ${MANYFORGE_HEADLESS_TEST_MAINTAINED_MODEL_INPUTS} ${ARGN}
        VERBATIM)
      add_custom_target(${target} DEPENDS "${fixture_root}/metadata.json")
    endfunction()

    # The Lite 6 machine with its mounted Lite gripper.
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/manager-apply-test-fixture")
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}/retained-package")
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}/metadata.json")
    manyforge_headless_manager_apply_package(
      manyforge_headless_test_manager_apply_package
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}" ros2-control
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}")

    # The UR10e + Robotiq 2F-140 cell with its scene-authoring assistant.
    set(MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/assistant-native-test-fixture")
    set(MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}/retained-package")
    set(MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_METADATA
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}/metadata.json")
    manyforge_headless_manager_apply_package(
      manyforge_headless_test_assistant_native_package
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}"
      assistant-ros2-control
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}"
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER}")

    # The Lite gripper alone: pneumatic, commanded output, with feedback.
    set(MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/pneumatic-commanded-feedback-manager-test-fixture")
    set(MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_FIXTURE_ROOT}/retained-package")
    set(MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_METADATA
      "${MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_FIXTURE_ROOT}/metadata.json")
    manyforge_headless_manager_apply_package(
      manyforge_headless_test_pneumatic_commanded_feedback_package
      "${MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_FIXTURE_ROOT}"
      gripper-pneumatic-commanded
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}")

    # The Robotiq 2F-85 alone: a Position gripper with no arm.
    set(MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/robotiq-position-manager-test-fixture")
    set(MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_FIXTURE_ROOT}/retained-package")
    set(MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_METADATA
      "${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_FIXTURE_ROOT}/metadata.json")
    manyforge_headless_manager_apply_package(
      manyforge_headless_test_robotiq_position_package
      "${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_FIXTURE_ROOT}"
      gripper-only-robotiq-position
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}"
      "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER}")

    if(MANYFORGE_HEADLESS_BUILD_PYTHON)
      if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_ROS_SETUP}" OR
          IS_DIRECTORY "${MANYFORGE_HEADLESS_TEST_ROS_SETUP}")
        message(FATAL_ERROR
          "Manager Python process tests require MANYFORGE_HEADLESS_TEST_ROS_SETUP "
          "to name the current chained built ROS overlay setup.bash")
      endif()
      file(REAL_PATH "${MANYFORGE_HEADLESS_TEST_ROS_SETUP}"
        MANYFORGE_HEADLESS_TEST_ROS_SETUP)
      add_dependencies(manyforge_headless_py
        manyforge_manager_daemon manyforge_headless_test_canonical_empty_package
        manyforge_headless_test_robotiq_position_package)
      add_test(NAME manager_robotiq_position_process
        COMMAND bash -c [=[set -e; source "$1"; shift; exec "$@"]=]
          manager-robotiq-position "${MANYFORGE_HEADLESS_TEST_ROS_SETUP}"
          ${MANYFORGE_HEADLESS_ROS_TEST_LAUNCHER} "${Python3_EXECUTABLE}" -m pytest
          "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_headless_api/process_tests/test_process_stack.py::test_manager_robotiq_position_cell_has_no_motion_target_and_refuses_disabled_command"
          -p no:cacheprovider -v)
      set_tests_properties(manager_robotiq_position_process PROPERTIES
        ENVIRONMENT
          "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/..:${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior:$ENV{PYTHONPATH};MANYFORGE_MANAGER_DAEMON=$<TARGET_FILE:manyforge_manager_daemon>;MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT=${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT};MANYFORGE_ROBOTIQ_POSITION_PACKAGE_ROOT=${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_PACKAGE_ROOT};MANYFORGE_ROBOTIQ_POSITION_METADATA=${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_METADATA};MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST};MANYFORGE_ROBOPLAN_CANDIDATE_WORKER=${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER};MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/manager-robotiq-position-evidence"
        TIMEOUT 180 RUN_SERIAL TRUE)
    endif()

    # A compatible pick/place host consumes a second copy of the same
    # semantic package.
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/manager-apply-replacement-fixture")
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}/retained-package")
    set(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}/metadata.json")
    manyforge_headless_manager_apply_package(
      manyforge_headless_test_manager_apply_replacement_package
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}"
      ros2-control
      "${MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST}")
  endif()

  add_executable(manyforge_publish_headless_assistant_test_source
    test/publish_assistant_authoring_source.cpp)
  add_dependencies(manyforge_publish_headless_assistant_test_source
    manyforge_headless_test_assistant_package
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(
    manyforge_publish_headless_assistant_test_source PRIVATE
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE="${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(
    manyforge_publish_headless_assistant_test_source PRIVATE
    manyforge_headless_owner_publication
    manyforge::deployment_admission)

  add_executable(test_headless_api_core test/test_headless_api_core.cpp)
  add_dependencies(test_headless_api_core
    manyforge_headless_test_canonical_empty_package
    manyforge_headless_test_assistant_package
    manyforge_headless_test_alarm_registry_package)
  target_include_directories(test_headless_api_core PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
  target_compile_definitions(test_headless_api_core PRIVATE
    MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}"
    MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}"
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml"
    MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE="${MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_NODE_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_catalog.yaml"
    MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_environment_manifest.yaml")
  target_link_libraries(test_headless_api_core PRIVATE
    manyforge_preparation_support
    manyforge_event_historian
    manyforge::core_sqlite_operation_journal
    manyforge::deployment
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_headless_api_core
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_owner_publication test/test_owner_publication.cpp)
  add_dependencies(test_owner_publication
    manyforge_headless_test_canonical_empty_package
    manyforge_headless_test_assistant_package)
  target_compile_definitions(test_owner_publication PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE="${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}")
  target_link_libraries(test_owner_publication PRIVATE
    manyforge_headless_owner_publication
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_owner_publication
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_node_environment_projection
    test/test_node_environment_projection.cpp)
  add_dependencies(test_node_environment_projection
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_node_environment_projection PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_NODE_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_catalog.yaml"
    MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_environment_manifest.yaml")
  target_link_libraries(test_node_environment_projection PRIVATE
    manyforge_node_environment_projection
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_node_environment_projection
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_source_model_projection
    test/test_source_model_projection.cpp)
  add_dependencies(test_source_model_projection
    manyforge_headless_test_canonical_empty_package
    manyforge_headless_test_assistant_package)
  target_include_directories(test_source_model_projection PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
  target_compile_definitions(test_source_model_projection PRIVATE
    MANYFORGE_WORKSPACE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE="${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(test_source_model_projection PRIVATE
    manyforge_preparation_support
    manyforge_node_environment_projection
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_source_model_projection
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_assistant_mode_projection
    test/test_assistant_mode_projection.cpp)
  add_dependencies(test_assistant_mode_projection
    manyforge_headless_test_canonical_empty_package
    manyforge_headless_test_assistant_package)
  target_compile_definitions(test_assistant_mode_projection PRIVATE
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE="${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(test_assistant_mode_projection PRIVATE
    manyforge_node_environment_projection
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_assistant_mode_projection
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_manager_daemon_process
    test/test_manager_daemon_process.cpp)
  add_dependencies(test_manager_daemon_process
    manyforge_manager_daemon
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_manager_daemon_process PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON="$<TARGET_FILE:manyforge_manager_daemon>")
  if(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
    set_target_properties(test_manager_daemon_process PROPERTIES
      CROSSCOMPILING_EMULATOR "${MANYFORGE_HEADLESS_ROS_TEST_LAUNCHER}")
    add_dependencies(test_manager_daemon_process
      manyforge_headless_test_manager_apply_package
      manyforge_headless_test_assistant_native_package
      manyforge_headless_test_robotiq_position_package
      manyforge_headless_test_pneumatic_commanded_feedback_package)
    target_compile_definitions(test_manager_daemon_process PRIVATE
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT}"
      MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_METADATA="${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_METADATA}"
      MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_PACKAGE_ROOT}"
      MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_METADATA="${MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_METADATA}"
      MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_PACKAGE_ROOT}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT}"
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST="${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}"
      MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST="${MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST}"
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER="${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER}")
    add_dependencies(test_manager_daemon_process
      manyforge_headless_test_manager_apply_replacement_package)
  endif()
  # Register the actual selected ROS host directly.
  set(_manager_ros_host "${MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST}")
  if(_manager_ros_host)
    if(NOT EXISTS "${_manager_ros_host}")
      message(FATAL_ERROR "the selected ROS execution host does not exist: ${_manager_ros_host}")
    endif()
    file(REAL_PATH "${_manager_ros_host}" _manager_ros_host)
    get_filename_component(_manager_ros_host_directory "${_manager_ros_host}" DIRECTORY)
    set(_manager_ros_worker "${_manager_ros_host_directory}/manyforge_roboplan_candidate_worker")
    if(NOT EXISTS "${_manager_ros_worker}")
      message(FATAL_ERROR "the selected ROS candidate worker does not exist: ${_manager_ros_worker}")
    endif()
    file(REAL_PATH "${_manager_ros_worker}" _manager_ros_worker)
    set(_manager_ros_fixture "${CMAKE_CURRENT_BINARY_DIR}/manager-ros-apply-fixture")
    file(GLOB_RECURSE _manager_ros_fixture_models CONFIGURE_DEPENDS
      LIST_DIRECTORIES false
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper/*")
    add_custom_command(
      OUTPUT "${_manager_ros_fixture}/metadata.json"
      COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/..:${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior"
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/test/prepare_activation_tool.py"
        --tool "$<TARGET_FILE:manyforge_prepare_activation>"
        --workspace "${CMAKE_CURRENT_SOURCE_DIR}/.."
        --scenario examples/mock_lite6_pick_and_place.scenario
        --manager-ros-apply-fixture-root "${_manager_ros_fixture}"
      DEPENDS manyforge_prepare_activation
        "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/test/prepare_activation_tool.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/mock_lite6_pick_and_place.scenario/scenario.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/mock_lite6_pick_and_place.scenario/deployment.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/mock_lite6_pick_and_place.scenario/program.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/lite6_lite_gripper_pick_and_place.program.yaml"
        ${_manager_ros_fixture_models}
      VERBATIM)
    add_custom_target(manyforge_headless_test_manager_ros_apply_fixture
      DEPENDS "${_manager_ros_fixture}/metadata.json")
    add_dependencies(test_manager_daemon_process
      manyforge_headless_test_manager_ros_apply_fixture)
    target_compile_definitions(test_manager_daemon_process PRIVATE
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST="${_manager_ros_host}"
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER="${_manager_ros_worker}"
      MANYFORGE_HEADLESS_TEST_MANAGER_ROS_APPLY_FIXTURE="${_manager_ros_fixture}"
      MANYFORGE_HEADLESS_TEST_ROS_LAUNCH_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_ros_bringup/launch/ros2_control.launch.py")
  endif()
  if(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST OR _manager_ros_host)
    target_compile_definitions(test_manager_daemon_process PRIVATE
      MANYFORGE_HEADLESS_TEST_PYTHON="${Python3_EXECUTABLE}"
      MANYFORGE_HEADLESS_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
      MANYFORGE_HEADLESS_TEST_PYTHON_MODULE_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  target_link_libraries(test_manager_daemon_process PRIVATE
    manyforge_headless_owner_publication
    # Publishes real prepared activations for each Apply.
    manyforge::deployment
    GTest::gtest_main)
  gtest_discover_tests(test_manager_daemon_process
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})
endif()
