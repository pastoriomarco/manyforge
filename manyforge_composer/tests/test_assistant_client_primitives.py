"""Focused tests for retained assistant thin-client primitives."""

from __future__ import annotations

import pytest

from manyforge_composer.backend.assistant_client_primitives import (
    AssistantInputError,
    finite_number,
    finite_number_list,
    flatten_provider_message,
    partition_runtime_warnings,
    required_string,
)


def test_runtime_warning_partition_is_allowlisted_and_lossless() -> None:
    warnings = [
        "(node:42) [DEP0040] DeprecationWarning: runtime notice",
        "provider returned a recoverable warning",
        "Traceback: provider transport failed",
    ]

    kept, runtime_noise = partition_runtime_warnings(warnings)

    assert kept == warnings[1:]
    assert runtime_noise == warnings[:1]


def test_provider_message_flattens_only_text_content() -> None:
    assert flatten_provider_message("plain") == "plain"
    assert flatten_provider_message({"type": "text", "text": "block"}) == "block"
    assert flatten_provider_message(
        [
            {"type": "text", "text": "first"},
            {"type": "image", "text": "ignored"},
            " + second",
            {"text": " + third"},
            5,
        ]
    ) == "first + second + third"
    assert flatten_provider_message({"text": 5}) == ""


def test_assistant_value_validation_is_bounded_and_finite() -> None:
    assert finite_number("1.25", "value") == pytest.approx(1.25)
    assert finite_number_list([1, "2"], 2, "point") == [1.0, 2.0]
    assert required_string({"name": "  item  "}, "name") == "item"

    for value in (True, float("nan"), float("inf"), object()):
        with pytest.raises(AssistantInputError, match="finite number"):
            finite_number(value, "value")
    with pytest.raises(AssistantInputError, match="exactly 3"):
        finite_number_list([1, 2], 3, "point")
    with pytest.raises(AssistantInputError, match="non-empty"):
        required_string({"name": "  "}, "name")
