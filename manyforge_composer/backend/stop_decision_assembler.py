"""Sequence-authoritative stop-decision assembly.

This module is deliberately storage-agnostic.  It consumes the deduplicated
native kernel stream in kernel-sequence order and returns immutable payloads for
the recorder to persist.  Recorder indices, HTTP polling order, and wall time
never participate in causal decisions.
"""

from __future__ import annotations

from collections import OrderedDict, deque
from dataclasses import dataclass, field
from enum import StrEnum
from typing import Any, Iterable, Optional


class StopDecisionTrigger(StrEnum):
    FAULT = "fault"
    INTERRUPT = "interrupt"
    RESET = "reset"
    OPERATOR = "operator"
    UNOBSERVED = "unobserved"


class StopDecisionBoundary(StrEnum):
    INTERRUPT_PREEMPT = "interrupt_preempt_applied"
    MOTION_TRIP_COMPLETED = "motion_trip_completed"
    MOTION_TRIP_FAILED = "motion_trip_failed"
    STATE_TERMINAL = "state_terminal"
    RESET = "reset"
    RUN_END = "run_end"
    PROCESS_EXIT = "process_exit"
    EVENT_GAP = "kernel_event_gap"
    KERNEL_SOURCE_REPLACED = "kernel_event_source_replaced"
    CAPACITY_EVICTION = "capacity_eviction"
    IDENTITY_CONFLICT = "identity_conflict"
    ABANDONED = "execution_progressed_without_stop_terminal"


@dataclass(frozen=True)
class StopDecisionRecord:
    """One derived record ready for recorder persistence."""

    execution_id: int
    timestamp_ns: int
    run_id: Optional[str]
    details: dict[str, str]


@dataclass
class _StopEpisode:
    token: int
    execution_id: int
    run_id: Optional[str]
    started_ts_ns: int
    first_sequence: int
    last_sequence: int
    latch_id: str = ""
    source_id: str = ""
    policy_id: str = ""
    trigger_hint: Optional[StopDecisionTrigger] = None
    requested_ts_ns: int = 0
    issued_ts_ns: int = 0
    latched_ts_ns: int = 0
    interrupted_ts_ns: int = 0
    interrupted_sequence: int = 0
    terminal_state: str = ""
    preemption_result: str = ""
    preemption_action: str = ""
    certificate_observed: bool = False
    coverage_observed: bool = False
    latch_observed: bool = False
    cause_observed: bool = False
    evidence: dict[str, str] = field(default_factory=dict)


_RUNTIME_COLLISION_SOURCE_ID = "manyforge.kernel.runtime_collision_fault"
_DEFINITIVE_PREEMPT_RESULTS = frozenset(
    {"succeeded", "partial", "backend_preempt_failed", "none_required"}
)
_FAILED_PREEMPT_RESULTS = frozenset({"partial", "backend_preempt_failed"})
_STATE_TERMINALS = frozenset(
    {"Stopped", "Failed", "InterventionRequired"}
)
_TRANSIENT_PROGRESS_STATES = frozenset({"Planning", "Validating"})

# Stable, intentionally bounded evidence vocabulary.  Raw native events remain
# the full-fidelity source; the derived chain carries only fields needed to
# answer the stop/recovery questions in spec 520 section 6.4.
_STOP_DETAIL_KEYS = (
    "requested_stop_mode",
    "executed_stop_mode",
    "controlled_stop_success",
    "emergency_fallback",
    "hard_stop_transitioned",
    "stop_command_source",
    "preempt_path",
    "fault_stop_profile_id",
    "reason",
    "phase",
    "scope_id",
)
_CERTIFICATE_KEYS = (
    "certificate_id",
    "certificate_revision",
    "completion_result",
    "stop_owner",
    "stop_owner_generation",
    "command_source",
    "backend_stop_confirmed",
    "observed_state_fresh",
    "observed_state_at_rest",
    "at_rest_evidence",
)
_COVERAGE_KEYS = (
    "monitor_generation",
    "ticks_attempted",
    "collision_queries_executed",
    "unavailable_ticks",
    "longest_unavailable_interval_ns",
    "coverage_complete",
    "effective_coverage_ratio",
)
_COLLISION_KEYS = (
    "contact_a",
    "contact_b",
    "reason_during_stop",
    "stop_owner",
    "query_id",
    "world_revision_id",
    "world_content_hash",
)


def _positive_int(value: Any) -> int:
    try:
        parsed = int(value or 0)
    except (TypeError, ValueError):
        return 0
    return parsed if parsed > 0 else 0


def _text(value: Any) -> str:
    return "" if value is None else str(value)


class StopDecisionAssembler:
    """Bounded state machine over one authoritative native event sequence.

    The caller serializes access.  This class intentionally has no lock and no
    I/O so recorder storage cannot become part of stop causality.
    """

    def __init__(self, *, capacity: int = 64, marker_capacity: int = 256) -> None:
        if capacity <= 0:
            raise ValueError("stop-decision assembler capacity must be positive")
        if marker_capacity <= 0:
            raise ValueError("stop-decision marker capacity must be positive")
        self._capacity = capacity
        self._episodes: OrderedDict[int, _StopEpisode] = OrderedDict()
        self._execution_tokens: dict[int, int] = {}
        self._latch_tokens: dict[str, int] = {}
        self._next_token = 1
        self._completed_markers: deque[tuple[int, str]] = deque(
            maxlen=marker_capacity
        )
        self._completed_marker_set: set[tuple[int, str]] = set()

    @property
    def open_episode_count(self) -> int:
        return len(self._episodes)

    def observe(
        self,
        native_type: str,
        native_event: dict[str, Any],
        details: dict[str, Any],
        *,
        run_id: Optional[str],
    ) -> list[StopDecisionRecord]:
        """Consume one deduplicated event with a positive kernel sequence."""
        sequence = _positive_int(native_event.get("sequence"))
        if sequence == 0:
            raise ValueError("stop-decision assembly requires kernel sequence")
        timestamp_ns = int(native_event.get("timestamp_ns") or 0)
        execution_id = _positive_int(native_event.get("execution_id"))
        latch_id = _text(details.get("latch_id"))
        records: list[StopDecisionRecord] = []

        if native_type == "collision_detected":
            episode, evicted = self._get_or_open(
                execution_id, latch_id, run_id, timestamp_ns, sequence
            )
            records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            stop_path_collision = (
                _text(details.get("reason_during_stop")) == "stop_path_collision"
            )
            stop_owner = _text(details.get("stop_owner"))
            if not stop_path_collision:
                episode.trigger_hint = StopDecisionTrigger.FAULT
                episode.cause_observed = True
                episode.evidence["collision_trigger_observed"] = "true"
            elif stop_owner == "operator_stop":
                episode.trigger_hint = episode.trigger_hint or StopDecisionTrigger.OPERATOR
            elif stop_owner in {"fault_preempt_stop", "motion_trip_stop"}:
                episode.trigger_hint = (
                    StopDecisionTrigger.FAULT
                    if stop_owner == "fault_preempt_stop"
                    else StopDecisionTrigger.INTERRUPT
                )
            episode.evidence["collision_observed"] = "true"
            for key in _COLLISION_KEYS:
                value = details.get(key)
                if value not in (None, ""):
                    episode.evidence[f"collision_{key}"] = _text(value)
            return records

        if native_type == "state_transition":
            to_state = _text(details.get("to_state") or native_event.get("to_state"))
            if to_state == "Interrupted":
                episode, evicted = self._get_or_open(
                    execution_id, latch_id, run_id, timestamp_ns, sequence
                )
                records.extend(evicted)
                self._touch(episode, timestamp_ns, sequence)
                episode.interrupted_ts_ns = timestamp_ns
                episode.interrupted_sequence = sequence
                episode.evidence["interrupted_phase_observed"] = "true"
                reason = _text(details.get("reason"))
                if reason:
                    episode.evidence["interrupted_reason"] = reason
                # Interrupted is only the ownership phase boundary.  It cannot
                # close while physical preemption is still pending.
                return records
            if to_state in _TRANSIENT_PROGRESS_STATES:
                episode = self._find(execution_id, latch_id)
                if episode is not None:
                    self._touch(episode, timestamp_ns, sequence)
                return records
            if to_state == "Executing":
                episode = self._find(execution_id, latch_id)
                if episode is not None:
                    # A pause-like request that is explicitly followed by
                    # resumed execution never became a stop/recovery episode.
                    # Raw events remain durable; do not manufacture a chain.
                    self._remove(episode)
                return records
            if to_state == "Completed":
                episode = self._find(execution_id, latch_id)
                if episode is not None:
                    self._touch(episode, timestamp_ns, sequence)
                    records.append(
                        self._finish(
                            episode,
                            timestamp_ns=timestamp_ns,
                            sequence=sequence,
                            boundary=StopDecisionBoundary.ABANDONED,
                            terminal_state=to_state,
                            complete=False,
                            incomplete_reason=(
                                "execution_completed_without_stop_terminal"
                            ),
                            terminal_details=details,
                        )
                    )
                # Ordinary successful execution is not a stop decision.
                return records
            if to_state == "Idle":
                episode = self._find(execution_id, latch_id)
                if episode is None:
                    return records
                self._touch(episode, timestamp_ns, sequence)
                records.append(
                    self._finish(
                        episode,
                        timestamp_ns=timestamp_ns,
                        sequence=sequence,
                        boundary=StopDecisionBoundary.RESET,
                        terminal_state=to_state,
                        complete=episode.cause_observed,
                        incomplete_reason=(
                            None if episode.cause_observed else "reset_without_trigger_evidence"
                        ),
                        terminal_details=details,
                    )
                )
                return records
            if to_state not in _STATE_TERMINALS:
                return records

            episode = self._find(execution_id, latch_id)
            marker = (execution_id, to_state)
            if episode is None:
                if marker in self._completed_marker_set:
                    return records
                if to_state == "Failed":
                    # Plain planning/validation failure is not a stop episode.
                    return records
                # The kernel owns one execution lifecycle at a time.  A
                # terminal for B cannot leave A's stop episode available for a
                # later accidental merge; seal A separately and loudly.
                for stale in list(self._episodes.values()):
                    if (
                        execution_id > 0
                        and stale.execution_id > 0
                        and stale.execution_id != execution_id
                    ):
                        records.append(
                            self._finish(
                                stale,
                                timestamp_ns=timestamp_ns,
                                sequence=stale.last_sequence,
                                boundary=StopDecisionBoundary.ABANDONED,
                                terminal_state=stale.terminal_state or "Unknown",
                                complete=False,
                                incomplete_reason=(
                                    "superseded_by_other_execution_terminal"
                                ),
                                extra_details={
                                    "superseding_execution_id": str(execution_id)
                                },
                            )
                        )
                episode, evicted = self._get_or_open(
                    execution_id, latch_id, run_id, timestamp_ns, sequence
                )
                records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            complete = episode.cause_observed
            records.append(
                self._finish(
                    episode,
                    timestamp_ns=timestamp_ns,
                    sequence=sequence,
                    boundary=StopDecisionBoundary.STATE_TERMINAL,
                    terminal_state=to_state,
                    complete=complete,
                    incomplete_reason=(
                        None if complete else "terminal_without_trigger_evidence"
                    ),
                    terminal_details=details,
                )
            )
            return records

        if native_type == "interrupt_latched":
            episode, evicted = self._get_or_open(
                execution_id, latch_id, run_id, timestamp_ns, sequence
            )
            records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            self._bind_latch(episode, latch_id)
            episode.latch_observed = True
            episode.cause_observed = True
            episode.latched_ts_ns = episode.latched_ts_ns or timestamp_ns
            episode.source_id = episode.source_id or _text(details.get("source_id"))
            episode.policy_id = episode.policy_id or _text(details.get("policy_id"))
            episode.trigger_hint = (
                StopDecisionTrigger.FAULT
                if episode.source_id == _RUNTIME_COLLISION_SOURCE_ID
                else episode.trigger_hint or StopDecisionTrigger.INTERRUPT
            )
            self._copy_stop_details(episode, details)
            return records

        if native_type in {"stop_requested", "stop_issued"}:
            episode, evicted = self._get_or_open(
                execution_id, latch_id, run_id, timestamp_ns, sequence
            )
            records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            episode.cause_observed = True
            reason = _text(details.get("reason"))
            if reason:
                episode.evidence.setdefault("stop_reason", reason)
            if episode.trigger_hint is None:
                episode.trigger_hint = (
                    StopDecisionTrigger.RESET
                    if "reset" in reason
                    else StopDecisionTrigger.OPERATOR
                )
            if native_type == "stop_requested":
                episode.requested_ts_ns = episode.requested_ts_ns or timestamp_ns
            else:
                episode.issued_ts_ns = episode.issued_ts_ns or timestamp_ns
            self._copy_stop_details(episode, details)
            return records

        if native_type == "runtime_collision_coverage":
            episode = self._find(execution_id, latch_id)
            if episode is not None:
                self._touch(episode, timestamp_ns, sequence)
                episode.coverage_observed = True
                for key in _COVERAGE_KEYS:
                    value = details.get(key)
                    if value not in (None, ""):
                        episode.evidence[f"stop_monitor_{key}"] = _text(value)
            return records

        if native_type == "stop_certificate_published":
            episode, evicted = self._get_or_open(
                execution_id, latch_id, run_id, timestamp_ns, sequence
            )
            records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            self._bind_latch(episode, latch_id)
            episode.certificate_observed = True
            episode.cause_observed = True
            for key in _CERTIFICATE_KEYS:
                value = details.get(key)
                if value in (None, ""):
                    continue
                target = "stop_certificate_id" if key == "certificate_id" else key
                episode.evidence[target] = _text(value)
            self._copy_stop_details(episode, details)
            # The certificate precedes the state/preemption event by design.
            # Keep it pending so the final chain records both physical outcome
            # and execution terminal authority.
            return records

        if native_type == "interrupt_preempt_applied":
            episode, evicted = self._get_or_open(
                execution_id, latch_id, run_id, timestamp_ns, sequence
            )
            records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            self._bind_latch(episode, latch_id)
            episode.preemption_action = _text(details.get("preemption_action"))
            episode.preemption_result = _text(details.get("preemption_result"))
            self._copy_stop_details(episode, details)
            if episode.preemption_result not in _DEFINITIVE_PREEMPT_RESULTS:
                return records
            if episode.preemption_result in _FAILED_PREEMPT_RESULTS:
                # Kernel act-before-emit ordering commits InterventionRequired
                # after publishing the failed result.  Keep the episode so that
                # final terminal authority is included in the one chain.
                return records
            complete = episode.latch_observed and episode.cause_observed
            records.append(
                self._finish(
                    episode,
                    timestamp_ns=timestamp_ns,
                    sequence=sequence,
                    boundary=StopDecisionBoundary.INTERRUPT_PREEMPT,
                    terminal_state="Interrupted",
                    complete=complete,
                    incomplete_reason=(
                        None if complete else "preemption_without_latch_evidence"
                    ),
                    terminal_details=details,
                )
            )
            return records

        if native_type in {"motion_trip_completed", "motion_trip_failed"}:
            episode, evicted = self._get_or_open(
                execution_id, latch_id, run_id, timestamp_ns, sequence
            )
            records.extend(evicted)
            self._touch(episode, timestamp_ns, sequence)
            self._bind_latch(episode, latch_id)
            episode.cause_observed = True
            self._copy_stop_details(episode, details)
            succeeded = native_type == "motion_trip_completed"
            records.append(
                self._finish(
                    episode,
                    timestamp_ns=timestamp_ns,
                    sequence=sequence,
                    boundary=(
                        StopDecisionBoundary.MOTION_TRIP_COMPLETED
                        if succeeded
                        else StopDecisionBoundary.MOTION_TRIP_FAILED
                    ),
                    terminal_state="Completed" if succeeded else "InterventionRequired",
                    complete=True,
                    terminal_details=details,
                )
            )
            return records

        return records

    def invalidate_all(
        self,
        reason: str,
        *,
        boundary: StopDecisionBoundary,
        missing_sequence_start: int = 0,
        missing_sequence_end: int = 0,
        timestamp_ns: int = 0,
        run_id: Optional[str] = None,
    ) -> list[StopDecisionRecord]:
        """Seal every matching open episode as explicitly incomplete."""
        records: list[StopDecisionRecord] = []
        for episode in list(self._episodes.values()):
            if run_id is not None and episode.run_id not in {None, run_id}:
                continue
            details: dict[str, str] = {}
            if missing_sequence_start > 0:
                details["missing_sequence_start"] = str(missing_sequence_start)
            if missing_sequence_end > 0:
                details["missing_sequence_end"] = str(missing_sequence_end)
            records.append(
                self._finish(
                    episode,
                    timestamp_ns=timestamp_ns or episode.started_ts_ns,
                    sequence=episode.last_sequence,
                    boundary=boundary,
                    terminal_state=episode.terminal_state or "Unknown",
                    complete=False,
                    incomplete_reason=reason,
                    extra_details=details,
                )
            )
        return records

    def _find(self, execution_id: int, latch_id: str) -> Optional[_StopEpisode]:
        latch_token = self._latch_tokens.get(latch_id) if latch_id else None
        execution_token = (
            self._execution_tokens.get(execution_id) if execution_id > 0 else None
        )
        if latch_token is not None and execution_token is not None:
            if latch_token != execution_token:
                latch_episode = self._episodes.get(latch_token)
                execution_episode = self._episodes.get(execution_token)
                if (
                    latch_episode is not None
                    and execution_episode is not None
                    and latch_episode.execution_id > 0
                    and execution_episode.execution_id > 0
                    and latch_episode.execution_id
                    != execution_episode.execution_id
                ):
                    # Contradictory native identity must be handled by
                    # _get_or_open(), which can emit both incomplete records.
                    # Read-only lookups decline to attach the event to either.
                    return None
                return self._merge(latch_token, execution_token)
            return self._episodes.get(latch_token)
        token = latch_token if latch_token is not None else execution_token
        return self._episodes.get(token) if token is not None else None

    def _get_or_open(
        self,
        execution_id: int,
        latch_id: str,
        run_id: Optional[str],
        timestamp_ns: int,
        sequence: int,
    ) -> tuple[_StopEpisode, list[StopDecisionRecord]]:
        latch_token = self._latch_tokens.get(latch_id) if latch_id else None
        execution_token = (
            self._execution_tokens.get(execution_id) if execution_id > 0 else None
        )
        if (
            latch_token is not None
            and execution_token is not None
            and latch_token != execution_token
        ):
            latch_episode = self._episodes.get(latch_token)
            execution_episode = self._episodes.get(execution_token)
            if (
                latch_episode is not None
                and execution_episode is not None
                and latch_episode.execution_id > 0
                and execution_episode.execution_id > 0
                and latch_episode.execution_id != execution_episode.execution_id
            ):
                conflicted: list[StopDecisionRecord] = []
                for stale in (latch_episode, execution_episode):
                    if stale.token not in self._episodes:
                        continue
                    conflicted.append(
                        self._finish(
                            stale,
                            timestamp_ns=timestamp_ns,
                            sequence=stale.last_sequence,
                            boundary=StopDecisionBoundary.IDENTITY_CONFLICT,
                            terminal_state=stale.terminal_state or "Unknown",
                            complete=False,
                            incomplete_reason=(
                                "conflicting_execution_latch_identity"
                            ),
                            extra_details={
                                "conflicting_event_execution_id": str(
                                    execution_id
                                ),
                                "conflicting_event_latch_id": latch_id,
                            },
                        )
                    )
                opened, evicted = self._open(
                    execution_id, latch_id, run_id, timestamp_ns, sequence
                )
                return opened, [*conflicted, *evicted]
        episode = self._find(execution_id, latch_id)
        if episode is not None:
            if episode.run_id and run_id and episode.run_id != run_id:
                record = self._finish(
                    episode,
                    timestamp_ns=timestamp_ns,
                    sequence=episode.last_sequence,
                    boundary=StopDecisionBoundary.RUN_END,
                    terminal_state=episode.terminal_state or "Unknown",
                    complete=False,
                    incomplete_reason="run_identity_changed",
                )
                opened, evicted = self._open(
                    execution_id, latch_id, run_id, timestamp_ns, sequence
                )
                return opened, [record, *evicted]
            if episode.execution_id == 0 and execution_id > 0:
                episode.execution_id = execution_id
                self._execution_tokens[execution_id] = episode.token
            if episode.run_id is None and run_id is not None:
                episode.run_id = run_id
            self._bind_latch(episode, latch_id)
            return episode, []
        return self._open(execution_id, latch_id, run_id, timestamp_ns, sequence)

    def _open(
        self,
        execution_id: int,
        latch_id: str,
        run_id: Optional[str],
        timestamp_ns: int,
        sequence: int,
    ) -> tuple[_StopEpisode, list[StopDecisionRecord]]:
        evicted: list[StopDecisionRecord] = []
        if len(self._episodes) >= self._capacity:
            oldest = next(iter(self._episodes.values()))
            evicted.append(
                self._finish(
                    oldest,
                    timestamp_ns=timestamp_ns,
                    sequence=oldest.last_sequence,
                    boundary=StopDecisionBoundary.CAPACITY_EVICTION,
                    terminal_state=oldest.terminal_state or "Unknown",
                    complete=False,
                    incomplete_reason="assembler_capacity_eviction",
                )
            )
        token = self._next_token
        self._next_token += 1
        episode = _StopEpisode(
            token=token,
            execution_id=execution_id,
            run_id=run_id,
            started_ts_ns=timestamp_ns,
            first_sequence=sequence,
            last_sequence=sequence,
        )
        self._episodes[token] = episode
        if execution_id > 0:
            self._execution_tokens[execution_id] = token
        self._bind_latch(episode, latch_id)
        return episode, evicted

    def _merge(self, first_token: int, second_token: int) -> _StopEpisode:
        first = self._episodes[first_token]
        second = self._episodes[second_token]
        if (
            first.execution_id > 0
            and second.execution_id > 0
            and first.execution_id != second.execution_id
        ):
            # Conflicting identities are never coalesced.  Latch identity is
            # stronger for a latch-bearing event; retain that episode.
            return first
        primary, secondary = (
            (first, second)
            if first.first_sequence <= second.first_sequence
            else (second, first)
        )
        primary.execution_id = primary.execution_id or secondary.execution_id
        primary.run_id = primary.run_id or secondary.run_id
        primary.started_ts_ns = min(primary.started_ts_ns, secondary.started_ts_ns)
        primary.first_sequence = min(primary.first_sequence, secondary.first_sequence)
        primary.last_sequence = max(primary.last_sequence, secondary.last_sequence)
        primary.latch_id = primary.latch_id or secondary.latch_id
        primary.source_id = primary.source_id or secondary.source_id
        primary.policy_id = primary.policy_id or secondary.policy_id
        primary.trigger_hint = primary.trigger_hint or secondary.trigger_hint
        for name in (
            "requested_ts_ns",
            "issued_ts_ns",
            "latched_ts_ns",
            "interrupted_ts_ns",
            "interrupted_sequence",
        ):
            current = getattr(primary, name)
            other = getattr(secondary, name)
            if current == 0 or (other > 0 and other < current):
                setattr(primary, name, other)
        primary.preemption_result = (
            secondary.preemption_result or primary.preemption_result
        )
        primary.preemption_action = (
            secondary.preemption_action or primary.preemption_action
        )
        primary.certificate_observed |= secondary.certificate_observed
        primary.coverage_observed |= secondary.coverage_observed
        primary.latch_observed |= secondary.latch_observed
        primary.cause_observed |= secondary.cause_observed
        for key, value in secondary.evidence.items():
            primary.evidence.setdefault(key, value)
        self._remove(secondary)
        if primary.execution_id > 0:
            self._execution_tokens[primary.execution_id] = primary.token
        if primary.latch_id:
            self._latch_tokens[primary.latch_id] = primary.token
        return primary

    @staticmethod
    def _touch(episode: _StopEpisode, timestamp_ns: int, sequence: int) -> None:
        episode.last_sequence = max(episode.last_sequence, sequence)
        if timestamp_ns and not episode.started_ts_ns:
            episode.started_ts_ns = timestamp_ns

    def _bind_latch(self, episode: _StopEpisode, latch_id: str) -> None:
        if not latch_id:
            return
        episode.latch_id = episode.latch_id or latch_id
        self._latch_tokens[latch_id] = episode.token

    @staticmethod
    def _copy_stop_details(episode: _StopEpisode, details: dict[str, Any]) -> None:
        for key in _STOP_DETAIL_KEYS:
            value = details.get(key)
            if value not in (None, ""):
                episode.evidence[key] = _text(value)
        for key in ("latch_id", "source_id", "policy_id"):
            value = _text(details.get(key))
            if value:
                setattr(episode, key, getattr(episode, key) or value)

    def _finish(
        self,
        episode: _StopEpisode,
        *,
        timestamp_ns: int,
        sequence: int,
        boundary: StopDecisionBoundary,
        terminal_state: str,
        complete: bool,
        incomplete_reason: Optional[str] = None,
        terminal_details: Optional[dict[str, Any]] = None,
        extra_details: Optional[dict[str, str]] = None,
    ) -> StopDecisionRecord:
        terminal_details = terminal_details or {}
        self._copy_stop_details(episode, terminal_details)
        episode.terminal_state = terminal_state
        trigger = episode.trigger_hint or self._infer_trigger(episode)
        if trigger == StopDecisionTrigger.UNOBSERVED:
            complete = False
            incomplete_reason = incomplete_reason or "trigger_evidence_not_observed"

        last_sequence = max(episode.last_sequence, sequence)
        details: dict[str, str] = {
            "chain_complete": "true" if complete else "false",
            "completion_boundary": boundary.value,
            "trigger": trigger.value,
            "terminal_state": terminal_state,
            "terminal_authority": self._terminal_authority(boundary, terminal_details),
            "physical_stop_result": self._physical_stop_result(
                episode, boundary, terminal_state
            ),
            "kernel_execution_id": str(episode.execution_id),
            "native_sequence_start": str(episode.first_sequence),
            "native_sequence_end": str(last_sequence),
            # Retain the OW4 field names for existing consumers while making
            # the native-sequence range explicit.
            "first_kernel_sequence": str(episode.first_sequence),
            "terminal_kernel_sequence": str(last_sequence),
        }
        if not complete:
            details["incomplete_reason"] = incomplete_reason or "incomplete"
        if episode.latch_id:
            details["latch_id"] = episode.latch_id
        if episode.source_id:
            details["source_id"] = episode.source_id
        if episode.policy_id:
            details["policy_id"] = episode.policy_id
        if episode.preemption_action:
            details["preemption_action"] = episode.preemption_action
        if episode.preemption_result:
            details["preemption_result"] = episode.preemption_result
        details.update(episode.evidence)

        reason = _text(terminal_details.get("reason"))
        if not reason and terminal_state == "Interrupted":
            reason = episode.evidence.get("interrupted_reason", "")
        if reason:
            details["terminal_reason"] = reason
        if _text(terminal_details.get("terminal_authority_override")) == "true":
            details["override"] = "true"
            proposed = _text(terminal_details.get("proposed_terminal"))
            if proposed:
                details["proposed_terminal"] = proposed
        if episode.interrupted_sequence:
            details["interrupted_kernel_sequence"] = str(
                episode.interrupted_sequence
            )
        if not episode.certificate_observed:
            details["stop_certificate_absence_reason"] = (
                "not_observed_in_native_sequence_range"
            )
        if not episode.coverage_observed:
            details["stop_monitor_coverage_absence_reason"] = (
                "not_observed_in_native_sequence_range"
            )
        if extra_details:
            details.update(extra_details)

        self._add_durations(details, episode, timestamp_ns)
        self._remember_marker(episode.execution_id, terminal_state)
        self._remove(episode)
        return StopDecisionRecord(
            execution_id=episode.execution_id,
            timestamp_ns=timestamp_ns,
            run_id=episode.run_id,
            details=details,
        )

    @staticmethod
    def _infer_trigger(episode: _StopEpisode) -> StopDecisionTrigger:
        if episode.source_id == _RUNTIME_COLLISION_SOURCE_ID:
            return StopDecisionTrigger.FAULT
        if episode.source_id or episode.latch_id:
            return StopDecisionTrigger.INTERRUPT
        reason = episode.evidence.get("reason", "")
        if "reset" in reason:
            return StopDecisionTrigger.RESET
        if episode.requested_ts_ns or episode.issued_ts_ns:
            return StopDecisionTrigger.OPERATOR
        return StopDecisionTrigger.UNOBSERVED

    @staticmethod
    def _terminal_authority(
        boundary: StopDecisionBoundary, details: dict[str, Any]
    ) -> str:
        explicit = _text(details.get("terminal_authority"))
        if explicit:
            return explicit
        if boundary == StopDecisionBoundary.INTERRUPT_PREEMPT:
            return "interrupt_preemption"
        if boundary in {
            StopDecisionBoundary.MOTION_TRIP_COMPLETED,
            StopDecisionBoundary.MOTION_TRIP_FAILED,
        }:
            return "motion_trip_stop"
        if boundary == StopDecisionBoundary.RESET:
            return "reset"
        if boundary == StopDecisionBoundary.STATE_TERMINAL:
            return "execution_state_machine"
        return "none"

    @staticmethod
    def _physical_stop_result(
        episode: _StopEpisode,
        boundary: StopDecisionBoundary,
        terminal_state: str,
    ) -> str:
        certificate_result = episode.evidence.get("completion_result", "")
        if certificate_result:
            return certificate_result
        if episode.preemption_result:
            return episode.preemption_result
        if episode.evidence.get("controlled_stop_success") == "true":
            return "succeeded"
        if boundary == StopDecisionBoundary.RESET:
            return "superseded_by_reset"
        if terminal_state == "InterventionRequired":
            return "intervention_required"
        if terminal_state == "Failed":
            return "failed"
        if terminal_state == "Stopped":
            return "stopped_without_native_certificate"
        return "unknown"

    @staticmethod
    def _add_durations(
        details: dict[str, str], episode: _StopEpisode, terminal_ts_ns: int
    ) -> None:
        requested = episode.requested_ts_ns
        issued = episode.issued_ts_ns
        latched = episode.latched_ts_ns
        if requested and issued and issued >= requested:
            details["request_to_issued_ns"] = str(issued - requested)
        if issued and terminal_ts_ns >= issued:
            details["issued_to_terminal_ns"] = str(terminal_ts_ns - issued)
        if requested and terminal_ts_ns >= requested:
            details["request_to_terminal_ns"] = str(terminal_ts_ns - requested)
        if latched and terminal_ts_ns >= latched:
            details["latch_to_terminal_ns"] = str(terminal_ts_ns - latched)

    def _remember_marker(self, execution_id: int, terminal_state: str) -> None:
        marker = (execution_id, terminal_state)
        if marker in self._completed_marker_set:
            return
        if len(self._completed_markers) == self._completed_markers.maxlen:
            evicted = self._completed_markers[0]
            self._completed_marker_set.discard(evicted)
        self._completed_markers.append(marker)
        self._completed_marker_set.add(marker)

    def _remove(self, episode: _StopEpisode) -> None:
        self._episodes.pop(episode.token, None)
        if self._execution_tokens.get(episode.execution_id) == episode.token:
            self._execution_tokens.pop(episode.execution_id, None)
        if episode.latch_id and self._latch_tokens.get(episode.latch_id) == episode.token:
            self._latch_tokens.pop(episode.latch_id, None)


def sequence_ordered(events: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Stable native-sequence ordering; unsequenced loss evidence sorts first."""
    indexed = list(enumerate(events))
    indexed.sort(
        key=lambda item: (
            1 if _positive_int(item[1].get("sequence")) else 0,
            _positive_int(item[1].get("sequence"))
            or _positive_int(item[1].get("event_index"))
            or item[0],
            item[0],
        )
    )
    return [event for _, event in indexed]
