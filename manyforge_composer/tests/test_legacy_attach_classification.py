"""T-N2 must use the holder's actual capability facts, not its tags."""

from copy import deepcopy
import json

import pytest

from manyforge_composer.backend import component_catalog, legacy_attach_migration


@pytest.mark.parametrize("capabilities", [
    [{"capability_id": "execution.trajectory.execute",
      "capability_definition_revision": "sha256:" + "1" * 64}],
    None,
], ids=["motion_target_tagged_tool", "capability_facts_missing"])
def test_motion_or_unknown_holder_stays_unconverted(tmp_path, monkeypatch, capabilities):
    source = tmp_path / "sources.json"
    source.write_text(json.dumps({
        "runtime_restoration_profiles": [{
            "runtime_restoration_profile_id": "profile", "model_family_id": "family",
        }],
        "model_families": [],
    }), encoding="utf-8")
    target = {
        "command_target_id": "holder", "frame_roles": [
            {"frame_role_id": "tip", "tcp": True},
        ], "default_tcp_frame_role_id": "tip",
    }
    if capabilities is not None:
        target["capabilities"] = capabilities
    (tmp_path / "family.component.json").write_text(json.dumps({
        "tags": ["tool"], "attachment_interfaces": [], "command_targets": [target],
    }), encoding="utf-8")
    monkeypatch.setattr(component_catalog, "_SOURCES", source)
    monkeypatch.setattr(component_catalog, "_COMPONENT_TYPES", tmp_path)

    authoring = {
        "target_roles": {"holder": {"component_role": "component", "required_frame_roles": ["tcp"]}},
        "forest": [{"node_id": "scene.attach_carried_object", "instance_id": "attach",
                    "parameters": {"target_role": "holder", "carrier_frame_role": "tcp"}, "children": []}],
    }
    deployment = {"components": [{"component_id": "device", "component_profile_id": "profile"}]}
    assignment = {"component_roles": [{"component_role_id": "component", "controllable_component_id": "device"}],
                  "target_roles": [{"target_role_id": "holder", "command_target_id": "holder"}]}
    before = deepcopy(authoring)
    assert not legacy_attach_migration.migrate_legacy_attach(authoring, deployment, assignment)
    assert authoring == before


@pytest.mark.parametrize("other_capabilities,converted", [
    ([{"capability_id": "execution.trajectory.execute"}], False),
    ([], True),
    (None, False),
], ids=["motion_on_another_target", "untagged_non_robot", "another_target_unknown"])
def test_component_scope_and_untagged_holder(tmp_path, monkeypatch, other_capabilities, converted):
    source = tmp_path / "sources.json"
    source.write_text(json.dumps({
        "runtime_restoration_profiles": [{"runtime_restoration_profile_id": "profile", "model_family_id": "family"}],
        "model_families": [],
    }), encoding="utf-8")
    other = {"command_target_id": "other", "frame_roles": []}
    if other_capabilities is not None:
        other["capabilities"] = other_capabilities
    (tmp_path / "family.component.json").write_text(json.dumps({
        "tags": [], "attachment_interfaces": [], "command_targets": [
            {"command_target_id": "holder", "frame_roles": [{"frame_role_id": "tip", "tcp": True}],
             "default_tcp_frame_role_id": "tip", "capabilities": []}, other,
        ],
    }), encoding="utf-8")
    monkeypatch.setattr(component_catalog, "_SOURCES", source)
    monkeypatch.setattr(component_catalog, "_COMPONENT_TYPES", tmp_path)
    authoring = {
        "target_roles": {"holder": {"component_role": "component", "required_frame_roles": ["tcp"]}},
        "forest": [{"node_id": "scene.attach_carried_object", "instance_id": "attach",
                    "parameters": {"target_role": "holder", "carrier_frame_role": "tcp"}, "children": []}],
    }
    deployment = {"components": [{"component_id": "device", "component_profile_id": "profile"}]}
    assignment = {"component_roles": [{"component_role_id": "component", "controllable_component_id": "device"}],
                  "target_roles": [{"target_role_id": "holder", "command_target_id": "holder"}]}
    before = deepcopy(authoring)
    assert legacy_attach_migration.migrate_legacy_attach(authoring, deployment, assignment) is converted
    if converted:
        assert authoring["forest"][0]["parameters"]["carrier_frame"] == {"kind": "active_tcp"}
        assert authoring["target_roles"]["holder"]["required_frame_roles"] == []
    else:
        assert authoring == before
