from __future__ import annotations

import os
from pathlib import Path
import subprocess


_REPO_ROOT = Path(__file__).resolve().parents[2]
_HELPER = _REPO_ROOT / "scripts" / "lib" / "realtime-resources.sh"
_COMPOSE_HELPER = _REPO_ROOT / "scripts" / "lib" / "compose.sh"


def _available_cpus() -> list[int]:
    return sorted(os.sched_getaffinity(0))


def _profile_environment() -> dict[str, str]:
    cpus = _available_cpus()
    assert len(cpus) >= 4
    return {
        "MANYFORGE_RT_RESOURCE_PROFILE": "strict",
        "MANYFORGE_NRT_CPU_AFFINITY": str(cpus[0]),
        "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY": str(cpus[1]),
        "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY": str(cpus[2]),
        "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY": str(cpus[3]),
    }


def _validate(environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "bash",
            "-lc",
            f"source {str(_HELPER)!r}; manyforge_validate_rt_resource_profile",
        ],
        cwd=_REPO_ROOT,
        env={**os.environ, **environment},
        capture_output=True,
        text=True,
        check=False,
    )


def test_strict_resource_profile_accepts_disjoint_ordered_reservations():
    completed = _validate(_profile_environment())

    assert completed.returncode == 0, completed.stderr


def test_strict_resource_profile_rejects_nrt_overlap():
    environment = _profile_environment()
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = environment[
        "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY"
    ]

    completed = _validate(environment)

    assert completed.returncode != 0
    assert "must be disjoint" in completed.stderr


def test_strict_resource_profile_rejects_inverted_priority_order():
    environment = _profile_environment()
    environment["MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY"] = "46"

    completed = _validate(environment)

    assert completed.returncode != 0
    assert "controller_manager > controller_command" in completed.stderr


def test_strict_resource_profile_rejects_unbounded_cpu_range():
    environment = _profile_environment()
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = "0-2147483647"

    completed = _validate(environment)

    assert completed.returncode != 0
    assert "out-of-range CPU token" in completed.stderr


def test_native_composer_strict_profile_forwards_minimum_container_contract():
    completed = subprocess.run(
        [
            "bash",
            "-lc",
            (
                f"source {str(_COMPOSE_HELPER)!r}; "
                "prepare_native_composer_rt_docker_args; "
                "printf '<%s>\\n' \"${NATIVE_COMPOSER_RT_DOCKER_ARGS[@]}\""
            ),
        ],
        cwd=_REPO_ROOT,
        env={**os.environ, **_profile_environment()},
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    arguments = completed.stdout.splitlines()
    assert "<--cap-add>" in arguments
    assert "<SYS_NICE>" in arguments
    assert "<--ulimit>" in arguments
    assert "<rtprio=99:99>" in arguments
    assert "<-e>" in arguments
    assert "<MANYFORGE_RT_RESOURCE_PROFILE=strict>" in arguments
    assert "<MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=false>" in arguments
    assert any(
        value.startswith("<MANYFORGE_NRT_CPU_AFFINITY=")
        for value in arguments
    )

    source = _COMPOSE_HELPER.read_text(encoding="utf-8")
    assert "manyforge_exec_with_nrt_affinity python -m manyforge_composer" in source


def test_native_profile_can_reclaim_absent_controller_manager_cpu_for_nrt():
    environment = _profile_environment()
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = ",".join(
        [
            environment["MANYFORGE_NRT_CPU_AFFINITY"],
            environment["MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY"],
        ]
    )
    completed = subprocess.run(
        [
            "bash",
            "-lc",
            (
                f"source {str(_COMPOSE_HELPER)!r}; "
                "prepare_native_composer_rt_docker_args"
            ),
        ],
        cwd=_REPO_ROOT,
        env={**os.environ, **environment},
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
