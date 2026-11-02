"""Installation Composer settings: persistence, revisions and profiles."""

from __future__ import annotations

import json
import os
from pathlib import Path

from fastapi import FastAPI
from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.composer_settings import (
    SETTINGS_FILE_NAME,
    SETTINGS_SCHEMA_VERSION,
    ComposerSettings,
    ComposerSettingsConflictError,
    ComposerSettingsPersistenceError,
    ComposerSettingsReadOnlyError,
    ComposerSettingsStore,
)
from manyforge_composer.backend.routes_composer_settings import (
    create_composer_settings_router,
)


def _client(store: ComposerSettingsStore) -> TestClient:
    app = FastAPI()
    app.include_router(create_composer_settings_router(store), prefix="/api/composer/settings")
    return TestClient(app)


def _outline_on() -> ComposerSettings:
    settings = ComposerSettings()
    settings.viewer.object_outline = True
    return settings


def test_missing_file_uses_defaults_silently_and_first_save_creates_it(tmp_path: Path) -> None:
    store = ComposerSettingsStore(tmp_path / "composer")
    snapshot = store.snapshot()
    assert snapshot["revision"] == 0
    assert snapshot["writable"] is True
    assert snapshot["notice"] is None
    assert snapshot["settings"] == ComposerSettings().model_dump(mode="json", by_alias=True)

    saved = store.replace(0, _outline_on())
    assert saved["revision"] == 1
    document = json.loads((tmp_path / "composer" / SETTINGS_FILE_NAME).read_text())
    assert document["schemaVersion"] == SETTINGS_SCHEMA_VERSION
    assert document["revision"] == 1
    assert document["current"]["viewer"]["objectOutline"] is True

    # A restart reads the same state back.
    reloaded = ComposerSettingsStore(tmp_path / "composer").snapshot()
    assert reloaded["revision"] == 1
    assert reloaded["settings"]["viewer"]["objectOutline"] is True


@pytest.mark.parametrize(
    "content",
    [
        "{not json",
        json.dumps({"schemaVersion": "manyforge.composer_settings.v2", "revision": 3,
                    "current": {}, "profiles": {}}),
        json.dumps({"schemaVersion": SETTINGS_SCHEMA_VERSION, "revision": 1,
                    "current": {"viewer": {"objectOutline": "yes"}}, "profiles": {}}),
        json.dumps({"schemaVersion": SETTINGS_SCHEMA_VERSION, "revision": 1,
                    "current": {}, "profiles": {"../escape": {}}}),
    ],
    ids=["corrupt", "other-version", "invalid-field", "invalid-profile-name"],
)
def test_unreadable_file_is_kept_read_only_until_an_explicit_reset(
    tmp_path: Path, content: str,
) -> None:
    directory = tmp_path / "composer"
    directory.mkdir()
    path = directory / SETTINGS_FILE_NAME
    path.write_text(content)
    store = ComposerSettingsStore(directory)
    snapshot = store.snapshot()
    assert snapshot["writable"] is False
    assert snapshot["notice"]["code"] == "composer_settings_file_unreadable"
    assert snapshot["settings"] == ComposerSettings().model_dump(mode="json", by_alias=True)

    # An ordinary toggle never overwrites the unreadable file.
    with pytest.raises(ComposerSettingsReadOnlyError):
        store.replace(0, _outline_on())
    with pytest.raises(ComposerSettingsReadOnlyError):
        store.save_profile(0, "Desk")
    assert path.read_text() == content

    reset = store.reset_damaged(0)
    assert reset["writable"] is True
    assert reset["notice"] is None
    backups = sorted(directory.glob(f"{SETTINGS_FILE_NAME}.bak-*"))
    assert len(backups) == 1
    assert backups[0].read_text() == content
    assert json.loads(path.read_text())["schemaVersion"] == SETTINGS_SCHEMA_VERSION


def test_unconfigured_location_serves_defaults_and_saves_nothing() -> None:
    store = ComposerSettingsStore(None)
    snapshot = store.snapshot()
    assert snapshot["writable"] is False
    assert snapshot["notice"]["code"] == "composer_settings_location_unconfigured"
    with pytest.raises(ComposerSettingsReadOnlyError):
        store.replace(0, _outline_on())


def test_failed_write_changes_nothing_and_is_reported(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    store = ComposerSettingsStore(tmp_path / "composer")
    store.replace(0, ComposerSettings())

    def failing_replace(*_args: object) -> None:
        raise OSError(28, "No space left on device")

    monkeypatch.setattr(os, "replace", failing_replace)
    with pytest.raises(ComposerSettingsPersistenceError):
        store.replace(1, _outline_on())
    monkeypatch.undo()

    snapshot = store.snapshot()
    assert snapshot["revision"] == 1
    assert snapshot["settings"]["viewer"]["objectOutline"] is False
    # No temporary file is left behind.
    assert not list((tmp_path / "composer").glob(".settings.*.tmp"))


def test_every_mutation_is_revision_checked(tmp_path: Path) -> None:
    store = ComposerSettingsStore(tmp_path / "composer")
    store.save_profile(0, "Desk")
    stale = 0
    for mutate in (
        lambda: store.replace(stale, _outline_on()),
        lambda: store.restore_defaults(stale),
        lambda: store.save_profile(stale, "Other"),
        lambda: store.load_profile(stale, "Desk"),
        lambda: store.delete_profile(stale, "Desk"),
    ):
        with pytest.raises(ComposerSettingsConflictError) as conflict:
            mutate()
        assert conflict.value.current["revision"] == 1
    assert store.snapshot()["revision"] == 1


def test_profiles_save_load_and_delete(tmp_path: Path) -> None:
    store = ComposerSettingsStore(tmp_path / "composer")
    store.replace(0, _outline_on())
    saved = store.save_profile(1, "Jetson display")
    assert saved["profiles"] == ["Jetson display"]

    store.restore_defaults(2)
    assert store.snapshot()["settings"]["viewer"]["objectOutline"] is False
    loaded = store.load_profile(3, "Jetson display")
    assert loaded["settings"]["viewer"]["objectOutline"] is True

    deleted = store.delete_profile(4, "Jetson display")
    assert deleted["profiles"] == []
    persisted = ComposerSettingsStore(tmp_path / "composer").snapshot()
    assert persisted["profiles"] == []
    assert persisted["settings"]["viewer"]["objectOutline"] is True


def test_routes_report_conflict_with_current_state_and_validate_input(tmp_path: Path) -> None:
    client = _client(ComposerSettingsStore(tmp_path / "composer"))
    current = client.get("/api/composer/settings").json()
    settings = current["settings"]
    assert settings["treeEditor"] == {
        "linkStyle": "default", "layoutDirection": "vertical", "toolsWidthPx": 224,
    }
    settings["treeEditor"]["linkStyle"] = "step"
    settings["treeEditor"]["layoutDirection"] = "horizontal"
    saved = client.put(
        "/api/composer/settings",
        json={"expectedRevision": 0, "settings": settings},
    )
    assert saved.status_code == 200
    assert saved.json()["settings"]["treeEditor"]["linkStyle"] == "step"
    assert saved.json()["settings"]["treeEditor"]["layoutDirection"] == "horizontal"

    stale = client.put(
        "/api/composer/settings",
        json={"expectedRevision": 0, "settings": settings},
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "composer_settings_revision_stale"
    assert stale.json()["detail"]["current"]["revision"] == 1

    unknown_field = client.put(
        "/api/composer/settings",
        json={"expectedRevision": 1, "settings": {**settings, "speedLimit": 1}},
    )
    assert unknown_field.status_code == 422
    out_of_range = client.put(
        "/api/composer/settings",
        json={"expectedRevision": 1, "settings": {
            **settings, "motionPanel": {**settings["motionPanel"], "controlWidthRem": 40},
        }},
    )
    assert out_of_range.status_code == 422
    graphics = settings["viewerGraphics"]
    assert graphics["gpu"] == {
        "maxPixelRatio": 1.5, "antialias": True, "outlineSmoothing": True,
        "poseRateHz": 30, "motionSmoothing": False, "cameraDamping": True,
    }
    assert graphics["software"]["maxPixelRatio"] == 1.0
    assert graphics["software"]["poseRateHz"] == 10
    for field, value in (
        ("maxPixelRatio", 0.4), ("maxPixelRatio", 1.1), ("maxPixelRatio", 3.0),
        ("poseRateHz", 4), ("poseRateHz", 61), ("poseRateHz", 30.5),
        ("antialias", "yes"),
    ):
        rejected = client.put(
            "/api/composer/settings",
            json={"expectedRevision": 1, "settings": {
                **settings, "viewerGraphics": {
                    **graphics, "software": {**graphics["software"], field: value},
                },
            }},
        )
        assert rejected.status_code == 422, (field, value)
    for tools_width in (100, 400, 216.5, "216"):
        rejected = client.put(
            "/api/composer/settings",
            json={"expectedRevision": 1, "settings": {
                **settings, "treeEditor": {**settings["treeEditor"], "toolsWidthPx": tools_width},
            }},
        )
        assert rejected.status_code == 422, tools_width

    assert client.post(
        "/api/composer/settings/profiles/Desk", json={"expectedRevision": 1},
    ).json()["profiles"] == ["Desk"]
    assert client.post(
        "/api/composer/settings/profiles/bad%2Fname", json={"expectedRevision": 2},
    ).status_code in (400, 404)
    missing = client.post(
        "/api/composer/settings/profiles/Nope/load", json={"expectedRevision": 2},
    )
    assert missing.status_code == 404
    deleted = client.delete("/api/composer/settings/profiles/Desk?expectedRevision=2")
    assert deleted.status_code == 200
    assert deleted.json()["profiles"] == []

    readable_reset = client.post(
        "/api/composer/settings/reset-damaged", json={"expectedRevision": 3},
    )
    assert readable_reset.status_code == 400
    assert readable_reset.json()["detail"]["code"] == "composer_settings_not_damaged"


def test_writes_are_serialized_by_a_file_lock(tmp_path: Path) -> None:
    store = ComposerSettingsStore(tmp_path / "composer")
    store.replace(0, ComposerSettings())
    assert (tmp_path / "composer" / ".settings.lock").exists()
