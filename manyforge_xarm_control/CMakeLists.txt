cmake_minimum_required(VERSION 3.14)
project(manyforge_xarm_control VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(xarm_msgs REQUIRED)

set(
  MANYFORGE_CORE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core"
  CACHE PATH
  "Path to the ROS-independent ManyForge Core source tree")
if(NOT TARGET manyforge_core)
  if(NOT EXISTS
     "${MANYFORGE_CORE_SOURCE_DIR}/include/manyforge_core/types/lifecycle_cause.hpp")
    message(FATAL_ERROR
      "manyforge_xarm_control requires the current Core lifecycle causes")
  endif()
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

# The xArm vendor adapter (R7): status classification, the robot_states
# transport, the recovery and tool context services and the lifecycle
# profiles.
add_library(${PROJECT_NAME}_brand_lifecycle SHARED
  src/lifecycle_profile.cpp
  src/xarm_brand_lifecycle_port.cpp
  src/xarm_driver_services.cpp
  src/xarm_robot_states_transport.cpp
  src/xarm_tool_context.cpp
)
target_include_directories(${PROJECT_NAME}_brand_lifecycle PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(${PROJECT_NAME}_brand_lifecycle
  rclcpp
  xarm_msgs
)
set_property(TARGET ${PROJECT_NAME}_brand_lifecycle APPEND PROPERTY
  LINK_LIBRARIES manyforge_core)
set_property(TARGET ${PROJECT_NAME}_brand_lifecycle APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES "$<BUILD_INTERFACE:manyforge_core>")

# V-A: ManyForge's generic vendor device adapter interface, header-only
# (`manyforge_roboplan/vendor_device_adapter.hpp`, Core types only): the xArm
# device adapter implements it without linking RoboPlan, whose Real-lane
# handle includes the same header from its own tree.
set(
  MANYFORGE_ROBOPLAN_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_roboplan"
  CACHE PATH
  "Path to the ManyForge RoboPlan source tree (the vendor device adapter interface)")
if(NOT TARGET manyforge_vendor_device_adapter_interface)
  if(NOT EXISTS
     "${MANYFORGE_ROBOPLAN_SOURCE_DIR}/include/manyforge_roboplan/vendor_device_adapter.hpp")
    message(FATAL_ERROR
      "manyforge_xarm_control requires the vendor device adapter interface")
  endif()
  add_library(manyforge_vendor_device_adapter_interface INTERFACE)
  target_include_directories(manyforge_vendor_device_adapter_interface INTERFACE
    "$<BUILD_INTERFACE:${MANYFORGE_ROBOPLAN_SOURCE_DIR}/include>")
  target_link_libraries(manyforge_vendor_device_adapter_interface INTERFACE
    manyforge_core)
endif()

# The xArm device adapter (R7 packages 3 and 4) and the xArm family's
# registry row (V-A), which RoboPlan's vendor registry lists.
add_library(${PROJECT_NAME}_device_adapter SHARED
  src/xarm_device_adapter.cpp
)
target_include_directories(${PROJECT_NAME}_device_adapter PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(${PROJECT_NAME}_device_adapter PUBLIC
  ${PROJECT_NAME}_brand_lifecycle
  "$<BUILD_INTERFACE:manyforge_vendor_device_adapter_interface>"
)

install(
  DIRECTORY include/
  DESTINATION include
)
install(
  TARGETS ${PROJECT_NAME}_brand_lifecycle ${PROJECT_NAME}_device_adapter
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  ament_add_gtest(test_xarm_brand_lifecycle_port
    test/test_xarm_brand_lifecycle_port.cpp
  )
  target_link_libraries(
    test_xarm_brand_lifecycle_port
    ${PROJECT_NAME}_brand_lifecycle
    manyforge_core
  )
  target_compile_definitions(test_xarm_brand_lifecycle_port PRIVATE
    MANYFORGE_XARM_CONTROL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )

  ament_add_gtest(test_xarm_robot_states_transport
    test/test_xarm_robot_states_transport.cpp
  )
  target_link_libraries(
    test_xarm_robot_states_transport
    ${PROJECT_NAME}_brand_lifecycle
    manyforge_core
  )

  ament_add_gtest(test_xarm_driver_services
    test/test_xarm_driver_services.cpp
  )
  target_link_libraries(
    test_xarm_driver_services
    ${PROJECT_NAME}_brand_lifecycle
    manyforge_core
  )

  ament_add_gtest(test_xarm_tool_context
    test/test_xarm_tool_context.cpp
  )
  target_link_libraries(
    test_xarm_tool_context
    ${PROJECT_NAME}_brand_lifecycle
    manyforge_core
  )

  ament_add_gtest(test_xarm_device_adapter
    test/test_xarm_device_adapter.cpp
  )
  target_link_libraries(
    test_xarm_device_adapter
    ${PROJECT_NAME}_device_adapter
    manyforge_core
  )

  ament_lint_auto_find_test_dependencies()
endif()

ament_export_dependencies(
  rclcpp
  xarm_msgs
)
ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME}_brand_lifecycle ${PROJECT_NAME}_device_adapter)
ament_export_targets(export_${PROJECT_NAME})
ament_package()
