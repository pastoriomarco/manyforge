"""Closed Draft/Saved Program workspace and descriptor-bound artifact tests."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import shutil
from types import SimpleNamespace

from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend import program_artifacts
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.draft_session import DraftSessionError
from manyforge_composer.backend.execution_observation_stream import (
    ExecutionObservationSource,
)
from manyforge_composer.backend.program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    save_program_artifact,
)
from manyforge_composer.backend import routes_draft
from manyforge_composer.backend.routes_draft import (
    _apply_program,
    _assistant_proposal_operations,
    _observed_scene_arguments,
)
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState


_ROOT = Path(__file__).resolve().parents[2]
_SOURCE_PROGRAM = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"


class _AuthoringPassThrough:
    """Keep route-unit tests focused; the real gate has dedicated tests."""

    receiver_epoch = "composer-snapshot.test"
    mutation_ready = True

    def execute(
        self,
        _envelope,
        *,
        effect,
        prepare_effect=None,
        validate_under_claim=None,
        **_kwargs,
    ):
        if prepare_effect is not None:
            prepare_effect()
        if validate_under_claim is not None:
            validate_under_claim({})
        return effect({})


def _enable_authoring(state: ComposerState) -> ComposerState:
    state.authoring_admission = _AuthoringPassThrough()  # type: ignore[assignment]
    return state


def _copy_program(tmp_path: Path, name: str = "fixture.program.yaml") -> Path:
    target = tmp_path / name
    shutil.copyfile(_SOURCE_PROGRAM, target)
    return target


def _client(
    tmp_path: Path,
    *,
    allowed_fs_roots: list[str] | None = None,
    allowed_program_write_roots: list[str] | None = None,
) -> TestClient:
    return TestClient(
        create_app(
            _enable_authoring(ComposerState()),
            security=SecurityConfig(
                allowed_fs_roots=allowed_fs_roots or [str(tmp_path)],
                allowed_program_write_roots=allowed_program_write_roots,
                cors_origins=["http://localhost:3000"],
            ),
        )
    )


def _fence(workspace: dict) -> dict:
    return {
        "draftId": workspace["draft"]["draftId"],
        "expectedDraftRevision": workspace["draft"]["draftRevision"],
        "expectedAdmissionGeneration": 1,
        "requestOwnerEpoch": "test-browser.epoch.1",
        "requestId": "test-browser.request.1",
    }


def _motion_identity() -> dict:
    return {
        "owner_epoch": "owner-1",
        "graph_revision": "graph-1",
        "retained_bundle_id": "bundle-1",
    }


def _motion_targets_projection() -> dict:
    return {
        "ok": True,
        "identity": _motion_identity(),
        "targets": {
            "direct_control": {
                "target_role_id": "arm",
                "controllable_component_id": "robot",
                "command_target_id": "arm",
            },
        },
    }


def _motion_control_projection(*, sequence: int = 1) -> dict:
    names = [
        "shoulder_pan_joint",
        "shoulder_lift_joint",
        "elbow_joint",
        "wrist_1_joint",
        "wrist_2_joint",
        "wrist_3_joint",
    ]
    return {
        "ok": True,
        "identity": _motion_identity(),
        "control_state": {
            "observed_motion": {
                "complete": True,
                "current_tcp_pose": {
                    "position_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
                    "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                },
                "positions_rad": [0.1, -1.1, 0.9, -1.4, -1.2, 0.3],
                "sequence": sequence,
                "source_epoch": "native-source",
                "source_joint_names": names,
            }
        },
    }


def test_observed_move_reads_one_exact_source_snapshot_under_the_claim(
    tmp_path: Path,
) -> None:
    class Headless:
        def __init__(self) -> None:
            self.reads = 0

        def execution_targets(self) -> dict:
            return _motion_targets_projection()

        def execution_control_state(self) -> dict:
            self.reads += 1
            return _motion_control_projection(sequence=self.reads)

        def program_applied(self) -> dict:
            return {}

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_store_observed_move",
            "arguments": {
                "instance_id": "move_to_pick",
                "observed_kind": "joint",
            },
        },
    )

    assert response.status_code == 200, response.text
    assert state.headless_client.reads == 1
    goal = response.json()["workspace"]["draft"]["authoring"]["forest"][0][
        "children"
    ][0]["parameters"]["goal"]
    assert [row["position_rad"] for row in goal] == [
        0.1,
        -1.1,
        0.9,
        -1.4,
        -1.2,
        0.3,
    ]


def test_observed_move_is_one_revision_fenced_draft_mutation(tmp_path: Path) -> None:
    class Headless:
        def execution_targets(self) -> dict:
            return _motion_targets_projection()

        def execution_control_state(self) -> dict:
            return _motion_control_projection()

        def program_applied(self) -> dict:
            return {}

    program = Program(_SOURCE_PROGRAM, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(program=program))
    state.headless_client = Headless()  # type: ignore[assignment]
    state.execution_observation_source = ExecutionObservationSource(
        state.headless_client  # type: ignore[arg-type]
    )
    workspace = state.draft_session.workspace_snapshot()
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_store_observed_move",
            "arguments": {
                "instance_id": "move_to_pick",
                "observed_kind": "joint",
            },
        },
    )

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["workspace"]["draft"]["draftRevision"] == (
        workspace["draft"]["draftRevision"] + 1
    )
    goal = body["workspace"]["draft"]["authoring"]["forest"][0]["children"][0][
        "parameters"
    ]["goal"]
    assert [row["position_rad"] for row in goal] == [
        0.1,
        -1.1,
        0.9,
        -1.4,
        -1.2,
        0.3,
    ]


def test_observed_scene_save_preserves_program_metadata_and_source_identity() -> None:
    class Headless:
        def scene(self) -> dict:
            return {
                "logical_scene": {
                    "access": {
                        "active_projection": {
                            "retained_bundle_id": "bundle",
                            "graph_revision": "graph",
                            "scene_revision": {
                                "scene_id": "cell",
                                "revision_id": 3,
                                "content_hash": "sha256:" + "1" * 64,
                            },
                        }
                    },
                    "snapshot": {
                        "transaction_sequence": 7,
                        "objects": [{
                            "scene_resource_id": "table",
                            "placement": {
                                "kind": "cell_fixed",
                                "cell_id": "cell",
                                "pose_in_cell": {
                                    "position_m": {"x": 1.0, "y": 2.0, "z": 3.0},
                                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                                },
                            },
                            "collision_shapes": [{
                                "type": "box",
                                "dimensions_m": {"x": 0.5, "y": 0.6, "z": 0.7},
                            }],
                            "provider_evidence": None,
                            "support_contacts": [],
                        }],
                    },
                }
            }

    catalog = build_node_environment().catalog
    document = Program(_SOURCE_PROGRAM, catalog).to_dict()
    document["scene"][0]["resource_roles"] = ["support_surface"]
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    state = ComposerState(program=Program.from_dict(document, catalog))
    state.headless_client = Headless()  # type: ignore[assignment]

    arguments, identity = _observed_scene_arguments(
        state, {"scene_resource_id": "table"}
    )

    resource = arguments["scene_resource"]
    assert resource["resource_roles"] == ["support_surface"]
    assert resource["shape"] == {
        "type": "box",
        "box_dimensions_m": [0.5, 0.6, 0.7],
    }
    assert resource["pose"]["position_m"] == [1.0, 2.0, 3.0]
    assert identity["transaction_sequence"] == 7


def test_assistant_scene_proposal_honours_reviewed_filters_and_overrides(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    digest = "sha256:" + "a" * 64
    arguments = {
        "inspection_report": {
            "objects": [
                {
                    "source_root": "/World/keep",
                    "scene_resource": {
                        "scene_resource_id": "part",
                        "shape": {
                            "type": "mesh",
                            "mesh_resource_uri": "old.glb",
                            "mesh_scale": [1.0, 1.0, 1.0],
                        },
                        "pose": {
                            "frame_ref": {"kind": "cell_frame"},
                            "position_m": [0.0, 0.0, 0.0],
                            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                        },
                    },
                },
                {
                    "source_root": "/World/drop",
                    "scene_resource": {
                        "scene_resource_id": "ignored",
                        "shape": {"type": "sphere", "sphere_radius_m": 0.1},
                        "pose": {
                            "frame_ref": {"kind": "cell_frame"},
                            "position_m": [0.0, 0.0, 0.0],
                            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                        },
                    },
                },
            ]
        },
        "source_roots": ["/World/keep"],
        "scene_resource_id_prefix": "imported_",
        "mesh_resource_overrides": [{
            "source_root": "/World/keep",
            "mesh_resource_uri": "approved.glb",
        }],
        "max_proposals": 1,
    }

    class Assistant:
        def status(self, _request_id: str) -> dict:
            return {
                "state": "completed",
                "response": {
                    "assistantMode": "composer-assistant",
                    "catalogHash": digest,
                    "proposals": [{
                        "toolId": "propose_scene_objects",
                        "effect": "proposal",
                        "arguments": arguments,
                        "inputSchemaHash": digest,
                        "status": "draft",
                        "requiresReview": True,
                    }],
                },
            }

    state = ComposerState()
    state.assistant_client = Assistant()  # type: ignore[assignment]
    monkeypatch.setattr(
        routes_draft,
        "read_active_assistant_context",
        lambda *_args, **_kwargs: SimpleNamespace(
            assistant_mode=SimpleNamespace(payload={
                "catalog_hash": digest,
                "tools": [{
                    "tool_id": "propose_scene_objects",
                    "effect": "proposal",
                    "input_schema_hash": digest,
                    "input_schema": {
                        "type": "object",
                        "additionalProperties": True,
                    },
                }]
            })
        ),
    )

    resolved, identity = _assistant_proposal_operations(
        state, {"request_id": "assistant.request.1", "proposal_index": 0}
    )

    resources = resolved["operations"][0]["arguments"]["scene_resources"]
    assert len(resources) == 1
    assert resources[0]["scene_resource_id"] == "imported_part"
    assert resources[0]["shape"]["mesh_resource_uri"] == "approved.glb"
    assert identity["arguments_digest"].startswith("sha256:")


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


def test_node_policy_mutation_is_closed_draft_only_and_revision_fenced(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    original_bytes = source.read_bytes()
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()
    arguments = {
        "instance_id": "move_to_pick",
        "modification_policy": {
            "runtime_override": "human_approval",
            "persistent_edit": "human_approval",
            "intervention_delegate": "human_only",
        },
        "recovery_policy": {
            "allow_intervention": True,
            "allowed_recovery_actions": ["intervention"],
            "max_retries": 1,
            "allowed_skills": [],
            "persistence_ceiling": "draft_patch",
        },
        "operator_stop_policy": "hold",
    }

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "program_draft_update_node_policies",
            "arguments": arguments,
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert set(body) == {"mutation", "details", "workspace"}
    assert body["mutation"] == "program_draft_update_node_policies"
    assert body["details"] == {"updated_instance_id": "move_to_pick"}
    draft = body["workspace"]["draft"]
    assert draft["draftRevision"] == opened["draft"]["draftRevision"] + 1
    assert source.read_bytes() == original_bytes

    stale = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence({"draft": draft}),
            "mutation": "program_draft_update_node_policies",
            "arguments": {
                **arguments,
                "expected_program_revision": "sha256:" + "f" * 64,
            },
        },
    )
    assert stale.status_code == 400
    assert stale.json()["detail"]["code"] == "draft_arguments_invalid"
    assert client.get("/api/program/workspace").json()["draft"] == draft


def test_program_settings_mutation_is_validated_and_draft_only(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    original_bytes = source.read_bytes()
    client = _client(tmp_path)
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(opened),
            "mutation": "program_draft_update_settings",
            "arguments": {
                "name": "Portable pick",
                "description": "Edited only in the Composer Draft.",
                "program_profile": "composable-cell-v1",
                "compatibility": {
                    "lanes": ["manyforge-only", "real-robot"],
                    "assistant": "optional",
                    "requires": ["scene.mutable"],
                    "tags": ["pick"],
                },
                "execution_groups": {},
            },
        },
    )

    assert response.status_code == 200, response.text
    workspace = response.json()["workspace"]
    assert workspace["draft"]["program"]["name"] == "Portable pick"
    assert workspace["draft"]["program"]["compatibility"]["lanes"] == [
        "manyforge-only",
        "real-robot",
    ]
    assert workspace["hasUnsavedChanges"] is True
    assert source.read_bytes() == original_bytes


@pytest.mark.parametrize("invalid", ["skills", "instance_id"])
def test_node_policy_rejections_are_atomic_and_do_not_read_headless(
    tmp_path: Path,
    invalid: str,
) -> None:
    source = _copy_program(tmp_path)
    original_bytes = source.read_bytes()

    class FailOnHeadlessCall:
        def __init__(self) -> None:
            self.calls: list[str] = []

        def __getattr__(self, name: str) -> object:
            self.calls.append(name)
            raise AssertionError(f"unexpected headless call: {name}")

    headless = FailOnHeadlessCall()
    program = Program(source, build_node_environment().catalog)
    state = _enable_authoring(ComposerState(
        program=program,
        program_source_path=str(source),
    ))
    before = state.draft_session.workspace_snapshot()
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )
    arguments = {
        "instance_id": "move_to_pick",
        "modification_policy": {
            "runtime_override": "human_approval",
            "persistent_edit": "human_approval",
            "intervention_delegate": "human_only",
        },
        "recovery_policy": {
            "allow_intervention": True,
            "allowed_recovery_actions": ["intervention"],
            "max_retries": 1,
            "allowed_skills": ["inspect"] if invalid == "skills" else [],
            "persistence_ceiling": "draft_patch",
        },
        "operator_stop_policy": "hold",
    }
    if invalid == "instance_id":
        arguments["instance_id"] = "a" + ("b" * 128)

    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(before),
            "mutation": "program_draft_update_node_policies",
            "arguments": arguments,
        },
    )

    assert response.status_code == 409
    if invalid == "skills":
        assert response.json()["detail"] == {
            "code": "composer_policy_skill_referent_unavailable",
            "message": (
                "Allowed recovery skills are unavailable: each skill requires "
                "an exact revision-qualified retained referent. Leave Allowed "
                "skills empty."
            ),
        }
    else:
        assert response.json()["detail"]["code"] == "draft_arguments_invalid"
    assert headless.calls == []
    assert state.draft_session.workspace_snapshot() == before
    assert source.read_bytes() == original_bytes


def test_node_policy_success_with_imported_node_a_skills_is_closed_and_headless_free(
    tmp_path: Path,
) -> None:
    catalog = build_node_environment().catalog
    document = Program(_SOURCE_PROGRAM, catalog).to_dict()
    document["tree"]["children"][0]["modification_policy"] = {
        "runtime_override": "human_approval",
        "persistent_edit": "human_approval",
        "intervention_delegate": "human_only",
    }
    document["tree"]["children"][0]["recovery_policy"] = {
        "allow_intervention": True,
        "allowed_recovery_actions": ["intervention"],
        "max_retries": 0,
        "allowed_skills": ["legacy.inspect"],
        "persistence_ceiling": "session_only",
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, catalog)

    class FailOnHeadlessCall:
        def __init__(self) -> None:
            self.calls: list[str] = []

        def __getattr__(self, name: str) -> object:
            self.calls.append(name)
            raise AssertionError(f"unexpected headless call: {name}")

    headless = FailOnHeadlessCall()
    state = _enable_authoring(ComposerState(
        program=program,
    ))
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
                cors_origins=["http://localhost:3000"],
            ),
        )
    )
    before = state.draft_session.snapshot()
    workspace = state.draft_session.workspace_snapshot()
    assert workspace["applicability"] == {
        "applyReady": False,
        "affectedInstanceIds": ["move_to_pick"],
        "unavailableReason": {
            "code": "composer_policy_skill_referent_unavailable",
            "message": (
                "Allowed recovery skills are unavailable: each skill requires "
                "an exact revision-qualified retained referent. Leave Allowed "
                "skills empty."
            ),
            "restorationGates": [],
        },
    }
    response = client.post(
        "/api/program/draft/mutations",
        json={
            **_fence(workspace),
            "mutation": "program_draft_update_node_policies",
            "arguments": {
                "instance_id": "remove_table",
                "modification_policy": {
                    "runtime_override": "forbidden",
                    "persistent_edit": "forbidden",
                    "intervention_delegate": "forbidden",
                },
                "recovery_policy": {
                    "allow_intervention": False,
                    "allowed_recovery_actions": [],
                    "max_retries": 0,
                    "allowed_skills": [],
                    "persistence_ceiling": "session_only",
                },
                "operator_stop_policy": "hold",
            },
        },
    )

    assert response.status_code == 200
    body = response.json()
    assert set(body) == {"mutation", "details", "workspace"}
    assert body["workspace"]["draft"]["program"]["tree"]["children"][0][
        "recovery_policy"
    ]["allowed_skills"] == ["legacy.inspect"]
    assert body["workspace"]["draft"]["draftRevision"] == before["draft_revision"] + 1

    blocked_apply = client.post(
        "/api/program/apply",
        json={
            **_fence(body["workspace"]),
        },
    )
    assert blocked_apply.status_code == 409
    assert blocked_apply.json()["detail"] == {
        "code": "composer_policy_skill_referent_unavailable",
        "message": (
            "Allowed recovery skills are unavailable: each skill requires "
            "an exact revision-qualified retained referent. Leave Allowed "
            "skills empty."
        ),
        "context": {"affectedInstanceIds": ["move_to_pick"]},
    }
    assert headless.calls == []

    with pytest.raises(DraftSessionError) as direct_apply:
        _apply_program(
            state,
            program=program,
            scenario_id="cell-pick",
            scenario_catalog_revision="sha256:" + "7" * 64,
            expected_applied_record_digest=None,
            authoring_claim={},
        )
    assert direct_apply.value.status_code == 409
    assert direct_apply.value.code == "composer_policy_skill_referent_unavailable"
    assert headless.calls == []


def test_examples_open_read_only_and_save_as_is_confined_to_program_workspace(
    tmp_path: Path,
) -> None:
    workspace_root = tmp_path / "program-workspace"
    outside_root = tmp_path / "outside"
    workspace_root.mkdir()
    outside_root.mkdir()
    client = _client(
        tmp_path,
        allowed_fs_roots=[str(_ROOT / "examples"), str(workspace_root)],
        allowed_program_write_roots=[str(workspace_root)],
    )

    empty = client.get("/api/program/workspace").json()
    opened_response = client.post(
        "/api/program/open-draft",
        json={
            **_fence(empty),
            "path": str(_SOURCE_PROGRAM),
            "confirmDiscard": False,
        },
    )
    assert opened_response.status_code == 200
    opened = opened_response.json()
    assert opened["saved"]["path"] == str(_SOURCE_PROGRAM.resolve())

    before_failed_save = client.get("/api/program/workspace").json()
    denied_save = client.post("/api/program/save", json=_fence(before_failed_save))
    assert denied_save.status_code == 403
    assert denied_save.json()["detail"]["code"] == "program_artifact_path_invalid"
    assert client.get("/api/program/workspace").json() == before_failed_save

    for path in (_SOURCE_PROGRAM, outside_root / "outside"):
        denied_save_as = client.post(
            "/api/program/save-as",
            json={
                **_fence(before_failed_save),
                "path": str(path),
                "confirmOverwrite": False,
            },
        )
        assert denied_save_as.status_code == 403
        assert (
            denied_save_as.json()["detail"]["code"]
            == "program_artifact_path_invalid"
        )
        assert client.get("/api/program/workspace").json() == before_failed_save

    saved = client.post(
        "/api/program/save-as",
        json={
            **_fence(before_failed_save),
            "path": str(workspace_root / "saved"),
            "confirmOverwrite": False,
        },
    )
    assert saved.status_code == 200
    assert saved.json()["saved"]["path"] == str(
        (workspace_root / "saved.program.yaml").resolve()
    )


def test_explicit_invalid_program_write_roots_fail_closed_without_draft_change(
    tmp_path: Path,
) -> None:
    source = _copy_program(tmp_path)
    client = _client(
        tmp_path,
        allowed_program_write_roots=["invalid\x00root"],
    )
    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()
    before = client.get("/api/program/workspace").json()

    rejected = client.post("/api/program/save", json=_fence(before))

    assert rejected.status_code == 503
    assert rejected.json()["detail"]["code"] == "program_artifact_roots_unavailable"
    assert client.get("/api/program/workspace").json() == before


def test_create_app_security_fallback_overrides_preseeded_write_roots(
    tmp_path: Path,
) -> None:
    read_root = tmp_path / "read-root"
    preseeded_write_root = tmp_path / "preseeded-write-root"
    read_root.mkdir()
    preseeded_write_root.mkdir()
    source = _copy_program(read_root)
    state = _enable_authoring(ComposerState(
        allowed_fs_roots=[str(preseeded_write_root)],
        allowed_program_write_roots=[str(preseeded_write_root)],
    ))
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(read_root)],
                allowed_program_write_roots=None,
                cors_origins=["http://localhost:3000"],
            ),
        )
    )

    empty = client.get("/api/program/workspace").json()
    opened = client.post(
        "/api/program/open-draft",
        json={**_fence(empty), "path": str(source)},
    ).json()
    before = client.get("/api/program/workspace").json()

    rejected = client.post(
        "/api/program/save-as",
        json={
            **_fence(before),
            "path": str(preseeded_write_root / "bypass"),
            "confirmOverwrite": False,
        },
    )

    assert state.allowed_program_write_roots is None
    assert rejected.status_code == 403
    assert rejected.json()["detail"]["code"] == "program_artifact_path_invalid"
    assert client.get("/api/program/workspace").json() == before
    assert not (preseeded_write_root / "bypass.program.yaml").exists()


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
    assert legacy_load.status_code == 410
    assert legacy_load.json()["detail"]["code"] == (
        "program_load_removed_use_scenario_load"
    )
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
