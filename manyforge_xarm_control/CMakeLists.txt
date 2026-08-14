cmake_minimum_required(VERSION 3.14)
project(manyforge_xarm_control VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(ament_cmake REQUIRED)
find_package(controller_manager_msgs REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(xarm_msgs REQUIRED)

set(
  MANYFORGE_CORE_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core"
  CACHE PATH
  "Path to the ROS-independent ManyForge Core source tree")
if(NOT TARGET manyforge_core)
  if(NOT EXISTS
     "${MANYFORGE_CORE_SOURCE_DIR}/include/manyforge_core/ports/ibrand_lifecycle_port.hpp")
    message(FATAL_ERROR
      "manyforge_xarm_control requires the current Core brand-lifecycle port")
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

set(
  MANYFORGE_LITE6_PRODUCT_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_lite6_product"
  CACHE PATH
  "Path to the ROS-independent Lite6 product source tree")
if(NOT TARGET manyforge_lite6_product)
  add_subdirectory(
    "${MANYFORGE_LITE6_PRODUCT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_lite6_product"
    EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET manyforge_core)
  message(FATAL_ERROR "manyforge_xarm_control could not resolve manyforge_core")
endif()

add_library(${PROJECT_NAME}_brand_lifecycle SHARED
  src/lite6_brand_lifecycle_catalog.cpp
  src/lite6_brand_lifecycle_port.cpp
  src/xarm_ros_transport_session.cpp
)
target_include_directories(${PROJECT_NAME}_brand_lifecycle PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(${PROJECT_NAME}_brand_lifecycle
  controller_manager_msgs
  lifecycle_msgs
  rclcpp
  xarm_msgs
)
set_property(TARGET ${PROJECT_NAME}_brand_lifecycle APPEND PROPERTY
  LINK_LIBRARIES manyforge_core manyforge_lite6_product)
set_property(TARGET ${PROJECT_NAME}_brand_lifecycle APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES
    "$<BUILD_INTERFACE:manyforge_core>"
    "$<BUILD_INTERFACE:manyforge_lite6_product>")

install(
  DIRECTORY include/
  DESTINATION include
)
install(
  DIRECTORY resources/
  DESTINATION share/${PROJECT_NAME}/resources
)
install(
  TARGETS ${PROJECT_NAME}_brand_lifecycle
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  ament_add_gtest(test_lite6_brand_lifecycle
    test/test_lite6_brand_lifecycle.cpp
  )
  target_link_libraries(
    test_lite6_brand_lifecycle
    ${PROJECT_NAME}_brand_lifecycle
    manyforge_core
  )
  target_compile_definitions(test_lite6_brand_lifecycle PRIVATE
    MANYFORGE_LITE6_LIFECYCLE_PRODUCT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )

  ament_add_gtest(test_xarm_ros_transport_session
    test/test_xarm_ros_transport_session.cpp
  )
  target_link_libraries(
    test_xarm_ros_transport_session
    ${PROJECT_NAME}_brand_lifecycle
  )

  ament_lint_auto_find_test_dependencies()
endif()

ament_export_dependencies(
  controller_manager_msgs
  lifecycle_msgs
  rclcpp
  xarm_msgs
)
ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME}_brand_lifecycle)
ament_export_targets(export_${PROJECT_NAME})
ament_package()
