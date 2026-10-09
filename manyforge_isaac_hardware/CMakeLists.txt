cmake_minimum_required(VERSION 3.16)
project(manyforge_isaac_hardware VERSION 0.1.0 LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(hardware_interface REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(rosgraph_msgs REQUIRED)
find_package(Threads REQUIRED)

add_library(${PROJECT_NAME} SHARED src/isaac_system.cpp)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(${PROJECT_NAME} Threads::Threads)
ament_target_dependencies(${PROJECT_NAME}
  hardware_interface pluginlib rclcpp rclcpp_lifecycle sensor_msgs rosgraph_msgs)

pluginlib_export_plugin_description_file(hardware_interface manyforge_isaac_hardware.xml)
install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}Targets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include)
install(FILES LICENSE README.md DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()

  ament_add_gtest(test_transport test/test_transport.cpp)
  target_compile_features(test_transport PRIVATE cxx_std_20)
  target_include_directories(test_transport PRIVATE src)
  target_link_libraries(test_transport Threads::Threads)
  ament_target_dependencies(test_transport sensor_msgs rosgraph_msgs)

  ament_add_gtest(test_isaac_system test/test_isaac_system.cpp)
  target_link_libraries(test_isaac_system ${PROJECT_NAME})
  ament_target_dependencies(test_isaac_system hardware_interface pluginlib rclcpp)
endif()

ament_export_targets(${PROJECT_NAME}Targets HAS_LIBRARY_TARGET)
ament_export_dependencies(hardware_interface pluginlib rclcpp rclcpp_lifecycle sensor_msgs rosgraph_msgs)
ament_package()
