"""Shared transport checks for Program-Binding projections and Apply results."""

from __future__ import annotations

import posixpath
from typing import Any
import unicodedata

from .program_binding_transport import (
    ProgramBindingTransportError,
    validate_program_binding,
)


_APPLIED_SELECTION_FIELDS = frozenset(
    {
        "assistant_mode",
        "assistant_projection",
        "baseline_program_binding_id",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "owner_identities",
        "preparation_digest",
        "program",
        "program_binding",
        "program_binding_id",
        "program_id",
        "program_revision",
        "project_path",
        "retained_bundle_id",
        "source_document_digest",
        "scenario_id",
        "schema_version",
        "source",
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
_OWNER_IDENTITY_FIELDS = frozenset({"owner_epoch", "owner_id", "owner_role"})
_SHA256_PREFIX = "sha256:"
PROJECT_PATH_MAX_BYTES = 4096


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


def validate_project_path(value: object, path: str = "project_path") -> str:
    """The Scenario project folder: absolute and lexically normalized.

    The sender resolves it; nothing here touches the filesystem.
    """

    text = _text(value, path, PROJECT_PATH_MAX_BYTES)
    if (
        "\x00" in text
        or not text.startswith("/")
        or text.startswith("//")
        or posixpath.normpath(text) != text
    ):
        _fail(path, "a normalized absolute project folder path")
    return text


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


def validate_applied_program_selection(
    value: object,
    *,
    label: str = "applied_program",
) -> dict[str, Any]:
    """Validate the complete transport-owned Applied projection closure."""

    if not isinstance(value, dict) or set(value) != _APPLIED_SELECTION_FIELDS:
        _fail(label, "the closed Applied Program selection record")
    if value.get("schema_version") != "manyforge.applied_program_selection.v3":
        _fail(f"{label}.schema_version", "manyforge.applied_program_selection.v3")
    if value.get("assistant_mode") not in {"required", "optional", "forbidden"}:
        _fail(f"{label}.assistant_mode", "a supported assistant mode")
    if value.get("assistant_projection") is not None and not isinstance(
        value.get("assistant_projection"), dict
    ):
        _fail(f"{label}.assistant_projection", "an object or null")
    owner_identities = value.get("owner_identities")
    if not isinstance(owner_identities, list) or not owner_identities:
        _fail(f"{label}.owner_identities", "a non-empty array")
    owner_roles: list[str] = []
    for index, identity in enumerate(owner_identities):
        owner_label = f"{label}.owner_identities[{index}]"
        if not isinstance(identity, dict) or set(identity) != _OWNER_IDENTITY_FIELDS:
            _fail(owner_label, "the closed owner identity record")
        owner_roles.append(_text(identity.get("owner_role"), f"{owner_label}.owner_role", 256))
        _text(identity.get("owner_id"), f"{owner_label}.owner_id", 256)
        _text(identity.get("owner_epoch"), f"{owner_label}.owner_epoch", 256)
    if owner_roles != sorted(set(owner_roles)):
        _fail(f"{label}.owner_identities", "strictly role-sorted unique identities")
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
        "source_document_digest",
    ):
        _sha256(value.get(field), f"{label}.{field}")
    _text(value.get("deployment_artifact_id"), f"{label}.deployment_artifact_id", 256)
    _text(value.get("program_id"), f"{label}.program_id", 256)
    _text(value.get("scenario_id"), f"{label}.scenario_id", 256)
    validate_project_path(value.get("project_path"), f"{label}.project_path")
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


def validate_prepared_activation_origin(
    value: object, *, label: str = "origin",
) -> dict[str, Any]:
    """Check the exact typed origin without manufacturing Scenario facts."""
    if not isinstance(value, dict):
        _fail(label, "a closed prepared origin")
    if value.get("kind") == "scenario":
        if set(value) != {"kind", "scenario_id", "source_document_digest", "project_path"}:
            _fail(label, "the closed Scenario origin")
        _text(value["scenario_id"], f"{label}.scenario_id", 256)
        _sha256(value["source_document_digest"], f"{label}.source_document_digest")
        validate_project_path(value["project_path"], f"{label}.project_path")
    elif value.get("kind") == "sealed_canonical_empty":
        if set(value) != {"kind", "cell_id", "deployment_artifact_id", "deployment_revision",
                          "retained_bundle_id", "graph_revision", "admission_digest"}:
            _fail(label, "the closed sealed canonical-empty origin")
        for name in ("cell_id", "deployment_artifact_id"):
            text = _text(value[name], f"{label}.{name}", 128)
            if unicodedata.normalize("NFC", text) != text:
                _fail(f"{label}.{name}", "NFC identity")
        for name in ("deployment_revision", "retained_bundle_id", "graph_revision", "admission_digest"):
            _sha256(value[name], f"{label}.{name}")
    else:
        _fail(f"{label}.kind", "scenario or sealed_canonical_empty")
    return value


_COMMITTED_ACTIVATION_FIELDS = frozenset(
    {
        "apply_generation",
        "apply_id",
        "committed_applies",
        "scene_initialization",
        "program",
        "publication_generation",
        "record_digest",
        "origin",
        "schema_version",
    }
)
_COMMITTED_APPLY_FIELDS = frozenset(
    {"apply_generation", "apply_id", "record_digest", "origin", "scene_initialization"}
)
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991


def validate_committed_scenario_activation(
    value: object,
    *,
    label: str = "committed_activation",
) -> dict[str, Any]:
    """Validate Manager's current committed Scenario activation.

    Publication 1 is the canonical-empty bootstrap and every later one is
    one committed named Apply, so the committed Applies must account for
    every publication and include the current one.
    """

    if not isinstance(value, dict) or set(value) != _COMMITTED_ACTIVATION_FIELDS:
        _fail(label, "the closed committed Scenario activation record")
    if value.get("schema_version") != "manyforge.committed_scenario_activation.v3":
        _fail(
            f"{label}.schema_version", "manyforge.committed_scenario_activation.v3"
        )
    generation = value.get("publication_generation")
    if type(generation) is not int or not 2 <= generation <= _MAXIMUM_SAFE_INTEGER:
        _fail(f"{label}.publication_generation", "an integer of at least 2")
    current = _committed_apply(value, label)
    if value.get("program") is not None and not isinstance(value.get("program"), dict):
        _fail(f"{label}.program", "an object or null")
    if value["origin"]["kind"] == "sealed_canonical_empty" and value["program"] is not None:
        _fail(f"{label}.program", "null for sealed canonical-empty origin")
    committed = value.get("committed_applies")
    if not isinstance(committed, list) or len(committed) != generation - 1:
        _fail(f"{label}.committed_applies", "one record per Apply publication")
    applies: list[tuple[str, dict[str, Any], str, int, str]] = []
    for index, row in enumerate(committed):
        row_label = f"{label}.committed_applies[{index}]"
        if not isinstance(row, dict) or set(row) != _COMMITTED_APPLY_FIELDS:
            _fail(row_label, "the closed committed Apply record")
        applies.append(_committed_apply(row, row_label))
    digests = [apply[0] for apply in applies]
    if digests != sorted(set(digests)):
        _fail(f"{label}.committed_applies", "unique records in digest order")
    if current not in applies:
        _fail(f"{label}.committed_applies", "the current Apply among them")
    return value


def _committed_apply(value: dict[str, Any], label: str) -> tuple[str, dict[str, Any], str, int, str]:
    """One committed Apply: record digest, exact origin, and Apply identity."""

    apply_generation = value.get("apply_generation")
    if (
        type(apply_generation) is not int
        or not 1 <= apply_generation <= _MAXIMUM_SAFE_INTEGER
    ):
        _fail(f"{label}.apply_generation", "a positive integer")
    initialization = value.get("scene_initialization")
    if initialization not in ("preserve", "initialize"):
        _fail(f"{label}.scene_initialization", "preserve or initialize")
    origin = validate_prepared_activation_origin(value.get("origin"), label=f"{label}.origin")
    if origin["kind"] == "sealed_canonical_empty" and initialization != "preserve":
        _fail(f"{label}.scene_initialization", "preserve for sealed canonical-empty origin")
    return (
        _sha256(value.get("record_digest"), f"{label}.record_digest"),
        origin,
        _text(value.get("apply_id"), f"{label}.apply_id", 256),
        apply_generation,
        initialization,
    )
