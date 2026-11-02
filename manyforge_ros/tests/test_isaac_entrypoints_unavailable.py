"""Keep retired private Isaac wires fenced while selected clients are active."""

from __future__ import annotations

import inspect

import pytest

from manyforge_ros import inspect_isaac_scene
from manyforge_ros import isaac_mesh_transfer
from manyforge_ros import scene_mirror_probe
from manyforge_ros import simulation_control


@pytest.mark.parametrize(
    ("module", "removed_client"),
    [
        (inspect_isaac_scene, "InspectIsaacSceneClient"),
        (isaac_mesh_transfer, "IsaacMeshTransferClient"),
    ],
)
def test_retired_private_isaac_entrypoints_stay_fenced(
    module: object,
    removed_client: str,
    capsys: pytest.CaptureFixture[str],
) -> None:
    assert module.main([]) == 78
    assert capsys.readouterr().err.strip() == module.UNAVAILABLE_MESSAGE
    whole_module = inspect.getsource(module)
    assert f"class {removed_client}" not in whole_module
    assert "create_client" not in whole_module


def test_selected_standard_simulation_clients_are_active_and_owner_neutral() -> None:
    assert hasattr(simulation_control, "SimulationControlClient")
    assert hasattr(scene_mirror_probe, "SimulationSceneMirrorClient")
    for module in (simulation_control, scene_mirror_probe):
        source = inspect.getsource(module)
        assert "simulation_interfaces" in source
        assert "create_client" in source
        assert "create_publisher" not in source
        assert "isaac_arm_command_enable" not in source
        assert "isaac_demo_supervisor" not in source
