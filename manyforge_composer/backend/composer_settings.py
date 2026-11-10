"""Installation-level Composer presentation settings and named profiles.

One small JSON file per installation holds the current settings and any named
profiles. It is presentation only: nothing here reaches the Program, the
Deployment or execution. A setting that names things a Program defines
(execution groups and authored references) belongs to the Program instead.

Persistence rules:

- A missing file means defaults, silently.
- Any unreadable file (corrupt, or any other schema version) means defaults,
  read-only settings and one notice. Only an explicit reset replaces it, and
  the original is kept as a timestamped backup. There are no migrations.
- Writes are atomic (synced temporary file, replace, synced folder) under a
  file lock; memory changes only after persistence succeeded.
- Every mutation names the revision it started from; a stale one changes
  nothing and returns the current state.
"""

from __future__ import annotations

from contextlib import contextmanager
from copy import deepcopy
import fcntl
import json
import os
from pathlib import Path
import re
import tempfile
import threading
import time
from typing import Any, Final, Iterator, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError


SETTINGS_SCHEMA_VERSION: Final = "manyforge.composer_settings.v1"
SETTINGS_FILE_NAME: Final = "settings.json"
_LOCK_FILE_NAME: Final = ".settings.lock"
MAX_PROFILES: Final = 32
_PROFILE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}$")

_STRICT = ConfigDict(
    populate_by_name=True,
    extra="forbid",
    strict=True,
    allow_inf_nan=False,
    validate_assignment=True,
)


class ViewerSettings(BaseModel):
    """3D viewer display toggles. Availability of the data is separate: a
    toggle only says what to show when the Deployment provides it."""

    model_config = _STRICT

    wireframe_overlays: bool = Field(default=False, alias="wireframeOverlays")
    object_outline: bool = Field(default=False, alias="objectOutline")
    collision_geometry: bool = Field(default=False, alias="collisionGeometry")
    point_cloud: bool = Field(default=False, alias="pointCloud")


class TreeEditorSettings(BaseModel):
    """How the Program tree is drawn; never changes the Program.

    ``layout_direction`` is only the direction the editor starts in. Changing
    direction in the toolbar re-lays out node positions, which is a Program
    edit for that session, not a setting. ``tools_width_px`` is likewise the
    width the Program tools pane opens at; dragging its divider changes only
    the session."""

    model_config = _STRICT

    link_style: Literal[
        "smoothstep", "default", "simplebezier", "straight", "step"
    ] = Field(default="default", alias="linkStyle")
    layout_direction: Literal["vertical", "horizontal"] = Field(
        default="vertical", alias="layoutDirection",
    )
    tools_width_px: int = Field(
        default=224, ge=160, le=360, alias="toolsWidthPx",
    )


class MotionPanelSettings(BaseModel):
    """Motion/Program panel interaction preferences. They never add a
    confirmation to Stop."""

    model_config = _STRICT

    confirm_halt: bool = Field(default=True, alias="confirmHalt")
    confirm_reset: bool = Field(default=True, alias="confirmReset")
    control_width_rem: float = Field(
        default=7.0, ge=6.0, le=11.0, alias="controlWidthRem",
    )


class ViewerGraphicsProfile(BaseModel):
    """How the 3D viewer renders on one kind of browser renderer.

    Presentation cost only: nothing here changes what the viewer shows as
    truth. ``max_pixel_ratio`` caps the rendered resolution relative to CSS
    pixels; ``pose_rate_hz`` is how often live robot poses are redrawn."""

    model_config = _STRICT

    max_pixel_ratio: float = Field(
        ge=0.5, le=2.0, multiple_of=0.25, alias="maxPixelRatio",
    )
    antialias: bool
    outline_smoothing: bool = Field(alias="outlineSmoothing")
    pose_rate_hz: int = Field(ge=5, le=60, alias="poseRateHz")
    motion_smoothing: bool = Field(alias="motionSmoothing")
    camera_damping: bool = Field(alias="cameraDamping")


def _gpu_graphics() -> ViewerGraphicsProfile:
    return ViewerGraphicsProfile(
        max_pixel_ratio=1.5, antialias=True, outline_smoothing=True,
        pose_rate_hz=30, motion_smoothing=False, camera_damping=True,
    )


def _software_graphics() -> ViewerGraphicsProfile:
    return ViewerGraphicsProfile(
        max_pixel_ratio=1.0, antialias=False, outline_smoothing=False,
        pose_rate_hz=10, motion_smoothing=False, camera_damping=True,
    )


class ViewerGraphicsSettings(BaseModel):
    """One profile per renderer kind. Every browser uses the one matching how
    it renders: the GPU, or software rendering (e.g. llvmpipe)."""

    model_config = _STRICT

    gpu: ViewerGraphicsProfile = Field(default_factory=_gpu_graphics)
    software: ViewerGraphicsProfile = Field(default_factory=_software_graphics)


class ComposerSettings(BaseModel):
    model_config = _STRICT

    viewer: ViewerSettings = Field(default_factory=ViewerSettings)
    viewer_graphics: ViewerGraphicsSettings = Field(
        default_factory=ViewerGraphicsSettings, alias="viewerGraphics",
    )
    tree_editor: TreeEditorSettings = Field(
        default_factory=TreeEditorSettings, alias="treeEditor",
    )
    motion_panel: MotionPanelSettings = Field(
        default_factory=MotionPanelSettings, alias="motionPanel",
    )


class _SettingsDocument(BaseModel):
    model_config = _STRICT

    schema_version: Literal["manyforge.composer_settings.v1"] = Field(
        alias="schemaVersion",
    )
    revision: int = Field(ge=0, le=2**53 - 1)
    current: ComposerSettings
    profiles: dict[str, ComposerSettings] = Field(max_length=MAX_PROFILES)


class ComposerSettingsError(Exception):
    """Base for typed settings failures; ``code`` is stable for clients."""

    status_code = 400

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class ComposerSettingsConflictError(ComposerSettingsError):
    """The request started from an older revision; nothing changed."""

    status_code = 409

    def __init__(self, code: str, message: str, current: dict[str, Any]) -> None:
        super().__init__(code, message)
        self.current = current


class ComposerSettingsReadOnlyError(ComposerSettingsError):
    """Settings cannot be saved: unconfigured location or unreadable file."""

    status_code = 409


class ComposerSettingsNotFoundError(ComposerSettingsError):
    status_code = 404


class ComposerSettingsPersistenceError(ComposerSettingsError):
    """The file could not be written; memory is unchanged."""

    status_code = 503


def default_settings() -> ComposerSettings:
    return ComposerSettings()


def _settings_json(settings: ComposerSettings) -> dict[str, Any]:
    return settings.model_dump(mode="json", by_alias=True)


def validate_profile_name(name: str) -> str:
    if not isinstance(name, str) or _PROFILE_NAME.fullmatch(name) is None:
        raise ComposerSettingsError(
            "composer_settings_profile_name_invalid",
            "A profile name has 1-64 letters, digits, spaces, '.', '_' or '-',"
            " starting with a letter or digit.",
        )
    return name


class ComposerSettingsStore:
    """The one owner of the installation's Composer settings file.

    ``directory`` is ``None`` when no settings location is configured: the
    defaults apply and nothing can be saved.
    """

    def __init__(self, directory: Path | None) -> None:
        self._directory = None if directory is None else Path(directory)
        self._lock = threading.Lock()
        self._revision = 0
        self._current = default_settings()
        self._profiles: dict[str, ComposerSettings] = {}
        self._notice: dict[str, str] | None = None
        self._damaged = False
        if self._directory is None:
            self._notice = {
                "code": "composer_settings_location_unconfigured",
                "message": (
                    "Settings are not saved: this Composer has no settings"
                    " location. Defaults are in use."
                ),
            }
        else:
            self._load()

    @property
    def path(self) -> Path | None:
        return None if self._directory is None else self._directory / SETTINGS_FILE_NAME

    @property
    def writable(self) -> bool:
        return self._directory is not None and not self._damaged

    # -- reading ---------------------------------------------------------

    def _load(self) -> None:
        path = self.path
        assert path is not None
        with self._file_lock(create=False):
            try:
                raw = path.read_text(encoding="utf-8")
            except FileNotFoundError:
                return
            except OSError as error:
                self._mark_damaged(f"it could not be read ({error.strerror})")
                return
        try:
            document = _SettingsDocument.model_validate(json.loads(raw))
            for name in document.profiles:
                validate_profile_name(name)
        except (ValueError, ValidationError, ComposerSettingsError):
            self._mark_damaged("it is not a valid settings file for this Composer")
            return
        self._revision = document.revision
        self._current = document.current
        self._profiles = dict(document.profiles)

    def _mark_damaged(self, reason: str) -> None:
        self._damaged = True
        self._notice = {
            "code": "composer_settings_file_unreadable",
            "message": (
                f"The settings file {self.path} was not used: {reason}."
                " Defaults are in use and settings are read-only. Reset"
                " settings to start again; the file is kept as a backup."
            ),
        }

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return self._snapshot_locked()

    def _snapshot_locked(self) -> dict[str, Any]:
        return {
            "schemaVersion": SETTINGS_SCHEMA_VERSION,
            "revision": self._revision,
            "settings": _settings_json(self._current),
            "profiles": sorted(self._profiles),
            "writable": self.writable,
            "notice": deepcopy(self._notice),
        }

    # -- mutations -------------------------------------------------------

    def replace(self, expected_revision: int, settings: ComposerSettings) -> dict[str, Any]:
        return self._mutate(expected_revision, current=settings)

    def restore_defaults(self, expected_revision: int) -> dict[str, Any]:
        return self._mutate(expected_revision, current=default_settings())

    def save_profile(self, expected_revision: int, name: str) -> dict[str, Any]:
        validate_profile_name(name)
        with self._lock:
            self._require_writable_current(expected_revision)
            if name not in self._profiles and len(self._profiles) >= MAX_PROFILES:
                raise ComposerSettingsError(
                    "composer_settings_profile_limit",
                    f"At most {MAX_PROFILES} profiles can be saved.",
                )
            profiles = {**self._profiles, name: self._current.model_copy(deep=True)}
            return self._commit_locked(self._current, profiles, backup=False)

    def load_profile(self, expected_revision: int, name: str) -> dict[str, Any]:
        validate_profile_name(name)
        with self._lock:
            self._require_writable_current(expected_revision)
            profile = self._require_profile(name)
            return self._commit_locked(
                profile.model_copy(deep=True), self._profiles, backup=False,
            )

    def delete_profile(self, expected_revision: int, name: str) -> dict[str, Any]:
        validate_profile_name(name)
        with self._lock:
            self._require_writable_current(expected_revision)
            self._require_profile(name)
            profiles = {key: value for key, value in self._profiles.items() if key != name}
            return self._commit_locked(self._current, profiles, backup=False)

    def reset_damaged(self, expected_revision: int) -> dict[str, Any]:
        """Replace an unreadable file with defaults, keeping it as a backup."""
        with self._lock:
            self._require_current(expected_revision)
            if self._directory is None:
                raise ComposerSettingsReadOnlyError(
                    "composer_settings_location_unconfigured",
                    "This Composer has no settings location.",
                )
            if not self._damaged:
                raise ComposerSettingsError(
                    "composer_settings_not_damaged",
                    "The settings file is readable; use Restore defaults instead.",
                )
            return self._commit_locked(default_settings(), {}, backup=True)

    def _mutate(
        self, expected_revision: int, *, current: ComposerSettings,
    ) -> dict[str, Any]:
        with self._lock:
            self._require_writable_current(expected_revision)
            return self._commit_locked(current, self._profiles, backup=False)

    def _require_current(self, expected_revision: int) -> None:
        if expected_revision != self._revision:
            raise ComposerSettingsConflictError(
                "composer_settings_revision_stale",
                "Settings changed elsewhere; nothing was saved.",
                self._snapshot_locked(),
            )

    def _require_writable_current(self, expected_revision: int) -> None:
        self._require_current(expected_revision)
        if not self.writable:
            assert self._notice is not None
            raise ComposerSettingsReadOnlyError(self._notice["code"], self._notice["message"])

    def _require_profile(self, name: str) -> ComposerSettings:
        profile = self._profiles.get(name)
        if profile is None:
            raise ComposerSettingsNotFoundError(
                "composer_settings_profile_not_found",
                f"No profile named '{name}'.",
            )
        return profile

    def _commit_locked(
        self,
        current: ComposerSettings,
        profiles: dict[str, ComposerSettings],
        *,
        backup: bool,
    ) -> dict[str, Any]:
        revision = self._revision + 1
        document = {
            "schemaVersion": SETTINGS_SCHEMA_VERSION,
            "revision": revision,
            "current": _settings_json(current),
            "profiles": {name: _settings_json(value) for name, value in sorted(profiles.items())},
        }
        try:
            self._persist(document, backup=backup)
        except OSError as error:
            raise ComposerSettingsPersistenceError(
                "composer_settings_persistence_failed",
                f"Settings could not be saved ({error.strerror or error}); nothing changed.",
            ) from error
        # Memory follows only a durable file.
        self._revision = revision
        self._current = current.model_copy(deep=True)
        self._profiles = {name: value.model_copy(deep=True) for name, value in profiles.items()}
        if backup:
            self._damaged = False
            self._notice = None
        return self._snapshot_locked()

    # -- persistence -----------------------------------------------------

    @contextmanager
    def _file_lock(self, *, create: bool) -> Iterator[None]:
        assert self._directory is not None
        if create:
            self._directory.mkdir(parents=True, exist_ok=True)
        elif not self._directory.is_dir():
            yield
            return
        with open(self._directory / _LOCK_FILE_NAME, "a+") as lock_file:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)

    def _backup_path(self) -> Path:
        assert self._directory is not None
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        candidate = self._directory / f"{SETTINGS_FILE_NAME}.bak-{stamp}"
        suffix = 1
        while candidate.exists():
            candidate = self._directory / f"{SETTINGS_FILE_NAME}.bak-{stamp}-{suffix}"
            suffix += 1
        return candidate

    def _persist(self, document: dict[str, Any], *, backup: bool) -> None:
        path = self.path
        assert path is not None and self._directory is not None
        with self._file_lock(create=True):
            descriptor, temporary = tempfile.mkstemp(
                prefix=".settings.", suffix=".tmp", dir=self._directory,
            )
            try:
                with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
                    json.dump(document, handle, indent=2, sort_keys=True)
                    handle.write("\n")
                    handle.flush()
                    os.fsync(handle.fileno())
                if backup and path.exists():
                    # A link keeps the original readable while the new file
                    # replaces it, so there is never a moment without one.
                    os.link(path, self._backup_path())
                os.replace(temporary, path)
            except BaseException:
                try:
                    os.unlink(temporary)
                except FileNotFoundError:
                    pass
                raise
            directory = os.open(self._directory, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
