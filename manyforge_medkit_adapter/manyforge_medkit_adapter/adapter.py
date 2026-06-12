"""ros2_medkit adapter node (audit Phase 6I).

Bridges ManyForge core traceability to a running ros2_medkit fault manager:

- polls the Composer REST surface (``/api/execution/digest``,
  ``/api/infra/heartbeat``) — after Phase 6A these carry structured
  ``reason_code`` + measurements;
- calls ``<prefix>/report_fault`` (``ros2_medkit_msgs/srv/ReportFault``)
  with FAILED on fault latch and PASSED on clear, severities from the 6A
  taxonomy mapping;
- announces the ManyForge entity via a ``MedkitDiscoveryHint`` beacon so
  the SOVD hierarchy shows ManyForge core even without ROS-graph presence;
- fail-closed startup: the fault-manager services must be reachable and
  the pinned medkit message contract importable, otherwise the adapter
  exits non-zero with a clear log instead of silently dropping faults.

ManyForge core never imports medkit types; only this adapter does, lazily.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
from typing import Any, Optional

from manyforge_medkit_adapter.forwarder import EVENT_FAILED, FaultForwarder


def _fetch_json(url: str, timeout_s: float = 2.0) -> Optional[dict[str, Any]]:
    try:
        with urllib.request.urlopen(url, timeout=timeout_s) as response:
            return json.load(response)
    except Exception:
        return None


def fetch_digest(composer_url: str) -> Optional[dict[str, Any]]:
    return _fetch_json(f"{composer_url.rstrip('/')}/api/execution/digest")


def fetch_archive_ref(composer_url: str) -> Optional[str]:
    heartbeat = _fetch_json(f"{composer_url.rstrip('/')}/api/infra/heartbeat")
    if not heartbeat:
        return None
    archive = (heartbeat.get("components") or {}).get("failure_archive") or {}
    archive_id = archive.get("last_archive_id")
    return str(archive_id) if archive_id else None


def fetch_deployment_name(composer_url: str) -> str:
    payload = _fetch_json(f"{composer_url.rstrip('/')}/api/runtime/status")
    if payload:
        return str(payload.get("profileId") or "manyforge")
    return "manyforge"


def run_adapter(
    *,
    composer_url: str,
    period_s: float,
    fault_manager_prefix: str,
    service_wait_s: float,
    discovery_topic: str = "/ros2_medkit/discovery",
) -> int:
    """Blocking adapter loop. Returns non-zero on fail-closed startup."""
    try:
        from ros2_medkit_msgs.srv import ReportFault  # noqa: PLC0415
    except ImportError:
        print(
            "manyforge_medkit_adapter: ros2_medkit_msgs is not importable - "
            "install/source ros2_medkit before running the adapter "
            "(fail-closed; no faults will be silently dropped)",
            file=sys.stderr,
        )
        return 2

    import rclpy

    initialized_here = False
    if not rclpy.ok():
        rclpy.init(args=None)
        initialized_here = True
    node = rclpy.create_node("manyforge_medkit_adapter")
    exit_code = 0
    try:
        report_client = node.create_client(
            ReportFault, f"{fault_manager_prefix.rstrip('/')}/report_fault"
        )
        if not report_client.wait_for_service(timeout_sec=service_wait_s):
            node.get_logger().error(
                "fault_manager report_fault service unavailable after "
                f"{service_wait_s}s - exiting fail-closed"
            )
            return 3

        deployment_name = fetch_deployment_name(composer_url)
        source_id = f"/manyforge/{deployment_name}/composer"

        # Optional entity enrichment: a missing hint type must not stop
        # fault forwarding (the hint only improves SOVD presentation).
        try:
            from ros2_medkit_msgs.msg import MedkitDiscoveryHint  # noqa: PLC0415
            from rclpy.qos import (  # noqa: PLC0415
                DurabilityPolicy,
                QoSProfile,
                ReliabilityPolicy,
            )

            hint_qos = QoSProfile(
                depth=1,
                durability=DurabilityPolicy.TRANSIENT_LOCAL,
                reliability=ReliabilityPolicy.RELIABLE,
            )
            hint_pub = node.create_publisher(
                MedkitDiscoveryHint, discovery_topic, hint_qos
            )
            hint = MedkitDiscoveryHint()
            hint.entity_id = "manyforge_composer"
            hint.display_name = f"ManyForge Composer ({deployment_name})"
            hint.function_ids = ["manyforge"]
            hint_pub.publish(hint)
            node.get_logger().info(
                f"published MedkitDiscoveryHint for manyforge_composer "
                f"on {discovery_topic}"
            )
        except Exception as exc:  # pragma: no cover - enrichment only
            node.get_logger().warning(f"discovery hint unavailable: {exc}")

        forwarder = FaultForwarder(source_id=source_id)
        node.get_logger().info(
            f"ManyForge medkit adapter: {composer_url} -> "
            f"{fault_manager_prefix}/report_fault every {period_s}s "
            f"(source_id={source_id})"
        )

        while rclpy.ok():
            digest = fetch_digest(composer_url)
            archive_ref = None
            if digest is not None and digest.get("error"):
                archive_ref = fetch_archive_ref(composer_url)
            for report in forwarder.observe(digest, archive_ref=archive_ref):
                request = ReportFault.Request()
                request.fault_code = report.fault_code
                request.event_type = report.event_type
                request.severity = report.severity
                request.description = report.description
                request.source_id = report.source_id
                future = report_client.call_async(request)
                rclpy.spin_until_future_complete(node, future, timeout_sec=2.0)
                response = future.result()
                accepted = bool(response and response.accepted)
                node.get_logger().info(
                    f"reported {report.fault_code} "
                    f"{'FAILED' if report.event_type == EVENT_FAILED else 'PASSED'} "
                    f"severity={report.severity} accepted={accepted}"
                )
            time.sleep(period_s)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        if initialized_here:
            rclpy.shutdown()
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument("--period", type=float, default=1.0)
    parser.add_argument("--fault-manager-prefix", default="/fault_manager")
    parser.add_argument("--service-wait", type=float, default=10.0)
    args = parser.parse_args()
    return run_adapter(
        composer_url=args.composer_url,
        period_s=args.period,
        fault_manager_prefix=args.fault_manager_prefix,
        service_wait_s=args.service_wait,
    )


if __name__ == "__main__":
    raise SystemExit(main())
