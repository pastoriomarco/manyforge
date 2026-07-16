#!/usr/bin/env python3
"""Validate and inventory the RoboPlan 0.5 transition benchmark inputs.

The tool is deliberately standard-library only so the same serialized corpus
can be inspected in the native and ROS/Isaac containers before and after the
dependency repin. Large raw samples remain qualification artifacts; this tool
emits the small, reviewable manifest that identifies their exact source.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import math
import os
import platform
import re
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


CORPUS_SCHEMA = "manyforge.roboplan_transition_corpus.v1"
CAPTURE_SCHEMA = "manyforge.roboplan_transition_capture.v1"
EVIDENCE_SCHEMA = "manyforge.roboplan_transition_evidence.v1"
CAPABILITY_SCHEMA = "manyforge.roboplan_capability_inventory.v1"
WORKAROUND_SCHEMA = "manyforge.roboplan_workaround_inventory.v1"
ROBOPLAN_05_VERSION = "0.5.1"
ROBOPLAN_05_SCHEMA_VERSIONS = frozenset({"0.5.0", ROBOPLAN_05_VERSION})
ROBOPLAN_05_PINNED_COMMIT = "e06d1a2f1b749c38742aee9e401bbe97fa967f7b"
ROBOPLAN_05_PINOCCHIO_VERSION = "4.1.0"
ROBOPLAN_05_COAL_VERSION = "3.0.3"
ROBOPLAN_05_PACKAGE_VERSIONS = {
    "roboplan": ROBOPLAN_05_VERSION,
    "roboplan_cartesian_planning": ROBOPLAN_05_VERSION,
    "roboplan_example_models": ROBOPLAN_05_VERSION,
    "roboplan_examples": ROBOPLAN_05_VERSION,
    "roboplan_oink": ROBOPLAN_05_VERSION,
    "roboplan_rrt": ROBOPLAN_05_VERSION,
    "roboplan_simple_ik": ROBOPLAN_05_VERSION,
    "roboplan_toppra": ROBOPLAN_05_VERSION,
}
ROBOPLAN_05_PATCH_MANIFEST_SHA256 = (
    "06d0c235d7cba107468d89f642f76b3459ff9d0c4884fbfbbcdc8f40e60504ed"
)
ROBOPLAN_05_PATCH_DIGESTS = {
    "patches/0001-explicit-isolated-scene-clone.patch": (
        "cfa676dca94d8a8e9d4f61fb76b4988dc0810a5484b4d3a42416b5d294363344"
    ),
    "patches/0002-cartesian-planner-cooperative-cancellation.patch": (
        "344123b71a6eb2bcf8217ca7aea77413f68e70c5e96a397f6deb856c0cf8c9bf"
    ),
    "patches/0003-own-collision-context-scene-generation.patch": (
        "0e83a7b644bf033e9d00d3ec1f638f8126593602c04416ab9fc70e98d186c81c"
    ),
    "patches/0004-configurable-source-build-cmake-arguments.patch": (
        "8867a87ea695b4196e9d05b424c1a4be0219fe9017ab21fa8e89aeb7b89ac086"
    ),
    "patches/0005-construct-scene-scratch-without-copy-assignment.patch": (
        "e13cdc712cd98c411f8f1a0dbc2ee9944fed536745fc92ddf64e60c41ed2b537"
    ),
    "patches/0006-bound-cartesian-duration-arithmetic.patch": (
        "c0b699d5f2eb28edda43290710b328aca6837cee2290ec5b3a339b1d1051a581"
    ),
    "patches/0007-explicit-serialized-collision-query-view.patch": (
        "d14a8eb7b2f99dfc37a59475e0add3514fe1d96d78a818ca4c7ce658cbc2e14d"
    ),
}
ROBOPLAN_PACKAGES = (
    "roboplan",
    "roboplan_rrt",
    "roboplan_toppra",
    "roboplan_simple_ik",
    "roboplan_oink",
    "roboplan_cartesian_planning",
)
ROBOPLAN_INVENTORY_FILES = {
    "0.4.0": (
        "roboplan-0.4.0-capabilities.json",
        "roboplan-0.4.0-workarounds.json",
    ),
    ROBOPLAN_05_VERSION: (
        "roboplan-0.5.1-capabilities.json",
        "roboplan-0.5.1-workarounds.json",
    ),
}
SCAN_SUFFIXES = {
    ".cmake",
    ".cpp",
    ".hpp",
    ".md",
    ".py",
    ".sh",
    ".txt",
    ".yaml",
    ".yml",
}
EXCLUDED_PARTS = {
    ".docker",
    ".git",
    ".pytest_cache",
    "__pycache__",
    "build",
    "install",
    "log",
    "node_modules",
    "qualification",
}


class ValidationError(ValueError):
    """Raised when a checked-in transition artifact is internally invalid."""


def cycle_failure_reason(status: dict[str, Any]) -> str | None:
    """Return a stable failure reason for every Composer fault parking state."""

    state = str(status.get("state") or "")
    error_code = str(status.get("errorCode") or "")
    error = str(status.get("error") or "")
    if state in {"failed", "intervention_required"}:
        return error_code or state
    # Runtime-domain faults deliberately hold a behavior node RUNNING while the
    # cycle carries a recovery offer; motion faults often park on Stopped. Both
    # are terminal for an unattended qualification run even though neither uses
    # the kernel's Failed state name.
    if error_code or error:
        return error_code or error
    return None


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"{path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValidationError(f"{path}: top-level JSON value must be an object")
    return value


def write_evidence_exclusive(
    path: Path,
    evidence: dict[str, Any],
    *,
    validator: Any | None = None,
) -> None:
    """Validate and durably publish evidence without replacing another run."""

    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(evidence, indent=2, sort_keys=True) + "\n"
    temporary = path.with_name(
        f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp"
    )
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        if validator is not None:
            validator(temporary)
        # link() is an atomic no-replace publication on the same filesystem.
        os.link(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        temporary.unlink(missing_ok=True)


def _require_nonempty_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValidationError(f"{field} must be a non-empty string")
    return value


def _validate_unique_ids(rows: Any, field: str) -> set[str]:
    if not isinstance(rows, list) or not rows:
        raise ValidationError(f"{field} must be a non-empty list")
    result: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValidationError(f"{field}[{index}] must be an object")
        row_id = _require_nonempty_string(row.get("id"), f"{field}[{index}].id")
        if row_id in result:
            raise ValidationError(f"{field} contains duplicate id {row_id!r}")
        result.add(row_id)
    return result


def _finite_vector(value: Any, size: int, field: str) -> list[float]:
    if not isinstance(value, list) or len(value) != size:
        raise ValidationError(f"{field} must contain exactly {size} numbers")
    try:
        result = [float(item) for item in value]
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"{field} must contain numbers") from exc
    if not all(math.isfinite(item) for item in result):
        raise ValidationError(f"{field} must contain finite numbers")
    return result


def _finite_scalar(value: Any, field: str, *, positive: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError(f"{field} must be a number")
    result = float(value)
    if not math.isfinite(result) or (positive and result <= 0.0):
        qualifier = "finite and positive" if positive else "finite"
        raise ValidationError(f"{field} must be {qualifier}")
    return result


def _validate_pose(value: Any, field: str) -> None:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    _finite_vector(value.get("position_m"), 3, f"{field}.position_m")
    orientation = _finite_vector(
        value.get("orientation_xyzw"), 4, f"{field}.orientation_xyzw"
    )
    norm = math.sqrt(sum(item * item for item in orientation))
    if abs(norm - 1.0) > 1e-6:
        raise ValidationError(f"{field}.orientation_xyzw must be normalized")


def _validate_collision_pair(value: Any, field: str) -> None:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    for name in ("body_a", "body_b"):
        _require_nonempty_string(value.get(name), f"{field}.{name}")
    if value["body_a"] == value["body_b"]:
        raise ValidationError(f"{field} must identify two different bodies")


def _validate_collision_policy(value: Any, field: str) -> None:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    _require_nonempty_string(value.get("policy_id"), f"{field}.policy_id")
    revision = value.get("revision")
    if not isinstance(revision, dict):
        raise ValidationError(f"{field}.revision must be an object")
    revision_id = revision.get("revision_id")
    if (
        isinstance(revision_id, bool)
        or not isinstance(revision_id, int)
        or revision_id <= 0
    ):
        raise ValidationError(f"{field}.revision.revision_id must be positive")
    _require_nonempty_string(
        revision.get("content_hash"), f"{field}.revision.content_hash"
    )
    if not isinstance(value.get("require_full_diagnostics"), bool):
        raise ValidationError(f"{field}.require_full_diagnostics must be boolean")
    for pair_list in ("allowed_static_static_pairs", "allowed_touch_link_pairs"):
        pairs = value.get(pair_list)
        if not isinstance(pairs, list):
            raise ValidationError(f"{field}.{pair_list} must be a list")
        for index, pair in enumerate(pairs):
            _validate_collision_pair(pair, f"{field}.{pair_list}[{index}]")
    support_contacts = value.get("support_contacts")
    if not isinstance(support_contacts, list):
        raise ValidationError(f"{field}.support_contacts must be a list")
    for index, contact in enumerate(support_contacts):
        item = f"{field}.support_contacts[{index}]"
        if not isinstance(contact, dict):
            raise ValidationError(f"{item} must be an object")
        _validate_collision_pair(contact.get("pair"), f"{item}.pair")
        if contact.get("mode") not in {"contact_release", "contact_process"}:
            raise ValidationError(f"{item}.mode is unsupported")
        for name in (
            "max_start_penetration_m",
            "release_tolerance_m",
            "min_release_progress_m",
            "min_separation_clearance_m",
        ):
            numeric = _finite_scalar(contact.get(name), f"{item}.{name}")
            if numeric < 0.0:
                raise ValidationError(f"{item}.{name} must be non-negative")
        for name in (
            "enforce_max_start_penetration",
            "require_separation_by_end",
            "require_strictly_decreasing_penetration",
            "allow_recontact",
        ):
            if not isinstance(contact.get(name), bool):
                raise ValidationError(f"{item}.{name} must be boolean")
    contact_grasps = value.get("contact_grasps")
    if not isinstance(contact_grasps, list):
        raise ValidationError(f"{field}.contact_grasps must be a list")
    for index, grasp in enumerate(contact_grasps):
        item = f"{field}.contact_grasps[{index}]"
        if not isinstance(grasp, dict):
            raise ValidationError(f"{item} must be an object")
        for name in ("policy_id", "selected_object_id"):
            _require_nonempty_string(grasp.get(name), f"{item}.{name}")
        links = grasp.get("allowed_robot_links")
        if (
            not isinstance(links, list)
            or not links
            or any(not isinstance(link, str) or not link for link in links)
        ):
            raise ValidationError(
                f"{item}.allowed_robot_links must contain non-empty names"
            )
        penetration = _finite_scalar(
            grasp.get("max_penetration_m"), f"{item}.max_penetration_m"
        )
        if penetration < 0.0:
            raise ValidationError(f"{item}.max_penetration_m must be non-negative")
        fraction = _finite_scalar(
            grasp.get("terminal_window_start_fraction"),
            f"{item}.terminal_window_start_fraction",
        )
        if not 0.0 <= fraction <= 1.0:
            raise ValidationError(
                f"{item}.terminal_window_start_fraction must be in [0, 1]"
            )


def validate_capture(
    capture_path: Path,
    *,
    expected_capture_id: str | None = None,
    expected_joint_names: list[str] | None = None,
    required_attachment: str | None = None,
) -> dict[str, Any]:
    capture = _read_json(capture_path)
    if capture.get("schema_version") != CAPTURE_SCHEMA:
        raise ValidationError(
            f"{capture_path}: schema_version must be {CAPTURE_SCHEMA!r}"
        )
    capture_id = _require_nonempty_string(capture.get("capture_id"), "capture_id")
    if expected_capture_id is not None and capture_id != expected_capture_id:
        raise ValidationError(
            f"capture_id {capture_id!r} does not match {expected_capture_id!r}"
        )

    source = capture.get("source")
    if not isinstance(source, dict):
        raise ValidationError("source must be an object")
    for field in (
        "program",
        "deployment",
        "program_run_id",
        "predecessor_node",
        "move_id",
    ):
        _require_nonempty_string(source.get(field), f"source.{field}")
    if source["move_id"] != "drop_approach":
        raise ValidationError("source.move_id must be 'drop_approach'")
    if source["predecessor_node"] != "pick_retreat":
        raise ValidationError("source.predecessor_node must be 'pick_retreat'")
    planning_execution_id = source.get("planning_execution_id")
    if (
        isinstance(planning_execution_id, bool)
        or not isinstance(planning_execution_id, int)
        or planning_execution_id <= 0
    ):
        raise ValidationError("source.planning_execution_id must be a positive integer")
    for field in ("program_sha256", "deployment_sha256"):
        value = _require_nonempty_string(source.get(field), f"source.{field}")
        if re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise ValidationError(f"source.{field} must be a SHA-256 digest")
    diagnostics = source.get("visual_state_diagnostics")
    if not isinstance(diagnostics, dict) or diagnostics.get(
        "served_from_visual_snapshot"
    ) is not True or diagnostics.get("snapshot_stale") is True:
        raise ValidationError("capture source must identify a fresh visual snapshot")

    authority = capture.get("robot_model_authority")
    if not isinstance(authority, dict):
        raise ValidationError("robot_model_authority must be an object")
    for field in ("canonical_fingerprint", "view_fingerprint"):
        value = _require_nonempty_string(
            authority.get(field), f"robot_model_authority.{field}"
        )
        if re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise ValidationError(
                f"robot_model_authority.{field} must be a SHA-256 digest"
            )

    start = capture.get("start_state")
    if not isinstance(start, dict):
        raise ValidationError("start_state must be an object")
    joint_names = start.get("joint_names")
    positions = start.get("positions_rad")
    if (
        not isinstance(joint_names, list)
        or not joint_names
        or any(not isinstance(name, str) or not name for name in joint_names)
        or len(set(joint_names)) != len(joint_names)
    ):
        raise ValidationError("start_state.joint_names must be unique names")
    _finite_vector(positions, len(joint_names), "start_state.positions_rad")
    if expected_joint_names is not None and joint_names != expected_joint_names:
        raise ValidationError("capture joint order does not match corpus model authority")
    if start.get("state_source") != "exact_plan_request":
        raise ValidationError("start_state.state_source must be exact_plan_request")
    velocities = start.get("velocities_rad_s")
    parsed_velocities: list[float] = []
    if not isinstance(velocities, list) or len(velocities) not in {0, len(joint_names)}:
        raise ValidationError(
            "start_state.velocities_rad_s must be empty or match joint order"
        )
    if velocities:
        parsed_velocities = _finite_vector(
            velocities, len(joint_names), "start_state.velocities_rad_s"
        )
    provenance = start.get("provenance")
    if not isinstance(provenance, dict):
        raise ValidationError("start_state.provenance must be an object")
    expected_provenance = {
        "execution_state_source_mode": "commanded_sim",
        "capture_method": "planner_qualification_observer",
        "predecessor_move_id": "pick_retreat",
    }
    for field, expected in expected_provenance.items():
        if provenance.get(field) != expected:
            raise ValidationError(
                f"start_state.provenance.{field} must be {expected!r}"
            )
    if len(velocities) != len(joint_names) or any(
        value != 0.0 for value in parsed_velocities
    ):
        raise ValidationError(
            "commanded_sim capture start velocities must match the joint "
            "order and be zero"
        )
    commandable = authority.get("commandable_joint_names")
    if isinstance(commandable, list) and not set(joint_names).issubset(commandable):
        raise ValidationError(
            "capture start joints are not public Robot Model Authority names"
        )

    resolved = capture.get("resolved_motion")
    if not isinstance(resolved, dict) or resolved.get("type") != "pose_goal":
        raise ValidationError("resolved_motion must be a pose_goal")
    _validate_pose(resolved.get("pose_goal"), "resolved_motion.pose_goal")
    resolved_source = resolved.get("source")
    if not isinstance(resolved_source, dict):
        raise ValidationError("resolved_motion.source must be an object")
    for field in ("target_reference", "offset_reference"):
        reference = resolved_source.get(field)
        if not isinstance(reference, dict):
            raise ValidationError(f"resolved_motion.source.{field} must be an object")
        if reference.get("type") not in {"object", "frame", "robot_link"}:
            raise ValidationError(
                f"resolved_motion.source.{field}.type is unsupported"
            )
        _require_nonempty_string(
            reference.get("id"), f"resolved_motion.source.{field}.id"
        )
    _validate_pose(
        resolved_source.get("offset_pose"),
        "resolved_motion.source.offset_pose",
    )

    plan_request = capture.get("plan_request")
    if not isinstance(plan_request, dict):
        raise ValidationError("plan_request must be an object")
    for field in ("robot_id", "group_id", "move_id", "base_frame", "tcp_frame"):
        _require_nonempty_string(plan_request.get(field), f"plan_request.{field}")
    if plan_request["move_id"] != source["move_id"]:
        raise ValidationError("plan_request.move_id does not match source.move_id")
    request_start = plan_request.get("start_state")
    if not isinstance(request_start, dict):
        raise ValidationError("plan_request.start_state must be an object")
    if request_start.get("joint_names") != joint_names:
        raise ValidationError("plan_request start joint order does not match capture")
    request_positions = _finite_vector(
        request_start.get("positions_rad"),
        len(joint_names),
        "plan_request.start_state.positions_rad",
    )
    if request_positions != [float(value) for value in positions]:
        raise ValidationError("plan_request start positions do not match capture")
    request_velocities = _finite_vector(
        request_start.get("velocities_rad_s"),
        len(joint_names),
        "plan_request.start_state.velocities_rad_s",
    )
    if request_velocities != parsed_velocities:
        raise ValidationError("plan_request start velocities do not match capture")
    request_motion = plan_request.get("motion")
    if not isinstance(request_motion, dict) or request_motion.get("type") != "pose_goal":
        raise ValidationError("plan_request.motion must be a pose_goal")
    _validate_pose(request_motion.get("pose"), "plan_request.motion.pose")
    if request_motion["pose"] != resolved["pose_goal"]:
        raise ValidationError("plan_request pose does not match resolved_motion")
    tolerances = plan_request.get("tolerances")
    if not isinstance(tolerances, dict):
        raise ValidationError("plan_request.tolerances must be an object")
    for field in (
        "joint_position_tol_rad",
        "translation_tol_m",
        "rotation_tol_rad",
    ):
        value = _finite_scalar(tolerances.get(field), f"plan_request.tolerances.{field}")
        if value < 0.0:
            raise ValidationError(f"plan_request.tolerances.{field} must be non-negative")
    speed = plan_request.get("speed_profile")
    if not isinstance(speed, dict):
        raise ValidationError("plan_request.speed_profile must be an object")
    for field in ("velocity_scale", "acceleration_scale"):
        value = _finite_scalar(
            speed.get(field), f"plan_request.speed_profile.{field}", positive=True
        )
        if value > 1.0:
            raise ValidationError(f"plan_request.speed_profile.{field} must be <= 1")
    tcp_cap = speed.get("max_tcp_linear_speed_mps")
    if tcp_cap is not None:
        _finite_scalar(
            tcp_cap,
            "plan_request.speed_profile.max_tcp_linear_speed_mps",
            positive=True,
        )
    _finite_scalar(
        plan_request.get("planning_timeout_s"),
        "plan_request.planning_timeout_s",
        positive=True,
    )
    attempt = plan_request.get("planning_attempt_index")
    if isinstance(attempt, bool) or not isinstance(attempt, int) or attempt != 0:
        raise ValidationError("plan_request.planning_attempt_index must be zero")
    if not isinstance(plan_request.get("per_joint_dynamics_limits"), dict):
        raise ValidationError("plan_request.per_joint_dynamics_limits must be an object")
    if not isinstance(plan_request.get("contact_policy_id"), str):
        raise ValidationError("plan_request.contact_policy_id must be a string")
    scene_binding = plan_request.get("scene_binding")
    if not isinstance(scene_binding, dict):
        raise ValidationError("plan_request.scene_binding must be an object")
    binding_revision_id = scene_binding.get("revision_id")
    if (
        isinstance(binding_revision_id, bool)
        or not isinstance(binding_revision_id, int)
        or binding_revision_id <= 0
    ):
        raise ValidationError("plan_request.scene_binding.revision_id must be positive")
    _require_nonempty_string(
        scene_binding.get("content_hash"),
        "plan_request.scene_binding.content_hash",
    )
    _require_nonempty_string(
        scene_binding.get("projection_lease_id"),
        "plan_request.scene_binding.projection_lease_id",
    )
    planner_contract = capture.get("planner_contract")
    if not isinstance(planner_contract, dict):
        raise ValidationError("planner_contract must be an object")
    for field in ("rng_base_seed", "rng_seed", "planning_attempt_index"):
        value = planner_contract.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValidationError(f"planner_contract.{field} must be non-negative")
    shortcut_seed = planner_contract.get("shortcut_rng_seed_argument")
    if isinstance(shortcut_seed, bool) or not isinstance(shortcut_seed, int):
        raise ValidationError(
            "planner_contract.shortcut_rng_seed_argument must be an integer"
        )
    if planner_contract["planning_attempt_index"] != attempt:
        raise ValidationError("planner contract attempt does not match request")
    if planner_contract["rng_seed"] != planner_contract["rng_base_seed"]:
        raise ValidationError("initial planning attempt must use the base RNG seed")

    scene = capture.get("scene")
    if not isinstance(scene, dict):
        raise ValidationError("scene must be an object")
    _require_nonempty_string(scene.get("world_frame"), "scene.world_frame")
    scene_authority = scene.get("authority")
    if not isinstance(scene_authority, dict):
        raise ValidationError("scene.authority must be an object")
    authority_revision = scene_authority.get("revision")
    if not isinstance(authority_revision, dict):
        raise ValidationError("scene.authority.revision must be an object")
    if authority_revision != {
        "revision_id": scene_binding["revision_id"],
        "content_hash": scene_binding["content_hash"],
    }:
        raise ValidationError("scene authority revision does not match plan binding")
    for field in (
        "transaction_sequence",
        "scene_admission_epoch",
        "topology_epoch",
        "policy_epoch",
    ):
        value = scene_authority.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValidationError(f"scene.authority.{field} must be non-negative")
    _validate_collision_policy(
        scene_authority.get("configured_collision_policy"),
        "scene.authority.configured_collision_policy",
    )
    robot = scene.get("robot")
    if not isinstance(robot, dict):
        raise ValidationError("scene.robot must be an object")
    for field in ("robot_id", "base_frame"):
        _require_nonempty_string(robot.get(field), f"scene.robot.{field}")
    if robot["robot_id"] != plan_request["robot_id"]:
        raise ValidationError("scene robot does not match plan request")
    if robot["base_frame"] != plan_request["base_frame"]:
        raise ValidationError("scene robot base frame does not match plan request")
    _validate_pose(robot.get("pose_in_world"), "scene.robot.pose_in_world")

    objects = scene.get("objects")
    if not isinstance(objects, list) or not objects:
        raise ValidationError("scene.objects must be a non-empty list")
    object_ids: set[str] = set()
    attached_ids: set[str] = set()
    for index, obj in enumerate(objects):
        field = f"scene.objects[{index}]"
        if not isinstance(obj, dict):
            raise ValidationError(f"{field} must be an object")
        object_id = _require_nonempty_string(obj.get("object_id"), f"{field}.object_id")
        if object_id in object_ids:
            raise ValidationError(f"scene.objects contains duplicate id {object_id!r}")
        object_ids.add(object_id)
        _validate_pose(obj.get("pose_in_world"), f"{field}.pose_in_world")
        shape = obj.get("shape")
        if not isinstance(shape, dict):
            raise ValidationError(f"{field}.shape must be an object")
        shape_type = shape.get("type")
        if shape_type == "mesh":
            _require_nonempty_string(
                shape.get("resource_uri"), f"{field}.shape.resource_uri"
            )
            _finite_vector(shape.get("mesh_scale"), 3, f"{field}.shape.mesh_scale")
        elif shape_type == "box":
            dimensions = _finite_vector(
                shape.get("dimensions_m"), 3, f"{field}.shape.dimensions_m"
            )
            if any(item <= 0.0 for item in dimensions):
                raise ValidationError(f"{field}.shape dimensions must be positive")
        else:
            raise ValidationError(f"{field}.shape.type is unsupported: {shape_type!r}")
        attached = obj.get("attached")
        if not isinstance(attached, bool):
            raise ValidationError(f"{field}.attached must be boolean")
        if attached:
            attached_ids.add(object_id)
            _require_nonempty_string(obj.get("attached_to"), f"{field}.attached_to")
            touch_links = obj.get("touch_links")
            if not isinstance(touch_links, list) or not touch_links:
                raise ValidationError(f"{field}.touch_links must be non-empty")
        support_contacts = obj.get("support_contacts")
        if not isinstance(support_contacts, list):
            raise ValidationError(f"{field}.support_contacts must be a list")
        for contact_index, contact in enumerate(support_contacts):
            contact_field = f"{field}.support_contacts[{contact_index}]"
            if not isinstance(contact, dict):
                raise ValidationError(f"{contact_field} must be an object")
            support_object_id = _require_nonempty_string(
                contact.get("support_object_id"),
                f"{contact_field}.support_object_id",
            )
            if support_object_id == object_id:
                raise ValidationError(f"{contact_field} cannot reference itself")
            if contact.get("mode", "contact_release") not in {
                "contact_release",
                "contact_process",
            }:
                raise ValidationError(f"{contact_field}.mode is unsupported")
            for numeric_field in (
                "max_start_penetration_m",
                "release_tolerance_m",
                "min_release_progress_m",
                "min_separation_clearance_m",
            ):
                value = contact.get(numeric_field, 0.0)
                if (
                    isinstance(value, bool)
                    or not isinstance(value, (int, float))
                    or not math.isfinite(float(value))
                    or float(value) < 0.0
                ):
                    raise ValidationError(
                        f"{contact_field}.{numeric_field} must be finite and non-negative"
                    )
            for boolean_field in (
                "enforce_max_start_penetration",
                "require_separation_by_end",
                "require_strictly_decreasing_penetration",
                "allow_recontact",
            ):
                if boolean_field in contact and not isinstance(
                    contact[boolean_field], bool
                ):
                    raise ValidationError(
                        f"{contact_field}.{boolean_field} must be boolean"
                    )
    for index, obj in enumerate(objects):
        for contact in obj.get("support_contacts", []):
            if contact["support_object_id"] not in object_ids:
                raise ValidationError(
                    "scene support contact references an unknown object: "
                    f"{contact['support_object_id']!r}"
                )
    if required_attachment is not None and required_attachment not in attached_ids:
        raise ValidationError(
            f"required attached object {required_attachment!r} is not attached"
        )

    semantic = _require_nonempty_string(
        capture.get("semantic_sha256"), "semantic_sha256"
    )
    unsigned = dict(capture)
    unsigned.pop("semantic_sha256", None)
    expected_semantic = hashlib.sha256(
        json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    if semantic != expected_semantic:
        raise ValidationError("semantic_sha256 does not match capture content")
    return {
        "schema_version": CAPTURE_SCHEMA,
        "capture_id": capture_id,
        "objects": len(objects),
        "attached_objects": len(attached_ids),
    }


def validate_corpus(
    corpus_path: Path, *, allow_pending_capture: bool = False
) -> dict[str, Any]:
    corpus = _read_json(corpus_path)
    if corpus.get("schema_version") != CORPUS_SCHEMA:
        raise ValidationError(
            f"{corpus_path}: schema_version must be {CORPUS_SCHEMA!r}"
        )
    defaults = corpus.get("defaults")
    if not isinstance(defaults, dict):
        raise ValidationError("defaults must be an object")
    for field, minimum in (
        ("collision_measured_queries", 10_000),
        ("collision_process_batches", 2),
        ("planning_measured_repetitions", 50),
        ("live_successful_observations", 30),
    ):
        value = defaults.get(field)
        if not isinstance(value, int) or value < minimum:
            raise ValidationError(f"defaults.{field} must be >= {minimum}")

    models = corpus.get("models")
    if not isinstance(models, dict) or not models:
        raise ValidationError("models must be a non-empty object")
    model_ids = set(models)
    for model_id, model in models.items():
        if not isinstance(model, dict):
            raise ValidationError(f"models.{model_id} must be an object")
        for field in ("source", "model_name", "group_name", "base_frame", "tcp_frame"):
            _require_nonempty_string(model.get(field), f"models.{model_id}.{field}")

    collision_ids = _validate_unique_ids(
        corpus.get("collision_fixtures"), "collision_fixtures"
    )
    planning_ids = _validate_unique_ids(
        corpus.get("planning_fixtures"), "planning_fixtures"
    )
    live_ids = _validate_unique_ids(corpus.get("live_cases"), "live_cases")
    revision_ids = _validate_unique_ids(
        corpus.get("revision_fixtures"), "revision_fixtures"
    )

    for section in ("collision_fixtures", "planning_fixtures"):
        for row in corpus[section]:
            model = row.get("model")
            if model not in model_ids:
                raise ValidationError(
                    f"{section}.{row['id']} references unknown model {model!r}"
                )
    for row in corpus["revision_fixtures"]:
        if row.get("base_fixture") not in collision_ids:
            raise ValidationError(
                f"revision fixture {row['id']!r} references unknown collision fixture"
            )
    for row in corpus["planning_fixtures"]:
        capture = row.get("capture_file")
        if capture is None:
            if "capture_source_policy" in row:
                raise ValidationError(
                    f"planning fixture {row['id']!r} has a capture source policy "
                    "without a capture"
                )
            continue
        capture_source_policy = row.get(
            "capture_source_policy", "current_artifacts"
        )
        if capture_source_policy not in {
            "current_artifacts",
            "frozen_rp5_0_manifest",
        }:
            raise ValidationError(
                f"planning fixture {row['id']!r} has unsupported "
                f"capture_source_policy {capture_source_policy!r}"
            )
        capture_root = corpus_path.parent.resolve()
        capture_path = (capture_root / str(capture)).resolve()
        if not capture_path.is_relative_to(capture_root):
            raise ValidationError(
                f"planning fixture {row['id']!r} capture escapes the corpus directory"
            )
        if not capture_path.is_file() and not allow_pending_capture:
            raise ValidationError(
                f"planning fixture {row['id']!r} capture is missing: {capture_path}"
            )
        if capture_path.is_file():
            model = models[row["model"]]
            validate_capture(
                capture_path,
                expected_capture_id=row.get("capture_id"),
                expected_joint_names=model.get("joint_names"),
                required_attachment=row.get("required_attachment"),
            )
    for row in corpus["live_cases"]:
        strata = row.get("viewer_strata")
        if strata != ["disconnected", "focused"]:
            raise ValidationError(
                f"live case {row['id']!r} must keep disconnected/focused strata separate"
            )
        if row.get("successful_observations_per_stratum", 0) < 30:
            raise ValidationError(
                f"live case {row['id']!r} requires at least 30 successes per stratum"
            )

    return {
        "schema_version": CORPUS_SCHEMA,
        "models": len(model_ids),
        "collision_fixtures": len(collision_ids),
        "revision_fixtures": len(revision_ids),
        "planning_fixtures": len(planning_ids),
        "live_cases": len(live_ids),
        "pending_capture_allowed": allow_pending_capture,
    }


def _validate_latency_summary(value: Any, field: str, *, require_p99: bool) -> None:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    ordered: list[float] = []
    for name in ("p50", "p95"):
        ordered.append(_finite_scalar(value.get(name), f"{field}.{name}"))
    p99 = value.get("p99")
    if require_p99:
        ordered.append(_finite_scalar(p99, f"{field}.p99"))
    elif p99 is not None:
        ordered.append(_finite_scalar(p99, f"{field}.p99"))
    ordered.append(_finite_scalar(value.get("max"), f"{field}.max"))
    if any(item < 0.0 for item in ordered):
        raise ValidationError(f"{field} values must be non-negative")
    if ordered != sorted(ordered):
        raise ValidationError(f"{field} percentiles must be monotonic")
    if "mad" in value and value["mad"] is not None:
        mad = _finite_scalar(value["mad"], f"{field}.mad")
        if mad < 0.0:
            raise ValidationError(f"{field}.mad must be non-negative")


def _positive_sample_count(record: dict[str, Any], field: str) -> int:
    value = record.get("sample_count")
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValidationError(f"{field}.sample_count must be a positive integer")
    return value


def _validate_revision_identity(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    revision_id = value.get("revision_id")
    if (
        isinstance(revision_id, bool)
        or not isinstance(revision_id, int)
        or revision_id <= 0
    ):
        raise ValidationError(f"{field}.revision_id must be positive")
    content_hash = _require_nonempty_string(
        value.get("content_hash"), f"{field}.content_hash"
    )
    return {
        "revision_id": revision_id,
        "content_hash": content_hash,
    }


def _nonnegative_integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValidationError(f"{field} must be a non-negative integer")
    return value


def _positive_integer(value: Any, field: str) -> int:
    result = _nonnegative_integer(value, field)
    if result == 0:
        raise ValidationError(f"{field} must be positive")
    return result


def _nonnegative_scalar(value: Any, field: str) -> float:
    result = _finite_scalar(value, field)
    if result < 0.0:
        raise ValidationError(f"{field} must be non-negative")
    return result


def _validate_collision_validation_worker_sample(
    sample: Any,
    field: str,
    *,
    passed: bool,
    require_cold_sync: bool,
) -> int:
    if not isinstance(sample, dict):
        raise ValidationError(f"{field} must be a mapping")
    requested = _positive_integer(
        sample.get("workers_requested"), f"{field}.workers_requested"
    )
    if not isinstance(sample.get("decision_match"), bool):
        raise ValidationError(f"{field}.decision_match must be boolean")
    if passed and sample.get("decision_match") is not True:
        raise ValidationError(f"{field}.decision_match must be true")

    cold_sync = sample.get("cold_snapshot_sync")
    if require_cold_sync and not isinstance(cold_sync, dict):
        raise ValidationError(
            f"{field}.cold_snapshot_sync must be an object for formal evidence"
        )
    if isinstance(cold_sync, dict):
        _nonnegative_scalar(
            cold_sync.get("wall_us"),
            f"{field}.cold_snapshot_sync.wall_us",
        )
        _nonnegative_scalar(
            cold_sync.get("process_cpu_us"),
            f"{field}.cold_snapshot_sync.process_cpu_us",
        )
        _nonnegative_integer(
            cold_sync.get("allocations"),
            f"{field}.cold_snapshot_sync.allocations",
        )
    return requested


def _validate_collision_revision_record(
    record: dict[str, Any], field: str, *, formal: bool
) -> None:
    _validate_latency_summary(
        record.get("latency_ns"), f"{field}.latency_ns", require_p99=formal
    )
    details = record.get("details")
    if not isinstance(details, dict):
        raise ValidationError(f"{field}.details must be an object")
    allocations = _nonnegative_integer(
        details.get("allocations"), f"{field}.details.allocations"
    )
    allocations_per_query = _nonnegative_scalar(
        details.get("allocations_per_query"),
        f"{field}.details.allocations_per_query",
    )
    allocation_bound = _positive_integer(
        details.get("allocation_bound_per_query"),
        f"{field}.details.allocation_bound_per_query",
    )
    wcet_budget_ns = _positive_integer(
        details.get("wcet_budget_ns"), f"{field}.details.wcet_budget_ns"
    )
    if allocations > 0 and allocations_per_query <= 0.0:
        raise ValidationError(
            f"{field}.details.allocations_per_query contradicts allocations"
        )
    if record.get("passed") is True:
        if allocations_per_query > allocation_bound:
            raise ValidationError(
                f"{field}.details.allocations_per_query exceeds its bound"
            )
        if float(record["latency_ns"]["max"]) > wcet_budget_ns:
            raise ValidationError(f"{field}.latency_ns.max exceeds the WCET budget")


def _validate_trajectory_validation_record(
    record: dict[str, Any], field: str, *, formal: bool
) -> None:
    samples = _positive_sample_count(record, field)
    details = record.get("details")
    if not isinstance(details, dict):
        raise ValidationError(f"{field}.details must be an object")
    collision = details.get("collision_validation")
    noncollision = details.get("noncollision_validation")
    if not isinstance(collision, dict) or not isinstance(noncollision, dict):
        raise ValidationError(
            f"{field}.details must retain collision and non-collision validation"
        )
    collision_enabled = collision.get("enabled") is True
    noncollision_enabled = noncollision.get("enabled") is True
    if not isinstance(collision.get("enabled"), bool):
        raise ValidationError(f"{field}.details.collision_validation.enabled must be boolean")
    if not isinstance(noncollision.get("enabled"), bool):
        raise ValidationError(
            f"{field}.details.noncollision_validation.enabled must be boolean"
        )
    if not collision_enabled and not noncollision_enabled:
        raise ValidationError(f"{field} does not contain an enabled validation surface")

    run_counts: list[int] = []
    if collision_enabled:
        collision_field = f"{field}.details.collision_validation"
        run_counts.append(_positive_integer(collision.get("runs"), f"{collision_field}.runs"))
        _positive_integer(collision.get("waypoints"), f"{collision_field}.waypoints")
        workers = _positive_integer(collision.get("workers"), f"{collision_field}.workers")
        if not isinstance(collision.get("decision_match"), bool):
            raise ValidationError(f"{collision_field}.decision_match must be boolean")
        if record.get("passed") is True and collision.get("decision_match") is not True:
            raise ValidationError(f"{collision_field}.decision_match must be true")
        _nonnegative_scalar(
            collision.get("serial_p95_us"), f"{collision_field}.serial_p95_us"
        )
        _nonnegative_scalar(
            collision.get("parallel_p95_us"), f"{collision_field}.parallel_p95_us"
        )
        support_active = collision.get("support_contact_active")
        if not isinstance(support_active, bool):
            raise ValidationError(f"{collision_field}.support_contact_active must be boolean")
        fallback_reason = collision.get("fallback_reason")
        if not isinstance(fallback_reason, str):
            raise ValidationError(f"{collision_field}.fallback_reason must be a string")
        summary_contract_declared = (
            "support_contact_ordered_summary_verified" in details
        )
        summary_verified = details.get(
            "support_contact_ordered_summary_verified"
        )
        if summary_contract_declared and not isinstance(summary_verified, bool):
            raise ValidationError(
                f"{field}.details.support_contact_ordered_summary_verified "
                "must be boolean when the ordered-summary contract is declared"
            )
        if summary_verified is not None and not isinstance(summary_verified, bool):
            raise ValidationError(
                f"{field}.details.support_contact_ordered_summary_verified "
                "must be boolean when present"
            )
        worker_sweep = collision.get("worker_sweep", [])
        if not isinstance(worker_sweep, list):
            raise ValidationError(
                f"{collision_field}.worker_sweep must be a list"
            )
        if summary_contract_declared and not worker_sweep:
            raise ValidationError(
                f"{collision_field}.worker_sweep must be a non-empty list"
            )
        previous_requested = 0
        for sample_index, sample in enumerate(worker_sweep):
            sample_field = f"{collision_field}.worker_sweep[{sample_index}]"
            requested = _validate_collision_validation_worker_sample(
                sample,
                sample_field,
                passed=record.get("passed") is True,
                require_cold_sync=formal,
            )
            if requested <= previous_requested:
                raise ValidationError(
                    f"{collision_field}.worker_sweep must be strictly ordered"
                )
            previous_requested = requested
        waypoint_sweep = collision.get("waypoint_sweep", [])
        if not isinstance(waypoint_sweep, list):
            raise ValidationError(
                f"{collision_field}.waypoint_sweep must be a list"
            )
        previous_waypoint_count = 0
        for point_index, point in enumerate(waypoint_sweep):
            point_field = (
                f"{collision_field}.waypoint_sweep[{point_index}]"
            )
            if not isinstance(point, dict):
                raise ValidationError(f"{point_field} must be a mapping")
            waypoint_count = _positive_integer(
                point.get("waypoints"), f"{point_field}.waypoints"
            )
            if waypoint_count <= previous_waypoint_count:
                raise ValidationError(
                    f"{collision_field}.waypoint_sweep must be strictly ordered"
                )
            previous_waypoint_count = waypoint_count
            point_workers = point.get("worker_sweep")
            if not isinstance(point_workers, list) or not point_workers:
                raise ValidationError(
                    f"{point_field}.worker_sweep must be a non-empty list"
                )
            previous_point_workers = 0
            for sample_index, sample in enumerate(point_workers):
                sample_field = f"{point_field}.worker_sweep[{sample_index}]"
                requested = _validate_collision_validation_worker_sample(
                    sample,
                    sample_field,
                    passed=record.get("passed") is True,
                    require_cold_sync=formal,
                )
                if requested <= previous_point_workers:
                    raise ValidationError(
                        f"{point_field}.worker_sweep must be strictly ordered"
                    )
                previous_point_workers = requested
        if support_active is True and summary_contract_declared and (
            workers <= 1
            or fallback_reason
            or summary_verified is not True
            or worker_sweep[-1].get("ordered_contact_summary_used") is not True
        ):
            raise ValidationError(
                f"{collision_field} must prove the ordered support-contact summary path"
            )

    if noncollision_enabled:
        noncollision_field = f"{field}.details.noncollision_validation"
        run_counts.append(
            _positive_integer(noncollision.get("runs"), f"{noncollision_field}.runs")
        )
        _nonnegative_integer(
            noncollision.get("allocations"), f"{noncollision_field}.allocations"
        )
        detail_latency = {
            name: _nonnegative_scalar(
                noncollision.get(f"{name}_us"), f"{noncollision_field}.{name}_us"
            )
            * 1000.0
            for name in ("p50", "p95", "p99", "max")
        }
        _validate_latency_summary(
            detail_latency, f"{noncollision_field}.latency_ns", require_p99=True
        )
        _validate_latency_summary(
            record.get("latency_ns"), f"{field}.latency_ns", require_p99=formal
        )
        for name, expected in detail_latency.items():
            observed = float(record["latency_ns"][name])
            if not math.isclose(observed, expected, rel_tol=1e-12, abs_tol=1e-6):
                raise ValidationError(
                    f"{field}.latency_ns.{name} contradicts non-collision details"
                )
    elif record.get("latency_ns") is not None:
        raise ValidationError(
            f"{field}.latency_ns is present without non-collision validation"
        )

    if samples != max(run_counts):
        raise ValidationError(f"{field}.sample_count contradicts validation run counts")


def _validate_rma_contract_record(record: dict[str, Any], field: str) -> None:
    details = record.get("details")
    if not isinstance(details, dict):
        raise ValidationError(f"{field}.details must be an object")
    for name in ("canonical_fingerprint", "view_fingerprint"):
        value = _require_nonempty_string(details.get(name), f"{field}.details.{name}")
        if re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise ValidationError(f"{field}.details.{name} must be a lowercase SHA-256")
    for name in (
        "view_changed",
        "mapping_bijective",
        "public_planning_joint_names_preserved",
    ):
        if not isinstance(details.get(name), bool):
            raise ValidationError(f"{field}.details.{name} must be boolean")
    renamed = _nonnegative_integer(
        details.get("renamed_joint_count"), f"{field}.details.renamed_joint_count"
    )
    if renamed > 0 and details["view_changed"] is not True:
        raise ValidationError(f"{field}.details.view_changed contradicts renamed joints")
    if record.get("passed") is True and (
        details["mapping_bijective"] is not True
        or details["public_planning_joint_names_preserved"] is not True
    ):
        raise ValidationError(f"{field} passed without preserving the RMA contract")


def _validate_live_scene_authority(value: Any, field: str) -> None:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    _validate_revision_identity(value.get("revision"), f"{field}.revision")
    policy = value.get("configured_collision_policy")
    if not isinstance(policy, dict):
        raise ValidationError(
            f"{field}.configured_collision_policy must be an object"
        )
    _require_nonempty_string(
        policy.get("policy_id"),
        f"{field}.configured_collision_policy.policy_id",
    )
    _validate_revision_identity(
        policy.get("revision"),
        f"{field}.configured_collision_policy.revision",
    )


def _validate_source_artifact(value: Any, field: str, *, named: bool) -> None:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    if named:
        _require_nonempty_string(value.get("name"), f"{field}.name")
    _require_nonempty_string(value.get("path"), f"{field}.path")
    digest = _require_nonempty_string(value.get("sha256"), f"{field}.sha256")
    if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise ValidationError(f"{field}.sha256 must be a lowercase SHA-256")


def _validate_rp5_record(
    record: dict[str, Any],
    field: str,
    *,
    formal: bool,
    roboplan_version: str | None = None,
) -> None:
    record_type = str(record["record_type"])
    if record_type == "collision_query":
        samples = _positive_sample_count(record, field)
        if formal and record.get("passed") is True and samples < 10_000:
            raise ValidationError(
                f"{field}.sample_count must be at least 10000 for formal collision evidence"
            )
        _validate_latency_summary(
            record.get("latency_ns"), f"{field}.latency_ns", require_p99=formal
        )
        _validate_latency_summary(
            record.get("cpu_time_ns"), f"{field}.cpu_time_ns", require_p99=formal
        )
        _validate_latency_summary(
            record.get("allocation_count"),
            f"{field}.allocation_count",
            require_p99=formal,
        )
        _require_nonempty_string(record.get("query_class"), f"{field}.query_class")
        return
    if record_type == "collision_revision":
        _validate_collision_revision_record(record, field, formal=formal)
        return
    if record_type == "trajectory_validation":
        _validate_trajectory_validation_record(record, field, formal=formal)
        return
    if record_type == "planning_pipeline":
        samples = _positive_sample_count(record, field)
        if formal and record.get("passed") is True and samples < 50:
            raise ValidationError(
                f"{field}.sample_count must be at least 50 for formal planning evidence"
            )
        mode = record.get("measurement_mode")
        if mode not in {
            "primary",
            "collision_probe_attribution",
            "capability_opt_in",
        }:
            raise ValidationError(f"{field}.measurement_mode is invalid")
        policy = record.get("planner_policy")
        if policy is None and roboplan_version not in ROBOPLAN_05_SCHEMA_VERSIONS:
            policy = "compatibility"
            planner_options = None
        else:
            planner_options = record.get("planner_options")
        if policy not in {"compatibility", "rrt_star", "budget_spending"}:
            raise ValidationError(f"{field}.planner_policy is invalid")
        if (
            roboplan_version in ROBOPLAN_05_SCHEMA_VERSIONS
            and not isinstance(planner_options, dict)
        ):
            raise ValidationError(f"{field}.planner_options must be an object")
        expected_policy_options = {
            "compatibility": (False, True),
            "rrt_star": (True, False),
            "budget_spending": (False, False),
        }
        expected_rrt_star, expected_fast_return = expected_policy_options[policy]
        if isinstance(planner_options, dict) and (
            planner_options.get("rrt_star") is not expected_rrt_star
            or planner_options.get("fast_return") is not expected_fast_return
        ):
            raise ValidationError(
                f"{field}.planner_options do not match planner_policy"
            )
        if mode == "capability_opt_in" and policy == "compatibility":
            raise ValidationError(
                f"{field} capability measurement uses the compatibility policy"
            )
        if mode != "capability_opt_in" and policy != "compatibility":
            raise ValidationError(
                f"{field} primary/attribution measurement changed production policy"
            )
        probe = record.get("collision_probe_available") is True
        if mode == "primary" and probe:
            raise ValidationError(f"{field} primary measurement loaded the collision probe")
        if mode == "collision_probe_attribution" and (
            not probe or int(record.get("total_collision_query_count") or 0) <= 0
        ):
            raise ValidationError(f"{field} attribution measurement has no collision evidence")
        if (
            mode == "collision_probe_attribution"
            and roboplan_version in ROBOPLAN_05_SCHEMA_VERSIONS
        ):
            stages = record.get("stages")
            rrt = stages.get("rrt") if isinstance(stages, dict) else None
            rrt_queries = (
                rrt.get("collision_query_count") if isinstance(rrt, dict) else None
            )
            if (
                not isinstance(rrt_queries, dict)
                or int(rrt_queries.get("p50") or 0) <= 0
            ):
                raise ValidationError(
                    f"{field} attribution did not observe RoboPlan 0.5 "
                    "CollisionContext queries inside the RRT stage"
                )
        if mode == "capability_opt_in" and probe:
            raise ValidationError(f"{field} capability measurement loaded the collision probe")
        for name in ("invariants_passed", "outcomes_passed", "deterministic_trajectory"):
            if record.get(name) is not True:
                raise ValidationError(f"{field}.{name} must be true")
        if int(record.get("distinct_trajectory_hashes") or 0) != 1:
            raise ValidationError(f"{field}.distinct_trajectory_hashes must equal one")
        _validate_latency_summary(
            record.get("latency_ns"), f"{field}.latency_ns", require_p99=False
        )
        _validate_latency_summary(
            record.get("cpu_time_ns"), f"{field}.cpu_time_ns", require_p99=False
        )
        _validate_latency_summary(
            record.get("allocation_count"),
            f"{field}.allocation_count",
            require_p99=False,
        )
        return
    if record_type == "live_planning_stage":
        details = record.get("details")
        if not isinstance(details, dict):
            raise ValidationError(f"{field}.details must be an object")
        if details.get("viewer_stratum") not in {"disconnected", "focused"}:
            raise ValidationError(f"{field}.details.viewer_stratum is invalid")
        if details.get("measurement_mode") not in {
            "primary",
            "collision_probe_attribution",
        }:
            raise ValidationError(f"{field}.details.measurement_mode is invalid")
        if record.get("passed") is True:
            samples = _positive_sample_count(record, field)
            if formal and samples < 30:
                raise ValidationError(
                    f"{field}.sample_count must be at least 30 for formal live evidence"
                )
            _validate_latency_summary(
                record.get("latency_ns"), f"{field}.latency_ns", require_p99=False
            )
            if record["latency_ns"].get("p99") is not None:
                raise ValidationError(f"{field}.latency_ns.p99 must be null for a live stratum")
            retained_samples = details.get("samples")
            if not isinstance(retained_samples, list) or len(retained_samples) != samples:
                raise ValidationError(f"{field}.details.samples must retain every live sample")
            mode = details["measurement_mode"]
            _validate_live_scene_authority(
                details.get("scene_authority_before"),
                f"{field}.details.scene_authority_before",
            )
            _validate_live_scene_authority(
                details.get("scene_authority_after"),
                f"{field}.details.scene_authority_after",
            )
            for index, sample in enumerate(retained_samples):
                metrics = sample.get("planner_metrics") if isinstance(sample, dict) else None
                if not isinstance(metrics, dict) or metrics.get("measurement_complete") is not True:
                    raise ValidationError(
                        f"{field}.details.samples[{index}] has incomplete planner metrics"
                    )
                probe = metrics.get("collision_probe_available") is True
                if mode == "primary" and probe:
                    raise ValidationError(
                        f"{field}.details.samples[{index}] primary sample loaded the probe"
                    )
                if mode == "collision_probe_attribution" and (
                    not probe or int(metrics.get("total_collision_query_count") or 0) <= 0
                ):
                    raise ValidationError(
                        f"{field}.details.samples[{index}] has no collision attribution"
                    )
                request = sample.get("request")
                if not isinstance(request, dict):
                    raise ValidationError(
                        f"{field}.details.samples[{index}].request must be an object"
                    )
                contact_policy_id = request.get("contact_policy_id")
                if not isinstance(contact_policy_id, str):
                    raise ValidationError(
                        f"{field}.details.samples[{index}].request.contact_policy_id "
                        "must be a string"
                    )
                expected_resolution = (
                    "explicit" if contact_policy_id else "configured_default"
                )
                if request.get("contact_policy_resolution") != expected_resolution:
                    raise ValidationError(
                        f"{field}.details.samples[{index}].request."
                        "contact_policy_resolution is inconsistent"
                    )
                binding = _validate_revision_identity(
                    request.get("scene_binding"),
                    f"{field}.details.samples[{index}].request.scene_binding",
                )
                result = sample.get("result")
                if not isinstance(result, dict):
                    raise ValidationError(
                        f"{field}.details.samples[{index}].result must be an object"
                    )
                result_revision = _validate_revision_identity(
                    result.get("scene_revision"),
                    f"{field}.details.samples[{index}].result.scene_revision",
                )
                if binding != result_revision:
                    raise ValidationError(
                        f"{field}.details.samples[{index}] changed scene revision"
                    )
        else:
            failures = details.get("failed_or_timeout_observations")
            if not isinstance(failures, list) or not failures:
                raise ValidationError(f"{field} failed live evidence must retain its failure")
        return
    if record_type == "rma_contract":
        _validate_rma_contract_record(record, field)
        return
    raise ValidationError(f"{field}.record_type is not supported: {record_type!r}")


def validate_evidence(path: Path) -> dict[str, Any]:
    evidence = _read_json(path)
    if evidence.get("schema_version") != EVIDENCE_SCHEMA:
        raise ValidationError(f"schema_version must be {EVIDENCE_SCHEMA!r}")
    for field in ("run_id", "phase"):
        _require_nonempty_string(evidence.get(field), field)
    if evidence["phase"] not in {"rp5_0", "rp5_3", "rp5_7"}:
        raise ValidationError("phase must be rp5_0, rp5_3, or rp5_7")
    for field in ("source", "environment"):
        if not isinstance(evidence.get(field), dict):
            raise ValidationError(f"{field} must be an object")
    source = evidence["source"]
    environment = evidence["environment"]
    # A clean-source RP5-3 or RP5-7 artifact is release evidence just as the
    # RP5-0 baseline is. Apply the same provenance and sample-size contract to
    # every formal phase; dirty-source preflights remain explicitly non-formal.
    formal = source.get("manyforge_dirty") is False
    if formal:
        commit = _require_nonempty_string(
            source.get("manyforge_commit"), "source.manyforge_commit"
        )
        if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
            raise ValidationError("source.manyforge_commit must be a full Git SHA")
        for name in ("roboplan_version", "roboplan_commit", "pinocchio_version"):
            _require_nonempty_string(source.get(name), f"source.{name}")
        expected_version = {
            "rp5_0": "0.4.0",
            "rp5_3": ROBOPLAN_05_VERSION,
            "rp5_7": ROBOPLAN_05_VERSION,
        }[str(evidence["phase"])]
        if source.get("roboplan_version") != expected_version:
            raise ValidationError(
                f"formal {evidence['phase']} evidence requires RoboPlan "
                f"{expected_version}"
            )
        if source.get("roboplan_version") == ROBOPLAN_05_VERSION:
            if source.get("roboplan_source_clean") is not True:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence requires a verified clean source checkout"
                )
            if source.get("pinocchio_version") != ROBOPLAN_05_PINOCCHIO_VERSION:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence must use Pinocchio "
                    f"{ROBOPLAN_05_PINOCCHIO_VERSION}"
                )
            if source.get("coal_version") != ROBOPLAN_05_COAL_VERSION:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence must use Coal "
                    f"{ROBOPLAN_05_COAL_VERSION}"
                )
            package_versions = source.get("roboplan_package_versions")
            if package_versions != ROBOPLAN_05_PACKAGE_VERSIONS:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence package-version inventory "
                    "differs from the reviewed source generation"
                )
            if source.get("roboplan_commit") != ROBOPLAN_05_PINNED_COMMIT:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence must use the reviewed pinned commit"
                )
            if source.get("roboplan_ref") != ROBOPLAN_05_PINNED_COMMIT:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence cannot use ROBOPLAN_REF override"
                )
            patch_manifest = _require_nonempty_string(
                source.get("roboplan_patch_manifest_sha256"),
                "source.roboplan_patch_manifest_sha256",
            )
            if patch_manifest != ROBOPLAN_05_PATCH_MANIFEST_SHA256:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence must use the reviewed patch manifest"
                )
            patches = source.get("roboplan_patches")
            if not isinstance(patches, list) or not patches:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence must retain checked patches"
                )
            seen_paths: set[str] = set()
            for index, patch in enumerate(patches):
                if not isinstance(patch, dict):
                    raise ValidationError(
                        f"source.roboplan_patches[{index}] must be an object"
                    )
                patch_path = _require_nonempty_string(
                    patch.get("path"),
                    f"source.roboplan_patches[{index}].path",
                )
                digest = _require_nonempty_string(
                    patch.get("sha256"),
                    f"source.roboplan_patches[{index}].sha256",
                )
                if patch_path in seen_paths:
                    raise ValidationError(
                        f"source.roboplan_patches repeats {patch_path!r}"
                    )
                if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
                    raise ValidationError(
                        f"source.roboplan_patches[{index}].sha256 must be a SHA-256"
                    )
                seen_paths.add(patch_path)
            observed_patches = {
                str(patch["path"]): str(patch["sha256"]) for patch in patches
            }
            if observed_patches != ROBOPLAN_05_PATCH_DIGESTS:
                raise ValidationError(
                    "formal RoboPlan 0.5 evidence patch set differs from the reviewed manifest"
                )
        _require_nonempty_string(environment.get("command"), "environment.command")
    records = evidence.get("records")
    if not isinstance(records, list) or not records:
        raise ValidationError("records must be a non-empty list")
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise ValidationError(f"records[{index}] must be an object")
        _require_nonempty_string(record.get("record_type"), f"records[{index}].record_type")
        _require_nonempty_string(record.get("fixture_id"), f"records[{index}].fixture_id")
        if not isinstance(record.get("passed"), bool):
            raise ValidationError(f"records[{index}].passed must be boolean")
        _validate_rp5_record(
            record,
            f"records[{index}]",
            formal=formal,
            roboplan_version=str(source.get("roboplan_version") or ""),
        )
    if formal and all(record.get("passed") is True for record in records):
        if environment.get("source_verified_after_run") is not True:
            raise ValidationError(
                "formal successful evidence must verify source after the run"
            )
    if formal and any(
        record.get("record_type") == "live_planning_stage"
        and record.get("passed") is True
        for record in records
    ):
        _validate_source_artifact(source.get("program"), "source.program", named=True)
        _validate_source_artifact(
            source.get("deployment"), "source.deployment", named=True
        )
    inventory = evidence.get("inventory")
    if formal and (
        not isinstance(inventory, dict)
        or not isinstance(inventory.get("build_references"), dict)
        or not isinstance(inventory.get("capabilities"), list)
        or not isinstance(inventory.get("workarounds"), list)
    ):
        raise ValidationError("formal RP5 evidence must retain the complete inventory")
    return {"schema_version": EVIDENCE_SCHEMA, "records": len(records)}


def require_source_inventory_stable(
    before: dict[str, Any], after: dict[str, Any]
) -> None:
    """Reject evidence if its source identity changed during measurement."""

    if before.get("source") != after.get("source"):
        raise RuntimeError(
            "ManyForge or RoboPlan source identity changed during qualification"
        )


def _manyforge_worktree_sha256(repo_root: Path) -> str:
    """Fingerprint every non-ignored source change relative to HEAD."""

    digest = hashlib.sha256()
    tracked = subprocess.check_output(
        [
            "git",
            "-C",
            str(repo_root),
            "diff",
            "--binary",
            "--full-index",
            "--no-ext-diff",
            "HEAD",
            "--",
        ]
    )
    digest.update(b"tracked-diff\0")
    digest.update(len(tracked).to_bytes(8, "big"))
    digest.update(tracked)

    untracked_output = subprocess.check_output(
        [
            "git",
            "-C",
            str(repo_root),
            "ls-files",
            "--others",
            "--exclude-standard",
            "-z",
        ]
    )
    untracked = sorted(name for name in untracked_output.split(b"\0") if name)
    digest.update(b"untracked-files\0")
    digest.update(len(untracked).to_bytes(8, "big"))
    for encoded_name in untracked:
        relative = os.fsdecode(encoded_name)
        path = repo_root / relative
        metadata = path.lstat()
        mode = stat.S_IMODE(metadata.st_mode)
        digest.update(len(encoded_name).to_bytes(8, "big"))
        digest.update(encoded_name)
        digest.update(mode.to_bytes(4, "big"))
        if stat.S_ISLNK(metadata.st_mode):
            payload = os.fsencode(os.readlink(path))
        elif stat.S_ISREG(metadata.st_mode):
            payload = path.read_bytes()
        else:
            raise ValidationError(
                f"{path}: unsupported untracked source file type"
            )
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def _git(repo_root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(repo_root), *args], text=True
    ).strip()


def _distribution_version(name: str) -> str | None:
    try:
        return importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return None


def _package_xml_versions(roboplan_root: Path) -> dict[str, str]:
    versions: dict[str, str] = {}
    version_pattern = re.compile(r"<version>([^<]+)</version>")
    for package_xml in sorted(roboplan_root.glob("*/package.xml")):
        match = version_pattern.search(package_xml.read_text(encoding="utf-8"))
        if match:
            versions[package_xml.parent.name] = match.group(1)
    return versions


def _checked_patch_manifest(repo_root: Path) -> dict[str, Any]:
    root = repo_root / "third_party/roboplan"
    manifest = root / "patches.sha256"
    if not manifest.is_file():
        raise ValidationError(f"{manifest}: RoboPlan patch manifest is missing")
    patches: list[dict[str, str]] = []
    listed_paths: set[str] = set()
    for line_number, raw_line in enumerate(
        manifest.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = raw_line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2 or re.fullmatch(r"[0-9a-f]{64}", parts[0]) is None:
            raise ValidationError(
                f"{manifest}:{line_number}: expected '<sha256> <relative path>'"
            )
        digest, relative_name = parts
        if re.fullmatch(r"patches/[A-Za-z0-9][A-Za-z0-9._-]*\.patch", relative_name) is None:
            raise ValidationError(
                f"{manifest}:{line_number}: patch must be a direct patches/*.patch entry"
            )
        if relative_name in listed_paths:
            raise ValidationError(
                f"{manifest}:{line_number}: duplicate patch path {relative_name!r}"
            )
        listed_paths.add(relative_name)
        patch = (root / relative_name).resolve()
        if not patch.is_relative_to(root.resolve()) or not patch.is_file():
            raise ValidationError(
                f"{manifest}:{line_number}: patch path is unavailable or escapes the manifest root"
            )
        actual = hashlib.sha256(patch.read_bytes()).hexdigest()
        if actual != digest:
            raise ValidationError(
                f"{patch}: digest {actual} does not match manifest {digest}"
            )
        patches.append({"path": relative_name, "sha256": digest})
    if not patches:
        raise ValidationError(f"{manifest}: at least one checked patch is required")
    available_paths = {
        path.relative_to(root).as_posix() for path in (root / "patches").glob("*.patch")
    }
    if listed_paths != available_paths:
        unlisted = sorted(available_paths - listed_paths)
        missing = sorted(listed_paths - available_paths)
        raise ValidationError(
            f"{manifest}: manifest/patch-directory mismatch; "
            f"unlisted={unlisted}, missing={missing}"
        )
    return {
        "manifest_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
        "patches": patches,
    }


def _version_inventory(
    repo_root: Path, roboplan_version: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    names = ROBOPLAN_INVENTORY_FILES.get(roboplan_version)
    if names is None:
        raise ValidationError(
            f"RoboPlan {roboplan_version!r} has no reviewed capability/workaround inventory"
        )
    inventory_root = repo_root / "benchmarks/roboplan_05/inventory"
    capability_path = inventory_root / names[0]
    workaround_path = inventory_root / names[1]
    capabilities = _read_json(capability_path)
    workarounds = _read_json(workaround_path)
    if capabilities.get("schema_version") != CAPABILITY_SCHEMA:
        raise ValidationError(f"{capability_path}: invalid schema_version")
    if workarounds.get("schema_version") != WORKAROUND_SCHEMA:
        raise ValidationError(f"{workaround_path}: invalid schema_version")
    if capabilities.get("roboplan_version") != roboplan_version:
        raise ValidationError(
            f"{capability_path}: inventory version does not match the source"
        )
    if workarounds.get("roboplan_version") != roboplan_version:
        raise ValidationError(
            f"{workaround_path}: inventory version does not match the source"
        )
    return capabilities, workarounds


def _build_reference_inventory(repo_root: Path) -> dict[str, list[str]]:
    patterns = {
        package: re.compile(rf"\b{re.escape(package)}\b")
        for package in ROBOPLAN_PACKAGES
    }
    references: dict[str, list[str]] = {package: [] for package in ROBOPLAN_PACKAGES}
    for path in repo_root.rglob("*"):
        if not path.is_file() or path.suffix not in SCAN_SUFFIXES:
            continue
        relative = path.relative_to(repo_root)
        if any(part in EXCLUDED_PARTS or part.startswith("build_") for part in relative.parts):
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeDecodeError):
            continue
        for line_number, line in enumerate(lines, 1):
            for package, pattern in patterns.items():
                if pattern.search(line):
                    references[package].append(f"{relative}:{line_number}")
    return references


def _verified_roboplan_checkout(repo_root: Path) -> tuple[Path, str, str]:
    roboplan_root = repo_root / ".docker" / "roboplan"
    ref_path = repo_root / ".docker" / ".manyforge-roboplan-ref"
    sha_path = repo_root / ".docker" / ".manyforge-roboplan-sha"
    if not ref_path.is_file() or not sha_path.is_file():
        raise ValidationError(
            f"{roboplan_root}: resolved RoboPlan ref/SHA markers are missing"
        )
    if not (roboplan_root / ".git").exists():
        raise ValidationError(
            f"{roboplan_root}: verifiable Git metadata is missing; rerun scripts/setup-docker.sh"
        )
    try:
        actual_sha = _git(roboplan_root, "rev-parse", "HEAD")
        status = _git(
            roboplan_root,
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--ignore-submodules=none",
        )
        submodules = _git(roboplan_root, "submodule", "status", "--recursive")
    except subprocess.CalledProcessError as exc:
        raise ValidationError(
            f"{roboplan_root}: RoboPlan source checkout cannot be verified"
        ) from exc
    expected_sha = sha_path.read_text(encoding="utf-8").strip()
    if actual_sha != expected_sha:
        raise ValidationError(
            f"{roboplan_root}: checkout SHA {actual_sha} does not match recorded {expected_sha}"
        )
    if status:
        raise ValidationError(
            f"{roboplan_root}: RoboPlan source checkout is dirty"
        )
    invalid_submodules = [
        line for line in submodules.splitlines() if line[:1] in {"-", "+", "U"}
    ]
    if invalid_submodules:
        raise ValidationError(
            f"{roboplan_root}: RoboPlan submodule checkout is not exact"
        )
    return (
        roboplan_root,
        ref_path.read_text(encoding="utf-8").strip(),
        actual_sha,
    )


def collect_inventory(repo_root: Path) -> dict[str, Any]:
    roboplan_root, roboplan_ref, roboplan_sha = _verified_roboplan_checkout(
        repo_root
    )
    package_versions = _package_xml_versions(roboplan_root)
    roboplan_version = package_versions.get("roboplan")
    if not roboplan_version:
        raise ValidationError(
            f"{roboplan_root}: RoboPlan package version is unavailable"
        )
    capabilities, workarounds = _version_inventory(
        repo_root, roboplan_version
    )
    patch_manifest = (
        _checked_patch_manifest(repo_root)
        if roboplan_version == ROBOPLAN_05_VERSION
        else {"manifest_sha256": None, "patches": []}
    )

    governors: set[str] = set()
    for governor in Path("/sys/devices/system/cpu").glob(
        "cpu[0-9]*/cpufreq/scaling_governor"
    ):
        try:
            governors.add(governor.read_text(encoding="utf-8").strip())
        except OSError:
            pass
    dirty = bool(_git(repo_root, "status", "--porcelain"))
    return {
        "schema_version": "manyforge.roboplan_transition_inventory.v1",
        "source": {
            "manyforge_commit": _git(repo_root, "rev-parse", "HEAD"),
            "manyforge_dirty": dirty,
            "manyforge_worktree_sha256": _manyforge_worktree_sha256(repo_root),
            "roboplan_ref": roboplan_ref,
            "roboplan_commit": roboplan_sha,
            "roboplan_source_clean": True,
            "roboplan_packages": package_versions,
            "roboplan_patch_manifest_sha256": patch_manifest[
                "manifest_sha256"
            ],
            "roboplan_patches": patch_manifest["patches"],
            "python_packages": {
                name: _distribution_version(name)
                for name in (
                    "pin",
                    "libpinocchio",
                    "coal",
                    "libcoal",
                    "eigenpy",
                    "cmeel",
                    "cmeel-urdfdom",
                    "cmeel-octomap",
                )
            },
        },
        "environment": {
            "container": os.environ.get("HOSTNAME", "unknown"),
            "architecture": platform.machine(),
            "platform": platform.platform(),
            "cpu_count": os.cpu_count(),
            "cpu_affinity": sorted(os.sched_getaffinity(0)),
            "cpu_governors": sorted(governors),
        },
        "build_references": _build_reference_inventory(repo_root),
        "capabilities": capabilities["capabilities"],
        "workarounds": workarounds["workarounds"],
    }


def _write_or_print(value: dict[str, Any], output: Path | None) -> None:
    payload = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if output is None:
        sys.stdout.write(payload)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(payload, encoding="utf-8")


def _repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=_repo_root_from_script())
    subparsers = parser.add_subparsers(dest="command", required=True)

    corpus_parser = subparsers.add_parser("validate-corpus")
    corpus_parser.add_argument(
        "--corpus",
        type=Path,
        default=Path("benchmarks/roboplan_05/corpus/corpus-v1.json"),
    )
    corpus_parser.add_argument("--allow-pending-capture", action="store_true")

    capture_parser = subparsers.add_parser("validate-capture")
    capture_parser.add_argument("capture", type=Path)

    evidence_parser = subparsers.add_parser("validate-evidence")
    evidence_parser.add_argument("evidence", type=Path)

    inventory_parser = subparsers.add_parser("inventory")
    inventory_parser.add_argument("--output", type=Path)

    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve()
    try:
        if args.command == "validate-corpus":
            corpus_path = args.corpus
            if not corpus_path.is_absolute():
                corpus_path = repo_root / corpus_path
            _write_or_print(
                validate_corpus(
                    corpus_path,
                    allow_pending_capture=args.allow_pending_capture,
                ),
                None,
            )
        elif args.command == "validate-capture":
            _write_or_print(validate_capture(args.capture.resolve()), None)
        elif args.command == "validate-evidence":
            _write_or_print(validate_evidence(args.evidence.resolve()), None)
        elif args.command == "inventory":
            _write_or_print(collect_inventory(repo_root), args.output)
    except ValidationError as exc:
        print(f"roboplan qualification error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
