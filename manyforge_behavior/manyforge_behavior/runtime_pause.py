"""Bounded diagnostic sanitation and the pause cutover gate.

The former registry minted live runtime-fault identities, generations, resume
grants, and mutable pause state for the Python CycleManager.  With no current
resolver consumer, that was a dormant recovery owner.  Exact pause/recovery
identity returns with the native Program Binding and unified recovery owner.
The JSON-safe diagnostic sanitizer is transport-neutral and remains active.
"""

from __future__ import annotations

from collections.abc import Mapping
import math
from typing import Any, Final, Optional


_MAX_DETAIL_ITEMS: Final = 64
_MAX_NESTED_DETAIL_ITEMS: Final = 32
_MAX_DETAIL_DEPTH: Final = 6
_MAX_DETAIL_NODES: Final = 256
_MAX_DETAIL_STRING_CHARS: Final = 1024
_MAX_DETAIL_TEXT_CHARS: Final = 16_384
_MAX_DETAIL_KEY_CHARS: Final = 128
_DETAILS_TRUNCATED_KEY: Final = "_manyforge_details_truncated"
RUNTIME_PAUSE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_runtime_pause_owner_unavailable"
)


class _DetailSanitizer:
    def __init__(self) -> None:
        self.remaining_nodes = _MAX_DETAIL_NODES
        self.remaining_text_chars = _MAX_DETAIL_TEXT_CHARS
        self.truncated = False

    def _text(self, value: str, *, limit: int = _MAX_DETAIL_STRING_CHARS) -> str:
        allowed = min(limit, self.remaining_text_chars)
        if len(value) > allowed:
            self.truncated = True
        result = value[:allowed]
        self.remaining_text_chars -= len(result)
        return result

    def _key(self, value: Any) -> str:
        if isinstance(value, str):
            text = value
        elif value is None or isinstance(value, (bool, int, float)):
            text = str(value)
        else:
            value_type = type(value)
            text = f"<key:{value_type.__module__}.{value_type.__qualname__}>"
        return self._text(text, limit=_MAX_DETAIL_KEY_CHARS)

    def sanitize(
        self,
        value: Any,
        *,
        depth: int = 0,
        ancestors: Optional[set[int]] = None,
    ) -> Any:
        if self.remaining_nodes <= 0:
            self.truncated = True
            return "<truncated>"
        self.remaining_nodes -= 1
        if value is None or isinstance(value, (bool, str)):
            return self._text(value) if isinstance(value, str) else value
        if isinstance(value, int):
            if value.bit_length() <= 256:
                return value
            self.truncated = True
            return "<integer_out_of_range>"
        if isinstance(value, float):
            if math.isfinite(value):
                return value
            self.truncated = True
            return f"<non_finite:{value}>"
        if depth >= _MAX_DETAIL_DEPTH:
            self.truncated = True
            return "<max_depth>"

        object_id = id(value)
        active_ancestors = ancestors if ancestors is not None else set()
        if object_id in active_ancestors:
            self.truncated = True
            return "<cycle>"

        if isinstance(value, Mapping):
            active_ancestors.add(object_id)
            result: dict[str, Any] = {}
            try:
                for index, (key, item) in enumerate(value.items()):
                    if index >= _MAX_NESTED_DETAIL_ITEMS:
                        self.truncated = True
                        break
                    if self.remaining_nodes <= 0 or self.remaining_text_chars <= 0:
                        self.truncated = True
                        break
                    result[self._key(key)] = self.sanitize(
                        item,
                        depth=depth + 1,
                        ancestors=active_ancestors,
                    )
            finally:
                active_ancestors.remove(object_id)
            return result

        if isinstance(value, (list, tuple)):
            active_ancestors.add(object_id)
            result_list: list[Any] = []
            try:
                for index, item in enumerate(value):
                    if index >= _MAX_NESTED_DETAIL_ITEMS:
                        self.truncated = True
                        break
                    if self.remaining_nodes <= 0 or self.remaining_text_chars <= 0:
                        self.truncated = True
                        break
                    result_list.append(
                        self.sanitize(
                            item,
                            depth=depth + 1,
                            ancestors=active_ancestors,
                        )
                    )
            finally:
                active_ancestors.remove(object_id)
            return result_list

        value_type = type(value)
        self.truncated = True
        return self._text(
            f"<unsupported:{value_type.__module__}.{value_type.__qualname__}>"
        )


def sanitize_runtime_diagnostics(
    details: Mapping[str, Any] | None,
) -> dict[str, Any]:
    """Return a bounded, cycle-safe, JSON-compatible diagnostic mapping."""
    if not details:
        return {}
    sanitizer = _DetailSanitizer()
    result: dict[str, Any] = {}
    for index, (key, value) in enumerate(details.items()):
        if index >= _MAX_DETAIL_ITEMS:
            sanitizer.truncated = True
            break
        if sanitizer.remaining_nodes <= 0 or sanitizer.remaining_text_chars <= 0:
            sanitizer.truncated = True
            break
        result[sanitizer._key(key)] = sanitizer.sanitize(value, depth=1)
    if sanitizer.truncated:
        if _DETAILS_TRUNCATED_KEY not in result and len(result) >= _MAX_DETAIL_ITEMS:
            result.pop(next(reversed(result)))
        result[_DETAILS_TRUNCATED_KEY] = True
    return result


class RuntimePauseOwnerUnavailableError(RuntimeError):
    """Raised before a node, registry, lock, clock, or recovery token is used."""


def _raise_runtime_pause_unavailable() -> None:
    raise RuntimePauseOwnerUnavailableError(
        f"{RUNTIME_PAUSE_UNAVAILABLE_REASON_CODE}: exact native pause and "
        "recovery identity is not available"
    )


class RuntimePauseRegistry:
    """Unavailable compatibility name; it owns no live pause tokens."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_pause_unavailable()


class RuntimePauseMixin:
    """Gate inherited by legacy effectful nodes until they are ported."""

    def _configure_runtime_pause(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_pause_unavailable()

    def _raise_runtime_pause(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_pause_unavailable()

    def _consume_runtime_resume(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_pause_unavailable()

    def _runtime_pause_active(self) -> bool:
        _raise_runtime_pause_unavailable()

    def _clear_runtime_pause(self) -> None:
        _raise_runtime_pause_unavailable()
