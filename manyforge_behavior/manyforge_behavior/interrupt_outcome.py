"""Runner mapping of a typed interrupt result to a node result (P5, I5).

The Kernel reports facts only (`io_rule_outcomes`, kernel-rule-result-wire01
and the compact BB evidence of kernel-rule-result-wire02); the Runner maps them
for `motion.move_until_signal` / `motion.probe_search`:

- a trip contained by the rule's entry stage (``terminal_cause:
  signal_trip``, ``stop_result: succeeded_nominal``; spring-back substitution
  stays nominal) maps per ``on_signal`` (default success) with node outcome
  ``signal_trip``;
- a trip contained by a later stage (``succeeded_escalated``) maps per
  ``on_signal`` only when that stage is at or before the rule's
  ``success_through_stage_id`` in the robot's qualified Stop chain (contract
  70; omitted = only the entry stage counts). That is the requested response:
  success and no escalated-failure handling here (contract 69; the latch
  decision is the Kernel's). A later stage is a Failure;
- a natural completion without trip (``no_signal_trip``) maps per
  ``on_no_signal`` (default = the existing spec 260 mapping: success for
  move_until_signal, failure for probe_search) with ``no_signal_trip``;
- a source loss is a Failure through normal failure handling (contract 46c);
- an interrupted fault (``interrupted_fault``) is a Failure, never a trip
  success, even with trip evidence and a contained Stop;
- a timeout, unknown or failed containment, a missing or malformed result
  (including malformed BB evidence): Failure (``effect_uncertain`` when
  containment is not proven). A match or an accepted Stop alone never gives
  Success.

An ordinary Stop (``terminal_cause: stopped``) is not an interrupt terminal:
``map_interrupt_result`` returns None and the existing Stop handling applies.

The rule row itself names its Stop certificate, not the stage that contained
it: the caller passes the robot's qualified stage order (``stop_chain``, per
participant for a coordinated Stop) and the containing stage
(``contained_stage_id``, or per participant, see
``contained_stages_from_stop_terminal``).
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import json
from math import isfinite
from typing import Any, Final

DEFAULT_ON_SIGNAL: Final = "success"
DEFAULT_ON_NO_SIGNAL: Final = {
    # signal_trip_or_allowed_natural_completion
    "motion.move_until_signal": "success",
    # precision_contact_trip
    "motion.probe_search": "failure",
}

# `interrupted_fault` (Core MotionExecutionTerminalCause::kInterruptedFault):
# the existing fault handling, a node Failure and never a trip success.
_TERMINAL_CAUSES: Final = frozenset(
    {"signal_trip", "no_signal_trip", "stopped", "failed", "interrupted_fault"}
)
_STOP_RESULTS: Final = frozenset(
    {"succeeded_nominal", "succeeded_escalated", "partial", "failed_uncontained"}
)
_UNCONTAINED: Final = frozenset({"partial", "failed_uncontained"})
_MAX_SAFE: Final = 9_007_199_254_740_991
_MAX_UINT64: Final = 2**64 - 1
_INT64: Final = (-(2**63), 2**63 - 1)
_SCOPE_FIELDS: Final = frozenset(
    {
        "owner_epoch",
        "program_runner_owner_epoch",
        "program_binding_id",
        "active_publication_generation",
        "cycle_generation",
        "claim_generation",
        "program_run_id",
    }
)
_TRIP_BB_FIELDS: Final = frozenset(
    {"scope", "source_id", "source_epoch", "message_sequence", "received_at_ns", "values"}
)
_LOSS_BB_FIELDS: Final = frozenset(
    {"scope", "source_id", "source_epoch", "last_accepted_message_sequence", "unavailable_key"}
)


@dataclass(frozen=True, slots=True)
class InterruptNodeResult:
    success: bool
    node_outcome: str
    reason_code: str | None = None
    # The history row text, e.g. "rule r tripped: door_open=true (frame 4)".
    message: str = ""
    effect_uncertain: bool = False


class _Malformed(ValueError):
    pass


def _failure(code: str, message: str, *, uncertain: bool = False) -> InterruptNodeResult:
    return InterruptNodeResult(False, "failed", code, message, uncertain)


def _mapped(choice: str, outcome: str, message: str) -> InterruptNodeResult:
    if choice == "success":
        return InterruptNodeResult(True, outcome, None, message)
    return InterruptNodeResult(False, outcome, f"motion_{outcome}_mapped_failure", message)


# Compact BB evidence (kernel-rule-result-wire02) ---------------------------


def _text(value: Any) -> str:
    if type(value) is not str or not value:
        raise _Malformed("identity must be nonempty text")
    return value


def _safe_positive(value: Any) -> int:
    if type(value) is not int or not 1 <= value <= _MAX_SAFE:
        raise _Malformed("generation must be a positive safe integer")
    return value


def _decimal(value: Any, *, minimum: int, maximum: int = _MAX_UINT64) -> int:
    if type(value) is not str or not value.isascii() or not value.isdigit():
        raise _Malformed("sequence must be a canonical decimal string")
    if value != str(int(value)) or not minimum <= int(value) <= maximum:
        raise _Malformed("sequence must be a canonical decimal string")
    return int(value)


def _closed(value: Any, fields: frozenset[str]) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or set(value) != fields:
        raise _Malformed("BB evidence object is not closed")
    return value


def _scope(value: Any, registration: Mapping[str, Any]) -> Mapping[str, Any]:
    scope = _closed(value, _SCOPE_FIELDS)
    for field in ("owner_epoch", "program_runner_owner_epoch", "program_binding_id"):
        _text(scope[field])
    _safe_positive(scope["active_publication_generation"])
    _safe_positive(scope["cycle_generation"])
    claim, run = scope["claim_generation"], scope["program_run_id"]
    if (claim is None) != (run is None):
        raise _Malformed("claim and Run are both null or both present")
    if claim is not None:
        _safe_positive(claim)
        _text(run)
    if scope["owner_epoch"] != registration.get("owner_epoch"):
        raise _Malformed("BB evidence names another Kernel owner")
    return scope


def _bb_value(row: Any) -> tuple[str, str]:
    row = _closed(row, frozenset({"name", "value_type", "value"}))
    name, value_type, value = _text(row["name"]), row["value_type"], row["value"]
    if value_type == "bool" and type(value) is bool:
        text = "true" if value else "false"
    elif value_type == "int64":
        if type(value) is not str or value != value.strip() or value in {"-0", "+0"}:
            raise _Malformed("int64 value must be canonical")
        try:
            number = int(value)
        except ValueError as error:
            raise _Malformed("int64 value must be canonical") from error
        if value != str(number) or not _INT64[0] <= number <= _INT64[1]:
            raise _Malformed("int64 value must be canonical")
        text = value
    elif (
        value_type == "float64"
        and type(value) in (int, float)
        and isfinite(value)
    ):
        text = repr(float(value))
    elif value_type == "string" and type(value) is str:
        text = json.dumps(value, ensure_ascii=False)
    else:
        raise _Malformed(f"BB value {name!r} is not a known {value_type!r} value")
    return name, text


def _trip_blackboard(trip: Mapping[str, Any], registration: Mapping[str, Any]) -> str | None:
    """Validate a trip's optional BB evidence; return its "k=v, ... (frame N)"."""

    if "blackboard" not in trip:
        return None
    evidence = _closed(trip["blackboard"], _TRIP_BB_FIELDS)
    _scope(evidence["scope"], registration)
    source_id, source_epoch = _text(evidence["source_id"]), _text(evidence["source_epoch"])
    sequence = _decimal(evidence["message_sequence"], minimum=1)
    _decimal(evidence["received_at_ns"], minimum=0, maximum=2**63 - 1)
    values = evidence["values"]
    if not isinstance(values, list) or not values:
        raise _Malformed("BB evidence carries no used value")
    rows = [_bb_value(row) for row in values]
    names = [name for name, _text_value in rows]
    if names != sorted(set(names)):
        raise _Malformed("BB evidence values are not sorted and unique")
    acquisition = trip.get("triggering_acquisition")
    if isinstance(acquisition, Mapping) and acquisition.get("source_id") == source_id:
        if (
            acquisition.get("source_epoch") != source_epoch
            or acquisition.get("source_sequence") != evidence["message_sequence"]
        ):
            raise _Malformed("BB evidence contradicts the causal acquisition")
    return ", ".join(f"{name}={text}" for name, text in rows) + f" (frame {sequence})"


def _loss_blackboard(loss: Mapping[str, Any], registration: Mapping[str, Any]) -> str | None:
    """Validate a source loss's optional BB evidence; return its summary."""

    if "blackboard" not in loss:
        return None
    evidence = _closed(loss["blackboard"], _LOSS_BB_FIELDS)
    _scope(evidence["scope"], registration)
    if (
        _text(evidence["source_id"]) != loss.get("source_id")
        or _text(evidence["source_epoch"]) != loss.get("source_epoch")
    ):
        raise _Malformed("BB loss evidence names another source")
    last = _decimal(evidence["last_accepted_message_sequence"], minimum=0)
    key = evidence["unavailable_key"]
    if key is not None:
        _text(key)
        return f"key {key} unavailable (last frame {last})"
    return f"BB source unavailable (last frame {last})"


# Stop stage threshold (contract 70) ----------------------------------------


def contained_stages_from_stop_terminal(
    row: Mapping[str, Any], terminal: Mapping[str, Any], *, operation_id: str
) -> dict[str, str] | None:
    """Per participant, the stage that contained this row's Stop, from the
    existing detailed Stop terminal projection
    (``manyforge.stop_terminal_projection.v1``, the ``stop_terminal_projected``
    event row) of exactly the row's Stop id/generation and certificate; None
    when that is not proven (another Stop or certificate, a truncated summary
    without details, malformed rows): unresolved evidence never implies
    success.

    Only ``attempted_stages[]`` rows of this operation that are
    ``authoritative``, ``succeeded`` and error-free count, and each must carry
    the participant's own ``participant_stop_invocation_plan_digest`` from the
    certificate's ``participant_settlement``. A coordinated Stop can contain
    different participants at different stages, so the result is per
    participant. A natural finish or pre-dispatch no-effect has no attempt and
    so no containing stage (an empty mapping).
    """

    certificate_id = row.get("stop_certificate_id")
    if (
        type(certificate_id) is not str
        or not certificate_id
        or terminal.get("stop_certificate_id") != certificate_id
        or terminal.get("stop_certificate_revision") != row.get("stop_certificate_revision")
        or type(row.get("stop_id")) is not str
        or terminal.get("stop_id") != row["stop_id"]
        or type(row.get("stop_generation")) is not int
        or terminal.get("stop_generation") != row["stop_generation"]
        or terminal.get("details_truncated") is not False
    ):
        return None
    attempts = terminal.get("attempted_stages")
    settlement = terminal.get("participant_settlement")
    if not isinstance(attempts, list) or not isinstance(settlement, list):
        return None

    def participant(scope_key: Any) -> str | None:
        subject = scope_key.get("subject") if isinstance(scope_key, Mapping) else None
        if (
            not isinstance(subject, Mapping)
            or subject.get("kind") != "participant"
            or subject.get("operation_id") != operation_id
            or type(subject.get("participant_id")) is not str
        ):
            return None
        return subject["participant_id"]

    digests = {
        participant(entry.get("scope_key")): entry.get("participant_stop_invocation_plan_digest")
        for entry in settlement
        if isinstance(entry, Mapping)
    }
    contained: dict[str, tuple[int, str]] = {}
    for attempt in attempts:
        if not isinstance(attempt, Mapping):
            return None
        participant_id = participant(attempt.get("scope_key"))
        stage = attempt.get("stage")
        if (
            participant_id is None
            or attempt.get("operation_id") != operation_id
            or attempt.get("participant_id") != participant_id
            or attempt.get("authority") != "authoritative"
            or attempt.get("outcome") != "succeeded"
            or attempt.get("error") is not None
        ):
            continue
        digest = attempt.get("participant_stop_invocation_plan_digest")
        if (
            not isinstance(stage, Mapping)
            or type(stage.get("stage_id")) is not str
            or type(stage.get("stage_index")) is not int
            or type(digest) is not str
            or digests.get(participant_id) != digest
        ):
            return None
        index = stage["stage_index"]
        if participant_id not in contained or index > contained[participant_id][0]:
            contained[participant_id] = (index, stage["stage_id"])
    return {key: stage_id for key, (_index, stage_id) in contained.items()}


def contained_stage_from_stop_terminal(
    row: Mapping[str, Any],
    terminal: Mapping[str, Any],
    *,
    operation_id: str,
    participant_id: str | None = None,
) -> str | None:
    """The one participant's containing stage (``participant_id``, or the only
    contained participant); None when not proven or when several participants
    were contained (no single guessed stage)."""

    stages = contained_stages_from_stop_terminal(row, terminal, operation_id=operation_id)
    if not stages:
        return None
    if participant_id is not None:
        return stages.get(participant_id)
    return next(iter(stages.values())) if len(stages) == 1 else None


def _within_one(entry: Any, threshold: Any, chain: tuple[str, ...], contained: str) -> bool | None:
    entry = entry or (chain[0] if chain else None)
    if entry not in chain or threshold not in chain or contained not in chain:
        return None
    entry_index = chain.index(entry)
    threshold_index = chain.index(threshold)
    contained_index = chain.index(contained)
    if threshold_index < entry_index or contained_index < entry_index:
        return None
    return contained_index <= threshold_index


def _within_success_threshold(
    parameters: Mapping[str, Any],
    stop_chain: Sequence[str] | Mapping[str, Sequence[str]],
    contained_stage_id: str | Mapping[str, str] | None,
) -> bool | None:
    """True/False: every containing stage is / some is not at or before the
    rule's success threshold; None: not provable (no threshold counts as
    False). A coordinated Stop is judged per participant against that
    participant's chain (``stop_chain`` keyed by participant)."""

    threshold = parameters.get("success_through_stage_id")
    if threshold is None:
        return False
    if contained_stage_id is None or (
        isinstance(contained_stage_id, Mapping) and not contained_stage_id
    ):
        return None
    stages = (
        contained_stage_id
        if isinstance(contained_stage_id, Mapping)
        else {None: contained_stage_id}
    )
    results = []
    for participant_id, stage_id in stages.items():
        chain = (
            stop_chain.get(participant_id) if isinstance(stop_chain, Mapping) else stop_chain
        )
        if chain is None or type(stage_id) is not str:
            return None
        results.append(
            _within_one(parameters.get("stop_entry_stage_id"), threshold, tuple(chain), stage_id)
        )
    if any(result is None for result in results):
        return None
    return all(results)


# Mapping -------------------------------------------------------------------


def _rule_rows(operation: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    """The operation-scoped rule results of this operation: the node's own
    rule (contract 63: a node rule references no Deployment policy, and
    global rules have activation scopes, never operation results)."""

    rows: list[Mapping[str, Any]] = []
    for participant in operation.get("participants") or ():
        if not isinstance(participant, Mapping):
            continue
        for row in participant.get("io_rule_outcomes") or ():
            registration = row.get("registration") if isinstance(row, Mapping) else None
            if not isinstance(registration, Mapping):
                continue
            scope = registration.get("scope")
            if not isinstance(scope, Mapping) or scope.get("kind") != "operation":
                continue
            if scope.get("operation_id") != operation.get("operation_id"):
                continue
            rows.append(row)
    return rows


def map_interrupt_result(
    node_id: str,
    parameters: Mapping[str, Any],
    operation: Mapping[str, Any],
    *,
    stop_chain: Sequence[str] | Mapping[str, Sequence[str]] = (),
    contained_stage_id: str | Mapping[str, str] | None = None,
) -> InterruptNodeResult | None:
    """Map the terminal operation's typed rule result for this node's rule."""

    on_signal = parameters.get("on_signal", DEFAULT_ON_SIGNAL)
    on_no_signal = parameters.get("on_no_signal", DEFAULT_ON_NO_SIGNAL[node_id])
    rows = _rule_rows(operation)
    if len(rows) != 1:
        return _failure(
            "motion_interrupt_result_missing",
            "the operation reports no single result for the node's interrupt rule",
            uncertain=True,
        )
    row = rows[0]
    registration = row["registration"]
    rule = registration.get("policy_id") or "node rule"
    evidence = row.get("evidence")
    cause = row.get("terminal_cause")
    stop_result = row.get("stop_result")
    if (
        not isinstance(evidence, Mapping)
        or (cause is not None and cause not in _TERMINAL_CAUSES)
        or (stop_result is not None and stop_result not in _STOP_RESULTS)
    ):
        return _failure(
            "motion_interrupt_result_invalid",
            "the interrupt result is malformed",
            uncertain=True,
        )
    if cause == "stopped":
        return None
    trip = evidence.get("trip")
    losses = evidence.get("source_losses") or ()
    try:
        trip_text = (
            _trip_blackboard(trip, registration) if isinstance(trip, Mapping) else None
        )
        loss_texts = [
            _loss_blackboard(loss, registration)
            for loss in losses
            if isinstance(loss, Mapping)
        ]
    except _Malformed as error:
        return _failure(
            "motion_interrupt_result_invalid",
            f"the interrupt BB evidence is malformed: {error}",
            uncertain=True,
        )
    tripped = f"rule {rule} tripped" + (f": {trip_text}" if trip_text else "")
    if cause == "signal_trip" and trip is not None and stop_result == "succeeded_nominal":
        return _mapped(on_signal, "signal_trip", f"{tripped}; contained by the entry stage")
    if losses:
        source = losses[0].get("source_id") if isinstance(losses[0], Mapping) else None
        detail = next((text for text in loss_texts if text), None)
        return _failure(
            "interrupt_source_unavailable",
            f"interrupt guard source {source or 'unknown'} was lost"
            + (f": {detail}" if detail else ""),
            uncertain=stop_result is None or stop_result in _UNCONTAINED,
        )
    if cause == "signal_trip" and (trip is None or stop_result != "succeeded_escalated"):
        return _failure(
            "motion_interrupt_result_invalid",
            "a signal trip needs its trip evidence and a contained Stop",
            uncertain=True,
        )
    if cause == "no_signal_trip":
        return _mapped(on_no_signal, "no_signal_trip", "the move ended without a signal trip")
    if (
        cause in {"signal_trip", "failed"}
        and trip is not None
        and stop_result == "succeeded_escalated"
        and evidence.get("evaluation_error") is None
    ):
        within = _within_success_threshold(parameters, stop_chain, contained_stage_id)
        if within:
            stages = (
                ", ".join(f"{key}: {value}" for key, value in sorted(contained_stage_id.items()))
                if isinstance(contained_stage_id, Mapping)
                else contained_stage_id
            )
            return _mapped(
                on_signal,
                "signal_trip",
                f"{tripped}; contained by stage {stages} within success_through_stage_id",
            )
        if within is None:
            return _failure(
                "motion_interrupt_containing_stage_unproven",
                f"{tripped}; the stage that contained the Stop is not proven "
                "within success_through_stage_id",
            )
        return _failure(
            "motion_interrupt_stop_failed",
            f"{tripped}; contained only by a stage after the rule's success threshold",
        )
    error = row.get("error")
    code = error.get("code") if isinstance(error, Mapping) else None
    if cause is None:
        return _failure(
            "motion_interrupt_result_missing",
            "the interrupt result has no terminal cause",
            uncertain=True,
        )
    return _failure(
        code if isinstance(code, str) and code else "motion_interrupt_stop_failed",
        "the interrupt did not end in an allowed contained trip",
        uncertain=stop_result is None or stop_result in _UNCONTAINED,
    )


__all__ = [
    "DEFAULT_ON_NO_SIGNAL",
    "DEFAULT_ON_SIGNAL",
    "InterruptNodeResult",
    "contained_stage_from_stop_terminal",
    "contained_stages_from_stop_terminal",
    "map_interrupt_result",
]
