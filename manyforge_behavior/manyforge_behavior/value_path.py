"""Program value paths: the keys of a node's ``value_sources``.

A path names one value inside a node's parameters, for example
``goal.position_m[2]``. There is exactly one spelling per path, shared with
the C++ Program contract and the Composer editor through
``manyforge_deployment/resources/expression/value_path_conformance.v1.yaml``.
"""

from __future__ import annotations

from typing import Any, Final, Union

MAX_PATH_LENGTH: Final = 1024
MAX_PATH_SEGMENTS: Final = 33
MAX_NAME_LENGTH: Final = 256
MAX_INDEX_DIGITS: Final = 9

Segment = Union[str, int]


class ValuePathError(ValueError):
    """A path is not in the one canonical grammar."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _is_name_start(char: str) -> bool:
    return "a" <= char <= "z"


def _is_name_char(char: str) -> bool:
    return "a" <= char <= "z" or "0" <= char <= "9" or char in "_-"


def parse_value_path(text: str) -> tuple[Segment, ...]:
    """Return the segments of ``text``: names as ``str``, indices as ``int``."""

    if type(text) is not str:
        raise ValuePathError("path_syntax_invalid", "value path must be text")
    if not text:
        raise ValuePathError("path_empty", "value path is empty")
    if len(text) > MAX_PATH_LENGTH:
        raise ValuePathError(
            "path_too_long", f"value path is longer than {MAX_PATH_LENGTH} characters"
        )
    segments: list[Segment] = []
    position = 0

    def name() -> str:
        nonlocal position
        start = position
        if position >= len(text) or not _is_name_start(text[position]):
            raise ValuePathError(
                "path_name_invalid",
                f"value path {text!r} needs a lower-case name at character {start}",
            )
        while position < len(text) and _is_name_char(text[position]):
            position += 1
        if position - start > MAX_NAME_LENGTH:
            raise ValuePathError(
                "path_name_invalid",
                f"value path {text!r} has a name longer than {MAX_NAME_LENGTH}",
            )
        return text[start:position]

    def index() -> int:
        nonlocal position
        start = position
        while position < len(text) and "0" <= text[position] <= "9":
            position += 1
        digits = text[start:position]
        if (
            not digits
            or (len(digits) > 1 and digits[0] == "0")
            or len(digits) > MAX_INDEX_DIGITS
            or position >= len(text)
            or text[position] != "]"
        ):
            raise ValuePathError(
                "path_index_invalid",
                f"value path {text!r} needs a decimal index without leading "
                f"zeros at character {start}",
            )
        position += 1
        return int(digits)

    def append(segment: Segment) -> None:
        if len(segments) == MAX_PATH_SEGMENTS:
            raise ValuePathError(
                "path_too_deep",
                f"value path {text!r} has more than {MAX_PATH_SEGMENTS} segments",
            )
        segments.append(segment)

    append(name())
    while position < len(text):
        separator = text[position]
        position += 1
        if separator == ".":
            append(name())
        elif separator == "[":
            append(index())
        else:
            raise ValuePathError(
                "path_syntax_invalid",
                f"value path {text!r} has an unexpected character at {position - 1}",
            )
    return tuple(segments)


def format_value_path(segments: tuple[Segment, ...] | list[Segment]) -> str:
    """Spell ``segments`` canonically; the inverse of :func:`parse_value_path`."""

    if not segments or type(segments[0]) is not str:
        raise ValuePathError("path_name_invalid", "a value path starts with a name")
    text = segments[0]
    for segment in segments[1:]:
        text += f"[{segment}]" if type(segment) is int else f".{segment}"
    if parse_value_path(text) != tuple(segments):
        raise ValuePathError("path_syntax_invalid", f"{segments!r} is not a value path")
    return text


def path_covers(outer: tuple[Segment, ...], inner: tuple[Segment, ...]) -> bool:
    """True when ``inner`` is ``outer`` or a value inside it."""

    return inner[: len(outer)] == outer


def value_at_path(value: Any, segments: tuple[Segment, ...]) -> Any:
    """Return the value below the parameter at ``segments[1:]``, or raise KeyError."""

    current = value
    for segment in segments[1:]:
        if type(segment) is int:
            if not isinstance(current, (list, tuple)) or segment >= len(current):
                raise KeyError(segment)
            current = current[segment]
        else:
            if not isinstance(current, dict) or segment not in current:
                raise KeyError(segment)
            current = current[segment]
    return current


def replace_at_path(value: Any, segments: tuple[Segment, ...], replacement: Any) -> Any:
    """Set the value at ``segments[1:]`` of a mutable JSON value; returns the root.

    A final name may add an absent entry to an object: the Program validator
    admits that only for map entries, which a value source may supply alone.
    """

    if len(segments) == 1:
        return replacement
    parent = value_at_path(value, segments[:-1])
    last = segments[-1]
    if type(last) is int:
        if not isinstance(parent, list) or last >= len(parent):
            raise KeyError(last)
    elif not isinstance(parent, dict):
        raise KeyError(last)
    parent[last] = replacement
    return value


__all__ = [
    "MAX_INDEX_DIGITS",
    "MAX_NAME_LENGTH",
    "MAX_PATH_LENGTH",
    "MAX_PATH_SEGMENTS",
    "Segment",
    "ValuePathError",
    "format_value_path",
    "parse_value_path",
    "path_covers",
    "replace_at_path",
    "value_at_path",
]
