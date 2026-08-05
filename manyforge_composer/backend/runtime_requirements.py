"""Typed cutover gate for runtime requirement and availability projection.

The former helper inferred fixed lanes, ROS topics, action servers, and factory
presence from the v1 deployment model and returned hard-coded launcher advice.
The replacement must consume v2 retained-unit/provider availability from the
headless manager and the selected Program Binding.  No legacy inference is a
valid fallback.
"""

from __future__ import annotations

from typing import Final


RUNTIME_REQUIREMENTS_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_runtime_requirements_client_unavailable"
)
RUNTIME_REQUIREMENTS_REPLACEMENT_GATES: Final = ("I5/H0", "I6/N")


class RuntimeRequirementsClientUnavailableError(RuntimeError):
    """Raised before deployment, environment, manager, or route state access."""


def _raise_runtime_requirements_unavailable() -> None:
    raise RuntimeRequirementsClientUnavailableError(
        f"{RUNTIME_REQUIREMENTS_UNAVAILABLE_REASON_CODE}: H0 replaced the "
        "environment-availability portion with manyforge_headless_api; the "
        "selected Program Binding completes the replacement at I6/N, and this "
        "legacy inference helper does not return"
    )


class RuntimeRequirementsClient:
    """Unavailable headless-client shell; it owns no runtime compatibility."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_requirements_unavailable()


def assess_runtime_requirements(*_args: object, **_kwargs: object) -> None:
    """Fail closed until typed v2 requirement and availability facts exist."""
    _raise_runtime_requirements_unavailable()
