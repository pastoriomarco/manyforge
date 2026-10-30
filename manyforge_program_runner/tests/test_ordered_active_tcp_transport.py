"""Exercise the wire option without a native host."""

import pytest

from manyforge_program_runner.execution_transport import (
    EXECUTION_RESPONSE_VERSION,
    ProgramRunnerExecutionTransport,
)


class _Transport(ProgramRunnerExecutionTransport):
    def __init__(self):
        self.commands = []

    def _exchange(self, command):
        self.commands.append(command)
        return {
            "api_version": EXECUTION_RESPONSE_VERSION,
            "command": command["command"],
            "ok": True,
            "robots": [],
        }


def test_ordered_read_is_opt_in_on_wire():
    transport = _Transport()
    transport.read_active_tcp()
    transport.read_active_tcp(ordered=True)
    assert "ordered" not in transport.commands[0]
    assert transport.commands[1]["ordered"] is True
    with pytest.raises(TypeError):
        transport.read_active_tcp(ordered=1)
