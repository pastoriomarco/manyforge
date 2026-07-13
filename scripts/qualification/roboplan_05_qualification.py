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
ROBOPLAN_PACKAGES = (
    "roboplan",
    "roboplan_rrt",
    "roboplan_toppra",
    "roboplan_simple_ik",
    "roboplan_oink",
    "roboplan_cartesian_planning",
)
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
            continue
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
    record: dict[str, Any], field: str, *, formal: bool
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
    if record_type == "planning_pipeline":
        samples = _positive_sample_count(record, field)
        if formal and record.get("passed") is True and samples < 50:
            raise ValidationError(
                f"{field}.sample_count must be at least 50 for formal planning evidence"
            )
        mode = record.get("measurement_mode")
        if mode not in {"primary", "collision_probe_attribution"}:
            raise ValidationError(f"{field}.measurement_mode is invalid")
        probe = record.get("collision_probe_available") is True
        if mode == "primary" and probe:
            raise ValidationError(f"{field} primary measurement loaded the collision probe")
        if mode == "collision_probe_attribution" and (
            not probe or int(record.get("total_collision_query_count") or 0) <= 0
        ):
            raise ValidationError(f"{field} attribution measurement has no collision evidence")
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
    formal = evidence["phase"] == "rp5_0" and source.get("manyforge_dirty") is False
    if formal:
        commit = _require_nonempty_string(
            source.get("manyforge_commit"), "source.manyforge_commit"
        )
        if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
            raise ValidationError("source.manyforge_commit must be a full Git SHA")
        for name in ("roboplan_version", "roboplan_commit", "pinocchio_version"):
            _require_nonempty_string(source.get(name), f"source.{name}")
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
        _validate_rp5_record(record, f"records[{index}]", formal=formal)
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
        raise ValidationError("formal RP5-0 evidence must retain the complete inventory")
    return {"schema_version": EVIDENCE_SCHEMA, "records": len(records)}


def require_source_inventory_stable(
    before: dict[str, Any], after: dict[str, Any]
) -> None:
    """Reject evidence if its source identity changed during measurement."""

    if before.get("source") != after.get("source"):
        raise RuntimeError(
            "ManyForge or RoboPlan source identity changed during qualification"
        )


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


def collect_inventory(repo_root: Path) -> dict[str, Any]:
    roboplan_root = repo_root / ".docker" / "roboplan"
    ref_path = roboplan_root / ".manyforge-roboplan-ref"
    sha_path = roboplan_root / ".manyforge-roboplan-sha"
    if not ref_path.is_file() or not sha_path.is_file():
        raise ValidationError(
            f"{roboplan_root}: resolved RoboPlan ref/SHA markers are missing"
        )
    capability_path = (
        repo_root
        / "benchmarks/roboplan_05/inventory/roboplan-0.4.0-capabilities.json"
    )
    workaround_path = (
        repo_root
        / "benchmarks/roboplan_05/inventory/roboplan-0.4.0-workarounds.json"
    )
    capabilities = _read_json(capability_path)
    workarounds = _read_json(workaround_path)
    if capabilities.get("schema_version") != CAPABILITY_SCHEMA:
        raise ValidationError(f"{capability_path}: invalid schema_version")
    if workarounds.get("schema_version") != WORKAROUND_SCHEMA:
        raise ValidationError(f"{workaround_path}: invalid schema_version")

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
            "roboplan_ref": ref_path.read_text(encoding="utf-8").strip(),
            "roboplan_commit": sha_path.read_text(encoding="utf-8").strip(),
            "roboplan_packages": _package_xml_versions(roboplan_root),
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
