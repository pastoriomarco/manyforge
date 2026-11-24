cmake_minimum_required(VERSION 3.14)

# The closed/orientation check of a triangle mesh (OD-363), shared by the
# collision engine's mesh preparation and the Manager's Apply preview. The
# check itself is coal-free; the reader loads a mesh file with assimp the way
# coal's mesh loader does. Added by manyforge_collision and manyforge_headless.

add_library(manyforge_mesh_topology src/mesh_topology.cpp)
target_include_directories(manyforge_mesh_topology PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(manyforge_mesh_topology PUBLIC cxx_std_20)
set_target_properties(manyforge_mesh_topology PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

# The reader only where assimp is installed (the engine needs none of it).
find_package(assimp QUIET)
if(assimp_FOUND)
  add_library(manyforge_mesh_file_topology src/mesh_file_topology.cpp)
  target_link_libraries(manyforge_mesh_file_topology
    PUBLIC manyforge_mesh_topology
    PRIVATE assimp::assimp)
  set_target_properties(manyforge_mesh_file_topology PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()
