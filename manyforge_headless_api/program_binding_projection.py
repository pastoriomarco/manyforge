"""Shared transport checks for Program-Binding projections and Apply results."""

from __future__ import annotations

from typing import Any

from .program_binding_transport import (
    ProgramBindingTransportError,
    validate_program_binding,
)


_SCENARIO_SELECTION_FIELDS = frozenset(
    {
        "assistant_mode",
        "assistant_projection",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "owner_identities",
        "program",
        "program_binding",
        "program_binding_id",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "scenario_catalog_revision",
        "scenario_id",
        "schema_version",
        "typed_parameter_overrides",
    }
)
_APPLIED_SELECTION_FIELDS = frozenset(
    {
        "assistant_mode",
        "baseline_program_binding_id",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "preparation_digest",
        "program",
        "program_binding",
        "program_binding_id",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "scenario_catalog_revision",
        "scenario_id",
        "schema_version",
        "source",
        "typed_parameter_overrides",
    }
)
_APPLIED_SOURCE_FIELDS = frozenset(
    {
        "record_digest",
        "semantic_content_digest",
        "source_generation",
        "source_owner_epoch",
        "source_owner_id",
    }
)
_APPLY_RESULT_FIELDS = frozenset(
    {
        "applied",
        "preparation",
        "schema_version",
        "status",
    }
)
_SHA256_PREFIX = "sha256:"


def _fail(path: str, expected: str) -> None:
    raise ProgramBindingTransportError(f"{path}: {expected}")


def _sha256(value: object, path: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != len(_SHA256_PREFIX) + 64
        or not value.startswith(_SHA256_PREFIX)
        or any(character not in "0123456789abcdef" for character in value[7:])
    ):
        _fail(path, "lowercase SHA-256 identity")
    return value


def _text(value: object, path: str, maximum: int = 4096) -> str:
    if not isinstance(value, str) or not value:
        _fail(path, "bounded non-empty string")
    try:
        if len(value.encode("utf-8")) > maximum:
            _fail(path, "bounded non-empty string")
    except UnicodeEncodeError:
        _fail(path, "valid UTF-8 text")
    return value


def _binding_correspondence(
    projection: dict[str, Any],
    *,
    label: str,
) -> dict[str, Any]:
    binding = projection.get("program_binding")
    try:
        validated = validate_program_binding(binding, label=f"{label}.program_binding")
    except ProgramBindingTransportError:
        raise
    if validated is not binding or not isinstance(binding, dict):
        _fail(f"{label}.program_binding", "the exact validated Binding document")
    for field in (
        "program_binding_id",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "graph_revision",
    ):
        if binding.get(field) != projection.get(field):
            _fail(
                f"{label}.{field}",
                "the corresponding value from program_binding",
            )
    node_environment = binding.get("node_environment")
    if (
        not isinstance(node_environment, dict)
        or node_environment.get("environment_digest")
        != projection.get("node_environment_digest")
    ):
        _fail(
            f"{label}.node_environment_digest",
            "the corresponding Binding environment digest",
        )
    return binding


def validate_scenario_program_selection(
    value: object,
    *,
    scenario_id: str,
    label: str = "scenario_program_selection",
) -> dict[str, Any]:
    """Validate a scenario envelope and its exact nested Binding."""

    if not isinstance(value, dict) or set(value) != _SCENARIO_SELECTION_FIELDS:
        _fail(label, "the closed scenario Program selection record")
    if value.get("schema_version") != "manyforge.scenario_program_selection.v2":
        _fail(f"{label}.schema_version", "manyforge.scenario_program_selection.v2")
    if value.get("scenario_id") != scenario_id:
        _fail(f"{label}.scenario_id", "the requested scenario ID")
    if value.get("assistant_mode") not in {"required", "optional", "forbidden"}:
        _fail(f"{label}.assistant_mode", "a supported assistant mode")
    if value.get("assistant_projection") is not None and not isinstance(
        value.get("assistant_projection"), dict
    ):
        _fail(f"{label}.assistant_projection", "an object or null")
    if not isinstance(value.get("owner_identities"), list):
        _fail(f"{label}.owner_identities", "an array")
    if not isinstance(value.get("typed_parameter_overrides"), dict):
        _fail(f"{label}.typed_parameter_overrides", "an object")
    program = value.get("program")
    if not isinstance(program, dict):
        _fail(f"{label}.program", "an object")
    for field in ("program_id", "program_revision"):
        if program.get(field) != value.get(field):
            _fail(f"{label}.{field}", "the corresponding Program value")
    _sha256(value.get("scenario_catalog_revision"), f"{label}.scenario_catalog_revision")
    _text(value.get("deployment_artifact_id"), f"{label}.deployment_artifact_id", 256)
    for field in (
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "program_binding_id",
        "program_revision",
        "retained_bundle_id",
    ):
        _sha256(value.get(field), f"{label}.{field}")
    _text(value.get("program_id"), f"{label}.program_id", 256)
    _binding_correspondence(value, label=label)
    return value


def validate_applied_program_selection(
    value: object,
    *,
    label: str = "applied_program",
) -> dict[str, Any]:
    """Validate the complete transport-owned Applied projection closure."""

    if not isinstance(value, dict) or set(value) != _APPLIED_SELECTION_FIELDS:
        _fail(label, "the closed Applied Program selection record")
    if value.get("schema_version") != "manyforge.applied_program_selection.v1":
        _fail(f"{label}.schema_version", "manyforge.applied_program_selection.v1")
    if value.get("assistant_mode") not in {"required", "optional", "forbidden"}:
        _fail(f"{label}.assistant_mode", "a supported assistant mode")
    if not isinstance(value.get("typed_parameter_overrides"), dict):
        _fail(f"{label}.typed_parameter_overrides", "an object")
    if not isinstance(value.get("program"), dict):
        _fail(f"{label}.program", "an object")
    for field in (
        "baseline_program_binding_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "preparation_digest",
        "program_binding_id",
        "program_revision",
        "retained_bundle_id",
        "scenario_catalog_revision",
    ):
        _sha256(value.get(field), f"{label}.{field}")
    _text(value.get("deployment_artifact_id"), f"{label}.deployment_artifact_id", 256)
    _text(value.get("program_id"), f"{label}.program_id", 256)
    _text(value.get("scenario_id"), f"{label}.scenario_id", 256)
    program = value["program"]
    if (
        program.get("program_id") != value["program_id"]
        or program.get("program_revision") != value["program_revision"]
    ):
        _fail(f"{label}.program", "the corresponding Program identity")
    source = value.get("source")
    if not isinstance(source, dict) or set(source) != _APPLIED_SOURCE_FIELDS:
        _fail(f"{label}.source", "the closed Applied source record")
    _sha256(source.get("record_digest"), f"{label}.source.record_digest")
    _sha256(
        source.get("semantic_content_digest"),
        f"{label}.source.semantic_content_digest",
    )
    if (
        type(source.get("source_generation")) is not int
        or source["source_generation"] < 1
    ):
        _fail(f"{label}.source.source_generation", "a positive integer")
    _text(source.get("source_owner_epoch"), f"{label}.source.source_owner_epoch", 256)
    _text(source.get("source_owner_id"), f"{label}.source.source_owner_id", 256)
    _binding_correspondence(value, label=label)
    return value


def applied_program_belongs_to_scenario(
    applied: dict[str, Any], selection: dict[str, Any]
) -> bool:
    """Return exact Scenario anchoring across Program-only Apply."""

    fields = (
        "assistant_mode",
        "scenario_id",
        "scenario_catalog_revision",
        "deployment_artifact_id",
        "deployment_revision",
        "retained_bundle_id",
        "graph_revision",
        "node_environment_digest",
        "typed_parameter_overrides",
    )
    return applied.get("baseline_program_binding_id") == selection.get(
        "program_binding_id"
    ) and all(
        field in applied
        and field in selection
        and applied[field] == selection[field]
        for field in fields
    )


def applied_program_matches_scenario_selection(
    applied: dict[str, Any], selection: dict[str, Any]
) -> bool:
    """Return exact Load correspondence with the Scenario's selected Program."""

    return applied_program_belongs_to_scenario(applied, selection) and all(
        applied.get(field) == selection.get(field)
        for field in ("program_id", "program_revision", "program")
    )


def validate_program_apply_result(
    value: object,
    *,
    label: str = "program_apply_response",
) -> dict[str, Any]:
    """Validate Apply response framing and, when successful, its Applied result."""

    if not isinstance(value, dict) or set(value) != _APPLY_RESULT_FIELDS:
        _fail(label, "the closed Program Apply result record")
    if value.get("schema_version") != "manyforge.program_apply_result.v2":
        _fail(f"{label}.schema_version", "manyforge.program_apply_result.v2")
    status = value.get("status")
    if status != "applied":
        _fail(f"{label}.status", "applied")
    if not isinstance(value.get("preparation"), dict):
        _fail(f"{label}.preparation", "an object")
    applied = value.get("applied")
    if not isinstance(applied, dict):
        _fail(f"{label}.applied", "a complete Applied Program selection")
    else:
        validate_applied_program_selection(applied, label=f"{label}.applied")
    return value
