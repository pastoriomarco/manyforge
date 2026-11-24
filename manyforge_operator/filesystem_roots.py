"""The allowed folders of every ManyForge launch (OD-335).

Composer and Headless read Scenario files (model sources, scene meshes) only
inside their allowed folders (``MANYFORGE_FS_ROOTS``, the project path rule
of ``manyforge_behavior.project_paths``). Every launch allows ManyForge's own
shipped content, named here explicitly: ``examples/`` and the shipped model
sources the examples name (``manyforge_example_models``, which holds every
shipped model package root). Nothing else of the source tree is allowed.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Final

# The shipped model sources below the source root: the examples' model files
# and every shipped model package root (each package lives in this folder).
SHIPPED_MODEL_SOURCE_FOLDERS: Final = ("manyforge_example_models",)


def shipped_content_roots(source_root: str | os.PathLike[str]) -> tuple[Path, ...]:
    """``examples/`` (Composer's default browse folder) and the shipped model
    sources of ManyForge's source root (checkout, image or operator source)."""

    root = Path(os.fspath(source_root))
    return (root / "examples", *(root / name for name in SHIPPED_MODEL_SOURCE_FOLDERS))


def launch_filesystem_roots(*folders: str | os.PathLike[str]) -> tuple[Path, ...]:
    """The allowed folders of one launch, in order (the first is Composer's
    default browse folder), each once."""

    roots: list[Path] = []
    for raw in folders:
        root = Path(os.path.normpath(os.fspath(raw)))
        if not root.is_absolute():
            raise ValueError(f"allowed folder {root} is not an absolute path")
        if root not in roots:
            roots.append(root)
    return tuple(roots)


def filesystem_roots_value(roots: tuple[Path, ...]) -> str:
    """The ``MANYFORGE_FS_ROOTS`` value of ``roots``."""

    return ",".join(str(root) for root in roots)
