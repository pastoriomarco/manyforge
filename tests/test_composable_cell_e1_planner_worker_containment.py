"""Focused qualification tests for the E1 opaque-planner worker boundary."""

from __future__ import annotations

import dataclasses
import hashlib
import math
import os
import pathlib
import signal
import threading
import time

import pytest

from verification.composable_cell_e1.planner_worker_containment import (
    PlannerWorkRequest,
    PlannerWorkerError,
    PlannerWorkerOutcome,
    PlannerWorkerPolicy,
    PlannerWorkerSupervisor,
    run_candidate_probe,
    run_probe,
)


def _request(
    *,
    operation: str = "success",
    deadline_ms: int = 2_000,
    projection_generation: int = 1,
    work_ms: int = 0,
    **overrides: object,
) -> PlannerWorkRequest:
    request = PlannerWorkRequest(
        projection_generation=projection_generation,
        deadline_monotonic_ns=time.monotonic_ns() + deadline_ms * 1_000_000,
        operation=operation,
        work_ms=work_ms,
    )
    return dataclasses.replace(request, **overrides)


@pytest.mark.parametrize(
    ("overrides", "reason"),
    (
        ({"sample_period_s": math.nan}, "nonfinite_input"),
        ({"requested_duration_s": math.inf}, "nonfinite_input"),
        ({"sample_period_s": math.ulp(0.0)}, "sample_period_out_of_bounds"),
        ({"requested_duration_s": 3_601.0}, "duration_out_of_bounds"),
        (
            {
                "requested_duration_s": 20.0,
                "sample_period_s": 0.01,
                "max_output_samples": 1_000,
            },
            "sample_budget_exceeded",
        ),
    ),
)
def test_hostile_input_is_rejected_before_worker_dispatch(
    overrides: dict[str, object],
    reason: str,
) -> None:
    with PlannerWorkerSupervisor() as supervisor:
        original_identity = (supervisor.worker_generation, supervisor.worker_pid)

        outcome = supervisor.execute(_request(**overrides))

        assert not outcome.ok
        assert outcome.code == "invalid_request"
        assert outcome.reason == reason
        assert outcome.request_id is None
        assert outcome.progress == ()
        assert supervisor.dispatch_count == 0
        assert (
            supervisor.worker_generation,
            supervisor.worker_pid,
        ) == original_identity


def test_expired_deadline_is_rejected_before_worker_dispatch() -> None:
    with PlannerWorkerSupervisor() as supervisor:
        original_identity = (supervisor.worker_generation, supervisor.worker_pid)
        work_request = dataclasses.replace(
            _request(),
            deadline_monotonic_ns=time.monotonic_ns() - 1,
        )

        outcome = supervisor.execute(work_request)

        assert outcome.code == "invalid_request"
        assert outcome.reason == "deadline_expired"
        assert outcome.request_id is None
        assert supervisor.dispatch_count == 0
        assert (
            supervisor.worker_generation,
            supervisor.worker_pid,
        ) == original_identity


def test_normal_path_reuses_one_warmed_resource_bounded_worker() -> None:
    policy = PlannerWorkerPolicy()
    with PlannerWorkerSupervisor(policy) as supervisor:
        identity = (supervisor.worker_generation, supervisor.worker_pid)

        first = supervisor.execute(_request())
        second = supervisor.execute(_request())

        assert first.ok and second.ok
        assert (first.worker_generation, first.worker_pid) == identity
        assert (second.worker_generation, second.worker_pid) == identity
        assert first.progress == ("admitted", "dispatched", "running", "terminal")
        assert second.progress == first.progress
        assert supervisor.dispatch_count == 2
        assert supervisor.replacement_count == 0
        assert supervisor.projection_generation == 1
        assert supervisor.projection_digest == (
            "sha256:"
            + hashlib.sha256(b"manyforge-e1-synthetic-projection-v1").hexdigest()
        )
        assert supervisor.worker_native_modules == {}
        assert supervisor.worker_limits == {
            "address_space_bytes": policy.worker_memory_bytes,
            "file_bytes": policy.worker_file_bytes,
            "cpu_seconds": policy.worker_cpu_seconds,
            "core_bytes": 0,
        }


def test_wrong_projection_generation_is_rejected_before_native_dispatch() -> None:
    with PlannerWorkerSupervisor(projection_generation=41) as supervisor:
        outcome = supervisor.execute(_request(projection_generation=42))

        assert not outcome.ok
        assert outcome.code == "invalid_request"
        assert outcome.reason == "projection_generation_mismatch"
        assert outcome.request_id is None
        assert supervisor.dispatch_count == 0


def test_wrong_projection_content_is_rejected_before_worker_readiness() -> None:
    with pytest.raises(PlannerWorkerError, match="readiness identity mismatch"):
        PlannerWorkerSupervisor(expected_projection_digest="sha256:" + "0" * 64)


def test_replacement_with_projection_content_drift_fails_closed() -> None:
    policy = PlannerWorkerPolicy(worker_bootstrap="synthetic_projection_drift")
    with PlannerWorkerSupervisor(policy) as supervisor:
        with pytest.raises(PlannerWorkerError, match="readiness identity mismatch"):
            supervisor.execute(_request(operation="stale_worker_generation"))


def test_result_projection_content_drift_is_fenced_then_recovers() -> None:
    with PlannerWorkerSupervisor() as supervisor:
        old_generation = supervisor.worker_generation

        outcome = supervisor.execute(_request(operation="stale_projection_digest"))

        assert not outcome.ok
        assert outcome.code == "planner_worker_protocol_error"
        assert outcome.reason == "result_shape_invalid"
        assert outcome.replacement_generation == old_generation + 1
        assert supervisor.projection_digest == (
            "sha256:"
            + hashlib.sha256(b"manyforge-e1-synthetic-projection-v1").hexdigest()
        )
        assert supervisor.execute(_request()).ok


def test_unknown_worker_bootstrap_is_rejected_before_process_creation() -> None:
    policy = PlannerWorkerPolicy(worker_bootstrap="unknown")

    with pytest.raises(ValueError, match="worker bootstrap is unsupported"):
        PlannerWorkerSupervisor(policy)


def test_candidate_probe_requires_candidate_bootstrap() -> None:
    with pytest.raises(ValueError, match="requires the RoboPlan bootstrap"):
        run_candidate_probe(
            iterations=10,
            policy=PlannerWorkerPolicy(worker_bootstrap="synthetic"),
        )


def _execute_in_thread(
    supervisor: PlannerWorkerSupervisor,
    request: PlannerWorkRequest,
) -> tuple[threading.Thread, dict[str, PlannerWorkerOutcome]]:
    result: dict[str, PlannerWorkerOutcome] = {}
    thread = threading.Thread(
        target=lambda: result.setdefault("outcome", supervisor.execute(request)),
        daemon=True,
    )
    thread.start()
    return thread, result


def _process_is_executing(pid: int) -> bool:
    try:
        state = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split()[2]
    except FileNotFoundError:
        return False
    return state != "Z"


def test_explicit_cancel_kills_noncooperative_process_group_and_recovers() -> None:
    with PlannerWorkerSupervisor() as supervisor:
        thread, result = _execute_in_thread(
            supervisor,
            _request(operation="noncooperative_descendant"),
        )
        active = supervisor.wait_until_active(1.0)
        assert active is not None
        assert active.descendant_pid is not None

        assert supervisor.cancel_active()
        thread.join(2.0)

        assert not thread.is_alive()
        outcome = result["outcome"]
        assert outcome.code == "planner_cancelled"
        assert outcome.reason == "cancel_killed_worker"
        assert outcome.progress == (
            "admitted",
            "dispatched",
            "running",
            "cancel_requested",
            "worker_retired",
            "replacement_ready",
            "terminal",
        )
        assert outcome.replacement_generation == active.worker_generation + 1
        assert outcome.replacement_pid == supervisor.worker_pid
        assert not _process_is_executing(active.worker_pid)
        assert not _process_is_executing(active.descendant_pid)

        recovered = supervisor.execute(_request())
        assert recovered.ok
        assert recovered.worker_pid == outcome.replacement_pid


def test_deadline_is_distinct_from_cancel_and_replaces_worker_before_return() -> None:
    with PlannerWorkerSupervisor() as supervisor:
        old_identity = (supervisor.worker_generation, supervisor.worker_pid)

        outcome = supervisor.execute(
            _request(operation="noncooperative", deadline_ms=60)
        )

        assert outcome.code == "planner_deadline_exceeded"
        assert outcome.reason == "deadline_killed_worker"
        assert outcome.worker_generation == old_identity[0]
        assert outcome.worker_pid == old_identity[1]
        assert outcome.replacement_generation == supervisor.worker_generation
        assert outcome.replacement_pid == supervisor.worker_pid
        assert outcome.kill_ms is not None
        assert outcome.replacement_ms is not None
        assert not _process_is_executing(old_identity[1])
        assert supervisor.execute(_request()).ok


@pytest.mark.parametrize(
    ("operation", "code", "reason"),
    (
        (
            "stale_worker_generation",
            "planner_worker_protocol_error",
            "result_generation_fence",
        ),
        (
            "stale_projection_generation",
            "planner_worker_protocol_error",
            "result_generation_fence",
        ),
        (
            "oversized_result",
            "planner_worker_protocol_error",
            "worker_output_frame_invalid",
        ),
        (
            "crash",
            "planner_worker_crashed",
            "worker_transport_closed:86:EOFError",
        ),
        (
            "memory_exhaustion",
            "planner_worker_resource_exhausted",
            "worker_address_space_limit",
        ),
    ),
)
def test_untrusted_worker_failure_is_fenced_then_recovers(
    operation: str,
    code: str,
    reason: str,
) -> None:
    with PlannerWorkerSupervisor() as supervisor:
        old_generation = supervisor.worker_generation

        outcome = supervisor.execute(_request(operation=operation))

        assert not outcome.ok
        assert outcome.code == code
        assert outcome.reason == reason
        assert outcome.replacement_generation == old_generation + 1
        assert outcome.replacement_generation == supervisor.worker_generation
        assert outcome.replacement_pid == supervisor.worker_pid
        assert supervisor.execute(_request()).ok


def test_worker_dies_if_its_supervisor_is_sigkilled() -> None:
    read_descriptor, write_descriptor = os.pipe()
    supervisor_pid = os.fork()
    if supervisor_pid == 0:
        os.close(read_descriptor)
        try:
            with PlannerWorkerSupervisor() as supervisor:
                os.write(write_descriptor, f"{supervisor.worker_pid}\n".encode("ascii"))
                while True:
                    signal.pause()
        finally:
            os._exit(91)

    os.close(write_descriptor)
    worker_pid: int | None = None
    try:
        payload = os.read(read_descriptor, 64)
        worker_pid = int(payload.decode("ascii").strip())
        assert _process_is_executing(worker_pid)

        os.kill(supervisor_pid, signal.SIGKILL)
        os.waitpid(supervisor_pid, 0)
        supervisor_pid = -1

        deadline = time.monotonic() + 2.0
        while _process_is_executing(worker_pid) and time.monotonic() < deadline:
            time.sleep(0.01)
        assert not _process_is_executing(worker_pid)
    finally:
        os.close(read_descriptor)
        if supervisor_pid > 0:
            try:
                os.kill(supervisor_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(supervisor_pid, 0)
        if worker_pid is not None and _process_is_executing(worker_pid):
            try:
                os.kill(worker_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def test_compact_probe_records_warmed_and_recovery_evidence() -> None:
    result = run_probe(iterations=10)

    assert result["schema"] == "manyforge.e1.worker_containment_result.v1"
    assert result["iterations"] == 10
    assert result["replacement_count"] == 4
    assert result["post_replacement_success"]["ok"] is True
    assert result["initial_worker"]["start_ms"] > 0.0
    assert result["initial_worker"]["first_request_ms"] >= 0.0
    assert result["initial_worker"]["memory_after_warmup"]["rss_bytes"] > 0
    assert result["final_worker"]["memory"]["peak_rss_bytes"] > 0
    assert result["warmed_latency_ms"]["p99"] >= 0.0
    assert result["warmed_latency_ms"]["max"] >= result["warmed_latency_ms"]["p50"]
    assert result["initial_worker"] != result["final_worker"]
