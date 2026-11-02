"""Pure model-output authoring helpers and the I5/I6 model-node gate.

Provider HTTP calls, shared executors/clients, environment secrets, scene
snapshot transport, retry policy, output commits, and pause/recovery ownership
were coupled inside the legacy py_trees node.  They return through the
qualified assistant settings/provider boundary.  Bounded JSON parsing,
schema validation, confidence derivation, and template resolution remain as
transport-neutral authoring inputs.

Historical source: ``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``
blob ``e5385b4e22a75c2049fb55bef5a0cd85868f5403``.  The retained validator and
confidence responsibilities were previously concentrated at lines 1123-1173;
the provider/runtime body remains restoration input only.
"""

from __future__ import annotations

import json
import math
import re
from typing import Any, Final, NoReturn, Optional


MODEL_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_model_node_unavailable"
)
MODEL_NODE_RESTORATION_GATES: Final = ("I5", "H0", "I6", "N")
_MAX_TEMPLATE_CHARS: Final = 1_000_000
_MAX_TEMPLATE_VARIABLES: Final = 256
_MAX_SCHEMA_DEPTH: Final = 16
_MAX_SCHEMA_NODES: Final = 2048
_MAX_SCHEMA_PROPERTIES: Final = 512
_MAX_ENUM_VALUES: Final = 512
_MAX_OUTPUT_DEPTH: Final = 16
_MAX_OUTPUT_NODES: Final = 16_384
_MAX_OUTPUT_PROPERTIES: Final = 1024
_MAX_OUTPUT_ARRAY_ITEMS: Final = 4096
_MAX_SCHEMA_NAME_CHARS: Final = 256
_SUPPORTED_JSON_SCHEMA_TYPES: Final = frozenset(
    {"array", "boolean", "integer", "null", "number", "object", "string"}
)
_SCHEMA_ANNOTATION_KEYS: Final = frozenset({"description", "title"})
_SCHEMA_KEYS_BY_TYPE: Final = {
    "array": frozenset({"items", "minItems", "maxItems"}),
    "boolean": frozenset(),
    "integer": frozenset(),
    "null": frozenset(),
    "number": frozenset(),
    "object": frozenset({"properties", "required", "additionalProperties"}),
    "string": frozenset(),
}
_FENCED_JSON_RE = re.compile(
    r"^\s*```(?:json)?\s*(.*?)\s*```\s*$",
    re.IGNORECASE | re.DOTALL,
)


class ModelNodeUnavailableError(RuntimeError):
    """Raised before environment, provider, network, thread, or scene access."""


class ModelOutputValidationError(ValueError):
    """A model-output schema or instance is invalid or outside safe bounds."""


def _raise_model_node_unavailable() -> NoReturn:
    raise ModelNodeUnavailableError(
        f"{MODEL_NODE_UNAVAILABLE_REASON_CODE}: qualified model provider and "
        "Program-Binding execution return at I5/H0 and I6/N"
    )


def _call_model(*_args: object, **_kwargs: object) -> NoReturn:
    """Fail before reading provider configuration, credentials, or payloads."""
    _raise_model_node_unavailable()


def _reject_nonfinite_json_constant(value: str) -> NoReturn:
    raise ValueError(f"non-finite JSON constant {value}")


def _reject_duplicate_json_names(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"duplicate JSON object name {name!r}")
        result[name] = value
    return result


def _parse_json_text(text: Any) -> Optional[Any]:
    """Parse bounded JSON, tolerating one surrounding Markdown JSON fence."""
    if not isinstance(text, str) or len(text) > _MAX_TEMPLATE_CHARS:
        return None
    candidates = [text]
    fenced = _FENCED_JSON_RE.match(text)
    if fenced:
        candidates.append(fenced.group(1))
    for candidate in candidates:
        try:
            return json.loads(
                candidate,
                object_pairs_hook=_reject_duplicate_json_names,
                parse_constant=_reject_nonfinite_json_constant,
            )
        except (json.JSONDecodeError, RecursionError, TypeError, ValueError):
            continue
    return None


def _render_prompt_template(
    template_text: str,
    template_variables: dict[str, str],
) -> str:
    """Render bounded ``{{ variable }}`` placeholders with inner whitespace."""
    if len(template_text) > _MAX_TEMPLATE_CHARS:
        raise ValueError("template text exceeds the bounded text limit")
    if len(template_variables) > _MAX_TEMPLATE_VARIABLES:
        raise ValueError("template variable count exceeds 256")
    rendered = template_text
    for var_name, var_value in template_variables.items():
        if not isinstance(var_name, str) or not var_name:
            raise ValueError("template variable names must be non-empty strings")
        if not isinstance(var_value, str):
            raise TypeError("rendered template variable values must be strings")
        pattern = re.compile(r"\{\{\s*" + re.escape(var_name) + r"\s*\}\}")
        rendered = pattern.sub(lambda _match: var_value, rendered)
        if len(rendered) > _MAX_TEMPLATE_CHARS:
            raise ValueError("rendered template exceeds the bounded text limit")
    return rendered


def _resolve_output_schema_template(
    schema: Optional[dict[str, Any]],
    template_variables: dict[str, Any],
) -> Optional[dict[str, Any]]:
    """Resolve bounded runtime enum placeholders without mutating inputs."""
    if schema is None:
        return None
    if not isinstance(schema, dict):
        raise ValueError("output schema must be a mapping")
    remaining_nodes = _MAX_SCHEMA_NODES

    def walk(node: Any, *, depth: int, ancestors: set[int]) -> Any:
        nonlocal remaining_nodes
        if depth > _MAX_SCHEMA_DEPTH or remaining_nodes <= 0:
            raise ValueError("output schema exceeds structural bounds")
        remaining_nodes -= 1
        if isinstance(node, dict):
            object_id = id(node)
            if object_id in ancestors:
                raise ValueError("output schema contains a cycle")
            ancestors.add(object_id)
            try:
                resolved_node = {
                    key: walk(value, depth=depth + 1, ancestors=ancestors)
                    for key, value in node.items()
                }
            finally:
                ancestors.remove(object_id)
            enum_var = resolved_node.pop(
                "x-manyforge-enum-template-variable",
                None,
            )
            has_allow_empty = "x-manyforge-enum-allow-empty-string" in resolved_node
            allow_empty = resolved_node.pop(
                "x-manyforge-enum-allow-empty-string",
                False,
            )
            if not isinstance(allow_empty, bool):
                raise ValueError(
                    "x-manyforge-enum-allow-empty-string must be a boolean"
                )
            if has_allow_empty and enum_var is None:
                raise ValueError(
                    "x-manyforge-enum-allow-empty-string requires "
                    "x-manyforge-enum-template-variable"
                )
            if enum_var is not None:
                if "enum" in resolved_node:
                    raise ValueError(
                        "static enum cannot be combined with "
                        "x-manyforge-enum-template-variable"
                    )
                if not isinstance(enum_var, str) or not enum_var:
                    raise ValueError(
                        "x-manyforge-enum-template-variable must be a "
                        "non-empty string"
                    )
                enum_source = template_variables.get(enum_var)
                if not isinstance(enum_source, list) or not enum_source:
                    raise ValueError(
                        "x-manyforge-enum-template-variable must resolve to a "
                        "non-empty list"
                    )
                if len(enum_source) > _MAX_ENUM_VALUES:
                    raise ValueError("runtime enum exceeds 512 values")
                enum_values: list[Any] = []
                for item in enum_source:
                    if not isinstance(item, (str, int, float, bool)) or (
                        isinstance(item, float) and not math.isfinite(item)
                    ):
                        raise ValueError(
                            "runtime enum items must be finite JSON scalars"
                        )
                    enum_values.append(item)
                if allow_empty and "" not in enum_values:
                    enum_values.append("")
                resolved_node["enum"] = enum_values
            return resolved_node
        if isinstance(node, list):
            object_id = id(node)
            if object_id in ancestors:
                raise ValueError("output schema contains a cycle")
            ancestors.add(object_id)
            try:
                return [
                    walk(item, depth=depth + 1, ancestors=ancestors)
                    for item in node
                ]
            finally:
                ancestors.remove(object_id)
        return node

    result = walk(schema, depth=0, ancestors=set())
    if not isinstance(result, dict):
        raise ValueError("resolved output schema must be a mapping")
    _validate_schema_definition(
        result,
        path="$schema",
        depth=0,
        remaining_nodes=[_MAX_SCHEMA_NODES],
        ancestors=set(),
    )
    return result


def _json_type_name(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, str):
        return "string"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    return type(value).__name__


def _matches_json_schema_type(value: Any, schema_type: str) -> bool:
    if schema_type == "object":
        return isinstance(value, dict)
    if schema_type == "array":
        return isinstance(value, list)
    if schema_type == "string":
        return isinstance(value, str)
    if schema_type == "number":
        return (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and (not isinstance(value, float) or math.isfinite(value))
        )
    if schema_type == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if schema_type == "boolean":
        return isinstance(value, bool)
    if schema_type == "null":
        return value is None
    return False


def _enum_identity(value: Any) -> tuple[str, Any]:
    """Return type-aware identity for the supported scalar enum subset."""
    value_type = _json_type_name(value)
    if value_type in {"integer", "number"}:
        return ("number", value)
    return (value_type, value)


def _validate_schema_definition(
    schema: Any,
    *,
    path: str,
    depth: int,
    remaining_nodes: list[int],
    ancestors: set[int],
) -> None:
    """Validate the explicit, bounded JSON Schema subset used for model output."""
    if depth > _MAX_SCHEMA_DEPTH or remaining_nodes[0] <= 0:
        raise ModelOutputValidationError(
            f"{path}: schema exceeds depth/node bounds"
        )
    remaining_nodes[0] -= 1
    if not isinstance(schema, dict):
        raise ModelOutputValidationError(f"{path}: schema must be an object")
    if len(schema) > _MAX_SCHEMA_PROPERTIES:
        raise ModelOutputValidationError(f"{path}: schema has too many keywords")
    if any(not isinstance(key, str) for key in schema):
        raise ModelOutputValidationError(f"{path}: schema keywords must be strings")

    object_id = id(schema)
    if object_id in ancestors:
        raise ModelOutputValidationError(f"{path}: schema contains a cycle")
    ancestors.add(object_id)
    try:
        schema_type = schema.get("type")
        if not isinstance(schema_type, str) or schema_type not in _SUPPORTED_JSON_SCHEMA_TYPES:
            supported = ", ".join(sorted(_SUPPORTED_JSON_SCHEMA_TYPES))
            raise ModelOutputValidationError(
                f"{path}.type: expected one of {supported}"
            )

        allowed_keys = {
            "type",
            "enum",
            *_SCHEMA_ANNOTATION_KEYS,
            *_SCHEMA_KEYS_BY_TYPE[schema_type],
        }
        unsupported = sorted(set(schema) - allowed_keys)
        if unsupported:
            raise ModelOutputValidationError(
                f"{path}: unsupported schema keywords {unsupported}"
            )

        for annotation in _SCHEMA_ANNOTATION_KEYS:
            if annotation in schema:
                value = schema[annotation]
                if not isinstance(value, str):
                    raise ModelOutputValidationError(
                        f"{path}.{annotation}: expected a string"
                    )
                if len(value) > _MAX_TEMPLATE_CHARS:
                    raise ModelOutputValidationError(
                        f"{path}.{annotation}: annotation exceeds text bound"
                    )

        if "enum" in schema:
            if schema_type in {"array", "object"}:
                raise ModelOutputValidationError(
                    f"{path}.enum: composite enum values are unsupported"
                )
            enum_values = schema["enum"]
            if not isinstance(enum_values, list) or not enum_values:
                raise ModelOutputValidationError(
                    f"{path}.enum: expected a non-empty array"
                )
            if len(enum_values) > _MAX_ENUM_VALUES:
                raise ModelOutputValidationError(
                    f"{path}.enum: exceeds {_MAX_ENUM_VALUES} values"
                )
            identities: set[tuple[str, Any]] = set()
            for index, enum_value in enumerate(enum_values):
                if not _matches_json_schema_type(enum_value, schema_type):
                    raise ModelOutputValidationError(
                        f"{path}.enum[{index}]: value does not match type {schema_type}"
                    )
                if isinstance(enum_value, str) and len(enum_value) > _MAX_TEMPLATE_CHARS:
                    raise ModelOutputValidationError(
                        f"{path}.enum[{index}]: string exceeds text bound"
                    )
                identity = _enum_identity(enum_value)
                if identity in identities:
                    raise ModelOutputValidationError(
                        f"{path}.enum[{index}]: duplicate enum value"
                    )
                identities.add(identity)

        if schema_type == "object":
            properties = schema.get("properties", {})
            if not isinstance(properties, dict):
                raise ModelOutputValidationError(
                    f"{path}.properties: expected an object"
                )
            if len(properties) > _MAX_SCHEMA_PROPERTIES:
                raise ModelOutputValidationError(
                    f"{path}.properties: exceeds {_MAX_SCHEMA_PROPERTIES} fields"
                )
            for name, property_schema in properties.items():
                if (
                    not isinstance(name, str)
                    or not name
                    or len(name) > _MAX_SCHEMA_NAME_CHARS
                ):
                    raise ModelOutputValidationError(
                        f"{path}.properties: names must be bounded non-empty strings"
                    )
                _validate_schema_definition(
                    property_schema,
                    path=f"{path}.properties.{name}",
                    depth=depth + 1,
                    remaining_nodes=remaining_nodes,
                    ancestors=ancestors,
                )

            required = schema.get("required", [])
            if not isinstance(required, list):
                raise ModelOutputValidationError(
                    f"{path}.required: expected an array"
                )
            if len(required) > _MAX_SCHEMA_PROPERTIES:
                raise ModelOutputValidationError(
                    f"{path}.required: has too many fields"
                )
            required_names: set[str] = set()
            for index, name in enumerate(required):
                if not isinstance(name, str) or not name:
                    raise ModelOutputValidationError(
                        f"{path}.required[{index}]: expected a non-empty string"
                    )
                if name in required_names:
                    raise ModelOutputValidationError(
                        f"{path}.required[{index}]: duplicate field {name!r}"
                    )
                if name not in properties:
                    raise ModelOutputValidationError(
                        f"{path}.required[{index}]: undeclared field {name!r}"
                    )
                required_names.add(name)

            additional_properties = schema.get("additionalProperties", True)
            if not isinstance(additional_properties, bool):
                raise ModelOutputValidationError(
                    f"{path}.additionalProperties: only boolean values are supported"
                )

        elif schema_type == "array":
            if "items" not in schema:
                raise ModelOutputValidationError(
                    f"{path}.items: bounded array schemas require an item schema"
                )
            _validate_schema_definition(
                schema["items"],
                path=f"{path}.items",
                depth=depth + 1,
                remaining_nodes=remaining_nodes,
                ancestors=ancestors,
            )
            minimum = schema.get("minItems", 0)
            maximum = schema.get("maxItems", _MAX_OUTPUT_ARRAY_ITEMS)
            for keyword, value in (("minItems", minimum), ("maxItems", maximum)):
                if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                    raise ModelOutputValidationError(
                        f"{path}.{keyword}: expected a non-negative integer"
                    )
                if value > _MAX_OUTPUT_ARRAY_ITEMS:
                    raise ModelOutputValidationError(
                        f"{path}.{keyword}: exceeds the output array bound"
                    )
            if minimum > maximum:
                raise ModelOutputValidationError(
                    f"{path}: minItems cannot exceed maxItems"
                )
    finally:
        ancestors.remove(object_id)


def _validate_json_structure(
    value: Any,
    *,
    path: str,
    depth: int,
    remaining_nodes: list[int],
    ancestors: set[int],
) -> None:
    """Reject non-JSON, cyclic, non-finite, or structurally excessive output."""
    if depth > _MAX_OUTPUT_DEPTH or remaining_nodes[0] <= 0:
        raise ModelOutputValidationError(
            f"{path}: output exceeds depth/node bounds"
        )
    remaining_nodes[0] -= 1

    if value is None or isinstance(value, (bool, int)):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ModelOutputValidationError(f"{path}: number must be finite")
        return
    if isinstance(value, str):
        if len(value) > _MAX_TEMPLATE_CHARS:
            raise ModelOutputValidationError(f"{path}: string exceeds text bound")
        return
    if isinstance(value, list):
        if len(value) > _MAX_OUTPUT_ARRAY_ITEMS:
            raise ModelOutputValidationError(
                f"{path}: array exceeds {_MAX_OUTPUT_ARRAY_ITEMS} items"
            )
        object_id = id(value)
        if object_id in ancestors:
            raise ModelOutputValidationError(f"{path}: output contains a cycle")
        ancestors.add(object_id)
        try:
            for index, item in enumerate(value):
                _validate_json_structure(
                    item,
                    path=f"{path}[{index}]",
                    depth=depth + 1,
                    remaining_nodes=remaining_nodes,
                    ancestors=ancestors,
                )
        finally:
            ancestors.remove(object_id)
        return
    if isinstance(value, dict):
        if len(value) > _MAX_OUTPUT_PROPERTIES:
            raise ModelOutputValidationError(
                f"{path}: object exceeds {_MAX_OUTPUT_PROPERTIES} properties"
            )
        object_id = id(value)
        if object_id in ancestors:
            raise ModelOutputValidationError(f"{path}: output contains a cycle")
        ancestors.add(object_id)
        try:
            for name, item in value.items():
                if not isinstance(name, str):
                    raise ModelOutputValidationError(
                        f"{path}: object property names must be strings"
                    )
                if len(name) > _MAX_SCHEMA_NAME_CHARS:
                    raise ModelOutputValidationError(
                        f"{path}: object property name exceeds bound"
                    )
                _validate_json_structure(
                    item,
                    path=f"{path}.{name}",
                    depth=depth + 1,
                    remaining_nodes=remaining_nodes,
                    ancestors=ancestors,
                )
        finally:
            ancestors.remove(object_id)
        return
    raise ModelOutputValidationError(
        f"{path}: value of type {type(value).__name__} is not JSON-compatible"
    )


def _validate_json_value(data: Any, schema: dict[str, Any], *, path: str) -> None:
    schema_type = schema["type"]
    if not _matches_json_schema_type(data, schema_type):
        raise ModelOutputValidationError(
            f"{path}: expected {schema_type}, got {_json_type_name(data)}"
        )

    if "enum" in schema:
        actual_identity = _enum_identity(data)
        if not any(
            actual_identity == _enum_identity(candidate)
            for candidate in schema["enum"]
        ):
            raise ModelOutputValidationError(
                f"{path}: value is not in the declared enum"
            )

    if schema_type == "object":
        properties = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in data:
                raise ModelOutputValidationError(
                    f"{path}.{name}: required property is missing"
                )
        unknown = set(data) - set(properties)
        if unknown and not schema.get("additionalProperties", True):
            raise ModelOutputValidationError(
                f"{path}: undeclared properties are forbidden: {sorted(unknown)}"
            )
        for name, property_schema in properties.items():
            if name in data:
                _validate_json_value(
                    data[name],
                    property_schema,
                    path=f"{path}.{name}",
                )
    elif schema_type == "array":
        minimum = schema.get("minItems", 0)
        maximum = schema.get("maxItems", _MAX_OUTPUT_ARRAY_ITEMS)
        if len(data) < minimum or len(data) > maximum:
            raise ModelOutputValidationError(
                f"{path}: array length {len(data)} is outside [{minimum}, {maximum}]"
            )
        for index, item in enumerate(data):
            _validate_json_value(item, schema["items"], path=f"{path}[{index}]")


def _validate_json_schema(data: Any, schema: dict[str, Any]) -> None:
    """Validate model output against a bounded fail-closed JSON Schema subset.

    The supported assertion keywords are ``type``, ``required``,
    ``properties``, ``additionalProperties`` (boolean), ``items``,
    ``minItems``, ``maxItems``, and scalar ``enum``.  ``title`` and
    ``description`` are accepted annotations.  Every other keyword is
    rejected so an unsupported constraint can never be silently ignored.
    """
    _validate_schema_definition(
        schema,
        path="$schema",
        depth=0,
        remaining_nodes=[_MAX_SCHEMA_NODES],
        ancestors=set(),
    )
    _validate_json_structure(
        data,
        path="$",
        depth=0,
        remaining_nodes=[_MAX_OUTPUT_NODES],
        ancestors=set(),
    )
    _validate_json_value(data, schema, path="$")


def _derive_confidence(
    parsed: Any,
    schema: Optional[dict[str, Any]] = None,
) -> Optional[float]:
    """Return explicit finite confidence, never an invented success default.

    When a schema is supplied, only a declared numeric ``confidence``
    property participates, matching spec 470's threshold contract.
    """
    if schema is not None:
        _validate_schema_definition(
            schema,
            path="$schema",
            depth=0,
            remaining_nodes=[_MAX_SCHEMA_NODES],
            ancestors=set(),
        )
        properties = schema.get("properties", {}) if schema.get("type") == "object" else {}
        confidence_schema = properties.get("confidence")
        if confidence_schema is None:
            return None
        if confidence_schema.get("type") not in {"integer", "number"}:
            raise ModelOutputValidationError(
                "$schema.properties.confidence: confidence must be numeric"
            )
    if not isinstance(parsed, dict) or "confidence" not in parsed:
        return None
    raw_confidence = parsed["confidence"]
    if (
        not isinstance(raw_confidence, (int, float))
        or isinstance(raw_confidence, bool)
        or (isinstance(raw_confidence, float) and not math.isfinite(raw_confidence))
    ):
        raise ModelOutputValidationError(
            "$.confidence: expected a finite number"
        )
    try:
        confidence = float(raw_confidence)
    except OverflowError as exc:
        raise ModelOutputValidationError(
            "$.confidence: expected a finite number"
        ) from exc
    if not math.isfinite(confidence):
        raise ModelOutputValidationError(
            "$.confidence: expected a finite number"
        )
    return confidence


def _validate_model_output(
    data: Any,
    schema: dict[str, Any],
    *,
    min_confidence: Optional[float] = None,
) -> Optional[float]:
    """Validate one output and apply spec 470's optional confidence gate."""
    _validate_json_schema(data, schema)
    confidence = _derive_confidence(data, schema)
    if min_confidence is None:
        return confidence
    if (
        not isinstance(min_confidence, (int, float))
        or isinstance(min_confidence, bool)
        or (isinstance(min_confidence, float) and not math.isfinite(min_confidence))
    ):
        raise ModelOutputValidationError(
            "min_confidence: expected a finite number"
        )
    try:
        minimum = float(min_confidence)
    except OverflowError as exc:
        raise ModelOutputValidationError(
            "min_confidence: expected a finite number"
        ) from exc
    if not math.isfinite(minimum):
        raise ModelOutputValidationError(
            "min_confidence: expected a finite number"
        )
    if confidence is None:
        raise ModelOutputValidationError(
            "min_confidence requires a declared numeric confidence output"
        )
    if confidence < minimum:
        raise ModelOutputValidationError(
            f"$.confidence: {confidence} is below minimum {minimum}"
        )
    return confidence


def _resolve_response_tool_template(
    response_tool: Optional[dict[str, Any]],
    template_variables: dict[str, Any],
) -> Optional[dict[str, Any]]:
    if response_tool is None:
        return None
    if not isinstance(response_tool, dict):
        raise ValueError("response_tool must be a mapping")
    if any(not isinstance(key, str) for key in response_tool):
        raise ValueError("response_tool field names must be strings")
    unsupported = sorted(
        set(response_tool) - {"name", "description", "parameters"}
    )
    if unsupported:
        raise ValueError(f"response_tool contains unsupported fields {unsupported}")
    resolved = dict(response_tool)
    name = resolved.get("name")
    if (
        not isinstance(name, str)
        or not name
        or len(name) > _MAX_SCHEMA_NAME_CHARS
    ):
        raise ValueError("response_tool.name must be a bounded non-empty string")
    description = resolved.get("description", "")
    if description is not None and not isinstance(description, str):
        raise ValueError("response_tool.description must be a string")
    if isinstance(description, str) and len(description) > _MAX_TEMPLATE_CHARS:
        raise ValueError("response_tool.description exceeds the text bound")
    parameters = resolved.get("parameters")
    if parameters is not None and not isinstance(parameters, dict):
        raise ValueError("response_tool.parameters must be a mapping when provided")
    resolved["parameters"] = _resolve_output_schema_template(
        parameters,
        template_variables,
    )
    return resolved


class QueryModelNode:
    """Unavailable legacy provider node; no shared client or executor is built."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_model_node_unavailable()
