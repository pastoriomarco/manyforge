"""Bounded Isaac stop-topic translation and unavailable I9 entrypoint.

The former node combined an old Kernel enable topic with an independent
DemoSupervisor enable topic and treated an absent supervisor as enabled.  That
two-owner/default-open admission policy is deliberately removed: I9 must
consume one owner-issued command-admission fact.  The exact historical node is
preserved at ManyForge commit 08222188293df8e12f90dc72e204c21f22f92ea1,
blob 3519ee5ad8ebbe83ace6686be7fca02f967b81fc, as I8/I9 acceptance input for
loss, restart, freshness, and fail-closed behavior.
"""

from __future__ import annotations

import sys


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: the Isaac topic command adapter requires the "
    f"{RESTORATION_GATE} owner-issued command-admission gate"
)


def stop_topic_value(*, owner_command_enabled: bool) -> bool:
    """Translate one owner-issued enable fact to the vendor stop-topic wire."""

    if type(owner_command_enabled) is not bool:
        raise TypeError("owner_command_enabled must be an exact bool")
    return not owner_command_enabled


def main() -> int:
    """Fail before ROS import, initialization, subscription, or publication."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
