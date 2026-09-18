#!/usr/bin/env python3
"""Validate the one maintained N6 deployment/model-family authoring input."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Final

import yaml

API_VERSION: Final = "manyforge.maintained_deployment_sources.v1"
PROGRAM_API_VERSION: Final = "manyforge.program.v2"
_EXPECTED_HISTORICAL_SOURCE_COMMIT: Final = "c4b075fa6dfb69e463c61f3efa8cfce8de681e8a"
_ID = re.compile(r"^[a-z][a-z0-9_.-]{0,127}$")
_HEX40 = re.compile(r"^[0-9a-f]{40}$")
_NATIVE_DEPLOYMENT_SOURCES: Final = frozenset(
    {
        "examples/assistant_modes_scene_authoring.deployment.yaml",
        "examples/lite6_lite_gripper_pick_and_place.deployment.yaml",
        "examples/pick_and_place.deployment.yaml",
        "examples/pick_and_place_ur10e_robotiq.deployment.yaml",
        "examples/semantic_shape_pick.deployment.yaml",
        "examples/vision_guided_bin_pick.deployment.yaml",
        "examples/vision_guided_pose_pick.deployment.yaml",
    }
)
_I9_DEPLOYMENT_SOURCES: Final = frozenset(
    {
        "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml",
        "examples/isaac_manipulator_scene_light_assistant.deployment.yaml",
        "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
        "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml",
        "examples/trocar_lite6_foundationpose_pick.deployment.yaml",
    }
)
_EXPECTED_DEPLOYMENT_SOURCES: Final = (
    _NATIVE_DEPLOYMENT_SOURCES | _I9_DEPLOYMENT_SOURCES
)
_EXPECTED_I9_ROOT_POSES_IN_CELL: Final = {
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": {
        "position_m": [0.0, 0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    },
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": {
        "position_m": [-0.21814000606536865, -0.571590006351471, 0.12999999523162842],
        "orientation_xyzw": [0.0, 0.0, -0.7071067690846544, 0.7071067932884405],
    },
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": {
        "position_m": [-0.21814000606536865, -0.571590006351471, 0.12999999523162842],
        "orientation_xyzw": [0.0, 0.0, -0.7071067690846544, 0.7071067932884405],
    },
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": {
        "position_m": [-0.21814000606536865, -0.571590006351471, 0.12999999523162842],
        "orientation_xyzw": [0.0, 0.0, -0.7071067690846544, 0.7071067932884405],
    },
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": {
        "position_m": [0.0, 0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    },
}
_EXPECTED_HISTORICAL_BLOBS: Final = {
    "examples/assistant_modes_scene_authoring.deployment.yaml": (
        "7149aba89619f3f1c426148ef2f2972cd0c730f0"
    ),
    "examples/lite6_lite_gripper_pick_and_place.deployment.yaml": (
        "d0141f0042f3b7af5850e651befb3e2550d907c0"
    ),
    "examples/pick_and_place.deployment.yaml": "bee699e23c42a7b8810dbf27b1f8bd261f514834",
    "examples/pick_and_place_ur10e_robotiq.deployment.yaml": (
        "f31c1af91814b9da7a1d121531c0c3c814001781"
    ),
    "examples/semantic_shape_pick.deployment.yaml": "115ab078116696847a9c037f0b20d5b3e39b1741",
    "examples/vision_guided_bin_pick.deployment.yaml": "332b8b6daa93cf4b0533e93a42736fd67b8654c5",
    "examples/vision_guided_pose_pick.deployment.yaml": "a65a0011ee0247cb3bb0281360fa4c46073bbf9d",
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": (
        "cb66876a3af94a41836fa4b20bae9cd4d38d405b"
    ),
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": (
        "fbf2ec5572e3aec68fef1644515396950cbd6a9f"
    ),
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": (
        "0178a5f9d2d1ee603cf63feddafa68d14e707c43"
    ),
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": (
        "aa9b85224e9590e26ebf59fd753ed17ba972e393"
    ),
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": (
        "df371a183e2f8340071a34ec41e646814f99995f"
    ),
}
_EXPECTED_I9_HISTORICAL_PROGRAMS: Final = {
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": {
        "source": "examples/isaac_lite6_lite_gripper_pick_and_place.program.yaml",
        "blob": "f9df086c34a89a4bab38da348fa01702bbe3e67c",
    },
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": {
        "source": "examples/isaac_manipulator_scene_light.program.yaml",
        "blob": "a6bde14809eb17a2ac812e089b75495c823e4ec3",
    },
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": {
        "source": "examples/isaac_manipulator_scene_light.program.yaml",
        "blob": "a6bde14809eb17a2ac812e089b75495c823e4ec3",
    },
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": {
        "source": "examples/isaac_manipulator_scene_light_collision.program.yaml",
        "blob": "e10e0575e8365034b99ade64dc1157a0e0ed7a74",
    },
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": {
        "source": "examples/trocar_lite6_foundationpose_pick.program.yaml",
        "blob": "f492ed998341143e8733bf193dc29b2492904615",
    },
}
_EXPECTED_HISTORICAL_PROGRAM_BLOBS: Final = {
    "examples/lite6_lite_gripper_pick_and_place.program.yaml": (
        "a1966be7d0b655ff852a71c45ff6a8dfbc279e20"
    ),
    "examples/pick_and_place.program.yaml": "2ec800075c5373ccb0b4ac50b34eb56289b82fdc",
    "examples/pick_and_place_minimal.program.yaml": (
        "bedd3b63135d5156ac729e8e2f3346aa267c26ef"
    ),
    "examples/pick_and_place_ur10e_robotiq.program.yaml": (
        "4892381f2eb75ead0f8ffdfbacc74504214a3db4"
    ),
    "examples/semantic_shape_pick.program.yaml": (
        "82aaf828329c3d3f9acb5524358a827b57fd636b"
    ),
    "examples/vision_guided_bin_pick.program.yaml": (
        "c9ac59881ff64f5d9572281c4251655b2f4d3dfa"
    ),
    "examples/vision_guided_pose_pick.program.yaml": (
        "4e18c00ae08d74effd89d88c37be2f2b9e5d0b2d"
    ),
}

_RUNTIME_RESTORATION_SOURCE_FIELDS: Final = {
    "collision_monitor_period_ms": "orchestrator.config.collision_monitor_period_ms",
    "default_fault_stop_profile": (
        "adapters.execution.config.default_fault_stop_profile"
    ),
    "direct_control_joint_limits": "robot_config.robot_control_joint_limits",
    "execution_tick_hz": "adapters.execution.config.execution_tick_hz",
    "initial_joint_positions_rad": (
        "adapters.execution.config.initial_joint_positions_rad"
    ),
    "mode": "adapters.execution.config.mode",
    "motion_target_limits": (
        "deployment.motion_target_limits[].maximum_tcp_linear_speed_mps"
    ),
    "operator_stop_profile": "adapters.execution.config.operator_stop_profile",
    "planner_rng_seed": "adapters.planning.config.rng_seed",
}
_RUNTIME_RESTORATION_ACCEPTANCE: Final = [
    {
        "obligation_id": "retained_native_initial_state_and_execution_cadence",
        "acceptance_steps": ["N6-a3", "N7"],
    },
    {
        "obligation_id": "retained_native_planner_seed",
        "acceptance_steps": ["N6-a3", "N7"],
    },
    {
        "obligation_id": "retained_native_stop_profiles",
        "acceptance_steps": ["I8/P5"],
    },
    {
        "obligation_id": "retained_runtime_collision_monitor_period",
        "acceptance_steps": ["N6-a3", "N7", "I8/P5"],
    },
]
_UI6I_RUNTIME_RESTORATION_ACCEPTANCE: Final = [
    {
        "obligation_id": "retained_native_initial_state_and_execution_cadence",
        "acceptance_steps": ["UI6-I/T3"],
    },
    {
        "obligation_id": "retained_native_planner_seed",
        "acceptance_steps": ["UI6-I/T3"],
    },
    {
        "obligation_id": "retained_native_stop_profiles",
        "acceptance_steps": ["UI6-I/T4"],
    },
    {
        "obligation_id": "retained_runtime_collision_monitor_period",
        "acceptance_steps": ["UI6-I/T3", "UI6-I/T4"],
    },
]
_ROS2_CONTROL_RUNTIME_RESTORATION_SOURCE_FIELDS: Final = {
    "collision_monitor_period_ms": "orchestrator.config.collision_monitor_period_ms",
    "default_fault_stop_profile": (
        "adapters.execution.config.default_fault_stop_profile"
    ),
    "execution_tick_hz": "retained_runtime_defaults.execution_tick_hz",
    "initial_joint_positions_rad": (
        "adapters.execution.config.initial_joint_positions_rad"
    ),
    "isaac_profile": "robot_config.robot_id",
    "mode": "adapters.execution.config.execution_state_source_mode",
    "operator_stop_profile": "adapters.execution.config.operator_stop_profile",
    "planner_rng_seed": "adapters.planning.config.rng_seed",
    "runtime_collision_immediate_query_budget_ms": (
        "orchestrator.config.runtime_collision_immediate_query_budget_ms"
    ),
    "runtime_collision_lookahead_cycle_budget_ms": (
        "orchestrator.config.runtime_collision_lookahead_cycle_budget_ms"
    ),
    "runtime_collision_lookahead_max_samples": (
        "orchestrator.config.runtime_collision_lookahead_max_samples"
    ),
    "runtime_collision_lookahead_query_budget_ms": (
        "orchestrator.config.runtime_collision_lookahead_query_budget_ms"
    ),
    "runtime_collision_lookahead_s": (
        "orchestrator.config.runtime_collision_lookahead_s"
    ),
    "runtime_collision_lookahead_workers": (
        "orchestrator.config.runtime_collision_lookahead_workers"
    ),
    "runtime_dynamic_body_max_age_cap_ms": (
        "orchestrator.config.runtime_dynamic_body_max_age_cap_ms"
    ),
    "validation_collision_workers": (
        "orchestrator.config.validation_collision_workers"
    ),
}
_ROS2_CONTROL_RUNTIME_RESTORATION_ACCEPTANCE: Final = [
    {
        "obligation_id": "retained_isaac_initial_state_and_execution_cadence",
        "acceptance_steps": ["I9/I"],
    },
    {
        "obligation_id": "retained_isaac_planner_seed",
        "acceptance_steps": ["I9/I"],
    },
    {
        "obligation_id": "retained_ros2_control_stop_profiles",
        "acceptance_steps": ["I8/P5", "I9/I"],
    },
    {
        "obligation_id": "retained_runtime_collision_monitor_period",
        "acceptance_steps": ["I9/I"],
    },
    {
        "obligation_id": "retained_isaac_runtime_collision_tuning",
        "acceptance_steps": ["I9/I"],
    },
    {
        "obligation_id": "commanded_simulation_state_authority",
        "acceptance_steps": ["I9/I"],
    },
    {
        "obligation_id": "exact_isaac_ros2_control_profile_and_transport",
        "acceptance_steps": ["I9/I"],
    },
]
_EXPECTED_RUNTIME_RESTORATION_PROFILE_DIGESTS: Final = {
    "lite6_isaac_commanded_v0": (
        "5465175c487df3cc2cc34ef714b3372550bec75396cbb24809f3a5140dab9ed1"
    ),
    "lite6_native_simulated_v0": (
        "de7787ba7631291e7701432b4f1a14d0296082af345c0a6197acc42dde2ec425"
    ),
    "ur10e_robotiq_assistant_isaac_commanded_v0": (
        "c334ba022f55d83199ef995747d778016d498753cbf28dec99d22bc9de6f7330"
    ),
    "ur10e_robotiq_assistant_native_simulated_v0": (
        "ebc4ae29cb59420a463895a8b2f43d7155f56c320e8bdcc0794f9f6deda10613"
    ),
    "ur10e_robotiq_isaac_commanded_v0": (
        "8f1205f803354b32c887ee9db8ef98ab3a599e30cb5b22dbfb0aad4e83b7c3a8"
    ),
    "ur10e_robotiq_native_simulated_v0": (
        "1bf900ab5269eadf578a5344bf87d974a02c3ac52131db5f2e3e9533f7822f0a"
    ),
    "ur5_native_simulated_v0": (
        "f0c73afe865b8952c82310ef47f9db62ccd2ab5c7002fc76262be7f07f1abac5"
    ),
    "ur5e_robotiq_native_simulated_v0": (
        "3f500ed608e42f2855cb45a74b1fc940ce68f20e0f1758b9259f382e56f4364f"
    ),
}
_EXPECTED_RUNTIME_RESTORATION_PROFILE_FAMILIES: Final = {
    "lite6_isaac_commanded_v0": "lite6_lite_gripper",
    "lite6_native_simulated_v0": "lite6_lite_gripper",
    "ur10e_robotiq_assistant_isaac_commanded_v0": "ur10e_robotiq_2f_140",
    "ur10e_robotiq_assistant_native_simulated_v0": "ur10e_robotiq_2f_140",
    "ur10e_robotiq_isaac_commanded_v0": "ur10e_robotiq_2f_140",
    "ur10e_robotiq_native_simulated_v0": "ur10e_robotiq_2f_140",
    "ur5_native_simulated_v0": "ur5_roboplan",
    "ur5e_robotiq_native_simulated_v0": "ur5e_robotiq_2f_85",
}
_EXPECTED_DEPLOYMENT_RUNTIME_PROFILE_IDS: Final = {
    "examples/assistant_modes_scene_authoring.deployment.yaml": (
        "ur10e_robotiq_assistant_native_simulated_v0"
    ),
    "examples/lite6_lite_gripper_pick_and_place.deployment.yaml": (
        "lite6_native_simulated_v0"
    ),
    "examples/pick_and_place.deployment.yaml": "ur5_native_simulated_v0",
    "examples/pick_and_place_ur10e_robotiq.deployment.yaml": (
        "ur10e_robotiq_native_simulated_v0"
    ),
    "examples/semantic_shape_pick.deployment.yaml": "ur5_native_simulated_v0",
    "examples/vision_guided_bin_pick.deployment.yaml": "ur5_native_simulated_v0",
    "examples/vision_guided_pose_pick.deployment.yaml": "ur5_native_simulated_v0",
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": (
        "lite6_isaac_commanded_v0"
    ),
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": (
        "ur10e_robotiq_assistant_isaac_commanded_v0"
    ),
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": (
        "ur10e_robotiq_isaac_commanded_v0"
    ),
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": (
        "ur10e_robotiq_isaac_commanded_v0"
    ),
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": (
        "lite6_isaac_commanded_v0"
    ),
}
_EXPECTED_OPEN_LOOP_SOURCES: Final = frozenset(
    {
        "examples/assistant_modes_scene_authoring.deployment.yaml",
        "examples/lite6_lite_gripper_pick_and_place.deployment.yaml",
        "examples/pick_and_place_ur10e_robotiq.deployment.yaml",
        *_I9_DEPLOYMENT_SOURCES,
    }
)
_EXPECTED_ASSISTANT_PROFILES_SOURCE: Final = (
    "manyforge_deployment/resources/maintained/maintained_assistant_profiles.v1.json"
)
_EXPECTED_ASSISTANT_PROFILE_IDS: Final = {
    "examples/assistant_modes_scene_authoring.deployment.yaml": (
        "assistant_scene_authoring"
    ),
    "examples/lite6_lite_gripper_pick_and_place.deployment.yaml": None,
    "examples/pick_and_place.deployment.yaml": None,
    "examples/pick_and_place_ur10e_robotiq.deployment.yaml": None,
    "examples/semantic_shape_pick.deployment.yaml": "semantic_shape_selection",
    "examples/vision_guided_bin_pick.deployment.yaml": "vision_bin_selection",
    "examples/vision_guided_pose_pick.deployment.yaml": "vision_pose_selection",
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": None,
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": (
        "assistant_scene_authoring"
    ),
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": None,
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": None,
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": None,
}
_EXPECTED_COMPATIBILITY: Final = {
    "examples/assistant_modes_scene_authoring.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": ["robotiq_2f_140_gripper", "scene_authoring_tools"],
        "tags": ["robotiq", "scene-authoring", "ur10e"],
    },
    "examples/lite6_lite_gripper_pick_and_place.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": ["lite_gripper"],
        "tags": ["lite6", "pick-and-place"],
    },
    "examples/pick_and_place.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": [],
        "tags": ["pick-and-place", "ur5"],
    },
    "examples/pick_and_place_ur10e_robotiq.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": ["robotiq_2f_140_gripper"],
        "tags": ["pick-and-place", "robotiq", "ur10e"],
    },
    "examples/semantic_shape_pick.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": ["semantic_shape_inference"],
        "tags": ["semantic-pick", "ur5"],
    },
    "examples/vision_guided_bin_pick.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": ["vision_pose_inference"],
        "tags": ["bin-pick", "ur5", "vision-pick"],
    },
    "examples/vision_guided_pose_pick.deployment.yaml": {
        "lane": "manyforge-only",
        "provides": ["vision_pose_inference"],
        "tags": ["ur5", "vision-pick"],
    },
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": {
        "lane": "isaac-sim",
        "provides": ["lite_gripper", "ros2_control"],
        "tags": ["isaac-sim", "lite6", "pick-and-place"],
    },
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": {
        "lane": "isaac-sim",
        "provides": [
            "robotiq_2f_140_gripper",
            "ros2_control",
            "scene_authoring_tools",
        ],
        "tags": ["assistant", "isaac-sim", "pick-and-place", "robotiq", "ur10e"],
    },
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": {
        "lane": "isaac-sim",
        "provides": ["robotiq_2f_140_gripper", "ros2_control"],
        "tags": ["isaac-sim", "pick-and-place", "robotiq", "ur10e"],
    },
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": {
        "lane": "isaac-sim",
        "provides": ["robotiq_2f_140_gripper", "ros2_control"],
        "tags": ["isaac-sim", "robotiq", "runtime-collision", "ur10e"],
    },
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": {
        "lane": "isaac-sim",
        "provides": ["foundationpose", "lite_gripper", "ros2_control"],
        "tags": ["foundationpose", "isaac-sim", "lite6", "pick-and-place"],
    },
}
_EXPECTED_I9_BEHAVIOR_OBLIGATIONS: Final = {
    "examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml": [
        "articulated_lite_gripper_effort_mimic_and_observed_state",
        "commanded_sim_arm_jtc_and_joint_state_authority",
        "contact_force_collision_and_touch_link_behavior",
        "scene_load_reset_world_readback_and_object_motion",
        "viewer_ghost_status_and_gripper_convergence",
    ],
    "examples/isaac_manipulator_scene_light_assistant.deployment.yaml": [
        "assistant_scene_inspection_and_operator_approved_mutation",
        "commanded_sim_arm_jtc_and_joint_state_authority",
        "scene_load_reset_world_readback_and_object_motion",
        "viewer_mesh_ghost_reset_and_structured_status",
    ],
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml": [
        "commanded_sim_arm_jtc_and_joint_state_authority",
        "robotiq_gripper_command_feedback_and_convergence",
        "scene_load_reset_world_readback_and_object_motion",
        "viewer_mesh_ghost_reset_and_structured_status",
    ],
    "examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml": [
        "commanded_sim_arm_jtc_and_joint_state_authority",
        "pose_source_snapshot_and_runtime_collision_stop",
        "scene_load_reset_world_readback_and_object_motion",
        "structured_stop_trace_status_and_alarm_correlation",
    ],
    "examples/trocar_lite6_foundationpose_pick.deployment.yaml": [
        "articulated_lite_gripper_effort_mimic_and_observed_state",
        "camera_tf_and_foundationpose_detection_source",
        "commanded_sim_arm_jtc_and_joint_state_authority",
        "pose_source_snapshot_pick_and_drop",
        "scene_load_reset_world_readback_and_object_motion",
    ],
}
_EXPECTED_ASSISTANT_PROFILE_CONTRACTS: Final = {
    "assistant_scene_authoring": {
        "mode_profile_id": "scene_authoring",
        "endpoint_aliases": ("assistant_model",),
        "historical_profiles": ("thor_gemma4_12b_gguf",),
        "provider_kind": "nemoclaw",
    },
    "semantic_shape_selection": {
        "mode_profile_id": "scene_authoring",
        "endpoint_aliases": ("assistant_model", "selection_model"),
        "historical_profiles": (
            "thor_cosmos_reason2_8b",
            "thor_manyforge_qwen36",
        ),
        "provider_kind": "direct",
    },
    "vision_bin_selection": {
        "mode_profile_id": None,
        "endpoint_aliases": ("vision_model",),
        "historical_profiles": ("thor_manyforge_qwen36",),
        "provider_kind": None,
    },
    "vision_pose_selection": {
        "mode_profile_id": "scene_authoring",
        "endpoint_aliases": ("assistant_model", "vision_model"),
        "historical_profiles": (
            "thor_cosmos_reason2_8b",
            "thor_cosmos_reason2_8b",
        ),
        "provider_kind": "direct",
    },
}
_EXPECTED_ASSISTANT_REQUEST_CONTRACTS: Final = {
    ("assistant_scene_authoring", "assistant_model"): {
        "default_timeout_s": 180.0,
        "max_output_tokens": 8192,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": False,
        "supported_image_transports": [],
        "tool_call_family": "openai_chat_tools",
    },
    ("semantic_shape_selection", "assistant_model"): {
        "default_timeout_s": 180.0,
        "max_output_tokens": 8192,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
    ("semantic_shape_selection", "selection_model"): {
        "default_timeout_s": 30.0,
        "max_output_tokens": 384,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": False,
        "supported_image_transports": [],
        "tool_call_family": "openai_chat_tools",
    },
    ("vision_bin_selection", "vision_model"): {
        "default_timeout_s": 30.0,
        "max_output_tokens": 512,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
    ("vision_pose_selection", "assistant_model"): {
        "default_timeout_s": 180.0,
        "max_output_tokens": 8192,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
    ("vision_pose_selection", "vision_model"): {
        "default_timeout_s": 120.0,
        "max_output_tokens": 512,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
}
_EXPECTED_ASSISTANT_PROMPT_CONTRACTS: Final = {
    ("semantic_shape_selection", "select_scene_object_v1"): {
        "version": "1",
        "template_text_sha256": (
            "f5acfe169fa25ab07f94ec2711cf8872a96a869def7275a9b3be8f46418d83ef"
        ),
        "variable_ids": (
            "candidate_object_ids",
            "scene_context_json",
            "selection_request_text",
        ),
        "response_contract": {
            "response_mode": "tool_call",
            "tool_name": "select_scene_object",
            "tool_description": (
                "Choose one candidate object from the current runtime scene context."
            ),
        },
    },
    ("vision_bin_selection", "choose_bin_target_v1"): {
        "version": "1",
        "template_text_sha256": (
            "8a1f6703c7aadaa2c427c43932a4b7c6bfe45468a02a221f130917a90f5e6314"
        ),
        "variable_ids": (),
        "response_contract": {"response_mode": "json_object"},
    },
    ("vision_pose_selection", "infer_grasp_pose_v1"): {
        "version": "1",
        "template_text_sha256": (
            "29888cd8ca46c597e6fae283d3f7e2512092fe9f1a9874781c46da5754516c49"
        ),
        "variable_ids": (),
        "response_contract": {"response_mode": "json_object"},
    },
}
_EXPECTED_MODEL_FAMILIES: Final = {
    "lite6_lite_gripper": {
        "frames": {
            "base": "link_base",
            "flange": "link_eef",
            "gripper_tcp": "link_tcp",
            "root": "world",
            "tcp": "link_tcp",
        },
        "groups": {"arm": "lite6", "gripper": "lite_gripper"},
        "resources": {
            "arm_joints": ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6"),
            "fingers": ("right_finger_joint",),
        },
        "collisions": {
            "arm_collision": (
                "link1",
                "link2",
                "link3",
                "link4",
                "link5",
                "link6",
                "link_base",
            ),
            "gripper_collision": (
                "lite_gripper_base_link",
                "lite_gripper_left_finger",
                "lite_gripper_right_finger",
            ),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/lite6_lite_gripper/",
            "historical_srdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.srdf",
            "historical_urdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf",
            "resource_root": "manyforge_example_models/models/lite6_lite_gripper",
            "srdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.srdf",
            "srdf_projection_kind": "identity",
            "urdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf",
            "urdf_projection_kind": "identity",
        },
    },
    "uf850_xarm_gripper": {
        "frames": {
            "base": "link_base",
            "flange": "link_eef",
            "gripper_tcp": "link_tcp",
            "root": "world",
            "tcp": "link_tcp",
        },
        "groups": {"arm": "uf850", "gripper": "uf850_gripper"},
        "resources": {
            "arm_joints": ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6"),
            "fingers": ("drive_joint",),
        },
        "collisions": {
            "arm_collision": (
                "link1",
                "link2",
                "link3",
                "link4",
                "link5",
                "link6",
                "link_base",
            ),
            "gripper_collision": (
                "left_finger",
                "left_inner_knuckle",
                "left_outer_knuckle",
                "right_finger",
                "right_inner_knuckle",
                "right_outer_knuckle",
                "xarm_gripper_base_link",
            ),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/uf850_xarm_gripper/",
            "historical_srdf_path": "manyforge_example_models/models/uf850_xarm_gripper/uf850_xarm_gripper.srdf",
            "historical_urdf_path": "manyforge_example_models/models/uf850_xarm_gripper/uf850_xarm_gripper.urdf",
            "resource_root": "manyforge_example_models/models/uf850_xarm_gripper",
            "srdf_path": "manyforge_example_models/models/uf850_xarm_gripper/uf850_xarm_gripper.srdf",
            "srdf_projection_kind": "identity",
            "urdf_path": "manyforge_example_models/models/uf850_xarm_gripper/uf850_xarm_gripper.urdf",
            "urdf_projection_kind": "identity",
        },
    },
    "ur10e_robotiq_2f_140": {
        "frames": {
            "base": "base_link",
            "flange": "flange",
            "gripper_tcp": "grasp_frame",
            "tcp": "grasp_frame",
            "tool_mount": "tool0",
        },
        "groups": {"arm": "arm", "gripper": "gripper"},
        "resources": {
            "arm_joints": (
                "shoulder_pan_joint",
                "shoulder_lift_joint",
                "elbow_joint",
                "wrist_1_joint",
                "wrist_2_joint",
                "wrist_3_joint",
            ),
            "fingers": ("finger_joint",),
        },
        "collisions": {
            "arm_collision": (
                "base_link_inertia",
                "forearm_link",
                "shoulder_link",
                "upper_arm_link",
                "wrist_1_link",
                "wrist_2_link",
                "wrist_3_link",
            ),
            "gripper_collision": (
                "left_inner_finger",
                "left_inner_finger_pad",
                "left_inner_knuckle",
                "left_outer_finger",
                "left_outer_knuckle",
                "right_inner_finger",
                "right_inner_finger_pad",
                "right_inner_knuckle",
                "right_outer_finger",
                "right_outer_knuckle",
                "robotiq_base_link",
            ),
        },
        "root": "base",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/ur10e_robotiq_2f_140/",
            "historical_srdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.srdf",
            "historical_urdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.urdf",
            "resource_root": "manyforge_example_models/models/ur10e_robotiq_2f_140",
            "srdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.structural.srdf",
            "srdf_projection_kind": "remove_hpp_gripper_extension",
            "urdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.structural.urdf",
            "urdf_projection_kind": "remove_unsupported_collision_materials",
        },
    },
    "ur5_roboplan": {
        "frames": {
            "base": "base_link",
            "flange": "ee_link",
            "gripper_tcp": "tool0",
            "root": "world",
            "tcp": "tool0",
        },
        "groups": {"arm": "arm"},
        "resources": {
            "arm_joints": (
                "shoulder_pan_joint",
                "shoulder_lift_joint",
                "elbow_joint",
                "wrist_1_joint",
                "wrist_2_joint",
                "wrist_3_joint",
            )
        },
        "collisions": {
            "arm_collision": (
                "base_link",
                "forearm_link",
                "shoulder_link",
                "upper_arm_link",
                "wrist_1_link",
                "wrist_2_link",
                "wrist_3_link",
            ),
            "gripper_collision": ("ee_link", "tool0"),
            "tool_collision": ("ee_link", "tool0"),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://roboplan_example_models/models/ur_robot_model/",
            "historical_srdf_path": ".docker/roboplan/roboplan_example_models/models/ur_robot_model/ur5_gripper.srdf",
            "historical_urdf_path": ".docker/roboplan/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf",
            "resource_root": ".docker/roboplan/roboplan_example_models/models/ur_robot_model",
            "srdf_path": "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/ur5_gripper.structural.srdf",
            "srdf_projection_kind": "remove_hpp_gripper_extension",
            "urdf_path": "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/ur5_gripper.structural.urdf",
            "urdf_projection_kind": "remove_unused_xacro_namespace",
        },
    },
    "ur5e_robotiq_2f_85": {
        "frames": {
            "base": "base_link",
            "flange": "flange",
            "gripper_tcp": "robotiq_85_base_link",
            "root": "world",
            "tcp": "tool0",
            "tool_mount": "tool0",
        },
        "groups": {"arm": "ur_manipulator", "gripper": "robotiq_2f_85_gripper"},
        "resources": {
            "arm_joints": (
                "shoulder_pan_joint",
                "shoulder_lift_joint",
                "elbow_joint",
                "wrist_1_joint",
                "wrist_2_joint",
                "wrist_3_joint",
            ),
            "fingers": ("robotiq_85_left_knuckle_joint",),
        },
        "collisions": {
            "arm_collision": (
                "base_link_inertia",
                "forearm_link",
                "shoulder_link",
                "upper_arm_link",
                "wrist_1_link",
                "wrist_2_link",
                "wrist_3_link",
            ),
            "gripper_collision": (
                "robotiq_85_base_link",
                "robotiq_85_left_finger_link",
                "robotiq_85_left_finger_tip_link",
                "robotiq_85_left_inner_knuckle_link",
                "robotiq_85_left_knuckle_link",
                "robotiq_85_right_finger_link",
                "robotiq_85_right_finger_tip_link",
                "robotiq_85_right_inner_knuckle_link",
                "robotiq_85_right_knuckle_link",
            ),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/ur5e_robotiq_2f_85/",
            "historical_srdf_path": "manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.srdf",
            "historical_urdf_path": "manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.urdf",
            "resource_root": "manyforge_example_models/models/ur5e_robotiq_2f_85",
            "srdf_path": "manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.srdf",
            "srdf_projection_kind": "identity",
            "urdf_path": "manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.urdf",
            "urdf_projection_kind": "identity",
        },
    },
}


class SourceDescriptionError(ValueError):
    """The maintained authoring description is malformed or stale."""


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise SourceDescriptionError(f"duplicate JSON member {name!r}")
        result[name] = value
    return result


def _object(value: object, *, path: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise SourceDescriptionError(f"{path} must be an object")
    return value


def _array(value: object, *, path: str) -> list[Any]:
    if type(value) is not list:
        raise SourceDescriptionError(f"{path} must be an array")
    return value


def _exact(
    value: object,
    *,
    required: frozenset[str],
    path: str,
) -> dict[str, Any]:
    row = _object(value, path=path)
    missing = sorted(required - set(row))
    unknown = sorted(set(row) - required)
    if missing or unknown:
        raise SourceDescriptionError(
            f"{path} has wrong fields; missing={missing}, unknown={unknown}"
        )
    return row


def _text(value: object, *, path: str) -> str:
    if type(value) is not str or not value.strip() or len(value) > 4096:
        raise SourceDescriptionError(f"{path} must be bounded non-empty text")
    return value


def _identifier(value: object, *, path: str) -> str:
    text = _text(value, path=path)
    if _ID.fullmatch(text) is None:
        raise SourceDescriptionError(f"{path} must be a portable identifier")
    return text


def _source_path(value: object, *, root: Path, path: str) -> str:
    text = _text(value, path=path)
    candidate = Path(text)
    if (
        candidate.is_absolute()
        or ".." in candidate.parts
        or text != candidate.as_posix()
    ):
        raise SourceDescriptionError(f"{path} must be a root-local normalized path")
    resolved = root / candidate
    if resolved.is_symlink() or not resolved.is_file():
        raise SourceDescriptionError(f"{path} does not select a plain source file")
    return text


def _directory_path(value: object, *, root: Path, path: str) -> str:
    text = _text(value, path=path)
    candidate = Path(text)
    if (
        candidate.is_absolute()
        or ".." in candidate.parts
        or text != candidate.as_posix()
    ):
        raise SourceDescriptionError(f"{path} must be a root-local normalized path")
    resolved = root / candidate
    if resolved.is_symlink() or not resolved.is_dir():
        raise SourceDescriptionError(f"{path} does not select a plain source directory")
    return text


def _source_symbol(value: object, *, path: str) -> str:
    text = _text(value, path=path)
    if len(text) > 1024 or any(
        character.isspace() or not character.isprintable() for character in text
    ):
        raise SourceDescriptionError(f"{path} must be one bounded source symbol")
    return text


def _canonical_unique(
    values: list[str], *, path: str, sort_values: bool = True
) -> None:
    if len(values) != len(set(values)) or (sort_values and values != sorted(values)):
        raise SourceDescriptionError(f"{path} must be unique and canonical-sorted")


def _validate_assistant_profiles(source: Path) -> set[str]:
    try:
        value = json.loads(
            source.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise SourceDescriptionError(
            f"cannot read maintained assistant profiles: {error}"
        ) from error
    top = _exact(
        value,
        required=frozenset({"api_version", "mode_profiles", "profiles"}),
        path="assistant_profiles",
    )
    if top["api_version"] != "manyforge.maintained_assistant_profiles.v1":
        raise SourceDescriptionError(
            "assistant_profiles.api_version is not the maintained v1 contract"
        )

    mode_profiles = _object(
        top["mode_profiles"], path="assistant_profiles.mode_profiles"
    )
    if set(mode_profiles) != {"scene_authoring"}:
        raise SourceDescriptionError(
            "assistant_profiles.mode_profiles changes the exact maintained set"
        )
    modes = _object(
        mode_profiles["scene_authoring"],
        path="assistant_profiles.mode_profiles.scene_authoring",
    )
    if set(modes) != {"query", "composer-assistant"}:
        raise SourceDescriptionError(
            "assistant scene-authoring profile changes the reserved mode set"
        )
    for mode_id, raw_mode in modes.items():
        mode_path = f"assistant_profiles.mode_profiles.scene_authoring.{mode_id}"
        mode = _exact(
            raw_mode,
            required=frozenset({"enabled", "description", "grant_policy", "catalog"}),
            path=mode_path,
        )
        if mode["enabled"] is not True:
            raise SourceDescriptionError(f"{mode_path}.enabled must remain true")
        _text(mode["description"], path=f"{mode_path}.description")
        grant = _exact(
            mode["grant_policy"],
            required=frozenset({"session_kind", "approval_mode", "review_policy"}),
            path=f"{mode_path}.grant_policy",
        )
        expected_review = "not_required" if mode_id == "query" else "required"
        if (
            grant["session_kind"] is not None
            or grant["approval_mode"] is not None
            or grant["review_policy"] != expected_review
        ):
            raise SourceDescriptionError(
                f"{mode_path}.grant_policy changes reserved offline semantics"
            )
        catalog = _exact(
            mode["catalog"],
            required=frozenset({"skills", "tools", "nodes"}),
            path=f"{mode_path}.catalog",
        )
        for kind in ("skills", "tools", "nodes"):
            ids = [
                _identifier(item, path=f"{mode_path}.catalog.{kind}[{index}]")
                for index, item in enumerate(
                    _array(catalog[kind], path=f"{mode_path}.catalog.{kind}")
                )
            ]
            _canonical_unique(ids, path=f"{mode_path}.catalog.{kind}")

    profile_ids: list[str] = []
    site_overlay_value_refs: set[str] = set()
    for index, raw_profile in enumerate(
        _array(top["profiles"], path="assistant_profiles.profiles")
    ):
        profile_path = f"assistant_profiles.profiles[{index}]"
        profile = _exact(
            raw_profile,
            required=frozenset(
                {
                    "profile_id",
                    "audit_policy",
                    "prompt_templates",
                    "model_endpoints",
                    "provider_alias",
                    "mode_profile_id",
                }
            ),
            path=profile_path,
        )
        profile_id = _identifier(
            profile["profile_id"], path=f"{profile_path}.profile_id"
        )
        profile_ids.append(profile_id)
        expected = _EXPECTED_ASSISTANT_PROFILE_CONTRACTS.get(profile_id)
        if expected is None:
            raise SourceDescriptionError(
                f"{profile_path}.profile_id is outside the maintained set"
            )
        audit = _exact(
            profile["audit_policy"],
            required=frozenset(
                {
                    "store_full_prompts",
                    "store_full_responses",
                    "redact_argument_values",
                }
            ),
            path=f"{profile_path}.audit_policy",
        )
        if audit != {
            "store_full_prompts": False,
            "store_full_responses": False,
            "redact_argument_values": True,
        }:
            raise SourceDescriptionError(
                f"{profile_path}.audit_policy weakens the historical redaction policy"
            )
        templates = _object(
            profile["prompt_templates"],
            path=f"{profile_path}.prompt_templates",
        )
        for template_id, raw_template in templates.items():
            _identifier(template_id, path=f"{profile_path}.prompt_templates key")
            template = _exact(
                raw_template,
                required=frozenset(
                    {
                        "version",
                        "template_text",
                        "variable_ids",
                        "response_contract",
                    }
                ),
                path=f"{profile_path}.prompt_templates.{template_id}",
            )
            expected_template = _EXPECTED_ASSISTANT_PROMPT_CONTRACTS.get(
                (profile_id, template_id)
            )
            if expected_template is None:
                raise SourceDescriptionError(
                    f"{profile_path}.prompt_templates.{template_id} is outside "
                    "the maintained historical set"
                )
            version = _text(
                template["version"],
                path=f"{profile_path}.prompt_templates.{template_id}.version",
            )
            if not version.isascii() or any(
                not (character.isalnum() or character in "_.-") for character in version
            ):
                raise SourceDescriptionError(
                    f"{profile_path}.prompt_templates.{template_id}.version "
                    "must be a portable token"
                )
            template_text = _text(
                template["template_text"],
                path=f"{profile_path}.prompt_templates.{template_id}.template_text",
            )
            variables = [
                _identifier(
                    item,
                    path=(
                        f"{profile_path}.prompt_templates.{template_id}"
                        f".variable_ids[{variable_index}]"
                    ),
                )
                for variable_index, item in enumerate(
                    _array(
                        template["variable_ids"],
                        path=(
                            f"{profile_path}.prompt_templates.{template_id}"
                            ".variable_ids"
                        ),
                    )
                )
            ]
            _canonical_unique(
                variables,
                path=f"{profile_path}.prompt_templates.{template_id}.variable_ids",
            )
            response_path = (
                f"{profile_path}.prompt_templates.{template_id}.response_contract"
            )
            raw_response = _object(template["response_contract"], path=response_path)
            response_mode = _text(
                raw_response.get("response_mode"),
                path=f"{response_path}.response_mode",
            )
            if response_mode == "json_object":
                response = _exact(
                    raw_response,
                    required=frozenset({"response_mode"}),
                    path=response_path,
                )
            elif response_mode == "tool_call":
                response = _exact(
                    raw_response,
                    required=frozenset(
                        {"response_mode", "tool_name", "tool_description"}
                    ),
                    path=response_path,
                )
                _identifier(response["tool_name"], path=f"{response_path}.tool_name")
                _text(
                    response["tool_description"],
                    path=f"{response_path}.tool_description",
                )
            else:
                raise SourceDescriptionError(
                    f"{response_path}.response_mode is outside the closed set"
                )
            if (
                version != expected_template["version"]
                or hashlib.sha256(template_text.encode("utf-8")).hexdigest()
                != expected_template["template_text_sha256"]
                or tuple(variables) != expected_template["variable_ids"]
                or response != expected_template["response_contract"]
            ):
                raise SourceDescriptionError(
                    f"{profile_path}.prompt_templates.{template_id} changes "
                    "the exact maintained prompt/response contract"
                )

        expected_template_ids = {
            template_id
            for (expected_profile_id, template_id) in (
                _EXPECTED_ASSISTANT_PROMPT_CONTRACTS
            )
            if expected_profile_id == profile_id
        }
        if set(templates) != expected_template_ids:
            raise SourceDescriptionError(
                f"{profile_path}.prompt_templates changes the maintained set"
            )

        endpoint_aliases: list[str] = []
        historical_profiles: list[str] = []
        for endpoint_index, raw_endpoint in enumerate(
            _array(profile["model_endpoints"], path=f"{profile_path}.model_endpoints")
        ):
            endpoint_path = f"{profile_path}.model_endpoints[{endpoint_index}]"
            endpoint = _exact(
                raw_endpoint,
                required=frozenset(
                    {
                        "model_endpoint_alias",
                        "model_id",
                        "site_overlay_value_ref",
                        "endpoint_value",
                        "historical_profile_ref",
                        "request_contract",
                    }
                ),
                path=endpoint_path,
            )
            endpoint_alias = _identifier(
                endpoint["model_endpoint_alias"],
                path=f"{endpoint_path}.model_endpoint_alias",
            )
            endpoint_aliases.append(endpoint_alias)
            _identifier(endpoint["model_id"], path=f"{endpoint_path}.model_id")
            site_overlay_value_ref = _identifier(
                endpoint["site_overlay_value_ref"],
                path=f"{endpoint_path}.site_overlay_value_ref",
            )
            if site_overlay_value_ref in site_overlay_value_refs:
                raise SourceDescriptionError(
                    f"{endpoint_path}.site_overlay_value_ref is duplicated"
                )
            site_overlay_value_refs.add(site_overlay_value_ref)
            endpoint_value = _text(
                endpoint["endpoint_value"], path=f"{endpoint_path}.endpoint_value"
            )
            if not endpoint_value.startswith("http://127.0.0.1:"):
                raise SourceDescriptionError(
                    f"{endpoint_path}.endpoint_value is not the maintained local endpoint"
                )
            historical_profiles.append(
                _identifier(
                    endpoint["historical_profile_ref"],
                    path=f"{endpoint_path}.historical_profile_ref",
                )
            )
            request_path = f"{endpoint_path}.request_contract"
            request = _exact(
                endpoint["request_contract"],
                required=frozenset(
                    {
                        "default_timeout_s",
                        "max_output_tokens",
                        "supports_response_tools",
                        "supports_json_object",
                        "supports_image_input",
                        "supported_image_transports",
                        "tool_call_family",
                    }
                ),
                path=request_path,
            )
            timeout = request["default_timeout_s"]
            tokens = request["max_output_tokens"]
            if (
                type(timeout) not in {int, float}
                or not math.isfinite(timeout)
                or timeout <= 0.0
                or type(tokens) is not int
                or not 0 < tokens <= 1_048_576
            ):
                raise SourceDescriptionError(
                    f"{request_path} has invalid bounded request limits"
                )
            for field in (
                "supports_response_tools",
                "supports_json_object",
                "supports_image_input",
            ):
                if type(request[field]) is not bool:
                    raise SourceDescriptionError(
                        f"{request_path}.{field} must be boolean"
                    )
            transports = [
                _identifier(
                    item,
                    path=(
                        f"{request_path}.supported_image_transports"
                        f"[{transport_index}]"
                    ),
                )
                for transport_index, item in enumerate(
                    _array(
                        request["supported_image_transports"],
                        path=f"{request_path}.supported_image_transports",
                    )
                )
            ]
            _canonical_unique(
                transports,
                path=f"{request_path}.supported_image_transports",
            )
            if any(
                transport not in {"http_url", "inline_data_url"}
                for transport in transports
            ):
                raise SourceDescriptionError(
                    f"{request_path}.supported_image_transports is outside the closed set"
                )
            if request["supports_image_input"] != bool(transports):
                raise SourceDescriptionError(
                    f"{request_path}.supports_image_input contradicts its transports"
                )
            _identifier(
                request["tool_call_family"],
                path=f"{request_path}.tool_call_family",
            )
            normalized_request = dict(request)
            normalized_request["supported_image_transports"] = transports
            if normalized_request != _EXPECTED_ASSISTANT_REQUEST_CONTRACTS.get(
                (profile_id, endpoint_alias)
            ):
                raise SourceDescriptionError(
                    f"{request_path} changes the exact maintained request contract"
                )
        _canonical_unique(endpoint_aliases, path=f"{profile_path}.model_endpoints")
        if (
            tuple(endpoint_aliases) != expected["endpoint_aliases"]
            or tuple(historical_profiles) != expected["historical_profiles"]
            or profile["mode_profile_id"] != expected["mode_profile_id"]
        ):
            raise SourceDescriptionError(
                f"{profile_path} changes the maintained endpoint/mode mapping"
            )

        provider = profile["provider_alias"]
        if provider is None:
            provider_kind = None
        else:
            provider_path = f"{profile_path}.provider_alias"
            provider_row = _exact(
                provider,
                required=frozenset(
                    {
                        "provider_alias_id",
                        "provider_kind",
                        "bridge_external_resource_alias",
                        "site_overlay_value_ref",
                        "endpoint_value",
                    }
                ),
                path=provider_path,
            )
            _identifier(
                provider_row["provider_alias_id"],
                path=f"{provider_path}.provider_alias_id",
            )
            _identifier(
                provider_row["bridge_external_resource_alias"],
                path=f"{provider_path}.bridge_external_resource_alias",
            )
            site_overlay_value_ref = _identifier(
                provider_row["site_overlay_value_ref"],
                path=f"{provider_path}.site_overlay_value_ref",
            )
            if site_overlay_value_ref in site_overlay_value_refs:
                raise SourceDescriptionError(
                    f"{provider_path}.site_overlay_value_ref is duplicated"
                )
            site_overlay_value_refs.add(site_overlay_value_ref)
            _text(
                provider_row["endpoint_value"], path=f"{provider_path}.endpoint_value"
            )
            provider_kind = provider_row["provider_kind"]
            if provider_kind not in {"direct", "nemoclaw"}:
                raise SourceDescriptionError(
                    f"{provider_path}.provider_kind is outside the closed v1 set"
                )
        if provider_kind != expected["provider_kind"]:
            raise SourceDescriptionError(
                f"{profile_path}.provider_alias changes the maintained provider kind"
            )

    if profile_ids != sorted(profile_ids) or set(profile_ids) != set(
        _EXPECTED_ASSISTANT_PROFILE_CONTRACTS
    ):
        raise SourceDescriptionError(
            "assistant_profiles.profiles must contain the canonical maintained set"
        )
    return set(profile_ids)


def _validate_model_semantics(
    value: object,
    *,
    family_id: str,
    path: str,
) -> None:
    semantics = _exact(
        value,
        required=frozenset(
            {
                "collision_body_role_bindings",
                "frame_role_bindings",
                "instance_cardinality",
                "model_group_role_bindings",
                "resource_role_bindings",
                "root_frame_role_id",
            }
        ),
        path=path,
    )
    if semantics["instance_cardinality"] != "multiple_per_cell":
        raise SourceDescriptionError(
            f"{path}.instance_cardinality must preserve multi-instance model use"
        )

    frame_roles: list[str] = []
    frame_bindings: dict[str, str] = {}
    for index, raw_binding in enumerate(
        _array(semantics["frame_role_bindings"], path=f"{path}.frame_role_bindings")
    ):
        binding_path = f"{path}.frame_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"frame_role_id", "source_link_name"}),
            path=binding_path,
        )
        frame_role = _identifier(
            binding["frame_role_id"], path=f"{binding_path}.frame_role_id"
        )
        frame_roles.append(frame_role)
        frame_bindings[frame_role] = _source_symbol(
            binding["source_link_name"], path=f"{binding_path}.source_link_name"
        )
    _canonical_unique(frame_roles, path=f"{path}.frame_role_bindings")

    group_roles: list[str] = []
    group_bindings: dict[str, str] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["model_group_role_bindings"],
            path=f"{path}.model_group_role_bindings",
        )
    ):
        binding_path = f"{path}.model_group_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"model_group_role_id", "source_group_name"}),
            path=binding_path,
        )
        group_role = _identifier(
            binding["model_group_role_id"],
            path=f"{binding_path}.model_group_role_id",
        )
        group_roles.append(group_role)
        group_bindings[group_role] = _source_symbol(
            binding["source_group_name"], path=f"{binding_path}.source_group_name"
        )
    _canonical_unique(group_roles, path=f"{path}.model_group_role_bindings")

    resource_roles: list[str] = []
    resource_bindings: dict[str, tuple[str, ...]] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["resource_role_bindings"],
            path=f"{path}.resource_role_bindings",
        )
    ):
        binding_path = f"{path}.resource_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"resource_role_id", "source_joint_names"}),
            path=binding_path,
        )
        resource_role = _identifier(
            binding["resource_role_id"], path=f"{binding_path}.resource_role_id"
        )
        resource_roles.append(resource_role)
        joint_names = [
            _source_symbol(
                joint, path=f"{binding_path}.source_joint_names[{joint_index}]"
            )
            for joint_index, joint in enumerate(
                _array(
                    binding["source_joint_names"],
                    path=f"{binding_path}.source_joint_names",
                )
            )
        ]
        if not joint_names or len(joint_names) != len(set(joint_names)):
            raise SourceDescriptionError(
                f"{binding_path}.source_joint_names must be non-empty and unique"
            )
        resource_bindings[resource_role] = tuple(joint_names)
    _canonical_unique(resource_roles, path=f"{path}.resource_role_bindings")

    collision_roles: list[str] = []
    collision_bindings: dict[str, tuple[str, ...]] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["collision_body_role_bindings"],
            path=f"{path}.collision_body_role_bindings",
        )
    ):
        binding_path = f"{path}.collision_body_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"collision_body_role_id", "source_link_names"}),
            path=binding_path,
        )
        collision_role = _identifier(
            binding["collision_body_role_id"],
            path=f"{binding_path}.collision_body_role_id",
        )
        collision_roles.append(collision_role)
        link_names = [
            _source_symbol(link, path=f"{binding_path}.source_link_names[{link_index}]")
            for link_index, link in enumerate(
                _array(
                    binding["source_link_names"],
                    path=f"{binding_path}.source_link_names",
                )
            )
        ]
        if not link_names:
            raise SourceDescriptionError(
                f"{binding_path}.source_link_names must not be empty"
            )
        _canonical_unique(link_names, path=f"{binding_path}.source_link_names")
        collision_bindings[collision_role] = tuple(link_names)
    _canonical_unique(collision_roles, path=f"{path}.collision_body_role_bindings")

    expected = _EXPECTED_MODEL_FAMILIES[family_id]
    root_frame_role = _identifier(
        semantics["root_frame_role_id"], path=f"{path}.root_frame_role_id"
    )
    if (
        frame_bindings != expected["frames"]
        or group_bindings != expected["groups"]
        or resource_bindings != expected["resources"]
        or collision_bindings != expected["collisions"]
        or root_frame_role != expected["root"]
        or root_frame_role not in frame_roles
    ):
        raise SourceDescriptionError(
            f"{path} changes the accepted source-local family role contract"
        )


def _program_roles(program_path: Path) -> tuple[set[str], dict[str, str]]:
    try:
        value = yaml.safe_load(program_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        raise SourceDescriptionError(
            f"cannot read Program source {program_path}: {error}"
        ) from error
    root = _object(value, path=str(program_path))
    if root.get("api_version") != PROGRAM_API_VERSION:
        raise SourceDescriptionError(f"{program_path} is not a direct v1 Program")
    component_roles = _object(
        root.get("component_roles"), path=f"{program_path}.component_roles"
    )
    target_rows = _object(root.get("target_roles"), path=f"{program_path}.target_roles")
    target_roles: dict[str, str] = {}
    for name, raw_target in target_rows.items():
        target = _object(raw_target, path=f"{program_path}.target_roles.{name}")
        target_roles[name] = _identifier(
            target.get("component_role"),
            path=f"{program_path}.target_roles.{name}.component_role",
        )
    return set(component_roles), target_roles


def _validate_role_assignment(
    value: object,
    *,
    root: Path,
    component_occurrence_id: str,
    path: str,
) -> tuple[set[str], str, tuple[str, ...]]:
    row = _exact(
        value,
        required=frozenset(
            {
                "scenario_id",
                "scenario_aliases",
                "program_source",
                "component_roles",
                "target_roles",
            }
        ),
        path=path,
    )
    scenario_id = _identifier(row["scenario_id"], path=f"{path}.scenario_id")
    aliases = tuple(
        _identifier(alias, path=f"{path}.scenario_aliases[{index}]")
        for index, alias in enumerate(
            _array(row["scenario_aliases"], path=f"{path}.scenario_aliases")
        )
    )
    if (
        len(aliases) > 64
        or aliases != tuple(sorted(set(aliases)))
        or scenario_id in aliases
    ):
        raise SourceDescriptionError(
            f"{path}.scenario_aliases must be bounded, unique, "
            "canonical-sorted, and distinct from scenario_id"
        )
    program_source = _source_path(
        row["program_source"], root=root, path=f"{path}.program_source"
    )
    declared_components, declared_targets = _program_roles(root / program_source)
    components = _object(row["component_roles"], path=f"{path}.component_roles")
    if set(components) != declared_components:
        raise SourceDescriptionError(
            f"{path}.component_roles does not exactly cover the Program"
        )
    for role, occurrence in components.items():
        _identifier(role, path=f"{path}.component_roles key")
        if occurrence != component_occurrence_id:
            raise SourceDescriptionError(
                f"{path}.component_roles.{role} must select the deployment occurrence"
            )

    targets = _object(row["target_roles"], path=f"{path}.target_roles")
    if set(targets) != set(declared_targets):
        raise SourceDescriptionError(
            f"{path}.target_roles does not exactly cover the Program"
        )
    for role, raw_target in targets.items():
        target = _exact(
            raw_target,
            required=frozenset({"component_role", "command_target_id"}),
            path=f"{path}.target_roles.{role}",
        )
        component_role = _identifier(
            target["component_role"], path=f"{path}.target_roles.{role}.component_role"
        )
        _identifier(
            target["command_target_id"],
            path=f"{path}.target_roles.{role}.command_target_id",
        )
        if component_role != declared_targets[role]:
            raise SourceDescriptionError(
                f"{path}.target_roles.{role} changes the Program component role"
            )
    return set(targets), scenario_id, aliases


def _validate_policy(value: object, *, target_roles: set[str], path: str) -> str:
    row = _object(value, path=path)
    kind = row.get("kind")
    if kind == "open_loop_grasp_acknowledgement":
        row = _exact(
            row,
            required=frozenset(
                {
                    "kind",
                    "source_field",
                    "target_role_id",
                    "allowed",
                    "scope",
                    "rationale",
                    "runtime_acceptance_step",
                    "tool_acceptance_step",
                    "acceptance_obligation_id",
                }
            ),
            path=path,
        )
        if row["source_field"] != "grasp_risk_policy.open_loop_grasp":
            raise SourceDescriptionError(
                f"{path}.source_field is not the historical grasp field"
            )
        if row["allowed"] is not True or row["scope"] != "simulation":
            raise SourceDescriptionError(
                f"{path} changes the historical open-loop scope"
            )
        _text(row["rationale"], path=f"{path}.rationale")
        if (
            row["runtime_acceptance_step"] != "I8/P5"
            or row["tool_acceptance_step"] != "I11/T"
            or row["acceptance_obligation_id"] != "open_loop_grasp_scope_and_evidence"
        ):
            raise SourceDescriptionError(
                f"{path} changes the deferred grasp owner/acceptance obligation"
            )
    else:
        raise SourceDescriptionError(
            f"{path}.kind is outside the closed restoration union"
        )
    target_role = _identifier(row["target_role_id"], path=f"{path}.target_role_id")
    if target_role not in target_roles:
        raise SourceDescriptionError(
            f"{path}.target_role_id is absent from every Program assignment"
        )
    return kind


def validate(source: Path, root: Path) -> None:
    try:
        value = json.loads(
            source.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise SourceDescriptionError(
            f"cannot read maintained source description: {error}"
        ) from error
    top = _exact(
        value,
        required=frozenset(
            {
                "api_version",
                "historical_source_commit",
                "assistant_profiles_source",
                "historical_program_sources",
                "runtime_restoration_profiles",
                "model_families",
                "deployments",
            }
        ),
        path="$",
    )
    if top["api_version"] != API_VERSION:
        raise SourceDescriptionError(f"$.api_version must be {API_VERSION}")
    if (
        type(top["historical_source_commit"]) is not str
        or _HEX40.fullmatch(top["historical_source_commit"]) is None
        or top["historical_source_commit"] != _EXPECTED_HISTORICAL_SOURCE_COMMIT
    ):
        raise SourceDescriptionError(
            "$.historical_source_commit must be the exact pinned I0 source commit"
        )
    assistant_profiles_source = _source_path(
        top["assistant_profiles_source"],
        root=root,
        path="$.assistant_profiles_source",
    )
    if assistant_profiles_source != _EXPECTED_ASSISTANT_PROFILES_SOURCE:
        raise SourceDescriptionError(
            "$.assistant_profiles_source changes the maintained profile closure"
        )
    assistant_profile_ids = _validate_assistant_profiles(
        root / assistant_profiles_source
    )

    historical_program_sources: list[str] = []
    for index, raw_program in enumerate(
        _array(top["historical_program_sources"], path="$.historical_program_sources")
    ):
        path = f"$.historical_program_sources[{index}]"
        program = _exact(
            raw_program,
            required=frozenset({"program_source", "historical_blob"}),
            path=path,
        )
        program_source = _source_path(
            program["program_source"], root=root, path=f"{path}.program_source"
        )
        historical_program_sources.append(program_source)
        if (
            program_source not in _EXPECTED_HISTORICAL_PROGRAM_BLOBS
            or program["historical_blob"]
            != _EXPECTED_HISTORICAL_PROGRAM_BLOBS[program_source]
        ):
            raise SourceDescriptionError(
                f"{path} changes the exact pinned I0 Program source"
            )
    if historical_program_sources != sorted(historical_program_sources) or set(
        historical_program_sources
    ) != set(_EXPECTED_HISTORICAL_PROGRAM_BLOBS):
        raise SourceDescriptionError(
            "$.historical_program_sources must exactly cover the seven canonical-sorted Programs"
        )

    family_ids: list[str] = []
    for index, raw_family in enumerate(
        _array(top["model_families"], path="$.model_families")
    ):
        path = f"$.model_families[{index}]"
        family = _exact(
            raw_family,
            required=frozenset(
                {
                    "catalog_source",
                    "model_family_id",
                    "model_asset_id",
                    "semantics",
                    "source",
                }
            ),
            path=path,
        )
        family_id = _identifier(
            family["model_family_id"], path=f"{path}.model_family_id"
        )
        family_ids.append(family_id)
        model_asset_id = _identifier(
            family["model_asset_id"], path=f"{path}.model_asset_id"
        )
        if model_asset_id != family_id or family_id not in _EXPECTED_MODEL_FAMILIES:
            raise SourceDescriptionError(
                f"{path} changes the exact maintained model-family set"
            )
        expected_catalog_source = (
            f"manyforge_deployment/resources/maintained/model_assets/"
            f"{model_asset_id}.model_asset.json"
        )
        catalog_source = _source_path(
            family["catalog_source"], root=root, path=f"{path}.catalog_source"
        )
        if catalog_source != expected_catalog_source:
            raise SourceDescriptionError(
                f"{path}.catalog_source is not the shared family source"
            )
        _validate_model_semantics(
            family["semantics"], family_id=family_id, path=f"{path}.semantics"
        )
        model_source = _exact(
            family["source"],
            required=frozenset(
                {
                    "accepted_package_uri_prefix",
                    "historical_srdf_path",
                    "historical_urdf_path",
                    "resource_root",
                    "srdf_path",
                    "srdf_projection_kind",
                    "urdf_path",
                    "urdf_projection_kind",
                }
            ),
            path=f"{path}.source",
        )
        prefix = _text(
            model_source["accepted_package_uri_prefix"],
            path=f"{path}.source.accepted_package_uri_prefix",
        )
        if not prefix.startswith("package://") or not prefix.endswith("/"):
            raise SourceDescriptionError(
                f"{path}.source.accepted_package_uri_prefix is invalid"
            )
        resource_root = _directory_path(
            model_source["resource_root"],
            root=root,
            path=f"{path}.source.resource_root",
        )
        srdf_path = _source_path(
            model_source["srdf_path"], root=root, path=f"{path}.source.srdf_path"
        )
        historical_srdf_path = _source_path(
            model_source["historical_srdf_path"],
            root=root,
            path=f"{path}.source.historical_srdf_path",
        )
        projection_kind = _text(
            model_source["srdf_projection_kind"],
            path=f"{path}.source.srdf_projection_kind",
        )
        expected_source = _EXPECTED_MODEL_FAMILIES[family_id]["source"]
        if projection_kind != expected_source["srdf_projection_kind"]:
            raise SourceDescriptionError(
                f"{path}.source.srdf_projection_kind changes the accepted projection"
            )
        if (projection_kind == "identity") != (srdf_path == historical_srdf_path):
            raise SourceDescriptionError(
                f"{path}.source SRDF projection paths contradict their declared kind"
            )
        urdf_path = _source_path(
            model_source["urdf_path"], root=root, path=f"{path}.source.urdf_path"
        )
        historical_urdf_path = _source_path(
            model_source["historical_urdf_path"],
            root=root,
            path=f"{path}.source.historical_urdf_path",
        )
        urdf_projection_kind = _text(
            model_source["urdf_projection_kind"],
            path=f"{path}.source.urdf_projection_kind",
        )
        if urdf_projection_kind != expected_source["urdf_projection_kind"]:
            raise SourceDescriptionError(
                f"{path}.source.urdf_projection_kind changes the accepted projection"
            )
        if (urdf_projection_kind == "identity") != (urdf_path == historical_urdf_path):
            raise SourceDescriptionError(
                f"{path}.source URDF projection paths contradict their declared kind"
            )
        actual_source = {
            "accepted_package_uri_prefix": prefix,
            "historical_srdf_path": historical_srdf_path,
            "historical_urdf_path": historical_urdf_path,
            "resource_root": resource_root,
            "srdf_path": srdf_path,
            "srdf_projection_kind": projection_kind,
            "urdf_path": urdf_path,
            "urdf_projection_kind": urdf_projection_kind,
        }
        if actual_source != expected_source:
            raise SourceDescriptionError(
                f"{path}.source changes the accepted model-family source closure"
            )
    if (
        family_ids != sorted(family_ids)
        or len(set(family_ids)) != len(family_ids)
        or set(family_ids) != set(_EXPECTED_MODEL_FAMILIES)
    ):
        raise SourceDescriptionError(
            "$.model_families must contain the canonical-sorted maintained families"
        )

    runtime_profile_ids: list[str] = []
    runtime_profile_families: dict[str, str] = {}
    for index, raw_profile in enumerate(
        _array(
            top["runtime_restoration_profiles"],
            path="$.runtime_restoration_profiles",
        )
    ):
        path = f"$.runtime_restoration_profiles[{index}]"
        raw_profile_object = _object(raw_profile, path=path)
        is_ros2_control = "ros2_control_runtime" in raw_profile_object
        profile = _exact(
            raw_profile,
            required=frozenset(
                {
                    "runtime_restoration_profile_id",
                    "model_family_id",
                    "source_fields",
                    ("ros2_control_runtime" if is_ros2_control else "native_runtime"),
                    "stop_profiles",
                    "acceptance_obligations",
                }
            ),
            path=path,
        )
        profile_id = _identifier(
            profile.get("runtime_restoration_profile_id"),
            path=f"{path}.runtime_restoration_profile_id",
        )
        runtime_profile_ids.append(profile_id)
        expected_digest = _EXPECTED_RUNTIME_RESTORATION_PROFILE_DIGESTS.get(profile_id)
        profile_digest = hashlib.sha256(
            json.dumps(
                profile,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        ).hexdigest()
        if expected_digest is None or profile_digest != expected_digest:
            raise SourceDescriptionError(
                f"{path} changes the exact historical runtime restoration input"
            )
        expected_source_fields = (
            _ROS2_CONTROL_RUNTIME_RESTORATION_SOURCE_FIELDS
            if is_ros2_control
            else _RUNTIME_RESTORATION_SOURCE_FIELDS
        )
        if profile_id in {
            "lite6_native_simulated_v0",
            "ur5e_robotiq_native_simulated_v0",
        }:
            expected_source_fields = {
                **expected_source_fields,
                "gripper_target_configurations": "grippers",
            }
        if profile["source_fields"] != expected_source_fields:
            raise SourceDescriptionError(
                f"{path}.source_fields changes the pinned I0 field mapping"
            )
        expected_acceptance = (
            _ROS2_CONTROL_RUNTIME_RESTORATION_ACCEPTANCE
            if is_ros2_control
            else _RUNTIME_RESTORATION_ACCEPTANCE
        )
        if profile_id == "ur5e_robotiq_native_simulated_v0":
            expected_acceptance = _UI6I_RUNTIME_RESTORATION_ACCEPTANCE
        if profile["acceptance_obligations"] != expected_acceptance:
            raise SourceDescriptionError(
                f"{path}.acceptance_obligations changes the deferred owner gates"
            )
        family_id = _identifier(
            profile.get("model_family_id"), path=f"{path}.model_family_id"
        )
        if (
            family_id not in family_ids
            or family_id
            != _EXPECTED_RUNTIME_RESTORATION_PROFILE_FAMILIES.get(profile_id)
        ):
            raise SourceDescriptionError(
                f"{path}.model_family_id is absent from the maintained model closure"
            )
        runtime_profile_families[profile_id] = family_id
    if runtime_profile_ids != sorted(runtime_profile_ids) or set(
        runtime_profile_ids
    ) != set(_EXPECTED_RUNTIME_RESTORATION_PROFILE_DIGESTS):
        raise SourceDescriptionError(
            "$.runtime_restoration_profiles must exactly cover the "
            "canonical-sorted pinned profiles"
        )

    deployment_sources: list[str] = []
    scenario_identifiers: set[str] = set()
    open_loop_sources: set[str] = set()
    for index, raw_deployment in enumerate(
        _array(top["deployments"], path="$.deployments")
    ):
        path = f"$.deployments[{index}]"
        raw_deployment_object = _object(raw_deployment, path=path)
        raw_deployment_source = _text(
            raw_deployment_object.get("deployment_source"),
            path=f"{path}.deployment_source",
        )
        is_i9 = raw_deployment_source in _I9_DEPLOYMENT_SOURCES
        deployment_fields = {
            "deployment_source",
            "deployment_artifact_id",
            "cell_id",
            "name",
            "description",
            "compatibility",
            "assistant_profile_id",
            "historical_blob",
            "model_family_id",
            "runtime_restoration_profile_id",
            "component_occurrence_id",
            "program_role_assignments",
            "policy_restoration_inputs",
        }
        if is_i9:
            deployment_fields.update(
                {
                    "historical_program_source",
                    "historical_program_blob",
                    "i9_behavior_obligations",
                    "root_pose_in_cell",
                }
            )
        row = _exact(
            raw_deployment,
            required=frozenset(deployment_fields),
            path=path,
        )
        deployment_source = _source_path(
            row["deployment_source"], root=root, path=f"{path}.deployment_source"
        )
        deployment_sources.append(deployment_source)
        deployment_artifact_id = _identifier(
            row["deployment_artifact_id"], path=f"{path}.deployment_artifact_id"
        )
        _identifier(row["cell_id"], path=f"{path}.cell_id")
        name = _text(row["name"], path=f"{path}.name")
        if name != deployment_artifact_id:
            raise SourceDescriptionError(
                f"{path}.name must retain the deployment artifact name"
            )
        _text(row["description"], path=f"{path}.description")
        compatibility = _exact(
            row["compatibility"],
            required=frozenset({"lane", "provides", "tags"}),
            path=f"{path}.compatibility",
        )
        for field in ("provides", "tags"):
            values = [
                _identifier(
                    item,
                    path=f"{path}.compatibility.{field}[{value_index}]",
                )
                for value_index, item in enumerate(
                    _array(
                        compatibility[field],
                        path=f"{path}.compatibility.{field}",
                    )
                )
            ]
            _canonical_unique(values, path=f"{path}.compatibility.{field}")
        if compatibility != _EXPECTED_COMPATIBILITY[deployment_source]:
            raise SourceDescriptionError(
                f"{path}.compatibility changes the maintained lane/capability facts"
            )
        assistant_profile_id = row["assistant_profile_id"]
        if assistant_profile_id is not None:
            assistant_profile_id = _identifier(
                assistant_profile_id, path=f"{path}.assistant_profile_id"
            )
            if assistant_profile_id not in assistant_profile_ids:
                raise SourceDescriptionError(
                    f"{path}.assistant_profile_id is absent from the maintained closure"
                )
        if assistant_profile_id != _EXPECTED_ASSISTANT_PROFILE_IDS[deployment_source]:
            raise SourceDescriptionError(
                f"{path}.assistant_profile_id changes the historical assistant mapping"
            )
        if row["historical_blob"] != _EXPECTED_HISTORICAL_BLOBS[deployment_source]:
            raise SourceDescriptionError(
                f"{path}.historical_blob changes the pinned I0 source"
            )
        if is_i9:
            historical_program = _EXPECTED_I9_HISTORICAL_PROGRAMS[deployment_source]
            if (
                row["historical_program_source"] != historical_program["source"]
                or row["historical_program_blob"] != historical_program["blob"]
            ):
                raise SourceDescriptionError(
                    f"{path} changes the exact pinned I9 Program audit source"
                )
            obligations = [
                _identifier(item, path=f"{path}.i9_behavior_obligations[{item_index}]")
                for item_index, item in enumerate(
                    _array(
                        row["i9_behavior_obligations"],
                        path=f"{path}.i9_behavior_obligations",
                    )
                )
            ]
            _canonical_unique(obligations, path=f"{path}.i9_behavior_obligations")
            if obligations != _EXPECTED_I9_BEHAVIOR_OBLIGATIONS[deployment_source]:
                raise SourceDescriptionError(
                    f"{path}.i9_behavior_obligations changes the I9 audit closure"
                )
            root_pose = _exact(
                row["root_pose_in_cell"],
                required=frozenset({"position_m", "orientation_xyzw"}),
                path=f"{path}.root_pose_in_cell",
            )
            expected_root_pose = _EXPECTED_I9_ROOT_POSES_IN_CELL[deployment_source]
            if root_pose != expected_root_pose:
                raise SourceDescriptionError(
                    f"{path}.root_pose_in_cell changes the exact historical "
                    "Isaac world placement"
                )
        if row["model_family_id"] not in family_ids:
            raise SourceDescriptionError(f"{path}.model_family_id is unavailable")
        runtime_profile_id = _identifier(
            row["runtime_restoration_profile_id"],
            path=f"{path}.runtime_restoration_profile_id",
        )
        if (
            runtime_profile_id
            != _EXPECTED_DEPLOYMENT_RUNTIME_PROFILE_IDS[deployment_source]
            or runtime_profile_families.get(runtime_profile_id)
            != row["model_family_id"]
        ):
            raise SourceDescriptionError(
                f"{path}.runtime_restoration_profile_id changes the historical runtime family"
            )
        occurrence = _identifier(
            row["component_occurrence_id"], path=f"{path}.component_occurrence_id"
        )
        assignment_rows = _array(
            row["program_role_assignments"], path=f"{path}.program_role_assignments"
        )
        if not assignment_rows:
            raise SourceDescriptionError(
                f"{path}.program_role_assignments must not be empty"
            )
        all_target_roles: set[str] = set()
        for assignment_index, assignment in enumerate(assignment_rows):
            target_roles, scenario_id, scenario_aliases = _validate_role_assignment(
                assignment,
                root=root,
                component_occurrence_id=occurrence,
                path=f"{path}.program_role_assignments[{assignment_index}]",
            )
            for identifier in (scenario_id, *scenario_aliases):
                if identifier in scenario_identifiers:
                    raise SourceDescriptionError(
                        f"{path}.program_role_assignments[{assignment_index}] "
                        f"duplicates scenario identifier {identifier!r}"
                    )
                scenario_identifiers.add(identifier)
            all_target_roles.update(target_roles)
        policy_rows = _array(
            row["policy_restoration_inputs"], path=f"{path}.policy_restoration_inputs"
        )
        kinds: list[str] = []
        for policy_index, policy in enumerate(policy_rows):
            kind = _validate_policy(
                policy,
                target_roles=all_target_roles,
                path=f"{path}.policy_restoration_inputs[{policy_index}]",
            )
            kinds.append(kind)
        if len(kinds) != len(set(kinds)):
            raise SourceDescriptionError(
                f"{path} contains a duplicate runtime-policy kind"
            )
        if "open_loop_grasp_acknowledgement" in kinds:
            open_loop_sources.add(deployment_source)

    if deployment_sources != sorted(deployment_sources) or len(
        set(deployment_sources)
    ) != len(deployment_sources):
        raise SourceDescriptionError(
            "$.deployments must be unique and canonical-sorted"
        )
    if set(deployment_sources) != _EXPECTED_DEPLOYMENT_SOURCES:
        raise SourceDescriptionError(
            "the exact twelve maintained Deployment inputs are incomplete"
        )
    if open_loop_sources != _EXPECTED_OPEN_LOOP_SOURCES:
        raise SourceDescriptionError(
            "the exact eight historical open-loop inputs are incomplete"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--workspace-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        validate(args.input.resolve(), args.workspace_root.resolve())
    except SourceDescriptionError as error:
        print(f"check_maintained_deployment_sources: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
