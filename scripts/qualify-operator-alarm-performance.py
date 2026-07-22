#!/usr/bin/env python3
"""Reproducible Step 20 operator-attention projection benchmark.

The baseline implementation is loaded from the recorded Step 19 git commit;
the current implementation is imported from the worktree. This isolates the
non-real-time live-attention projection cost. Raw EventRecorder persistence is
qualified by its own protected-history tests and is intentionally not timed
twice here.
"""

from __future__ import annotations

import argparse
import gc
import hashlib
import json
from pathlib import Path
import platform
import resource
import statistics
import subprocess
import time
import tracemalloc
from typing import Any, Callable


DEFAULT_BASELINE_REF = "33ca99a73742718970ea122c34070d6d4d72682a"
BASELINE_PATH = "manyforge_composer/backend/operator_alerts.py"
CURRENT_IMPLEMENTATION_PATHS = (
    "manyforge_composer/backend/operator_alarm_catalog.py",
    "manyforge_composer/backend/operator_alarms.py",
    "manyforge_composer/resources/operator_alarm_catalog.v1.yaml",
)


def _git(*args: str) -> str:
    return subprocess.run(
        ["git", "-c", "safe.directory=*", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def _current_implementation_identity() -> dict[str, Any]:
    """Bind benchmark results to the exact dirty-worktree inputs under test."""

    repository_root = Path(__file__).resolve().parent.parent
    files: dict[str, str] = {}
    digest = hashlib.sha256()
    for relative_path in CURRENT_IMPLEMENTATION_PATHS:
        content = (repository_root / relative_path).read_bytes()
        file_hash = hashlib.sha256(content).hexdigest()
        files[relative_path] = file_hash
        digest.update(relative_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(content)
        digest.update(b"\0")
    return {
        "aggregateSha256": digest.hexdigest(),
        "files": files,
    }


def _summary(samples_ns: list[int]) -> dict[str, int]:
    ordered = sorted(samples_ns)

    def percentile(percent: int) -> int:
        rank = max(0, (len(ordered) * percent + 99) // 100 - 1)
        return ordered[min(rank, len(ordered) - 1)]

    return {
        "count": len(ordered),
        "minNs": ordered[0],
        "p50Ns": percentile(50),
        "p95Ns": percentile(95),
        "p99Ns": percentile(99),
        "maxNs": ordered[-1],
        "meanNs": int(statistics.fmean(ordered)),
    }


def _measure(
    case_factory: Callable[[], tuple[Callable[[int], None], Callable[[], dict[str, Any]]]],
    iterations: int,
) -> dict[str, Any]:
    gc.collect()
    call, summarize_case = case_factory()
    cpu_started_ns = time.process_time_ns()
    wall_started_ns = time.perf_counter_ns()
    samples: list[int] = []
    for index in range(iterations):
        started_ns = time.perf_counter_ns()
        call(index)
        samples.append(time.perf_counter_ns() - started_ns)
    wall_ns = time.perf_counter_ns() - wall_started_ns
    cpu_ns = time.process_time_ns() - cpu_started_ns
    result = summarize_case()

    # Allocation tracing materially changes Python hot-path latency. Measure
    # peak memory in a fresh, shorter case so latency remains representative.
    memory_iterations = min(iterations, 5_000)
    gc.collect()
    memory_call, _summarize_memory_case = case_factory()
    rss_before_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    tracemalloc.start()
    for index in range(memory_iterations):
        memory_call(index)
    _current_bytes, peak_bytes = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    rss_after_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    result.update({
        "latency": _summary(samples),
        "totalWallNs": wall_ns,
        "totalCpuNs": cpu_ns,
        "operationsPerSecond": int(iterations * 1_000_000_000 / max(1, wall_ns)),
        "tracemallocPeakBytes": peak_bytes,
        "memoryMeasurementOperations": memory_iterations,
        "maxRssDeltaKiB": max(0, rss_after_kib - rss_before_kib),
    })
    return result


def _baseline_inbox_class(baseline_ref: str) -> tuple[Any, str]:
    source = _git("show", f"{baseline_ref}:{BASELINE_PATH}")
    namespace: dict[str, Any] = {
        "__name__": "manyforge_step19_operator_alerts",
        "__file__": f"git:{baseline_ref}:{BASELINE_PATH}",
    }
    exec(compile(source, namespace["__file__"], "exec"), namespace)  # noqa: S102
    return namespace["OperatorAlertInbox"], source


def _baseline_case(baseline_ref: str, iterations: int) -> dict[str, Any]:
    inbox_class, source = _baseline_inbox_class(baseline_ref)

    def case_factory() -> tuple[Callable[[int], None], Callable[[], dict[str, Any]]]:
        inbox = inbox_class()

        def ingest(index: int) -> None:
            inbox.ingest_incident(
                {
                    "incidentId": f"incident-{index}",
                    "recoveryContextId": "recovery-context-benchmark",
                    "triggerSeverity": "ERROR",
                    "triggerType": "collision_detected",
                    "triggerChannel": "fault_safety_stop",
                    "timestampNs": 1_000_000_000 + index,
                    "triggerEvent": {
                        "type": "collision_detected",
                        "severity": "ERROR",
                        "execution_id": 41,
                        "message": "Runtime collision benchmark",
                        "details": {
                            "reason_code": "runtime_collision",
                            "contact_pair": "robot/wrist <-> object/wall",
                        },
                    },
                    "headlineCause": {
                        "summary": "Runtime collision benchmark",
                        "errorCode": "runtime_collision",
                    },
                }
            )

        def summarize_case() -> dict[str, Any]:
            snapshot = inbox.snapshot()
            return {
                "liveCardinality": snapshot["blockingCount"],
                "occurrenceCount": snapshot["alerts"][0]["occurrenceCount"],
            }

        return ingest, summarize_case

    result = _measure(case_factory, iterations)
    result["implementationSha256"] = hashlib.sha256(
        source.encode("utf-8")
    ).hexdigest()
    return result


def _current_system() -> Any:
    from manyforge_composer.backend.operator_alarm_catalog import (
        load_operator_alarm_catalog,
    )
    from manyforge_composer.backend.operator_alarms import OperatorAlarmManager

    manager = OperatorAlarmManager(
        load_operator_alarm_catalog(),
        required_domains=("motion",),
        activation_factory=lambda: "benchmark-activation",
    )
    manager.register_source(
        domain="motion",
        source_id="benchmark-motion-source",
        adapter_id="manyforge.adapter.motion_condition",
        source_epoch="benchmark-epoch",
        source_epoch_order=1,
    )
    manager.complete_source_snapshot(
        source_id="benchmark-motion-source",
        source_epoch="benchmark-epoch",
    )
    return manager


def _current_update(sequence: int, *, observed_base_ns: int) -> Any:
    from manyforge_composer.backend.operator_alarms import AlarmConditionUpdate

    return AlarmConditionUpdate(
        definition_id="manyforge.motion.runtime_collision",
        scope={
            "robot_resource": "robot0",
            "contact_pair": "robot/wrist <-> object/wall",
        },
        active=True,
        source_id="benchmark-motion-source",
        source_epoch="benchmark-epoch",
        source_sequence=sequence,
        observed_at_ns=observed_base_ns + sequence,
        observed_at_wall="2026-07-21T00:00:00Z",
        quality="good",
        details={
            "reason_code": "runtime_collision",
            "contact_pair": "robot/wrist <-> object/wall",
        },
        evidence_refs=(f"kernel-event:{sequence}",),
        correlations={"execution_id": 41},
    )


def _current_repeat_case(iterations: int) -> dict[str, Any]:
    def case_factory() -> tuple[Callable[[int], None], Callable[[], dict[str, Any]]]:
        manager = _current_system()
        observed_base_ns = time.monotonic_ns()

        def ingest(index: int) -> None:
            manager.submit(
                _current_update(index + 1, observed_base_ns=observed_base_ns)
            )

        def summarize_case() -> dict[str, Any]:
            snapshot = manager.snapshot()
            return {
                "liveCardinality": snapshot["liveCount"],
                "occurrenceCount": snapshot["alarms"][0]["occurrenceCount"],
                "managerMetrics": manager.metrics(),
            }

        return ingest, summarize_case

    return _measure(case_factory, iterations)


def _current_duplicate_case(iterations: int) -> dict[str, Any]:
    def case_factory() -> tuple[Callable[[int], None], Callable[[], dict[str, Any]]]:
        manager = _current_system()
        update = _current_update(1, observed_base_ns=time.monotonic_ns())
        manager.submit(update)

        def ingest(_index: int) -> None:
            manager.submit(update)

        def summarize_case() -> dict[str, Any]:
            snapshot = manager.snapshot()
            return {
                "liveCardinality": snapshot["liveCount"],
                "occurrenceCount": snapshot["alarms"][0]["occurrenceCount"],
                "managerMetrics": manager.metrics(),
            }

        return ingest, summarize_case

    return _measure(case_factory, iterations)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-ref", default=DEFAULT_BASELINE_REF)
    parser.add_argument("--iterations", type=int, default=20_000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.iterations < 100 or args.iterations > 100_000:
        parser.error("--iterations must be between 100 and 100000")

    baseline = _baseline_case(args.baseline_ref, args.iterations)
    current_repeat = _current_repeat_case(args.iterations)
    current_duplicate = _current_duplicate_case(args.iterations)
    assertions = {
        "baselineOneLiveRecord": baseline["liveCardinality"] == 1,
        "currentRepeatOneLiveRecord": current_repeat["liveCardinality"] == 1,
        "currentRepeatOccurrenceComplete": (
            current_repeat["occurrenceCount"] == args.iterations
        ),
        "currentDuplicateOneLiveRecord": current_duplicate["liveCardinality"] == 1,
        "currentDuplicateDoesNotInventOccurrence": (
            current_duplicate["occurrenceCount"] == 1
        ),
        "currentMetricSamplesBounded": (
            current_repeat["managerMetrics"]["distributions"]["ingestLatencyNs"][
                "count"
            ]
            <= 4096
        ),
    }
    payload = {
        "schemaVersion": "manyforge.operator_alarm_performance.v1",
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "baselineRef": args.baseline_ref,
        "currentHead": _git("rev-parse", "HEAD"),
        "worktreeDirty": bool(_git("status", "--porcelain")),
        "currentImplementation": _current_implementation_identity(),
        "iterations": args.iterations,
        "environment": {
            "python": platform.python_version(),
            "platform": platform.platform(),
            "processor": platform.processor(),
        },
        "scope": (
            "NRT live-attention projection only; raw protected-history "
            "persistence is covered by EventRecorder qualification"
        ),
        "baselineStep19SeverityInbox": baseline,
        "currentStep20ConditionRepeat": current_repeat,
        "currentStep20ExactDuplicate": current_duplicate,
        "assertions": assertions,
        "passed": all(assertions.values()),
    }
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if payload["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
