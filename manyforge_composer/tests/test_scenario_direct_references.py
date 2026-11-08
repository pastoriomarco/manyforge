"""Scenario v3 projects preserve only the two authored members."""

import json

import pytest

from manyforge_composer.backend.authoring_admission import scenario_artifact_http_exception
from manyforge_behavior.program import ProgramFormatUnsupported
from manyforge_composer.backend.scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
    capture_scenario_artifact,
    save_scenario_artifact,
    save_scenario_member,
    scenario_content_digest,
    validate_scenario_document,
)


def _document():
    return build_scenario_document(
        scenario_id="cell", name="cell", description="",
        program_draft={"api_version": "manyforge.program.v3"},
        deployment_draft=None, manyforge_version="test", last_known_status="unknown",
    )


def test_two_member_project_round_trip_and_save(tmp_path):
    target = tmp_path / "cell.scenario"
    document = _document()
    save_scenario_artifact(document, str(target), allowed_roots=[str(tmp_path)], overwrite=False)
    assert {p.name for p in target.iterdir()} == {"program.yaml", "deployment.yaml", "scenario.yaml"}
    loaded, _, _ = capture_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert loaded == document
    updated = {"api_version": "manyforge.program.v3", "program_id": "changed"}
    loaded, _, _ = save_scenario_member(str(target), "program_draft", updated, allowed_roots=[str(tmp_path)])
    manifest = json.loads((target / "scenario.yaml").read_text())
    assert loaded["program_draft"] == updated
    assert set(manifest["members"]) == {"program.yaml", "deployment.yaml"}
    assert manifest["saved_status"]["last_known_status"] == "unknown"
    assert manifest["saved_status"]["checked_content_digest"] == scenario_content_digest(updated, None)


def test_old_authoring_version_refused_with_typed_error():
    old = {**_document(), "api_version": "manyforge.scenario.authoring.v3",
           "program_role_assignment": {"component_roles": [], "target_roles": []}}
    with pytest.raises(ScenarioArtifactError) as error:
        validate_scenario_document(old, source="old")
    assert error.value.code == "scenario_artifact_version_unsupported"
    response_error = scenario_artifact_http_exception(error.value)
    assert response_error.detail["code"] == "scenario_artifact_version_unsupported"
    assert "re-author" in response_error.detail["message"]


def test_program_format_refusal_preserves_details_through_scenario_http_error():
    original = ProgramFormatUnsupported("manyforge.program.v2", "manyforge.program.v3")
    error = ScenarioArtifactError(original.code, str(original), details=original.details)
    response_error = scenario_artifact_http_exception(error)
    assert response_error.detail["code"] == "program_format_unsupported"
    assert response_error.detail["context"] == original.details


def test_old_project_and_assignment_member_refused(tmp_path):
    target = tmp_path / "cell.scenario"
    save_scenario_artifact(_document(), str(target), allowed_roots=[str(tmp_path)], overwrite=False)
    manifest_path = target / "scenario.yaml"
    manifest = json.loads(manifest_path.read_text())
    manifest["api_version"] = "manyforge.scenario.project.v2"
    manifest_path.write_text(json.dumps(manifest))
    with pytest.raises(ScenarioArtifactError) as old:
        capture_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert old.value.code == "scenario_artifact_version_unsupported"
    manifest["api_version"] = "manyforge.scenario.project.v3"
    manifest_path.write_text(json.dumps(manifest))
    (target / "assignment.yaml").write_text("{}")
    with pytest.raises(ScenarioArtifactError) as removed:
        capture_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert removed.value.code == "scenario_artifact_version_unsupported"
    original_manifest = manifest_path.read_bytes()
    with pytest.raises(ScenarioArtifactError) as overwrite:
        save_scenario_artifact(_document(), str(target), allowed_roots=[str(tmp_path)], overwrite=True)
    assert overwrite.value.code == "scenario_artifact_version_unsupported"
    assert manifest_path.read_bytes() == original_manifest
