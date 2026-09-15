"""Independent Program orchestration owner assembled from focused clients."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import dataclass
import hashlib
import math
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
from manyforge_behavior.node_availability import (
    AvailabilityFact,
    project_node_execution_availability,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    NodeAdapterPorts,
    OwnerTaskPort,
    ProgramRunScopeState,
)
from manyforge_behavior.production_ports import (
    HeadlessSceneProgramPort,
    NativeKernelProgramPort,
)
from manyforge_behavior.program_run_owner import (
    PROGRAM_RESET_STOP_REASON,
    ProgramRunOwner,
    ProgramRunOwnerError,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunOwnerRecoveryIncompleteError,
    ProgramRunState,
    build_applied_program_run_admission,
)
from manyforge_behavior.program_scheduling import (
    ProgramInterventionRegistry,
    ProgramRunTickHost,
    ProgramSchedulingError,
)
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.models import (
    ManualProgramHandover,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramScenePreparationRequest,
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
from .execution_transport import ProgramRunnerExecutionTransport
from .liveness import (
    ProgramRunnerLivenessError,
    ProgramRunnerLivenessReceiver,
)
from .models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramCycleStatusDTO,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramNodeAvailabilityRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
)
from .runtime_controls import (
    ProgramRuntimeControlError,
    ProgramRuntimeControls,
    manual_result_availability,
)
from .diagnostics import ProgramDiagnosticPublisher


PROGRAM_RUN_INDEX_VERSION: Final = "manyforge.program_run_index.v1"
_MAX_INFLIGHT_COMMANDS: Final = 4_096


class ProgramRunnerError(RuntimeError):
    """Stable failure from the isolated Program-runner owner."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = deepcopy(dict(details or {}))


@dataclass(frozen=True, slots=True)
class ProgramRunnerConfig:
    headless_url: str
    execution_run_root: Path | None = None
    cycle_journal_path: Path | None = None
    owner_id: str = "program-runner"
    owner_epoch: str | None = None
    headless_timeout_seconds: float = 30.0
    tick_interval_seconds: float = 0.01
    tick_workers: int = 4
    maximum_retained_runs: int = 4_096
    maximum_inflight_commands: int = _MAX_INFLIGHT_COMMANDS
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
        for field_name in (
            "maximum_retained_runs",
            "maximum_inflight_commands",
        ):
            value = getattr(self, field_name)
            if type(value) is not int or not 1 <= value <= 4_096:
                raise ValueError(f"{field_name} must be in [1, 4096]")
        enabled = self.execution_run_root is not None
        if enabled != (self.cycle_journal_path is not None):
            raise ValueError(
                "execution run root and cycle journal must be configured together"
            )
        if enabled and (
            not self.execution_run_root.is_absolute()
            or not self.cycle_journal_path.is_absolute()
        ):
            raise ValueError(
                "execution run root and cycle journal must be stable absolute paths"
            )
        if enabled:
            try:
                execution_run_root = self.execution_run_root.resolve(strict=False)
                cycle_journal_path = self.cycle_journal_path.resolve(strict=False)
            except (OSError, RuntimeError) as error:
                raise ValueError(
                    "execution run root and cycle journal must be resolvable"
                ) from error
            if (
                cycle_journal_path == execution_run_root
                or execution_run_root in cycle_journal_path.parents
            ):
                raise ValueError(
                    "cycle journal must be outside disposable execution run storage"
                )
        liveness_enabled = self.manager_liveness_socket is not None
        if liveness_enabled != (self.cell_id is not None):
            raise ValueError(
                "Manager liveness socket and cell identity must be configured together"
            )
        if liveness_enabled and not self.manager_liveness_socket.is_absolute():
            raise ValueError("Manager liveness socket must be a stable absolute path")


@dataclass(slots=True)
class _InflightStop:
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
        execution_transport: Any | None = None,
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
            if config.execution_run_root is None
            else CellAdmissionClient(
                config.execution_run_root,
                timeout_seconds=min(config.headless_timeout_seconds, 30.0),
            )
        )
        self._journal = cycle_journal or (
            None
            if config.cycle_journal_path is None
            else ProgramCycleJournal(
                config.cycle_journal_path,
                owner_epoch=self.owner_epoch,
                maximum_inflight_commands=config.maximum_inflight_commands,
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
        if execution_transport is None:
            if config.execution_run_root is None:
                raise ValueError(
                    "Program runtime requires the private execution run root"
                )
            execution_transport = ProgramRunnerExecutionTransport(
                config.execution_run_root,
                timeout_seconds=min(config.headless_timeout_seconds, 30.0),
            )
        self._kernel = KernelClient(execution_transport)
        # Starting has no bound tree yet. Retain its already-read Kernel
        # attribution so priority Stop never needs an ordinary owner read.
        self._starting_kernel_owner_epoch: str | None = None
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
        self._native = NativeKernelProgramPort(
            self._kernel,
            current_owner_epoch=self._current_kernel_owner_epoch,
        )
        self._scene = HeadlessSceneProgramPort(
            self._headless, program_node_evidence=self._program_node_admission_evidence
        )
        self._cclc = CCLCProgramTaskPort(
            self._headless,
            maximum_retained_tasks=config.maximum_retained_runs,
            program_node_evidence=self._program_node_admission_evidence,
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
            kernel_controls=self._native.controls,
            model_queries=self._native.queries,
            gripper_status=self._native.queries,
            scene_transactions=self._scene,
            assistant_requests=self._assistant,
            simulator_control=self._simulation_control,
            cclc_lifecycle=self._cclc,
            cclc_routes=self._cclc,
            program_run_scope=self._native.scope,
            program_run_context=CompositeProgramRunContextPort(*context_members),
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
        self._runtime_controls: dict[str, ProgramRuntimeControls] = {}
        self._runtime_controls_lock = RLock()
        # A separate bounded-time HTTP client prevents forensic I/O from
        # occupying the tree/command lanes or their shared connection pool.
        self._diagnostic_client = (
            HeadlessHttpClient(config.headless_url, timeout_seconds=0.25)
            if headless_client is None
            else None
        )
        diagnostic_client = self._diagnostic_client or self._headless
        self._diagnostics = ProgramDiagnosticPublisher(
            lambda record: diagnostic_client.record_program_diagnostic(record)
        )
        self._control_state_observers: set[Callable[[], None]] = set()
        self._control_state_observer_lock = RLock()
        self._host = ProgramRunTickHost(
            self._owner,
            tick_interval_s=config.tick_interval_seconds,
            maximum_scheduled_runs=config.maximum_retained_runs,
            maximum_workers=config.tick_workers,
            terminal_observer=self._record_terminal_snapshot,
            state_observer=self._observe_run_state,
        )
        if self._journal is not None:
            self._journal.set_change_observer(self._notify_control_state_change)
        # Serialize ordinary Start/Reset lifecycle admission and journal writes.
        # Stop never takes this lock.
        self._transition_lock = RLock()
        # Stop correlation is deliberately independent of the ordinary durable
        # request ledger and lifecycle mutex. Full/unavailable ordinary state
        # can therefore never reject recognized containment.
        self._inflight_stops_lock = RLock()
        self._inflight_stops: dict[tuple[str, str, str], _InflightStop] = {}
        self._started = False
        self._closed = False

    def add_control_state_observer(self, observer: Callable[[], None]) -> None:
        """Subscribe to capacity-free current-state wakes, not event history."""

        if not callable(observer):
            raise TypeError("control-state observer must be callable")
        with self._control_state_observer_lock:
            self._control_state_observers.add(observer)

    def remove_control_state_observer(self, observer: Callable[[], None]) -> None:
        with self._control_state_observer_lock:
            self._control_state_observers.discard(observer)

    def _notify_control_state_change(self) -> None:
        with self._control_state_observer_lock:
            observers = tuple(self._control_state_observers)
        for observer in observers:
            try:
                observer()
            except Exception:
                # Projection consumers are never execution dependencies. A
                # later current-state wake repairs a missed notification.
                continue

    def _observe_run_state(self, snapshot: Any) -> None:
        """Wake readers; selected holds do not transition the whole cycle."""
        self._notify_control_state_change()
        record = self._owner.take_diagnostic(snapshot.capture.program_run_id)
        if record is not None:
            self._diagnostics.submit(record)

    def diagnostic_health(self) -> dict[str, Any]:
        return self._diagnostics.health()

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
        return self._cycle_status_core()

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
            cycle = journal.read()
            liveness_status = (
                None if self._liveness is None else self._liveness.status()
            )
            fault_reference = (
                None
                if liveness_status is None
                else liveness_status.get("fault_reference")
            )
            if (
                cycle.reconciliation_required
                and cycle.program_run_id is not None
                and cycle.state not in {"idle_after_reset", "terminal_awaiting_reset"}
                and isinstance(fault_reference, str)
            ):
                self._contain_retained_run(
                    cycle.program_run_id,
                    fault_reference=fault_reference,
                )
                cycle = journal.read()
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
            reset_recovery_phase = self._reset_recovery_phase(
                cycle,
                allow_reconciliation=True,
            )
            contained_reset_recovery = bool(
                reset_recovery_phase is not None
                and self._cycle_terminal_evidence_is_exact(journal, cycle)
            )
            contained_owner_state = bool(
                cycle.state == "terminal_awaiting_reset" or contained_reset_recovery
            )
            if cycle.state != "idle_after_reset" and not contained_owner_state:
                raise RuntimeError(
                    "active Program cycle is not contained for reconciliation"
                )
            if cycle.acquisition_pending is not None:
                raise RuntimeError("unknown Program-cycle acquisition is unresolved")

            if cycle.claim is None:
                receipt = admission.recovery_inventory(owner_epoch=self.owner_epoch)
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
            current_reconciliation_digest = (
                cell_admission_owner_epoch_reconciliation_evidence_digest(
                    "program_runner", self.owner_epoch
                )
            )

            def reconciled_receipt_is_exact(successor: str) -> bool:
                if cycle.claim is None:
                    return False
                evidence = cell_admission_owner_epoch_reconciliation_evidence_digest(
                    "program_runner", successor
                )
                return bool(
                    receipt.get("status") == "reconciled_exact"
                    and receipt.get("matched_claim") == cycle.claim
                    and receipt.get("matched_release") is None
                    and receipt.get("matched_reconcile_settlement")
                    == {
                        "predecessor_claim": cycle.claim,
                        "successor_owner_role": "program_runner",
                        "successor_owner_epoch": successor,
                        "settlement_predicate": "reconciled",
                        "reconciliation_evidence_digest": evidence,
                    }
                )

            def unknown_receipt_is_exact() -> bool:
                return bool(
                    receipt.get("status") == "unknown"
                    and receipt.get("matched_claim") is None
                    and receipt.get("matched_release") is None
                    and receipt.get("matched_reconcile_settlement") is None
                )

            def require_terminal_evidence() -> Mapping[str, Any]:
                claim = cycle.claim
                intent = claim.get("intent") if isinstance(claim, Mapping) else None
                if (
                    not isinstance(claim, Mapping)
                    or not isinstance(intent, Mapping)
                    or intent.get("owner_role") != "program_runner"
                    or intent.get("claim_kind") != "program_cycle"
                    or not self._cycle_terminal_evidence_is_exact(journal, cycle)
                ):
                    raise RuntimeError("terminal Program-cycle evidence is not exact")
                return intent

            if not program_owners:
                claimless_contained = bool(
                    contained_owner_state
                    and cycle.claim is None
                    and not held_program_claims
                    and self._cycle_terminal_evidence_is_exact(journal, cycle)
                )
                if (
                    held_program_claims
                    or cycle.claim is not None
                    or (cycle.state != "idle_after_reset" and not claimless_contained)
                ):
                    raise RuntimeError(
                        "absent Program Runner owner is not a clean bootstrap"
                    )
                if (
                    cycle.owner_epoch != self.owner_epoch
                    or cycle.reconciliation_required
                ):
                    journal.reconcile_liveness(
                        owner_epoch=self.owner_epoch,
                        evidence_digest=current_reconciliation_digest,
                        require_terminal_release=False,
                        allow_contained_reset=contained_reset_recovery,
                    )
            elif len(program_owners) != 1:
                raise RuntimeError("Kernel Program Runner owner is not exact")
            elif program_owners[0].get("owner_epoch") == self.owner_epoch:
                if (
                    cycle.owner_epoch != self.owner_epoch
                    or cycle.reconciliation_required
                ):
                    if contained_owner_state:
                        intent = require_terminal_evidence()
                        if cycle.release_intent is not None:
                            if (
                                held_program_claims
                                or receipt.get("status") != "released_exact"
                                or receipt.get("matched_claim") != cycle.claim
                                or receipt.get("matched_release")
                                != cycle.release_intent
                            ):
                                raise RuntimeError(
                                    "terminal Program-cycle release is not exact"
                                )
                        elif intent.get("owner_epoch") == self.owner_epoch:
                            if (
                                held_program_claims != [cycle.claim]
                                or receipt.get("status") != "held_exact"
                                or receipt.get("matched_claim") != cycle.claim
                            ):
                                raise RuntimeError(
                                    "terminal Program-cycle claim is not exact"
                                )
                        elif not (
                            reconciled_receipt_is_exact(self.owner_epoch)
                            or (
                                not held_program_claims
                                and unknown_receipt_is_exact()
                                and cycle.reconciliation_evidence_digest
                                == current_reconciliation_digest
                            )
                        ):
                            raise RuntimeError(
                                "terminal Program-cycle reconciliation settlement "
                                "is not exact"
                            )
                    journal.reconcile_liveness(
                        owner_epoch=self.owner_epoch,
                        evidence_digest=current_reconciliation_digest,
                        require_terminal_release=False,
                        allow_contained_reset=contained_reset_recovery,
                    )
            else:
                predecessor = program_owners[0].get("owner_epoch")
                if not isinstance(predecessor, str) or not predecessor:
                    raise RuntimeError(
                        "predecessor Program Runner ownership is unresolved"
                    )
                if contained_owner_state:
                    intent = require_terminal_evidence()
                    if cycle.release_intent is not None:
                        if (
                            held_program_claims
                            or receipt.get("status") != "released_exact"
                            or receipt.get("matched_claim") != cycle.claim
                            or receipt.get("matched_release") != cycle.release_intent
                        ):
                            raise RuntimeError(
                                "terminal predecessor release is not exact"
                            )
                    elif held_program_claims:
                        if (
                            held_program_claims != [cycle.claim]
                            or receipt.get("status") != "held_exact"
                            or receipt.get("matched_claim") != cycle.claim
                            or intent.get("owner_epoch") != predecessor
                        ):
                            raise RuntimeError(
                                "terminal predecessor claim is not exact"
                            )
                    else:
                        predecessor_digest = (
                            cell_admission_owner_epoch_reconciliation_evidence_digest(
                                "program_runner", predecessor
                            )
                        )
                        local_predecessor_is_exact = bool(
                            cycle.owner_epoch == predecessor
                            and intent.get("owner_epoch") != predecessor
                            and (
                                cycle.reconciliation_required
                                or cycle.reconciliation_evidence_digest
                                == predecessor_digest
                            )
                        )
                        if not local_predecessor_is_exact or not (
                            reconciled_receipt_is_exact(predecessor)
                            or unknown_receipt_is_exact()
                        ):
                            raise RuntimeError(
                                "terminal predecessor settlement is not exact"
                            )
                elif cycle.claim is not None or held_program_claims:
                    raise RuntimeError("idle predecessor retains a Program-cycle claim")
                journal.prepare_liveness_reconciliation(
                    predecessor_owner_epoch=predecessor,
                    evidence_digest=current_reconciliation_digest,
                    require_terminal_release=False,
                    allow_contained_reset=contained_reset_recovery,
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
                            current_reconciliation_digest
                        ),
                    }
                )
                # The Kernel is authoritative.  Rotate it first, then catch
                # the local journal up.  If either process dies between the
                # two writes, the next reconciliation can repeat the Kernel
                # effect or observe it already complete and finish the local
                # write without inventing another recovery state.
                journal.reconcile_liveness(
                    owner_epoch=self.owner_epoch,
                    evidence_digest=current_reconciliation_digest,
                    require_terminal_release=False,
                    allow_contained_reset=contained_reset_recovery,
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
                and cycle.state not in {"idle_after_reset", "terminal_awaiting_reset"}
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
                    self._owner.read(program_run_id)
                except ProgramRunOwnerError:
                    try:
                        self._contain_retained_run(
                            program_run_id,
                            fault_reference=fault_reference,
                        )
                    except Exception:
                        # The exact Kernel scope did not prove settlement. The
                        # retained cycle stays fenced for later reconciliation.
                        pass
                    continue
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

    def _contain_retained_run(
        self,
        program_run_id: str,
        *,
        fault_reference: str,
    ) -> bool:
        """Contain one non-resident retained run from exact Kernel evidence.

        A replacement Program Runner deliberately cannot recreate an in-flight
        behavior tree. It can still fence the immutable Program-run identity at
        the Kernel and, after certified settlement, retain the same terminal
        presentation that an ordinary owner Stop would have produced.
        """

        journal = self._journal
        if journal is None:
            return False
        retained = journal.read_run_snapshot(program_run_id)
        if not isinstance(retained, Mapping):
            return False
        capture = retained.get("capture")
        if (
            not isinstance(capture, Mapping)
            or capture.get("program_run_id") != program_run_id
        ):
            return False
        state = retained.get("state")
        if state in {"stopped", "succeeded", "failed"}:
            self._record_terminal_snapshot(retained)
            return journal.read().state == "terminal_awaiting_reset"
        revision = retained.get("run_revision")
        node_revision = retained.get("node_status_revision")
        statuses = retained.get("node_statuses")
        if (
            type(revision) is not int
            or revision < 1
            or type(node_revision) is not int
            or node_revision < 1
            or not isinstance(statuses, Mapping)
        ):
            return False

        scope = self._native.scope
        settlement = scope.request_stop(
            program_run_id,
            self._digest(
                {
                    "fault_reference": fault_reference,
                    "kind": "manager_loss_retained_run_stop",
                    "program_run_id": program_run_id,
                }
            ),
            "manager_presence_expired",
        )
        # Reconciliation is retried by the existing Manager-renewal cadence.
        # Never hold that liveness request open while physical settlement is
        # pending: the exact Kernel Stop identity is idempotent, so the next
        # renewal observes or rejoins the same containment operation.
        if settlement.state is not ProgramRunScopeState.SETTLED:
            return False

        stopped_statuses = {instance_id: "idle" for instance_id in statuses}
        terminal = deepcopy(dict(retained))
        terminal.update(
            {
                "run_revision": revision + 1,
                "state": "stopped",
                "tree_status": "invalid",
                "node_status_revision": (
                    node_revision + 1
                    if dict(statuses) != stopped_statuses
                    else node_revision
                ),
                "node_statuses": stopped_statuses,
                "active_operation_count": 0,
                "reason_code": None,
                "message": "",
            }
        )
        self._record_terminal_snapshot(terminal)
        cycle = journal.read()
        return bool(
            cycle.state == "terminal_awaiting_reset"
            and cycle.program_run_id == program_run_id
        )

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

    def _current_node_owner_availability(
        self,
        applied: Mapping[str, Any],
        *,
        admission_generation: int,
    ) -> dict[str, AvailabilityFact]:
        """Join configured ports to the current exact native owner identity."""

        availability = dict(self._owner_availability)
        native_ports = {
            "gripper_status",
            "kernel_controls",
            "kernel_operations",
            "model_queries",
        }.intersection(availability)
        if not native_ports:
            return availability
        try:
            self._run_recovery_fences(
                applied,
                self._kernel.identity(),
                scene_admission_epoch=admission_generation,
            )
        except Exception:
            unavailable = AvailabilityFact(
                False,
                ("program_execution_owner_unavailable",),
            )
            for port_name in native_ports:
                availability[port_name] = unavailable
        return availability

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
    def _require_new_command(record: Any) -> None:
        if record.disposition == "new":
            return
        raise ProgramRunnerError(
            "execution_control_command_in_progress",
            "the exact command is already unresolved in the current owner state",
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
                details=error.details,
                message=str(error),
            )
        except Exception:
            # Failure to remove the row leaves it unresolved and therefore
            # deliberately non-executable.
            pass

    def _require_cycle_owner(self) -> tuple[CellAdmissionClient, ProgramCycleJournal]:
        if self._admission is None or self._journal is None:
            raise ProgramRunnerError(
                "program_cycle_admission_unavailable",
                "durable Program-cycle admission is not configured",
            )
        return self._admission, self._journal

    def _program_node_admission_evidence(
        self, identity: Any, owner_api: str
    ) -> dict[str, Any]:
        _admission, journal = self._require_cycle_owner()
        cycle = journal.read()
        if (
            cycle.owner_epoch != self.owner_epoch
            or cycle.program_run_id != identity.program_run_id
            or cycle.state != "running"
            or cycle.claim is None
        ):
            raise ProgramRunnerError(
                "program_node_parent_not_current",
                "Node transaction has no current running Program claim",
            )
        return {
            "program_runner_owner_epoch": cycle.owner_epoch,
            "program_run_id": cycle.program_run_id,
            "parent_program_cycle_claim_generation": cycle.claim["claim_generation"],
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
            "owner_api": owner_api,
        }

    def _current_kernel_owner_epoch(self) -> str:
        """Read the stable Kernel identity without requiring a robot product."""

        starting_epoch = self._starting_kernel_owner_epoch
        if starting_epoch is not None:
            return starting_epoch
        admission, _journal = self._require_cycle_owner()
        inventory = admission.recovery_inventory(owner_epoch=self.owner_epoch)
        owner_epoch = inventory.get("kernel_owner_epoch")
        if not isinstance(owner_epoch, str) or not owner_epoch:
            raise ProgramRunnerError(
                "execution_command_response_invalid",
                "cell admission omitted the current Kernel owner epoch",
            )
        return owner_epoch

    @staticmethod
    def _admission_generation(value: Mapping[str, Any]) -> int:
        generation = value.get("admission_generation")
        if type(generation) is not int or not 1 <= generation <= 9_007_199_254_740_991:
            raise ProgramRunnerError(
                "execution_command_response_invalid",
                "cell admission omitted its generation",
            )
        return generation

    @staticmethod
    def _reset_recovery_phase(
        cycle: Any,
        *,
        allow_reconciliation: bool,
    ) -> str | None:
        """Return an exact retained Reset phase, optionally while fenced."""

        if cycle.state != "recovery_required" or not isinstance(cycle.failure, Mapping):
            return None
        failure_code = cycle.failure.get("code")
        failed_phase = cycle.failure.get("failed_phase")
        if (
            (allow_reconciliation or not cycle.reconciliation_required)
            and cycle.reset_phase
            in {"stopping", "settling", "restoring_scene", "releasing"}
            and failed_phase == cycle.reset_phase
            and isinstance(failure_code, str)
            and failure_code
        ):
            return cycle.reset_phase
        # Schema-v4 UI6-F candidates briefly recorded a known scene-owner
        # rejection as generic reconciliation with phase ``failed``. No claim
        # release was attempted, so the same exact scene convergence remains
        # safely idempotent and can be repaired in place after upgrade.
        if (
            cycle.reconciliation_required
            and cycle.reset_phase == "failed"
            and failure_code == "program_cycle_reset_scene_preparation_failed"
            and cycle.release_intent is None
        ):
            return "restoring_scene"
        return None

    @classmethod
    def _reset_retry_phase(cls, cycle: Any) -> str | None:
        """Return the one durable Reset phase safe to resume, if any."""

        return cls._reset_recovery_phase(
            cycle,
            allow_reconciliation=False,
        )

    def _current_cycle_recovery_stop(self, cycle: Any) -> bool:
        """Contain current-owner work without claiming uncertainty resolved."""
        claim = cycle.claim
        intent = claim.get("intent") if isinstance(claim, Mapping) else None
        return bool(
            cycle.state == "recovery_required"
            and cycle.owner_epoch == self.owner_epoch
            and isinstance(cycle.program_run_id, str)
            and bool(cycle.program_run_id)
            and isinstance(intent, Mapping)
            and intent.get("owner_role") == "program_runner"
            and intent.get("claim_kind") == "program_cycle"
            and intent.get("owner_epoch") == self.owner_epoch
            and intent.get("owner_transaction_id") == cycle.program_run_id
            and type(claim.get("claim_generation")) is int
            and 1 <= claim["claim_generation"] <= 9_007_199_254_740_991
            and cycle.acquisition_pending is None
            and cycle.reset_phase is None
            and cycle.release_intent is None
        )

    def _current_cycle_recovery_reset(self, cycle: Any, pending=None) -> bool:
        """Recognize only retained, current-owner command uncertainty."""
        claim = cycle.claim
        intent = claim.get("intent") if isinstance(claim, Mapping) else None
        if not (
            cycle.reconciliation_required
            and cycle.state in {"recovery_required", "terminal_awaiting_reset"}
            and cycle.owner_epoch == self.owner_epoch
            and cycle.program_run_id
            and cycle.reset_phase in {None, "stopping", "settling"}
            and cycle.start_settlement is None
            and cycle.acquisition_pending is None
            and cycle.release_intent is None
            and isinstance(intent, Mapping)
            and intent.get("owner_role") == "program_runner"
            and intent.get("claim_kind") == "program_cycle"
            and intent.get("owner_epoch") == self.owner_epoch
            and intent.get("owner_transaction_id") == cycle.program_run_id
            and type(claim.get("claim_generation")) is int
            and 1 <= claim["claim_generation"] <= 9_007_199_254_740_991
        ):
            return False
        journal = self._require_cycle_owner()[1]
        if journal.liveness().expired:
            return False
        rows = journal.pending_commands() if pending is None else pending
        resets = [row for row in rows if row["kind"] == "reset"]
        if not rows or len(resets) > 1:
            return False
        for row in rows:
            command = row["intent"]
            if (
                row["cycle_generation"] != cycle.cycle_generation
                or command.get("expected_owner_epoch") != self.owner_epoch
            ):
                return False
            if row["kind"] in {
                "resume_motion",
                "resume_full_stop",
                "prepare_manual_handover",
                "reset",
            }:
                if command.get("program_run_id") != cycle.program_run_id:
                    return False
                if row["kind"] == "reset" and (
                    command.get("expected_cycle_generation") != cycle.cycle_generation
                    or command.get("expected_claim_generation")
                    != claim["claim_generation"]
                ):
                    return False
            elif row["kind"] == "recovery_action":
                if command.get("action_id") == "reset_cycle" and len(resets) == 1:
                    # The existing stopped-motion Reset offer creates this
                    # exact nested Reset. It is not arbitrary owner recovery.
                    nested_id = self._digest(
                        {
                            "kind": "operator_stop_reset_cycle",
                            "request_owner_epoch": row["request_owner_epoch"],
                            "request_id": row["request_id"],
                            "program_run_id": cycle.program_run_id,
                        }
                    )
                    if (resets[0]["request_id"], resets[0]["request_owner_epoch"]) != (
                        nested_id,
                        row["request_owner_epoch"],
                    ):
                        return False
                elif command.get("action_id") != "replan_from_stopped_state":
                    return False
            else:
                return False
        return True

    def _prove_reset_recovery(self, cycle, *, kernel_owner_epoch: str) -> None:
        """Join current terminal/tree and exact Kernel-held parent/children."""
        admission, journal = self._require_cycle_owner()
        terminal = self._project_run(self._owner.read(cycle.program_run_id).to_dict())
        claim = cycle.claim
        if (
            terminal.get("owner_epoch") != self.owner_epoch
            or terminal.get("active_operation_count") != 0
            or not self._cycle_terminal_evidence_is_exact(journal, cycle)
            or terminal != journal.read_run_snapshot(cycle.program_run_id)
        ):
            raise RuntimeError("Reset has no exact current terminal Run")
        receipt = admission.recovery_receipt(
            {
                "intent": claim["intent"],
                "expected_claim": claim,
                "expected_release": None,
            }
        )
        program_owners = [
            owner
            for owner in receipt.get("current_owner_epochs", [])
            if owner.get("owner_role") == "program_runner"
        ]
        held = receipt.get("held_claims")
        if not (
            receipt.get("kernel_owner_epoch") == kernel_owner_epoch
            and receipt.get("status") == "held_exact"
            and receipt.get("matched_claim") == claim
            and receipt.get("matched_release") is None
            and receipt.get("matched_reconcile_settlement") is None
            and program_owners
            == [{"owner_role": "program_runner", "owner_epoch": self.owner_epoch}]
            and isinstance(held, list)
            and held.count(claim) == 1
        ):
            raise RuntimeError("Reset parent claim/Kernel owner is not exact")
        for child in held:
            child_intent = child.get("intent", {})
            for key in ("program_node_evidence", "intervention_evidence"):
                evidence = child_intent.get(key)
                if (
                    isinstance(evidence, Mapping)
                    and evidence.get("parent_program_cycle_claim_generation")
                    == claim["claim_generation"]
                ):
                    raise RuntimeError("Reset still has an unsettled child transaction")

    def _cycle_status_core(self) -> dict[str, Any]:
        """Return the closed, dependency-free Manager liveness projection."""

        _admission, journal = self._require_cycle_owner()
        cycle = journal.read()
        acquisition_pending = cycle.acquisition_pending is not None
        claimless_no_effect = bool(
            cycle.claim is None
            and cycle.state in {"terminal_awaiting_reset", "resetting"}
            and isinstance(cycle.start_settlement, Mapping)
            and cycle.start_settlement.get("kind") == "proven_no_effect"
            and cycle.start_settlement.get("claim") is None
            and cycle.start_settlement.get("program_run_id") == cycle.program_run_id
            and self._digest(cycle.start_settlement) == cycle.terminal_reference
        )
        if (
            acquisition_pending
            and (
                cycle.state not in {"starting", "stopping", "recovery_required"}
                or cycle.claim is not None
                or cycle.program_run_id is None
            )
        ) or (
            not acquisition_pending
            and cycle.claim is None
            and cycle.state != "idle_after_reset"
            and not claimless_no_effect
        ):
            raise ProgramRunnerError(
                "program_cycle_owner_unknown",
                "the acquire response was lost before its exact claim was journaled",
            )
        command_target = {
            "program_runner_owner_epoch": cycle.owner_epoch,
            "cycle_generation": cycle.cycle_generation,
            "program_run_id": cycle.program_run_id,
        }
        reset_retry_phase = self._reset_retry_phase(cycle)
        recovery_stop = self._current_cycle_recovery_stop(cycle)
        recovery_reset = self._current_cycle_recovery_reset(cycle)
        allowed_by_state = {
            "program_run": cycle.state == "idle_after_reset",
            "program_motion_stop": cycle.state in {"starting", "running"},
            "program_motion_resume": cycle.state == "running",
            "program_full_stop": cycle.state in {"starting", "running"},
            "program_stop": cycle.state in {"starting", "running"} or recovery_stop,
            "program_reset": cycle.state
            in {
                "running",
                "terminal_awaiting_reset",
            }
            or reset_retry_phase is not None
            or recovery_reset,
            # The dependency-free liveness projection cannot inspect the
            # retained Program tree. The complete cycle_status() projection
            # enables these only after proving the tree is Full-stopped.
            "program_resume_full_stop": False,
            "program_discard_full_stop": False,
            "program_runtime_override_stage": cycle.state == "running",
            "program_runtime_override_activate": cycle.state == "running",
            "program_runtime_override_clear": cycle.state == "running",
            "program_runtime_override_promote": cycle.state == "running",
            "program_runtime_blackboard_write": cycle.state == "running",
            "program_manual_result_stage": cycle.state == "running",
            # Only the Kernel-selected held tasks may be retired; the owner
            # rechecks the exact capture and keeps all Program holds intact.
            "program_manual_handover": cycle.state == "running",
            # Evidence input remains available while a Program waits for it.
            # The native owner decides exact command/source/CAS eligibility.
            "gripper_confirm": cycle.state
            in {
                "idle_after_reset",
                "running",
                "terminal_awaiting_reset",
            },
            # These are coarse lifecycle prerequisites, not target permission.
            # During a Program, shared ingress requires exact native manual
            # identity/context; the Kernel retains final admission authority.
            "operation_start": cycle.state in {"idle_after_reset", "running"},
            "operation_stop": cycle.state != "resetting",
            "operation_pause": cycle.state in {"idle_after_reset", "running"},
            "operation_resume": cycle.state in {"idle_after_reset", "running"},
            "operation_reset": cycle.state != "starting",
            "operation_acknowledge_completed": cycle.state != "starting",
            "cell_full_stop": True,
            "execution_recovery": cycle.state
            in {
                "running",
                "recovery_required",
            },
            "stop_recovery": cycle.state == "recovery_required",
            "runtime_scale": cycle.state != "recovery_required",
        }
        if cycle.reconciliation_required:
            allowed_by_state = {
                kind: kind
                in {
                    "cell_full_stop",
                    "execution_recovery",
                    "stop_recovery",
                    # Exact native manual retirement is needed to release an
                    # unresolved child before Program recovery can complete.
                    "operation_reset",
                    "operation_acknowledge_completed",
                }
                or (
                    kind == "program_reset"
                    and (reset_retry_phase is not None or recovery_reset)
                )
                or (kind == "program_stop" and recovery_stop)
                for kind in allowed_by_state
            }
        allowed_commands = [
            {
                "command_kind": kind,
                "semantic_target": command_target,
                "allowed": allowed,
                "disabled_reason": (
                    None
                    if allowed
                    else (
                        "program_cycle_reconciliation_required"
                        if cycle.reconciliation_required
                        else f"program_cycle_state_{cycle.state}"
                    )
                ),
            }
            for kind, allowed in sorted(allowed_by_state.items())
        ]
        failure = cycle.failure
        if (
            cycle.reconciliation_required
            and failure == {"code": "program_cycle_reconciliation_required"}
        ):
            pending = [
                row
                for row in journal.pending_commands()
                if row["cycle_generation"] == cycle.cycle_generation
            ]
            if len(pending) == 1:
                failure = self._unknown_control_failure(pending[0]["kind"])
        status = ProgramCycleStatusDTO.model_validate(
            {
                "program_runner_owner_epoch": cycle.owner_epoch,
                "cycle_generation": cycle.cycle_generation,
                "state": cycle.state,
                "program_run_id": cycle.program_run_id,
                "cycle_claim_generation": (
                    None if cycle.claim is None else cycle.claim.get("claim_generation")
                ),
                "terminal_or_settlement_reference": cycle.terminal_reference,
                "execution_control_revision": cycle.execution_control_revision,
                "causal_correlation_id": cycle.causal_correlation_id,
                "causal_session_id": cycle.causal_session_id,
                "causal_client_sequence": cycle.causal_client_sequence,
                "current_command_kind": cycle.current_command_kind,
                "failure": failure,
                "reset_phase": cycle.reset_phase,
                "allowed_commands": allowed_commands,
            }
        )
        return status.model_dump(mode="json")

    def cycle_status(self) -> dict[str, Any]:
        result = self._cycle_status_core()
        program_run_id = result["program_run_id"]
        controls = (
            None if program_run_id is None else self._optional_controls(program_run_id)
        )
        recovery_offer = self._recovery_offer(
            program_run_id,
            cycle_generation=result["cycle_generation"],
        )
        run = None
        selections = None
        if program_run_id is not None:
            try:
                if controls is None:
                    run = self.read_run(program_run_id)
                else:
                    live, selections = self._owner.read_selection_controls(
                        program_run_id
                    )
                    run = self._project_run(live.to_dict())
            except ProgramRunOwnerError:
                # Retired runs have a journal snapshot but no live controls.
                try:
                    run = self.read_run(program_run_id)
                except ProgramRunnerError:
                    run = None
            except (KeyError, ProgramRunnerError):
                # The cycle journal deliberately outlives an in-memory run.
                # Revision hints are nullable and must not make an otherwise
                # valid terminal/reconciliation cycle unreadable.
                run = None
        result["run_revision"] = None if run is None else run["run_revision"]
        result["node_status_revision"] = (
            None if run is None else run["node_status_revision"]
        )
        result["runtime_control_revision"] = (
            None if controls is None else controls.revision
        )
        result["recovery_offer"] = recovery_offer
        # This is the single shared browser observation. Publishing the exact
        # projections here prevents every connected browser from re-reading
        # the same run and controls after receiving their revision hints.
        result["run"] = run
        result["runtime_controls"] = (
            None
            if run is None or controls is None or selections is None
            else self._runtime_controls_projection(
                program_run_id,
                live=run,
                controls=controls,
                recovery_offer=recovery_offer,
                execution_selections=selections,
            )
        )
        # Refine Program-tree-dependent availability in the complete owner
        # projection. The cycle journal remains the lifecycle authority; the
        # retained tree only narrows commands whose meaning depends on its
        # exact Full-stop/runtime state.
        run_state = None if run is None else run.get("state")
        cycle_running = result["state"] == "running"
        base_permissions = {
            command["command_kind"]: command["allowed"]
            for command in result["allowed_commands"]
        }
        reconciliation_fenced = any(
            command["disabled_reason"] == "program_cycle_reconciliation_required"
            for command in result["allowed_commands"]
        )

        def run_allows(kind: str, *states: str) -> bool:
            return bool(
                base_permissions.get(kind, False)
                and cycle_running
                and run_state in states
            )

        stoppable = ("running", "intervention_required")
        exact_run_permissions = {
            "program_motion_resume": run_allows("program_motion_resume", *stoppable),
            # A nominal held Stop may retain an interrupted task awaiting
            # cleanup. Its intervention state must not prevent that cleanup;
            # the exact Kernel proof still rejects faulted or unsettled work.
            "program_manual_handover": run_allows(
                "program_manual_handover", *stoppable
            ),
            "program_motion_stop": run_allows("program_motion_stop", *stoppable),
            "program_full_stop": run_allows("program_full_stop", *stoppable),
            # Starting has no retained Program tree yet, so its authoritative
            # cycle permission must survive this tree-dependent refinement.
            # Program Stop is the one Stop form that can overtake acquisition.
            "program_stop": base_permissions.get("program_stop", False)
            and (
                result["state"] == "starting"
                or cycle_running
                and run_state in {"running", "intervention_required", "full_stopping"}
                or result["state"] == "recovery_required"
                and selections is not None
                and run_state
                in {"running", "intervention_required", "full_stopping", "full_stopped"}
            ),
            "program_reset": base_permissions.get("program_reset", False)
            and (not reconciliation_fenced or selections is not None)
            and (
                not cycle_running
                or run_state in {"running", "intervention_required", "full_stopped"}
            ),
            "program_resume_full_stop": (
                not reconciliation_fenced
                and cycle_running
                and run_state == "full_stopped"
            ),
            "program_discard_full_stop": (
                not reconciliation_fenced
                and cycle_running
                and run_state == "full_stopped"
            ),
            **{
                kind: run_allows(kind, "running")
                for kind in (
                    "program_runtime_override_stage",
                    "program_runtime_override_activate",
                    "program_runtime_override_clear",
                    "program_runtime_override_promote",
                    "program_runtime_blackboard_write",
                    "program_manual_result_stage",
                )
            },
        }
        for command in result["allowed_commands"]:
            kind = command["command_kind"]
            if kind not in exact_run_permissions:
                continue
            allowed = exact_run_permissions[kind]
            command["allowed"] = allowed
            command["disabled_reason"] = (
                None if allowed else f"program_run_state_{run_state or 'unavailable'}"
            )
        return result

    def _controls(self, program_run_id: str) -> ProgramRuntimeControls:
        with self._runtime_controls_lock:
            controls = self._runtime_controls.get(program_run_id)
            if controls is not None:
                return controls
            run = self.read_run(program_run_id)
            capture = run.get("capture")
            if not isinstance(capture, Mapping):
                raise ProgramRunnerError(
                    "program_runtime_controls_unavailable",
                    "retained Program run has no exact capture",
                )
            retained = (
                None
                if self._journal is None
                else self._journal.read_runtime_controls(program_run_id)
            )
            recovery_fences = None
            if retained is None and self._journal is not None:
                cycle = self._journal.read()
                if cycle.program_run_id == program_run_id and isinstance(
                    cycle.capture_identities, Mapping
                ):
                    value = cycle.capture_identities.get("runtime_recovery_fences")
                    if isinstance(value, Mapping):
                        recovery_fences = value
            try:
                controls = (
                    ProgramRuntimeControls(
                        capture,
                        recovery_fences=recovery_fences,
                    )
                    if retained is None
                    else ProgramRuntimeControls.restore(capture, retained)
                )
            except ValueError as error:
                raise ProgramRunnerError(
                    "program_runtime_controls_invalid",
                    str(error),
                ) from error
            self._runtime_controls[program_run_id] = controls
            return controls

    def _optional_controls(self, program_run_id: str) -> ProgramRuntimeControls | None:
        """Read C2 controls without making legacy terminal cycles unreadable."""

        try:
            return self._controls(program_run_id)
        except ProgramRunnerError as error:
            if error.code not in {
                "program_run_not_found",
                "program_runtime_controls_unavailable",
            }:
                raise
            return None

    def _recovery_offer(
        self,
        program_run_id: str | None,
        *,
        cycle_generation: int,
        selection_scope=None,
    ) -> dict[str, Any] | None:
        if program_run_id is None:
            return None
        # A node-level recovery offer is valid only while its owning Program
        # cycle is current and free of unresolved control effects.  Keep this
        # check here so status, controls, results, and rejection responses
        # cannot disagree about whether the offer is actionable.
        if self._journal is not None:
            cycle = self._journal.read()
            if not (
                cycle.state == "running"
                and not cycle.reconciliation_required
                and cycle.program_run_id == program_run_id
                and cycle.cycle_generation == cycle_generation
            ):
                return None
        intervention = self._owner.intervention(program_run_id)
        if intervention is None:
            return None
        if intervention.policy_parameter == "operator_stop_policy":
            self._owner.refresh_operator_stop_intervention(program_run_id)
            intervention = self._owner.intervention(program_run_id)
            if intervention is None:
                return None
        controls = self._optional_controls(program_run_id)
        if controls is None:
            return None
        try:
            live = self._owner.read(program_run_id)
        except ProgramRunOwnerError:
            return None
        policy = live.capture.node_policy_by_instance_id.get(
            intervention.identity.node_instance_id
        )
        selection_revision = None
        if intervention.policy_parameter == "operator_stop_policy":
            if selection_scope is None:
                selection_scope = self._owner.operator_stop_scope(program_run_id)
            if intervention.identity.node_instance_id in selection_scope.held_nodes:
                selection_revision = selection_scope.selection_revision
        return controls.recovery_offer(
            cycle_generation=cycle_generation,
            owner_epoch=self.owner_epoch,
            intervention=intervention,
            policy={} if policy is None else policy.recovery_policy,
            selection_revision=selection_revision,
        )

    @staticmethod
    def _runtime_override_candidate_unavailable(
        program_run_id: str,
        *,
        reason_code: str,
        reason: str,
        node_instance_id: str | None = None,
        node_attempt_id: str | None = None,
        node_id: str | None = None,
    ) -> dict[str, Any]:
        return {
            "schema_version": ("manyforge.runtime_override_observation_candidate.v1"),
            "available": False,
            "reason_code": reason_code,
            "reason": reason,
            "program_run_id": program_run_id,
            "node_instance_id": node_instance_id,
            "node_attempt_id": node_attempt_id,
            "node_id": node_id,
            "parameter_id": None,
            "value": None,
            "source_observation_id": None,
            "source_owner_generation": None,
        }

    def _runtime_override_observation_candidate(
        self, program_run_id: str
    ) -> dict[str, Any]:
        """Project one exact owner-derived Robot snapshot staging candidate."""

        try:
            intervention = self._owner.intervention(program_run_id)
        except ProgramRunOwnerError as error:
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="program_attempt_context_unavailable",
                reason=str(error),
            )
        if intervention is None:
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="program_attempt_not_held",
                reason="Stage requires the exact held Program node attempt.",
            )
        node_instance_id = intervention.identity.node_instance_id
        node_attempt_id = intervention.identity.node_attempt_id
        try:
            live = self._owner.read(program_run_id)
            context = self._owner.runtime_override_source_context(
                program_run_id,
                node_instance_id=node_instance_id,
            )
        except ProgramRunOwnerError as error:
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="program_attempt_context_unavailable",
                reason=str(error),
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
            )
        node_id = context["node_id"]
        parameters = context["parameters"]
        if (
            not isinstance(parameters, Mapping)
            or parameters.get("target_mode", "absolute") != "absolute"
            or node_id not in {"motion.move_to_joint_target", "motion.move_to_pose"}
        ):
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="robot_snapshot_override_not_supported",
                reason=(
                    "The held node does not accept an absolute Robot snapshot "
                    "as its goal."
                ),
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                node_id=node_id,
            )
        try:
            targets_projection = self._kernel.targets()
            control_projection = self._kernel.control_state()
        except Exception as error:
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="robot_observation_unavailable",
                reason=str(error) or "The current Robot observation is unavailable.",
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                node_id=node_id,
            )
        targets = targets_projection.get("targets")
        control = control_projection.get("control_state")
        identity = targets_projection.get("identity")
        direct = targets.get("direct_control") if isinstance(targets, Mapping) else None
        observed = (
            control.get("observed_motion") if isinstance(control, Mapping) else None
        )
        capture = live.capture
        if (
            targets_projection.get("ok") is not True
            or control_projection.get("ok") is not True
            or identity != control_projection.get("identity")
            or not isinstance(identity, Mapping)
            or not isinstance(direct, Mapping)
            or not isinstance(observed, Mapping)
            or identity.get("graph_revision") != capture.graph_revision
            or identity.get("retained_bundle_id") != capture.retained_bundle_id
            or direct.get("program_binding_id") != capture.program_binding_id
            or direct.get("target_role_id") != parameters.get("target_role")
            or direct.get("base_frame_role_id") != parameters.get("base_frame_role")
            or direct.get("tcp_frame_role_id") != parameters.get("tcp_frame_role")
            or observed.get("complete") is not True
            or not isinstance(observed.get("source_epoch"), str)
            or not observed["source_epoch"]
            or type(observed.get("sequence")) is not int
            or observed["sequence"] < 1
        ):
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="robot_observation_identity_mismatch",
                reason=(
                    "The execution owner has no complete Robot observation "
                    "for this exact Program target."
                ),
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                node_id=node_id,
            )
        if node_id == "motion.move_to_joint_target":
            names = observed.get("source_joint_names")
            positions = observed.get("positions_rad")
            if (
                not isinstance(names, list)
                or not isinstance(positions, list)
                or not names
                or len(names) != len(positions)
                or any(not isinstance(name, str) or not name for name in names)
                or any(
                    isinstance(position, bool)
                    or not isinstance(position, (int, float))
                    or not math.isfinite(position)
                    for position in positions
                )
            ):
                return self._runtime_override_candidate_unavailable(
                    program_run_id,
                    reason_code="robot_observation_value_invalid",
                    reason="The current joint observation is incomplete.",
                    node_instance_id=node_instance_id,
                    node_attempt_id=node_attempt_id,
                    node_id=node_id,
                )
            value: Any = [
                {"joint_name": name, "position_rad": position}
                for name, position in zip(names, positions, strict=True)
            ]
        else:
            pose = observed.get("current_tcp_pose")
            position = pose.get("position_m") if isinstance(pose, Mapping) else None
            orientation = pose.get("orientation") if isinstance(pose, Mapping) else None
            if not isinstance(position, Mapping) or not isinstance(
                orientation, Mapping
            ):
                return self._runtime_override_candidate_unavailable(
                    program_run_id,
                    reason_code="robot_observation_value_invalid",
                    reason="The current TCP observation is incomplete.",
                    node_instance_id=node_instance_id,
                    node_attempt_id=node_attempt_id,
                    node_id=node_id,
                )
            try:
                position_value = [position[field] for field in ("x_m", "y_m", "z_m")]
                orientation_value = [
                    orientation[field] for field in ("x", "y", "z", "w")
                ]
            except KeyError:
                return self._runtime_override_candidate_unavailable(
                    program_run_id,
                    reason_code="robot_observation_value_invalid",
                    reason="The current TCP observation is incomplete.",
                    node_instance_id=node_instance_id,
                    node_attempt_id=node_attempt_id,
                    node_id=node_id,
                )
            if any(
                isinstance(component, bool)
                or not isinstance(component, (int, float))
                or not math.isfinite(component)
                for component in (*position_value, *orientation_value)
            ):
                return self._runtime_override_candidate_unavailable(
                    program_run_id,
                    reason_code="robot_observation_value_invalid",
                    reason="The current TCP observation is incomplete.",
                    node_instance_id=node_instance_id,
                    node_attempt_id=node_attempt_id,
                    node_id=node_id,
                )
            value = {
                "position_m": position_value,
                "orientation": {"xyzw": orientation_value},
            }
            authored_goal = parameters.get("goal")
            if isinstance(authored_goal, Mapping) and "frame_ref" in authored_goal:
                value["frame_ref"] = deepcopy(authored_goal["frame_ref"])
        try:
            validated = self._owner.validate_runtime_override(
                program_run_id,
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                parameter_id="goal",
                value=value,
            )
        except ProgramRunOwnerError as error:
            return self._runtime_override_candidate_unavailable(
                program_run_id,
                reason_code="robot_snapshot_override_rejected",
                reason=str(error),
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                node_id=node_id,
            )
        return {
            "schema_version": ("manyforge.runtime_override_observation_candidate.v1"),
            "available": True,
            "reason_code": None,
            "reason": None,
            "program_run_id": program_run_id,
            "node_instance_id": node_instance_id,
            "node_attempt_id": node_attempt_id,
            "node_id": node_id,
            "parameter_id": "goal",
            "value": validated,
            "source_observation_id": observed["source_epoch"],
            "source_owner_generation": observed["sequence"],
        }

    @staticmethod
    def _run_recovery_fences(
        applied: Mapping[str, Any],
        execution: Mapping[str, Any],
        *,
        scene_admission_epoch: int,
    ) -> dict[str, int]:
        source = applied.get("source")
        status = execution.get("identity")
        scene_revision = (
            status.get("scene_revision") if isinstance(status, Mapping) else None
        )
        if (
            execution.get("ok") is not True
            or not isinstance(source, Mapping)
            or not isinstance(status, Mapping)
            or status.get("retained_bundle_id") != applied.get("retained_bundle_id")
            or status.get("graph_revision") != applied.get("graph_revision")
            or type(source.get("source_generation")) is not int
            or source["source_generation"] < 1
            or type(status.get("active_publication_generation")) is not int
            or status["active_publication_generation"] < 1
            or not isinstance(scene_revision, Mapping)
            or type(scene_revision.get("revision_id")) is not int
            or scene_revision["revision_id"] < 1
        ):
            raise ProgramRunnerError(
                "program_run_recovery_fence_unavailable",
                "current owners did not provide exact recovery generations",
            )
        return {
            "program_generation": source["source_generation"],
            "deployment_generation": status["active_publication_generation"],
            "scene_admission_epoch": scene_admission_epoch,
            "pose_source_apply_generation": 0,
        }

    def _project_run(self, value: Mapping[str, Any]) -> dict[str, Any]:
        # Program-run status v1 stays a stable owner contract. C2 state is a
        # separate joined projection so adding controls never weakens or
        # silently version-skews existing run consumers.
        return deepcopy(dict(value))

    def _terminalize_failed_start(
        self,
        journal: ProgramCycleJournal,
        *,
        scene_proof: Mapping[str, Any] | None,
        effect_may_have_started: bool,
        started_snapshot: Mapping[str, Any] | None = None,
    ) -> None:
        current = journal.read()
        if current.state == "terminal_awaiting_reset":
            return
        if current.claim is None:
            if current.state != "stopping" or not isinstance(scene_proof, Mapping):
                journal.require_reconciliation(
                    expected_generation=current.cycle_generation
                )
                return
            settlement = {
                "schema_version": "manyforge.program_cycle_start_settlement.v1",
                "kind": "proven_no_effect",
                "claim": None,
                "program_run_id": current.program_run_id,
                "prepared_scene": deepcopy(dict(scene_proof)),
            }
            journal.transition(
                expected_generation=current.cycle_generation,
                expected_states=("stopping",),
                state="terminal_awaiting_reset",
                terminal_reference=self._digest(settlement),
                start_settlement=settlement,
                program_run_id=current.program_run_id,
                command_kind="program_stop",
            )
            return
        if effect_may_have_started and started_snapshot is None:
            journal.require_reconciliation(expected_generation=current.cycle_generation)
            return
        settlement: dict[str, Any] = {
            "schema_version": "manyforge.program_cycle_start_settlement.v1",
            "kind": "proven_no_effect",
            "claim": deepcopy(current.claim),
            "program_run_id": current.program_run_id,
            "prepared_scene": deepcopy(dict(scene_proof or {})),
        }
        program_run_id = current.program_run_id
        terminal_snapshot: dict[str, Any] | None = None
        if started_snapshot is not None:
            capture = started_snapshot.get("capture")
            started_run_id = (
                capture.get("program_run_id") if isinstance(capture, Mapping) else None
            )
            if not isinstance(started_run_id, str) or not started_run_id:
                journal.require_reconciliation(
                    expected_generation=current.cycle_generation
                )
                return
            try:
                stopped = self._host.stop_run(
                    started_run_id,
                    reason="Program Start settlement failed",
                ).to_dict()
            except Exception:
                journal.require_reconciliation(
                    expected_generation=current.cycle_generation
                )
                return
            if stopped.get("state") not in {"stopped", "succeeded", "failed"}:
                journal.require_reconciliation(
                    expected_generation=current.cycle_generation
                )
                return
            settlement = {
                **settlement,
                "kind": "terminal_effects_settled",
                "program_run_id": started_run_id,
                "terminal_run": stopped,
            }
            terminal_snapshot = stopped
            program_run_id = started_run_id
        reference = self._digest(settlement)
        try:
            journal.transition(
                expected_generation=current.cycle_generation,
                expected_states=(
                    "idle_after_reset",
                    "starting",
                    "running",
                    "stopping",
                ),
                state="terminal_awaiting_reset",
                terminal_reference=reference,
                start_settlement=settlement,
                program_run_id=program_run_id,
                run_snapshot=terminal_snapshot,
            )
        except Exception as error:
            try:
                journal.require_reconciliation(
                    expected_generation=current.cycle_generation
                )
            except Exception:
                pass
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required",
                "failed Program Start settlement was not durably retained",
            ) from error

    def _cycle_terminal_evidence_is_exact(
        self, journal: ProgramCycleJournal, cycle: Any
    ) -> bool:
        reference = cycle.terminal_reference
        program_run_id = cycle.program_run_id
        if not isinstance(reference, str) or not isinstance(program_run_id, str):
            return False
        settlement = cycle.start_settlement
        if settlement is not None:
            prepared_scene = settlement.get("prepared_scene")
            if (
                settlement.get("schema_version")
                != "manyforge.program_cycle_start_settlement.v1"
                or settlement.get("claim") != cycle.claim
                or not isinstance(prepared_scene, Mapping)
                or prepared_scene.get("schema_version")
                != "manyforge.prepared_program_scene.v1"
                or settlement.get("kind")
                not in {"proven_no_effect", "terminal_effects_settled"}
                or self._digest(settlement) != reference
            ):
                return False
            if settlement["kind"] == "proven_no_effect":
                return settlement.get("program_run_id") == program_run_id
            terminal = settlement.get("terminal_run")
            capture = terminal.get("capture") if isinstance(terminal, Mapping) else None
            return bool(
                isinstance(capture, Mapping)
                and capture.get("program_run_id") == program_run_id
                and terminal.get("state") in {"stopped", "succeeded", "failed"}
                and journal.read_run_snapshot(program_run_id) == terminal
            )
        terminal = journal.read_run_snapshot(program_run_id)
        capture = terminal.get("capture") if isinstance(terminal, Mapping) else None
        return bool(
            isinstance(capture, Mapping)
            and capture.get("program_run_id") == program_run_id
            and terminal.get("state") in {"stopped", "succeeded", "failed"}
            and self._digest(
                {
                    "program_run_id": program_run_id,
                    "run_revision": terminal.get("run_revision"),
                    "state": terminal.get("state"),
                }
            )
            == reference
        )

    def _prepare_run_scene(
        self,
        run_admission: Any,
        provisional_run_id: str,
        *,
        program_reset_scene_authorization: Mapping[str, Any] | None = None,
    ) -> tuple[Any, Sequence[Mapping[str, Any]], dict[str, Any]]:
        context = run_admission.port_context(provisional_run_id)
        initial_scene = run_admission.program.to_dict().get("scene", [])
        if program_reset_scene_authorization is None:
            proof = self._scene.prepare_initial_scene(context, initial_scene)
        else:
            proof = self._scene.prepare_initial_scene(
                context,
                initial_scene,
                program_reset_scene_authorization=(program_reset_scene_authorization),
            )
        return context, initial_scene, proof

    def _verify_run_scene(
        self,
        context: Any,
        initial_scene: Sequence[Mapping[str, Any]],
        proof: Mapping[str, Any],
    ) -> None:
        self._scene.verify_initial_scene(context, initial_scene, proof)

    def _prepare_current_applied_scene(
        self,
        *,
        expected_record_digest: str | None,
        context_id: str,
        program_reset_scene_authorization: Mapping[str, Any] | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """Converge and verify one exact Applied Program scene.

        This is the single idempotent primitive shared by activation and Reset.
        It owns no cycle transition and always releases its temporary scene
        context; callers decide when their exact lifecycle fence permits it.
        """

        applied = self._headless.program_applied()
        source = applied.get("source")
        if (
            not isinstance(source, Mapping)
            or not isinstance(source.get("record_digest"), str)
            or (
                expected_record_digest is not None
                and source.get("record_digest") != expected_record_digest
            )
        ):
            raise ProgramRunnerError(
                "program_run_applied_selection_stale",
                "Applied Program changed before scene preparation",
            )
        scenario_id = applied.get("scenario_id")
        scenario_catalog_revision = applied.get("scenario_catalog_revision")
        if not isinstance(scenario_id, str) or not isinstance(
            scenario_catalog_revision, str
        ):
            raise ProgramRunnerError(
                "program_run_applied_selection_stale",
                "Applied Program omitted its Scenario identity",
            )
        live = self._headless.scenario_program_selection(scenario_id)
        try:
            run_admission = build_applied_program_run_admission(
                applied,
                live,
                expected_scenario_id=scenario_id,
                expected_scenario_catalog_revision=scenario_catalog_revision,
                environment=self._environment,
                owner_availability=self._owner_availability,
            )
            if program_reset_scene_authorization is None:
                context, initial_scene, proof = self._prepare_run_scene(
                    run_admission,
                    context_id,
                )
            else:
                context, initial_scene, proof = self._prepare_run_scene(
                    run_admission,
                    context_id,
                    program_reset_scene_authorization=(
                        program_reset_scene_authorization
                    ),
                )
            self._verify_run_scene(context, initial_scene, proof)
            if self._headless.program_applied() != applied:
                raise ProgramRunnerError(
                    "program_run_applied_selection_stale",
                    "Applied Program changed during scene preparation",
                )
            return deepcopy(applied), deepcopy(proof)
        finally:
            self._scene.release(context_id)

    def _prepare_scene_after_reset(
        self,
        request: ProgramCycleResetRequest,
        authorization: Mapping[str, Any] | None,
    ) -> None:
        """Converge the current Applied scene before Reset publishes Idle.

        The exact Program claim and durable ``resetting`` projection authorize
        this owner-internal restoration without reopening ordinary authoring.
        The claim is released only after exact scene readback. No new Run or
        external scene edit can overtake the idempotent convergence, and a
        failure remains backend-visible as ``recovery_required`` instead of
        exposing a false Idle state.
        """

        context_id = (
            "program-reset-scene."
            + self._digest(
                {
                    "request_id": request.request_id,
                    "request_owner_epoch": request.request_owner_epoch,
                    "program_run_id": request.program_run_id,
                    "program_runner_owner_epoch": request.expected_owner_epoch,
                    "reset_cycle_generation": request.expected_cycle_generation,
                    "terminal_reference": request.terminal_reference,
                }
            )[7:39]
        )
        try:
            self._prepare_current_applied_scene(
                expected_record_digest=None,
                context_id=context_id,
                program_reset_scene_authorization=authorization,
            )
        except ProgramRunnerError as error:
            raise ProgramRunnerError(
                "program_cycle_reset_scene_preparation_failed",
                str(error),
                details={"cause_code": error.code},
            ) from error
        except HeadlessClientError as error:
            raise ProgramRunnerError(
                "program_cycle_reset_scene_preparation_failed",
                str(error),
                details={"cause_code": error.code},
            ) from error
        except (ProgramRunOwnerError, ProgramSchedulingError) as error:
            cause = self._runner_error(error)
            raise ProgramRunnerError(
                "program_cycle_reset_scene_preparation_failed",
                str(cause),
                details={"cause_code": cause.code},
            ) from error
        except Exception as error:
            raise ProgramRunnerError(
                "program_cycle_reset_scene_preparation_failed",
                str(error) or type(error).__name__,
            ) from error

    def _clear_reset_stop_fault(
        self,
        request: ProgramCycleResetRequest,
    ) -> None:
        """Consume only the current exact Kernel Stop offer during Reset."""

        for attempt in range(8):
            response = self._kernel.stop_fault()
            offer = response.get("stop_fault")
            if offer is None:
                return
            if not isinstance(offer, Mapping):
                raise ProgramRunnerError(
                    "program_cycle_reset_stop_recovery_invalid",
                    "Kernel returned an invalid Stop recovery offer",
                )
            latch = offer.get("latch")
            expectations = offer.get("configuration_expectations")
            certificate = (
                latch.get("stop_certificate") if isinstance(latch, Mapping) else None
            )
            identity = self._kernel.identity().get("identity")
            if (
                not isinstance(latch, Mapping)
                or not isinstance(expectations, list)
                or not isinstance(certificate, Mapping)
                or not isinstance(identity, Mapping)
                or not isinstance(identity.get("owner_epoch"), str)
            ):
                raise ProgramRunnerError(
                    "program_cycle_reset_stop_recovery_invalid",
                    "Kernel omitted exact Stop recovery evidence",
                )
            recovery_id = (
                "program-reset-recovery-"
                + hashlib.sha256(
                    (
                        request.request_owner_epoch
                        + "\0"
                        + request.request_id
                        + "\0"
                        + str(attempt)
                    ).encode("utf-8")
                ).hexdigest()[:32]
            )
            result = self._kernel.recover_stop_fault(
                {
                    "request_context": {
                        "request_id": recovery_id,
                        "request_owner_epoch": identity["owner_epoch"],
                        "source_label": "program_runner_cycle_reset",
                        "correlation_id": request.correlation_id or request.request_id,
                    },
                    "expected_latch_id": latch.get("latch_id"),
                    "expected_latch_generation": latch.get("latch_generation"),
                    "expected_recovery_assessment_revision": latch.get(
                        "recovery_assessment_revision"
                    ),
                    "expected_stop_certificate_id": certificate.get(
                        "stop_certificate_id"
                    ),
                    "expected_stop_certificate_revision": certificate.get(
                        "stop_certificate_revision"
                    ),
                    "expected_cell_id": latch.get("cell_id"),
                    "expected_retained_bundle_id": latch.get("retained_bundle_id"),
                    "expected_graph_revision": latch.get("graph_revision"),
                    "configuration_expectations": expectations,
                    "reason": "Program cycle Reset settled its controlled Stop",
                }
            ).get("result")
            if (
                not isinstance(result, Mapping)
                or type(result.get("accepted")) is not bool
            ):
                raise ProgramRunnerError(
                    "program_cycle_reset_stop_recovery_invalid",
                    "Kernel returned an invalid Stop recovery result",
                )
        raise ProgramRunnerError(
            "program_cycle_reset_stop_recovery_pending",
            "Kernel Stop recovery did not settle within its bounded attempts",
        )

    def prepare_applied_scene(
        self, request: ProgramScenePreparationRequest
    ) -> dict[str, Any]:
        """Converge the current Applied Program scene without acquiring Run."""

        if not isinstance(request, ProgramScenePreparationRequest):
            raise TypeError(
                "prepare_applied_scene requires one ProgramScenePreparationRequest"
            )
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
                cycle.state != "idle_after_reset"
                or cycle.reconciliation_required
                or cycle.owner_epoch != self.owner_epoch
                or cycle.acquisition_pending is not None
            ):
                raise ProgramRunnerError(
                    "authoring_not_ready",
                    "Applied Program scene preparation requires authoritative Idle",
                )
            admission = admission_client.get()
            generation = self._admission_generation(admission)
            if (
                admission.get("authoring_allowed") is not True
                or generation != request.expected_admission_generation
            ):
                raise ProgramRunnerError(
                    "stale_generation",
                    "authoring admission changed before scene preparation",
                )
            context_id = (
                "program-scene."
                + self._digest(
                    {
                        "request_id": request.request_id,
                        "request_owner_epoch": request.request_owner_epoch,
                        "applied_record_digest": (
                            request.expected_applied_record_digest
                        ),
                    }
                )[7:39]
            )
            try:
                applied, proof = self._prepare_current_applied_scene(
                    expected_record_digest=request.expected_applied_record_digest,
                    context_id=context_id,
                )
                resulting_generation = self._admission_generation(
                    admission_client.get()
                )
                return {
                    "schema_version": "manyforge.program_scene_preparation.v1",
                    "status": "prepared",
                    "applied_record_digest": request.expected_applied_record_digest,
                    "program_revision": applied.get("program_revision"),
                    "admission_generation": resulting_generation,
                    "prepared_scene": deepcopy(proof),
                }
            except ProgramRunnerError:
                raise
            except HeadlessClientError as error:
                raise self._headless_error(error) from error
            except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                raise self._runner_error(error) from error
            except Exception as error:
                raise ProgramRunnerError(
                    "program_scene_preparation_failed",
                    str(error) or type(error).__name__,
                ) from error

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

    def node_availability(
        self, request: ProgramNodeAvailabilityRequest
    ) -> dict[str, Any]:
        """Project the exact availability already enforced by Run admission."""

        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is not started",
            )
        self._require_manager_current()
        admission_client, journal = self._require_cycle_owner()
        admission_generation = self._admission_generation(admission_client.get())
        cycle = journal.read()
        try:
            applied = request.applied
            scenario_id = applied.get("scenario_id")
            scenario_revision = applied.get("scenario_catalog_revision")
            if not isinstance(scenario_id, str) or not isinstance(
                scenario_revision, str
            ):
                raise ProgramRunOwnerError(
                    "Applied Program omitted its Scenario identity"
                )
            live = request.selection
            owner_availability = self._current_node_owner_availability(
                applied,
                admission_generation=admission_generation,
            )
            admitted = build_applied_program_run_admission(
                applied,
                live,
                expected_scenario_id=scenario_id,
                expected_scenario_catalog_revision=scenario_revision,
                environment=self._environment,
                owner_availability=owner_availability,
            )
            required_node_ids: set[str] = set()

            def collect(node: Any) -> None:
                required_node_ids.add(node.node_id)
                for child in node.children:
                    collect(child)

            collect(admitted.program.tree)
            availability = project_node_execution_availability(
                build_executable_node_resolver(self._environment),
                admitted.readiness,
                required_node_ids=tuple(sorted(required_node_ids)),
            )
        except (ProgramRunOwnerError, TypeError, ValueError) as error:
            raise ProgramRunnerError(
                "program_node_availability_incompatible",
                str(error) or type(error).__name__,
            ) from error
        source_value = {
            "admission_generation": admission_generation,
            "applied_record_digest": admitted.applied_record_digest,
            "cycle_generation": cycle.cycle_generation,
            "graph_revision": admitted.graph_revision,
            "node_environment_digest": self._environment.environment_digest,
            "owner_epoch": self.owner_epoch,
            "owner_id": self.owner_id,
            "program_binding_id": admitted.program_binding_id,
            "program_id": admitted.program.program_id,
            "program_revision": admitted.program.program_revision,
            "retained_bundle_id": admitted.retained_bundle_id,
            "scenario_id": admitted.scenario_id,
        }
        return {
            "schema_version": "manyforge.program_node_availability.v1",
            "source": {
                **source_value,
                "content_digest": self._digest(
                    {"availability": availability, **source_value}
                ),
            },
            "availability": availability,
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
        values = [self._project_run(snapshot.to_dict()) for snapshot in snapshots]
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
        if isinstance(value, Mapping):
            value = self._project_run(value)
        if (
            self._journal is None
            or not isinstance(value, Mapping)
            or value.get("state")
            not in {
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
                cycle.state
                not in {
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
                        "execution_control_ingress_busy"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            self._require_new_command(record)
            acquired_for_request = False
            acquire_attempted = False
            starting_committed = False
            scene_context_id: str | None = None
            scene_proof: dict[str, Any] | None = None
            started_snapshot: dict[str, Any] | None = None
            host_start_invoked = False

            def release_scene_context() -> None:
                nonlocal scene_context_id
                if scene_context_id is not None:
                    self._scene.release(scene_context_id)
                    scene_context_id = None

            def retain_unacquired_start_uncertainty() -> None:
                nonlocal starting_committed
                if not starting_committed or acquired_for_request:
                    return
                if not acquire_attempted:
                    try:
                        journal.cancel_rejected_acquire(
                            expected_generation=cycle.cycle_generation
                        )
                    finally:
                        starting_committed = False
                    return
                try:
                    journal.require_reconciliation(
                        expected_generation=cycle.cycle_generation
                    )
                finally:
                    starting_committed = False

            def abort_if_stop_won() -> None:
                """Prevent an accepted Stop from being overtaken by this Run."""

                nonlocal starting_committed
                current = journal.read()
                if (
                    current.cycle_generation != cycle.cycle_generation
                    or current.program_run_id != program_run_id
                    or current.state != "stopping"
                ):
                    return
                if current.claim is not None and not host_start_invoked:
                    self._native.scope.request_stop(
                        program_run_id,
                        f"start-race-stop:{request.request_id}",
                        "accepted Program Stop overtook Run admission",
                    )
                self._terminalize_failed_start(
                    journal,
                    scene_proof=scene_proof,
                    effect_may_have_started=host_start_invoked,
                    started_snapshot=started_snapshot,
                )
                starting_committed = False
                raise ProgramRunnerError(
                    "program_run_superseded_by_stop",
                    "Program Stop was accepted before Run admission completed",
                )

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
                live = self._headless.scenario_program_selection(request.scenario_id)
                if request.draft_program_revision != applied.get("program_revision"):
                    raise ProgramRunnerError(
                        "program_run_draft_applied_mismatch",
                        "Draft and Applied Program revisions differ",
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
                # One identity spans the synchronous Program-cycle claim,
                # scene context, Program owner, and every Kernel operation.
                # The Kernel can therefore fence a Run before its first
                # ordinary motion submission arrives.
                program_run_id = f"program-cycle.{cycle.cycle_generation}"
                scene_context_id = program_run_id
                claim_intent = {
                    "claim_kind": "program_cycle",
                    "owner_role": "program_runner",
                    "owner_epoch": self.owner_epoch,
                    "owner_transaction_id": program_run_id,
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
                # Starting is backend truth before scene convergence or owner
                # acquisition can take noticeable time.  The pending evidence
                # is refreshed below if scene commits advance admission.
                journal.begin_acquire(
                    expected_generation=cycle.cycle_generation,
                    provisional_run_id=program_run_id,
                    evidence=claim_intent,
                    correlation_id=request.correlation_id or request.request_id,
                    session_id=request.session_id,
                    client_sequence=request.client_sequence,
                )
                starting_committed = True
                scene_context, initial_scene, scene_proof = self._prepare_run_scene(
                    run_admission, program_run_id
                )
                abort_if_stop_won()
                execution_identity = self._kernel.identity()
                kernel_epoch = execution_identity["identity"].get("owner_epoch")
                if not isinstance(kernel_epoch, str) or not kernel_epoch:
                    raise ProgramRunnerError(
                        "execution_command_response_invalid",
                        "execution identity omitted the Kernel owner epoch",
                    )
                self._starting_kernel_owner_epoch = kernel_epoch
                abort_if_stop_won()
                # Scene preparation may have consumed one or more short
                # authored-scene claims. Acquire Run only from the fresh
                # generation that follows those exact owner commits.
                owner_admission = admission_client.get()
                generation = self._admission_generation(owner_admission)
                claim_intent["expected_admission_generation"] = generation
                try:
                    journal.refresh_pending_acquire(
                        expected_generation=cycle.cycle_generation,
                        provisional_run_id=program_run_id,
                        evidence=claim_intent,
                    )
                except RuntimeError:
                    abort_if_stop_won()
                    raise
                try:
                    acquire_attempted = True
                    claim = admission_client.acquire(claim_intent)
                    acquired_for_request = True
                except CellAdmissionError as error:
                    if error.outcome_unknown:
                        journal.mark_acquire_unknown(
                            expected_generation=cycle.cycle_generation
                        )
                    else:
                        abort_if_stop_won()
                        journal.cancel_rejected_acquire(
                            expected_generation=cycle.cycle_generation
                        )
                        starting_committed = False
                    raise
                except Exception:
                    journal.mark_acquire_unknown(
                        expected_generation=cycle.cycle_generation
                    )
                    raise
                try:
                    journal.remember_acquired_claim(
                        expected_generation=cycle.cycle_generation,
                        provisional_run_id=program_run_id,
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
                abort_if_stop_won()
                # Re-read Applied after the Kernel has made mutation exclusion
                # authoritative, before any capture or execution effect.
                applied_after_claim = self._headless.program_applied()
                if applied_after_claim != applied:
                    raise ProgramRunnerError(
                        "program_run_applied_selection_stale",
                        "Applied Program changed during Run admission",
                    )
                self._verify_run_scene(
                    scene_context,
                    initial_scene,
                    scene_proof,
                )
                recovery_fences = self._run_recovery_fences(
                    applied_after_claim,
                    execution_identity,
                    scene_admission_epoch=generation,
                )
                self._kernel.register_program_run_capture(
                    {
                        "schema_version": "manyforge.program_run_execution_capture.v1",
                        "request_owner_epoch": execution_identity["identity"][
                            "owner_epoch"
                        ],
                        "program_runner_owner_epoch": self.owner_epoch,
                        "claim_generation": claim["claim_generation"],
                        "program_run_id": program_run_id,
                        "program_id": scene_context.program_id,
                        "program_revision": scene_context.program_revision,
                        "program_binding_id": scene_context.program_binding_id,
                        "retained_bundle_id": scene_context.retained_bundle_id,
                        "graph_revision": scene_context.graph_revision,
                    }
                )
                release_scene_context()
                abort_if_stop_won()
                host_start_invoked = True
                snapshot = self._host.start_run(
                    run_admission,
                    program_run_id=program_run_id,
                    defer_ticks=True,
                )
                if snapshot.capture.program_run_id != program_run_id:
                    raise ProgramRunnerError(
                        "program_run_identity_mismatch",
                        "Program owner did not retain the admitted Program-run identity",
                    )
                started_snapshot = snapshot.to_dict()
                abort_if_stop_won()
                with self._runtime_controls_lock:
                    self._runtime_controls[snapshot.capture.program_run_id] = (
                        ProgramRuntimeControls(
                            snapshot.capture.to_dict(),
                            recovery_fences=recovery_fences,
                        )
                    )
                result = self._project_run(snapshot.to_dict())
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
                identities["runtime_recovery_fences"] = recovery_fences
                identities["prepared_scene"] = deepcopy(scene_proof)
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
                # Capture first, then publish the durable Running owner state,
                # and only then enable the first effectful tree tick.
                self._host.schedule(program_run_id)
                journal.complete_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    result=result,
                )
            except HeadlessClientError as error:
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                    )
                else:
                    retain_unacquired_start_uncertainty()
                outcome = self._headless_error(error)
            except CellAdmissionError as error:
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                    )
                else:
                    retain_unacquired_start_uncertainty()
                outcome = ProgramRunnerError(error.code, str(error))
            except ProgramRunnerError as error:
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                    )
                else:
                    retain_unacquired_start_uncertainty()
                outcome = error
            except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                    )
                else:
                    retain_unacquired_start_uncertainty()
                outcome = self._runner_error(error)
            except Exception as error:
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                    )
                else:
                    retain_unacquired_start_uncertainty()
                outcome = ProgramRunnerError(
                    "program_run_admission_failed",
                    str(error) or type(error).__name__,
                )
            else:
                return result
            finally:
                self._starting_kernel_owner_epoch = None
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
            return self._project_run(self._owner.read(program_run_id).to_dict())
        except ProgramRunOwnerError as error:
            if self._journal is not None:
                retained = self._journal.read_run_snapshot(program_run_id)
                if retained is not None:
                    return retained
            raise ProgramRunnerError(
                "program_run_not_found",
                str(error),
            ) from error

    def _runtime_controls_projection(
        self,
        program_run_id: str,
        *,
        live: Mapping[str, Any],
        controls: ProgramRuntimeControls,
        recovery_offer: dict[str, Any] | None,
        execution_selections: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.program_runtime_control_projection.v4",
            "owner_id": self.owner_id,
            "owner_epoch": self.owner_epoch,
            "program_run_id": program_run_id,
            "run_revision": live["run_revision"],
            "runtime_controls": controls.snapshot(),
            "recovery_offer": recovery_offer,
            "execution_selections": execution_selections,
            "observation_override_candidate": (
                self._runtime_override_observation_candidate(program_run_id)
            ),
        }

    def read_runtime_controls(self, program_run_id: str) -> dict[str, Any]:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is closed"
            )
        controls = self._controls(program_run_id)
        cycle = self._journal.read() if self._journal is not None else None
        cycle_generation = 1 if cycle is None else cycle.cycle_generation
        recovery_offer = self._recovery_offer(
            program_run_id, cycle_generation=cycle_generation
        )
        try:
            live, selections = self._owner.read_selection_controls(program_run_id)
        except ProgramRunOwnerError as error:
            raise ProgramRunnerError("program_run_not_found", str(error)) from error
        return self._runtime_controls_projection(
            program_run_id,
            live=live.to_dict(),
            controls=controls,
            recovery_offer=recovery_offer,
            execution_selections=selections,
        )

    def _control_effect(self, request, *, kind: str, effect) -> dict[str, Any]:
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is not started"
            )
        self._require_manager_current()
        with self._transition_lock:
            self._require_manager_current()
            _admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            intent = request.model_dump(mode="json")
            try:
                record = journal.record_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    kind=kind,
                    intent=intent,
                    cycle_generation=cycle.cycle_generation,
                )
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    (
                        "execution_control_ingress_busy"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            self._require_new_command(record)

            def reject(error: ProgramRunnerError) -> NoReturn:
                self._retain_rejection(
                    journal,
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    error=error,
                )
                raise error

            if request.expected_owner_epoch != self.owner_epoch:
                reject(
                    ProgramRunnerError(
                        "program_run_owner_stale",
                        "Program Runner owner epoch changed",
                    )
                )
            if (
                cycle.program_run_id != request.program_run_id
                or cycle.state not in {"running", "stopping"}
                or cycle.reconciliation_required
                or (
                    kind in {"resume_motion", "prepare_manual_handover"}
                    and cycle.state != "running"
                )
            ):
                reject(
                    ProgramRunnerError(
                        "program_runtime_control_stale",
                        "runtime control does not name the current Program cycle",
                    )
                )
            try:
                live = self._owner.read(request.program_run_id)
            except ProgramRunOwnerError as error:
                reject(ProgramRunnerError("program_run_not_found", str(error)))
            if live.run_revision != request.expected_run_revision:
                reject(
                    ProgramRunnerError(
                        "program_runtime_control_stale",
                        "Program run changed before runtime control admission",
                        details={
                            "expected_run_revision": request.expected_run_revision,
                            "current_run_revision": live.run_revision,
                        },
                    )
                )
            effect_completed = False

            def unresolved(error: Exception) -> NoReturn:
                try:
                    journal.require_reconciliation(
                        expected_generation=cycle.cycle_generation,
                        failure=self._unknown_control_failure(kind, error),
                    )
                except Exception:
                    pass
                # Keep the exact command in-flight; deleting it would falsely
                # assert that an already-started effect did nothing.
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required",
                    str(error) or type(error).__name__,
                ) from error

            try:
                payload = effect()
                effect_completed = True
                projected = self._project_run(
                    self._owner.read(request.program_run_id).to_dict()
                )
                result = {
                    "schema_version": "manyforge.program_runtime_control_result.v1",
                    "request_id": request.request_id,
                    "accepted": True,
                    "result": payload,
                    "run": projected,
                    "runtime_controls": self.read_runtime_controls(
                        request.program_run_id
                    ),
                }
                journal.complete_control_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    result=result,
                    run_snapshot=projected,
                )
                return result
            except ProgramRunOwnerOutcomeUnknownError as error:
                unresolved(error)
            except ProgramRunnerError as error:
                if effect_completed:
                    unresolved(error)
                reject(error)
            except (
                ProgramRunOwnerError,
                ProgramRuntimeControlError,
                ProgramSchedulingError,
            ) as error:
                if effect_completed:
                    unresolved(error)
                code = getattr(error, "code", "program_runtime_control_rejected")
                reject(
                    ProgramRunnerError(
                        code, str(error), details=getattr(error, "details", None)
                    )
                )
            except Exception as error:
                # Unexpected exceptions are not a trustworthy no-effect
                # result: a local transition may already have happened.
                unresolved(error)

    @staticmethod
    def _unknown_control_failure(
        kind: str, error: Exception | None = None
    ) -> dict[str, Any]:
        """Describe one uncertain effect without guessing whether it ran."""

        label = kind.replace("_", " ").title()
        failure = {
            "code": "program_runtime_control_outcome_unknown",
            "command_kind": kind,
            "message": (
                f"The {label} outcome could not be confirmed. Further execution "
                "is blocked until the Program is safely reconciled."
            ),
        }
        if error is not None:
            failure["cause_code"] = getattr(error, "code", type(error).__name__)
        return failure

    def stage_runtime_override(
        self, program_run_id: str, request: RuntimeOverrideStageRequest
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        controls = self._controls(program_run_id)

        def effect():
            if request.source_observation_id is not None:
                candidate = self._runtime_override_observation_candidate(program_run_id)
                if (
                    candidate.get("available") is not True
                    or candidate.get("node_instance_id") != request.node_instance_id
                    or candidate.get("node_attempt_id") != request.node_attempt_id
                    or candidate.get("parameter_id") != request.parameter_id
                    or candidate.get("value") != request.value
                    or candidate.get("source_observation_id")
                    != request.source_observation_id
                    or candidate.get("source_owner_generation")
                    != request.source_owner_generation
                ):
                    raise ProgramRunnerError(
                        "program_runtime_observation_stale",
                        (
                            candidate.get("reason")
                            if isinstance(candidate.get("reason"), str)
                            else (
                                "Robot observation changed before runtime override "
                                "staging"
                            )
                        ),
                    )
            before = controls.revision
            record = controls.stage_override(
                node_instance_id=request.node_instance_id,
                node_attempt_id=request.node_attempt_id,
                parameter_id=request.parameter_id,
                value=request.value,
                source_observation_id=request.source_observation_id,
                source_owner_generation=request.source_owner_generation,
                validator=lambda **values: self._owner.validate_runtime_override(
                    program_run_id, **values
                ),
            )
            if controls.revision != before:
                try:
                    self._owner.note_runtime_control_change(program_run_id)
                except Exception as error:
                    raise RuntimeError(
                        "runtime override was staged but its run projection "
                        "could not be advanced"
                    ) from error
            return record

        return self._control_effect(
            request, kind="runtime_override_stage", effect=effect
        )

    def activate_runtime_override(
        self, program_run_id: str, request: RuntimeOverrideActionRequest
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        controls = self._controls(program_run_id)

        def effect():
            current = next(
                (
                    row
                    for row in controls.snapshot()["overrides"]
                    if row["override_id"] == request.override_id
                ),
                None,
            )
            if (
                current is None
                or current["override_revision"] != request.expected_override_revision
            ):
                raise ProgramRuntimeControlError(
                    "program_runtime_override_stale",
                    "runtime override revision changed",
                )
            return controls.activate_override(
                request.override_id,
                activator=lambda **values: self._owner.activate_runtime_override(
                    program_run_id, **values
                ),
            )

        return self._control_effect(
            request, kind="runtime_override_activate", effect=effect
        )

    def clear_runtime_override(
        self, program_run_id: str, request: RuntimeOverrideActionRequest
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        controls = self._controls(program_run_id)

        def effect():
            current = next(
                (
                    row
                    for row in controls.snapshot()["overrides"]
                    if row["override_id"] == request.override_id
                ),
                None,
            )
            if (
                current is None
                or current["override_revision"] != request.expected_override_revision
            ):
                raise ProgramRuntimeControlError(
                    "program_runtime_override_stale",
                    "runtime override revision changed",
                )
            return controls.clear_override(
                request.override_id,
                clearer=lambda **values: self._owner.clear_runtime_override(
                    program_run_id, **values
                ),
            )

        return self._control_effect(
            request, kind="runtime_override_clear", effect=effect
        )

    def promote_runtime_override(
        self, program_run_id: str, request: RuntimeOverridePromotionRequest
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        controls = self._controls(program_run_id)

        def effect():
            current = next(
                (
                    row
                    for row in controls.snapshot()["overrides"]
                    if row["override_id"] == request.override_id
                ),
                None,
            )
            if (
                current is None
                or current["override_revision"] != request.expected_override_revision
            ):
                raise ProgramRuntimeControlError(
                    "program_runtime_override_stale",
                    "runtime override revision changed",
                )
            node_id = self._owner.validate_runtime_override_promotion(
                program_run_id,
                node_instance_id=current["node_instance_id"],
            )
            before = controls.revision
            proposal = controls.request_promotion(
                request.override_id,
                node_id=node_id,
                expected_draft_id=request.expected_draft_id,
                expected_draft_revision=request.expected_draft_revision,
            )
            if controls.revision != before:
                try:
                    self._owner.note_runtime_control_change(program_run_id)
                except Exception as error:
                    raise RuntimeError(
                        "runtime promotion was staged but its run projection "
                        "could not be advanced"
                    ) from error
            return proposal

        return self._control_effect(
            request, kind="runtime_override_promote", effect=effect
        )

    def write_runtime_blackboard(
        self, program_run_id: str, request: RuntimeBlackboardWriteRequest
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        controls = self._controls(program_run_id)

        def effect():
            controls.require_event_revisions()
            self._owner.write_runtime_blackboard(
                program_run_id,
                key=request.key,
                value=request.value,
                request_id=request.request_id,
            )
            controls.audit_value("runtime_blackboard_written", key=request.key)
            return {"key": request.key, "status": "written"}

        return self._control_effect(
            request, kind="runtime_blackboard_write", effect=effect
        )

    def stage_manual_result(
        self, program_run_id: str, request: ManualResultStageRequest
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        controls = self._controls(program_run_id)

        def effect():
            intervention = self._owner.intervention(program_run_id)
            live = self._owner.read(program_run_id)
            policy = live.capture.node_policy_by_instance_id.get(
                request.node_instance_id
            )
            if (
                intervention is None
                or intervention.identity.node_attempt_id != request.node_attempt_id
                or intervention.identity.node_instance_id != request.node_instance_id
            ):
                raise ProgramRunnerError(
                    "program_runtime_control_stale",
                    "manual result identifies a stale node attempt",
                )
            if intervention.policy_parameter == "operator_stop_policy":
                raise ProgramRunnerError(
                    "operator_stop_requires_owner_recovery",
                    "operator Stop must resume through the exact owner action",
                )
            allowed, disabled_reason = manual_result_availability(
                intervention,
                {} if policy is None else policy.recovery_policy,
            )
            if not allowed:
                raise ProgramRunnerError(
                    disabled_reason or "program_manual_result_forbidden",
                    ("manual result is unavailable for the exact held " "attempt"),
                )
            controls.require_event_revisions()
            snapshot = self._host.resume_recovery(
                program_run_id,
                node_attempt_id=request.node_attempt_id,
                action=(
                    "manual_success"
                    if request.outcome == "succeeded"
                    else "manual_failure"
                ),
                result=request.output,
            )
            controls.audit_value(
                "manual_result_staged",
                node_instance_id=request.node_instance_id,
                node_attempt_id=request.node_attempt_id,
                outcome=request.outcome,
            )
            return {
                "node_instance_id": request.node_instance_id,
                "node_attempt_id": request.node_attempt_id,
                "outcome": request.outcome,
                "run_revision": snapshot.run_revision,
            }

        return self._control_effect(request, kind="manual_result_stage", effect=effect)

    def apply_recovery_action(
        self, request: CycleRecoveryActionRequest
    ) -> dict[str, Any]:
        if not isinstance(request, CycleRecoveryActionRequest):
            raise TypeError("recovery requires one CycleRecoveryActionRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is not started"
            )
        self._require_manager_current()
        with self._transition_lock:
            self._require_manager_current()
            _admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            intent = request.model_dump(mode="json")
            try:
                record = journal.record_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    kind="recovery_action",
                    intent=intent,
                    cycle_generation=cycle.cycle_generation,
                )
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    (
                        "execution_control_ingress_busy"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            self._require_new_command(record)

            def reject(error: ProgramRunnerError) -> NoReturn:
                latest_offer = self._recovery_offer(
                    cycle.program_run_id,
                    cycle_generation=cycle.cycle_generation,
                )
                if latest_offer is not None:
                    error = ProgramRunnerError(
                        error.code,
                        str(error),
                        details={**error.details, "recovery_offer": latest_offer},
                    )
                self._retain_rejection(
                    journal,
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    error=error,
                )
                raise error

            if (
                request.expected_owner_epoch != self.owner_epoch
                or cycle.program_run_id is None
                or cycle.state not in {"running", "stopping"}
            ):
                reject(
                    ProgramRunnerError(
                        "cycle_recovery_offer_stale",
                        "recovery does not name the current Program cycle",
                    )
                )
            program_run_id = cycle.program_run_id
            selection_scope = self._owner.operator_stop_scope(program_run_id)
            offer = self._recovery_offer(
                program_run_id,
                cycle_generation=cycle.cycle_generation,
                selection_scope=selection_scope,
            )
            action = (
                None
                if offer is None
                else next(
                    (
                        row
                        for row in offer["actions"]
                        if row["action_id"] == request.action_id
                    ),
                    None,
                )
            )
            if (
                offer is None
                or offer.get("offer_id") != request.offer_id
                or offer.get("offer_revision") != request.offer_revision
                or action is None
                or action.get("enabled") is not True
            ):
                reject(
                    ProgramRunnerError(
                        "cycle_recovery_offer_stale",
                        "recovery offer or selected action is no longer current",
                    )
                )
            intervention = self._owner.intervention(program_run_id)
            if intervention is None:
                reject(
                    ProgramRunnerError(
                        "cycle_recovery_offer_stale",
                        "held Program attempt is no longer current",
                    )
                )
            controls = self._controls(program_run_id)
            effect_started = False
            owner_effect_accepted = False

            def unresolved(error: Exception) -> NoReturn:
                try:
                    journal.require_reconciliation(
                        expected_generation=cycle.cycle_generation,
                        failure=self._unknown_control_failure(
                            "recovery_action", error
                        ),
                    )
                except Exception:
                    pass
                # Keep the request in-progress. The same request identity is
                # the only safe key for later owner reconciliation.
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required",
                    str(error) or type(error).__name__,
                ) from error

            try:
                owner_result = None
                cycle_resumed = False
                controls.require_event_revisions(
                    events=(
                        1
                        if request.action_id
                        in {"acknowledge_and_continue", "reset_cycle"}
                        else 2
                    ),
                    reserved_after=0 if request.action_id == "reset_cycle" else 1,
                )
                if (
                    request.action_id not in {"reset_cycle", "acknowledge_and_continue"}
                    and intervention.policy_parameter != "operator_stop_policy"
                ):
                    controls.require_retry_capacity(
                        intervention.identity.node_instance_id
                    )
                if request.action_id == "reset_cycle":
                    # The offer only authorizes the action. The ordinary Reset
                    # state machine owns stopping, settlement and restoration.
                    # Do not run a second Stop-before-Reset protocol here.
                    reset_request = ProgramCycleResetRequest.model_validate(
                        {
                            "request_id": self._digest(
                                {
                                    "kind": "operator_stop_reset_cycle",
                                    "request_owner_epoch": (
                                        request.request_owner_epoch
                                    ),
                                    "request_id": request.request_id,
                                    "program_run_id": program_run_id,
                                }
                            ),
                            "request_owner_epoch": request.request_owner_epoch,
                            "source_label": request.source_label,
                            "correlation_id": (
                                request.correlation_id or request.request_id
                            ),
                            "program_run_id": program_run_id,
                            "expected_owner_epoch": self.owner_epoch,
                            "expected_cycle_generation": cycle.cycle_generation,
                            "expected_claim_generation": (
                                None
                                if cycle.claim is None
                                else cycle.claim["claim_generation"]
                            ),
                            "session_id": request.session_id,
                            "client_sequence": request.client_sequence,
                        }
                    )
                    try:
                        reset_result = self.reset_cycle(reset_request)
                    except ProgramRunnerError as error:
                        # Reset has already retained its exact phase/failure.
                        # Close only this adapter's request; do not relabel a
                        # known Reset failure as unknown or overwrite its state.
                        self._retain_rejection(
                            journal,
                            owner_epoch=request.request_owner_epoch,
                            request_id=request.request_id,
                            error=error,
                        )
                        raise
                    terminal = journal.read_run_snapshot(program_run_id)
                    return {
                        "schema_version": "manyforge.cycle_recovery_result.v2",
                        "request_id": request.request_id,
                        "accepted": True,
                        "action_id": request.action_id,
                        "cycle_resumed": False,
                        "result_code": "accepted",
                        "owner_result": {
                            "program_stop": terminal,
                            "cycle_reset": reset_result,
                        },
                        "run": terminal,
                        # The run is retired; the final audit snapshot is in
                        # cycle_reset.runtime_controls, not live eligibility.
                        "runtime_controls": None,
                        "recovery_offer": None,
                    }
                elif request.action_id == "retry_runtime_step":
                    effect_started = True
                    snapshot = self._host.resume_recovery(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action="retry",
                    )
                    if intervention.policy_parameter != "operator_stop_policy":
                        controls.note_retry(intervention.identity.node_instance_id)
                    cycle_resumed = True
                elif (
                    intervention.policy_parameter == "operator_stop_policy"
                    and intervention.identity.node_instance_id
                    in selection_scope.held_nodes
                    and request.action_id == "replan_from_stopped_state"
                ):
                    effect_started = True
                    snapshot = self._host.resume_motion(
                        program_run_id,
                        expected_run_revision=self._owner.read(
                            program_run_id
                        ).run_revision,
                        expected_selection_revision=selection_scope.selection_revision,
                        request_id=request.request_id,
                        reason=request.reason,
                    )
                    owner_effect_accepted = True
                    owner_result = {"accepted": True, "action": request.action_id}
                    cycle_resumed = True
                else:
                    effect_started = True
                    owner_result = self._owner.apply_owner_recovery(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action=request.action_id,
                        request_id=request.request_id,
                        reason=request.reason,
                    )
                    if owner_result.get("accepted") is not True:
                        # The owner returned a durable, explicit no-effect
                        # result. It is safe to retain as an ordinary rejection.
                        effect_started = False
                        error = owner_result.get("error")
                        code = (
                            error.get("code")
                            if isinstance(error, Mapping)
                            and isinstance(error.get("code"), str)
                            else "cycle_recovery_action_rejected"
                        )
                        message = (
                            error.get("message")
                            if isinstance(error, Mapping)
                            and isinstance(error.get("message"), str)
                            else "addressed owner rejected recovery"
                        )
                        reject(ProgramRunnerError(code, message))
                    owner_effect_accepted = True
                    if request.action_id == "acknowledge_and_continue":
                        snapshot = self._owner.note_runtime_control_change(
                            program_run_id
                        )
                    else:
                        snapshot = self._host.resume_recovery(
                            program_run_id,
                            node_attempt_id=(intervention.identity.node_attempt_id),
                            action="retry",
                        )
                        if intervention.policy_parameter != "operator_stop_policy":
                            controls.note_retry(intervention.identity.node_instance_id)
                        cycle_resumed = True
                controls.audit_value(
                    "cycle_recovery_action_applied",
                    offer_id=request.offer_id,
                    action_id=request.action_id,
                    node_instance_id=intervention.identity.node_instance_id,
                    node_attempt_id=intervention.identity.node_attempt_id,
                    cycle_resumed=cycle_resumed,
                )
                projected = self._project_run(snapshot.to_dict())
                result = {
                    "schema_version": "manyforge.cycle_recovery_result.v2",
                    "request_id": request.request_id,
                    "accepted": True,
                    "action_id": request.action_id,
                    "cycle_resumed": cycle_resumed,
                    "result_code": "accepted",
                    "owner_result": owner_result,
                    "run": projected,
                    "runtime_controls": self.read_runtime_controls(program_run_id),
                    "recovery_offer": self._recovery_offer(
                        program_run_id,
                        cycle_generation=cycle.cycle_generation,
                    ),
                }
                journal.complete_control_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    result=result,
                    run_snapshot=projected,
                )
                return result
            except ProgramRunnerError as error:
                if effect_started:
                    unresolved(error)
                raise
            except ProgramSchedulingError as error:
                # resume_recovery checks capacity and owner state before its
                # atomic transition, so its direct rejection is a no-effect.
                # After an addressed owner action, however, that owner effect
                # has already succeeded and the combined outcome is unknown.
                if effect_started and request.action_id != "retry_runtime_step":
                    unresolved(error)
                reject(ProgramRunnerError("program_run_scheduling_failed", str(error)))
            except ProgramRunOwnerOutcomeUnknownError as error:
                unresolved(error)
            except ProgramRunOwnerRecoveryIncompleteError as error:
                # Known cleanup is retained by current attempts. Report it
                # explicitly without faulting an otherwise coherent cycle.
                reject(
                    ProgramRunnerError(
                        "program_motion_resume_incomplete",
                        str(error),
                        details={"cleanup_accepted": True},
                    )
                )
            except (ProgramRunOwnerError, ProgramRuntimeControlError) as error:
                if owner_effect_accepted or (
                    effect_started and isinstance(error, ProgramRuntimeControlError)
                ):
                    unresolved(error)
                reject(
                    ProgramRunnerError(
                        getattr(error, "code", "cycle_recovery_action_rejected"),
                        str(error),
                        details=getattr(error, "details", None),
                    )
                )
            except Exception as error:
                unresolved(error)

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
        key = ("execution", request.request_owner_epoch, request.request_id)
        retained: _InflightStop | None = None
        owns_record = False
        with self._inflight_stops_lock:
            resident = self._inflight_stops.get(key)
            if resident is not None and resident.intent == intent:
                retained = resident
            elif resident is None and len(self._inflight_stops) < (
                self.config.maximum_inflight_commands
            ):
                retained = _InflightStop(intent, Event())
                self._inflight_stops[key] = retained
                owns_record = True

        if retained is None:
            raise ProgramRunnerError(
                "execution_control_ingress_busy",
                "Program Stop in-flight command bound is full or identity conflicts",
            )

        if retained is not None and not owns_record:
            retained.settled.wait()
            if retained.result is not None:
                return deepcopy(retained.result)
            if retained.error is not None:
                raise ProgramRunnerError(retained.error.code, str(retained.error))
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required",
                "resident Stop outcome is unavailable",
            )

        stopping_committed = False
        try:
            _admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            if (
                cycle.owner_epoch != self.owner_epoch
                or cycle.program_run_id != program_run_id
            ):
                raise ProgramRunnerError(
                    "program_run_not_found",
                    "Program Stop does not identify the current Program cycle",
                )

            state_before = cycle.state
            recovery_stop = self._current_cycle_recovery_stop(cycle)
            if state_before in {"starting", "running"} or recovery_stop:
                journal.mark_stopping(
                    expected_generation=cycle.cycle_generation,
                    program_run_id=program_run_id,
                    correlation_id=request.correlation_id or request.request_id,
                    session_id=request.session_id,
                    client_sequence=request.client_sequence,
                )
                stopping_committed = True
                cycle = journal.read()
            elif state_before in {
                "stopping",
                "resetting",
                "terminal_awaiting_reset",
            }:
                result = {
                    "schema_version": "manyforge.program_stop_acceptance.v1",
                    "program_run_id": program_run_id,
                    "cycle_generation": cycle.cycle_generation,
                    "state": cycle.state,
                    "execution_control_revision": (cycle.execution_control_revision),
                }
            else:
                raise ProgramRunnerError(
                    "program_run_not_found",
                    "Program cycle is already Idle",
                )

            if state_before == "starting":
                # Starting owns its own cleanup.  If acquisition already won,
                # close the exact Kernel scope now; otherwise the Start path
                # observes Stopping before acquisition and proves no effect.
                if cycle.claim is not None:
                    self._native.scope.request_stop(
                        program_run_id,
                        (
                            "execution:"
                            f"{request.request_owner_epoch}:{request.request_id}"
                        ),
                        request.reason,
                    )
                result = {
                    "schema_version": "manyforge.program_stop_acceptance.v1",
                    "program_run_id": program_run_id,
                    "cycle_generation": cycle.cycle_generation,
                    "state": "stopping",
                    "execution_control_revision": (cycle.execution_control_revision),
                }
            elif state_before == "running" or recovery_stop:
                snapshot = self._host.stop_run(
                    program_run_id,
                    reason=request.reason,
                    stop_request_id=(
                        "execution:"
                        f"{request.request_owner_epoch}:{request.request_id}"
                    ),
                )
                result = self._project_run(snapshot.to_dict())
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
            if result.get("schema_version") != ("manyforge.program_stop_acceptance.v1"):
                # Journal projection is subordinate to recognized
                # containment. It must neither gate the owner call nor turn
                # success into failure.
                try:
                    _admission, journal = self._require_cycle_owner()
                    journal.retain_run_snapshot(result)
                    cycle = journal.read()
                    if result.get("state") in {"stopped", "succeeded", "failed"}:
                        self._record_terminal_snapshot(result)
                    elif cycle.program_run_id == program_run_id and cycle.state in {
                        "running",
                        "stopping",
                    }:
                        journal.mark_stopping(
                            expected_generation=cycle.cycle_generation,
                            program_run_id=program_run_id,
                            correlation_id=(
                                request.correlation_id or request.request_id
                            ),
                            session_id=request.session_id,
                            client_sequence=request.client_sequence,
                        )
                except Exception:
                    pass
            if retained is not None:
                retained.result = deepcopy(result)
                retained.settled.set()
                with self._inflight_stops_lock:
                    self._inflight_stops.pop(key, None)
            return result

        if stopping_committed:
            try:
                journal.require_reconciliation(
                    expected_generation=cycle.cycle_generation
                )
            except Exception:
                pass
            outcome = ProgramRunnerError(
                "program_cycle_recovery_required",
                str(outcome),
                details={"cause_code": outcome.code},
            )
        if retained is not None:
            retained.error = outcome
            retained.settled.set()
            with self._inflight_stops_lock:
                self._inflight_stops.pop(key, None)
        raise outcome

    def stop_motion(
        self,
        program_run_id: str,
        request: ProgramSelectionStopRequest,
    ) -> dict[str, Any]:
        """Stop current addressed work without terminating the Program."""

        if not isinstance(request, ProgramSelectionStopRequest):
            raise TypeError("stop_motion requires one ProgramSelectionStopRequest")
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
                "Program-run owner epoch changed before motion Stop",
            )

        intent = request.model_dump(mode="json")
        key = ("motion", request.request_owner_epoch, request.request_id)
        retained: _InflightStop | None = None
        owns_record = False
        with self._inflight_stops_lock:
            resident = self._inflight_stops.get(key)
            if resident is not None and resident.intent == intent:
                retained = resident
            elif len(self._inflight_stops) < self.config.maximum_inflight_commands:
                retained = _InflightStop(intent, Event())
                self._inflight_stops[key] = retained
                owns_record = True

        if retained is None:
            raise ProgramRunnerError(
                "execution_control_ingress_busy",
                "Program motion Stop in-flight command bound is full or identity conflicts",
            )

        if retained is not None and not owns_record:
            retained.settled.wait()
            if retained.result is not None:
                return deepcopy(retained.result)
            if retained.error is not None:
                raise ProgramRunnerError(
                    retained.error.code,
                    str(retained.error),
                )
            raise ProgramRunnerError(
                "program_motion_stop_outcome_unknown",
                "resident motion Stop outcome is unavailable",
            )

        try:
            # The resident Run supplies the immutable claim. The Kernel
            # validates and holds its selection before any journal/projection
            # work; a partly held Program remains a running cycle.
            snapshot = self._host.stop_motion(
                program_run_id,
                group_id=request.group_ids,
                reason=request.reason,
                stop_request_id=(
                    "motion:" f"{request.request_owner_epoch}:{request.request_id}"
                ),
            )
            result = self._project_run(snapshot.to_dict())
        except ProgramRunOwnerOutcomeUnknownError as error:
            outcome = ProgramRunnerError(
                "program_motion_stop_outcome_unknown",
                str(error),
            )
        except ProgramRunOwnerError as error:
            outcome = ProgramRunnerError(
                "program_motion_stop_invalid",
                str(error),
            )
        except ProgramSchedulingError as error:
            outcome = self._runner_error(error)
        except ProgramRunnerError as error:
            outcome = error
        except Exception as error:
            outcome = ProgramRunnerError(
                "program_motion_stop_outcome_unknown",
                str(error) or type(error).__name__,
            )
        else:
            if retained is not None:
                retained.result = deepcopy(result)
                retained.settled.set()
                with self._inflight_stops_lock:
                    self._inflight_stops.pop(key, None)
            return result

        if retained is not None:
            retained.error = outcome
            retained.settled.set()
            with self._inflight_stops_lock:
                self._inflight_stops.pop(key, None)
        raise outcome

    def resume_motion(
        self, program_run_id: str, request: ProgramSelectionResumeRequest
    ) -> dict[str, Any]:
        if not isinstance(request, ProgramSelectionResumeRequest):
            raise TypeError("resume_motion requires one ProgramSelectionResumeRequest")
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid", "motion Resume changed its Run"
            )

        def effect() -> dict[str, Any]:
            try:
                snapshot = self._host.resume_motion(
                    program_run_id,
                    expected_run_revision=request.expected_run_revision,
                    expected_selection_revision=request.expected_selection_revision,
                    request_id=request.request_id,
                    group_id=request.group_ids,
                    reason=request.reason,
                )
            except ProgramRunOwnerRecoveryIncompleteError as error:
                raise ProgramRunnerError(
                    "program_motion_resume_incomplete",
                    str(error),
                    details={"cleanup_accepted": True},
                ) from error
            return {
                "state": snapshot.state.value,
                "run_revision": snapshot.run_revision,
            }

        # Reuse ordinary current-run command ordering, attribution and outcome
        # retention. Unknown effects enter its existing reconciliation path.
        result = self._control_effect(request, kind="resume_motion", effect=effect)
        return self._project_run(result["run"])

    def prepare_manual_handover(
        self,
        program_run_id: str,
        request: ProgramManualHandoverRequest,
    ) -> dict[str, Any]:
        if not isinstance(request, ProgramManualHandoverRequest):
            raise TypeError("manual handover requires one ProgramManualHandoverRequest")
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid", "manual handover changed its Program"
            )

        def effect() -> dict[str, Any]:
            # This read cannot retire work. Even a lost/malformed query reply
            # is a known no-effect failure, not an uncertain cleanup result.
            try:
                raw = self._kernel.read_manual_handover(
                    request.operation.model_dump(mode="json")
                )
                proof = ManualProgramHandover.model_validate(raw)
            except Exception as error:
                raise ProgramRunnerError(
                    "manual_handover_query_unavailable", str(error)
                ) from error
            if proof.context != request.operation.manual_handover:
                raise ProgramRunnerError(
                    "manual_handover_scope_stale",
                    "Kernel held Program changed before cleanup",
                )
            if not proof.producer_node_indices:
                snapshot = self._owner.read(program_run_id)
            else:
                try:
                    snapshot = self._host.prepare_manual_handover(
                        program_run_id,
                        node_indices=tuple(proof.producer_node_indices),
                        expected_run_revision=request.expected_run_revision,
                        expected_selection_revision=proof.context.selection_revision,
                        request_id=request.request_id,
                        reason=request.reason,
                    )
                except ProgramRunOwnerRecoveryIncompleteError as error:
                    raise ProgramRunnerError(
                        "manual_handover_cleanup_incomplete",
                        str(error),
                        details={"cleanup_accepted": True},
                    ) from error
            return {
                "state": snapshot.state.value,
                "run_revision": snapshot.run_revision,
            }

        result = self._control_effect(
            request, kind="prepare_manual_handover", effect=effect
        )
        return self._project_run(result["run"])

    def full_stop(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        """Contain one Program run, then preserve its quiesced tree."""

        if not isinstance(request, ProgramRunStopRequest):
            raise TypeError("full_stop requires one ProgramRunStopRequest")
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
                "Program-run owner epoch changed before Full Stop",
            )

        intent = request.model_dump(mode="json")
        key = ("full", request.request_owner_epoch, request.request_id)
        retained: _InflightStop | None = None
        owns_record = False
        with self._inflight_stops_lock:
            resident = self._inflight_stops.get(key)
            if resident is not None and resident.intent == intent:
                retained = resident
            elif resident is None and len(self._inflight_stops) < (
                self.config.maximum_inflight_commands
            ):
                retained = _InflightStop(intent, Event())
                self._inflight_stops[key] = retained
                owns_record = True

        if retained is None:
            raise ProgramRunnerError(
                "execution_control_ingress_busy",
                "Program Full Stop in-flight command bound is full or identity conflicts",
            )

        if retained is not None and not owns_record:
            retained.settled.wait()
            if retained.result is not None:
                return deepcopy(retained.result)
            if retained.error is not None:
                raise ProgramRunnerError(
                    retained.error.code,
                    str(retained.error),
                )
            raise ProgramRunnerError(
                "program_full_stop_outcome_unknown",
                "resident Full Stop outcome is unavailable",
            )

        try:
            snapshot = self._host.full_stop(
                program_run_id,
                reason=request.reason,
                stop_request_id=(
                    "full:" f"{request.request_owner_epoch}:{request.request_id}"
                ),
            )
            result = self._project_run(snapshot.to_dict())
        except ProgramRunOwnerOutcomeUnknownError as error:
            outcome = ProgramRunnerError(
                "program_full_stop_outcome_unknown",
                str(error),
            )
        except ProgramRunOwnerError as error:
            outcome = ProgramRunnerError(
                "program_full_stop_invalid",
                str(error),
            )
        except ProgramSchedulingError as error:
            outcome = self._runner_error(error)
        except ProgramRunnerError as error:
            outcome = error
        except Exception as error:
            outcome = ProgramRunnerError(
                "program_full_stop_outcome_unknown",
                str(error) or type(error).__name__,
            )
        else:
            try:
                _admission, journal = self._require_cycle_owner()
                journal.retain_run_snapshot(result)
            except Exception:
                pass
            if retained is not None:
                retained.result = deepcopy(result)
                retained.settled.set()
                with self._inflight_stops_lock:
                    self._inflight_stops.pop(key, None)
            return result

        if retained is not None:
            retained.error = outcome
            retained.settled.set()
            with self._inflight_stops_lock:
                self._inflight_stops.pop(key, None)
        raise outcome

    def resume_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResumeRequest,
    ) -> dict[str, Any]:
        """Resume one exact preserved tree through fresh-state recovery."""

        if not isinstance(request, ProgramFullStopResumeRequest):
            raise TypeError(
                "resume_full_stop requires one ProgramFullStopResumeRequest"
            )
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )

        def effect() -> dict[str, Any]:
            snapshot = self._host.resume_full_stop(
                program_run_id,
                request_id=request.request_id,
                reason=request.reason,
            )
            return {
                "state": getattr(snapshot.state, "value", snapshot.state),
                "run_revision": snapshot.run_revision,
            }

        result = self._control_effect(
            request,
            kind="resume_full_stop",
            effect=effect,
        )
        run = result.get("run")
        if not isinstance(run, Mapping):
            raise ProgramRunnerError(
                "program_runner_response_invalid",
                "Full Stop resume omitted its Program-run result",
            )
        return self._project_run(run)

    def discard_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResetRequest,
    ) -> dict[str, Any]:
        """Discard one preserved tree as the first phase of one Reset."""

        if not isinstance(request, ProgramFullStopResetRequest):
            raise TypeError(
                "discard_full_stop requires one ProgramFullStopResetRequest"
            )
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is not started",
            )
        self._require_manager_current()
        with self._transition_lock:
            self._require_manager_current()
            _admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            try:
                record = journal.record_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    kind="discard_full_stop",
                    intent=request.model_dump(mode="json"),
                    cycle_generation=cycle.cycle_generation,
                )
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    (
                        "execution_control_ingress_busy"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            self._require_new_command(record)

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
                request.expected_owner_epoch != self.owner_epoch
                or cycle.owner_epoch != self.owner_epoch
                or cycle.reconciliation_required
                or cycle.state != "running"
                or cycle.program_run_id != program_run_id
                or cycle.cycle_generation != request.expected_cycle_generation
                or not isinstance(claim, Mapping)
                or claim.get("claim_generation") != request.expected_claim_generation
            ):
                reject(
                    ProgramRunnerError(
                        "program_full_stop_reset_stale",
                        "Reset does not identify the current Full-stopped cycle",
                    )
                )
            try:
                live = self._owner.read(program_run_id)
            except ProgramRunOwnerError as error:
                reject(ProgramRunnerError("program_run_not_found", str(error)))
            if (
                getattr(live.state, "value", live.state)
                != ProgramRunState.FULL_STOPPED.value
                or live.run_revision != request.expected_run_revision
            ):
                reject(
                    ProgramRunnerError(
                        "program_full_stop_reset_stale",
                        "Reset does not identify the exact Full-stopped tree",
                    )
                )

            effect_started = False
            try:
                effect_started = True
                terminal = self._host.stop_run(
                    program_run_id,
                    reason=request.reason or "operator Reset discarded Full Stop",
                    stop_request_id=(
                        "full-stop-reset:"
                        f"{request.request_owner_epoch}:{request.request_id}"
                    ),
                )
                if getattr(terminal.state, "value", terminal.state) not in {
                    ProgramRunState.STOPPED.value,
                    ProgramRunState.SUCCEEDED.value,
                    ProgramRunState.FAILED.value,
                }:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Full-stopped tree did not settle for Reset"
                    )
                result = self._project_run(terminal.to_dict())
                self._record_terminal_snapshot(result)
                settled_cycle = journal.read()
                if (
                    settled_cycle.state != "terminal_awaiting_reset"
                    or settled_cycle.program_run_id != program_run_id
                ):
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Full-stopped Reset settlement was not retained"
                    )
                journal.complete_request(
                    owner_epoch=request.request_owner_epoch,
                    request_id=request.request_id,
                    result=result,
                )
                return result
            except Exception as error:
                if effect_started:
                    try:
                        journal.require_reconciliation(
                            expected_generation=cycle.cycle_generation
                        )
                    except Exception:
                        pass
                    raise ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        str(error) or type(error).__name__,
                    ) from error
                reject(
                    ProgramRunnerError(
                        "program_full_stop_reset_failed",
                        str(error) or type(error).__name__,
                    )
                )

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
            pending = journal.pending_commands()
            requested_intent = request.model_dump(mode="json")
            try:
                if any(
                    (row["request_owner_epoch"], row["request_id"])
                    == (request.request_owner_epoch, request.request_id)
                    for row in pending
                ):
                    # An identical unresolved request is not a fresh operator
                    # confirmation; a conflicting identity must not delete it.
                    self._require_new_command(
                        journal.record_request(
                            owner_epoch=request.request_owner_epoch,
                            request_id=request.request_id,
                            kind="reset",
                            intent=requested_intent,
                            cycle_generation=request.expected_cycle_generation,
                        )
                    )
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    (
                        "execution_control_ingress_busy"
                        if isinstance(error, OverflowError)
                        else "program_run_request_conflict"
                    ),
                    str(error),
                ) from error
            reset_accepted = False
            record_created = False
            recovery_reset = self._current_cycle_recovery_reset(cycle, pending)
            confirmation_request = request
            stop_request_identity = (
                f"program-reset:{request.request_owner_epoch}:{request.request_id}"
            )

            def reject(error: ProgramRunnerError) -> NoReturn:
                if reset_accepted:
                    reconciliation_required = (
                        error.code == "program_cycle_reconciliation_required"
                    )
                    try:
                        journal.fail_reset(
                            expected_generation=cycle.cycle_generation,
                            program_run_id=request.program_run_id,
                            code=error.code,
                            message=str(error),
                            reconciliation_required=reconciliation_required,
                        )
                    except Exception:
                        pass
                    # Keep one Reset occurrence through known/unknown phase
                    # failures. Fresh confirmations continue it; they never
                    # grow the reserved slot or lose its original identity.
                    raise ProgramRunnerError(
                        "program_cycle_recovery_required",
                        str(error),
                        details={"cause_code": error.code},
                    ) from error
                if record_created:
                    self._retain_rejection(
                        journal,
                        owner_epoch=request.request_owner_epoch,
                        request_id=request.request_id,
                        error=error,
                    )
                raise error

            reset_retry_phase = self._reset_retry_phase(cycle)
            claim = cycle.claim
            claimless_reset = bool(
                claim is None
                and request.expected_claim_generation is None
                and self._cycle_terminal_evidence_is_exact(journal, cycle)
                and (
                    cycle.state == "terminal_awaiting_reset"
                    or reset_retry_phase is not None
                )
            )
            if (
                cycle.owner_epoch != request.expected_owner_epoch
                or cycle.owner_epoch != self.owner_epoch
                or cycle.cycle_generation != request.expected_cycle_generation
                or cycle.program_run_id != request.program_run_id
                or (
                    cycle.reconciliation_required
                    and reset_retry_phase is None
                    and not recovery_reset
                )
                or (not isinstance(claim, Mapping) and not claimless_reset)
                or request.expected_claim_generation
                != (None if claim is None else claim.get("claim_generation"))
                or (
                    cycle.state in {"terminal_awaiting_reset", "recovery_required"}
                    and (
                        request.terminal_reference != cycle.terminal_reference
                        or request.settlement_evidence_digest
                        != cycle.terminal_reference
                    )
                )
                or (
                    cycle.state == "running"
                    and (
                        request.terminal_reference is not None
                        or request.settlement_evidence_digest is not None
                    )
                )
            ):
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_stale",
                        "Reset does not identify the current Program cycle",
                    )
                )
            if cycle.state == "stopping":
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_unavailable_while_stopping",
                        "Reset is unavailable after an independent Stop won",
                    )
                )
            if cycle.state not in {
                "running",
                "terminal_awaiting_reset",
                "recovery_required",
            } or (
                cycle.state == "recovery_required"
                and reset_retry_phase is None
                and not recovery_reset
            ):
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_stale",
                        "Reset is not eligible in the current Program state",
                    )
                )
            retained_resets = [row for row in pending if row["kind"] == "reset"]
            if len(retained_resets) > 1 or any(
                row["cycle_generation"] != cycle.cycle_generation
                or row["intent"].get("program_run_id") != request.program_run_id
                for row in retained_resets
            ):
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "Retained Reset occurrence is ambiguous",
                    )
                )
            if recovery_reset:
                try:
                    resident = self._owner.read(request.program_run_id)
                    if resident.owner_epoch != self.owner_epoch:
                        raise RuntimeError("Reset requires the current resident Run")
                    recovery_kernel_epoch = self._current_kernel_owner_epoch()
                    recovery_liveness = journal.liveness()
                except Exception as error:
                    reject(
                        ProgramRunnerError(
                            "program_cycle_reconciliation_required", str(error)
                        )
                    )
            if not retained_resets:
                try:
                    record = journal.record_request(
                        owner_epoch=request.request_owner_epoch,
                        request_id=request.request_id,
                        kind="reset",
                        intent=requested_intent,
                        cycle_generation=request.expected_cycle_generation,
                    )
                    self._require_new_command(record)
                    record_created = True
                except (ValueError, OverflowError) as error:
                    raise ProgramRunnerError(
                        "execution_control_ingress_busy", str(error)
                    ) from error
            try:
                initial_state = cycle.state
                if reset_retry_phase is None:
                    journal.begin_resetting(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=request.program_run_id,
                        correlation_id=request.correlation_id or request.request_id,
                        session_id=request.session_id,
                        client_sequence=request.client_sequence,
                        expected_recovery=cycle if recovery_reset else None,
                    )
                else:
                    failure_code = cycle.failure.get("code")
                    assert isinstance(failure_code, str)
                    journal.resume_resetting(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=request.program_run_id,
                        expected_failure_code=failure_code,
                        phase=reset_retry_phase,
                        correlation_id=request.correlation_id or request.request_id,
                        session_id=request.session_id,
                        client_sequence=request.client_sequence,
                        allow_legacy_scene_failure=(cycle.reset_phase == "failed"),
                    )
                reset_accepted = True
                if retained_resets:
                    # Restoration and publication belong to the retained
                    # occurrence. Only this explicit confirmation's Stop is
                    # fresh; overlapping containment retains its old deadline.
                    request = request.model_copy(
                        update={
                            "request_owner_epoch": retained_resets[0][
                                "request_owner_epoch"
                            ],
                            "request_id": retained_resets[0]["request_id"],
                        }
                    )
                cycle = journal.read()
                if cycle.reset_phase == "stopping" and initial_state in {
                    "running",
                    "recovery_required",
                }:
                    terminal = self._host.stop_run(
                        request.program_run_id,
                        reason=PROGRAM_RESET_STOP_REASON,
                        stop_request_id=stop_request_identity,
                    )
                    if getattr(terminal.state, "value", terminal.state) not in {
                        ProgramRunState.STOPPED.value,
                        ProgramRunState.SUCCEEDED.value,
                        ProgramRunState.FAILED.value,
                    }:
                        terminal = self._host.wait_terminal(
                            request.program_run_id,
                            timeout_s=self.config.headless_timeout_seconds,
                        )
                    terminal_result = self._project_run(terminal.to_dict())
                    if terminal_result.get("state") not in {
                        "stopped",
                        "succeeded",
                        "failed",
                    }:
                        raise ProgramRunnerError(
                            "program_cycle_reset_stop_unsettled",
                            "Program Stop did not produce terminal settlement",
                        )
                    journal.retain_run_snapshot(terminal_result)
                    reference = self._digest(
                        {
                            "program_run_id": request.program_run_id,
                            "run_revision": terminal_result.get("run_revision"),
                            "state": terminal_result.get("state"),
                        }
                    )
                elif cycle.reset_phase == "stopping":
                    reference = cycle.terminal_reference
                if cycle.reset_phase == "stopping":
                    if (
                        isinstance(claim, Mapping)
                        and isinstance(claim.get("intent"), Mapping)
                        and claim["intent"].get("owner_epoch") == self.owner_epoch
                        and cycle.start_settlement is None
                    ):
                        self._host.contain_reset_run(
                            request.program_run_id,
                            stop_request_id=stop_request_identity,
                            timeout_s=self.config.headless_timeout_seconds,
                        )
                    journal.update_reset_phase(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=request.program_run_id,
                        expected_phase="stopping",
                        phase="settling",
                        terminal_reference=reference,
                    )
                # Any entrypoint must reconcile the exact Kernel Stop-fault
                # latch before publishing Idle for the next Run.
                self._clear_reset_stop_fault(confirmation_request)
                cycle = journal.read()
                request = request.model_copy(
                    update={
                        "expected_claim_generation": (
                            None if claim is None else claim.get("claim_generation")
                        ),
                        "terminal_reference": cycle.terminal_reference,
                        "settlement_evidence_digest": cycle.terminal_reference,
                    }
                )
            except ProgramRunnerError as error:
                reject(error)
            except (
                ProgramRunOwnerOutcomeUnknownError,
                ProgramSchedulingError,
            ) as error:
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required", str(error)
                    )
                )
            except ProgramRunOwnerError as error:
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_stop_failed",
                        str(error),
                    )
                )
            except Exception as error:
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_failed",
                        str(error) or type(error).__name__,
                    )
                )

            claim = cycle.claim
            if (
                cycle.state != "resetting"
                or cycle.cycle_generation != request.expected_cycle_generation
                or cycle.owner_epoch != request.expected_owner_epoch
                or cycle.program_run_id != request.program_run_id
                or cycle.terminal_reference != request.terminal_reference
                or request.settlement_evidence_digest != request.terminal_reference
                or (claim is None and request.expected_claim_generation is not None)
                or (
                    isinstance(claim, Mapping)
                    and claim.get("claim_generation")
                    != request.expected_claim_generation
                )
            ):
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_stale",
                        "Reset does not identify the exact terminal Program cycle",
                    )
                )
            if not self._cycle_terminal_evidence_is_exact(journal, cycle):
                reject(
                    ProgramRunnerError(
                        "program_cycle_settlement_invalid",
                        "Program-cycle terminal evidence is incomplete or invalid",
                    )
                )
            intent = None if claim is None else claim.get("intent")
            if claim is not None and not isinstance(intent, dict):
                reject(
                    ProgramRunnerError(
                        "program_cycle_journal_invalid",
                        "retained admission claim has no exact intent",
                    )
                )
            release = (
                None
                if claim is None
                else {
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
            )
            reconciliation_digest = (
                cell_admission_owner_epoch_reconciliation_evidence_digest(
                    "program_runner", self.owner_epoch
                )
            )
            claim_was_settled_by_recovery = bool(
                claim is not None
                and intent.get("owner_epoch") != cycle.owner_epoch
                and cycle.reconciliation_evidence_digest == reconciliation_digest
            )
            run_owner_was_replaced = bool(
                claim is not None and intent.get("owner_epoch") != cycle.owner_epoch
            )
            if (
                cycle.reconciliation_required and not recovery_reset
            ) or cycle.owner_epoch != self.owner_epoch:
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "Reset cannot settle an unreconciled Program-cycle owner",
                    )
                )
            if claim_was_settled_by_recovery:
                try:
                    receipt = admission.recovery_receipt(
                        {
                            "intent": claim["intent"],
                            "expected_claim": claim,
                            "expected_release": None,
                        }
                    )
                    program_owners = [
                        owner
                        for owner in receipt.get("current_owner_epochs", [])
                        if isinstance(owner, Mapping)
                        and owner.get("owner_role") == "program_runner"
                    ]
                    held_program_claims = [
                        retained_claim
                        for retained_claim in receipt.get("held_claims", [])
                        if isinstance(retained_claim, Mapping)
                        and isinstance(retained_claim.get("intent"), Mapping)
                        and retained_claim["intent"].get("owner_role")
                        == "program_runner"
                    ]
                    expected_settlement = {
                        "predecessor_claim": claim,
                        "successor_owner_role": "program_runner",
                        "successor_owner_epoch": self.owner_epoch,
                        "settlement_predicate": "reconciled",
                        "reconciliation_evidence_digest": reconciliation_digest,
                    }
                    receipt_is_exact = bool(
                        program_owners
                        == [
                            {
                                "owner_role": "program_runner",
                                "owner_epoch": self.owner_epoch,
                            }
                        ]
                        and not held_program_claims
                        and (
                            (
                                receipt.get("status") == "reconciled_exact"
                                and receipt.get("matched_claim") == claim
                                and receipt.get("matched_release") is None
                                and receipt.get("matched_reconcile_settlement")
                                == expected_settlement
                            )
                            or (
                                receipt.get("status") == "unknown"
                                and receipt.get("matched_claim") is None
                                and receipt.get("matched_release") is None
                                and receipt.get("matched_reconcile_settlement") is None
                            )
                        )
                    )
                    if not receipt_is_exact:
                        raise RuntimeError(
                            "reconciled Program-cycle claim is not exact"
                        )
                    released = admission.get()
                except Exception as error:
                    reject(
                        ProgramRunnerError(
                            "program_cycle_reconciliation_required",
                            str(error) or type(error).__name__,
                        )
                    )
            if cycle.reset_phase == "settling":
                retained_run = journal.read_run_snapshot(request.program_run_id)
                try:
                    if retained_run is None and claim is not None:
                        self._native.scope.reset(request.program_run_id)
                    elif retained_run is not None and run_owner_was_replaced:
                        self._host.reset_retained_run(request.program_run_id)
                    elif retained_run is not None:
                        self._host.reset_run(request.program_run_id)
                    if recovery_reset:
                        try:
                            self._prove_reset_recovery(
                                cycle, kernel_owner_epoch=recovery_kernel_epoch
                            )
                            self._require_manager_current()
                            journal.settle_reset_recovery(
                                expected=cycle,
                                manager_runtime_epoch=recovery_liveness.manager_runtime_epoch,
                                manager_owner_epoch=recovery_liveness.manager_owner_epoch,
                            )
                        except Exception as error:
                            raise ProgramRunnerError(
                                "program_cycle_reconciliation_required", str(error)
                            ) from error
                    else:
                        journal.update_reset_phase(
                            expected_generation=cycle.cycle_generation,
                            program_run_id=request.program_run_id,
                            expected_phase="settling",
                            phase="restoring_scene",
                            terminal_reference=request.terminal_reference,
                        )
                    cycle = journal.read()
                except ProgramRunnerError as error:
                    reject(error)
                except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                    reject(
                        ProgramRunnerError(
                            "program_cycle_reset_failed",
                            str(error),
                        )
                    )
                except Exception as error:
                    reject(
                        ProgramRunnerError(
                            "program_cycle_reset_failed",
                            str(error) or type(error).__name__,
                        )
                    )
            elif cycle.reset_phase not in {"restoring_scene", "releasing"}:
                reject(
                    ProgramRunnerError(
                        "program_cycle_reset_phase_invalid",
                        "Reset retained an unknown durable phase",
                    )
                )
            reset_scene_authorization = (
                None
                if claim is None
                else {
                    "program_runner_owner_epoch": cycle.owner_epoch,
                    "program_run_id": request.program_run_id,
                    "cycle_claim_generation": claim["claim_generation"],
                    "terminal_or_settlement_reference": (request.terminal_reference),
                }
            )
            controls = self._optional_controls(request.program_run_id)
            if cycle.reset_phase == "restoring_scene":
                try:
                    self._prepare_scene_after_reset(
                        request,
                        reset_scene_authorization,
                    )
                    if controls is not None:
                        controls.mark_reset(
                            request_id=request.request_id,
                            correlation_id=request.correlation_id,
                            source_label=request.source_label,
                        )
                    journal.update_reset_phase(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=request.program_run_id,
                        expected_phase="restoring_scene",
                        phase="releasing",
                        terminal_reference=request.terminal_reference,
                    )
                    cycle = journal.read()
                except ProgramRunnerError as error:
                    reject(error)
                except Exception as error:
                    reject(
                        ProgramRunnerError(
                            "program_cycle_reset_scene_preparation_failed",
                            str(error) or type(error).__name__,
                        )
                    )
            if not claim_was_settled_by_recovery:
                if claim is None:
                    released = admission.get()
                else:
                    assert release is not None
                try:
                    if claim is not None:
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
                if claim is not None:
                    try:
                        released = admission.release(release)
                    except CellAdmissionError as error:
                        if error.outcome_unknown:
                            journal.require_reconciliation(
                                expected_generation=cycle.cycle_generation
                            )
                            reject(ProgramRunnerError(error.code, str(error)))
                        reject(ProgramRunnerError(error.code, str(error)))
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
                "runtime_controls": (None if controls is None else controls.snapshot()),
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
            # Idle and the bounded history row are now durable.  The
            # executable tree is no longer current and must not accumulate
            # for the lifetime of this process. This is idempotent when Reset
            # resumed under a replacement Program Runner.
            self._host.forget_reset_run(request.program_run_id)
            with self._runtime_controls_lock:
                self._runtime_controls.pop(request.program_run_id, None)
            with self._terminal_observation_lock:
                self._pending_terminal_snapshots.pop(
                    request.program_run_id,
                    None,
                )
            return result

    def shutdown(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._journal is not None:
            self._journal.set_change_observer(None)
        with self._control_state_observer_lock:
            self._control_state_observers.clear()
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
        try:
            self._diagnostics.close()
        except Exception as error:
            errors.append(error)
        if self._diagnostic_client is not None:
            try:
                self._diagnostic_client.close()
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
