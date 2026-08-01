"""Typed unavailable gate for the dynamic viewer route at I9."""

from __future__ import annotations

from typing import Final, TYPE_CHECKING

from fastapi import APIRouter

from .security import SecurityConfig

if TYPE_CHECKING:
    from .state import ComposerState


VIEWER_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_viewer_client_unavailable"
)
VIEWER_CLIENT_RESTORATION_GATE: Final = "I9"


class ViewerClientUnavailableError(RuntimeError):
    """Raised before state access, lease creation, or route registration."""


def create_viewer_router(
    _state: "ComposerState",
    _security: SecurityConfig,
) -> APIRouter:
    raise ViewerClientUnavailableError(
        f"{VIEWER_CLIENT_UNAVAILABLE_REASON_CODE}: the dynamic viewer returns "
        "at I9 after binding to the headless observer owner"
    )


__all__ = ["ViewerClientUnavailableError", "create_viewer_router"]
