from copy import deepcopy

import pytest

from manyforge_headless_api.models import PreparedScenarioOrigin
from manyforge_headless_api.program_binding_projection import (
    validate_committed_scenario_activation,
    validate_prepared_activation_origin,
)
from manyforge_headless_api.program_binding_transport import ProgramBindingTransportError


def _origin(scenario_id="scenario_a"):
    return {"kind": "scenario", "scenario_id": scenario_id, "source_document_digest": "sha256:" + "a" * 64,
            "project_path": f"/projects/{scenario_id}.scenario"}


def _publication(scenario_id="scenario_b"):
    return {"schema_version": "manyforge.committed_scenario_activation.v6",
            "apply_id": 2, "apply_generation": 2, "scene_initialization": "preserve",
            "record_digest": "sha256:" + "b" * 64, "origin": _origin(scenario_id),
            "publication_generation": 3, "program": None, "program_frame_facts": None,
            "program_binding_id": None, "retained_bundle_id": "sha256:" + "c" * 64,
            "graph_revision": "sha256:" + "d" * 64}


def test_activation_returns_exact_source_without_mutation():
    value = _publication()
    original = deepcopy(value)
    assert validate_committed_scenario_activation(value) is value
    assert value == original
    assert value["origin"]["scenario_id"] == "scenario_b"


@pytest.mark.parametrize("mutation", ["with_history", "v5", "old_scenario_key", "text_current_id"])
def test_activation_rejects_a_mismatched_record(mutation):
    value = _publication()
    if mutation == "with_history": value["committed_applies"] = []
    elif mutation == "v5": value["schema_version"] = "manyforge.committed_scenario_activation.v5"
    elif mutation == "old_scenario_key": value["scenario_id"] = "fake"
    # An Apply is named by its integer Apply number (1.16.1).
    elif mutation == "text_current_id": value["apply_id"] = "2"
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


@pytest.mark.parametrize("change", [{"kind": "unknown"}, {"scenario_id": "s" * 257},
    {"cell_id": "cell_a"}, {"source_document_digest": None}, {"project_path": "relative"}])
def test_activation_validates_its_origin(change):
    value = _publication()
    value["origin"] = {**_origin(), **change}
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


@pytest.mark.parametrize("purpose", [None, "initialize_once", True, "", "reset"])
def test_committed_scene_initialization_rejects_untyped_purpose(purpose):
    value = _publication()
    value["scene_initialization"] = purpose
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


def test_an_apply_can_request_scene_initialization():
    value = _publication()
    value["scene_initialization"] = "initialize"
    validate_committed_scenario_activation(value)


def test_scenario_origin_names_its_normalized_absolute_project_folder():
    assert validate_prepared_activation_origin(_origin())["project_path"] == (
        "/projects/scenario_a.scenario"
    )
    PreparedScenarioOrigin.model_validate(_origin())
    assert validate_prepared_activation_origin({**_origin(), "project_path": "/"})
    longest = "/" + "p" * 4095
    assert validate_prepared_activation_origin({**_origin(), "project_path": longest})


@pytest.mark.parametrize("project_path", [
    None, "", "projects/a.scenario", "./a.scenario", "/projects/../a.scenario",
    "/projects/./a.scenario", "/projects//a.scenario", "/projects/a.scenario/",
    "//projects/a.scenario", "/projects/a\x00.scenario", "/" + "p" * 4096, 7,
])
def test_scenario_origin_refuses_a_relative_unnormalized_or_unbounded_folder(project_path):
    origin = {**_origin(), "project_path": project_path}
    with pytest.raises(ProgramBindingTransportError, match="project_path"):
        validate_prepared_activation_origin(origin)
    with pytest.raises(ValueError):
        PreparedScenarioOrigin.model_validate(origin)


def test_scenario_origin_requires_its_project_folder():
    origin = _origin()
    del origin["project_path"]
    with pytest.raises(ProgramBindingTransportError, match="closed Scenario origin"):
        validate_prepared_activation_origin(origin)
    with pytest.raises(ValueError):
        PreparedScenarioOrigin.model_validate(origin)


def test_committed_frame_facts_are_tied_to_current_program_and_apply():
    value = _publication()
    revision = "sha256:" + "c" * 64
    value["program"] = {"program_revision": revision}
    value["program_binding_id"] = "sha256:" + "d" * 64
    value["retained_bundle_id"] = "sha256:" + "e" * 64
    value["graph_revision"] = "sha256:" + "f" * 64
    value["program_frame_facts"] = {
        "applied_record_digest": value["record_digest"],
        "program_binding_id": "sha256:" + "d" * 64,
        "program_revision": revision,
        "retained_bundle_id": "sha256:" + "e" * 64,
        "graph_revision": "sha256:" + "f" * 64,
        "components": ["arm"],
        "frames": [{"reference": {"component_id": "arm", "frame_id": "base"}, "match_count": 1}],
    }
    assert validate_committed_scenario_activation(value) is value
    for field in ("applied_record_digest", "program_binding_id", "program_revision", "retained_bundle_id", "graph_revision"):
        stale = deepcopy(value)
        stale["program_frame_facts"][field] = "sha256:" + "1" * 64
        with pytest.raises(ProgramBindingTransportError):
            validate_committed_scenario_activation(stale)
    unbound = deepcopy(value)
    unbound["program_binding_id"] = None
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(unbound)
    unbound["program_frame_facts"] = None
    assert validate_committed_scenario_activation(unbound) is unbound
    no_program = deepcopy(value)
    no_program["program"] = None
    no_program["program_frame_facts"] = None
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(no_program)


def test_older_committed_schema_is_refused_without_rewriting():
    value = _publication()
    value["schema_version"] = "manyforge.committed_scenario_activation.v3"
    before = deepcopy(value)
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)
    assert value == before
