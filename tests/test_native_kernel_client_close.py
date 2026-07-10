"""Unit tests for NativeKernelClient.close() — deterministic shutdown teardown.

Pure-Python: the bundle is duck-typed, so a stub exercises the close
sequence without the native extension.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(
    0, str(Path(__file__).parent.parent / "manyforge_roboplan" / "python")
)

from native_kernel_client import NativeKernelClient  # noqa: E402


class _StubBundle:
    """Records the close-sequence calls in order."""

    # state_name/int-state shape mirrors bindings.cpp status_to_python():
    # the real payload carries `state` as an INT and the stable name in
    # `state_name` — close() must decode via _state_from_native, not the
    # numeric field.
    def __init__(
        self,
        state_name: str = "Executing",
        gate_raises: bool = False,
        status_raises: bool = False,
    ):
        self.calls: list[str] = []
        self._state_name = state_name
        self._gate_raises = gate_raises
        self._status_raises = status_raises

    def get_status(self):
        self.calls.append("get_status")
        if self._status_raises:
            raise RuntimeError("status unavailable")
        return {
            "state": 3,
            "state_name": self._state_name,
            "execution_id": 7,
            "progress": 0.5,
            "active_move_id": "move_1",
            "last_error": "None",
        }

    def stop(self):
        self.calls.append("stop")
        return {"accepted": True}

    def set_command_gate(self, enabled: bool):
        self.calls.append(f"set_command_gate:{enabled}")
        if self._gate_raises:
            raise RuntimeError("no gate surface")
        return True

    def close(self):
        self.calls.append("close")


def test_close_stops_active_execution_then_closes_gate_then_drops_bundle():
    bundle = _StubBundle(state_name="Executing")
    client = NativeKernelClient(bundle)

    client.close()

    assert bundle.calls == ["get_status", "stop", "set_command_gate:False", "close"]
    assert client._bundle is None


def test_close_skips_stop_when_kernel_terminal_but_still_closes_gate():
    bundle = _StubBundle(state_name="Idle")
    client = NativeKernelClient(bundle)

    client.close()

    assert "stop" not in bundle.calls
    assert "set_command_gate:False" in bundle.calls
    assert bundle.calls[-1] == "close"
    assert client._bundle is None


def test_close_is_idempotent_and_survives_gateless_backend():
    bundle = _StubBundle(state_name="Executing", gate_raises=True)
    client = NativeKernelClient(bundle)

    client.close()
    client.close()  # second call must be a no-op, not an AttributeError

    assert bundle.calls.count("stop") == 1
    assert bundle.calls.count("close") == 1
    assert client._bundle is None


def test_close_still_closes_native_bundle_when_status_probe_fails():
    bundle = _StubBundle(status_raises=True)
    client = NativeKernelClient(bundle)

    client.close()

    assert bundle.calls == ["get_status", "set_command_gate:False", "close"]
    assert client._bundle is None


def test_command_result_preserves_structured_error_details():
    client = object.__new__(NativeKernelClient)

    result = client._command_result(
        {
            "accepted": False,
            "error": "runtime-collision stop is not yet certified",
            "error_details": {
                "reason_code": "runtime_collision_stop_not_yet_certified",
                "retryable": "false",
                "preemption_result": "backend_preempt_failed",
            },
        }
    )

    assert result.accepted is False
    assert result.reason_code == "runtime_collision_stop_not_yet_certified"
    assert result.details == {
        "reason_code": "runtime_collision_stop_not_yet_certified",
        "retryable": "false",
        "preemption_result": "backend_preempt_failed",
    }
