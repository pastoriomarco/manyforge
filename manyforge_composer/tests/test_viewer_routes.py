"""Disposition contract for the historical Composer viewer WebSocket routes.

Historical blob ``862b3db72aa7608c6203be6170185fa8d9b1b63e`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains seven cases.
The route is now a fail-before-state I9 gate; no MockKernel, ComposerState,
v1 DTO, auth-wire, or qualification-delay fixture is retained here.
"""

from typing import Final

from manyforge_composer.backend.routes_viewer import (
    VIEWER_CLIENT_RESTORATION_GATE,
)


HISTORICAL_CASE_FAMILIES: Final = (
    (
        "lease_watchdog",
        "historical line 81",
        1,
        ("I9", "I"),
        "expired client demand closes boundedly",
    ),
    (
        "manifest_frame_control_and_auth_wire",
        "historical lines 112-178",
        3,
        ("I9", "I"),
        "manifest-before-frame order, clamped lease acknowledgement, strict "
        "control schema, and transport authentication projection",
    ),
    (
        "qualification_delay_and_client_diagnostics",
        "historical lines 197-229",
        3,
        ("I9", "I"),
        "delay injection rejects by default, is explicitly gated per client, "
        "and client-reported windows remain bounded",
    ),
)


def test_viewer_route_cases_are_fully_assigned_to_the_i9_gate() -> None:
    assert VIEWER_CLIENT_RESTORATION_GATE == "I9"
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 7
