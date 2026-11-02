"""Owner-neutral primitives shared by bounded assistant clients.

These functions preserve bounded input and response handling from the former
Composer provider/tool modules.  They perform no provider discovery, network
access, tool dispatch, document mutation, or runtime-state interpretation.
Read-only assistant observations use the separate headless client. Provider
mediation and Program authoring now use their strict bridge/Draft boundaries;
these helpers still own no effects or semantic state.
"""

from __future__ import annotations

import math
import re
from collections.abc import Sequence
from typing import Any


_NODE_RUNTIME_WARNING_RE = re.compile(
    r"\(node:\d+\)\s*"
    r"(?:\[[\w.+-]+\]\s*)?"
    r"\w*Warning:"
)


class AssistantInputError(ValueError):
    """Raised when a thin-client input fails bounded value validation."""


def partition_runtime_warnings(
    warnings: Sequence[str],
) -> tuple[list[str], list[str]]:
    """Split actionable warnings from recognized Node process warnings.

    The recognizer is an allowlist: unexpected stderr remains actionable.
    Callers decide how to record the returned runtime-noise list; this helper
    never silently discards it.
    """

    kept: list[str] = []
    runtime_noise: list[str] = []
    for warning in warnings:
        if _NODE_RUNTIME_WARNING_RE.search(warning):
            runtime_noise.append(warning)
        else:
            kept.append(warning)
    return kept, runtime_noise


def flatten_provider_message(value: Any) -> str:
    """Normalize a string or typed text-block collection to plain text."""

    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        text = value.get("text")
        return text if isinstance(text, str) else ""
    if isinstance(value, list):
        parts: list[str] = []
        for block in value:
            if isinstance(block, str):
                parts.append(block)
                continue
            if not isinstance(block, dict):
                continue
            block_type = block.get("type")
            text = block.get("text")
            if isinstance(text, str) and block_type in (None, "text"):
                parts.append(text)
        return "".join(parts)
    return ""


def finite_number(value: Any, label: str) -> float:
    """Return one finite float, rejecting booleans and non-numeric values."""

    if isinstance(value, bool):
        raise AssistantInputError(f"{label} must be a finite number")
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise AssistantInputError(f"{label} must be a finite number") from exc
    if not math.isfinite(result):
        raise AssistantInputError(f"{label} must be a finite number")
    return result


def finite_number_list(
    value: Any,
    expected_len: int,
    label: str,
) -> list[float]:
    """Validate one fixed-length finite numeric vector."""

    if expected_len < 0:
        raise ValueError("expected_len must be non-negative")
    if not isinstance(value, list) or len(value) != expected_len:
        raise AssistantInputError(
            f"{label} must contain exactly {expected_len} values"
        )
    return [
        finite_number(item, f"{label}[{index}]")
        for index, item in enumerate(value)
    ]


def required_string(data: dict[str, Any], key: str) -> str:
    """Return one non-empty trimmed string field."""

    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise AssistantInputError(f"{key} must be a non-empty string")
    return value.strip()


__all__ = [
    "AssistantInputError",
    "finite_number",
    "finite_number_list",
    "flatten_provider_message",
    "partition_runtime_warnings",
    "required_string",
]
