from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace
import threading

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend import state as state_module
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.tests.node_environment_support import (
    node_environment_for_catalog,
)
from manyforge_supervision import ComponentHealthReducer
from manyforge_supervision import ComponentHeartbeat
from manyforge_supervision import ComponentHealthState
from manyforge_supervision import ComponentPolicy
from manyforge_supervision import ComponentRole
from manyforge_supervision import SupervisionPolicy


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = (
    _REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)


class _LifecycleClient:
    def __init__(self) -> None:
        self.payload = {
            "providerHealthy": True,
            "fresh": True,
            "canExecute": True,
            "runtimeGenerationId": "runtime-1",
            "authoritySessionGeneration": 7,
            "snapshotGeneration": 9,
        }

    def status(self) -> dict[str, object]:
        return dict(self.payload)


class _CycleManager:
    def __init__(self) -> None:
        self.program_run_id = "run-1"
        self.state = SimpleNamespace(value="running")
        self.containment_calls: list[dict[str, object]] = []
        self.contained = threading.Event()

    def contain_execution_environment_loss(
        self,
        *,
        reason_code: str,
        details: dict[str, object],
        is_current,
    ) -> bool:
        if not is_current():
            return False
        self.containment_calls.append(
            {"reason_code": reason_code, "details": details}
        )
        self.contained.set()
        return True


def _reducer() -> ComponentHealthReducer:
    policies = (
        ComponentPolicy(
            component_role=ComponentRole.CELL_SUPERVISOR,
            component_id="supervisor",
            maximum_heartbeat_age_ns=10_000_000_000,
            maximum_critical_progress_age_ns=10_000_000_000,
        ),
        ComponentPolicy(
            component_role=ComponentRole.TRANSPORT_OWNER,
            component_id="owner",
            maximum_heartbeat_age_ns=10_000_000_000,
            maximum_critical_progress_age_ns=10_000_000_000,
            dependency_ids=("supervisor",),
            expected_runtime_generation="runtime-1",
        ),
        ComponentPolicy(
            component_role=ComponentRole.LIFECYCLE_PROVIDER,
            component_id="provider",
            maximum_heartbeat_age_ns=10_000_000_000,
            maximum_critical_progress_age_ns=10_000_000_000,
            dependency_ids=("supervisor", "owner"),
            expected_binding_generation="binding-1",
        ),
        ComponentPolicy(
            component_role=ComponentRole.EXECUTION_BACKEND,
            component_id="backend",
            maximum_heartbeat_age_ns=10_000_000_000,
            maximum_critical_progress_age_ns=10_000_000_000,
            dependency_ids=("supervisor", "owner", "provider"),
        ),
        ComponentPolicy(
            component_role=ComponentRole.CONTROL_AUTHORITY,
            component_id="authority",
            maximum_heartbeat_age_ns=10_000_000_000,
            maximum_critical_progress_age_ns=10_000_000_000,
            dependency_ids=("supervisor", "provider", "backend"),
        ),
    )
    reducer = ComponentHealthReducer(
        SupervisionPolicy(
            cell_id="lite6-a",
            robot_ids=("lite6-a",),
            authority_profile="local_operator",
            components=policies,
        ),
        clock_ns=lambda: 1_000_000_000,
    )
    epochs: dict[str, str] = {}
    for policy in policies:
        epoch = f"{policy.component_id}-epoch-1"
        assert reducer.ingest(
            ComponentHeartbeat(
                component_role=policy.component_role,
                component_id=policy.component_id,
                robot_id="lite6-a",
                cell_id="lite6-a",
                component_epoch=epoch,
                sequence=1,
                health_state=ComponentHealthState.HEALTHY,
                critical_progress_sequence=1,
                dependency_epochs={
                    dependency: epochs[dependency]
                    for dependency in policy.dependency_ids
                },
                runtime_generation=(
                    "runtime-1"
                    if policy.component_role
                    == ComponentRole.TRANSPORT_OWNER
                    else ""
                ),
                binding_generation=(
                    "binding-1"
                    if policy.component_role
                    == ComponentRole.LIFECYCLE_PROVIDER
                    else ""
                ),
            ),
            received_at_ns=1_000_000_000,
        )
        epochs[policy.component_id] = epoch
    assert reducer.snapshot(now_ns=1_000_000_000).dispatch_admitted
    return reducer


def test_active_cycle_epoch_change_is_contained_once_and_requires_reset(
    monkeypatch,
) -> None:
    monkeypatch.setattr(state_module, "RobotLifecycleClient", _LifecycleClient)
    state = ComposerState(
        node_environment=node_environment_for_catalog(str(_CATALOG_PATH)),
        kernel=MockKernelClient(),
    )
    manager = _CycleManager()
    lifecycle = _LifecycleClient()
    reducer = _reducer()
    state.cycle_manager = manager
    state._robot_lifecycle_client = lifecycle
    state._cell_supervision_reducer = reducer

    state._arm_cycle_execution_environment(manager)
    pin = state._active_cycle_environment_pin
    assert pin is not None
    assert pin.program_run_id == "run-1"
    assert pin.component_epochs["owner"] == "owner-epoch-1"

    assert reducer.ingest(
        ComponentHeartbeat(
            component_role=ComponentRole.TRANSPORT_OWNER,
            component_id="owner",
            robot_id="lite6-a",
            cell_id="lite6-a",
            component_epoch="owner-epoch-2",
            sequence=1,
            health_state=ComponentHealthState.HEALTHY,
            critical_progress_sequence=1,
            dependency_epochs={"supervisor": "supervisor-epoch-1"},
            runtime_generation="runtime-1",
        ),
        received_at_ns=1_000_000_000,
    )
    changed = reducer.snapshot(now_ns=1_000_000_000)
    state._validate_active_cycle_execution_environment(
        reducer=reducer,
        snapshot=changed,
        lifecycle_status=lifecycle.status(),
    )
    assert manager.contained.wait(timeout=1.0)
    assert len(manager.containment_calls) == 1
    assert state._active_cycle_environment_pin is None
    assert state._cycle_environment_reset_required is True

    state._validate_active_cycle_execution_environment(
        reducer=reducer,
        snapshot=changed,
        lifecycle_status=lifecycle.status(),
    )
    assert len(manager.containment_calls) == 1

    state._retire_cycle_execution_environment_after_reset(manager)
    assert state._cycle_environment_reset_required is False


def test_active_cycle_lifecycle_admission_loss_is_terminal(
    monkeypatch,
) -> None:
    monkeypatch.setattr(state_module, "RobotLifecycleClient", _LifecycleClient)
    state = ComposerState(
        node_environment=node_environment_for_catalog(str(_CATALOG_PATH)),
        kernel=MockKernelClient(),
    )
    manager = _CycleManager()
    lifecycle = _LifecycleClient()
    reducer = _reducer()
    state.cycle_manager = manager
    state._robot_lifecycle_client = lifecycle
    state._cell_supervision_reducer = reducer
    state._arm_cycle_execution_environment(manager)

    lifecycle.payload["canExecute"] = False
    state._validate_active_cycle_execution_environment(
        reducer=reducer,
        snapshot=reducer.snapshot(now_ns=1_000_000_000),
        lifecycle_status=lifecycle.status(),
    )

    assert manager.contained.wait(timeout=1.0)
    assert manager.containment_calls[0]["reason_code"] == (
        "execution_environment_admission_lost"
    )
    assert state._cycle_environment_reset_required is True


def test_composer_reducer_consumes_external_and_provider_health(
    monkeypatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setattr(state_module, "RobotLifecycleClient", _LifecycleClient)
    state = ComposerState(
        node_environment=node_environment_for_catalog(str(_CATALOG_PATH)),
        kernel=MockKernelClient(),
    )
    lifecycle = _LifecycleClient()
    lifecycle.payload.update(
        {
            "transportOwnerComponentEpoch": "owner-epoch-1",
            "providerComponentEpoch": "provider-epoch-1",
            "observationFresh": True,
            "transportOwnerProgressSequence": 10,
            "transportOwnerProgressHealthy": True,
            "providerDeliverySequence": 10,
        }
    )
    state._robot_lifecycle_client = lifecycle
    reducer = _reducer()
    heartbeat_path = tmp_path / "cell-supervisor-heartbeat.json"
    heartbeat_path.write_text(
        json.dumps(
            {
                "schema": "manyforge.cell_supervisor.heartbeat.v1",
                "componentRole": "cell_supervisor",
                "componentId": "supervisor",
                "robotId": "lite6-a",
                "cellId": "lite6-a",
                "componentEpoch": "supervisor-epoch-1",
                "sequence": 1,
                "healthState": "healthy",
                "criticalProgressSequence": 1,
                "dependencyEpochs": {},
                "runtimeGeneration": "",
                "bindingGeneration": "",
                "configurationGeneration": "",
                "desiredState": "running",
                "reasonCodes": [],
                "requestedContainment": "none",
                "observedContainers": {
                    "composer": True,
                    "owner": True,
                },
                "uncleanTerminations": [],
                "observedAtSteadyTimeNs": 1,
                "observedAtUtc": "2026-07-26T00:00:00+00:00",
            }
        ),
        encoding="utf-8",
    )
    state._cell_supervision_reducer = reducer
    state._cell_supervision_manifest = {
        "runtime_generation_id": "runtime-1"
    }
    state._cell_supervision_heartbeat_path = heartbeat_path
    state._cell_supervision_expected_epoch = "supervisor-epoch-1"
    state._cell_supervision_component_ids = {
        ComponentRole.CELL_SUPERVISOR: "supervisor",
        ComponentRole.TRANSPORT_OWNER: "owner",
        ComponentRole.LIFECYCLE_PROVIDER: "provider",
        ComponentRole.EXECUTION_BACKEND: "backend",
        ComponentRole.CONTROL_AUTHORITY: "authority",
    }

    healthy, reason = state._reduce_cell_supervision(
        heartbeat_sequence=1,
        critical_probe_healthy=True,
        critical_probe_reason="",
    )
    assert healthy is True
    assert reason == ""

    lifecycle.payload["fresh"] = False
    healthy, reason = state._reduce_cell_supervision(
        heartbeat_sequence=2,
        critical_probe_healthy=True,
        critical_probe_reason="",
    )
    assert healthy is True
    assert reason == ""

    lifecycle.payload["providerHealthy"] = False
    healthy, reason = state._reduce_cell_supervision(
        heartbeat_sequence=3,
        critical_probe_healthy=True,
        critical_probe_reason="",
    )
    assert healthy is False
    assert reason.startswith("authority:")
    snapshot = state._cell_supervision_last_snapshot
    assert snapshot is not None
    provider = next(
        component
        for component in snapshot["components"]
        if component["id"] == "provider"
    )
    assert provider["healthState"] == "degraded"
    assert provider["healthy"] is False
