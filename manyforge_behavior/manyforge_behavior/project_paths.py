"""The one rule for paths inside a Scenario project (OD-335).

A path that names a file or folder of a Scenario (a Program scene mesh's
``mesh_resource_uri``, a Deployment ``model_source`` ``project_path`` or
``external_path`` and its package roots, a generated tool's mesh paths, whose
base is the parameter file's folder when they come from one, OD-364) is
either relative to the Scenario project folder (the ``.scenario`` folder) or
an absolute path. After
normalization it must lie inside one of the allowed folders (``..`` is
allowed only while the normalized path stays inside one), reached without a
symlink below that folder. No URI scheme, no current-directory fallback and
no "try each root" fallback. A refusal names the path and the allowed
folders. The viewer, Load/Apply capture, the runtime capture of API scene
calls and Export all use this rule through one ``ProjectPathRule`` held
where the allowed folders are known.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import stat
from typing import Iterable, Literal

_URI_SCHEME = re.compile(r"[A-Za-z][A-Za-z0-9+.-]*:")


class ProjectPathError(ValueError):
    """A path breaks the project path rule; the message names the path and
    the allowed folders."""


class ProjectPathRule:
    """The allowed folders and the one rule that resolves project paths."""

    def __init__(self, allowed_roots: Iterable[str | os.PathLike[str]]) -> None:
        roots: list[Path] = []
        for raw in allowed_roots:
            text = os.fspath(raw)
            if not text or "\x00" in text or not os.path.isabs(text):
                raise ProjectPathError(
                    f"allowed folder {text!r} is not an absolute path"
                )
            root = Path(os.path.normpath(text))
            if root not in roots:
                roots.append(root)
        self.roots: tuple[Path, ...] = tuple(roots)

    def allowed_text(self) -> str:
        return ", ".join(str(root) for root in self.roots) or "(none configured)"

    def refusal(self, message: str) -> ProjectPathError:
        return ProjectPathError(f"{message} (allowed folders: {self.allowed_text()})")

    def resolve(
        self,
        path_text: str,
        *,
        project_folder: str | os.PathLike[str] | None,
        label: str,
        kind: Literal["file", "directory"] = "file",
    ) -> Path:
        """Resolve one project path to its normalized absolute path."""

        if not isinstance(path_text, str) or not path_text or "\x00" in path_text:
            raise self.refusal(f"{label} path {path_text!r} is empty or invalid")
        if _URI_SCHEME.match(path_text):
            raise self.refusal(
                f"{label} path {path_text!r} uses a URI scheme; use a path "
                "relative to the Scenario project folder or an absolute path"
            )
        if os.path.isabs(path_text):
            candidate = path_text
        else:
            folder = None if project_folder is None else os.fspath(project_folder)
            if not folder or not os.path.isabs(folder):
                raise self.refusal(
                    f"{label} path {path_text!r} is relative but no Scenario "
                    "project folder is known"
                )
            candidate = os.path.join(folder, path_text)
        path = Path(os.path.normpath(candidate))
        containing = [
            root for root in self.roots
            if path != root and (root == Path("/") or root in path.parents)
        ]
        if kind == "directory" and path in self.roots:
            containing.append(path)
        if not containing:
            raise self.refusal(
                f"{label} {path_text!r} resolves to {path}, which is outside "
                "the allowed folders"
            )
        root = max(containing, key=lambda value: len(value.parts))
        probe = root
        try:
            status = os.lstat(root)
            for part in path.relative_to(root).parts:
                probe = probe / part
                status = os.lstat(probe)
                if stat.S_ISLNK(status.st_mode):
                    raise self.refusal(
                        f"{label} {path_text!r} ({path}) passes through the "
                        f"symlink {probe}"
                    )
        except FileNotFoundError as error:
            raise self.refusal(f"{label} {path_text!r} ({path}) does not exist") from error
        except OSError as error:
            raise self.refusal(
                f"{label} {path_text!r} ({path}) cannot be inspected: {error}"
            ) from error
        wanted = stat.S_ISDIR if kind == "directory" else stat.S_ISREG
        if not wanted(status.st_mode):
            raise self.refusal(
                f"{label} {path_text!r} ({path}) is not a "
                f"{'folder' if kind == 'directory' else 'regular file'}"
            )
        return path
