cmake_minimum_required(VERSION 3.14)
project(manyforge_headless VERSION 0.1.0 LANGUAGES CXX)

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
  src/program_apply_owner.cpp
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
     "${MANYFORGE_SCENE_SOURCE_DIR}/include/manyforge_scene/canonical_empty_scene_owner.hpp")
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
  manyforge::deployment_admission)

option(BUILD_TESTING "Build unit tests" ON)
set(
  MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST
  ""
  CACHE FILEPATH
  "Optional selected native execution host for the real Manager Apply process test")
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
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

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
  set(
    MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
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
  set(
    MANYFORGE_HEADLESS_TEST_ASSISTANT_INSTRUMENT_SOURCE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/resources/assistant/assistant_instrument_registry_source.v1.json")
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
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_INSTRUMENT_SOURCE}"
      ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
    VERBATIM)
  add_custom_target(manyforge_headless_test_assistant_package
    DEPENDS "${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}")

  if(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST)
    if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}")
      message(FATAL_ERROR
        "the selected native execution host for the Manager Apply process "
        "test does not exist: ${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}")
    endif()
    get_filename_component(
      MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST
      "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}"
      REALPATH)
    get_filename_component(
      _MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_TARGET_ROOT
      "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}"
      DIRECTORY)
    set(
      MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER
      "${_MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_TARGET_ROOT}/manyforge_roboplan_candidate_worker")
    if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER}")
      message(FATAL_ERROR
        "the build-owned native candidate worker for the Manager Apply "
        "process test does not exist: "
        "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER}")
    endif()
    set(
      MANYFORGE_HEADLESS_TEST_NATIVE_REPLACEMENT_EXECUTION_HOST
      "${_MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_TARGET_ROOT}/manyforge_native_execution_host_pick_place_fixture")
    if(NOT EXISTS "${MANYFORGE_HEADLESS_TEST_NATIVE_REPLACEMENT_EXECUTION_HOST}")
      message(FATAL_ERROR
        "the compatible replacement native execution host does not exist: "
        "${MANYFORGE_HEADLESS_TEST_NATIVE_REPLACEMENT_EXECUTION_HOST}")
    endif()
    if(NOT TARGET manyforge_lite6_product)
      add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_lite6_product"
        "${CMAKE_CURRENT_BINARY_DIR}/manyforge_lite6_product"
        EXCLUDE_FROM_ALL)
    endif()
    add_executable(manyforge_generate_headless_manager_apply_test_package
      test/generate_manager_apply_package.cpp)
    target_compile_definitions(
      manyforge_generate_headless_manager_apply_test_package PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${MANYFORGE_HEADLESS_TEST_LITE6_SOURCE_ROOT}")
    target_include_directories(
      manyforge_generate_headless_manager_apply_test_package PRIVATE
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test")
    target_link_libraries(
      manyforge_generate_headless_manager_apply_test_package PRIVATE
      manyforge::deployment
      manyforge::deployment_admission
      manyforge_deployment_canonical_package_generator
      manyforge_lite6_product)

    set(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/manager-apply-test-fixture")
    set(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}/retained-package")
    set(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}/metadata.json")
    add_custom_command(
      OUTPUT "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA}"
      COMMAND
        "${CMAKE_COMMAND}" -E remove_directory
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:manyforge_generate_headless_manager_apply_test_package>"
        --package-root
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT}"
        --metadata
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA}"
      DEPENDS
        manyforge_generate_headless_manager_apply_test_package
        "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.urdf"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.srdf"
        ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(manyforge_headless_test_manager_apply_package
      DEPENDS "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA}")

    set(
      MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/assistant-native-test-fixture")
    set(
      MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}/retained-package")
    set(
      MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_METADATA
      "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}/metadata.json")
    add_custom_command(
      OUTPUT "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_METADATA}"
      COMMAND
        "${CMAKE_COMMAND}" -E remove_directory
        "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:manyforge_generate_headless_manager_apply_test_package>"
        --package-root
        "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_PACKAGE_ROOT}"
        --metadata
        "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_METADATA}"
        --profile assistant-native
      DEPENDS
        manyforge_generate_headless_manager_apply_test_package
        "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}"
        "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER}"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.urdf"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.srdf"
        "${MANYFORGE_HEADLESS_TEST_ASSISTANT_INSTRUMENT_SOURCE}"
        ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(manyforge_headless_test_assistant_native_package
      DEPENDS "${MANYFORGE_HEADLESS_TEST_ASSISTANT_NATIVE_METADATA}")

    set(MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/gripper-confirmation-test-fixture")
    add_custom_command(
      OUTPUT "${MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT}/metadata.json"
      COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "${MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT}"
      COMMAND "$<TARGET_FILE:manyforge_generate_headless_manager_apply_test_package>"
        --package-root
        "${MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT}/retained-package"
        --metadata
        "${MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT}/metadata.json"
        --profile operator-confirmation
      DEPENDS
        manyforge_generate_headless_manager_apply_test_package
        "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}"
        "${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER}"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.urdf"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.srdf"
        ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(manyforge_headless_test_gripper_confirmation_package
      DEPENDS "${MANYFORGE_HEADLESS_TEST_GRIPPER_CONFIRMATION_FIXTURE_ROOT}/metadata.json")

    # O1.4g uses a compatible pick/place host to materialize a second package.
    # Its semantic component declaration is admitted independently from the
    # primary host; no executable-proof bundle is involved.
    set(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/manager-apply-o1-4g-replacement-fixture")
    set(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}/retained-package")
    set(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA
      "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}/metadata.json")
    add_custom_command(
      OUTPUT "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA}"
      COMMAND
        "${CMAKE_COMMAND}" -E remove_directory
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:manyforge_generate_headless_manager_apply_test_package>"
        --package-root
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT}"
        --metadata
        "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA}"
      DEPENDS
        manyforge_generate_headless_manager_apply_test_package
        "${MANYFORGE_HEADLESS_TEST_NATIVE_REPLACEMENT_EXECUTION_HOST}"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.urdf"
        "${MANYFORGE_HEADLESS_TEST_LITE6_FIXTURE_ROOT}/model.srdf"
        ${MANYFORGE_HEADLESS_TEST_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(manyforge_headless_test_manager_apply_replacement_package
      DEPENDS "${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA}")
  endif()

  add_executable(manyforge_publish_headless_assistant_test_source
    test/publish_assistant_authoring_source.cpp)
  add_dependencies(manyforge_publish_headless_assistant_test_source
    manyforge_headless_test_assistant_package)
  target_compile_definitions(
    manyforge_publish_headless_assistant_test_source PRIVATE
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE="${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}")
  target_link_libraries(
    manyforge_publish_headless_assistant_test_source PRIVATE
    manyforge_headless_owner_publication)

  add_executable(test_headless_api_core test/test_headless_api_core.cpp)
  add_dependencies(test_headless_api_core
    manyforge_headless_test_canonical_empty_package
    manyforge_headless_test_assistant_package)
  target_compile_definitions(test_headless_api_core PRIVATE
    MANYFORGE_HEADLESS_TEST_ALARM_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/resources/operator-alarm-catalog.v1.yaml"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_NODE_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_catalog.yaml"
    MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_behavior/resources/node_environment_manifest.yaml")
  target_link_libraries(test_headless_api_core PRIVATE
    manyforge_event_historian
    manyforge::deployment
    manyforge::deployment_admission
    GTest::gtest_main)
  gtest_discover_tests(test_headless_api_core
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_program_apply_owner
    test/test_program_apply_owner.cpp)
  add_dependencies(test_program_apply_owner
    manyforge_headless_test_assistant_package)
  target_compile_definitions(test_program_apply_owner PRIVATE
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}")
  target_include_directories(test_program_apply_owner PRIVATE
    "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
  target_link_libraries(test_program_apply_owner PRIVATE
    manyforge_event_historian
    manyforge::deployment
    GTest::gtest_main)
  gtest_discover_tests(test_program_apply_owner
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})

  add_executable(test_owner_publication test/test_owner_publication.cpp)
  add_dependencies(test_owner_publication
    manyforge_headless_test_canonical_empty_package)
  target_compile_definitions(test_owner_publication PRIVATE
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
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
  target_compile_definitions(test_source_model_projection PRIVATE
    MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE="${MANYFORGE_HEADLESS_TEST_ASSISTANT_BUNDLE_ID_FILE}"
    MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT}"
    MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT}")
  target_link_libraries(test_source_model_projection PRIVATE
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
  if(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST)
    add_dependencies(test_manager_daemon_process
      manyforge_headless_test_manager_apply_package
      manyforge_headless_test_assistant_native_package
      manyforge_headless_test_gripper_confirmation_package)
    target_compile_definitions(test_manager_daemon_process PRIVATE
      MANYFORGE_HEADLESS_TEST_PYTHON="${Python3_EXECUTABLE}"
      MANYFORGE_HEADLESS_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
      MANYFORGE_HEADLESS_TEST_PYTHON_MODULE_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA}"
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT="${MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT}"
      MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST="${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST}"
      MANYFORGE_HEADLESS_TEST_NATIVE_REPLACEMENT_EXECUTION_HOST="${MANYFORGE_HEADLESS_TEST_NATIVE_REPLACEMENT_EXECUTION_HOST}"
      MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER="${MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER}")
    add_dependencies(test_manager_daemon_process
      manyforge_headless_test_manager_apply_replacement_package)
  endif()
  target_link_libraries(test_manager_daemon_process PRIVATE
    manyforge_headless_owner_publication
    GTest::gtest_main)
  gtest_discover_tests(test_manager_daemon_process
    PROPERTIES TIMEOUT ${MANYFORGE_HEADLESS_GTEST_TIMEOUT_SECONDS})
endif()
