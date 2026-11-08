"""Pure model-output helpers plus provider and output-binding cutover gates.

Historical acceptance source:
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` blob
``85a3dfdfbb1e66684736fa285eae7b02ab8cda54``.  In particular, schema cases
at lines 222-421, confidence cases at lines 571-614, structured-bundle cases
at lines 813-884, and the atomic-write failure at lines 1010-1035 remain
future runtime acceptance input.  These tests qualify only the retained pure
logic and pre-effect gates.
"""

import math

import pytest

from manyforge_behavior.nodes.blackboard_output import (
    BLACKBOARD_OUTPUT_UNAVAILABLE_REASON_CODE,
    BlackboardOutputBindingUnavailableError,
    BlackboardOutputMixin,
)
from manyforge_behavior.nodes.model import (
    ModelNodeUnavailableError,
    ModelOutputValidationError,
    QueryModelNode,
    _MAX_OUTPUT_ARRAY_ITEMS,
    _MAX_OUTPUT_DEPTH,
    _MAX_SCHEMA_DEPTH,
    _call_model,
    _derive_confidence,
    _parse_json_text,
    _render_prompt_template,
    _resolve_output_schema_template,
    _resolve_response_tool_template,
    _validate_json_schema,
    _validate_model_output,
)


class _PoisonInput:
    """Raises if a cutover gate inspects, formats, or otherwise uses it."""

    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"gated input was accessed: {name}")

    def __str__(self) -> str:
        raise AssertionError("gated input was formatted")

    def __repr__(self) -> str:
        raise AssertionError("gated input was represented")


def test_parse_json_text_accepts_plain_and_fenced_json() -> None:
    assert _parse_json_text('{"choice":"a"}') == {"choice": "a"}
    assert _parse_json_text('```json\n{"choice":"b"}\n```') == {"choice": "b"}
    assert _parse_json_text("not-json") is None
    assert _parse_json_text('{"confidence":NaN}') is None
    assert _parse_json_text('{"choice":"a","choice":"b"}') is None
    assert _parse_json_text(object()) is None


def test_prompt_template_allows_inner_whitespace_and_literal_replacements() -> None:
    assert _render_prompt_template(
        "Pick {{ object }} or {{object}}",
        {"object": "box\\1"},
    ) == "Pick box\\1 or box\\1"


def test_runtime_enum_template_is_bounded_and_does_not_mutate_source() -> None:
    schema = {
        "type": "string",
        "x-manyforge-enum-template-variable": "candidates",
        "x-manyforge-enum-allow-empty-string": True,
    }
    resolved = _resolve_output_schema_template(
        schema,
        {"candidates": ["a", "b"]},
    )
    assert resolved == {"type": "string", "enum": ["a", "b", ""]}
    assert "x-manyforge-enum-template-variable" in schema
    with pytest.raises(ValueError, match="non-empty list"):
        _resolve_output_schema_template(schema, {"candidates": []})
    with pytest.raises(ValueError, match="requires"):
        _resolve_output_schema_template(
            {
                "type": "string",
                "x-manyforge-enum-allow-empty-string": True,
            },
            {},
        )
    with pytest.raises(ModelOutputValidationError, match="unsupported"):
        _resolve_output_schema_template(
            {"type": "string", "pattern": "must-not-be-ignored"},
            {},
        )
    with pytest.raises(ValueError, match="cannot be combined"):
        _resolve_output_schema_template(
            {
                "type": "string",
                "enum": ["fixed"],
                "x-manyforge-enum-template-variable": "candidates",
            },
            {"candidates": ["dynamic"]},
        )


def test_response_tool_requires_name_and_resolves_parameter_enums() -> None:
    resolved = _resolve_response_tool_template(
        {
            "name": "choose",
            "parameters": {
                "type": "string",
                "x-manyforge-enum-template-variable": "choices",
            },
        },
        {"choices": ["left", "right"]},
    )
    assert resolved is not None
    assert resolved["parameters"]["enum"] == ["left", "right"]
    with pytest.raises(ValueError, match="non-empty string"):
        _resolve_response_tool_template({"name": ""}, {})


def test_schema_validator_accepts_required_properties_and_json_types() -> None:
    schema = {
        "type": "object",
        "required": ["name", "score", "count", "enabled", "empty"],
        "additionalProperties": False,
        "properties": {
            "name": {"type": "string"},
            "score": {"type": "number"},
            "count": {"type": "integer"},
            "enabled": {"type": "boolean"},
            "empty": {"type": "null"},
        },
    }
    _validate_json_schema(
        {
            "name": "candidate-a",
            "score": 0.75,
            "count": 2,
            "enabled": True,
            "empty": None,
        },
        schema,
    )

    with pytest.raises(ModelOutputValidationError, match="required property"):
        _validate_json_schema(
            {"name": "candidate-a", "score": 0.75, "count": 2, "enabled": True},
            schema,
        )
    with pytest.raises(ModelOutputValidationError, match="expected number"):
        _validate_json_schema(
            {
                "name": "candidate-a",
                "score": True,
                "count": 2,
                "enabled": True,
                "empty": None,
            },
            schema,
        )
    with pytest.raises(ModelOutputValidationError, match="undeclared properties"):
        _validate_json_schema(
            {
                "name": "candidate-a",
                "score": 0.75,
                "count": 2,
                "enabled": True,
                "empty": None,
                "hidden_reasoning": "must not be silently accepted",
            },
            schema,
        )


def test_schema_validator_enforces_enum_and_nested_array_items() -> None:
    schema = {
        "type": "array",
        "minItems": 1,
        "maxItems": 2,
        "items": {
            "type": "object",
            "required": ["outcome", "poses"],
            "additionalProperties": False,
            "properties": {
                "outcome": {
                    "type": "string",
                    "enum": ["selected", "no_match"],
                },
                "poses": {
                    "type": "array",
                    "minItems": 1,
                    "maxItems": 2,
                    "items": {
                        "type": "array",
                        "minItems": 3,
                        "maxItems": 3,
                        "items": {"type": "number"},
                    },
                },
            },
        },
    }
    _validate_json_schema(
        [{"outcome": "selected", "poses": [[0.4, -0.1, 0.2]]}],
        schema,
    )

    with pytest.raises(ModelOutputValidationError, match="declared enum"):
        _validate_json_schema(
            [{"outcome": "invented", "poses": [[0.4, -0.1, 0.2]]}],
            schema,
        )
    with pytest.raises(ModelOutputValidationError, match=r"\$\[0\]\.poses\[0\]\[1\]"):
        _validate_json_schema(
            [{"outcome": "selected", "poses": [[0.4, "bad", 0.2]]}],
            schema,
        )
    with pytest.raises(ModelOutputValidationError, match="array length"):
        _validate_json_schema(
            [{"outcome": "selected", "poses": [[0.4, -0.1]]}],
            schema,
        )


@pytest.mark.parametrize(
    ("schema", "message"),
    [
        ({}, r"\$schema\.type"),
        ({"type": ["string", "null"]}, r"\$schema\.type"),
        ({"type": "string", "pattern": "unsafe-to-ignore"}, "unsupported"),
        ({"type": "object", "properties": []}, "properties"),
        ({"type": "object", "required": "name"}, "required"),
        (
            {"type": "object", "required": ["name"], "properties": {}},
            "undeclared field",
        ),
        ({"type": "array"}, "require an item schema"),
        (
            {
                "type": "array",
                "items": {"type": "string"},
                "minItems": 2,
                "maxItems": 1,
            },
            "minItems cannot exceed",
        ),
        (
            {
                "type": "object",
                "additionalProperties": {"type": "string"},
            },
            "only boolean values",
        ),
        ({"type": "string", "enum": []}, "non-empty array"),
    ],
)
def test_schema_validator_rejects_unsupported_or_malformed_schemas(
    schema: object,
    message: str,
) -> None:
    with pytest.raises(ModelOutputValidationError, match=message):
        _validate_json_schema(None, schema)  # type: ignore[arg-type]


def test_schema_validator_enforces_structural_and_finite_json_bounds() -> None:
    too_deep_schema: dict[str, object] = {"type": "string"}
    for _ in range(_MAX_SCHEMA_DEPTH + 1):
        too_deep_schema = {"type": "array", "items": too_deep_schema}
    with pytest.raises(ModelOutputValidationError, match="depth/node bounds"):
        _validate_json_schema([], too_deep_schema)

    too_deep_output: object = "leaf"
    for index in range(_MAX_OUTPUT_DEPTH + 1):
        too_deep_output = {f"level_{index}": too_deep_output}
    with pytest.raises(ModelOutputValidationError, match="depth/node bounds"):
        _validate_json_schema(too_deep_output, {"type": "object"})

    with pytest.raises(ModelOutputValidationError, match="array exceeds"):
        _validate_json_schema(
            [None] * (_MAX_OUTPUT_ARRAY_ITEMS + 1),
            {"type": "array", "items": {"type": "null"}},
        )

    for nonfinite in (math.nan, math.inf, -math.inf):
        with pytest.raises(ModelOutputValidationError, match="must be finite"):
            _validate_json_schema(nonfinite, {"type": "number"})
        with pytest.raises(ModelOutputValidationError, match="does not match type"):
            _validate_json_schema(
                "finite",
                {"type": "number", "enum": [nonfinite]},
            )


def test_confidence_is_explicit_finite_and_applied_after_schema_validation() -> None:
    schema = {
        "type": "object",
        "required": ["choice", "confidence"],
        "properties": {
            "choice": {"type": "string", "enum": ["left", "right"]},
            "confidence": {"type": "number"},
        },
    }
    output = {"choice": "left", "confidence": 0.75}

    assert _derive_confidence(output) == 0.75
    assert _derive_confidence(output, schema) == 0.75
    assert _validate_model_output(output, schema, min_confidence=0.75) == 0.75

    with pytest.raises(ModelOutputValidationError, match="below minimum"):
        _validate_model_output(output, schema, min_confidence=0.8)
    with pytest.raises(ModelOutputValidationError, match="finite number"):
        _validate_model_output(output, schema, min_confidence=math.inf)
    with pytest.raises(ModelOutputValidationError, match="finite number"):
        _derive_confidence({"confidence": True})
    with pytest.raises(ModelOutputValidationError, match="number must be finite"):
        _validate_model_output(
            {"choice": "left", "confidence": math.nan},
            schema,
        )

    schema_without_confidence = {
        "type": "object",
        "properties": {"choice": {"type": "string"}},
    }
    assert _derive_confidence(output, schema_without_confidence) is None
    with pytest.raises(ModelOutputValidationError, match="must be numeric"):
        _derive_confidence(
            {"confidence": "high"},
            {
                "type": "object",
                "properties": {"confidence": {"type": "string"}},
            },
        )
    with pytest.raises(ModelOutputValidationError, match="requires a declared"):
        _validate_model_output(
            {"choice": "left"},
            schema_without_confidence,
            min_confidence=0.5,
        )


def test_provider_call_and_node_fail_before_secrets_or_inputs() -> None:
    poison = _PoisonInput()
    with pytest.raises(
        ModelNodeUnavailableError,
        match="composable_cell_model_node_unavailable",
    ):
        _call_model(provider=poison, prompt=poison)
    with pytest.raises(
        ModelNodeUnavailableError,
        match="composable_cell_model_node_unavailable",
    ):
        QueryModelNode(poison, endpoint=poison)


def test_blackboard_output_gate_fails_before_binding_or_payload_access() -> None:
    class UnboundOutput(BlackboardOutputMixin):
        @property
        def _bb(self) -> object:
            raise AssertionError("blackboard was accessed before Program Binding")

        @property
        def _output_key_prefix(self) -> str:
            raise AssertionError("output prefix was accessed before Program Binding")

    poison = _PoisonInput()
    with pytest.raises(
        BlackboardOutputBindingUnavailableError,
        match=BLACKBOARD_OUTPUT_UNAVAILABLE_REASON_CODE,
    ):
        UnboundOutput()._write_namespace_output(
            poison,
            status=poison,
            confidence=poison,
        )
