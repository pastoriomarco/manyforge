"""Schema-only v3 policy rejection checks; these inputs are not valid bindings."""

import copy

import pytest

from manyforge_headless_api.program_binding_transport import (
    ProgramBindingTransportError,
    validate_program_binding,
)


def partial_binding(policy):
    # The deliberately incomplete provenance and placeholder identity are never
    # presented as a native artifact; each case must fail at node_policies first.
    digest = "sha256:" + "0" * 64
    return {
        "api_version": "manyforge.program_binding.v3", "program_binding_id": digest,
        "program_id": "program", "program_revision": digest, "retained_bundle_id": digest,
        "graph_revision": digest,
        "node_environment": {"catalog_id": "catalog", "content_version": "1",
                             "catalog_digest": digest, "environment_digest": digest},
        "component_bindings": [], "target_bindings": [], "frame_bindings": [],
        "collision_body_bindings": [], "signal_bindings": [], "node_policies": [policy],
        "contact_grasp_bindings": [], "provenance": {},
    }


@pytest.mark.parametrize("field,value", [
    ("instance_id", "Root"), ("instance_id", "a" * 129),
    ("operator_stop_policy", "pause"), ("operator_stop_policy", None),
    ("operator_stop_policy", 1), ("recovery_policy", None), ("recovery_policy", {}),
])
def test_invalid_policy_field(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    policy[field] = value
    with pytest.raises(ProgramBindingTransportError, match="node_policies"):
        validate_program_binding(partial_binding(policy))


@pytest.mark.parametrize("field,value", [
    ("allow_intervention", 0), ("max_retries", True), ("max_retries", -1),
    ("max_retries", 256), ("intervention_stop_scope", None),
    ("intervention_stop_scope", True), ("intervention_stop_scope", 0),
    ("intervention_stop_scope", []), ("intervention_stop_scope", {}),
    ("intervention_stop_scope", "own_group"), ("intervention_stop_scope", "selected"),
    ("allowed_recovery_actions", ["intervention"]),
])
def test_invalid_recovery_field(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    policy["recovery_policy"][field] = copy.deepcopy(value)
    with pytest.raises(ProgramBindingTransportError, match="node_policies"):
        validate_program_binding(partial_binding(policy))


@pytest.mark.parametrize("scope", [None, "all"])
def test_default_policy_must_be_omitted(scope):
    recovery = {"allow_intervention": False, "max_retries": 0}
    if scope is not None:
        recovery["intervention_stop_scope"] = scope
    policy = {"instance_id": "root", "recovery_policy": recovery, "operator_stop_policy": "inherit"}
    with pytest.raises(ProgramBindingTransportError, match="default node-policy rows must be omitted"):
        validate_program_binding(partial_binding(policy))


@pytest.mark.parametrize("field,value", [
    ("program_id", None), ("program_id", "not portable"), ("program_id", "a" * 129),
    ("component_bindings", {}), ("component_bindings", [{}]),
    ("target_bindings", {}), ("target_bindings", [{}]),
    ("frame_bindings", [{}]), ("collision_body_bindings", [{}]), ("signal_bindings", [{}]),
    ("node_policies", {}), ("node_policies", [None]),
])
def test_invalid_root_fields_are_rejected_at_their_actual_path(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    binding[field] = copy.deepcopy(value)
    with pytest.raises(ProgramBindingTransportError, match=field):
        validate_program_binding(binding)


@pytest.mark.parametrize("ordering", ["duplicate", "unsorted"])
def test_policy_rows_are_sorted_and_unique(ordering):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    second = copy.deepcopy(policy)
    if ordering == "unsorted":
        second["instance_id"] = "a"
    binding["node_policies"].append(second)
    with pytest.raises(ProgramBindingTransportError, match="node_policies.*sorted unique"):
        validate_program_binding(binding)


@pytest.mark.parametrize("field,value", [
    ("max_penetration_m", 0), ("max_penetration_m", True),
    ("max_penetration_m", float("nan")), ("terminal_window_start_fraction", 1.01),
    ("bodies_from_motion_endpoint", False), ("unknown", True),
])
def test_invalid_contact_policy_fields(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    contact = {"contact_policy_id": "policy", "contact_policy_revision": "sha256:" + "0" * 64,
               "runtime_object_id": "object", "allowed_body_bindings": [],
               "bodies_from_motion_endpoint": True, "max_penetration_m": 0.001,
               "terminal_window_start_fraction": 0.8}
    contact[field] = value
    binding["contact_grasp_bindings"] = [contact]
    with pytest.raises(ProgramBindingTransportError, match="contact_grasp_bindings"):
        validate_program_binding(binding)


@pytest.mark.parametrize("field,value", [
    ("mode", "contact_grasp"), ("release_evidence_mode", "distance"),
    ("enforce_max_start_penetration", 1), ("max_start_penetration_m", -0.001),
    ("min_release_progress_m", 1000.001), ("release_tolerance_m", True),
    ("require_separation_by_end", None), ("max_penetration_m", 0.002),
    ("terminal_window_start_fraction", 0.5),
])
def test_invalid_contact_release_envelope(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    release = {"contact_policy_id": "policy", "contact_policy_revision": "sha256:" + "0" * 64,
               "runtime_object_id": "object", "allowed_body_bindings": [],
               "bodies_from_motion_endpoint": True, "mode": "contact_release",
               "release_evidence_mode": "signed_penetration", "enforce_max_start_penetration": True,
               "max_start_penetration_m": 0.002, "min_release_progress_m": 0.0,
               "release_tolerance_m": 0.00001, "require_strictly_decreasing_penetration": True,
               "require_separation_by_end": True, "allow_recontact": False}
    release[field] = value
    binding["contact_grasp_bindings"] = [release]
    with pytest.raises(ProgramBindingTransportError, match="contact_grasp_bindings"):
        validate_program_binding(binding)


@pytest.mark.parametrize("kind", ["unknown_kind", [], {}])
def test_unrecognized_provenance_kind_is_rejected(kind):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    binding["provenance"] = {
        "binding_compiler_contract_digest": "sha256:" + "0" * 64,
        "source_records": [{"record_kind": kind, "record_id": "record", "revision": "sha256:" + "0" * 64}],
        "resolved_output_records": [],
    }
    with pytest.raises(ProgramBindingTransportError, match="provenance.source_records.*record_kind"):
        validate_program_binding(binding)


@pytest.mark.parametrize("collection,id_field", [
    ("frame_bindings", "frame_id"), ("collision_body_bindings", "collision_body_id"),
])
@pytest.mark.parametrize("fault", ["missing_component", "unknown_field", "invalid_id"])
def test_geometry_reference_rejects_malformed_direct_identity(collection, id_field, fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    reference = {"component_id": "station", id_field: "item"}
    if fault == "missing_component":
        del reference["component_id"]
    elif fault == "unknown_field":
        reference["unknown"] = True
    else:
        reference[id_field] = "invalid id"
    geometry = {"reference": reference, "model_instance_id": "model", "model_revision": "sha256:" + "0" * 64}
    if collection == "frame_bindings":
        geometry["compiled_frame_name"] = "frame"
    else:
        geometry["compiled_collision_body_names"] = ["body"]
    binding[collection] = [geometry]
    with pytest.raises(ProgramBindingTransportError, match=collection + r".*reference"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["missing_component", "unknown_field", "invalid_id"])
def test_target_reference_rejects_malformed_direct_identity(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    reference = {"component_id": "station", "command_target_id": "io"}
    if fault == "missing_component":
        del reference["component_id"]
    elif fault == "unknown_field":
        reference["unknown"] = True
    else:
        reference["command_target_id"] = "invalid id"
    binding["target_bindings"] = [{"target": reference, "frame_references": [], "frame_associations": [],
                                   "collision_body_references": [], "signal_references": [],
                                   "resource_role_bindings": [], "capability_bindings": []}]
    with pytest.raises(ProgramBindingTransportError, match=r"target_bindings\[0\]\.target"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["missing_component", "unknown_field", "invalid_id"])
def test_signal_reference_rejects_malformed_direct_identity(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    reference = {"component_id": "station", "signal_name": "ready"}
    if fault == "missing_component":
        del reference["component_id"]
    elif fault == "unknown_field":
        reference["unknown"] = True
    else:
        reference["signal_name"] = "invalid\x00name"
    binding["signal_bindings"] = [{"reference": reference,
        "target": {"component_id": "station", "command_target_id": "io"},
        "runtime_unit_id": "station", "value_type": "bool", "direction": "input",
        "transition_capability": "sampled_only", "signal_descriptor_digest": "sha256:" + "0" * 64}]
    with pytest.raises(ProgramBindingTransportError, match=r"signal_bindings\[0\]\.reference"):
        validate_program_binding(binding)


@pytest.mark.parametrize("field,value", [
    ("value_type", True), ("direction", "bidirectional"),
    ("transition_capability", "inferred"), ("signal_descriptor_digest", "sha256:" + "A" * 64),
    ("unknown", True),
])
def test_signal_binding_rejects_malformed_contract(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    signal = {"reference": {"component_id": "station", "signal_name": "ready"},
              "target": {"component_id": "station", "command_target_id": "io"},
              "runtime_unit_id": "station", "value_type": "bool", "direction": "input",
              "transition_capability": "sampled_only", "signal_descriptor_digest": "sha256:" + "0" * 64}
    signal[field] = value
    binding["signal_bindings"] = [signal]
    with pytest.raises(ProgramBindingTransportError, match=r"signal_bindings\[0\]"):
        validate_program_binding(binding)


@pytest.mark.parametrize("field,value", [
    ("enforce_max_start_penetration", True), ("require_strictly_decreasing_penetration", True),
    ("max_start_penetration_m", 0.002), ("min_release_progress_m", 0.001), ("release_tolerance_m", 0.00001),
])
def test_binary_contact_release_requires_neutral_metric_fields(field, value):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    release = {"contact_policy_id": "policy", "contact_policy_revision": "sha256:" + "0" * 64,
               "runtime_object_id": "object", "allowed_body_bindings": [], "bodies_from_motion_endpoint": True,
               "mode": "contact_release", "release_evidence_mode": "binary_contact",
               "enforce_max_start_penetration": False, "max_start_penetration_m": 0,
               "min_release_progress_m": 0, "release_tolerance_m": 0,
               "require_strictly_decreasing_penetration": False, "require_separation_by_end": True,
               "allow_recontact": False}
    release[field] = value
    binding["contact_grasp_bindings"] = [release]
    with pytest.raises(ProgramBindingTransportError, match=r"contact_grasp_bindings\[0\].*binary"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["missing_retry", "duplicate_component", "duplicate_model"])
def test_incomplete_recovery_or_duplicate_component_model_rows(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    if fault == "missing_retry":
        del policy["recovery_policy"]["max_retries"]
        expected = r"node_policies\[0\]\.recovery_policy"
    else:
        digest = "sha256:" + "0" * 64
        component = {"component_id": "station", "component_type_id": "type", "component_type_revision": digest,
                     "model_bindings": [{"model_role_id": "model", "model_instance_id": "model", "model_revision": digest}]}
        if fault == "duplicate_model":
            component["model_bindings"].append(copy.deepcopy(component["model_bindings"][0]))
            expected = r"component_bindings\[0\]\.model_bindings"
        else:
            expected = r"component_bindings"
        binding["component_bindings"] = [component] if fault == "duplicate_model" else [component, copy.deepcopy(component)]
    with pytest.raises(ProgramBindingTransportError, match=expected):
        validate_program_binding(binding)


@pytest.mark.parametrize("field", ["mode", "release_evidence_mode", "enforce_max_start_penetration",
    "max_start_penetration_m", "min_release_progress_m", "release_tolerance_m",
    "require_strictly_decreasing_penetration", "require_separation_by_end", "allow_recontact"])
def test_contact_release_requires_every_normalized_field(field):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    release = {"contact_policy_id": "policy", "contact_policy_revision": "sha256:" + "0" * 64,
               "runtime_object_id": "object", "allowed_body_bindings": [], "bodies_from_motion_endpoint": True,
               "mode": "contact_release", "release_evidence_mode": "binary_contact",
               "enforce_max_start_penetration": False, "max_start_penetration_m": 0,
               "min_release_progress_m": 0, "release_tolerance_m": 0,
               "require_strictly_decreasing_penetration": False, "require_separation_by_end": True,
               "allow_recontact": False}
    del release[field]
    binding["contact_grasp_bindings"] = [release]
    with pytest.raises(ProgramBindingTransportError, match=r"contact_grasp_bindings\[0\]"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["vector_size", "resource_id_duplicate", "resource_name_duplicate", "unknown_field"])
def test_target_resource_rejects_malformed_correspondence(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    digest = "sha256:" + "0" * 64
    binding["component_bindings"] = [{"component_id": "station", "component_type_id": "type",
                                      "component_type_revision": digest, "model_bindings": []}]
    resource = {"resource_role_id": "role", "resource_ids": ["resource"], "compiled_resource_names": ["name"]}
    if fault == "vector_size":
        resource["compiled_resource_names"].append("extra")
    elif fault == "resource_id_duplicate":
        resource["resource_ids"].append("resource")
        resource["compiled_resource_names"].append("extra")
    elif fault == "resource_name_duplicate":
        resource["resource_ids"].append("extra")
        resource["compiled_resource_names"].append("name")
    else:
        resource["unknown"] = True
    binding["target_bindings"] = [{"target": {"component_id": "station", "command_target_id": "io"},
        "frame_references": [], "frame_associations": [], "collision_body_references": [],
        "signal_references": [], "resource_role_bindings": [resource], "capability_bindings": []}]
    with pytest.raises(ProgramBindingTransportError, match=r"target_bindings\[0\]\.resource_role_bindings"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["no_provider", "unknown_field", "control_extra", "provider_extra", "schema_extra"])
def test_target_capability_rejects_malformed_resolution(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    digest = "sha256:" + "0" * 64
    binding["component_bindings"] = [{"component_id": "station", "component_type_id": "type",
                                      "component_type_revision": digest, "model_bindings": []}]
    capability = {"capability_id": "cap", "capability_definition_revision": digest,
                  "control_binding": {"control_binding_id": "control", "control_binding_revision": digest},
                  "capability_provider_binding": None, "command_schema": None}
    if fault == "no_provider":
        capability["control_binding"] = None
    elif fault == "unknown_field":
        capability["unknown"] = True
    elif fault == "control_extra":
        capability["control_binding"]["unknown"] = True
    elif fault == "provider_extra":
        capability["capability_provider_binding"] = {"capability_provider_binding_id": "provider",
            "capability_provider_binding_revision": digest, "unknown": True}
    else:
        capability["command_schema"] = {"command_schema_id": "schema", "command_schema_revision": digest, "unknown": True}
    binding["target_bindings"] = [{"target": {"component_id": "station", "command_target_id": "io"},
        "frame_references": [], "frame_associations": [], "collision_body_references": [],
        "signal_references": [], "resource_role_bindings": [], "capability_bindings": [capability]}]
    with pytest.raises(ProgramBindingTransportError, match=r"target_bindings\[0\]\.capability_bindings"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["route_extra", "missing_capability", "empty_outputs", "bad_output_level"])
def test_pneumatic_route_rejects_malformed_shape(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    digest = "sha256:" + "0" * 64
    binding["component_bindings"] = [{"component_id": "station", "component_type_id": "type",
                                      "component_type_revision": digest, "model_bindings": []}]
    capability = {"capability_id": "tool.gripper.command", "capability_definition_revision": digest,
                  "control_binding": {"control_binding_id": "control", "control_binding_revision": digest},
                  "capability_provider_binding": None, "command_schema": None}
    ref = {"component_id": "station", "signal_name": "output"}
    route = {"outputs": [{"signal": ref, "open_value": True, "close_value": False}], "inputs": []}
    if fault == "route_extra":
        route["unknown"] = True
    elif fault == "empty_outputs":
        route["outputs"] = []
    elif fault == "bad_output_level":
        route["outputs"][0]["open_value"] = 1
    target = {"target": {"component_id": "station", "command_target_id": "io"},
              "frame_references": [], "frame_associations": [], "collision_body_references": [],
              "signal_references": [], "resource_role_bindings": [],
              "capability_bindings": [] if fault == "missing_capability" else [capability], "pneumatic_io": route}
    binding["target_bindings"] = [target]
    with pytest.raises(ProgramBindingTransportError, match=r"target_bindings\[0\]\.pneumatic_io"):
        validate_program_binding(binding)


@pytest.mark.parametrize("fault", ["entry_extra", "frame_extra", "frame_mismatch", "end_effector_type", "default_type"])
def test_tcp_row_rejects_malformed_direct_frame(fault):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    digest = "sha256:" + "0" * 64
    binding["component_bindings"] = [{"component_id": "station", "component_type_id": "type",
                                      "component_type_revision": digest, "model_bindings": []}]
    reference = {"component_id": "station", "frame_id": "tcp"}
    row = {"entry": {"target": {"component_id": "station", "command_target_id": "io"},
                      "frame": copy.deepcopy(reference)},
           "frame": {"reference": reference, "model_instance_id": "model", "model_revision": digest,
                     "compiled_frame_name": "tcp"},
           "contact_body_role_ids": [], "end_effector": False, "component_default": False}
    if fault == "entry_extra":
        row["entry"]["unknown"] = True
    elif fault == "frame_extra":
        row["frame"]["unknown"] = True
    elif fault == "frame_mismatch":
        row["frame"]["reference"]["frame_id"] = "other"
    elif fault == "end_effector_type":
        row["end_effector"] = 1
    else:
        row["component_default"] = 1
    binding["target_bindings"] = [{"target": {"component_id": "station", "command_target_id": "io"},
        "frame_references": [], "frame_associations": [], "collision_body_references": [],
        "signal_references": [], "resource_role_bindings": [], "capability_bindings": [], "robot_tcps": [row]}]
    with pytest.raises(ProgramBindingTransportError, match=r"target_bindings\[0\]\.robot_tcps\[0\]"):
        validate_program_binding(binding)


@pytest.mark.parametrize("collection", ["component_bindings", "target_bindings", "frame_bindings", "collision_body_bindings", "signal_bindings"])
def test_binding_table_enforces_bound_before_examining_rows(collection):
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    binding[collection] = [{}] * 4097
    with pytest.raises(ProgramBindingTransportError, match=collection + r".*bounded array"):
        validate_program_binding(binding)


def test_node_policy_table_enforces_bound_before_examining_rows():
    policy = {"instance_id": "root", "recovery_policy": {"allow_intervention": True, "max_retries": 1},
              "operator_stop_policy": "hold"}
    binding = partial_binding(policy)
    binding["node_policies"] = [policy] * 100001
    with pytest.raises(ProgramBindingTransportError, match=r"node_policies.*bounded array"):
        validate_program_binding(binding)
