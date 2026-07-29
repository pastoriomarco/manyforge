from __future__ import annotations

from dataclasses import replace
import json
from pathlib import Path

import pytest

from manyforge_supervision import ComponentHealthReducer
from manyforge_supervision import ComponentHeartbeat
from manyforge_supervision import ComponentHealthState
from manyforge_supervision import ComponentPolicy
from manyforge_supervision import ComponentRole
from manyforge_supervision import SupervisionPolicy
from manyforge_supervision import read_cell_supervisor_heartbeat
from manyforge_supervision import supervision_policy_from_mapping


def policy_mapping() -> dict[str, object]:
    return {
        "schema": "manyforge.cell_supervision.v1",
        "cell_id": "lite6-cell",
        "robot_ids": ["lite6-a"],
        "authority_profile": "local_operator",
        "supervisor_heartbeat_path": "/tmp/cell-supervisor.json",
        "cell_supervisor_component_epoch": "supervisor-epoch-1",
        "components": [
            {
                "role": "cell_supervisor",
                "id": "cell-supervisor",
                "maximum_heartbeat_age_ms": 500,
                "maximum_critical_progress_age_ms": 1_000,
                "dependencies": [],
                "restart_domain": "cell-control",
            }
        ],
    }


def supervisor_heartbeat_payload() -> dict[str, object]:
    return {
        "schema": "manyforge.cell_supervisor.heartbeat.v1",
        "componentRole": "cell_supervisor",
        "componentId": "cell-supervisor",
        "robotId": "lite6-a",
        "cellId": "lite6-cell",
        "componentEpoch": "supervisor-epoch-1",
        "sequence": 1,
        "healthState": "starting",
        "criticalProgressSequence": 1,
        "dependencyEpochs": {},
        "runtimeGeneration": "",
        "bindingGeneration": "",
        "configurationGeneration": "",
        "desiredState": "starting",
        "reasonCodes": ["supervisor_not_armed"],
        "requestedContainment": "dispatch_fenced",
        "observedContainers": {"composer": False, "owner": False},
        "uncleanTerminations": [],
        "observedAtSteadyTimeNs": 1,
        "observedAtUtc": "2026-07-26T00:00:00+00:00",
    }


class FakeClock:
    def __init__(self) -> None:
        self.now_ns = 1_000_000_000

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, delta_ns: int) -> None:
        self.now_ns += delta_ns


def policy() -> SupervisionPolicy:
    return SupervisionPolicy(
        cell_id="lite6-cell",
        robot_ids=("lite6-a",),
        authority_profile="local_operator",
        components=(
            ComponentPolicy(
                component_role=ComponentRole.CELL_SUPERVISOR,
                component_id="cell-supervisor",
                maximum_heartbeat_age_ns=500,
                maximum_critical_progress_age_ns=1_000,
                restart_domain="cell-control",
            ),
            ComponentPolicy(
                component_role=ComponentRole.TRANSPORT_OWNER,
                component_id="transport-owner",
                maximum_heartbeat_age_ns=500,
                maximum_critical_progress_age_ns=1_000,
                dependency_ids=("cell-supervisor",),
                restart_domain="robot-control",
                expected_runtime_generation="runtime-1",
            ),
            ComponentPolicy(
                component_role=ComponentRole.LIFECYCLE_PROVIDER,
                component_id="lifecycle-provider",
                maximum_heartbeat_age_ns=500,
                maximum_critical_progress_age_ns=1_000,
                dependency_ids=("cell-supervisor", "transport-owner"),
                restart_domain="robot-control",
                expected_binding_generation="binding-1",
            ),
            ComponentPolicy(
                component_role=ComponentRole.EXECUTION_BACKEND,
                component_id="execution-backend",
                maximum_heartbeat_age_ns=500,
                maximum_critical_progress_age_ns=1_000,
                dependency_ids=(
                    "cell-supervisor",
                    "transport-owner",
                    "lifecycle-provider",
                ),
                restart_domain="execution",
            ),
            ComponentPolicy(
                component_role=ComponentRole.CONTROL_AUTHORITY,
                component_id="local-authority",
                maximum_heartbeat_age_ns=500,
                maximum_critical_progress_age_ns=1_000,
                dependency_ids=(
                    "cell-supervisor",
                    "lifecycle-provider",
                    "execution-backend",
                ),
                restart_domain="authority",
            ),
        ),
    )


def heartbeat(
    component_id: str,
    *,
    epoch: str | None = None,
    sequence: int = 1,
    progress: int = 1,
    dependencies: dict[str, str] | None = None,
) -> ComponentHeartbeat:
    roles = {
        "cell-supervisor": ComponentRole.CELL_SUPERVISOR,
        "transport-owner": ComponentRole.TRANSPORT_OWNER,
        "lifecycle-provider": ComponentRole.LIFECYCLE_PROVIDER,
        "execution-backend": ComponentRole.EXECUTION_BACKEND,
        "local-authority": ComponentRole.CONTROL_AUTHORITY,
    }
    return ComponentHeartbeat(
        component_role=roles[component_id],
        component_id=component_id,
        robot_id="lite6-a",
        cell_id="lite6-cell",
        component_epoch=epoch or f"{component_id}-epoch-1",
        sequence=sequence,
        health_state=ComponentHealthState.HEALTHY,
        critical_progress_sequence=progress,
        dependency_epochs=dependencies or {},
        runtime_generation=(
            "runtime-1" if component_id == "transport-owner" else ""
        ),
        binding_generation=(
            "binding-1" if component_id == "lifecycle-provider" else ""
        ),
    )


def healthy_reducer(clock: FakeClock) -> ComponentHealthReducer:
    reducer = ComponentHealthReducer(policy(), clock_ns=clock)
    epochs: dict[str, str] = {}
    for component in policy().components:
        dependencies = {
            dependency: epochs[dependency]
            for dependency in component.dependency_ids
        }
        sample = heartbeat(
            component.component_id,
            dependencies=dependencies,
        )
        assert reducer.ingest(sample)
        epochs[component.component_id] = sample.component_epoch
    assert reducer.snapshot().dispatch_admitted
    return reducer


def test_policy_rejects_unknown_dependency_and_cycle() -> None:
    with pytest.raises(ValueError, match="unknown"):
        SupervisionPolicy(
            cell_id="cell",
            robot_ids=("robot",),
            authority_profile="local_operator",
            components=(
                ComponentPolicy(
                    component_role=ComponentRole.CONTROL_AUTHORITY,
                    component_id="authority",
                    maximum_heartbeat_age_ns=1,
                    maximum_critical_progress_age_ns=1,
                    dependency_ids=("missing",),
                ),
            ),
        )
    with pytest.raises(ValueError, match="cycle"):
        SupervisionPolicy(
            cell_id="cell",
            robot_ids=("robot",),
            authority_profile="local_operator",
            components=(
                ComponentPolicy(
                    component_role=ComponentRole.TRANSPORT_OWNER,
                    component_id="a",
                    maximum_heartbeat_age_ns=1,
                    maximum_critical_progress_age_ns=1,
                    dependency_ids=("b",),
                ),
                ComponentPolicy(
                    component_role=ComponentRole.LIFECYCLE_PROVIDER,
                    component_id="b",
                    maximum_heartbeat_age_ns=1,
                    maximum_critical_progress_age_ns=1,
                    dependency_ids=("a",),
                ),
            ),
        )


def test_manifest_parser_rejects_unknown_top_and_component_keys() -> None:
    top_level = policy_mapping()
    top_level["typo"] = True
    with pytest.raises(ValueError, match="unknown keys: typo"):
        supervision_policy_from_mapping(top_level)

    component_level = policy_mapping()
    components = component_level["components"]
    assert isinstance(components, list)
    components[0]["maximum_heartbeat_age_msec"] = 500
    with pytest.raises(
        ValueError,
        match="unknown keys: maximum_heartbeat_age_msec",
    ):
        supervision_policy_from_mapping(component_level)


def test_manifest_parser_accepts_runtime_supervision_metadata() -> None:
    parsed = supervision_policy_from_mapping(policy_mapping())
    assert parsed.cell_id == "lite6-cell"
    assert parsed.components[0].component_role == (
        ComponentRole.CELL_SUPERVISOR
    )


def test_external_heartbeat_reader_is_strict_and_identity_bound(
    tmp_path: Path,
) -> None:
    path = tmp_path / "heartbeat.json"
    payload = supervisor_heartbeat_payload()
    path.write_text(json.dumps(payload), encoding="utf-8")

    heartbeat = read_cell_supervisor_heartbeat(
        path,
        expected_component_id="cell-supervisor",
        expected_component_epoch="supervisor-epoch-1",
    )
    assert heartbeat.health_state == ComponentHealthState.STARTING

    payload["unknownField"] = "not-accepted"
    path.write_text(json.dumps(payload), encoding="utf-8")
    with pytest.raises(ValueError, match="unknown keys: unknownField"):
        read_cell_supervisor_heartbeat(
            path,
            expected_component_id="cell-supervisor",
            expected_component_epoch="supervisor-epoch-1",
        )

    payload.pop("unknownField")
    payload["componentRole"] = "hmi"
    path.write_text(json.dumps(payload), encoding="utf-8")
    with pytest.raises(ValueError, match="role_mismatch"):
        read_cell_supervisor_heartbeat(
            path,
            expected_component_id="cell-supervisor",
            expected_component_epoch="supervisor-epoch-1",
        )

    payload["componentRole"] = "cell_supervisor"
    payload["componentEpoch"] = "supervisor-epoch-2"
    path.write_text(json.dumps(payload), encoding="utf-8")
    with pytest.raises(ValueError, match="identity_mismatch"):
        read_cell_supervisor_heartbeat(
            path,
            expected_component_id="cell-supervisor",
            expected_component_epoch="supervisor-epoch-1",
        )


def test_missing_duplicate_regressed_and_conflicting_samples_fail_closed() -> None:
    clock = FakeClock()
    reducer = ComponentHealthReducer(policy(), clock_ns=clock)
    sample = heartbeat("cell-supervisor")
    assert reducer.ingest(sample)
    assert not reducer.ingest(sample)
    assert reducer.duplicate_total == 1
    next_sample = replace(sample, sequence=2)
    assert reducer.ingest(next_sample)
    assert not reducer.ingest(replace(sample, sequence=1))
    assert not reducer.ingest(
        replace(
            next_sample,
            health_state=ComponentHealthState.DEGRADED,
        )
    )
    snapshot = reducer.snapshot()
    assert not snapshot.dispatch_admitted
    assert "lifecycle-provider:component_heartbeat_missing" in (
        snapshot.reason_codes
    )


def test_same_epoch_heartbeat_does_not_fake_critical_progress() -> None:
    clock = FakeClock()
    reducer = healthy_reducer(clock)
    clock.advance(750)
    current = heartbeat(
        "execution-backend",
        sequence=2,
        progress=1,
        dependencies={
            "cell-supervisor": "cell-supervisor-epoch-1",
            "transport-owner": "transport-owner-epoch-1",
            "lifecycle-provider": "lifecycle-provider-epoch-1",
        },
    )
    assert reducer.ingest(current)
    clock.advance(300)
    snapshot = reducer.snapshot()
    record = snapshot.component("execution-backend")
    assert record is not None
    assert "component_critical_progress_stale" in record.reason_codes
    assert not snapshot.dispatch_admitted


def test_dependency_epoch_mismatch_propagates_to_dependants() -> None:
    clock = FakeClock()
    reducer = healthy_reducer(clock)
    replacement = heartbeat(
        "transport-owner",
        epoch="transport-owner-epoch-2",
        dependencies={"cell-supervisor": "cell-supervisor-epoch-1"},
    )
    assert reducer.ingest(replacement)
    snapshot = reducer.snapshot()
    provider = snapshot.component("lifecycle-provider")
    authority = snapshot.component("local-authority")
    assert provider is not None and authority is not None
    assert (
        "component_dependency_epoch_mismatch:transport-owner"
        in provider.reason_codes
    )
    assert not authority.healthy


def test_generation_changes_only_for_semantics_not_freshness() -> None:
    clock = FakeClock()
    reducer = healthy_reducer(clock)
    first = reducer.snapshot()
    clock.advance(100)
    second = reducer.snapshot()
    assert second.generation == first.generation
    authority = heartbeat(
        "local-authority",
        sequence=2,
        progress=2,
        dependencies={
            "cell-supervisor": "cell-supervisor-epoch-1",
            "lifecycle-provider": "lifecycle-provider-epoch-1",
            "execution-backend": "execution-backend-epoch-1",
        },
    )
    assert reducer.ingest(authority)
    assert reducer.snapshot().generation == first.generation
    degraded = replace(
        authority,
        sequence=3,
        critical_progress_sequence=3,
        health_state=ComponentHealthState.DEGRADED,
    )
    assert reducer.ingest(degraded)
    assert reducer.snapshot().generation == first.generation + 1


def test_execution_pin_rejects_replacement_and_never_auto_repairs() -> None:
    clock = FakeClock()
    reducer = healthy_reducer(clock)
    admitted = reducer.snapshot()
    pin = reducer.mint_execution_pin(
        snapshot=admitted,
        execution_id="execution-1",
        program_run_id="run-1",
        robot_id="lite6-a",
        program_generation="program-1",
        deployment_generation="deployment-1",
        runtime_generation="runtime-1",
        controller_binding_generation="binding-1",
        lifecycle_generation=4,
        admission_generation=9,
    )
    assert reducer.validate_pin(pin, snapshot=admitted).valid

    replacement = heartbeat(
        "lifecycle-provider",
        epoch="lifecycle-provider-epoch-2",
        dependencies={
            "cell-supervisor": "cell-supervisor-epoch-1",
            "transport-owner": "transport-owner-epoch-1",
        },
    )
    assert reducer.ingest(replacement)
    changed = reducer.snapshot()
    validation = reducer.validate_pin(pin, snapshot=changed)
    assert not validation.valid
    assert (
        "execution_environment_component_epoch_changed:lifecycle-provider"
        in validation.reason_codes
    )

    # The new provider can become healthy for a later run after every
    # dependant explicitly binds its new epoch. That cannot repair the old pin.
    execution = heartbeat(
        "execution-backend",
        sequence=2,
        progress=2,
        dependencies={
            "cell-supervisor": "cell-supervisor-epoch-1",
            "transport-owner": "transport-owner-epoch-1",
            "lifecycle-provider": "lifecycle-provider-epoch-2",
        },
    )
    authority = heartbeat(
        "local-authority",
        sequence=2,
        progress=2,
        dependencies={
            "cell-supervisor": "cell-supervisor-epoch-1",
            "lifecycle-provider": "lifecycle-provider-epoch-2",
            "execution-backend": "execution-backend-epoch-1",
        },
    )
    assert reducer.ingest(execution)
    assert reducer.ingest(authority)
    recovered = reducer.snapshot()
    assert recovered.dispatch_admitted
    assert not reducer.validate_pin(pin, snapshot=recovered).valid
