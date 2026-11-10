"""Direct target selection uses the exact captured Program Binding."""

from types import SimpleNamespace

import pytest

from manyforge_behavior.program_runtime import ProgramRuntimeError, _effectful_targets
from manyforge_behavior.program_run_owner import (
    _runtime_scale_selector_for_group,
    _selection_resource_ids,
)


def _entry():
    return SimpleNamespace(
        parameters=[SimpleNamespace(id="signal", has_default=False)],
        required_capabilities=[SimpleNamespace(
            scope="target_binding", effectful=True, target_parameter="signal",
            when_parameter=None,
        )],
    )


def test_effectful_signal_uses_its_exact_sealed_target():
    signal = {"component_id": "plc_io", "signal_name": "TO0"}
    binding = {"signal_bindings": [
        {"reference": signal, "target": {"component_id": "plc_io", "command_target_id": "io"}},
        {"reference": {"component_id": "station_io", "signal_name": "TO0"},
         "target": {"component_id": "station_io", "command_target_id": "io"}},
    ]}
    assert _effectful_targets(_entry(), {"signal": signal}, binding) == {("plc_io", "io")}
    with pytest.raises(ProgramRuntimeError, match="unique captured target"):
        _effectful_targets(_entry(), {"signal": signal}, {"signal_bindings": []})
    with pytest.raises(ProgramRuntimeError, match="unique captured target"):
        _effectful_targets(_entry(), {"signal": signal}, {"signal_bindings": [binding["signal_bindings"][0]] * 2})


def test_selection_uses_exact_component_and_target_for_scale_and_resources():
    run = SimpleNamespace(
        capture=SimpleNamespace(program_binding_id="binding"),
        execution_selection_motion_targets=lambda _group: (("robot_a", "arm"),),
        execution_selection_effect_targets=lambda _group: (("robot_a", "arm"),),
    )
    binding = {"target_bindings": [
        {"target": {"component_id": "robot_a", "command_target_id": "arm"},
         "resource_role_bindings": [{"resource_ids": ["joint_a"]}]},
        {"target": {"component_id": "robot_b", "command_target_id": "arm"},
         "resource_role_bindings": [{"resource_ids": ["joint_b"]}]},
    ]}
    assert _runtime_scale_selector_for_group(run, binding, "all") == (
        {"program_binding_id": "binding", "component_id": "robot_a", "command_target_id": "arm"}, None
    )
    assert _selection_resource_ids(run, binding, "all") == ["joint_a"]
