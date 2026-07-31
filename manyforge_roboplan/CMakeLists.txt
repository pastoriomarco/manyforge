cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenSSL REQUIRED)
option(BUILD_TESTING "Build unit tests" ON)

message(STATUS
  "manyforge_roboplan destructive cutover: native, ROS, Python, mutable "
  "scene-projection, dynamic-object collision, and execution surfaces are "
  "unavailable until their composable-cell target adapters are restored")

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

# Tests
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Direct retained-product qualification imports the model through the
    # active RoboPlan/Pinocchio installation. Production staging itself has no
    # planner dependency.
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

    set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
        "Per-test timeout for manyforge_roboplan gtest cases")

    add_executable(test_retained_planning_model_stage
      test/test_retained_planning_model_stage.cpp)
    target_link_libraries(test_retained_planning_model_stage PRIVATE
      manyforge_roboplan_retained_consumer GTest::gtest_main)
    gtest_discover_tests(test_retained_planning_model_stage
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

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
        "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment"
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
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_example_models/models/lite6_lite_gripper")
    set(_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_structural_consumer_fixture")
    set(_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/tools/prepare_structural_model.py")
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
      ../manyforge_deployment/test/generate_lite6_retained_consumer_fixture.cpp)
    target_compile_definitions(
      generate_lite6_retained_consumer_fixture PRIVATE
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
    target_include_directories(
      generate_lite6_retained_consumer_fixture PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/test")
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
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/test")
    target_link_libraries(test_retained_product_consumer PRIVATE
      manyforge_roboplan_retained_consumer
      manyforge_deployment
      manyforge_deployment_admission
      roboplan::roboplan
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

    gtest_discover_tests(test_retained_product_consumer
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
      DISCOVERY_EXTRA_ARGS --gtest_filter=*)
    add_test(
      NAME retained_compiled_visual_fk_composer_consumer
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_composer/tests/qualify_retained_compiled_visual_fk.py"
        --fixture-root
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}")
    set_tests_properties(
      retained_compiled_visual_fk_composer_consumer
      PROPERTIES
        ENVIRONMENT
          "PYTHONOPTIMIZE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/..;LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    add_test(
      NAME retained_compiled_model_independent_standard_parser
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_deployment/test/qualify_retained_model_standard_parsers.py"
        --fixture-root
        "${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
        --urdfdom-checker
        "${_MANYFORGE_URDFDOM_CHECK_URDF_EXECUTABLE}")
    set_tests_properties(
      retained_compiled_model_independent_standard_parser
      PROPERTIES
        ENVIRONMENT
          "PYTHONOPTIMIZE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/..;LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

  endif()
endif()
