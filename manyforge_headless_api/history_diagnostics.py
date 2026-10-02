"""Bounded, on-demand forensic projections over retained immutable events.

No timer, observer, execution authority or independent log lives here. Missing
causal samples stay missing; current state is never substituted for an incident.
"""

from __future__ import annotations

from collections import Counter
from math import ceil
from typing import Any

from .history_query import CORRELATION_FIELDS


_REFERENCE_FIELDS = frozenset(
    (
        *CORRELATION_FIELDS,
        "graph_revision",
        "retained_bundle_id",
        "scene_id",
        "revision_id",
        "content_hash",
        "owner_epoch",
        "owner_id",
        "recovery_context_id",
        "stop_decision_id",
        "stop_certificate_id",
        "control_binding_id",
        "control_domain_id",
        "parent_program_run_id",
        "intervention_program_run_id",
        "program_runner_owner_epoch",
        "scene_source_owner_id",
        "scene_source_owner_epoch",
        "scene_content_hash",
    )
)


# Retained Program diagnostic versions. Originals recorded before a schema
# cutover keep their recorded version and this producer-only attribution.
_PROGRAM_DIAGNOSTIC_SCHEMAS = frozenset(
    ("manyforge.program_diagnostic.v1", "manyforge.program_diagnostic.v2")
)


def causal_references(payload: Any) -> tuple[dict[str, list[str]], bool]:
    """All applicable bounded references, never an arbitrary first sibling."""
    # Values can legitimately be named program_run_id, operation_id, etc.
    # Their names are data, never alternate identities for this capture.
    if (
        isinstance(payload, dict)
        and payload.get("schema_version") in _PROGRAM_DIAGNOSTIC_SCHEMAS
    ):
        payload = {
            key: value
            for key, value in payload.items()
            if key not in {"blackboard", "runtime_overrides"}
        }
    found: dict[str, set[str]] = {}
    pending = [
        (
            iter(payload.items())
            if isinstance(payload, dict)
            else enumerate(payload) if isinstance(payload, list) else iter(())
        )
    ]
    visited = 0
    truncated = False
    while pending:
        try:
            name, value = next(pending[-1])
        except StopIteration:
            pending.pop()
            continue
        visited += 1
        if visited > 8192:
            truncated = True
            break
        if name in _REFERENCE_FIELDS and isinstance(value, str) and value:
            values = found.setdefault(name, set())
            if len(values) < 128 and len(value.encode("utf-8")) <= 1024:
                values.add(value)
            else:
                truncated = True
        if isinstance(value, (dict, list)):
            if len(pending) >= 65:
                truncated = True
            else:
                pending.append(iter(value.items()) if isinstance(value, dict) else enumerate(value))
    return {key: sorted(values) for key, values in sorted(found.items())}, truncated


def _ns(value: Any) -> int | None:
    if type(value) is int and 0 <= value <= 2**63 - 1:
        return value
    if isinstance(value, str) and value.isascii() and value.isdecimal() and len(value) <= 19:
        parsed = int(value)
        if str(parsed) == value and parsed <= 2**63 - 1:
            return parsed
    return None


def _distribution(samples: list[int]) -> dict[str, Any]:
    ordered = sorted(samples)
    return {
        "sample_count": len(ordered),
        "unit": "ns",
        "minimum": str(ordered[0]) if ordered else None,
        "maximum": str(ordered[-1]) if ordered else None,
        **{
            name: (str(ordered[max(0, ceil(len(ordered) * quantile) - 1)]) if ordered else None)
            for name, quantile in (("p50", 0.5), ("p95", 0.95), ("p99", 0.99))
        },
    }


def forensic_bundle(
    page: dict[str, Any], *, program_run_id: str | None, exported_at_ns: int
) -> dict[str, Any]:
    events = page["events"]
    # The service supplies at most 256 verified events / eight byte-bounded
    # pages. Each operation sample is keyed by owner clock AND operation.
    if len(events) > 256:
        raise ValueError("forensic event budget exceeded")
    operations: dict[tuple[str, str], dict[str, Any]] = {}
    records, incidents, blackboards = [], [], []
    stop_latencies: list[int] = []
    reference_truncated = False
    for stored in sorted(events, key=lambda item: item["sequence"]):
        event = stored["event"]
        attributes = event["payload"]
        payload = attributes if isinstance(attributes, dict) else {}
        references, clipped = causal_references(attributes)
        reference_truncated |= clipped

        def unique(name: str) -> str | None:
            values = references.get(name, [])
            return values[0] if not clipped and len(values) == 1 else None

        record = {
            "record_id": event["event_id"],
            "event_index": stored["sequence"],
            "timeline_id": unique("program_run_id"),
            "parent_timeline_id": unique("parent_program_run_id"),
            "monotonic_ns": event["occurred_at_ns"],
            "clock_owner": stored["clock_owner"],
            "wall_time": event["occurred_at_wall"],
            "recorded_utc_ns": stored["occurred_at_utc_ns"],
            "wall_clock_regressed": stored["wall_clock_regressed"],
            "level": event["severity"],
            "condition_severity": stored["condition_severity"],
            "component": event["component"],
            "event_type": event["event_type"],
            "message": (
                payload["message"]
                if isinstance(payload.get("message"), str)
                else event["event_type"].replace("_", " ")
            ),
            "program_run_id": unique("program_run_id"),
            "operation_id": unique("operation_id"),
            "action_span_id": unique("action_span_id"),
            "node": unique("node_instance_id"),
            "intervention_session_id": unique("intervention_session_id"),
            "causal_references": references,
            "references_truncated": clipped,
            "attributes": attributes,
            "persistence_digest": stored["persistence_digest"],
        }
        records.append(record)
        if (
            stored["condition_severity"] in {"WARN", "ERROR", "CRITICAL"}
            or event["component"] == "manyforge.alarm"
        ):
            incidents.append(
                {
                    "event_id": event["event_id"],
                    "sequence": stored["sequence"],
                    "causal_references": references,
                    "references_truncated": clipped,
                    "state_source": "incident-time recorded facts only",
                }
            )
        schema = payload.get("schema_version")
        journal = (
            payload.get("journal_payload", {})
            if schema == "manyforge.operation_history_event.v1"
            else {}
        )
        if not isinstance(journal, dict):
            journal = {}
        # Blackboard evidence must be recorded by its producer, not read from
        # a mutable current Program and relabelled with an old event's time.
        for source in (payload, journal):
            if isinstance(source.get("blackboard"), dict):
                blackboards.append(
                    {
                        "event_id": event["event_id"],
                        "sequence": stored["sequence"],
                        "causal_references": references,
                        "values": source["blackboard"],
                        "runtime_overrides": source.get("runtime_overrides"),
                        "omitted_value_count": source.get("omitted_blackboard_count"),
                        "omitted_override_count": source.get("omitted_override_count"),
                        "delivery_loss_count": source.get("delivery_loss_count"),
                    }
                )
        operation_id, clock = payload.get("operation_id"), stored["clock_owner"]
        state = journal.get("operation_state")
        if isinstance(operation_id, str) and isinstance(clock, str) and isinstance(state, dict):
            operation = operations.setdefault(
                (clock, operation_id),
                {
                    "clock_owner": clock,
                    "operation_id": operation_id,
                    "accepted_ns": None,
                    "executing_ns": None,
                    "terminal_ns": None,
                    "latest_state": None,
                    "latest_revision": None,
                    "latest_native_sequence": None,
                    "source_order_unknown_count": 0,
                    "stage_sequences": {},
                    "event_ids": [],
                },
            )
            operation["event_ids"].append(event["event_id"])
            revision = payload.get("operation_revision")
            sequence = payload.get("native_sequence")
            # Revisions fence commands; normal feedback can advance without
            # changing them. Ordered journal captures can also reach storage
            # out of order, so their private order outranks the record sequence.
            # Legacy records retain their source-sequence ordering.
            if type(sequence) is not int or not 0 < sequence <= 9_007_199_254_740_991:
                operation["source_order_unknown_count"] += 1
                continue
            capture_order = _ns(state.get("journal_capture_order")) or 0
            previous_state = operation["latest_state"] or {}
            previous_capture = _ns(previous_state.get("journal_capture_order")) or 0
            if operation["latest_native_sequence"] is None or (
                capture_order,
                sequence,
            ) > (previous_capture, operation["latest_native_sequence"]):
                operation["latest_native_sequence"] = sequence
                operation["latest_revision"] = revision
                operation["latest_state"] = state
            occurred = _ns(event["occurred_at_ns"])

            def stage(field: str) -> None:
                previous = operation["stage_sequences"].get(field)
                order = (capture_order, sequence)
                if occurred is not None and (previous is None or order < previous):
                    operation["stage_sequences"][field] = order
                    operation[field] = occurred

            if payload.get("record_kind") == "operation_accepted":
                stage("accepted_ns")
            if state.get("state") == "executing":
                stage("executing_ns")
            if state.get("state") in {
                "completed",
                "failed",
                "stopped",
                "cancelled_pre_dispatch",
                "interrupted",
                "intervention_required",
            }:
                stage("terminal_ns")
        if payload.get("record_kind") == "stop_terminal_projected":
            duration = _ns(journal.get("acceptance_to_terminal_latency_ns"))
            if duration is not None:
                stop_latencies.append(duration)
    admission, completion = [], []
    incomplete_operations = 0
    for operation in operations.values():
        operation.pop("stage_sequences")
        accepted = operation["accepted_ns"]
        for field, samples in (
            ("executing_ns", admission),
            ("terminal_ns", completion),
        ):
            end = operation[field]
            if accepted is not None and end is not None and end >= accepted:
                samples.append(end - accepted)
        incomplete_operations += accepted is None or operation["terminal_ns"] is None
        for field in ("accepted_ns", "executing_ns", "terminal_ns"):
            if operation[field] is not None:
                operation[field] = str(operation[field])
    coverage = {
        "truncated": page["truncated"],
        "next_before_sequence": page["next_before_sequence"],
        "retention": page.get("retention"),
        "references_truncated": reference_truncated,
        "scope": "retained exported subset; never evidence that omitted effects did not occur",
    }
    return {
        "schema_version": "manyforge.debug_export.v1",
        "program_run_id": program_run_id,
        "coverage": coverage,
        "time_window": page.get("time_window"),
        "execution_digest": {
            "schema_version": "manyforge.execution_digest.v1",
            "program_run_id": program_run_id,
            "event_count": len(events),
            "event_counts": dict(
                sorted(Counter(event["event"]["event_type"] for event in events).items())
            ),
            "operations": list(operations.values()),
            "incomplete_operation_count": incomplete_operations,
        },
        "incidents": incidents,
        "blackboard": {
            "available": bool(blackboards),
            "samples": blackboards,
            "reason": (
                None
                if blackboards
                else "No producer-recorded blackboard evidence is retained in this subset."
            ),
        },
        "latency": {
            "schema_version": "manyforge.run_latency.v1",
            "sample_scope": "exact same-clock retained pairs",
            "acceptance_to_execution": _distribution(admission),
            "acceptance_to_terminal": _distribution(completion),
            "stop_to_terminal": _distribution(stop_latencies),
        },
        "normalized_trace": {
            "schema_version": "manyforge.ros2_medkit_trace.v2",
            "trace_source": "manyforge_headless",
            "exported_at_ns": str(exported_at_ns),
            "program_run_id": program_run_id,
            "run_id": program_run_id,
            "intervention_run_ids": sorted(
                {
                    value
                    for record in records
                    for value in record["causal_references"].get("intervention_program_run_id", [])
                }
            ),
            "parent_run_ids": sorted(
                {
                    value
                    for record in records
                    for value in record["causal_references"].get("parent_program_run_id", [])
                }
            ),
            "coverage": coverage,
            "records": records,
        },
    }
