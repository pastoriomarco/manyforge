#!/usr/bin/env python3
"""Validate a recorded Isaac Lite6 live-smoke JSON artifact.

The live smoke script proves the command surface while Isaac is running. This
checker is intentionally ROS-free: it lets CI or a reviewer accept/reject a
recorded artifact after the live run without replaying the simulator.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
import math
from pathlib import Path
from typing import Any, Sequence


EXPECTED_SCHEMA = "manyforge.isaac_lite6_live_smoke.v1"
EXPECTED_ARM_JOINTS = ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
EXPECTED_GRIPPER_JOINT = "right_finger_joint"


class ValidationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ValidationError(message)


def require_dict(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{path} must be an object")
    return value


def require_list(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        fail(f"{path} must be a list")
    return value


def require_bool(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        fail(f"{path} must be a boolean")
    return value


def require_string(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{path} must be a non-empty string")
    return value


def require_number(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{path} must be a finite number")
    result = float(value)
    if not math.isfinite(result):
        fail(f"{path} must be finite")
    return result


def parse_timestamp(value: Any, path: str) -> datetime:
    timestamp = require_string(value, path)
    try:
        parsed = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValidationError(f"{path} must be ISO-8601: {exc}") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        fail(f"{path} must include a timezone offset")
    return parsed


def require_joint_map(
    value: Any, path: str, joints: Sequence[str] = EXPECTED_ARM_JOINTS
) -> dict[str, float]:
    mapping = require_dict(value, path)
    missing = [joint for joint in joints if joint not in mapping]
    if missing:
        fail(f"{path} missing joints: {', '.join(missing)}")
    return {
        joint: require_number(mapping[joint], f"{path}.{joint}") for joint in joints
    }


def require_arm_result(
    value: Any, path: str, max_target_error_rad: float, error_tolerance: float
) -> dict[str, Any]:
    result = require_dict(value, path)
    target = require_joint_map(
        result.get("target_positions_rad"), f"{path}.target_positions_rad"
    )
    final_arm = require_joint_map(
        result.get("final_arm_state_rad"), f"{path}.final_arm_state_rad"
    )
    final_isaac = result.get("final_isaac_state_rad")
    if final_isaac is not None:
        require_joint_map(final_isaac, f"{path}.final_isaac_state_rad")
    reported_max_error = require_number(
        result.get("max_abs_target_error_rad"), f"{path}.max_abs_target_error_rad"
    )
    computed_max_error = max(
        abs(final_arm[joint] - target[joint]) for joint in EXPECTED_ARM_JOINTS
    )
    if abs(reported_max_error - computed_max_error) > error_tolerance:
        fail(
            f"{path}.max_abs_target_error_rad={reported_max_error:.6g} does "
            f"not match computed error {computed_max_error:.6g}"
        )
    if computed_max_error > max_target_error_rad:
        fail(
            f"{path}.computed_max_abs_target_error_rad={computed_max_error:.6g} exceeds "
            f"{max_target_error_rad:.6g}"
        )
    return result


def require_gripper_result(
    value: Any,
    path: str,
    *,
    transport_kind: str,
    expected_target: float,
    command_tolerance: float,
    expected_effort: float | None = None,
    expected_dwell_s: float | None = None,
) -> None:
    result = require_dict(value, path)
    result_transport_kind = result.get("transport_kind", transport_kind)
    if result_transport_kind != transport_kind:
        fail(
            f"{path}.transport_kind={result_transport_kind!r} does not match "
            f"details.gripper_transport_kind={transport_kind!r}"
        )
    target = require_number(result.get("target_position"), f"{path}.target_position")
    if abs(target - expected_target) > command_tolerance:
        fail(f"{path}.target_position={target:.6g} does not match expected target")
    if transport_kind == "ros_gripper_command":
        raw_command = require_number(
            result.get("raw_command_position"), f"{path}.raw_command_position"
        )
        if abs(raw_command - expected_target) > command_tolerance:
            fail(
                f"{path}.raw_command_position={raw_command:.6g} does not match "
                f"expected target {expected_target:.6g}"
            )
        require_number(result.get("result_position"), f"{path}.result_position")
        if not require_bool(result.get("reached_goal"), f"{path}.reached_goal"):
            fail(f"{path}.reached_goal must be true")
        if require_bool(result.get("stalled"), f"{path}.stalled"):
            fail(f"{path}.stalled must be false")
    elif transport_kind == "isaac_joint_state_effort":
        if result.get("actuation_kind") != "pneumatic":
            fail(f"{path}.actuation_kind must be pneumatic for direct effort")
        if result.get("state_authority") != "sim_observed":
            fail(f"{path}.state_authority must be sim_observed for direct effort")
        if result.get("completion_policy") != "dwell_elapsed":
            fail(f"{path}.completion_policy must be dwell_elapsed for direct effort")
        raw_command = require_number(
            result.get("raw_command_position"), f"{path}.raw_command_position"
        )
        if abs(raw_command - expected_target) > command_tolerance:
            fail(
                f"{path}.raw_command_position={raw_command:.6g} does not "
                f"match expected target {expected_target:.6g}"
            )
        raw_effort = require_number(
            result.get("raw_command_effort"), f"{path}.raw_command_effort"
        )
        if (
            expected_effort is not None
            and abs(raw_effort - expected_effort) > command_tolerance
        ):
            fail(
                f"{path}.raw_command_effort={raw_effort:.6g} does not match "
                f"expected effort {expected_effort:.6g}"
            )
        dwell_s = require_number(result.get("dwell_s"), f"{path}.dwell_s")
        dwell_elapsed_s = require_number(
            result.get("dwell_elapsed_s"), f"{path}.dwell_elapsed_s"
        )
        if expected_dwell_s is not None and abs(dwell_s - expected_dwell_s) > 1e-6:
            fail(
                f"{path}.dwell_s={dwell_s:.6g} does not match configured dwell "
                f"{expected_dwell_s:.6g}"
            )
        if dwell_elapsed_s + 1e-3 < dwell_s:
            fail(
                f"{path}.dwell_elapsed_s={dwell_elapsed_s:.6g} is shorter than "
                f"dwell_s={dwell_s:.6g}"
            )
        require_number(
            result.get("isaac_state_position"), f"{path}.isaac_state_position"
        )
    else:
        fail(f"unsupported gripper transport kind: {transport_kind}")
    if transport_kind != "isaac_joint_state_effort" and result.get(
        "isaac_state_position"
    ) is not None:
        require_number(
            result.get("isaac_state_position"), f"{path}.isaac_state_position"
        )


def validate_artifact(payload: dict[str, Any], args: argparse.Namespace) -> None:
    if not require_bool(payload.get("passed"), "passed"):
        fail("passed must be true for qualification")
    details = require_dict(payload.get("details"), "details")
    if details.get("schema") != EXPECTED_SCHEMA:
        fail(f"details.schema must be {EXPECTED_SCHEMA}")
    started_at = parse_timestamp(
        details.get("started_at_utc"), "details.started_at_utc"
    )
    ended_at = parse_timestamp(details.get("ended_at_utc"), "details.ended_at_utc")
    if ended_at < started_at:
        fail("details.ended_at_utc must be greater than or equal to started_at_utc")

    arm_joints = require_list(details.get("arm_joints"), "details.arm_joints")
    if arm_joints != EXPECTED_ARM_JOINTS:
        fail(f"details.arm_joints must be {EXPECTED_ARM_JOINTS}")
    if details.get("gripper_joint") != EXPECTED_GRIPPER_JOINT:
        fail(f"details.gripper_joint must be {EXPECTED_GRIPPER_JOINT}")
    require_string(details.get("arm_action"), "details.arm_action")
    transport_kind = str(details.get("gripper_transport_kind") or "ros_gripper_command")
    if transport_kind not in {"ros_gripper_command", "isaac_joint_state_effort"}:
        fail(
            "details.gripper_transport_kind must be ros_gripper_command "
            "or isaac_joint_state_effort"
        )
    if transport_kind == "ros_gripper_command":
        require_string(details.get("gripper_action"), "details.gripper_action")
    require_string(details.get("arm_joint_state_topic"), "details.arm_joint_state_topic")
    require_string(
        details.get("isaac_joint_state_topic"), "details.isaac_joint_state_topic"
    )
    require_string(details.get("gripper_command_topic"), "details.gripper_command_topic")

    start = require_joint_map(
        details.get("start_positions_rad"), "details.start_positions_rad"
    )
    probe = require_joint_map(
        details.get("probe_positions_rad"), "details.probe_positions_rad"
    )
    arm_delta = require_number(details.get("arm_delta_rad"), "details.arm_delta_rad")
    require_number(details.get("arm_motion_duration_s"), "details.arm_motion_duration_s")
    if abs((probe["joint1"] - start["joint1"]) - arm_delta) > args.position_tolerance:
        fail("details.probe_positions_rad.joint1 does not match arm_delta_rad")
    for joint in EXPECTED_ARM_JOINTS[1:]:
        if abs(probe[joint] - start[joint]) > args.position_tolerance:
            fail(f"details.probe_positions_rad.{joint} changed unexpectedly")

    probe_result = require_arm_result(
        details.get("arm_probe_result"),
        "details.arm_probe_result",
        args.max_arm_target_error_rad,
        args.position_tolerance,
    )
    restore_result = require_arm_result(
        details.get("arm_restore_result"),
        "details.arm_restore_result",
        args.max_arm_target_error_rad,
        args.position_tolerance,
    )
    restore_target = require_joint_map(
        restore_result.get("target_positions_rad"),
        "details.arm_restore_result.target_positions_rad",
    )
    for joint, value in start.items():
        if abs(restore_target[joint] - value) > args.position_tolerance:
            fail(f"restore target for {joint} does not match start position")
    probe_target = require_joint_map(
        probe_result.get("target_positions_rad"),
        "details.arm_probe_result.target_positions_rad",
    )
    for joint, value in probe.items():
        if abs(probe_target[joint] - value) > args.position_tolerance:
            fail(f"probe target for {joint} does not match probe position")

    close_target = require_number(
        details.get("gripper_close_position"), "details.gripper_close_position"
    )
    open_target = require_number(
        details.get("gripper_open_position"), "details.gripper_open_position"
    )
    close_effort = (
        require_number(
            details.get("gripper_close_effort"), "details.gripper_close_effort"
        )
        if transport_kind == "isaac_joint_state_effort"
        else None
    )
    open_effort = (
        require_number(
            details.get("gripper_open_effort"), "details.gripper_open_effort"
        )
        if transport_kind == "isaac_joint_state_effort"
        else None
    )
    dwell_s = (
        require_number(details.get("gripper_dwell_s"), "details.gripper_dwell_s")
        if transport_kind == "isaac_joint_state_effort"
        else None
    )
    require_gripper_result(
        details.get("gripper_close_result"),
        "details.gripper_close_result",
        transport_kind=transport_kind,
        expected_target=close_target,
        command_tolerance=args.gripper_command_tolerance,
        expected_effort=close_effort,
        expected_dwell_s=dwell_s,
    )
    require_gripper_result(
        details.get("gripper_open_result"),
        "details.gripper_open_result",
        transport_kind=transport_kind,
        expected_target=open_target,
        command_tolerance=args.gripper_command_tolerance,
        expected_effort=open_effort,
        expected_dwell_s=dwell_s,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", help="Path to the Lite6 live-smoke JSON artifact")
    parser.add_argument("--json", action="store_true", help="Emit validation result JSON")
    parser.add_argument(
        "--max-arm-target-error-rad",
        type=float,
        default=0.05,
        help="Maximum allowed final arm target error for the smoke probe/restore.",
    )
    parser.add_argument(
        "--position-tolerance",
        type=float,
        default=1e-9,
        help="Tolerance for internal target/metadata consistency checks.",
    )
    parser.add_argument(
        "--gripper-command-tolerance",
        type=float,
        default=1e-9,
        help="Tolerance for raw gripper bridge command vs requested target.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    artifact = Path(args.artifact)
    try:
        payload = json.loads(artifact.read_text(encoding="utf-8"))
        validate_artifact(require_dict(payload, "artifact"), args)
    except Exception as exc:  # noqa: BLE001 - CLI validation surface
        if args.json:
            print(json.dumps({"passed": False, "error": str(exc)}, indent=2))
        else:
            print(f"Lite6 live smoke artifact rejected: {exc}")
        return 1

    if args.json:
        print(json.dumps({"passed": True, "artifact": str(artifact)}, indent=2))
    else:
        print(f"Lite6 live smoke artifact accepted: {artifact}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
