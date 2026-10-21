"""First-Execute stale-generation race and the Runner's single retry.

Production interleaving (code-inferred):

1. ``start_run`` commits ``starting`` + the provisional run id through
   ``journal.begin_acquire``; the Runner liveness projection reports it.
2. ``start_run`` re-reads the owner admission generation G (fresh ``get``)
   and checkpoints it with ``journal.refresh_pending_acquire``.
3. Independently, the Manager liveness lane probes the Runner, sees
   ``starting``, builds a changed readiness snapshot and submits it; the
   Kernel advances the generation to G+1 on any snapshot change
   (``KernelCellAdmissionOwner::UpdateCellAdmissionReadiness``).
4. ``acquire`` carries ``expected_admission_generation == G`` and the
   Kernel rejects it (``stale_generation`` / ``expected_generation_stale``).
5. The Runner retries exactly once from a fresh read with the same request
   identity; any other failure, or a second stale rejection, fails as before.

The Runner talks to the Kernel through the REAL ``CellAdmissionClient`` over a
real same-user ``SOCK_SEQPACKET`` socket. The Kernel side is a faithful Python
mirror of the C++ owner rules used here (generation bump on readiness change,
expected-generation check, ``Reject`` error shape). The Manager publication
reads the Runner's REAL liveness probe projection.
"""

from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import shutil
import socket
import tempfile
from threading import Lock, Thread
from typing import Any, Callable

import pytest
import rfc8785

from manyforge_headless_api.cell_admission import (
    EXECUTION_COMMAND_VERSION,
    EXECUTION_RESPONSE_VERSION,
    EXECUTION_SOCKET_FILE,
    CellAdmissionClient,
    CellAdmissionError,
)
from manyforge_program_runner.app import _status
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.liveness import (
    API_VERSION as LIVENESS_API_VERSION,
    ProgramRunnerLivenessReceiver,
    RetainedLiveness,
)
from manyforge_headless_api.models import ProgramRunStopRequest
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.tests.test_runtime import (
    _Headless,
    _Host,
    _ObservationOwner,
    _PreparedRunAdmission,
    _start_request,
)


_MAX_GENERATION = 9_007_199_254_740_991


class _KernelAdmissionOwner:
    """Python mirror of the KernelCellAdmissionOwner rules this race uses.

    * ``update_readiness``: ``if (readiness_ != snapshot) { generation_ + 1 }``
      (kernel_cell_admission_owner.cpp UpdateCellAdmissionReadiness).
    * ``acquire``: exact replay first, then
      ``intent.expected_admission_generation != generation_`` ->
      ``Reject("stale_generation", "expected_generation_stale")`` whose
      ErrorDTO is ``{code, "cell-admission request rejected",
      {"reason": ...}}`` (Reject helper, same file).
    """

    def __init__(self, *, generation: int) -> None:
        self.lock = Lock()
        self.generation = generation
        self.readiness: dict[str, Any] | None = None
        self.claims: list[dict[str, Any]] = []
        self.acquire_attempts: list[dict[str, Any]] = []
        self.generation_advances: list[tuple[int, int]] = []

    def update_readiness(self, snapshot: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            if self.readiness != snapshot:
                assert self.generation < _MAX_GENERATION
                self.generation_advances.append((self.generation, self.generation + 1))
                self.readiness = deepcopy(snapshot)
                self.generation += 1
            return self._admission()

    def _admission(self) -> dict[str, Any]:
        return {"admission_generation": self.generation, "authoring_allowed": True}

    def get(self) -> dict[str, Any]:
        with self.lock:
            return self._admission()

    def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            self.acquire_attempts.append(deepcopy(intent))
            for claim in self.claims:
                prior = claim["intent"]
                if (
                    prior["owner_epoch"] == intent["owner_epoch"]
                    and prior["request_id"] == intent["request_id"]
                ):
                    if prior != intent:
                        raise _Rejected("request_id_conflict", "request_reuse_conflict")
                    return deepcopy(claim)
            if (
                "program_node_evidence" not in intent
                and intent["expected_admission_generation"] != self.generation
            ):
                raise _Rejected("stale_generation", "expected_generation_stale")
            claim = {
                "accepted_at_ns": 1,
                "acquired_admission_generation": self.generation,
                "claim_generation": len(self.claims) + 1,
                "intent": deepcopy(intent),
            }
            self.claims.append(claim)
            return deepcopy(claim)

    def recovery_receipt(self, _query: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            return {
                "admission_generation": self.generation,
                "current_owner_epochs": [],
                "held_claims": [],
                "kernel_owner_epoch": "kernel.epoch.1",
                "matched_claim": None,
                "matched_reconcile_settlement": None,
                "matched_release": None,
                "status": "unknown",
            }


class _Rejected(Exception):
    def __init__(self, code: str, reason: str) -> None:
        super().__init__(code)
        self.error = {
            "code": code,
            "details": {"reason": reason},
            "message": "cell-admission request rejected",
        }


class _KernelExecutionSocket:
    """Real ``execution-command.sock`` server speaking the native envelope."""

    def __init__(
        self,
        run_root: Path,
        owner: _KernelAdmissionOwner,
        *,
        on_get: Callable[[int, Callable[[], dict[str, Any]]], dict[str, Any]],
        on_acquire: Callable[[int], str | None] | None = None,
    ) -> None:
        self.owner = owner
        self.on_get = on_get
        self.on_acquire = on_acquire
        self.get_count = 0
        self.acquire_count = 0
        self.commands: list[str] = []
        self.path = run_root / EXECUTION_SOCKET_FILE
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind(str(self.path))
        os.chmod(self.path, 0o600)
        self.listener.listen(8)
        self.listener.settimeout(0.05)
        self.stopped = False
        self.failure: BaseException | None = None
        self.thread = Thread(target=self._serve, daemon=True)
        self.thread.start()

    def close(self) -> None:
        self.stopped = True
        self.thread.join(5)
        self.listener.close()

    def _serve(self) -> None:
        while not self.stopped:
            try:
                connection, _ = self.listener.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(5)
                try:
                    packet = json.loads(connection.recv(1024 * 1024))
                    response = self._handle(packet)
                    connection.send(rfc8785.dumps(response))
                except BaseException as error:  # surfaced by the test
                    self.failure = error
                    raise

    def _handle(self, packet: dict[str, Any]) -> dict[str, Any]:
        assert packet["api_version"] == EXECUTION_COMMAND_VERSION
        command = packet["command"]
        self.commands.append(command)
        envelope = {"api_version": EXECUTION_RESPONSE_VERSION, "command": command}
        try:
            if command == "cell_admission_get":
                self.get_count += 1
                result_key, result = "authoring_admission", self.on_get(
                    self.get_count, self.owner.get
                )
            elif command == "cell_admission_acquire":
                self.acquire_count += 1
                if self.on_acquire is not None:
                    # May raise ``_Rejected``; ``"malformed"`` loses the result.
                    if self.on_acquire(self.acquire_count) == "malformed":
                        self.owner.acquire_attempts.append(deepcopy(packet["request"]))
                        return {**envelope, "ok": True}
                result_key, result = "claim", self.owner.acquire(packet["request"])
            elif command == "cell_admission_recovery_receipt":
                result_key, result = "recovery_receipt", self.owner.recovery_receipt(
                    packet["request"]
                )
            else:
                raise AssertionError(f"unexpected native command {command}")
        except _Rejected as rejected:
            return {**envelope, "error": rejected.error, "ok": False}
        return {**envelope, "ok": True, result_key: result}


class _Manager:
    """Manager liveness lane: probe the Runner, publish the readiness reduction."""

    def __init__(self, runtime: ProgramRuntime, owner: _KernelAdmissionOwner) -> None:
        self.owner = owner
        # The real Runner-side receiver projection the Manager probes; it is
        # never started, only its probe handler is exercised.
        self.receiver = ProgramRunnerLivenessReceiver(
            Path("/nonexistent/runner-liveness.sock"),
            cell_id="cell",
            receiver_runtime_unit_id=runtime.owner_id,
            receiver_runtime_epoch=runtime.owner_epoch,
            retained=RetainedLiveness(None, None, False, None),
            on_expiry=lambda reference: reference,
            on_reconciliation=lambda *_args: None,
            cycle_status=runtime._current_cycle_status_projection,  # noqa: SLF001
        )
        self.published: list[dict[str, Any]] = []

    def heartbeat(self, **other_readiness: Any) -> dict[str, Any]:
        probe = self.receiver._handle(  # noqa: SLF001
            {"api_version": LIVENESS_API_VERSION, "cell_id": "cell", "kind": "probe"}
        )
        assert probe["kind"] == "probe_ack"
        # The Program-cycle part of the readiness reduction is probed; any
        # other readiness input is held constant unless a test supplies it.
        snapshot = {
            "program_cycle_status": deepcopy(probe["program_cycle_status"]),
            **other_readiness,
        }
        self.published.append(snapshot)
        return self.owner.update_readiness(snapshot)


@pytest.fixture
def run_root():
    # AF_UNIX paths are length-bounded; keep the pinned run root short.
    root = Path(tempfile.mkdtemp(prefix="mf-sg-", dir="/tmp")).resolve()
    os.chmod(root, 0o700)
    try:
        yield root
    finally:
        shutil.rmtree(root, ignore_errors=True)


def _fixture(
    monkeypatch,
    tmp_path,
    run_root,
    *,
    inject: dict[int, str] | None = None,
    on_acquire: Callable[[int], str | None] | None = None,
    after_rejected_acquire: Callable[[Any], None] | None = None,
):
    """Build a Runner wired to the real client, a mirrored Kernel and Manager.

    ``inject`` maps a ``get`` index to where the Manager readiness publication
    lands: get 1 is the pre-``begin_acquire`` read, get 2 the fresh read, get 3
    the retry read. ``"after_read"`` lets that read return G and then publishes
    (the race); ``"before_read"`` publishes first (absorbed). Each publication
    also changes one unrelated readiness input so repeated ``starting``
    publications still advance the generation. ``after_rejected_acquire`` runs
    in the Runner thread after a rejected first acquire (Stop/readiness).
    """

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    inject = inject or {}
    owner = _KernelAdmissionOwner(generation=1)
    holder: dict[str, Any] = {}
    observed_at_publication: list[dict[str, Any]] = []

    def publish() -> None:
        cycle = holder["journal"].read()
        observed_at_publication.append(
            {"state": cycle.state, "program_run_id": cycle.program_run_id}
        )
        holder["manager"].heartbeat(other_readiness_revision=len(observed_at_publication))

    def on_get(index: int, read: Callable[[], dict[str, Any]]) -> dict[str, Any]:
        where = inject.get(index)
        if where is None:
            return read()
        if where == "before_read":
            publish()
            return read()
        assert where == "after_read"
        admission = read()  # the Runner will receive G ...
        publish()  # ... while the Kernel is already at G+1
        return admission

    kernel = _KernelExecutionSocket(run_root, owner, on_get=on_get, on_acquire=on_acquire)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.sg.1")
    headless = _Headless()
    execution = _Headless()
    client = CellAdmissionClient(run_root)
    refreshed: list[int] = []
    if after_rejected_acquire is not None:
        acquire = client.acquire

        def acquire_then_hook(intent):
            try:
                return acquire(intent)
            except CellAdmissionError:
                if len(owner.acquire_attempts) == 1:
                    after_rejected_acquire(holder["runtime"])
                raise

        client.acquire = acquire_then_hook  # type: ignore[method-assign]
    refresh = journal.refresh_pending_acquire

    def recording_refresh(**kwargs):
        refreshed.append(kwargs["evidence"]["expected_admission_generation"])
        return refresh(**kwargs)

    journal.refresh_pending_acquire = recording_refresh  # type: ignore[method-assign]
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.sg.1"),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=execution,  # type: ignore[arg-type]
        admission_client=client,
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    observation_owner = _ObservationOwner()
    runtime._owner = observation_owner  # noqa: SLF001
    runtime._host = _Host(  # noqa: SLF001
        observation_owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch
    )
    manager = _Manager(runtime, owner)
    holder.update(journal=journal, manager=manager, runtime=runtime)
    # Manager has already published the idle cycle before the operator Run.
    manager.heartbeat()
    assert manager.published[-1]["program_cycle_status"]["state"] == "idle_after_reset"
    return SimpleFixture(
        runtime=runtime,
        journal=journal,
        owner=owner,
        kernel=kernel,
        manager=manager,
        headless=headless,
        execution=execution,
        host_owner=observation_owner,
        observed_at_publication=observed_at_publication,
        refreshed=refreshed,
    )


class SimpleFixture:
    def __init__(self, **values: Any) -> None:
        self.__dict__.update(values)


def _assert_no_effect_idle(f: SimpleFixture) -> None:
    assert f.owner.claims == []
    assert f.execution.registered_captures == []
    cycle = f.journal.read()
    assert cycle.state == "idle_after_reset"
    assert cycle.program_run_id is None
    assert cycle.claim is None
    assert cycle.acquisition_pending is None
    assert cycle.cycle_generation == 1


_GET, _ACQUIRE = "cell_admission_get", "cell_admission_acquire"


def test_publication_between_read_and_acquire_is_retried_once_at_new_generation(
    monkeypatch: pytest.MonkeyPatch, tmp_path, run_root
) -> None:
    """The race: the Manager publishes ``starting`` between the fresh ``get``
    and ``acquire``; the first claim is rejected stale, the single retry
    acquires at G+1 with the same request identity."""

    f = _fixture(monkeypatch, tmp_path, run_root, inject={2: "after_read"})
    generation_g = f.owner.generation
    f.runtime.start()
    try:
        f.runtime.start_run(_start_request())
        assert f.kernel.failure is None

        # The publication saw exactly what begin_acquire committed.
        assert f.observed_at_publication == [
            {"state": "starting", "program_run_id": "program-cycle.1"}
        ]
        assert f.owner.generation_advances[-1] == (generation_g, generation_g + 1)

        assert f.kernel.commands[:5] == [_GET, _GET, _ACQUIRE, _GET, _ACQUIRE]
        first, second = f.owner.acquire_attempts
        assert first["expected_admission_generation"] == generation_g
        assert second["expected_admission_generation"] == generation_g + 1
        assert first["request_id"] == second["request_id"] == "request.start.1"
        assert first["owner_epoch"] == second["owner_epoch"]
        assert first["owner_transaction_id"] == second["owner_transaction_id"]
        assert {k: v for k, v in first.items() if k != "expected_admission_generation"} == {
            k: v for k, v in second.items() if k != "expected_admission_generation"
        }
        # The pending-acquire checkpoint was refreshed to G+1 before the retry.
        assert f.refreshed == [generation_g, generation_g + 1]

        assert len(f.owner.claims) == 1
        assert f.owner.claims[0]["acquired_admission_generation"] == generation_g + 1
        cycle = f.journal.read()
        assert cycle.state == "running"
        assert cycle.claim is not None
        assert cycle.claim["intent"]["expected_admission_generation"] == generation_g + 1
        assert len(f.execution.registered_captures) == 1
    finally:
        f.runtime.shutdown()
        f.kernel.close()


def test_second_stale_rejection_fails_like_before_without_further_retry(
    monkeypatch: pytest.MonkeyPatch, tmp_path, run_root
) -> None:
    f = _fixture(
        monkeypatch, tmp_path, run_root, inject={2: "after_read", 3: "after_read"}
    )
    generation_g = f.owner.generation
    f.runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as rejected:
            f.runtime.start_run(_start_request())
        assert f.kernel.failure is None
        assert f.kernel.commands == [_GET, _GET, _ACQUIRE, _GET, _ACQUIRE]
        assert [a["expected_admission_generation"] for a in f.owner.acquire_attempts] == [
            generation_g,
            generation_g + 1,
        ]
        assert f.owner.generation == generation_g + 2
        error = rejected.value
        assert error.code == "stale_generation"
        assert str(error) == "cell-admission request rejected"
        assert error.details == {"reason": "expected_generation_stale"}
        assert _status(error.code) == 409
        _assert_no_effect_idle(f)
    finally:
        f.runtime.shutdown()
        f.kernel.close()


def test_stop_won_between_attempts_supersedes_without_second_acquire(
    monkeypatch: pytest.MonkeyPatch, tmp_path, run_root
) -> None:
    def stop(runtime: ProgramRuntime) -> None:
        request = ProgramRunStopRequest.model_validate(
            {
                "request_id": "stop.between-attempts",
                "request_owner_epoch": "composer.epoch.1",
                "source_label": "operator",
                "correlation_id": None,
                "program_run_id": "program-cycle.1",
                "expected_owner_epoch": runtime.owner_epoch,
                "reason": "operator Stop during Run admission",
            }
        )
        assert runtime.stop_run("program-cycle.1", request)["state"] == "stopping"

    f = _fixture(
        monkeypatch,
        tmp_path,
        run_root,
        inject={2: "after_read"},
        after_rejected_acquire=stop,
    )
    f.runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as superseded:
            f.runtime.start_run(_start_request())
        assert f.kernel.failure is None
        # The existing no-claim projection of a Stop that won admission.
        assert superseded.value.code == "program_run_stopped_before_start"
        assert superseded.value.details["cause_code"] == "program_run_superseded_by_stop"
        assert superseded.value.details["no_effect"] is True
        assert f.journal.read().state == "terminal_awaiting_reset"
        assert f.journal.read().claim is None
        assert f.kernel.commands == [_GET, _GET, _ACQUIRE]
        assert len(f.owner.acquire_attempts) == 1
        assert f.owner.claims == []
        assert f.execution.registered_captures == []
    finally:
        f.runtime.shutdown()
        f.kernel.close()


def test_required_component_not_active_between_attempts_refuses_as_before(
    monkeypatch: pytest.MonkeyPatch, tmp_path, run_root
) -> None:
    def disable(runtime: ProgramRuntime) -> None:
        headless = runtime._headless  # noqa: SLF001
        active = headless.cclc_component_lifecycle_snapshot

        def disabled(component_id: str) -> dict[str, Any]:
            return {**active(component_id), "component_lifecycle_state": "disabled"}

        headless.cclc_component_lifecycle_snapshot = disabled

    f = _fixture(
        monkeypatch,
        tmp_path,
        run_root,
        inject={2: "after_read"},
        after_rejected_acquire=disable,
    )
    f.runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            f.runtime.start_run(_start_request())
        assert f.kernel.failure is None
        assert refused.value.code == "program_required_component_not_active"
        assert f.kernel.commands == [_GET, _GET, _ACQUIRE]
        assert len(f.owner.acquire_attempts) == 1
        _assert_no_effect_idle(f)
    finally:
        f.runtime.shutdown()
        f.kernel.close()


@pytest.mark.parametrize(
    ("first_acquire", "expected_code", "expected_state"),
    [
        ("malformed", "execution_command_response_invalid", "recovery_required"),
        (("authoring_not_ready", "route_recovery_pending"), "authoring_not_ready", "idle_after_reset"),
        (("stale_generation", "other_reason"), "stale_generation", "idle_after_reset"),
    ],
)
def test_unknown_outcome_or_other_rejection_is_not_retried(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    run_root,
    first_acquire,
    expected_code: str,
    expected_state: str,
) -> None:
    def on_acquire(index: int) -> str | None:
        assert index == 1, "no retry expected"
        if first_acquire == "malformed":
            return "malformed"
        raise _Rejected(*first_acquire)

    f = _fixture(monkeypatch, tmp_path, run_root, on_acquire=on_acquire)
    f.runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as failed:
            f.runtime.start_run(_start_request())
        assert f.kernel.failure is None
        assert failed.value.code == expected_code
        assert f.kernel.commands == [_GET, _GET, _ACQUIRE]
        assert f.kernel.acquire_count == 1
        assert f.owner.claims == []
        cycle = f.journal.read()
        assert cycle.state == expected_state
        if expected_state == "idle_after_reset":
            _assert_no_effect_idle(f)
        else:
            assert cycle.claim is None
    finally:
        f.runtime.shutdown()
        f.kernel.close()


@pytest.mark.parametrize("inject", [None, "before_read"])
def test_first_execute_without_publication_between_read_and_acquire_succeeds(
    monkeypatch: pytest.MonkeyPatch, tmp_path, run_root, inject: str | None
) -> None:
    """Control: no publication in the get->acquire gap (none at all, or one
    that lands before the fresh read and is absorbed) -> one acquire."""

    f = _fixture(
        monkeypatch, tmp_path, run_root, inject={2: inject} if inject else None
    )
    generation_before = f.owner.generation
    f.runtime.start()
    try:
        f.runtime.start_run(_start_request())
        assert f.kernel.failure is None
        assert f.kernel.commands[:3] == [_GET, _GET, _ACQUIRE]
        assert f.kernel.commands.count(_ACQUIRE) == 1
        expected = generation_before + (1 if inject == "before_read" else 0)
        assert len(f.owner.claims) == 1
        assert f.owner.claims[0]["intent"]["expected_admission_generation"] == expected
        assert f.owner.claims[0]["acquired_admission_generation"] == expected
        if inject == "before_read":
            assert f.observed_at_publication == [
                {"state": "starting", "program_run_id": "program-cycle.1"}
            ]
        assert f.journal.read().state == "running"
        assert f.journal.read().claim is not None
    finally:
        f.runtime.shutdown()
        f.kernel.close()
