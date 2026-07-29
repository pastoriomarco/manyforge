cmake_minimum_required(VERSION 3.14)
project(manyforge_robot_lifecycle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(ament_cmake REQUIRED)

add_library(${PROJECT_NAME}
  src/admission_publication_driver.cpp
  src/admission_projector.cpp
  src/local_operator_gateway.cpp
  src/supervisor.cpp
)
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

install(
  DIRECTORY include/
  DESTINATION include
)
install(
  TARGETS ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  ament_add_gtest(test_supervisor
    test/test_supervisor.cpp
  )
  target_link_libraries(test_supervisor ${PROJECT_NAME})

  ament_add_gtest(test_admission_projector
    test/test_admission_projector.cpp
  )
  target_link_libraries(test_admission_projector ${PROJECT_NAME})

  ament_add_gtest(test_admission_publication_driver
    test/test_admission_publication_driver.cpp
  )
  target_link_libraries(test_admission_publication_driver ${PROJECT_NAME})

  ament_add_gtest(test_local_operator_gateway
    test/test_local_operator_gateway.cpp
  )
  target_link_libraries(test_local_operator_gateway ${PROJECT_NAME})

  ament_lint_auto_find_test_dependencies()
endif()

ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME})
ament_export_targets(export_${PROJECT_NAME})
ament_package()
