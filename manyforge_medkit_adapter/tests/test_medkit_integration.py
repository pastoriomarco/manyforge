"""Audit 6I: real ros2_medkit integration (service transport + aggregation).

Requires ros2_medkit to be built/sourced AND a running fault_manager
(`ros2 run ros2_medkit_fault_manager fault_manager_node --ros-args
-p database_path:=/tmp/medkit_faults.db`); skips otherwise with a clear
reason. The ManyForge-side digest production is live-verified separately
(Phase 6A acceptance probe); this test verifies the forwarder's reports
drive the real medkit fault lifecycle: FAILED -> CONFIRMED (CRITICAL
bypasses debounce), PASSED -> healed.
"""

from __future__ import annotations

import time
import uuid

import pytest

from manyforge_medkit_adapter.forwarder import FaultForwarder


def _tracking_fault_digest():
    return {
        "state": "stopped",
        "error": "Controller tracking error exceeded tolerance: 0.078 rad",
        "errorCode": "tracking_error_exceeded",
        "errorDetails": {
            "reason_code": "controller_cancel_timeout",
        },
    }


def test_forwarder_reports_drive_real_fault_manager_lifecycle():
    rclpy = pytest.importorskip("rclpy")
    medkit_srv = pytest.importorskip(
        "ros2_medkit_msgs.srv",
        reason="ros2_medkit not built/sourced in this lane",
    )
    ReportFault = medkit_srv.ReportFault
    ListFaults = medkit_srv.ListFaults

    initialized_here = False
    if not rclpy.ok():
        rclpy.init(args=None)
        initialized_here = True
    node = rclpy.create_node(f"manyforge_medkit_itest_{uuid.uuid4().hex[:8]}")
    try:
        report_client = node.create_client(ReportFault, "/fault_manager/report_fault")
        list_client = node.create_client(ListFaults, "/fault_manager/list_faults")
        if not report_client.wait_for_service(timeout_sec=3.0):
            pytest.skip("no running fault_manager (/fault_manager/report_fault)")
        assert list_client.wait_for_service(timeout_sec=3.0)

        source_id = f"/manyforge/itest_{uuid.uuid4().hex[:8]}/composer"
        forwarder = FaultForwarder(source_id=source_id)

        def call(request):
            future = report_client.call_async(request)
            rclpy.spin_until_future_complete(node, future, timeout_sec=5.0)
            response = future.result()
            assert response is not None and response.accepted
            return response

        def list_faults():
            future = list_client.call_async(ListFaults.Request())
            rclpy.spin_until_future_complete(node, future, timeout_sec=5.0)
            response = future.result()
            assert response is not None
            return {fault.fault_code: fault for fault in response.faults}

        # FAILED: controller_cancel_timeout is CRITICAL in the 6A taxonomy ->
        # severity 3 -> medkit confirms immediately (debounce bypass).
        reports = forwarder.observe(_tracking_fault_digest())
        assert len(reports) == 1
        report = reports[0]
        assert report.severity == 3
        request = ReportFault.Request()
        request.fault_code = report.fault_code
        request.event_type = report.event_type
        request.severity = report.severity
        request.description = report.description
        request.source_id = report.source_id
        call(request)

        deadline = time.monotonic() + 5.0
        fault = None
        while time.monotonic() < deadline:
            faults = list_faults()
            fault = faults.get("MANYFORGE_CONTROLLER_CANCEL_TIMEOUT")
            if fault is not None and fault.status == "CONFIRMED":
                break
            time.sleep(0.2)
        assert fault is not None, "fault never reached the fault manager"
        assert fault.status == "CONFIRMED"
        assert fault.severity == 3
        assert source_id in list(fault.reporting_sources)
        assert "tracking error exceeded" in fault.description

        # Clear -> the forwarder emits a PASSED healing stream (one per
        # poll); medkit's DEM debounce counts them toward healing. Drive the
        # stream exactly as the adapter loop would and require the fault to
        # leave CONFIRMED.
        status = None
        for _ in range(forwarder.healing_passes + 2):
            for report in forwarder.observe({"state": "idle", "error": None}):
                request = ReportFault.Request()
                request.fault_code = report.fault_code
                request.event_type = report.event_type
                request.severity = report.severity
                request.description = report.description
                request.source_id = report.source_id
                call(request)
            fault = list_faults().get("MANYFORGE_CONTROLLER_CANCEL_TIMEOUT")
            status = fault.status if fault is not None else "CLEARED"
            if status in ("HEALED", "PREPASSED", "CLEARED"):
                break
            time.sleep(0.1)
        assert status in ("HEALED", "PREPASSED", "CLEARED"), status
    finally:
        node.destroy_node()
        if initialized_here:
            rclpy.shutdown()
