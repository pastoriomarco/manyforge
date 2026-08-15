if(NOT DEFINED MANYFORGE_PROGRAM_REVISION_TOOL OR
   NOT DEFINED MANYFORGE_WORKSPACE_SOURCE_ROOT)
  message(FATAL_ERROR "maintained Program source test inputs are missing")
endif()

set(maintained_programs
  examples/isaac_lite6_lite_gripper_pick_and_place.program.yaml
  examples/isaac_manipulator_scene_light.program.yaml
  examples/isaac_manipulator_scene_light_collision.program.yaml
  examples/lite6_lite_gripper_pick_and_place.program.yaml
  examples/pick_and_place.program.yaml
  examples/pick_and_place_minimal.program.yaml
  examples/pick_and_place_ur10e_robotiq.program.yaml
  examples/semantic_shape_pick.program.yaml
  examples/trocar_lite6_foundationpose_pick.program.yaml
  examples/vision_guided_bin_pick.program.yaml
  examples/vision_guided_pose_pick.program.yaml)

foreach(relative_path IN LISTS maintained_programs)
  execute_process(
    COMMAND
      "${MANYFORGE_PROGRAM_REVISION_TOOL}"
      --check
      --input
      "${MANYFORGE_WORKSPACE_SOURCE_ROOT}/${relative_path}"
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE checked_revision
    ERROR_VARIABLE check_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT check_result EQUAL 0)
    message(FATAL_ERROR
      "${relative_path} failed revision verification: ${check_error}")
  endif()
  string(LENGTH "${checked_revision}" checked_revision_length)
  if(NOT checked_revision MATCHES "^sha256:[0-9a-f]+$" OR
     NOT checked_revision_length EQUAL 71)
    message(FATAL_ERROR "${relative_path} returned an invalid revision")
  endif()
endforeach()
