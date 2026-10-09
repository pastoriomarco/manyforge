"""Pinned local ROS image extension without a source or ROS dependency rebuild."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

_ROOT = Path(__file__).resolve().parents[1]
_BUILDER = _ROOT / 'scripts/ros/build-jazzy-cache-image.sh'
_BASE_ID = 'sha256:' + '1' * 64


@pytest.fixture
def image_env(tmp_path: Path) -> dict[str, str]:
    binary_root = tmp_path / 'bin'
    binary_root.mkdir()
    stub = binary_root / 'docker'
    stub.write_text(
        f'#!{sys.executable}\n'
        'import json, os, sys\n'
        'from pathlib import Path\n'
        'args = sys.argv[1:]\n'
        "with Path(os.environ['DOCKER_CALLS']).open('a') as calls:\n"
        "    calls.write(json.dumps(args) + '\\n')\n"
        "if args[:2] == ['image', 'inspect']:\n"
        "    if args[-1].startswith('manyforge-ros-jazzy-cache-base:'):\n"
        "        if not os.environ.get('BASE_ALIAS'): sys.exit(1)\n"
        "        print(os.environ['BASE_ALIAS'])\n"
        "    else: print('sha256:' + '1' * 64)\n"
        "elif args[0] == 'build':\n"
        "    Path(os.environ['DOCKERFILE_CAPTURE']).write_text(sys.stdin.read())\n",
        encoding='utf-8',
    )
    stub.chmod(0o755)
    return {**os.environ, 'PATH': f'{binary_root}:{os.environ["PATH"]}',
            'DOCKER_CALLS': str(tmp_path / 'calls.jsonl'),
            'DOCKERFILE_CAPTURE': str(tmp_path / 'Dockerfile'),
            'MANYFORGE_DIAGNOSTIC_IMAGE': 'test-ros-cache:local'}


def _build(env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(_BUILDER), *args], env=env, capture_output=True, text=True)


def _calls(env: dict[str, str]) -> list[list[str]]:
    return [json.loads(line) for line in Path(env['DOCKER_CALLS']).read_text().splitlines()]


def test_requires_explicit_local_base_before_docker(image_env: dict[str, str]) -> None:
    assert _build(image_env).returncode == 64
    assert not Path(image_env['DOCKER_CALLS']).exists()


def test_builds_cache_only_layer_from_resolved_local_image(image_env: dict[str, str]) -> None:
    assert _build(image_env, 'user-jazzy:existing').returncode == 0
    calls = _calls(image_env)
    alias = 'manyforge-ros-jazzy-cache-base:' + '1' * 64
    assert ['image', 'tag', _BASE_ID, alias] in calls
    build = next(call for call in calls if call[0] == 'build')
    assert '--pull=false' in build
    assert f'MANYFORGE_ROS_BASE={alias}' in build
    assert f'MANYFORGE_ROS_BASE_ID={_BASE_ID}' in build
    assert build[-1] == '-'
    dockerfile = Path(image_env['DOCKERFILE_CAPTURE']).read_text()
    assert 'ccache=4.9.1-1' in dockerfile
    assert 'CCACHE_DISABLE=1' in dockerfile
    assert 'ENTRYPOINT []' in dockerfile
    assert 'ubuntu:noble' in dockerfile
    assert 'ros-jazzy-' not in dockerfile


def test_reuses_matching_immutable_alias(image_env: dict[str, str]) -> None:
    assert _build({**image_env, 'BASE_ALIAS': _BASE_ID}, 'user-jazzy:existing').returncode == 0
    assert not any(call[:2] == ['image', 'tag'] for call in _calls(image_env))


def test_rejects_conflicting_alias_without_build_or_retag(image_env: dict[str, str]) -> None:
    result = _build({**image_env, 'BASE_ALIAS': 'sha256:' + '2' * 64}, 'user-jazzy:existing')
    assert result.returncode == 69
    assert 'different image ID' in result.stderr
    assert not any(call[0] == 'build' or call[:2] == ['image', 'tag'] for call in _calls(image_env))
