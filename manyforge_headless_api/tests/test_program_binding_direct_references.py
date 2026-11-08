"""Direct-reference mutations against the genuine native v3 binding fixture."""

import copy
import json
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_headless_api.program_binding_transport import (
    ProgramBindingTransportError,
    validate_program_binding,
)


FIXTURE = (Path(__file__).resolve().parents[2] /
           "manyforge_deployment/resources/program_binding_lite6_native.fixture.json")
VERSION = "manyforge.program_binding.v3"


@pytest.mark.parametrize("collection", ["target_bindings", "frame_bindings", "collision_body_bindings"])
@pytest.mark.parametrize("fault", ["missing_component", "unknown_field"])
def test_direct_reference_is_a_closed_qualified_identity(collection, fault):
    document = json.loads(FIXTURE.read_text(encoding="utf-8"))
    if document.get("api_version") != VERSION:
        pytest.skip("genuine native v3 binding fixture is not available")
    validate_program_binding(document)
    mutated = copy.deepcopy(document)
    reference = mutated[collection][0]["target" if collection == "target_bindings" else "reference"]
    if fault == "missing_component":
        del reference["component_id"]
    else:
        reference["unknown"] = True
    mutated["program_binding_id"] = compute_content_identity(
        mutated, own_field="program_binding_id", identity_domain="program-binding-id", api_version=VERSION
    )
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(mutated)
