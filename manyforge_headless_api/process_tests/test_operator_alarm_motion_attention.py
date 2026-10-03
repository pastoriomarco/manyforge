"""Kernel motion admission consults Headless operator-alarm attention.

Spec 450 §5.2 / spec 530 §11 in the composed process stack: an
acknowledgement-gated alarm on a robot's resources refuses the Program Run
that would move it until the operator acknowledges it, and the Kernel admits
the same Run once the Headless publication reaches it.
"""

from __future__ import annotations

from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import shutil
import socket
import sys
from tempfile import TemporaryDirectory
import threading
import time
from typing import Any, Callable, Iterator

import pytest

from manyforge_headless_api.alarm_attention import (
    KERNEL_FRESHNESS_SECONDS,
    PUBLICATION_DEADLINE_SECONDS,
    PUBLICATION_PERIOD_SECONDS,
    OperatorAlarmAttentionPublisher,
)
from manyforge_headless_api.cell_admission import (
    EXECUTION_CONTROL_SOCKET_FILE,
    MAXIMUM_RESPONSE_BYTES,
    CellAdmissionClient,
)
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient, ExecutionEventPump
from manyforge_headless_api.native import HeadlessApiConfig, NativeHeadlessCore
from manyforge_headless_api.process_tests.test_process_stack import (
    CATALOG,
    NODE_CATALOG,
    NODE_ENVIRONMENT_MANIFEST,
    ProcessStack,
    _read_only_tree,
    _required_path,
)
from manyforge_headless_api.process_tests.test_ui6i_grouped_execution import (
    ADMISSION_FILENAME,
    CELL_ID,
    GROUPED_SCENARIO,
    _dual_robot_stack,
    _load_scenario,
    _reset_cycle,
    _wait_run_state,
    _write_mixed_variants,
)

_ALARM_REFUSALS = {
    "operator_alarm_interlock_active",
    "unacknowledged_operator_alarms",
    "operator_alarm_state_unavailable",
}


def _motion_start(
    client: HeadlessHttpClient, started: dict[str, Any], request_id: str
) -> dict[str, Any]:
    """Return the owner's result, or its refusal as `{"refused": code, ...}`."""
    program_run_id = started["capture"]["program_run_id"]
    deadline = time.monotonic() + 10.0
    while True:
        cycle = client.program_cycle_status()
        controls = cycle.get("runtime_controls")
        selections = None if controls is None else controls.get("execution_selections")
        if cycle.get("program_run_id") == program_run_id and isinstance(selections, dict):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(f"motion controls were not published: {cycle!r}")
        time.sleep(0.01)
    try:
        result = client.program_run_motion_start(
            program_run_id,
            {
                "request_id": request_id,
                "request_owner_epoch": "alarm.attention.process-test",
                "source_label": "alarm_attention_process_test",
                "correlation_id": request_id,
                "program_run_id": program_run_id,
                "expected_owner_epoch": cycle["program_runner_owner_epoch"],
                "expected_run_revision": cycle["run"]["run_revision"],
                "expected_selection_revision": selections["selection_revision"],
                "reason": "operator motion Run",
                "group_ids": ["all"],
            },
        )
    except HeadlessClientError as error:
        return {"refused": error.code, "details": error.details}
    if result.get("ok") is False:
        error = result.get("error") or {}
        return {"refused": error.get("code"), "details": error.get("details") or {}}
    return result


def _start_run_once(
    stack: ProcessStack, applied: dict[str, Any], digest: str, label: str
) -> dict[str, Any]:
    """Start a Program run under fresh request identities (cf. `_start_run`)."""
    deadline = time.monotonic() + 30.0
    attempt = 0
    with HeadlessHttpClient(stack.composer_url, timeout_seconds=20.0) as composer:
        while True:
            attempt += 1
            draft = composer.request("GET", "/api/program/draft")["program"]
            try:
                return composer.program_run_start(
                    {
                        "request_id": f"alarm.attention.{label}.{attempt}",
                        "request_owner_epoch": "alarm.attention.process-test",
                        "source_label": "alarm_attention_process_test",
                        "correlation_id": f"alarm.attention.{label}",
                        "scenario_id": applied["scenario_id"],
                        "source_document_digest": digest,
                        "expected_applied_record_digest": applied["source"]["record_digest"],
                        "draft_program_revision": draft["program_revision"],
                        "expected_admission_generation": stack.wait_authoring_admission(
                            expected_allowed=True
                        )["admission_generation"],
                    }
                )
            except HeadlessClientError as error:
                # Reset releases the previous cycle claim asynchronously.
                if error.status_code != 409 or time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)


def _gating_alarms(stack: ProcessStack) -> dict[str, Any]:
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        return headless.alarms()


def _acknowledge_all(stack: ProcessStack) -> None:
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        for attempt in range(8):
            snapshot = headless.alarms()
            members = snapshot["unacknowledged_membership"]
            if not members:
                return
            try:
                headless.acknowledge_all(
                    {
                        "request_id": f"alarm.attention.ack.{attempt}",
                        "request_owner_epoch": snapshot["owner_epoch"],
                        "source_label": "alarm_attention_process_test",
                        "members": members,
                        "expected_generation": snapshot["generation"],
                        "confirmed": True,
                    }
                )
                return
            except HeadlessClientError as error:
                # A concurrent activation changed the exact membership.
                if attempt == 7 or error.status_code != 409:
                    raise
    raise AssertionError("unreachable")


def test_program_run_is_refused_by_an_unacknowledged_alarm_until_acknowledged() -> None:
    with _dual_robot_stack() as stack:
        _load_scenario(stack, GROUPED_SCENARIO)
        # The moved UR5e's Program goal is in collision with the wall, so its
        # operation fails and raises an acknowledgement-gated execution alarm
        # scoped to the UR5e's exact resources.
        paths = _write_mixed_variants(stack, ("move",))
        applied, digest = _load_scenario(stack, "mixed_move", scenario_path=paths["move"])
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            first = _start_run_once(stack, applied, digest, "first")
            first_run = first["capture"]["program_run_id"]
            # Attention is unavailable until the alarm owner has observed the
            # Kernel's newly applied configuration (spec 530 §11); that
            # converges within one source refresh and publication.
            deadline = time.monotonic() + 10.0
            attempt = 0
            while True:
                attempt += 1
                initial = _motion_start(
                    client, first, f"alarm.attention.first.run.{attempt}"
                )
                if "refused" not in initial:
                    break
                assert initial["refused"] == "operator_alarm_state_unavailable", initial
                assert initial["details"].get("availability") == "configuration_mismatch", initial
                assert time.monotonic() < deadline, initial
                time.sleep(0.1)
            terminal = _wait_run_state(
                client, first_run,
                {"succeeded", "failed", "intervention_required", "stopped"},
            )
            assert terminal["state"] != "succeeded", terminal

            deadline = time.monotonic() + 20.0
            while True:
                snapshot = _gating_alarms(stack)
                if snapshot["unacknowledged_membership"]:
                    break
                assert time.monotonic() < deadline, snapshot
                time.sleep(0.05)
            assert snapshot["reconciled"] is True, snapshot
            gating = snapshot["motion_attention_membership"]
            assert gating, snapshot
            assert all(row["lifecycle_state"] != "active_acknowledged" for row in gating)

            _reset_cycle(client, first_run)
            second = _start_run_once(stack, applied, digest, "second")
            # The alarm still requires acknowledgement after Reset returned
            # its condition: the Kernel refuses the Run that would move UR5e.
            refused = _motion_start(client, second, "alarm.attention.refused.run")
            assert refused.get("refused") == "unacknowledged_operator_alarms", refused
            assert refused["details"]["interlock_alarms"] == "[]", refused
            assert '"condition_id"' in refused["details"]["unacknowledged_alarms"], refused

            _acknowledge_all(stack)
            # Headless republishes within its read period; the Kernel then
            # admits the same Run with no other change.
            deadline = time.monotonic() + 10.0
            attempt = 0
            while True:
                attempt += 1
                admitted = _motion_start(
                    client, second, f"alarm.attention.acknowledged.run.{attempt}"
                )
                if "refused" not in admitted:
                    break
                assert admitted["refused"] in _ALARM_REFUSALS, admitted
                assert time.monotonic() < deadline, admitted
                time.sleep(0.1)
            assert not _gating_alarms(stack)["unacknowledged_membership"]


# --- Real-path publication timing qualification (spec 530 §11) -------------

_PUBLISH_COMMAND = b'"command":"cell_admission_publish_operator_alarm_attention"'


class _KernelRelay:
    """Same-user relay in front of the real native host's control socket.

    The attention heartbeat is published beside Stop on the control socket,
    so that is the socket the relay serves and forwards to.

    It forwards every packet unchanged and records, for each publication, when
    the request arrived and when the Kernel's acceptance came back. A
    configurable delay holds the publication before the Kernel sees it, which
    is a Kernel that answers late; abandoned client sockets are tolerated.
    """

    def __init__(self, upstream: Path, run_root: Path) -> None:
        self._upstream = upstream
        run_root.mkdir(mode=0o700)
        self.run_root = run_root
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        path = run_root / EXECUTION_CONTROL_SOCKET_FILE
        self._listener.bind(str(path))
        os.chmod(path, 0o600)
        self._listener.listen(16)
        self.delay_seconds = 0.0
        self.lock = threading.Lock()
        self.requested_at: list[float] = []
        self.accepted_at: list[float] = []
        self.in_flight = threading.Event()
        self._closed = threading.Event()
        self._forwarders: list[threading.Thread] = []
        self._thread = threading.Thread(target=self._accept, daemon=True)
        self._thread.start()

    def _accept(self) -> None:
        while True:
            try:
                connection, _address = self._listener.accept()
            except OSError:
                return
            forwarder = threading.Thread(
                target=self._forward, args=(connection,), daemon=True
            )
            with self.lock:
                self._forwarders.append(forwarder)
            forwarder.start()

    def _forward(self, connection: socket.socket) -> None:
        with connection:
            request = connection.recv(MAXIMUM_RESPONSE_BYTES)
            publication = _PUBLISH_COMMAND in request
            if publication:
                with self.lock:
                    self.requested_at.append(time.monotonic())
                    delay = self.delay_seconds
                self.in_flight.set()
                # A closed relay ends a held exchange at once; it never
                # forwards to a stack that has already been torn down.
                if self._closed.wait(delay):
                    return
            if self._closed.is_set():
                return
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as upstream:
                upstream.setsockopt(
                    socket.SOL_SOCKET, socket.SO_RCVBUF, MAXIMUM_RESPONSE_BYTES
                )
                upstream.settimeout(10.0)
                upstream.connect(str(self._upstream))
                upstream.send(request)
                response = upstream.recv(MAXIMUM_RESPONSE_BYTES)
            if publication and json.loads(response).get("ok") is True:
                with self.lock:
                    self.accepted_at.append(time.monotonic())
            try:
                connection.send(response)
            except OSError:
                pass  # the publisher abandoned this exchange at its deadline

    def mark(self) -> tuple[int, int]:
        with self.lock:
            return len(self.requested_at), len(self.accepted_at)

    def since(self, mark: tuple[int, int]) -> tuple[list[float], list[float]]:
        with self.lock:
            return self.requested_at[mark[0]:], self.accepted_at[mark[1]:]

    def close(self) -> None:
        self._closed.set()
        self._listener.close()
        self._thread.join(timeout=10.0)
        with self.lock:
            forwarders = list(self._forwarders)
        for forwarder in forwarders:
            forwarder.join(timeout=15.0)
            assert not forwarder.is_alive(), "relay exchange outlived its test"


class _TimedAttentionSource:
    """The real core, with each owner read's wall duration recorded."""

    def __init__(self, core: NativeHeadlessCore) -> None:
        self._core = core
        self.read_seconds: list[float] = []
        self.last: dict[str, Any] | None = None

    def operator_alarm_motion_attention(self) -> dict[str, Any]:
        started = time.monotonic()
        projection = self._core.operator_alarm_motion_attention()
        self.read_seconds.append(time.monotonic() - started)
        self.last = projection
        return projection


def _intervals(times: list[float]) -> list[float]:
    return [later - earlier for earlier, later in zip(times, times[1:])]


def _summary(values: list[float]) -> str:
    ordered = sorted(values)
    p99 = ordered[max(0, math.ceil(0.99 * len(ordered)) - 1)]
    return (
        f"n={len(ordered)} min={ordered[0] * 1000:.1f}ms "
        f"p99={p99 * 1000:.1f}ms max={ordered[-1] * 1000:.1f}ms"
    )


def _wait_for(predicate: Callable[[], bool], timeout_seconds: float, what: str) -> None:
    deadline = time.monotonic() + timeout_seconds
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(0.01)


@contextmanager
def _native_host_stack() -> Iterator[ProcessStack]:
    """Manager and the real native execution host, without a Headless API."""
    with TemporaryDirectory(prefix="mf-oaq-") as temporary_root:
        root = Path(temporary_root)
        bootstrap = root / "cell-bootstrap"
        shutil.copytree(_required_path("MANYFORGE_UI6I_CELL_BOOTSTRAP_ROOT"), bootstrap)
        stack_root = root / "stack"
        stack_root.mkdir()
        with _read_only_tree(bootstrap):
            stack = ProcessStack(
                stack_root,
                package_root=bootstrap,
                cell_id=CELL_ID,
                freshness_milliseconds=120_000,
            )
            try:
                stack.prepare_native_package(ADMISSION_FILENAME)
                stack.start_manager()
                admission = CellAdmissionClient(stack.execution_run_root, timeout_seconds=1.0)

                def host_ready() -> bool:
                    try:
                        admission.get()
                    except Exception:
                        return False
                    return True

                _wait_for(host_ready, 60.0, "the native execution host")
                yield stack
            except BaseException:
                stack.log.flush()
                log_path = stack_root / "process-stack.log"
                if log_path.is_file():
                    print(
                        "\n--- attention qualification process log ---\n"
                        + log_path.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                raise
            finally:
                stack.close()


def test_attention_publication_meets_its_deadline_on_the_real_core_and_kernel(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Real Headless core + real native host: P + D < F under adversity.

    The publisher reads the real core while other threads hold its
    coordination lock with source refreshes, while the Kernel answers late,
    and is shut down with one exchange outstanding.
    """
    period, deadline = PUBLICATION_PERIOD_SECONDS, PUBLICATION_DEADLINE_SECONDS
    with _native_host_stack() as stack:
        relay = _KernelRelay(
            stack.execution_run_root / EXECUTION_CONTROL_SOCKET_FILE,
            stack.root / "attention-relay-run",
        )
        core = NativeHeadlessCore(
            HeadlessApiConfig(
                projection_root=stack.publication_root,
                install_root=stack.package_root,
                artifact_root=stack.artifact_root,
                node_catalog_path=NODE_CATALOG,
                node_environment_manifest_path=NODE_ENVIRONMENT_MANIFEST,
                alarm_catalog_path=CATALOG,
                historian_database_path=stack.history_database,
                manager_run_root=stack.manager_run_root,
                execution_run_root=stack.execution_run_root,
                cell_id=CELL_ID,
                owner_id="headless-api.attention-qualification",
                freshness_milliseconds=120_000,
            )
        )
        source = _TimedAttentionSource(core)
        # Production composition: the event pump drives the alarm-source
        # refresh; the publisher reaches the Kernel through the relay.
        pump = ExecutionEventPump(
            core,
            ExecutionCommandClient(stack.execution_run_root, timeout_seconds=1.0),
            poll_seconds=0.1,
        )
        publisher = OperatorAlarmAttentionPublisher(
            source,
            CellAdmissionClient(relay.run_root, timeout_seconds=deadline),
        )
        report: list[str] = []
        load_stop = threading.Event()
        load_calls = [0]

        def core_load() -> None:
            # Each call takes the core coordination lock and runs a full
            # source refresh (RefreshLocked) under it.
            calls = (core.runtime_snapshot, core.alarm_snapshot, core.alarm_metrics)
            index = 0
            while not load_stop.is_set():
                calls[index % len(calls)]()
                index += 1
                load_calls[0] += 1

        def phase(name: str, samples: int) -> tuple[list[float], list[float]]:
            mark = relay.mark()
            reads = len(source.read_seconds)
            _wait_for(
                lambda: len(relay.since(mark)[1]) >= samples
                and len(relay.since(mark)[0]) >= samples,
                samples * period + 10.0,
                f"{samples} Kernel receipts in phase {name}",
            )
            requested, accepted = relay.since(mark)
            report.append(
                f"{name}: Kernel receipt intervals {_summary(_intervals(accepted))}; "
                f"publication start intervals {_summary(_intervals(requested))}; "
                f"owner read {_summary(source.read_seconds[reads:])}"
            )
            return requested, accepted

        loaders: list[threading.Thread] = []
        pump.start()
        try:
            publisher.start()
            # Steady state: the pump's refresh keeps the sources current.
            _wait_for(
                lambda: source.last is not None and source.last["reconciled"] is True,
                30.0,
                "a reconciled motion-attention projection",
            )
            _requested, baseline = phase("baseline", 16)
            assert max(_intervals(baseline)) < period + deadline

            loaders = [
                threading.Thread(target=core_load, name=f"core-load-{index}", daemon=True)
                for index in range(4)
            ]
            for loader in loaders:
                loader.start()
            reads = len(source.read_seconds)
            _requested, contended = phase("core-lock contention (4 refresh loops)", 40)
            contended_reads = source.read_seconds[reads:]
            assert load_calls[0] > 100
            assert max(contended_reads) < deadline
            assert max(_intervals(contended)) < period + deadline
            assert source.last is not None and source.last["reconciled"] is True

            relay.delay_seconds = deadline / 2
            _requested, delayed = phase(f"Kernel answers after {deadline / 2:.2f}s", 16)
            assert max(_intervals(delayed)) < period + deadline
            before = publisher.counts
            relay.delay_seconds = deadline + 0.15
            requested, late = phase(f"Kernel answers after {deadline + 0.15:.2f}s (> D)", 12)
            after = publisher.counts
            abandoned = after.abandoned - before.abandoned
            report.append(f"abandoned at D in the late phase: {abandoned}")
            assert abandoned >= 10
            # An abandoned publication never delays the next period's start.
            assert max(_intervals(requested)) < period + 0.1
            load_stop.set()
            for loader in loaders:
                loader.join(timeout=10.0)

            # Shutdown while one exchange is outstanding at the Kernel.
            relay.delay_seconds = 5.0
            relay.in_flight.clear()
            assert relay.in_flight.wait(timeout=5.0)
            started = time.monotonic()
            publisher.shutdown()
            shutdown_seconds = time.monotonic() - started
            report.append(f"shutdown with an outstanding exchange: {shutdown_seconds * 1000:.1f}ms")
            assert shutdown_seconds <= deadline + 0.1
        finally:
            load_stop.set()
            publisher.shutdown()
            pump.shutdown()
            core.shutdown()
            relay.close()
        counts = publisher.counts
        with capsys.disabled():
            print(
                "\nalarm attention real-path qualification "
                f"(P={period}s D={deadline}s F={KERNEL_FRESHNESS_SECONDS}s, "
                f"P+D={period + deadline:.2f}s < F):"
            )
            for line in report:
                print("  " + line)
            print(f"  outcomes: {counts}")
