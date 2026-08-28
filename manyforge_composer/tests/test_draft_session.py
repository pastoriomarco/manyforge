"""Atomic, Draft-only Composer Program authoring semantics."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.draft_session import (
    ComposerDraftSession,
    DraftSessionError,
    OPERATOR_DRAFT_MUTATIONS,
    SUPPORTED_DRAFT_MUTATIONS,
)


_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _REPOSITORY_ROOT / "examples" / "pick_and_place_minimal.program.yaml"
_PICK_PROGRAM_PATH = (
    _REPOSITORY_ROOT / "examples" / "trocar_lite6_foundationpose_pick.program.yaml"
)


def _program(*, metadata: dict | None = None) -> Program:
    catalog = build_node_environment().catalog
    program = Program(_PROGRAM_PATH, catalog)
    if metadata is None:
        return program
    document = program.to_dict()
    document["scene"][0]["metadata"] = metadata
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v1",
    )
    return Program.from_dict(document, catalog)


def _program_with_instance_id(instance_id: str) -> Program:
    catalog = build_node_environment().catalog
    document = _program().to_dict()
    document["tree"]["children"][0]["instance_id"] = instance_id
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v1",
    )
    return Program.from_dict(document, catalog)


def _pick_program() -> Program:
    return Program(_PICK_PROGRAM_PATH, build_node_environment().catalog)


def _box_resource(identity: str) -> dict:
    return {
        "scene_resource_id": identity,
        "shape": {"type": "box", "box_dimensions_m": [0.1, 0.2, 0.3]},
        "pose": {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [0.0, 0.0, 0.0],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
    }


def _mutate(session: ComposerDraftSession, tool_id: str, arguments: dict):
    return session.mutate(
        tool_id=tool_id,
        arguments=arguments,
        expected_revision=session.revision,
        catalog=build_node_environment().catalog,
    )


def _pick_macro_arguments() -> dict:
    return {
        "macro_id": "pick_plan_subtree",
        "scene_resource_id": "graspable",
        "target_role": "arm",
        "tool_target_role": "gripper",
        "base_frame_role": "base",
        "tcp_frame_role": "tcp",
        "carrier_frame_role": "tcp",
        "touch_body_roles": ["gripper_collision"],
        "tool_position": {"kind": "linear_m", "value_m": 0.0},
        "contact_policy_id": "selected_object_grasp",
        "blackboard_namespace_id": "planning",
        "blackboard_key": "planning.pick_plan",
        "instance_prefix": "authored_pick",
    }


def _node_policy_arguments(
    session: ComposerDraftSession,
    *,
    instance_id: str = "move_to_pick",
) -> dict:
    return {
        "instance_id": instance_id,
        "modification_policy": {
            "runtime_override": "human_approval",
            "persistent_edit": "human_approval",
            "intervention_delegate": "human_only",
        },
        "recovery_policy": {
            "allow_intervention": True,
            "allowed_recovery_actions": ["intervention"],
            "max_retries": 2,
            "allowed_skills": [],
            "persistence_ceiling": "draft_patch",
        },
        "operator_stop_policy": "hold",
    }


def test_contact_policy_upsert_mints_identity_and_remove_is_atomic() -> None:
    session = ComposerDraftSession(_pick_program())
    policy = deepcopy(session.snapshot()["authoring"]["contact_policies"][0])
    previous_version = policy.pop("version")
    policy["zones"][0]["max_penetration_m"] = 0.003

    result = _mutate(
        session,
        "program_draft_upsert_contact_policies",
        {"contact_policies": [policy]},
    )

    stored = session.snapshot()["authoring"]["contact_policies"][0]
    assert result.details == {
        "inserted": [],
        "updated": ["selected_object_grasp"],
    }
    assert stored["version"] != previous_version
    assert stored["version"] == compute_content_identity(
        stored,
        own_field="version",
        identity_domain="catalog-record-revision/contact_policy",
        api_version="manyforge.contact_policy.v1",
    )

    removed = _mutate(
        session,
        "program_draft_remove_contact_policies",
        {"contact_policy_ids": ["selected_object_grasp"]},
    )
    assert removed.details == {"removed": ["selected_object_grasp"]}
    assert session.snapshot()["authoring"]["contact_policies"] == []


def test_pick_authoring_macro_expands_visible_nodes_and_declarations_once() -> None:
    session = ComposerDraftSession(_pick_program())
    before_revision = session.revision

    result = _mutate(
        session,
        "program_draft_expand_authoring_macro",
        _pick_macro_arguments(),
    )

    snapshot = session.snapshot()
    forest = snapshot["authoring"]["forest"]
    inserted = next(
        node for node in forest if node["instance_id"] == "authored_pick"
    )
    assert session.revision == before_revision + 1
    assert result.details["macro_id"] == "pick_plan_subtree"
    assert result.details["inserted_instance_ids"] == [
        "authored_pick",
        "authored_pick.plan",
        "authored_pick.approach",
        "authored_pick.descend",
        "authored_pick.close",
        "authored_pick.attach",
        "authored_pick.retreat",
    ]
    assert [child["node_id"] for child in inserted["children"]] == [
        "planning.plan_pick_sequence",
        "motion.move_to_pose",
        "motion.move_to_pose",
        "gripper.command_position",
        "scene.attach_carried_object",
        "motion.move_to_pose",
    ]
    assert all(child["children"] == [] for child in inserted["children"])
    assert inserted["children"][0]["parameters"]["target_role"] == "arm"
    assert inserted["children"][4]["parameters"]["target_role"] == "gripper"
    declaration = next(
        row
        for row in snapshot["authoring"]["blackboard"]["keys"]
        if row["key"] == "planning.pick_plan"
    )
    assert declaration["type"] == "pick_planning_result"


def test_pick_authoring_macro_failure_leaves_draft_unchanged() -> None:
    session = ComposerDraftSession(_pick_program())
    arguments = _pick_macro_arguments()
    arguments["scene_resource_id"] = "missing"
    before = session.retained_state()

    with pytest.raises(DraftSessionError):
        _mutate(session, "program_draft_expand_authoring_macro", arguments)

    assert session.retained_state() == before


@pytest.mark.parametrize(
    ("node_id", "observed_kind", "observed", "expected_parameter"),
    [
        (
            "motion.move_to_joint_target",
            "joint",
            {
                "source_joint_names": ["joint1", "joint2"],
                "positions_rad": [0.25, -0.5],
            },
            {
                "goal": [
                    {"joint_name": "joint1", "position_rad": 0.25},
                    {"joint_name": "joint2", "position_rad": -0.5},
                ]
            },
        ),
        (
            "motion.move_to_pose",
            "tcp_pose",
            {
                "current_tcp_pose": {
                    "position_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
                    "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                }
            },
            {
                "goal": {
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [0.1, 0.2, 0.3],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                }
            },
        ),
    ],
)
def test_store_observed_move_updates_only_selected_node(
    node_id: str,
    observed_kind: str,
    observed: dict,
    expected_parameter: dict,
) -> None:
    catalog = build_node_environment().catalog
    document = _pick_program().to_dict()
    node = document["tree"]["children"][3]
    node["node_id"] = node_id
    node["instance_id"] = "observed_target"
    node["parameters"] = {
        "target_role": "arm",
        "base_frame_role": "base",
        "tcp_frame_role": "tcp",
        "goal": (
            [{"joint_name": "joint1", "position_rad": 0.0}]
            if observed_kind == "joint"
            else {
                "frame_ref": {"kind": "cell_frame"},
                "position_m": [0.0, 0.0, 0.0],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            }
        ),
    }
    node.pop("value_sources", None)
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v1",
    )
    session = ComposerDraftSession(Program.from_dict(document, catalog))

    _mutate(
        session,
        "program_draft_store_observed_move",
        {
            "instance_id": "observed_target",
            "expected_node_id": node_id,
            "observed_kind": observed_kind,
            "observed": observed,
        },
    )

    stored = next(
        row
        for row in session.snapshot()["authoring"]["forest"][0]["children"]
        if row["instance_id"] == "observed_target"
    )
    assert {key: stored["parameters"][key] for key in expected_parameter} == expected_parameter


def test_store_observed_pose_replaces_old_goal_source_and_relative_target() -> None:
    session = ComposerDraftSession(_pick_program())
    observed = {
        "current_tcp_pose": {
            "position_m": {"x_m": 0.4, "y_m": 0.2, "z_m": 0.1},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        }
    }

    _mutate(
        session,
        "program_draft_store_observed_move",
        {
            "instance_id": "move_to_planned_pick_approach",
            "expected_node_id": "motion.move_to_pose",
            "observed_kind": "tcp_pose",
            "observed": observed,
        },
    )

    stored = next(
        row
        for row in session.snapshot()["authoring"]["forest"][0]["children"]
        if row["instance_id"] == "move_to_planned_pick_approach"
    )
    assert stored["parameters"]["goal"]["position_m"] == [0.4, 0.2, 0.1]
    assert "goal" not in stored.get("value_sources", {})
    assert stored["value_sources"]["velocity_scale"] == {
        "kind": "program_parameter",
        "reference": "velocity_scale",
    }


def test_support_contact_replacement_is_one_validated_draft_change() -> None:
    session = ComposerDraftSession(_pick_program())
    contact = {
        "support_object_id": "ground",
        "mode": "contact_release",
        "release_evidence_mode": "binary_contact",
        "require_separation_by_end": True,
        "allow_recontact": False,
    }

    result = _mutate(
        session,
        "program_draft_replace_support_contacts",
        {
            "scene_resource_id": "graspable",
            "support_contacts": [contact],
        },
    )

    resource = next(
        row
        for row in session.snapshot()["authoring"]["scene"]
        if row["scene_resource_id"] == "graspable"
    )
    assert result.details["support_contact_count"] == 1
    assert resource["metadata"]["support_contacts"] == [contact]

    invalid = _mutate(
        session,
        "program_draft_replace_support_contacts",
        {
            "scene_resource_id": "graspable",
            "support_contacts": [{"support_object_id": "graspable"}],
        },
    )
    assert invalid.snapshot["program"] is None
    assert any(
        "cannot name the supported object itself" in row["message"]
        for row in invalid.snapshot["diagnostics"]
    )

    _mutate(
        session,
        "program_draft_replace_support_contacts",
        {"scene_resource_id": "graspable", "support_contacts": []},
    )
    resource = next(
        row
        for row in session.snapshot()["authoring"]["scene"]
        if row["scene_resource_id"] == "graspable"
    )
    assert "metadata" not in resource


def test_assistant_proposal_batch_has_no_partial_failure() -> None:
    session = ComposerDraftSession(_pick_program())
    before = session.retained_state()

    with pytest.raises(DraftSessionError):
        _mutate(
            session,
            "assistant_draft_materialize_proposal",
            {
                "proposal_tool_id": "propose_scene_object_nodes",
                "operations": [
                    {
                        "mutation": "scene_draft_upsert_objects",
                        "arguments": {"scene_resources": [_box_resource("new-object")]},
                    },
                    {
                        "mutation": "tree_draft_insert_node",
                        "arguments": {
                            "node": {
                                "node_id": "missing.node",
                                "instance_id": "bad",
                                "parameters": {},
                                "children": [],
                            }
                        },
                    },
                ],
            },
        )

    assert session.retained_state() == before


def test_session_is_an_unsaved_unapplied_in_memory_draft() -> None:
    session = ComposerDraftSession(_program())

    snapshot = session.snapshot()

    assert snapshot["schema_version"] == "manyforge.composer_draft.v2"
    assert snapshot["draft_id"].startswith("draft-")
    assert snapshot["draft_revision"] == 1
    assert snapshot["program_loaded"] is True
    assert snapshot["saved"] is False
    assert snapshot["applied"] is False


def test_workspace_distinguishes_saved_draft_from_unavailable_applied() -> None:
    program = _program()
    session = ComposerDraftSession(program, source_path=str(_PROGRAM_PATH))

    workspace = session.workspace_snapshot()

    assert workspace["schemaVersion"] == "manyforge.composer_program_workspace.v2"
    assert workspace["saved"] == {
        "available": True,
        "matchesDraft": True,
        "path": str(_PROGRAM_PATH.resolve()),
        "programRevision": program.program_revision,
    }
    assert workspace["hasUnsavedChanges"] is False
    assert workspace["hasUnappliedChanges"] is None
    assert workspace["applied"]["available"] is False
    assert workspace["applied"]["matchesDraft"] is None
    assert workspace["applied"]["unavailableReason"]["restorationGates"] == []

    _mutate(
        session,
        "tree_draft_update_node_params",
        {
            "instance_id": "move_to_pick",
            "expected_node_id": "motion.move_to_joint_target",
            "parameters": {"planning_timeout_s": 3.0},
        },
    )

    assert session.snapshot()["saved"] is False
    assert session.workspace_snapshot()["hasUnsavedChanges"] is True


def test_replacing_a_draft_rotates_identity_and_requires_dirty_confirmation() -> None:
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    original = session.workspace_snapshot()
    _mutate(
        session,
        "tree_draft_update_node_params",
        {
            "instance_id": "move_to_pick",
            "expected_node_id": "motion.move_to_joint_target",
            "parameters": {"planning_timeout_s": 3.0},
        },
    )
    dirty = session.workspace_snapshot()

    with pytest.raises(DraftSessionError) as confirmation:
        session.require_replace_allowed(
            draft_id=dirty["draft"]["draftId"],
            expected_revision=dirty["draft"]["draftRevision"],
            discard_changes=False,
        )
    assert confirmation.value.code == "composer_draft_discard_confirmation_required"

    replacement = session.replace_program(
        _program(),
        source_path=str(_PROGRAM_PATH),
        draft_id=dirty["draft"]["draftId"],
        expected_revision=dirty["draft"]["draftRevision"],
        discard_changes=True,
    )

    assert replacement["draft"]["draftId"] != original["draft"]["draftId"]
    assert replacement["draft"]["draftRevision"] == 1
    assert replacement["saved"]["matchesDraft"] is True
    with pytest.raises(DraftSessionError) as stale:
        session.replace_program(
            _program(),
            source_path=str(_PROGRAM_PATH),
            draft_id=dirty["draft"]["draftId"],
            expected_revision=dirty["draft"]["draftRevision"],
            discard_changes=True,
        )
    assert stale.value.code == "composer_draft_id_stale"


def test_save_updates_saved_identity_only_after_writer_success(tmp_path: Path) -> None:
    session = ComposerDraftSession(_program())
    before = session.workspace_snapshot()

    def fail_writer(_program: Program, _path: str) -> str:
        raise RuntimeError("disk failed")

    with pytest.raises(RuntimeError, match="disk failed"):
        session.save_program(
            draft_id=before["draft"]["draftId"],
            expected_revision=before["draft"]["draftRevision"],
            target_path=str(tmp_path / "saved.program.yaml"),
            writer=fail_writer,
        )
    assert session.workspace_snapshot() == before

    target = tmp_path / "saved.program.yaml"
    saved = session.save_program(
        draft_id=before["draft"]["draftId"],
        expected_revision=before["draft"]["draftRevision"],
        target_path=str(target),
        writer=lambda _program, path: path,
    )
    assert saved["saved"]["path"] == str(target.resolve())
    assert saved["saved"]["matchesDraft"] is True
    assert type(session._saved_document) is dict
    assert session._saved_document == session._program.to_dict()


def test_saved_document_lifecycle_never_deepcopies_program_graph(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def reject_program_deepcopy(
        _program: Program,
        _memo: dict,
    ) -> None:
        pytest.fail("immutable Program graphs must not be deep-copied")

    monkeypatch.setattr(Program, "__deepcopy__", reject_program_deepcopy, raising=False)
    catalog = build_node_environment().catalog
    initial = _program()
    session = ComposerDraftSession(initial, source_path=str(_PROGRAM_PATH))
    assert session._saved_document == initial.to_dict()

    retained_initial = deepcopy(session._saved_document)

    def corrupt_prepare(document: dict) -> tuple[Program, Program, object]:
        document["name"] = "Corrupted callback copy"
        raise RuntimeError("preparation failed")

    with pytest.raises(RuntimeError, match="preparation failed"):
        session.revert_saved_after_apply(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            prepare=corrupt_prepare,
            applier=lambda _program, _context: pytest.fail("must not Apply"),
        )
    assert session._saved_document == retained_initial

    loaded = _program(metadata={"selection": "loaded"})
    session.replace_program(
        loaded,
        source_path=str(tmp_path / "loaded.program.yaml"),
        draft_id=session.draft_id,
        expected_revision=session.revision,
        discard_changes=True,
    )
    session.save_program(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        target_path=str(tmp_path / "saved.program.yaml"),
        writer=lambda _program, path: path,
    )
    retained_saved = deepcopy(session._saved_document)

    applied = _program(metadata={"selection": "applied"})
    session.discard_to_applied(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        reader=lambda: applied,
    )
    result, reverted = session.revert_saved_after_apply(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        prepare=lambda document: (
            Program.from_dict(document, catalog),
            applied,
            "current",
        ),
        applier=lambda _program, _context: {"status": "applied"},
    )

    assert result == {"status": "applied"}
    assert session._saved_document == retained_saved
    assert reverted["draft"]["program"] == retained_saved


def test_discard_and_revert_use_exact_independent_owner_snapshots() -> None:
    saved_program = _program()
    applied_program = _program(metadata={"selection": "applied"})
    session = ComposerDraftSession(saved_program, source_path=str(_PROGRAM_PATH))
    original_saved = deepcopy(saved_program.to_dict())
    _mutate(
        session,
        "tree_draft_update_positions",
        {"positions": {"move_to_pick": {"x": 320.0, "y": 180.0}}},
    )

    discarded = session.discard_to_applied(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        reader=lambda: applied_program,
    )
    assert discarded["draft"]["program"] == applied_program.to_dict()
    assert discarded["draft"]["authoring"]["positions"] == {}
    assert discarded["saved"]["programRevision"] == saved_program.program_revision
    assert discarded["saved"]["path"] == str(_PROGRAM_PATH.resolve())
    discarded_revision = session.revision

    no_op = session.discard_to_applied(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        reader=lambda: applied_program,
    )
    assert no_op["draft"]["draftRevision"] == discarded_revision

    applied: list[dict] = []
    result, reverted = session.revert_saved_after_apply(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        prepare=lambda document: (
            Program.from_dict(document, build_node_environment().catalog),
            applied_program,
            "current",
        ),
        applier=lambda program, _context: (
            applied.append(deepcopy(program.to_dict()))
            or {"status": "applied"}
        ),
    )
    assert result["status"] == "applied"
    assert applied == [original_saved]
    assert reverted["draft"]["program"] == original_saved
    assert reverted["saved"]["programRevision"] == saved_program.program_revision
    reverted_revision = session.revision

    session.revert_saved_after_apply(
        draft_id=session.draft_id,
        expected_revision=session.revision,
        prepare=lambda document: (
            Program.from_dict(document, build_node_environment().catalog),
            saved_program,
            "current",
        ),
        applier=lambda _program, _context: pytest.fail("no-op must not Apply"),
    )
    assert session.revision == reverted_revision


def test_revert_failure_stale_fences_and_exhaustion_are_atomic() -> None:
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    _mutate(
        session,
        "tree_draft_update_node_params",
        {
            "instance_id": "move_to_pick",
            "expected_node_id": "motion.move_to_joint_target",
            "parameters": {"planning_timeout_s": 3.0},
        },
    )
    before = deepcopy(session.workspace_snapshot())

    with pytest.raises(DraftSessionError) as malformed:
        session.revert_saved_after_apply(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            prepare=lambda document: (
                Program.from_dict(document, build_node_environment().catalog),
                _program(metadata={"selection": "applied"}),
                "current",
            ),
            applier=lambda _program, _context: {"status": "pending_deployment"},
        )
    assert malformed.value.code == "program_apply_response_invalid"
    assert session.workspace_snapshot() == before

    with pytest.raises(DraftSessionError, match="deployment is not active"):
        session.revert_saved_after_apply(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            prepare=lambda document: (
                Program.from_dict(document, build_node_environment().catalog),
                _program(metadata={"selection": "applied"}),
                "current",
            ),
            applier=lambda _program, _context: (_ for _ in ()).throw(
                DraftSessionError(
                    "program_apply_deployment_not_active",
                    "deployment is not active",
                )
            ),
        )
    assert session.workspace_snapshot() == before

    with pytest.raises(DraftSessionError) as stale:
        session.discard_to_applied(
            draft_id=session.draft_id,
            expected_revision=session.revision - 1,
            reader=lambda: _program(metadata={"unexpected": True}),
        )
    assert stale.value.code == "composer_draft_revision_stale"
    assert session.workspace_snapshot() == before

    effects: list[str] = []
    session._revision = 9_007_199_254_740_991
    for action in (
        lambda: session.discard_to_applied(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            reader=lambda: effects.append("read") or _program(),
        ),
        lambda: session.revert_saved_after_apply(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            prepare=lambda document: (
                effects.append("read")
                or Program.from_dict(
                    document,
                    build_node_environment().catalog,
                ),
                _program(metadata={"selection": "applied"}),
                "current",
            ),
            applier=lambda _program, _context: (
                effects.append("apply") or {"status": "applied"}
            ),
        ),
    ):
        with pytest.raises(DraftSessionError) as exhausted:
            action()
        assert exhausted.value.code == "composer_draft_revision_exhausted"
    assert effects == ["read", "read"]


def test_new_draft_is_fresh_empty_authoring_and_clears_saved_identity() -> None:
    catalog = build_node_environment().catalog
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    before = session.workspace_snapshot(
        applied_program_revision=session.snapshot()["program_revision"]
    )

    created = session.new_draft(
        draft_id=before["draft"]["draftId"],
        expected_revision=before["draft"]["draftRevision"],
        confirm_discard=False,
        applied_revision_reader=lambda: before["draft"]["programRevision"],
        catalog_reader=lambda: catalog,
    )

    authoring = created["draft"]["authoring"]
    assert created["draft"]["program"] is None
    assert created["draft"]["candidateAvailable"] is False
    assert authoring["program_id"].startswith("program-")
    assert len(authoring["program_id"]) == len("program-") + 32
    assert authoring["forest"] == []
    assert created["draft"]["diagnostics"] == [{
        "code": "authoring_forest_root_count",
        "path": "forest",
        "message": "Program requires exactly one root; found 0.",
        "severity": "error",
    }]
    assert created["draft"]["draftId"] != before["draft"]["draftId"]
    assert created["draft"]["draftRevision"] == 1
    assert created["saved"]["available"] is False
    assert created["saved"]["path"] is None

    second = session.new_draft(
        draft_id=created["draft"]["draftId"],
        expected_revision=created["draft"]["draftRevision"],
        confirm_discard=True,
        applied_revision_reader=lambda: pytest.fail("confirmed replacement"),
        catalog_reader=lambda: catalog,
    )
    assert second["draft"]["authoring"]["program_id"] != authoring["program_id"]
    assert second["draft"]["program"] is None
    assert second["draft"]["programRevision"] is None

    with pytest.raises(DraftSessionError) as replay:
        session.new_draft(
            draft_id=before["draft"]["draftId"],
            expected_revision=before["draft"]["draftRevision"],
            confirm_discard=True,
            applied_revision_reader=lambda: pytest.fail("stale read"),
            catalog_reader=lambda: pytest.fail("stale catalog"),
        )
    assert replay.value.code == "composer_draft_id_stale"


def test_new_draft_confirmation_and_catalog_failure_are_atomic() -> None:
    catalog = build_node_environment().catalog
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    before = session.workspace_snapshot()

    with pytest.raises(DraftSessionError) as unapplied:
        session.new_draft(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            confirm_discard=False,
            applied_revision_reader=lambda: "sha256:" + "f" * 64,
            catalog_reader=lambda: pytest.fail("confirmation precedes catalog"),
        )
    assert unapplied.value.code == "composer_draft_discard_confirmation_required"
    assert session.workspace_snapshot() == before

    session._revision = 9_007_199_254_740_991
    exhausted_id = session.draft_id
    created = session.new_draft(
        draft_id=exhausted_id,
        expected_revision=session.revision,
        confirm_discard=True,
        applied_revision_reader=lambda: pytest.fail("confirmed replacement"),
        catalog_reader=lambda: catalog,
    )
    assert created["draft"]["draftId"] != exhausted_id
    assert created["draft"]["draftRevision"] == 1


def test_successful_edit_reidentifies_and_atomically_swaps_the_program() -> None:
    original = _program()
    session = ComposerDraftSession(original)

    result = _mutate(
        session,
        "scene_draft_add_object",
        {"scene_resource": _box_resource("workpiece")},
    )

    assert result.snapshot["draft_revision"] == 2
    assert result.snapshot["program_revision"] != original.program_revision
    assert [row["scene_resource_id"] for row in result.snapshot["program"]["scene"]] == [
        "table",
        "workpiece",
    ]
    assert [row["scene_resource_id"] for row in original.to_dict()["scene"]] == [
        "table"
    ]


def test_incomplete_candidate_is_retained_and_stale_revision_is_atomic() -> None:
    session = ComposerDraftSession(_program())
    incomplete = _mutate(
        session,
        "scene_draft_remove_objects",
        {"scene_resource_ids": ["table"]},
    )
    assert incomplete.snapshot["program"] is None
    assert incomplete.snapshot["candidate_available"] is False
    assert incomplete.snapshot["authoring"]["scene"] == []
    assert any(
        diagnostic["code"] == "authoring_program_invalid"
        for diagnostic in incomplete.snapshot["diagnostics"]
    )
    before_stale = deepcopy(session.snapshot())
    with pytest.raises(DraftSessionError) as stale:
        session.mutate(
            tool_id="scene_draft_add_object",
            arguments={"scene_resource": _box_resource("workpiece")},
            expected_revision=1,
            catalog=build_node_environment().catalog,
        )
    assert stale.value.code == "composer_draft_revision_stale"
    assert stale.value.status_code == 409
    assert session.snapshot() == before_stale


def test_node_policy_mutation_round_trips_nondefaults_and_reidentifies() -> None:
    original = _program()
    session = ComposerDraftSession(original)

    result = _mutate(
        session,
        "program_draft_update_node_policies",
        _node_policy_arguments(session),
    )

    node = result.snapshot["program"]["tree"]["children"][0]
    assert result.snapshot["draft_revision"] == 2
    assert result.snapshot["program_revision"] != original.program_revision
    assert node["instance_id"] == "move_to_pick"
    assert node["modification_policy"] == {
        "runtime_override": "human_approval",
        "persistent_edit": "human_approval",
        "intervention_delegate": "human_only",
    }
    assert node["recovery_policy"] == {
        "allow_intervention": True,
        "allowed_recovery_actions": ["intervention"],
        "max_retries": 2,
        "allowed_skills": [],
        "persistence_ceiling": "draft_patch",
    }
    assert node["operator_stop_policy"] == "hold"
    assert Program.from_dict(
        result.snapshot["program"],
        build_node_environment().catalog,
    ).to_dict() == result.snapshot["program"]


def test_node_policy_rejects_unavailable_skills_atomically() -> None:
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    arguments = _node_policy_arguments(session)
    arguments["recovery_policy"]["allowed_skills"] = ["inspect"]
    before = deepcopy(session.workspace_snapshot())

    with pytest.raises(DraftSessionError) as failed:
        _mutate(session, "program_draft_update_node_policies", arguments)

    assert failed.value.status_code == 409
    assert failed.value.code == "composer_policy_skill_referent_unavailable"
    assert str(failed.value) == (
        "Allowed recovery skills are unavailable: each skill requires an "
        "exact revision-qualified retained referent. Leave Allowed skills empty."
    )
    assert session.workspace_snapshot() == before


def test_node_policy_instance_id_uses_the_128_byte_portable_bound() -> None:
    accepted_id = "a" + ("b" * 127)
    accepted = ComposerDraftSession(_program_with_instance_id(accepted_id))
    accepted_result = _mutate(
        accepted,
        "program_draft_update_node_policies",
        _node_policy_arguments(accepted, instance_id=accepted_id),
    )
    assert accepted_result.snapshot["draft_revision"] == 2

    rejected = ComposerDraftSession(
        _program_with_instance_id("a" + ("b" * 128))
    )
    rejected_arguments = _node_policy_arguments(
        rejected,
        instance_id="a" + ("b" * 128),
    )
    before = deepcopy(rejected.workspace_snapshot())
    with pytest.raises(DraftSessionError) as failed:
        _mutate(
            rejected,
            "program_draft_update_node_policies",
            rejected_arguments,
        )
    assert failed.value.status_code == 409
    assert failed.value.code == "draft_arguments_invalid"
    assert rejected.workspace_snapshot() == before


def test_node_policy_explicit_defaults_normalize_to_omission_identity() -> None:
    original = _program()
    session = ComposerDraftSession(original)
    arguments = _node_policy_arguments(session)
    arguments["modification_policy"] = {
        "runtime_override": "forbidden",
        "persistent_edit": "forbidden",
        "intervention_delegate": "forbidden",
    }
    arguments["recovery_policy"] = {
        "allow_intervention": False,
        "allowed_recovery_actions": [],
        "max_retries": 0,
        "allowed_skills": [],
        "persistence_ceiling": "session_only",
    }
    arguments["operator_stop_policy"] = "inherit"

    result = _mutate(session, "program_draft_update_node_policies", arguments)
    repeated = _mutate(session, "program_draft_update_node_policies", arguments)

    assert result.snapshot["program_revision"] == original.program_revision
    assert repeated.snapshot["program_revision"] == original.program_revision
    assert repeated.snapshot["draft_revision"] == result.snapshot["draft_revision"] == 1
    node = result.snapshot["program"]["tree"]["children"][0]
    assert "modification_policy" not in node
    assert "recovery_policy" not in node
    assert "operator_stop_policy" not in node


def test_node_policy_noop_is_allowed_at_revision_exhaustion() -> None:
    session = ComposerDraftSession(_program())
    session._revision = 9_007_199_254_740_991
    arguments = _node_policy_arguments(session)
    arguments["modification_policy"] = {
        "runtime_override": "forbidden",
        "persistent_edit": "forbidden",
        "intervention_delegate": "forbidden",
    }
    arguments["recovery_policy"] = {
        "allow_intervention": False,
        "allowed_recovery_actions": [],
        "max_retries": 0,
        "allowed_skills": [],
        "persistence_ceiling": "session_only",
    }
    arguments["operator_stop_policy"] = "inherit"
    before = deepcopy(session.snapshot())

    result = _mutate(session, "program_draft_update_node_policies", arguments)

    assert result.snapshot == before


def test_node_policy_mutation_uses_the_draft_fence_and_instance_identity_atomically() -> None:
    session = ComposerDraftSession(_program())
    arguments = _node_policy_arguments(session)
    before = deepcopy(session.snapshot())

    with pytest.raises(DraftSessionError) as unexpected_fence:
        _mutate(
            session,
            "program_draft_update_node_policies",
            {**arguments, "expected_program_revision": "sha256:" + "f" * 64},
        )
    assert unexpected_fence.value.code == "draft_arguments_invalid"
    assert session.snapshot() == before

    with pytest.raises(DraftSessionError) as unknown:
        _mutate(
            session,
            "program_draft_update_node_policies",
            {**arguments, "instance_id": "missing"},
        )
    assert unknown.value.code == "draft_node_not_found"
    assert session.snapshot() == before

    with pytest.raises(DraftSessionError) as stale_draft:
        session.mutate(
            tool_id="program_draft_update_node_policies",
            arguments=arguments,
            expected_revision=session.revision - 1,
            catalog=build_node_environment().catalog,
            draft_id=session.draft_id,
        )
    assert stale_draft.value.code == "composer_draft_revision_stale"
    assert session.snapshot() == before

    with pytest.raises(DraftSessionError) as stale_id:
        session.mutate(
            tool_id="program_draft_update_node_policies",
            arguments=arguments,
            expected_revision=session.revision,
            catalog=build_node_environment().catalog,
            draft_id="draft-stale",
        )
    assert stale_id.value.code == "composer_draft_id_stale"
    assert session.snapshot() == before

    session._revision = 9_007_199_254_740_991
    exhausted_before = deepcopy(session.snapshot())
    with pytest.raises(DraftSessionError) as exhausted:
        _mutate(
            session,
            "program_draft_update_node_policies",
            _node_policy_arguments(session),
        )
    assert exhausted.value.code == "composer_draft_revision_exhausted"
    assert session.snapshot() == exhausted_before


@pytest.mark.parametrize(
    "change",
    [
        lambda args: args.update({"modification_policy": None}),
        lambda args: args["recovery_policy"].pop("max_retries"),
        lambda args: args["recovery_policy"].update({"unexpected": 1}),
        lambda args: args["modification_policy"].update({"runtime_override": "pause"}),
        lambda args: args.update({"operator_stop_policy": "pause"}),
    ],
)
def test_node_policy_mutation_rejects_closed_schema_values(change) -> None:
    session = ComposerDraftSession(_program())
    arguments = _node_policy_arguments(session)
    change(arguments)
    before = deepcopy(session.workspace_snapshot())

    with pytest.raises(DraftSessionError):
        _mutate(session, "program_draft_update_node_policies", arguments)
    assert session.workspace_snapshot() == before


def test_node_policy_cross_field_failure_preserves_saved_applied_and_revisions() -> None:
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    before = deepcopy(
        session.workspace_snapshot(
            applied_program_revision=session.snapshot()["program_revision"],
        )
    )
    arguments = _node_policy_arguments(session)
    arguments["modification_policy"]["runtime_override"] = "forbidden"
    arguments["recovery_policy"]["allowed_recovery_actions"] = ["runtime_override"]
    arguments["recovery_policy"]["allow_intervention"] = False
    arguments["recovery_policy"]["max_retries"] = 0
    arguments["recovery_policy"]["allowed_skills"] = []
    arguments["recovery_policy"]["persistence_ceiling"] = "session_only"

    with pytest.raises(DraftSessionError) as failed:
        _mutate(session, "program_draft_update_node_policies", arguments)

    assert failed.value.code == "composer_draft_validation_failed"
    assert session.workspace_snapshot(
        applied_program_revision=before["draft"]["programRevision"],
    ) == before


def test_relative_orientation_resolves_to_absolute_and_preserves_metadata() -> None:
    session = ComposerDraftSession(_program(metadata={"operator_note": "retain"}))
    resource = _box_resource("table")
    resource["shape"] = {"type": "box", "box_dimensions_m": [1.0, 0.6, 0.02]}
    resource["pose"] = {
        "frame_ref": {
            "kind": "component_frame",
            "component_role": "robot",
            "model_role": "arm_model",
            "frame_role": "base",
        },
        "position_m": [0.5, 0.0, 0.4],
        "rotate_local_rpy_deg": [0.0, 0.0, 90.0],
    }

    result = _mutate(
        session,
        "scene_draft_update_object",
        {"scene_resource": resource},
    )
    updated = result.snapshot["program"]["scene"][0]

    assert updated["metadata"] == {"operator_note": "retain"}
    assert set(updated["pose"]) == {"frame_ref", "position_m", "orientation"}
    assert updated["pose"]["orientation"]["xyzw"] == pytest.approx(
        [0.0, 0.0, 2**-0.5, 2**-0.5]
    )


@pytest.mark.parametrize(
    ("intent", "expected"),
    [
        ({"rotate_world_rpy_deg": [0.0, 0.0, 90.0]}, [0.0, 0.0, 2**-0.5, 2**-0.5]),
        ({"rpy_delta_deg": [0.0, 0.0, 90.0]}, [0.0, 0.0, 2**-0.5, 2**-0.5]),
        ({"orientation_from": {"source": "current_pose"}}, [0.0, 0.0, 0.0, 1.0]),
        (
            {
                "orientation_from": {
                    "source": "scene_resource",
                    "scene_resource_id": "table",
                }
            },
            [0.0, 0.0, 0.0, 1.0],
        ),
    ],
)
def test_every_source_local_relative_orientation_form_becomes_absolute(
    intent: dict,
    expected: list[float],
) -> None:
    session = ComposerDraftSession(_program())
    resource = _box_resource("table")
    resource["shape"] = {"type": "box", "box_dimensions_m": [1.0, 0.6, 0.02]}
    resource["pose"] = {
        "frame_ref": {
            "kind": "component_frame",
            "component_role": "robot",
            "model_role": "arm_model",
            "frame_role": "base",
        },
        "position_m": [0.5, 0.0, 0.4],
        **intent,
    }

    result = _mutate(
        session,
        "scene_draft_update_object",
        {"scene_resource": resource},
    )

    pose = result.snapshot["program"]["scene"][0]["pose"]
    assert set(pose) == {"frame_ref", "position_m", "orientation"}
    assert pose["orientation"]["xyzw"] == pytest.approx(expected)


def test_new_resource_requires_an_absolute_orientation_even_for_direct_calls() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    resource = _box_resource("workpiece")
    resource["pose"].pop("orientation")
    resource["pose"]["orientation_from"] = {
        "source": "scene_resource",
        "scene_resource_id": "table",
    }

    with pytest.raises(DraftSessionError) as invalid:
        _mutate(
            session,
            "scene_draft_add_object",
            {"scene_resource": resource},
        )

    assert invalid.value.code == "composer_draft_validation_failed"
    assert session.snapshot() == before


def test_binding_dependent_target_frame_intent_is_typed_unavailable_and_atomic() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    resource = _box_resource("table")
    resource["pose"].pop("orientation")
    resource["pose"]["orientation_from"] = {
        "source": "target_frame",
        "component_id": "lite6_arm",
        "target_id": "arm",
        "frame_role_id": "tcp",
    }

    with pytest.raises(DraftSessionError) as unavailable:
        _mutate(
            session,
            "scene_draft_update_object",
            {"scene_resource": resource},
        )

    assert unavailable.value.code == "draft_pose_reference_unavailable"
    assert session.snapshot() == before


def test_tree_edits_use_instance_ids_and_the_explicit_root_alias() -> None:
    session = ComposerDraftSession(_program())
    inserted = _mutate(
        session,
        "tree_draft_insert_node",
        {
            "parent_instance_id": "@root",
            "index": 2,
            "node": {
                "node_id": "timer",
                "instance_id": "settle",
                "parameters": {"duration_s": 0.1},
                "children": [],
            },
        },
    )
    assert inserted.details["parent_instance_id"] == "main_sequence"

    wrapped = _mutate(
        session,
        "tree_draft_wrap_node",
        {
            "target_instance_id": "@root",
            "wrapper": {
                "node_id": "fallback",
                "instance_id": "fallback_root",
                "parameters": {},
            },
        },
    )
    root = wrapped.snapshot["program"]["tree"]
    assert root["instance_id"] == "fallback_root"
    assert [child["instance_id"] for child in root["children"]] == [
        "main_sequence"
    ]


def test_tree_rename_is_atomic_and_moves_position_identity() -> None:
    catalog = build_node_environment().catalog
    session = ComposerDraftSession(_program())
    _mutate(
        session,
        "tree_draft_update_positions",
        {"positions": {"move_to_pick": {"x": 42.0, "y": 84.0}}},
    )

    renamed = _mutate(
        session,
        "tree_draft_rename_node",
        {
            "instance_id": "move_to_pick",
            "new_instance_id": "move_to_pick_safely",
        },
    )

    assert renamed.details == {
        "previous_instance_id": "move_to_pick",
        "renamed_instance_id": "move_to_pick_safely",
    }
    authoring = renamed.snapshot["authoring"]
    assert "move_to_pick" not in authoring["positions"]
    assert authoring["positions"]["move_to_pick_safely"] == {"x": 42.0, "y": 84.0}
    assert renamed.snapshot["program"]["tree"]["children"][0]["instance_id"] == (
        "move_to_pick_safely"
    )

    retained = session.retained_state()
    restored = ComposerDraftSession()
    restored.restore_retained_state(retained, catalog)
    assert restored.snapshot() == session.snapshot()

    before = session.snapshot()
    with pytest.raises(DraftSessionError) as duplicate:
        _mutate(
            session,
            "tree_draft_rename_node",
            {
                "instance_id": "move_to_pick_safely",
                "new_instance_id": "main_sequence",
            },
        )
    assert duplicate.value.code == "draft_node_exists"
    assert session.snapshot() == before

    with pytest.raises(DraftSessionError) as invalid:
        _mutate(
            session,
            "tree_draft_rename_node",
            {
                "instance_id": "move_to_pick_safely",
                "new_instance_id": "Not portable",
            },
        )
    assert invalid.value.code == "draft_arguments_invalid"
    assert session.snapshot() == before


def test_tree_delete_removes_only_the_node_and_detaches_direct_children() -> None:
    root_session = ComposerDraftSession(_program())
    _mutate(
        root_session,
        "tree_draft_update_positions",
        {
            "positions": {
                "main_sequence": {"x": 40.0, "y": 100.0},
                "move_to_pick": {"x": 320.0, "y": 80.0},
                "remove_table": {"x": 320.0, "y": 180.0},
            }
        },
    )
    deleted_root = _mutate(
        root_session,
        "tree_draft_delete_node",
        {"instance_id": "main_sequence"},
    )
    assert deleted_root.details == {
        "deleted_instance_id": "main_sequence",
        "detached_child_instance_ids": ["move_to_pick", "remove_table"],
    }
    assert [
        node["instance_id"]
        for node in deleted_root.snapshot["authoring"]["forest"]
    ] == ["move_to_pick", "remove_table"]
    assert "main_sequence" not in deleted_root.snapshot["authoring"]["positions"]
    assert deleted_root.snapshot["authoring"]["positions"]["move_to_pick"] == {
        "x": 320.0,
        "y": 80.0,
    }

    nested_session = ComposerDraftSession(_program())
    _mutate(
        nested_session,
        "tree_draft_insert_node",
        {
            "parent_instance_id": "main_sequence",
            "node": {
                "node_id": "fallback",
                "instance_id": "delete_parent",
                "parameters": {},
                "children": [
                    {
                        "node_id": "sequence",
                        "instance_id": "detached_sequence",
                        "parameters": {},
                        "children": [
                            {
                                "node_id": "timer",
                                "instance_id": "retained_grandchild",
                                "parameters": {"duration_s": 0.1},
                                "children": [],
                            }
                        ],
                    },
                    {
                        "node_id": "timer",
                        "instance_id": "detached_timer",
                        "parameters": {"duration_s": 0.2},
                        "children": [],
                    },
                ],
            },
        },
    )
    _mutate(
        nested_session,
        "tree_draft_update_positions",
        {
            "positions": {
                "delete_parent": {"x": 300.0, "y": 280.0},
                "detached_sequence": {"x": 520.0, "y": 240.0},
                "retained_grandchild": {"x": 740.0, "y": 240.0},
                "detached_timer": {"x": 520.0, "y": 340.0},
            }
        },
    )
    deleted_nested = _mutate(
        nested_session,
        "tree_draft_delete_node",
        {"instance_id": "delete_parent"},
    )
    forest = deleted_nested.snapshot["authoring"]["forest"]
    assert [node["instance_id"] for node in forest] == [
        "main_sequence",
        "detached_sequence",
        "detached_timer",
    ]
    assert forest[1]["children"][0]["instance_id"] == "retained_grandchild"
    assert "delete_parent" not in deleted_nested.snapshot["authoring"]["positions"]
    for instance_id in (
        "detached_sequence",
        "retained_grandchild",
        "detached_timer",
    ):
        assert instance_id in deleted_nested.snapshot["authoring"]["positions"]


def test_program_role_requirements_replace_through_one_operator_mutation() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    component_roles = deepcopy(before["authoring"]["component_roles"])
    component_roles["inspection_tool"] = {
        "required_component_type_tags": ["camera"]
    }

    replaced = _mutate(
        session,
        "program_draft_replace_roles",
        {
            "component_roles": component_roles,
            "target_roles": before["authoring"]["target_roles"],
        },
    )

    assert replaced.mutation in OPERATOR_DRAFT_MUTATIONS
    assert replaced.details["component_role_count"] == len(component_roles)
    assert replaced.snapshot["authoring"]["component_roles"] == {
        key: component_roles[key] for key in sorted(component_roles)
    }
    assert replaced.snapshot["program"] is not None

    retained = session.snapshot()
    with pytest.raises(DraftSessionError) as invalid:
        _mutate(
            session,
            "program_draft_replace_roles",
            {"component_roles": [], "target_roles": {}},
        )
    assert invalid.value.code == "draft_arguments_invalid"
    assert session.snapshot() == retained


def test_authoring_forest_retains_incomplete_topology_until_it_derives_a_program() -> None:
    catalog = build_node_environment().catalog
    session = ComposerDraftSession(_program(), source_path=str(_PROGRAM_PATH))
    before_revision = session.snapshot()["program_revision"]

    detached = _mutate(
        session,
        "tree_draft_insert_node",
        {
            "node": {
                "node_id": "timer",
                "instance_id": "detached_timer",
                "parameters": {"duration_s": 0.2},
                "children": [],
            },
            "position": {"x": 120.0, "y": 240.0},
        },
    )

    assert detached.snapshot["candidate_available"] is False
    assert detached.snapshot["program"] is None
    assert [node["instance_id"] for node in detached.snapshot["authoring"]["forest"]] == [
        "main_sequence",
        "detached_timer",
    ]
    assert detached.snapshot["authoring"]["positions"]["detached_timer"] == {
        "x": 120.0,
        "y": 240.0,
    }
    assert detached.snapshot["diagnostics"][0]["code"] == "authoring_forest_root_count"
    retained = session.retained_state()
    restored = ComposerDraftSession()
    restored.restore_retained_state(retained, catalog)
    assert restored.snapshot() == session.snapshot()

    with pytest.raises(DraftSessionError) as incomplete_save:
        session.save_program(
            draft_id=session.draft_id,
            expected_revision=session.revision,
            target_path=None,
            writer=lambda _program, path: path,
        )
    assert incomplete_save.value.code == "composer_authoring_draft_incomplete"

    attached = _mutate(
        session,
        "tree_draft_move_node",
        {
            "instance_id": "detached_timer",
            "new_parent_instance_id": "main_sequence",
            "index": 2,
        },
    )
    assert attached.snapshot["candidate_available"] is True
    assert attached.snapshot["program"] is not None
    assert attached.snapshot["program_revision"] != before_revision
    assert attached.snapshot["diagnostics"] == []


def test_authoring_forest_rejects_invalid_values_but_allows_missing_required_fields() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    with pytest.raises(DraftSessionError) as invalid:
        _mutate(
            session,
            "tree_draft_insert_node",
            {
                "node": {
                    "node_id": "timer",
                    "instance_id": "invalid_timer",
                    "parameters": {"duration_s": "slow"},
                    "children": [],
                }
            },
        )
    assert invalid.value.code == "composer_authoring_field_invalid"
    assert session.snapshot() == before

    incomplete = _mutate(
        session,
        "tree_draft_insert_node",
        {
            "node": {
                "node_id": "motion.move_to_joint_target",
                "instance_id": "missing_fields",
                "parameters": {},
                "children": [],
            }
        },
    )
    assert incomplete.snapshot["candidate_available"] is False
    assert any(
        diagnostic["code"] == "authoring_parameter_required"
        for diagnostic in incomplete.snapshot["diagnostics"]
    )


def test_authoring_position_edit_advances_only_the_draft_identity() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    changed = _mutate(
        session,
        "tree_draft_update_positions",
        {"positions": {"main_sequence": {"x": 50.0, "y": 75.0}}},
    )
    assert changed.snapshot["draft_revision"] == before["draft_revision"] + 1
    assert changed.snapshot["program_revision"] == before["program_revision"]
    assert changed.snapshot["authoring"]["positions"]["main_sequence"] == {
        "x": 50.0,
        "y": 75.0,
    }


def test_authoring_detach_atomically_retains_the_presented_layout() -> None:
    session = ComposerDraftSession(_program())
    changed = _mutate(
        session,
        "tree_draft_move_node",
        {
            "instance_id": "move_to_pick",
            "new_parent_instance_id": None,
            "index": 1,
            "positions": {
                "main_sequence": {"x": 40.0, "y": 90.0},
                "move_to_pick": {"x": 320.0, "y": 180.0},
            },
        },
    )

    assert changed.snapshot["authoring"]["positions"]["main_sequence"] == {
        "x": 40.0,
        "y": 90.0,
    }
    assert changed.snapshot["authoring"]["positions"]["move_to_pick"] == {
        "x": 320.0,
        "y": 180.0,
    }
    assert [
        node["instance_id"] for node in changed.snapshot["authoring"]["forest"]
    ] == ["main_sequence", "move_to_pick"]


def test_authoring_same_parent_reorder_is_semantic_and_retains_the_presented_layout() -> None:
    session = ComposerDraftSession(_program())
    changed = _mutate(
        session,
        "tree_draft_move_node",
        {
            "instance_id": "remove_table",
            "new_parent_instance_id": "main_sequence",
            "index": 0,
            "positions": {
                "move_to_pick": {"x": 320.0, "y": 180.0},
                "remove_table": {"x": 320.0, "y": 90.0},
            },
        },
    )

    root = changed.snapshot["authoring"]["forest"][0]
    assert [child["instance_id"] for child in root["children"]] == [
        "remove_table",
        "move_to_pick",
    ]
    assert changed.snapshot["authoring"]["positions"]["remove_table"] == {
        "x": 320.0,
        "y": 90.0,
    }


def test_tree_pose_patch_preserves_frame_and_position_and_resolves_orientation() -> None:
    session = ComposerDraftSession(_program())
    stored_pose = {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [0.3, -0.3, 0.15],
        "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
    }
    _mutate(
        session,
        "tree_draft_insert_node",
        {
            "parent_instance_id": "@root",
            "node": {
                "node_id": "scene.update_object_pose",
                "instance_id": "assistant_pose_update",
                "parameters": {
                    "scene_resource_id": "table",
                    "pose": stored_pose,
                },
                "children": [],
            },
        },
    )

    result = _mutate(
        session,
        "tree_draft_update_node_params",
        {
            "instance_id": "assistant_pose_update",
            "expected_node_id": "scene.update_object_pose",
            "parameters": {
                "pose": {"rotate_local_rpy_deg": [0.0, 0.0, 90.0]},
            },
        },
    )
    node = next(
        child
        for child in result.snapshot["program"]["tree"]["children"]
        if child["instance_id"] == "assistant_pose_update"
    )
    pose = node["parameters"]["pose"]
    assert pose["frame_ref"] == stored_pose["frame_ref"]
    assert pose["position_m"] == stored_pose["position_m"]
    assert pose["orientation"]["xyzw"] == pytest.approx(
        [0.0, 0.0, 2**-0.5, 2**-0.5]
    )

    before = session.snapshot()
    with pytest.raises(DraftSessionError) as unresolved:
        _mutate(
            session,
            "tree_draft_update_node_params",
            {
                "instance_id": "assistant_pose_update",
                "expected_node_id": "scene.update_object_pose",
                "parameters": {
                    "pose": {
                        "orientation_from": {
                            "source": "scene_resource",
                            "scene_resource_id": "missing",
                        }
                    }
                },
            },
        )
    assert unresolved.value.code == "draft_scene_resource_not_found"
    assert session.snapshot() == before


def test_duplicate_declaration_identity_is_rejected_without_last_write_wins() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    declaration = {
        "parameter_id": "speed",
        "type": "float64",
        "default": 0.5,
        "constraints": {},
        "runtime_mutability": "before_run",
    }

    with pytest.raises(DraftSessionError) as duplicate:
        _mutate(
            session,
            "program_draft_upsert_parameters",
            {"parameters": [declaration, {**declaration, "default": 0.6}]},
        )

    assert duplicate.value.code == "draft_declaration_duplicate"
    assert session.snapshot() == before


def test_every_generated_draft_mutation_has_a_valid_atomic_path() -> None:
    session = ComposerDraftSession(_program())
    invoked: set[str] = set()

    def invoke(tool_id: str, arguments: dict):
        invoked.add(tool_id)
        return _mutate(session, tool_id, arguments)

    invoke(
        "blackboard_draft_upsert_namespaces",
        {"namespaces": [{"namespace_id": "scratch", "owner": "program"}]},
    )
    invoke(
        "blackboard_draft_upsert_keys",
        {
            "keys": [
                {
                    "key": "scratch.flag",
                    "type": "bool",
                    "access": "read_write",
                    "persistence": "run",
                }
            ]
        },
    )
    invoke(
        "program_draft_upsert_parameters",
        {
            "parameters": [
                {
                    "parameter_id": "scratch_limit",
                    "type": "int64",
                    "default": 2,
                    "constraints": {"min_inclusive": 1},
                    "runtime_mutability": "before_run",
                }
            ]
        },
    )

    first = _box_resource("assistant_first")
    second = _box_resource("assistant_second")
    invoke("scene_draft_add_object", {"scene_resource": first})
    updated = _box_resource("assistant_first")
    updated["pose"]["position_m"] = [0.1, 0.0, 0.0]
    invoke("scene_draft_update_object", {"scene_resource": updated})
    second["pose"]["position_m"] = [0.2, 0.0, 0.0]
    invoke(
        "scene_draft_upsert_objects",
        {"scene_resources": [updated, second]},
    )
    invoke(
        "scene_draft_remove_objects",
        {"scene_resource_ids": ["assistant_first", "assistant_second"]},
    )

    invoke(
        "tree_draft_insert_node",
        {
            "parent_instance_id": "@root",
            "node": {
                "node_id": "timer",
                "instance_id": "assistant_timer",
                "parameters": {"duration_s": 0.1},
                "children": [],
            },
        },
    )
    invoke(
        "tree_draft_update_node_params",
        {
            "instance_id": "assistant_timer",
            "expected_node_id": "timer",
            "parameters": {"duration_s": 0.2},
        },
    )
    invoke(
        "tree_draft_move_node",
        {
            "instance_id": "assistant_timer",
            "new_parent_instance_id": "@root",
            "index": 0,
        },
    )
    invoke(
        "tree_draft_rename_node",
        {
            "instance_id": "assistant_timer",
            "new_instance_id": "assistant_timer_renamed",
        },
    )
    invoke(
        "tree_draft_replace_subtree",
        {
            "instance_id": "assistant_timer_renamed",
            "subtree": {
                "node_id": "timer",
                "instance_id": "assistant_timer_replaced",
                "parameters": {"duration_s": 0.3},
                "children": [],
            },
        },
    )
    invoke(
        "tree_draft_change_node_kind",
        {
            "instance_id": "assistant_timer_replaced",
            "replacement": {
                "node_id": "logic.set_bool",
                "parameters": {"key": "scratch.flag", "value": True},
            },
            "parameters_mode": "replace",
        },
    )
    invoke(
        "tree_draft_delete_node",
        {"instance_id": "assistant_timer_replaced"},
    )
    invoke(
        "tree_draft_wrap_node",
        {
            "target_instance_id": "@root",
            "wrapper": {
                "node_id": "fallback",
                "instance_id": "assistant_fallback",
                "parameters": {},
            },
        },
    )

    invoke(
        "program_draft_remove_parameters",
        {"parameter_ids": ["scratch_limit"]},
    )
    invoke("blackboard_draft_remove_keys", {"keys": ["scratch.flag"]})
    invoke(
        "blackboard_draft_remove_namespaces",
        {"namespace_ids": ["scratch"]},
    )
    assert invoked == set(SUPPORTED_DRAFT_MUTATIONS)

    operator_result = _mutate(
        session,
        "program_draft_update_node_policies",
        _node_policy_arguments(session),
    )

    assert operator_result.mutation in OPERATOR_DRAFT_MUTATIONS
    assert operator_result.mutation not in SUPPORTED_DRAFT_MUTATIONS
    assert session.snapshot()["saved"] is False
    assert session.snapshot()["applied"] is False
