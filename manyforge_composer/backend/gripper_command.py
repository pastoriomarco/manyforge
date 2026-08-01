"""Typed cutover gate for the future addressed gripper-command client.

The historical module read singleton Composer deployment/Kernel state,
validated commands against a structural-viewer URDF, and made CycleManager
admission decisions.  None of that implementation is a valid fallback.

I6/N must restore, in the C++ command owner and its thin Python client:

* explicit component, command-target, capability, resource, and model identity;
* unit-tagged finite coordinates (prismatic positions are metres);
* authoritative model-limit and active-operation validation;
* preview with no motion, final revalidation, and exactly one dispatch; and
* deterministic backend-unavailable and conflicting-operation failures.

I11/T later requalifies these cases across tool and gripper variants.
"""

from __future__ import annotations

from typing import Final, NoReturn


GRIPPER_COMMAND_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_gripper_command_client_unavailable"
)
GRIPPER_COMMAND_RESTORATION_GATES: Final = ("I6", "N", "I11", "T")


class GripperCommandClientUnavailableError(RuntimeError):
    """Raised before deployment, model, Kernel, route, or robot-state access."""


def _raise_gripper_command_unavailable() -> NoReturn:
    raise GripperCommandClientUnavailableError(
        f"{GRIPPER_COMMAND_CLIENT_UNAVAILABLE_REASON_CODE}: addressed, "
        "unit-tagged gripper commands return at I6/N and are requalified at "
        "I11/T"
    )


class GripperCommandClient:
    """Unavailable client shell; construction has no observable side effect."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_gripper_command_unavailable()


def require_gripper_command_client(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Fail closed before examining any former Composer-state argument."""

    _raise_gripper_command_unavailable()


__all__ = [
    "GRIPPER_COMMAND_CLIENT_UNAVAILABLE_REASON_CODE",
    "GRIPPER_COMMAND_RESTORATION_GATES",
    "GripperCommandClient",
    "GripperCommandClientUnavailableError",
    "require_gripper_command_client",
]
