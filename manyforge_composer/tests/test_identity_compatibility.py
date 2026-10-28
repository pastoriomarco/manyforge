"""Compatible catalog versions and computed self-digests at Composer load."""

from __future__ import annotations

import json
from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.node_environment import build_node_environment
from manyforge_composer.backend.authoring_forest import (
    adopt_active_catalog,
    derive_candidate,
)
from manyforge_composer.backend.program_artifacts import load_program_artifact
from manyforge_composer.backend.scenario_artifacts import (
    capture_scenario_artifact_with_diagnostics,
    save_scenario_artifact,
)
from manyforge_composer.tests.test_scenario_artifacts import (
    _PROGRAM_FIXTURE,
    _document,
)


_EXAMPLE = Path(__file__).resolve().parents[2] / "examples/pick_and_place_minimal.program.yaml"


def _older_minor(version: str) -> str:
    major, minor, _patch = version.split(".")
    return f"{major}.{int(minor) - 1}.0"


def test_hand_edited_scenario_member_loads_with_edited_outside_warning(
    tmp_path: Path,
) -> None:
    (tmp_path / "model.urdf").write_text('<robot name="fixture"/>\n', encoding="utf-8")
    target = Path(
        save_scenario_artifact(
            _document("edited"),
            str(tmp_path / "edited.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    _, _, _, clean = capture_scenario_artifact_with_diagnostics(
        str(target), allowed_roots=[str(tmp_path)]
    )
    assert clean == []

    edited = {"component_roles": [], "target_roles": [
        {"command_target_id": "arm", "target_role_id": "picker"}
    ]}
    (target / "assignment.yaml").write_text(json.dumps(edited) + "\n", encoding="utf-8")
    document, _, _, diagnostics = capture_scenario_artifact_with_diagnostics(
        str(target), allowed_roots=[str(tmp_path)]
    )

    assert document["program_role_assignment"] == edited
    assert diagnostics == [
        {
            "code": "edited_outside_manyforge",
            "source_id": "member:assignment.yaml",
            "message": "Edited outside ManyForge since the last Save",
        }
    ]


def test_scenario_program_under_older_catalog_minor_adopts_the_active_version() -> None:
    catalog = build_node_environment().catalog
    active = derive_candidate(deepcopy(_PROGRAM_FIXTURE), catalog)[0]
    older = deepcopy(_PROGRAM_FIXTURE)
    older["catalog"]["content_version"] = _older_minor(catalog.content_version)

    adopt_active_catalog(older, catalog)

    assert older["catalog"]["content_version"] == catalog.content_version
    program, diagnostics = derive_candidate(older, catalog)
    assert diagnostics == []
    assert program is not None and active is not None
    assert program.program_revision == active.program_revision


@pytest.mark.parametrize("older", (False, True))
def test_extra_catalog_selector_key_is_kept_and_refused(older: bool) -> None:
    catalog = build_node_environment().catalog
    version = _older_minor(catalog.content_version) if older else catalog.content_version
    extra = deepcopy(_PROGRAM_FIXTURE)
    extra["catalog"] = {**extra["catalog"], "content_version": version, "unexpected": True}

    adopt_active_catalog(extra, catalog)

    assert extra["catalog"] == {
        "api_version": catalog.api_version,
        "content_version": catalog.content_version,
        "unexpected": True,
    }
    program, diagnostics = derive_candidate(extra, catalog)
    assert program is None
    assert any("unexpected" in row["message"] for row in diagnostics)


def test_incompatible_catalog_major_is_kept_and_reported() -> None:
    catalog = build_node_environment().catalog
    major = int(catalog.content_version.split(".")[0])
    newer = deepcopy(_PROGRAM_FIXTURE)
    newer["catalog"]["content_version"] = f"{major + 1}.0.0"

    adopt_active_catalog(newer, catalog)

    assert newer["catalog"]["content_version"] == f"{major + 1}.0.0"
    program, diagnostics = derive_candidate(newer, catalog)
    assert program is None
    assert any(row["path"].startswith("program.catalog") for row in diagnostics)


def test_hand_edited_program_file_loads_with_computed_revision(tmp_path: Path) -> None:
    catalog = build_node_environment().catalog
    document = load_strict_json_yaml(_EXAMPLE)
    document["name"] = "Edited in a text editor"
    document["catalog"]["content_version"] = _older_minor(catalog.content_version)
    path = tmp_path / "edited.program.yaml"
    path.write_text(json.dumps(document), encoding="utf-8")

    program, _source = load_program_artifact(
        str(path), allowed_roots=[str(tmp_path)], catalog=catalog
    )

    assert [row["code"] for row in program.load_diagnostics] == [
        "edited_outside_manyforge"
    ]
    assert program.name == "Edited in a text editor"
    assert program.to_dict()["catalog"]["content_version"] == catalog.content_version
    assert program.program_revision != document["program_revision"]
