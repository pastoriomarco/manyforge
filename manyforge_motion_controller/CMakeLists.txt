cmake_minimum_required(VERSION 3.14)
project(manyforge_motion_controller VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(ament_cmake REQUIRED)
find_package(controller_interface REQUIRED)
find_package(hardware_interface REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)

if(NOT TARGET manyforge_motion_numerics)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../manyforge_core/motion_numerics"
    "${CMAKE_CURRENT_BINARY_DIR}/manyforge_motion_numerics" EXCLUDE_FROM_ALL)
endif()
add_library(${PROJECT_NAME} SHARED src/motion_controller.cpp src/controller_mechanism.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE manyforge::motion_numerics)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--no-undefined)
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
ament_target_dependencies(${PROJECT_NAME} PUBLIC
  controller_interface hardware_interface pluginlib rclcpp rclcpp_lifecycle)
pluginlib_export_plugin_description_file(controller_interface controller_plugins.xml)

install(DIRECTORY include/ DESTINATION include)
install(TARGETS ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_copyright REQUIRED)
  find_package(ament_cmake_cpplint REQUIRED)
  find_package(ament_cmake_uncrustify REQUIRED)
  find_package(lifecycle_msgs REQUIRED)
  ament_add_gtest(test_motion_controller test/test_motion_controller.cpp)
  add_dependencies(test_motion_controller ${PROJECT_NAME})
  ament_target_dependencies(test_motion_controller
    controller_interface hardware_interface lifecycle_msgs pluginlib rclcpp rclcpp_lifecycle)
  find_package(Threads REQUIRED)
  ament_add_gtest(test_controller_mechanism test/test_controller_mechanism.cpp)
  target_include_directories(test_controller_mechanism PRIVATE src)
  target_link_libraries(test_controller_mechanism
    ${PROJECT_NAME} manyforge::motion_numerics Threads::Threads)
  ament_target_dependencies(test_controller_mechanism controller_interface hardware_interface rclcpp)
  ament_lint_auto_find_test_dependencies()
endif()

ament_export_dependencies(
  controller_interface hardware_interface pluginlib rclcpp rclcpp_lifecycle)
ament_export_include_directories(include)
ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_package()
