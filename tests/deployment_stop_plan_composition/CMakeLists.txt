cmake_minimum_required(VERSION 3.18)
project(manyforge_deployment_stop_plan_composition LANGUAGES CXX)

include(CTest)

# Core supplies the production targets. Its independent
# tests run in the Core lane; this tree runs Deployment's Stop-plan composition
# tests against those real targets.
set(BUILD_TESTING OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../manyforge_core"
                 "${CMAKE_CURRENT_BINARY_DIR}/manyforge_core" EXCLUDE_FROM_ALL)

set(BUILD_TESTING ON)
set(MANYFORGE_BUILD_RESOLVED_STOP_BASE_PLAN_RESOLVER ON)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../manyforge_deployment"
                 "${CMAKE_CURRENT_BINARY_DIR}/manyforge_deployment")

if(NOT TARGET test_resolved_stop_base_plan_resolver)
  message(FATAL_ERROR "Deployment Stop-plan composition tests were not registered")
endif()
