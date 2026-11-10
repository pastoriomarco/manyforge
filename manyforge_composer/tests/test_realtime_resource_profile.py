from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_HELPER = _REPO_ROOT / "scripts" / "lib" / "realtime-resources.sh"


def _available_cpus() -> list[int]:
    return sorted(os.sched_getaffinity(0))


def _profile_environment(cpus: list[int] | None = None) -> dict[str, str]:
    # Policy-unit tests use a synthetic five-CPU launch topology. Physical CPU
    # availability is a separate admission rule and has its own real-host test
    # below, so hosted CI does not need five actual CPUs to exercise the strict
    # isolation and priority contracts.
    cpus = cpus or [0, 1, 2, 3, 4]
    assert len(cpus) >= 5
    return {
        "MANYFORGE_RT_RESOURCE_PROFILE": "strict",
        "MANYFORGE_NRT_CPU_AFFINITY": str(cpus[0]),
        "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY": str(cpus[1]),
        "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY": str(cpus[2]),
        "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY": str(cpus[3]),
        # OD-58: StopControl on its own CPUs.
        "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY": str(cpus[4]),
    }


_SYNTHETIC_CPU_AVAILABILITY_PROBE = r"""
taskset() {
    if [[ "$#" -eq 3 && "$1" == "--cpu-list" && "$3" == "true" ]]; then
        return 0
    fi
    command taskset "$@"
}
"""


def _run_shell(
    script: str,
    environment: dict[str, str],
    *,
    assume_profile_cpus_available: bool = True,
) -> subprocess.CompletedProcess[str]:
    if assume_profile_cpus_available:
        script = f"{_SYNTHETIC_CPU_AVAILABILITY_PROBE}\n{script}"
    return subprocess.run(
        ["bash", "-lc", script],
        cwd=_REPO_ROOT,
        env={**os.environ, **environment},
        capture_output=True,
        text=True,
        check=False,
    )


def _validate(
    environment: dict[str, str],
    *,
    assume_profile_cpus_available: bool = True,
) -> subprocess.CompletedProcess[str]:
    return _run_shell(
        f"source {str(_HELPER)!r}; manyforge_validate_rt_resource_profile",
        environment,
        assume_profile_cpus_available=assume_profile_cpus_available,
    )


def test_strict_resource_profile_accepts_disjoint_ordered_reservations():
    completed = _validate(_profile_environment())

    assert completed.returncode == 0, completed.stderr


def test_strict_resource_profile_accepts_real_launch_topology_when_capacity_available():
    cpus = _available_cpus()
    if len(cpus) < 5:
        pytest.skip("launch context does not expose five CPUs")

    completed = _validate(
        _profile_environment(cpus[:5]),
        assume_profile_cpus_available=False,
    )

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


def test_strict_resource_profile_requires_its_own_stop_control_cpus():
    environment = _profile_environment()
    del environment["MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY"]

    completed = _validate(environment)

    assert completed.returncode != 0
    assert "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY" in completed.stderr


def test_strict_resource_profile_makes_stop_control_the_highest_lane():
    completed = _run_shell(
        f"source {str(_HELPER)!r}; manyforge_validate_rt_resource_profile && "
        'printf "%s\\n" "$MANYFORGE_RT_STOP_CONTROL_PRIORITY"',
        _profile_environment(),
    )
    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip().splitlines()[-1] == "48"

    environment = _profile_environment()
    environment["MANYFORGE_RT_STOP_CONTROL_PRIORITY"] = "45"
    completed = _validate(environment)
    assert completed.returncode != 0
    assert "stop_control > controller_command > runtime_safety" in completed.stderr


def test_strict_resource_profile_rejects_unbounded_cpu_range():
    environment = _profile_environment()
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = "0-2147483647"

    completed = _validate(environment)

    assert completed.returncode != 0
    assert "out-of-range CPU token" in completed.stderr


def test_strict_resource_profile_rejects_cpu_unavailable_to_launch_context():
    available = set(_available_cpus())
    unavailable = next((cpu for cpu in range(1024) if cpu not in available), None)
    if unavailable is None:
        pytest.skip("launch context exposes every taskset CPU in the supported test range")
    environment = _profile_environment()
    # NRT is checked first, so the test isolates the physical-admission rule
    # before any synthetic reservation can affect the result.
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = str(unavailable)

    completed = _validate(
        environment,
        assume_profile_cpus_available=False,
    )

    assert completed.returncode != 0
    assert "selects a CPU unavailable to this launch context" in completed.stderr
