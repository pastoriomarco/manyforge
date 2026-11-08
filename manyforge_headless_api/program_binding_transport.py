"""GENERATED FILE -- program_binding_transport.v3.json; DO NOT EDIT."""

from __future__ import annotations

from collections.abc import Mapping
import math
import re
from typing import Any, cast, TypeAlias

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    verify_content_identity,
)

ProgramBindingDocument: TypeAlias = dict[str, Any]
_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
_IDENTITY = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")
_KINDS = frozenset({
    "model_asset", "runtime_profile", "component_type",
    "control_interface_contract", "controller_topology", "provider_manifest",
    "command_schema", "capability_definition", "contact_policy",
    "stop_evidence_policy",
})
_MAX = 4096
_ROLE_MAX = 4096
_NODE_POLICY_MAX = 100000
_RETRY_MAX = 255
_PROVENANCE_MAX = 16384
_TEXT_MAX = 4096
_IDENTITY_MAX = 128
_BINDING_API_VERSION = "manyforge.program_binding.v3"
_FIELDS = {
    "binding": ("api_version", "program_binding_id", "program_id", "program_revision", "retained_bundle_id", "graph_revision", "node_environment", "component_bindings", "target_bindings", "frame_bindings", "collision_body_bindings", "signal_bindings", "node_policies", "contact_grasp_bindings", "provenance"),
    "node_environment": ("catalog_id", "content_version", "catalog_digest", "environment_digest"),
    "component": ("component_id", "component_type_id", "component_type_revision", "model_bindings"),
    "model": ("model_role_id", "model_instance_id", "model_revision"),
    "target": ("target", "frame_references", "frame_associations", "collision_body_references", "signal_references", "resource_role_bindings", "capability_bindings", "pneumatic_io", "robot_tcps", "default_tcp"),
    "frame": ("reference", "model_instance_id", "model_revision", "compiled_frame_name"),
    "tcp_entry": ("target", "frame"),
    "robot_tcp": ("entry", "frame", "contact_body_role_ids", "end_effector", "component_default"),
    "resource": ("resource_role_id", "resource_ids", "compiled_resource_names"),
    "collision": ("reference", "model_instance_id", "model_revision", "compiled_collision_body_names"),
    "capability": ("capability_id", "capability_definition_revision", "control_binding", "capability_provider_binding", "command_schema"),
    "node_policy": ("instance_id", "recovery_policy", "operator_stop_policy"),
    "recovery_policy": ("allow_intervention", "max_retries", "intervention_stop_scope"),
    "control": ("control_binding_id", "control_binding_revision"),
    "provider": ("capability_provider_binding_id", "capability_provider_binding_revision"),
    "command_schema": ("command_schema_id", "command_schema_revision"),
    "contact": ("contact_policy_id", "contact_policy_revision", "runtime_object_id", "allowed_body_bindings", "max_penetration_m", "terminal_window_start_fraction", "bodies_from_motion_endpoint"),
    "contact_body": ("body", "model_role_id", "model_instance_id", "model_revision", "collision_body_ids"),
    "provenance": ("binding_compiler_contract_digest", "source_records", "resolved_output_records"),
    "provenance_record": ("record_kind", "record_id", "revision"),
    "signal": ("reference", "target", "runtime_unit_id", "value_type", "direction", "transition_capability", "signal_descriptor_digest"),
    "pneumatic_io": ("outputs", "inputs"),
    "pneumatic_output": ("signal", "open_value", "close_value"),
    "pneumatic_input": ("input", "signal"),
    "contact_release": ("contact_policy_id", "contact_policy_revision", "runtime_object_id", "allowed_body_bindings", "mode", "release_evidence_mode", "enforce_max_start_penetration", "max_start_penetration_m", "min_release_progress_m", "release_tolerance_m", "require_strictly_decreasing_penetration", "require_separation_by_end", "allow_recontact", "bodies_from_motion_endpoint"),
    "target_reference": ("component_id", "command_target_id"),
    "frame_reference": ("component_id", "frame_id"),
    "body_reference": ("component_id", "collision_body_id"),
    "signal_reference": ("component_id", "signal_name"),
    "frame_association": ("frame", "contact_body_role_ids"),
}
_OPTIONAL_FIELDS = {'recovery_policy': ('intervention_stop_scope',), 'target': ('pneumatic_io', 'robot_tcps', 'default_tcp'), 'contact': ('bodies_from_motion_endpoint',), 'contact_release': ('bodies_from_motion_endpoint',)}
_STOP_POLICIES = frozenset(('inherit', 'hold', 'continue'))
_INTERVENTION_STOP_SCOPES = frozenset(('all', 'own_groups'))
_PROGRAM_ID = re.compile(r"^[a-z][a-z0-9_.-]*\Z")
_COLLISION_BODY_ID = "component/{controllable_component_id}/model/{model_instance_id}/body/{collision_body_role_id}/collision/{index}"


class ProgramBindingTransportError(ValueError):
    """One bounded error for a malformed Program-Binding transport value."""


def _fail(path: str, expected: str) -> None:
    raise ProgramBindingTransportError(f"{path}: {expected}")


def _record(
    value: object, keys: tuple[str, ...], path: str, *, optional: tuple[str, ...] = (),
) -> Mapping[str, Any]:
    allowed = set(keys)
    if not isinstance(value, dict) or not allowed.difference(optional) <= set(value) <= allowed:
        _fail(path, "expected closed record")
    return cast(ProgramBindingDocument, value)


def _array(value: object, path: str, *, minimum: int = 0, maximum: int = _MAX) -> list[Any]:
    if not isinstance(value, list) or not minimum <= len(value) <= maximum:
        _fail(path, "expected bounded array")
    return value


def _string(value: object, path: str, maximum: int = _TEXT_MAX) -> str:
    if not isinstance(value, str) or not value:
        _fail(path, "expected bounded non-empty string")
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError:
        _fail(path, "expected valid UTF-8 text")
    if size > maximum:
        _fail(path, "expected bounded non-empty string")
    return value


def _identity(value: object, path: str) -> str:
    text = _string(value, path, _IDENTITY_MAX)
    if _IDENTITY.fullmatch(text) is None:
        _fail(path, "expected portable identity")
    return text


def _digest(value: object, path: str) -> str:
    if not isinstance(value, str) or _DIGEST.fullmatch(value) is None:
        _fail(path, "expected lowercase SHA-256 identity")
    return value


def _number(value: object, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail(path, "expected finite number")
    result = float(value)
    if not math.isfinite(result):
        _fail(path, "expected finite number")
    return result


def _sorted_unique(rows: list[Mapping[str, Any]], key, path: str) -> None:
    keys = [key(row) for row in rows]
    if keys != sorted(keys) or len(keys) != len(set(keys)):
        _fail(path, "expected canonical sorted unique keys")


def _reference(value: object, kind: str, path: str) -> tuple[str, str]:
    """Validate a closed qualified identity without resolving or repairing it."""
    fields = {
        "target": "command_target_id", "frame": "frame_id",
        "body": "collision_body_id", "signal": "signal_name",
    }
    member = fields[kind]
    row = _record(value, _FIELDS[f"{kind}_reference"], path)
    component = _identity(row["component_id"], f"{path}.component_id")
    if kind == "signal":
        identity = _string(row[member], f"{path}.{member}", 512)
        if "\x00" in identity:
            _fail(f"{path}.{member}", "signal name must not contain NUL")
    else:
        identity = _identity(row[member], f"{path}.{member}")
    return component, identity


def _id_array(value: object, path: str, *, text: bool = False) -> list[str]:
    rows = _array(value, path, minimum=1)
    result = [
        _string(item, f"{path}[{index}]") if text else _identity(item, f"{path}[{index}]")
        for index, item in enumerate(rows)
    ]
    if len(result) != len(set(result)):
        _fail(path, "expected duplicate-free values")
    return result


def _policy_id(value: object, path: str) -> str:
    result = _string(value, path, _IDENTITY_MAX)
    if _PROGRAM_ID.fullmatch(result) is None:
        _fail(path, "expected portable lower-case ID")
    return result


def _node_policy(value: object, path: str) -> Mapping[str, Any]:
    row = _record(value, _FIELDS["node_policy"], path)
    _policy_id(row["instance_id"], f"{path}.instance_id")
    recovery = _record(
        row["recovery_policy"], _FIELDS["recovery_policy"],
        f"{path}.recovery_policy",
        optional=_OPTIONAL_FIELDS["recovery_policy"],
    )
    if type(recovery["allow_intervention"]) is not bool:
        _fail(f"{path}.recovery_policy.allow_intervention", "boolean")
    retries = recovery["max_retries"]
    if type(retries) is not int or not 0 <= retries <= _RETRY_MAX:
        _fail(f"{path}.recovery_policy.max_retries", "integer in bounded range")
    scope = recovery.get("intervention_stop_scope", "all")
    if type(scope) is not str or scope not in _INTERVENTION_STOP_SCOPES:
        _fail(f"{path}.recovery_policy.intervention_stop_scope", "closed enum value")
    if (
        type(row["operator_stop_policy"]) is not str
        or row["operator_stop_policy"] not in _STOP_POLICIES
    ):
        _fail(f"{path}.operator_stop_policy", "closed enum value")
    if (
        recovery["allow_intervention"] is False
        and retries == 0
        and scope == "all"
        and row["operator_stop_policy"] == "inherit"
    ):
        _fail(path, "default node-policy rows must be omitted")
    return row


def validate_program_binding(value: object, *, label: str = "program_binding") -> ProgramBindingDocument:
    """Validate without repair and return the original exact dict."""
    root = _record(value, _FIELDS["binding"], label)
    if root["api_version"] != _BINDING_API_VERSION:
        _fail(f"{label}.api_version", f"expected {_BINDING_API_VERSION}")
    _digest(root["program_binding_id"], f"{label}.program_binding_id")
    _identity(root["program_id"], f"{label}.program_id")
    for field in ("program_revision", "retained_bundle_id", "graph_revision"):
        _digest(root[field], f"{label}.{field}")
    env = _record(root["node_environment"], _FIELDS["node_environment"], f"{label}.node_environment")
    _identity(env["catalog_id"], f"{label}.node_environment.catalog_id")
    _string(env["content_version"], f"{label}.node_environment.content_version", _IDENTITY_MAX)
    _digest(env["catalog_digest"], f"{label}.node_environment.catalog_digest")
    _digest(env["environment_digest"], f"{label}.node_environment.environment_digest")

    components = _array(root["component_bindings"], f"{label}.component_bindings", maximum=_ROLE_MAX)
    component_by_id: dict[str, Mapping[str, Any]] = {}
    for index, raw in enumerate(components):
        path = f"{label}.component_bindings[{index}]"
        row = _record(raw, _FIELDS["component"], path)
        for field in ("component_id", "component_type_id"):
            _identity(row[field], f"{path}.{field}")
        _digest(row["component_type_revision"], f"{path}.component_type_revision")
        models = _array(row["model_bindings"], f"{path}.model_bindings")
        for mi, raw_model in enumerate(models):
            mp = f"{path}.model_bindings[{mi}]"
            model = _record(raw_model, _FIELDS["model"], mp)
            _identity(model["model_role_id"], f"{mp}.model_role_id")
            _identity(model["model_instance_id"], f"{mp}.model_instance_id")
            _digest(model["model_revision"], f"{mp}.model_revision")
        _sorted_unique(models, lambda x: x["model_role_id"], f"{path}.model_bindings")
        component_by_id[row["component_id"]] = row
    _sorted_unique(components, lambda x: x["component_id"], f"{label}.component_bindings")

    def geometry(kind: str, row: object, path: str) -> tuple[tuple[str, str], Mapping[str, Any]]:
        binding = _record(row, _FIELDS[kind], path)
        ref = _reference(binding["reference"], "frame" if kind == "frame" else "body", f"{path}.reference")
        _identity(binding["model_instance_id"], f"{path}.model_instance_id")
        _digest(binding["model_revision"], f"{path}.model_revision")
        if kind == "frame":
            _string(binding["compiled_frame_name"], f"{path}.compiled_frame_name")
        else:
            _id_array(binding["compiled_collision_body_names"], f"{path}.compiled_collision_body_names", text=True)
        return ref, binding

    frames: dict[tuple[str, str], Mapping[str, Any]] = {}
    body_bindings: dict[tuple[str, str], Mapping[str, Any]] = {}
    for field, kind, lookup in (("frame_bindings", "frame", frames), ("collision_body_bindings", "collision", body_bindings)):
        rows = _array(root[field], f"{label}.{field}", maximum=_ROLE_MAX)
        for index, raw in enumerate(rows):
            ref, binding = geometry(kind, raw, f"{label}.{field}[{index}]")
            lookup[ref] = binding
        _sorted_unique(rows, lambda x: (x["reference"]["component_id"], x["reference"]["frame_id" if kind == "frame" else "collision_body_id"]), f"{label}.{field}")

    signals: dict[tuple[str, str], Mapping[str, Any]] = {}
    signal_rows = _array(root["signal_bindings"], f"{label}.signal_bindings", maximum=_ROLE_MAX)
    for index, raw in enumerate(signal_rows):
        path = f"{label}.signal_bindings[{index}]"
        signal = _record(raw, _FIELDS["signal"], path)
        ref = _reference(signal["reference"], "signal", f"{path}.reference")
        selected_target = _reference(signal["target"], "target", f"{path}.target")
        if ref[0] != selected_target[0]: _fail(path, "signal and target component must match")
        _identity(signal["runtime_unit_id"], f"{path}.runtime_unit_id")
        _digest(signal["signal_descriptor_digest"], f"{path}.signal_descriptor_digest")
        if signal["value_type"] != "bool" or signal["direction"] not in ("input", "output") or signal["transition_capability"] not in ("sampled_only", "captured"):
            _fail(path, "invalid signal declaration")
        signals[ref] = signal
    _sorted_unique(signal_rows, lambda x: (x["reference"]["component_id"], x["reference"]["signal_name"]), f"{label}.signal_bindings")

    targets = _array(root["target_bindings"], f"{label}.target_bindings", maximum=_ROLE_MAX)
    target_by_ref: dict[tuple[str, str], Mapping[str, Any]] = {}
    for index, raw in enumerate(targets):
        path = f"{label}.target_bindings[{index}]"
        target = _record(raw, _FIELDS["target"], path, optional=_OPTIONAL_FIELDS["target"])
        target_ref = _reference(target["target"], "target", f"{path}.target")
        if target_ref[0] not in component_by_id:
            _fail(path, "target has no component binding")
        target_by_ref[target_ref] = target
        inventories: dict[str, list[tuple[str, str]]] = {}
        for field, kind, lookup in (("frame_references", "frame", frames), ("collision_body_references", "body", body_bindings), ("signal_references", "signal", signals)):
            refs = _array(target[field], f"{path}.{field}")
            keys = [_reference(ref, kind, f"{path}.{field}[{i}]") for i, ref in enumerate(refs)]
            if keys != sorted(set(keys)) or any(key not in lookup for key in keys):
                _fail(f"{path}.{field}", "references must be sorted unique and resolve")
            if kind == "signal" and any(_reference(signals[key]["target"], "target", path) != target_ref for key in keys):
                _fail(f"{path}.{field}", "signal must resolve through the exact target")
            inventories[field] = keys
        associations = _array(target["frame_associations"], f"{path}.frame_associations")
        assoc_keys = []
        for ai, raw_assoc in enumerate(associations):
            ap = f"{path}.frame_associations[{ai}]"
            assoc = _record(raw_assoc, _FIELDS["frame_association"], ap)
            ref = _reference(assoc["frame"], "frame", f"{ap}.frame")
            assoc_keys.append(ref)
            roles = _array(assoc["contact_body_role_ids"], f"{ap}.contact_body_role_ids")
            for role in roles: _identity(role, f"{ap}.contact_body_role_ids")
            _sorted_unique(roles, lambda x: x, f"{ap}.contact_body_role_ids")
            for role in roles:
                body = body_bindings.get((ref[0], role))
                frame = frames.get(ref)
                if body is None or frame is None or body["model_instance_id"] != frame["model_instance_id"] or body["model_revision"] != frame["model_revision"]:
                    _fail(ap, "contact body must resolve through target and frame model")
        if assoc_keys != inventories["frame_references"]:
            _fail(path, "frame associations must match frame references")
        resources = _array(target["resource_role_bindings"], f"{path}.resource_role_bindings")
        for ri, raw_resource in enumerate(resources):
            rp = f"{path}.resource_role_bindings[{ri}]"
            resource = _record(raw_resource, _FIELDS["resource"], rp)
            _identity(resource["resource_role_id"], f"{rp}.resource_role_id")
            ids = _id_array(resource["resource_ids"], f"{rp}.resource_ids")
            names = _id_array(resource["compiled_resource_names"], f"{rp}.compiled_resource_names", text=True)
            if len(ids) != len(names): _fail(rp, "resource correspondence vectors must have equal length")
        _sorted_unique(resources, lambda x: x["resource_role_id"], f"{path}.resource_role_bindings")
        capabilities = _array(target["capability_bindings"], f"{path}.capability_bindings")
        for ci, raw_cap in enumerate(capabilities):
            cp = f"{path}.capability_bindings[{ci}]"
            cap = _record(raw_cap, _FIELDS["capability"], cp)
            _identity(cap["capability_id"], f"{cp}.capability_id")
            _digest(cap["capability_definition_revision"], f"{cp}.capability_definition_revision")
            for field, name in (("control_binding", "control"), ("capability_provider_binding", "provider"), ("command_schema", "command_schema")):
                if cap[field] is not None:
                    keys = _FIELDS[name]
                    ref = _record(cap[field], keys, f"{cp}.{field}")
                    _identity(ref[keys[0]], f"{cp}.{field}.{keys[0]}")
                    _digest(ref[keys[1]], f"{cp}.{field}.{keys[1]}")
            if cap["control_binding"] is None and cap["capability_provider_binding"] is None: _fail(cp, "expected control or provider resolution")
        _sorted_unique(capabilities, lambda x: x["capability_id"], f"{path}.capability_bindings")
    _sorted_unique(targets, lambda x: (x["target"]["component_id"], x["target"]["command_target_id"]), f"{label}.target_bindings")
    for index, target in enumerate(targets):
        path = f"{label}.target_bindings[{index}]"
        def tcp_entry(raw: object, where: str) -> tuple[tuple[str, str], tuple[str, str]]:
            entry = _record(raw, _FIELDS["tcp_entry"], where)
            return (_reference(entry["target"], "target", f"{where}.target"),
                    _reference(entry["frame"], "frame", f"{where}.frame"))
        if "default_tcp" in target: tcp_entry(target["default_tcp"], f"{path}.default_tcp")
        tcps = _array(target.get("robot_tcps", []), f"{path}.robot_tcps")
        entries = []
        for ti, raw_tcp in enumerate(tcps):
            tp = f"{path}.robot_tcps[{ti}]"
            tcp = _record(raw_tcp, _FIELDS["robot_tcp"], tp)
            entry = tcp_entry(tcp["entry"], f"{tp}.entry")
            frame_ref, _ = geometry("frame", tcp["frame"], f"{tp}.frame")
            if frame_ref != entry[1]: _fail(tp, "TCP entry/frame correspondence")
            roles = _array(tcp["contact_body_role_ids"], f"{tp}.contact_body_role_ids")
            for role in roles: _identity(role, f"{tp}.contact_body_role_ids")
            _sorted_unique(roles, lambda x: x, f"{tp}.contact_body_role_ids")
            if type(tcp["end_effector"]) is not bool or type(tcp["component_default"]) is not bool:
                _fail(tp, "TCP marks must be boolean")
            entries.append(entry)
        if entries != sorted(set(entries)): _fail(path, "TCP entries must be sorted unique")
        if "pneumatic_io" not in target: continue
        rp = f"{path}.pneumatic_io"
        route = _record(target["pneumatic_io"], _FIELDS["pneumatic_io"], rp)
        if not any(cap["capability_id"] == "tool.gripper.command" for cap in target["capability_bindings"]):
            _fail(rp, "requires a gripper command target")
        outputs = _array(route["outputs"], f"{rp}.outputs", minimum=1, maximum=2)
        inputs = _array(route["inputs"], f"{rp}.inputs", maximum=4)
        output_keys = []
        for rows, kind in ((outputs, "output"), (inputs, "input")):
            for ri, raw_row in enumerate(rows):
                sp = f"{rp}.{kind}s[{ri}]"
                row = _record(raw_row, _FIELDS[f"pneumatic_{kind}"], sp)
                signal_ref = _reference(row["signal"], "signal", f"{sp}.signal")
                signal = signals.get(signal_ref)
                selected = target_by_ref.get(_reference(signal["target"], "target", f"{sp}.target")) if signal else None
                capability = "io.gpio.set_output" if kind == "output" else "io.gpio.get_input"
                if signal is None or selected is None or signal["direction"] != kind or not any(_reference(ref, "signal", sp) == signal_ref for ref in selected["signal_references"]) or not any(cap["capability_id"] == capability for cap in selected["capability_bindings"]):
                    _fail(sp, "signal does not resolve through its exact bound I/O target")
                if kind == "output":
                    if type(row["open_value"]) is not bool or type(row["close_value"]) is not bool: _fail(sp, "output levels must be boolean")
                    output_keys.append(signal_ref)
                elif row["input"] not in ("open", "closed", "pressure", "workpiece_grasped"):
                    _fail(sp, "unknown pneumatic input")
        if output_keys != sorted(set(output_keys)): _fail(f"{rp}.outputs", "expected sorted unique outputs")
        _sorted_unique(inputs, lambda x: x["input"], f"{rp}.inputs")

    node_policies = _array(
        root["node_policies"], f"{label}.node_policies",
        maximum=_NODE_POLICY_MAX,
    )
    for index, item in enumerate(node_policies):
        _node_policy(item, f"{label}.node_policies[{index}]")
    _sorted_unique(
        node_policies,
        lambda x: x["instance_id"],
        f"{label}.node_policies",
    )

    contacts = _array(root["contact_grasp_bindings"], f"{label}.contact_grasp_bindings", maximum=_ROLE_MAX)
    for pi, item in enumerate(contacts):
        pp = f"{label}.contact_grasp_bindings[{pi}]"
        branch = "contact_release" if isinstance(item, dict) and "mode" in item else "contact"
        policy = _record(item, _FIELDS[branch], pp, optional=_OPTIONAL_FIELDS[branch])
        _identity(policy["contact_policy_id"], f"{pp}.contact_policy_id")
        _digest(policy["contact_policy_revision"], f"{pp}.contact_policy_revision")
        _identity(policy["runtime_object_id"], f"{pp}.runtime_object_id")
        if branch == "contact_release":
            if policy["mode"] != "contact_release":
                _fail(pp, "unsupported contact mode")
            if policy["release_evidence_mode"] not in ("signed_penetration", "binary_contact"):
                _fail(pp, "unsupported release evidence")
            for field in ("enforce_max_start_penetration", "require_strictly_decreasing_penetration",
                          "require_separation_by_end", "allow_recontact"):
                if type(policy[field]) is not bool:
                    _fail(pp, "release flags must be boolean")
            for field in ("max_start_penetration_m", "min_release_progress_m", "release_tolerance_m"):
                if not 0 <= _number(policy[field], f"{pp}.{field}") <= 1000:
                    _fail(pp, "release metric ranges are invalid")
            if policy["release_evidence_mode"] == "binary_contact" and (
                policy["enforce_max_start_penetration"]
                or policy["require_strictly_decreasing_penetration"]
                or any(policy[field] != 0 for field in (
                    "max_start_penetration_m", "min_release_progress_m", "release_tolerance_m"))
            ):
                _fail(pp, "binary contact requires neutral metric fields")
        else:
            penetration = _number(policy["max_penetration_m"], f"{pp}.max_penetration_m")
            fraction = _number(policy["terminal_window_start_fraction"], f"{pp}.terminal_window_start_fraction")
            if penetration <= 0 or not 0 <= fraction <= 1:
                _fail(pp, "contact numeric ranges are invalid")
        endpoint = "bodies_from_motion_endpoint" in policy
        if endpoint and policy["bodies_from_motion_endpoint"] is not True:
            _fail(pp, "bodies_from_motion_endpoint must be true or omitted")
        bodies = _array(policy["allowed_body_bindings"], f"{pp}.allowed_body_bindings", minimum=0 if endpoint else 1)
        if endpoint != (len(bodies) == 0):
            _fail(pp, "endpoint and explicit contact bodies are mutually exclusive")
        for bi, item2 in enumerate(bodies):
            bp = f"{pp}.allowed_body_bindings[{bi}]"
            body = _record(item2, _FIELDS["contact_body"], bp)
            ref = _reference(body["body"], "body", f"{bp}.body")
            _identity(body["model_instance_id"], f"{bp}.model_instance_id")
            if body["model_role_id"] is not None:
                _identity(body["model_role_id"], f"{bp}.model_role_id")
            _digest(body["model_revision"], f"{bp}.model_revision")
            ids = _id_array(body["collision_body_ids"], f"{bp}.collision_body_ids", text=True)
            expected = [_COLLISION_BODY_ID.format(
                controllable_component_id=ref[0], model_instance_id=body["model_instance_id"],
                collision_body_role_id=ref[1], index=i,
            ) for i in range(len(ids))]
            if ids != expected:
                _fail(bp, "collision body IDs differ from generated scope")
            collision = body_bindings.get(ref)
            if collision is None or collision["model_instance_id"] != body["model_instance_id"] or collision["model_revision"] != body["model_revision"] or len(collision["compiled_collision_body_names"]) != len(ids):
                _fail(bp, "contact body differs from collision binding")
            if body["model_role_id"] is not None:
                component = component_by_id.get(ref[0])
                models = component["model_bindings"] if component else []
                if sum(x["model_role_id"] == body["model_role_id"] and x["model_instance_id"] == body["model_instance_id"] and x["model_revision"] == body["model_revision"] for x in models) != 1:
                    _fail(bp, "contact body differs from model binding")
        _sorted_unique(bodies, lambda x: (x["body"]["component_id"], x["body"]["collision_body_id"],
            (0, "") if x["model_role_id"] is None else (1, x["model_role_id"])), f"{pp}.allowed_body_bindings")
    _sorted_unique(contacts, lambda x: x["contact_policy_id"], f"{label}.contact_grasp_bindings")

    provenance = _record(root["provenance"], _FIELDS["provenance"], f"{label}.provenance")
    for field in ("binding_compiler_contract_digest",):
        _digest(provenance[field], f"{label}.provenance.{field}")
    for field in ("source_records", "resolved_output_records"):
        records = _array(provenance[field], f"{label}.provenance.{field}", maximum=_PROVENANCE_MAX)
        for ri, item in enumerate(records):
            rp = f"{label}.provenance.{field}[{ri}]"
            record = _record(item, _FIELDS["provenance_record"], rp)
            if (
                not isinstance(record["record_kind"], str)
                or record["record_kind"] not in _KINDS
            ):
                _fail(f"{rp}.record_kind", "expected recognized CatalogRecordKind")
            _identity(record["record_id"], f"{rp}.record_id")
            _digest(record["revision"], f"{rp}.revision")
        _sorted_unique(records, lambda x: (x["record_kind"], x["record_id"], x["revision"]), f"{label}.provenance.{field}")
    try:
        identity = verify_content_identity(root, own_field="program_binding_id",
            identity_domain="program-binding-id", api_version=_BINDING_API_VERSION,
            label=label)
    except (StrictArtifactError, TypeError, ValueError) as error:
        raise ProgramBindingTransportError(f"{label}: invalid content identity") from error
    if identity != root["program_binding_id"]:
        _fail(f"{label}.program_binding_id", "content identity mismatch")
    return value
