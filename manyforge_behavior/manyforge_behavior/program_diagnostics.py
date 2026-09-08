"""Bounded copies of Program-owned facts, never a runtime-state authority."""

from __future__ import annotations

from collections.abc import Mapping
from itertools import islice
from typing import Any

from .canonical_artifact import canonical_json_bytes


def diagnostic_values(rows, *, total_count: int) -> dict[str, Any]:
    """Copy small complete values; identify omissions without clipping a value.

    The caller holds its existing value-store lock. Traversal, strings and the
    total serialized result have fixed budgets, independent of Program size.
    No oversized value is copied/serialized before its traversal is bounded.
    """
    values: dict[str, Any] = {}
    omitted = max(0, total_count - 32)
    budget = 16 * 1024
    for key, value in islice(rows, 32):
        remaining = 512
        # Per-value traversal is independent of key order. The exact aggregate
        # size is debited below; canonical wire ordering cannot change validity.
        remaining_bytes = 16 * 1024 - 2

        def copy(item, depth=0):
            nonlocal remaining, remaining_bytes
            remaining -= 1
            # Conservative separators/brackets, debited before walking children.
            remaining_bytes -= 2
            if remaining < 0 or depth > 16:
                raise ValueError("diagnostic traversal budget")
            if remaining_bytes < 0:
                raise ValueError("diagnostic byte budget")
            if isinstance(item, str):
                if len(item) > 2048 or len(item.encode("utf-8")) > 2048:
                    raise ValueError("diagnostic string budget")
            elif isinstance(item, Mapping):
                return {
                    copy(name, depth + 1): copy(nested, depth + 1) for name, nested in item.items()
                }
            elif isinstance(item, (tuple, list)):
                return [copy(nested, depth + 1) for nested in item]
            elif item is not None and type(item) not in (bool, int, float):
                raise ValueError("diagnostic value is not portable JSON")
            # A scalar is itself bounded above; never serialize an unbounded
            # compound value just to discover it exceeds the capture budget.
            remaining_bytes -= len(canonical_json_bytes(item))
            if remaining_bytes < 0:
                raise ValueError("diagnostic byte budget")
            return item

        try:
            projected = {copy(key): copy(value)}
            size = len(canonical_json_bytes(projected))
            if size > budget:
                raise ValueError("diagnostic byte budget")
        except (ValueError, TypeError):
            omitted += 1
            continue
        values.update(projected)
        budget -= size
    return {"values": values, "omitted_count": omitted}
