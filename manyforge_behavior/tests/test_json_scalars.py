import pytest

from manyforge_behavior.json_scalars import (
    MAXIMUM_INT64,
    MAXIMUM_SAFE_INTEGER,
    nonnegative_int64,
    nonnegative_int64_value,
)


@pytest.mark.parametrize("value", [0, MAXIMUM_SAFE_INTEGER, MAXIMUM_SAFE_INTEGER + 1,
                                  MAXIMUM_SAFE_INTEGER + 2, 105 * 86400 * 10**9,
                                  31 * 366 * 86400 * 10**9, MAXIMUM_INT64])
def test_exact_ns_and_legacy_bytes(value):
    wire = nonnegative_int64_value(value)
    assert nonnegative_int64(wire) == value
    assert (type(wire) is int) == (value <= MAXIMUM_SAFE_INTEGER)


@pytest.mark.parametrize("value", [True, False, -1, 0.5, 1.0, MAXIMUM_SAFE_INTEGER + 1,
                                  "", "00", "+1", "-1", "1e3", "１", str(MAXIMUM_INT64 + 1)])
def test_invalid_or_potentially_rounded_ns_are_rejected(value):
    with pytest.raises(ValueError):
        nonnegative_int64(value)
