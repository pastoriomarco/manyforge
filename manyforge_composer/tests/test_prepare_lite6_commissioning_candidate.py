from __future__ import annotations

import importlib.util
import math
from pathlib import Path
from typing import Any, Mapping

import pytest


_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "xarm"
    / "prepare_lite6_commissioning_candidate.py"
)
_SPEC = importlib.util.spec_from_file_location(
    "prepare_lite6_commissioning_candidate",
    _SCRIPT,
)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)


def _responses() -> dict[str, dict[str, Any]]:
    joints = [f"joint{index}" for index in range(1, 7)]
    positions = [0.1, -0.2, 0.3, -0.4, 0.5, -0.6]
    return {
        "/api/robot-lifecycle/status": {
            "configured": True,
            "providerHealthy": True,
            "fresh": True,
            "canExecute": True,
            "motionAdmitted": True,
            "positionValid": "true",
            "robotEnabled": "true",
            "vendorControlReady": "true",
            "hardwareComponentActive": "true",
            "trajectoryControllerActive": "true",
            "reconciliationRequired": False,
        },
        "/api/runtime/status": {
            "profileKind": "real_robot",
            "canExecute": True,
        },
        "/api/cycle/status": {
            "state": "idle",
            "motionStopState": "idle",
            "error": None,
        },
        "/api/scene/state": {
            "robot": {
                "robot_id": "lite6-a",
                "state_source": "observed_ros2",
                "source_age_ns": 10_000_000,
                "joint_names": joints,
                "joint_positions": dict(zip(joints, positions)),
            },
        },
    }


class FakeTransport:
    def __init__(self, responses: dict[str, dict[str, Any]]) -> None:
        self.responses = responses
        self.calls: list[tuple[str, str, Mapping[str, Any] | None]] = []

    def __call__(
        self,
        method: str,
        path: str,
        payload: Mapping[str, Any] | None,
    ) -> dict[str, Any]:
        self.calls.append((method, path, payload))
        if path == "/api/robot-control/candidates":
            assert payload is not None
            joint = payload["jointTarget"]["jointNames"][0]
            requested = payload["jointTarget"]["positionsRad"][0]
            joints = [f"joint{index}" for index in range(1, 7)]
            positions = [0.1, -0.2, 0.3, -0.4, 0.5, -0.6]
            positions[joints.index(joint)] = requested
            return {
                "candidateId": "candidate-1",
                "status": "ready",
                "commitEligible": True,
                "normalizedJointTarget": {
                    "jointNames": joints,
                    "positionsRad": positions,
                },
            }
        return self.responses[path]


def test_prepares_bounded_candidate_without_execute_request() -> None:
    transport = FakeTransport(_responses())

    result = _MODULE.prepare_candidate(
        base_url="http://unused",
        joint="joint2",
        delta_deg=-0.5,
        request_json=transport,
    )

    assert result["motionDispatched"] is False
    assert result["operatorApprovalRequired"] is True
    assert result["candidate"]["candidateId"] == "candidate-1"
    post = transport.calls[-1]
    assert post[0:2] == ("POST", "/api/robot-control/candidates")
    assert math.isclose(
        post[2]["jointTarget"]["positionsRad"][0],
        -0.2 + math.radians(-0.5),
    )
    assert not any(path.endswith("/execute") for _, path, _ in transport.calls)


def test_refuses_closed_lifecycle_before_candidate_creation() -> None:
    responses = _responses()
    responses["/api/robot-lifecycle/status"]["canExecute"] = False
    transport = FakeTransport(responses)

    with pytest.raises(
        _MODULE.CommissioningRefusal,
        match="canExecute=true",
    ):
        _MODULE.prepare_candidate(
            base_url="http://unused",
            joint="joint1",
            delta_deg=0.5,
            request_json=transport,
        )

    assert not any(method == "POST" for method, _, _ in transport.calls)


def test_refuses_stale_observed_state_before_candidate_creation() -> None:
    responses = _responses()
    responses["/api/scene/state"]["robot"]["source_age_ns"] = 251_000_000
    transport = FakeTransport(responses)

    with pytest.raises(_MODULE.CommissioningRefusal, match="state is stale"):
        _MODULE.prepare_candidate(
            base_url="http://unused",
            joint="joint1",
            delta_deg=0.5,
            request_json=transport,
        )

    assert not any(method == "POST" for method, _, _ in transport.calls)


@pytest.mark.parametrize("delta", [0.0, -1.001, 1.001, math.inf])
def test_refuses_unbounded_or_invalid_delta(delta: float) -> None:
    transport = FakeTransport(_responses())

    with pytest.raises(_MODULE.CommissioningRefusal):
        _MODULE.prepare_candidate(
            base_url="http://unused",
            joint="joint1",
            delta_deg=delta,
            request_json=transport,
        )

    assert transport.calls == []
