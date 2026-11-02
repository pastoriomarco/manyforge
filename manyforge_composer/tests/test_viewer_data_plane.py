"""Current viewer primitives plus the historical v1 data-plane disposition.

Historical blob ``8362ddb488892bf00ad0659828061abf834f85d6`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains 18 cases.
Render-content reduction and bounded demand mechanics remain payload-neutral
and are tested below.  Generic capacity-one fanout/envelope behavior is also
covered by ``test_viewer_transport_primitives.py``.  Manifest/frame semantics,
resynchronization, diagnostics, and qualification-delay wire behavior return
only with the I9/I observer contract.
"""

from __future__ import annotations

from collections.abc import Iterator
from typing import Final

import pytest

from manyforge_composer.backend.viewer_data_plane import (
    ViewerDemandRegistry,
    freeze_viewer_render_signature,
    viewer_effective_period_s,
)


HISTORICAL_CASE_COUNT: Final = 18
HISTORICAL_CASE_FAMILIES: Final = (
    (
        "v1_manifest_frame_and_encoder_contract",
        "historical lines 33 and 223-254",
        3,
        ("I9", "I"),
        "strict v1 fixture/order, manifest revision reuse, self-contained "
        "frames, and suppression of evidence-time-only publications",
    ),
    (
        "render_content_signature",
        "historical line 69",
        1,
        (),
        "retained below without a viewer wire or runtime owner",
    ),
    (
        "bounded_viewer_demand",
        "historical lines 135-208",
        5,
        (),
        "retained below and in test_viewer_transport_primitives.py",
    ),
    (
        "bounded_fanout_and_v1_resynchronization",
        "historical lines 310-567",
        9,
        ("I9", "I"),
        "generic latest-slot/send bounds remain current; v1 manifest resync, "
        "per-client delay, diagnostics, epoch and cached-generation targeting "
        "return with the new observer-owned protocol",
    ),
)


class _FakeClock:
    def __init__(self) -> None:
        self.now_ns = 1_000_000_000

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, seconds: float) -> None:
        self.now_ns += int(seconds * 1e9)


def test_render_signature_ignores_time_but_retains_pose_and_status() -> None:
    first = {
        "status": "streaming",
        "received_at_ns": 100,
        "objects": [
            {
                "object_id": "target",
                "age_s": 0.01,
                "update": {
                    "source_sequence": 1,
                    "source_timestamp_ns": 90,
                    "position_m": [0.1, 0.2, 0.3],
                },
            }
        ],
    }
    later_same_pose = {
        **first,
        "received_at_ns": 200,
        "objects": [
            {
                **first["objects"][0],
                "age_s": 0.02,
                "update": {
                    **first["objects"][0]["update"],
                    "source_sequence": 2,
                    "source_timestamp_ns": 190,
                },
            }
        ],
    }

    signature = freeze_viewer_render_signature(first)
    assert signature == freeze_viewer_render_signature(later_same_pose)

    moved = {
        **later_same_pose,
        "objects": [
            {
                **later_same_pose["objects"][0],
                "update": {
                    **later_same_pose["objects"][0]["update"],
                    "position_m": [0.2, 0.2, 0.3],
                },
            }
        ],
    }
    assert signature != freeze_viewer_render_signature(moved)
    assert signature != freeze_viewer_render_signature(
        {**later_same_pose, "status": "stale"}
    )


def test_demand_leases_clamp_expire_and_hide_without_runtime_state() -> None:
    clock = _FakeClock()
    registry = ViewerDemandRegistry(
        lease_timeout_s=5.0,
        disconnect_grace_s=0.25,
        max_rate_hz=20.0,
        clock_ns=clock,
    )
    summary = registry.attach(
        "visible",
        visibility="focused",
        requested_rate_hz=100.0,
        layers=("geometry",),
    )
    assert summary.active is True
    assert summary.requested_rate_hz == 20.0
    assert summary.layers == frozenset({"geometry"})

    registry.attach(
        "hidden",
        visibility="hidden",
        requested_rate_hz=20.0,
        layers=("diagnostics",),
    )
    assert registry.summary().active_client_count == 1

    registry.detach("visible")
    assert registry.summary().active is True
    clock.advance(0.3)
    assert registry.summary().active is False
    clock.advance(5.0)
    assert registry.summary().client_count == 0


def test_demand_rejects_invalid_visibility_rate_and_layer_values() -> None:
    class PoisonLayer:
        def __str__(self) -> str:
            raise AssertionError("arbitrary layer stringification attempted")

    registry = ViewerDemandRegistry()

    with pytest.raises(ValueError, match="visibility"):
        registry.attach("invalid-visibility", visibility="background")
    with pytest.raises(ValueError, match="finite and non-negative"):
        registry.attach("negative-rate", requested_rate_hz=-0.1)
    for rate in (float("nan"), float("inf")):
        with pytest.raises(ValueError, match="finite and non-negative"):
            registry.attach("nonfinite-rate", requested_rate_hz=rate)
    with pytest.raises(ValueError, match="portable tokens"):
        registry.attach("invalid-layer", layers=("Not Portable",))
    with pytest.raises(TypeError, match="only strings"):
        registry.attach("non-string-layer", layers=(PoisonLayer(),))


def test_demand_accepts_a_finite_generator_at_the_layer_limit() -> None:
    produced: list[int] = []

    def layers() -> Iterator[str]:
        for index in range(32):
            produced.append(index)
            yield f"layer_{index}"

    summary = ViewerDemandRegistry().attach("observer", layers=layers())

    assert produced == list(range(32))
    assert summary.layers == frozenset(
        f"layer_{index}" for index in range(32)
    )


def test_demand_stops_a_layer_generator_at_the_first_over_limit_item() -> None:
    produced: list[int] = []

    def layers() -> Iterator[str]:
        for index in range(1_000):
            produced.append(index)
            yield f"layer_{index}"

    with pytest.raises(ValueError, match="exceeds 32 entries"):
        ViewerDemandRegistry().attach("observer", layers=layers())

    assert produced == list(range(33))


def test_requested_rate_only_reduces_the_server_sampling_ceiling() -> None:
    assert viewer_effective_period_s(
        fastest_period_s=1.0 / 15.0,
        requested_rate_hz=5.0,
    ) == pytest.approx(0.2)
    assert viewer_effective_period_s(
        fastest_period_s=1.0 / 15.0,
        requested_rate_hz=30.0,
    ) == pytest.approx(1.0 / 15.0)
    assert viewer_effective_period_s(
        fastest_period_s=1.0 / 15.0,
        requested_rate_hz=0.0,
    ) is None


def test_historical_viewer_data_plane_cases_are_split_without_loss() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 18
    retained = {family[0] for family in HISTORICAL_CASE_FAMILIES if not family[3]}
    assert retained == {"render_content_signature", "bounded_viewer_demand"}
