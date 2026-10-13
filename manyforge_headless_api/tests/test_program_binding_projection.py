from copy import deepcopy

import pytest

from manyforge_headless_api.program_binding_projection import validate_committed_scenario_activation
from manyforge_headless_api.program_binding_transport import ProgramBindingTransportError


def _origin(kind="scenario"):
    digest = "sha256:" + "a" * 64
    if kind == "scenario":
        return {"kind": kind, "scenario_id": "scenario_a", "source_document_digest": digest}
    return {"kind": kind, "cell_id": "cell_a", "deployment_artifact_id": "deployment_a",
            "deployment_revision": digest, "retained_bundle_id": digest,
            "graph_revision": digest, "admission_digest": digest}


def _publication(kind="sealed_canonical_empty"):
    rows = [{"apply_id": "apply-1", "apply_generation": 1,
             "record_digest": "sha256:" + "a" * 64, "origin": _origin()},
            {"apply_id": "apply-2", "apply_generation": 2,
             "record_digest": "sha256:" + "b" * 64, "origin": _origin(kind)}]
    return {"schema_version": "manyforge.committed_scenario_activation.v2", **deepcopy(rows[-1]),
            "publication_generation": 3, "program": None, "committed_applies": rows}


def test_typed_history_keeps_every_apply_and_returns_exact_source_without_mutation():
    value = _publication()
    original = deepcopy(value)
    assert validate_committed_scenario_activation(value) is value
    assert value == original
    assert len(value["committed_applies"]) + 1 == value["publication_generation"]
    assert value["origin"]["kind"] == "sealed_canonical_empty"
    later = _publication("scenario")
    later["committed_applies"].insert(1, value["committed_applies"][1])
    later["committed_applies"][-1].update({"apply_id": "apply-3", "apply_generation": 3,
                                         "record_digest": "sha256:" + "c" * 64})
    later.update({"apply_id": "apply-3", "apply_generation": 3,
                  "record_digest": "sha256:" + "c" * 64, "publication_generation": 4})
    assert validate_committed_scenario_activation(later) is later
    assert [row["origin"]["kind"] for row in later["committed_applies"]] == ["scenario", "sealed_canonical_empty", "scenario"]


@pytest.mark.parametrize("mutation", ["omit_empty", "omit_old", "current_foreign_origin", "duplicate",
    "unsorted", "v1", "old_scenario_key", "sealed_program", "foreign_current_id", "foreign_current_generation"])
def test_typed_history_rejects_incomplete_or_mismatched_publication(mutation):
    value = _publication()
    if mutation == "omit_empty": value["committed_applies"].pop()
    elif mutation == "omit_old": value["committed_applies"].pop(0)
    elif mutation == "current_foreign_origin": value["origin"]["admission_digest"] = "sha256:" + "f" * 64
    elif mutation == "duplicate": value["committed_applies"][0] = deepcopy(value["committed_applies"][1])
    elif mutation == "unsorted": value["committed_applies"].reverse()
    elif mutation == "v1": value["schema_version"] = "manyforge.committed_scenario_activation.v1"
    elif mutation == "old_scenario_key": value["scenario_id"] = "fake"
    elif mutation == "sealed_program": value["program"] = {"program_id": "fake"}
    elif mutation == "foreign_current_id": value["apply_id"] = "foreign"
    elif mutation == "foreign_current_generation": value["apply_generation"] = 3
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)


@pytest.mark.parametrize("change", [{"kind": "unknown"}, {"scenario_id": "fake"},
    {"cell_id": "x" * 129}, {"source_document_digest": None}, {"admission_digest": "sha256:invalid"}])
def test_typed_history_validates_all_rows_before_consumer_scenario_subset(change):
    value = _publication("scenario")
    value["committed_applies"][0]["origin"] = {**_origin("sealed_canonical_empty"), **change}
    with pytest.raises(ProgramBindingTransportError):
        validate_committed_scenario_activation(value)
