"""GENERATED FILE -- program_binding_transport.v1.json; DO NOT EDIT."""

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
_BINDING_API_VERSION = "manyforge.program_binding.v2"
_FIELDS = {
    "binding": ("api_version", "program_binding_id", "program_id", "program_revision", "retained_bundle_id", "graph_revision", "node_environment", "component_role_bindings", "target_role_bindings", "node_policies", "contact_grasp_bindings", "provenance"),
    "node_environment": ("catalog_id", "content_version", "catalog_digest", "environment_digest"),
    "component": ("component_role_id", "controllable_component_id", "component_type_id", "component_type_revision", "model_role_bindings"),
    "model": ("model_role_id", "model_instance_id", "model_revision"),
    "target": ("target_role_id", "component_role_id", "controllable_component_id", "command_target_id", "frame_role_bindings", "resource_role_bindings", "collision_body_role_bindings", "capability_bindings", "signal_role_bindings"),
    "frame": ("frame_role_id", "model_instance_id", "model_revision", "compiled_frame_name"),
    "resource": ("resource_role_id", "resource_ids", "compiled_resource_names"),
    "collision": ("collision_body_role_id", "model_instance_id", "model_revision", "compiled_collision_body_names"),
    "capability": ("capability_id", "capability_definition_revision", "control_binding", "capability_provider_binding", "command_schema"),
    "node_policy": ("instance_id", "recovery_policy", "operator_stop_policy"),
    "recovery_policy": ("allow_intervention", "max_retries", "intervention_stop_scope"),
    "control": ("control_binding_id", "control_binding_revision"),
    "provider": ("capability_provider_binding_id", "capability_provider_binding_revision"),
    "command_schema": ("command_schema_id", "command_schema_revision"),
    "contact": ("contact_policy_id", "contact_policy_revision", "runtime_object_id", "allowed_body_bindings", "max_penetration_m", "terminal_window_start_fraction"),
    "contact_body": ("component_role_id", "target_role_id", "controllable_component_id", "command_target_id", "model_role_id", "collision_body_role_id", "model_instance_id", "model_revision", "collision_body_ids"),
    "provenance": ("binding_compiler_contract_digest", "role_assignment_digest", "source_records", "resolved_output_records"),
    "provenance_record": ("record_kind", "record_id", "revision"),
    "signal": ("resource_role_id", "signal_name", "value_type", "direction", "transition_capability", "signal_descriptor_digest"),
}
_OPTIONAL_FIELDS = {'recovery_policy': ('intervention_stop_scope',), 'target': ('signal_role_bindings',)}
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

    components = _array(root["component_role_bindings"], f"{label}.component_role_bindings", maximum=_ROLE_MAX)
    component_by_role: dict[str, Mapping[str, Any]] = {}
    for index, item in enumerate(components):
        path = f"{label}.component_role_bindings[{index}]"
        row = _record(item, _FIELDS["component"], path)
        for field in ("component_role_id", "controllable_component_id", "component_type_id"):
            _identity(row[field], f"{path}.{field}")
        _digest(row["component_type_revision"], f"{path}.component_type_revision")
        models = _array(row["model_role_bindings"], f"{path}.model_role_bindings")
        for mi, model_item in enumerate(models):
            mp = f"{path}.model_role_bindings[{mi}]"
            model = _record(model_item, _FIELDS["model"], mp)
            _identity(model["model_role_id"], f"{mp}.model_role_id")
            _identity(model["model_instance_id"], f"{mp}.model_instance_id")
            _digest(model["model_revision"], f"{mp}.model_revision")
        _sorted_unique(models, lambda x: x["model_role_id"], f"{path}.model_role_bindings")
        component_by_role[row["component_role_id"]] = row
    _sorted_unique(components, lambda x: x["component_role_id"], f"{label}.component_role_bindings")

    targets = _array(root["target_role_bindings"], f"{label}.target_role_bindings", maximum=_ROLE_MAX)
    target_by_role: dict[str, Mapping[str, Any]] = {}
    for index, item in enumerate(targets):
        path = f"{label}.target_role_bindings[{index}]"
        target = _record(item, _FIELDS["target"], path, optional=_OPTIONAL_FIELDS["target"])
        for field in ("target_role_id", "component_role_id", "controllable_component_id", "command_target_id"):
            _identity(target[field], f"{path}.{field}")
        component = component_by_role.get(target["component_role_id"])
        if component is None or component["controllable_component_id"] != target["controllable_component_id"]:
            _fail(path, "target must resolve through component binding")
        frames = _array(target["frame_role_bindings"], f"{path}.frame_role_bindings")
        for fi, item2 in enumerate(frames):
            fp = f"{path}.frame_role_bindings[{fi}]"
            frame = _record(item2, _FIELDS["frame"], fp)
            _identity(frame["frame_role_id"], f"{fp}.frame_role_id")
            _identity(frame["model_instance_id"], f"{fp}.model_instance_id")
            _digest(frame["model_revision"], f"{fp}.model_revision")
            _string(frame["compiled_frame_name"], f"{fp}.compiled_frame_name")
        _sorted_unique(frames, lambda x: x["frame_role_id"], f"{path}.frame_role_bindings")
        resources = _array(target["resource_role_bindings"], f"{path}.resource_role_bindings")
        for ri, item2 in enumerate(resources):
            rp = f"{path}.resource_role_bindings[{ri}]"
            resource = _record(item2, _FIELDS["resource"], rp)
            _identity(resource["resource_role_id"], f"{rp}.resource_role_id")
            ids = _id_array(resource["resource_ids"], f"{rp}.resource_ids")
            names = _id_array(resource["compiled_resource_names"], f"{rp}.compiled_resource_names", text=True)
            if len(ids) != len(names):
                _fail(rp, "resource correspondence vectors must have equal length")
        _sorted_unique(resources, lambda x: x["resource_role_id"], f"{path}.resource_role_bindings")
        if "signal_role_bindings" in target:
            signals = _array(target["signal_role_bindings"], f"{path}.signal_role_bindings", minimum=1)
            coordinate_roles = {row["resource_role_id"] for row in resources}
            for si, item2 in enumerate(signals):
                sp = f"{path}.signal_role_bindings[{si}]"
                signal = _record(item2, _FIELDS["signal"], sp)
                _identity(signal["resource_role_id"], f"{sp}.resource_role_id")
                _identity(signal["signal_name"], f"{sp}.signal_name")
                _digest(signal["signal_descriptor_digest"], f"{sp}.signal_descriptor_digest")
                if signal["value_type"] != "bool" or signal["direction"] not in ("input", "output") or signal["transition_capability"] not in ("sampled_only", "captured"):
                    _fail(sp, "invalid signal declaration")
                if signal["resource_role_id"] in coordinate_roles:
                    _fail(sp, "resource role cannot resolve to both coordinate and signal")
            _sorted_unique(signals, lambda x: x["resource_role_id"], f"{path}.signal_role_bindings")
        collisions = _array(target["collision_body_role_bindings"], f"{path}.collision_body_role_bindings")
        for ci, item2 in enumerate(collisions):
            cp = f"{path}.collision_body_role_bindings[{ci}]"
            collision = _record(item2, _FIELDS["collision"], cp)
            _identity(collision["collision_body_role_id"], f"{cp}.collision_body_role_id")
            _identity(collision["model_instance_id"], f"{cp}.model_instance_id")
            _digest(collision["model_revision"], f"{cp}.model_revision")
            _id_array(collision["compiled_collision_body_names"], f"{cp}.compiled_collision_body_names", text=True)
        _sorted_unique(collisions, lambda x: x["collision_body_role_id"], f"{path}.collision_body_role_bindings")
        capabilities = _array(target["capability_bindings"], f"{path}.capability_bindings")
        for ci, item2 in enumerate(capabilities):
            cp = f"{path}.capability_bindings[{ci}]"
            cap = _record(item2, _FIELDS["capability"], cp)
            _identity(cap["capability_id"], f"{cp}.capability_id")
            _digest(cap["capability_definition_revision"], f"{cp}.capability_definition_revision")
            specs = (
                ("control_binding", "control"),
                ("capability_provider_binding", "provider"),
                ("command_schema", "command_schema"),
            )
            for field, record_name in specs:
                nested = cap[field]
                if nested is not None:
                    keys = _FIELDS[record_name]
                    ref = _record(nested, keys, f"{cp}.{field}")
                    _identity(ref[keys[0]], f"{cp}.{field}.{keys[0]}")
                    _digest(ref[keys[1]], f"{cp}.{field}.{keys[1]}")
            if cap["control_binding"] is None and cap["capability_provider_binding"] is None:
                _fail(cp, "expected control or provider resolution")
        _sorted_unique(capabilities, lambda x: x["capability_id"], f"{path}.capability_bindings")
        target_by_role[target["target_role_id"]] = target
    _sorted_unique(targets, lambda x: x["target_role_id"], f"{label}.target_role_bindings")

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
        policy = _record(item, _FIELDS["contact"], pp)
        _identity(policy["contact_policy_id"], f"{pp}.contact_policy_id")
        _digest(policy["contact_policy_revision"], f"{pp}.contact_policy_revision")
        _identity(policy["runtime_object_id"], f"{pp}.runtime_object_id")
        penetration = _number(policy["max_penetration_m"], f"{pp}.max_penetration_m")
        fraction = _number(policy["terminal_window_start_fraction"], f"{pp}.terminal_window_start_fraction")
        if penetration <= 0 or not 0 <= fraction <= 1:
            _fail(pp, "contact numeric ranges are invalid")
        bodies = _array(policy["allowed_body_bindings"], f"{pp}.allowed_body_bindings", minimum=1)
        for bi, item2 in enumerate(bodies):
            bp = f"{pp}.allowed_body_bindings[{bi}]"
            body = _record(item2, _FIELDS["contact_body"], bp)
            for field in ("component_role_id", "target_role_id", "controllable_component_id",
                          "command_target_id", "collision_body_role_id", "model_instance_id"):
                _identity(body[field], f"{bp}.{field}")
            if body["model_role_id"] is not None:
                _identity(body["model_role_id"], f"{bp}.model_role_id")
            _digest(body["model_revision"], f"{bp}.model_revision")
            ids = _id_array(body["collision_body_ids"], f"{bp}.collision_body_ids", text=True)
            expected = [_COLLISION_BODY_ID.format(
                controllable_component_id=body["controllable_component_id"],
                model_instance_id=body["model_instance_id"],
                collision_body_role_id=body["collision_body_role_id"],
                index=i,
            ) for i in range(len(ids))]
            if ids != expected:
                _fail(bp, "collision body IDs differ from generated scope")
            target = target_by_role.get(body["target_role_id"])
            if target is None or any(target[field] != body[field] for field in
                    ("component_role_id", "controllable_component_id", "command_target_id")):
                _fail(bp, "contact body has no exact target")
            collisions = [x for x in target["collision_body_role_bindings"]
                if x["collision_body_role_id"] == body["collision_body_role_id"]]
            if len(collisions) != 1 or collisions[0]["model_instance_id"] != body["model_instance_id"] or \
                    collisions[0]["model_revision"] != body["model_revision"] or \
                    len(collisions[0]["compiled_collision_body_names"]) != len(ids):
                _fail(bp, "contact body differs from collision binding")
            if body["model_role_id"] is not None:
                models = component_by_role[body["component_role_id"]]["model_role_bindings"]
                if sum(x["model_role_id"] == body["model_role_id"] and
                       x["model_instance_id"] == body["model_instance_id"] and
                       x["model_revision"] == body["model_revision"] for x in models) != 1:
                    _fail(bp, "contact body differs from model binding")
        _sorted_unique(bodies, lambda x: (x["component_role_id"], x["target_role_id"],
            (0, "") if x["model_role_id"] is None else (1, x["model_role_id"]),
            x["collision_body_role_id"]), f"{pp}.allowed_body_bindings")
    _sorted_unique(contacts, lambda x: x["contact_policy_id"], f"{label}.contact_grasp_bindings")

    provenance = _record(root["provenance"], _FIELDS["provenance"], f"{label}.provenance")
    for field in ("binding_compiler_contract_digest", "role_assignment_digest"):
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
