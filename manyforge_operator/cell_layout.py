"""Pinned one-cell storage layout created only by the operator launcher."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import stat
from typing import Final


CELL_LAYOUT_SCHEMA: Final = "manyforge.cell_instance_layout.v1"
CELL_LAYOUT_MARKER: Final = ".manyforge-cell-layout.json"
SEMANTIC_OWNERS: Final = (
    "alarm-history",
    "composer-authoring",
    "execution-host",
    "logical-scene",
    "manager",
    "operator",
    "program-runner",
    "program-selection",
)
RUN_DOMAINS: Final = (
    "composer",
    "execution-host",
    "headless",
    "manager",
    "operator",
    "program-runner",
)


class CellLayoutError(RuntimeError):
    """A cell root or one of its typed children is unsafe or was replaced."""


def _plain_owned_directory(path: Path, *, writable: bool) -> os.stat_result:
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise CellLayoutError(f"cannot inspect cell-layout directory {path}: {exc}") from exc
    if not stat.S_ISDIR(metadata.st_mode) or path.is_symlink():
        raise CellLayoutError(f"cell-layout path is not a plain directory: {path}")
    if metadata.st_uid != os.geteuid():
        raise CellLayoutError(f"cell-layout path has another owner: {path}")
    if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise CellLayoutError(f"cell-layout path is group/world writable: {path}")
    if writable and not metadata.st_mode & stat.S_IWUSR:
        raise CellLayoutError(f"cell-layout path is not owner-writable: {path}")
    return metadata


def _canonical_plain_directory(path: Path, *, writable: bool) -> tuple[Path, os.stat_result]:
    if not path.is_absolute():
        raise CellLayoutError(f"cell-layout path must be absolute: {path}")
    try:
        canonical = path.resolve(strict=True)
    except OSError as exc:
        raise CellLayoutError(f"cell-layout path is unavailable: {path}: {exc}") from exc
    if canonical != path:
        raise CellLayoutError(f"cell-layout path is not canonical: {path}")
    return canonical, _plain_owned_directory(canonical, writable=writable)


@dataclass(frozen=True, slots=True)
class CellInstanceLayout:
    cell_id: str
    cell_root: Path
    install_root: Path
    cell_device: int
    cell_inode: int
    install_device: int
    install_inode: int
    marker_device: int
    marker_inode: int
    root_identities: tuple[tuple[str, int, int], ...]

    @classmethod
    def open_existing(cls, cell_root: Path) -> "CellInstanceLayout":
        cell_root = Path(cell_root)
        cell_root, _metadata = _canonical_plain_directory(cell_root, writable=True)
        marker = cell_root / CELL_LAYOUT_MARKER
        try:
            marker_stat = marker.lstat()
            observed = json.loads(marker.read_text(encoding="utf-8"))
            expected_keys = {
                "schema_version", "cell_id", "cell_root", "cell_device",
                "cell_inode", "install_root", "install_device",
                "install_inode", "roots",
            }
            if not isinstance(observed, dict) or set(observed) != expected_keys:
                raise ValueError("marker is not one exact closed object")
            rows = observed["roots"]
            if not isinstance(rows, list) or any(
                not isinstance(row, dict)
                or set(row) != {"name", "device", "inode"}
                or not isinstance(row["name"], str)
                or type(row["device"]) is not int
                or type(row["inode"]) is not int
                for row in rows
            ):
                raise ValueError("marker roots are invalid")
            if (
                observed["schema_version"] != CELL_LAYOUT_SCHEMA
                or observed["cell_root"] != str(cell_root)
                or not isinstance(observed["cell_id"], str)
                or not isinstance(observed["install_root"], str)
                or [row["name"] for row in rows]
                != (
                    ["artifacts", "state", "projections", "run"]
                    + [f"state/{owner}" for owner in SEMANTIC_OWNERS]
                    + [f"run/{domain}" for domain in RUN_DOMAINS]
                )
                or any(
                    type(observed[name]) is not int
                    for name in (
                        "cell_device", "cell_inode", "install_device", "install_inode"
                    )
                )
            ):
                raise ValueError("marker identity is invalid")
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
            raise CellLayoutError(f"cell-layout marker is unreadable: {exc}") from exc
        layout = cls(
            observed["cell_id"],
            cell_root,
            Path(observed["install_root"]),
            observed["cell_device"],
            observed["cell_inode"],
            observed["install_device"],
            observed["install_inode"],
            marker_stat.st_dev,
            marker_stat.st_ino,
            tuple((row["name"], row["device"], row["inode"]) for row in rows),
        )
        layout.validate()
        return layout

    @classmethod
    def open_or_create(
        cls, *, cell_id: str, cell_root: Path, install_root: Path
    ) -> "CellInstanceLayout":
        if (
            not cell_id
            or len(cell_id.encode("utf-8")) > 256
            or cell_id in {".", ".."}
            or "/" in cell_id
            or "\\" in cell_id
        ):
            raise CellLayoutError("cell identity is not one bounded path-safe token")
        cell_root = Path(cell_root)
        install_root = Path(install_root)
        if not cell_root.is_absolute() or not install_root.is_absolute():
            raise CellLayoutError("cell and install roots must be absolute")
        cell_root_existed = cell_root.exists()
        if not cell_root.exists():
            cell_root.mkdir(parents=True, mode=0o700)
        cell_root, cell_stat = _canonical_plain_directory(cell_root, writable=True)
        install_root, install_stat = _canonical_plain_directory(
            install_root, writable=False
        )
        marker = cell_root / CELL_LAYOUT_MARKER
        if (
            cell_root_existed
            and not marker.exists()
            and any(cell_root.iterdir())
        ):
            raise CellLayoutError(
                "an existing non-empty cell root has no supported layout marker"
            )
        typed_paths: list[tuple[str, Path]] = []
        for child in ("artifacts", "state", "projections", "run"):
            path = cell_root / child
            if not path.exists():
                path.mkdir(mode=0o700)
            _canonical_plain_directory(path, writable=True)
            typed_paths.append((child, path))
        for owner in SEMANTIC_OWNERS:
            path = cell_root / "state" / owner
            if not path.exists():
                path.mkdir(mode=0o700)
            _canonical_plain_directory(path, writable=True)
            typed_paths.append((f"state/{owner}", path))
        for domain in RUN_DOMAINS:
            path = cell_root / "run" / domain
            if not path.exists():
                path.mkdir(mode=0o700)
            _canonical_plain_directory(path, writable=True)
            typed_paths.append((f"run/{domain}", path))
        root_identities = tuple(
            (name, metadata.st_dev, metadata.st_ino)
            for name, path in typed_paths
            for _canonical, metadata in (
                _canonical_plain_directory(path, writable=True),
            )
        )

        expected = {
            "schema_version": CELL_LAYOUT_SCHEMA,
            "cell_id": cell_id,
            "cell_root": str(cell_root),
            "cell_device": cell_stat.st_dev,
            "cell_inode": cell_stat.st_ino,
            "install_root": str(install_root),
            "install_device": install_stat.st_dev,
            "install_inode": install_stat.st_ino,
            "roots": [
                {"name": name, "device": device, "inode": inode}
                for name, device, inode in root_identities
            ],
        }
        encoded = json.dumps(expected, sort_keys=True, separators=(",", ":")) + "\n"
        try:
            descriptor = os.open(
                marker,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
            )
        except FileExistsError:
            descriptor = -1
        if descriptor >= 0:
            try:
                remaining = memoryview(encoded.encode("utf-8"))
                while remaining:
                    written = os.write(descriptor, remaining)
                    if written <= 0:
                        raise CellLayoutError("cell-layout marker write stalled")
                    remaining = remaining[written:]
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            directory = os.open(cell_root, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        try:
            marker_stat = marker.lstat()
            observed = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise CellLayoutError(f"cell-layout marker is unreadable: {exc}") from exc
        if (
            not stat.S_ISREG(marker_stat.st_mode)
            or marker.is_symlink()
            or marker_stat.st_uid != os.geteuid()
            or marker_stat.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or observed != expected
        ):
            raise CellLayoutError("cell-layout marker or pinned identity is invalid")
        return cls(
            cell_id,
            cell_root,
            install_root,
            cell_stat.st_dev,
            cell_stat.st_ino,
            install_stat.st_dev,
            install_stat.st_ino,
            marker_stat.st_dev,
            marker_stat.st_ino,
            root_identities,
        )

    def validate(self) -> None:
        cell_root, cell_stat = _canonical_plain_directory(self.cell_root, writable=True)
        install_root, install_stat = _canonical_plain_directory(
            self.install_root, writable=False
        )
        if (
            cell_root != self.cell_root
            or install_root != self.install_root
            or (cell_stat.st_dev, cell_stat.st_ino)
            != (self.cell_device, self.cell_inode)
            or (install_stat.st_dev, install_stat.st_ino)
            != (self.install_device, self.install_inode)
        ):
            raise CellLayoutError("a pinned cell-layout root was replaced")
        marker = self.cell_root / CELL_LAYOUT_MARKER
        try:
            marker_stat = marker.lstat()
            observed = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise CellLayoutError(f"cell-layout marker is unreadable: {exc}") from exc
        expected = {
            "schema_version": CELL_LAYOUT_SCHEMA,
            "cell_id": self.cell_id,
            "cell_root": str(self.cell_root),
            "cell_device": self.cell_device,
            "cell_inode": self.cell_inode,
            "install_root": str(self.install_root),
            "install_device": self.install_device,
            "install_inode": self.install_inode,
            "roots": [
                {"name": name, "device": device, "inode": inode}
                for name, device, inode in self.root_identities
            ],
        }
        if (
            not stat.S_ISREG(marker_stat.st_mode)
            or marker.is_symlink()
            or marker_stat.st_uid != os.geteuid()
            or marker_stat.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or (marker_stat.st_dev, marker_stat.st_ino)
            != (self.marker_device, self.marker_inode)
            or observed != expected
        ):
            raise CellLayoutError("the pinned cell-layout marker was replaced or changed")
        for name, device, inode in self.root_identities:
            _canonical, metadata = _canonical_plain_directory(
                self.cell_root / name, writable=True
            )
            if (metadata.st_dev, metadata.st_ino) != (device, inode):
                raise CellLayoutError(
                    f"a pinned cell-layout capability root was replaced: {name}"
                )

    @property
    def artifact_root(self) -> Path:
        return self.cell_root / "artifacts"

    @property
    def projection_root(self) -> Path:
        return self.cell_root / "projections"

    @property
    def run_root(self) -> Path:
        return self.cell_root / "run"

    def owner_state_root(self, owner: str) -> Path:
        if owner not in SEMANTIC_OWNERS:
            raise CellLayoutError(f"unknown semantic-owner root: {owner}")
        return self.cell_root / "state" / owner
