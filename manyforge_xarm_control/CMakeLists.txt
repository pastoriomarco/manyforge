cmake_minimum_required(VERSION 3.14)
project(manyforge_xarm_control VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(xarm_msgs REQUIRED)
find_package(yaml-cpp REQUIRED)

set(
  MANYFORGE_CORE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core"
  CACHE PATH
  "Path to the ROS-independent ManyForge Core source tree")
if(NOT TARGET manyforge_core)
  set(_MANYFORGE_XARM_BUILD_TESTING "${BUILD_TESTING}")
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${MANYFORGE_CORE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core"
    EXCLUDE_FROM_ALL)
  set(BUILD_TESTING "${_MANYFORGE_XARM_BUILD_TESTING}")
  unset(_MANYFORGE_XARM_BUILD_TESTING)
endif()
if(NOT TARGET manyforge_core)
  message(FATAL_ERROR "manyforge_xarm_control could not resolve manyforge_core")
endif()

# The execution host's adapter interfaces this package implements
# (`launch_contribution.hpp`, `io_output_writer.hpp`: header-only, Core types
# only).
set(
  MANYFORGE_ROBOPLAN_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_roboplan"
  CACHE PATH
  "Path to the ManyForge RoboPlan source tree (the adapter interfaces)")

# The xArm vendor adapter: the machine owner,
# arm and I/O member adapters, the driver services, the status and I/O
# subscriptions and the launch contribution.
add_library(${PROJECT_NAME}
  src/xarm_adapters.cpp
  src/xarm_arm_adapter.cpp
  src/xarm_driver_services.cpp
  src/xarm_fault.cpp
  src/xarm_io_component.cpp
  src/xarm_io_states_transport.cpp
  src/xarm_launch_contribution.cpp
  src/xarm_owner_adapter.cpp
  src/xarm_robot_states_transport.cpp
  src/xarm_tool_context.cpp
)
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  "$<BUILD_INTERFACE:${MANYFORGE_ROBOPLAN_SOURCE_DIR}/include>"
)
target_link_libraries(${PROJECT_NAME}
  rclcpp::rclcpp
  ${xarm_msgs_TARGETS}
)
target_link_libraries(${PROJECT_NAME} yaml-cpp)
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY
  LINK_LIBRARIES manyforge_core)
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES "$<BUILD_INTERFACE:manyforge_core>")

install(
  DIRECTORY include/
  DESTINATION include
)
# The adapter folder: driver.yaml, manyforge_settings.yaml,
# README.md.
install(
  DIRECTORY vendor
  DESTINATION share/${PROJECT_NAME}
)
install(
  TARGETS ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

# The xArm adapter family, which the execution host loads from
# the ament resource index. It links the host's adapter runtime, so it is
# built inside the RoboPlan execution host graph only.
if(TARGET manyforge_roboplan_ros2_control_runtime)
  add_library(manyforge_xarm_adapter_family SHARED
    src/xarm_adapter_family.cpp)
  target_link_libraries(manyforge_xarm_adapter_family PRIVATE
    ${PROJECT_NAME}
    manyforge_roboplan_ros2_control_runtime
    ament_index_cpp::ament_index_cpp)
  manyforge_register_component_adapter_family(manyforge_xarm_adapter_family xarm)
  manyforge_stage_component_adapter_data(${PROJECT_NAME} vendor)
endif()

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_ros REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  # Each test process runs in its own ROS domain (Fast DDS #6599).
  set(MANYFORGE_ROS_TEST_ENV ENV ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST)

  foreach(test_name
      test_xarm_fault
      test_xarm_arm_adapter
      test_xarm_owner_adapter
      test_xarm_io_component
      test_xarm_robot_states_transport
      test_xarm_driver_services
      test_xarm_tool_context)
    ament_add_ros_isolated_gtest(${test_name} test/${test_name}.cpp ${MANYFORGE_ROS_TEST_ENV})
    target_link_libraries(${test_name} ${PROJECT_NAME} manyforge_core)
  endforeach()

  ament_add_ros_isolated_gtest(test_xarm_launch_contribution
    test/test_xarm_launch_contribution.cpp ${MANYFORGE_ROS_TEST_ENV})
  target_link_libraries(test_xarm_launch_contribution ${PROJECT_NAME} manyforge_core yaml-cpp)
  target_compile_definitions(test_xarm_launch_contribution PRIVATE
    MANYFORGE_XARM_CONTROL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")

  if(TARGET manyforge_xarm_adapter_family)
    ament_add_ros_isolated_gtest(test_xarm_adapter_family
      test/test_xarm_adapter_family.cpp ${MANYFORGE_ROS_TEST_ENV})
    target_link_libraries(test_xarm_adapter_family
      manyforge_roboplan_ros2_control_runtime manyforge_core yaml-cpp)
    target_compile_definitions(test_xarm_adapter_family PRIVATE
      MANYFORGE_XARM_ADAPTER_FAMILY_LIBRARY="$<TARGET_FILE:manyforge_xarm_adapter_family>"
      MANYFORGE_XARM_CONTROL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(test_xarm_adapter_family manyforge_xarm_adapter_family)
  endif()

  ament_lint_auto_find_test_dependencies()
endif()

ament_export_dependencies(
  ament_index_cpp
  rclcpp
  xarm_msgs
  yaml-cpp
)
ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME})
ament_export_targets(export_${PROJECT_NAME})
ament_package()
