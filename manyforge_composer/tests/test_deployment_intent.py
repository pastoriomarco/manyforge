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


def test_deployment_intent_is_portable_and_rejects_authored_runtime_identity() -> None:
    intent = _intent()
    normalized = validate_deployment_intent(intent, source="test")
    assert "cell_id" not in normalized
    intent["cell_id"] = "foreign_runtime"
    with pytest.raises(DeploymentIntentError, match="cell_id"):
        validate_deployment_intent(intent, source="test")


def _intent() -> dict:
    return {
        "api_version": DEPLOYMENT_INTENT_API_VERSION,
        "deployment_artifact_id": "dual_robot_cell",
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


def _stop_limits(value: dict) -> list:
    return value["components"][0]["target_settings"]["arm"]["controlled_stop"][
        "per_joint_limits"
    ]


def test_intent_keeps_authored_stop_limit_order() -> None:
    intent = _intent()
    _stop_limits(intent).insert(
        0,
        {
            "source_joint_name": "joint2",
            "max_deceleration_rad_s2": 2.0,
            "settle_velocity_threshold_rad_s": 0.02,
        },
    )

    normalized = validate_deployment_intent(intent, source="fixture")

    assert [
        row["source_joint_name"]
        for row in normalized["components"][1]["target_settings"]["arm"][
            "controlled_stop"
        ]["per_joint_limits"]
    ] == ["joint2", "joint1"]


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
        (
            lambda value: _stop_limits(value).append(dict(_stop_limits(value)[0])),
            "duplicate source joint 'joint1'",
        ),
        (
            lambda value: _stop_limits(value)[0].pop("source_joint_name"),
            "missing source_joint_name",
        ),
        (
            lambda value: _stop_limits(value)[0].__setitem__("source_joint_name", " "),
            "source_joint_name must be bounded non-empty text",
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


@pytest.mark.parametrize("stop_mode", ["spring_back_hold", "spring_back_fast"])
def test_draft_session_edits_one_authored_stop_profile_by_target(stop_mode: str) -> None:
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
            "stop_mode": stop_mode,
            "allow_natural_completion_on_short_path": False,
            "jerk_limits_enabled": False,
            "deceleration_time_s": 1.25,
            "max_stop_time_s": 3.0,
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
        draft_id=current["draft_id"],
        expected_revision=current["draft_revision"],
    )

    stop = edited.document["components"][1]["target_settings"]["arm"][
        "controlled_stop"
    ]
    assert stop["stop_mode"] == stop_mode
    assert stop["per_joint_limits"] == [
        {
            "source_joint_name": "joint1",
            "max_deceleration_rad_s2": 3.5,
            "settle_velocity_threshold_rad_s": 0.02,
        }
    ]
    # Q51: the robot's own explicit maximum Stop time lives in its profile.
    assert stop["max_stop_time_s"] == 3.0
    assert "timing" not in stop


def test_frame_prefix_has_empty_default_and_preserves_authored_literal() -> None:
    intent = _intent()
    normalized = validate_deployment_intent(intent, source="test")
    assert all(component["frame_prefix"] == "" for component in normalized["components"])
    intent["components"][0]["frame_prefix"] = "second/"
    normalized = validate_deployment_intent(intent, source="test")
    assert next(component for component in normalized["components"] if component["component_id"] == "robot_b")["frame_prefix"] == "second/"
    intent["components"][0]["frame_prefix"] = 1
    with pytest.raises(DeploymentIntentError, match="frame_prefix must be a string"):
        validate_deployment_intent(intent, source="test")


@pytest.mark.parametrize("prefix", ["", "second/", "Robot_2/nested/", "0/", "_/", "robot//"])
def test_frame_prefix_preserves_valid_custom_text(prefix: str) -> None:
    intent = _intent()
    intent["components"][0]["frame_prefix"] = prefix
    normalized = validate_deployment_intent(intent, source="test")
    assert next(component for component in normalized["components"]
                if component["component_id"] == "robot_b")["frame_prefix"] == prefix


@pytest.mark.parametrize("prefix", [
    "literal prefix/", "/robot/", "/", "robot", "robot-b/", "robot.b/",
    "röbot/", "robot/\n", "robot\\/",
])
def test_frame_prefix_refuses_invalid_format_with_reason(prefix: str) -> None:
    intent = _intent()
    intent["components"][0]["frame_prefix"] = prefix
    with pytest.raises(DeploymentIntentError, match="frame_prefix must be empty or use only ASCII letters"):
        validate_deployment_intent(intent, source="test")


def test_existing_robot_prefix_edit_preserves_peers_and_add_boundary() -> None:
    from copy import deepcopy
    from manyforge_composer.backend.state import _controllable_composition_change_kinds
    before = validate_deployment_intent(_intent(), source="test")
    changed = deepcopy(before)
    changed["components"][0]["frame_prefix"] = "robot_a/"
    assert _controllable_composition_change_kinds(before, changed) == ()
    assert changed["components"][1] == before["components"][1]
    newcomer = deepcopy(changed["components"][0])
    newcomer["component_id"] = "robot_c"
    changed["components"].append(newcomer)
    assert "controllable_components" in _controllable_composition_change_kinds(before, changed)


def _io_device() -> dict:
    return {
        "component_id": "station_io",
        "component_profile_id": "io_device_v0",
        "target_settings": {},
        "configuration": {
            "runtime_kind": "io_provider",
            "runtime_configuration": {
                "io_provider_devices": [
                    {
                        "controllable_component_id": "station_io",
                        "lifecycle_command_timeout_s": 30,
                    }
                ],
                "io_signals": [],
            },
        },
    }


def test_io_device_component_is_model_free_and_appendable() -> None:
    intent = _intent()
    session = DeploymentDraftSession()
    initial = session.retained_state()
    session.commit_open(
        session.preview_source(json.dumps(intent)),
        "/tmp/cell/deployment.yaml",
        draft_id=initial["draft_id"],
        expected_revision=initial["draft_revision"],
        confirm_discard=False,
    )
    state = session.retained_state()
    appended = session.preview_update(
        ["components"],
        _io_device(),
        "append",
        draft_id=state["draft_id"],
        expected_revision=state["draft_revision"],
    )
    rows = {row["component_id"]: row for row in appended.document["components"]}
    assert rows["station_io"] == _io_device()
    for forbidden in ("model_source", "root_pose_in_cell", "frame_prefix"):
        assert forbidden not in rows["station_io"]
    # The device contributes no source reference.
    with_device = json.loads(json.dumps(intent))
    with_device["components"].append(_io_device())
    assert list(iter_source_references(with_device)) == list(
        iter_source_references(intent)
    )


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (lambda row: row.update(model_source={}), "model_source"),
        (lambda row: row.update(root_pose_in_cell={}), "root_pose_in_cell"),
        (lambda row: row.update(frame_prefix="io/"), "frame_prefix"),
        (lambda row: row.update(default_fault_stop_profile={}), "default_fault_stop_profile"),
        (lambda row: row.update(target_settings={"io": {}}), "target_settings"),
        (lambda row: row.update(component_id="Station"), "component_id"),
    ],
)
def test_io_device_component_refuses_physical_fields(mutate, message: str) -> None:
    intent = _intent()
    device = _io_device()
    mutate(device)
    intent["components"].append(device)
    with pytest.raises(DeploymentIntentError, match=message):
        validate_deployment_intent(intent, source="test")


def test_io_device_component_id_stays_unique() -> None:
    intent = _intent()
    device = _io_device()
    device["component_id"] = intent["components"][0]["component_id"]
    intent["components"].append(device)
    with pytest.raises(DeploymentIntentError, match="duplicate component_id"):
        validate_deployment_intent(intent, source="test")


def test_io_device_component_has_no_model_source_closure(tmp_path: Path) -> None:
    from manyforge_behavior.model_source_capture import (
        ModelSourceCaptureError,
        capture_deployment_model_sources,
    )

    closure = capture_deployment_model_sources(
        {"components": [_io_device()]}, project_root=tmp_path
    )
    assert closure["components"] == []
    with pytest.raises(ModelSourceCaptureError):
        capture_deployment_model_sources(
            {"components": [_io_device(), _io_device()]}, project_root=tmp_path
        )


def _pose_source() -> dict:
    return {
        "component_id": "isaac_scene_objects",
        "component_profile_id": "pose_source_v0",
        "target_settings": {},
        "configuration": {
            "runtime_kind": "pose_source",
            "runtime_configuration": {
                "pose_sources": [
                    {
                        "controllable_component_id": "isaac_scene_objects",
                        "simulation_poll_period_ms": 250,
                        "simulation_entities": [],
                        "detection3d_sources": [],
                    }
                ]
            },
        },
    }


def test_pose_source_component_is_model_free_and_preserved() -> None:
    intent = _intent()
    intent["components"].append(_pose_source())
    normalized = validate_deployment_intent(intent, source="test")
    rows = {row["component_id"]: row for row in normalized["components"]}
    assert rows["isaac_scene_objects"] == _pose_source()
    # It contributes no source reference (normalization sorts components, so
    # compare the references without their component index).
    assert [ref for _, ref, _ in iter_source_references(intent)] == [
        ref for _, ref, _ in iter_source_references(_intent())
    ]


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (lambda row: row.update(model_source={}), "model_source"),
        (lambda row: row.update(root_pose_in_cell={}), "root_pose_in_cell"),
        (lambda row: row.update(frame_prefix="poses/"), "frame_prefix"),
        (lambda row: row.update(target_settings={"poses": {}}), "target_settings"),
    ],
)
def test_pose_source_component_refuses_physical_fields(mutate, message: str) -> None:
    intent = _intent()
    pose_source = _pose_source()
    mutate(pose_source)
    intent["components"].append(pose_source)
    with pytest.raises(DeploymentIntentError, match=message):
        validate_deployment_intent(intent, source="test")


def test_appending_model_free_components_is_a_composition_change() -> None:
    from manyforge_composer.backend.state import _controllable_composition_change_kinds
    before = validate_deployment_intent(_intent(), source="test")
    after = validate_deployment_intent(_intent(), source="test")
    after["components"] += [_io_device(), _pose_source()]
    assert _controllable_composition_change_kinds(before, after) == (
        "controllable_components",
    )


def _with_fallbacks(primary: str, fallbacks: object) -> dict:
    intent = _intent()
    stop = intent["components"][0]["target_settings"]["arm"]["controlled_stop"]
    stop["stop_mode"] = primary
    stop["fallback_stop_modes"] = fallbacks
    return intent


def _normalized_stop(intent: dict) -> dict:
    normalized = validate_deployment_intent(intent, source="test")
    robot = next(
        row for row in normalized["components"] if row["component_id"] == "robot_b"
    )
    return robot["target_settings"]["arm"]["controlled_stop"]


@pytest.mark.parametrize(
    ("primary", "fallbacks"),
    [
        ("soft_on_path", ["spring_back_hold"]),
        ("soft_on_path", ["spring_back_fast"]),
        ("soft_on_path", ["spring_back_hold", "spring_back_fast"]),
        ("spring_back_hold", ["spring_back_fast"]),
    ],
)
def test_intent_accepts_ordered_fallback_stop_modes(primary, fallbacks) -> None:
    stop = _normalized_stop(_with_fallbacks(primary, fallbacks))
    assert stop["stop_mode"] == primary
    assert stop["fallback_stop_modes"] == fallbacks


@pytest.mark.parametrize("primary", ["soft_on_path", "spring_back_hold", "spring_back_fast"])
def test_intent_fallback_stop_modes_absent_or_empty_is_primary_then_hard(primary) -> None:
    absent = _intent()
    absent["components"][0]["target_settings"]["arm"]["controlled_stop"][
        "stop_mode"
    ] = primary
    assert "fallback_stop_modes" not in _normalized_stop(absent)
    # [] equals absent and is not emitted, so documents without fallbacks stay
    # byte-identical.
    assert "fallback_stop_modes" not in _normalized_stop(_with_fallbacks(primary, []))


@pytest.mark.parametrize(
    ("primary", "fallbacks", "message"),
    [
        ("spring_back_hold", ["soft_on_path"], "only list modes below"),
        ("spring_back_fast", ["spring_back_hold"], "only list modes below"),
        ("spring_back_hold", ["spring_back_hold"], "only list modes below"),
        ("spring_back_fast", ["spring_back_fast"], "only list modes below"),
        ("soft_on_path", ["spring_back_fast", "spring_back_hold"], "fixed order"),
        ("soft_on_path", ["spring_back_hold", "spring_back_hold"], "duplicate"),
        ("soft_on_path", ["hard_deceleration"], "always the final step"),
        ("soft_on_path", ["spring_back_hold", "hard_deceleration"], "always the final step"),
        ("soft_on_path", ["spring_back_slow"], "unknown Stop mode"),
        ("soft_on_path", [1], "unknown Stop mode"),
        ("soft_on_path", "spring_back_hold", "must be a list"),
        ("soft_on_path", None, "must be a list"),
    ],
)
def test_intent_rejects_invalid_fallback_stop_modes(primary, fallbacks, message) -> None:
    with pytest.raises(DeploymentIntentError, match="fallback_stop_modes") as error:
        validate_deployment_intent(_with_fallbacks(primary, fallbacks), source="test")
    assert message in str(error.value)


def _stop_update(stop_mode: str, **extra: object) -> dict:
    return {
        "stop_mode": stop_mode,
        "allow_natural_completion_on_short_path": False,
        "jerk_limits_enabled": False,
        "deceleration_time_s": 1.0,
        "per_resource_limits": [
            {
                "resource_id": "joint1",
                "max_deceleration": {"coordinate_kind": "angular_rad_s2", "value": 4.0},
                "settle_velocity_threshold": {
                    "coordinate_kind": "angular_rad_s",
                    "value": 0.01,
                },
            }
        ],
        **extra,
    }


def _opened_session() -> tuple[DeploymentDraftSession, dict]:
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
    return session, session.retained_state()


def test_draft_stop_update_writes_validated_fallback_sequence() -> None:
    from manyforge_composer.backend.deployment_draft import DeploymentDraftError

    session, current = _opened_session()
    fence = {
        "controllable_component_id": "robot_b",
        "command_target_id": "arm",
        "draft_id": current["draft_id"],
        "expected_revision": current["draft_revision"],
    }

    def stop_of(edited) -> dict:
        return edited.document["components"][1]["target_settings"]["arm"][
            "controlled_stop"
        ]

    edited = session.preview_controlled_stop_update(
        _stop_update(
            "soft_on_path",
            fallback_stop_modes=["spring_back_hold", "spring_back_fast"],
        ),
        **fence,
    )
    assert stop_of(edited)["fallback_stop_modes"] == [
        "spring_back_hold",
        "spring_back_fast",
    ]
    cleared = session.preview_controlled_stop_update(
        _stop_update("soft_on_path", fallback_stop_modes=[]), **fence
    )
    assert "fallback_stop_modes" not in stop_of(cleared)
    # A profile without the field keeps today's behaviour: primary, then hard.
    plain = session.preview_controlled_stop_update(_stop_update("soft_on_path"), **fence)
    assert "fallback_stop_modes" not in stop_of(plain)
    # A primary change must prune entries no longer below it (the GUI does);
    # the backend refuses a stale list with a clear reason.
    with pytest.raises(DeploymentDraftError, match="only list modes below") as error:
        session.preview_controlled_stop_update(
            _stop_update(
                "spring_back_hold",
                fallback_stop_modes=["spring_back_hold", "spring_back_fast"],
            ),
            **fence,
        )
    assert error.value.code == "deployment_controlled_stop_invalid"


def test_recorded_fallbacks_read_legacy_substitution_and_refuse_both() -> None:
    from manyforge_composer.backend.stop_fallback_sequence import (
        fallback_candidates,
        recorded_fallback_stop_modes,
    )

    legacy = {
        "stop_mode": "soft_on_path",
        "recoverable_substitution_mode": "spring_back_hold",
    }
    assert recorded_fallback_stop_modes(legacy) == ["spring_back_hold"]
    # The old field keeps its Soft-only meaning.
    assert (
        recorded_fallback_stop_modes({**legacy, "stop_mode": "spring_back_hold"}) == []
    )
    assert recorded_fallback_stop_modes({"stop_mode": "soft_on_path"}) == []
    assert recorded_fallback_stop_modes(None) == []
    assert recorded_fallback_stop_modes(
        {"stop_mode": "soft_on_path", "fallback_stop_modes": ["spring_back_fast"]}
    ) == ["spring_back_fast"]
    with pytest.raises(ValueError, match="both"):
        recorded_fallback_stop_modes({**legacy, "fallback_stop_modes": []})
    assert fallback_candidates("soft_on_path") == ("spring_back_hold", "spring_back_fast")
    assert fallback_candidates("spring_back_hold") == ("spring_back_fast",)
    assert fallback_candidates("spring_back_fast") == ()
    assert fallback_candidates("hard_deceleration") == ()
