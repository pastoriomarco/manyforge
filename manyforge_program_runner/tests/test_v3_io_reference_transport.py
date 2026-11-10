"""I/O transport carries the exact component/signal pair."""

import pytest
from pydantic import ValidationError
from pathlib import Path
from types import SimpleNamespace

from manyforge_behavior.catalog import NodeCatalog
from manyforge_program_runner.execution_transport import (
    _IoWatchRequest,
    _MockSimulationRow,
    _selectors,
)
from manyforge_program_runner.runtime import ProgramRuntime
from manyforge_program_runner.active_tcp_mirror import decode_active_tcp_rows


def test_watch_uses_direct_signal_and_rejects_role_fields():
    request = {
        "schema_version": "manyforge.io_watch_request.v2",
        "owner_epoch": "owner",
        "program_binding_id": "sha256:" + "0" * 64,
        "program_runner_owner_epoch": "runner",
        "claim_generation": 1,
        "program_run_id": "run",
        "node_instance_id": "node",
        "node_attempt_id": "attempt",
        "signal": {"component_id": "robot_io", "signal_name": "CI0"},
        "condition": {"kind": "level", "expected_value": True},
    }
    parsed = _IoWatchRequest.model_validate(request)
    assert parsed.signal.signal_name == "CI0"
    with pytest.raises(ValidationError):
        _IoWatchRequest.model_validate({**request, "target_role_id": "arm"})


def test_mock_receipt_selector_preserves_component_and_signal_name():
    rows = [_MockSimulationRow.model_validate({
        "signal": {"component_id": "robot_io", "signal_name": "CI0"},
        "start_value": False,
        "change": None,
    })]
    assert _selectors(rows) == [("robot_io", "CI0")]


def test_simulator_dependency_keeps_direct_signal_identity_until_binding_join():
    catalog = NodeCatalog(Path(__file__).parents[2] / "manyforge_behavior/resources/node_catalog.yaml")
    owner = SimpleNamespace(_environment=SimpleNamespace(catalog=catalog))
    simulated, references = ProgramRuntime._simulation_node_requirements(
        owner, "io.set_output", {
            "signal": {"component_id": "plc_io", "signal_name": "TO0"}, "value": True
        },
    )
    assert not simulated
    assert references == {("signal", "plc_io", "TO0")}


def test_active_tcp_reading_preserves_mounted_tool_frame_owner():
    entry = {
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "frame": {"component_id": "tool", "frame_id": "tcp"},
    }
    decoded = decode_active_tcp_rows([{
        "component_id": "robot", "owner_epoch": "owner",
        "registration_generation": 1, "sequence": 1, "entry": entry,
    }])
    assert decoded[0][-1] == entry


def test_active_tcp_mirror_writes_exact_component_and_target():
    binding = {"target_bindings": [
        {"target": {"component_id": "robot_a", "command_target_id": "arm"}, "robot_tcps": []},
        {"target": {"component_id": "robot_b", "command_target_id": "arm"}, "robot_tcps": []},
    ]}
    selected = ProgramRuntime._active_tcp_targets(binding)
    written = []
    store = SimpleNamespace(write_active_tcp=lambda component, target, entry: written.append(
        (component, target, entry)
    ))
    ProgramRuntime._write_store_active_tcp(store, selected, "robot_a", None)
    assert written == [("robot_a", "arm", None)]
