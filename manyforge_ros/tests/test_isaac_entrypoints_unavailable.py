"""Source-level acceptance for pre-I9 Isaac entrypoint fences."""

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
        (scene_mirror_probe, "SimulationSceneMirrorClient"),
        (simulation_control, "SimulationControlClient"),
    ],
)
def test_live_isaac_entrypoints_fail_before_ros_effects(
    module: object,
    removed_client: str,
    capsys: pytest.CaptureFixture[str],
) -> None:
    assert module.main() == 78
    assert capsys.readouterr().err.strip() == module.UNAVAILABLE_MESSAGE

    source = inspect.getsource(module.main)
    for forbidden in (
        "parse_args",
        "rclpy",
        "create_node",
        "publish",
        "transfer",
        "capture",
    ):
        assert forbidden not in source

    whole_module = inspect.getsource(module)
    assert f"class {removed_client}" not in whole_module
    assert "def _import_module" not in whole_module


def test_one_shot_simulator_reset_is_unavailable_without_a_live_import_hook() -> None:
    assert not hasattr(simulation_control, "_import_module")
    source = inspect.getsource(simulation_control.reset_once)
    for forbidden in ("import", "rclpy", "create_node", "SimulationControlClient"):
        assert forbidden not in source

    with pytest.raises(
        simulation_control.SimulationControlUnavailableError,
        match=simulation_control.UNAVAILABLE_REASON_CODE,
    ):
        simulation_control.reset_once()
