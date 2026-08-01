#!/usr/bin/env python3
"""Bounded source-text rule engine plus the I9/I viewer-boundary gate.

The complete historical checker remains available at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``12f03a8451b0e529119b87fa2630b67faad6d142``).  Its line 14-19 file reader
and line 21-138 checker were inseparable from deleted Composer viewer routes,
stores, worker names, v0/v1 schemas, and an obsolete process topology.

Those hard-coded rules must be recreated at I9/I against the actual new
observer/client boundaries; silently checking missing historical paths would
be false qualification.  The reusable responsibility is only the bounded,
effect-free act of applying caller-supplied regex/token rules to caller-
supplied source text, which remains here.  The CLI fails before reading a
single path.  The historical rules and exact source remain the restoration
reference rather than dormant executable code.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import re
import sys
from typing import Final, NoReturn


RUNTIME_UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
VIEWER_BOUNDARY_RESTORATION_GATES: Final = ("I9", "I")

MAX_SOURCE_COUNT: Final = 4_096
MAX_SOURCE_CHARS: Final = 8 * 1024 * 1024
MAX_TOTAL_SOURCE_CHARS: Final = 64 * 1024 * 1024
MAX_RULE_COUNT: Final = 4_096
MAX_RULE_TEXT_CHARS: Final = 4_096


class ViewerBoundaryCheckUnavailableError(RuntimeError):
    """Raised before any repository source is opened or traversed."""


class ViewerBoundaryBoundsError(ValueError):
    """Raised when a caller supplies an unbounded source/rule corpus."""


def _raise_viewer_boundary_unavailable() -> NoReturn:
    raise ViewerBoundaryCheckUnavailableError(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: qualified viewer boundary rules "
        "return with the observer/client architecture at I9/I"
    )


def _validate_relative_path(value: str) -> None:
    if not isinstance(value, str):
        raise TypeError("source path must be a string")
    if not value or len(value) > MAX_RULE_TEXT_CHARS:
        raise ValueError(
            f"source path must contain 1..{MAX_RULE_TEXT_CHARS} characters"
        )
    parts = value.replace("\\", "/").split("/")
    if value.startswith(("/", "\\")) or ".." in parts:
        raise ValueError("source path must be repository-relative")


def _validate_rule_text(value: str, *, field_name: str) -> None:
    if not isinstance(value, str):
        raise TypeError(f"{field_name} must be a string")
    if not value or len(value) > MAX_RULE_TEXT_CHARS:
        raise ValueError(
            f"{field_name} must contain 1..{MAX_RULE_TEXT_CHARS} characters"
        )


@dataclass(frozen=True, slots=True)
class ForbiddenSourcePattern:
    """One forbidden regex for one already-provided source value."""

    relative_path: str
    pattern: str
    description: str
    require_source: bool = True

    def __post_init__(self) -> None:
        _validate_relative_path(self.relative_path)
        _validate_rule_text(self.pattern, field_name="pattern")
        _validate_rule_text(self.description, field_name="description")
        if not isinstance(self.require_source, bool):
            raise TypeError("require_source must be a boolean")
        try:
            re.compile(self.pattern)
        except re.error as exc:
            raise ValueError(f"invalid forbidden regex: {exc}") from exc


@dataclass(frozen=True, slots=True)
class RequiredSourceToken:
    """One literal architecture token required in one source value."""

    relative_path: str
    token: str
    description: str
    require_source: bool = True

    def __post_init__(self) -> None:
        _validate_relative_path(self.relative_path)
        _validate_rule_text(self.token, field_name="token")
        _validate_rule_text(self.description, field_name="description")
        if not isinstance(self.require_source, bool):
            raise TypeError("require_source must be a boolean")


def evaluate_source_boundaries(
    sources: Mapping[str, str],
    *,
    required_paths: Sequence[str] = (),
    forbidden_patterns: Sequence[ForbiddenSourcePattern] = (),
    required_tokens: Sequence[RequiredSourceToken] = (),
) -> list[str]:
    """Evaluate bounded source values without filesystem or process access."""

    if len(sources) > MAX_SOURCE_COUNT:
        raise ViewerBoundaryBoundsError(
            f"sources exceeds {MAX_SOURCE_COUNT} entries"
        )
    if len(required_paths) > MAX_RULE_COUNT:
        raise ViewerBoundaryBoundsError(
            f"required_paths exceeds {MAX_RULE_COUNT} entries"
        )
    if len(forbidden_patterns) > MAX_RULE_COUNT:
        raise ViewerBoundaryBoundsError(
            f"forbidden_patterns exceeds {MAX_RULE_COUNT} entries"
        )
    if len(required_tokens) > MAX_RULE_COUNT:
        raise ViewerBoundaryBoundsError(
            f"required_tokens exceeds {MAX_RULE_COUNT} entries"
        )

    normalized: dict[str, str] = {}
    total_chars = 0
    for relative_path, source in sources.items():
        _validate_relative_path(relative_path)
        if not isinstance(source, str):
            raise TypeError(f"source for {relative_path} must be text")
        if len(source) > MAX_SOURCE_CHARS:
            raise ViewerBoundaryBoundsError(
                f"source {relative_path} exceeds {MAX_SOURCE_CHARS} characters"
            )
        total_chars += len(source)
        if total_chars > MAX_TOTAL_SOURCE_CHARS:
            raise ViewerBoundaryBoundsError(
                f"source corpus exceeds {MAX_TOTAL_SOURCE_CHARS} characters"
            )
        normalized[relative_path] = source

    failures: list[str] = []
    for relative_path in required_paths:
        _validate_relative_path(relative_path)
        if relative_path not in normalized:
            failures.append(f"missing required viewer source: {relative_path}")

    for rule in forbidden_patterns:
        if not isinstance(rule, ForbiddenSourcePattern):
            raise TypeError(
                "forbidden_patterns must contain ForbiddenSourcePattern values"
            )
        source = normalized.get(rule.relative_path)
        if source is None:
            if rule.require_source:
                failures.append(f"missing checked viewer source: {rule.relative_path}")
            continue
        if re.search(rule.pattern, source):
            failures.append(
                f"{rule.relative_path}: contains {rule.description} "
                f"({rule.pattern})"
            )

    for rule in required_tokens:
        if not isinstance(rule, RequiredSourceToken):
            raise TypeError(
                "required_tokens must contain RequiredSourceToken values"
            )
        source = normalized.get(rule.relative_path)
        if source is None:
            if rule.require_source:
                failures.append(f"missing checked viewer source: {rule.relative_path}")
            continue
        if rule.token not in source:
            failures.append(
                f"{rule.relative_path}: missing {rule.description} ({rule.token})"
            )
    return failures


def require_viewer_boundary_check(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Typed gate for callers that would scan the live repository."""

    _raise_viewer_boundary_unavailable()


def main(_argv: list[str] | None = None) -> int:
    print(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: historical viewer boundary "
        "scanning returns at I9/I",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "ForbiddenSourcePattern",
    "RUNTIME_UNAVAILABLE_REASON_CODE",
    "RequiredSourceToken",
    "VIEWER_BOUNDARY_RESTORATION_GATES",
    "ViewerBoundaryBoundsError",
    "ViewerBoundaryCheckUnavailableError",
    "evaluate_source_boundaries",
    "require_viewer_boundary_check",
]
