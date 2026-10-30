cmake_minimum_required(VERSION 3.18)
project(manyforge_ua7wl_deployment_qualification LANGUAGES CXX)

include(CTest)

# Core supplies production journals. Its independent
# tests run in the Core lane; this tree qualifies Deployment's composition
# tests against those real targets.
set(BUILD_TESTING OFF)
set(MANYFORGE_CORE_BUILD_SQLITE_CCLC_JOURNAL ON)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../manyforge_core"
                 "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core" EXCLUDE_FROM_ALL)

set(BUILD_TESTING ON)
set(MANYFORGE_BUILD_RESOLVED_STOP_BASE_PLAN_RESOLVER ON)
set(MANYFORGE_BUILD_COMPONENT_LIFECYCLE_CURRENT_SNAPSHOT_SOURCE ON)
set(MANYFORGE_BUILD_CCLC_RUNTIME_COMPOSITION ON)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../manyforge_deployment"
                 "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment")

if(NOT TARGET test_resolved_stop_base_plan_resolver OR
   NOT TARGET test_cclc_runtime_composition)
  message(FATAL_ERROR "UA7W.L Deployment composition tests were not registered")
endif()
