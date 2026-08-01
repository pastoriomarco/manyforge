"""Typed, source-neutral diagnostic presentation for future clients.

The former helper reconciled CycleManager fields with a second Kernel snapshot
and therefore selected runtime truth inside Composer.  The replacement accepts
one already-authoritative headless snapshot and performs presentation only.
"""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping


def _state_token(value: Any) -> str:
    raw = getattr(value, "state", value)
    token = getattr(raw, "name", None) or getattr(raw, "value", None) or str(raw)
    return str(token).split(".")[-1]


def _dict_or_none(value: Any) -> dict[str, Any] | None:
    if not value:
        return None
    if isinstance(value, dict):
        return dict(value)
    try:
        return dict(value)
    except Exception:  # noqa: BLE001 - a presentation path stays non-fatal.
        return None


@dataclass(frozen=True, slots=True)
class HeadlessDiagnosticSnapshot:
    """One immutable client value produced by the future headless owner."""

    state: Any
    error_code: str | None = None
    message: str | None = None
    details: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        state = _state_token(self.state).strip()
        if not state:
            raise ValueError("diagnostic state must be non-empty")
        error_code = _optional_text(self.error_code)
        message = _optional_text(self.message)
        details = _dict_or_none(self.details)
        object.__setattr__(self, "state", state)
        object.__setattr__(self, "error_code", error_code)
        object.__setattr__(self, "message", message)
        object.__setattr__(
            self,
            "details",
            None if details is None else MappingProxyType(details),
        )


@dataclass(frozen=True, slots=True)
class DiagnosticPresentation:
    """Display fields with no inference from secondary runtime sources."""

    state: str
    message: str | None
    error_code: str | None
    details: Mapping[str, Any] | None


def present_diagnostic_snapshot(
    snapshot: HeadlessDiagnosticSnapshot,
) -> DiagnosticPresentation:
    """Project one typed snapshot without manager, Kernel, or backend reads."""

    return DiagnosticPresentation(
        state=snapshot.state,
        message=snapshot.message or snapshot.error_code,
        error_code=snapshot.error_code,
        details=(
            None
            if snapshot.details is None
            else MappingProxyType(dict(snapshot.details))
        ),
    )


def _optional_text(value: str | None) -> str | None:
    if value is None:
        return None
    normalized = value.strip()
    return normalized or None


__all__ = [
    "DiagnosticPresentation",
    "HeadlessDiagnosticSnapshot",
    "present_diagnostic_snapshot",
]
