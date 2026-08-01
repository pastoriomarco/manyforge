"""Unavailable assistant-bridge entrypoint retained for the H0 client port.

The former v0 HTTP loop mixed reusable bounded-assistant behavior with old
Composer callback, scene, Program, and catalog trust contracts. Its complete
interaction source is pinned at commit
``08222188293df8e12f90dc72e204c21f22f92ea1``. H0 must port the conversation,
sanitation, proposal-only, cancellation, timeout, turn-bound, stuck-loop, and
audit behavior against an independently trusted headless catalog.
"""

from __future__ import annotations

import sys
from typing import NoReturn


ASSISTANT_BRIDGE_UNAVAILABLE_REASON_CODE = (
    "composable_cell_assistant_bridge_unavailable"
)
ASSISTANT_BRIDGE_RESTORATION_GATES = ("H0", "I5", "I6")


class AssistantBridgeUnavailableError(RuntimeError):
    """The assistant bridge is not bound to a qualified headless owner."""


def require_assistant_bridge() -> NoReturn:
    """Reject activation before network, model, callback, or audit effects."""
    raise AssistantBridgeUnavailableError(
        f"{ASSISTANT_BRIDGE_UNAVAILABLE_REASON_CODE}: assistant mediation "
        "returns at H0/I5, with Program authoring tools following at I6"
    )


def main() -> int:
    """Return the standard unavailable exit status without starting a server."""
    print(
        f"{ASSISTANT_BRIDGE_UNAVAILABLE_REASON_CODE}: assistant mediation "
        "returns at H0/I5 (Program tools at I6)",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
