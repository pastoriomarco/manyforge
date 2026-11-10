"""The shared Draft pass keeps existing incomplete-field and numeric behavior."""
from pathlib import Path
from copy import deepcopy

import pytest

from manyforge_behavior.authoring_forest import (
    AUTHORING_FOREST_VERSION, AuthoringForestError, derive_candidate, empty_authoring,
    validate_invariants,
)
from manyforge_behavior.catalog import NodeCatalog


def test_incomplete_forest_and_raw_integer_kind():
    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    authoring = empty_authoring(catalog)
    assert authoring["api_version"] == "manyforge.program.v3"
    assert authoring["schema_version"] == AUTHORING_FOREST_VERSION
    before = deepcopy(authoring)
    candidate, diagnostics = derive_candidate(authoring, catalog)
    assert candidate is None and any(row["code"] == "authoring_forest_root_count" for row in diagnostics)
    assert authoring == before
    authoring["forest"] = [{"node_id": "repeat", "instance_id": "repeat", "parameters": {}, "children": []}]
    validate_invariants(authoring, catalog)
    candidate, diagnostics = derive_candidate(authoring, catalog)
    assert candidate is None and diagnostics
    authoring["forest"][0]["parameters"]["num_cycles"] = 1.0
    with pytest.raises(AuthoringForestError) as error:
        validate_invariants(authoring, catalog)
    assert error.value.code == "composer_authoring_field_invalid"
    authoring["forest"][0]["parameters"]["num_cycles"] = 1
    validate_invariants(authoring, catalog)
