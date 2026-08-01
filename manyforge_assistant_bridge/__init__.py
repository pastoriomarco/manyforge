"""Retained assistant-client port inputs for the composable-cell cutover.

The live provider bridge is unavailable. ``protocol`` and ``supervisor`` keep
deterministic client/reducer inputs, while ``bridge`` is a fail-before-effects
entrypoint shell for H0/I5.
"""

__all__ = ["bridge", "protocol", "supervisor"]
