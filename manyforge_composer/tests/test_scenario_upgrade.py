"""Upgrade and load (OD-391): a Scenario saved in an older file format is
upgraded in place by the converters' own code, with one backup kept."""

from __future__ import annotations

from copy import deepcopy
import errno
import json
import os
from pathlib import Path
import uuid

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend import scenario_upgrade
from manyforge_composer.backend.authoring_forest import (
    adopt_active_catalog,
    derive_candidate,
    from_program,
    validate_invariants,
)
from manyforge_composer.backend.deployment_intent import validate_deployment_intent
from manyforge_composer.backend.routes_scenario_documents import (
    create_scenario_document_router,
)
from manyforge_composer.backend.scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
    capture_scenario_artifact,
    discover_scenario_artifacts,
    save_scenario_artifact,
)
from manyforge_composer.backend.state import ComposerState


_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM = from_program(
    Program(_ROOT / "examples/pick_and_place_minimal.program.yaml",
            build_node_environment().catalog)
)
_ALL = ["collision_rules", "scenario_identity", "program_profile", "lane_labels"]
# A Scenario as 0.3.1 saved it (examples/mock_ur5_pick_and_place_minimal.scenario
# at v0.3.1).
_V031 = Path(__file__).resolve().parent / "fixtures/scenario_v0_3_1/mock_ur5_pick_and_place_minimal"


def _deployment() -> dict:
    return {
        "api_version": "manyforge.deployment_intent.v3",
        "collision_rules": [{"rule_id": "everything", "clearance_m": 0.001}],
        "components": [{
            "component_id": "fixture_robot",
            "component_profile_id": "fixture_native_v1",
            "model_source": {"urdf": {"project_path": "../model.urdf"}, "package_roots": {}},
            "root_pose_in_cell": {"position_m": [0.0, 0.0, 0.0],
                                  "orientation_xyzw": [0.0, 0.0, 0.0, 1.0]},
            "target_settings": {},
            "configuration": {},
        }],
        "attachments": [],
        "policies": {},
    }


def _current(tmp_path: Path, name: str = "cell") -> Path:
    """A Scenario saved in the current format, with an extra asset file."""

    tmp_path.mkdir(parents=True, exist_ok=True)
    (tmp_path / "model.urdf").write_text('<robot name="fixture"/>\n', encoding="utf-8")
    document = build_scenario_document(
        scenario_id=name, name=name, description="Upgrade fixture",
        program_draft=deepcopy(_PROGRAM), deployment_draft=_deployment(),
        manyforge_version="0.4.0", last_known_status="ready",
    )
    project = Path(save_scenario_artifact(
        document, str(tmp_path / f"{name}.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    (project / "assets").mkdir()
    (project / "assets" / "part.stl").write_bytes(b"solid part\nendsolid part\n")
    return project


def _edit(project: Path, member: str, change) -> None:
    path = project / member
    value = json.loads(path.read_text(encoding="utf-8"))
    change(value)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def _age(project: Path, conversions: list[str]) -> None:
    """Write the older formats the named conversions convert back."""

    if "collision_rules" in conversions:
        def old_deployment(value: dict) -> None:
            value["api_version"] = "manyforge.deployment_intent.v2"
            del value["collision_rules"]

        def old_program(value: dict) -> None:
            value["api_version"] = "manyforge.program.v3"
            value["catalog"]["content_version"] = "3.0.0"

        _edit(project, "deployment.yaml", old_deployment)
        _edit(project, "program.yaml", old_program)
    if "scenario_identity" in conversions:
        _edit(project, "program.yaml", lambda value: value.update(
            program_id="cell", name="Cell", description="An older Program"))
        _edit(project, "deployment.yaml", lambda value: value.update(
            deployment_artifact_id="cell", name="Cell"))
    if "program_profile" in conversions:
        _edit(project, "program.yaml", lambda value: value.update(
            program_profile="composable-cell-v1"))
    if "lane_labels" in conversions:
        _edit(project, "program.yaml", lambda value: value["compatibility"].update(
            lanes=["manyforge-only"]))
        _edit(project, "deployment.yaml", lambda value: value.update(
            compatibility={"lane": "manyforge-only", "tags": ["cell"]}))


def _files(folder: Path) -> dict[str, bytes]:
    return {str(path.relative_to(folder)): path.read_bytes()
            for path in sorted(folder.rglob("*")) if path.is_file()}


def _client(tmp_path: Path) -> TestClient:
    app = FastAPI()
    app.include_router(create_scenario_document_router(
        ComposerState(application_start_id=0x3F9A0000000000B1,
                      allowed_fs_roots=[str(tmp_path)])))
    return TestClient(app)


def _upgrade(client: TestClient, project: Path, key: str | None = None):
    return client.post("/api/scenario-documents/upgrade",
                       json={"path": str(project), "request_key": key or uuid.uuid4().hex})


def _check(client: TestClient, project: Path):
    return client.get("/api/scenario-documents/upgrade", params={"path": str(project)})


def _siblings(project: Path) -> list[str]:
    return sorted(path.name for path in project.parent.iterdir() if path.is_dir())


def test_check_names_every_conversion_and_writes_nothing(tmp_path: Path) -> None:
    project = _current(tmp_path)
    _age(project, _ALL)
    before = _files(project)

    reply = _check(_client(tmp_path), project)

    assert reply.status_code == 200
    body = reply.json()
    assert body["api_version"] == "manyforge.scenario_upgrade.v1"
    assert body["status"] == "available"
    assert [row["conversion"] for row in body["conversions"]] == _ALL
    assert body["backup_path"] is None
    assert body["backup_pattern"] == f"{project}.before-0.4.0-YYYYMMDDTHHMMSSZ"
    assert _files(project) == before
    assert _siblings(project) == ["cell.scenario"]


def test_upgrade_keeps_the_original_as_backup_and_the_scenario_loads(tmp_path: Path) -> None:
    project = _current(tmp_path)
    current_program = json.loads((project / "program.yaml").read_text(encoding="utf-8"))
    _age(project, _ALL)
    with pytest.raises(ScenarioArtifactError):
        capture_scenario_artifact(str(project), allowed_roots=[str(tmp_path)])
    before = _files(project)

    reply = _upgrade(_client(tmp_path), project)

    assert reply.status_code == 200, reply.text
    body = reply.json()
    assert body["status"] == "upgraded"
    assert [row["conversion"] for row in body["conversions"]] == _ALL
    backup = Path(body["backup_path"])
    assert backup.parent == project.parent
    assert backup.name.startswith("cell.scenario.before-0.4.0-")
    # One backup, exactly the original; no copy left behind.
    assert _files(backup) == before
    assert _siblings(project) == sorted(["cell.scenario", backup.name])
    # The upgraded Scenario loads, as current as a Save writes it.
    document, _source, _digest = capture_scenario_artifact(
        str(project), allowed_roots=[str(tmp_path)])
    assert document["program_draft"] == current_program
    assert document["deployment_draft"]["collision_rules"] == [
        {"rule_id": "everything", "clearance_m": 0.001}]
    assert document["deployment_draft"]["compatibility"] == {"tags": ["cell"]}
    assert (project / "assets" / "part.stl").read_bytes() == before["assets/part.stl"]


@pytest.mark.parametrize("conversion", _ALL)
def test_each_conversion_alone_is_offered_and_applied(tmp_path: Path, conversion: str) -> None:
    project = _current(tmp_path)
    _age(project, [conversion])
    client = _client(tmp_path)

    assert [row["conversion"] for row in _check(client, project).json()["conversions"]] == [
        conversion]
    reply = _upgrade(client, project)

    assert reply.status_code == 200, reply.text
    assert [row["conversion"] for row in reply.json()["conversions"]] == [conversion]
    capture_scenario_artifact(str(project), allowed_roots=[str(tmp_path)])


def test_a_current_scenario_is_left_alone_without_a_backup(tmp_path: Path) -> None:
    project = _current(tmp_path)
    before = _files(project)
    client = _client(tmp_path)

    checked = _check(client, project).json()
    upgraded = _upgrade(client, project).json()

    assert checked["status"] == "current" and checked["conversions"] == []
    assert upgraded["status"] == "current" and upgraded["backup_path"] is None
    assert _files(project) == before
    assert _siblings(project) == ["cell.scenario"]


def test_the_same_request_again_keeps_one_backup(tmp_path: Path) -> None:
    project = _current(tmp_path)
    _age(project, _ALL)
    client = _client(tmp_path)
    key = uuid.uuid4().hex

    first = _upgrade(client, project, key).json()
    upgraded = _files(project)
    again = _upgrade(client, project, key).json()

    assert first["status"] == "upgraded"
    assert again["status"] == "current"
    assert _files(project) == upgraded
    assert len(_siblings(project)) == 2


def test_a_part_without_a_counterpart_is_refused_and_nothing_changes(tmp_path: Path) -> None:
    project = _current(tmp_path)
    _age(project, _ALL)

    def undeclared_policy(value: dict) -> None:
        value["forest"][0]["children"][0].setdefault("parameters", {})[
            "contact_policy_id"] = "missing_policy"

    _edit(project, "program.yaml", undeclared_policy)
    before = _files(project)
    client = _client(tmp_path)

    for reply in (_check(client, project), _upgrade(client, project)):
        assert reply.status_code == 409
        detail = reply.json()["detail"]
        assert detail["code"] == "scenario_upgrade_conversion_failed"
        assert detail["context"] == {"conversion": "collision_rules"}
        assert "missing_policy" in detail["message"]
        assert str(project / "program.yaml") in detail["message"]
    assert _files(project) == before
    assert _siblings(project) == ["cell.scenario"]
    # Hidden staging copies are gone too.
    assert sorted(os.listdir(tmp_path)) == ["cell.scenario", "model.urdf"]


def test_a_failed_exchange_is_refused_and_nothing_changes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Where renameat2(RENAME_EXCHANGE) is unavailable (old kernel, file
    system without it), the upgrade is refused; there is no two-step swap."""

    project = _current(tmp_path)
    _age(project, _ALL)
    before = _files(project)

    def unsupported(_first, _second) -> None:
        raise OSError(errno.EINVAL, "Invalid argument")

    monkeypatch.setattr(scenario_upgrade, "_exchange", unsupported)
    reply = _upgrade(_client(tmp_path), project)

    assert reply.status_code == 400
    detail = reply.json()["detail"]
    assert detail["code"] == "scenario_artifact_write_failed"
    assert "the Scenario is unchanged" in detail["message"]
    assert _files(project) == before
    assert sorted(os.listdir(tmp_path)) == ["cell.scenario", "model.urdf"]


class _Crash(BaseException):
    """The process dying between the exchange and the backup rename."""


def test_a_crash_after_the_exchange_leaves_the_upgraded_scenario_in_place(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    project = _current(tmp_path)
    current_program = json.loads((project / "program.yaml").read_text(encoding="utf-8"))
    _age(project, _ALL)
    before = _files(project)

    def crash(_source, _destination) -> None:
        raise _Crash

    monkeypatch.setattr(scenario_upgrade.os, "rename", crash)
    with pytest.raises(_Crash):
        scenario_upgrade.upgrade_scenario(
            str(project), allowed_roots=[str(tmp_path)], version="0.4.0",
            request_key=uuid.uuid4().hex)

    # The Scenario's folder is the converted one ...
    document, _source, _digest = capture_scenario_artifact(
        str(project), allowed_roots=[str(tmp_path)])
    assert document["program_draft"] == current_program
    # ... and the original is whole at the copy's hidden name.
    stages = [path for path in tmp_path.iterdir()
              if path.name.startswith(".cell.scenario.upgrade-")]
    assert len(stages) == 1
    assert _files(stages[0]) == before
    assert sorted(path.name for path in tmp_path.iterdir()) == sorted(
        ["cell.scenario", "model.urdf", stages[0].name])


def test_an_upgrade_outside_the_allowed_folders_is_refused(tmp_path: Path) -> None:
    project = _current(tmp_path / "elsewhere")
    _age(project, _ALL)
    allowed = tmp_path / "allowed"
    allowed.mkdir()

    reply = _upgrade(_client(allowed), project)

    assert reply.status_code == 403
    assert reply.json()["detail"]["code"] == "scenario_artifact_path_invalid"


def test_an_upgraded_0_3_1_scenario_builds(tmp_path: Path) -> None:
    project = tmp_path / "mock_ur5_pick_and_place_minimal.scenario"
    project.mkdir()
    for member in ("scenario.yaml", "program.yaml", "deployment.yaml"):
        (project / member).write_bytes((_V031 / member).read_bytes())

    reply = _upgrade(_client(tmp_path), project)

    assert reply.status_code == 200, reply.text
    assert [row["conversion"] for row in reply.json()["conversions"]] == _ALL
    program = json.loads((project / "program.yaml").read_text(encoding="utf-8"))
    assert "lanes" not in program["compatibility"]
    lane_changes = reply.json()["conversions"][-1]["changes"]
    assert lane_changes == ["program.yaml: compatibility.lanes removed",
                            "deployment.yaml: compatibility.lane removed"]
    catalog = build_node_environment().catalog
    adopt_active_catalog(program, catalog)
    validate_invariants(program, catalog)
    built, diagnostics = derive_candidate(program, catalog)
    assert built is not None, diagnostics
    validate_deployment_intent(
        json.loads((project / "deployment.yaml").read_text(encoding="utf-8")),
        source="deployment.yaml")


def test_the_load_list_skips_upgrade_backups_and_staging_copies(tmp_path: Path) -> None:
    project = _current(tmp_path)
    _age(project, _ALL)
    backup = Path(_upgrade(_client(tmp_path), project).json()["backup_path"])
    staging = tmp_path / ".cell.scenario.upgrade-0123456789abcdef"
    staging.mkdir()
    (staging / "scenario.yaml").write_bytes((backup / "scenario.yaml").read_bytes())

    _directory, entries = discover_scenario_artifacts(
        str(tmp_path), allowed_roots=[str(tmp_path)], manyforge_version="0.4.0")

    assert [entry["path"] for entry in entries] == [str(project)]


def test_a_lanes_only_scenario_is_refused_as_older_and_upgrades_to_load(tmp_path: Path) -> None:
    """OD-392: lane labels are refused like the other removed fields, so the
    Load dialog's upgrade check runs and offers the one conversion."""

    project = _current(tmp_path)
    _edit(project, "program.yaml", lambda value: value["compatibility"].update(
        lanes=["manyforge-only"]))
    with pytest.raises(ScenarioArtifactError) as refused:
        capture_scenario_artifact(str(project), allowed_roots=[str(tmp_path)])
    assert refused.value.code == "scenario_document_dropped_fields"
    assert "program.yaml: compatibility.lanes" in str(refused.value)
    client = _client(tmp_path)

    checked = _check(client, project).json()
    upgraded = _upgrade(client, project).json()

    assert checked["status"] == "available"
    assert [row["conversion"] for row in checked["conversions"]] == ["lane_labels"]
    assert upgraded["status"] == "upgraded"
    document, _source, _digest = capture_scenario_artifact(
        str(project), allowed_roots=[str(tmp_path)])
    assert "lanes" not in document["program_draft"]["compatibility"]
