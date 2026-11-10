"""OD-28 contact edits accept only exact v3 body references."""

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.draft_session import ComposerDraftSession, DraftSessionError


ROOT = Path(__file__).resolve().parents[2]


def test_contact_policy_upsert_accepts_direct_body_and_refuses_legacy_without_mutation():
    catalog = build_node_environment().catalog
    program = Program(ROOT / "examples/isaac_lite6_lite_gripper_pick_and_place.program.yaml", catalog)
    session = ComposerDraftSession(program)
    source = load_strict_json_yaml(ROOT / "examples/sim_lite6_foundationpose_trocar_pick.scenario/program.yaml")
    policy = deepcopy(source["contact_policies"][0])
    result = session.mutate(tool_id="program_draft_upsert_contact_policies",
                            arguments={"contact_policies": [policy]},
                            expected_revision=session.revision, catalog=catalog)
    assert result.details["inserted"] == ["selected_object_grasp"]
    assert result.snapshot["authoring"]["contact_policies"][0]["zones"][0]["body_pairs"][0]["body_a"] == {
        "kind": "component_model_body", "body": {"component_id": "lite_gripper", "collision_body_id": "gripper_collision"}}
    before, revision = session.snapshot(), session.revision
    policy["zones"][0]["body_pairs"][0]["body_a"] = {
        "kind": "component_model_body", "component_role_id": "tool", "collision_body_role_id": "gripper_collision"}
    with pytest.raises(DraftSessionError) as refused:
        session.mutate(tool_id="program_draft_upsert_contact_policies",
                       arguments={"contact_policies": [policy]}, expected_revision=revision, catalog=catalog)
    assert refused.value.code == "draft_arguments_invalid"
    assert session.revision == revision
    assert session.snapshot() == before


def test_contact_policy_body_uses_catalog_identifier_rules():
    source = load_strict_json_yaml(ROOT / "examples/sim_lite6_foundationpose_trocar_pick.scenario/program.yaml")
    policy = deepcopy(source["contact_policies"][0])
    body = policy["zones"][0]["body_pairs"][0]["body_a"]["body"]
    body["collision_body_id"] = "Finger_Left"
    canonical = ComposerDraftSession._canonical_contact_policy(policy)
    assert canonical["zones"][0]["body_pairs"][0]["body_a"]["body"] == body
