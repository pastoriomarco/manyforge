"""Container launch, cache isolation and failure contracts for diagnostics."""

from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


_ROOT = Path(__file__).resolve().parents[1]
_LAUNCHER = _ROOT / "scripts/diagnostic-build.sh"


@pytest.fixture
def docker_stub(tmp_path: Path) -> dict[str, str]:
    binary_root = tmp_path / "bin"
    binary_root.mkdir()
    docker = binary_root / "docker"
    docker.write_text(
        f"#!{sys.executable}\n"
        "import json, os, sys\n"
        "from pathlib import Path\n"
        "args = sys.argv[1:]\n"
        "with Path(os.environ['DOCKER_CALLS']).open('a') as calls:\n"
        "    calls.write(json.dumps(args) + '\\n')\n"
        "if args[:2] == ['image', 'inspect']:\n"
        "    print('amd64' if '.Architecture' in args[3] else 'sha256:' + '1' * 64)\n"
        "elif args[0] == 'run' and '/bin/sh' in args:\n"
        "    print('ccache version 4.7.5\\ncompiler-fingerprint')\n"
        "elif args[0] == 'create':\n"
        "    print('test-container-id')\n"
        "elif args[0] == 'start':\n"
        "    sys.exit(int(os.environ.get('DOCKER_RUN_RESULT', '0')))\n",
        encoding="utf-8",
    )
    docker.chmod(0o755)
    return {
        **os.environ,
        "PATH": f"{binary_root}:{os.environ['PATH']}",
        "DOCKER_CALLS": str(tmp_path / "docker-calls.jsonl"),
        "MANYFORGE_DIAGNOSTIC_CACHE_BASE": str(tmp_path / "cache"),
        "CMAKE_BUILD_PARALLEL_LEVEL": "4",
        "CMAKE_GENERATOR": "Ninja",
        "CFLAGS": "",
        "CXXFLAGS": "",
        "LDFLAGS": "",
    }


def _launch(env: dict[str, str], variant: str = "core-release") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(_LAUNCHER), variant, "--", "cmake", "--version"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


def _tree(result: subprocess.CompletedProcess[str]) -> Path:
    return Path(next(line.removeprefix("Diagnostic tree: ") for line in result.stdout.splitlines()
                     if line.startswith("Diagnostic tree: ")))


def test_reuses_exact_tree_and_isolates_variant_and_flags(docker_stub: dict[str, str]) -> None:
    first = _launch(docker_stub)
    repeat = _launch(docker_stub)
    other_variant = _launch(docker_stub, "core-asan")
    other_flags = _launch({**docker_stub, "CXXFLAGS": "-fsanitize=address"})
    assert all(result.returncode == 0 for result in (first, repeat, other_variant, other_flags))
    assert _tree(first) == _tree(repeat)
    assert len({_tree(first), _tree(other_variant), _tree(other_flags)}) == 3


def test_isolates_source_checkouts_under_one_variant(
    docker_stub: dict[str, str], tmp_path: Path
) -> None:
    other_source = tmp_path / "other-checkout"
    other_source.mkdir()
    first = _launch(docker_stub)
    other = _launch({**docker_stub, "MANYFORGE_DIAGNOSTIC_SOURCE": str(other_source)})
    assert first.returncode == 0 and other.returncode == 0
    assert _tree(first) != _tree(other)
    assert f"SOURCE={other_source.resolve()}" in (
        _tree(other).parent / "configuration.txt"
    ).read_text()


def test_variants_share_one_compiler_cache(
    docker_stub: dict[str, str],
) -> None:
    assert _launch(docker_stub).returncode == 0
    assert _launch(docker_stub, "core-asan").returncode == 0
    calls = [json.loads(line) for line in Path(docker_stub["DOCKER_CALLS"]).read_text().splitlines()]
    launches = [call for call in calls if call[0] == "create"]
    caches = {
        argument for launch in launches for argument in launch
        if argument.startswith("type=bind,") and argument.endswith(",target=/ccache")
    }
    assert len(launches) == 2 and len(caches) == 1
    assert all("CCACHE_DIR=/ccache" in launch for launch in launches)


def test_launch_uses_pinned_image_stable_paths_and_explicit_cache_activation(
    docker_stub: dict[str, str],
) -> None:
    assert _launch(docker_stub).returncode == 0
    calls = [json.loads(line) for line in Path(docker_stub["DOCKER_CALLS"]).read_text().splitlines()]
    launch = next(call for call in calls if call[0] == "create")
    assert "sha256:" + "1" * 64 in launch
    assert f"type=bind,source={_ROOT},target=/workspace,readonly" in launch
    assert "HOME=/runner/home" in launch
    assert "MANYFORGE_BUILD_DIR=/runner/build" in launch
    assert "CCACHE_MAXSIZE=20G" in launch
    assert "CMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache" in launch
    assert launch[launch.index("-u") + 1] == "CCACHE_DISABLE"
    assert launch[-2:] == ["cmake", "--version"]


def test_returns_container_failure(docker_stub: dict[str, str]) -> None:
    result = _launch({**docker_stub, "DOCKER_RUN_RESULT": "23"})
    assert result.returncode == 23
    calls = [json.loads(line) for line in Path(docker_stub["DOCKER_CALLS"]).read_text().splitlines()]
    assert calls[-1] == ["rm", "--force", "test-container-id"]


def test_rejects_concurrent_tree_owner(docker_stub: dict[str, str]) -> None:
    first = _launch(docker_stub)
    assert first.returncode == 0
    with (_tree(first).parent / "build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = _launch(docker_stub)
    assert result.returncode == 75
    assert "Diagnostic tree is in use" in result.stderr


@pytest.mark.parametrize("variant", ("../escape", "bad/name", ""))
def test_rejects_invalid_variant_before_docker(docker_stub: dict[str, str], variant: str) -> None:
    assert _launch(docker_stub, variant).returncode == 64
    assert not Path(docker_stub["DOCKER_CALLS"]).exists()


@pytest.mark.parametrize("setting", ({"CMAKE_BUILD_PARALLEL_LEVEL": "0"}, {"CMAKE_GENERATOR": "Unix Makefiles"}))
def test_rejects_invalid_configuration_before_docker(
    docker_stub: dict[str, str], setting: dict[str, str],
) -> None:
    assert _launch({**docker_stub, **setting}).returncode == 64
    assert not Path(docker_stub["DOCKER_CALLS"]).exists()
