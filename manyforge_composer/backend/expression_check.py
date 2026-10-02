"""Formula checks for the editor, against the current Draft's declarations.

The page has no expression engine of its own: diagnostics, the result type,
the readable names and a preview all come from here, through the one engine.
The preview uses authoring values only (parameter defaults, key `initial:`
values and computed keys evaluated from them); it is never a runtime value.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Final

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import program_value_declarations
from manyforge_behavior.program_expressions import (
    CheckedExpression,
    ComputedKeyCycleError,
    ExpressionEnvironment,
    ExpressionError,
    computed_key_order,
    dependents_of,
    describe_diagnostic,
    evaluate,
)

SCHEMA_VERSION: Final = "manyforge.expression_check.v1"


class ExpressionCheckRefusal(ValueError):
    """The request cannot be checked as asked; ``code`` names why."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _checked_computed_keys(
    environment: ExpressionEnvironment, keys: Mapping[str, Any]
) -> dict[str, CheckedExpression]:
    checked: dict[str, CheckedExpression] = {}
    for name, declaration in keys.items():
        if declaration.expression is None:
            continue
        try:
            checked[name] = environment.check(declaration.expression, declaration.type)
        except ExpressionError:
            continue  # Its own editor shows why; it cannot be previewed.
    return checked


def _unavailable(diagnostic: Mapping[str, Any]) -> str:
    if diagnostic["code"] == "missing_value":
        return f"{diagnostic['name']} has no authoring value"
    return describe_diagnostic(diagnostic)


def check_draft_expression(
    authoring: Mapping[str, Any],
    catalog: NodeCatalog,
    *,
    expression: str,
    destination_type: str,
    exclude_computed_key: str | None,
) -> dict[str, Any]:
    """Check one formula; the Draft's declarations must themselves be valid."""

    parameters, keys = program_value_declarations(
        authoring, catalog=catalog, source="program"
    )
    everything = ExpressionEnvironment.from_declarations(parameters, keys)
    all_computed = _checked_computed_keys(everything, keys)
    excluded: frozenset[str] = frozenset()
    if exclude_computed_key is not None:
        declaration = keys.get(exclude_computed_key)
        if declaration is None or declaration.expression is None:
            raise ExpressionCheckRefusal(
                "expression_check_exclusion_invalid",
                f"exclude_computed_key {exclude_computed_key!r} names no computed "
                "key of the Draft",
            )
        excluded = dependents_of(exclude_computed_key, all_computed)
    environment = ExpressionEnvironment.from_declarations(
        parameters, keys, exclude_keys=excluded
    )
    names = environment.names()
    try:
        checked = environment.check(expression, destination_type)
    except ExpressionError as error:
        return {
            "schema_version": SCHEMA_VERSION,
            "result_type": None,
            "diagnostics": [dict(row) for row in error.diagnostics],
            "preview": {
                "value": None,
                "unavailable_reason": "the expression has errors",
            },
            "names": names,
        }

    parameter_values = {
        name: declaration.default
        for name, declaration in parameters.items()
        if declaration.default is not None
    }
    key_values = {
        name: declaration.initial
        for name, declaration in keys.items()
        if declaration.has_initial
    }
    computed = {
        name: value for name, value in all_computed.items() if name not in excluded
    }
    reasons: dict[str, str] = {}
    try:
        order = computed_key_order(computed)
    except ComputedKeyCycleError as error:
        order = ()
        reasons.update(
            (name, f"computed keys read each other in a cycle: {error}")
            for name in computed
        )
    for name in order:
        try:
            key_values[name] = evaluate(
                computed[name],
                parameters=parameter_values,
                keys=key_values,
                parameter_declarations=parameters,
                key_declarations=keys,
            )
        except ExpressionError as error:
            reasons[name] = _unavailable(error.diagnostics[0])
    for name, declaration in keys.items():
        if (
            declaration.expression is not None
            and name not in key_values
            and name not in reasons
            and name not in excluded
        ):
            reasons[name] = "its expression has errors"
    value: Any = None
    reason: str | None = None
    blocked = [
        name
        for source, name in checked.references
        if source == "key" and name in reasons
    ]
    if blocked:
        reason = f"key.{blocked[0]} cannot be previewed: {reasons[blocked[0]]}"
    else:
        try:
            value = evaluate(
                checked,
                parameters=parameter_values,
                keys=key_values,
                parameter_declarations=parameters,
                key_declarations=keys,
            )
        except ExpressionError as error:
            reason = _unavailable(error.diagnostics[0])
    return {
        "schema_version": SCHEMA_VERSION,
        "result_type": checked.result_type,
        "diagnostics": [],
        "preview": {"value": value, "unavailable_reason": reason},
        "names": names,
    }


__all__ = [
    "ExpressionCheckRefusal",
    "SCHEMA_VERSION",
    "check_draft_expression",
]
