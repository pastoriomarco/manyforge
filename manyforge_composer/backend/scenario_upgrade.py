"""Upgrade a Scenario saved in an older file format in place (OD-391).

Load refuses a Scenario saved in an older format with a typed code; the Load
dialog then offers Upgrade and load. :func:`check_scenario_upgrade` says which
conversions apply, :func:`upgrade_scenario` applies them. They are the
converter scripts' own conversions, in their order:

1. ``collision_rules`` (``scripts/convert_scenario_od46.py``): Program v3 to
   v4 and Deployment intent v2 to v3;
2. ``scenario_identity``, ``program_profile`` and ``lane_labels``
   (``scripts/convert_scenarios_od369.py``): the Program and Deployment lose
   their own name and description, the Program profile
   ``composable-cell-v1`` becomes ``manyforge-cell-v1``, and the lane labels
   (Program ``compatibility.lanes``, Deployment ``compatibility.lane``) are
   removed. This one reads the
   Deployment in the current format, so it runs second.

The check converts in memory and writes nothing. An upgrade copies the
project folder next to it, converts the copy, then swaps the two folders in
one step (Linux ``renameat2`` with ``RENAME_EXCHANGE``; where the kernel or
file system cannot, the upgrade is refused with
``scenario_artifact_write_failed`` and nothing changes): the converted copy
takes the Scenario's name, and the original, now at the copy's hidden name, is
then renamed to the backup ``<name>.scenario.before-<version>-<UTC time>``.
A crash between the two steps leaves the converted Scenario in place and the
original at the hidden name. Until the swap the original is untouched; a conversion that fails removes the copy and is refused with
``scenario_upgrade_conversion_failed``, naming what could not be converted.
Each upgrade keeps exactly one backup. A Scenario already in the current
format is left alone and gets none, so repeating a request (the same
``request_key`` after a lost reply) changes nothing.
"""

from __future__ import annotations

import ctypes
import errno
import logging
import os
from pathlib import Path
import shutil
import time
from typing import Any, NoReturn
import uuid

import yaml

from manyforge_behavior.canonical_artifact import StrictArtifactError

from . import scenario_artifacts as sa
from . import scenario_convert_od46 as od46
from . import scenario_convert_od369 as od369

_LOGGER = logging.getLogger(__name__)

SCENARIO_UPGRADE_API_VERSION = "manyforge.scenario_upgrade.v1"
# The files the conversions read and write; every other file of the project
# folder (sources, assets) is copied as it is.
_CONVERTED_FILES = (sa.SCENARIO_MANIFEST, *sa.SCENARIO_PROJECT_MEMBERS.values())
_OD46_ERRORS = (od46.ConversionError, OSError, ValueError, yaml.YAMLError)
_OD369_ERRORS = (
    OSError, StrictArtifactError, sa.ScenarioArtifactError, KeyError, ValueError,
)


def _conversion_failed(conversion: str, error: Exception) -> NoReturn:
    reason = f"{error.args[0]!r} is missing" if isinstance(error, KeyError) else str(error)
    raise sa.ScenarioArtifactError(
        "scenario_upgrade_conversion_failed",
        reason,
        status_code=409,
        details={"conversion": conversion},
    ) from error


def _read_files(folder: Path, project: Path) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for name in _CONVERTED_FILES:
        try:
            files[name] = (folder / name).read_bytes()
        except FileNotFoundError:
            continue
        except OSError as error:
            raise sa.ScenarioArtifactError(
                "scenario_artifact_invalid", f"{project / name}: {error}"
            ) from error
    return files


def _convert(folder: Path, project: Path) -> tuple[list[dict[str, Any]], dict[str, bytes]]:
    """The conversions that apply to the files in ``folder`` and the new
    content of each changed file; ``project`` names the files in messages."""

    files = _read_files(folder, project)
    conversions: list[dict[str, Any]] = []
    changed: dict[str, bytes] = {}
    try:
        result = od46.convert_scenario_files(project, files)
    except _OD46_ERRORS as error:
        _conversion_failed("collision_rules", error)
    if result is not None:
        notes, outputs = result
        files.update(outputs)
        changed.update(outputs)
        conversions.append({"conversion": "collision_rules", "changes": notes})
    try:
        changes, outputs = od369.convert_scenario_files(project, files)
    except _OD369_ERRORS as error:
        _conversion_failed("scenario_identity", error)
    renamed = [change for change in changes if change.endswith(od369.RENAMED)]
    lanes = [change for change in changes
             if change.endswith((od369.LANES_REMOVED, od369.LANE_REMOVED))]
    dropped = [change for change in changes if change not in renamed and change not in lanes]
    for conversion, rows in (("scenario_identity", dropped), ("program_profile", renamed),
                             ("lane_labels", lanes)):
        if rows:
            conversions.append({"conversion": conversion, "changes": rows})
    changed.update(outputs)
    return conversions, changed


def _backup_pattern(project: Path, version: str) -> str:
    # The names match sa.SCENARIO_BACKUP_INFIX and SCENARIO_STAGING_INFIX,
    # which the Load list skips.
    return f"{project}.before-{version}-YYYYMMDDTHHMMSSZ"


def _backup_path(project: Path, version: str) -> Path:
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    candidate = project.with_name(f"{project.name}.before-{version}-{stamp}")
    suffix = 1
    while candidate.exists() or candidate.is_symlink():
        candidate = project.with_name(f"{project.name}.before-{version}-{stamp}-{suffix}")
        suffix += 1
    return candidate


def _reply(
    project: Path, status: str, conversions: list[dict[str, Any]],
    version: str, backup: Path | None = None,
) -> dict[str, Any]:
    return {
        "api_version": SCENARIO_UPGRADE_API_VERSION,
        "path": str(project),
        "status": status,
        "conversions": conversions,
        "backup_pattern": _backup_pattern(project, version),
        "backup_path": None if backup is None else str(backup),
    }


def check_scenario_upgrade(
    raw_path: str, *, allowed_roots: list[str], version: str
) -> dict[str, Any]:
    """Which conversions an upgrade would apply (``available``), or
    ``current``. Converts in memory; writes nothing."""

    project = sa._existing_project(raw_path, sa._roots(allowed_roots))
    conversions, _changed = _convert(project, project)
    return _reply(project, "available" if conversions else "current", conversions, version)


_AT_FDCWD = -100
_RENAME_EXCHANGE = 2
_RENAMEAT2 = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
if _RENAMEAT2 is not None:
    _RENAMEAT2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
                           ctypes.c_uint]
    _RENAMEAT2.restype = ctypes.c_int


def _exchange(first: Path, second: Path) -> None:
    """Swap two paths in one step (``renameat2(RENAME_EXCHANGE)``)."""

    if _RENAMEAT2 is None:
        raise OSError(errno.ENOSYS, "renameat2 is not available in the C library")
    if _RENAMEAT2(_AT_FDCWD, os.fsencode(first), _AT_FDCWD, os.fsencode(second),
                  _RENAME_EXCHANGE) != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(first), None, str(second))


def _write_failed(project: Path, message: str, error: OSError) -> NoReturn:
    raise sa.ScenarioArtifactError(
        "scenario_artifact_write_failed", f"{project}: {message}: {error}",
    ) from error


def upgrade_scenario(
    raw_path: str, *, allowed_roots: list[str], version: str, request_key: str,
) -> dict[str, Any]:
    """Upgrade one Scenario folder in place, keeping the original as its
    backup (``upgraded``); a current Scenario is left alone (``current``)."""

    roots = sa._roots(allowed_roots)
    project = sa._existing_project(raw_path, roots)
    # The folder next to it is written: inside the allowed folders, never the
    # read-only Untitled Scenario.
    sa._writable_project(str(project), roots)
    conversions, _changed = _convert(project, project)
    if not conversions:
        return _reply(project, "current", [], version)
    stage = project.with_name(f".{project.name}.upgrade-{uuid.uuid4().hex}")
    exchanged = False
    try:
        try:
            shutil.copytree(project, stage, symlinks=True)
        except OSError as error:
            _write_failed(project, "the copy to upgrade could not be written; the "
                          "Scenario is unchanged", error)
        # Converted from the copy itself, so the swap publishes exactly it.
        conversions, changed = _convert(stage, project)
        stage_fd = os.open(stage, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            for name, raw in sorted(changed.items()):
                sa._replace_regular_at(stage_fd, name, raw)
            os.fsync(stage_fd)
        except OSError as error:
            _write_failed(project, "the upgraded copy could not be written; the "
                          "Scenario is unchanged", error)
        finally:
            os.close(stage_fd)
        backup = _backup_path(project, version)
        try:
            _exchange(stage, project)
        except OSError as error:
            _write_failed(project, "the upgraded copy could not take the Scenario's "
                          "place; the Scenario is unchanged", error)
        # The original is now at the copy's hidden name.
        exchanged = True
        try:
            os.rename(stage, backup)
        except OSError as error:
            _write_failed(project, f"the Scenario is upgraded, but its original could "
                          f"not be moved to its backup; it is kept at {stage}", error)
    finally:
        if not exchanged:
            shutil.rmtree(stage, ignore_errors=True)
    parent_fd = os.open(project.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(parent_fd)
    finally:
        os.close(parent_fd)
    _LOGGER.info(
        "scenario_upgraded",
        extra={
            "path": str(project),
            "backup_path": str(backup),
            "conversions": [row["conversion"] for row in conversions],
            "request_key": request_key,
        },
    )
    return _reply(project, "upgraded", conversions, version, backup)


__all__ = [
    "SCENARIO_UPGRADE_API_VERSION",
    "check_scenario_upgrade",
    "upgrade_scenario",
]
