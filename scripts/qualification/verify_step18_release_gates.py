#!/usr/bin/env python3
"""Verify the corrected-source Step 18 release-gate campaign.

This verifier owns only the conventional regression, architecture-hygiene,
and sanitizer gates.  A passing summary deliberately does not claim overall
Step 18 closure; the common-candidate aggregator must combine it with the
proof, benchmark, loaded-kernel, and live-service evidence families.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

from source_identity import (
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
    validate_identity,
    validate_specs_contract_identity,
)


LANE_SCHEMA = "manyforge.step18.release_gate_lane.v1"
ARTIFACT_MANIFEST_SCHEMA = "manyforge.step18.release_gate_artifact_manifest.v1"
SUMMARY_SCHEMA = "manyforge.step18.release_gate_verification.v1"
VERIFICATION_SCOPE = "corrected_source_release_gates"

LANES = (
    "full_dev_regression",
    "full_ros_regression",
    "contract_architecture_hygiene",
    "core_asan_ubsan",
    "core_tsan",
    "collision_asan_ubsan",
    "collision_tsan",
)
SANITIZER_LANES = frozenset(
    {
        "core_asan_ubsan",
        "core_tsan",
        "collision_asan_ubsan",
        "collision_tsan",
    }
)
ROS_FULL_LANES = frozenset({"full_ros_regression", *SANITIZER_LANES})
ROS_AMBIENT_PASSTHROUGH_VARIABLES = (
    "FASTDDS_DEFAULT_PROFILES_FILE",
    "FASTRTPS_DEFAULT_PROFILES_FILE",
    "ROS_AUTOMATIC_DISCOVERY_RANGE",
    "ROS_STATIC_PEERS",
)

_LANE_SERVICE = {
    "full_dev_regression": "dev",
    "full_ros_regression": "ros-jazzy-full",
    "contract_architecture_hygiene": "dev",
    "core_asan_ubsan": "ros-jazzy-full",
    "core_tsan": "ros-jazzy-full",
    "collision_asan_ubsan": "ros-jazzy-full",
    "collision_tsan": "ros-jazzy-full",
}
_SANITIZER_PREFIX = {
    "core_asan_ubsan": "CORE_ASAN_UBSAN",
    "core_tsan": "CORE_TSAN",
    "collision_asan_ubsan": "COLLISION_ASAN_UBSAN",
    "collision_tsan": "COLLISION_TSAN",
}
_SANITIZER_SCRIPT = {
    "core_asan_ubsan": "scripts/ci/run-core-asan.sh",
    "core_tsan": "scripts/ci/run-core-tsan.sh",
    "collision_asan_ubsan": "scripts/ci/run-collision-engine-asan.sh",
    "collision_tsan": "scripts/ci/run-collision-engine-tsan.sh",
}
HYGIENE_CHECKS = (
    ("generated_public_contracts", "bash scripts/ci/check-public-contracts-fresh.sh"),
    ("version_sync", "bash scripts/ci/verify-version-sync.sh"),
    ("private_spec_boundary", "bash scripts/ci/check-private-spec-boundary.sh"),
    ("terminal_commit_boundary", "bash scripts/ci/check-terminal-commit-boundary.sh"),
    ("authority_port_alignment", "python3 scripts/ci/check-authority-port-alignment.py"),
    ("planner_portability", "python3 scripts/ci/check-planner-portability.py"),
    ("roboplan_dependency_stack", "python3 scripts/ci/check-roboplan-dependency-stack.py"),
    ("viewer_boundary", "python3 scripts/ci/check-viewer-data-plane-boundaries.py"),
    (
        "step18_removed_field_and_distance_scope",
        "python3 scripts/qualification/verify_step18_release_gates.py "
        "hygiene --repo-root /workspace",
    ),
)

_BASE_INPUT_PATHS = {
    "docker-compose.yaml",
    "scripts/qualification/run_step18_release_gates.py",
    "scripts/qualification/source_identity.py",
    "scripts/qualification/verify_step18_release_gates.py",
}
_HYGIENE_INPUT_PATHS = {
    command.split()[-1]
    for _, command in HYGIENE_CHECKS[:-1]
}
_LANE_INPUT_PATHS = {
    "full_dev_regression": {"scripts/test-all.sh"},
    "full_ros_regression": {
        "scripts/ros/build-ros2-control-runtime.sh",
    },
    "contract_architecture_hygiene": _HYGIENE_INPUT_PATHS,
    "core_asan_ubsan": {
        "scripts/ci/run-core-asan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
    },
    "core_tsan": {
        "scripts/ci/run-core-tsan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
        "scripts/ci/tsan.supp",
    },
    "collision_asan_ubsan": {
        "scripts/ci/run-collision-engine-asan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
    },
    "collision_tsan": {
        "scripts/ci/run-collision-engine-tsan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
        "scripts/ci/tsan.supp",
    },
}

_SHA256 = re.compile(r"[0-9a-f]{64}")
_UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
    r"[89ab][0-9a-f]{3}-[0-9a-f]{12}"
)
_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_SANITIZER_FINDING = re.compile(
    r"(?:"
    r"ERROR: AddressSanitizer|SUMMARY: AddressSanitizer|"
    r"ERROR: LeakSanitizer|SUMMARY: LeakSanitizer|"
    r"WARNING: ThreadSanitizer|SUMMARY: ThreadSanitizer|"
    r"ThreadSanitizer: reported|"
    r"UndefinedBehaviorSanitizer|runtime error:|"
    r"Sanitizer CHECK failed"
    r")",
    flags=re.IGNORECASE,
)


class ValidationError(RuntimeError):
    """A retained release-gate claim is incomplete or inconsistent."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise ValidationError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _sha256_value(value: Any, label: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ValidationError(f"{label} must be a lowercase SHA-256")
    return value


def _exact_keys(value: Any, expected: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must be an object")
    actual = set(value)
    if actual != expected:
        raise ValidationError(
            f"{label} fields differ: missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )
    return value


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} is not readable JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must contain one JSON object")
    return value


def _resolve_file(
    owner_path: Path,
    raw_path: Any,
    expected_hash: Any,
    expected_bytes: Any,
    label: str,
) -> tuple[Path, str, int]:
    if not isinstance(raw_path, str) or not raw_path:
        raise ValidationError(f"{label} path must be a non-empty string")
    candidate = Path(raw_path)
    path = candidate if candidate.is_absolute() else owner_path.parent / candidate
    path = path.resolve()
    if not path.is_file():
        raise ValidationError(f"{label} is not a retained regular file: {path}")
    digest = _sha256(path)
    if digest != _sha256_value(expected_hash, f"{label} hash"):
        raise ValidationError(f"{label} hash changed")
    if (
        not isinstance(expected_bytes, int)
        or isinstance(expected_bytes, bool)
        or expected_bytes < 0
        or path.stat().st_size != expected_bytes
    ):
        raise ValidationError(f"{label} byte count changed")
    return path, digest, expected_bytes


def _identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def _contract_identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_specs_contract_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def lane_input_paths(lane: str) -> tuple[str, ...]:
    if lane not in LANES:
        raise ValidationError(f"unknown release-gate lane: {lane}")
    return tuple(sorted(_BASE_INPUT_PATHS | _LANE_INPUT_PATHS[lane]))


def lane_service(lane: str) -> str:
    try:
        return _LANE_SERVICE[lane]
    except KeyError as error:
        raise ValidationError(f"unknown release-gate lane: {lane}") from error


def lane_profile(lane: str) -> str | None:
    if lane in ROS_FULL_LANES:
        return "ros-full"
    if lane in LANES:
        return None
    raise ValidationError(f"unknown release-gate lane: {lane}")


def lane_security_options(lane: str) -> list[str]:
    # The build-only ``dev`` service carries this option in the repository's
    # canonical Compose contract; sanitizer lanes add the same option to the
    # ROS service through their retained override.
    if lane in SANITIZER_LANES or lane_service(lane) == "dev":
        return ["seccomp=unconfined"]
    if lane in LANES:
        return []
    raise ValidationError(f"unknown release-gate lane: {lane}")


def lane_security_override(lane: str) -> str | None:
    if lane in SANITIZER_LANES:
        return (
            "services:\n"
            "  ros-jazzy-full:\n"
            "    security_opt:\n"
            "      - seccomp=unconfined\n"
        )
    if lane in LANES:
        return None
    raise ValidationError(f"unknown release-gate lane: {lane}")


def lane_build_root(run_id: str, lane: str) -> str:
    if _UUID.fullmatch(run_id) is None or lane not in LANES:
        raise ValidationError("invalid run id or lane for build root")
    return f"/workspace/build/step18-release-gates/{run_id}/{lane}"


def lane_environment(
    lane: str,
    run_id: str,
    source_fingerprint: str,
    specs_source_fingerprint: str,
    specs_contract_fingerprint: str,
) -> dict[str, str]:
    build_root = lane_build_root(run_id, lane)
    environment = {
        "CC": "gcc",
        "CFLAGS": "",
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "CXX": "g++",
        "CXXFLAGS": "",
        "LDFLAGS": "",
        "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256": source_fingerprint,
        "MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256": (
            specs_contract_fingerprint
        ),
        "MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256": (
            specs_source_fingerprint
        ),
        "MANYFORGE_RELEASE_GATE_BUILD_ROOT": build_root,
        "MANYFORGE_RELEASE_GATE_LANE": lane,
        "MANYFORGE_RELEASE_GATE_RUN_ID": run_id,
        "NO_COLOR": "1",
        "PYTHONPATH": (
            "/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace"
        ),
        "PYTHONPYCACHEPREFIX": (
            f"/tmp/manyforge-step18-release-gate-pycache/{run_id}/{lane}"
        ),
        "PYTEST_ADDOPTS": "--color=no",
        "WORKSPACE": "/workspace",
    }
    if lane in ROS_FULL_LANES:
        # Compose otherwise preserves these as bare host-passthrough keys.
        # Formal lanes neutralize them explicitly so ambient DDS discovery or
        # profile settings cannot alter the isolated test process.
        environment.update(
            {key: "" for key in ROS_AMBIENT_PASSTHROUGH_VARIABLES}
        )
    if lane == "full_dev_regression":
        environment.update(
            {
                "MANYFORGE_BUILD_DIR": build_root,
                "MANYFORGE_TEST_STAMP": f"{build_root}/test-all.stamp",
            }
        )
    elif lane == "full_ros_regression":
        environment.update(
            {
                "MANYFORGE_ROBOPLAN_BUILD_DIR": f"{build_root}/roboplan",
                "MANYFORGE_TEST_ROS_DOMAIN_ID": "119",
                "ROS_DOMAIN_ID": "119",
            }
        )
    elif lane == "contract_architecture_hygiene":
        environment["INSIDE_MANYFORGE_DEV_CONTAINER"] = "1"
    elif lane in SANITIZER_LANES:
        environment["BUILD_DIR"] = build_root
        if lane.endswith("asan_ubsan"):
            environment.update(
                {
                    "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1:exitcode=66",
                    "UBSAN_OPTIONS": (
                        "halt_on_error=1:print_stacktrace=1:exitcode=66"
                    ),
                }
            )
        else:
            environment.update(
                {
                    "TSAN_OPTIONS": (
                        "halt_on_error=1 second_deadlock_stack=1 history_size=7 "
                        "exitcode=66 suppressions=/workspace/scripts/ci/tsan.supp"
                    ),
                    "TSAN_SUPPRESSIONS": "/workspace/scripts/ci/tsan.supp",
                }
            )
    return dict(sorted(environment.items()))


def lane_labels(
    lane: str,
    run_id: str,
    source_fingerprint: str,
    specs_source_fingerprint: str,
    specs_contract_fingerprint: str,
    inner_script_sha256: str,
) -> dict[str, str]:
    return dict(
        sorted(
            {
                "manyforge.step18.release-gate.schema": LANE_SCHEMA,
                "manyforge.step18.release-gate.lane": lane,
                "manyforge.step18.release-gate.run-id": run_id,
                "manyforge.step18.release-gate.source": source_fingerprint,
                "manyforge.step18.release-gate.specs-source": (
                    specs_source_fingerprint
                ),
                "manyforge.step18.release-gate.specs-contract": (
                    specs_contract_fingerprint
                ),
                "manyforge.step18.release-gate.inner-script": inner_script_sha256,
            }.items()
        )
    )


def _identity_preamble() -> str:
    return r'''python3 /workspace/scripts/qualification/source_identity.py \
  --repo-root /workspace \
  --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256}" \
  >/dev/null
python3 /workspace/scripts/qualification/source_identity.py \
  --repo-root /step18-specs \
  --include-specs-contract-identity \
  --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
  --expect-contract-fingerprint-sha256 "${MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}" \
  >/dev/null'''


def _gcc13_probe() -> str:
    return r'''step18_gxx_version="$(g++ -dumpfullversion -dumpversion)"
printf 'STEP18_GXX_VERSION=%s\n' "${step18_gxx_version}"
[[ "${step18_gxx_version}" == 13.* ]]'''


def _ros_setup() -> str:
    return r'''set +u
source /opt/ros/jazzy/setup.bash
source /opt/manyforge/topic_based_ros2_control/install/setup.bash
source /opt/manyforge/venv/bin/activate
set -u'''


def _lane_body(lane: str) -> str:
    if lane == "full_dev_regression":
        return "./scripts/test-all.sh --force all"
    if lane == "full_ros_regression":
        return rf'''{_gcc13_probe()}
{_ros_setup()}
ros_build="${{MANYFORGE_RELEASE_GATE_BUILD_ROOT}}/ros2_control_execution"
cmake -S /workspace/manyforge_ros2_control_execution -B "${{ros_build}}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-Werror \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS=300
cmake --build "${{ros_build}}" --parallel 2
printf 'STEP18_ROS_CTEST_BEGIN\n'
ctest --test-dir "${{ros_build}}" --output-on-failure --timeout 300 --no-tests=error
printf 'STEP18_ROS_CTEST_END status=0\n'
MANYFORGE_ROBOPLAN_BUILD_DIR="${{MANYFORGE_ROBOPLAN_BUILD_DIR}}" \
  /workspace/scripts/ros/build-ros2-control-runtime.sh
export PYTHONPATH="${{MANYFORGE_ROBOPLAN_BUILD_DIR}}:/workspace/manyforge_roboplan/python:\
/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${{PYTHONPATH:-}}"
printf 'STEP18_ROS_PYTEST_BEGIN\n'
pytest /workspace/manyforge_ros/tests -q --color=no
printf 'STEP18_ROS_PYTEST_END status=0\n' '''.rstrip()
    if lane == "contract_architecture_hygiene":
        commands: list[str] = []
        for check_id, command in HYGIENE_CHECKS:
            commands.extend(
                [
                    f"printf 'STEP18_HYGIENE_CHECK_BEGIN id={check_id}\\n'",
                    command,
                    f"printf 'STEP18_HYGIENE_CHECK_END id={check_id} status=0\\n'",
                ]
            )
        return "\n".join(commands)
    if lane in SANITIZER_LANES:
        return f"{_gcc13_probe()}\nbash /workspace/{_SANITIZER_SCRIPT[lane]}"
    raise ValidationError(f"unknown release-gate lane: {lane}")


def _ros_ambient_environment_preamble() -> str:
    ros_keys = " ".join(ROS_AMBIENT_PASSTHROUGH_VARIABLES)
    return f'''builtin unset -v -- {ros_keys}
for step18_ros_ambient_key in {ros_keys}; do
  if [[ -v "${{step18_ros_ambient_key}}" ]]; then
    builtin printf 'Step 18 ROS ambient environment neutralization failed: %s\\n' \
      "${{step18_ros_ambient_key}}" >&2
    exit 2
  fi
done
builtin unset -v -- step18_ros_ambient_key'''


def lane_inner_script(lane: str) -> str:
    if lane not in LANES:
        raise ValidationError(f"unknown release-gate lane: {lane}")
    preamble = _identity_preamble()
    body = _lane_body(lane)
    ros_environment_preamble = ""
    ros_environment_marker = ""
    if lane in ROS_FULL_LANES:
        ros_environment_preamble = _ros_ambient_environment_preamble()
        ros_environment_marker = (
            "builtin printf 'STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4\\n'"
        )
    return f'''set -euo pipefail
cd /workspace
{ros_environment_preamble}
{preamble}
printf 'STEP18_RELEASE_GATE_BEGIN lane={lane}\\n'
{ros_environment_marker}
{body}
printf 'STEP18_RELEASE_GATE_END lane={lane} status=0\\n'
{preamble}'''


def expected_outer_command(
    *,
    repo_root: Path,
    specs_root: Path,
    lane: str,
    container_name: str,
    environment: dict[str, str],
    labels: dict[str, str],
    security_override_path: Path | None = None,
) -> list[str]:
    command = [
        "docker",
        "compose",
        "-f",
        str((repo_root / "docker-compose.yaml").resolve()),
    ]
    override = lane_security_override(lane)
    if override is not None:
        if security_override_path is None:
            raise ValidationError(f"{lane} requires a Compose security override")
        command.extend(["-f", str(security_override_path.resolve())])
    elif security_override_path is not None:
        raise ValidationError(f"{lane} must not use a Compose security override")
    profile = lane_profile(lane)
    if profile is not None:
        command.extend(["--profile", profile])
    command.extend(
        [
            "run",
            "--detach",
            "--no-deps",
            "-T",
            "--name",
            container_name,
            "--entrypoint",
            "/bin/bash",
        ]
    )
    command.extend(["-v", f"{specs_root.resolve()}:/step18-specs:ro"])
    for key, value in sorted(labels.items()):
        command.extend(["--label", f"{key}={value}"])
    for key, value in sorted(environment.items()):
        command.extend(["-e", f"{key}={value}"])
    command.extend([lane_service(lane), "-lc", lane_inner_script(lane)])
    return command


def normalize_mounts(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise ValidationError("container inspect mounts must be a list")
    mounts: list[dict[str, Any]] = []
    for index, raw in enumerate(value):
        if not isinstance(raw, dict):
            raise ValidationError(f"container mount {index} is not an object")
        destination = raw.get("Destination")
        source = raw.get("Source")
        mount_type = raw.get("Type")
        if not all(isinstance(item, str) and item for item in (destination, source, mount_type)):
            raise ValidationError(f"container mount {index} lacks type/source/destination")
        rw = raw.get("RW")
        if not isinstance(rw, bool):
            raise ValidationError(f"container mount {index} lacks boolean RW")
        mounts.append(
            {
                "type": mount_type,
                "source": source,
                "destination": destination,
                "mode": str(raw.get("Mode", "")),
                "rw": rw,
                "propagation": str(raw.get("Propagation", "")),
                "name": str(raw.get("Name", "")),
                "driver": str(raw.get("Driver", "")),
            }
        )
    destinations = [mount["destination"] for mount in mounts]
    if len(set(destinations)) != len(destinations):
        raise ValidationError("container inspect has duplicate mount destinations")
    return sorted(mounts, key=lambda item: item["destination"])


def _clean_log(raw: str) -> str:
    return _ANSI.sub("", raw).replace("\r", "")


def _one_marked_section(log: str, begin: str, end: str, label: str) -> str:
    begin_matches = list(
        re.finditer(rf"^{re.escape(begin)}$", log, flags=re.MULTILINE)
    )
    end_matches = list(re.finditer(rf"^{re.escape(end)}$", log, flags=re.MULTILINE))
    if len(begin_matches) != 1 or len(end_matches) != 1:
        raise ValidationError(f"{label} has missing or ambiguous boundary markers")
    start = begin_matches[0].end()
    finish = end_matches[0].start()
    if finish <= start:
        raise ValidationError(f"{label} section is empty")
    return log[start:finish]


def _verify_lane_log(lane: str, log_path: Path) -> dict[str, Any]:
    if log_path.stat().st_size <= 0:
        raise ValidationError(f"{lane} container log is empty")
    text = _clean_log(log_path.read_text(encoding="utf-8", errors="replace"))
    begin = f"STEP18_RELEASE_GATE_BEGIN lane={lane}"
    end = f"STEP18_RELEASE_GATE_END lane={lane} status=0"
    section = _one_marked_section(text, begin, end, f"{lane} release gate")

    gcc_version: str | None = None
    neutralization_pattern = r"^STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4$"
    log_neutralization_matches = re.findall(
        neutralization_pattern,
        text,
        flags=re.MULTILINE,
    )
    section_neutralization_matches = re.findall(
        neutralization_pattern,
        section,
        flags=re.MULTILINE,
    )
    if lane in ROS_FULL_LANES:
        if (
            len(log_neutralization_matches) != 1
            or len(section_neutralization_matches) != 1
        ):
            raise ValidationError(
                f"{lane} does not prove exact ROS ambient environment neutralization"
            )
    elif log_neutralization_matches or section_neutralization_matches:
        raise ValidationError(f"{lane} unexpectedly claims ROS environment neutralization")
    gcc_matches = re.findall(r"^STEP18_GXX_VERSION=([^\s]+)$", section, re.MULTILINE)
    if lane in ROS_FULL_LANES:
        if len(gcc_matches) != 1 or not gcc_matches[0].startswith("13."):
            raise ValidationError(f"{lane} does not prove the GCC 13 ros-full lane")
        gcc_version = gcc_matches[0]
    elif gcc_matches:
        raise ValidationError(f"{lane} unexpectedly claims a GCC probe")

    summary: dict[str, Any]
    if lane == "full_dev_regression":
        if re.search(r"^\s*SKIP:", section, flags=re.MULTILINE):
            raise ValidationError("full_dev_regression skipped a capability lane")
        if "already passed" in section or "skipping re-test" in section:
            raise ValidationError(
                "full_dev_regression used the re-test shortcut despite --force"
            )
        matches = re.findall(
            r"^All ([0-9]+) test suite\(s\) passed\.$",
            section,
            flags=re.MULTILINE,
        )
        if matches != ["11"]:
            raise ValidationError(
                "full_dev_regression lacks one exact 11-suite passing summary"
            )
        summary = {"suite_count": 11, "capability_skips": 0}
    elif lane == "full_ros_regression":
        ctest = _one_marked_section(
            section,
            "STEP18_ROS_CTEST_BEGIN",
            "STEP18_ROS_CTEST_END status=0",
            "full_ros_regression CTest",
        )
        ctest_matches = re.findall(
            r"^100% tests passed, 0 tests failed out of ([1-9][0-9]*)$",
            ctest,
            flags=re.MULTILINE,
        )
        if len(ctest_matches) != 1 or re.search(
            r"No tests were found|\bNot Run\b", ctest
        ):
            raise ValidationError(
                "full_ros_regression CTest summary is missing or ambiguous"
            )
        pytest = _one_marked_section(
            section,
            "STEP18_ROS_PYTEST_BEGIN",
            "STEP18_ROS_PYTEST_END status=0",
            "full_ros_regression pytest",
        )
        pytest_matches = re.findall(
            r"^(?:=+ )?([1-9][0-9]*) passed in [^\n]+?(?: =+)?$",
            pytest,
            flags=re.MULTILINE,
        )
        if len(pytest_matches) != 1 or re.search(
            r"[0-9]+ (?:failed|skipped|errors?|xfailed|xpassed)\b",
            pytest,
            flags=re.IGNORECASE,
        ):
            raise ValidationError(
                "full_ros_regression pytest did not prove one non-skipped passing suite"
            )
        summary = {
            "ctest_tests": int(ctest_matches[0]),
            "pytest_tests": int(pytest_matches[0]),
            "pytest_skipped": 0,
            "gcc_version": gcc_version,
        }
    elif lane == "contract_architecture_hygiene":
        completed: list[str] = []
        for check_id, _ in HYGIENE_CHECKS:
            _one_marked_section(
                section,
                f"STEP18_HYGIENE_CHECK_BEGIN id={check_id}",
                f"STEP18_HYGIENE_CHECK_END id={check_id} status=0",
                f"hygiene check {check_id}",
            )
            completed.append(check_id)
        if section.count("STEP18_HYGIENE_CHECK_BEGIN") != len(HYGIENE_CHECKS):
            raise ValidationError("hygiene lane contains duplicate or unknown checks")
        if section.count("STEP18_HYGIENE_CHECK_END") != len(HYGIENE_CHECKS):
            raise ValidationError("hygiene lane contains ambiguous completion markers")
        if section.count("STEP18_SPECIFIC_HYGIENE passed") != 1:
            raise ValidationError("Step 18-specific hygiene result is ambiguous")
        summary = {"checks": completed, "check_count": len(completed)}
    elif lane in SANITIZER_LANES:
        if _SANITIZER_FINDING.search(section):
            raise ValidationError(f"{lane} log contains a sanitizer signature")
        prefix = _SANITIZER_PREFIX[lane]
        pattern = re.compile(
            rf"^{prefix}_GROUP_SUMMARY binaries=([0-9]+) "
            r"registered_tests=([0-9]+) enabled_tests=([0-9]+) "
            r"disabled_tests=([0-9]+) ran=([0-9]+) passed=([0-9]+) "
            r"skipped=([0-9]+) fallback_binaries=([0-9]+)$",
            flags=re.MULTILINE,
        )
        matches = pattern.findall(section)
        if len(matches) != 1:
            raise ValidationError(f"{lane} grouped test summary is missing or ambiguous")
        (
            binaries,
            registered,
            enabled,
            disabled,
            ran,
            passed,
            skipped,
            fallback,
        ) = (int(item) for item in matches[0])
        if (
            binaries <= 0
            or registered <= 0
            or enabled <= 0
            or ran != enabled
            or passed != enabled
            or skipped != 0
            or fallback != 0
        ):
            raise ValidationError(
                f"{lane} grouped lane used fallback/skip or did not run every enabled test"
            )
        summary = {
            "binaries": binaries,
            "registered_tests": registered,
            "enabled_tests": enabled,
            "disabled_tests": disabled,
            "ran": ran,
            "passed": passed,
            "skipped": skipped,
            "fallback_binaries": fallback,
            "gcc_version": gcc_version,
        }
    else:  # pragma: no cover - guarded by caller
        raise ValidationError(f"unknown release-gate lane: {lane}")
    return summary


def _verify_container(
    *,
    inspect_path: Path,
    repo_root: Path,
    specs_root: Path,
    lane: str,
    execution: dict[str, Any],
    security_override_path: Path | None,
) -> dict[str, Any]:
    try:
        value = json.loads(inspect_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"{lane} container inspect is invalid: {error}") from error
    if not isinstance(value, list) or len(value) != 1 or not isinstance(value[0], dict):
        raise ValidationError(f"{lane} inspect must contain exactly one container")
    record = value[0]
    state = _exact_keys(
        execution["terminal_status"],
        {"docker_wait_status", "exit_code", "oom_killed", "error"},
        f"{lane} terminal status",
    )
    inspect_state = record.get("State")
    if not isinstance(inspect_state, dict):
        raise ValidationError(f"{lane} inspect omits container state")
    observed_status = {
        "docker_wait_status": execution["terminal_status"]["docker_wait_status"],
        "exit_code": inspect_state.get("ExitCode"),
        "oom_killed": inspect_state.get("OOMKilled"),
        "error": inspect_state.get("Error"),
    }
    if observed_status != state:
        raise ValidationError(f"{lane} terminal status differs from inspect")
    if (
        state["docker_wait_status"] != 0
        or state["exit_code"] != 0
        or state["oom_killed"] is not False
        or state["error"] not in ("", None)
    ):
        raise ValidationError(f"{lane} container did not exit successfully")

    container_id = execution.get("container_id")
    container_name = execution.get("container_name")
    declared_image_id = str(execution.get("declared_image_id", "")).removeprefix(
        "sha256:"
    )
    image_id = str(execution.get("image_id", "")).removeprefix("sha256:")
    if (
        not isinstance(container_id, str)
        or _SHA256.fullmatch(container_id) is None
        or not isinstance(container_name, str)
        or not container_name
        or _SHA256.fullmatch(declared_image_id) is None
        or _SHA256.fullmatch(image_id) is None
    ):
        raise ValidationError(f"{lane} container identity fields are invalid")
    if (
        record.get("Id") != container_id
        or str(record.get("Name", "")).removeprefix("/") != container_name
        or str(record.get("Image", "")).removeprefix("sha256:") != image_id
        or declared_image_id != image_id
    ):
        raise ValidationError(f"{lane} container id/name/image differs from inspect")

    config = record.get("Config")
    host_config = record.get("HostConfig")
    if not isinstance(config, dict) or not isinstance(host_config, dict):
        raise ValidationError(f"{lane} inspect omits Config or HostConfig")
    if config.get("Image") != execution.get("image_reference"):
        raise ValidationError(f"{lane} configured image reference differs")
    configured_process = [record.get("Path"), *record.get("Args", [])]
    expected_process = ["/bin/bash", "-lc", lane_inner_script(lane)]
    if (
        configured_process != execution.get("configured_process_argv")
        or configured_process != expected_process
    ):
        raise ValidationError(f"{lane} configured process argv differs")
    requested_argv = execution.get("requested_container_argv")
    expected_requested = expected_process
    if requested_argv != expected_requested:
        raise ValidationError(f"{lane} requested container argv is not canonical")
    if config.get("Entrypoint") != ["/bin/bash"]:
        raise ValidationError(f"{lane} configured entrypoint is not canonical")
    if config.get("Cmd") != ["-lc", lane_inner_script(lane)]:
        raise ValidationError(f"{lane} configured command is not canonical")

    configured_environment = config.get("Env")
    if (
        not isinstance(configured_environment, list)
        or not all(isinstance(item, str) and "=" in item for item in configured_environment)
        or configured_environment != execution.get("configured_environment")
    ):
        raise ValidationError(f"{lane} configured environment is not exact")
    required_environment = execution.get("required_environment")
    if not isinstance(required_environment, dict):
        raise ValidationError(f"{lane} required environment is not an object")
    observed_environment: dict[str, list[str]] = {}
    for item in configured_environment:
        key, value = item.split("=", 1)
        observed_environment.setdefault(key, []).append(value)
    for key, expected in required_environment.items():
        if observed_environment.get(key) != [expected]:
            raise ValidationError(f"{lane} environment {key} differs or is duplicated")

    labels = config.get("Labels")
    required_labels = execution.get("required_labels")
    if not isinstance(labels, dict) or not isinstance(required_labels, dict):
        raise ValidationError(f"{lane} container labels are unavailable")
    for key, expected in required_labels.items():
        if labels.get(key) != expected:
            raise ValidationError(f"{lane} container label {key} differs")

    security = host_config.get("SecurityOpt") or []
    if security != execution.get("security_options"):
        raise ValidationError(f"{lane} security options differ from inspect")
    if security != lane_security_options(lane):
        raise ValidationError(f"{lane} security profile is not canonical")

    mounts = normalize_mounts(record.get("Mounts"))
    if mounts != execution.get("mounts"):
        raise ValidationError(f"{lane} exact mount inventory differs from inspect")
    workspace = [item for item in mounts if item["destination"] == "/workspace"]
    specs = [item for item in mounts if item["destination"] == "/step18-specs"]
    if (
        len(workspace) != 1
        or workspace[0]["type"] != "bind"
        or Path(workspace[0]["source"]).resolve() != repo_root
        or workspace[0]["rw"] is not True
        or len(specs) != 1
        or specs[0]["type"] != "bind"
        or Path(specs[0]["source"]).resolve() != specs_root
        or specs[0]["rw"] is not False
    ):
        raise ValidationError(f"{lane} source or Specs mount is not exact")

    expected_outer = expected_outer_command(
        repo_root=repo_root,
        specs_root=specs_root,
        lane=lane,
        container_name=container_name,
        environment=required_environment,
        labels=required_labels,
        security_override_path=security_override_path,
    )
    if execution.get("outer_command_argv") != expected_outer:
        raise ValidationError(f"{lane} outer Docker command is not canonical")
    return {
        "id": container_id,
        "name": container_name,
        "service": lane_service(lane),
        "image_reference": execution["image_reference"],
        "image_id": image_id,
        "security_options": security,
        "mounts": mounts,
    }


def _verify_lane_manifest(
    *,
    manifest_path: Path,
    candidate_source: dict[str, Any],
    candidate_specs_source: dict[str, Any],
    candidate_specs_contract: dict[str, Any],
    repo_root: Path,
    specs_root: Path,
) -> dict[str, Any]:
    manifest = _load_json(manifest_path, "release-gate lane manifest")
    _exact_keys(
        manifest,
        {
            "schema_version",
            "verification_scope",
            "lane",
            "run_id",
            "paths",
            "identities",
            "execution",
            "artifacts",
            "script_hashes",
            "overall_step18_closure_claimed",
            "requires_common_candidate_aggregator",
        },
        "release-gate lane manifest",
    )
    if manifest["schema_version"] != LANE_SCHEMA:
        raise ValidationError(f"lane manifest schema must be {LANE_SCHEMA}")
    if manifest["verification_scope"] != VERIFICATION_SCOPE:
        raise ValidationError("lane manifest verification scope differs")
    lane = manifest.get("lane")
    if lane not in LANES:
        raise ValidationError(f"lane manifest has unknown lane: {lane}")
    run_id = manifest.get("run_id")
    if not isinstance(run_id, str) or _UUID.fullmatch(run_id) is None:
        raise ValidationError(f"{lane} run id is not a UUIDv4")
    if (
        manifest["overall_step18_closure_claimed"] is not False
        or manifest["requires_common_candidate_aggregator"] is not True
    ):
        raise ValidationError(f"{lane} improperly claims overall Step 18 closure")

    paths = _exact_keys(
        manifest["paths"],
        {"manyforge_root", "specs_root"},
        f"{lane} paths",
    )
    if (
        Path(paths["manyforge_root"]).resolve() != repo_root
        or Path(paths["specs_root"]).resolve() != specs_root
    ):
        raise ValidationError(f"{lane} source roots differ from the campaign")

    identities = _exact_keys(
        manifest["identities"],
        {
            "manyforge_before",
            "manyforge_after",
            "specs_before",
            "specs_after",
            "specs_contract_before",
            "specs_contract_after",
        },
        f"{lane} identities",
    )
    manyforge_before = _identity(identities["manyforge_before"], f"{lane} ManyForge before")
    manyforge_after = _identity(identities["manyforge_after"], f"{lane} ManyForge after")
    specs_before = _identity(identities["specs_before"], f"{lane} Specs before")
    specs_after = _identity(identities["specs_after"], f"{lane} Specs after")
    contract_before = _contract_identity(
        identities["specs_contract_before"], f"{lane} Specs contract before"
    )
    contract_after = _contract_identity(
        identities["specs_contract_after"], f"{lane} Specs contract after"
    )
    if manyforge_before != manyforge_after:
        raise ValidationError(f"{lane} ManyForge source changed during execution")
    if specs_before != specs_after:
        raise ValidationError(f"{lane} Specs source changed during execution")
    if contract_before != contract_after:
        raise ValidationError(f"{lane} Specs contract changed during execution")
    if manyforge_after != candidate_source:
        raise ValidationError(f"{lane} uses a mixed ManyForge source")
    if specs_after != candidate_specs_source:
        raise ValidationError(f"{lane} uses a mixed Specs source")
    if contract_after != candidate_specs_contract:
        raise ValidationError(f"{lane} uses a mixed Specs contract")

    artifacts = _exact_keys(
        manifest["artifacts"],
        {"container_inspect", "container_log"},
        f"{lane} artifacts",
    )
    resolved_artifacts: dict[str, tuple[Path, str, int]] = {}
    for name in ("container_inspect", "container_log"):
        record = _exact_keys(
            artifacts[name], {"path", "sha256", "bytes"}, f"{lane} {name}"
        )
        resolved_artifacts[name] = _resolve_file(
            manifest_path,
            record["path"],
            record["sha256"],
            record["bytes"],
            f"{lane} {name}",
        )
    inspect_path, inspect_hash, _ = resolved_artifacts["container_inspect"]
    log_path, log_hash, log_bytes = resolved_artifacts["container_log"]
    if inspect_path == log_path:
        raise ValidationError(f"{lane} inspect and log artifacts alias")

    script_hash_bundle = _exact_keys(
        manifest.get("script_hashes"),
        {"before", "after"},
        f"{lane} script hash bundle",
    )
    expected_inputs = lane_input_paths(lane)
    for phase in ("before", "after"):
        if (
            not isinstance(script_hash_bundle[phase], dict)
            or set(script_hash_bundle[phase]) != set(expected_inputs)
        ):
            raise ValidationError(
                f"{lane} {phase}-run script hash inventory is not exact"
            )
    if script_hash_bundle["before"] != script_hash_bundle["after"]:
        raise ValidationError(f"{lane} qualification inputs changed during execution")
    for relative in expected_inputs:
        path = (repo_root / relative).resolve()
        try:
            path.relative_to(repo_root)
        except ValueError as error:
            raise ValidationError(f"{lane} script path escapes source root") from error
        if not path.is_file():
            raise ValidationError(f"{lane} qualification input is missing: {relative}")
        if _sha256(path) != _sha256_value(
            script_hash_bundle["after"][relative], relative
        ):
            raise ValidationError(f"{lane} qualification input hash changed: {relative}")

    execution = _exact_keys(
        manifest["execution"],
        {
            "compose_service",
            "compose_profile",
            "container_id",
            "container_name",
            "image_reference",
            "declared_image_id",
            "image_id",
            "outer_command_argv",
            "requested_container_argv",
            "configured_process_argv",
            "required_environment",
            "configured_environment",
            "required_labels",
            "security_override",
            "security_options",
            "mounts",
            "terminal_status",
        },
        f"{lane} execution",
    )
    if execution["compose_service"] != lane_service(lane):
        raise ValidationError(f"{lane} compose service differs")
    if execution["compose_profile"] != lane_profile(lane):
        raise ValidationError(f"{lane} compose profile differs")
    expected_environment = lane_environment(
        lane,
        run_id,
        candidate_source["fingerprint_sha256"],
        candidate_specs_source["fingerprint_sha256"],
        candidate_specs_contract["contract_fingerprint_sha256"],
    )
    if execution["required_environment"] != expected_environment:
        raise ValidationError(f"{lane} required environment differs from the frozen profile")
    expected_inner = lane_inner_script(lane)
    expected_labels = lane_labels(
        lane,
        run_id,
        candidate_source["fingerprint_sha256"],
        candidate_specs_source["fingerprint_sha256"],
        candidate_specs_contract["contract_fingerprint_sha256"],
        _sha256_text(expected_inner),
    )
    if execution["required_labels"] != expected_labels:
        raise ValidationError(f"{lane} source-binding labels differ")

    security_override_path: Path | None = None
    security_override_binding: dict[str, Any] | None = None
    expected_override = lane_security_override(lane)
    override_record = execution["security_override"]
    if expected_override is None:
        if override_record is not None:
            raise ValidationError(f"{lane} unexpectedly retains a security override")
    else:
        override = _exact_keys(
            override_record,
            {"path", "sha256", "bytes"},
            f"{lane} security override",
        )
        security_override_path, override_digest, override_bytes = _resolve_file(
            manifest_path,
            override["path"],
            override["sha256"],
            override["bytes"],
            f"{lane} security override",
        )
        try:
            observed_override = security_override_path.read_text(encoding="utf-8")
        except OSError as error:
            raise ValidationError(f"cannot read {lane} security override: {error}") from error
        if observed_override != expected_override:
            raise ValidationError(f"{lane} security override content differs")
        security_override_binding = {
            "path": str(security_override_path),
            "sha256": override_digest,
            "bytes": override_bytes,
        }
    container = _verify_container(
        inspect_path=inspect_path,
        repo_root=repo_root,
        specs_root=specs_root,
        lane=lane,
        execution=execution,
        security_override_path=security_override_path,
    )
    test_summary = _verify_lane_log(lane, log_path)
    return {
        "name": lane,
        "run_id": run_id,
        "lane_manifest": {"path": str(manifest_path), "sha256": _sha256(manifest_path)},
        "container": container,
        "container_inspect": {"path": str(inspect_path), "sha256": inspect_hash},
        "container_log": {"path": str(log_path), "sha256": log_hash, "bytes": log_bytes},
        "security_override": security_override_binding,
        "test_summary": test_summary,
        "passed": True,
    }


def verify_artifact_manifest(manifest_path: Path) -> dict[str, Any]:
    manifest_path = manifest_path.resolve()
    manifest = _load_json(manifest_path, "release-gate artifact manifest")
    _exact_keys(
        manifest,
        {
            "schema_version",
            "verification_scope",
            "candidate",
            "lanes",
            "overall_step18_closure_claimed",
            "requires_common_candidate_aggregator",
        },
        "release-gate artifact manifest",
    )
    if manifest["schema_version"] != ARTIFACT_MANIFEST_SCHEMA:
        raise ValidationError(
            f"artifact manifest schema must be {ARTIFACT_MANIFEST_SCHEMA}"
        )
    if manifest["verification_scope"] != VERIFICATION_SCOPE:
        raise ValidationError("artifact manifest verification scope differs")
    if (
        manifest["overall_step18_closure_claimed"] is not False
        or manifest["requires_common_candidate_aggregator"] is not True
    ):
        raise ValidationError("release-gate artifact manifest claims overall closure")

    candidate = _exact_keys(
        manifest["candidate"],
        {
            "manyforge_root",
            "specs_root",
            "source_identity",
            "specs_source_identity",
            "specs_contract_identity",
        },
        "release-gate candidate",
    )
    repo_root = Path(candidate["manyforge_root"]).resolve()
    specs_root = Path(candidate["specs_root"]).resolve()
    if not repo_root.is_dir() or not specs_root.is_dir():
        raise ValidationError("release-gate candidate roots are unavailable")
    candidate_source = _identity(candidate["source_identity"], "candidate source")
    candidate_specs_source = _identity(
        candidate["specs_source_identity"], "candidate Specs source"
    )
    candidate_specs_contract = _contract_identity(
        candidate["specs_contract_identity"], "candidate Specs contract"
    )
    if (
        candidate_specs_source["head_commit"]
        != candidate_specs_contract["head_commit"]
        or candidate_specs_source["head_tree"]
        != candidate_specs_contract["head_tree"]
    ):
        raise ValidationError(
            "candidate Specs source and contract do not share one HEAD/tree"
        )
    try:
        current_source = source_identity(repo_root)
        current_specs_source = source_identity(specs_root)
        current_specs_contract = specs_contract_identity(specs_root)
    except SourceIdentityError as error:
        raise ValidationError(f"cannot collect current candidate identity: {error}") from error
    if current_source != candidate_source:
        raise ValidationError("current ManyForge source differs from the release-gate candidate")
    if (
        current_specs_source["head_commit"] != candidate_specs_source["head_commit"]
        or current_specs_source["head_tree"] != candidate_specs_source["head_tree"]
    ):
        raise ValidationError(
            "current Specs HEAD/tree differs from the release-gate candidate"
        )
    if current_specs_contract != candidate_specs_contract:
        raise ValidationError("current Specs contract differs from the release-gate candidate")

    references = manifest.get("lanes")
    if not isinstance(references, list) or len(references) != len(LANES):
        raise ValidationError("artifact manifest must contain exactly seven lane references")
    lane_names: list[str] = []
    manifest_paths: list[Path] = []
    verified: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(references):
        reference = _exact_keys(
            raw,
            {"lane", "manifest_path", "manifest_sha256", "manifest_bytes"},
            f"lane reference {index}",
        )
        lane = reference["lane"]
        if lane not in LANES:
            raise ValidationError(f"lane reference {index} has unknown lane: {lane}")
        lane_names.append(lane)
        lane_path, _, _ = _resolve_file(
            manifest_path,
            reference["manifest_path"],
            reference["manifest_sha256"],
            reference["manifest_bytes"],
            f"{lane} lane manifest",
        )
        manifest_paths.append(lane_path)
        result = _verify_lane_manifest(
            manifest_path=lane_path,
            candidate_source=candidate_source,
            candidate_specs_source=candidate_specs_source,
            candidate_specs_contract=candidate_specs_contract,
            repo_root=repo_root,
            specs_root=specs_root,
        )
        if result["name"] != lane:
            raise ValidationError(f"lane reference {lane} points to {result['name']}")
        verified[lane] = result
    if len(set(lane_names)) != len(LANES) or set(lane_names) != set(LANES):
        raise ValidationError("release-gate lanes are missing or duplicated")
    if len(set(manifest_paths)) != len(LANES):
        raise ValidationError("release-gate lane references alias the same manifest")

    run_ids = [verified[lane]["run_id"] for lane in LANES]
    container_ids = [verified[lane]["container"]["id"] for lane in LANES]
    container_names = [verified[lane]["container"]["name"] for lane in LANES]
    log_paths = [Path(verified[lane]["container_log"]["path"]) for lane in LANES]
    if len(set(run_ids)) != len(LANES):
        raise ValidationError("release-gate lanes reuse a run id")
    if len(set(container_ids)) != len(LANES) or len(set(container_names)) != len(LANES):
        raise ValidationError("release-gate lanes reuse a container identity")
    if len(set(log_paths)) != len(LANES):
        raise ValidationError("release-gate lanes reuse a log artifact")

    dev_images = {
        verified[lane]["container"]["image_id"]
        for lane in ("full_dev_regression", "contract_architecture_hygiene")
    }
    ros_images = {
        verified[lane]["container"]["image_id"] for lane in ROS_FULL_LANES
    }
    if len(dev_images) != 1:
        raise ValidationError("dev regression and hygiene used different images")
    if len(ros_images) != 1:
        raise ValidationError("ROS and sanitizer lanes used different ros-full images")

    verifier_path = (
        repo_root / "scripts/qualification/verify_step18_release_gates.py"
    ).resolve()
    runner_path = (
        repo_root / "scripts/qualification/run_step18_release_gates.py"
    ).resolve()
    retained_artifact_manifest: list[dict[str, Any]] = [
        {
            "role": "release_artifact_manifest",
            "path": str(manifest_path),
            "sha256": _sha256(manifest_path),
        },
        {
            "role": "release_verifier",
            "path": str(verifier_path),
            "sha256": _sha256(verifier_path),
        },
        {
            "role": "release_runner",
            "path": str(runner_path),
            "sha256": _sha256(runner_path),
        },
    ]
    for lane in LANES:
        result = verified[lane]
        for field, role in (
            ("lane_manifest", "release_lane_manifest"),
            ("container_inspect", "release_container_inspect"),
            ("container_log", "release_container_log"),
        ):
            retained_artifact_manifest.append(
                {"role": role, "lane": lane, **result[field]}
            )
        if result["security_override"] is not None:
            retained_artifact_manifest.append(
                {
                    "role": "release_security_override",
                    "lane": lane,
                    **result["security_override"],
                }
            )
    _assert_retained_artifact_manifest_current(retained_artifact_manifest)

    return {
        "schema_version": SUMMARY_SCHEMA,
        "verification_scope": VERIFICATION_SCOPE,
        "candidate_source_identity": candidate_source,
        "candidate_specs_source_identity": candidate_specs_source,
        "candidate_specs_contract_identity": candidate_specs_contract,
        "lanes": [verified[lane] for lane in LANES],
        "artifact_manifest": {
            "path": str(manifest_path),
            "sha256": _sha256(manifest_path),
        },
        "retained_artifact_manifest": retained_artifact_manifest,
        "runner": {
            "path": str(runner_path),
            "sha256": _sha256(runner_path),
        },
        "verifier": {
            "path": str(verifier_path),
            "sha256": _sha256(verifier_path),
        },
        "failure_count": 0,
        "release_gates_passed": True,
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
    }


def _assert_retained_artifact_manifest_current(value: Any) -> None:
    if not isinstance(value, list) or len(value) != 28:
        raise ValidationError(
            "release retained artifact manifest must contain exactly 28 files"
        )
    paths: list[Path] = []
    roles: list[str] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict) or set(item) not in (
            {"role", "path", "sha256"},
            {"role", "lane", "path", "sha256"},
            {"role", "lane", "path", "sha256", "bytes"},
        ):
            raise ValidationError(
                f"release retained artifact {index} has unexpected fields"
            )
        role = item.get("role")
        if not isinstance(role, str) or not role:
            raise ValidationError(f"release retained artifact {index} has no role")
        path = Path(str(item.get("path", ""))).resolve()
        if not path.is_file():
            raise ValidationError(f"release retained artifact disappeared: {path}")
        if _sha256(path) != _sha256_value(
            item.get("sha256"), f"release retained artifact {index}"
        ):
            raise ValidationError(f"release retained artifact changed: {path}")
        if "bytes" in item and (
            not isinstance(item["bytes"], int)
            or isinstance(item["bytes"], bool)
            or item["bytes"] <= 0
            or path.stat().st_size != item["bytes"]
        ):
            raise ValidationError(
                f"release retained artifact byte count changed: {path}"
            )
        paths.append(path)
        roles.append(role)
    if len(set(paths)) != len(paths):
        raise ValidationError("release retained artifact paths are not globally unique")
    expected_counts = {
        "release_artifact_manifest": 1,
        "release_verifier": 1,
        "release_runner": 1,
        "release_lane_manifest": 7,
        "release_container_inspect": 7,
        "release_container_log": 7,
        "release_security_override": 4,
    }
    if set(roles) != set(expected_counts) or any(
        roles.count(role) != count for role, count in expected_counts.items()
    ):
        raise ValidationError("release retained artifact roles/counts are not exact")
    expected_lane_roles = {
        "release_lane_manifest": list(LANES),
        "release_container_inspect": list(LANES),
        "release_container_log": list(LANES),
        "release_security_override": [lane for lane in LANES if lane in SANITIZER_LANES],
    }
    for role, expected_lanes in expected_lane_roles.items():
        observed_lanes = [
            item.get("lane") for item in value if item.get("role") == role
        ]
        if observed_lanes != expected_lanes:
            raise ValidationError(
                f"release retained {role} lane order/inventory is not exact"
            )


def _step18_removed_field_hygiene(repo_root: Path) -> list[str]:
    removed_field = "min_separation_" + "clearance_m"
    allowed_test_paths = {
        "manyforge_behavior/tests/test_kernel_client.py",
        "manyforge_behavior/tests/test_program.py",
        "manyforge_composer/tests/test_roboplan_05_qualification.py",
        "manyforge_roboplan/test/test_native_e2e.py",
    }
    failures: list[str] = []
    for path in sorted(repo_root.rglob("*")):
        if not path.is_file() or any(
            part
            in {
                ".git",
                ".manyforge-var",
                ".pytest_cache",
                "build",
                "__pycache__",
                "node_modules",
            }
            or part.startswith("build-")
            for part in path.parts
        ):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if removed_field not in text:
            continue
        relative = path.relative_to(repo_root).as_posix()
        if relative.startswith("scripts/qualification/baseline/") and relative.endswith(".patch"):
            continue
        is_test_source = (
            relative in allowed_test_paths
            or ("tests" in path.relative_to(repo_root).parts and path.name.startswith("test_"))
            or ("test" in path.relative_to(repo_root).parts and path.name.startswith("test_"))
        )
        if is_test_source:
            lowered = text.lower()
            if not any(word in lowered for word in ("unknown", "reject", "legacy", "baseline")):
                failures.append(f"{relative}: removed field is not rejection/baseline evidence")
            continue
        if relative == "scripts/qualification/capture_isaac_drop_approach.py":
            required_boundary_fragments = {
                f'if "{removed_field}" in normalized:',
                f'clearance = normalized.pop("{removed_field}")',
                f'"Step 18 capture rejects nonzero {removed_field}"',
                f'contact.pop("{removed_field}", None)',
            }
            matching = {
                line.strip() for line in text.splitlines() if removed_field in line
            }
            if (
                matching == required_boundary_fragments
                and "historical baseline" in text
                and "never preserves or silently emulates it" in text
            ):
                continue
        if relative == "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp":
            if (
                text.count(removed_field) == 4
                and text.count("#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE") >= 2
                and "baseline mechanism replay requires captured contact_release policy"
                in text
            ):
                continue
        failures.append(f"{relative}: removed field remains outside strict test/baseline evidence")
    if failures:
        raise ValidationError("; ".join(failures))
    return [
        "removed_field_absent_from_production",
        "removed_field_occurrences_restricted_to_rejection_or_baseline_evidence",
    ]


def _step18_distance_scope_hygiene(repo_root: Path) -> list[str]:
    producer_roots = (
        "manyforge_core/include",
        "manyforge_core/src",
        "manyforge_collision/include",
        "manyforge_behavior/manyforge_behavior",
        "manyforge_composer/backend",
        "manyforge_roboplan/include",
        "manyforge_roboplan/src",
        "manyforge_ros2_control_execution/include",
        "manyforge_ros2_control_execution/src",
    )
    forbidden_tokens = ("kAllActivePairs", "all_active_pairs")
    failures: list[str] = []
    for relative_root in producer_roots:
        root = repo_root / relative_root
        if not root.is_dir():
            failures.append(f"missing production source root: {relative_root}")
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in {".cpp", ".hpp", ".h", ".py"}:
                continue
            if (
                path.relative_to(repo_root).as_posix()
                == "manyforge_core/include/manyforge_core/types/collision.hpp"
            ):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for token in forbidden_tokens:
                if token in text:
                    failures.append(
                        f"{path.relative_to(repo_root)}: production selects {token}"
                    )

    evaluator = repo_root / "manyforge_core/src/collision_policy_evaluator.cpp"
    coordinator = repo_root / "manyforge_core/src/collision_validation_coordinator.cpp"
    runtime = repo_root / "manyforge_core/src/runtime_collision_query_session.cpp"
    engine = repo_root / "manyforge_collision/src/manyforge_collision_engine.cpp"
    for path in (evaluator, coordinator, runtime, engine):
        if not path.is_file():
            failures.append(f"missing Step 18 distance-scope source: {path.relative_to(repo_root)}")
    if failures:
        raise ValidationError("; ".join(failures))

    evaluator_text = evaluator.read_text(encoding="utf-8")
    required_evaluator_fragments = (
        "entry.mode == CollisionContactPolicyMode::kBoundedPenetration",
        "ContactReleaseEvidenceMode::kSignedPenetration",
        "selected.push_back(canonicalize(entry.pair))",
        "contact_grasp_pair_matches(pair, *grasp)",
    )
    for fragment in required_evaluator_fragments:
        if fragment not in evaluator_text:
            failures.append(f"distance selection lost required fragment: {fragment}")
    for path in (coordinator, runtime):
        text = path.read_text(encoding="utf-8")
        if "CollisionDistanceRequestMode::kSelectedCollidingPairs" not in text:
            failures.append(f"{path.relative_to(repo_root)} no longer selects colliding pairs")
        modes = set(re.findall(r"CollisionDistanceRequestMode::(k[A-Za-z]+)", text))
        unexpected = modes - {"kNone", "kSelectedCollidingPairs"}
        if unexpected:
            failures.append(
                f"{path.relative_to(repo_root)} selects unexpected distance "
                f"modes: {sorted(unexpected)}"
            )

    engine_text = engine.read_text(encoding="utf-8")
    collision_calls = [
        match.start()
        for match in re.finditer(r"pinocchio::computeCollisions\(", engine_text)
    ]
    distance_calls = [
        match.start()
        for match in re.finditer(
            r"pinocchio::computeDistance(?:s)?\(", engine_text
        )
    ]
    if len(collision_calls) != 2 or len(distance_calls) != 4:
        failures.append(
            "collision engine call topology differs from the reviewed "
            "two-path contract"
        )
    elif not (
        collision_calls[0] < distance_calls[0] < distance_calls[1] < collision_calls[1]
        < distance_calls[2] < distance_calls[3]
    ):
        failures.append("collision engine no longer enumerates collisions before distance work")
    if engine_text.count("stop_at_first_collision=*/false") + engine_text.count(
        "stop_at_first_collision */ false"
    ) != 2:
        failures.append("collision engine does not prove complete enumeration in both paths")
    if failures:
        raise ValidationError("; ".join(failures))
    checks = [
        "production_request_producers_never_select_all_active_pairs",
        "binary_contact_selects_no_distance_pairs",
        "signed_bounded_and_contact_grasp_select_configured_pairs",
        "collision_enumeration_precedes_sparse_distance_work",
    ]
    checks.extend(_step18_distance_capability_hygiene(repo_root))
    return checks


def _step18_distance_capability_hygiene(repo_root: Path) -> list[str]:
    paths = {
        "types": repo_root / "manyforge_core/include/manyforge_core/types/collision.hpp",
        "interface": repo_root
        / "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp",
        "engine": repo_root / "manyforge_collision/src/manyforge_collision_engine.cpp",
        "coordinator": repo_root
        / "manyforge_core/src/collision_validation_coordinator.cpp",
        "runtime": repo_root / "manyforge_core/src/runtime_collision_query_session.cpp",
    }
    failures: list[str] = []
    texts: dict[str, str] = {}
    for name, path in paths.items():
        if not path.is_file():
            failures.append(f"missing distance-capability source: {path.relative_to(repo_root)}")
            continue
        texts[name] = path.read_text(encoding="utf-8", errors="replace")
    if failures:
        raise ValidationError("; ".join(failures))

    types = texts["types"]
    descriptor_declarations = re.findall(
        r"std::vector\s*<\s*CollisionDistanceRequestMode\s*>\s*"
        r"supported_distance_request_modes\s*;",
        types,
    )
    if len(descriptor_declarations) != 1:
        failures.append(
            "distance capability descriptor does not declare one exact mode inventory"
        )
    validity_start = types.find("collision_distance_request_modes_valid(")
    exact_start = types.find("collision_engine_supports_distance_request_mode(")
    exact_end = types.find("/// Compare-mode policy", exact_start)
    if validity_start < 0 or exact_start < 0 or exact_end < 0:
        failures.append("shared exact distance-scope admission helpers are missing")
        validity = ""
        exact = ""
    else:
        validity = types[validity_start:exact_start]
        exact = types[exact_start:exact_end]
    required_validity_fragments = (
        "modes.empty()",
        "static_cast<int>(modes[index - 1]) >=",
        "!capability.supports_distance_query",
    )
    if any(fragment not in validity for fragment in required_validity_fragments):
        failures.append(
            "distance mode declaration no longer fails closed on empty, unordered, "
            "duplicate, or contradictory scopes"
        )
    if (
        "collision_distance_request_modes_valid(capability)" not in exact
        or "std::binary_search(" not in exact
        or "requested_mode" not in exact
        or "kAllCollidingPairs" in exact
        or "kAllActivePairs" in exact
    ):
        failures.append(
            "shared distance-scope admission is not an exact declared-mode lookup"
        )

    engine = texts["engine"]
    declarations = re.findall(
        r"capability_\.supported_distance_request_modes\s*=\s*\{(.*?)\};",
        engine,
        flags=re.DOTALL,
    )
    declared_modes = (
        re.findall(r"CollisionDistanceRequestMode::(k[A-Za-z]+)", declarations[0])
        if len(declarations) == 1
        else []
    )
    expected_modes = [
        "kNone",
        "kSelectedCollidingPairs",
        "kAllCollidingPairs",
        "kAllActivePairs",
    ]
    if declared_modes != expected_modes:
        failures.append(
            "ManyForge collision engine distance scopes are not the exact sorted "
            "none/selected/all-colliding/all-active inventory"
        )

    interface = texts["interface"]
    status_start = interface.find("runtime_collision_query_capability_status(")
    status_end = interface.find(
        "runtime_collision_query_capability_status_name(", status_start
    )
    status = (
        interface[status_start:status_end]
        if status_start >= 0 and status_end >= 0
        else ""
    )
    if "collision_distance_request_modes_valid(capability)" not in status:
        failures.append(
            "runtime capability admission no longer rejects an invalid exact-scope inventory"
        )

    coordinator = texts["coordinator"]
    admission_start = coordinator.find("bool engine_admits_query_capabilities(")
    admission_end = coordinator.find(
        "CollisionResult capability_admission_failure(", admission_start
    )
    admission = (
        coordinator[admission_start:admission_end]
        if admission_start >= 0 and admission_end >= 0
        else ""
    )
    if (
        "collision_engine_supports_distance_request_mode(" not in admission
        or "capability, distance_request_mode" not in admission
        or coordinator.count("engine_admits_query_capabilities(") != 3
    ):
        failures.append(
            "validation does not admit both query paths through exact distance scope"
        )
    required_start = coordinator.find("std::vector<std::string> required_capabilities_for(")
    required_end = coordinator.find("bool engine_admits_query_capabilities(", required_start)
    required = (
        coordinator[required_start:required_end]
        if required_start >= 0 and required_end >= 0
        else ""
    )
    if (
        "CollisionDistanceRequestMode distance_request_mode" not in required
        or "distance_request_mode != CollisionDistanceRequestMode::kNone" not in required
        or 'capabilities.push_back("penetration_depth")' not in required
    ):
        failures.append(
            "validation required capabilities are no longer scoped by the selected mode"
        )

    runtime = texts["runtime"]
    bind_start = runtime.find("bool RuntimeCollisionQuerySession::bind_distance_selection(")
    bind_end = runtime.find("bool RuntimeCollisionQuerySession::bind_policy(", bind_start)
    binding = (
        runtime[bind_start:bind_end]
        if bind_start >= 0 and bind_end >= 0
        else ""
    )
    if (
        runtime.count("collision_engine_supports_distance_request_mode(") != 1
        or "collision_engine_supports_distance_request_mode(*capability_," not in binding
        or "requested_mode" not in binding
    ):
        failures.append(
            "runtime policy binding does not use exact distance-scope admission"
        )
    if any(
        token in coordinator or token in runtime
        for token in (
            "CollisionDistanceRequestMode::kAllCollidingPairs",
            "CollisionDistanceRequestMode::kAllActivePairs",
        )
    ):
        failures.append(
            "validation/runtime contains a broader distance-scope fallback"
        )

    if failures:
        raise ValidationError("; ".join(failures))
    return [
        "distance_scope_capability_descriptor_is_explicit",
        "distance_scope_declaration_is_sorted_unique_and_exactly_admitted",
        "manyforge_engine_publishes_all_four_exact_distance_scopes",
        "validation_uses_exact_scope_admission_on_both_query_paths",
        "runtime_uses_exact_scope_admission",
        "distance_capabilities_are_mode_scoped_without_broader_fallback",
    ]


def run_step18_hygiene(repo_root: Path) -> dict[str, Any]:
    root = repo_root.resolve()
    if not root.is_dir():
        raise ValidationError(f"repository root is unavailable: {root}")
    checks = _step18_removed_field_hygiene(root)
    checks.extend(_step18_distance_scope_hygiene(root))
    return {
        "schema_version": "manyforge.step18.release_gate_hygiene.v1",
        "checks": checks,
        "check_count": len(checks),
        "passed": True,
        "overall_step18_closure_claimed": False,
    }


def _write_exclusive(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def publish_verification_summary(
    output: Path, result: dict[str, Any], manifest_path: Path
) -> None:
    campaign = _load_json(manifest_path.resolve(), "release-gate artifact manifest")
    candidate = campaign.get("candidate")
    if not isinstance(candidate, dict):
        raise ValidationError("release-gate publication candidate is missing")
    repo_root = Path(str(candidate.get("manyforge_root", ""))).resolve()
    specs_root = Path(str(candidate.get("specs_root", ""))).resolve()
    destination = output.resolve()
    evidence_root = (repo_root / ".manyforge-var/qualification").resolve()
    try:
        relative = destination.relative_to(evidence_root)
    except ValueError as error:
        raise ValidationError(
            f"release summary output must be below {evidence_root}"
        ) from error
    if not relative.parts:
        raise ValidationError("release summary output must name a file")
    ignored = subprocess.run(
        ["git", "-C", str(repo_root), "check-ignore", "--quiet", "--", str(destination)],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ignored.returncode != 0:
        raise ValidationError("release summary output is not ignored")
    if destination.exists():
        raise ValidationError(f"refusing to overwrite release summary: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    if temporary.exists():
        raise ValidationError(f"release summary temporary output exists: {temporary}")
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        _assert_retained_artifact_manifest_current(
            result.get("retained_artifact_manifest")
        )
        if source_identity(repo_root) != result.get("candidate_source_identity"):
            raise ValidationError(
                "ManyForge source changed before release summary publication"
            )
        publication_specs = source_identity(specs_root)
        campaign_specs = result.get("candidate_specs_source_identity")
        if not isinstance(campaign_specs, dict) or (
            publication_specs["head_commit"] != campaign_specs.get("head_commit")
            or publication_specs["head_tree"] != campaign_specs.get("head_tree")
        ):
            raise ValidationError(
                "full Specs Git base changed before release summary publication"
            )
        if specs_contract_identity(specs_root) != result.get(
            "candidate_specs_contract_identity"
        ):
            raise ValidationError(
                "Specs contract changed before release summary publication"
            )
        os.link(temporary, destination)
        try:
            temporary.unlink()
        except OSError:
            pass
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify", help="verify all seven retained lanes")
    verify_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser.add_argument("--output", type=Path, required=True)
    hygiene_parser = subparsers.add_parser(
        "hygiene", help="run Step 18 removed-field and distance-scope guards"
    )
    hygiene_parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "hygiene":
            result = run_step18_hygiene(args.repo_root)
            print(
                "STEP18_SPECIFIC_HYGIENE passed "
                f"checks={result['check_count']}"
            )
            print(json.dumps(result, sort_keys=True, allow_nan=False))
            return 0
        result = verify_artifact_manifest(args.manifest)
        publish_verification_summary(args.output, result, args.manifest)
    except (OSError, SourceIdentityError, ValidationError, ValueError) as error:
        print(f"Step 18 release-gate verification failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"passed": True, "lanes": len(LANES)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
