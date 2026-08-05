"""Retained assistant-client port inputs for the composable-cell cutover.

The live provider bridge is unavailable. ``headless`` exposes the closed H0
read client, ``protocol`` and ``supervisor`` keep deterministic client/reducer
inputs, and ``bridge`` remains a fail-before-effects provider entrypoint.
"""

__all__ = ["bridge", "headless", "protocol", "supervisor"]
