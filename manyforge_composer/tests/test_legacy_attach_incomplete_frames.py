"""An absent type frame list is not evidence of an empty composed target."""

from copy import deepcopy
import json

from manyforge_composer.backend import component_catalog, legacy_attach_migration


def test_missing_type_frame_facts_leave_legacy_attach_unchanged(tmp_path, monkeypatch):
    source = tmp_path / "sources.json"
    source.write_text(json.dumps({"model_families": [{
        "model_family_id": "family", "model_asset_id": "family",
        "catalog_source": "family.model_asset.json",
        "component_descriptor": "family.component.yaml",
    }]}), encoding="utf-8")
    (tmp_path / "family.component.yaml").write_text(
        json.dumps({"id": "profile", "lanes": {}}), encoding="utf-8")
    (tmp_path / "family.model_asset.json").write_text(
        json.dumps({"model_asset_id": "family"}), encoding="utf-8")
    monkeypatch.setattr(component_catalog, "_REPOSITORY", tmp_path)
    (tmp_path / "family.component.json").write_text(json.dumps({
        "tags": ["tool"], "attachment_interfaces": [], "command_targets": [{
            "command_target_id": "holder", "capabilities": [],
        }],
    }), encoding="utf-8")
    monkeypatch.setattr(component_catalog, "_SOURCES", source)
    monkeypatch.setattr(component_catalog, "_COMPONENT_TYPES", tmp_path)
    _, _, facts = component_catalog._profiles()
    assert facts["profile"]["frameTargets"] == []

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
