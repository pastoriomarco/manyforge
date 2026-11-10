cmake_minimum_required(VERSION 3.14)
project(manyforge_roboplan VERSION 0.2.1 LANGUAGES CXX)

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

# Restore the selected native planning dependency as an ordinary product
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
  set(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY ON)
  add_subdirectory(
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
    EXCLUDE_FROM_ALL)
  cmake_policy(POP)
  set(BUILD_TESTING "${_MANYFORGE_ROBOPLAN_BUILD_TESTING}")
  unset(_MANYFORGE_ROBOPLAN_BUILD_TESTING)
  unset(MANYFORGE_CORE_BUILD_SQLITE_OPERATION_JOURNAL)
  unset(MANYFORGE_CORE_BUILD_FILESYSTEM_RESOLVED_STOP_BASE_PLAN_REPOSITORY)
endif()

# The native projection boundary consumes the ManyForge-owned collision
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

# The selected host is a read-only consumer of retained products and the
# durable active-bundle publication. Embed those production libraries
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
    "Native execution requires retained-product verification and the "
    "active-bundle publication reader")
endif()

# Keep only the bounded, file-only bridge required to qualify exact
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

add_library(manyforge_roboplan_execution_host_support
  src/execution_host_support.cpp
  src/host_component_runtime.cpp
  src/host_component_stream_health.cpp)
target_include_directories(manyforge_roboplan_execution_host_support PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_execution_host_support
  PUBLIC manyforge_core
  PRIVATE Threads::Threads)

# The ROS-free mapping of a ros2_control connection's
# observations to the component state.
add_library(manyforge_roboplan_ros2_control_component_state
  src/ros2_control_component_state.cpp)
target_include_directories(manyforge_roboplan_ros2_control_component_state PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_ros2_control_component_state
  PUBLIC manyforge_core)

# ROS-free controller robot_description generation (model URDF in,
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

# The vendor-free launch generator and the Mock lane's
# launch contribution.
add_library(manyforge_roboplan_controller_launch_artifacts
  src/controller_launch_artifacts.cpp
  src/mock_lane.cpp)
target_include_directories(manyforge_roboplan_controller_launch_artifacts
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(manyforge_roboplan_controller_launch_artifacts
  PUBLIC manyforge_roboplan_controller_robot_description
  PRIVATE manyforge::content_identity)

# Command and process ownership of the controller launches the
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
# owner returned. The same implementations are promoted into the production
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

add_library(manyforge_roboplan_execution_host
  src/retained_gripper_runtime.cpp
  src/retained_gripper_route_publication_owner.cpp
  src/retained_execution_host.cpp
  src/retained_cell_runtime.cpp)
target_include_directories(manyforge_roboplan_execution_host PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(manyforge_roboplan_execution_host PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(manyforge_roboplan_execution_host PUBLIC
  manyforge_roboplan_native_state_projection
  manyforge_roboplan_execution_host_support
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
  manyforge_roboplan_execution_host)

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
  find_package(control_msgs REQUIRED)
  add_library(manyforge_ros_boolean_output_provider
    src/ros_boolean_output_provider.cpp)
  target_include_directories(manyforge_ros_boolean_output_provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_ros_boolean_output_provider PUBLIC
    manyforge_core rclcpp::rclcpp ${std_srvs_TARGETS} ${control_msgs_TARGETS}
    PRIVATE Threads::Threads manyforge_ros2_control_execution)
  add_library(manyforge_ros_boolean_input_provider
    src/ros_boolean_input_provider.cpp
    src/ros_mapped_boolean_input_provider.cpp)
  target_include_directories(manyforge_ros_boolean_input_provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(manyforge_ros_boolean_input_provider PUBLIC
    manyforge_core rclcpp::rclcpp ${std_msgs_TARGETS} ${control_msgs_TARGETS}
    PRIVATE manyforge_ros2_control_execution)

  find_package(pal_statistics_msgs REQUIRED)
  find_package(diagnostic_msgs REQUIRED)
  target_link_libraries(manyforge_roboplan_execution_host PRIVATE
    manyforge_ros2_control_execution ${pal_statistics_msgs_TARGETS})
  find_package(ament_cmake_core REQUIRED)
  find_package(ament_index_cpp REQUIRED)
  # The ros2_control lane runtime, the component adapter registry and the
  # generic adapters. Vendor and lane families are separate
  # libraries the registry loads; nothing here links them.
  add_library(manyforge_roboplan_ros2_control_runtime
    src/component_adapter_registry.cpp
    src/component_stream_sample_bridge.cpp
    src/io_provider_lifecycle_product.cpp
    src/passive_component_adapter.cpp
    src/simulated_plc_readiness.cpp
    src/retained_ros2_control_backend_factory.cpp
    src/retained_ros2_control_lane_config.cpp
    src/retained_ros2_control_runtime_handle.cpp
    src/retained_ros2_control_statistics_monitor.cpp
    src/retained_tool_context.cpp
    src/ros2_control_component_adapter.cpp)
  target_include_directories(manyforge_roboplan_ros2_control_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_include_directories(manyforge_roboplan_ros2_control_runtime PRIVATE
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_component_manager/include")
  target_link_libraries(manyforge_roboplan_ros2_control_runtime PUBLIC
    manyforge_geometric_contact_input_provider
    manyforge_mock_boolean_input_provider
    manyforge_ros_boolean_input_provider
    manyforge_mock_boolean_output_provider
    manyforge_ros_boolean_output_provider
    manyforge_roboplan_execution_host
    manyforge_roboplan_controller_robot_description
    manyforge_roboplan_controller_launch_artifacts
    manyforge_roboplan_controller_launch_process
    manyforge_roboplan_ros2_control_component_state
    manyforge_ros2_control_execution
    ${pal_statistics_msgs_TARGETS}
    ${diagnostic_msgs_TARGETS}
    ament_index_cpp::ament_index_cpp
    manyforge::content_identity
    PRIVATE ${CMAKE_DL_LIBS})
  install(DIRECTORY
    "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_ros2_control_execution/lane"
    DESTINATION share/manyforge_ros2_control_execution)

  # Every adapter family is a shared library registered in the
  # ament resource index `manyforge_component_adapters`. In this build tree
  # the index prefix is MANYFORGE_COMPONENT_ADAPTER_PREFIX (the execution
  # host's environment adds it to AMENT_PREFIX_PATH): family libraries are
  # written to its `lib/`, and a family's data folder is linked under its
  # `share/<package>/`. Family libraries link the runtime above (shared in
  # the integration graph); the host links none of them.
  set(MANYFORGE_COMPONENT_ADAPTER_PREFIX "${CMAKE_BINARY_DIR}/ament_cmake_index"
    CACHE INTERNAL "The build tree's component adapter prefix")
  function(manyforge_register_component_adapter_family target family)
    set_target_properties(${target} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${MANYFORGE_COMPONENT_ADAPTER_PREFIX}/lib")
    ament_index_register_resource(manyforge_component_adapters
      PACKAGE_NAME ${family}
      CONTENT "lib${target}.so")
    install(TARGETS ${target} LIBRARY DESTINATION lib)
  endfunction()
  function(manyforge_stage_component_adapter_data package directory)
    file(MAKE_DIRECTORY "${MANYFORGE_COMPONENT_ADAPTER_PREFIX}/share/${package}")
    file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/${directory}"
      "${MANYFORGE_COMPONENT_ADAPTER_PREFIX}/share/${package}/${directory}" SYMBOLIC)
  endfunction()

  # A model lane row's relative `controllers_file` resolves against the
  # model folder in the `manyforge_example_models` share.
  file(MAKE_DIRECTORY "${MANYFORGE_COMPONENT_ADAPTER_PREFIX}/share/manyforge_example_models")
  file(CREATE_LINK "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_example_models/models"
    "${MANYFORGE_COMPONENT_ADAPTER_PREFIX}/share/manyforge_example_models/models" SYMBOLIC)
  ament_index_register_resource(packages PACKAGE_NAME manyforge_example_models)

  add_library(manyforge_mock_adapter_family SHARED
    src/mock_adapter_family.cpp)
  target_link_libraries(manyforge_mock_adapter_family PRIVATE
    manyforge_roboplan_ros2_control_runtime)
  manyforge_register_component_adapter_family(manyforge_mock_adapter_family mock)

  find_package(simulation_interfaces REQUIRED)
  add_library(manyforge_simulation_adapter_family SHARED
    src/simulation_adapter_family.cpp
    src/simulator_component_adapter.cpp
    src/ros_simulation_control.cpp)
  target_include_directories(manyforge_simulation_adapter_family PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(manyforge_simulation_adapter_family PRIVATE
    manyforge_roboplan_ros2_control_runtime
    rclcpp::rclcpp
    ${simulation_interfaces_TARGETS})
  manyforge_register_component_adapter_family(
    manyforge_simulation_adapter_family simulation_interfaces)

  # The vendor-neutral point-cloud source (a `kind: sensor` model); its
  # bridge runs as the component's own process.
  add_library(manyforge_pointcloud_adapter_family SHARED
    src/pointcloud_adapter_family.cpp
    src/pointcloud_source_adapter.cpp
    src/ros_pointcloud_bridge_link.cpp)
  target_include_directories(manyforge_pointcloud_adapter_family PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(manyforge_pointcloud_adapter_family PRIVATE
    manyforge_roboplan_ros2_control_runtime
    rclcpp::rclcpp
    ${std_msgs_TARGETS})
  manyforge_register_component_adapter_family(
    manyforge_pointcloud_adapter_family pointcloud)

  # The assistant (a `kind: assistant` model); its host runs as the
  # component's own process and owns the chat bridge.
  add_library(manyforge_assistant_adapter_family SHARED
    src/assistant_adapter_family.cpp
    src/assistant_component_adapter.cpp
    src/ros_assistant_host_link.cpp)
  target_include_directories(manyforge_assistant_adapter_family PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(manyforge_assistant_adapter_family PRIVATE
    manyforge_roboplan_ros2_control_runtime
    rclcpp::rclcpp
    ${std_msgs_TARGETS})
  manyforge_register_component_adapter_family(
    manyforge_assistant_adapter_family assistant)

  # The vendor and simulator adapter packages build their families against
  # the runtime above.
  foreach(_adapter_package manyforge_xarm_control manyforge_isaac_adapter)
    if(NOT TARGET ${_adapter_package})
      add_subdirectory(
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/${_adapter_package}"
        "${CMAKE_CURRENT_BINARY_DIR}/${_adapter_package}")
    endif()
  endforeach()
  set(MANYFORGE_COMPONENT_ADAPTER_FAMILIES
    manyforge_mock_adapter_family
    manyforge_simulation_adapter_family
    manyforge_pointcloud_adapter_family
    manyforge_assistant_adapter_family
    manyforge_xarm_adapter_family
    manyforge_isaac_adapter_family)
endif()

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
  # The host loads the adapter families at run time; building it builds them.
  add_dependencies(manyforge_ros2_control_execution_host
    ${MANYFORGE_COMPONENT_ADAPTER_FAMILIES})
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
      # Each test process runs in its own ROS domain (Fast DDS #6599).
      # ROS 2's run_rmw_isolated (the ament_cmake_ros isolation runner) holds
      # a free domain 1..101 for the process; LOCALHOST keeps test containers
      # on one Docker bridge apart. CMake 3.28 has no TEST_LAUNCHER;
      # add_test and gtest_discover_tests prepend this to every test command.
      find_program(MANYFORGE_RUN_RMW_ISOLATED run_rmw_isolated
        PATHS ENV AMENT_PREFIX_PATH
        PATH_SUFFIXES lib/rmw_test_fixture_implementation
        NO_DEFAULT_PATH REQUIRED)
      set(CMAKE_CROSSCOMPILING_EMULATOR
        env ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST "${MANYFORGE_RUN_RMW_ISOLATED}")

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
      MANYFORGE_ROS2_CONTROL_EXECUTION_HOST=1)
    target_link_libraries(
      manyforge_ros2_control_execution_host_pick_place_planning_fixture PRIVATE
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_deployment_program_io_condition_compiler
      manyforge_roboplan_cell_admission_command_codec
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
        "Retained collision projection requires the authoritative "
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

    add_executable(test_execution_host_support
      test/test_execution_host_support.cpp)
    target_link_libraries(test_execution_host_support PRIVATE
      manyforge_roboplan_execution_host_support GTest::gtest_main)

    add_executable(test_direct_control_manual_runs
      test/test_direct_control_manual_runs.cpp)
    target_include_directories(test_direct_control_manual_runs PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_direct_control_manual_runs PRIVATE
      GTest::gtest_main)

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
      manyforge_roboplan_execution_host_support
      manyforge_core manyforge::content_identity GTest::gtest_main)
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      target_sources(test_controller_launch_process PRIVATE
        src/ros_host_composition.cpp src/execution_host_posix.cpp
        src/execution_host_commands.cpp
        src/product_backed_runtime_participant_resolver.cpp
        src/sealed_program_execution_capture.cpp)
      target_compile_definitions(test_controller_launch_process PRIVATE MANYFORGE_TEST_RETIRED_COMPOSITION)
      target_include_directories(test_controller_launch_process PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
      target_link_libraries(test_controller_launch_process PRIVATE
        manyforge_roboplan_ros2_control_runtime manyforge_roboplan_retained_consumer
        manyforge_core_sqlite_operation_journal manyforge_deployment manyforge_component_manager
        manyforge::scene_value_codec SQLite::SQLite3 Threads::Threads)
    endif()

    add_executable(test_cell_admission_command_codec
      test/test_cell_admission_command_codec.cpp)
    target_link_libraries(test_cell_admission_command_codec PRIVATE
      manyforge_roboplan_cell_admission_command_codec GTest::gtest_main)

    add_executable(test_owner_settlement_readers
      src/applied_program_owner_reader.cpp
      test/test_owner_settlement_readers.cpp)
    target_link_libraries(test_owner_settlement_readers PRIVATE
      manyforge_roboplan_cell_admission_command_codec manyforge_deployment
      GTest::gtest_main SQLite::SQLite3)

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
      roboplan_toppra::roboplan_toppra
      GTest::gtest_main)

    add_executable(test_roboplan_planner_backend
      test/test_roboplan_planner_backend.cpp)
    target_include_directories(test_roboplan_planner_backend PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
    target_link_libraries(test_roboplan_planner_backend PRIVATE
      manyforge_roboplan_planner_backend GTest::gtest_main)

    add_executable(test_retained_backends
      test/test_retained_backends.cpp)
    target_include_directories(test_retained_backends PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
    target_compile_definitions(test_retained_backends PRIVATE
      MANYFORGE_CORE_TEST_GOLDEN_DIR="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test/golden")
    target_link_libraries(test_retained_backends PRIVATE
      manyforge_roboplan_native_gripper
      manyforge_roboplan_execution_host
      GTest::gtest_main)
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      target_link_libraries(test_retained_backends PRIVATE
        manyforge_roboplan_ros2_control_runtime)
    endif()

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      add_executable(test_ros_boolean_output_provider
        test/test_ros_boolean_output_provider.cpp)
      target_link_libraries(test_ros_boolean_output_provider PRIVATE
        manyforge_ros_boolean_output_provider manyforge_ros2_control_execution GTest::gtest_main)

      # Isolated OD55 timing build: only this binary instruments GPIO leaves.
      add_executable(test_od55_gpio_leaf_timing
        test/test_ros_boolean_output_provider.cpp src/ros_boolean_output_provider.cpp)
      target_compile_definitions(test_od55_gpio_leaf_timing PRIVATE MANYFORGE_LEAF_TIMING)
      target_include_directories(test_od55_gpio_leaf_timing PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
      target_link_libraries(test_od55_gpio_leaf_timing PRIVATE
        manyforge_core rclcpp::rclcpp ${std_srvs_TARGETS} ${control_msgs_TARGETS}
        Threads::Threads manyforge_ros2_control_execution GTest::gtest_main)
      add_test(NAME od55_gpio_leaf_recovery_cut
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
          $<TARGET_FILE:test_od55_gpio_leaf_timing>
          --gtest_filter=RosBooleanOutputProviderTest.GpioRecoveryCopiesInflightAndFencesQueuedWrite)
      set_tests_properties(od55_gpio_leaf_recovery_cut PROPERTIES TIMEOUT 30)
      add_test(NAME od55_service_disable_during_send
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
          $<TARGET_FILE:test_od55_gpio_leaf_timing>
          --gtest_filter=RosBooleanOutputProviderTest.ServiceDisableDoesNotWaitForAdmittedSend)
      set_tests_properties(od55_service_disable_during_send PROPERTIES TIMEOUT 30)
      add_test(NAME od55_service_leaf_deadline
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
          $<TARGET_FILE:test_od55_gpio_leaf_timing>
          --gtest_filter=RosBooleanOutputProviderTest.ServiceLeafWaitPastDeadlineIsKnownUnsent)
      set_tests_properties(od55_service_leaf_deadline PROPERTIES TIMEOUT 30)
      add_test(NAME od55_gpio_leaf_timing
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
          $<TARGET_FILE:test_od55_gpio_leaf_timing>
          --gtest_filter=RosBooleanOutputProviderTest.GpioLeafTimingConcurrentWriters)
      set_tests_properties(od55_gpio_leaf_timing PROPERTIES TIMEOUT 120)

      add_executable(test_ros_boolean_input_provider
        test/test_ros_boolean_input_provider.cpp)
      target_link_libraries(test_ros_boolean_input_provider PRIVATE
        manyforge_ros_boolean_input_provider manyforge_ros2_control_execution GTest::gtest_main)

      add_executable(test_ros_mapped_boolean_input_provider
        test/test_ros_mapped_boolean_input_provider.cpp)
      target_link_libraries(test_ros_mapped_boolean_input_provider PRIVATE
        manyforge_ros_boolean_input_provider manyforge_ros2_control_execution GTest::gtest_main)

      add_executable(test_retained_ros2_control_backend_factory
        test/test_retained_ros2_control_backend_factory.cpp
        test/test_retained_tool_context.cpp
        src/ros_host_composition.cpp
        src/execution_host_posix.cpp
        src/execution_host_commands.cpp
        src/product_backed_runtime_participant_resolver.cpp
        src/sealed_program_execution_capture.cpp)
      target_include_directories(test_retained_ros2_control_backend_factory PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
      target_link_libraries(test_retained_ros2_control_backend_factory PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_execution_host_support
        manyforge_roboplan_controller_launch_process
        manyforge_roboplan_retained_consumer
        manyforge_core_sqlite_operation_journal
        manyforge_deployment manyforge_component_manager manyforge_core
        manyforge::content_identity manyforge::scene_value_codec
        SQLite::SQLite3 Threads::Threads
        GTest::gtest_main)
      gtest_discover_tests(test_retained_ros2_control_backend_factory)

      # Runtime handle, controller switching and binding epochs against
      # in-process ROS stand-ins.
      add_executable(test_retained_ros2_control_runtime_handle
        test/test_io_provider_lifecycle_product.cpp
        test/test_simulated_plc_readiness.cpp
        test/test_retained_ros2_control_runtime_handle.cpp)
      target_include_directories(test_retained_ros2_control_runtime_handle PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
      target_link_libraries(test_retained_ros2_control_runtime_handle PRIVATE
        manyforge_component_manager
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_execution_host_support
        manyforge_deployment_canonical_package_generator
        GTest::gtest_main)
      # The display-joints case publishes a vendor joint topic.
      find_package(xarm_msgs REQUIRED)
      target_link_libraries(test_retained_ros2_control_runtime_handle PRIVATE
        ${xarm_msgs_TARGETS})
      gtest_discover_tests(test_retained_ros2_control_runtime_handle
        PROPERTIES TIMEOUT 120)

      # Real addressed Kernel + the Core participant over the real JTC adapter
      # against an in-process trajectory controller (requires a ROS 2 runtime).
      if(TARGET manyforge_ros2_control_leaf_timing_test_support)
        # Isolated test only: use the instrumented adapter while retaining the
        # real runtime handle, addressed Kernel, and SQLite journal fixture.
        add_executable(test_retained_motion_leaf_timing
          test/test_retained_motion_leaf_timing.cpp
          src/ros_host_composition.cpp
          src/execution_host_posix.cpp
          src/execution_host_commands.cpp
          src/product_backed_runtime_participant_resolver.cpp
          src/sealed_program_execution_capture.cpp)
        target_include_directories(test_retained_motion_leaf_timing PRIVATE
          "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
        target_compile_definitions(test_retained_motion_leaf_timing PRIVATE
          MANYFORGE_LEAF_TIMING
          MANYFORGE_CORE_TEST_GOLDEN_DIR="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test/golden")
        target_link_libraries(test_retained_motion_leaf_timing PRIVATE
          manyforge_ros2_control_leaf_timing_test_support
          manyforge_roboplan_ros2_control_runtime
          manyforge_roboplan_execution_host_support
          manyforge_roboplan_controller_launch_process
          manyforge_roboplan_retained_consumer
          manyforge_deployment manyforge_component_manager manyforge_core
          manyforge::content_identity manyforge::scene_value_codec
          SQLite::SQLite3 Threads::Threads
          manyforge_core_sqlite_operation_journal GTest::gtest_main)
        add_test(NAME retained_motion_leaf_timing
          COMMAND test_retained_motion_leaf_timing --gtest_filter=RetainedMotionLeafTiming.*)
        set_tests_properties(retained_motion_leaf_timing PROPERTIES TIMEOUT 30)
      endif()

      add_executable(test_ros2_control_kernel_stop_integration
        test/test_ros2_control_kernel_stop_integration.cpp)
      target_include_directories(test_ros2_control_kernel_stop_integration PRIVATE
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include")
      target_compile_definitions(test_ros2_control_kernel_stop_integration PRIVATE
        MANYFORGE_CORE_TEST_GOLDEN_DIR="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test/golden")
      target_link_libraries(test_ros2_control_kernel_stop_integration PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_execution_host_support
        manyforge_core_sqlite_operation_journal
        GTest::gtest_main)
      gtest_discover_tests(test_ros2_control_kernel_stop_integration
        PROPERTIES TIMEOUT 120)

      add_executable(test_passive_component_adapter
        test/test_passive_component_adapter.cpp)
      target_link_libraries(test_passive_component_adapter PRIVATE
        manyforge_roboplan_ros2_control_runtime GTest::gtest_main)
      gtest_discover_tests(test_passive_component_adapter
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_simulator_component_adapter
        test/test_simulator_component_adapter.cpp
        src/simulator_component_adapter.cpp)
      target_include_directories(test_simulator_component_adapter PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
      target_link_libraries(test_simulator_component_adapter PRIVATE
        manyforge_core GTest::gtest_main)
      gtest_discover_tests(test_simulator_component_adapter
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_ros_simulation_control
        test/test_ros_simulation_control.cpp
        src/simulator_component_adapter.cpp
        src/ros_simulation_control.cpp)
      target_include_directories(test_ros_simulation_control PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_link_libraries(test_ros_simulation_control PRIVATE
        manyforge_roboplan_ros2_control_runtime rclcpp::rclcpp
        ${simulation_interfaces_TARGETS} GTest::gtest_main)
      gtest_discover_tests(test_ros_simulation_control
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_ros_adapter_entity_owner
        test/test_ros_adapter_entity_owner.cpp
        src/ros_simulation_control.cpp
        src/ros_pointcloud_bridge_link.cpp
        src/ros_assistant_host_link.cpp)
      target_include_directories(test_ros_adapter_entity_owner PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_link_libraries(test_ros_adapter_entity_owner PRIVATE
        manyforge_roboplan_ros2_control_runtime rclcpp::rclcpp
        ${simulation_interfaces_TARGETS} ${std_msgs_TARGETS} GTest::gtest_main)
      gtest_discover_tests(test_ros_adapter_entity_owner
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_pointcloud_source_adapter
        test/test_pointcloud_source_adapter.cpp
        src/pointcloud_source_adapter.cpp)
      target_include_directories(test_pointcloud_source_adapter PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
      target_link_libraries(test_pointcloud_source_adapter PRIVATE
        manyforge_core manyforge::content_identity GTest::gtest_main)
      gtest_discover_tests(test_pointcloud_source_adapter
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_assistant_component_adapter
        test/test_assistant_component_adapter.cpp
        src/assistant_component_adapter.cpp)
      target_include_directories(test_assistant_component_adapter PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
      target_link_libraries(test_assistant_component_adapter PRIVATE
        manyforge_core manyforge::content_identity GTest::gtest_main)
      gtest_discover_tests(test_assistant_component_adapter
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      # The registry loads the real Mock and simulation_interfaces family
      # libraries from a test index.
      add_executable(test_component_adapter_registry
        test/test_component_adapter_registry.cpp)
      target_compile_definitions(test_component_adapter_registry PRIVATE
        MANYFORGE_MOCK_ADAPTER_FAMILY_LIBRARY="$<TARGET_FILE:manyforge_mock_adapter_family>"
        MANYFORGE_SIMULATION_ADAPTER_FAMILY_LIBRARY="$<TARGET_FILE:manyforge_simulation_adapter_family>")
      add_dependencies(test_component_adapter_registry
        manyforge_mock_adapter_family manyforge_simulation_adapter_family)
      target_link_libraries(test_component_adapter_registry PRIVATE
        manyforge_roboplan_ros2_control_runtime GTest::gtest_main)
      gtest_discover_tests(test_component_adapter_registry
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    endif()

    add_executable(test_ros2_control_component_state
      test/test_ros2_control_component_state.cpp)
    target_link_libraries(test_ros2_control_component_state PRIVATE
      manyforge_roboplan_ros2_control_component_state GTest::gtest_main)

    add_executable(test_host_component_runtime
      test/test_host_component_runtime.cpp)
    target_include_directories(test_host_component_runtime PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_host_component_runtime PRIVATE
      manyforge_roboplan_execution_host_support GTest::gtest_main)

    add_executable(test_retained_cell_runtime
      test/test_retained_cell_runtime.cpp)
    target_link_libraries(test_retained_cell_runtime PRIVATE
      manyforge_roboplan_execution_host GTest::gtest_main)
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
      target_link_libraries(test_retained_cell_runtime PRIVATE
        manyforge_roboplan_ros2_control_runtime)
      target_compile_definitions(test_retained_cell_runtime PRIVATE
        MANYFORGE_ROS_GRIPPER_CELL_TEST=1)
    endif()

    # Measured geometry of the retained robots, over the prepared dual-robot cell
    # package (`manyforge_dual_robot_cell_bootstrap`, where Deployment tests exist).
    add_executable(test_retained_measured_robot_geometry
      test/test_retained_measured_robot_geometry.cpp)
    target_include_directories(test_retained_measured_robot_geometry PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/test"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/test"
      "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
    if(TARGET manyforge_dual_robot_cell_bootstrap)
      add_dependencies(test_retained_measured_robot_geometry
        manyforge_dual_robot_cell_bootstrap)
    endif()
    # The maintained Lite6 Scenario is materialized through the normal
    # activation path (robot and mounted tool as separate components).
    target_compile_definitions(test_retained_measured_robot_geometry PRIVATE
      MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}")
    target_link_libraries(test_retained_measured_robot_geometry PRIVATE
      manyforge_preparation_support
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
        manyforge_ros_boolean_output_provider manyforge_ros2_control_execution manyforge_deployment
        manyforge_roboplan_controller_launch_artifacts
        manyforge_deployment_canonical_package_generator GTest::gtest_main)
      gtest_discover_tests(test_host_io_output_channels
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})

      add_executable(test_host_io_provider_installation
        test/test_host_io_provider_installation.cpp
        test/test_host_composition_members.cpp
        test/test_retired_composition.cpp
        test/test_applied_program_blackboard_context.cpp
        test/test_applied_global_io_rule_installation.cpp
        test/test_global_rule_event_responses.cpp
        test/test_io_rule_result_responses.cpp
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
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_core/test_support/include"
        "${MANYFORGE_DEPLOYMENT_SOURCE_DIR}/src")
      get_target_property(_MANYFORGE_PROGRAM_CAPTURE_BINARY_DIR
        manyforge_prepare_activation BINARY_DIR)
      target_compile_definitions(test_host_io_provider_installation PRIVATE
        MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT="${_MANYFORGE_PROGRAM_CAPTURE_BINARY_DIR}/program_capture_package")
      target_link_libraries(test_host_io_provider_installation PRIVATE
        manyforge_roboplan_ros2_control_runtime
        manyforge_roboplan_controller_launch_process
        manyforge_roboplan_retained_consumer
        manyforge_roboplan_cell_admission_command_codec
        manyforge_deployment_program_io_condition_compiler
        manyforge_deployment_canonical_package_generator
        manyforge_deployment manyforge_component_manager manyforge_core
        manyforge_core_sqlite_operation_journal
        manyforge::content_identity manyforge::scene_value_codec
        SQLite::SQLite3 Threads::Threads GTest::gtest_main)
      add_dependencies(test_host_io_provider_installation
        manyforge_prepare_activation)
      gtest_discover_tests(test_host_io_provider_installation
        PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
          FIXTURES_REQUIRED ProgramCapturePackage)
      # The Deployment package's fixture test isn't part of this build's
      # tests; produce the same prepared activation here.
      add_test(NAME ProgramCapturePackage
        COMMAND "${CMAKE_COMMAND}" -E env
          "PYTHONPATH=${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_behavior:${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
          "${Python3_EXECUTABLE}"
          "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test/produce_activation_package.py"
          --tool "$<TARGET_FILE:manyforge_prepare_activation>"
          --workspace "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
          --package-root "${_MANYFORGE_PROGRAM_CAPTURE_BINARY_DIR}/program_capture_package"
          --scenario "examples/mock_lite6_pick_and_place.scenario=lite6_pick_place_cell")
      set_tests_properties(ProgramCapturePackage PROPERTIES
        FIXTURES_SETUP ProgramCapturePackage TIMEOUT 120)
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

    add_executable(test_execution_event_stream
      test/test_execution_event_stream.cpp)
    target_include_directories(test_execution_event_stream PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_execution_event_stream PRIVATE
      manyforge_core manyforge::content_identity Threads::Threads
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

    # Consumer-equivalence integration: embed the production deployment
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
        "Retained-product consumer qualification requires the "
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
        "Non-empty RoboPlan consumer qualification requires Python")
    endif()
    find_program(_MANYFORGE_URDFDOM_CHECK_URDF_EXECUTABLE
      NAMES check_urdf)
    if(NOT _MANYFORGE_URDFDOM_CHECK_URDF_EXECUTABLE)
      message(FATAL_ERROR
        "Independent retained-model qualification requires urdfdom check_urdf")
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
    # The split Lite6 arm and gripper (tool split), beside the combined
    # root: the maintained activation fixture prepares each model component.
    foreach(_lite6_split_asset lite6 lite_gripper)
      _manyforge_prepare_roboplan_model_fixture(
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_example_models/models/${_lite6_split_asset}"
        "${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}"
        ${_lite6_split_asset} ${_lite6_split_asset}.urdf ${_lite6_split_asset}.srdf
        "package://manyforge_example_models/models/lite6_lite_gripper/"
        _MANYFORGE_ROBOPLAN_LITE6_SPLIT_FIXTURE_ROOT
        _MANYFORGE_ROBOPLAN_LITE6_SPLIT_RESOURCE_INPUTS)
      list(APPEND _MANYFORGE_LITE6_MAINTAINED_FIXTURE_INPUTS
        "${_MANYFORGE_ROBOPLAN_LITE6_SPLIT_FIXTURE_ROOT}/model.urdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_SPLIT_FIXTURE_ROOT}/model.srdf"
        "${_MANYFORGE_ROBOPLAN_LITE6_SPLIT_FIXTURE_ROOT}/preparation-manifest.json")
    endforeach()
    # Inputs of the generated packages built from the maintained Lite6 Scenario.
    list(APPEND _MANYFORGE_LITE6_MAINTAINED_FIXTURE_INPUTS
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/examples/mock_lite6_pick_and_place.scenario/deployment.yaml"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/examples/mock_lite6_pick_and_place.scenario/program.yaml"
      ${_MANYFORGE_ROBOPLAN_LITE6_SPLIT_RESOURCE_INPUTS})

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
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/resources/maintained/model_sources/robotiq_2f_85/robotiq_2f_85.model_asset.json")

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

    # Process qualification needs one ordinary retained bundle, the exact
    # compiled Program Binding selected for it, and the durable active
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
      # The maintained Lite6/UR5 Scenarios are materialized through the normal
      # activation path; each model's prepared root is a sibling of this one.
      MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
    target_include_directories(
      generate_lite6_native_execution_fixture_tool PRIVATE
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/src"
      "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test")
    target_link_libraries(
      generate_lite6_native_execution_fixture_tool PRIVATE
      manyforge_preparation_support
      manyforge_deployment
      manyforge_component_manager
      manyforge_core)

    set(_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_execution_fixture")
    set(_MANYFORGE_NATIVE_EXECUTION_FIXTURE_MANIFEST
      "${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root
        "${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT}"
        --fixture-kind application-ros
      DEPENDS
        generate_lite6_native_execution_fixture_tool
        ${_MANYFORGE_LITE6_MAINTAINED_FIXTURE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_execution_fixture ALL
      DEPENDS "${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_MANIFEST}")
    add_dependencies(test_managed_kernel_admission_gate
      generate_lite6_native_execution_fixture)
    target_compile_definitions(test_managed_kernel_admission_gate PRIVATE
      MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT="${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
      MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")

    # The standalone Position source uses a genuine Robotiq-only retained model.
    # Historical pneumatic CLI selections remain separate fixture inputs.
    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    # Retain native-named output paths used by the owning process test.
    set(_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/lite6_native_gripper_only_fixture")
    set(_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_MANIFEST
      "${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}/manifest.json")
    add_custom_command(
      OUTPUT "${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_MANIFEST}"
      COMMAND
        "${CMAKE_COMMAND}" -E rm -rf
        "${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
      COMMAND
        "$<TARGET_FILE:generate_lite6_native_execution_fixture_tool>"
        --output-root "${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
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
      DEPENDS "${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_MANIFEST}")

    endif()

    if(TARGET test_retained_ros2_control_runtime_handle)
      target_compile_definitions(test_retained_ros2_control_runtime_handle PRIVATE
        MANYFORGE_LITE6_STRUCTURAL_FIXTURE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_FIXTURE_ROOT}"
        MANYFORGE_LITE6_STRUCTURAL_SOURCE_ROOT="${_MANYFORGE_ROBOPLAN_LITE6_SOURCE_ROOT}")
    endif()
    foreach(_MANYFORGE_IO_HELPER_TEST IN ITEMS
        test_host_io_output_channels test_host_io_provider_installation
        test_retained_measured_robot_geometry)
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
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/examples/mock_ur5_pick_and_place.scenario/deployment.yaml"
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/examples/mock_ur5_pick_and_place.scenario/program.yaml"
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
        ${_MANYFORGE_LITE6_MAINTAINED_FIXTURE_INPUTS}
      VERBATIM)
    add_custom_target(generate_lite6_native_projection_successor_fixture ALL
      DEPENDS "${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_MANIFEST}")

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
      manyforge_roboplan_retained_consumer
      manyforge_roboplan_collision_projection
      manyforge_roboplan_native_state_projection
      manyforge_roboplan_execution_host
      manyforge_roboplan_process_candidate_client
      manyforge_roboplan_scene_factory
      manyforge_roboplan_managed_kernel_apply_fence
      manyforge_collision
      manyforge_core
      manyforge_deployment
      manyforge_deployment_admission
      manyforge_preparation_support
      roboplan::roboplan
      GTest::gtest_main)
    # Maintained Scenarios are materialized through the normal activation path.
    target_compile_definitions(test_retained_product_consumer PRIVATE
      MANYFORGE_WORKSPACE_ROOT="${_MANYFORGE_REPOSITORY_SOURCE_ROOT}")

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
        manyforge_roboplan_ros2_control_runtime
        manyforge::component_manager)
      target_compile_definitions(test_retained_product_consumer PRIVATE
        MANYFORGE_RETAINED_ROS2_CONTROL_TEST_FACTORY=1)
    endif()

    if(MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME)
    add_executable(test_retained_execution_host_unit_adapter
      test/test_retained_execution_host_unit_adapter.cpp)
    add_dependencies(test_retained_execution_host_unit_adapter
      generate_ur5_arm_only_ros_fixture
      generate_lite6_native_execution_fixture
      generate_lite6_native_gripper_only_fixture
      generate_lite6_native_projection_successor_fixture
      manyforge_ros2_control_execution_host
      manyforge_ros2_control_execution_host_projection_fixture
      manyforge_ros2_control_execution_host_pick_place_planning_fixture
      manyforge_roboplan_candidate_worker)
    target_compile_definitions(
      test_retained_execution_host_unit_adapter PRIVATE
      MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT="${_MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT="${_MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT="${_MANYFORGE_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT="${_MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT}"
      MANYFORGE_NATIVE_PICK_PLACE_PLANNING_TARGET="$<TARGET_FILE:manyforge_ros2_control_execution_host_pick_place_planning_fixture>"
      MANYFORGE_NATIVE_PROJECTION_TEST_TARGET_ROOT="$<TARGET_FILE_DIR:manyforge_ros2_control_execution_host_projection_fixture>"
      MANYFORGE_EXECUTION_HOST_TARGET_ROOT="$<TARGET_FILE_DIR:manyforge_ros2_control_execution_host>")
    target_link_libraries(test_retained_execution_host_unit_adapter PRIVATE
      manyforge_component_manager
      manyforge_deployment_verification_core
      manyforge_scene
      # The test encodes logical Scene snapshots itself; link the codec
      # directly (strict --no-copy-dt-needed-entries linkers reject reaching
      # it only through another shared library's DT_NEEDED).
      manyforge::scene_value_codec
      manyforge_roboplan_execution_host
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
    gtest_discover_tests(test_execution_host_support
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_direct_control_manual_runs
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
    if(TARGET test_retained_execution_host_unit_adapter)
    # The execution host loads its adapter families from the build tree's
    # ament prefix, as scripts/test-all.sh does.
    gtest_discover_tests(test_retained_execution_host_unit_adapter
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        ENVIRONMENT_MODIFICATION
          "AMENT_PREFIX_PATH=path_list_prepend:${MANYFORGE_COMPONENT_ADAPTER_PREFIX}"
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
    gtest_discover_tests(test_retained_backends
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_retained_cell_runtime
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    get_target_property(_MANYFORGE_DUAL_ROBOT_BOOTSTRAP_BINARY_DIR
      manyforge_prepare_activation BINARY_DIR)
    gtest_discover_tests(test_retained_measured_robot_geometry
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        # gtest_discover_tests splits a ';' list into separate properties, so
        # the second variable goes through its own single-item property.
        ENVIRONMENT_MODIFICATION
          "MANYFORGE_DUAL_ROBOT_CELL_BOOTSTRAP_ROOT=set:${_MANYFORGE_DUAL_ROBOT_BOOTSTRAP_BINARY_DIR}/dual-robot-scenario-fixture/retained-package"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}
        FIXTURES_REQUIRED DualRobotCellBootstrapPackage)
    # The Deployment package's dual-robot cell bootstrap target isn't part of this
    # build; produce the same prepared package as this test's fixture.
    add_test(NAME DualRobotCellBootstrapPackage
      COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_behavior:${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
        "${Python3_EXECUTABLE}"
        "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}/manyforge_deployment/test/produce_activation_package.py"
        --tool "$<TARGET_FILE:manyforge_prepare_activation>"
        --workspace "${_MANYFORGE_REPOSITORY_SOURCE_ROOT}"
        --package-root "${_MANYFORGE_DUAL_ROBOT_BOOTSTRAP_BINARY_DIR}/dual-robot-scenario-fixture/retained-package"
        --publish-canonical-empty
        --scenario "examples/mock_dual_arm_shared_group.scenario=dual_robot_wall_cell")
    set_tests_properties(DualRobotCellBootstrapPackage PROPERTIES
      FIXTURES_SETUP DualRobotCellBootstrapPackage TIMEOUT 300)
    gtest_discover_tests(test_ros2_control_component_state
      PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${_LIB_PATHS}"
        TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS})
    gtest_discover_tests(test_host_component_runtime
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
    gtest_discover_tests(test_execution_event_stream
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

  endif()
endif()
