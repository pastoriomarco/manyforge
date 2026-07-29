from __future__ import annotations

import hashlib
import json

import pytest

from verification.composable_cell_i1a.topology_prototype import (
    ProcessIsolationProbe,
    RuntimePlacement,
    RuntimeRole,
    TopologyCandidate,
    selected_topology_candidate,
)


def test_selected_topology_uses_restart_boundaries_not_component_count() -> None:
    candidate = selected_topology_candidate()

    assert len(candidate.control_domains) == 1
    assert candidate.control_domains[0].controllable_component_ids == (
        "component.arm-a",
        "component.tool-a",
    )
    assert sum(
        placement.role is RuntimeRole.CONTROL_DOMAIN
        for placement in candidate.placements
    ) == 1


def test_manager_compiler_and_nrt_role_boundaries_are_explicit() -> None:
    candidate = selected_topology_candidate()
    by_role = {item.role: item for item in candidate.placements}

    assert not by_role[RuntimeRole.COMPONENT_MANAGER].managed_by_component_manager
    assert by_role[RuntimeRole.COMPILER_WORKER].bounded_lifetime
    assert len(
        {
            by_role[RuntimeRole.EXECUTION_HOST].process_boundary,
            by_role[RuntimeRole.PROGRAM_RUNNER].process_boundary,
            by_role[RuntimeRole.HEADLESS_STATE_API].process_boundary,
        }
    ) == 3


def test_invalid_self_managed_or_colocated_candidate_is_rejected() -> None:
    selected = selected_topology_candidate()
    placements = list(selected.placements)
    manager = placements[0]
    placements[0] = RuntimePlacement(
        manager.runtime_unit_id,
        manager.role,
        manager.process_boundary,
        managed_by_component_manager=True,
    )
    with pytest.raises(ValueError, match="cannot manage itself"):
        TopologyCandidate(tuple(placements), selected.control_domains).validate()

    placements = list(selected.placements)
    execution_index = next(
        index
        for index, item in enumerate(placements)
        if item.role is RuntimeRole.EXECUTION_HOST
    )
    runner_index = next(
        index
        for index, item in enumerate(placements)
        if item.role is RuntimeRole.PROGRAM_RUNNER
    )
    runner = placements[runner_index]
    placements[runner_index] = RuntimePlacement(
        runner.runtime_unit_id,
        runner.role,
        placements[execution_index].process_boundary,
        managed_by_component_manager=True,
    )
    with pytest.raises(ValueError, match="independent process boundaries"):
        TopologyCandidate(tuple(placements), selected.control_domains).validate()


def test_manager_and_program_runner_loss_leave_independent_roles_alive() -> None:
    with ProcessIsolationProbe() as probe:
        for role in (
            RuntimeRole.COMPONENT_MANAGER,
            RuntimeRole.EXECUTION_HOST,
            RuntimeRole.PROGRAM_RUNNER,
            RuntimeRole.HEADLESS_STATE_API,
        ):
            probe.start(role)

        before = probe.measurement()
        assert before.process_count == 4
        assert before.aggregate_rss_kib > 0

        probe.terminate(RuntimeRole.COMPONENT_MANAGER)
        assert probe.alive(RuntimeRole.EXECUTION_HOST)
        assert probe.alive(RuntimeRole.PROGRAM_RUNNER)
        assert probe.alive(RuntimeRole.HEADLESS_STATE_API)

        probe.terminate(RuntimeRole.PROGRAM_RUNNER)
        after = probe.measurement()
        assert after.live_roles == (
            RuntimeRole.EXECUTION_HOST,
            RuntimeRole.HEADLESS_STATE_API,
        )
        assert after.process_count == 2
        assert after.aggregate_rss_kib > 0


def test_compiler_worker_is_bounded_and_leaves_no_resident_role() -> None:
    value = {"graph_revision": 7, "components": ["arm-a", "tool-a"]}
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":"))

    with ProcessIsolationProbe() as probe:
        digest, returncode = probe.run_bounded_compiler(value)

        assert returncode == 0
        assert digest == hashlib.sha256(canonical.encode()).hexdigest()
        assert probe.measurement().process_count == 0
