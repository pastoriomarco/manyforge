cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-deprecated-copy)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenSSL REQUIRED)
find_package(EXPAT REQUIRED)
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
find_package(roboplan_core REQUIRED)
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

add_library(manyforge_roboplan_scene_factory
  src/roboplan_scene_factory.cpp)
target_include_directories(manyforge_roboplan_scene_factory PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_scene_factory PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_scene_factory PUBLIC
  roboplan::roboplan)

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


# One synthetic model per canonical output channel, subordinate to the existing
# command owner. This leaf has no ROS, input ingress, timer or process surface.
add_library(manyforge_mock_boolean_output_provider
  src/mock_boolean_output_provider.cpp)
target_include_directories(manyforge_mock_boolean_output_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_mock_boolean_output_provider
  PUBLIC manyforge_core PRIVATE Threads::Threads)
# Event-driven mock input model; uses only the shared Core ingress/time ports.
add_library(manyforge_mock_boolean_input_provider
  src/mock_boolean_input_provider.cpp)
target_include_directories(manyforge_mock_boolean_input_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_mock_boolean_input_provider PUBLIC manyforge_core)

add_library(manyforge_geometric_contact_input_provider
  src/geometric_contact_input_provider.cpp)
target_include_directories(manyforge_geometric_contact_input_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_geometric_contact_input_provider PUBLIC
  manyforge_core PRIVATE Threads::Threads)

add_library(manyforge_roboplan_native_runtime_support
  src/native_runtime_support.cpp
  src/host_component_stream_health.cpp
  src/retained_externally_managed_lifecycle_ports.cpp)
target_include_directories(manyforge_roboplan_native_runtime_support PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_native_runtime_support
  PUBLIC manyforge_core
  PRIVATE Threads::Threads)

# Q69: ROS-free controller robot_description generation (model URDF in,
# URDF text out). Expat is the XML parser Deployment already links into every
# host process; no other XML library is introduced.
add_library(manyforge_roboplan_controller_robot_description
  src/controller_robot_description.cpp)
target_include_directories(manyforge_roboplan_controller_robot_description PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_controller_robot_description
  PUBLIC manyforge_core
  PRIVATE EXPAT::EXPAT)

# Q63-Q74: Track 2's controller templates and the retained Isaac controller
# files are build inputs of the host's controller launch artifacts, embedded so
# the generated files never depend on a runtime package lookup.
set(_MANYFORGE_CONTROLLER_TEMPLATE_ROOT
  "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_ros_bringup/config")
set(_MANYFORGE_CONTROLLER_TEMPLATE_INCLUDES "")
foreach(_entry
    "kEmbeddedMockUFactoryControllersTemplate=mock_ufactory_controllers.template.yaml"
    "kEmbeddedMockUrControllersTemplate=mock_ur_controllers.template.yaml"
    "kEmbeddedMockGripperControllerTemplate=mock_gripper_controller.template.yaml"
    "kEmbeddedIsaacLite6ControllersYaml=isaac_lite6_topic_based_controllers.yaml"
    "kEmbeddedIsaacUr10eControllersYaml=isaac_ur10e_topic_based_controllers.yaml")
  string(REPLACE "=" ";" _pair "${_entry}")
  list(GET _pair 0 MANYFORGE_EMBEDDED_CONTROLLER_SYMBOL)
  list(GET _pair 1 _file)
  set(MANYFORGE_EMBEDDED_CONTROLLER_SOURCE
    "manyforge_ros_bringup/config/${_file}")
  set(_path "${_MANYFORGE_CONTROLLER_TEMPLATE_ROOT}/${_file}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
  file(READ "${_path}" MANYFORGE_EMBEDDED_CONTROLLER_TEXT)
  string(FIND "${MANYFORGE_EMBEDDED_CONTROLLER_TEXT}" ")mf_ctl_tpl\""
    _collision)
  if(NOT _collision EQUAL -1)
    message(FATAL_ERROR "${_file} collides with its embedded raw-string delimiter")
  endif()
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_controller_template.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/controller_templates/${MANYFORGE_EMBEDDED_CONTROLLER_SYMBOL}.hpp"
    @ONLY)
  string(APPEND _MANYFORGE_CONTROLLER_TEMPLATE_INCLUDES
    "#include \"controller_templates/${MANYFORGE_EMBEDDED_CONTROLLER_SYMBOL}.hpp\"\n")
endforeach()
file(WRITE
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_controller_templates.hpp.tmp"
  "#pragma once\n${_MANYFORGE_CONTROLLER_TEMPLATE_INCLUDES}")
configure_file(
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_controller_templates.hpp.tmp"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_controller_templates.hpp"
  COPYONLY)
unset(_MANYFORGE_CONTROLLER_TEMPLATE_INCLUDES)

add_library(manyforge_roboplan_controller_launch_artifacts
  src/controller_launch_artifacts.cpp)
target_include_directories(manyforge_roboplan_controller_launch_artifacts
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manyforge_roboplan_controller_launch_artifacts
  PUBLIC manyforge_roboplan_controller_robot_description
  PRIVATE manyforge::content_identity)

# Q78/§7.5: command and process ownership of the controller launches the
# execution host starts per component (ROS-free).
add_library(manyforge_roboplan_controller_launch_process
  src/controller_launch_process.cpp
  src/pose_source_launch.cpp
  src/simulated_plc_launch.cpp)
target_include_directories(manyforge_roboplan_controller_launch_process
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_controller_launch_process
  PUBLIC manyforge_roboplan_controller_launch_artifacts manyforge::content_identity
  PRIVATE Threads::Threads)

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


# The safety/fidelity algorithms were retained and qualified before the native
# owner returned. I6 promotes the same implementations into the production
# planning boundary; no duplicate test-only copy remains.
add_library(manyforge_roboplan_planning_contracts STATIC
  src/cartesian_planning_contract.cpp
  src/roboplan_cartesian_trajectory_dynamics.cpp)
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
  src/retained_native_gripper_runtime.cpp
  src/retained_native_gripper_route_publication_owner.cpp
  src/retained_native_runtime.cpp
  src/retained_native_cell_runtime.cpp
  src/retained_stop_surveillance_owner.cpp)
target_include_directories(manyforge_roboplan_native_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_native_runtime PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_native_runtime PUBLIC
  manyforge_roboplan_native_state_projection
  manyforge_roboplan_native_runtime_support
  manyforge_deployment
  manyforge_roboplan_native_focused_adapters
  manyforge_roboplan_planner_backend
  manyforge_roboplan_collision_projection
  manyforge_collision
  manyforge_core
  manyforge_core_sqlite_operation_journal
  manyforge::deployment_resolved_stop_base_plan_resolver
  PRIVATE manyforge::content_identity)

# Scalar routes use the same cell/root and measured-source implementation.
add_library(manyforge_roboplan_native_gripper ALIAS
  manyforge_roboplan_native_runtime)

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
  # Transport-only I/O adapter: deliberately no RoboPlan/planner dependency.
  find_package(rclcpp REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(std_srvs REQUIRED)
  find_package(ur_msgs REQUIRED)
  find_package(control_msgs REQUIRED)
  add_library(manyforge_ros_boolean_output_provider
    src/ros_boolean_output_provider.cpp)
  target_include_directories(manyforge_ros_boolean_output_provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_ros_boolean_output_provider PUBLIC
    manyforge_core rclcpp::rclcpp ${std_srvs_TARGETS}
    ${ur_msgs_TARGETS} ${control_msgs_TARGETS}
    PRIVATE Threads::Threads)
  add_library(manyforge_ros_boolean_input_provider
    src/ros_boolean_input_provider.cpp
    src/ros_mapped_boolean_input_provider.cpp)
  target_include_directories(manyforge_ros_boolean_input_provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_ros_boolean_input_provider PUBLIC
    manyforge_core rclcpp::rclcpp ${std_msgs_TARGETS}
    ${ur_msgs_TARGETS} ${control_msgs_TARGETS})

  find_package(pal_statistics_msgs REQUIRED)
  find_package(diagnostic_msgs REQUIRED)
  add_library(manyforge_roboplan_ros2_control_runtime
    src/component_stream_sample_bridge.cpp
    src/io_provider_lifecycle_product.cpp
    src/simulated_plc_readiness.cpp
    src/retained_isaac_ros2_control_profile.cpp
    src/retained_ros2_control_backend_factory.cpp
    src/retained_ros2_control_lifecycle_product.cpp
    src/retained_ros2_control_runtime_handle.cpp
    src/retained_ros2_control_statistics_monitor.cpp)
  target_include_directories(manyforge_roboplan_ros2_control_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_roboplan_ros2_control_runtime PUBLIC
    manyforge_geometric_contact_input_provider
    manyforge_mock_boolean_input_provider
    manyforge_ros_boolean_input_provider
    manyforge_mock_boolean_output_provider
    manyforge_ros_boolean_output_provider
    manyforge_roboplan_native_runtime
    manyforge_roboplan_controller_robot_description
    manyforge_roboplan_controller_launch_artifacts
    manyforge_roboplan_controller_launch_process
    manyforge_ros2_control_execution
    ${pal_statistics_msgs_TARGETS}
    ${diagnostic_msgs_TARGETS}
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
    manyforge_collision
    manyforge_roboplan_worker_protocol
    Threads::Threads)

add_executable(manyforge_roboplan_candidate_worker
  src/roboplan_candidate_worker_main.cpp)
target_include_directories(manyforge_roboplan_candidate_worker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(manyforge_roboplan_candidate_worker PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_candidate_worker PRIVATE
  manyforge_collision
  manyforge_roboplan_worker_protocol
  manyforge_roboplan_native_candidate
  manyforge_roboplan_scene_factory)

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

add_library(manyforge_roboplan_cell_admission_command_codec
  src/cell_admission_command_codec.cpp
  src/owner_settlement_readers.cpp)
target_include_directories(manyforge_roboplan_cell_admission_command_codec
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(manyforge_roboplan_cell_admission_command_codec
  PUBLIC manyforge_component_manager manyforge::content_identity
  PRIVATE SQLite::SQLite3)

install(TARGETS manyforge_roboplan_candidate_worker RUNTIME DESTINATION bin)

if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
  add_executable(manyforge_ros2_control_execution_host
    src/product_backed_runtime_participant_resolver.cpp
    src/sealed_program_execution_capture.cpp
    src/managed_projection_registry.cpp
  src/execution_host_posix.cpp
  src/execution_host_commands.cpp
  src/execution_host_responses.cpp
  src/execution_host_scene_bridge.cpp
  src/execution_host_surface.cpp
  src/applied_program_owner_reader.cpp
  src/applied_program_blackboard_installation.cpp
  src/applied_global_io_rule_installation.cpp
  src/selected_program_products.cpp
  src/host_io_output_channels.cpp
  src/host_io_provider_installation.cpp
  src/ros_host_composition.cpp
  src/ros_managed_runtime_host.cpp
  src/ros_execution_host_main.cpp)
  target_include_directories(manyforge_ros2_control_execution_host PRIVATE
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
  target_compile_definitions(manyforge_ros2_control_execution_host PRIVATE
    COAL_DISABLE_HPP_FCL_WARNINGS
    MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=1)
  target_link_libraries(manyforge_ros2_control_execution_host PRIVATE
    manyforge_roboplan_managed_kernel_apply_fence
    manyforge_deployment_program_io_condition_compiler
    manyforge_roboplan_cell_admission_command_codec
    manyforge_roboplan_native_cclc_runtime
    manyforge_roboplan_ros2_control_runtime
    manyforge_roboplan_process_candidate_client
    manyforge_roboplan_retained_consumer
    manyforge_roboplan_scene_factory
    manyforge_component_manager
    manyforge::content_identity
    manyforge::scene
    manyforge::scene_value_codec
    manyforge_deployment
    manyforge::persistence
    SQLite::SQLite3
    Threads::Threads)
  install(TARGETS manyforge_ros2_control_execution_host
    RUNTIME DESTINATION bin)
  message(STATUS
    "manyforge_roboplan retained runtime: ros2_control execution is selected")
else()
  message(STATUS
    "manyforge_roboplan retained runtime: no execution host is selected in this build")
endif()

# Tests
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    find_package(roboplan_example_models REQUIRED)

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    # Test-only variant of the exact ROS transport host. It joins the real
    # addressed C++ pick/place planning owner to deterministic subordinate
    # ports so the Python/headless/Behavior/Composer process path can be
    # qualified without introducing a second production scene or runtime
    # owner. The installed host above never compiles this fixture.
    add_executable(manyforge_ros2_control_execution_host_pick_place_planning_fixture
      src/product_backed_runtime_participant_resolver.cpp
      src/sealed_program_execution_capture.cpp
      src/managed_projection_registry.cpp
      src/execution_host_posix.cpp
      src/execution_host_commands.cpp
      src/execution_host_responses.cpp
      src/execution_host_scene_bridge.cpp
      src/execution_host_surface.cpp
      src/applied_program_owner_reader.cpp
  src/applied_program_blackboard_installation.cpp
  src/applied_global_io_rule_installation.cpp
  src/selected_program_products.cpp
  src/host_io_output_channels.cpp
  src/host_io_provider_installation.cpp
  src/ros_host_composition.cpp
  src/ros_managed_runtime_host.cpp
      src/ros_execution_host_main.cpp
      test/focused_pick_place_planning_fixture.cpp)
    target_include_directories(
      manyforge_ros2_control_execution_host_pick_place_planning_fixture PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_compile_definitions(
      manyforge_ros2_control_execution_host_pick_place_planning_fixture PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS
      MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE=1
      MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=1
      MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE=1)
    target_link_libraries(
      manyforge_ros2_control_execution_host_pick_place_planning_fixture PRIVATE
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_deployment_program_io_condition_compiler
      manyforge_roboplan_cell_admission_command_codec
      manyforge_roboplan_native_cclc_runtime
      manyforge_roboplan_ros2_control_runtime
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_scene_factory
      manyforge_component_manager
      manyforge::content_identity
      manyforge::scene
      manyforge::scene_value_codec
      manyforge_deployment
      manyforge::persistence
      SQLite::SQLite3
      Threads::Threads)

    # Compile-time-only standalone admission fixture. The installed native
    # host remains fail-closed without Manager liveness; this target supplies
    # deterministic Idle facts solely to exercise real CCLC claim mechanics.
    add_executable(manyforge_ros2_control_execution_host_admission_fixture
      src/product_backed_runtime_participant_resolver.cpp
      src/sealed_program_execution_capture.cpp
      src/managed_projection_registry.cpp
  src/execution_host_posix.cpp
  src/execution_host_commands.cpp
  src/execution_host_responses.cpp
  src/execution_host_scene_bridge.cpp
  src/execution_host_surface.cpp
  src/applied_program_owner_reader.cpp
  src/applied_program_blackboard_installation.cpp
  src/applied_global_io_rule_installation.cpp
  src/selected_program_products.cpp
  src/host_io_output_channels.cpp
  src/host_io_provider_installation.cpp
  src/ros_host_composition.cpp
  src/ros_managed_runtime_host.cpp
  src/ros_execution_host_main.cpp)
    target_include_directories(
      manyforge_ros2_control_execution_host_admission_fixture PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
    target_compile_definitions(
      manyforge_ros2_control_execution_host_admission_fixture PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS
      MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE=1
      MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=1)
    target_link_libraries(
      manyforge_ros2_control_execution_host_admission_fixture PRIVATE
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_deployment_program_io_condition_compiler
      manyforge_roboplan_cell_admission_command_codec
      manyforge_roboplan_native_cclc_runtime
      manyforge_roboplan_ros2_control_runtime
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_scene_factory
      manyforge_component_manager
      manyforge::content_identity
      manyforge::scene
      manyforge::scene_value_codec
      manyforge_deployment
      manyforge::persistence
      SQLite::SQLite3
      Threads::Threads)

    # Test-only build of the native host at the real projection-construction
    # boundary. The projection source is compiled directly into this one
    # executable so its barrier/failure definition can never reach either
    # installed host or the ordinary collision-projection library. Its product
    # output name lives in a dedicated directory consumed only by the focused
    # native adapter cases that request injection.
    add_executable(manyforge_ros2_control_execution_host_projection_fixture
      src/product_backed_runtime_participant_resolver.cpp
      src/sealed_program_execution_capture.cpp
      src/managed_projection_registry.cpp
      src/execution_host_posix.cpp
      src/execution_host_commands.cpp
      src/execution_host_responses.cpp
      src/execution_host_scene_bridge.cpp
      src/execution_host_surface.cpp
      src/applied_program_owner_reader.cpp
  src/applied_program_blackboard_installation.cpp
  src/applied_global_io_rule_installation.cpp
  src/selected_program_products.cpp
  src/host_io_output_channels.cpp
  src/host_io_provider_installation.cpp
  src/ros_host_composition.cpp
  src/ros_managed_runtime_host.cpp
      src/ros_execution_host_main.cpp
      src/retained_planner_scene_projection.cpp)
    set_target_properties(
      manyforge_ros2_control_execution_host_projection_fixture PROPERTIES
      OUTPUT_NAME manyforge_ros2_control_execution_host
      RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/ros_projection_host_fixture")
    target_include_directories(
      manyforge_ros2_control_execution_host_projection_fixture PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
    target_compile_definitions(
      manyforge_ros2_control_execution_host_projection_fixture PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS
      MANYFORGE_ENABLE_NATIVE_PROJECTION_CONSTRUCTION_TEST_BOUNDARY=1
      MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=1)
    target_link_libraries(
      manyforge_ros2_control_execution_host_projection_fixture PRIVATE
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_deployment_program_io_condition_compiler
      manyforge_roboplan_cell_admission_command_codec
      manyforge_roboplan_native_cclc_runtime
      manyforge_roboplan_ros2_control_runtime
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_scene_factory
      manyforge_component_manager
      manyforge::content_identity
      manyforge::scene
      manyforge::scene_value_codec
      manyforge_deployment
      manyforge::persistence
      SQLite::SQLite3
      Threads::Threads)
    add_dependencies(manyforge_ros2_control_execution_host_projection_fixture
      manyforge_roboplan_candidate_worker)
    add_custom_command(
      TARGET manyforge_ros2_control_execution_host_projection_fixture POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:manyforge_roboplan_candidate_worker>"
        "$<TARGET_FILE_DIR:manyforge_ros2_control_execution_host_projection_fixture>/$<TARGET_FILE_NAME:manyforge_roboplan_candidate_worker>"
      VERBATIM)

    endif() # ROS execution host fixture variants

    if(NOT TARGET manyforge_collision)
      message(FATAL_ERROR
        "I6 retained collision projection requires the authoritative "
        "collision consumer target")
    endif()

    set(MANYFORGE_GTEST_TIMEOUT_SECONDS "60" CACHE STRING
        "Per-test timeout for manyforge_roboplan gtest cases")


    add_executable(test_mock_boolean_output_provider
      test/test_mock_boolean_output_provider.cpp)
    target_link_libraries(test_mock_boolean_output_provider PRIVATE
      manyforge_mock_boolean_output_provider GTest::gtest_main)
    add_executable(test_mock_boolean_input_provider
      test/test_mock_boolean_input_provider.cpp)
    target_link_libraries(test_mock_boolean_input_provider PRIVATE
      manyforge_mock_boolean_input_provider GTest::gtest_main)
    gtest_discover_tests(test_mock_boolean_input_provider
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

    add_executable(test_geometric_contact_input_provider
      test/test_geometric_contact_input_provider.cpp)
    target_link_libraries(test_geometric_contact_input_provider PRIVATE
      manyforge_geometric_contact_input_provider GTest::gtest_main)
    gtest_discover_tests(test_geometric_contact_input_provider
      PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

    add_executable(test_native_runtime_support
      test/test_native_runtime_support.cpp)
    target_link_libraries(test_native_runtime_support PRIVATE
      manyforge_roboplan_native_runtime_support GTest::gtest_main)

    add_executable(test_controller_robot_description
      test/test_controller_robot_description.cpp)
    target_compile_definitions(test_controller_robot_description PRIVATE
      MANYFORGE_TEST_REPOSITORY_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}")
    target_link_libraries(test_controller_robot_description PRIVATE
      manyforge_roboplan_controller_robot_description EXPAT::EXPAT
      GTest::gtest_main)

    add_executable(test_controller_launch_artifacts
      test/test_controller_launch_artifacts.cpp)
    target_compile_definitions(test_controller_launch_artifacts PRIVATE
      MANYFORGE_TEST_REPOSITORY_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}")
    target_link_libraries(test_controller_launch_artifacts PRIVATE
      manyforge_roboplan_controller_launch_artifacts
      manyforge_core manyforge::content_identity GTest::gtest_main)

    add_executable(test_controller_launch_process
      test/test_controller_launch_process.cpp
      test/test_pose_source_launch.cpp)
    target_compile_definitions(test_controller_launch_process PRIVATE
      MANYFORGE_TEST_REPOSITORY_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}")
    target_link_libraries(test_controller_launch_process PRIVATE
      manyforge_roboplan_controller_launch_process
      manyforge_roboplan_native_runtime_support
      manyforge_core manyforge::content_identity GTest::gtest_main)

    add_executable(test_cell_admission_command_codec
      test/test_cell_admission_command_codec.cpp)
    target_link_libraries(test_cell_admission_command_codec PRIVATE
      manyforge_roboplan_cell_admission_command_codec GTest::gtest_main)

    add_executable(test_owner_settlement_readers
      test/test_owner_settlement_readers.cpp)
    target_link_libraries(test_owner_settlement_readers PRIVATE
      manyforge_roboplan_cell_admission_command_codec GTest::gtest_main
      SQLite::SQLite3)

    # The projection registry compiled alone, behind an injected asset source
    # and clock; the installed hosts compile the same source unit.
    add_executable(test_managed_projection_registry
      src/managed_projection_registry.cpp
      test/test_managed_projection_registry.cpp)
    target_include_directories(test_managed_projection_registry PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/src")
    target_compile_definitions(test_managed_projection_registry PRIVATE
      COAL_DISABLE_HPP_FCL_WARNINGS)
    target_link_libraries(test_managed_projection_registry PRIVATE
      manyforge_roboplan_collision_projection
      manyforge_roboplan_retained_consumer
      manyforge_component_manager
      manyforge_deployment
      GTest::gtest_main
      Threads::Threads)

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
      manyforge_roboplan_native_gripper
      manyforge_roboplan_native_runtime
      GTest::gtest_main)

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      add_executable(test_ros_boolean_output_provider
        test/test_ros_boolean_output_provider.cpp)
      target_link_libraries(test_ros_boolean_output_provider PRIVATE
        manyforge_ros_boolean_output_provider GTest::gtest_main)

      add_executable(test_ros_boolean_input_provider
        test/test_ros_boolean_input_provider.cpp)
      target_link_libraries(test_ros_boolean_input_provider PRIVATE
        manyforge_ros_boolean_input_provider GTest::gtest_main)
      add_executable(test_ros_mapped_boolean_input_provider
        test/test_ros_mapped_boolean_input_provider.cpp)
      target_link_libraries(test_ros_mapped_boolean_input_provider PRIVATE
        manyforge_ros_boolean_input_provider GTest::gtest_main)

      add_executable(test_retained_ros2_control_backend_factory
        test/test_retained_ros2_control_backend_factory.cpp)
      target_include_directories(test_retained_ros2_control_backend_factory PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(test_retained_ros2_control_backend_factory PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_component_manager
        GTest::gtest_main)
      gtest_discover_tests(test_retained_ros2_control_backend_factory)

      # Runtime handle, CCLC controller switching and binding epochs against
      # in-process ROS stand-ins.
      add_executable(test_retained_ros2_control_runtime_handle
        test/test_io_provider_lifecycle_product.cpp
        test/test_simulated_plc_readiness.cpp
        test/test_retained_ros2_control_runtime_handle.cpp
        test/test_retained_ros2_control_lifecycle_product.cpp
        test/test_retained_native_lifecycle_product.cpp)
      target_include_directories(test_retained_ros2_control_runtime_handle PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
      target_link_libraries(test_retained_ros2_control_runtime_handle PRIVATE
        manyforge_component_manager
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_native_runtime_support
        manyforge_deployment_canonical_package_generator
        GTest::gtest_main)
      gtest_discover_tests(test_retained_ros2_control_runtime_handle
        PROPERTIES TIMEOUT 120)

      # Real addressed Kernel + the Core participant over the real JTC adapter
      # against an in-process trajectory controller (requires a ROS 2 runtime).
      add_executable(test_ros2_control_kernel_stop_integration
        test/test_ros2_control_kernel_stop_integration.cpp)
      target_link_libraries(test_ros2_control_kernel_stop_integration PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_native_runtime_support
        manyforge_core_sqlite_operation_journal
        GTest::gtest_main)
      gtest_discover_tests(test_ros2_control_kernel_stop_integration
        PROPERTIES TIMEOUT 120)
    endif()

    add_executable(test_retained_stop_surveillance_owner
      test/test_retained_stop_surveillance_owner.cpp)
    target_include_directories(test_retained_stop_surveillance_owner PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_link_libraries(test_retained_stop_surveillance_owner PRIVATE
      manyforge_roboplan_native_runtime GTest::gtest_main)

    add_executable(test_retained_native_cell_runtime
      test/test_retained_native_cell_runtime.cpp)
    target_link_libraries(test_retained_native_cell_runtime PRIVATE
      manyforge_roboplan_native_runtime GTest::gtest_main)
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      target_link_libraries(test_retained_native_cell_runtime PRIVATE
        manyforge_roboplan_ros2_control_runtime)
      target_compile_definitions(test_retained_native_cell_runtime PRIVATE
        MANYFORGE_A1_ROS_GRIPPER_CELL_TEST=1)
    endif()

    add_executable(test_retained_native_controller_lifecycle_port
      test/test_retained_native_controller_lifecycle_port.cpp)
    target_link_libraries(test_retained_native_controller_lifecycle_port PRIVATE
      manyforge_roboplan_native_runtime_support GTest::gtest_main)

    # Exercise the existing Deployment facade test in this consumer graph too;
    # dependency packages deliberately have BUILD_TESTING disabled here.
    add_executable(test_retained_native_cclc_runtime
      src/product_backed_runtime_participant_resolver.cpp
      src/sealed_program_execution_capture.cpp
      test/test_retained_native_cclc_runtime.cpp
      test/test_retained_measured_robot_geometry.cpp
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test/test_cclc_runtime_composition.cpp")
    target_include_directories(test_retained_native_cclc_runtime PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
    get_target_property(_MANYFORGE_NATIVE_PROGRAM_CAPTURE_BINARY_DIR
      manyforge_maintained_deployment_occurrences BINARY_DIR)
    target_compile_definitions(test_retained_native_cclc_runtime PRIVATE
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${_MANYFORGE_NATIVE_PROGRAM_CAPTURE_BINARY_DIR}/program_capture_package")
    add_dependencies(test_retained_native_cclc_runtime
      manyforge_maintained_deployment_occurrences)
    target_link_libraries(test_retained_native_cclc_runtime PRIVATE
      manyforge_deployment_program_io_condition_compiler
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_roboplan_native_cclc_runtime
      manyforge_component_manager
      manyforge_deployment
      manyforge::deployment_admission
      manyforge_deployment_canonical_package_generator
      GTest::gtest_main)

    add_executable(test_native_stop_qualification_seams
      test/test_native_stop_qualification_seams.cpp)
    target_include_directories(test_native_stop_qualification_seams PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/test"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
    target_link_libraries(test_native_stop_qualification_seams PRIVATE
      manyforge_core
      manyforge_roboplan_native_state_projection
      GTest::gtest_main)

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      add_executable(test_host_io_signal_runtime
        test/test_host_io_signal_runtime.cpp)
      target_include_directories(test_host_io_signal_runtime PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_link_libraries(test_host_io_signal_runtime PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_core manyforge_deployment manyforge_component_manager
        manyforge_mock_boolean_input_provider manyforge_ros_boolean_input_provider
        GTest::gtest_main)
      gtest_discover_tests(test_host_io_signal_runtime
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_host_io_output_channels
        test/test_host_io_output_channels.cpp src/host_io_output_channels.cpp)
      target_include_directories(test_host_io_output_channels PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
      target_link_libraries(test_host_io_output_channels PRIVATE
        manyforge_core manyforge_mock_boolean_output_provider
        manyforge_ros_boolean_output_provider manyforge_deployment
        manyforge_deployment_canonical_package_generator GTest::gtest_main)
      gtest_discover_tests(test_host_io_output_channels
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_host_io_provider_installation
        test/test_host_io_provider_installation.cpp
        test/test_applied_program_blackboard_context.cpp
        test/test_applied_global_io_rule_installation.cpp
        test/test_global_rule_event_responses.cpp
        test/test_io_rule_result_responses.cpp
        test/test_global_rule_recovery_wire.cpp
        test/test_host_rule_evidence_wakeup.cpp
        test/test_io_liveness_responses.cpp
        test/test_program_blackboard_command_codec.cpp
        src/execution_host_responses.cpp
        src/ros_host_composition.cpp
        src/host_io_provider_installation.cpp
        src/host_io_output_channels.cpp
        src/applied_program_blackboard_installation.cpp
        src/applied_global_io_rule_installation.cpp
        src/selected_program_products.cpp
        src/applied_program_owner_reader.cpp
        src/product_backed_runtime_participant_resolver.cpp
        src/sealed_program_execution_capture.cpp
        src/execution_host_commands.cpp
        src/execution_host_posix.cpp)
      target_include_directories(test_host_io_provider_installation PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
      get_target_property(_MANYFORGE_PROGRAM_CAPTURE_BINARY_DIR
        manyforge_maintained_deployment_occurrences BINARY_DIR)
      target_compile_definitions(test_host_io_provider_installation PRIVATE
        MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${_MANYFORGE_PROGRAM_CAPTURE_BINARY_DIR}/program_capture_package")
      target_link_libraries(test_host_io_provider_installation PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_native_cclc_runtime
        manyforge_roboplan_controller_launch_process
        manyforge_roboplan_retained_consumer
        manyforge_roboplan_cell_admission_command_codec
        manyforge_deployment_program_io_condition_compiler
        manyforge_deployment_canonical_package_generator
        manyforge_deployment manyforge_component_manager manyforge_core
        manyforge::content_identity manyforge::scene_value_codec
        SQLite::SQLite3 Threads::Threads GTest::gtest_main)
      add_dependencies(test_host_io_provider_installation
        manyforge_maintained_deployment_occurrences)
      gtest_discover_tests(test_host_io_provider_installation
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
          FIXTURES_REQUIRED ProgramCapturePackage)
    endif()

    add_executable(test_managed_kernel_admission_gate
      test/test_managed_kernel_admission_gate.cpp)
    target_include_directories(test_managed_kernel_admission_gate PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
    target_link_libraries(test_managed_kernel_admission_gate PRIVATE
      SQLite::SQLite3
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge::core_sqlite_operation_journal
      manyforge::core_sqlite_cclc_journal
      manyforge_component_manager
      manyforge_deployment
      manyforge_deployment_canonical_package_generator
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
      manyforge_collision
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_scene_factory
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
      manyforge_roboplan_scene_factory
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
    add_dependencies(test_managed_kernel_admission_gate
      generate_canonical_empty_product_package)
    target_compile_definitions(test_managed_kernel_admission_gate PRIVATE
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT="${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}")

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
    set(_MANYFORGE_ROBOPLAN_UR5E_SOURCE_ROOT
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_example_models/models/ur5e_robotiq_2f_85")
    set(_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/tools/prepare_structural_model.py")
    get_filename_component(
      _MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL
      "${_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL}"
      REALPATH)
    function(_manyforge_prepare_roboplan_model_fixture
        document_root resource_root model_asset_id urdf_name srdf_name
        package_uri_prefix output_root output_resources)
      set(fixture_root
        "${CMAKE_CURRENT_BINARY_DIR}/${model_asset_id}_structural_consumer_fixture")
      file(GLOB_RECURSE resource_inputs
        CONFIGURE_DEPENDS LIST_DIRECTORIES false "${resource_root}/meshes/*")
      file(MAKE_DIRECTORY "${fixture_root}")
      execute_process(
        COMMAND "${Python3_EXECUTABLE}"
          "${_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL}"
          --urdf "${document_root}/${urdf_name}"
          --srdf "${document_root}/${srdf_name}"
          --model-asset-id "${model_asset_id}"
          --accepted-package-uri-prefix "${package_uri_prefix}"
          --resource-root "${resource_root}"
          --output-directory "${fixture_root}"
        RESULT_VARIABLE preparation_result
        OUTPUT_VARIABLE preparation_output
        ERROR_VARIABLE preparation_error)
      if(NOT preparation_result EQUAL 0)
        message(FATAL_ERROR
          "RoboPlan consumer fixture preparation failed for ${model_asset_id}:\n"
          "${preparation_output}${preparation_error}")
      endif()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_MANYFORGE_ROBOPLAN_STRUCTURAL_PREPARATION_TOOL}"
        "${document_root}/${urdf_name}" "${document_root}/${srdf_name}"
        ${resource_inputs})
      set(${output_root} "${fixture_root}" PARENT_SCOPE)
      set(${output_resources} "${resource_inputs}" PARENT_SCOPE)
    endfunction()
    _manyforge_prepare_roboplan_model_fixture(
      "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
      "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
      lite6_lite_gripper lite6_lite_gripper.urdf lite6_lite_gripper.srdf
      "package://manyforge_example_models/models/lite6_lite_gripper/"
      _MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT
      _MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS)

    set(_MANYFORGE_ROBOPLAN_IMPORT_VIEW_TOOL
      "${CMAKE_CURRENT_SOURCE_DIR}/test/prepare_roboplan_import_view.py")
    set(_MANYFORGE_ROBOPLAN_UR5E_IMPORT_VIEW_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/ur5e_robotiq_2f_85_roboplan_import_view")
    file(MAKE_DIRECTORY "${_MANYFORGE_ROBOPLAN_UR5E_IMPORT_VIEW_ROOT}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_behavior"
        "${Python3_EXECUTABLE}" "${_MANYFORGE_ROBOPLAN_IMPORT_VIEW_TOOL}"
        --model-root "${_MANYFORGE_ROBOPLAN_UR5E_SOURCE_ROOT}"
        --package-root "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
        --model-name ur5e_robotiq_2f_85
        --output-directory "${_MANYFORGE_ROBOPLAN_UR5E_IMPORT_VIEW_ROOT}"
      RESULT_VARIABLE import_view_result
      OUTPUT_VARIABLE import_view_output
      ERROR_VARIABLE import_view_error)
    if(NOT import_view_result EQUAL 0)
      message(FATAL_ERROR
        "RoboPlan UR5e import-view preparation failed:\n"
        "${import_view_output}${import_view_error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${_MANYFORGE_ROBOPLAN_IMPORT_VIEW_TOOL}"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_behavior/manyforge_behavior/robot_model_authority.py"
      "${_MANYFORGE_ROBOPLAN_UR5E_SOURCE_ROOT}/ur5e_robotiq_2f_85.urdf"
      "${_MANYFORGE_ROBOPLAN_UR5E_SOURCE_ROOT}/ur5e_robotiq_2f_85.srdf")
    _manyforge_prepare_roboplan_model_fixture(
      "${_MANYFORGE_ROBOPLAN_UR5E_IMPORT_VIEW_ROOT}"
      "${_MANYFORGE_ROBOPLAN_UR5E_SOURCE_ROOT}"
      ur5e_robotiq_2f_85 ur5e_robotiq_2f_85.urdf ur5e_robotiq_2f_85.srdf
      "package://manyforge_example_models/models/ur5e_robotiq_2f_85/"
      _MANYFORGE_ROBOPLAN_UR5E_FIXTURE_ROOT
      _MANYFORGE_ROBOPLAN_UR5E_RESOURCE_INPUTS)

    # Dedicated genuine UR5 arm-only model for generic absence/protocol coverage.
    set(_MANYFORGE_UR5_ARM_ONLY_SOURCE_ROOT
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/maintained/model_sources/ur5_roboplan")
    set(_MANYFORGE_UR5_ARM_ONLY_RESOURCE_ROOT
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/.docker/roboplan/roboplan_example_models/models/ur_robot_model")
    if(NOT EXISTS "${_MANYFORGE_UR5_ARM_ONLY_RESOURCE_ROOT}/meshes/visual/base.dae")
      find_package(roboplan_example_models CONFIG REQUIRED)
      get_filename_component(_MANYFORGE_UR5_ARM_ONLY_RESOURCE_ROOT
        "${roboplan_example_models_DIR}/../../../share/roboplan_example_models/models/ur_robot_model"
        REALPATH)
    endif()
    _manyforge_prepare_roboplan_model_fixture(
      "${_MANYFORGE_UR5_ARM_ONLY_SOURCE_ROOT}"
      "${_MANYFORGE_UR5_ARM_ONLY_RESOURCE_ROOT}"
      ur5_roboplan ur5_gripper.structural.urdf ur5_gripper.structural.srdf
      "package://roboplan_example_models/models/ur_robot_model/"
      _MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT
      _MANYFORGE_UR5_ARM_ONLY_RESOURCE_INPUTS)
    set(_MANYFORGE_UR5_ARM_ONLY_CATALOG_PATH
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/maintained/model_assets/ur5_roboplan.model_asset.json")

    # Standalone Position qualification uses the already-normalized Robotiq
    # subtree and the existing mesh resource root, with no arm coordinates.
    set(_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_SOURCE_ROOT
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/maintained/model_sources/robotiq_2f_85")
    set(_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_RESOURCE_ROOT "${_MANYFORGE_ROBOPLAN_UR5E_SOURCE_ROOT}")
    _manyforge_prepare_roboplan_model_fixture(
      "${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_SOURCE_ROOT}"
      "${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_RESOURCE_ROOT}"
      robotiq_2f_85 robotiq_2f_85.urdf robotiq_2f_85.srdf
      "package://manyforge_example_models/models/ur5e_robotiq_2f_85/"
      _MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
      _MANYFORGE_ROBOTIQ_GRIPPER_ONLY_RESOURCE_INPUTS)
    set(_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_CATALOG_PATH
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/maintained/model_assets/robotiq_2f_85.model_asset.json")

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
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT}"
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_SOURCE_ROOT}"
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT="${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_RESOURCE_ROOT}"
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH="${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_CATALOG_PATH}"
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT}"
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_UR5_ARM_ONLY_SOURCE_ROOT}"
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_RESOURCE_ROOT="${_MANYFORGE_UR5_ARM_ONLY_RESOURCE_ROOT}"
      MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_CATALOG_PATH="${_MANYFORGE_UR5_ARM_ONLY_CATALOG_PATH}"
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
        --fixture-kind application-ros
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
      MANYFORGE_I6_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    # L0.7 production-process fixture with complete arm+gripper lifecycle
    # catalogs, runnable Program products, and two-domain route configuration.
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
        --fixture-kind cclc-product-ros
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        manyforge_ros2_control_execution_host
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_cclc_product_fixture ALL
      DEPENDS "${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_MANIFEST}")

    endif()

    # M1 standalone Position source uses a genuine Robotiq-only retained model.
    # Historical pneumatic CLI selections remain separate R5 fixture inputs.
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    # Retain native-named output paths used by the owning process test.
    set(_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_gripper_only_fixture")
    set(_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_MANIFEST
      "${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
        --fixture-kind gripper-only-robotiq-position
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        manyforge_ros2_control_execution_host
        "${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT}/model.srdf"
        "${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_CATALOG_PATH}"
        ${_MANYFORGE_ROBOTIQ_GRIPPER_ONLY_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_gripper_only_fixture ALL
      DEPENDS "${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_MANIFEST}")

    endif()

    # Focused single-domain fixture for the CCLC repository-readiness test.
    # It exercises the ordinary retained product path without pulling the
    # application fixture's independent gripper domain into that unit test.
    set(_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_cclc_readiness_fixture")
    set(_MANYFORGE_CCLC_READINESS_FIXTURE_MANIFEST
      "${_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_CCLC_READINESS_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT}"
        --fixture-kind cclc-readiness-ros
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        "${CMAKE_CURRENT_SOURCE_DIR}/src/retained_ros2_control_lifecycle_plans_internal.hpp"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_cclc_readiness_fixture ALL
      DEPENDS "${_MANYFORGE_CCLC_READINESS_FIXTURE_MANIFEST}")
    add_dependencies(test_retained_native_cclc_runtime
      generate_canonical_empty_product_package
      generate_lite6_native_cclc_readiness_fixture
      generate_lite6_native_execution_fixture)
    target_compile_definitions(test_retained_native_cclc_runtime PRIVATE
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT="${_MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
      MANYFORGE_I6_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_CCLC_READINESS_FIXTURE_ROOT="${_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT}")
    if(TARGET test_retained_ros2_control_runtime_handle)
      target_compile_definitions(test_retained_ros2_control_runtime_handle PRIVATE
        MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
        MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
    endif()
    foreach(_MANYFORGE_IO_HELPER_TEST IN ITEMS
        test_host_io_output_channels test_host_io_provider_installation)
      if(TARGET ${_MANYFORGE_IO_HELPER_TEST})
        target_compile_definitions(${_MANYFORGE_IO_HELPER_TEST} PRIVATE
          MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
          MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
      endif()
    endforeach()

    set(_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/ur5_arm_only_ros_fixture")
    add_custom_command(
      OUTPUT "${_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT}/manifest.json"
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT}"
      COMMAND "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT}"
        --fixture-kind ur5-arm-only-ros
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test/ur5_arm_only_retention_fixture.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/retained_ros2_control_lifecycle_plans_internal.hpp"
        "${_MANYFORGE_UR5_ARM_ONLY_CATALOG_PATH}"
        "${_MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT}/model.srdf"
        "${_MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT}/preparation-manifest.json"
        ${_MANYFORGE_UR5_ARM_ONLY_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_ur5_arm_only_ros_fixture
      DEPENDS "${_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT}/manifest.json")

    # Same provider/controller catalog and host infrastructure as the ordinary
    # fixture, with a new graph/bundle for projection candidate staging.
    set(_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_projection_successor_fixture")
    set(_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_MANIFEST
      "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_MANIFEST}"
      COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}"
      COMMAND "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}"
        --fixture-kind projection-successor-ros
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}/model.srdf"
        ${_MANYFORGE_ROBOPLAN_LITE6_RESOURCE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_projection_successor_fixture ALL
      DEPENDS "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_MANIFEST}")
    add_dependencies(test_retained_native_cclc_runtime
      generate_lite6_native_projection_successor_fixture)
    target_compile_definitions(test_retained_native_cclc_runtime PRIVATE
      MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT="${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}")

    add_executable(test_retained_product_consumer
      src/product_backed_runtime_participant_resolver.cpp
      src/sealed_program_execution_capture.cpp
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
      MANYFORGE_UR5E_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_example_models/models/ur5e_robotiq_2f_85"
      MANYFORGE_UR5E_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_UR5E_FIXTURE_ROOT}"
      MANYFORGE_UR5E_ROBOPLAN_IMPORT_JOINT_MAP="${_MANYFORGE_ROBOPLAN_UR5E_IMPORT_VIEW_ROOT}/joint-map.tsv"
      MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT="${_MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT}"
      MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH="$<TARGET_FILE:manyforge_roboplan_candidate_worker>")
    target_include_directories(test_retained_product_consumer PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test")
    target_link_libraries(test_retained_product_consumer PRIVATE
      manyforge_deployment_program_io_condition_compiler
      manyforge_core_filesystem_resolved_stop_base_plan_repository
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_collision_projection
      manyforge_roboplan_native_state_projection
      manyforge_roboplan_native_runtime
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_scene_factory
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_collision
      manyforge_core
      manyforge_deployment
      manyforge_deployment_admission
      roboplan::roboplan
      GTest::gtest_main)

    if(TARGET manyforge_roboplan_ros2_control_runtime)
      target_sources(test_retained_product_consumer PRIVATE
        src/applied_global_io_rule_installation.cpp
        src/applied_program_blackboard_installation.cpp
        src/applied_program_owner_reader.cpp
        src/execution_host_commands.cpp
        src/execution_host_posix.cpp
        src/host_io_output_channels.cpp
        src/host_io_provider_installation.cpp
        src/ros_host_composition.cpp)
      target_link_libraries(test_retained_product_consumer PRIVATE
        manyforge_roboplan_cell_admission_command_codec
        manyforge_roboplan_native_cclc_runtime
        manyforge_roboplan_ros2_control_runtime
        manyforge::component_manager
        manyforge::core_sqlite_cclc_journal
        manyforge::deployment_component_lifecycle_current_snapshot_source
        manyforge_preparation_support)
      target_compile_definitions(test_retained_product_consumer PRIVATE
        MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY=1
        MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}")
    endif()

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    add_executable(test_retained_native_runtime_unit_adapter
      test/test_retained_native_runtime_unit_adapter.cpp)
    add_dependencies(test_retained_native_runtime_unit_adapter
      generate_ur5_arm_only_ros_fixture
      generate_lite6_native_execution_fixture
      generate_lite6_native_gripper_only_fixture
      generate_lite6_native_cclc_readiness_fixture
      generate_lite6_native_projection_successor_fixture
      manyforge_ros2_control_execution_host
      manyforge_ros2_control_execution_host_projection_fixture
      manyforge_ros2_control_execution_host_pick_place_planning_fixture
      manyforge_roboplan_candidate_worker)
    target_compile_definitions(
      test_retained_native_runtime_unit_adapter PRIVATE
      MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT="${_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT}"
      MANYFORGE_I6_NATIVE_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT="${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
      MANYFORGE_CCLC_READINESS_FIXTURE_ROOT="${_MANYFORGE_CCLC_READINESS_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT="${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_PICK_PLACE_PLANNING_TARGET="$<TARGET_FILE:manyforge_ros2_control_execution_host_pick_place_planning_fixture>"
      MANYFORGE_NATIVE_PROJECTION_TEST_TARGET_ROOT="$<TARGET_FILE_DIR:manyforge_ros2_control_execution_host_projection_fixture>"
      MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT="$<TARGET_FILE_DIR:manyforge_ros2_control_execution_host>")
    target_link_libraries(test_retained_native_runtime_unit_adapter PRIVATE
      manyforge_component_manager
      manyforge_deployment_verification_core
      manyforge_scene
      # The test encodes logical Scene snapshots itself; link the codec
      # directly (strict --no-copy-dt-needed-entries linkers reject reaching
      # it only through another shared library's DT_NEEDED).
      manyforge::scene_value_codec
      manyforge_roboplan_native_runtime
      GTest::gtest_main)

    endif()

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

    gtest_discover_tests(test_mock_boolean_output_provider
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    if(TARGET test_ros_boolean_output_provider)
      gtest_discover_tests(test_ros_boolean_output_provider
        PROPERTIES
          ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
          TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    endif()
    if(TARGET test_ros_boolean_input_provider)
      gtest_discover_tests(test_ros_boolean_input_provider
        PROPERTIES
          ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
          TIMEOUT 30)
    endif()
    if(TARGET test_ros_mapped_boolean_input_provider)
      gtest_discover_tests(test_ros_mapped_boolean_input_provider
        PROPERTIES
          ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
          TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    endif()
    # The qualification runner removes its ambient loader environment before
    # CTest execution. Register every RoboPlan GTest with the exact library
    # closure derived above instead of relying on inherited process state.
    gtest_discover_tests(test_native_runtime_support
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_controller_robot_description
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_controller_launch_artifacts
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_controller_launch_process
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_cell_admission_command_codec
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_owner_settlement_readers
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_managed_projection_registry
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
    if(TARGET test_retained_native_runtime_unit_adapter)
    gtest_discover_tests(test_retained_native_runtime_unit_adapter
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    endif()
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
    gtest_discover_tests(test_retained_native_cell_runtime
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
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
        FIXTURES_REQUIRED ProgramCapturePackage)
    gtest_discover_tests(test_native_stop_qualification_seams
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_managed_kernel_admission_gate
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    # The existing Scene target can exercise its private adoption through the
    # actual managed owner when this complete host graph is configured.
    if(TARGET test_canonical_empty_scene_owner)
      target_link_libraries(test_canonical_empty_scene_owner PRIVATE
        manyforge_roboplan_managed_kernel_apply_fence
        manyforge_component_manager)
      target_include_directories(test_canonical_empty_scene_owner PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_compile_definitions(test_canonical_empty_scene_owner PRIVATE
        MANYFORGE_SCENE_TEST_MANAGED_INTERLOCK=1)
    endif()
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
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    add_test(
      NAME native_addressed_execution_process
      COMMAND
        "${Python3_EXECUTABLE}" -m pytest
        "${_MANYFORGE_WORKSPACE_ROOT}/manyforge_roboplan/test/test_native_execution_path.py")
    set_tests_properties(
      native_addressed_execution_process
      PROPERTIES
        ENVIRONMENT
          "LD_LIBRARY_PATH=${_LIB_PATHS};MANYFORGE_NATIVE_EXECUTION_HOST=$<TARGET_FILE:manyforge_ros2_control_execution_host_admission_fixture>;MANYFORGE_NATIVE_PRODUCTION_EXECUTION_HOST=$<TARGET_FILE:manyforge_ros2_control_execution_host>;MANYFORGE_NATIVE_PICK_PLACE_PLANNING_FIXTURE_HOST=$<TARGET_FILE:manyforge_ros2_control_execution_host_pick_place_planning_fixture>;MANYFORGE_ROBOPLAN_CANDIDATE_WORKER=$<TARGET_FILE:manyforge_roboplan_candidate_worker>;MANYFORGE_ROBOPLAN_FAKE_WORKER=$<TARGET_FILE:fake_process_candidate_worker>;MANYFORGE_I6_NATIVE_FIXTURE_ROOT=${_MANYFORGE_L07_NATIVE_CCLC_FIXTURE_ROOT};MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT=${_MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
        ENVIRONMENT_MODIFICATION
          "PYTHONPATH=path_list_prepend:${_MANYFORGE_WORKSPACE_ROOT};PYTHONPATH=path_list_prepend:${_MANYFORGE_WORKSPACE_ROOT}/manyforge_behavior"
        # This process test intentionally executes many sequential, model-
        # limited trajectories. Keep its harness deadline independent from
        # the much shorter per-operation execution deadlines it verifies.
        TIMEOUT 180)

    endif() # actual ROS process fixture targets

  endif()
endif()
