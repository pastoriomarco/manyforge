"""Composer-owned HTTP routes for installation settings and profiles.

These routes never touch the Program, the Deployment, the Headless control
connection or execution. Every mutation is revision-checked.
"""

from __future__ import annotations

from typing import Any, NoReturn

from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel, ConfigDict, Field

from .composer_settings import (
    ComposerSettings,
    ComposerSettingsConflictError,
    ComposerSettingsError,
    ComposerSettingsStore,
)


_REVISION_MAX = 2**53 - 1


class _RevisionRequest(BaseModel):
    model_config = ConfigDict(populate_by_name=True, extra="forbid")

    expected_revision: int = Field(ge=0, le=_REVISION_MAX, alias="expectedRevision")


class _ReplaceSettingsRequest(_RevisionRequest):
    settings: ComposerSettings


def _raise(error: ComposerSettingsError) -> NoReturn:
    detail: dict[str, Any] = {"code": error.code, "message": str(error)}
    if isinstance(error, ComposerSettingsConflictError):
        detail["current"] = error.current
    raise HTTPException(status_code=error.status_code, detail=detail) from error


def create_composer_settings_router(store: ComposerSettingsStore) -> APIRouter:
    router = APIRouter()

    @router.get("")
    def read_settings() -> dict[str, Any]:
        return store.snapshot()

    @router.put("")
    def replace_settings(request: _ReplaceSettingsRequest) -> dict[str, Any]:
        try:
            return store.replace(request.expected_revision, request.settings)
        except ComposerSettingsError as error:
            _raise(error)

    @router.post("/restore-defaults")
    def restore_defaults(request: _RevisionRequest) -> dict[str, Any]:
        try:
            return store.restore_defaults(request.expected_revision)
        except ComposerSettingsError as error:
            _raise(error)

    @router.post("/reset-damaged")
    def reset_damaged(request: _RevisionRequest) -> dict[str, Any]:
        try:
            return store.reset_damaged(request.expected_revision)
        except ComposerSettingsError as error:
            _raise(error)

    @router.post("/profiles/{name}")
    def save_profile(name: str, request: _RevisionRequest) -> dict[str, Any]:
        try:
            return store.save_profile(request.expected_revision, name)
        except ComposerSettingsError as error:
            _raise(error)

    @router.post("/profiles/{name}/load")
    def load_profile(name: str, request: _RevisionRequest) -> dict[str, Any]:
        try:
            return store.load_profile(request.expected_revision, name)
        except ComposerSettingsError as error:
            _raise(error)

    @router.delete("/profiles/{name}")
    def delete_profile(
        name: str,
        expected_revision: int = Query(ge=0, le=_REVISION_MAX, alias="expectedRevision"),
    ) -> dict[str, Any]:
        try:
            return store.delete_profile(expected_revision, name)
        except ComposerSettingsError as error:
            _raise(error)

    return router
