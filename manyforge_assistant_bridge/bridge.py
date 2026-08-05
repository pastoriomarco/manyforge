"""Unavailable provider bridge retained beside the completed H0 read client.

The former v0 HTTP loop mixed reusable bounded-assistant behavior with old
Composer callback, scene, Program, and catalog trust contracts. Its complete
interaction source is pinned at commit
``08222188293df8e12f90dc72e204c21f22f92ea1``. H0 restores only the closed
read client in ``headless.py``. Provider conversation, sanitation,
proposal-only, cancellation, timeout, turn-bound, stuck-loop, audit, and
Program-authoring behavior remain acceptance input for I6/N.
"""

from __future__ import annotations

import sys
from typing import NoReturn


ASSISTANT_BRIDGE_UNAVAILABLE_REASON_CODE = (
    "composable_cell_assistant_bridge_unavailable"
)
ASSISTANT_BRIDGE_RESTORATION_GATES = ("I6", "N")


class AssistantBridgeUnavailableError(RuntimeError):
    """The assistant bridge lacks the qualified I6/N provider boundary."""


def require_assistant_bridge() -> NoReturn:
    """Reject activation before network, model, callback, or audit effects."""
    raise AssistantBridgeUnavailableError(
        f"{ASSISTANT_BRIDGE_UNAVAILABLE_REASON_CODE}: assistant mediation "
        "and Program authoring return at I6/N; H0 read observations are "
        "available through manyforge_assistant_bridge.headless"
    )


def main() -> int:
    """Return the standard unavailable exit status without starting a server."""
    print(
        f"{ASSISTANT_BRIDGE_UNAVAILABLE_REASON_CODE}: assistant mediation "
        "and Program authoring return at I6/N (H0 reads are available)",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
