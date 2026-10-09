"""Explicit ROS experiment graph and diagnostic configuration contracts."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

_ROOT = Path(__file__).resolve().parents[1]
_BUILDER = _ROOT / "scripts/ros/build-motion-packages.sh"
_PACKAGES = ("manyforge_isaac_hardware", "manyforge_motion_controller")


@pytest.fixture
def build_env(tmp_path: Path) -> dict[str, str]:
    source = tmp_path / "source"
    for package in _PACKAGES:
        directory = source / package
        directory.mkdir(parents=True)
        (directory / "CMakeLists.txt").touch()
        (directory / "package.xml").touch()
    binary_root = tmp_path / "bin"
    binary_root.mkdir()
    stub = binary_root / "colcon"
    stub.write_text(
        f"#!{sys.executable}\n"
        "import json, os, sys\n"
        "from pathlib import Path\n"
        "args = sys.argv[1:]\n"
        "with Path(os.environ['COLCON_CALLS']).open('a') as calls:\n"
        "    calls.write(json.dumps({'args': args, 'prefix': os.environ.get('CMAKE_PREFIX_PATH', ''), 'defaults': os.environ.get('COLCON_DEFAULTS_FILE')}) + '\\n')\n"
        "if 'list' in args:\n"
        "    if os.environ.get('COLCON_LIST_RESULT'): sys.exit(int(os.environ['COLCON_LIST_RESULT']))\n"
        "    if 'COLCON_INVENTORY' in os.environ: print(os.environ['COLCON_INVENTORY'])\n"
        "    else: print('\\n'.join(Path(p).name for p in args[args.index('--base-paths') + 1:-1]))\n"
        "else:\n"
        "    result = int(os.environ.get('COLCON_RESULT', '0'))\n"
        "    if result: sys.exit(result)\n"
        "    install = Path(args[args.index('--install-base') + 1])\n"
        "    install.mkdir(parents=True, exist_ok=True)\n"
        "    (install / 'setup.bash').touch()\n",
        encoding="utf-8",
    )
    stub.chmod(0o755)
    return {
        **os.environ,
        "PATH": f"{binary_root}:{os.environ['PATH']}",
        "WORKSPACE": str(source),
        "MANYFORGE_BUILD_DIR": str(tmp_path / "build"),
        "COLCON_CALLS": str(tmp_path / "calls.jsonl"),
        "ROS_DISTRO": "jazzy",
        "CMAKE_GENERATOR": "Ninja",
        "CMAKE_BUILD_PARALLEL_LEVEL": "8",
        "CMAKE_C_COMPILER_LAUNCHER": "/usr/bin/ccache",
        "CMAKE_CXX_COMPILER_LAUNCHER": "/usr/bin/ccache",
        "MANYFORGE_MOTION_BUILD_TYPE": "Release",
        "MANYFORGE_MOTION_BUILD_TESTING": "ON",
    }


def _build(env: dict[str, str], *packages: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(_BUILDER), *packages], env=env, capture_output=True, text=True)


def _calls(env: dict[str, str]) -> list[dict[str, object]]:
    return [json.loads(line) for line in Path(env["COLCON_CALLS"]).read_text().splitlines()]


@pytest.mark.parametrize("packages", [_PACKAGES, _PACKAGES[:1], _PACKAGES[1:]])
def test_builds_only_explicit_packages(build_env: dict[str, str], packages: tuple[str, ...]) -> None:
    assert _build(build_env, *packages).returncode == 0
    calls = _calls(build_env)
    assert all(call['defaults'] == '/dev/null' for call in calls)
    # Even colcon inventory discovery writes logs; keep it off the read-only
    # source mount as well as the subsequent build.
    assert all(call['args'][:2] == [
        '--log-base', str(Path(build_env['MANYFORGE_BUILD_DIR']) / 'motion-ros-overlay/log')
    ] for call in calls)
    args = calls[-1]["args"]
    assert isinstance(args, list)
    assert args[args.index('--base-paths') + 1:args.index('--build-base')] == [
        str(Path(build_env['WORKSPACE']) / package) for package in packages
    ]
    assert args[args.index('--packages-select') + 1:args.index('--cmake-args')] == list(packages)
    assert args[args.index('--executor') + 1] == 'sequential'
    assert args[args.index('--cmake-args') + 1:] == [
        '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=ON',
        '-DCMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache',
        '-DCMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache',
    ]
    assert args[args.index('--install-base') + 1] == str(
        Path(build_env['MANYFORGE_BUILD_DIR']) / 'motion-ros-overlay/install'
    )


def test_clears_inherited_overlay_prefixes(build_env: dict[str, str]) -> None:
    env = {**build_env, 'CMAKE_PREFIX_PATH': '/unqualified/cmeel.prefix',
           'AMENT_PREFIX_PATH': '/unqualified/overlay'}
    assert _build(env, *_PACKAGES).returncode == 0
    assert all('/unqualified' not in str(call['prefix']) for call in _calls(env))


def test_disables_explicit_user_defaults_for_inventory_and_build(build_env: dict[str, str]) -> None:
    env = {**build_env, 'COLCON_DEFAULTS_FILE': '/unqualified/defaults.yaml'}
    assert _build(env, *_PACKAGES).returncode == 0
    assert all(call['defaults'] == '/dev/null' for call in _calls(env))


@pytest.mark.parametrize('packages', [(), ('manyforge_ros2_control_execution',),
                                     ('../manyforge_core',), (_PACKAGES[0], _PACKAGES[0])])
def test_rejects_missing_unavailable_or_duplicate_selection(
    build_env: dict[str, str], packages: tuple[str, ...]
) -> None:
    assert _build(build_env, *packages).returncode == 64
    assert not Path(build_env['COLCON_CALLS']).exists()


@pytest.mark.parametrize('setting', [
    {'ROS_DISTRO': 'lyrical'}, {'MANYFORGE_MOTION_BUILD_TYPE': 'MinSizeRel'},
    {'MANYFORGE_MOTION_BUILD_TESTING': 'yes'}, {'CMAKE_GENERATOR': 'Unix Makefiles'},
    {'CMAKE_BUILD_PARALLEL_LEVEL': '0'}, {'CMAKE_CXX_COMPILER_LAUNCHER': ''},
])
def test_rejects_invalid_diagnostic_configuration_before_colcon(
    build_env: dict[str, str], setting: dict[str, str]
) -> None:
    assert _build({**build_env, **setting}, *_PACKAGES).returncode == 64
    assert not Path(build_env['COLCON_CALLS']).exists()


def test_rejects_incomplete_source_before_colcon(build_env: dict[str, str]) -> None:
    (Path(build_env['WORKSPACE']) / _PACKAGES[0] / 'package.xml').unlink()
    assert _build(build_env, *_PACKAGES).returncode == 69
    assert not Path(build_env['COLCON_CALLS']).exists()


@pytest.mark.parametrize('inventory', ['', 'renamed_hardware', '\n'.join((*_PACKAGES, 'nested_package'))])
def test_rejects_package_inventory_drift(build_env: dict[str, str], inventory: str) -> None:
    result = _build({**build_env, 'COLCON_INVENTORY': inventory}, *_PACKAGES)
    assert result.returncode == 69
    assert 'inventory mismatch' in result.stderr
    assert len(_calls(build_env)) == 1


@pytest.mark.parametrize('setting', [{'COLCON_RESULT': '23'}, {'COLCON_LIST_RESULT': '23'}])
def test_preserves_colcon_failure(build_env: dict[str, str], setting: dict[str, str]) -> None:
    assert _build({**build_env, **setting}, *_PACKAGES).returncode == 23


def test_explicit_build_options_and_persistent_output(build_env: dict[str, str]) -> None:
    env = {**build_env, 'MANYFORGE_MOTION_BUILD_TYPE': 'Debug', 'MANYFORGE_MOTION_BUILD_TESTING': 'OFF'}
    assert _build(env, *_PACKAGES).returncode == 0
    marker = Path(env['MANYFORGE_BUILD_DIR']) / 'motion-ros-overlay/install/retained-marker'
    marker.touch()
    assert _build(env, *_PACKAGES).returncode == 0
    assert marker.exists()
    assert '-DCMAKE_BUILD_TYPE=Debug' in _calls(env)[-1]['args']
    assert '-DBUILD_TESTING=OFF' in _calls(env)[-1]['args']


@pytest.mark.parametrize('default_location', ['explicit', 'home'])
def test_real_colcon_cannot_skip_selected_package_with_user_defaults(
    build_env: dict[str, str], tmp_path: Path, default_location: str
) -> None:
    """Prove the defaults skip a real package, then prove the builder builds it."""
    package = _PACKAGES[1]
    source = Path(build_env['WORKSPACE']) / package
    (source / 'package.xml').write_text(
        '<?xml version="1.0"?>\n<package format="3">\n'
        f'<name>{package}</name><version>0.0.0</version>\n'
        '<description>Colcon defaults regression fixture</description>\n'
        '<maintainer email="dev@example.test">Dev</maintainer>\n'
        '<license>Apache-2.0</license><buildtool_depend>ament_cmake</buildtool_depend>\n'
        '<export><build_type>ament_cmake</build_type></export></package>\n',
        encoding='utf-8',
    )
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.16)\n'
        f'project({package} LANGUAGES NONE)\n'
        'find_package(ament_cmake REQUIRED)\n'
        'install(FILES selection-marker DESTINATION share/${PROJECT_NAME})\n'
        'ament_package()\n',
        encoding='utf-8',
    )
    (source / 'selection-marker').write_text('selected-package-built\n', encoding='utf-8')
    task_home = tmp_path / 'user-home'
    defaults = task_home / '.colcon/defaults.yaml'
    defaults.parent.mkdir(parents=True)
    defaults.write_text(f'build:\n  packages-skip:\n    - {package}\n', encoding='utf-8')
    env = {
        **build_env,
        'PATH': os.environ['PATH'],  # Exercise actual colcon, bypassing the fixture stub.
        'HOME': str(task_home),
        'MANYFORGE_MOTION_BUILD_TESTING': 'OFF',
    }
    env.pop('COLCON_DEFAULTS_FILE', None)
    env.pop('COLCON_HOME', None)
    if default_location == 'explicit':
        env['COLCON_DEFAULTS_FILE'] = str(defaults)

    # Establish that this installed colcon honors the hostile defaults. A
    # zero exit here with no installed marker exposes the original failure.
    baseline_install = tmp_path / 'baseline-install'
    baseline = subprocess.run(
        ['bash', '-c', 'set +u; source /opt/ros/jazzy/setup.bash; exec colcon "$@"',
         'colcon-defaults-baseline', '--log-base', str(tmp_path / 'baseline-log'),
         'build', '--base-paths', str(source),
         '--build-base', str(tmp_path / 'baseline-build'),
         '--install-base', str(baseline_install), '--packages-select', package,
         '--cmake-args', '-DBUILD_TESTING=OFF'],
        env=env, cwd=tmp_path, capture_output=True, text=True,
    )
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr
    assert not (baseline_install / package / 'share' / package / 'selection-marker').exists()

    # Retained output must not turn a skipped rebuild into false success.
    install = Path(env['MANYFORGE_BUILD_DIR']) / 'motion-ros-overlay/install'
    install.mkdir(parents=True)
    (install / 'setup.bash').touch()
    result = _build(env, package)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (install / package / 'share' / package / 'selection-marker').read_text() == (
        'selected-package-built\n'
    )
