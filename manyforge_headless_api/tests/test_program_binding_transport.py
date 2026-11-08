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
    document = json.loads(FIXTURE.read_text(encoding="utf-8"))
    if document.get("api_version") != "manyforge.program_binding.v3":
        pytest.skip("held: genuine native v3 Binding fixture required")
    return document


def test_old_program_binding_is_refused_without_repair() -> None:
    document = json.loads(FIXTURE.read_text(encoding="utf-8"))
    document["api_version"] = "manyforge.program_binding.v2"
    before = copy.deepcopy(document)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(document)
    assert document == before


def test_native_v3_binding_fixture_is_required_for_cumulative_gate() -> None:
    document = json.loads(FIXTURE.read_text(encoding="utf-8"))
    assert document.get("api_version") == "manyforge.program_binding.v3", (
        "native v3 Binding fixture is required"
    )


def _rehash(value: dict[str, Any]) -> None:
    if value.get("api_version") != "manyforge.program_binding.v3":
        raise ValueError("only native v3 Binding mutations may be rehashed")
    value["program_binding_id"] = compute_content_identity(
        value,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v3",
    )


def _fixture() -> dict[str, Any]:
    """Enrich the native fixture with valid sparse rows for parser coverage."""
    value = _native_fixture()
    collision = value["collision_body_bindings"][0]
    component = next(row for row in value["component_bindings"]
                     if row["component_id"] == collision["reference"]["component_id"])
    model = next(
        row
        for row in component["model_bindings"]
        if row["model_instance_id"] == collision["model_instance_id"]
    )
    value["contact_grasp_bindings"] = [
        {
            "contact_policy_id": "fixture_contact",
            "contact_policy_revision": "sha256:" + "c" * 64,
            "runtime_object_id": "fixture_object",
            "allowed_body_bindings": [
                {
                    "body": copy.deepcopy(collision["reference"]),
                    "model_role_id": model["model_role_id"],
                    "model_instance_id": collision["model_instance_id"],
                    "model_revision": collision["model_revision"],
                    "collision_body_ids": [
                        "component/"
                        f"{collision['reference']['component_id']}/model/"
                        f"{collision['model_instance_id']}/body/"
                        f"{collision['reference']['collision_body_id']}/collision/{index}"
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
    value["node_policies"] = [_node_policy()]
    _rehash(value)
    return value


def _robot_tcp_fixture(*, duplicate_display_identity: bool = False) -> dict[str, Any]:
    value = _native_fixture()
    target = value["target_bindings"][0]
    target.pop("default_tcp", None)
    frame = copy.deepcopy(value["frame_bindings"][0])
    entry = {
        "target": copy.deepcopy(target["target"]),
        "frame": copy.deepcopy(frame["reference"]),
    }
    row = {
        "entry": entry,
        "frame": frame,
        "contact_body_role_ids": [],
        "end_effector": False,
        "component_default": False,
    }
    rows = [row]
    if duplicate_display_identity:
        # B-dep-1: two targets of one component can each declare `tcp`.
        # Their entry triples differ even when the display role and resolved
        # compiled frame name are the same.
        second = copy.deepcopy(row)
        second["entry"]["target"]["command_target_id"] += "_secondary"
        rows.append(second)
    target["robot_tcps"] = rows
    _rehash(value)
    return value


def _node_policy(*, instance_id: str = "root") -> dict[str, Any]:
    return {
        "instance_id": instance_id,
        "recovery_policy": {"allow_intervention": True, "max_retries": 1},
        "operator_stop_policy": "hold",
    }


def _rows(value: dict[str, Any]) -> tuple[dict[str, Any], ...]:
    component = value["component_bindings"][0]
    target = value["target_bindings"][0]
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
    (".component_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[0])),
    (".model_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[0]["model_bindings"][0])),
    (".target_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1])),
    (".frame_bindings[0]: expected closed record", _unknown(lambda value: value["frame_bindings"][0])),
    (".resource_role_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1]["resource_role_bindings"][0])),
    (".collision_body_bindings[0]: expected closed record", _unknown(lambda value: value["collision_body_bindings"][0])),
    (".capability_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[1]["capability_bindings"][0])),
    (".control_binding: expected closed record", _invalid_ref("control_binding", "control_binding_id", "control_binding_revision")),
    (".capability_provider_binding: expected closed record", _invalid_ref("capability_provider_binding", "capability_provider_binding_id", "capability_provider_binding_revision")),
    (".command_schema: expected closed record", _invalid_ref("command_schema", "command_schema_id", "command_schema_revision")),
    (".node_policies[0]: expected closed record", _unknown(lambda value: value["node_policies"][0])),
    (".node_policies[0]: expected closed record", lambda value: value["node_policies"][0].pop("recovery_policy")),
    (".node_policies[0]: expected closed record", lambda value: value["node_policies"][0].__setitem__("modification_policy", {"runtime_override": "forbidden", "persistent_edit": "forbidden", "intervention_delegate": "forbidden"})),
    (".node_policies[0].recovery_policy: expected closed record", _unknown(lambda value: value["node_policies"][0]["recovery_policy"])),
    (".contact_grasp_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[2])),
    (".allowed_body_bindings[0]: expected closed record", _unknown(lambda value: _rows(value)[2]["allowed_body_bindings"][0])),
    (".provenance: expected closed record", _unknown(lambda value: value["provenance"])),
    (".source_records[0]: expected closed record", _unknown(lambda value: value["provenance"]["source_records"][0])),
)


def _case(name: str) -> Mutation:
    def mutate(value: dict[str, Any]) -> None:
        component, target, contact = _rows(value)
        resource = target["resource_role_bindings"][0]
        collision = value["collision_body_bindings"][0]
        capability = target["capability_bindings"][0]
        body = contact["allowed_body_bindings"][0]
        provenance = value["provenance"]["source_records"]
        policy = value["node_policies"][0]
        recovery = policy["recovery_policy"]
        if name == "null":
            value["program_id"] = None
        elif name == "wrong_type":
            value["target_bindings"] = {}
        elif name == "identity_pattern":
            value["program_id"] = "not portable"
        elif name == "identity_bytes":
            value["program_id"] = "a" * 129
        elif name == "text_bytes":
            value["frame_bindings"][0]["compiled_frame_name"] = "é" * 2049
        elif name == "invalid_utf8":
            value["frame_bindings"][0]["compiled_frame_name"] = "\ud800"
        elif name == "root_maximum":
            value["component_bindings"] = [component] * 4097
        elif name == "nested_maximum":
            value["frame_bindings"] = [value["frame_bindings"][0]] * 4097
        elif name == "provenance_maximum":
            value["provenance"]["source_records"] = [provenance[0]] * 16385
        elif name == "component_order":
            value["component_bindings"] = [component, copy.deepcopy(component)]
        elif name == "model_order":
            component["model_bindings"].append(copy.deepcopy(component["model_bindings"][0]))
        elif name == "target_order":
            value["target_bindings"].append(copy.deepcopy(target))
        elif name == "frame_order":
            value["frame_bindings"].append(copy.deepcopy(value["frame_bindings"][0]))
        elif name == "resource_order":
            target["resource_role_bindings"].append(copy.deepcopy(resource))
        elif name == "collision_order":
            value["collision_body_bindings"].append(copy.deepcopy(collision))
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
            target["target"]["component_id"] = "mismatch"
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
            body_target = next(row for row in value["target_bindings"]
                               if row["collision_body_references"])
            body_target["collision_body_references"][0]["collision_body_id"] = "mismatch"
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
        elif name == "node_policies_missing":
            del value["node_policies"]
        elif name == "node_policies_wrong_type":
            value["node_policies"] = {}
        elif name == "node_policy_null":
            value["node_policies"] = [None]
        elif name == "recovery_null":
            policy["recovery_policy"] = None
        elif name == "recovery_empty":
            policy["recovery_policy"] = {}
        elif name == "recovery_partial":
            del recovery["max_retries"]
        elif name == "recovery_removed_field":
            recovery["allowed_recovery_actions"] = ["intervention"]
        elif name == "stop_enum":
            policy["operator_stop_policy"] = "pause"
        elif name == "stop_null":
            policy["operator_stop_policy"] = None
        elif name == "stop_wrong_type":
            policy["operator_stop_policy"] = 1
        elif name == "allow_intervention_type":
            recovery["allow_intervention"] = 0
        elif name == "retry_bool":
            recovery["max_retries"] = True
        elif name == "retry_low":
            recovery["max_retries"] = -1
        elif name == "retry_high":
            recovery["max_retries"] = 256
        elif name == "instance_upper":
            policy["instance_id"] = "Root"
        elif name == "instance_bytes":
            policy["instance_id"] = "a" * 129
        elif name == "table_duplicate":
            value["node_policies"].append(copy.deepcopy(policy))
        elif name == "table_unsorted":
            value["node_policies"].append(_node_policy(instance_id="a"))
        elif name == "table_bound":
            value["node_policies"] = [policy] * 100001
        elif name == "default_row":
            value["node_policies"] = [{
                "instance_id": "root",
                "recovery_policy": {"allow_intervention": False, "max_retries": 0},
                "operator_stop_policy": "inherit",
            }]
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
    ("target_component", "target has no component binding"),
    ("contact_penetration", "numeric ranges"),
    ("contact_fraction", "numeric ranges"),
    ("contact_bool", "finite number"),
    ("contact_nonfinite", "finite number"),
    ("body_minimum", "bounded array"),
    ("body_order", "canonical sorted unique"),
    ("generated_body_id", "generated scope"),
    ("body_target", "references must be sorted unique and resolve"),
    ("body_collision", "collision binding"),
    ("body_model", "model binding"),
    ("provenance_kind", "CatalogRecordKind"),
    ("provenance_kind_list", "CatalogRecordKind"),
    ("provenance_kind_object", "CatalogRecordKind"),
    ("provenance_order", "canonical sorted unique"),
    ("node_policies_missing", "expected closed record"),
    ("node_policies_wrong_type", "expected bounded array"),
    ("node_policy_null", "expected closed record"),
    ("recovery_null", "expected closed record"),
    ("recovery_empty", "expected closed record"),
    ("recovery_partial", "expected closed record"),
    ("recovery_removed_field", "expected closed record"),
    ("stop_enum", "closed enum value"),
    ("stop_null", "closed enum value"),
    ("stop_wrong_type", "closed enum value"),
    ("allow_intervention_type", "boolean"),
    ("retry_bool", "integer in bounded range"),
    ("retry_low", "integer in bounded range"),
    ("retry_high", "integer in bounded range"),
    ("instance_upper", "portable lower-case ID"),
    ("instance_bytes", "bounded non-empty string"),
    ("table_duplicate", "canonical sorted unique"),
    ("table_unsorted", "canonical sorted unique"),
    ("table_bound", "expected bounded array"),
    ("default_row", "default node-policy rows must be omitted"),
)


def test_native_binding_fixture_is_accepted_without_copy() -> None:
    document = _native_fixture()
    assert validate_program_binding(document) is document


def test_robot_tcp_rows_preserve_list_without_a_default() -> None:
    document = _robot_tcp_fixture()
    target = document["target_bindings"][0]
    assert "default_tcp" not in target
    assert target["robot_tcps"][0]["end_effector"] is False
    assert validate_program_binding(document) is document


def test_robot_tcp_presence_distinguishes_empty_from_absent() -> None:
    document = _native_fixture()
    target = document["target_bindings"][0]
    target.pop("robot_tcps", None)
    _rehash(document)
    assert validate_program_binding(document) is document
    assert "robot_tcps" not in target
    target["robot_tcps"] = []
    _rehash(document)
    assert validate_program_binding(document) is document
    assert target["robot_tcps"] == []
    target["robot_tcps"] = None
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(document)


def test_robot_tcp_identity_is_the_full_entry_triple() -> None:
    document = _robot_tcp_fixture(duplicate_display_identity=True)
    rows = document["target_bindings"][0]["robot_tcps"]
    assert rows[0]["entry"]["target"]["component_id"] == rows[1]["entry"]["target"]["component_id"]
    assert rows[0]["entry"]["frame"] == rows[1]["entry"]["frame"]
    assert rows[0]["frame"]["compiled_frame_name"] == rows[1]["frame"]["compiled_frame_name"]
    assert rows[0]["entry"]["target"]["command_target_id"] != rows[1]["entry"]["target"]["command_target_id"]
    assert validate_program_binding(document) is document


@pytest.mark.parametrize(
    "mutation",
    [
        lambda row: row["frame"]["reference"].__setitem__("frame_id", "another_frame"),
        lambda row: row.__setitem__("end_effector", 1),
    ],
)
def test_robot_tcp_rows_reject_malformed_frame_and_marks(mutation) -> None:
    document = _robot_tcp_fixture()
    mutation(document["target_bindings"][0]["robot_tcps"][0])
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(document)


def test_robot_tcp_rows_reject_duplicate_entry_triples() -> None:
    document = _robot_tcp_fixture()
    rows = document["target_bindings"][0]["robot_tcps"]
    rows.append(copy.deepcopy(rows[0]))
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError, match="TCP entries must be sorted unique"):
        validate_program_binding(document)


def test_generated_parser_accepts_a_nonempty_sparse_node_policy() -> None:
    document = _fixture()
    assert validate_program_binding(document) is document
    assert document["node_policies"][0]["instance_id"] == "root"


def test_intervention_scope_omission_preserves_exact_transport_identity() -> None:
    document = _fixture()
    before = copy.deepcopy(document)
    assert validate_program_binding(document) is document
    assert document == before
    assert "intervention_stop_scope" not in document["node_policies"][0]["recovery_policy"]


@pytest.mark.parametrize("scope", ["all", "own_groups"])
def test_intervention_scope_is_closed_and_preserved_without_repair(scope: str) -> None:
    document = _fixture()
    document["node_policies"][0]["recovery_policy"]["intervention_stop_scope"] = scope
    _rehash(document)
    before = copy.deepcopy(document)
    assert validate_program_binding(document) is document
    assert document == before


def test_own_group_scope_alone_is_a_nondefault_node_policy() -> None:
    document = _fixture()
    document["node_policies"] = [{
        "instance_id": "root",
        "recovery_policy": {
            "allow_intervention": False,
            "max_retries": 0,
            "intervention_stop_scope": "own_groups",
        },
        "operator_stop_policy": "inherit",
    }]
    _rehash(document)
    assert validate_program_binding(document) is document


def test_explicit_all_scope_does_not_make_a_default_policy_nondefault() -> None:
    document = _fixture()
    document["node_policies"] = [{
        "instance_id": "root",
        "recovery_policy": {
            "allow_intervention": False,
            "max_retries": 0,
            "intervention_stop_scope": "all",
        },
        "operator_stop_policy": "inherit",
    }]
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError, match="default node-policy rows must be omitted"):
        validate_program_binding(document)


@pytest.mark.parametrize("scope", [None, True, 0, [], {}, "own_group", "selected"])
def test_intervention_scope_rejects_malformed_or_unknown_values(scope: object) -> None:
    document = _fixture()
    document["node_policies"][0]["recovery_policy"]["intervention_stop_scope"] = scope
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError, match="intervention_stop_scope"):
        validate_program_binding(document)


def test_intervention_scope_change_requires_a_fresh_content_identity() -> None:
    document = _fixture()
    document["node_policies"][0]["recovery_policy"]["intervention_stop_scope"] = "own_groups"
    with pytest.raises(ProgramBindingTransportError, match="content identity"):
        validate_program_binding(document)


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
    if name not in {"invalid_utf8", "contact_nonfinite", "table_bound"}:
        _rehash(document)
    with pytest.raises(ProgramBindingTransportError, match=re.escape(expected)):
        validate_program_binding(document)


def test_logic_only_binding_accepts_empty_component_and_target_tables() -> None:
    # A zero-component Program binds against the empty model map.
    document = _fixture()
    document["component_bindings"] = []
    document["target_bindings"] = []
    document["frame_bindings"] = []
    document["collision_body_bindings"] = []
    document["signal_bindings"] = []
    document["contact_grasp_bindings"] = []
    _rehash(document)
    assert validate_program_binding(document) is document


def test_binding_transport_still_requires_the_component_table() -> None:
    document = _fixture()
    del document["component_bindings"]
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError):
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



def _signal_binding_fixture() -> dict[str, Any]:
    document = _native_fixture()
    target = document["target_bindings"][0]
    reference = {"component_id": target["target"]["component_id"], "signal_name": "fixture_ready"}
    document["signal_bindings"].append({
        "reference": reference,
        "target": copy.deepcopy(target["target"]),
        "runtime_unit_id": "fixture_runtime",
        "value_type": "bool",
        "direction": "input",
        "transition_capability": "sampled_only",
        "signal_descriptor_digest": "sha256:" + "a" * 64,
    })
    document["signal_bindings"].sort(key=lambda row: (row["reference"]["component_id"], row["reference"]["signal_name"]))
    target["signal_references"].append(copy.deepcopy(reference))
    target["signal_references"].sort(key=lambda row: (row["component_id"], row["signal_name"]))
    _rehash(document)
    return document


def test_signal_binding_closed_row_preserves_native_document() -> None:
    ordinary = _native_fixture()
    assert validate_program_binding(ordinary) is ordinary
    document = _signal_binding_fixture()
    assert validate_program_binding(document) is document
    assert document["program_binding_id"] != ordinary["program_binding_id"]


@pytest.mark.parametrize("mutation", [
    lambda document, row: document["signal_bindings"].remove(row),
    lambda document, row: row.__setitem__("unknown", True),
    lambda document, row: row.__setitem__("value_type", True),
    lambda document, row: row.__setitem__("direction", "bidirectional"),
    lambda document, row: row.__setitem__("transition_capability", "inferred"),
    lambda document, row: row.__setitem__("signal_descriptor_digest", "sha256:" + "A" * 64),
    lambda document, row: document["signal_bindings"].append(copy.deepcopy(row)),
    lambda document, row: row["target"].__setitem__("command_target_id", "other_target"),
])
def test_signal_binding_shape_and_target_correspondence_fail_closed(mutation) -> None:
    document = _signal_binding_fixture()
    row = next(row for row in document["signal_bindings"]
               if row["reference"]["signal_name"] == "fixture_ready")
    mutation(document, row)
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(document)


def _pneumatic_binding_fixture() -> dict[str, Any]:
    document = _native_fixture()
    gripper = next(row for row in document["target_bindings"]
                   if any(cap["capability_id"] == "tool.gripper.command"
                          for cap in row["capability_bindings"]))
    component = gripper["target"]["component_id"]
    references = {}
    for kind in ("input", "output"):
        target = copy.deepcopy(gripper)
        target["target"] = {"component_id": component, "command_target_id": f"fixture_{kind}"}
        target["frame_references"] = []
        target["frame_associations"] = []
        target["resource_role_bindings"] = []
        target["collision_body_references"] = []
        for field in ("robot_tcps", "default_tcp", "pneumatic_io"):
            target.pop(field, None)
        capability = copy.deepcopy(gripper["capability_bindings"][0])
        capability["capability_id"] = "io.gpio.get_input" if kind == "input" else "io.gpio.set_output"
        target["capability_bindings"] = [capability]
        reference = {"component_id": component, "signal_name": f"fixture_{kind}"}
        references[kind] = reference
        target["signal_references"] = [copy.deepcopy(reference)]
        document["signal_bindings"].append({
            "reference": copy.deepcopy(reference), "target": copy.deepcopy(target["target"]),
            "runtime_unit_id": "fixture_runtime", "value_type": "bool",
            "direction": kind, "transition_capability": "sampled_only",
            "signal_descriptor_digest": "sha256:" + "a" * 64,
        })
        document["target_bindings"].append(target)
    gripper["pneumatic_io"] = {
        "outputs": [{"signal": references["output"],
                     "open_value": True, "close_value": False}],
        "inputs": [{"input": "workpiece_grasped", "signal": references["input"]}],
    }
    document["target_bindings"].sort(key=lambda row: (row["target"]["component_id"], row["target"]["command_target_id"]))
    document["signal_bindings"].sort(key=lambda row: (row["reference"]["component_id"], row["reference"]["signal_name"]))
    _rehash(document)
    return document


def test_pneumatic_binding_routes_exact_bound_signals() -> None:
    document = _pneumatic_binding_fixture()
    assert validate_program_binding(document) is document


@pytest.mark.parametrize("mutation", [
    lambda route: route["outputs"][0].__setitem__("signal", copy.deepcopy(route["inputs"][0]["signal"])),
    lambda route: route["outputs"][0].__setitem__("open_value", 1),
    lambda route: route["inputs"][0].__setitem__("input", "unknown"),
    lambda route: route["inputs"].append(copy.deepcopy(route["inputs"][0])),
])
def test_pneumatic_binding_rejects_wrong_signal_values_or_duplicate_inputs(mutation) -> None:
    document = _pneumatic_binding_fixture()
    gripper = next(row for row in document["target_bindings"] if "pneumatic_io" in row)
    mutation(gripper["pneumatic_io"])
    _rehash(document)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(document)


def _endpoint_fixture() -> dict[str, Any]:
    value = _fixture()
    body = value["collision_body_bindings"][0]
    # The target that carries the body (the fixture's targets are sorted).
    target = next(row for row in value["target_bindings"]
                  if body["reference"] in row["collision_body_references"])
    frame = next(frame for frame in value["frame_bindings"]
                 if frame["model_instance_id"] == body["model_instance_id"]
                 and frame["reference"]["component_id"] == body["reference"]["component_id"]
                 and frame["reference"] in target["frame_references"])
    association = next(row for row in target["frame_associations"] if row["frame"] == frame["reference"])
    association["contact_body_role_ids"] = [body["reference"]["collision_body_id"]]
    policy = value["contact_grasp_bindings"][0]
    policy["bodies_from_motion_endpoint"] = True
    policy["allowed_body_bindings"] = []
    _rehash(value)
    return value


def test_endpoint_binding_transport_preserves_exact_association() -> None:
    value = _endpoint_fixture()
    assert validate_program_binding(value) == value


@pytest.mark.parametrize("fault", ["missing_body", "wrong_model", "duplicate_body", "false_selector", "mixed_bodies"])
def test_endpoint_binding_transport_rejects_invalid_association(fault: str) -> None:
    value = _endpoint_fixture()
    association = next(row for target in value["target_bindings"]
                       for row in target["frame_associations"] if row["contact_body_role_ids"])
    frame = next(row for row in value["frame_bindings"] if row["reference"] == association["frame"])
    if fault == "missing_body":
        association["contact_body_role_ids"] = ["absent"]
    elif fault == "wrong_model":
        frame["model_revision"] = "sha256:" + "f" * 64
    elif fault == "duplicate_body":
        association["contact_body_role_ids"] *= 2
    elif fault == "false_selector":
        value["contact_grasp_bindings"][0]["bodies_from_motion_endpoint"] = False
    else:
        value["contact_grasp_bindings"][0]["allowed_body_bindings"] = _fixture()["contact_grasp_bindings"][0]["allowed_body_bindings"]
    _rehash(value)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(value)


def _release_fixture(*, binary=False, endpoint=False):
    value = _fixture()
    policy = value["contact_grasp_bindings"][0]
    policy.pop("max_penetration_m")
    policy.pop("terminal_window_start_fraction")
    policy.update(
        mode="contact_release",
        release_evidence_mode="binary_contact" if binary else "signed_penetration",
        enforce_max_start_penetration=not binary,
        max_start_penetration_m=0.0 if binary else 0.002,
        min_release_progress_m=0.0,
        release_tolerance_m=0.0 if binary else 1e-5,
        require_strictly_decreasing_penetration=not binary,
        require_separation_by_end=True,
        allow_recontact=False,
    )
    if endpoint:
        policy["bodies_from_motion_endpoint"] = True
        policy["allowed_body_bindings"] = []
    _rehash(value)
    return value


@pytest.mark.parametrize("binary", [False, True])
@pytest.mark.parametrize("endpoint", [False, True])
def test_contact_release_binding_uses_one_closed_mode_specific_envelope(binary, endpoint):
    value = _release_fixture(binary=binary, endpoint=endpoint)
    assert validate_program_binding(value) == value


@pytest.mark.parametrize("field,value", [
    ("mode", "contact_grasp"), ("mode", None),
    ("release_evidence_mode", "distance"), ("enforce_max_start_penetration", 1),
    ("require_separation_by_end", None), ("max_start_penetration_m", -0.001),
    ("min_release_progress_m", 1000.001), ("release_tolerance_m", True),
    ("max_penetration_m", 0.002), ("terminal_window_start_fraction", 0.5),
])
def test_contact_release_binding_rejects_crossed_or_invalid_fields(field, value):
    binding = _release_fixture(endpoint=True)
    binding["contact_grasp_bindings"][0][field] = value
    _rehash(binding)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(binding)


@pytest.mark.parametrize("field", [
    "mode", "release_evidence_mode", "enforce_max_start_penetration",
    "max_start_penetration_m", "min_release_progress_m", "release_tolerance_m",
    "require_strictly_decreasing_penetration", "require_separation_by_end", "allow_recontact",
])
def test_contact_release_binding_requires_every_normalized_release_field(field):
    binding = _release_fixture()
    binding["contact_grasp_bindings"][0].pop(field)
    _rehash(binding)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(binding)


@pytest.mark.parametrize("field,value", [
    ("enforce_max_start_penetration", True), ("require_strictly_decreasing_penetration", True),
    ("max_start_penetration_m", 0.002), ("min_release_progress_m", 0.001),
    ("release_tolerance_m", 1e-5),
])
def test_binary_release_binding_requires_neutral_metric_fields(field, value):
    binding = _release_fixture(binary=True)
    binding["contact_grasp_bindings"][0][field] = value
    _rehash(binding)
    with pytest.raises(ProgramBindingTransportError):
        validate_program_binding(binding)
