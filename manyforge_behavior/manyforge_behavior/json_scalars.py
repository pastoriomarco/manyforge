"""Lossless JSON scalars shared by owner records and thin API adapters.

No runtime/transport imports: using an exact scalar grants no clock authority.
"""

from __future__ import annotations

from typing import Any


MAXIMUM_INT64 = 9_223_372_036_854_775_807
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991


def nonnegative_int64(value: Any) -> int:
    """Decode safe legacy numbers or canonical decimal text without rounding."""
    if type(value) is int and 0 <= value <= MAXIMUM_SAFE_INTEGER:
        return value
    if (isinstance(value, str) and 1 <= len(value) <= 19 and value.isascii()
            and value.isdigit() and (len(value) == 1 or value[0] != "0")):
        parsed = int(value)
        if parsed <= MAXIMUM_INT64:
            return parsed
    raise ValueError("exact nonnegative int64 scalar required")


def nonnegative_int64_value(value: int) -> int | str:
    if type(value) is not int or not 0 <= value <= MAXIMUM_INT64:
        raise ValueError("nonnegative int64 required")
    return value if value <= MAXIMUM_SAFE_INTEGER else str(value)
