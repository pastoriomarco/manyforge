"""Typed refusal of ambiguous legacy TCP aliases at authoring Load."""

from pathlib import Path

import pytest

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import TcpEndpointAliasAmbiguous
from manyforge_composer.backend.authoring_forest import adopt_active_catalog, empty_authoring


def _move(instance, owner):
    return {
        "node_id": "motion.move_to_pose", "instance_id": instance,
        "parameters": {
            "target_role": "arm",
            "tcp_endpoint": {"target_role": owner, "frame_role": "tcp"},
        },
        "children": [],
    }


def test_mixed_legacy_alias_is_refused_before_draft_install():
    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    authoring = empty_authoring(catalog)
    authoring["catalog"]["content_version"] = "2.8.0"
    authoring["forest"] = [{
        "node_id": "sequence", "instance_id": "root", "parameters": {},
        "children": [_move("first", "arm"), _move("second", "gripper")],
    }]
    with pytest.raises(TcpEndpointAliasAmbiguous) as caught:
        adopt_active_catalog(authoring, catalog)
    assert caught.value.code == "tcp_endpoint_alias_ambiguous"
    assert caught.value.affected == (("first", "arm"),)
    assert authoring["catalog"]["content_version"] == "2.8.0"
    assert authoring["forest"][0]["children"][1]["parameters"]["tcp_endpoint"] == {
        "target_role": "gripper", "frame_role": "tcp"
    }


def _ambiguous_tree():
    return {
        "node_id": "sequence", "instance_id": "root", "parameters": {},
        "children": [_move("first", "arm"), _move("second", "arm"),
                     _move("tool_move", "gripper")],
    }


def _assert_ambiguity(response):
    assert response.status_code == 400, response.text
    detail = response.json()["detail"]
    assert detail["code"] == "tcp_endpoint_alias_ambiguous"
    assert "move first on role arm: choose its TCP" in detail["message"]
    assert "move second on role arm: choose its TCP" in detail["message"]


def test_program_open_refuses_ambiguous_tcp_without_changing_draft_or_source(tmp_path):
    import json
    import yaml

    from manyforge_behavior.canonical_artifact import compute_content_identity
    from .test_program_workspace import _SOURCE_PROGRAM, _client, _fence

    document = yaml.safe_load(_SOURCE_PROGRAM.read_text(encoding="utf-8"))
    document["catalog"]["content_version"] = "2.8.0"
    document["tree"] = _ambiguous_tree()
    document["program_revision"] = compute_content_identity(
        document, own_field="program_revision", identity_domain="program-revision",
        api_version=document["api_version"],
    )
    source = tmp_path / "ambiguous.program.yaml"
    source.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    source_before = source.read_bytes()
    client = _client(tmp_path)
    before = client.get("/api/program/workspace").json()
    response = client.post("/api/program/open-draft", json={
        **_fence(before), "path": str(source), "confirmDiscard": False,
    })
    _assert_ambiguity(response)
    assert client.get("/api/program/workspace").json() == before
    assert source.read_bytes() == source_before


def test_scenario_load_refuses_ambiguous_tcp_without_changing_draft_or_source(tmp_path):
    # The saved scenario's model source (the autouse fixture lives in test_scenario_artifacts).
    (tmp_path / "model.urdf").write_text('<robot name="fixture"/>\n', encoding="utf-8")
    from manyforge_composer.backend.scenario_artifacts import save_scenario_artifact
    from manyforge_composer.backend.state import ComposerState
    from .test_scenario_artifacts import (
        _document, _fence, _PassThroughAdmission, _scenario_client,
    )

    document = _document("ambiguous")
    authoring = document["program_draft"]
    authoring["catalog"]["content_version"] = "2.8.0"
    authoring["forest"] = [_ambiguous_tree()]
    source = tmp_path / "ambiguous.scenario"
    save_scenario_artifact(document, str(source), allowed_roots=[str(tmp_path)], overwrite=False)
    source_before = {path.relative_to(source): path.read_bytes()
                     for path in source.rglob("*") if path.is_file()}
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()
    client = _scenario_client(state)
    before = state.authoring_workspace.snapshot().as_dict()
    response = client.post("/api/scenario-documents/load", json={
        **_fence(state), "path": str(source), "confirmDiscard": False,
    })
    _assert_ambiguity(response)
    assert state.authoring_workspace.snapshot().as_dict() == before
    assert {path.relative_to(source): path.read_bytes()
            for path in source.rglob("*") if path.is_file()} == source_before


def test_catalog_adoption_cleans_partially_migrated_legacy_only():
    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    legacy = empty_authoring(catalog)
    legacy["catalog"]["content_version"] = "2.8.0"
    legacy["target_roles"] = {"arm": {"required_frame_roles": ["base", "tcp"]}}
    adopt_active_catalog(legacy, catalog)
    assert legacy["target_roles"]["arm"]["required_frame_roles"] == ["base"]
    adopt_active_catalog(legacy, catalog)
    assert legacy["target_roles"]["arm"]["required_frame_roles"] == ["base"]

    current = empty_authoring(catalog)
    current["target_roles"] = {"arm": {"required_frame_roles": ["base", "tcp"]}}
    adopt_active_catalog(current, catalog)
    assert current["target_roles"]["arm"]["required_frame_roles"] == ["base", "tcp"]


@pytest.mark.parametrize("node_id", ["planning.plan_pick_sequence", "planning.plan_place_sequence"])
def test_legacy_planning_carrier_requirement_belongs_to_tool(node_id):
    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    authoring = empty_authoring(catalog)
    authoring["catalog"]["content_version"] = "2.8.0"
    authoring["target_roles"] = {
        "arm": {"required_frame_roles": ["base", "tcp"]},
        "gripper": {"required_frame_roles": ["tcp", "tip"]},
    }
    authoring["forest"] = [{
        "node_id": node_id, "instance_id": "plan", "children": [],
        "parameters": {"target_role": "arm", "tool_target_role": "gripper",
                       "carrier_frame_role": "tcp",
                       "tcp_endpoint": {"target_role": "arm", "frame_role": "tcp"}},
    }]
    adopt_active_catalog(authoring, catalog)
    assert authoring["forest"][0]["parameters"]["tcp_endpoint"] == {"kind": "current"}
    assert authoring["target_roles"]["arm"]["required_frame_roles"] == ["base"]
    assert authoring["target_roles"]["gripper"]["required_frame_roles"] == ["tcp", "tip"]


def test_legacy_planning_carrier_unknown_tool_scope_keeps_all_requirements():
    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    authoring = empty_authoring(catalog)
    authoring["catalog"]["content_version"] = "2.8.0"
    authoring["target_roles"] = {"arm": {"required_frame_roles": ["tcp"]},
                                  "gripper": {"required_frame_roles": ["tcp"]}}
    authoring["forest"] = [{
        "node_id": "planning.plan_pick_sequence", "instance_id": "plan", "children": [],
        "parameters": {"target_role": "arm", "carrier_frame_role": "tcp"},
        "value_sources": {"tool_target_role": {"kind": "blackboard_key", "reference": "tool"}},
    }]
    adopt_active_catalog(authoring, catalog)
    assert authoring["target_roles"]["arm"]["required_frame_roles"] == ["tcp"]
    assert authoring["target_roles"]["gripper"]["required_frame_roles"] == ["tcp"]


def test_legacy_attach_carrier_scope_remains_target_role():
    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    authoring = empty_authoring(catalog)
    authoring["catalog"]["content_version"] = "2.8.0"
    authoring["target_roles"] = {"arm": {"required_frame_roles": ["tcp"]},
                                  "gripper": {"required_frame_roles": ["tcp"]}}
    authoring["forest"] = [{
        "node_id": "scene.attach_carried_object", "instance_id": "attach", "children": [],
        "parameters": {"target_role": "gripper", "carrier_frame_role": "tcp"},
    }]
    adopt_active_catalog(authoring, catalog)
    assert authoring["target_roles"]["arm"]["required_frame_roles"] == []
    assert authoring["target_roles"]["gripper"]["required_frame_roles"] == ["tcp"]
