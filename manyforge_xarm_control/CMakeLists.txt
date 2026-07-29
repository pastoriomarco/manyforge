cmake_minimum_required(VERSION 3.14)
project(manyforge_xarm_control VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(ament_cmake REQUIRED)
find_package(controller_manager_msgs REQUIRED)
find_package(hardware_interface REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(manyforge_robot_lifecycle REQUIRED)
find_package(manyforge_robot_lifecycle_msgs REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(xarm_msgs REQUIRED)

add_library(${PROJECT_NAME}_core
  src/coordinator.cpp
  src/fake_hardware_backend.cpp
  src/lifecycle_ros_port.cpp
  src/lite6_lifecycle_catalog.cpp
  src/real_hardware_backend.cpp
  src/xarm_ros_transport_session.cpp
)
set_target_properties(${PROJECT_NAME}_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(${PROJECT_NAME}_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(${PROJECT_NAME}_core
  controller_manager_msgs
  lifecycle_msgs
  manyforge_robot_lifecycle
  rclcpp
  xarm_msgs
)

add_library(${PROJECT_NAME}_plugin SHARED
  src/system_hardware.cpp
)
target_include_directories(${PROJECT_NAME}_plugin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(${PROJECT_NAME}_plugin
  hardware_interface
  controller_manager_msgs
  lifecycle_msgs
  manyforge_robot_lifecycle
  pluginlib
  rclcpp
  rclcpp_lifecycle
)
target_link_libraries(${PROJECT_NAME}_plugin ${PROJECT_NAME}_core)

add_executable(lite6_supervised_preflight
  src/lite6_supervised_preflight.cpp
)
target_link_libraries(
  lite6_supervised_preflight
  ${PROJECT_NAME}_core
)
ament_target_dependencies(lite6_supervised_preflight rclcpp)

add_executable(lite6_lifecycle_command_evidence
  src/lite6_lifecycle_command_evidence.cpp
)
target_link_libraries(
  lite6_lifecycle_command_evidence
  ${PROJECT_NAME}_core
)
ament_target_dependencies(
  lite6_lifecycle_command_evidence
  manyforge_robot_lifecycle
  rclcpp
)

add_executable(lite6_execution_admission_provider
  src/lite6_execution_admission_provider.cpp
)
target_link_libraries(
  lite6_execution_admission_provider
  ${PROJECT_NAME}_core
)
ament_target_dependencies(
  lite6_execution_admission_provider
  manyforge_robot_lifecycle
  manyforge_robot_lifecycle_msgs
  rclcpp
)

pluginlib_export_plugin_description_file(
  hardware_interface
  manyforge_xarm_control_plugins.xml
)

install(
  DIRECTORY include/
  DESTINATION include
)
install(
  FILES manyforge_xarm_control_plugins.xml
  DESTINATION share/${PROJECT_NAME}
)
install(
  TARGETS
    ${PROJECT_NAME}_core
    ${PROJECT_NAME}_plugin
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(
  TARGETS
    lite6_execution_admission_provider
    lite6_lifecycle_command_evidence
    lite6_supervised_preflight
  DESTINATION lib/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  find_package(controller_manager REQUIRED)
  find_package(joint_trajectory_controller REQUIRED)
  find_package(trajectory_msgs REQUIRED)

  ament_add_gtest(test_end_to_end_fake
    test/test_end_to_end_fake.cpp
  )
  target_link_libraries(
    test_end_to_end_fake
    ${PROJECT_NAME}_core
  )

  ament_add_gtest(test_system_hardware_boundary
    test/test_system_hardware_boundary.cpp
  )
  target_link_libraries(
    test_system_hardware_boundary
    ${PROJECT_NAME}_plugin
    ${PROJECT_NAME}_core
  )
  ament_target_dependencies(
    test_system_hardware_boundary
    hardware_interface
    pluginlib
  )

  ament_add_gtest(test_real_hardware_backend
    test/test_real_hardware_backend.cpp
  )
  target_link_libraries(
    test_real_hardware_backend
    ${PROJECT_NAME}_core
  )

  ament_add_gtest(test_lite6_production_path
    test/test_lite6_production_path.cpp
  )
  target_link_libraries(
    test_lite6_production_path
    ${PROJECT_NAME}_core
  )

  ament_add_gtest(test_controller_manager_integration
    test/test_controller_manager_integration.cpp
  )
  target_link_libraries(
    test_controller_manager_integration
    ${PROJECT_NAME}_plugin
    ${PROJECT_NAME}_core
  )
  ament_target_dependencies(
    test_controller_manager_integration
    controller_manager
    controller_manager_msgs
    hardware_interface
    joint_trajectory_controller
    lifecycle_msgs
    rclcpp
    trajectory_msgs
  )

  ament_lint_auto_find_test_dependencies()
endif()

ament_export_dependencies(
  controller_manager_msgs
  hardware_interface
  lifecycle_msgs
  manyforge_robot_lifecycle
  manyforge_robot_lifecycle_msgs
  pluginlib
  rclcpp
  rclcpp_lifecycle
  xarm_msgs
)
ament_export_include_directories(include)
ament_export_libraries(
  ${PROJECT_NAME}_core
  ${PROJECT_NAME}_plugin
)
ament_export_targets(export_${PROJECT_NAME})
ament_package()
