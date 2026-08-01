"""Unavailable boundary for the removed supervised Lite6 runtime resolver.

The historical resolver mixed otherwise retained model/controller inputs with
the obsolete singleton runtime schema, caller-selected authority profiles,
authority leases/heartbeats, the experimental execution-admission wire, and
filesystem publication.  The model and controller compilers remain in their
own modules.  Literal usable-unicast IPv4 validation remains below as a pure
commissioning/client check; it owns no runtime identity or transport.

Exact source is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``: source blob
``7d72defbfb7e6cf394be02891f08fc6c921e1068`` and three historical test
cohorts in blob ``9abbb0d1321a7ec1ff9a37da814331386ab099bc``.  Those cohorts
pin runtime-value rejection, model/controller preservation, and template-drift
rejection for future retained products and the real-Lite6 lane.

Retained artifacts return through I3; component/resource addressing through
I6/N; caller-neutral lifecycle and Kernel admission through I7/L0 and I8/P5;
and the focused xArm runtime client through I10/R.  The v0 experimental
runtime and admission schemas are never restored.
"""

from __future__ import annotations

import ipaddress
import sys
from typing import Final


UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATES: Final = (
    "I3",
    "I6/N",
    "I7/L0",
    "I8/P5",
    "I10/R",
)
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: the old supervised Lite6 runtime resolver "
    "was removed; retained products and new runtime owners must pass I3, "
    "I6/N, I7/L0, I8/P5, and I10/R"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "7d72defbfb7e6cf394be02891f08fc6c921e1068"
)


def validate_robot_ipv4(value: str) -> str:
    """Return one canonical usable-unicast IPv4 literal without I/O."""

    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise ValueError("robot_ip must be one literal IPv4 address") from exc
    if (
        address.is_unspecified
        or int(address) >> 24 == 0
        or address.is_loopback
        or address.is_multicast
        or int(address) == (1 << 32) - 1
    ):
        raise ValueError("robot_ip must be a usable unicast IPv4 address")
    return str(address)


def main() -> int:
    """Fail before argument parsing, imports, reads, writes, ROS, or hardware."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
