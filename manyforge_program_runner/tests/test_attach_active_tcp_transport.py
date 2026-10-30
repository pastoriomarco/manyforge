"""The attach query keeps native refusal codes across the transport."""

import pytest

from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.execution_transport import ProgramRunnerExecutionTransport


@pytest.mark.parametrize("code", ["attach_active_tcp_unavailable", "attach_active_tcp_not_holder"])
def test_native_attach_refusal_is_preserved(monkeypatch, tmp_path, code):
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    packets = []
    monkeypatch.setattr(transport, "_exchange", lambda packet: packets.append(packet) or {
        "ok": False, "error": {"code": code, "message": "refused", "details": {"reason": "test"}},
    })
    with pytest.raises(HeadlessApiError) as caught:
        transport.resolve_attach_active_tcp("binding", "tool")
    assert caught.value.code == code
    assert caught.value.details == {"reason": "test"}
    assert packets == [{"api_version": "manyforge.execution_command.v1",
                        "command": "resolve_attach_active_tcp",
                        "program_binding_id": "binding", "target_role_id": "tool"}]


def test_native_attach_success_returns_holder_frame(monkeypatch, tmp_path):
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1)
    monkeypatch.setattr(transport, "_exchange", lambda _packet: {
        "ok": True, "command": "resolve_attach_active_tcp", "frame_role_id": "tip",
    })
    assert transport.resolve_attach_active_tcp("binding", "tool") == "tip"
