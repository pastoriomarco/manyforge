"""Tests for the skill loader."""
from __future__ import annotations

from pathlib import Path

import pytest

from manyforge_behavior.skill_loader import (
    SkillCatalog,
    load_skill,
    load_skill_catalog,
    default_skills_dir,
)


SKILLS_DIR = default_skills_dir()


class TestLoadSkill:
    def test_loads_pick_object(self):
        skill = load_skill(SKILLS_DIR / "pick_object.skill.yaml")
        assert skill.id == "pick_object"
        assert skill.version == "0.1.0"
        assert "v0.0" in skill.compatible_profiles
        assert len(skill.parameters) >= 1
        assert skill.body["id"] == "sequence"
        assert len(skill.requires) > 0
        assert len(skill.provides) > 0

    def test_loads_place_object(self):
        skill = load_skill(SKILLS_DIR / "place_object.skill.yaml")
        assert skill.id == "place_object"
        assert "planning.motion.pose_goal" in skill.requires_capabilities

    def test_loads_move_to_named_config(self):
        skill = load_skill(SKILLS_DIR / "move_to_named_config.skill.yaml")
        assert skill.id == "move_to_named_config"
        assert skill.body["id"] == "move_manipulator_action"

    def test_rejects_missing_id(self, tmp_path):
        bad = tmp_path / "bad.skill.yaml"
        bad.write_text("api_version: manyforge.skill.v0\nbody: {id: x}\n")
        with pytest.raises(ValueError, match="missing required field 'skill_id'"):
            load_skill(bad)

    def test_rejects_wrong_api_version(self, tmp_path):
        bad = tmp_path / "bad.skill.yaml"
        bad.write_text("api_version: wrong\nskill_id: test\nbody: {id: x}\n")
        with pytest.raises(ValueError, match="api_version"):
            load_skill(bad)

    def test_rejects_missing_body(self, tmp_path):
        bad = tmp_path / "bad.skill.yaml"
        bad.write_text("api_version: manyforge.skill.v0\nskill_id: test\n")
        with pytest.raises(ValueError, match="body"):
            load_skill(bad)


class TestLoadSkillCatalog:
    def test_loads_all_default_skills(self):
        catalog = load_skill_catalog(SKILLS_DIR)
        assert len(catalog.skills) >= 3
        assert "pick_object" in catalog.skill_ids
        assert "place_object" in catalog.skill_ids
        assert "move_to_named_config" in catalog.skill_ids

    def test_get_by_id(self):
        catalog = load_skill_catalog(SKILLS_DIR)
        pick = catalog.get("pick_object")
        assert pick is not None
        assert pick.id == "pick_object"

    def test_get_missing_returns_none(self):
        catalog = load_skill_catalog(SKILLS_DIR)
        assert catalog.get("nonexistent") is None

    def test_nonexistent_dir_returns_empty(self, tmp_path):
        catalog = load_skill_catalog(tmp_path / "no_such_dir")
        assert len(catalog.skills) == 0

    def test_malformed_files_skipped(self, tmp_path):
        good = tmp_path / "good.skill.yaml"
        good.write_text(
            "api_version: manyforge.skill.v0\nskill_id: good\nbody: {id: x, name: ok}\n"
        )
        bad = tmp_path / "bad.skill.yaml"
        bad.write_text("not: valid: yaml: {{{{")

        catalog = load_skill_catalog(tmp_path)
        assert len(catalog.skills) == 1
        assert catalog.skills[0].id == "good"

    def test_duplicate_ids_skipped(self, tmp_path):
        for name in ["a.skill.yaml", "b.skill.yaml"]:
            (tmp_path / name).write_text(
                "api_version: manyforge.skill.v0\nskill_id: dup\nbody: {id: x, name: ok}\n"
            )
        catalog = load_skill_catalog(tmp_path)
        assert len(catalog.skills) == 1
