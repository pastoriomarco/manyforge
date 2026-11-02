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
    rows = [{"apply_id": "apply-1", "apply_generation": 1,
             "scene_initialization": "initialize",
             "record_digest": "sha256:" + "a" * 64, "origin": _origin()},
            {"apply_id": "apply-2", "apply_generation": 2,
             "scene_initialization": "preserve",
             "record_digest": "sha256:" + "b" * 64, "origin": _origin(scenario_id)}]
    return {"schema_version": "manyforge.committed_scenario_activation.v3", **deepcopy(rows[-1]),
            "publication_generation": 3, "program": None, "committed_applies": rows}


def test_typed_history_keeps_every_apply_and_returns_exact_source_without_mutation():
    value = _publication()
    original = deepcopy(value)
    assert validate_committed_scenario_activation(value) is value
    assert value == original
    assert len(value["committed_applies"]) + 1 == value["publication_generation"]
    assert value["origin"]["scenario_id"] == "scenario_b"
    later = _publication("scenario_a")
    later["committed_applies"].insert(1, value["committed_applies"][1])
    later["committed_applies"][-1].update({"apply_id": "apply-3", "apply_generation": 3,
                                         "record_digest": "sha256:" + "c" * 64})
    later.update({"apply_id": "apply-3", "apply_generation": 3,
                  "record_digest": "sha256:" + "c" * 64, "publication_generation": 4})
    assert validate_committed_scenario_activation(later) is later
    assert [row["origin"]["scenario_id"] for row in later["committed_applies"]] == [
        "scenario_a", "scenario_b", "scenario_a"]
    # Manager's journal evicted the older Applies: the retained ones remain
    # the history (F7: more Applies than the journal retains).
    evicted = _publication()
    evicted["committed_applies"].pop(0)
    evicted["publication_generation"] = 400
    assert validate_committed_scenario_activation(evicted) is evicted


@pytest.mark.parametrize("mutation", ["omit_current", "more_than_published", "current_foreign_origin", "duplicate",
    "unsorted", "v1", "old_scenario_key", "foreign_current_id", "foreign_current_generation"])
def test_typed_history_rejects_incomplete_or_mismatched_publication(mutation):
    value = _publication()
    if mutation == "omit_current": value["committed_applies"].pop()
    elif mutation == "more_than_published": value["publication_generation"] = 2
    elif mutation == "current_foreign_origin": value["origin"]["scenario_id"] = "foreign"
    elif mutation == "duplicate": value["committed_applies"][0] = deepcopy(value["committed_applies"][1])
    elif mutation == "unsorted": value["committed_applies"].reverse()
    elif mutation == "v1": value["schema_version"] = "manyforge.committed_scenario_activation.v1"
    elif mutation == "old_scenario_key": value["scenario_id"] = "fake"
    elif mutation == "foreign_current_id": value["apply_id"] = "foreign"
    elif mutation == "foreign_current_generation": value["apply_generation"] = 3
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


@pytest.mark.parametrize("change", [{"kind": "unknown"}, {"scenario_id": "s" * 257},
    {"cell_id": "cell_a"}, {"source_document_digest": None}, {"project_path": "relative"}])
def test_typed_history_validates_every_row(change):
    value = _publication()
    value["committed_applies"][0]["origin"] = {**_origin(), **change}
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


@pytest.mark.parametrize("purpose", [None, "initialize_once", True, "", "reset"])
def test_committed_scene_initialization_rejects_untyped_purpose(purpose):
    value = _publication()
    value["scene_initialization"] = purpose
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


def test_committed_scene_initialization_is_joined_to_exact_current_history():
    value = _publication()
    value["scene_initialization"] = "initialize"
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


def test_an_apply_can_request_scene_initialization():
    value = _publication()
    value["scene_initialization"] = "initialize"
    value["committed_applies"][-1]["scene_initialization"] = "initialize"
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
