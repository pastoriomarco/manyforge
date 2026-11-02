"""Type-aware legacy holder frame conversion."""

from copy import deepcopy

import pytest

from manyforge_composer.backend import legacy_attach_migration as migration


@pytest.fixture
def facts(monkeypatch):
    family = {"tags": ["gripper", "tool"], "hasMotionTarget": False,
              "frameTargets": [{"commandTargetId": "gripper", "frameRoleIds": ["tip"]}],
              "tcpTargets": [{"commandTargetId": "gripper", "tcpFrameRoleIds": ["tip"],
                              "defaultTcpFrameRoleId": "tip"}]}
    monkeypatch.setattr(migration, "_profiles", lambda: (["tool"], {"tool": "family"}, {"tool": family}))
    authoring = {
        "target_roles": {"hand": {"component_role": "tool", "required_frame_roles": ["tcp", "tip"]}},
        "forest": [{"node_id": "scene.attach_carried_object", "instance_id": "attach",
                    "parameters": {"target_role": "hand", "carrier_frame_role": "tcp"}, "children": []}],
    }
    deployment = {"components": [{"component_id": "device", "component_profile_id": "tool"}]}
    assignment = {"component_roles": [{"component_role_id": "tool", "controllable_component_id": "device"}],
                  "target_roles": [{"target_role_id": "hand", "command_target_id": "gripper"}]}
    return family, authoring, deployment, assignment


def test_removed_duplicate_converts_and_drops_only_its_holder_requirement(facts):
    _family, authoring, deployment, assignment = facts
    assert migration.migrate_legacy_attach(authoring, deployment, assignment)
    assert authoring["forest"][0]["parameters"] == {"target_role": "hand", "carrier_frame": {"kind": "active_tcp"}}
    assert authoring["target_roles"]["hand"]["required_frame_roles"] == ["tip"]


@pytest.mark.parametrize("condition", ["declared", "custom", "robot", "unknown", "unbound"])
def test_real_arm_or_unavailable_facts_leave_source_unchanged(facts, condition):
    family, authoring, deployment, assignment = facts
    if condition == "declared":
        family["frameTargets"][0]["frameRoleIds"].append("tcp")  # may be untagged
    elif condition == "custom":
        deployment["components"][0]["custom_tcps"] = [{"tcp_id": "tcp"}]
    elif condition == "robot":
        family["hasMotionTarget"] = True
    elif condition == "unknown":
        deployment["components"][0]["component_profile_id"] = "unknown"
    else:
        assignment["target_roles"] = []
    before = deepcopy(authoring)
    assert not migration.migrate_legacy_attach(authoring, deployment, assignment)
    assert authoring == before


def _legacy_example():
    import json
    from pathlib import Path

    root = Path(__file__).resolve().parents[2]
    source = root / "examples/mock_lite6_pick_and_place.scenario"
    authoring = json.loads((source / "program.yaml").read_text(encoding="utf-8"))
    authoring["catalog"]["content_version"] = "2.8.0"
    authoring["target_roles"]["gripper"]["required_frame_roles"] = ["tcp"]
    pending = list(authoring["forest"])
    while pending:
        node = pending.pop()
        pending.extend(node.get("children", []))
        parameters = node.get("parameters", {})
        if node.get("node_id") == "scene.attach_carried_object":
            parameters.pop("carrier_frame", None)
            parameters["carrier_frame_role"] = "tcp"
        elif node.get("node_id", "").startswith("motion.") and "tcp_endpoint" in parameters:
            parameters["tcp_endpoint"] = {"target_role": parameters["target_role"], "frame_role": "tcp"}
    return source, authoring


def _fixture_profile(monkeypatch, deployment):
    component = next(row for row in deployment["components"] if row["component_id"] == "lite_gripper")
    profile = component["component_profile_id"]
    facts = {"tags": ["tool", "gripper"], "hasMotionTarget": False,
             "frameTargets": [{"commandTargetId": "gripper", "frameRoleIds": ["tip"]}],
             "tcpTargets": [{"commandTargetId": "gripper", "tcpFrameRoleIds": ["tip"],
                             "defaultTcpFrameRoleId": "tip"}]}
    monkeypatch.setattr(migration, "_profiles", lambda: ([profile], {profile: "tool"}, {profile: facts}))


def _assert_converted(authoring):
    # A relative move still references gripper/tcp, so its requirement stays.
    assert authoring["target_roles"]["gripper"]["required_frame_roles"] == ["tcp"]
    pending = list(authoring["forest"])
    while pending:
        node = pending.pop()
        pending.extend(node.get("children", []))
        if node.get("node_id") == "scene.attach_carried_object":
            assert node["parameters"]["carrier_frame"] == {"kind": "active_tcp"}
            assert "carrier_frame_role" not in node["parameters"]
            return
    raise AssertionError("fixture has no attach node")


@pytest.mark.parametrize("have_facts", [True, False])
def test_program_open_uses_applied_facts_before_catalog_normalization(tmp_path, monkeypatch, have_facts):
    import json
    from manyforge_behavior.canonical_artifact import compute_content_identity
    from . import test_program_workspace as workspace_test

    scenario, authoring = _legacy_example()
    deployment = json.loads((scenario / "deployment.yaml").read_text(encoding="utf-8"))
    assignment = json.loads((scenario / "assignment.yaml").read_text(encoding="utf-8"))
    _fixture_profile(monkeypatch, deployment)
    document = deepcopy(authoring)
    document["tree"] = document.pop("forest")[0]
    document.pop("schema_version", None)
    document.pop("positions", None)
    document["program_revision"] = "sha256:" + "0" * 64
    document["program_revision"] = compute_content_identity(
        document, own_field="program_revision", identity_domain="program-revision",
        api_version=document["api_version"],
    )
    path = tmp_path / "legacy.program.yaml"
    path.write_text(json.dumps(document), encoding="utf-8")
    original = path.read_bytes()
    client = workspace_test._client(tmp_path)
    state = workspace_test._FENCED_STATE
    assert state is not None
    monkeypatch.setattr(state, "published_scenario", lambda: {
        "published_scenario": {"fixture": True} if have_facts else None,
    })
    monkeypatch.setattr(state, "applied_activation_for_publication", lambda _publication: {
        "deployment": deployment, "program_role_assignment": assignment,
    })
    before = client.get("/api/program/workspace").json()
    response = client.post("/api/program/open-draft", json={
        **workspace_test._fence(before), "path": str(path), "confirmDiscard": False,
    })
    assert response.status_code == 200, response.text
    opened = state.draft_session.snapshot()["authoring"]
    if have_facts:
        _assert_converted(opened)
    else:
        assert "tcp" in opened["target_roles"]["gripper"]["required_frame_roles"]
        pending = list(opened["forest"])
        while pending:
            node = pending.pop()
            pending.extend(node.get("children", []))
            if node.get("node_id") == "scene.attach_carried_object":
                assert node["parameters"]["carrier_frame_role"] == "tcp"
                break
        else:
            raise AssertionError("fixture has no attach node")
    assert path.read_bytes() == original


def test_scenario_load_converts_candidate_before_derive_without_applied_facts(tmp_path, monkeypatch):
    from manyforge_composer.backend import routes_scenario_documents
    from manyforge_composer.backend.scenario_artifacts import load_scenario_artifact, save_scenario_artifact
    from manyforge_composer.backend.state import ComposerState
    from .test_scenario_artifacts import _PassThroughAdmission, _fence, _scenario_client

    source, authoring = _legacy_example()
    roots = [str(tmp_path), str(source.parent)]
    document, _ = load_scenario_artifact(str(source), allowed_roots=roots)
    document["program_draft"] = authoring
    _fixture_profile(monkeypatch, document["deployment_draft"])
    target = tmp_path / f"{document['scenario_id']}.scenario"
    save_scenario_artifact(document, str(target), allowed_roots=roots, overwrite=False,
                           source_project_path=str(source))
    state = ComposerState(allowed_fs_roots=roots)
    state.authoring_admission = _PassThroughAdmission()
    client = _scenario_client(state)
    seen = []
    derive = routes_scenario_documents.derive_candidate
    def record_derive(value, catalog):
        seen.append(deepcopy(value))
        return derive(value, catalog)
    monkeypatch.setattr(routes_scenario_documents, "derive_candidate", record_derive)
    response = client.post("/api/scenario/load", json={"path": str(target)})
    # The real candidate derivation runs; this fixture has no Manager for Apply.
    assert response.status_code == 503, response.text
    assert seen
    _assert_converted(seen[0])


def test_attach_conversion_keeps_requirement_for_remaining_reference(facts):
    _family, authoring, deployment, assignment = facts
    authoring["blackboard"] = {"allowed_frames": [
        {"kind": "target_frame", "target_role": "hand", "frame_role": "tcp"}
    ]}
    assert migration.migrate_legacy_attach(authoring, deployment, assignment)
    assert authoring["target_roles"]["hand"]["required_frame_roles"] == ["tcp", "tip"]


def test_no_attach_conversion_does_not_change_requirements_or_report_change(facts):
    family, authoring, deployment, assignment = facts
    family["frameTargets"][0]["frameRoleIds"].append("tcp")
    authoring["target_roles"]["other"] = {"required_frame_roles": ["tcp"]}
    before = deepcopy(authoring)
    assert not migration.migrate_legacy_attach(authoring, deployment, assignment)
    assert authoring == before


def test_attach_conversion_keeps_requirement_for_unrestricted_pose(facts):
    _family, authoring, deployment, assignment = facts
    authoring["blackboard"] = {"keys": [{"key": "pose", "type": "stored_pose"}]}
    assert migration.migrate_legacy_attach(authoring, deployment, assignment)
    assert authoring["target_roles"]["hand"]["required_frame_roles"] == ["tcp", "tip"]
