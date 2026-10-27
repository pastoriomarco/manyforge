"""Focused task, context, translation, and recovery tests for I9."""

from __future__ import annotations

import json
from pathlib import Path
from threading import Event
import time
from types import SimpleNamespace

import pytest

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskState,
    ProgramRunPortContext,
)
from manyforge_ros.program_simulation_port import (
    SimulationProgramSiteConfig,
    SimulationProgramTaskPort,
    load_simulation_program_site_config,
)


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


def _site() -> SimulationProgramSiteConfig:
    return SimulationProgramSiteConfig(
        service_prefix="/isaacsim",
        service_timeout_s=2.0,
    )


_SIMULATION_SCENE = {
    "cell_frame_id": "world",
    "scene_entities": [
        {"entity": "/World/retained_part", "scene_resource_id": "part"},
        # An orphan link (object not declared by the Program) is ignored.
        {"entity": "/World/elsewhere", "scene_resource_id": "zz_orphan"},
    ],
}


def _context(
    *,
    program_run_id: str = "run.simulation",
    scene_resources: tuple[str, ...] = ("part",),
    simulation_scene: dict[str, object] | None = _SIMULATION_SCENE,
) -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id=program_run_id,
        applied_record_digest="sha256:" + "9" * 64,
        scenario_id="isaac-cell",
        source_document_digest=_DIGEST_A,
        program_id="simulation_program",
        program_revision=_DIGEST_B,
        program_binding_id=_DIGEST_C,
        retained_bundle_id=_DIGEST_A,
        graph_revision=_DIGEST_B,
        node_environment_digest=_DIGEST_C,
        program={
            "program_id": "simulation_program",
            "program_revision": _DIGEST_B,
            "scene": [
                {"scene_resource_id": resource_id}
                for resource_id in scene_resources
            ],
        },
        simulation_scene=simulation_scene,
        program_binding={
            "program_binding_id": _DIGEST_C,
            "program_id": "simulation_program",
            "program_revision": _DIGEST_B,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "target_role_bindings": [
                {
                    "target_role_id": "arm",
                    "frame_role_bindings": [
                        {
                            "frame_role_id": "base",
                            "compiled_frame_name": "retained_arm_base",
                        }
                    ],
                }
            ],
        },
    )


def _identity(index: int = 1) -> NodeAttemptIdentity:
    return NodeAttemptIdentity(
        program_run_id="run.simulation",
        node_instance_id=f"simulation_node_{index}",
        node_attempt_id=f"run.simulation/simulation_node_{index}/{index}",
    )


def _result(**values: object) -> SimpleNamespace:
    return SimpleNamespace(
        success=True,
        duration_s=0.001,
        result_code=0,
        service_name="/isaacsim/service",
        **values,
    )


class _ControlClient:
    def __init__(self) -> None:
        self.calls: list[tuple[str, object]] = []

    def reset(self, *, scope: str = "state") -> SimpleNamespace:
        self.calls.append(("reset", scope))
        return _result()

    def set_simulation_state(self, *, state: str) -> SimpleNamespace:
        self.calls.append(("set_state", state))
        return _result(state=state)

    def step(self, *, steps: int = 1) -> SimpleNamespace:
        self.calls.append(("step", steps))
        return _result(steps=steps)

    def set_entity_state(
        self,
        *,
        entity: str,
        pose: dict[str, object],
    ) -> SimpleNamespace:
        self.calls.append(("set_entity_state", (entity, pose)))
        return _result(entity=entity)


def _settled(port: SimulationProgramTaskPort, task_id: str):
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        snapshot = port.observe(task_id)
        if snapshot.state is not OwnerTaskState.ACTIVE:
            return snapshot
        time.sleep(0.005)
    raise AssertionError("simulation task did not settle")


def test_site_config_is_strict_canonical_and_rejects_symlinks(
    tmp_path: Path,
) -> None:
    value = {
        "api_version": "manyforge.simulation_program_site_config.v2",
        "service_prefix": "/isaacsim",
        "service_timeout_s": 2.0,
    }
    path = tmp_path / "simulation-site.json"
    path.write_text(json.dumps(value), encoding="utf-8")

    loaded = load_simulation_program_site_config(path)

    assert loaded == _site()
    link = tmp_path / "simulation-site-link.json"
    link.symlink_to(path)
    with pytest.raises(ValueError, match="readable plain file"):
        load_simulation_program_site_config(link)

    path.write_text('{"api_version":1,"api_version":2}', encoding="utf-8")
    with pytest.raises(ValueError, match="duplicate JSON field"):
        load_simulation_program_site_config(path)
    # v1 carried the per-scenario entity mapping; it is refused.
    path.write_text(
        json.dumps({**value, "scenarios": []}),
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="unknown or missing fields"):
        load_simulation_program_site_config(path)


def test_program_tasks_translate_all_four_nodes_without_blocking_ticks() -> None:
    client = _ControlClient()
    port = SimulationProgramTaskPort(client, _site())
    port.bind(_context())
    cases = (
        ("simulation.reset", {}, 1),
        ("simulation.set_state", {"state": "play"}, 2),
        ("simulation.step", {"steps": 7}, 3),
        (
            "simulation.set_entity_pose",
            {
                "scene_resource_id": "part",
                "pose": {
                    "frame_ref": {
                        "kind": "target_frame",
                        "target_role": "arm",
                        "frame_role": "base",
                    },
                    "position_m": [0.25, -0.5, 0.75],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
            },
            4,
        ),
    )
    for node_id, parameters, index in cases:
        identity = _identity(index)
        started = port.start(
            node_id,
            identity,
            OwnerNodeInputs(parameters=parameters),
        )
        assert started.state is OwnerTaskState.ACTIVE
        assert port.start(
            node_id,
            identity,
            OwnerNodeInputs(parameters=parameters),
        ).task_id == started.task_id
        terminal = _settled(port, started.task_id)
        assert terminal.state is OwnerTaskState.SUCCEEDED
        port.acknowledge(started.task_id, identity)

    assert client.calls[:3] == [
        ("reset", "state"),
        ("set_state", "play"),
        ("step", 7),
    ]
    assert client.calls[3] == (
        "set_entity_state",
        (
            "/World/retained_part",
            {
                "frame_id": "retained_arm_base",
                "position_m": [0.25, -0.5, 0.75],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        ),
    )
    assert len(client.calls) == 4
    with pytest.raises(ValueError, match="reused with new intent"):
        port.start(
            "simulation.step",
            _identity(3),
            OwnerNodeInputs(parameters={"steps": 8}),
        )
    with pytest.raises(ValueError, match="reused with new intent"):
        port.start(
            "simulation.set_state",
            _identity(3),
            OwnerNodeInputs(parameters={"state": "pause"}),
        )
    port.unbind("run.simulation")
    port.close()


class _BlockingControlClient(_ControlClient):
    def __init__(self) -> None:
        super().__init__()
        self.entered = Event()
        self.release = Event()

    def reset(self, *, scope: str = "state") -> SimpleNamespace:
        self.entered.set()
        assert self.release.wait(timeout=2.0)
        return super().reset(scope=scope)


def test_cancel_fences_late_results_and_context_cleanup_waits_for_settlement() -> None:
    client = _BlockingControlClient()
    port = SimulationProgramTaskPort(client, _site())
    port.bind(_context())
    identity = _identity()
    started = port.start(
        "simulation.reset",
        identity,
        OwnerNodeInputs(parameters={}),
    )
    assert client.entered.wait(timeout=1.0)

    port.cancel(started.task_id, identity)
    with pytest.raises(RuntimeError, match="still settling"):
        port.unbind("run.simulation")
    client.release.set()
    terminal = _settled(port, started.task_id)
    assert terminal.state is OwnerTaskState.INTERRUPTED
    assert terminal.reason_code == "simulation_request_cancelled"
    port.unbind("run.simulation")
    port.close()


def _entity_pose(resource_id: str, frame_ref: dict[str, object]) -> OwnerNodeInputs:
    return OwnerNodeInputs(
        parameters={
            "scene_resource_id": resource_id,
            "pose": {
                "frame_ref": frame_ref,
                "position_m": [0.0, 0.0, 0.0],
                "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
            },
        }
    )


def test_entity_motion_requires_declared_and_explicitly_mapped_identity() -> None:
    client = _ControlClient()
    port = SimulationProgramTaskPort(client, _site())
    port.bind(_context(scene_resources=("part", "unmapped")))
    with pytest.raises(ValueError, match="no simulator entity mapping"):
        port.start(
            "simulation.set_entity_pose",
            _identity(),
            _entity_pose("unmapped", {"kind": "cell_frame"}),
        )
    # The orphan link is not a declared resource of this Program.
    with pytest.raises(ValueError, match="not declared by the Program"):
        port.start(
            "simulation.set_entity_pose",
            _identity(2),
            _entity_pose("zz_orphan", {"kind": "cell_frame"}),
        )
    started = port.start(
        "simulation.set_entity_pose",
        _identity(3),
        _entity_pose("part", {"kind": "cell_frame"}),
    )
    assert _settled(port, started.task_id).state is OwnerTaskState.SUCCEEDED
    assert client.calls[0][1][0] == "/World/retained_part"
    assert client.calls[0][1][1]["frame_id"] == "world"
    port.close()


def test_cell_frame_without_simulation_links_fails_the_node_typed() -> None:
    client = _ControlClient()
    port = SimulationProgramTaskPort(client, _site())
    port.bind(_context(simulation_scene=None))
    refused = port.start(
        "simulation.set_entity_pose",
        _identity(),
        _entity_pose("part", {"kind": "cell_frame"}),
    )
    assert refused.state is OwnerTaskState.FAILED
    assert refused.reason_code == "simulation_cell_frame_unavailable"
    with pytest.raises(ValueError, match="no simulator entity mapping"):
        port.start(
            "simulation.set_entity_pose",
            _identity(2),
            _entity_pose(
                "part",
                {"kind": "target_frame", "target_role": "arm", "frame_role": "base"},
            ),
        )
    assert client.calls == []
    port.close()
