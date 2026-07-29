#!/usr/bin/env python3
"""Prepare, but never execute, one bounded Lite6 commissioning candidate."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import tempfile
from typing import Any, Callable, Mapping
import urllib.error
import urllib.request
import uuid


EXPECTED_JOINTS = tuple(f"joint{index}" for index in range(1, 7))
DEFAULT_MAX_SOURCE_AGE_MS = 250.0
MAX_ABS_DELTA_DEG = 1.0


class CommissioningRefusal(RuntimeError):
    """A precondition failed before candidate creation."""


JsonRequest = Callable[
    [str, str, Mapping[str, Any] | None],
    dict[str, Any],
]


def _http_json_request(
    base_url: str,
    method: str,
    path: str,
    payload: Mapping[str, Any] | None,
) -> dict[str, Any]:
    body = None
    headers: dict[str, str] = {}
    if payload is not None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}{path}",
        data=body,
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(request, timeout=5.0) as response:
            decoded = json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise CommissioningRefusal(
            f"{method} {path} was rejected with HTTP {exc.code}: {detail}"
        ) from exc
    except (OSError, ValueError) as exc:
        raise CommissioningRefusal(
            f"{method} {path} did not return usable JSON: {exc}"
        ) from exc
    if not isinstance(decoded, dict):
        raise CommissioningRefusal(f"{method} {path} did not return an object")
    return decoded


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CommissioningRefusal(message)


def _finite_number(value: object, *, label: str) -> float:
    _require(
        isinstance(value, (int, float)) and not isinstance(value, bool),
        f"{label} is not numeric",
    )
    result = float(value)
    _require(math.isfinite(result), f"{label} is not finite")
    return result


def _validate_preconditions(
    *,
    lifecycle: Mapping[str, Any],
    runtime: Mapping[str, Any],
    cycle: Mapping[str, Any],
    scene: Mapping[str, Any],
    max_source_age_ms: float,
) -> tuple[list[str], list[float]]:
    _require(
        runtime.get("profileKind") == "real_robot",
        "Composer is not running the real-robot profile",
    )
    _require(
        runtime.get("canExecute") is True,
        "Runtime execution admission is closed",
    )
    for key in ("configured", "providerHealthy", "fresh", "canExecute"):
        _require(
            lifecycle.get(key) is True,
            f"Robot lifecycle precondition {key}=true is not satisfied",
        )
    for key in (
        "positionValid",
        "robotEnabled",
        "vendorControlReady",
        "hardwareComponentActive",
        "trajectoryControllerActive",
    ):
        _require(
            lifecycle.get(key) == "true",
            f"Robot lifecycle fact {key}=true is not satisfied",
        )
    _require(
        lifecycle.get("motionAdmitted") is True,
        "The lifecycle provider has not admitted motion",
    )
    _require(
        lifecycle.get("reconciliationRequired") is not True,
        "The lifecycle provider requires reconciliation",
    )

    _require(
        cycle.get("state") == "idle",
        "The behavior cycle must be idle while preparing first motion",
    )
    _require(
        cycle.get("motionStopState") == "idle",
        "The motion-stop state must be idle while preparing first motion",
    )
    _require(
        cycle.get("error") in (None, ""),
        "The behavior cycle has an unresolved error",
    )

    robot = scene.get("robot")
    _require(isinstance(robot, dict), "No observed robot state is available")
    _require(
        robot.get("robot_id") == "lite6-a",
        "Observed robot identity is not lite6-a",
    )
    _require(
        robot.get("state_source") == "observed_ros2",
        "Robot state is not sourced from observed ROS 2 feedback",
    )
    source_age_ns = _finite_number(
        robot.get("source_age_ns"),
        label="robot.source_age_ns",
    )
    _require(source_age_ns >= 0.0, "Robot source age is negative")
    _require(
        source_age_ns <= max_source_age_ms * 1_000_000.0,
        (
            f"Robot state is stale ({source_age_ns / 1_000_000.0:.1f} ms; "
            f"limit {max_source_age_ms:.1f} ms)"
        ),
    )
    joint_names = robot.get("joint_names")
    _require(
        isinstance(joint_names, list)
        and tuple(joint_names) == EXPECTED_JOINTS,
        "Observed joint order does not match the exact Lite6 binding",
    )
    raw_positions = robot.get("joint_positions")
    _require(
        isinstance(raw_positions, dict),
        "Observed joint positions are unavailable",
    )
    _require(
        set(raw_positions) == set(EXPECTED_JOINTS),
        "Observed joint positions do not cover exactly the six Lite6 joints",
    )
    positions = [
        _finite_number(raw_positions[name], label=f"joint_positions.{name}")
        for name in EXPECTED_JOINTS
    ]
    return list(EXPECTED_JOINTS), positions


def prepare_candidate(
    *,
    base_url: str,
    joint: str,
    delta_deg: float,
    max_source_age_ms: float = DEFAULT_MAX_SOURCE_AGE_MS,
    request_json: JsonRequest | None = None,
) -> dict[str, Any]:
    """Create a preview candidate after fail-closed read-only checks.

    This function intentionally has no candidate-execute request path.
    """
    _require(joint in EXPECTED_JOINTS, f"Unsupported Lite6 joint: {joint}")
    _require(math.isfinite(delta_deg), "Joint delta must be finite")
    _require(delta_deg != 0.0, "Joint delta must be non-zero")
    _require(
        abs(delta_deg) <= MAX_ABS_DELTA_DEG,
        f"Absolute joint delta must be <= {MAX_ABS_DELTA_DEG:.1f} degree",
    )
    _require(
        math.isfinite(max_source_age_ms)
        and 0.0 < max_source_age_ms <= DEFAULT_MAX_SOURCE_AGE_MS,
        (
            "Maximum source age must be positive and no greater than "
            f"{DEFAULT_MAX_SOURCE_AGE_MS:.1f} ms"
        ),
    )

    transport = request_json
    if transport is None:
        transport = lambda method, path, payload: _http_json_request(
            base_url,
            method,
            path,
            payload,
        )
    lifecycle = transport("GET", "/api/robot-lifecycle/status", None)
    runtime = transport("GET", "/api/runtime/status", None)
    cycle = transport("GET", "/api/cycle/status", None)
    scene = transport("GET", "/api/scene/state", None)
    joint_names, positions = _validate_preconditions(
        lifecycle=lifecycle,
        runtime=runtime,
        cycle=cycle,
        scene=scene,
        max_source_age_ms=max_source_age_ms,
    )

    target_index = joint_names.index(joint)
    target_rad = positions[target_index] + math.radians(delta_deg)
    request_payload = {
        "clientRequestId": f"lite6-commissioning-{uuid.uuid4()}",
        "sourceSurface": "joint_input",
        "inputMode": "joint",
        "jointTarget": {
            "jointNames": [joint],
            "positionsRad": [target_rad],
        },
    }
    candidate = transport(
        "POST",
        "/api/robot-control/candidates",
        request_payload,
    )
    _require(
        candidate.get("status") == "ready"
        and candidate.get("commitEligible") is True,
        (
            "The bounded motion candidate is not ready: "
            f"{candidate.get('diagnostics') or candidate.get('status')}"
        ),
    )
    normalized = candidate.get("normalizedJointTarget")
    _require(
        isinstance(normalized, dict)
        and normalized.get("jointNames") == joint_names,
        "Candidate normalized joint order does not match the exact binding",
    )
    normalized_positions = normalized.get("positionsRad")
    _require(
        isinstance(normalized_positions, list)
        and len(normalized_positions) == len(positions),
        "Candidate normalized target is incomplete",
    )
    normalized_values = [
        _finite_number(value, label=f"normalized target {name}")
        for name, value in zip(joint_names, normalized_positions)
    ]
    for index, (start, target) in enumerate(
        zip(positions, normalized_values)
    ):
        expected = target_rad if index == target_index else start
        _require(
            math.isclose(target, expected, rel_tol=0.0, abs_tol=1e-9),
            "Candidate changed a joint outside the requested bounded delta",
        )

    return {
        "schema": "manyforge.real_lite6.commissioning_candidate.v1",
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "motionDispatched": False,
        "operatorApprovalRequired": True,
        "bounds": {
            "joint": joint,
            "deltaDeg": delta_deg,
            "maxAbsDeltaDeg": MAX_ABS_DELTA_DEG,
            "maxSourceAgeMs": max_source_age_ms,
        },
        "observations": {
            "robotLifecycle": lifecycle,
            "runtime": runtime,
            "cycle": cycle,
            "sceneRobot": scene["robot"],
        },
        "request": request_payload,
        "candidate": candidate,
    }


def _atomic_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.",
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def _default_output() -> Path:
    repo_root = Path(__file__).resolve().parents[2]
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return (
        repo_root
        / ".manyforge-var"
        / "real-lite6"
        / "candidate-previews"
        / f"{stamp}-{uuid.uuid4()}.json"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--composer-url",
        default="http://127.0.0.1:9000",
    )
    parser.add_argument("--joint", choices=EXPECTED_JOINTS, default="joint1")
    parser.add_argument("--delta-deg", type=float, default=0.5)
    parser.add_argument(
        "--max-source-age-ms",
        type=float,
        default=DEFAULT_MAX_SOURCE_AGE_MS,
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    try:
        record = prepare_candidate(
            base_url=args.composer_url,
            joint=args.joint,
            delta_deg=args.delta_deg,
            max_source_age_ms=args.max_source_age_ms,
        )
        output = args.output.resolve() if args.output else _default_output()
        _atomic_json(output, record)
    except CommissioningRefusal as exc:
        parser.exit(1, f"Candidate preparation refused: {exc}\n")

    candidate = record["candidate"]
    print(f"Candidate prepared: {candidate['candidateId']}")
    print(f"Retained preview evidence: {output}")
    print("No trajectory was dispatched.")
    print(
        "Inspect the ghost and trajectory in Composer. Approve it there only "
        "while the attended cell remains safe."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
