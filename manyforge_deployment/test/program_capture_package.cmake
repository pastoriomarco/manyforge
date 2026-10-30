# One real maintained closure for target-effect capture tests. Never weaken
# the exact no-unused-referent checks by bolting capabilities onto a mock.
foreach(required TOOL WORKSPACE BUILD_ROOT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing Program capture fixture input: ${required}")
  endif()
endforeach()
set(package "${BUILD_ROOT}/program_capture_package")
file(REMOVE_RECURSE "${package}")
file(MAKE_DIRECTORY "${package}")
# Every maintained family (robots and, since merge 6, their tools), each
# prepared by the build under generated/<family>_structural.
file(READ "${WORKSPACE}/manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
  description)
string(JSON family_count LENGTH "${description}" model_families)
math(EXPR family_last "${family_count} - 1")
set(prepared "")
foreach(index RANGE ${family_last})
  string(JSON family_id GET "${description}" model_families ${index} model_family_id)
  list(APPEND prepared --prepared
    "${family_id}=${BUILD_ROOT}/generated/${family_id}_structural")
endforeach()
execute_process(
  COMMAND "${TOOL}" --check --workspace-root "${WORKSPACE}" --package-root "${package}"
    ${prepared}
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "maintained Program capture fixture failed: ${result}")
endif()
