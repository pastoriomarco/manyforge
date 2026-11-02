"""Program expressions over the one C++ engine (docs/authoring/EXPRESSIONS.md).

Python never parses expressions itself: every check and evaluation goes
through the ``manyforge_headless_py.expression`` binding. This module only
maps Program declarations to engine types and values, orders computed keys
and names what an expression cannot read.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from functools import cache
from types import MappingProxyType
from typing import Any, Final

# A checked expression from the engine binding (CheckedExpression there).
CheckedExpression = Any


class ExpressionEngineUnavailable(ImportError):
    """The built engine binding is not importable in this process."""


@cache
def _engine() -> Any:
    """The engine binding, imported on first use.

    The C++ build's configure-time checks import the Program validator for
    Programs without expressions before the binding can exist, so the import
    happens here rather than at module load; every expression needs it.
    """

    try:
        from manyforge_headless_py import expression
    except ImportError as error:
        raise ExpressionEngineUnavailable(
            "Program expressions need the built manyforge_headless_py module; "
            "build manyforge_headless with MANYFORGE_HEADLESS_BUILD_PYTHON=ON "
            f"and put its build directory on PYTHONPATH ({error})"
        ) from error
    return expression


class ExpressionError(ValueError):
    """Structured engine diagnostics: code, message, position, length, name."""

    def __init__(self, diagnostics: list[dict[str, Any]]) -> None:
        super().__init__(diagnostics[0]["message"])
        self.diagnostics = diagnostics

#: Destination types an expression may produce.
DESTINATION_TYPES: Final = frozenset({"bool", "int64", "float64", "string"})

_ENGINE_TYPES: Final = MappingProxyType(
    {
        "bool": "bool",
        "int64": "int64",
        "float64": "float64",
        "string": "string",
        "scene_resource": "string",
        "stored_pose": "pose",
    }
)


class ComputedKeyCycleError(ValueError):
    """Computed keys read each other in a cycle."""

    def __init__(self, cycle: tuple[str, ...]) -> None:
        super().__init__(" -> ".join(cycle))
        self.cycle = cycle


def _engine_type(declared_type: str, fields: Mapping[str, Any]) -> Any:
    if fields:
        record = {
            name: _ENGINE_TYPES[field.type]
            for name, field in sorted(fields.items())
            if field.type in _ENGINE_TYPES
        }
        return record or None
    return _ENGINE_TYPES.get(declared_type)


def describe_diagnostic(diagnostic: Mapping[str, Any]) -> str:
    """One line naming the code, the character position and the message."""

    return (
        f"{diagnostic['code']} at character {diagnostic['position']}: "
        f"{diagnostic['message']}"
    )


@dataclass(frozen=True, slots=True)
class ExpressionEnvironment:
    """The ``param.``/``key.`` names an expression may read, with their types."""

    types: Mapping[str, Mapping[str, Any]]
    declared_types: Mapping[str, str]
    unavailable: Mapping[str, str]

    @classmethod
    def from_declarations(
        cls,
        parameters: Mapping[str, Any],
        keys: Mapping[str, Any],
        *,
        exclude_keys: Iterable[str] = (),
    ) -> "ExpressionEnvironment":
        excluded = frozenset(exclude_keys)
        types: dict[str, dict[str, Any]] = {"parameters": {}, "keys": {}}
        declared: dict[str, str] = {}
        unavailable: dict[str, str] = {}
        for name, declaration in parameters.items():
            engine_type = _engine_type(declaration.type, {})
            if engine_type is None:
                unavailable[f"param.{name}"] = (
                    f"param.{name} has type {declaration.type}, which "
                    "expressions cannot read"
                )
                continue
            types["parameters"][name] = engine_type
            declared[f"param.{name}"] = declaration.type
        for name, declaration in keys.items():
            label = f"key.{name}"
            if name in excluded:
                unavailable[label] = (
                    f"{label} is the computed key being edited or depends on it"
                )
                continue
            if declaration.access not in {"read", "read_write"}:
                unavailable[label] = f"{label} does not permit reads"
                continue
            engine_type = _engine_type(declaration.type, declaration.fields)
            if engine_type is None:
                unavailable[label] = (
                    f"{label} has type {declaration.type}, which expressions "
                    "cannot read"
                )
                continue
            types["keys"][name] = engine_type
            declared[label] = declaration.type
        return cls(
            types=MappingProxyType(types),
            declared_types=MappingProxyType(declared),
            unavailable=MappingProxyType(unavailable),
        )

    def names(self) -> list[dict[str, str]]:
        """The readable names in order, with their declared Program types."""

        return [
            {"name": name, "type": declared_type}
            for name, declared_type in sorted(self.declared_types.items())
        ]

    def _explain(self, written: str) -> str | None:
        prefix, _, rest = written.partition(".")
        segments = rest.split(".")
        for count in range(len(segments), 0, -1):
            reason = self.unavailable.get(f"{prefix}.{'.'.join(segments[:count])}")
            if reason is not None:
                return reason
        return None

    def check(self, text: str, destination: str) -> CheckedExpression:
        """Parse and type-check ``text``; raise ExpressionError with diagnostics."""

        if destination not in DESTINATION_TYPES:
            raise ValueError(f"expression destination {destination!r} is unsupported")
        engine = _engine()
        try:
            return engine.parse(text).check(
                {section: dict(rows) for section, rows in self.types.items()},
                destination,
            )
        except engine.ExpressionError as error:
            diagnostics = [dict(row) for row in error.diagnostics]
            for row in diagnostics:
                if row["code"] == "unknown_name":
                    reason = self._explain(row["name"])
                    if reason is not None:
                        row["message"] = reason
            raise ExpressionError(diagnostics) from None


def key_references(checked: CheckedExpression) -> tuple[str, ...]:
    return tuple(name for source, name in checked.references if source == "key")


def computed_key_order(
    checked: Mapping[str, CheckedExpression],
) -> tuple[str, ...]:
    """Order computed keys so each follows the computed keys it reads."""

    order: list[str] = []
    state: dict[str, str] = {}

    def visit(key: str, stack: tuple[str, ...]) -> None:
        if state.get(key) == "done":
            return
        if state.get(key) == "active":
            start = stack.index(key)
            raise ComputedKeyCycleError((*stack[start:], key))
        state[key] = "active"
        for dependency in key_references(checked[key]):
            if dependency in checked:
                visit(dependency, (*stack, key))
        state[key] = "done"
        order.append(key)

    for key in sorted(checked):
        visit(key, ())
    return tuple(order)


def dependents_of(key: str, checked: Mapping[str, CheckedExpression]) -> frozenset[str]:
    """``key`` plus every computed key that reads it, directly or indirectly."""

    result = {key}
    changed = True
    while changed:
        changed = False
        for name, expression in checked.items():
            if name not in result and result & set(key_references(expression)):
                result.add(name)
                changed = True
    return frozenset(result)


def engine_value(declaration: Any, value: Any) -> Any:
    """Keep only the record fields expressions can read; other values pass."""

    fields = getattr(declaration, "fields", None)
    if fields and isinstance(value, Mapping):
        return {
            name: item
            for name, item in value.items()
            if name in fields and fields[name].type in _ENGINE_TYPES
        }
    return value


def evaluate(
    checked: CheckedExpression,
    *,
    parameters: Mapping[str, Any],
    keys: Mapping[str, Any],
    parameter_declarations: Mapping[str, Any],
    key_declarations: Mapping[str, Any],
) -> Any:
    """Evaluate from one snapshot; a value absent from ``keys`` is missing."""

    values: dict[str, dict[str, Any]] = {"parameters": {}, "keys": {}}
    for source, name in checked.references:
        if source == "param":
            if name in parameters:
                values["parameters"][name] = engine_value(
                    parameter_declarations[name], parameters[name]
                )
        elif name in keys:
            values["keys"][name] = engine_value(key_declarations[name], keys[name])
    engine = _engine()
    try:
        return checked.evaluate(values)
    except engine.ExpressionError as error:
        raise ExpressionError([dict(row) for row in error.diagnostics]) from None


__all__ = [
    "CheckedExpression",
    "ComputedKeyCycleError",
    "DESTINATION_TYPES",
    "ExpressionEngineUnavailable",
    "ExpressionEnvironment",
    "ExpressionError",
    "computed_key_order",
    "dependents_of",
    "describe_diagnostic",
    "engine_value",
    "evaluate",
    "key_references",
]
