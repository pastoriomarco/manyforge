"""Permanent compatibility fence for headless scene initialization.

The deleted Composer barrier and ``manyforge.scene_initialization.v1`` mapping
were a mutable scene/runtime owner, not a client helper.  I5/H0 replaces this
responsibility with the C++ logical-scene/runtime owner.  Its focused evidence
must cover generation-bound tokens, reset/seed/replay/open ordering,
supersession and retirement, stale completion rejection, explicit capacities,
latest eligible observation per source/object, failed-replay recovery, and no
backend call while an owner lock is held.

This module intentionally preserves no dormant state machine or legacy wire.
"""

from __future__ import annotations

from typing import Final, NoReturn


SCENE_INITIALIZATION_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_scene_initialization_client_unavailable"
)
SCENE_INITIALIZATION_REPLACEMENT_GATE: Final = "I5/H0"


class SceneInitializationClientUnavailableError(RuntimeError):
    """Raised before scene, manager, backend, token, or observation access."""


def _raise_scene_initialization_unavailable() -> NoReturn:
    raise SceneInitializationClientUnavailableError(
        f"{SCENE_INITIALIZATION_UNAVAILABLE_REASON_CODE}: the Composer-local "
        "barrier was replaced at I5/H0 by the generation-fenced headless scene "
        "owner; use its headless projection"
    )


class SceneInitializationClient:
    """Unavailable thin-client shell with effect-free construction failure."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_scene_initialization_unavailable()


def require_scene_initialization_client(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Fail closed before inspecting any former Composer-state input."""

    _raise_scene_initialization_unavailable()


__all__ = [
    "SCENE_INITIALIZATION_REPLACEMENT_GATE",
    "SCENE_INITIALIZATION_UNAVAILABLE_REASON_CODE",
    "SceneInitializationClient",
    "SceneInitializationClientUnavailableError",
    "require_scene_initialization_client",
]
