"""Boundary tests for the follow-target WebSocket payload contract."""
from __future__ import annotations

from pathlib import Path
from typing import Optional

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import (
    CommandResult,
    ExecutionState,
    MockKernelClient,
    StreamingBeginResult,
    StreamingPushResult,
    StreamingSessionConfig,
    StreamingTarget,
)
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.follow_target_ws import (
    _frame_to_target,
    _streaming_begin_reason_code,
    _streaming_reason_code,
)
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(
    _REPO_ROOT
    / "manyforge_behavior"
    / "tests"
    / "fixtures"
    / "pick_and_place.program.yaml"
)
_DIRECT_CONTROL_EVENT_TYPES = (
    "streaming_session_started,"
    "streaming_target_rejected,"
    "streaming_session_ended"
)

_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=[str(_REPO_ROOT), "/workspace", "/tmp"],
)


class _StreamingKernel(MockKernelClient):
    """Small deterministic streaming kernel for end-to-end WS route tests."""

    def __init__(self) -> None:
        super().__init__(joint_names=["joint1", "joint2"])
        self.next_session_id = 1
        self.active_session_id = 0
        self.begin_configs: list[StreamingSessionConfig] = []
        self.push_targets: list[tuple[int, StreamingTarget]] = []
        self.ended_sessions: list[tuple[int, str]] = []
        self.reject_sequences: set[int] = set()
        self.begin_error_code: Optional[str] = None
        self.begin_error: Optional[str] = None

    def begin_streaming_session(
        self,
        config: StreamingSessionConfig,
    ) -> StreamingBeginResult:
        if self.begin_error_code is not None:
            return StreamingBeginResult(
                accepted=False,
                error_code=self.begin_error_code,
                error=self.begin_error or "begin rejected",
            )
        if self.active_session_id:
            return StreamingBeginResult(
                accepted=False,
                error_code="invalid_request",
                error="streaming session already active",
            )
        session_id = self.next_session_id
        self.next_session_id += 1
        self.active_session_id = session_id
        self.begin_configs.append(config)
        self._state = ExecutionState.STREAMING
        self._emit_kernel_event(
            "streaming_session_started",
            details={
                "session_id": session_id,
                "input_mode": config.input_mode,
                "principal_id": config.principal_id,
                "authority_source": config.authority_source,
                "trigger_reason": config.trigger_reason,
            },
        )
        return StreamingBeginResult(accepted=True, session_id=session_id)

    def push_streaming_target(
        self,
        session_id: int,
        target: StreamingTarget,
    ) -> StreamingPushResult:
        self.push_targets.append((session_id, target))
        if session_id != self.active_session_id:
            self._emit_kernel_event(
                "streaming_target_rejected",
                error="session mismatch",
                details={
                    "session_id": session_id,
                    "source_sequence": target.source_sequence,
                    "kind": target.kind,
                    "reason_code": "session_lost",
                },
            )
            return StreamingPushResult(
                accepted=False,
                error_code="invalid_request",
                error="active streaming session does not match session_id",
            )
        if target.source_sequence in self.reject_sequences:
            self._emit_kernel_event(
                "streaming_target_rejected",
                error="IK failed",
                details={
                    "session_id": session_id,
                    "source_sequence": target.source_sequence,
                    "kind": target.kind,
                    "reason_code": "ik_failed",
                },
            )
            return StreamingPushResult(
                accepted=False,
                backend_sequence=0,
                error_code="ik_failed",
                error="IK failed",
            )
        backend_sequence = len(self.push_targets)
        self._emit_kernel_event(
            "streaming_target_dispatched",
            details={
                "session_id": session_id,
                "source_sequence": target.source_sequence,
                "backend_sequence": backend_sequence,
                "kind": target.kind,
            },
        )
        return StreamingPushResult(
            accepted=True,
            backend_sequence=backend_sequence,
        )

    def end_streaming_session(
        self,
        session_id: int,
        soft_stop_strategy: str = "spring_back",
    ) -> CommandResult:
        self.ended_sessions.append((session_id, soft_stop_strategy))
        if session_id != self.active_session_id:
            return CommandResult(accepted=False, error="session mismatch")
        self.active_session_id = 0
        self._state = ExecutionState.IDLE
        self._emit_kernel_event(
            "streaming_session_ended",
            details={
                "session_id": session_id,
                "soft_stop_strategy": soft_stop_strategy,
            },
        )
        return CommandResult(accepted=True)


def _app_with_kernel(kernel: _StreamingKernel) -> TestClient:
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    client = TestClient(app)
    response = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert response.status_code == 200
    return client


def _cartesian_frame(sequence: int) -> dict[str, object]:
    return {
        "op": "frame",
        "kind": "cartesian",
        "position_m": [0.4, 0.0, 0.2],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "source_sequence": sequence,
    }


class TestFrameToTarget:
    def test_cartesian_frame_requires_finite_pose(self):
        assert _frame_to_target(
            {
                "kind": "cartesian",
                "position_m": [0.1, 0.2, 0.3],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                "source_sequence": 11,
            }
        ) is not None
        assert _frame_to_target(
            {
                "kind": "cartesian",
                "position_m": [0.1, float("nan"), 0.3],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        ) is None
        assert _frame_to_target(
            {
                "kind": "cartesian",
                "position_m": [0.1, 0.2, 0.3],
                "orientation_xyzw": [0.0, 0.0, 1.0],
            }
        ) is None

    def test_joint_frame_accepts_partial_finite_joint_maps(self):
        target = _frame_to_target(
            {
                "kind": "joint",
                "joint_names": ["shoulder_pan_joint"],
                "joint_positions_rad": [0.25],
                "source_sequence": 12,
            }
        )

        assert target is not None
        assert target.kind == "joint"
        assert target.joint_names == ["shoulder_pan_joint"]
        assert target.joint_positions_rad == [0.25]
        assert _frame_to_target(
            {
                "kind": "joint",
                "joint_names": ["shoulder_pan_joint"],
                "joint_positions_rad": [],
            }
        ) is None
        assert _frame_to_target(
            {
                "kind": "joint",
                "joint_names": ["shoulder_pan_joint"],
                "joint_positions_rad": [float("inf")],
            }
        ) is None


class TestReasonCodes:
    def test_push_reason_code_prefers_structured_codes(self):
        assert _streaming_reason_code("ik_failed", "anything") == "ik_failed"
        assert _streaming_reason_code("unknown_joint_name", "anything") == "unknown_joint_name"
        assert _streaming_reason_code("invalid_state_transition", "anything") == "invalid_state"

    def test_push_reason_code_keeps_stable_message_fallbacks(self):
        assert (
            _streaming_reason_code(
                "invalid_request",
                "joint streaming target carries no joint_positions_rad",
            )
            == "malformed_frame"
        )
        assert (
            _streaming_reason_code(
                "invalid_request",
                "current robot state does not cover shoulder_pan_joint",
            )
            == "stale_state"
        )
        assert (
            _streaming_reason_code(
                "invalid_request",
                "tcp_frame is not configured",
            )
            == "missing_tcp_frame"
        )

    def test_begin_reason_codes_are_ui_stable(self):
        assert (
            _streaming_begin_reason_code(
                "invalid_request",
                "streaming session already active",
            )
            == "session_already_active"
        )
        assert (
            _streaming_begin_reason_code(
                "unsupported_capability",
                "backend does not support streaming",
            )
            == "unsupported_capability"
        )
        assert (
            _streaming_begin_reason_code(
                "invalid_state_transition",
                "valid only from Idle",
            )
            == "invalid_state"
        )


class TestFollowTargetWebSocket:
    def test_begin_push_reject_recover_end_records_direct_control_history(self):
        kernel = _StreamingKernel()
        kernel.reject_sequences.add(2)
        client = _app_with_kernel(kernel)

        with client:
            with client.websocket_connect("/ws/follow-target") as ws:
                ws.send_json(
                    {
                        "op": "begin",
                        "input_mode": "cartesian",
                        "principal_id": "human:test",
                        "authority_source": "human",
                        "trigger_reason": "operator_direct_control",
                    }
                )
                began = ws.receive_json()
                assert began == {"op": "began", "session_id": 1}

                ws.send_json(_cartesian_frame(1))
                accepted = ws.receive_json()
                assert accepted["op"] == "pushed"
                assert accepted["accepted"] is True
                assert accepted["source_sequence"] == 1
                assert accepted["backend_sequence"] == 1

                ws.send_json(_cartesian_frame(2))
                rejected = ws.receive_json()
                assert rejected["op"] == "pushed"
                assert rejected["accepted"] is False
                assert rejected["source_sequence"] == 2
                assert rejected["reason_code"] == "ik_failed"
                assert rejected["reason"] == "IK failed"

                ws.send_json(_cartesian_frame(3))
                recovered = ws.receive_json()
                assert recovered["op"] == "pushed"
                assert recovered["accepted"] is True
                assert recovered["source_sequence"] == 3

                ws.send_json({"op": "end", "soft_stop_strategy": "spring_back"})
                ended = ws.receive_json()
                assert ended["op"] == "ended"
                assert ended["accepted"] is True
                assert ended["session_id"] == 1

            events_response = client.get("/api/kernel/events?since=0")
            assert events_response.status_code == 200
            native_events = events_response.json()["events"]
            native_types = [event["type"] for event in native_events]
            assert native_types == [
                "streaming_session_started",
                "streaming_target_dispatched",
                "streaming_target_rejected",
                "streaming_target_dispatched",
                "streaming_session_ended",
            ]

            history_response = client.get(
                "/api/execution/history",
                params={"eventTypes": _DIRECT_CONTROL_EVENT_TYPES},
            )
            assert history_response.status_code == 200
            history_events = history_response.json()["events"]
            assert [event["type"] for event in history_events] == [
                "streaming_session_started",
                "streaming_target_rejected",
                "streaming_session_ended",
            ]
            assert all(
                event["execution_mode"] == "direct_control"
                for event in history_events
            )
            assert all(
                event["component"] == "manyforge.direct_control"
                for event in history_events
            )
            reject_event = history_events[1]
            assert reject_event["severity"] == "WARN"
            assert reject_event["details"]["reason_code"] == "ik_failed"
            assert reject_event["details"]["source_sequence"] == 2
            assert reject_event["details"]["native_kernel_event_index"] == 3

            # Re-reading the same native range must not duplicate durable
            # history events: native event_index is the dedupe key across
            # websocket-status and route-drain ingestion paths.
            assert client.get("/api/kernel/events?since=0").status_code == 200
            second_history = client.get(
                "/api/execution/history",
                params={"eventTypes": _DIRECT_CONTROL_EVENT_TYPES},
            ).json()["events"]
            assert [event["event_index"] for event in second_history] == [
                event["event_index"] for event in history_events
            ]

    def test_malformed_frame_is_transient_and_next_valid_frame_recovers(self):
        kernel = _StreamingKernel()
        client = _app_with_kernel(kernel)

        with client:
            with client.websocket_connect("/ws/follow-target") as ws:
                ws.send_json({"op": "begin", "input_mode": "cartesian"})
                assert ws.receive_json()["op"] == "began"

                ws.send_json(
                    {
                        "op": "frame",
                        "kind": "cartesian",
                        "position_m": [0.1, 0.2],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        "source_sequence": 10,
                    }
                )
                rejected = ws.receive_json()
                assert rejected["accepted"] is False
                assert rejected["reason_code"] == "malformed_frame"
                assert rejected["source_sequence"] == 10

                ws.send_json(_cartesian_frame(11))
                accepted = ws.receive_json()
                assert accepted["accepted"] is True
                assert accepted["source_sequence"] == 11

                ws.send_json({"op": "end"})
                assert ws.receive_json()["op"] == "ended"

            assert [target.source_sequence for _, target in kernel.push_targets] == [11]

    def test_socket_disconnect_ends_session_and_next_session_can_begin(self):
        kernel = _StreamingKernel()
        client = _app_with_kernel(kernel)

        with client:
            with client.websocket_connect("/ws/follow-target") as ws:
                ws.send_json({"op": "begin", "input_mode": "joint"})
                assert ws.receive_json() == {"op": "began", "session_id": 1}
                ws.close()

            assert kernel.ended_sessions == [(1, "spring_back")]
            assert kernel.active_session_id == 0
            reset = client.post("/api/cycle/reset")
            assert reset.status_code == 200

            with client.websocket_connect("/ws/follow-target") as ws:
                ws.send_json({"op": "begin", "input_mode": "joint"})
                assert ws.receive_json() == {"op": "began", "session_id": 2}
                ws.send_json({"op": "end"})
                assert ws.receive_json()["op"] == "ended"

    def test_begin_failure_returns_stable_error_code_and_closes_socket(self):
        kernel = _StreamingKernel()
        kernel.begin_error_code = "invalid_state_transition"
        kernel.begin_error = "begin_streaming_session valid only from Idle"
        client = _app_with_kernel(kernel)

        with client:
            with client.websocket_connect("/ws/follow-target") as ws:
                ws.send_json({"op": "begin", "input_mode": "cartesian"})
                error = ws.receive_json()
                assert error["op"] == "error"
                assert error["code"] == "invalid_state"
                assert "Idle" in error["message"]
