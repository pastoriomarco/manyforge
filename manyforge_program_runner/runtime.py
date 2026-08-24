"""Independent Program orchestration owner assembled from focused clients."""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import dataclass
import hashlib
from pathlib import Path
from threading import Event, RLock
from typing import Any, Final, NoReturn
import unicodedata
from urllib.parse import urlsplit
from uuid import uuid4

import rfc8785

from manyforge_behavior.assistant_program_port import (
    HeadlessProgramSceneSource,
    ProgramAssistantPort,
    ProgramModelTransport,
    ProgramSnapshotSource,
)
from manyforge_behavior.cclc_program_port import CCLCProgramTaskPort
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.node_availability import AvailabilityFact
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    NodeAdapterPorts,
    OwnerTaskPort,
)
from manyforge_behavior.production_ports import (
    HeadlessSceneProgramPort,
    NativeKernelProgramPort,
)
from manyforge_behavior.program_run_owner import (
    ProgramRunOwner,
    ProgramRunOwnerError,
    build_applied_program_run_admission,
)
from manyforge_behavior.program_scheduling import (
    ProgramInterventionRegistry,
    ProgramRunTickHost,
    ProgramSchedulingError,
)
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.models import (
    ProgramRunStartRequest,
    ProgramRunStopRequest,
)
from manyforge_headless_api.program_model_transport import (
    HeadlessProgramModelAuditSink,
)

from manyforge_headless_api.cell_admission import (
    CellAdmissionClient,
    CellAdmissionError,
    cell_admission_owner_epoch_reconciliation_evidence_digest,
)
from .cycle_journal import ProgramCycleJournal
from .liveness import (
    ProgramRunnerLivenessError,
    ProgramRunnerLivenessReceiver,
)
from .models import ProgramCycleResetRequest, ProgramCycleStatusDTO


PROGRAM_RUN_INDEX_VERSION: Final = "manyforge.program_run_index.v1"
_MAX_REPLAY_RECORDS: Final = 4_096


class ProgramRunnerError(RuntimeError):
    """Stable failure from the isolated Program-runner owner."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class ProgramRunnerConfig:
    headless_url: str
    execution_state_root: Path | None = None
    cycle_journal_path: Path | None = None
    owner_id: str = "program-runner"
    owner_epoch: str | None = None
    headless_timeout_seconds: float = 30.0
    tick_interval_seconds: float = 0.01
    tick_workers: int = 4
    maximum_retained_runs: int = 4_096
    maximum_replay_records: int = _MAX_REPLAY_RECORDS
    manager_liveness_socket: Path | None = None
    cell_id: str | None = None

    def __post_init__(self) -> None:
        parsed = urlsplit(self.headless_url)
        if (
            parsed.scheme not in {"http", "https"}
            or not parsed.netloc
            or parsed.username is not None
            or parsed.password is not None
            or parsed.path not in {"", "/"}
            or bool(parsed.query)
            or bool(parsed.fragment)
        ):
            raise ValueError(
                "headless_url must be an absolute credential-free HTTP(S) URL"
            )
        for field_name in ("owner_id",):
            value = getattr(self, field_name)
            if (
                not isinstance(value, str)
                or not value
                or unicodedata.normalize("NFC", value) != value
                or len(value.encode("utf-8")) > 256
            ):
                raise ValueError(f"{field_name} must be bounded non-empty text")
        for field_name in ("owner_epoch",):
            value = getattr(self, field_name)
            if value is not None and (
                not value
                or unicodedata.normalize("NFC", value) != value
                or len(value.encode("utf-8")) > 256
            ):
                raise ValueError(f"{field_name} must be bounded non-empty text")
        if not 0 < self.headless_timeout_seconds <= 60:
            raise ValueError("headless_timeout_seconds must be in (0, 60]")
        if not 0.001 <= self.tick_interval_seconds <= 1.0:
            raise ValueError("tick_interval_seconds must be in [0.001, 1]")
        if not 1 <= self.tick_workers <= 32:
            raise ValueError("tick_workers must be in [1, 32]")
        for field_name in ("maximum_retained_runs", "maximum_replay_records"):
            value = getattr(self, field_name)
            if type(value) is not int or not 1 <= value <= 4_096:
                raise ValueError(f"{field_name} must be in [1, 4096]")
        enabled = self.execution_state_root is not None
        if enabled != (self.cycle_journal_path is not None):
            raise ValueError(
                "execution-state root and cycle journal must be configured together"
            )
        if enabled and (
            not self.execution_state_root.is_absolute()
            or not self.cycle_journal_path.is_absolute()
        ):
            raise ValueError(
                "execution-state root and cycle journal must be stable absolute paths"
            )
        liveness_enabled = self.manager_liveness_socket is not None
        if liveness_enabled != (self.cell_id is not None):
            raise ValueError(
                "Manager liveness socket and cell identity must be configured together"
            )
        if liveness_enabled and not self.manager_liveness_socket.is_absolute():
            raise ValueError("Manager liveness socket must be a stable absolute path")


@dataclass(slots=True)
class _EmergencyStopRecord:
    intent: dict[str, Any]
    settled: Event
    result: dict[str, Any] | None = None
    error: ProgramRunnerError | None = None


class ProgramRuntime:
    """Own Program lifecycle only; every effect remains in a focused owner."""

    def __init__(
        self,
        config: ProgramRunnerConfig,
        *,
        headless_client: HeadlessHttpClient | None = None,
        model_transport: ProgramModelTransport | None = None,
        snapshot_source: ProgramSnapshotSource | None = None,
        simulation_control: OwnerTaskPort | None = None,
        admission_client: CellAdmissionClient | None = None,
        cycle_journal: ProgramCycleJournal | None = None,
    ) -> None:
        if not isinstance(config, ProgramRunnerConfig):
            raise TypeError("Program runtime requires one ProgramRunnerConfig")
        if snapshot_source is not None and not callable(
            getattr(snapshot_source, "capture_snapshot", None)
        ):
            raise TypeError("snapshot_source must capture bounded snapshots")
        if snapshot_source is not None and model_transport is None:
            raise ValueError("snapshot_source requires a Program model transport")
        self.config = config
        self.owner_id = config.owner_id
        self.owner_epoch = config.owner_epoch or f"program-runner-{uuid4().hex}"
        self._admission = admission_client or (
            None
            if config.execution_state_root is None
            else CellAdmissionClient(
                config.execution_state_root,
                timeout_seconds=min(config.headless_timeout_seconds, 30.0),
            )
        )
        self._journal = cycle_journal or (
            None
            if config.cycle_journal_path is None
            else ProgramCycleJournal(
                config.cycle_journal_path,
                owner_epoch=self.owner_epoch,
                maximum_requests=config.maximum_replay_records,
                maximum_retained_runs=config.maximum_retained_runs,
            )
        )
        self._liveness: ProgramRunnerLivenessReceiver | None = None
        if config.manager_liveness_socket is not None:
            if self._journal is None:
                raise ValueError("Manager liveness requires the durable cycle journal")
            self._liveness = ProgramRunnerLivenessReceiver(
                config.manager_liveness_socket,
                cell_id=config.cell_id or "",
                receiver_runtime_unit_id=self.owner_id,
                receiver_runtime_epoch=self.owner_epoch,
                retained=self._journal.liveness(),
                on_expiry=self._contain_manager_loss,
                on_reconciliation=self._reconcile_manager_liveness,
                cycle_status=self._current_cycle_status_projection,
            )
        self._owns_headless_client = headless_client is None
        self._headless = headless_client or HeadlessHttpClient(
            config.headless_url,
            timeout_seconds=config.headless_timeout_seconds,
        )
        self._model_transport = model_transport
        self._snapshot_source = snapshot_source
        if simulation_control is not None and any(
            not callable(getattr(simulation_control, method, None))
            for method in (
                "acknowledge",
                "bind",
                "cancel",
                "close",
                "observe",
                "start",
                "unbind",
            )
        ):
            raise TypeError(
                "simulation_control must implement task, context, and close ports"
            )
        self._simulation_control = simulation_control
        self._environment = build_node_environment()
        self._native = NativeKernelProgramPort(KernelClient(self._headless))
        self._scene = HeadlessSceneProgramPort(self._headless)
        self._cclc = CCLCProgramTaskPort(
            self._headless,
            maximum_retained_tasks=config.maximum_retained_runs,
        )
        self._assistant = (
            None
            if model_transport is None
            else ProgramAssistantPort(
                model_transport,
                HeadlessProgramSceneSource(self._headless),
                audit_sink=HeadlessProgramModelAuditSink(self._headless),
                snapshot_source=snapshot_source,
            )
        )
        context_members = [self._native, self._scene, self._cclc]
        if self._assistant is not None:
            context_members.append(self._assistant)
        if self._simulation_control is not None:
            context_members.append(self._simulation_control)  # type: ignore[arg-type]
        self._interventions = ProgramInterventionRegistry(
            maximum_retained_runs=config.maximum_retained_runs
        )
        self._ports = NodeAdapterPorts(
            kernel_operations=self._native.operations,
            scene_transactions=self._scene,
            assistant_requests=self._assistant,
            pick_place=self._native.pick_place,
            simulator_control=self._simulation_control,
            cclc_lifecycle=self._cclc,
            cclc_routes=self._cclc,
            program_run_scope=self._native.scope,
            program_run_context=CompositeProgramRunContextPort(*context_members),
            program_run_initialization=self._scene,
            program_interventions=self._interventions,
        )
        self._owner_availability = self._build_owner_availability()
        self._owner = ProgramRunOwner(
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            ports=self._ports,
            maximum_retained_runs=config.maximum_retained_runs,
        )
        self._terminal_observation_lock = RLock()
        self._pending_terminal_snapshots: dict[str, dict[str, Any]] = {}
        self._host = ProgramRunTickHost(
            self._owner,
            tick_interval_s=config.tick_interval_seconds,
            maximum_scheduled_runs=config.maximum_retained_runs,
            maximum_workers=config.tick_workers,
            terminal_observer=self._record_terminal_snapshot,
        )
        # Serialize ordinary Start/Reset lifecycle admission and journal writes.
        # Stop never takes this lock.
        self._transition_lock = RLock()
        # Stop correlation is deliberately independent of the ordinary durable
        # request ledger and lifecycle mutex. Full/unavailable ordinary state
        # can therefore never reject recognized containment.
        self._stop_replay_lock = RLock()
        self._stop_replay: dict[tuple[str, str], _EmergencyStopRecord] = {}
        self._started = False
        self._closed = False

    def _require_manager_current(self) -> None:
        if self._liveness is None:
            return
        try:
            self._liveness.require_current()
        except ProgramRunnerLivenessError as error:
            raise ProgramRunnerError(
                "program_runner_manager_liveness_unavailable", str(error)
            ) from error

    def _current_cycle_status_projection(self) -> dict[str, Any]:
        """Project durable cycle facts without claiming current ownership.

        The private liveness probe must remain available while a replacement
        Runner is fenced, otherwise Manager cannot identify the receiver and
        deliver the reconciliation heartbeat.  Manager treats this probe as
        preflight only and publishes readiness after the heartbeat returns a
        stable current status.
        """
        return self.cycle_status()

    def _reconcile_manager_liveness(
        self,
        evidence_digest: str,
        manager_runtime_epoch: str,
        manager_owner_epoch: str,
    ) -> None:
        journal = self._journal
        admission = self._admission
        if journal is None or admission is None:
            raise RuntimeError("Program-cycle recovery owner is unavailable")
        with self._transition_lock:
            # The receiver has already validated the Manager's exact
            # reconciliation digest. Kernel owner rotation has its own
            # domain-separated evidence.
            del evidence_digest
            retained = journal.liveness()
            liveness_status = (
                None if self._liveness is None else self._liveness.status()
            )
            fault_reference = (
                None
                if liveness_status is None
                else liveness_status.get("fault_reference")
            )
            if (
                not retained.expired
                and isinstance(fault_reference, str)
                and fault_reference
            ):
                cycle = journal.read()
                journal.retain_liveness_loss(
                    manager_runtime_epoch=(
                        retained.manager_runtime_epoch or "unknown-manager"
                    ),
                    manager_owner_epoch=retained.manager_owner_epoch,
                    fault_reference=fault_reference,
                    expected_generation=cycle.cycle_generation,
                )
            cycle = journal.read()
            if cycle.state not in {
                "idle_after_reset",
                "terminal_awaiting_reset",
            }:
                raise RuntimeError(
                    "active Program cycle is not contained for reconciliation"
                )
            if cycle.acquisition_pending is not None:
                raise RuntimeError(
                    "unknown Program-cycle acquisition is unresolved"
                )

            if cycle.claim is None:
                receipt = admission.recovery_inventory(
                    owner_epoch=self.owner_epoch
                )
            else:
                receipt = admission.recovery_receipt(
                    {
                        "intent": cycle.claim["intent"],
                        "expected_claim": cycle.claim,
                        "expected_release": cycle.release_intent,
                    }
                )
            held = receipt.get("held_claims")
            owners = receipt.get("current_owner_epochs")
            if (
                not isinstance(receipt.get("kernel_owner_epoch"), str)
                or not isinstance(held, list)
                or not isinstance(owners, list)
            ):
                raise RuntimeError("Kernel owner recovery inventory is invalid")
            program_owners = [
                owner
                for owner in owners
                if isinstance(owner, dict)
                and owner.get("owner_role") == "program_runner"
            ]
            held_program_claims = [
                claim
                for claim in held
                if isinstance(claim, dict)
                and isinstance(claim.get("intent"), dict)
                and claim["intent"].get("owner_role") == "program_runner"
            ]
            if not program_owners:
                if (
                    held_program_claims
                    or cycle.owner_epoch != self.owner_epoch
                    or cycle.claim is not None
                    or cycle.state != "idle_after_reset"
                    or cycle.reconciliation_required
                ):
                    raise RuntimeError(
                        "absent Program Runner owner is not a clean bootstrap"
                    )
            elif len(program_owners) != 1:
                raise RuntimeError("Kernel Program Runner owner is not exact")
            elif program_owners[0].get("owner_epoch") == self.owner_epoch:
                if cycle.owner_epoch != self.owner_epoch:
                    raise RuntimeError(
                        "local Program Runner successor is not durable"
                    )
                journal.reconcile_liveness(
                    owner_epoch=self.owner_epoch,
                    evidence_digest=(
                        cell_admission_owner_epoch_reconciliation_evidence_digest(
                            "program_runner", self.owner_epoch
                        )
                    ),
                    require_terminal_release=False,
                )
            else:
                predecessor = program_owners[0].get("owner_epoch")
                if (
                    not isinstance(predecessor, str)
                    or not predecessor
                    or cycle.owner_epoch not in {predecessor, self.owner_epoch}
                    or held_program_claims
                ):
                    raise RuntimeError(
                        "predecessor Program Runner ownership is unresolved"
                    )
                if cycle.state == "terminal_awaiting_reset":
                    if (
                        cycle.claim is None
                        or cycle.release_intent is None
                        or receipt.get("status") != "released_exact"
                        or receipt.get("matched_claim") != cycle.claim
                        or receipt.get("matched_release")
                        != cycle.release_intent
                    ):
                        raise RuntimeError(
                            "terminal predecessor release is not exactly recovered"
                        )
                elif cycle.claim is not None:
                    raise RuntimeError(
                        "idle predecessor retains a Program-cycle claim"
                    )
                reconciliation_digest = (
                    cell_admission_owner_epoch_reconciliation_evidence_digest(
                        "program_runner", self.owner_epoch
                    )
                )
                journal.reconcile_liveness(
                    owner_epoch=self.owner_epoch,
                    evidence_digest=reconciliation_digest,
                )
                successor_owners = [
                    (
                        {
                            "owner_role": "program_runner",
                            "owner_epoch": self.owner_epoch,
                        }
                        if owner.get("owner_role") == "program_runner"
                        else owner
                    )
                    for owner in owners
                ]
                admission.reconcile(
                    {
                        "kernel_owner_epoch": receipt["kernel_owner_epoch"],
                        "retained_claims": held,
                        "current_owner_epochs": successor_owners,
                        "reconciliation_evidence_digest": (
                            reconciliation_digest
                        ),
                    }
                )
            journal.retain_liveness_manager(
                manager_runtime_epoch=manager_runtime_epoch,
                manager_owner_epoch=manager_owner_epoch,
            )

    def _contain_manager_loss(self, fault_reference: str) -> str:
        """Fence first, durably latch loss, then settle the current run."""
        journal = self._journal
        if journal is None:
            return fault_reference
        with self._transition_lock:
            try:
                cycle = journal.read()
            except Exception:
                cycle = None
            try:
                retained = journal.liveness()
                if cycle is not None:
                    journal.retain_liveness_loss(
                        manager_runtime_epoch=(
                            retained.manager_runtime_epoch or "unknown-manager"
                        ),
                        manager_owner_epoch=retained.manager_owner_epoch,
                        fault_reference=fault_reference,
                        expected_generation=cycle.cycle_generation,
                    )
            except Exception:
                # Durable-state failure must not block the independent Stop
                # path.  The receiver remains fenced and reconciliation cannot
                # be proven until the journal is available again.
                pass
            run_ids: list[str] = []
            if (
                cycle is not None
                and cycle.program_run_id is not None
                and cycle.state
                not in {"idle_after_reset", "terminal_awaiting_reset"}
            ):
                run_ids.append(cycle.program_run_id)
            if not run_ids:
                try:
                    snapshots, _truncated = self._owner.list_snapshots(
                        active_only=True,
                        limit=self.config.maximum_retained_runs,
                    )
                    run_ids.extend(
                        snapshot.capture.program_run_id for snapshot in snapshots
                    )
                except Exception:
                    pass
            for program_run_id in dict.fromkeys(run_ids):
                try:
                    snapshot = self._host.stop_run(
                        program_run_id,
                        reason="manager_presence_expired",
                    )
                    self._record_terminal_snapshot(snapshot.to_dict())
                except Exception:
                    # Reconciliation stays latched and Reset remains
                    # unavailable whenever containment cannot be proven.
                    pass
        return fault_reference

    def _build_owner_availability(self) -> dict[str, AvailabilityFact]:
        resolver = build_executable_node_resolver(self._environment)
        expected = sorted(
            {
                row.owner_port_name
                for row in resolver.dispositions
                if row.owner_port_name is not None
            }
        )
        return {
            port_name: (
                AvailabilityFact(True)
                if getattr(self._ports, port_name) is not None
                else AvailabilityFact(
                    False,
                    ("program_runtime_owner_unavailable",),
                )
            )
            for port_name in expected
        }

    @staticmethod
    def _headless_error(error: HeadlessClientError) -> ProgramRunnerError:
        return ProgramRunnerError(error.code, str(error))

    @staticmethod
    def _runner_error(error: Exception) -> ProgramRunnerError:
        if isinstance(error, ProgramRunnerError):
            return error
        code = (
            "program_run_scheduling_failed"
            if isinstance(error, ProgramSchedulingError)
            else "program_run_admission_failed"
        )
        return ProgramRunnerError(code, str(error) or type(error).__name__)

    @staticmethod
    def _digest(value: Mapping[str, Any]) -> str:
        encoded = rfc8785.dumps(dict(value))
        return "sha256:" + hashlib.sha256(encoded).hexdigest()

    @staticmethod
    def _ordinary_replay(record: Any) -> dict[str, Any] | None:
        if record.disposition == "new":
            return None
        if record.disposition == "succeeded" and record.result is not None:
            return record.result
        if record.disposition == "rejected" and record.error is not None:
            raise ProgramRunnerError(
                record.error["code"],
                record.error["message"],
            )
        raise ProgramRunnerError(
            "program_run_request_in_progress",
            "the exact request has an unresolved owner outcome",
        )

    @staticmethod
    def _retain_rejection(
        journal: ProgramCycleJournal,
        *,
        owner_epoch: str,
        request_id: str,
        error: ProgramRunnerError,
    ) -> None:
        try:
            journal.reject_request(
                owner_epoch=owner_epoch,
                request_id=request_id,
                code=error.code,
                message=str(error),
            )
        except Exception:
            # A NULL record means in-progress/unknown and is deliberately never
            # executable replay.
            pass

    def _require_cycle_owner(self) -> tuple[CellAdmissionClient, ProgramCycleJournal]:
        if self._admission is None or self._journal is None:
            raise ProgramRunnerError(
                "program_cycle_admission_unavailable",
                "durable Program-cycle admission is not configured",
            )
        return self._admission, self._journal

    @staticmethod
    def _admission_generation(value: Mapping[str, Any]) -> int:
        generation = value.get("admission_generation")
        if (
            type(generation) is not int
            or not 1 <= generation <= 9_007_199_254_740_991
        ):
            raise ProgramRunnerError(
                "execution_command_response_invalid",
                "cell admission omitted its generation",
            )
        return generation

    def cycle_status(self) -> dict[str, Any]:
        _admission, journal = self._require_cycle_owner()
        cycle = journal.read()
        if cycle.acquisition_pending is not None or (
            cycle.claim is None and cycle.state != "idle_after_reset"
        ):
            raise ProgramRunnerError(
                "program_cycle_owner_unknown",
                "the acquire response was lost before its exact claim was journaled",
            )
        status = ProgramCycleStatusDTO.model_validate({
            "program_runner_owner_epoch": cycle.owner_epoch,
            "cycle_generation": cycle.cycle_generation,
            "state": cycle.state,
            "program_run_id": cycle.program_run_id,
            "cycle_claim_generation": (
                None
                if cycle.claim is None
                else cycle.claim.get("claim_generation")
            ),
            "terminal_or_settlement_reference": cycle.terminal_reference,
        })
        return status.model_dump(mode="json")

    def _terminalize_failed_start(self, journal: ProgramCycleJournal) -> None:
        current = journal.read()
        if current.claim is None or current.state == "terminal_awaiting_reset":
            return
        reference = self._digest(
            {
                "cycle_generation": current.cycle_generation,
                "failure": "post_acquire_start_failure",
            }
        )
        journal.transition(
            expected_generation=current.cycle_generation,
            expected_states=("idle_after_reset", "starting", "running"),
            state="terminal_awaiting_reset",
            terminal_reference=reference,
            program_run_id=current.program_run_id,
        )

    def start(self) -> None:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is closed",
            )
        if self._started:
            return
        # An unresolved predecessor must fence commands, not make the owner
        # process disappear.  Manager still needs the health and cycle-status
        # surfaces in order to diagnose and reconcile the retained claim.
        self._host.start()
        if self._liveness is not None:
            self._liveness.start()
        self._started = True

    def health(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.program_runner_health.v1",
            "owner_id": self.owner_id,
            "owner_epoch": self.owner_epoch,
            "started": self._started,
            "closed": self._closed,
            "assistant_model_transport_available": self._assistant is not None,
            "assistant_snapshot_source_available": self._snapshot_source is not None,
            "simulation_control_available": self._simulation_control is not None,
        }

    def liveness_status(self) -> dict[str, Any]:
        if self._liveness is None:
            raise ProgramRunnerError(
                "program_runner_manager_liveness_unavailable",
                "Manager-presence liveness is not configured for this fixture",
            )
        return self._liveness.status()

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is closed",
            )
        snapshots, live_truncated = self._owner.list_snapshots(
            active_only=active_only,
            limit=limit,
        )
        values = [snapshot.to_dict() for snapshot in snapshots]
        for snapshot in snapshots:
            value = snapshot.to_dict()
            if self._journal is not None:
                self._journal.retain_run_snapshot(value)
            self._record_terminal_snapshot(value)
        history_truncated = False
        if not active_only and self._journal is not None:
            history, history_truncated = self._journal.list_run_snapshots(
                limit=self.config.maximum_retained_runs
            )
            resident = {
                value["capture"]["program_run_id"]
                for value in values
                if isinstance(value.get("capture"), dict)
            }
            values.extend(
                value
                for value in history
                if value["capture"]["program_run_id"] not in resident
                and value.get("state") in {"stopped", "succeeded", "failed"}
            )
            values.sort(key=lambda value: value["capture"]["program_run_id"])
            if len(values) > limit:
                values = values[:limit]
                history_truncated = True
        return {
            "schema_version": PROGRAM_RUN_INDEX_VERSION,
            "owner_id": self.owner_id,
            "owner_epoch": self.owner_epoch,
            "active_only": active_only,
            "truncated": live_truncated or history_truncated,
            "runs": values,
        }

    def _record_terminal_snapshot(self, snapshot: Any) -> None:
        value = (
            snapshot.to_dict()
            if callable(getattr(snapshot, "to_dict", None))
            else snapshot
        )
        if (
            self._journal is None
            or not isinstance(value, Mapping)
            or value.get("state") not in {
                "stopped",
                "succeeded",
                "failed",
            }
        ):
            return
        capture = value.get("capture")
        if not isinstance(capture, Mapping):
            return
        program_run_id = capture.get("program_run_id")
        if not isinstance(program_run_id, str):
            return
        with self._terminal_observation_lock:
            cycle = self._journal.read()
            if cycle.state == "starting":
                if len(self._pending_terminal_snapshots) >= (
                    self.config.maximum_retained_runs
                ):
                    raise ProgramRunnerError(
                        "program_run_terminal_observer_unavailable",
                        "pending terminal observation retention is full",
                    )
                self._pending_terminal_snapshots[program_run_id] = dict(value)
                return
            if (
                cycle.state not in {
                    "running",
                    "stopping",
                    "terminal_awaiting_reset",
                }
                or program_run_id != cycle.program_run_id
            ):
                return
            reference = self._digest(
                {
                    "program_run_id": program_run_id,
                    "run_revision": value.get("run_revision"),
                    "state": value.get("state"),
                }
            )
            self._journal.observe_terminal(
                expected_generation=cycle.cycle_generation,
                program_run_id=program_run_id,
                terminal_reference=reference,
                snapshot=value,
            )

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
        if not isinstance(request, ProgramRunStartRequest):
            raise TypeError("start_run requires one ProgramRunStartRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is not started",
            )
        self._require_manager_current()
        with self._transition_lock:
            self._require_manager_current()
            admission_client, journal = self._require_cycle_owner()
            cycle = journal.read()
            if (
                cycle.reconciliation_required
                or cycle.owner_epoch != self.owner_epoch
                or cycle.acquisition_pending is not None
            ):
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required",
                    "Program-cycle ownership requires authoritative reconciliation",
                )
            try:
                record = journal.record_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    kind="start",
                    intent=request.model_dump(mode="json"),
                    cycle_generation=cycle.cycle_generation,
                )
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    (
                        "program_run_request_ledger_full"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            replay = self._ordinary_replay(record)
            if replay is not None:
                return replay
            acquired_for_request = False
            try:
                if cycle.state != "idle_after_reset":
                    raise ProgramRunnerError(
                        "program_cycle_reset_required",
                        "Program cycle is not Idle after an explicit Reset",
                    )
                try:
                    journal.require_run_history_capacity()
                except OverflowError as error:
                    raise ProgramRunnerError(
                        "program_run_history_full", str(error)
                    ) from error
                owner_admission = admission_client.get()
                generation = self._admission_generation(owner_admission)
                if generation != request.expected_admission_generation:
                    raise ProgramRunnerError(
                        "stale_generation",
                        "Run admission generation changed before claim acquisition",
                    )
                applied = self._headless.program_applied()
                source = applied.get("source")
                if (
                    not isinstance(source, Mapping)
                    or source.get("record_digest")
                    != request.expected_applied_record_digest
                    or applied.get("scenario_id") != request.scenario_id
                    or applied.get("scenario_catalog_revision")
                    != request.scenario_catalog_revision
                ):
                    raise ProgramRunnerError(
                        "program_run_applied_selection_stale",
                        "Applied Program changed before Run admission",
                    )
                live = self._headless.scenario_program_selection(
                    request.scenario_id
                )
                if (
                    request.draft_program_revision
                    != applied.get("program_revision")
                ):
                    raise ProgramRunnerError(
                        "program_run_draft_applied_mismatch",
                        "Draft and Applied Program revisions differ",
                    )
                claim_intent = {
                    "claim_kind": "program_cycle",
                    "owner_role": "program_runner",
                    "owner_epoch": self.owner_epoch,
                    "owner_transaction_id": (
                        f"program-cycle.{cycle.cycle_generation}"
                    ),
                    "request_id": request.request_id,
                    "request_payload_digest": self._digest(
                        request.model_dump(mode="json")
                    ),
                    "expected_admission_generation": generation,
                    "scope": {"resources": [], "configurations": []},
                    "program_cycle_evidence": {
                        "draft_program_revision": request.draft_program_revision,
                        "applied_program_revision": applied["program_revision"],
                    },
                    "intervention_evidence": None,
                }
                provisional_run_id = f"program-cycle.{cycle.cycle_generation}"
                journal.begin_acquire(
                    expected_generation=cycle.cycle_generation,
                    provisional_run_id=provisional_run_id,
                    evidence=claim_intent,
                )
                try:
                    claim = admission_client.acquire(claim_intent)
                    acquired_for_request = True
                except CellAdmissionError as error:
                    if error.outcome_unknown:
                        journal.mark_acquire_unknown(
                            expected_generation=cycle.cycle_generation
                        )
                    else:
                        journal.cancel_rejected_acquire(
                            expected_generation=cycle.cycle_generation
                        )
                    raise
                except Exception:
                    journal.mark_acquire_unknown(
                        expected_generation=cycle.cycle_generation
                    )
                    raise
                try:
                    journal.remember_acquired_claim(
                        expected_generation=cycle.cycle_generation,
                        provisional_run_id=provisional_run_id,
                        claim=claim,
                    )
                except Exception as error:
                    try:
                        journal.require_reconciliation(
                            expected_generation=cycle.cycle_generation
                        )
                    except Exception:
                        pass
                    raise ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "acquired Program-cycle claim was not durably recorded",
                    ) from error
                # Re-read Applied after the Kernel has made mutation exclusion
                # authoritative, before any capture or execution effect.
                applied_after_claim = self._headless.program_applied()
                if applied_after_claim != applied:
                    raise ProgramRunnerError(
                        "program_run_applied_selection_stale",
                        "Applied Program changed during Run admission",
                    )
                run_admission = build_applied_program_run_admission(
                    applied,
                    live,
                    expected_scenario_id=request.scenario_id,
                    expected_scenario_catalog_revision=(
                        request.scenario_catalog_revision
                    ),
                    environment=self._environment,
                    owner_availability=self._owner_availability,
                )
                journal.transition(
                    expected_generation=cycle.cycle_generation,
                    expected_states=("idle_after_reset",),
                    state="starting",
                    program_run_id=provisional_run_id,
                    claim=claim,
                )
                snapshot = self._host.start_run(run_admission)
                result = snapshot.to_dict()
                capture = result.get("capture")
                if not isinstance(capture, dict):
                    raise ProgramRunnerError(
                        "program_run_capture_invalid",
                        "Program owner omitted immutable capture identities",
                    )
                identity_fields = (
                    "program_run_id",
                    "applied_record_digest",
                    "scenario_id",
                    "scenario_catalog_revision",
                    "program_id",
                    "program_revision",
                    "program_binding_id",
                    "retained_bundle_id",
                    "graph_revision",
                    "node_environment_digest",
                    "node_policies",
                    "node_policy_digest",
                )
                if any(key not in capture for key in identity_fields):
                    raise ProgramRunnerError(
                        "program_run_capture_invalid",
                        "Program owner omitted required immutable run evidence",
                    )
                identities = {key: capture[key] for key in identity_fields}
                with self._terminal_observation_lock:
                    journal.transition(
                        expected_generation=cycle.cycle_generation,
                        expected_states=("starting",),
                        state="running",
                        program_run_id=snapshot.capture.program_run_id,
                        capture_identities=identities,
                        run_snapshot=result,
                    )
                    pending_terminal = self._pending_terminal_snapshots.pop(
                        snapshot.capture.program_run_id,
                        None,
                    )
                    if pending_terminal is not None:
                        self._record_terminal_snapshot(pending_terminal)
                journal.complete_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    result=result,
                )
            except HeadlessClientError as error:
                if acquired_for_request:
                    self._terminalize_failed_start(journal)
                outcome = self._headless_error(error)
            except CellAdmissionError as error:
                if acquired_for_request:
                    self._terminalize_failed_start(journal)
                outcome = ProgramRunnerError(error.code, str(error))
            except ProgramRunnerError as error:
                if acquired_for_request:
                    self._terminalize_failed_start(journal)
                outcome = error
            except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                if acquired_for_request:
                    self._terminalize_failed_start(journal)
                outcome = self._runner_error(error)
            except Exception as error:
                if acquired_for_request:
                    self._terminalize_failed_start(journal)
                outcome = ProgramRunnerError(
                    "program_run_admission_failed",
                    str(error) or type(error).__name__,
                )
            else:
                return result
            self._retain_rejection(
                journal,
                owner_epoch=request.request_owner_epoch,
                request_id=request.request_id,
                error=outcome,
            )
            raise outcome

    def read_run(self, program_run_id: str) -> dict[str, Any]:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is closed",
            )
        try:
            result = self._owner.read(program_run_id).to_dict()
            if self._journal is not None:
                self._journal.retain_run_snapshot(result)
            self._record_terminal_snapshot(result)
            return result
        except ProgramRunOwnerError as error:
            if self._journal is not None:
                retained = self._journal.read_run_snapshot(program_run_id)
                if retained is not None:
                    return retained
            raise ProgramRunnerError(
                "program_run_not_found",
                str(error),
            ) from error

    def stop_run(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if not isinstance(request, ProgramRunStopRequest):
            raise TypeError("stop_run requires one ProgramRunStopRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is not started",
            )
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        if request.expected_owner_epoch != self.owner_epoch:
            raise ProgramRunnerError(
                "program_run_owner_stale",
                "Program-run owner epoch changed before Stop",
            )

        intent = request.model_dump(mode="json")
        key = (request.request_owner_epoch, request.request_id)
        retained: _EmergencyStopRecord | None = None
        owns_record = False
        with self._stop_replay_lock:
            resident = self._stop_replay.get(key)
            if resident is not None and resident.intent == intent:
                retained = resident
            elif resident is None and len(self._stop_replay) < (
                self.config.maximum_replay_records
            ):
                retained = _EmergencyStopRecord(intent, Event())
                self._stop_replay[key] = retained
                owns_record = True

        if retained is not None and not owns_record:
            retained.settled.wait()
            if retained.result is not None:
                return deepcopy(retained.result)
            if retained.error is not None:
                raise ProgramRunnerError(
                    retained.error.code, str(retained.error)
                )
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required",
                "resident Stop outcome is unavailable",
            )

        try:
            snapshot = self._host.stop_run(
                program_run_id,
                reason=request.reason,
            )
            result = snapshot.to_dict()
        except ProgramRunOwnerError as error:
            outcome = ProgramRunnerError("program_run_not_found", str(error))
        except ProgramSchedulingError as error:
            outcome = self._runner_error(error)
        except ProgramRunnerError as error:
            outcome = error
        except Exception as error:
            outcome = ProgramRunnerError(
                "program_run_scheduling_failed",
                str(error) or type(error).__name__,
            )
        else:
            # Journal projection is subordinate to recognized containment.
            # It must neither gate the owner call nor turn success into failure.
            try:
                _admission, journal = self._require_cycle_owner()
                journal.retain_run_snapshot(result)
                cycle = journal.read()
                if result.get("state") in {"stopped", "succeeded", "failed"}:
                    self._record_terminal_snapshot(result)
                elif (
                    cycle.program_run_id == program_run_id
                    and cycle.state in {"running", "stopping"}
                ):
                    journal.mark_stopping(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=program_run_id,
                    )
            except Exception:
                pass
            if retained is not None:
                retained.result = deepcopy(result)
                retained.settled.set()
            return result

        if retained is not None:
            retained.error = outcome
            retained.settled.set()
        raise outcome

    def reset_cycle(self, request: ProgramCycleResetRequest) -> dict[str, Any]:
        if not isinstance(request, ProgramCycleResetRequest):
            raise TypeError("reset_cycle requires one ProgramCycleResetRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is not started"
            )
        self._require_manager_current()
        with self._transition_lock:
            self._require_manager_current()
            admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            try:
                record = journal.record_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    kind="reset",
                    intent=request.model_dump(mode="json"),
                    cycle_generation=request.expected_cycle_generation,
                )
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    (
                        "program_run_request_ledger_full"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            replay = self._ordinary_replay(record)
            if replay is not None:
                return replay
            def reject(error: ProgramRunnerError) -> NoReturn:
                self._retain_rejection(
                    journal,
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    error=error,
                )
                raise error

            claim = cycle.claim
            if (
                cycle.state != "terminal_awaiting_reset"
                or cycle.cycle_generation != request.expected_cycle_generation
                or cycle.owner_epoch != request.expected_owner_epoch
                or cycle.program_run_id != request.program_run_id
                or cycle.terminal_reference != request.terminal_reference
                or request.settlement_evidence_digest
                != request.terminal_reference
                or claim is None
                or claim.get("claim_generation")
                != request.expected_claim_generation
            ):
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_stale",
                        "Reset does not identify the exact terminal Program cycle",
                    )
                )
            intent = claim.get("intent")
            if not isinstance(intent, dict):
                reject(
                    ProgramRunnerError(
                        "program_cycle_journal_invalid",
                        "retained admission claim has no exact intent",
                    )
                )
            release = {
                "owner_role": "program_runner",
                "owner_epoch": intent["owner_epoch"],
                "owner_transaction_id": intent["owner_transaction_id"],
                "request_id": intent["request_id"],
                "request_payload_digest": intent["request_payload_digest"],
                "acquired_admission_generation": claim[
                    "acquired_admission_generation"
                ],
                "claim_generation": claim["claim_generation"],
                "settlement": {
                    "settlement_kind": "explicit_reset",
                    "evidence_digest": request.settlement_evidence_digest,
                },
            }
            if (
                cycle.reconciliation_required
                or cycle.owner_epoch != self.owner_epoch
            ):
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "Reset cannot settle an unreconciled Program-cycle owner",
                    )
                )
            try:
                journal.remember_release_intent(
                    expected_generation=cycle.cycle_generation,
                    claim_generation=claim["claim_generation"],
                    release=release,
                )
            except Exception as error:
                reject(
                    ProgramRunnerError(
                        "program_cycle_journal_invalid",
                        "Program-cycle release intent was not durably retained",
                    )
                )
            try:
                released = admission.release(release)
            except CellAdmissionError as error:
                if error.outcome_unknown:
                    journal.require_reconciliation(
                        expected_generation=cycle.cycle_generation
                    )
                    reject(ProgramRunnerError(error.code, str(error)))
                # An explicit release rejection is no-effect and retryable.
                try:
                    journal.cancel_request(
                        owner_epoch=request.request_owner_epoch,
                        request_id=request.request_id,
                    )
                except Exception:
                    pass
                raise ProgramRunnerError(error.code, str(error)) from error
            except Exception:
                journal.require_reconciliation(
                    expected_generation=cycle.cycle_generation
                )
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "Program-cycle release outcome is unresolved",
                    )
                )
            result = {
                "schema_version": "manyforge.program_cycle_reset_result.v1",
                "program_run_id": request.program_run_id,
                "reset_cycle_generation": request.expected_cycle_generation,
                "idle_cycle_generation": request.expected_cycle_generation + 1,
                "settlement_reference": request.settlement_evidence_digest,
                "admission": released,
            }
            try:
                journal.publish_reset(
                    expected_generation=request.expected_cycle_generation,
                    owner_epoch=self.owner_epoch,
                    request_owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    result=result,
                )
            except Exception as error:
                try:
                    journal.require_reconciliation(
                        expected_generation=cycle.cycle_generation
                    )
                except Exception:
                    pass
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "released Program-cycle claim was not durably settled",
                    )
                )
            return result

    def shutdown(self) -> None:
        if self._closed:
            return
        self._closed = True
        errors: list[Exception] = []
        if self._liveness is not None:
            try:
                self._liveness.close()
            except Exception as error:
                errors.append(error)
        if self._started:
            try:
                # Deliberately does not submit Stop: runner loss cannot mutate
                # already-dispatched Kernel work.
                self._host.shutdown()
            except Exception as error:
                errors.append(error)
        if self._assistant is not None:
            try:
                self._assistant.close()
            except Exception as error:
                errors.append(error)
        if self._snapshot_source is not None:
            close = getattr(self._snapshot_source, "close", None)
            if callable(close):
                try:
                    close()
                except Exception as error:
                    errors.append(error)
        if self._simulation_control is not None:
            try:
                self._simulation_control.close()  # type: ignore[attr-defined]
            except Exception as error:
                errors.append(error)
        if self._model_transport is not None:
            close = getattr(self._model_transport, "close", None)
            if callable(close):
                try:
                    close()
                except Exception as error:
                    errors.append(error)
        if self._owns_headless_client:
            try:
                self._headless.close()
            except Exception as error:
                errors.append(error)
        if self._journal is not None:
            try:
                self._journal.close()
            except Exception as error:
                errors.append(error)
        if errors:
            raise ProgramRunnerError(
                "program_runner_shutdown_failed",
                "; ".join(str(error) or type(error).__name__ for error in errors),
            )


__all__ = [
    "PROGRAM_RUN_INDEX_VERSION",
    "ProgramRunnerConfig",
    "ProgramRunnerError",
    "ProgramRuntime",
]
