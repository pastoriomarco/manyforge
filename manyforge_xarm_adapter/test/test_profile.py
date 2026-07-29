from __future__ import annotations

from pathlib import Path

from manyforge_xarm_adapter.profile import load_profile
from manyforge_xarm_adapter.profile import PROFILE_SCHEMA
import pytest
import yaml


_PROFILE = Path(__file__).resolve().parents[1] / 'config' / (
    'lite6_fixed_pneumatic.profile.yaml'
)


def test_checked_in_profile_freezes_manymove_fixed_tool_inputs() -> None:
    profile = load_profile(_PROFILE)

    assert profile.robot_type == 'lite'
    assert profile.dof == 6
    assert profile.limited is True
    assert profile.geometry_mesh_filename == 'pneumatic_lite.stl'
    assert (
        profile.geometry_mesh_sha256
        == '6ec37b0104b946e08515f6f4c5707fdefe1e4f74448f4e1ed60b2719898afa4e'
    )
    assert profile.geometry_mesh_tcp_xyz == '0.03075 0 0.11885'
    assert profile.geometry_mesh_tcp_rpy == '0 0.52 0'
    assert profile.address_source_env == 'MANYFORGE_LITE6_ROBOT_IP'


def test_profile_rejects_unknown_fields(tmp_path: Path) -> None:
    payload = yaml.safe_load(_PROFILE.read_text(encoding='utf-8'))
    assert payload['schema'] == PROFILE_SCHEMA
    payload['robot']['auto_arm'] = True
    candidate = tmp_path / 'profile.yaml'
    candidate.write_text(yaml.safe_dump(payload), encoding='utf-8')

    with pytest.raises(ValueError, match='unsupported keys'):
        load_profile(candidate)


def test_profile_rejects_unqualified_mesh_digest(tmp_path: Path) -> None:
    payload = yaml.safe_load(_PROFILE.read_text(encoding='utf-8'))
    payload['tool']['geometry_mesh_sha256'] = 'not-a-digest'
    candidate = tmp_path / 'profile.yaml'
    candidate.write_text(yaml.safe_dump(payload), encoding='utf-8')

    with pytest.raises(ValueError, match='64 lowercase hex digits'):
        load_profile(candidate)


def test_profile_requires_rich_report_stream(tmp_path: Path) -> None:
    payload = yaml.safe_load(_PROFILE.read_text(encoding='utf-8'))
    payload['robot']['report_type'] = 'normal'
    candidate = tmp_path / 'profile.yaml'
    candidate.write_text(yaml.safe_dump(payload), encoding='utf-8')

    with pytest.raises(ValueError, match="report_type must be 'rich'"):
        load_profile(candidate)


def test_profile_rejects_mesh_path_traversal(tmp_path: Path) -> None:
    payload = yaml.safe_load(_PROFILE.read_text(encoding='utf-8'))
    payload['tool']['geometry_mesh_filename'] = '../pneumatic_lite.stl'
    candidate = tmp_path / 'profile.yaml'
    candidate.write_text(yaml.safe_dump(payload), encoding='utf-8')

    with pytest.raises(ValueError, match='one plain filename'):
        load_profile(candidate)
