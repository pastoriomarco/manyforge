#!/usr/bin/env python3
"""Capture the RP5 deterministic drop-approach fixture from live Isaac."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

import yaml


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.qualification.roboplan_05_qualification import (  # noqa: E402
    cycle_failure_reason,
    validate_capture,
    write_evidence_exclusive,
)


SCHEMA_VERSION = "manyforge.roboplan_transition_capture.v1"


def _read_yaml(path: Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a YAML mapping")
    return value


def _request_json(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
    timeout_s: float = 10.0,
) -> dict[str, Any]:
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}{path}",
        data=body,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            value = json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"{method} {path} failed ({exc.code}): {detail}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{method} {path} returned a non-object response")
    return value


def _find_node(node: dict[str, Any], name: str) -> dict[str, Any]:
    if node.get("name") == name:
        return node
    for child in node.get("children") or []:
        try:
            return _find_node(child, name)
        except KeyError:
            pass
    raise KeyError(name)


def _quaternion_from_rpy_deg(values: list[float]) -> list[float]:
    roll, pitch, yaw = (math.radians(value) for value in values)
    cr, sr = math.cos(roll / 2.0), math.sin(roll / 2.0)
    cp, sp = math.cos(pitch / 2.0), math.sin(pitch / 2.0)
    cy, sy = math.cos(yaw / 2.0), math.sin(yaw / 2.0)
    return [
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    ]


def _rotate_vector_inverse(quaternion: list[float], vector: list[float]) -> list[float]:
    x, y, z, w = quaternion
    # R(q)^T v, expanded to avoid a numeric dependency in the capture tool.
    return [
        (1 - 2 * (y * y + z * z)) * vector[0]
        + 2 * (x * y + w * z) * vector[1]
        + 2 * (x * z - w * y) * vector[2],
        2 * (x * y - w * z) * vector[0]
        + (1 - 2 * (x * x + z * z)) * vector[1]
        + 2 * (y * z + w * x) * vector[2],
        2 * (x * z + w * y) * vector[0]
        + 2 * (y * z - w * x) * vector[1]
        + (1 - 2 * (x * x + y * y)) * vector[2],
    ]


def _resolved_drop_goal(
    program: dict[str, Any], scene_state: dict[str, Any]
) -> dict[str, Any]:
    node = _find_node(program["tree"], "move_to_drop_approach")
    relative = node["params"]["relative_move"]
    target_id = relative["target_reference"]["id"]
    target = next(
        item for item in scene_state["objects"] if item["object_id"] == target_id
    )
    robot_base = scene_state["robot"]["base_pose"]
    base_position = [float(value) for value in robot_base["position_m"]]
    base_orientation = _quaternion_from_rpy_deg(
        [float(value) for value in robot_base["orientation_rpy_deg"]]
    )
    world_target = [float(value) for value in target["pose_in_universe"]["position_m"]]
    target_in_base = _rotate_vector_inverse(
        base_orientation,
        [world_target[index] - base_position[index] for index in range(3)],
    )
    offset_reference = relative.get("offset_reference") or {}
    if (
        offset_reference.get("type") != "frame"
        or offset_reference.get("id") != scene_state["robot"]["base_frame"]
    ):
        raise ValueError("capture supports the authored base-frame offset only")
    offset = relative["offset"]
    offset_position = [float(value) for value in offset["position_m"]]
    offset_orientation = _quaternion_from_rpy_deg(
        [float(value) for value in offset["orientation_rpy_deg"]]
    )
    return {
        "type": "pose_goal",
        "pose_goal": {
            "position_m": [
                target_in_base[index] + offset_position[index]
                for index in range(3)
            ],
            "orientation_xyzw": offset_orientation,
        },
        "source": {
            "target_reference": relative["target_reference"],
            "offset_reference": offset_reference,
            "offset_pose": {
                "position_m": offset_position,
                "orientation_xyzw": offset_orientation,
            },
            "authored_offset": offset,
        },
    }


def _shape_from_scene_object(value: dict[str, Any]) -> dict[str, Any]:
    shape = value["shape"]
    shape_type = shape["type"]
    if shape_type == "mesh":
        return {
            "type": "mesh",
            "resource_uri": shape["mesh_resource_uri"],
            "mesh_scale": [float(item) for item in shape["mesh_scale"]],
        }
    if shape_type == "box":
        return {
            "type": "box",
            "dimensions_m": [float(item) for item in shape["box_dimensions_m"]],
        }
    raise ValueError(f"unsupported captured object shape: {shape_type}")


def _capture_payload(
    *,
    program: dict[str, Any],
    visual_scene_state: dict[str, Any],
    authority_snapshot: dict[str, Any],
    planning_record: dict[str, Any],
    run_id: str,
    planning_execution_id: int,
    program_path: Path,
    deployment_path: Path,
    repo_root: Path,
    model_manifest: dict[str, Any],
) -> dict[str, Any]:
    request = planning_record.get("request") or {}
    metrics = planning_record.get("metrics") or {}
    if request.get("move_id") != "drop_approach":
        raise RuntimeError("qualification record is not the drop-approach request")
    start = request.get("start_state") or {}
    joint_names = list(start.get("joint_names") or [])
    planning_positions = list(start.get("positions_rad") or [])
    planning_velocities = list(start.get("velocities_rad_s") or [])
    if (
        not joint_names
        or len(planning_positions) != len(joint_names)
        or not all(math.isfinite(float(value)) for value in planning_positions)
    ):
        raise RuntimeError("captured PlanRequestDTO start state is invalid")
    if (
        len(planning_velocities) != len(joint_names)
        or not all(
            math.isfinite(float(value)) and float(value) == 0.0
            for value in planning_velocities
        )
    ):
        raise RuntimeError(
            "commanded-simulation drop capture requires an at-rest command "
            "reference with zero velocity"
        )
    motion = request.get("motion") or {}
    if motion.get("type") != "pose_goal" or not isinstance(
        motion.get("pose"), dict
    ):
        raise RuntimeError("drop-approach PlanRequestDTO is not a pose goal")
    scene_binding = request.get("scene_binding") or {}
    authority_revision = authority_snapshot.get("revision") or {}
    if (
        int(scene_binding.get("revision_id") or 0)
        != int(authority_revision.get("revision_id") or 0)
        or scene_binding.get("content_hash")
        != authority_revision.get("content_hash")
    ):
        raise RuntimeError(
            "captured logical scene does not match the planner's retained binding"
        )

    authored_resolution = _resolved_drop_goal(program, visual_scene_state)
    resolved_pose = motion["pose"]
    expected_pose = authored_resolution["pose_goal"]
    try:
        position_error = max(
            abs(float(left) - float(right))
            for left, right in zip(
                resolved_pose["position_m"],
                expected_pose["position_m"],
                strict=True,
            )
        )
        left_q = [float(value) for value in resolved_pose["orientation_xyzw"]]
        right_q = [float(value) for value in expected_pose["orientation_xyzw"]]
        quaternion_error = min(
            max(
                abs(left - right)
                for left, right in zip(left_q, right_q, strict=True)
            ),
            max(
                abs(left + right)
                for left, right in zip(left_q, right_q, strict=True)
            ),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError("captured PlanRequestDTO pose is malformed") from exc
    if position_error > 1e-8 or quaternion_error > 1e-8:
        raise RuntimeError(
            "captured PlanRequestDTO goal disagrees with authored reference resolution"
        )

    objects = []
    for value in authority_snapshot.get("objects") or []:
        attached = bool(value.get("attached"))
        objects.append(
            {
                "object_id": value["object_id"],
                "shape": _shape_from_scene_object(value),
                "pose_in_world": {
                    "position_m": [
                        float(item)
                        for item in value["pose_in_universe"]["position_m"]
                    ],
                    "orientation_xyzw": [
                        float(item)
                        for item in value["pose_in_universe"]["orientation_xyzw"]
                    ],
                },
                "attached": attached,
                "attached_to": value.get("frame_id") if attached else "",
                "touch_links": list(value.get("touch_links") or []),
                "support_contacts": list(value.get("support_contacts") or []),
                "observation": {
                    key: value.get(key)
                    for key in (
                        "source_id",
                        "source_timestamp_ns",
                        "source_freshness_budget_ns",
                        "scene_admission_role",
                        "scene_admission_lease_max_age_ns",
                        "scene_admission_target_drift_tolerance_m",
                        "scene_admission_target_angular_tolerance_rad",
                        "scene_admission_obstacle_drift_tolerance_m",
                        "scene_admission_obstacle_angular_tolerance_rad",
                        "scene_admission_allow_runtime_dynamic_obstacle",
                    )
                },
            }
        )
    cheese = next(item for item in objects if item["object_id"] == "isaac_cheese_box")
    if not cheese["attached"] or cheese["attached_to"] != "grasp_frame":
        raise RuntimeError("capture point does not have cheese attached to grasp_frame")
    robot_id = str(request.get("robot_id") or "")
    robot_placement = next(
        (
            value
            for value in authority_snapshot.get("robot_placements") or []
            if value.get("robot_id") == robot_id
        ),
        None,
    )
    if robot_placement is None:
        raise RuntimeError("logical scene has no placement for the planned robot")
    payload = {
        "schema_version": SCHEMA_VERSION,
        "capture_id": "isaac-light-drop-approach-v1",
        "source": {
            "program": str(program_path.relative_to(repo_root)),
            "program_sha256": hashlib.sha256(program_path.read_bytes()).hexdigest(),
            "deployment": str(deployment_path.relative_to(repo_root)),
            "deployment_sha256": hashlib.sha256(
                deployment_path.read_bytes()
            ).hexdigest(),
            "program_run_id": run_id,
            "planning_execution_id": planning_execution_id,
            "predecessor_node": "pick_retreat",
            "move_id": "drop_approach",
            "captured_at_wall_ns": time.time_ns(),
            "visual_state_diagnostics": visual_scene_state.get(
                "visual_state_diagnostics", {}
            ),
        },
        "robot_model_authority": model_manifest.get("authority"),
        "start_state": {
            "joint_names": joint_names,
            "positions_rad": [float(value) for value in planning_positions],
            "velocities_rad_s": [float(value) for value in planning_velocities],
            "state_source": "exact_plan_request",
            "provenance": {
                "execution_state_source_mode": "commanded_sim",
                "capture_method": "planner_qualification_observer",
                "predecessor_move_id": "pick_retreat",
            },
        },
        "resolved_motion": {
            "type": "pose_goal",
            "pose_goal": resolved_pose,
            "source": authored_resolution["source"],
        },
        "plan_request": request,
        "planner_contract": {
            "rng_base_seed": metrics.get("rng_base_seed"),
            "rng_seed": metrics.get("rng_seed"),
            "shortcut_rng_seed_argument": metrics.get(
                "shortcut_rng_seed_argument"
            ),
            "planning_attempt_index": metrics.get("planning_attempt_index"),
        },
        "scene": {
            "world_frame": authority_snapshot.get("world_frame"),
            "authority": {
                key: authority_snapshot.get(key)
                for key in (
                    "revision",
                    "transaction_sequence",
                    "scene_admission_epoch",
                    "topology_epoch",
                    "policy_epoch",
                    "configured_collision_policy",
                )
            },
            "robot": {
                "robot_id": robot_id,
                "base_frame": request.get("base_frame"),
                "pose_in_world": robot_placement["pose_in_world"],
            },
            "objects": objects,
        },
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    payload["semantic_sha256"] = hashlib.sha256(canonical).hexdigest()
    return payload


def _capture_scene_ready(
    scene_state: dict[str, Any], *, max_source_age_ns: int
) -> tuple[bool, str]:
    diagnostics = scene_state.get("visual_state_diagnostics") or {}
    if diagnostics.get("served_from_visual_snapshot") is not True:
        return False, "visual snapshot is not published yet"
    if diagnostics.get("snapshot_stale") is True:
        return False, "visual snapshot is stale"
    if diagnostics.get("snapshot_publisher_error"):
        return False, "visual snapshot publisher reports an error"
    if diagnostics.get("objects_source") != "visual_snapshot":
        return False, "scene objects are not from the coherent visual snapshot"

    robot = scene_state.get("robot") or {}
    joint_names = robot.get("joint_names") or []
    joint_positions = robot.get("joint_positions") or {}
    if not joint_names or set(joint_names) != set(joint_positions):
        return False, "robot joint state is incomplete"
    if not all(math.isfinite(float(joint_positions[name])) for name in joint_names):
        return False, "robot joint state is non-finite"
    source_age_ns = robot.get("source_age_ns")
    if source_age_ns is None or int(source_age_ns) > max_source_age_ns:
        return False, "robot observed state is not fresh enough"
    return True, ""


def _wait_for_cycle_quiescence(base_url: str, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        status = _request_json(base_url, "/api/cycle/status")
        if status.get("state") in {"idle", "stopped"}:
            return
        time.sleep(0.05)
    raise TimeoutError("cycle did not become idle or stopped")


def _quiesce_cycle(base_url: str, timeout_s: float) -> None:
    status = _request_json(base_url, "/api/cycle/status")
    if status.get("state") in {"idle", "stopped"}:
        return
    _request_json(
        base_url,
        "/api/cycle/full-stop",
        method="POST",
        payload={},
        timeout_s=timeout_s,
    )
    _wait_for_cycle_quiescence(base_url, timeout_s)


def _reset_cycle(base_url: str, timeout_s: float) -> None:
    _quiesce_cycle(base_url, timeout_s)
    _request_json(
        base_url,
        "/api/cycle/reset",
        method="POST",
        payload={},
        timeout_s=timeout_s,
    )
    _wait_for_cycle_quiescence(base_url, timeout_s)


def reset_and_start_simulation(base_url: str, timeout_s: float) -> None:
    """Reset external simulation state and verify physics is playing."""

    result = _request_json(
        base_url,
        "/api/simulation/reset",
        method="POST",
        payload={},
        timeout_s=timeout_s,
    )
    if result.get("success") is not True:
        raise RuntimeError(
            "deployment simulation reset did not report success: "
            f"{result.get('error') or result}"
        )
    playing = _request_json(
        base_url,
        "/api/simulation/state",
        method="POST",
        payload={
            "state": "playing",
            "reason": "rp5_qualification_start",
        },
        timeout_s=timeout_s,
    )
    if playing.get("success") is not True or playing.get("state") != "playing":
        raise RuntimeError(
            "deployment simulation play did not report success: "
            f"{playing.get('error') or playing}"
        )


def require_loaded_artifacts_match(
    base_url: str,
    *,
    program_path: Path,
    deployment_path: Path,
    expected_program_name: str,
) -> dict[str, Any]:
    current_program = _request_json(base_url, "/api/program")
    if current_program.get("name") != expected_program_name:
        raise RuntimeError(
            "Composer does not have the required normal Isaac light program loaded"
        )
    loaded_program_path = Path(str(current_program.get("sourcePath") or ""))
    if not loaded_program_path.is_absolute() or (
        loaded_program_path.resolve() != program_path.resolve()
    ):
        raise RuntimeError(
            "Composer program source does not match the capture source file"
        )
    current_tree = _request_json(base_url, "/api/program/tree")
    saved_tree = _request_json(base_url, "/api/program/saved/tree")
    if current_tree != saved_tree:
        raise RuntimeError(
            "Composer program has unsaved or runtime-overridden tree changes"
        )

    current_deployment = _request_json(base_url, "/api/deployment/full")
    loaded_deployment_path = Path(
        str(current_deployment.get("sourcePath") or "")
    )
    if not loaded_deployment_path.is_absolute() or (
        loaded_deployment_path.resolve() != deployment_path.resolve()
    ):
        raise RuntimeError(
            "Composer deployment source does not match the capture source file"
        )
    saved_deployment = _request_json(base_url, "/api/deployment/saved/full")
    if current_deployment != saved_deployment:
        raise RuntimeError("Composer deployment has unsaved changes")
    return {
        "program": {
            "name": expected_program_name,
            "path": str(
                program_path.relative_to(REPO_ROOT)
                if program_path.is_relative_to(REPO_ROOT)
                else program_path
            ),
            "sha256": hashlib.sha256(program_path.read_bytes()).hexdigest(),
        },
        "deployment": {
            "name": str(current_deployment.get("name") or ""),
            "path": str(
                deployment_path.relative_to(REPO_ROOT)
                if deployment_path.is_relative_to(REPO_ROOT)
                else deployment_path
            ),
            "sha256": hashlib.sha256(deployment_path.read_bytes()).hexdigest(),
        },
    }


def _wait_for_capture_point(
    base_url: str,
    timeout_s: float,
    *,
    previous_run_id: str,
    previous_execution_id: int,
    max_source_age_ns: int,
) -> tuple[str, int, dict[str, Any], dict[str, Any], dict[str, Any]]:
    deadline = time.monotonic() + timeout_s
    run_id = ""
    last_wait_reason = "drop-approach planning has not started"
    while time.monotonic() < deadline:
        kernel = _request_json(base_url, "/api/kernel/snapshot")
        execution_id = int(kernel.get("execution_id") or 0)
        if (
            kernel.get("state") != "planning"
            or kernel.get("active_move_id") != "drop_approach"
            or execution_id <= 0
            or execution_id == previous_execution_id
        ):
            if kernel.get("state") in {"failed", "intervention_required"}:
                raise RuntimeError(
                    "cycle failed before drop-approach capture: "
                    f"{kernel.get('error') or kernel.get('state')}"
                )
            cycle_status = _request_json(base_url, "/api/cycle/status")
            failure_reason = cycle_failure_reason(cycle_status)
            if failure_reason is not None:
                raise RuntimeError(
                    "cycle failed before drop-approach capture: "
                    f"{failure_reason}"
                )
            time.sleep(0.02)
            continue

        history = _request_json(base_url, "/api/execution/history?limit=1")
        run_id = str(history.get("runId") or run_id)
        if not run_id or run_id == previous_run_id:
            last_wait_reason = "new program run is not visible in history yet"
            time.sleep(0.02)
            continue

        scene_state = _request_json(base_url, "/api/scene/state")
        ready, last_wait_reason = _capture_scene_ready(
            scene_state, max_source_age_ns=max_source_age_ns
        )
        if not ready:
            time.sleep(0.02)
            continue
        authority_snapshot = _request_json(
            base_url, "/api/debug/qualification/scene-authority-snapshot"
        )
        cheese = next(
            (
                item
                for item in authority_snapshot.get("objects", [])
                if item.get("object_id") == "isaac_cheese_box"
            ),
            None,
        )
        if (
            not cheese
            or not cheese.get("attached")
            or cheese.get("frame_id") != "grasp_frame"
        ):
            last_wait_reason = (
                "logical scene does not have cheese attached at drop planning"
            )
            time.sleep(0.02)
            continue

        kernel_after = _request_json(base_url, "/api/kernel/snapshot")
        if (
            kernel_after.get("state") != "planning"
            or kernel_after.get("active_move_id") != "drop_approach"
            or int(kernel_after.get("execution_id") or 0)
            != execution_id
        ):
            last_wait_reason = "planning completed while snapshot was acquired"
            time.sleep(0.02)
            continue
        return (
            run_id,
            execution_id,
            scene_state,
            kernel_after,
            authority_snapshot,
        )
    raise TimeoutError(
        "drop-approach planning did not produce a coherent snapshot: "
        f"{last_wait_reason}"
    )


def _wait_for_completed_drop_plan_record(
    base_url: str,
    *,
    generation: int,
    timeout_s: float,
    expected_program_run_id: str,
    expected_node_attempt_id: str,
) -> list[dict[str, Any]]:
    deadline = time.monotonic() + timeout_s
    records: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        batch = _request_json(
            base_url,
            "/api/debug/qualification/planning-capture/drain",
            method="POST",
            payload={"generation": generation},
        )
        records.extend(batch.get("records") or [])
        drop_records = [
            record
            for record in records
            if (record.get("request") or {}).get("move_id") == "drop_approach"
            and (
                ((record.get("request") or {}).get("action_span") or {}).get(
                    "program_run_id"
                )
                == expected_program_run_id
            )
            and (
                ((record.get("request") or {}).get("action_span") or {}).get(
                    "node_attempt_id"
                )
                == expected_node_attempt_id
            )
        ]
        if len(drop_records) > 1:
            raise RuntimeError(
                "planning qualification capture observed multiple drop-approach "
                "requests before the fixture was frozen"
            )
        if drop_records:
            metrics = drop_records[0].get("metrics") or {}
            result = drop_records[0].get("result") or {}
            if (
                metrics.get("measurement_complete") is True
                and metrics.get("succeeded") is True
                and isinstance(metrics.get("shortcut_rng_seed_argument"), int)
                and result.get("trajectory") is not None
            ):
                return records
            raise RuntimeError(
                "drop-approach planner invocation completed without a valid "
                "successful qualification result"
            )
        status = _request_json(base_url, "/api/cycle/status")
        failure_reason = cycle_failure_reason(status)
        if failure_reason is not None:
            raise RuntimeError(
                "cycle failed before the drop-approach planner completed: "
                f"{failure_reason}"
            )
        time.sleep(0.02)
    raise TimeoutError("drop-approach planner did not complete before capture timeout")


def _planning_attempt_id_for_execution(
    base_url: str,
    *,
    program_run_id: str,
    execution_id: int,
) -> str:
    payload = _request_json(base_url, "/api/kernel/events?since=0")
    candidates = {
        str((event.get("details") or {}).get("node_attempt_id") or "")
        for event in payload.get("events", [])
        if event.get("type") == "plan_started"
        and int(event.get("execution_id") or 0) == execution_id
        and str(event.get("program_run_id") or "") == program_run_id
    }
    candidates.discard("")
    if len(candidates) != 1:
        raise RuntimeError(
            "could not identify exactly one planner attempt for captured "
            f"execution {execution_id} in run {program_run_id!r}"
        )
    return next(iter(candidates))


def capture(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = REPO_ROOT
    program_path = args.program.resolve()
    deployment_path = args.deployment.resolve()
    if not program_path.is_relative_to(repo_root) or not deployment_path.is_relative_to(
        repo_root
    ):
        raise ValueError("program and deployment must be inside the ManyForge repo")
    program = _read_yaml(program_path)
    deployment = _read_yaml(deployment_path)
    execution_mode = (
        deployment.get("adapters", {})
        .get("execution", {})
        .get("config", {})
        .get("execution_state_source_mode")
    )
    if execution_mode != "commanded_sim":
        raise RuntimeError(
            "Isaac drop-approach capture requires "
            "execution_state_source_mode=commanded_sim"
        )
    require_loaded_artifacts_match(
        args.composer_url,
        program_path=program_path,
        deployment_path=deployment_path,
        expected_program_name=str(program.get("name") or ""),
    )

    cycle_started = False
    planning_capture_generation = 0
    try:
        _reset_cycle(args.composer_url, 15.0)
        capture_start = _request_json(
            args.composer_url,
            "/api/debug/qualification/planning-capture/start",
            method="POST",
            payload={"capacity": 128},
        )
        planning_capture_generation = int(capture_start.get("generation") or 0)
        if planning_capture_generation <= 0:
            raise RuntimeError("planning qualification capture did not start")
        # Cycle reset clears ManyForge-owned execution and attachment state;
        # simulation reset independently restores external physics and robot
        # state. Both are required for a reproducible cross-version fixture.
        reset_and_start_simulation(args.composer_url, args.timeout_s)
        # Simulation readiness proves control/stream availability, not that
        # unconstrained objects have settled onto their support surfaces.
        # Keep this outside every measured stage and never turn a collision
        # failure into a retry/pass.
        time.sleep(args.simulation_settle_s)
        previous_history = _request_json(
            args.composer_url, "/api/execution/history?limit=1"
        )
        previous_run_id = str(previous_history.get("runId") or "")
        previous_kernel = _request_json(args.composer_url, "/api/kernel/snapshot")
        previous_execution_id = int(previous_kernel.get("execution_id") or 0)
        _request_json(args.composer_url, "/api/cycle/run", method="POST", payload={})
        cycle_started = True
        (
            run_id,
            planning_execution_id,
            visual_scene_state,
            _planning_snapshot,
            authority_snapshot,
        ) = _wait_for_capture_point(
            args.composer_url,
            args.timeout_s,
            previous_run_id=previous_run_id,
            previous_execution_id=previous_execution_id,
            max_source_age_ns=int(args.max_source_age_ms * 1_000_000),
        )
        planning_node_attempt_id = _planning_attempt_id_for_execution(
            args.composer_url,
            program_run_id=run_id,
            execution_id=planning_execution_id,
        )
        drained_records = _wait_for_completed_drop_plan_record(
            args.composer_url,
            generation=planning_capture_generation,
            timeout_s=args.timeout_s,
            expected_program_run_id=run_id,
            expected_node_attempt_id=planning_node_attempt_id,
        )
        # The exact request and scene values are immutable Python data. Stop
        # only after the planner observer has committed its complete record;
        # this avoids turning the baseline fixture into a cancelled plan.
        _quiesce_cycle(args.composer_url, 15.0)
        cycle_started = False
        captured_records = _request_json(
            args.composer_url,
            "/api/debug/qualification/planning-capture/stop",
            method="POST",
            payload={
                "generation": planning_capture_generation,
                "idle_timeout_ms": 10_000,
            },
            timeout_s=15.0,
        )
        planning_capture_generation = 0
        status = captured_records.get("status") or {}
        if int(status.get("dropped_total") or 0) != 0:
            raise RuntimeError("planning qualification capture dropped records")
        all_records = drained_records + list(captured_records.get("records") or [])
        drop_records = [
            record
            for record in all_records
            if (record.get("request") or {}).get("move_id") == "drop_approach"
            and (
                ((record.get("request") or {}).get("action_span") or {}).get(
                    "program_run_id"
                )
                == run_id
            )
            and (
                ((record.get("request") or {}).get("action_span") or {}).get(
                    "node_attempt_id"
                )
                == planning_node_attempt_id
            )
        ]
        if len(drop_records) != 1:
            raise RuntimeError(
                "planning qualification capture did not retain exactly one "
                "drop-approach request"
            )
        planning_record = drop_records[0]
        model_manifest = _request_json(
            args.composer_url, "/api/scene/urdf/manifest"
        )
        authority = model_manifest.get("authority")
        if not isinstance(authority, dict) or not authority.get(
            "canonical_fingerprint"
        ) or not authority.get("view_fingerprint"):
            raise RuntimeError("Robot Model Authority fingerprints are unavailable")
        return _capture_payload(
            program=program,
            visual_scene_state=visual_scene_state,
            authority_snapshot=authority_snapshot,
            planning_record=planning_record,
            run_id=run_id,
            planning_execution_id=planning_execution_id,
            program_path=program_path,
            deployment_path=deployment_path,
            repo_root=repo_root,
            model_manifest=model_manifest,
        )
    finally:
        original_error = sys.exception()
        stop_error: Exception | None = None
        if cycle_started:
            try:
                _quiesce_cycle(args.composer_url, 10.0)
            except Exception as exc:
                stop_error = exc
        if planning_capture_generation:
            try:
                _request_json(
                    args.composer_url,
                    "/api/debug/qualification/planning-capture/stop",
                    method="POST",
                    payload={
                        "generation": planning_capture_generation,
                        "idle_timeout_ms": 10_000,
                    },
                    timeout_s=15.0,
                )
            except Exception as exc:
                if stop_error is None:
                    stop_error = exc
        if stop_error is not None:
            if original_error is not None:
                original_error.add_note(
                    f"qualification cycle stop also failed: {stop_error}"
                )
            else:
                raise RuntimeError(
                    f"capture completed but cycle stop failed: {stop_error}"
                ) from stop_error


def main(argv: list[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument(
        "--program",
        type=Path,
        default=repo_root / "examples/isaac_manipulator_scene_light.program.yaml",
    )
    parser.add_argument(
        "--deployment",
        type=Path,
        default=repo_root
        / "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-s", type=float, default=180.0)
    parser.add_argument("--max-source-age-ms", type=float, default=250.0)
    parser.add_argument("--simulation-settle-s", type=float, default=3.0)
    args = parser.parse_args(argv)
    for field in ("timeout_s", "max_source_age_ms"):
        value = float(getattr(args, field))
        if not math.isfinite(value) or value <= 0.0:
            parser.error(f"--{field.replace('_', '-')} must be finite and positive")
    if (
        not math.isfinite(args.simulation_settle_s)
        or args.simulation_settle_s < 0.0
    ):
        parser.error("--simulation-settle-s must be finite and non-negative")
    try:
        payload = capture(args)
        write_evidence_exclusive(
            args.output,
            payload,
            validator=lambda path: validate_capture(
                path,
                expected_capture_id="isaac-light-drop-approach-v1",
                required_attachment="isaac_cheese_box",
            ),
        )
    except Exception as exc:
        print(f"RP5 Isaac capture failed: {exc}", file=sys.stderr)
        return 2
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
