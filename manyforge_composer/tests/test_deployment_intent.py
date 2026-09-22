"""Focused contract tests for editable source-referenced Deployment intent."""

from __future__ import annotations

from pathlib import Path
import json

import pytest

from manyforge_composer.backend.deployment_intent import (
    DEPLOYMENT_INTENT_API_VERSION,
    DeploymentIntentError,
    iter_source_references,
    rebase_deployment_intent_sources,
    resolve_source_reference,
    validate_deployment_intent,
)
from manyforge_composer.backend.deployment_draft import DeploymentDraftSession


def _intent() -> dict:
    return {
        "api_version": DEPLOYMENT_INTENT_API_VERSION,
        "deployment_artifact_id": "dual_robot_cell",
        "cell_id": "cell_a",
        "name": "Dual robot cell",
        "description": "Editable source intent",
        "components": [
            {
                "component_id": "robot_b",
                "component_profile_id": "lite6_native_v1",
                "model_source": {
                    "urdf": {"project_path": "../models/lite6/model.urdf"},
                    "srdf": {"external_path": "/opt/models/lite6/model.srdf"},
                    "package_roots": {
                        "manyforge_example_models": {
                            "project_path": "../models"
                        }
                    },
                },
                "root_pose_in_cell": {
                    "position_m": [1, 0, 0],
                    "orientation_xyzw": [0, 0, 0, 1],
                },
                "target_settings": {
                    "arm": {
                        "maximum_tcp_linear_speed_mps": 0.2,
                        "controlled_stop": {
                            "stop_mode": "soft_on_path",
                            "allow_natural_completion_on_short_path": True,
                            "jerk_limits_enabled": True,
                            "deceleration_time_s": 1.0,
                            "per_joint_limits": [
                                {
                                    "source_joint_name": "joint1",
                                    "max_deceleration_rad_s2": 4.0,
                                    "settle_velocity_threshold_rad_s": 0.01,
                                }
                            ],
                        },
                    }
                },
                "configuration": {},
            },
            {
                "component_id": "robot_a",
                "component_profile_id": "ur5e_native_v1",
                "model_source": {
                    "urdf": {"project_path": "../models/ur5e/model.urdf"},
                    "package_roots": {},
                },
                "root_pose_in_cell": {
                    "position_m": [0, 0, 0],
                    "orientation_xyzw": [0, 0, 0, 2],
                },
                "target_settings": {},
                "configuration": {"controller": "native"},
            },
        ],
        "attachments": [],
        "policies": {},
        "assistant_profile_id": None,
        "compatibility": {"lane": "simulation"},
    }


def test_intent_is_closed_normalized_and_component_ordered() -> None:
    normalized = validate_deployment_intent(_intent(), source="fixture")

    assert [row["component_id"] for row in normalized["components"]] == [
        "robot_a",
        "robot_b",
    ]
    assert normalized["components"][1]["target_settings"]["arm"][
        "maximum_tcp_linear_speed_mps"
    ] == 0.2
    assert normalized["components"][0]["root_pose_in_cell"][
        "orientation_xyzw"
    ] == [0.0, 0.0, 0.0, 2.0]


@pytest.mark.parametrize(
    "mutate, message",
    [
        (lambda value: value.__setitem__("compiled_graph", {}), "unexpected field"),
        (
            lambda value: value["components"][0].__setitem__(
                "component_id", "robot_a"
            ),
            "duplicate component_id",
        ),
        (
            lambda value: value["components"][0]["model_source"].__setitem__(
                "urdf", {"project_path": "/absolute/not/project-relative"}
            ),
            "project-relative",
        ),
        (
            lambda value: value["components"][0]["model_source"].__setitem__(
                "srdf", {"external_path": "relative/model.srdf"}
            ),
            "absolute path",
        ),
        (
            lambda value: value["components"][0]["target_settings"]["arm"].__setitem__(
                "maximum_tcp_linear_speed_mps", 0
            ),
            "positive and finite",
        ),
    ],
)
def test_intent_rejects_generated_fields_aliases_and_invalid_values(
    mutate, message: str
) -> None:
    value = _intent()
    mutate(value)

    with pytest.raises(DeploymentIntentError, match=message):
        validate_deployment_intent(value, source="fixture")


def test_source_reference_inventory_is_exact_and_stable() -> None:
    rows = list(iter_source_references(_intent()))

    assert [(path[-1], directory) for path, _reference, directory in rows] == [
        ("urdf", False),
        ("urdf", False),
        ("srdf", False),
        ("manyforge_example_models", True),
    ]


def test_save_as_rebases_only_project_references(tmp_path: Path) -> None:
    source = tmp_path / "old" / "cell"
    destination = tmp_path / "new" / "cell-copy"
    rebased = rebase_deployment_intent_sources(
        _intent(),
        source_project_root=source,
        destination_project_root=destination,
    )
    original_rows = list(iter_source_references(_intent()))
    rebased_rows = list(iter_source_references(rebased))

    for (_, original, _), (_, replacement, _) in zip(
        original_rows, rebased_rows, strict=True
    ):
        assert resolve_source_reference(
            original, project_root=source
        ) == resolve_source_reference(replacement, project_root=destination)
    assert rebased["components"][1]["model_source"]["srdf"] == {
        "external_path": "/opt/models/lite6/model.srdf"
    }


def test_draft_session_owns_intent_without_inventing_compiled_revisions() -> None:
    session = DeploymentDraftSession()
    initial = session.retained_state()
    prepared = session.preview_source(json.dumps(_intent()))

    session.commit_open(
        prepared,
        "/tmp/cell/deployment.yaml",
        draft_id=initial["draft_id"],
        expected_revision=initial["draft_revision"],
        confirm_discard=False,
    )

    snapshot = session.snapshot()
    assert snapshot["draft"]["document"] == validate_deployment_intent(
        _intent(), source="fixture"
    )
    assert snapshot["draft"]["deploymentArtifactId"] == "dual_robot_cell"
    assert snapshot["draft"]["deploymentRevision"] is None
    assert snapshot["draft"]["graphRevision"] is None
    assert snapshot["draft"]["differsFromSaved"] is False


def test_draft_session_edits_intent_target_setting_and_round_trips_state() -> None:
    session = DeploymentDraftSession()
    initial = session.retained_state()
    opened = session.preview_source(json.dumps(_intent()))
    session.commit_open(
        opened,
        "/tmp/cell/deployment.yaml",
        draft_id=initial["draft_id"],
        expected_revision=initial["draft_revision"],
        confirm_discard=False,
    )
    current = session.retained_state()
    edited = session.preview_motion_target_limit_update(
        controllable_component_id="robot_b",
        command_target_id="arm",
        maximum_tcp_linear_speed_mps=0.4,
        draft_id=current["draft_id"],
        expected_revision=current["draft_revision"],
    )
    session.commit_compiled(
        edited,
        draft_id=current["draft_id"],
        expected_revision=current["draft_revision"],
    )

    retained = session.retained_state()
    restored = DeploymentDraftSession()
    restored.restore_retained_state(retained)
    document = restored.snapshot()["draft"]["document"]
    assert document["components"][1]["target_settings"]["arm"][
        "maximum_tcp_linear_speed_mps"
    ] == 0.4
    assert restored.retained_state() == retained


def test_draft_session_edits_one_authored_stop_profile_by_target() -> None:
    session = DeploymentDraftSession()
    initial = session.retained_state()
    opened = session.preview_source(json.dumps(_intent()))
    session.commit_open(
        opened,
        "/tmp/cell/deployment.yaml",
        draft_id=initial["draft_id"],
        expected_revision=initial["draft_revision"],
        confirm_discard=False,
    )
    current = session.retained_state()
    edited = session.preview_controlled_stop_update(
        {
            "stop_mode": "spring_back_hold",
            "allow_natural_completion_on_short_path": False,
            "jerk_limits_enabled": False,
            "deceleration_time_s": 1.25,
            "per_resource_limits": [
                {
                    "resource_id": "joint1",
                    "max_deceleration": {
                        "coordinate_kind": "angular_rad_s2",
                        "value": 3.5,
                    },
                    "settle_velocity_threshold": {
                        "coordinate_kind": "angular_rad_s",
                        "value": 0.02,
                    },
                }
            ],
        },
        controllable_component_id="robot_b",
        command_target_id="arm",
        timing={
            "command_latency_allowance_ns": 250_000_000,
            "settling_allowance_ns": 750_000_000,
            "operational_timeout_ns": 3_000_000_000,
        },
        draft_id=current["draft_id"],
        expected_revision=current["draft_revision"],
    )

    stop = edited.document["components"][1]["target_settings"]["arm"][
        "controlled_stop"
    ]
    assert stop["stop_mode"] == "spring_back_hold"
    assert stop["per_joint_limits"] == [
        {
            "source_joint_name": "joint1",
            "max_deceleration_rad_s2": 3.5,
            "settle_velocity_threshold_rad_s": 0.02,
        }
    ]
    assert stop["timing"]["operational_timeout_ns"] == 3_000_000_000
