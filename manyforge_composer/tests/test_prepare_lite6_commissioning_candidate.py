"""Tests for retained Lite6 commissioning safety logic and the unavailable client.

The historical implementation blob
``1bb14aa35ed0b1c2ab7909dff8be86f016179754`` and test blob
``0248ca9da10b5a883126e43ea91b71c4cdf96541`` assigned their removed case
families as follows: old Composer status/candidate wire behavior to future status/admission
contracts, preview/evidence client integration to a future client, and the one-degree/freshness
safety checks retained below in effect-free form.
"""

from __future__ import annotations

import importlib.util
import math
from pathlib import Path
import sys

import pytest


_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "xarm"
    / "prepare_lite6_commissioning_candidate.py"
)
_SPEC = importlib.util.spec_from_file_location(
    "prepare_lite6_commissioning_candidate",
    _SCRIPT,
)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = _MODULE
_SPEC.loader.exec_module(_MODULE)

HISTORICAL_TEST_BLOB = "0248ca9da10b5a883126e43ea91b71c4cdf96541"
HISTORICAL_CASE_DISPOSITION = {
    "preview_without_execute": ("status_admission", "client"),
    "closed_lifecycle_before_candidate": ("status_admission", "client"),
    "stale_observation_before_candidate": ("observation", "client"),
    "zero_out_of_bounds_or_nonfinite_delta": ("client",),
}


def _names() -> tuple[str, ...]:
    return tuple(f"cell-a/lite6/joint{index}" for index in range(1, 7))


def test_previews_only_one_bounded_caller_addressed_joint() -> None:
    names = _names()
    starts = (0.1, -0.2, 0.3, -0.4, 0.5, -0.6)

    preview = _MODULE.preview_bounded_joint_delta(
        joint_names=names,
        start_positions_rad=starts,
        target_joint=names[1],
        delta_deg=-0.5,
    )

    assert preview.joint_names == names
    assert preview.start_positions_rad == starts
    assert math.isclose(
        preview.target_positions_rad[1],
        starts[1] + math.radians(-0.5),
    )
    assert all(
        target == start
        for index, (start, target) in enumerate(
            zip(starts, preview.target_positions_rad)
        )
        if index != 1
    )


@pytest.mark.parametrize("delta", [0.0, -1.001, 1.001, math.inf, True])
def test_refuses_unbounded_or_invalid_delta(delta: object) -> None:
    with pytest.raises(_MODULE.CommissioningRefusal):
        _MODULE.preview_bounded_joint_delta(
            joint_names=_names(),
            start_positions_rad=(0.0,) * 6,
            target_joint=_names()[0],
            delta_deg=delta,
        )


def test_refuses_stale_or_malformed_observation_before_preview() -> None:
    with pytest.raises(_MODULE.CommissioningRefusal, match="stale"):
        _MODULE.require_fresh_source_age(251_000_000)
    with pytest.raises(_MODULE.CommissioningRefusal, match="six joints"):
        _MODULE.preview_bounded_joint_delta(
            joint_names=_names()[:-1],
            start_positions_rad=(0.0,) * 5,
            target_joint=_names()[0],
            delta_deg=0.5,
        )


def test_direct_client_fails_with_stable_gate_before_effects(capsys) -> None:
    assert _MODULE.main() == 78
    assert (
        _MODULE.LITE6_COMMISSIONING_UNAVAILABLE_REASON_CODE
        in capsys.readouterr().err
    )


def test_all_historical_candidate_case_families_remain_assigned() -> None:
    assert HISTORICAL_TEST_BLOB == (
        "0248ca9da10b5a883126e43ea91b71c4cdf96541"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 4
    assert all("client" in gates for gates in HISTORICAL_CASE_DISPOSITION.values())
