"""Standard ROS 2 diagnostics surface for ManyForge (audit Phase 6H).

Publishes ``diagnostic_msgs/DiagnosticArray`` on ``/diagnostics`` from the
Composer's execution digest: one ``DiagnosticStatus`` per active backend
fault (level/values mapped from the Phase 6A fault taxonomy) and an OK
heartbeat when healthy. The heartbeat is what drives PASSED/healing on
AUTOSAR-DEM-style consumers (e.g. ros2_medkit's stock diagnostic bridge),
so it is not optional.

Deliberately medkit-free: this module depends only on standard
``diagnostic_msgs`` and the ManyForge fault taxonomy. Vendor-specific
enrichment (discovery hints, direct fault reporting) lives in the separate
``manyforge_medkit_adapter`` package (Phase 6I).

The mapping core (`build_diagnostic_statuses`) is pure data-in/data-out so
it tests hermetically; the rclpy node is a thin shell around it.
"""

from __future__ import annotations

import argparse
import json
import re
import time
import urllib.request
from typing import Any, Optional

from manyforge_behavior.fault_taxonomy import ROS2_CONTROL_BACKEND_FAULT_CLASSES

# diagnostic_msgs/DiagnosticStatus levels (bytes); kept as ints here so the
# mapping core stays import-free.
LEVEL_OK = 0
LEVEL_WARN = 1
LEVEL_ERROR = 2
LEVEL_STALE = 3

_SEVERITY_TO_LEVEL = {
    "INFO": LEVEL_OK,
    "WARN": LEVEL_WARN,
    "ERROR": LEVEL_ERROR,
    # diagnostic_msgs has no CRITICAL level; CRITICAL maps to ERROR and the
    # operational severity is preserved in values["manyforge_severity"]
    # (medkit-style consumers needing debounce bypass use the 6I adapter,
    # which reports CRITICAL natively).
    "CRITICAL": LEVEL_ERROR,
}

_SURFACE_TO_COMPONENT = {
    "ros2_control_execution_backend": "execution",
}

_FAULT_CODE_SAFE = re.compile(r"[^A-Z0-9_]")


def external_fault_code(reason_code: str) -> str:
    """MANYFORGE_-prefixed UPPER_SNAKE fault code (<=64 chars, [A-Z0-9_]).

    Known taxonomy tokens use their declared alias; anything else is
    sanitized to the same convention so downstream aggregation never sees a
    malformed code.
    """
    fault = ROS2_CONTROL_BACKEND_FAULT_CLASSES.get(reason_code)
    if fault is not None:
        return fault.external_alias
    sanitized = _FAULT_CODE_SAFE.sub("_", str(reason_code).upper()).strip("_")
    return f"MANYFORGE_{sanitized or 'UNKNOWN_FAULT'}"[:64]


def fault_severity(reason_code: str) -> str:
    fault = ROS2_CONTROL_BACKEND_FAULT_CLASSES.get(reason_code)
    return fault.severity if fault is not None else "ERROR"


def build_diagnostic_statuses(
    digest: Optional[dict[str, Any]],
    *,
    deployment_name: str,
) -> list[dict[str, Any]]:
    """Map an execution digest to DiagnosticStatus-shaped dicts.

    Returns dicts with keys: name, level, message, hardware_id, values
    (list of (key, value) string pairs). Composer unreachable (digest None)
    is reported as a WARN status rather than silence, so consumers can
    distinguish "healthy" from "unobserved".
    """
    if digest is None:
        return [
            {
                "name": "manyforge/bridge",
                "level": LEVEL_WARN,
                "message": "Composer unreachable; ManyForge state unobserved",
                "hardware_id": deployment_name,
                "values": [("observed", "false")],
            }
        ]

    error_code = digest.get("errorCode") or digest.get("error_code")
    error_details = digest.get("errorDetails") or digest.get("error_details") or {}
    error_message = digest.get("error")
    reason_code = None
    if isinstance(error_details, dict):
        reason_code = error_details.get("reason_code")
    if reason_code is None and isinstance(error_code, str) and error_code:
        reason_code = error_code

    state = str(digest.get("state") or "").lower()
    # The digest can carry the previous run's error prose while a new run is
    # active; only non-running/idle states with a message latch a fault
    # (same guard as the medkit forwarder — a stale fault must not mask the
    # healthy heartbeat).
    fault_active = (
        bool(error_message)
        and bool(reason_code)
        and state not in ("running", "idle")
    )

    if not fault_active:
        return [
            {
                "name": "manyforge/execution",
                "level": LEVEL_OK,
                "message": "ok",
                "hardware_id": deployment_name,
                "values": [("state", state or "unknown")],
            }
        ]

    component = "runtime"
    if isinstance(error_details, dict):
        component = _SURFACE_TO_COMPONENT.get(
            str(error_details.get("surface") or ""), "runtime"
        )
        if error_details.get("backend") == "ros2_control":
            component = "execution"

    severity = fault_severity(str(reason_code)) if reason_code else "ERROR"
    values: list[tuple[str, str]] = [
        ("state", state or "unknown"),
        ("manyforge_severity", severity),
        ("fault_code", external_fault_code(str(reason_code or "unknown_fault"))),
    ]
    if reason_code:
        values.append(("reason_code", str(reason_code)))
    if isinstance(error_details, dict):
        for key in sorted(error_details):
            if key in ("reason_code",):
                continue
            values.append((str(key), str(error_details[key])))

    return [
        {
            "name": f"manyforge/{component}",
            "level": _SEVERITY_TO_LEVEL.get(severity, LEVEL_ERROR),
            "message": str(error_message or "ManyForge backend fault"),
            "hardware_id": deployment_name,
            "values": values,
        }
    ]


def fetch_digest(composer_url: str, timeout_s: float = 2.0) -> Optional[dict[str, Any]]:
    try:
        with urllib.request.urlopen(
            f"{composer_url.rstrip('/')}/api/execution/digest", timeout=timeout_s
        ) as response:
            return json.load(response)
    except Exception:
        return None


def fetch_deployment_name(composer_url: str, timeout_s: float = 2.0) -> str:
    try:
        with urllib.request.urlopen(
            f"{composer_url.rstrip('/')}/api/runtime/status", timeout=timeout_s
        ) as response:
            payload = json.load(response)
            return str(payload.get("profileId") or "manyforge")
    except Exception:
        return "manyforge"


def statuses_to_msg(statuses: list[dict[str, Any]], stamp) -> Any:
    """Build a DiagnosticArray message (requires diagnostic_msgs)."""
    from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue

    array = DiagnosticArray()
    array.header.stamp = stamp
    for status in statuses:
        message = DiagnosticStatus()
        message.level = bytes([status["level"]])
        message.name = status["name"]
        message.message = status["message"]
        message.hardware_id = status["hardware_id"]
        message.values = [
            KeyValue(key=key, value=value) for key, value in status["values"]
        ]
        array.status.append(message)
    return array


def run_bridge(
    *,
    composer_url: str,
    period_s: float,
    diagnostics_topic: str = "/diagnostics",
) -> int:
    """Blocking rclpy bridge loop (thin shell over the pure mapping)."""
    import rclpy
    from diagnostic_msgs.msg import DiagnosticArray

    initialized_here = False
    if not rclpy.ok():
        rclpy.init(args=None)
        initialized_here = True
    node = rclpy.create_node("manyforge_diagnostics_bridge")
    publisher = node.create_publisher(DiagnosticArray, diagnostics_topic, 10)
    deployment_name = fetch_deployment_name(composer_url)

    def tick() -> None:
        digest = fetch_digest(composer_url)
        statuses = build_diagnostic_statuses(
            digest, deployment_name=deployment_name
        )
        publisher.publish(statuses_to_msg(statuses, node.get_clock().now().to_msg()))

    node.create_timer(period_s, tick)
    node.get_logger().info(
        f"ManyForge diagnostics bridge: {composer_url} -> {diagnostics_topic} "
        f"every {period_s}s (hardware_id={deployment_name})"
    )
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        if initialized_here:
            rclpy.shutdown()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument("--period", type=float, default=1.0)
    parser.add_argument("--diagnostics-topic", default="/diagnostics")
    args = parser.parse_args()
    return run_bridge(
        composer_url=args.composer_url,
        period_s=args.period,
        diagnostics_topic=args.diagnostics_topic,
    )


if __name__ == "__main__":
    raise SystemExit(main())
