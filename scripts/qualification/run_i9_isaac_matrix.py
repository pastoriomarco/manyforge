#!/usr/bin/env python3
"""Freeze, validate, and reduce the I9 Isaac qualification matrix.

The expensive live batch is split across an Isaac Sim host and one
source-matched ManyForge runtime host.  This module owns the small immutable
contract shared by those processes and reduces their bounded observations into
one compact verdict.  It never invents missing evidence: all five registered
Isaac rows and every matrix-level recovery obligation must be present.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Final

from manyforge_behavior.canonical_artifact import load_strict_json_yaml


MATRIX_SCHEMA: Final = "manyforge.i9_isaac_matrix.v1"
PREFLIGHT_SCHEMA: Final = "manyforge.i9_isaac_preflight.v1"
ROW_SCHEMA: Final = "manyforge.i9_isaac_row.v1"
WORLD_MANIFEST_SCHEMA: Final = "manyforge.i9_isaac_world_manifest.v1"

EXPECTED_DDS: Final = {
    "ROS_DOMAIN_ID": "0",
    "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp",
    "ROS_AUTOMATIC_DISCOVERY_RANGE": "LOCALHOST",
    "ROS_STATIC_PEERS": "192.168.1.132;192.168.1.68",
}
STALE_DDS_KEYS: Final = (
    "FASTRTPS_DEFAULT_PROFILES_FILE",
    "FASTDDS_DEFAULT_PROFILES_FILE",
    "ROS_DISCOVERY_SERVER",
    "ROS_SUPER_CLIENT",
    "CYCLONEDDS_URI",
)
SHA256_PATTERN: Final = re.compile(r"^[0-9a-f]{64}$")
MAX_JSON_BYTES: Final = 8 * 1024 * 1024
MAX_CHECK_DETAIL_CHARS: Final = 4096
TOPIC_TYPE_BY_NAME: Final = {
    "/clock": "rosgraph_msgs/msg/Clock",
    "/isaac_gripper_state": "sensor_msgs/msg/JointState",
    "/isaac_joint_commands": "sensor_msgs/msg/JointState",
    "/isaac_joint_commands_gripper": "sensor_msgs/msg/JointState",
    "/isaac_joint_states": "sensor_msgs/msg/JointState",
    "/output": "vision_msgs/msg/Detection3DArray",
    "/tf": "tf2_msgs/msg/TFMessage",
    "/tf_static": "tf2_msgs/msg/TFMessage",
}

COMMON_ROW_CHECKS: Final = frozenset(
    {
        "controller_ready",
        "cross_host_clock_payload",
        "exact_world_readback",
        "execution_owner_exact",
        "joint_command_payload",
        "joint_state_payload",
        "program_terminal",
        "reset_observed",
        "required_topics_exact",
        "runtime_namespace_exact",
        "simulation_stopped_after",
        "simulation_stopped_before",
        "status_trace_correlated",
    }
)
MATRIX_CHECKS: Final = frozenset(
    {
        "clock_synchronized",
        "dds_loss_rejoin_bounded",
        "demo_supervisor_absent",
        "direct_controller_route",
        "no_cross_drive",
        "provider_loss_recovery_bounded",
        "representative_chained_controller_route",
        "scene_reload_exact",
        "structured_alarm_correlation",
        "two_same_type_instances_isolated",
    }
)


@dataclass(frozen=True, slots=True)
class ScenarioSpec:
    scenario_id: str
    deployment_artifact_id: str
    cell_id: str
    component_id: str
    profile: str
    admission_filename: str
    program_filename: str
    pose_observer_filename: str
    world_uri: str
    world_name: str
    world_sha256: str
    required_topics: tuple[str, ...]
    required_checks: frozenset[str]


_ISAAC_ASSET_ROOT = (
    "/home/tndlux/workspaces/isaac_ros-dev/src/isaac_sim_custom_examples"
)
_UR10_WORLD = (
    f"{_ISAAC_ASSET_ROOT}/Scenario/IsaacManipulator/UniversalRobots/"
    "isaac_manipulator_scene_light.usda"
)

SCENARIOS: Final = (
    ScenarioSpec(
        scenario_id="isaac-lite6-pick-place",
        deployment_artifact_id="isaac_lite6_lite_gripper_pick_and_place",
        cell_id="isaac_lite6_pick_place_cell",
        component_id="isaac_lite6_robot",
        profile="lite6",
        admission_filename=(
            "canonical-empty-admission.isaac_lite6_pick_place_cell.json"
        ),
        program_filename="isaac_lite6_lite_gripper_pick_and_place.program.yaml",
        pose_observer_filename="isaac_lite6_pick_place.pose_observer.json",
        world_uri=f"file:{_ISAAC_ASSET_ROOT}/lite6_grf_ros2.usda",
        world_name="lite6_grf_ros2",
        world_sha256=(
            "64f9511a717f57556d028f171132e4ee8240daddbfb03114699098b4e7499e4e"
        ),
        required_topics=(
            "/clock",
            "/isaac_joint_commands",
            "/isaac_joint_commands_gripper",
            "/isaac_joint_states",
        ),
        required_checks=COMMON_ROW_CHECKS
        | frozenset({"gripper_converged", "scene_observation_fresh"}),
    ),
    ScenarioSpec(
        scenario_id="isaac-lite6-trocar-foundationpose",
        deployment_artifact_id="trocar_lite6_foundationpose_pick",
        cell_id="isaac_lite6_trocar_cell",
        component_id="isaac_lite6_trocar_robot",
        profile="lite6",
        admission_filename=(
            "canonical-empty-admission.isaac_lite6_trocar_cell.json"
        ),
        program_filename="trocar_lite6_foundationpose_pick.program.yaml",
        pose_observer_filename=(
            "isaac_lite6_trocar_foundationpose.pose_observer.json"
        ),
        world_uri=(
            f"file:{_ISAAC_ASSET_ROOT}/"
            "test_scene_realsense_foundationpose_trocar.usda"
        ),
        world_name="test_scene_realsense_foundationpose_trocar",
        world_sha256=(
            "7808872d07f9242976c0bc4f62e434b3d7299518ef61287dadc19ee286ef5aa9"
        ),
        required_topics=(
            "/clock",
            "/isaac_joint_commands",
            "/isaac_joint_commands_gripper",
            "/isaac_joint_states",
            "/output",
            "/tf",
            "/tf_static",
        ),
        required_checks=COMMON_ROW_CHECKS
        | frozenset(
            {
                "foundationpose_detection_fresh",
                "foundationpose_pose_latched",
                "gripper_converged",
                "scene_observation_fresh",
                "tf_chain_exact",
            }
        ),
    ),
    ScenarioSpec(
        scenario_id="isaac-ur10e-light-scene",
        deployment_artifact_id="isaac_manipulator_scene_light_ros2_control",
        cell_id="isaac_ur10e_light_cell",
        component_id="isaac_ur10e_light_robot",
        profile="ur10e",
        admission_filename=(
            "canonical-empty-admission.isaac_ur10e_light_cell.json"
        ),
        program_filename="isaac_manipulator_scene_light.program.yaml",
        pose_observer_filename="isaac_ur10e_light.pose_observer.json",
        world_uri=f"file:{_UR10_WORLD}",
        world_name="isaac_manipulator_scene_light",
        world_sha256=(
            "e659d8923b527a73e68840af3a6899353fdc4239a2b437104986e65e1aa43acb"
        ),
        required_topics=(
            "/clock",
            "/isaac_gripper_state",
            "/isaac_joint_commands",
            "/isaac_joint_states",
        ),
        required_checks=COMMON_ROW_CHECKS
        | frozenset(
            {
                "gripper_converged",
                "object_motion_observed",
                "scene_observation_fresh",
            }
        ),
    ),
    ScenarioSpec(
        scenario_id="isaac-ur10e-light-scene-assistant",
        deployment_artifact_id="isaac_manipulator_scene_light_assistant",
        cell_id="isaac_ur10e_assistant_cell",
        component_id="isaac_ur10e_assistant_robot",
        profile="ur10e",
        admission_filename=(
            "canonical-empty-admission.isaac_ur10e_assistant_cell.json"
        ),
        program_filename="isaac_manipulator_scene_light.program.yaml",
        pose_observer_filename="isaac_ur10e_assistant.pose_observer.json",
        world_uri=f"file:{_UR10_WORLD}",
        world_name="isaac_manipulator_scene_light",
        world_sha256=(
            "e659d8923b527a73e68840af3a6899353fdc4239a2b437104986e65e1aa43acb"
        ),
        required_topics=(
            "/clock",
            "/isaac_gripper_state",
            "/isaac_joint_commands",
            "/isaac_joint_states",
        ),
        required_checks=COMMON_ROW_CHECKS
        | frozenset(
            {
                "assistant_snapshot_owner_exact",
                "assistant_snapshot_transmitted",
                "ghost_and_viewer_owner_exact",
                "gripper_converged",
                "scene_observation_fresh",
            }
        ),
    ),
    ScenarioSpec(
        scenario_id="isaac-ur10e-light-scene-collision",
        deployment_artifact_id=(
            "isaac_manipulator_scene_light_ros2_control_collision"
        ),
        cell_id="isaac_ur10e_collision_cell",
        component_id="isaac_ur10e_collision_robot",
        profile="ur10e",
        admission_filename=(
            "canonical-empty-admission.isaac_ur10e_collision_cell.json"
        ),
        program_filename="isaac_manipulator_scene_light_collision.program.yaml",
        pose_observer_filename="isaac_ur10e_collision.pose_observer.json",
        world_uri=f"file:{_UR10_WORLD}",
        world_name="isaac_manipulator_scene_light",
        world_sha256=(
            "e659d8923b527a73e68840af3a6899353fdc4239a2b437104986e65e1aa43acb"
        ),
        required_topics=(
            "/clock",
            "/isaac_gripper_state",
            "/isaac_joint_commands",
            "/isaac_joint_states",
        ),
        required_checks=COMMON_ROW_CHECKS
        | frozenset(
            {
                "collision_stop_kernel_owned",
                "collision_stop_terminal",
                "scene_observation_fresh",
            }
        ),
    ),
)
SCENARIO_BY_ID: Final = {row.scenario_id: row for row in SCENARIOS}


class QualificationError(RuntimeError):
    """One fail-closed matrix contract or evidence error."""


def validate_dds_environment(
    environment: Mapping[str, str],
) -> dict[str, Any]:
    """Require the one plan-recorded Fast DDS topology."""

    mismatches = {
        key: {"expected": expected, "observed": environment.get(key)}
        for key, expected in EXPECTED_DDS.items()
        if environment.get(key) != expected
    }
    stale = sorted(key for key in STALE_DDS_KEYS if environment.get(key))
    if mismatches or stale:
        raise QualificationError(
            "I9 DDS environment differs from the recorded topology: "
            + json.dumps(
                {"mismatches": mismatches, "stale_keys": stale},
                separators=(",", ":"),
                sort_keys=True,
            )
        )
    return {
        "automatic_discovery_range": EXPECTED_DDS[
            "ROS_AUTOMATIC_DISCOVERY_RANGE"
        ],
        "domain_id": int(EXPECTED_DDS["ROS_DOMAIN_ID"]),
        "rmw_implementation": EXPECTED_DDS["RMW_IMPLEMENTATION"],
        "static_peers": EXPECTED_DDS["ROS_STATIC_PEERS"].split(";"),
    }


def build_world_manifest(asset_root: Path) -> dict[str, Any]:
    """Hash the three retained host-local USD inputs without loading Isaac."""

    rows: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for scenario in SCENARIOS:
        path = Path(scenario.world_uri.removeprefix("file:"))
        try:
            relative = path.relative_to(Path(_ISAAC_ASSET_ROOT))
        except ValueError as error:
            raise QualificationError(
                "scenario world is outside the asset root"
            ) from error
        candidate = asset_root / relative
        key = (scenario.world_uri, scenario.world_sha256)
        if key in seen:
            continue
        seen.add(key)
        if not candidate.is_file() or candidate.is_symlink():
            raise QualificationError(
                f"retained world is missing or not a regular file: {candidate}"
            )
        digest = _sha256_file(candidate)
        if digest != scenario.world_sha256:
            raise QualificationError(
                f"retained world digest changed for {candidate}: {digest}"
            )
        rows.append(
            {
                "name": scenario.world_name,
                "sha256": digest,
                "uri": scenario.world_uri,
            }
        )
    return {
        "schema_version": WORLD_MANIFEST_SCHEMA,
        "worlds": sorted(rows, key=lambda row: row["uri"]),
    }


def validate_world_manifest(value: Any) -> dict[str, Any]:
    manifest = _object(value, label="world manifest")
    if set(manifest) != {"schema_version", "worlds"}:
        raise QualificationError("world manifest fields differ from the v1 schema")
    if manifest["schema_version"] != WORLD_MANIFEST_SCHEMA:
        raise QualificationError("world manifest schema is not I9 v1")
    rows = _object_rows(manifest["worlds"], label="world manifest worlds")
    expected = {
        (scenario.world_uri, scenario.world_name, scenario.world_sha256)
        for scenario in SCENARIOS
    }
    observed: set[tuple[str, str, str]] = set()
    for row in rows:
        if set(row) != {"name", "sha256", "uri"}:
            raise QualificationError("world manifest row fields differ from v1")
        digest = _text(row["sha256"], label="world sha256")
        if SHA256_PATTERN.fullmatch(digest) is None:
            raise QualificationError("world manifest has an invalid sha256")
        observed.add(
            (
                _text(row["uri"], label="world uri"),
                _text(row["name"], label="world name"),
                digest,
            )
        )
    if observed != expected or len(rows) != len(expected):
        raise QualificationError("world manifest does not contain the exact I9 worlds")
    return manifest


def validate_package(package_root: Path, source_root: Path) -> dict[str, Any]:
    """Bind the materialized package and source configs to the five-row spec."""

    if not package_root.is_dir() or package_root.is_symlink():
        raise QualificationError("package root must be one real directory")
    catalog = _object(
        load_strict_json_yaml(package_root / "maintained.scenario-catalog.yaml"),
        label="scenario catalog",
    )
    catalog_rows = _object_rows(catalog.get("scenarios"), label="catalog scenarios")
    by_id = {
        _text(row.get("scenario_id"), label="catalog scenario id"): row
        for row in catalog_rows
    }
    if set(SCENARIO_BY_ID) - set(by_id):
        raise QualificationError("materialized catalog omits an I9 scenario")

    validated: list[dict[str, str]] = []
    for spec in SCENARIOS:
        row = by_id[spec.scenario_id]
        if row.get("deployment_artifact_id") != spec.deployment_artifact_id:
            raise QualificationError(
                f"{spec.scenario_id} selected another deployment artifact"
            )
        for filename in (spec.admission_filename, spec.program_filename):
            artifact = package_root / filename
            if not artifact.is_file() or artifact.is_symlink():
                raise QualificationError(
                    f"{spec.scenario_id} materialized artifact missing: {filename}"
                )
        program = _object(
            load_strict_json_yaml(package_root / spec.program_filename),
            label=f"{spec.scenario_id} program",
        )
        expected_program_id = spec.program_filename.removesuffix(".program.yaml")
        if program.get("program_id") != expected_program_id:
            raise QualificationError(f"{spec.scenario_id} selected another Program")
        pose_config = source_root / "manyforge_ros_bringup/config" / (
            spec.pose_observer_filename
        )
        pose = _object(
            load_strict_json_yaml(pose_config),
            label=f"{spec.scenario_id} pose observer",
        )
        if pose.get("cell_id") != spec.cell_id:
            raise QualificationError(
                f"{spec.scenario_id} pose observer selected another cell"
            )
        validated.append(
            {
                "cell_id": spec.cell_id,
                "component_id": spec.component_id,
                "deployment_artifact_id": spec.deployment_artifact_id,
                "profile": spec.profile,
                "scenario_id": spec.scenario_id,
            }
        )

    site = _object(
        load_strict_json_yaml(
            source_root
            / "manyforge_ros_bringup/config/isaac_program_simulation_site.json"
        ),
        label="simulation site config",
    )
    site_rows = _object_rows(site.get("scenarios"), label="simulation site rows")
    if tuple(row.get("scenario_id") for row in site_rows) != tuple(
        row.scenario_id for row in SCENARIOS
    ):
        raise QualificationError("simulation site row order/identity differs from I9")

    return {
        "scenario_catalog_revision": _text(
            catalog.get("scenario_catalog_revision"),
            label="scenario catalog revision",
        ),
        "scenarios": validated,
    }


def validate_row(value: Any) -> dict[str, Any]:
    """Validate one live row without accepting caller-selected obligations."""

    row = _object(value, label="I9 row")
    required_fields = {
        "checks",
        "component_id",
        "details",
        "elapsed_s",
        "scenario_id",
        "schema_version",
        "world",
    }
    if set(row) != required_fields:
        raise QualificationError("I9 row fields differ from the v1 schema")
    if row["schema_version"] != ROW_SCHEMA:
        raise QualificationError("I9 row schema is not v1")
    scenario_id = _text(row["scenario_id"], label="row scenario id")
    try:
        spec = SCENARIO_BY_ID[scenario_id]
    except KeyError as error:
        raise QualificationError(
            f"unregistered I9 scenario row: {scenario_id}"
        ) from error
    if row["component_id"] != spec.component_id:
        raise QualificationError(f"{scenario_id} row selected another component")
    world = _object(row["world"], label=f"{scenario_id} world")
    if set(world) != {"name", "sha256", "uri"} or world != {
        "name": spec.world_name,
        "sha256": spec.world_sha256,
        "uri": spec.world_uri,
    }:
        raise QualificationError(f"{scenario_id} row selected another world")
    checks = _object(row["checks"], label=f"{scenario_id} checks")
    if set(checks) != spec.required_checks:
        raise QualificationError(f"{scenario_id} checks differ from the frozen set")
    for check, passed in checks.items():
        if type(passed) is not bool:
            raise QualificationError(f"{scenario_id}/{check} must be boolean")
    details = _object(row["details"], label=f"{scenario_id} details")
    for key, detail in details.items():
        _text(key, label="row detail key")
        if len(json.dumps(detail, separators=(",", ":"), sort_keys=True)) > (
            MAX_CHECK_DETAIL_CHARS
        ):
            raise QualificationError(f"{scenario_id}/{key} detail exceeds its bound")
    elapsed = row["elapsed_s"]
    if isinstance(elapsed, bool) or not isinstance(elapsed, (int, float)):
        raise QualificationError(f"{scenario_id} elapsed_s must be numeric")
    if not 0.0 <= float(elapsed) <= 3600.0:
        raise QualificationError(f"{scenario_id} elapsed_s is outside its bound")
    return row


def reduce_matrix(
    rows: Sequence[Any],
    matrix_checks: Mapping[str, Any],
    *,
    preflight: Mapping[str, Any],
) -> dict[str, Any]:
    """Reduce exactly five rows plus recovery evidence into one verdict."""

    if len(rows) != len(SCENARIOS):
        raise QualificationError("I9 matrix requires exactly five rows")
    validated = [validate_row(row) for row in rows]
    by_id = {row["scenario_id"]: row for row in validated}
    if len(by_id) != len(validated) or set(by_id) != set(SCENARIO_BY_ID):
        raise QualificationError("I9 matrix rows are missing, duplicate, or extra")
    if set(matrix_checks) != MATRIX_CHECKS:
        raise QualificationError("I9 matrix checks differ from the frozen set")
    for check, passed in matrix_checks.items():
        if type(passed) is not bool:
            raise QualificationError(f"matrix check {check} must be boolean")
    if preflight.get("schema_version") != PREFLIGHT_SCHEMA:
        raise QualificationError("I9 matrix preflight is absent or incompatible")
    ordered = [by_id[spec.scenario_id] for spec in SCENARIOS]
    failed_rows = {
        row["scenario_id"]: sorted(
            check for check, passed in row["checks"].items() if not passed
        )
        for row in ordered
        if not all(row["checks"].values())
    }
    failed_matrix_checks = sorted(
        check for check, passed in matrix_checks.items() if not passed
    )
    return {
        "schema_version": MATRIX_SCHEMA,
        "passed": not failed_rows and not failed_matrix_checks,
        "preflight": dict(preflight),
        "rows": ordered,
        "matrix_checks": dict(sorted(matrix_checks.items())),
        "failures": {
            "matrix_checks": failed_matrix_checks,
            "rows": failed_rows,
        },
    }


def build_preflight(
    *,
    package_root: Path,
    source_root: Path,
    world_manifest: Mapping[str, Any],
    environment: Mapping[str, str],
) -> dict[str, Any]:
    return {
        "schema_version": PREFLIGHT_SCHEMA,
        "dds": validate_dds_environment(environment),
        "package": validate_package(package_root, source_root),
        "world_manifest": validate_world_manifest(world_manifest),
    }


def _load_json(path: Path) -> Any:
    if not path.is_file() or path.is_symlink():
        raise QualificationError(f"JSON input is missing or not regular: {path}")
    size = path.stat().st_size
    if not 0 < size <= MAX_JSON_BYTES:
        raise QualificationError(f"JSON input size is outside its bound: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise QualificationError(f"invalid JSON input {path}: {error}") from error


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _object(value: Any, *, label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or not all(
        isinstance(key, str) for key in value
    ):
        raise QualificationError(f"{label} must be one string-keyed object")
    return dict(value)


def _object_rows(value: Any, *, label: str) -> tuple[dict[str, Any], ...]:
    if not isinstance(value, list) or len(value) > 4096:
        raise QualificationError(f"{label} must be one bounded array")
    return tuple(_object(row, label=f"{label} row") for row in value)


def _text(value: Any, *, label: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 4096:
        raise QualificationError(f"{label} must be bounded non-empty text")
    return value


def _emit(value: Mapping[str, Any]) -> None:
    encoded = json.dumps(value, separators=(",", ":"), sort_keys=True)
    if len(encoded.encode("utf-8")) > MAX_JSON_BYTES:
        raise QualificationError("I9 result exceeds its byte bound")
    print(encoded)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    manifest_parser = subparsers.add_parser("world-manifest")
    manifest_parser.add_argument(
        "--asset-root", type=Path, default=Path(_ISAAC_ASSET_ROOT)
    )

    preflight_parser = subparsers.add_parser("preflight")
    preflight_parser.add_argument("--package-root", type=Path, required=True)
    preflight_parser.add_argument("--source-root", type=Path, required=True)
    preflight_parser.add_argument("--world-manifest", type=Path, required=True)

    reduce_parser = subparsers.add_parser("reduce")
    reduce_parser.add_argument("--preflight", type=Path, required=True)
    reduce_parser.add_argument("--matrix-checks", type=Path, required=True)
    reduce_parser.add_argument("rows", nargs="+", type=Path)

    args = parser.parse_args(argv)
    try:
        if args.command == "world-manifest":
            _emit(build_world_manifest(args.asset_root))
        elif args.command == "preflight":
            _emit(
                build_preflight(
                    package_root=args.package_root,
                    source_root=args.source_root,
                    world_manifest=_object(
                        _load_json(args.world_manifest), label="world manifest"
                    ),
                    environment=os.environ,
                )
            )
        else:
            result = reduce_matrix(
                [_load_json(path) for path in args.rows],
                _object(_load_json(args.matrix_checks), label="matrix checks"),
                preflight=_object(_load_json(args.preflight), label="preflight"),
            )
            _emit(result)
            return 0 if result["passed"] else 2
    except QualificationError as error:
        print(f"I9 qualification rejected: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
