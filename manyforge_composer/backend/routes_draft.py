"""Read-only HTTP projection of the Composer-owned in-memory Program Draft."""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter

from .state import ComposerState


def create_draft_router(state: ComposerState) -> APIRouter:
    """Expose Draft state without adding save, Apply, run, or runtime mutation."""

    router = APIRouter()

    @router.get("/api/program/draft")
    def read_draft() -> dict[str, Any]:
        return state.draft_session.snapshot()

    return router


__all__ = ["create_draft_router"]
