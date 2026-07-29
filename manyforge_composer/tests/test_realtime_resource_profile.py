from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_HELPER = _REPO_ROOT / "scripts" / "lib" / "realtime-resources.sh"
_COMPOSE_HELPER = _REPO_ROOT / "scripts" / "lib" / "compose.sh"


def _available_cpus() -> list[int]:
    return sorted(os.sched_getaffinity(0))


def _profile_environment(cpus: list[int] | None = None) -> dict[str, str]:
    # Policy-unit tests use a synthetic four-CPU launch topology. Physical CPU
    # availability is a separate admission rule and has its own real-host test
    # below, so hosted CI does not need four actual CPUs to exercise the strict
    # isolation and priority contracts.
    cpus = cpus or [0, 1, 2, 3]
    assert len(cpus) >= 4
    return {
        "MANYFORGE_RT_RESOURCE_PROFILE": "strict",
        "MANYFORGE_NRT_CPU_AFFINITY": str(cpus[0]),
        "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY": str(cpus[1]),
        "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY": str(cpus[2]),
        "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY": str(cpus[3]),
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
    if len(cpus) < 4:
        pytest.skip("launch context does not expose four CPUs")

    completed = _validate(
        _profile_environment(cpus[:4]),
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


def test_native_composer_strict_profile_forwards_minimum_container_contract():
    completed = _run_shell(
        (
            f"source {str(_COMPOSE_HELPER)!r}; "
            "prepare_native_composer_rt_docker_args; "
            "printf '<%s>\\n' \"${NATIVE_COMPOSER_RT_DOCKER_ARGS[@]}\""
        ),
        _profile_environment(),
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


def test_native_composer_invalid_strict_profile_fails_without_docker_arguments():
    environment = _profile_environment()
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = "not-a-cpu"
    completed = _run_shell(
        (
            f"source {str(_COMPOSE_HELPER)!r}; "
            "set +e; "
            "prepare_native_composer_rt_docker_args; "
            "rc=$?; "
            "printf '<%s>\\n' \"${NATIVE_COMPOSER_RT_DOCKER_ARGS[@]}\"; "
            'exit "${rc}"'
        ),
        environment,
    )

    assert completed.returncode != 0
    assert "<--cap-add>" not in completed.stdout
    assert "invalid CPU token" in completed.stderr


def test_native_composer_build_cache_is_bound_to_immutable_image_id():
    image_id = "sha256:" + ("a" * 64)
    completed = _run_shell(
        (
            f"source {str(_COMPOSE_HELPER)!r}; "
            f"native_composer_build_cache_volume_for_image_id {image_id!r}"
        ),
        {},
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == (
        "manyforge_native-composer-build-cache-" + ("a" * 64)
    )

    invalid = _run_shell(
        (
            f"source {str(_COMPOSE_HELPER)!r}; "
            "native_composer_build_cache_volume_for_image_id latest"
        ),
        {},
    )
    assert invalid.returncode != 0

    source = _COMPOSE_HELPER.read_text(encoding="utf-8")
    assert '-v "${NATIVE_COMPOSER_BUILD_CACHE_VOLUME}:/tmp/manyforge-build"' in source
    assert '"${NATIVE_COMPOSER_IMAGE_ID}" \\' in source
    assert "manyforge_build-cache:/tmp/manyforge-build" not in source


def test_native_composer_resolves_tag_once_to_image_and_cache_identity():
    image_id = "sha256:" + ("c" * 64)
    completed = _run_shell(
        (
            "docker() {\n"
            "  [[ \"$1 $2 $3\" == \"image inspect manyforge-dev:latest\" ]]\n"
            f"  printf '%s\\n' {image_id!r}\n"
            "}\n"
            f"source {str(_COMPOSE_HELPER)!r}; "
            "resolve_native_composer_image; "
            "printf '%s|%s\\n' "
            '"${NATIVE_COMPOSER_IMAGE_ID}" '
            '"${NATIVE_COMPOSER_BUILD_CACHE_VOLUME}"'
        ),
        {},
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == (
        f"{image_id}|manyforge_native-composer-build-cache-" + ("c" * 64)
    )


def test_native_profile_can_reclaim_absent_controller_manager_cpu_for_nrt():
    environment = _profile_environment()
    environment["MANYFORGE_NRT_CPU_AFFINITY"] = ",".join(
        [
            environment["MANYFORGE_NRT_CPU_AFFINITY"],
            environment["MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY"],
        ]
    )
    completed = _run_shell(
        (
            f"source {str(_COMPOSE_HELPER)!r}; "
            "prepare_native_composer_rt_docker_args"
        ),
        environment,
    )

    assert completed.returncode == 0, completed.stderr
