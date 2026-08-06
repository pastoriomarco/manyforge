cmake_minimum_required(VERSION 3.14)
project(manyforge_e1_patch0007_ab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Eigen3 REQUIRED)
find_package(roboplan REQUIRED)
find_package(roboplan_example_models REQUIRED)
find_package(roboplan_rrt REQUIRED)

add_executable(e1_patch0007_collision_reuse_ab
  patch0007_collision_reuse_ab.cpp)
target_compile_options(e1_patch0007_collision_reuse_ab PRIVATE
  -Wall -Wextra -Werror -pedantic)
target_compile_definitions(e1_patch0007_collision_reuse_ab PRIVATE
  COAL_DISABLE_HPP_FCL_WARNINGS)
target_link_libraries(e1_patch0007_collision_reuse_ab PRIVATE
  Eigen3::Eigen
  roboplan::roboplan
  roboplan_example_models::roboplan_example_models
  roboplan_rrt::roboplan_rrt)
