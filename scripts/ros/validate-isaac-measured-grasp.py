#!/usr/bin/env python3
"""Live Isaac qualification for measured grasp-integrity monitoring.

The light-scene Isaac demo does not physically constrain the mac-and-cheese
object to the Robotiq gripper. ManyForge's planning scene can attach and carry
the object, but Isaac's measured object entity stays behind unless a test
harness moves it. That is exactly what the measured-drift monitor should detect.

This qualifier therefore runs two phases in one program execution:

1. Once ManyForge attaches the object, capture the external TCP->object baseline
   from the same pose-source snapshot and use ROS ``SetEntityState`` to keep the
   measured Isaac object at ``measured_tcp * baseline``. The monitor should stay
   ``ok`` while the object is externally consistent with the gripper.
2. Inject a bounded external measured-object offset while the object is still
   attached. The diagnostic-only drift monitor should report ``exceeded``
   without requiring a product fault.

The script is a qualification harness, not runtime behavior. It deliberately
uses the simulator control service directly because the public Composer API is
correctly guarded against arbitrary scene mutation during an active cycle.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Sequence

import rclpy

from manyforge_ros.simulation_control import SimulationControlClient


DEFAULT_BASE_URL = "http://127.0.0.1:9000"
DEFAULT_SOURCE_ID = "isaac_scene_objects"
DEFAULT_OBJECT_ID = "isaac_cheese_box"
DEFAULT_ENTITY = "/mac_n_cheese_centered"


@dataclass
class EvidenceRow:
    t_s: float
    cycle_state: str
    attached: bool
    drift_status: str
    baseline_captured: bool
    translation_m: float | None
    sample_time_delta_s: float | None
    condition_raised: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "t_s": self.t_s,
            "cycle_state": self.cycle_state,
            "attached": self.attached,
            "drift_status": self.drift_status,
            "baseline_captured": self.baseline_captured,
            "translation_m": self.translation_m,
            "sample_time_delta_s": self.sample_time_delta_s,
            "condition_raised": self.condition_raised,
        }


@dataclass
class QualificationResult:
    passed: bool
    healthy_ok_samples: int
    diagnostic_exceeded: bool
    max_healthy_translation_m: float
    evidence: list[EvidenceRow] = field(default_factory=list)
    failure_reason: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "passed": self.passed,
            "healthy_ok_samples": self.healthy_ok_samples,
            "diagnostic_exceeded": self.diagnostic_exceeded,
            "max_healthy_translation_m": self.max_healthy_translation_m,
            "failure_reason": self.failure_reason,
            "evidence": [row.to_dict() for row in self.evidence],
        }


def _http_json(
    method: str,
    url: str,
    *,
    payload: dict[str, Any] | None = None,
    timeout_s: float = 5.0,
) -> tuple[int, Any]:
    data = None
    headers: dict[str, str] = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            body = response.read().decode("utf-8")
            return response.status, json.loads(body) if body else None
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        try:
            parsed: Any = json.loads(body) if body else None
        except json.JSONDecodeError:
            parsed = body
        return exc.code, parsed


def _get(base_url: str, path: str) -> Any:
    status, payload = _http_json("GET", base_url + path)
    if not (200 <= status < 300):
        raise RuntimeError(f"GET {path} failed: HTTP {status} {payload}")
    return payload


def _post(
    base_url: str,
    path: str,
    payload: dict[str, Any] | None = None,
    *,
    timeout_s: float = 5.0,
) -> Any:
    status, response = _http_json(
        "POST",
        base_url + path,
        payload=payload,
        timeout_s=timeout_s,
    )
    if not (200 <= status < 300):
        raise RuntimeError(f"POST {path} failed: HTTP {status} {response}")
    return response


def _quat_normalize(q: Sequence[float]) -> tuple[float, float, float, float]:
    x, y, z, w = (float(v) for v in q)
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm <= 0.0 or not math.isfinite(norm):
        raise ValueError("invalid quaternion")
    return x / norm, y / norm, z / norm, w / norm


def _pose_to_matrix(pose: dict[str, Any]) -> list[list[float]]:
    position = (
        pose.get("position_xyz")
        or pose.get("positionXYZ")
        or pose.get("position_m")
    )
    orientation = pose.get("orientation_xyzw") or pose.get("orientationXYZW")
    if not isinstance(position, list) or len(position) != 3:
        raise ValueError("pose position must have length 3")
    if not isinstance(orientation, list) or len(orientation) != 4:
        raise ValueError("pose orientation must have length 4")
    x, y, z, w = _quat_normalize(orientation)
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return [
        [
            1.0 - 2.0 * (yy + zz),
            2.0 * (xy - wz),
            2.0 * (xz + wy),
            float(position[0]),
        ],
        [
            2.0 * (xy + wz),
            1.0 - 2.0 * (xx + zz),
            2.0 * (yz - wx),
            float(position[1]),
        ],
        [
            2.0 * (xz - wy),
            2.0 * (yz + wx),
            1.0 - 2.0 * (xx + yy),
            float(position[2]),
        ],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _matrix_multiply(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    return [
        [sum(a[row][k] * b[k][col] for k in range(4)) for col in range(4)]
        for row in range(4)
    ]


def _matrix_inverse(transform: list[list[float]]) -> list[list[float]]:
    rotation_t = [[transform[col][row] for col in range(3)] for row in range(3)]
    translation = [transform[row][3] for row in range(3)]
    inv_translation = [
        -sum(rotation_t[row][col] * translation[col] for col in range(3))
        for row in range(3)
    ]
    return [
        [rotation_t[0][0], rotation_t[0][1], rotation_t[0][2], inv_translation[0]],
        [rotation_t[1][0], rotation_t[1][1], rotation_t[1][2], inv_translation[1]],
        [rotation_t[2][0], rotation_t[2][1], rotation_t[2][2], inv_translation[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _matrix_to_quat(matrix: list[list[float]]) -> list[float]:
    m00, m01, m02 = matrix[0][0], matrix[0][1], matrix[0][2]
    m10, m11, m12 = matrix[1][0], matrix[1][1], matrix[1][2]
    m20, m21, m22 = matrix[2][0], matrix[2][1], matrix[2][2]
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (m21 - m12) / s
        y = (m02 - m20) / s
        z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        w = (m21 - m12) / s
        x = 0.25 * s
        y = (m01 + m10) / s
        z = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        w = (m02 - m20) / s
        x = (m01 + m10) / s
        y = 0.25 * s
        z = (m12 + m21) / s
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        w = (m10 - m01) / s
        x = (m02 + m20) / s
        y = (m12 + m21) / s
        z = 0.25 * s
    return list(_quat_normalize((x, y, z, w)))


def _matrix_to_pose(matrix: list[list[float]], frame_id: str) -> dict[str, Any]:
    return {
        "frame_id": frame_id,
        "position_m": [matrix[0][3], matrix[1][3], matrix[2][3]],
        "orientation_xyzw": _matrix_to_quat(matrix),
    }


def _find_pose_source(scene: dict[str, Any], source_id: str) -> dict[str, Any]:
    for source in scene.get("pose_sources", []):
        if (source.get("pose_source_id") or source.get("id")) == source_id:
            return source
    raise RuntimeError(f"pose source not found: {source_id}")


def _find_object(scene: dict[str, Any], object_id: str) -> dict[str, Any]:
    for obj in scene.get("objects", []):
        if obj.get("object_id") == object_id:
            return obj
    raise RuntimeError(f"scene object not found: {object_id}")


def _find_pose_source_object(source: dict[str, Any], object_id: str) -> dict[str, Any]:
    for obj in source.get("objects", []):
        if obj.get("object_id") == object_id:
            return obj
    raise RuntimeError(f"pose-source object not found: {object_id}")


def _scene_measurements(
    base_url: str,
    *,
    source_id: str,
    object_id: str,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    scene = _get(base_url, "/api/scene/state")
    source = _find_pose_source(scene, source_id)
    source_object = _find_pose_source_object(source, object_id)
    measured_tcp = source.get("measured_tcp") or source.get("measuredTcp")
    if not isinstance(measured_tcp, dict):
        raise RuntimeError("measuredTcp is missing from scene state")
    return scene, source, source_object, measured_tcp


def _cycle_state(base_url: str) -> str:
    return str(_get(base_url, "/api/cycle/status").get("state") or "")


def _evidence_row(
    *,
    started_s: float,
    cycle_state: str,
    scene_object: dict[str, Any],
    source_object: dict[str, Any],
) -> EvidenceRow:
    drift = source_object.get("attached_drift") or source_object.get("attachedDrift") or {}
    return EvidenceRow(
        t_s=round(time.monotonic() - started_s, 3),
        cycle_state=cycle_state,
        attached=bool(scene_object.get("attached")),
        drift_status=str(drift.get("status") or ""),
        baseline_captured=bool(
            drift.get("baseline_captured") or drift.get("baselineCaptured")
        ),
        translation_m=(
            float(drift["translation_m"])
            if drift.get("translation_m") is not None
            else None
        ),
        sample_time_delta_s=(
            float(drift["sample_time_delta_s"])
            if drift.get("sample_time_delta_s") is not None
            else None
        ),
        condition_raised=bool(
            drift.get("condition_raised") or drift.get("conditionRaised")
        ),
    )


def _object_pose(source_object: dict[str, Any]) -> dict[str, Any]:
    update = source_object.get("update")
    if not isinstance(update, dict):
        raise RuntimeError("object pose-source update is missing")
    pose = update.get("pose")
    if not isinstance(pose, dict):
        raise RuntimeError("object pose-source update.pose is missing")
    return pose


def _tcp_pose(measured_tcp: dict[str, Any]) -> dict[str, Any]:
    pose = measured_tcp.get("pose")
    if not isinstance(pose, dict):
        raise RuntimeError("measuredTcp.pose is missing")
    return pose


def _qualify(args: argparse.Namespace) -> QualificationResult:
    base_url = args.base_url.rstrip("/")
    rclpy.init()
    node = rclpy.create_node("manyforge_isaac_measured_grasp_qualifier")
    sim_client = SimulationControlClient(
        node,
        service_prefix=args.service_prefix,
        service_timeout_s=args.service_timeout_s,
    )
    evidence: list[EvidenceRow] = []
    healthy_translations: list[float] = []
    baseline_tcp_to_object: list[list[float]] | None = None
    healthy_started_s: float | None = None
    inject_diagnostic_offset = False
    diagnostic_exceeded = False
    failure_reason = ""
    started_s = time.monotonic()

    try:
        status = _get(base_url, "/api/runtime/status")
        if status.get("readiness") != "ready" or not status.get("canExecute"):
            raise RuntimeError(f"runtime is not executable: {status}")
        _post(base_url, "/api/cycle/reset")
        if not args.skip_simulation_reset:
            _post(
                base_url,
                "/api/simulation/reset",
                timeout_s=args.simulation_reset_timeout_s,
            )
        time.sleep(args.post_reset_settle_s)
        _post(base_url, "/api/cycle/run")

        deadline_s = time.monotonic() + args.timeout_s
        last_print_key: tuple[Any, ...] | None = None
        while time.monotonic() < deadline_s:
            cycle_state = _cycle_state(base_url)
            _scene, _source, source_object, measured_tcp = _scene_measurements(
                base_url,
                source_id=args.source_id,
                object_id=args.object_id,
            )
            scene_object = _find_object(_scene, args.object_id)
            row = _evidence_row(
                started_s=started_s,
                cycle_state=cycle_state,
                scene_object=scene_object,
                source_object=source_object,
            )
            key = (
                row.cycle_state,
                row.attached,
                row.drift_status,
                row.baseline_captured,
                row.condition_raised,
            )
            if key != last_print_key or row.attached:
                evidence.append(row)
                print(json.dumps(row.to_dict(), sort_keys=True), flush=True)
                last_print_key = key

            if row.attached and row.baseline_captured and baseline_tcp_to_object is None:
                baseline_tcp_to_object = _matrix_multiply(
                    _matrix_inverse(_pose_to_matrix(_tcp_pose(measured_tcp))),
                    _pose_to_matrix(_object_pose(source_object)),
                )
                healthy_started_s = time.monotonic()

            if baseline_tcp_to_object is not None and row.attached:
                desired_object = _matrix_multiply(
                    _pose_to_matrix(_tcp_pose(measured_tcp)),
                    baseline_tcp_to_object,
                )
                if inject_diagnostic_offset:
                    desired_object[0][3] += float(args.diagnostic_offset_m)
                result = sim_client.set_entity_state(
                    entity=args.entity,
                    pose=_matrix_to_pose(desired_object, args.frame_id),
                )
                if not result.success:
                    raise RuntimeError(
                        "SetEntityState failed during measured-grasp coupling: "
                        + json.dumps(result.to_dict(), sort_keys=True)
                    )
                if (
                    not inject_diagnostic_offset
                    and row.drift_status == "ok"
                    and row.translation_m is not None
                ):
                    healthy_translations.append(row.translation_m)
                if (
                    not inject_diagnostic_offset
                    and healthy_started_s is not None
                    and time.monotonic() - healthy_started_s >= args.healthy_duration_s
                    and len(healthy_translations) >= args.min_healthy_ok_samples
                ):
                    inject_diagnostic_offset = True
                    print(
                        "healthy coupling proof complete; injecting diagnostic offset",
                        flush=True,
                    )

            if (
                inject_diagnostic_offset
                and row.drift_status == "exceeded"
                and row.condition_raised
            ):
                diagnostic_exceeded = True
                break

            if inject_diagnostic_offset and not row.attached:
                failure_reason = "object detached before diagnostic threshold was observed"
                break
            if cycle_state not in {"idle", "running", "stopping", "stopped"}:
                failure_reason = f"unexpected cycle state: {cycle_state}"
                break
            if cycle_state == "idle" and time.monotonic() - started_s > 2.0:
                failure_reason = "cycle returned idle before qualification completed"
                break
            time.sleep(args.poll_s)

        if baseline_tcp_to_object is None:
            failure_reason = failure_reason or "measured baseline was not captured"
        if len(healthy_translations) < args.min_healthy_ok_samples:
            failure_reason = failure_reason or "insufficient healthy ok samples"
        if not diagnostic_exceeded:
            failure_reason = failure_reason or "diagnostic drift threshold was not exercised"
        passed = not failure_reason
        return QualificationResult(
            passed=passed,
            healthy_ok_samples=len(healthy_translations),
            diagnostic_exceeded=diagnostic_exceeded,
            max_healthy_translation_m=max(healthy_translations or [0.0]),
            evidence=evidence,
            failure_reason=failure_reason,
        )
    finally:
        try:
            _post(base_url, "/api/cycle/stop")
        except Exception:
            pass
        try:
            _post(base_url, "/api/cycle/reset")
        except Exception:
            pass
        node.destroy_node()
        rclpy.shutdown()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--source-id", default=DEFAULT_SOURCE_ID)
    parser.add_argument("--object-id", default=DEFAULT_OBJECT_ID)
    parser.add_argument("--entity", default=DEFAULT_ENTITY)
    parser.add_argument("--frame-id", default="universe")
    parser.add_argument("--service-prefix", default="auto")
    parser.add_argument("--service-timeout-s", type=float, default=2.0)
    parser.add_argument("--timeout-s", type=float, default=45.0)
    parser.add_argument("--poll-s", type=float, default=0.05)
    parser.add_argument("--healthy-duration-s", type=float, default=1.2)
    parser.add_argument("--min-healthy-ok-samples", type=int, default=5)
    parser.add_argument("--diagnostic-offset-m", type=float, default=0.12)
    parser.add_argument("--post-reset-settle-s", type=float, default=1.0)
    parser.add_argument("--simulation-reset-timeout-s", type=float, default=15.0)
    parser.add_argument(
        "--skip-simulation-reset",
        action="store_true",
        help="Skip the public Isaac simulation reset before starting the cycle.",
    )
    args = parser.parse_args(argv)

    try:
        result = _qualify(args)
    except Exception as exc:
        print(
            json.dumps(
                {
                    "passed": False,
                    "failure_reason": str(exc),
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 1
    print(json.dumps(result.to_dict(), indent=2, sort_keys=True))
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
