#!/usr/bin/env python3
"""Collect Phase 4-P3 live Isaac commanded-sim evidence.

This probe is intentionally small and operator-auditable. It talks to a live
Composer instance, exercises the same direct-control WebSocket used by the UI,
and emits the JSON shape required by ``verify-kernel-architecture-phase4.py``
for strict Phase 4 live-Isaac evidence.

The probe assumes the Isaac commanded-sim lane is already running and ready.
It sends only small joint/cartesian frames, commands the declared gripper open
and closed, and finishes with a cycle reset so the command stream is refreshed
to the deployment initial state.
"""
from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass, field
import json
import math
from pathlib import Path
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlparse, urlunparse
from urllib.request import Request, urlopen


SCHEMA_VERSION = "manyforge.phase4_p3_commanded_sim.v1"
REQUIRED_CHECKS = {
    "runtime_profile_commanded_sim",
    "kernel_snapshot_authority_metadata",
    "scene_state_robot_authority_metadata",
    "streaming_collision_budget_configured",
    "joint_slider_accepted",
    "joint_spinner_input_accepted",
    "cartesian_gizmo_accepted",
    "cartesian_input_accepted",
    "gripper_open_close_commanded",
    "rejected_frame_recovery",
    "reset_home_command_stream_refresh",
}


@dataclass(slots=True)
class Check:
    name: str
    passed: bool
    details: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "passed": self.passed,
            "details": self.details,
        }


class Probe:
    def __init__(self, *, base_url: str, timeout_s: float) -> None:
        self.base_url = base_url.rstrip("/") + "/"
        self.timeout_s = timeout_s
        self.checks: list[Check] = []

    def add_check(self, name: str, passed: bool, **details: Any) -> None:
        self.checks.append(Check(name=name, passed=bool(passed), details=details))

    def get_json(self, path: str) -> dict[str, Any]:
        url = urljoin(self.base_url, path.lstrip("/"))
        request = Request(url, headers={"Accept": "application/json"})
        with urlopen(request, timeout=self.timeout_s) as response:
            body = response.read()
        return json.loads(body.decode("utf-8"))

    def post_json(self, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        url = urljoin(self.base_url, path.lstrip("/"))
        data = json.dumps(payload or {}).encode("utf-8")
        request = Request(
            url,
            data=data,
            method="POST",
            headers={
                "Accept": "application/json",
                "Content-Type": "application/json",
            },
        )
        with urlopen(request, timeout=self.timeout_s) as response:
            body = response.read()
        return json.loads(body.decode("utf-8"))

    def ws_url(self, path: str) -> str:
        parsed = urlparse(urljoin(self.base_url, path.lstrip("/")))
        scheme = "wss" if parsed.scheme == "https" else "ws"
        return urlunparse((scheme, parsed.netloc, parsed.path, "", "", ""))


def _lookup(mapping: dict[str, Any], *names: str, default: Any = None) -> Any:
    for name in names:
        if name in mapping:
            return mapping[name]
    return default


def _finite_float(value: Any) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def _runtime_profile(probe: Probe) -> tuple[str, str, dict[str, Any]]:
    runtime = probe.get_json("/api/runtime/status")
    profile_kind = str(runtime.get("profileKind") or runtime.get("profile_kind") or "")
    authority = str(runtime.get("authority") or "")
    readiness = str(runtime.get("readiness") or "")
    probe.add_check(
        "runtime_profile_commanded_sim",
        profile_kind == "commanded_sim" and authority == "backend_authored" and readiness == "ready",
        profile_kind=profile_kind,
        authority=authority,
        readiness=readiness,
        profile_id=runtime.get("profileId") or runtime.get("profile_id"),
    )
    return profile_kind, authority, runtime


def _execution_config(probe: Probe) -> dict[str, Any]:
    try:
        deployment = probe.get_json("/api/deployment/full")
    except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        probe.add_check("streaming_collision_budget_configured", False, error=str(exc))
        return {}
    adapters = deployment.get("adapters") or {}
    execution = adapters.get("execution") or {}
    config = execution.get("config") or {}
    timeout = _finite_float(
        config.get("streaming_collision_timeout_s")
        if "streaming_collision_timeout_s" in config
        else config.get("streaming_collision_timeout_ms")
    )
    timeout_s = timeout
    if timeout_s is not None and "streaming_collision_timeout_ms" in config and "streaming_collision_timeout_s" not in config:
        timeout_s = timeout_s / 1000.0
    probe.add_check(
        "streaming_collision_budget_configured",
        timeout_s is not None and timeout_s > 0.0,
        streaming_collision_timeout_s=timeout_s,
        execution_state_source_mode=config.get("execution_state_source_mode")
        or config.get("execution_state_authority_profile"),
    )
    return config


def _kernel_snapshot_authority(probe: Probe) -> dict[str, Any]:
    snapshot = probe.get_json("/api/kernel/snapshot")
    state_source = snapshot.get("state_source")
    timestamp_ns = snapshot.get("source_timestamp_ns")
    age_ns = snapshot.get("source_age_ns")
    probe.add_check(
        "kernel_snapshot_authority_metadata",
        isinstance(state_source, str)
        and state_source.startswith("commanded_sim")
        and isinstance(timestamp_ns, int)
        and timestamp_ns > 0
        and isinstance(age_ns, int)
        and age_ns >= 0,
        state_source=state_source,
        source_timestamp_ns=timestamp_ns,
        source_age_ns=age_ns,
    )
    return snapshot


def _scene_state(probe: Probe) -> dict[str, Any]:
    return probe.get_json("/api/scene/state")


def _scene_robot_authority(probe: Probe, scene: dict[str, Any]) -> dict[str, Any]:
    robot = scene.get("robot") or {}
    state_source = _lookup(robot, "state_source", "stateSource")
    timestamp_ns = _lookup(robot, "source_timestamp_ns", "sourceTimestampNs")
    age_ns = _lookup(robot, "source_age_ns", "sourceAgeNs")
    probe.add_check(
        "scene_state_robot_authority_metadata",
        isinstance(state_source, str)
        and state_source.startswith("commanded_sim")
        and isinstance(timestamp_ns, int)
        and timestamp_ns > 0
        and isinstance(age_ns, int)
        and age_ns >= 0,
        state_source=state_source,
        source_timestamp_ns=timestamp_ns,
        source_age_ns=age_ns,
    )
    return robot


async def _stream_frames(
    probe: Probe,
    *,
    begin: dict[str, Any],
    frames: list[dict[str, Any]],
) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any] | None]:
    try:
        import websockets
    except ImportError as exc:  # pragma: no cover - depends on runtime image
        raise RuntimeError(
            "the Python 'websockets' package is required for the live P3 probe"
        ) from exc

    url = probe.ws_url("/ws/follow-target")
    async with websockets.connect(url, open_timeout=probe.timeout_s) as ws:
        await ws.send(json.dumps({"op": "begin", **begin}))
        began = json.loads(await asyncio.wait_for(ws.recv(), timeout=probe.timeout_s))
        pushed: list[dict[str, Any]] = []
        if began.get("op") != "began":
            return began, pushed, None
        for frame in frames:
            await ws.send(json.dumps({"op": "frame", **frame}))
            pushed.append(json.loads(await asyncio.wait_for(ws.recv(), timeout=probe.timeout_s)))
        await ws.send(json.dumps({"op": "end", "soft_stop_strategy": "spring_back"}))
        ended: dict[str, Any] | None = None
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=probe.timeout_s)
            ended = json.loads(raw)
        except Exception:
            ended = None
        return began, pushed, ended


def _joint_positions(robot: dict[str, Any]) -> tuple[list[str], list[float]]:
    names = list(robot.get("joint_names") or robot.get("jointNames") or [])
    positions_map = robot.get("joint_positions") or robot.get("jointPositions") or {}
    if not isinstance(positions_map, dict):
        positions_map = {}
    positions = [float(positions_map[name]) for name in names if name in positions_map]
    if len(positions) != len(names):
        raise RuntimeError("scene-state robot does not contain a complete joint map")
    return [str(name) for name in names], positions


def _target_with_delta(positions: list[float], delta_rad: float) -> list[float]:
    target = list(positions)
    if not target:
        raise RuntimeError("cannot build a target without joints")
    target[-1] += delta_rad
    return target


def _joint_close_enough(
    scene: dict[str, Any],
    joint_name: str,
    target: float,
    tolerance_rad: float,
) -> bool:
    robot = scene.get("robot") or {}
    positions = robot.get("joint_positions") or robot.get("jointPositions") or {}
    actual = _finite_float(positions.get(joint_name)) if isinstance(positions, dict) else None
    return actual is not None and abs(actual - target) <= tolerance_rad


async def _accepted_joint_probe(
    probe: Probe,
    *,
    check_name: str,
    trigger_reason: str,
    robot: dict[str, Any],
    delta_rad: float,
    tolerance_rad: float,
) -> list[float]:
    names, positions = _joint_positions(robot)
    target = _target_with_delta(positions, delta_rad)
    begin, pushed, ended = await _stream_frames(
        probe,
        begin={
            "input_mode": "joint",
            "joint_names": names,
            "principal_id": "phase4_p3_probe",
            "authority_source": "human",
            "trigger_reason": trigger_reason,
        },
        frames=[
            {
                "kind": "joint",
                "joint_names": names,
                "joint_positions_rad": target,
                "source_timestamp_ns": time.monotonic_ns(),
                "source_sequence": 1,
            }
        ],
    )
    ack = pushed[-1] if pushed else {}
    scene_after = _scene_state(probe)
    applied = _joint_close_enough(scene_after, names[-1], target[-1], tolerance_rad)
    probe.add_check(
        check_name,
        begin.get("op") == "began" and bool(ack.get("accepted")) and applied,
        began=begin,
        ack=ack,
        ended=ended,
        checked_joint=names[-1],
        target=target[-1],
        scene_state_applied=applied,
    )
    return target


async def _accepted_cartesian_probe(
    probe: Probe,
    *,
    check_name: str,
    trigger_reason: str,
    robot: dict[str, Any],
) -> None:
    tcp = robot.get("tcp_pose_in_base") or robot.get("tcpPoseInBase") or {}
    position = tcp.get("position_m") or tcp.get("positionM")
    orientation = tcp.get("orientation_xyzw") or tcp.get("orientationXyzw")
    begin, pushed, ended = await _stream_frames(
        probe,
        begin={
            "input_mode": "cartesian",
            "cartesian_reference_frame": "base",
            "tcp_frame": str(robot.get("tcp_frame") or robot.get("tcpFrame") or ""),
            "principal_id": "phase4_p3_probe",
            "authority_source": "human",
            "trigger_reason": trigger_reason,
        },
        frames=[
            {
                "kind": "cartesian",
                "position_m": position,
                "orientation_xyzw": orientation,
                "source_timestamp_ns": time.monotonic_ns(),
                "source_sequence": 1,
            }
        ],
    )
    ack = pushed[-1] if pushed else {}
    probe.add_check(
        check_name,
        begin.get("op") == "began" and bool(ack.get("accepted")),
        began=begin,
        ack=ack,
        ended=ended,
        tcp_frame=robot.get("tcp_frame") or robot.get("tcpFrame"),
    )


async def _rejected_frame_recovery(probe: Probe, robot: dict[str, Any]) -> None:
    names, positions = _joint_positions(robot)
    begin, pushed, ended = await _stream_frames(
        probe,
        begin={
            "input_mode": "joint",
            "joint_names": names,
            "principal_id": "phase4_p3_probe",
            "authority_source": "human",
            "trigger_reason": "phase4_p3_rejected_frame_recovery",
        },
        frames=[
            {
                "kind": "joint",
                "joint_names": names,
                "joint_positions_rad": [],
                "source_timestamp_ns": time.monotonic_ns(),
                "source_sequence": 1,
            },
            {
                "kind": "joint",
                "joint_names": names,
                "joint_positions_rad": positions,
                "source_timestamp_ns": time.monotonic_ns(),
                "source_sequence": 2,
            },
        ],
    )
    rejected = pushed[0] if len(pushed) >= 1 else {}
    recovered = pushed[1] if len(pushed) >= 2 else {}
    cycle = probe.get_json("/api/cycle/status")
    probe.add_check(
        "rejected_frame_recovery",
        begin.get("op") == "began"
        and rejected.get("accepted") is False
        and bool(recovered.get("accepted"))
        and str(cycle.get("state")) in {"idle", "stopped"},
        began=begin,
        rejected_ack=rejected,
        recovered_ack=recovered,
        ended=ended,
        cycle_state=cycle.get("state"),
    )


def _command_gripper(
    probe: Probe,
    *,
    gripper_id: str,
    target: float,
    duration_s: float,
) -> dict[str, Any]:
    return probe.post_json(
        "/api/scene/gripper/command",
        {
            "gripperId": gripper_id,
            "targetPosition": target,
            "durationS": duration_s,
        },
    )


def _wait_gripper_not_executing(
    probe: Probe,
    *,
    gripper_id: str,
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_state: dict[str, Any] = {}
    while time.monotonic() <= deadline:
        scene = _scene_state(probe)
        for item in scene.get("grippers") or []:
            item_id = str(item.get("gripper_id") or item.get("gripperId") or "")
            if item_id != gripper_id:
                continue
            last_state = item
            state_name = str(item.get("state") or "").lower()
            if "executing" not in state_name:
                return item
        time.sleep(0.05)
    return last_state


def _gripper_open_close(probe: Probe, scene: dict[str, Any]) -> None:
    grippers = scene.get("grippers") or []
    if not grippers:
        probe.add_check("gripper_open_close_commanded", False, error="no grippers declared")
        return
    gripper = grippers[0]
    gripper_id = str(gripper.get("gripper_id") or gripper.get("gripperId") or "")
    presets = gripper.get("srdf_presets") or gripper.get("srdfPresets") or {}
    open_target = _finite_float(presets.get("open"))
    closed_target = _finite_float(presets.get("closed"))
    if open_target is None:
        open_target = _finite_float(gripper.get("limit_lower") or gripper.get("limitLower"))
    if closed_target is None:
        closed_target = _finite_float(gripper.get("limit_upper") or gripper.get("limitUpper"))
    if not gripper_id or open_target is None or closed_target is None:
        probe.add_check(
            "gripper_open_close_commanded",
            False,
            error="gripper target presets/limits unavailable",
            gripper=gripper,
        )
        return
    command_duration_s = 0.2
    first = _command_gripper(
        probe,
        gripper_id=gripper_id,
        target=closed_target,
        duration_s=command_duration_s,
    )
    # ROS action terminal state and the Composer scene-state cache can be
    # briefly out of phase: the scene can still report the previous completed
    # target while the new action is active. Respect the commanded duration
    # before polling the status surface so the second command proves the
    # gripper route is reusable rather than racing the first action.
    time.sleep(command_duration_s + 0.35)
    after_close = _wait_gripper_not_executing(
        probe,
        gripper_id=gripper_id,
        timeout_s=probe.timeout_s,
    )
    second = _command_gripper(
        probe,
        gripper_id=gripper_id,
        target=open_target,
        duration_s=command_duration_s,
    )
    time.sleep(command_duration_s + 0.35)
    after_state = _wait_gripper_not_executing(
        probe,
        gripper_id=gripper_id,
        timeout_s=probe.timeout_s,
    )
    visual = after_state.get("visualization") or {}
    after_close_state_name = str(after_close.get("state") or "").lower()
    final_state_name = str(after_state.get("state") or "").lower()
    probe.add_check(
        "gripper_open_close_commanded",
        bool(first.get("accepted"))
        and bool(second.get("accepted"))
        and "executing" not in after_close_state_name
        and "failed" not in after_close_state_name
        and "executing" not in final_state_name
        and "failed" not in final_state_name
        and isinstance(visual, dict)
        and bool(visual.get("subtree_links") or visual.get("subtreeLinks")),
        gripper_id=gripper_id,
        closed_response=first,
        after_close_state=after_close.get("state"),
        open_response=second,
        final_state=after_state.get("state"),
        final_target=after_state.get("target_position") or after_state.get("targetPosition"),
        visualization_links=len(visual.get("subtree_links") or visual.get("subtreeLinks") or []),
    )


def _reset_home(probe: Probe, initial_positions: dict[str, float], tolerance_rad: float) -> None:
    response = probe.post_json("/api/cycle/reset")
    time.sleep(0.2)
    scene = _scene_state(probe)
    robot = scene.get("robot") or {}
    positions = robot.get("joint_positions") or robot.get("jointPositions") or {}
    matched = True
    diffs: dict[str, float] = {}
    for name, target in initial_positions.items():
        actual = _finite_float(positions.get(name)) if isinstance(positions, dict) else None
        if actual is None:
            matched = False
            diffs[name] = math.inf
            continue
        diff = abs(actual - target)
        diffs[name] = diff
        matched = matched and diff <= tolerance_rad
    source = _lookup(robot, "state_source", "stateSource")
    probe.add_check(
        "reset_home_command_stream_refresh",
        bool(response.get("state") == "idle")
        and matched
        and isinstance(source, str)
        and source.startswith("commanded_sim"),
        reset_response=response,
        max_joint_error_rad=max(diffs.values(), default=0.0),
        state_source=source,
    )


async def run_probe(args: argparse.Namespace) -> dict[str, Any]:
    probe = Probe(base_url=args.base_url, timeout_s=args.timeout_s)
    started = time.monotonic()

    profile_kind, authority, runtime = _runtime_profile(probe)
    execution_config = _execution_config(probe)
    _kernel_snapshot_authority(probe)
    scene = _scene_state(probe)
    robot = _scene_robot_authority(probe, scene)
    names, positions = _joint_positions(robot)
    initial_positions = {name: pos for name, pos in zip(names, positions)}

    await _accepted_joint_probe(
        probe,
        check_name="joint_slider_accepted",
        trigger_reason="phase4_p3_joint_slider",
        robot=robot,
        delta_rad=args.joint_delta_rad,
        tolerance_rad=args.joint_tolerance_rad,
    )
    scene = _scene_state(probe)
    robot = scene.get("robot") or robot
    await _accepted_joint_probe(
        probe,
        check_name="joint_spinner_input_accepted",
        trigger_reason="phase4_p3_joint_spinner_input",
        robot=robot,
        delta_rad=-args.joint_delta_rad,
        tolerance_rad=args.joint_tolerance_rad,
    )
    scene = _scene_state(probe)
    robot = scene.get("robot") or robot
    await _accepted_cartesian_probe(
        probe,
        check_name="cartesian_gizmo_accepted",
        trigger_reason="phase4_p3_cartesian_gizmo",
        robot=robot,
    )
    scene = _scene_state(probe)
    robot = scene.get("robot") or robot
    await _accepted_cartesian_probe(
        probe,
        check_name="cartesian_input_accepted",
        trigger_reason="phase4_p3_cartesian_input",
        robot=robot,
    )
    scene = _scene_state(probe)
    _gripper_open_close(probe, scene)
    scene = _scene_state(probe)
    await _rejected_frame_recovery(probe, scene.get("robot") or robot)
    _reset_home(probe, initial_positions, args.joint_tolerance_rad)

    check_map = {check.name: check.passed for check in probe.checks}
    missing = sorted(REQUIRED_CHECKS - set(check_map))
    failed = [check.name for check in probe.checks if not check.passed]
    success = not missing and not failed
    return {
        "schema_version": SCHEMA_VERSION,
        "success": success,
        "phase": "4-P3",
        "scope": "live_isaac_commanded_sim_state_authority",
        "base_url": args.base_url,
        "profile_kind": profile_kind,
        "authority": authority,
        "runtime": {
            "profileId": runtime.get("profileId"),
            "readiness": runtime.get("readiness"),
            "summaryLabel": runtime.get("summaryLabel"),
        },
        "execution_config": {
            "execution_state_source_mode": execution_config.get("execution_state_source_mode"),
            "streaming_collision_timeout_s": execution_config.get("streaming_collision_timeout_s"),
            "streaming_collision_timeout_ms": execution_config.get("streaming_collision_timeout_ms"),
        },
        "checks": [check.to_json() for check in probe.checks],
        "missing_checks": missing,
        "failures": failed + [f"missing:{name}" for name in missing],
        "duration_s": round(time.monotonic() - started, 3),
        "timestamp_ns": time.monotonic_ns(),
    }


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:9000")
    parser.add_argument("--timeout-s", type=float, default=5.0)
    parser.add_argument("--joint-delta-rad", type=float, default=0.01)
    parser.add_argument("--joint-tolerance-rad", type=float, default=0.03)
    parser.add_argument("--summary-json", type=Path)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        result = asyncio.run(run_probe(args))
    except Exception as exc:
        result = {
            "schema_version": SCHEMA_VERSION,
            "success": False,
            "phase": "4-P3",
            "scope": "live_isaac_commanded_sim_state_authority",
            "base_url": args.base_url,
            "profile_kind": "",
            "authority": "",
            "checks": [],
            "missing_checks": sorted(REQUIRED_CHECKS),
            "failures": [f"{exc.__class__.__name__}: {exc}"],
            "timestamp_ns": time.monotonic_ns(),
        }
    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.summary_json is not None:
        args.summary_json.parent.mkdir(parents=True, exist_ok=True)
        args.summary_json.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0 if result.get("success") else 3


if __name__ == "__main__":
    sys.exit(main())
