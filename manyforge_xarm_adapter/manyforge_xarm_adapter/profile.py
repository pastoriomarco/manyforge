"""Strict loader for the experimental fixed-tool Lite6 adapter profile."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any

import yaml


PROFILE_SCHEMA = 'manyforge.xarm_adapter.lite6_profile.v1'
_SHA256 = re.compile(r'^[0-9a-f]{64}$')


@dataclass(frozen=True)
class Lite6Profile:
    """Generation and binding inputs that contain no cell-local secret."""

    profile_id: str
    robot_type: str
    dof: int
    limited: bool
    report_type: str
    ros_namespace: str
    prefix: str
    hardware_namespace: str
    controller_name: str
    tool_profile_id: str
    geometry_mesh_filename: str
    geometry_mesh_sha256: str
    geometry_mass_kg: float
    geometry_mesh_origin_xyz: str
    geometry_mesh_origin_rpy: str
    geometry_mesh_tcp_xyz: str
    geometry_mesh_tcp_rpy: str
    address_source_env: str
    source_repository: str
    xc4_base_commit: str
    xc4_base_tree: str


def _mapping(value: Any, label: str, expected: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f'{label} must be a mapping')
    unknown = sorted(set(value) - expected)
    missing = sorted(expected - set(value))
    if unknown:
        raise ValueError(f"{label} has unsupported keys: {', '.join(unknown)}")
    if missing:
        raise ValueError(f"{label} is missing keys: {', '.join(missing)}")
    return value


def _text(value: Any, label: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise ValueError(f'{label} must be a string')
    if not allow_empty and not value:
        raise ValueError(f'{label} must not be empty')
    return value


def load_profile(path: Path) -> Lite6Profile:
    """Load one exact profile and reject silent schema drift."""
    payload = yaml.safe_load(path.read_text(encoding='utf-8'))
    root = _mapping(
        payload,
        'profile',
        {'schema', 'profile_id', 'robot', 'tool', 'network', 'source'},
    )
    if root['schema'] != PROFILE_SCHEMA:
        raise ValueError(
            f"unsupported profile schema {root['schema']!r}; expected {PROFILE_SCHEMA!r}"
        )

    robot = _mapping(
        root['robot'],
        'profile.robot',
        {
            'robot_type',
            'dof',
            'limited',
            'report_type',
            'ros_namespace',
            'prefix',
            'hardware_namespace',
            'controller_name',
        },
    )
    tool = _mapping(
        root['tool'],
        'profile.tool',
        {
            'profile_id',
            'geometry_mesh_filename',
            'geometry_mesh_sha256',
            'geometry_mass_kg',
            'geometry_mesh_origin_xyz',
            'geometry_mesh_origin_rpy',
            'geometry_mesh_tcp_xyz',
            'geometry_mesh_tcp_rpy',
        },
    )
    network = _mapping(
        root['network'],
        'profile.network',
        {'address_source_env'},
    )
    source = _mapping(
        root['source'],
        'profile.source',
        {'repository', 'xc4_base_commit', 'xc4_base_tree'},
    )

    dof = robot['dof']
    if not isinstance(dof, int) or isinstance(dof, bool) or dof != 6:
        raise ValueError('profile.robot.dof must be exactly 6')
    limited = robot['limited']
    if limited is not True:
        raise ValueError('profile.robot.limited must be true')
    if robot['robot_type'] != 'lite':
        raise ValueError("profile.robot.robot_type must be 'lite'")
    if robot['report_type'] != 'rich':
        raise ValueError("profile.robot.report_type must be 'rich'")

    mass = tool['geometry_mass_kg']
    if not isinstance(mass, (int, float)) or isinstance(mass, bool) or mass <= 0:
        raise ValueError('profile.tool.geometry_mass_kg must be positive')
    mesh_sha256 = _text(
        tool['geometry_mesh_sha256'],
        'profile.tool.geometry_mesh_sha256',
    )
    if not _SHA256.fullmatch(mesh_sha256):
        raise ValueError(
            'profile.tool.geometry_mesh_sha256 must be 64 lowercase hex digits'
        )
    mesh_filename = _text(
        tool['geometry_mesh_filename'],
        'profile.tool.geometry_mesh_filename',
    )
    if Path(mesh_filename).name != mesh_filename or mesh_filename in {'.', '..'}:
        raise ValueError(
            'profile.tool.geometry_mesh_filename must be one plain filename'
        )

    return Lite6Profile(
        profile_id=_text(root['profile_id'], 'profile.profile_id'),
        robot_type=robot['robot_type'],
        dof=dof,
        limited=limited,
        report_type=_text(robot['report_type'], 'profile.robot.report_type'),
        ros_namespace=_text(
            robot['ros_namespace'],
            'profile.robot.ros_namespace',
            allow_empty=True,
        ),
        prefix=_text(robot['prefix'], 'profile.robot.prefix', allow_empty=True),
        hardware_namespace=_text(
            robot['hardware_namespace'],
            'profile.robot.hardware_namespace',
        ),
        controller_name=_text(
            robot['controller_name'],
            'profile.robot.controller_name',
        ),
        tool_profile_id=_text(tool['profile_id'], 'profile.tool.profile_id'),
        geometry_mesh_filename=mesh_filename,
        geometry_mesh_sha256=mesh_sha256,
        geometry_mass_kg=float(mass),
        geometry_mesh_origin_xyz=_text(
            tool['geometry_mesh_origin_xyz'],
            'profile.tool.geometry_mesh_origin_xyz',
        ),
        geometry_mesh_origin_rpy=_text(
            tool['geometry_mesh_origin_rpy'],
            'profile.tool.geometry_mesh_origin_rpy',
        ),
        geometry_mesh_tcp_xyz=_text(
            tool['geometry_mesh_tcp_xyz'],
            'profile.tool.geometry_mesh_tcp_xyz',
        ),
        geometry_mesh_tcp_rpy=_text(
            tool['geometry_mesh_tcp_rpy'],
            'profile.tool.geometry_mesh_tcp_rpy',
        ),
        address_source_env=_text(
            network['address_source_env'],
            'profile.network.address_source_env',
        ),
        source_repository=_text(source['repository'], 'profile.source.repository'),
        xc4_base_commit=_text(
            source['xc4_base_commit'],
            'profile.source.xc4_base_commit',
        ),
        xc4_base_tree=_text(source['xc4_base_tree'], 'profile.source.xc4_base_tree'),
    )
