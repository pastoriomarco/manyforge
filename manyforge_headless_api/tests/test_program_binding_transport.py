"""Cross-language fixture mutation corpus for the generated Binding validator."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import re
from typing import Any, Callable

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_headless_api.program_binding_transport import (
    ProgramBindingTransportError,
    validate_program_binding,
)


FIXTURE = (
    Path(__file__).resolve().parents[2]
    / "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
)
Mutation = Callable[[dict[str, Any]], None]


def _native_fixture() -> dict[str, Any]:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def _rehash(value: dict[str, Any]) -> None:
    value["program_binding_id"] = compute_content_identity(
        value,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )


def _fixture() -> dict[str, Any]:
    """Enrich the native fixture with one valid contact row for parser coverage."""
    value = _native_fixture()
    component = value["component_role_bindings"][0]
    target = value["target_role_bindings"][0]
    collision = target["collision_body_role_bindings"][0]
    model = next(
        row
        for row in component["model_role_bindings"]
        if row["model_instance_id"] == collision["model_instance_id"]
    )
    value["contact_grasp_bindings"] = [
        {
            "contact_policy_id": "fixture_contact",
            "contact_policy_revision": "sha256:" + "c" * 64,
            "runtime_object_id": "fixture_object",
            "allowed_body_bindings": [
                {
                    "component_role_id": target["component_role_id"],
                    "target_role_id": target["target_role_id"],
                    "controllable_component_id": target[
                        "controllable_component_id"
                    ],
                    "command_target_id": target["command_target_id"],
                    "model_role_id": model["model_role_id"],
                    "collision_body_role_id": collision[
                        "collision_body_role_id"
                    ],
                    "model_instance_id": collision["model_instance_id"],
                    "model_revision": collision["model_revision"],
                    "collision_body_ids": [
                        "component/"
                        f"{target['controllable_component_id']}/model/"
                        f"{collision['model_instance_id']}/body/"
                        f"{collision['collision_body_role_id']}/collision/{index}"
                        for index, _ in enumerate(
                            collision["compiled_collision_body_names"]
                        )
                    ],
                }
            ],
            "max_penetration_m": 0.001,
            "terminal_window_start_fraction": 0.8,
        }
    ]
    _rehash(value)
    return value


def _rows(value: dict[str, Any]) -> tuple[dict[str, Any], ...]:
    component = value["component_role_bindings"][0]
    target = value["target_role_bindings"][0]
    contact = value["contact_grasp_bindings"][0]
    return component, target, contact


def _unknown(record: Callable[[dict[str, Any]], dict[str, Any]]) -> Mutation:
    return lambda value: record(value).__setitem__("unknown", True)


def _invalid_ref(field: str, id_field: str, revision_field: str) -> Mutation:
    def mutate(value: dict[str, Any]) -> None:
        _rows(value)[1]["capability_bindings"][0][field] = {
            id_field: "fixture",
            revision_field: "sha256:" + "a" * 64,
            "unknown": True,
        }
    return mutate


SHAPE_MUTATIONS: tuple[tuple[str, Mutation], ...] = (
    ("program_binding: expected closed record", _unknown(lambda value: value)),
    (".node_environment: expected closed record", lambda value: value["node_environment"].pop("catalog_id")),
    (".node_environment: expected closed record", _unknown(lambda value: value["node_environment"])),
    (".component_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[0])),
    (".model_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[0]["model_role_bindings"][0])),
    (".target_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1])),
    (".frame_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1]["frame_role_bindings"][0])),
    (".resource_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1]["resource_role_bindings"][0])),
    (".collision_body_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1]["collision_body_role_bindings"][0])),
    (".capability_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1]["capability_bindings"][0])),
    (".control_binding: expected closed record", _invalid_ref("control_binding", "control_binding_id", "control_binding_revision")),
    (".capability_provider_binding: expected closed record", _invalid_ref("capability_provider_binding", "capability_provider_binding_id", "capability_provider_binding_revision")),
    (".command_schema: expected closed record", _invalid_ref("command_schema", "command_schema_id", "command_schema_revision")),
    (".contact_grasp_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[2])),
    (".allowed_body_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[2]["allowed_body_bindings"][0])),
    (".provenance: expected closed record", _unknown(lambda value: value["provenance"])),
    (".source_records[0]: expected closed record", _unknown(lambda value: value["provenance"]["source_records"][0])),
)


def _case(name: str) -> Mutation:
    def mutate(value: dict[str, Any]) -> None:
        component, target, contact = _rows(value)
        resource = target["resource_role_bindings"][0]
        collision = target["collision_body_role_bindings"][0]
        capability = target["capability_bindings"][0]
        body = contact["allowed_body_bindings"][0]
        provenance = value["provenance"]["source_records"]
        if name == "null":
            value["program_id"] = None
        elif name == "wrong_type":
            value["target_role_bindings"] = {}
        elif name == "identity_pattern":
            value["program_id"] = "not portable"
        elif name == "identity_bytes":
            value["program_id"] = "a" * 129
        elif name == "text_bytes":
            target["frame_role_bindings"][0]["compiled_frame_name"] = "é" * 2049
        elif name == "invalid_utf8":
            target["frame_role_bindings"][0]["compiled_frame_name"] = "\ud800"
        elif name == "root_minimum":
            value["component_role_bindings"] = []
        elif name == "root_maximum":
            value["component_role_bindings"] = [component] * 4097
        elif name == "nested_maximum":
            target["frame_role_bindings"] = [target["frame_role_bindings"][0]] * 4097
        elif name == "provenance_maximum":
            value["provenance"]["source_records"] = [provenance[0]] * 16385
        elif name == "component_order":
            value["component_role_bindings"] = [component, copy.deepcopy(component)]
        elif name == "model_order":
            component["model_role_bindings"].append(copy.deepcopy(component["model_role_bindings"][0]))
        elif name == "target_order":
            value["target_role_bindings"].append(copy.deepcopy(target))
        elif name == "frame_order":
            target["frame_role_bindings"].append(copy.deepcopy(target["frame_role_bindings"][0]))
        elif name == "resource_order":
            target["resource_role_bindings"].append(copy.deepcopy(resource))
        elif name == "collision_order":
            target["collision_body_role_bindings"].append(copy.deepcopy(collision))
        elif name == "capability_order":
            target["capability_bindings"].append(copy.deepcopy(capability))
        elif name == "contact_order":
            value["contact_grasp_bindings"].append(copy.deepcopy(contact))
        elif name == "resource_vector_size":
            resource["compiled_resource_names"].append("extra")
        elif name == "resource_id_duplicate":
            resource["resource_ids"].append(resource["resource_ids"][0])
            resource["compiled_resource_names"].append("extra")
        elif name == "resource_name_duplicate":
            resource["resource_ids"].append("extra")
            resource["compiled_resource_names"].append(resource["compiled_resource_names"][0])
        elif name == "collision_name_duplicate":
            collision["compiled_collision_body_names"].append(collision["compiled_collision_body_names"][0])
        elif name == "all_null_capability":
            capability["control_binding"] = None
            capability["capability_provider_binding"] = None
        elif name == "target_component":
            target["controllable_component_id"] = "mismatch"
        elif name == "contact_penetration":
            contact["max_penetration_m"] = 0
        elif name == "contact_fraction":
            contact["terminal_window_start_fraction"] = 1.01
        elif name == "contact_bool":
            contact["max_penetration_m"] = True
        elif name == "contact_nonfinite":
            contact["max_penetration_m"] = float("nan")
        elif name == "body_minimum":
            contact["allowed_body_bindings"] = []
        elif name == "body_order":
            contact["allowed_body_bindings"].append(copy.deepcopy(body))
        elif name == "generated_body_id":
            body["collision_body_ids"][0] = "wrong"
        elif name == "body_target":
            body["command_target_id"] = "mismatch"
        elif name == "body_collision":
            body["model_revision"] = "sha256:" + "f" * 64
        elif name == "body_model":
            body["model_role_id"] = "mismatch"
        elif name == "provenance_kind":
            provenance[0]["record_kind"] = "unknown_kind"
        elif name == "provenance_kind_list":
            provenance[0]["record_kind"] = []
        elif name == "provenance_kind_object":
            provenance[0]["record_kind"] = {}
        elif name == "provenance_order":
            provenance.append(copy.deepcopy(provenance[0]))
        else:
            raise AssertionError(name)
    return mutate


CASES = (
    ("null", "bounded non-empty string"),
    ("wrong_type", "bounded array"),
    ("identity_pattern", "portable identity"),
    ("identity_bytes", "bounded non-empty string"),
    ("text_bytes", "bounded non-empty string"),
    ("invalid_utf8", "valid UTF-8"),
    ("root_minimum", "bounded array"),
    ("root_maximum", "bounded array"),
    ("nested_maximum", "bounded array"),
    ("provenance_maximum", "bounded array"),
    ("component_order", "canonical sorted unique"),
    ("model_order", "canonical sorted unique"),
    ("target_order", "canonical sorted unique"),
    ("frame_order", "canonical sorted unique"),
    ("resource_order", "canonical sorted unique"),
    ("collision_order", "canonical sorted unique"),
    ("capability_order", "canonical sorted unique"),
    ("contact_order", "canonical sorted unique"),
    ("resource_vector_size", "equal length"),
    ("resource_id_duplicate", "duplicate-free"),
    ("resource_name_duplicate", "duplicate-free"),
    ("collision_name_duplicate", "duplicate-free"),
    ("all_null_capability", "control or provider"),
    ("target_component", "target must resolve"),
    ("contact_penetration", "numeric ranges"),
    ("contact_fraction", "numeric ranges"),
    ("contact_bool", "finite number"),
    ("contact_nonfinite", "finite number"),
    ("body_minimum", "bounded array"),
    ("body_order", "canonical sorted unique"),
    ("generated_body_id", "generated scope"),
    ("body_target", "exact target"),
    ("body_collision", "collision binding"),
    ("body_model", "model binding"),
    ("provenance_kind", "CatalogRecordKind"),
    ("provenance_kind_list", "CatalogRecordKind"),
    ("provenance_kind_object", "CatalogRecordKind"),
    ("provenance_order", "canonical sorted unique"),
)


def test_native_binding_fixture_is_accepted_without_copy() -> None:
    document = _native_fixture()
    assert validate_program_binding(document) is document


@pytest.mark.parametrize(("expected", "mutation"), SHAPE_MUTATIONS)
def test_every_binding_record_is_closed(expected: str, mutation: Mutation) -> None:
    document = _fixture()
    mutation(document)
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError, match=re.escape(expected)):
        validate_program_binding(document)


@pytest.mark.parametrize(("name", "expected"), CASES)
def test_binding_transport_invariant_mutation_corpus(
    name: str,
    expected: str,
) -> None:
    document = _fixture()
    _case(name)(document)
    if name not in {"invalid_utf8", "contact_nonfinite"}:
        _rehash(document)
    with pytest.raises(ProgramBindingTransportError, match=re.escape(expected)):
        validate_program_binding(document)


def test_binding_transport_rejects_stale_identity() -> None:
    document = _fixture()
    document["graph_revision"] = "sha256:" + "f" * 64
    with pytest.raises(ProgramBindingTransportError, match="content identity"):
        validate_program_binding(document)


def test_transport_does_not_infer_native_bundle_context() -> None:
    document = _fixture()
    document["retained_bundle_id"] = "sha256:" + "f" * 64
    _rehash(document)
    assert validate_program_binding(document) is document
