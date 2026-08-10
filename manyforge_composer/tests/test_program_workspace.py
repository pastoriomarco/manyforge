"""Closed Draft/Saved Program workspace and descriptor-bound artifact tests."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import shutil

from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend import program_artifacts
from manyforge_composer.backend.program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    save_program_artifact,
)
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState


_ROOT = Path(__file__).resolve().parents[2]
_SOURCE_PROGRAM = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"


def _copy_program(tmp_path: Path, name: str = "fixture.program.yaml") -> Path:
    target = tmp_path / name
    shutil.copyfile(_SOURCE_PROGRAM, target)
    return target


def _client(tmp_path: Path) -> TestClient:
    return TestClient(
        create_app(
            ComposerState(),
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )


def _fence(workspace: dict) -> dict:
    return {
        "draftId": workspace["draft"]["draftId"],
        "expectedDraftRevision": workspace["draft"]["draftRevision"],
    }


def test_open_mutate_save_and_save_as_preserve_exact_workspace_facts(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()

    opened_response = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source), "confirmDiscard": False},
    )
    assert opened_response.status_code == 200
    opened = opened_response.json()
    assert opened["draft"]["draftId"] != empty["draft"]["draftId"]
    assert opened["saved"]["path"] == str(source.resolve())
    assert opened["saved"]["matchesDraft"] is True
    assert opened["applied"]["available"] is False

    mutated_response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "parameters": {"planning_timeout_s": 3.0},
            },
        },
    )
    assert mutated_response.status_code == 200
    mutated = mutated_response.json()["workspace"]
    assert mutated["hasUnsavedChanges"] is True
    assert mutated["saved"]["matchesDraft"] is False

    saved_response = client.post("/api/program/save", json=_fence(mutated))
    assert saved_response.status_code == 200
    saved = saved_response.json()
    assert saved["saved"]["matchesDraft"] is True
    assert Program(source, build_node_environment().catalog).to_dict()["tree"][
        "children"
    ][0]["parameters"]["planning_timeout_s"] == 3.0

    save_as_response = client.post(
        "/api/program/save-as",
        json={
            **_fence(saved),
            "path": str(tmp_path / "copy"),
            "confirmOverwrite": False,
        },
    )
    assert save_as_response.status_code == 200
    save_as = save_as_response.json()
    expected_copy = tmp_path / "copy.program.yaml"
    assert save_as["saved"]["path"] == str(expected_copy.resolve())
    assert expected_copy.is_file()


def test_dirty_open_confirmation_and_rotated_identity_are_fail_closed(
    tmp_path: Path,
) -> None:
    first = _copy_program(tmp_path, "first.program.yaml")
    second = _copy_program(tmp_path, "second.program.yaml")
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(first)},
    ).json()
    dirty = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "tree_draft_update_node_params",
            "arguments": {
                "instance_id": "move_to_pick",
                "expected_node_id": "motion.move_to_joint_target",
                "parameters": {"planning_timeout_s": 3.0},
            },
        },
    ).json()["workspace"]
    before = deepcopy(client.get("/api/program/workspace").json())

    rejected = client.post(
        "/api/program/open-draft",
        json={**_fence(dirty), "path": str(second), "confirmDiscard": False},
    )
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == (
        "composer_draft_discard_confirmation_required"
    )
    assert client.get("/api/program/workspace").json() == before

    replaced = client.post(
        "/api/program/open-draft",
        json={**_fence(dirty), "path": str(second), "confirmDiscard": True},
    ).json()
    assert replaced["draft"]["draftId"] != dirty["draft"]["draftId"]
    stale = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(dirty),
            "mutation": "tree_draft_delete_node",
            "arguments": {"instance_id": "remove_table"},
        },
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "composer_draft_id_stale"


def test_routes_reject_unsupported_mutation_malformed_body_and_unavailable_load(
    tmp_path: Path,
) -> None:
    client = _client(tmp_path)
    workspace = client.get("/api/program/workspace").json()
    unsupported = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "runtime_apply",
            "arguments": {},
        },
    )
    assert unsupported.status_code == 404
    assert unsupported.json()["detail"]["code"] == (
        "composer_authoring_mutation_unsupported"
    )
    malformed = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "tree_draft_insert_node",
            "arguments": {},
            "unexpected": True,
        },
    )
    assert malformed.status_code == 422
    legacy_load = client.post("/api/program/load", json={})
    assert legacy_load.status_code == 422
    invalid_save_as = client.post(
        "/api/program/save-as",
        json={**_fence(workspace), "path": "/", "confirmOverwrite": False},
    )
    assert invalid_save_as.status_code == 400
    assert invalid_save_as.json()["detail"]["code"] == (
        "program_artifact_path_invalid"
    )


def test_scene_and_viewer_edits_share_one_revision_fenced_program_draft(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()

    scene_response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "scene_draft_add_object",
            "arguments": {
                "scene_resource": {
                    "scene_resource_id": "workpiece",
                    "shape": {"type": "sphere", "sphere_radius_m": 0.03},
                    "pose": {
                        "frame_ref": {"kind": "cell_frame"},
                        "position_m": [0.4, 0.0, 0.5],
                        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                    },
                }
            },
        },
    )
    assert scene_response.status_code == 200
    scene_workspace = scene_response.json()["workspace"]
    assert [
        row["scene_resource_id"]
        for row in scene_workspace["draft"]["program"]["scene"]
    ] == ["table", "workpiece"]

    stale = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "viewer_draft_replace",
            "arguments": {"viewer": {"frame_role_visibility": {"tcp": False}}},
        },
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "composer_draft_revision_stale"

    viewer_response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(scene_workspace),
            "mutation": "viewer_draft_replace",
            "arguments": {
                "viewer": {
                    "frame_role_visibility": {"tcp": False},
                    "frame_gizmo_size_m": 0.15,
                    "object_axes_gizmo_size_m": 0.1,
                }
            },
        },
    )
    assert viewer_response.status_code == 200
    viewer_workspace = viewer_response.json()["workspace"]
    assert viewer_workspace["draft"]["program"]["viewer"] == {
        "frame_role_visibility": {"tcp": False},
        "frame_gizmo_size_m": 0.15,
        "object_axes_gizmo_size_m": 0.1,
    }
    assert viewer_workspace["applied"]["available"] is False


def test_artifact_roots_symlinks_overwrite_confirmation_and_atomic_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    allowed = tmp_path / "allowed"
    outside = tmp_path / "outside"
    allowed.mkdir()
    outside.mkdir()
    outside_program = _copy_program(outside, "outside.program.yaml")
    symlink = allowed / "linked.program.yaml"
    symlink.symlink_to(outside_program)
    catalog = build_node_environment().catalog

    with pytest.raises(ProgramArtifactError) as escaped:
        load_program_artifact(
            str(symlink),
            allowed_roots=[str(allowed)],
            catalog=catalog,
        )
    assert escaped.value.status_code == 403

    program = Program(outside_program, catalog)
    existing = allowed / "existing.program.yaml"
    existing.write_text("sentinel", encoding="utf-8")
    with pytest.raises(ProgramArtifactError) as confirmation:
        save_program_artifact(
            program,
            str(existing),
            allowed_roots=[str(allowed)],
            overwrite=False,
        )
    assert confirmation.value.code == (
        "program_artifact_overwrite_confirmation_required"
    )
    assert existing.read_text(encoding="utf-8") == "sentinel"

    def fail_replace(*_args: object, **_kwargs: object) -> None:
        raise OSError("injected replace failure")

    monkeypatch.setattr(program_artifacts.os, "replace", fail_replace)
    with pytest.raises(ProgramArtifactError) as failed:
        save_program_artifact(
            program,
            str(existing),
            allowed_roots=[str(allowed)],
            overwrite=True,
        )
    assert failed.value.code == "program_artifact_write_failed"
    assert existing.read_text(encoding="utf-8") == "sentinel"
    assert list(allowed.glob(".*.tmp-*")) == []
