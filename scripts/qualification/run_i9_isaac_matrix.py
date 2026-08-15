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
import math
import os
from pathlib import Path
import re
import sys
from typing import Any, Final

from manyforge_behavior.canonical_artifact import load_strict_json_yaml


MATRIX_SCHEMA: Final = "manyforge.i9_isaac_matrix.v1"
MATRIX_EVIDENCE_SCHEMA: Final = "manyforge.i9_isaac_matrix_evidence.v1"
PREFLIGHT_SCHEMA: Final = "manyforge.i9_isaac_preflight.v1"
ROW_SCHEMA: Final = "manyforge.i9_isaac_row.v1"
WORLD_MANIFEST_SCHEMA: Final = "manyforge.i9_isaac_world_manifest.v1"
CLOCK_EVIDENCE_SCHEMA: Final = "manyforge.i9_cross_host_clock.v1"
STATIC_PROBES_SCHEMA: Final = "manyforge.i9_static_probes.v1"
RECOVERY_EVIDENCE_SCHEMA: Final = "manyforge.i9_isaac_recovery.v1"

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
CLOCK_SAMPLE_COUNT: Final = 7
CLOCK_MAX_ABS_OFFSET_NS: Final = 250_000_000
CLOCK_MAX_ROUND_TRIP_NS: Final = 1_000_000_000
RECOVERY_BOUND_S: Final = 60.0
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
        "cclc_route_ready",
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
PROBE_CONTRACTS: Final = {
    "demo_supervisor_absent": (
        "pytest",
        "manyforge_ros_bringup/tests/"
        "test_isaac_topic_based_control_launch.py::"
        "test_demo_supervisor_entrypoint_has_no_backup_owner",
    ),
    "direct_controller_route": (
        "gtest",
        "RetainedRos2ControlBackendFactoryTest."
        "RetainedIsaacProfileExpandsToExactControllerAndGripperContract",
    ),
    "representative_chained_controller_route": (
        "gtest",
        "VerifiedControllerLifecycleExpectedDomainTest."
        "ChainedProductPreservesCompleteTopologyOrderAndSelectedEndpoints",
    ),
    "two_same_type_instances_isolated": (
        "gtest",
        "RetainedRos2ControlBackendFactoryTest."
        "SameTypeIsaacInstancesHaveDisjointControlPlaneEndpoints",
    ),
}


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
            "94cf6c23e693f7034bbdaa08b7130e82366473412836082c881aac1386f133c0"
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
            "47574d43d9b05db72c92034a198be854da43ad1deaa783e256722676f1b76692"
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


def validate_clock_evidence(value: Any) -> tuple[dict[str, Any], bool]:
    """Validate raw cross-host samples and derive the synchronized verdict."""

    evidence = _object(value, label="clock evidence")
    if set(evidence) != {"hosts", "samples", "schema_version"}:
        raise QualificationError("clock evidence fields differ from the v1 schema")
    if evidence["schema_version"] != CLOCK_EVIDENCE_SCHEMA:
        raise QualificationError("clock evidence schema is not I9 v1")
    hosts = _object(evidence["hosts"], label="clock hosts")
    if set(hosts) != {"laptop", "orin"}:
        raise QualificationError("clock evidence must contain laptop and Orin")
    normalized_hosts: dict[str, Any] = {}
    hosts_synchronized = True
    for label, expected_hostname, expected_address in (
        ("laptop", "tndlux-G16", "192.168.1.132"),
        ("orin", "localhost.localdomain", "192.168.1.68"),
    ):
        host = _object(hosts[label], label=f"{label} clock host")
        required = {
            "address",
            "clock_synchronized",
            "hostname",
            "synchronization_marker",
        }
        if set(host) != required:
            raise QualificationError(
                f"{label} clock host fields differ from the v1 schema"
            )
        if host["hostname"] != expected_hostname:
            raise QualificationError(f"clock evidence selected another {label}")
        if host["address"] != expected_address:
            raise QualificationError(
                f"clock evidence selected another {label} address"
            )
        synchronized = _boolean(
            host["clock_synchronized"], label=f"{label} clock synchronized"
        )
        marker = _boolean(
            host["synchronization_marker"],
            label=f"{label} synchronization marker",
        )
        hosts_synchronized = hosts_synchronized and synchronized and marker
        normalized_hosts[label] = {
            "address": expected_address,
            "clock_synchronized": synchronized,
            "hostname": expected_hostname,
            "synchronization_marker": marker,
        }

    samples = _object_rows(evidence["samples"], label="clock samples")
    if len(samples) != CLOCK_SAMPLE_COUNT:
        raise QualificationError(
            f"clock evidence requires exactly {CLOCK_SAMPLE_COUNT} samples"
        )
    offsets: list[int] = []
    round_trips: list[int] = []
    for sample in samples:
        if set(sample) != {"local_receive_ns", "local_send_ns", "remote_ns"}:
            raise QualificationError(
                "clock sample fields differ from the v1 schema"
            )
        sent = _integer(
            sample["local_send_ns"], label="clock sample local send", minimum=1
        )
        received = _integer(
            sample["local_receive_ns"],
            label="clock sample local receive",
            minimum=sent,
        )
        remote = _integer(
            sample["remote_ns"], label="clock sample remote", minimum=1
        )
        round_trips.append(received - sent)
        offsets.append(remote - ((sent + received) // 2))
    maximum_offset = max(abs(offset) for offset in offsets)
    maximum_round_trip = max(round_trips)
    passed = bool(
        hosts_synchronized
        and maximum_offset <= CLOCK_MAX_ABS_OFFSET_NS
        and maximum_round_trip <= CLOCK_MAX_ROUND_TRIP_NS
    )
    return {
        "hosts": normalized_hosts,
        "maximum_abs_midpoint_offset_ns": maximum_offset,
        "maximum_round_trip_ns": maximum_round_trip,
        "sample_count": len(samples),
    }, passed


def validate_static_probes(
    value: Any,
) -> tuple[dict[str, Any], dict[str, bool]]:
    """Validate exact test selectors and derive their four pass/fail facts."""

    evidence = _object(value, label="static probe evidence")
    if set(evidence) != {"probes", "schema_version", "source"}:
        raise QualificationError(
            "static probe evidence fields differ from the v1 schema"
        )
    if evidence["schema_version"] != STATIC_PROBES_SCHEMA:
        raise QualificationError("static probe evidence schema is not I9 v1")
    source = _object(evidence["source"], label="static probe source")
    if set(source) != {"commit", "dirty", "tree"}:
        raise QualificationError("static probe source fields differ from v1")
    commit = _git_oid(source["commit"], label="static probe source commit")
    tree = _git_oid(source["tree"], label="static probe source tree")
    clean = not _boolean(source["dirty"], label="static probe source dirty")

    rows = _object_rows(evidence["probes"], label="static probes")
    by_id: dict[str, dict[str, Any]] = {}
    for row in rows:
        required = {
            "elapsed_s",
            "kind",
            "output_bytes",
            "output_sha256",
            "probe_id",
            "returncode",
            "selector",
            "subject_sha256",
        }
        if set(row) != required:
            raise QualificationError("static probe row fields differ from v1")
        probe_id = _text(row["probe_id"], label="static probe id")
        if probe_id in by_id:
            raise QualificationError(f"duplicate static probe: {probe_id}")
        by_id[probe_id] = row
    if set(by_id) != set(PROBE_CONTRACTS):
        raise QualificationError("static probes differ from the frozen I9 set")

    normalized_rows: list[dict[str, Any]] = []
    checks: dict[str, bool] = {}
    for probe_id in sorted(PROBE_CONTRACTS):
        row = by_id[probe_id]
        expected_kind, expected_selector = PROBE_CONTRACTS[probe_id]
        if row["kind"] != expected_kind or row["selector"] != expected_selector:
            raise QualificationError(
                f"static probe {probe_id} selected another test"
            )
        output_bytes = _integer(
            row["output_bytes"],
            label=f"{probe_id} output bytes",
            minimum=0,
            maximum=MAX_JSON_BYTES,
        )
        returncode = _integer(
            row["returncode"],
            label=f"{probe_id} return code",
            minimum=-255,
            maximum=255,
        )
        elapsed = _number(
            row["elapsed_s"],
            label=f"{probe_id} elapsed",
            minimum=0.0,
            maximum=600.0,
        )
        output_digest = _sha256(
            row["output_sha256"], label=f"{probe_id} output sha256"
        )
        subject_digest = _sha256(
            row["subject_sha256"], label=f"{probe_id} subject sha256"
        )
        checks[probe_id] = bool(clean and returncode == 0 and output_bytes > 0)
        normalized_rows.append(
            {
                "elapsed_s": elapsed,
                "kind": expected_kind,
                "output_bytes": output_bytes,
                "output_sha256": output_digest,
                "probe_id": probe_id,
                "returncode": returncode,
                "selector": expected_selector,
                "subject_sha256": subject_digest,
            }
        )
    return {
        "probes": normalized_rows,
        "source": {"commit": commit, "dirty": not clean, "tree": tree},
    }, checks


def validate_matrix_evidence(
    value: Any,
) -> tuple[dict[str, Any], bool, dict[str, bool]]:
    evidence = _object(value, label="matrix evidence")
    if set(evidence) != {"clock", "schema_version", "static_probes"}:
        raise QualificationError("matrix evidence fields differ from the v1 schema")
    if evidence["schema_version"] != MATRIX_EVIDENCE_SCHEMA:
        raise QualificationError("matrix evidence schema is not I9 v1")
    clock, clock_passed = validate_clock_evidence(evidence["clock"])
    probes, probe_checks = validate_static_probes(evidence["static_probes"])
    return {"clock": clock, "static_probes": probes}, clock_passed, probe_checks


def validate_recovery_evidence(
    row: Mapping[str, Any],
) -> tuple[dict[str, Any], dict[str, bool]]:
    details = _object(row.get("details"), label="FoundationPose row details")
    recovery = _object(details.get("recovery"), label="FoundationPose recovery")
    required = {
        "component_id",
        "dds",
        "exact_world_after_rejoin",
        "output_sample",
        "passed",
        "provider",
        "schema_version",
        "simulation_stopped_after_rejoin",
        "simulation_stopped_during_outage",
    }
    if set(recovery) != required:
        raise QualificationError("FoundationPose recovery fields differ from v1")
    if recovery["schema_version"] != RECOVERY_EVIDENCE_SCHEMA:
        raise QualificationError("FoundationPose recovery schema is not I9 v1")
    expected_component = SCENARIO_BY_ID[
        "isaac-lite6-trocar-foundationpose"
    ].component_id
    if recovery["component_id"] != expected_component:
        raise QualificationError("FoundationPose recovery crossed components")

    dds = _object(recovery["dds"], label="FoundationPose recovery DDS")
    if set(dds) != {"loss_elapsed_s", "rejoin_elapsed_s"}:
        raise QualificationError("FoundationPose DDS evidence differs from v1")
    dds_loss = _number(
        dds["loss_elapsed_s"],
        label="DDS loss elapsed",
        minimum=0.0,
        maximum=RECOVERY_BOUND_S,
    )
    dds_rejoin = _number(
        dds["rejoin_elapsed_s"],
        label="DDS rejoin elapsed",
        minimum=0.0,
        maximum=RECOVERY_BOUND_S,
    )

    provider = _object(
        recovery["provider"], label="FoundationPose provider recovery"
    )
    provider_fields = {
        "epoch_after",
        "epoch_before",
        "loss_elapsed_s",
        "recovery_elapsed_s",
        "sequence_after",
        "sequence_before",
        "source_id",
    }
    if set(provider) != provider_fields:
        raise QualificationError("FoundationPose provider fields differ from v1")
    if provider["source_id"] != "foundationpose_trocar":
        raise QualificationError("FoundationPose recovery selected another source")
    epoch_before = _text(provider["epoch_before"], label="provider epoch before")
    epoch_after = _text(provider["epoch_after"], label="provider epoch after")
    sequence_before = _integer(
        provider["sequence_before"],
        label="provider sequence before",
        minimum=1,
    )
    sequence_after = _integer(
        provider["sequence_after"],
        label="provider sequence after",
        minimum=1,
    )
    provider_loss = _number(
        provider["loss_elapsed_s"],
        label="provider loss elapsed",
        minimum=0.0,
        maximum=RECOVERY_BOUND_S,
    )
    provider_recovery = _number(
        provider["recovery_elapsed_s"],
        label="provider recovery elapsed",
        minimum=0.0,
        maximum=RECOVERY_BOUND_S,
    )

    output = _object(recovery["output_sample"], label="recovery output sample")
    if set(output) != {"bytes", "returncode", "seen", "sha256"}:
        raise QualificationError("recovery output sample fields differ from v1")
    output_bytes = _integer(
        output["bytes"],
        label="recovery output bytes",
        minimum=0,
        maximum=MAX_JSON_BYTES,
    )
    output_returncode = _integer(
        output["returncode"],
        label="recovery output return code",
        minimum=-255,
        maximum=255,
    )
    output_seen = _boolean(output["seen"], label="recovery output seen")
    output_digest = _sha256(
        output["sha256"], label="recovery output sha256"
    )
    exact_world = _boolean(
        recovery["exact_world_after_rejoin"],
        label="exact world after rejoin",
    )
    stopped_during = _boolean(
        recovery["simulation_stopped_during_outage"],
        label="simulation stopped during outage",
    )
    stopped_after = _boolean(
        recovery["simulation_stopped_after_rejoin"],
        label="simulation stopped after rejoin",
    )
    reported_pass = _boolean(recovery["passed"], label="recovery reported pass")
    dds_passed = stopped_during and stopped_after
    provider_passed = bool(
        epoch_before != epoch_after
        and output_seen
        and output_returncode == 0
        and output_bytes > 0
        and exact_world
        and stopped_during
        and stopped_after
    )
    derived_pass = dds_passed and provider_passed
    if reported_pass != derived_pass:
        raise QualificationError(
            "FoundationPose recovery pass field differs from derived evidence"
        )
    normalized = {
        "component_id": expected_component,
        "dds": {
            "loss_elapsed_s": dds_loss,
            "rejoin_elapsed_s": dds_rejoin,
        },
        "exact_world_after_rejoin": exact_world,
        "output_sample": {
            "bytes": output_bytes,
            "returncode": output_returncode,
            "seen": output_seen,
            "sha256": output_digest,
        },
        "provider": {
            "epoch_after": epoch_after,
            "epoch_before": epoch_before,
            "loss_elapsed_s": provider_loss,
            "recovery_elapsed_s": provider_recovery,
            "sequence_after": sequence_after,
            "sequence_before": sequence_before,
            "source_id": "foundationpose_trocar",
        },
        "simulation_stopped_after_rejoin": stopped_after,
        "simulation_stopped_during_outage": stopped_during,
    }
    return normalized, {
        "dds_loss_rejoin_bounded": dds_passed,
        "provider_loss_recovery_bounded": provider_passed,
    }


def reduce_matrix(
    rows: Sequence[Any],
    matrix_evidence: Mapping[str, Any],
    *,
    preflight: Mapping[str, Any],
) -> dict[str, Any]:
    """Derive one verdict from five rows and structured matrix evidence."""

    if len(rows) != len(SCENARIOS):
        raise QualificationError("I9 matrix requires exactly five rows")
    validated = [validate_row(row) for row in rows]
    by_id = {row["scenario_id"]: row for row in validated}
    if len(by_id) != len(validated) or set(by_id) != set(SCENARIO_BY_ID):
        raise QualificationError("I9 matrix rows are missing, duplicate, or extra")
    if preflight.get("schema_version") != PREFLIGHT_SCHEMA:
        raise QualificationError("I9 matrix preflight is absent or incompatible")
    ordered = [by_id[spec.scenario_id] for spec in SCENARIOS]
    evidence, clock_passed, probe_checks = validate_matrix_evidence(
        matrix_evidence
    )
    foundation_row = by_id["isaac-lite6-trocar-foundationpose"]
    recovery, recovery_checks = validate_recovery_evidence(foundation_row)
    collision_checks = by_id["isaac-ur10e-light-scene-collision"]["checks"]
    namespace_checks = all(
        row["checks"]["runtime_namespace_exact"]
        and row["checks"]["execution_owner_exact"]
        for row in ordered
    )
    exact_world_checks = all(
        row["checks"]["exact_world_readback"] for row in ordered
    )
    same_type_isolated = probe_checks["two_same_type_instances_isolated"]
    matrix_checks = {
        "clock_synchronized": clock_passed,
        "dds_loss_rejoin_bounded": recovery_checks[
            "dds_loss_rejoin_bounded"
        ],
        "demo_supervisor_absent": probe_checks["demo_supervisor_absent"],
        "direct_controller_route": probe_checks["direct_controller_route"],
        "no_cross_drive": namespace_checks and same_type_isolated,
        "provider_loss_recovery_bounded": recovery_checks[
            "provider_loss_recovery_bounded"
        ],
        "representative_chained_controller_route": probe_checks[
            "representative_chained_controller_route"
        ],
        "scene_reload_exact": bool(
            exact_world_checks and recovery["exact_world_after_rejoin"]
        ),
        "structured_alarm_correlation": bool(
            collision_checks["collision_stop_kernel_owned"]
            and collision_checks["collision_stop_terminal"]
            and collision_checks["status_trace_correlated"]
        ),
        "two_same_type_instances_isolated": same_type_isolated,
    }
    if set(matrix_checks) != MATRIX_CHECKS:
        raise QualificationError("derived matrix checks differ from the frozen set")
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
        "evidence": {**evidence, "recovery": recovery},
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


def build_matrix_evidence(
    *,
    clock: Mapping[str, Any],
    static_probes: Mapping[str, Any],
) -> dict[str, Any]:
    evidence = {
        "schema_version": MATRIX_EVIDENCE_SCHEMA,
        "clock": dict(clock),
        "static_probes": dict(static_probes),
    }
    validate_matrix_evidence(evidence)
    return evidence


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


def _boolean(value: Any, *, label: str) -> bool:
    if type(value) is not bool:
        raise QualificationError(f"{label} must be boolean")
    return value


def _integer(
    value: Any,
    *,
    label: str,
    minimum: int,
    maximum: int = 9_007_199_254_740_991_999,
) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise QualificationError(
            f"{label} must be an integer in [{minimum}, {maximum}]"
        )
    return value


def _number(
    value: Any,
    *,
    label: str,
    minimum: float,
    maximum: float,
) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise QualificationError(f"{label} must be numeric")
    number = float(value)
    if not math.isfinite(number) or not minimum <= number <= maximum:
        raise QualificationError(
            f"{label} must be finite and in [{minimum}, {maximum}]"
        )
    return number


def _sha256(value: Any, *, label: str) -> str:
    digest = _text(value, label=label)
    if SHA256_PATTERN.fullmatch(digest) is None:
        raise QualificationError(f"{label} must be one lowercase SHA-256")
    return digest


def _git_oid(value: Any, *, label: str) -> str:
    oid = _text(value, label=label)
    if re.fullmatch(r"[0-9a-f]{40,64}", oid) is None:
        raise QualificationError(f"{label} must be one lowercase Git object ID")
    return oid


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

    evidence_parser = subparsers.add_parser("matrix-evidence")
    evidence_parser.add_argument("--clock", type=Path, required=True)
    evidence_parser.add_argument("--static-probes", type=Path, required=True)

    reduce_parser = subparsers.add_parser("reduce")
    reduce_parser.add_argument("--preflight", type=Path, required=True)
    reduce_parser.add_argument("--matrix-evidence", type=Path, required=True)
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
        elif args.command == "matrix-evidence":
            _emit(
                build_matrix_evidence(
                    clock=_object(_load_json(args.clock), label="clock evidence"),
                    static_probes=_object(
                        _load_json(args.static_probes),
                        label="static probe evidence",
                    ),
                )
            )
        else:
            result = reduce_matrix(
                [_load_json(path) for path in args.rows],
                _object(
                    _load_json(args.matrix_evidence), label="matrix evidence"
                ),
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
