"""Bounded validator for the generated callback JSON-Schema subset."""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
import re
from typing import Any


_MAXIMUM_DEPTH = 96
_MAXIMUM_NODES = 300_000
_SCHEMA_KEYWORDS = {
    "$defs",
    "$ref",
    "additionalProperties",
    "allOf",
    "anyOf",
    "const",
    "description",
    "else",
    "enum",
    "exclusiveMaximum",
    "exclusiveMinimum",
    "if",
    "items",
    "maxItems",
    "maxLength",
    "maxProperties",
    "maximum",
    "minItems",
    "minLength",
    "minProperties",
    "minimum",
    "not",
    "oneOf",
    "pattern",
    "properties",
    "required",
    "then",
    "title",
    "type",
    "uniqueItems",
    "x-manyforge-semantic-type",
}


class CallbackSchemaValidationError(ValueError):
    """One callback argument does not satisfy the exact generated schema."""

    def __init__(self, path: str, message: str) -> None:
        super().__init__(f"{path}: {message}")
        self.path = path
        self.message = message


@dataclass(slots=True)
class _Budget:
    remaining: int = _MAXIMUM_NODES

    def consume(self, path: str) -> None:
        self.remaining -= 1
        if self.remaining < 0:
            raise CallbackSchemaValidationError(
                path, "value exceeds the callback validation node bound"
            )


def _fail(path: str, message: str) -> None:
    raise CallbackSchemaValidationError(path, message)


def _same_json_value(left: Any, right: Any) -> bool:
    if type(left) is not type(right):
        return False
    return left == right


def _matches_type(value: Any, expected: str) -> bool:
    return {
        "array": type(value) is list,
        "boolean": type(value) is bool,
        "integer": type(value) is int,
        "null": value is None,
        "number": type(value) in {int, float},
        "object": type(value) is dict,
        "string": type(value) is str,
    }.get(expected, False)


def _local_ref(root: dict[str, Any], reference: str, path: str) -> dict[str, Any]:
    prefix = "#/$defs/"
    if not reference.startswith(prefix):
        _fail(path, "only generated local $defs references are supported")
    name = reference.removeprefix(prefix)
    if not name or "/" in name or "~" in name:
        _fail(path, "local schema reference is malformed")
    definitions = root.get("$defs")
    if type(definitions) is not dict or type(definitions.get(name)) is not dict:
        _fail(path, "local schema reference is unresolved")
    return definitions[name]


def _branch_matches(
    value: Any,
    schema: dict[str, Any],
    *,
    root: dict[str, Any],
    path: str,
    depth: int,
    budget: _Budget,
    ref_stack: tuple[tuple[int, int], ...],
) -> bool:
    try:
        _validate(
            value,
            schema,
            root=root,
            path=path,
            depth=depth,
            budget=budget,
            ref_stack=ref_stack,
        )
    except CallbackSchemaValidationError:
        return False
    return True


def _validate(
    value: Any,
    schema: dict[str, Any],
    *,
    root: dict[str, Any],
    path: str,
    depth: int,
    budget: _Budget,
    ref_stack: tuple[tuple[int, int], ...],
) -> None:
    if depth > _MAXIMUM_DEPTH:
        _fail(path, "schema evaluation exceeds the callback depth bound")
    if type(schema) is not dict:
        _fail(path, "generated schema node is not an object")
    unknown = set(schema) - _SCHEMA_KEYWORDS
    if unknown:
        _fail(path, f"generated schema contains unsupported keywords {sorted(unknown)}")
    budget.consume(path)

    reference = schema.get("$ref")
    if reference is not None:
        if type(reference) is not str:
            _fail(path, "$ref must be text")
        resolved = _local_ref(root, reference, path)
        key = (id(resolved), id(value))
        if key in ref_stack:
            _fail(path, "generated schema reference is cyclic for one value")
        _validate(
            value,
            resolved,
            root=root,
            path=path,
            depth=depth + 1,
            budget=budget,
            ref_stack=(*ref_stack, key),
        )

    expected_type = schema.get("type")
    if expected_type is not None:
        if type(expected_type) is str:
            expected_types = (expected_type,)
        elif type(expected_type) is list and all(
            type(item) is str for item in expected_type
        ):
            expected_types = tuple(expected_type)
        else:
            _fail(path, "generated type constraint is malformed")
        if not any(_matches_type(value, item) for item in expected_types):
            _fail(path, f"expected JSON type {' or '.join(expected_types)}")

    if "const" in schema and not _same_json_value(value, schema["const"]):
        _fail(path, "value does not match the required constant")
    if "enum" in schema:
        enum = schema["enum"]
        if type(enum) is not list or not any(
            _same_json_value(value, candidate) for candidate in enum
        ):
            _fail(path, "value is outside the generated enum")

    for keyword in ("allOf", "anyOf", "oneOf"):
        if keyword not in schema:
            continue
        branches = schema[keyword]
        if type(branches) is not list or not branches:
            _fail(path, f"generated {keyword} constraint is malformed")
        if keyword == "allOf":
            for branch in branches:
                _validate(
                    value,
                    branch,
                    root=root,
                    path=path,
                    depth=depth + 1,
                    budget=budget,
                    ref_stack=ref_stack,
                )
            continue
        matches = sum(
            _branch_matches(
                value,
                branch,
                root=root,
                path=path,
                depth=depth + 1,
                budget=budget,
                ref_stack=ref_stack,
            )
            for branch in branches
        )
        if keyword == "anyOf" and matches < 1:
            _fail(path, "value matches no allowed generated schema branch")
        if keyword == "oneOf" and matches != 1:
            _fail(path, f"value must match exactly one schema branch, matched {matches}")

    if "not" in schema and _branch_matches(
        value,
        schema["not"],
        root=root,
        path=path,
        depth=depth + 1,
        budget=budget,
        ref_stack=ref_stack,
    ):
        _fail(path, "value matches a forbidden generated schema branch")
    if "if" in schema:
        condition = _branch_matches(
            value,
            schema["if"],
            root=root,
            path=path,
            depth=depth + 1,
            budget=budget,
            ref_stack=ref_stack,
        )
        selected = schema.get("then" if condition else "else")
        if selected is not None:
            _validate(
                value,
                selected,
                root=root,
                path=path,
                depth=depth + 1,
                budget=budget,
                ref_stack=ref_stack,
            )

    if type(value) is dict:
        if any(type(key) is not str for key in value):
            _fail(path, "object contains a non-string key")
        minimum = schema.get("minProperties", 0)
        maximum = schema.get("maxProperties", _MAXIMUM_NODES)
        if len(value) < minimum or len(value) > maximum:
            _fail(path, "object property count is outside the generated bounds")
        required = schema.get("required", [])
        if type(required) is not list or any(type(item) is not str for item in required):
            _fail(path, "generated required constraint is malformed")
        missing = [item for item in required if item not in value]
        if missing:
            _fail(path, f"required properties are missing: {missing}")
        properties = schema.get("properties", {})
        if type(properties) is not dict:
            _fail(path, "generated properties constraint is malformed")
        additional = schema.get("additionalProperties", True)
        unknown_properties = set(value) - set(properties)
        if additional is False and unknown_properties:
            _fail(path, f"unknown properties are forbidden: {sorted(unknown_properties)}")
        if additional is not True and additional is not False and type(additional) is not dict:
            _fail(path, "generated additionalProperties constraint is malformed")
        for name, item in value.items():
            child_schema = properties.get(name)
            if child_schema is None and type(additional) is dict:
                child_schema = additional
            if child_schema is not None:
                _validate(
                    item,
                    child_schema,
                    root=root,
                    path=f"{path}.{name}",
                    depth=depth + 1,
                    budget=budget,
                    ref_stack=ref_stack,
                )

    if type(value) is list:
        minimum = schema.get("minItems", 0)
        maximum = schema.get("maxItems", _MAXIMUM_NODES)
        if len(value) < minimum or len(value) > maximum:
            _fail(path, "array length is outside the generated bounds")
        if schema.get("uniqueItems") is True:
            encoded: list[str] = []
            for item in value:
                try:
                    identity = json.dumps(
                        item,
                        allow_nan=False,
                        ensure_ascii=False,
                        separators=(",", ":"),
                        sort_keys=True,
                    )
                except (TypeError, ValueError) as error:
                    _fail(path, f"array contains a non-JSON value: {error}")
                encoded.append(identity)
            if len(encoded) != len(set(encoded)):
                _fail(path, "array items must be unique")
        if "items" in schema:
            for index, item in enumerate(value):
                _validate(
                    item,
                    schema["items"],
                    root=root,
                    path=f"{path}[{index}]",
                    depth=depth + 1,
                    budget=budget,
                    ref_stack=ref_stack,
                )

    if type(value) is str:
        if len(value) < schema.get("minLength", 0):
            _fail(path, "string is shorter than the generated bound")
        if len(value) > schema.get("maxLength", _MAXIMUM_NODES):
            _fail(path, "string exceeds the generated bound")
        pattern = schema.get("pattern")
        if pattern is not None:
            if type(pattern) is not str:
                _fail(path, "generated pattern is malformed")
            try:
                matches = re.search(pattern, value) is not None
            except re.error as error:
                _fail(path, f"generated pattern is invalid: {error}")
            if not matches:
                _fail(path, "string does not match the generated pattern")

    if type(value) in {int, float}:
        numeric = float(value)
        if not math.isfinite(numeric):
            _fail(path, "number must be finite")
        for keyword, predicate in (
            ("minimum", lambda bound: numeric >= float(bound)),
            ("maximum", lambda bound: numeric <= float(bound)),
            ("exclusiveMinimum", lambda bound: numeric > float(bound)),
            ("exclusiveMaximum", lambda bound: numeric < float(bound)),
        ):
            if keyword in schema and not predicate(schema[keyword]):
                _fail(path, f"number violates {keyword}")


def validate_callback_arguments(value: Any, schema: Any, *, path: str = "$") -> None:
    """Validate one detached callback payload against one generated schema.

    `schema` is its own root: local references resolve inside it. `path`
    names the payload's location in the call's arguments.
    """

    if type(schema) is not dict:
        raise CallbackSchemaValidationError("$schema", "schema must be an object")
    _validate(
        value,
        schema,
        root=schema,
        path=path,
        depth=0,
        budget=_Budget(),
        ref_stack=(),
    )


__all__ = ["CallbackSchemaValidationError", "validate_callback_arguments"]
