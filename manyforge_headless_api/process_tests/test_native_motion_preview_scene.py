"""Managed motion preview accepts a lagging Scene revision of the same Scene."""

import copy
from pathlib import Path

import pytest

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.process_tests.test_process_stack import _assistant_stack
from manyforge_headless_api.tests.test_models import _planning_motion


def test_managed_preview_computes_on_the_current_scene_after_a_scene_edit(
    tmp_path: Path,
) -> None:
    with _assistant_stack(tmp_path) as stack, HeadlessHttpClient(
        stack.base_url, timeout_seconds=20.0
    ) as headless:
        status = headless.execution_diagnostics()["status"]
        identity = status["identity"]
        stale_revision = identity["scene_revision"]
        ((component, target),) = [
            (component, target)
            for component in status["targets"]["components"]
            for target in component["targets"]
            if target["presentation"] == "motion"
        ]
        (observed,) = [
            row["observed"]
            for row in status["control_state"]["observed_targets"]
            if (
                row["controllable_component_id"],
                row["command_target_id"],
                row["operation_capability_id"],
            )
            == (
                component["controllable_component_id"],
                target["command_target_id"],
                target["operation_capability_id"],
            )
        ]

        # One ordinary authored Scene edit, far from the arm, advances the
        # Manager-owned Scene revision past the one the status read named.
        scene = headless.scene()["logical_scene"]
        committed = headless.commit_scene_transaction(
            {
                "request_context": {
                    "request_id": "motion-preview-scene.add",
                    "request_owner_epoch": scene["source_owner_epoch"],
                    "source_label": "motion_preview_scene_process_test",
                    "correlation_id": None,
                },
                "expected_projection_revision": scene["payload"]["access"][
                    "active_projection"
                ],
                "expected_admission_generation": stack.wait_authoring_admission(
                    expected_allowed=True
                )["admission_generation"],
                "operation": {
                    "kind": "add_object",
                    "scene_resource_id": "motion_preview_scene_marker",
                    "shape": {
                        "type": "box",
                        "box_dimensions_m": {"x_m": 0.1, "y_m": 0.1, "z_m": 0.1},
                    },
                    "pose": {
                        "position_m": {"x_m": 3.0, "y_m": 0.0, "z_m": 0.0},
                        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                    },
                    "frame_ref": {"kind": "cell_frame", "cell_id": stack.cell_id},
                },
            }
        )
        assert committed["ok"] is True, committed
        current_revision = committed["committed_projection_revision"]["scene_revision"]
        assert current_revision["scene_id"] == stale_revision["scene_id"]
        assert current_revision["revision_id"] > stale_revision["revision_id"]

        command = target["command"]
        manual_control = command["manual_control"]
        positions = [float(value) for value in observed["positions_rad"]]
        positions[0] += 0.03
        intent = {
            "request_context": {
                "request_id": "motion-preview-scene.lagging",
                "request_owner_epoch": identity["owner_epoch"],
                "source_label": "composer_ui",
                "correlation_id": None,
            },
            "program_id": manual_control["program_id"],
            "program_revision": manual_control["program_revision"],
            "program_binding_id": manual_control["program_binding_id"],
            "program_run_id": manual_control["program_run_id"],
            "expected_graph_revision": identity["graph_revision"],
            "expected_retained_bundle_id": identity["retained_bundle_id"],
            "expected_scene_revision": stale_revision,
            "participants": [
                {
                    "component_role_id": command["component_role_id"],
                    "target_role_id": command["target_role_id"],
                    "required_capability_id": target["operation_capability_id"],
                    "action_span": None,
                    "payload": _planning_motion(
                        "robot-control-motion-preview-scene",
                        {
                            "kind": "joint_goal",
                            "joint_names": [
                                joint["source_joint_name"] for joint in command["joints"]
                            ],
                            "positions_rad": positions,
                        },
                    )
                    | {
                        "base_frame_role_id": command["base_frame_role_id"],
                        "tcp_frame_role_id": command["tcp_frame_role_id"],
                        "planning_profile": "balanced",
                        "speed_profile": {
                            "velocity_scale": 1.0,
                            "acceleration_scale": 1.0,
                            "max_tcp_linear_speed_mps": None,
                        },
                        "timeouts": {
                            "planning_timeout_s": 2.0,
                            "execution_timeout_s": 10.0,
                        },
                    },
                }
            ],
            "coordination_policy": "independent",
        }

        # The earlier revision is accepted; the receipt names the revision the
        # preview was computed on. No before-planning pose source is open, so
        # the latch leaves the committed revision current.
        receipt = headless.preview_motion(intent)["preview_receipt"]
        assert receipt["commit_eligible"] is True
        assert receipt["scene_revision"] == current_revision
        assert receipt["scene_revision"] != stale_revision

        # An equal revision id names exactly the current revision.
        forged_hash = copy.deepcopy(intent)
        forged_hash["request_context"]["request_id"] = "motion-preview-scene.hash"
        forged_hash["expected_scene_revision"] = {
            **current_revision,
            "content_hash": "sha256:" + "0" * 64,
        }
        with pytest.raises(HeadlessClientError) as different_hash:
            headless.preview_motion(forged_hash)
        assert different_hash.value.code == "scene_changed_during_validation"

        future = copy.deepcopy(intent)
        future["request_context"]["request_id"] = "motion-preview-scene.future"
        future["expected_scene_revision"] = {
            **current_revision,
            "revision_id": current_revision["revision_id"] + 1,
        }
        with pytest.raises(HeadlessClientError) as future_revision:
            headless.preview_motion(future)
        assert future_revision.value.code == "scene_changed_during_validation"
