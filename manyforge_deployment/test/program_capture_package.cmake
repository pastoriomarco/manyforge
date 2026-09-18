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
execute_process(
  COMMAND "${TOOL}" --check --workspace-root "${WORKSPACE}" --package-root "${package}"
    --prepared "lite6_lite_gripper=${BUILD_ROOT}/generated/lite6_lite_gripper_structural"
    --prepared "uf850_xarm_gripper=${BUILD_ROOT}/generated/uf850_xarm_gripper_structural"
    --prepared "ur10e_robotiq_2f_140=${BUILD_ROOT}/generated/ur10e_robotiq_2f_140_structural"
    --prepared "ur5_roboplan=${BUILD_ROOT}/generated/ur5_roboplan_structural"
    --prepared "ur5e_robotiq_2f_85=${BUILD_ROOT}/generated/ur5e_robotiq_2f_85_structural"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "maintained Program capture fixture failed: ${result}")
endif()
