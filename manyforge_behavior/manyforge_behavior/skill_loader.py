"""Typed I6/N gate for the superseded skill artifact and expander.

The removed loader accepted ``manyforge.skill.v0`` artifacts with unscoped
capability strings and deployed joint names.  Frozen spec/460 requires
portable component/target roles and exact Program-Binding compilation, so the
old file/catalog loader cannot remain as a compatibility path.

The historical parameter substitution implementation remains pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` in source blob
``c054596a9075db9de7891574fa9b6cec526a81f8``: ``_render_string`` and
``_render_value`` are lines 321-345, and their use by the v0 catalog expander
is at line 157.  That ``${param:...}`` contract is reimplementation input for
I6/N, not an active authoring helper or compatibility promise.
"""

from __future__ import annotations

from typing import Final, NoReturn


SKILL_ARTIFACT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_skill_artifact_unavailable"
)
SKILL_ARTIFACT_RESTORATION_GATES: Final = ("I6", "N")


class SkillArtifactUnavailableError(RuntimeError):
    """Raised before path, YAML, catalog, or binding state is accessed."""


def _raise_skill_artifact_unavailable() -> NoReturn:
    raise SkillArtifactUnavailableError(
        f"{SKILL_ARTIFACT_UNAVAILABLE_REASON_CODE}: role-bound skill loading "
        "and compilation return at I6/N"
    )


class SkillCatalog:
    """Unavailable old v0 catalog; it performs no discovery or expansion."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_skill_artifact_unavailable()


def load_skill(_path: object) -> NoReturn:
    _raise_skill_artifact_unavailable()


def load_skill_catalog(_skills_dir: object, **_kwargs: object) -> NoReturn:
    _raise_skill_artifact_unavailable()


def default_skills_dir() -> NoReturn:
    _raise_skill_artifact_unavailable()
