"""Retained-value and disposition tests for the viewer client fixture.

Historical blob ``a51df86d3aabb0152351fed5c8a746f4556c8720`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains four cases.
The two socket/lease cases return at I9/I.  The two URL transformations are
protocol-neutral value behavior and remain executable now; using them still
does not make the old viewer-v1 client available.
"""

from __future__ import annotations

import asyncio
import importlib.util
from pathlib import Path
import sys
from typing import Any, Final

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "scripts" / "qualification" / "viewer_client_fixture.py"
_SPEC = importlib.util.spec_from_file_location("viewer_client_fixture_test", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
fixture = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = fixture
_SPEC.loader.exec_module(fixture)


HISTORICAL_CASE_FAMILIES: Final = (
    (
        "lease_renewal_and_frame_consumption",
        "historical lines 53 and 64",
        2,
        ("I9", "I"),
        "normal and deliberately slow clients renew bounded leases and consume "
        "frames through the future observer-owned wire",
    ),
    (
        "bearer_query_preservation",
        "historical line 75",
        1,
        (),
        "adding a token preserves existing query parameters",
    ),
    (
        "qualification_delay_url_projection",
        "historical line 82",
        1,
        (),
        "slow-client delay selection remains explicit value-level configuration; "
        "the server-side gate itself returns at I9/I",
    ),
)


def _config(mode: str) -> Any:
    return fixture.FixtureConfig(
        url="ws://example.test/ws/scene/viewer",
        mode=mode,
        duration_s=0.35,
        renew_interval_s=0.1,
        requested_rate_hz=15.0,
        layers=("canonical", "diagnostics"),
        qualification_send_delay_ms=250.0,
    )


def test_viewer_client_fixture_cases_are_fully_disposed() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 4
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES if family[3]) == 2
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES if not family[3]) == 2


def test_token_is_added_without_discarding_existing_query() -> None:
    assert (
        fixture._authenticated_url("ws://host/ws?existing=1", "secret")
        == "ws://host/ws?existing=1&token=secret"
    )


def test_slow_fixture_projects_the_explicit_qualification_delay() -> None:
    assert (
        fixture._fixture_url(_config("slow"))
        == "ws://example.test/ws/scene/viewer?qualification_send_delay_ms=250"
    )


def test_fixture_configuration_is_bounded_before_any_client_exists() -> None:
    with pytest.raises(ValueError, match="renew_interval_s"):
        fixture.FixtureConfig(
            url="ws://example.test/ws/scene/viewer",
            mode="normal",
            duration_s=1.0,
            renew_interval_s=15.0,
            requested_rate_hz=15.0,
            layers=("canonical",),
        )


def test_network_fixture_is_a_typed_i9_i_gate() -> None:
    with pytest.raises(
        fixture.ViewerFixtureUnavailableError,
        match="returns at I9/I",
    ):
        asyncio.run(fixture.run_fixture(_config("normal")))
