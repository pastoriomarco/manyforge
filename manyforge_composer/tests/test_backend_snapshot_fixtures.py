"""The committed backend snapshot contract fixtures match their producers.

The frontend parses these files with its strict parsers, so a stale file
would let a backend wire change escape that check.
"""

from __future__ import annotations

import pytest

from manyforge_composer.tests.backend_snapshot_fixtures import (
    FIXTURE_DIR,
    FIXTURES,
    render,
    _stabilize,
)


def test_fixture_directory_holds_exactly_the_generated_files() -> None:
    assert sorted(path.name for path in FIXTURE_DIR.glob("*.json")) == sorted(FIXTURES)


@pytest.mark.parametrize("name", sorted(FIXTURES))
def test_committed_fixture_matches_the_backend_producer(name: str) -> None:
    committed = (FIXTURE_DIR / name).read_text(encoding="utf-8")
    assert committed == render(FIXTURES[name]), (
        f"{name} is stale: run "
        "`python -m manyforge_composer.tests.backend_snapshot_fixtures --write` "
        "and then the frontend vitest suite"
    )


@pytest.mark.parametrize("value", [True, False, 1.5, -1, 9_007_199_254_740_992,
                                  "garbage", "01", "-1", "9223372036854775808",
                                  "18446744073709551615", "18446744073709551616"])
@pytest.mark.parametrize("key", ["generated_at_ns"])
def test_timestamp_normalization_preserves_invalid_wire_values(key, value):
    result = _stabilize({key: value})[key]
    assert type(result) is type(value)
    assert result == value


@pytest.mark.parametrize("value", [0, 9_007_199_254_740_991, "0", "9223372036854775807"])
@pytest.mark.parametrize("key", ["generated_at_ns"])
def test_timestamp_normalization_replaces_valid_wire_values(key, value):
    result = _stabilize({key: value})[key]
    assert type(result) is type(value)
    assert result == ("1000000000" if isinstance(value, str) else 1_000_000_000)
