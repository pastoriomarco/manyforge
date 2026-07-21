"""Immutable, schema-validated operator-alarm definition catalog.

The shipped catalog contains rationalized metadata and identity fields only.
Executable set/clear logic remains in explicit domain adapters.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
from string import Formatter
from types import MappingProxyType
from typing import Any, Mapping
import unicodedata

import yaml


OPERATOR_ALARM_CATALOG_SCHEMA_VERSION = "manyforge.operator_alarm_catalog.v1"
_DEFINITION_FIELDS = {
    "definition_id",
    "version",
    "title",
    "message_template",
    "alarm_class",
    "priority",
    "priority_rationale",
    "consequence",
    "operator_response",
    "scope_fields",
    "set_authority",
    "clear_authority",
    "adapter_id",
    "runtime_modes",
    "acknowledgement_required",
    "acknowledgement_reason_required",
    "requires_motion_attention",
    "max_evidence_refs",
}
_PRIORITIES = {"critical", "high", "medium", "low"}
_ALARM_CLASSES = {"motion", "scene", "runtime_health"}
_STABLE_ID = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$")
_SCOPE_FIELD = re.compile(r"^[a-z][a-z0-9_]*$")
_MAX_TEXT_BYTES = 1024


class OperatorAlarmCatalogError(ValueError):
    """Stable catalog validation error."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class OperatorAlarmDefinition:
    definition_id: str
    version: int
    title: str
    message_template: str
    alarm_class: str
    priority: str
    priority_rationale: str
    consequence: str
    operator_response: str
    scope_fields: tuple[str, ...]
    set_authority: str
    clear_authority: str
    adapter_id: str
    runtime_modes: tuple[str, ...]
    acknowledgement_required: bool
    acknowledgement_reason_required: bool
    requires_motion_attention: bool
    max_evidence_refs: int


@dataclass(frozen=True, slots=True)
class OperatorAlarmCatalog:
    schema_version: str
    catalog_version: str
    definitions: Mapping[str, OperatorAlarmDefinition]
    sha256: str

    def definition(self, definition_id: str) -> OperatorAlarmDefinition:
        try:
            return self.definitions[definition_id]
        except KeyError as exc:
            raise OperatorAlarmCatalogError(
                "operator_alarm_definition_unknown",
                f"Unknown operator-alarm definition: {definition_id}",
            ) from exc


def default_operator_alarm_catalog_path() -> Path:
    return (
        Path(__file__).resolve().parent.parent
        / "resources"
        / "operator_alarm_catalog.v1.yaml"
    )


def _error(code: str, message: str) -> OperatorAlarmCatalogError:
    return OperatorAlarmCatalogError(code, message)


def _mapping(value: Any, *, field: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise _error("operator_alarm_catalog_invalid", f"{field} must be an object")
    return value


def _text(value: Any, *, field: str, stable_id: bool = False) -> str:
    if not isinstance(value, str) or not value:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{field} must be a non-empty string",
        )
    if value != unicodedata.normalize("NFC", value):
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{field} must use Unicode NFC normalization",
        )
    if len(value.encode("utf-8")) > _MAX_TEXT_BYTES:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{field} exceeds {_MAX_TEXT_BYTES} UTF-8 bytes",
        )
    if stable_id and _STABLE_ID.fullmatch(value) is None:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{field} is not a stable lowercase identifier",
        )
    return value


def _boolean(value: Any, *, field: str) -> bool:
    if type(value) is not bool:
        raise _error("operator_alarm_catalog_invalid", f"{field} must be boolean")
    return value


def _string_list(value: Any, *, field: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{field} must be a non-empty array",
        )
    result = tuple(_text(item, field=f"{field}[]") for item in value)
    if len(result) != len(set(result)):
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{field} entries must be unique",
        )
    return result


def _definition(raw: Any, *, index: int) -> OperatorAlarmDefinition:
    item = _mapping(raw, field=f"definitions[{index}]")
    unknown = set(item) - _DEFINITION_FIELDS
    missing = _DEFINITION_FIELDS - set(item)
    if unknown or missing:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"definitions[{index}] field mismatch; missing={sorted(missing)}, "
            f"unknown={sorted(unknown)}",
        )

    definition_id = _text(
        item["definition_id"],
        field=f"definitions[{index}].definition_id",
        stable_id=True,
    )
    version = item["version"]
    if type(version) is not int or version < 1:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.version must be a positive integer",
        )
    alarm_class = _text(item["alarm_class"], field=f"{definition_id}.alarm_class")
    if alarm_class not in _ALARM_CLASSES:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.alarm_class is unsupported",
        )
    priority = _text(item["priority"], field=f"{definition_id}.priority")
    if priority not in _PRIORITIES:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.priority is unsupported",
        )
    scope_fields = _string_list(
        item["scope_fields"], field=f"{definition_id}.scope_fields"
    )
    if any(_SCOPE_FIELD.fullmatch(field) is None for field in scope_fields):
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.scope_fields must be lowercase field identifiers",
        )
    runtime_modes = _string_list(
        item["runtime_modes"], field=f"{definition_id}.runtime_modes"
    )
    if "all" in runtime_modes and runtime_modes != ("all",):
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.runtime_modes 'all' must be the only entry",
        )
    message_template = _text(
        item["message_template"], field=f"{definition_id}.message_template"
    )
    placeholders = {
        field_name
        for _, field_name, _, _ in Formatter().parse(message_template)
        if field_name
    }
    if not placeholders.issubset(set(scope_fields)):
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.message_template references non-scope fields: "
            f"{sorted(placeholders - set(scope_fields))}",
        )
    max_evidence_refs = item["max_evidence_refs"]
    if (
        type(max_evidence_refs) is not int
        or max_evidence_refs < 1
        or max_evidence_refs > 32
    ):
        raise _error(
            "operator_alarm_catalog_invalid",
            f"{definition_id}.max_evidence_refs must be in [1, 32]",
        )

    return OperatorAlarmDefinition(
        definition_id=definition_id,
        version=version,
        title=_text(item["title"], field=f"{definition_id}.title"),
        message_template=message_template,
        alarm_class=alarm_class,
        priority=priority,
        priority_rationale=_text(
            item["priority_rationale"], field=f"{definition_id}.priority_rationale"
        ),
        consequence=_text(
            item["consequence"], field=f"{definition_id}.consequence"
        ),
        operator_response=_text(
            item["operator_response"], field=f"{definition_id}.operator_response"
        ),
        scope_fields=scope_fields,
        set_authority=_text(
            item["set_authority"],
            field=f"{definition_id}.set_authority",
            stable_id=True,
        ),
        clear_authority=_text(
            item["clear_authority"],
            field=f"{definition_id}.clear_authority",
            stable_id=True,
        ),
        adapter_id=_text(
            item["adapter_id"],
            field=f"{definition_id}.adapter_id",
            stable_id=True,
        ),
        runtime_modes=runtime_modes,
        acknowledgement_required=_boolean(
            item["acknowledgement_required"],
            field=f"{definition_id}.acknowledgement_required",
        ),
        acknowledgement_reason_required=_boolean(
            item["acknowledgement_reason_required"],
            field=f"{definition_id}.acknowledgement_reason_required",
        ),
        requires_motion_attention=_boolean(
            item["requires_motion_attention"],
            field=f"{definition_id}.requires_motion_attention",
        ),
        max_evidence_refs=max_evidence_refs,
    )


def load_operator_alarm_catalog(
    path: str | Path | None = None,
) -> OperatorAlarmCatalog:
    catalog_path = Path(path) if path is not None else default_operator_alarm_catalog_path()
    try:
        raw_bytes = catalog_path.read_bytes()
    except OSError as exc:
        raise _error(
            "operator_alarm_catalog_unavailable",
            f"Could not read operator-alarm catalog {catalog_path}: {exc}",
        ) from exc
    try:
        document = yaml.safe_load(raw_bytes)
    except yaml.YAMLError as exc:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"Could not parse operator-alarm catalog {catalog_path}: {exc}",
        ) from exc
    root = _mapping(document, field="catalog")
    unknown = set(root) - {"schema_version", "catalog_version", "definitions"}
    missing = {"schema_version", "catalog_version", "definitions"} - set(root)
    if unknown or missing:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"catalog field mismatch; missing={sorted(missing)}, unknown={sorted(unknown)}",
        )
    schema_version = _text(root["schema_version"], field="schema_version")
    if schema_version != OPERATOR_ALARM_CATALOG_SCHEMA_VERSION:
        raise _error(
            "operator_alarm_catalog_invalid",
            f"Unsupported operator-alarm catalog schema: {schema_version}",
        )
    catalog_version = _text(root["catalog_version"], field="catalog_version")
    raw_definitions = root["definitions"]
    if not isinstance(raw_definitions, list) or not raw_definitions:
        raise _error(
            "operator_alarm_catalog_invalid",
            "definitions must be a non-empty array",
        )
    definitions: dict[str, OperatorAlarmDefinition] = {}
    for index, raw_definition in enumerate(raw_definitions):
        definition = _definition(raw_definition, index=index)
        if definition.definition_id in definitions:
            raise _error(
                "operator_alarm_catalog_invalid",
                f"Duplicate definition_id: {definition.definition_id}",
            )
        definitions[definition.definition_id] = definition
    return OperatorAlarmCatalog(
        schema_version=schema_version,
        catalog_version=catalog_version,
        definitions=MappingProxyType(definitions),
        sha256=hashlib.sha256(raw_bytes).hexdigest(),
    )
