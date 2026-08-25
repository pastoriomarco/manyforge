"""Pinned one-cell storage layout created only by the operator launcher."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import stat
from typing import Any, Final


CELL_LAYOUT_SCHEMA: Final = "manyforge.cell_instance_layout.v1"
CELL_LAYOUT_MARKER: Final = ".manyforge-cell-layout.json"
CELL_LAYOUT_INITIALIZER: Final = ".manyforge-cell-layout.initializing"
CELL_LAYOUT_MARKER_MAX_BYTES: Final = 64 * 1024
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
_TOP_LEVEL_ROOTS: Final = ("artifacts", "state", "projections", "run")


class CellLayoutError(RuntimeError):
    """A cell root or one of its typed children is unsafe or was replaced."""


def portable_path_identity(value: object) -> bool:
    if not isinstance(value, str):
        return False
    try:
        encoded = value.encode("utf-8")
    except UnicodeError:
        return False

    def alphanumeric(character: str) -> bool:
        return (
            "a" <= character <= "z"
            or "A" <= character <= "Z"
            or "0" <= character <= "9"
        )

    return (
        bool(value)
        and len(encoded) <= 256
        and alphanumeric(value[0])
        and all(
            alphanumeric(character) or character in {".", "_", "-"}
            for character in value[1:]
        )
    )


def _strict_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON value: {value}")


def _validate_directory_metadata(
    path: Path, metadata: os.stat_result, *, writable: bool
) -> None:
    if not stat.S_ISDIR(metadata.st_mode):
        raise CellLayoutError(f"cell-layout path is not a plain directory: {path}")
    if metadata.st_uid != os.geteuid():
        raise CellLayoutError(f"cell-layout path has another owner: {path}")
    if metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise CellLayoutError(f"cell-layout path is group/world writable: {path}")
    if metadata.st_mode & (stat.S_IRUSR | stat.S_IXUSR) != (
        stat.S_IRUSR | stat.S_IXUSR
    ):
        raise CellLayoutError(f"cell-layout path is not owner-readable/searchable: {path}")
    if writable and not metadata.st_mode & stat.S_IWUSR:
        raise CellLayoutError(f"cell-layout path is not owner-writable: {path}")


def _canonical_plain_directory(
    path: Path, *, writable: bool
) -> tuple[Path, os.stat_result]:
    path = Path(path)
    if not path.is_absolute():
        raise CellLayoutError(f"cell-layout path must be absolute: {path}")
    try:
        before = path.lstat()
        _validate_directory_metadata(path, before, writable=writable)
        if path.resolve(strict=True) != path:
            raise CellLayoutError(f"cell-layout path is not canonical: {path}")
        descriptor = os.open(
            path,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except CellLayoutError:
        raise
    except OSError as exc:
        raise CellLayoutError(f"cell-layout path is unavailable: {path}: {exc}") from exc
    try:
        opened = os.fstat(descriptor)
        after = path.lstat()
        for metadata in (opened, after):
            _validate_directory_metadata(path, metadata, writable=writable)
        identity = (before.st_dev, before.st_ino)
        if (
            (opened.st_dev, opened.st_ino) != identity
            or (after.st_dev, after.st_ino) != identity
            or path.resolve(strict=True) != path
        ):
            raise CellLayoutError(f"cell-layout path changed while opening: {path}")
        return path, opened
    except CellLayoutError:
        raise
    except OSError as exc:
        raise CellLayoutError(f"cannot recheck cell-layout directory {path}: {exc}") from exc
    finally:
        os.close(descriptor)


def _open_or_create_private_directory_chain(
    path: Path,
) -> tuple[Path, os.stat_result]:
    """Create a missing owner-private namespace from one trusted ancestor."""

    path = Path(path)
    if not path.is_absolute() or ".." in path.parts:
        raise CellLayoutError(
            f"cell-layout namespace must be one absolute canonical path: {path}"
        )

    missing: list[str] = []
    anchor = path
    while True:
        try:
            anchor.lstat()
            break
        except FileNotFoundError:
            name = anchor.name
            if name in {"", ".", ".."} or anchor.parent == anchor:
                raise CellLayoutError(
                    f"cell-layout namespace has no safe existing anchor: {path}"
                )
            missing.append(name)
            anchor = anchor.parent
        except OSError as exc:
            raise CellLayoutError(
                f"cannot inspect cell-layout namespace {anchor}: {exc}"
            ) from exc

    anchor, anchor_stat = _canonical_plain_directory(anchor, writable=True)
    if not missing:
        return anchor, anchor_stat

    descriptor = os.open(
        anchor,
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
    )
    current_path = anchor
    current_identity = (anchor_stat.st_dev, anchor_stat.st_ino)
    try:
        opened_anchor = os.fstat(descriptor)
        if (opened_anchor.st_dev, opened_anchor.st_ino) != current_identity:
            raise CellLayoutError(
                "cell-layout namespace anchor changed before creation"
            )
        for name in reversed(missing):
            try:
                os.mkdir(name, mode=0o700, dir_fd=descriptor)
                child_descriptor = os.open(
                    name,
                    os.O_RDONLY
                    | os.O_DIRECTORY
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    dir_fd=descriptor,
                )
            except OSError as exc:
                raise CellLayoutError(
                    f"cannot create cell-layout namespace {current_path / name}: "
                    f"{exc}"
                ) from exc
            try:
                created = os.fstat(child_descriptor)
                observed = os.stat(
                    name,
                    dir_fd=descriptor,
                    follow_symlinks=False,
                )
                child_path = current_path / name
                for metadata in (created, observed):
                    _validate_directory_metadata(
                        child_path, metadata, writable=True
                    )
                created_identity = (created.st_dev, created.st_ino)
                if (observed.st_dev, observed.st_ino) != created_identity:
                    raise CellLayoutError(
                        f"new cell-layout namespace changed while opening: "
                        f"{child_path}"
                    )
                os.fsync(child_descriptor)
                os.fsync(descriptor)
            except CellLayoutError:
                os.close(child_descriptor)
                raise
            except OSError as exc:
                os.close(child_descriptor)
                raise CellLayoutError(
                    f"cannot persist cell-layout namespace "
                    f"{current_path / name}: {exc}"
                ) from exc
            os.close(descriptor)
            descriptor = child_descriptor
            current_path = child_path
            current_identity = created_identity
    finally:
        os.close(descriptor)

    canonical, metadata = _canonical_plain_directory(path, writable=True)
    if canonical != path or (metadata.st_dev, metadata.st_ino) != current_identity:
        raise CellLayoutError("new cell-layout namespace changed during creation")
    return canonical, metadata


def _directory_entries(path: Path) -> set[str]:
    try:
        descriptor = os.open(
            path,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except OSError as exc:
        raise CellLayoutError(f"cannot open cell-layout directory {path}: {exc}") from exc
    try:
        return set(os.listdir(descriptor))
    except OSError as exc:
        raise CellLayoutError(f"cannot list cell-layout directory {path}: {exc}") from exc
    finally:
        os.close(descriptor)


def _read_marker(path: Path) -> tuple[dict[str, Any], os.stat_result]:
    try:
        before = path.lstat()
    except OSError as exc:
        raise CellLayoutError(f"cell-layout marker is unavailable: {exc}") from exc
    def safe(metadata: os.stat_result) -> bool:
        return (
            stat.S_ISREG(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and metadata.st_nlink == 1
            and bool(metadata.st_mode & stat.S_IRUSR)
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_size <= CELL_LAYOUT_MARKER_MAX_BYTES
        )

    if not safe(before):
        raise CellLayoutError("cell-layout marker type, owner, mode, or size is unsafe")
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except OSError as exc:
        raise CellLayoutError(f"cell-layout marker cannot be opened safely: {exc}") from exc
    try:
        opened = os.fstat(descriptor)
        if not safe(opened) or (opened.st_dev, opened.st_ino) != (
            before.st_dev,
            before.st_ino,
        ):
            raise CellLayoutError("cell-layout marker changed while opening")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(
                descriptor,
                min(8192, CELL_LAYOUT_MARKER_MAX_BYTES + 1 - total),
            )
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > CELL_LAYOUT_MARKER_MAX_BYTES:
                raise CellLayoutError("cell-layout marker is oversized")
        after_fd = os.fstat(descriptor)
        after_path = path.lstat()
        identity = (before.st_dev, before.st_ino)
        if (
            not safe(after_fd)
            or not safe(after_path)
            or (after_fd.st_dev, after_fd.st_ino) != identity
            or (after_path.st_dev, after_path.st_ino) != identity
            or after_fd.st_size != total
            or after_path.st_size != total
        ):
            raise CellLayoutError("cell-layout marker changed while reading")
        raw = b"".join(chunks)
        observed = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_strict_json_object,
            parse_constant=_reject_json_constant,
        )
        if not isinstance(observed, dict) or raw != (
            json.dumps(observed, sort_keys=True, separators=(",", ":")) + "\n"
        ).encode("utf-8"):
            raise CellLayoutError(
                "cell-layout marker is not one canonical JSON object"
            )
    except CellLayoutError:
        raise
    except (OSError, UnicodeError, ValueError) as exc:
        raise CellLayoutError(f"cell-layout marker is unreadable: {exc}") from exc
    finally:
        os.close(descriptor)
    return observed, opened


def _expected_root_names() -> list[str]:
    return (
        list(_TOP_LEVEL_ROOTS)
        + [f"state/{owner}" for owner in SEMANTIC_OWNERS]
        + [f"run/{domain}" for domain in RUN_DOMAINS]
    )


def _decode_marker(
    observed: dict[str, Any], *, cell_root: Path
) -> tuple[str, Path, int, int, int, int, tuple[tuple[str, int, int], ...]]:
    expected_keys = {
        "schema_version",
        "cell_id",
        "cell_root",
        "cell_device",
        "cell_inode",
        "install_root",
        "install_device",
        "install_inode",
        "roots",
    }
    rows = observed.get("roots")
    cell_id = observed.get("cell_id")
    install_root = observed.get("install_root")
    if (
        set(observed) != expected_keys
        or observed.get("schema_version") != CELL_LAYOUT_SCHEMA
        or observed.get("cell_root") != str(cell_root)
        or not portable_path_identity(cell_id)
        or not isinstance(install_root, str)
        or not Path(install_root).is_absolute()
        or not isinstance(rows, list)
        or any(
            not isinstance(row, dict)
            or set(row) != {"name", "device", "inode"}
            or not isinstance(row["name"], str)
            or type(row["device"]) is not int
            or type(row["inode"]) is not int
            for row in rows
        )
        or [row["name"] for row in rows] != _expected_root_names()
        or any(
            type(observed.get(name)) is not int
            for name in ("cell_device", "cell_inode", "install_device", "install_inode")
        )
        or len({(row["device"], row["inode"]) for row in rows}) != len(rows)
    ):
        raise CellLayoutError("cell-layout marker identity is invalid")
    return (
        observed["cell_id"],
        Path(observed["install_root"]),
        observed["cell_device"],
        observed["cell_inode"],
        observed["install_device"],
        observed["install_inode"],
        tuple((row["name"], row["device"], row["inode"]) for row in rows),
    )


def _write_all(descriptor: int, payload: bytes, *, label: str) -> None:
    remaining = memoryview(payload)
    while remaining:
        written = os.write(descriptor, remaining)
        if written <= 0:
            raise CellLayoutError(f"{label} write stalled")
        remaining = remaining[written:]


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
        cell_root, _metadata = _canonical_plain_directory(
            Path(cell_root), writable=True
        )
        observed, marker_stat = _read_marker(cell_root / CELL_LAYOUT_MARKER)
        decoded = _decode_marker(observed, cell_root=cell_root)
        layout = cls(
            decoded[0],
            cell_root,
            decoded[1],
            decoded[2],
            decoded[3],
            decoded[4],
            decoded[5],
            marker_stat.st_dev,
            marker_stat.st_ino,
            decoded[6],
        )
        layout.validate()
        return layout

    @classmethod
    def open_or_create(
        cls, *, cell_id: str, cell_root: Path, install_root: Path
    ) -> "CellInstanceLayout":
        if not portable_path_identity(cell_id):
            raise CellLayoutError("cell identity is not one bounded path-safe token")
        cell_root = Path(cell_root)
        install_root = Path(install_root)
        if not cell_root.is_absolute() or not install_root.is_absolute():
            raise CellLayoutError("cell and install roots must be absolute")

        install_root, install_stat = _canonical_plain_directory(
            install_root, writable=False
        )
        if ".." in cell_root.parts or cell_root.name in {"", ".", ".."}:
            raise CellLayoutError("new cell root has an unsafe parent or name")
        if (
            cell_root == install_root
            or cell_root in install_root.parents
            or install_root in cell_root.parents
        ):
            raise CellLayoutError("cell and install roots must be disjoint")
        try:
            cell_root.lstat()
            root_exists = True
        except FileNotFoundError:
            root_exists = False
        except OSError as exc:
            raise CellLayoutError(f"cannot inspect cell root {cell_root}: {exc}") from exc
        created_root_identity: tuple[int, int] | None = None
        if not root_exists:
            parent, parent_stat = _open_or_create_private_directory_chain(
                cell_root.parent
            )
            if parent != cell_root.parent or cell_root.name in {"", ".", ".."}:
                raise CellLayoutError("new cell root has an unsafe parent or name")
            candidate = parent / cell_root.name
            if (
                candidate == install_root
                or candidate in install_root.parents
                or install_root in candidate.parents
            ):
                raise CellLayoutError("cell and install roots must be disjoint")
            parent_descriptor = os.open(
                parent,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
            try:
                opened_parent = os.fstat(parent_descriptor)
                if (opened_parent.st_dev, opened_parent.st_ino) != (
                    parent_stat.st_dev,
                    parent_stat.st_ino,
                ):
                    raise CellLayoutError(
                        "new cell root parent changed before creation"
                    )
                os.mkdir(cell_root.name, mode=0o700, dir_fd=parent_descriptor)
                created_descriptor = os.open(
                    cell_root.name,
                    os.O_RDONLY
                    | os.O_DIRECTORY
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    dir_fd=parent_descriptor,
                )
                try:
                    created = os.fstat(created_descriptor)
                    _validate_directory_metadata(
                        cell_root, created, writable=True
                    )
                    created_root_identity = (created.st_dev, created.st_ino)
                finally:
                    os.close(created_descriptor)
                os.fsync(parent_descriptor)
            except CellLayoutError:
                raise
            except OSError as exc:
                raise CellLayoutError(f"cannot create cell root {cell_root}: {exc}") from exc
            finally:
                os.close(parent_descriptor)
        cell_root, cell_stat = _canonical_plain_directory(cell_root, writable=True)
        if created_root_identity is not None and (
            cell_stat.st_dev,
            cell_stat.st_ino,
        ) != created_root_identity:
            raise CellLayoutError("new cell root changed during creation")
        if (
            cell_root == install_root
            or cell_root in install_root.parents
            or install_root in cell_root.parents
        ):
            raise CellLayoutError("cell and install roots must be disjoint")
        marker = cell_root / CELL_LAYOUT_MARKER
        try:
            marker.lstat()
            marker_exists = True
        except FileNotFoundError:
            marker_exists = False
        except OSError as exc:
            raise CellLayoutError(f"cannot inspect cell-layout marker: {exc}") from exc
        if marker_exists:
            layout = cls.open_existing(cell_root)
            if (
                layout.cell_id != cell_id
                or layout.install_root != install_root
                or (layout.install_device, layout.install_inode)
                != (install_stat.st_dev, install_stat.st_ino)
            ):
                raise CellLayoutError(
                    "existing cell layout belongs to another cell or install root"
                )
            return layout
        if _directory_entries(cell_root):
            raise CellLayoutError(
                "an existing non-empty cell root has no supported layout marker"
            )

        root_descriptor = os.open(
            cell_root,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
        opened_root = os.fstat(root_descriptor)
        if (opened_root.st_dev, opened_root.st_ino) != (
            cell_stat.st_dev,
            cell_stat.st_ino,
        ):
            os.close(root_descriptor)
            raise CellLayoutError("cell root changed before layout initialization")
        try:
            init_descriptor = os.open(
                CELL_LAYOUT_INITIALIZER,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
                dir_fd=root_descriptor,
            )
        except OSError as exc:
            os.close(root_descriptor)
            raise CellLayoutError(f"cannot reserve new cell layout: {exc}") from exc
        try:
            try:
                _write_all(
                    init_descriptor,
                    (CELL_LAYOUT_SCHEMA + "\n").encode("ascii"),
                    label="cell-layout initializer",
                )
                os.fsync(init_descriptor)
            finally:
                os.close(init_descriptor)
            os.fsync(root_descriptor)
        except (CellLayoutError, OSError) as exc:
            os.close(root_descriptor)
            if isinstance(exc, CellLayoutError):
                raise
            raise CellLayoutError(
                f"cannot persist cell-layout initializer: {exc}"
            ) from exc

        state_descriptor = -1
        run_descriptor = -1
        try:
            typed_paths: list[tuple[str, Path, tuple[int, int]]] = []
            for child in _TOP_LEVEL_ROOTS:
                path = cell_root / child
                os.mkdir(child, mode=0o700, dir_fd=root_descriptor)
                _canonical, metadata = _canonical_plain_directory(
                    path, writable=True
                )
                typed_paths.append(
                    (child, path, (metadata.st_dev, metadata.st_ino))
                )
            state_descriptor = os.open(
                "state",
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=root_descriptor,
            )
            run_descriptor = os.open(
                "run",
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=root_descriptor,
            )
            top_level_identities = {
                name: identity for name, _path, identity in typed_paths
            }
            for name, descriptor in (
                ("state", state_descriptor),
                ("run", run_descriptor),
            ):
                metadata = os.fstat(descriptor)
                _validate_directory_metadata(
                    cell_root / name, metadata, writable=True
                )
                if (metadata.st_dev, metadata.st_ino) != top_level_identities[name]:
                    raise CellLayoutError(
                        f"new cell-layout {name} root changed while opening"
                    )
            for owner in SEMANTIC_OWNERS:
                path = cell_root / "state" / owner
                os.mkdir(owner, mode=0o700, dir_fd=state_descriptor)
                _canonical, metadata = _canonical_plain_directory(
                    path, writable=True
                )
                typed_paths.append(
                    (
                        f"state/{owner}",
                        path,
                        (metadata.st_dev, metadata.st_ino),
                    )
                )
            for domain in RUN_DOMAINS:
                path = cell_root / "run" / domain
                os.mkdir(domain, mode=0o700, dir_fd=run_descriptor)
                _canonical, metadata = _canonical_plain_directory(
                    path, writable=True
                )
                typed_paths.append(
                    (
                        f"run/{domain}",
                        path,
                        (metadata.st_dev, metadata.st_ino),
                    )
                )
            root_identities_list: list[tuple[str, int, int]] = []
            for name, path, created_identity in typed_paths:
                _canonical, metadata = _canonical_plain_directory(
                    path, writable=True
                )
                observed_identity = (metadata.st_dev, metadata.st_ino)
                if observed_identity != created_identity:
                    raise CellLayoutError(
                        f"new cell-layout root changed during creation: {name}"
                    )
                root_identities_list.append((name, *observed_identity))
            root_identities = tuple(root_identities_list)
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
            encoded = (
                json.dumps(expected, sort_keys=True, separators=(",", ":")) + "\n"
            ).encode("utf-8")
            if len(encoded) > CELL_LAYOUT_MARKER_MAX_BYTES:
                raise CellLayoutError("cell-layout marker would be oversized")
            marker_descriptor = os.open(
                CELL_LAYOUT_MARKER,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
                dir_fd=root_descriptor,
            )
            try:
                _write_all(marker_descriptor, encoded, label="cell-layout marker")
                os.fsync(marker_descriptor)
            finally:
                os.close(marker_descriptor)
            os.fsync(state_descriptor)
            os.fsync(run_descriptor)
            os.fsync(root_descriptor)
            os.unlink(CELL_LAYOUT_INITIALIZER, dir_fd=root_descriptor)
            os.fsync(root_descriptor)
        except (CellLayoutError, OSError) as exc:
            # A partial tree remains inert diagnostic input. Runtime open never
            # removes the initializer, creates a child, or repairs a marker.
            if isinstance(exc, CellLayoutError):
                raise
            raise CellLayoutError(f"cannot create complete cell layout: {exc}") from exc
        finally:
            if state_descriptor >= 0:
                os.close(state_descriptor)
            if run_descriptor >= 0:
                os.close(run_descriptor)
            os.close(root_descriptor)
        return cls.open_existing(cell_root)

    def validate(self) -> None:
        cell_root, cell_stat = _canonical_plain_directory(
            self.cell_root, writable=True
        )
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
        capability_identities = (
            (self.cell_device, self.cell_inode),
            (self.install_device, self.install_inode),
            *((device, inode) for _name, device, inode in self.root_identities),
        )
        if len(set(capability_identities)) != len(capability_identities):
            raise CellLayoutError("cell-layout capabilities alias one another")
        observed, marker_stat = _read_marker(self.cell_root / CELL_LAYOUT_MARKER)
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
            (marker_stat.st_dev, marker_stat.st_ino)
            != (self.marker_device, self.marker_inode)
            or observed != expected
        ):
            raise CellLayoutError("the pinned cell-layout marker was replaced or changed")
        if _directory_entries(self.cell_root) != set(_TOP_LEVEL_ROOTS) | {
            CELL_LAYOUT_MARKER
        }:
            raise CellLayoutError("marked cell layout has unexpected or missing roots")
        if _directory_entries(self.cell_root / "state") != set(SEMANTIC_OWNERS):
            raise CellLayoutError(
                "marked cell layout has unexpected or missing roots under state"
            )
        if _directory_entries(self.cell_root / "run") != set(RUN_DOMAINS):
            raise CellLayoutError(
                "marked cell layout has unexpected or missing roots under run"
            )
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
