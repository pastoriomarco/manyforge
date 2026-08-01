"""Role-bound I6/N skill-artifact gate."""

import pytest

from manyforge_behavior.skill_loader import (
    SKILL_ARTIFACT_RESTORATION_GATES,
    SkillArtifactUnavailableError,
    SkillCatalog,
    default_skills_dir,
    load_skill,
    load_skill_catalog,
)


@pytest.mark.parametrize(
    "operation",
    [
        lambda poison: SkillCatalog(poison),
        lambda poison: load_skill(poison),
        lambda poison: load_skill_catalog(poison, catalog=poison),
    ],
)
def test_v0_skill_surfaces_fail_before_path_or_catalog_access(operation) -> None:
    class PoisonInput:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"skill input was accessed: {name}")

    with pytest.raises(
        SkillArtifactUnavailableError,
        match="composable_cell_skill_artifact_unavailable",
    ):
        operation(PoisonInput())

    assert SKILL_ARTIFACT_RESTORATION_GATES == ("I6", "N")


def test_default_v0_skill_directory_is_not_a_compatibility_path() -> None:
    with pytest.raises(
        SkillArtifactUnavailableError,
        match="composable_cell_skill_artifact_unavailable",
    ):
        default_skills_dir()
