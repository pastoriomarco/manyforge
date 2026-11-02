"""Typed I9 gate for the former Isaac collider-service extension.

The full extension remains exact historical input at ManyForge commit
08222188293df8e12f90dc72e204c21f22f92ea1, blob
fa0b812d0059793ca9f142d5aa25fc9ad4e9bb62.  Its Trigger/String RPC,
mesh-transfer topics, request queues, ROS executor/thread, and Kit update-loop
ownership do not remain as a dormant compatibility surface.  The exclusive
v0.1 query/mesh-request parser is historical source blob
395d16c3144186745882e9b09bc797fd63e601f7; its seven test definitions and 16
expanded cases are assigned to the I9/I acceptance gate in
``tests/test_isaac_collider_service_ros_contract.py``.  The separate historical
live probe is blob ``380c7dc4ff873aa510290b5f6df5a080e9698ab0``; only its
owner-neutral recommendation, convex-normalization, and primitive-OBJ helpers
remain in ``scripts/isaac_collider_probe_lib.py``.
"""

from __future__ import annotations

import omni.ext


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: the Isaac collider adapter requires the "
    f"{RESTORATION_GATE} retained-product and focused transport gate"
)


class ManyForgeIsaacColliderServiceExtension(omni.ext.IExt):
    """Reject activation before probe imports, ROS setup, threads, or Kit I/O."""

    def on_startup(self, ext_id: str) -> None:
        del ext_id
        raise RuntimeError(UNAVAILABLE_MESSAGE)

    def on_shutdown(self) -> None:
        pass


__all__ = ["ManyForgeIsaacColliderServiceExtension"]
