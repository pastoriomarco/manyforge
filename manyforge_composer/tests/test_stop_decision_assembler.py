"""Fail-before-state tests for the I8/P5 Stop-decision owner gate.

Historical acceptance source:
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``.  The exact standalone
cases ``test_capacity_eviction_is_explicit_and_identity_bounded``,
``test_gap_flushes_each_identity_without_cross_episode_merge``,
``test_run_end_flush_is_scoped_to_the_requested_run``, and
``test_conflicting_execution_and_latch_identities_are_fenced_loudly`` move to
the I8/P5 C++ Kernel/Stop registry.  So do the 27
``test_event_recorder.py::TestStopDecisionChains`` cases.  Their capacity,
loss, run/source fencing, identity-conflict, terminal-authority, certificate,
coverage, ordering, and causal-correlation requirements remain acceptance
inputs; the Python assembler itself does not return.
"""

import pytest

from manyforge_composer.backend import stop_decision_assembler
from manyforge_composer.backend.stop_decision_assembler import (
    STOP_DECISION_RESTORATION_GATES,
    StopDecisionAssembler,
    StopDecisionOwnerUnavailableError,
    require_stop_decision_owner,
)


class _PoisonEvidence:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"Stop gate accessed former evidence: {name}")

    def __iter__(self):
        raise AssertionError("Stop gate iterated former evidence")


def test_assembler_construction_fails_before_evidence_or_state_access() -> None:
    poison = _PoisonEvidence()
    with pytest.raises(
        StopDecisionOwnerUnavailableError,
        match="composable_cell_stop_decision_owner_unavailable",
    ):
        StopDecisionAssembler(poison, capacity=poison, kernel=poison)


def test_owner_gate_fails_before_event_inspection() -> None:
    poison = _PoisonEvidence()
    with pytest.raises(StopDecisionOwnerUnavailableError):
        require_stop_decision_owner(poison, events=poison)

    assert STOP_DECISION_RESTORATION_GATES == ("I8", "P5")


def test_old_python_stop_contract_and_ordering_helper_are_absent() -> None:
    assert not hasattr(stop_decision_assembler, "StopDecisionTrigger")
    assert not hasattr(stop_decision_assembler, "StopDecisionBoundary")
    assert not hasattr(stop_decision_assembler, "StopDecisionRecord")
    assert not hasattr(stop_decision_assembler, "sequence_ordered")
