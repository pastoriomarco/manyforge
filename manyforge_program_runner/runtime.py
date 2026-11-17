"""Independent Program orchestration owner assembled from focused clients."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Iterator, Mapping, Sequence
from contextlib import contextmanager
from copy import deepcopy
from dataclasses import dataclass, replace
from itertools import count
import hashlib
import logging
from pathlib import Path
import re
import sqlite3
from threading import Event, Lock, RLock, Thread, current_thread, get_ident, local
import time
from typing import Any, Final, NoReturn
import unicodedata
from urllib.parse import urlsplit

import rfc8785

from manyforge_behavior.assistant_program_port import (
    HeadlessProgramSceneSource,
    ProgramAssistantPort,
    ProgramModelTransport,
    ProgramSnapshotSource,
)
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.failure_details import exception_failure_details
from manyforge_behavior.kernel_client import KernelClient, PushedOperationTable
from manyforge_behavior.node_availability import (
    AvailabilityFact,
    project_node_execution_availability,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    NodeAdapterPorts,
    OwnerTaskPort,
    ProgramRunPortContext,
)
from manyforge_behavior.orchestration_values import (
    ProgramValueError,
    ProgramValueStore,
    input_failure_reason,
)
from manyforge_behavior.program import (
    CANONICAL_DEFAULT_RECOVERY_POLICY,
    BlackboardKeyDeclaration,
    INTERRUPT_STATE_NAMESPACE,
    Program,
    ProgramValidationError,
    ProgramFrameReferenceError,
    is_interrupt_state_key,
)
from manyforge_behavior.production_ports import (
    HeadlessSceneProgramPort,
    NativeKernelProgramPort,
    ProductionPortError,
    ProgramMotionPreviewUnavailableError,
    SimulationResetPoseSourceTaskPort,
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
from manyforge_behavior.program_runtime import NodeInputProposalError
from manyforge_behavior.recovery import is_non_moving_recovery, observed_state_refusal
from manyforge_behavior.request_keys import (
    RUNNER_KEYS,
    Part,
    format_request_key,
    parse_request_key,
    with_part,
)
from manyforge_behavior.program_scheduling import (
    ProgramInterventionRegistry,
    ProgramRunHoldFacts,
    ProgramRunTickHost,
    ProgramSchedulingError,
)
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.program_cycle import (
    MAXIMUM_ENDED_COMMAND_ROWS,
    PROGRAM_COMMAND_REPLY_SCHEMA,
)
from manyforge_headless_api.component_model import (
    RUN_READINESS_REQUEST_SCHEMA,
    run_cycle_facts,
)
from manyforge_behavior.program_frame_facts import ProgramFrameFacts
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.models import (
    ManualProgramHandover,
    ProgramRunStartRequest,
    ProgramScenePreparationRequest,
)
from manyforge_headless_api.program_model_transport import (
    HeadlessProgramModelAuditSink,
)
from manyforge_headless_api.program_binding_projection import (
    validate_committed_scenario_activation,
)
from manyforge_headless_api.program_binding_transport import ProgramBindingTransportError

from manyforge_headless_api.cell_admission import (
    CellAdmissionClient,
    CellAdmissionError,
)
from .cycle_journal import ProgramCycleJournal
from .bbkey_publication import (
    FAULT_CODE as BBKEY_PUBLICATION_FAULT_CODE,
    ProgramBlackboardKeyPublisher,
    ProgramBlackboardPublicationScope,
)
from .active_tcp_mirror import ActiveTcpMirror, ActiveTcpWireError, decode_active_tcp_rows
from .execution_transport import ProgramRunnerExecutionTransport
from .heap_settle import freeze_heap, settle_heap
from .interrupt_state_mirror import InterruptRuleStateMirror, RuleCatalog
from .liveness import (
    TICK_WORKER_FAULT_CODE,
    ProgramRunnerLivenessError,
    ProgramRunnerLivenessReceiver,
)
from .models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramCycleStatusDTO,
    ProgramHaltResumeRequest,
    ProgramNodeAvailabilityRequest,
    ProgramNodeCorrectionPreviewRequest,
    ProgramSelectionResumeRequest,
    ProgramSelectionStartRequest,
    ProgramManualHandoverRequest,
    PersistentKeyValueResetRequest,
    PersistentKeyValueSetRequest,
    PersistentKeyValuesClearRequest,
    RuntimeBlackboardWriteRequest,
)
from .persistent_keys import (
    OPERATOR_WRITABLE_ACCESS,
    CommittedActivation,
    PersistentKeyEdit,
    PersistentKeyReconciliation,
    PersistentKeyRegister,
    PersistentKeyRegisterError,
    PersistentKeyRegisterVersionError,
)
from .runtime_controls import (
    ProgramRuntimeControlError,
    ProgramRuntimeControls,
    manual_result_availability,
)
from .diagnostics import ProgramDiagnosticPublisher


_LOGGER = logging.getLogger(__name__)
# A transport without the rule-state channel has no global rules to wait for.
_NO_GLOBAL_RULES: Final = RuleCatalog(None, 1, "", "", (), ())
PROGRAM_RUN_INDEX_VERSION: Final = "manyforge.program_run_index.v3"
PERSISTENT_KEY_VALUES_VERSION: Final = "manyforge.persistent_key_values.v3"
PERSISTENT_KEY_EDIT_VERSION: Final = "manyforge.persistent_key_edit.v2"
PERSISTENT_KEY_RECONCILIATION_VERSION: Final = (
    "manyforge.persistent_key_reconciliation.v2"
)
PROGRAM_NODE_PREVIEW_INTENT_VERSION: Final = (
    "manyforge.program_node_correction_preview_intent.v3"
)
_MAX_INFLIGHT_COMMANDS: Final = 4_096
# Recovery actions that release Program motion; the non-moving
# acknowledge_and_continue and the Reset path stay available.
# Continue releases the hold's groups by Retry's rule, so it is checked alike.
_MOTION_PRODUCING_RECOVERY_ACTIONS: Final = frozenset(
    {
        "retry_motion_from_current_state",
        "retry_after_scene_repair",
        "replan_from_stopped_state",
        "retry_runtime_step",
        "continue_with_failure",
    }
)


def _released_failure_hold(
    held: Sequence[Mapping[str, Any]], projected: Mapping[str, Any]
) -> bool:
    """Whether an accepted effect resolved a failure row held before it."""

    remaining = projected.get("operation_failures") or ()
    return any(dict(row) not in remaining for row in held)


class ProgramRunnerError(RuntimeError):
    """Stable failure from the isolated Program-runner owner."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        details: Mapping[str, Any] | None = None,
        producer: str = "program_runner",
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = deepcopy(dict(details or {}))
        self.producer = producer


def _scheduling_cause(error: Exception) -> str | None:
    """The typed cause a tick-host refusal carries (OD-267), else None."""

    return error.code if isinstance(error, ProgramSchedulingError) else None


# OD-214: the codes that decide a command row's outcome (contract §0.1).
_RECONCILIATION_REQUIRED: Final = "program_cycle_reconciliation_required"
_RECOVERY_REQUIRED: Final = "program_cycle_recovery_required"
_FAILED_WITH_EFFECT: Final = frozenset({_RECOVERY_REQUIRED, "program_run_started_then_failed"})
# The command carrier of the thread running a command for a reply at
# acceptance (``run_command``); unset on every other thread.
_carrier_of_thread = local()


@dataclass(frozen=True)
class _KeyedRequest:
    request_key: str | None


class _CommandCarrier:
    """The hand-off between a reply waiting for acceptance and the thread
    running the command: set once, when the command is accepted or ends."""

    __slots__ = ("decided", "row", "error")

    def __init__(self) -> None:
        self.decided = Event()
        self.row: _CommandRow | None = None
        self.error: BaseException | None = None


@dataclass(eq=False)
class _CommandRow:
    """One runner command from taking the runner to its end (OD-214).
    Mutated only under the runner's command lock."""

    kind: str
    request_key: str
    thread: int
    carrier: _CommandCarrier | None
    step: str | None = None
    accepted_at_ns: str | None = None
    end: dict[str, Any] | None = None
    # Its request was forgotten as a proven no-effect command (reject_request).
    rejected: bool = False
    # A refusal before acceptance, given again to a resend of the key.
    refusal: ProgramRunnerError | None = None

    def view(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "request_key": self.request_key,
            "step": self.step,
            "accepted_at_ns": self.accepted_at_ns,
            "end": None if self.end is None else dict(self.end),
        }


def _command_end(row: _CommandRow, error: BaseException | None) -> dict[str, Any]:
    """The row's end from how the command's body ended (contract §0.1)."""

    finished_at_ns = str(time.time_ns())
    if error is None:
        return {"outcome": "succeeded", "code": None, "details": None,
                "finished_at_ns": finished_at_ns}
    if not isinstance(error, ProgramRunnerError):
        return {
            "outcome": "unknown",
            "code": "program_runtime_control_outcome_unknown",
            "details": {"command_kind": row.kind, "cause_code": type(error).__name__},
            "finished_at_ns": finished_at_ns,
        }
    # The cycle status publishes the current recovery offer itself.
    details = {key: value for key, value in error.details.items() if key != "recovery_offer"}
    cause = details.get("cause_code")
    if row.accepted_at_ns is None:
        outcome = "refused"
    elif error.code == _RECONCILIATION_REQUIRED or (
        error.code == _RECOVERY_REQUIRED and cause == _RECONCILIATION_REQUIRED
    ):
        outcome = "unknown"
    elif error.code in _FAILED_WITH_EFFECT:
        outcome = "failed"
    elif row.rejected:
        outcome = "refused"
    else:
        outcome = "failed"
    return {"outcome": outcome, "code": error.code, "details": details or None,
            "finished_at_ns": finished_at_ns}


def _persistent_storage_error(error: PersistentKeyRegisterError) -> ProgramRunnerError:
    return ProgramRunnerError("persistent_key_storage_unavailable", str(error))


def _validate_persistent_value(
    program: Program, declaration: BlackboardKeyDeclaration, value: Any
) -> None:
    """Check one value exactly as a run's own write does: access, type,
    fields, and pose frames and scene resources against the Program."""

    store = ProgramValueStore(
        program,
        persistent_commit=lambda _declaration, _value, _metadata: None,
    )
    try:
        store.write(declaration.key, value)
    except ProgramValueError as error:
        cause = error.__cause__
        details = cause.details if isinstance(cause, ProgramFrameReferenceError) else None
        raise ProgramRunnerError("persistent_key_value_invalid", str(error), details=details) from error


_APPLICATION_START_ID = re.compile(r"[0-9a-f]{16}")


def _request_key(request: Any) -> bytes:
    """The key Headless formed for this forwarded request (1.15.1).

    A request without one is an internal contract violation, refused before
    any effect; the runner mints no fallback.
    """

    text = request.request_key
    if text is None:
        raise ProgramRunnerError(
            "request_key_required", "the forwarded request carries no request key"
        )
    try:
        return parse_request_key(text)
    except ValueError as error:
        raise ProgramRunnerError("request_key_invalid", str(error)) from error


def parse_application_start_id(text: str) -> int:
    """The application-start identity ``A`` from its only text form: exactly
    16 lowercase hexadecimal characters, not all zeros."""

    if not isinstance(text, str) or _APPLICATION_START_ID.fullmatch(text) is None:
        raise ValueError("application start id must be 16 lowercase hex characters")
    value = int(text, 16)
    if value == 0:
        raise ValueError("application start id must not be zero")
    return value


def format_application_start_id(value: int) -> str:
    return f"{value:016x}"


@dataclass(frozen=True, slots=True)
class ProgramRunnerConfig:
    headless_url: str
    # ``A``, handed over by the Manager at launch (never minted here).
    application_start_id: int
    execution_run_root: Path | None = None
    cycle_journal_path: Path | None = None
    owner_id: str = "program-runner"
    headless_timeout_seconds: float = 30.0
    tick_interval_seconds: float = 0.01
    tick_workers: int = 4
    maximum_retained_runs: int = 4_096
    maximum_inflight_commands: int = _MAX_INFLIGHT_COMMANDS
    manager_liveness_socket: Path | None = None
    cell_id: str | None = None
    persistent_key_register_directory: Path | None = None

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
        if (
            type(self.application_start_id) is not int
            or not 0 < self.application_start_id < 2**64
        ):
            raise ValueError("application_start_id must be a non-zero 64-bit value")
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
        if (
            self.persistent_key_register_directory is not None
            and not self.persistent_key_register_directory.is_absolute()
        ):
            raise ValueError(
                "persistent-key register directory must be a stable absolute path"
            )


class _SelectedSimulationContextPort:
    """Bind the configured simulator only for this captured Program's dependency."""

    def __init__(
        self, port: Any, required: Callable[[ProgramRunPortContext], tuple[bool, AvailabilityFact]]
    ) -> None:
        self._port = port
        self._required = required

    def bind(self, context: ProgramRunPortContext) -> None:
        required, availability = self._required(context)
        if required:
            if not availability.available:
                raise ProgramRunOwnerError(
                    "Cannot bind Program context: required simulator is unavailable for the applied cell or configured owner"
                )
            self._port.bind(context)

    def unbind(self, program_run_id: int) -> None:
        self._port.unbind(program_run_id)


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
        # The runner's identity is the application start ``A`` (16 hex).
        self.application_start_id = format_application_start_id(
            config.application_start_id
        )
        # Opened first: a register this runner cannot read is refused by name
        # before any other owner resource exists. It is never rewritten here.
        try:
            self._persistent_keys = (
                None
                if config.persistent_key_register_directory is None
                else PersistentKeyRegister(
                    config.persistent_key_register_directory,
                    application_start_id=self.application_start_id,
                )
            )
        except PersistentKeyRegisterVersionError as error:
            raise ProgramRunnerError(error.code, str(error)) from error
        except PersistentKeyRegisterError as error:
            raise ProgramRunnerError(
                "persistent_key_storage_unavailable", str(error)
            ) from error
        self._admission = admission_client or (
            None
            if config.execution_run_root is None
            else CellAdmissionClient(
                config.execution_run_root,
                timeout_seconds=min(config.headless_timeout_seconds, 30.0),
            )
        )
        try:
            self._journal = cycle_journal or (
                None
                if config.cycle_journal_path is None
                else ProgramCycleJournal(
                    config.cycle_journal_path,
                    maximum_inflight_commands=config.maximum_inflight_commands,
                    maximum_retained_runs=config.maximum_retained_runs,
                )
            )
        except (OSError, RuntimeError, sqlite3.Error) as error:
            # A journal that cannot be created or opened refuses the start
            # by name, like the register above.
            raise ProgramRunnerError(
                "program_cycle_journal_unavailable", str(error)
            ) from error
        # One synchronous BB key sender per captured Run.
        # Key None is the cell-level source between Runs.
        self._bbkey_publishers: dict[int | None, ProgramBlackboardKeyPublisher] = {}
        # Sources whose baseline waits for the global rule catalog of
        # their own publication: (publisher, store, Program keys, deadline ns).
        self._bbkey_pending: dict[
            str | None,
            tuple[
                ProgramBlackboardKeyPublisher,
                ProgramValueStore,
                tuple[str, ...],
                tuple[int, str, str],
                int,
            ],
        ] = {}
        # The Applied Program's condition-key union, learned at cold Apply and
        # Run start; empty until then.
        self._applied_condition_keys: tuple[str, ...] = ()
        self._bbkey_idle_store: ProgramValueStore | None = None
        self._bbkey_publishers_lock = RLock()
        # The Run and between-Runs stores are registered independently of BB
        # publication: a Program with no keys still needs the robot-owned Active TCP values.
        self._active_tcp_stores_lock = Lock()
        self._active_tcp_stores: dict[
            str | None, tuple[ProgramValueStore, dict[str, tuple[str, ...]]]
        ] = {}
        self._active_tcp_mirror = ActiveTcpMirror(on_change=self._write_active_tcp)
        # One source creation at a time (check, retire, create, install,
        # start). Held across I/O, so never the publishers lock above.
        self._bbkey_source_lock = RLock()
        # The publisher fence held by source faults until every loss is
        # confirmed or superseded by an ACKed successor.
        self._bbkey_fence_lock = Lock()
        # Unconfirmed losses: token -> the lost source's creation ordinal.
        self._bbkey_open_losses: dict[object, int] = {}
        self._bbkey_source_ordinals = count(1)
        self._bbkey_fence_reference: str | None = None
        # The Kernel's global rule states, mirrored into the
        # reserved interrupt_state.<rule_id> keys of the current stores.
        self._interrupt_state_mirror: InterruptRuleStateMirror | None = None
        self._interrupt_state_run_id: int | None = None
        # The grasp-lost key mirror's own Run binding: bound before
        # the Running CAS, separate from the interrupt-state mirror's.
        self._carried_object_lost_run_id: int | None = None
        self._carried_object_lost_run_active = False
        self._carried_object_lost_seen_event_ids: set[str] = set()
        # The mirror's event subscription (one thread, started with the Runner).
        self._interrupt_state_stop = Event()
        self._interrupt_state_follower: Thread | None = None
        self._grasp_lost_mirror_lock = Lock()
        self._liveness: ProgramRunnerLivenessReceiver | None = None
        if config.manager_liveness_socket is not None:
            if self._journal is None:
                raise ValueError("Manager liveness requires the durable cycle journal")
            self._liveness = ProgramRunnerLivenessReceiver(
                config.manager_liveness_socket,
                cell_id=config.cell_id or "",
                receiver_runtime_unit_id=self.owner_id,
                receiver_runtime_epoch=self.application_start_id,
                cycle_status=self._current_cycle_status_projection,
                publication_check=self._retry_pending_bbkey_sources,
            )
        self._owns_headless_client = headless_client is None
        self._headless = headless_client or HeadlessHttpClient(
            config.headless_url,
            timeout_seconds=config.headless_timeout_seconds,
        )
        # A Runner under a Manager latches pose observations into Scene
        # between nodes, through Headless (OD-294).
        scene_latch = config.manager_liveness_socket is not None
        if execution_transport is None:
            if config.execution_run_root is None:
                raise ValueError(
                    "Program runtime requires the private execution run root"
                )
            execution_transport = ProgramRunnerExecutionTransport(
                config.execution_run_root,
                timeout_seconds=min(config.headless_timeout_seconds, 30.0),
            )
        # OD-213: the nodes read their operations from the event stream's
        # pushed table, which the follower thread keeps; a runner without the
        # follower (no Manager liveness or no subscription) reads instead.
        self._operation_table = PushedOperationTable()
        follows_events = (
            self._liveness is not None
            and callable(getattr(execution_transport, "read_interrupt_rule_state_events", None))
            and callable(getattr(execution_transport, "subscribe_events", None))
        )
        self._kernel = KernelClient(
            execution_transport,
            self._operation_table if follows_events else None,
        )
        self._execution_transport = execution_transport
        self._scene = HeadlessSceneProgramPort(
            self._headless,
            resolve_attach_active_tcp=getattr(execution_transport, "resolve_attach_active_tcp", None),
            program_node_evidence=self._program_node_admission_evidence,
            application_start_id=self.application_start_id,
        )
        read_rule_states = getattr(execution_transport, "read_interrupt_rule_state_events", None)
        if callable(read_rule_states):
            subscribe = getattr(execution_transport, "subscribe_events", None)
            self._interrupt_state_mirror = InterruptRuleStateMirror(
                read_events=read_rule_states,
                on_change=self._apply_interrupt_states,
                subscribe=subscribe if callable(subscribe) else None,
                on_carried_object_lost=self._mirror_carried_object_lost_signals,
                on_program_run_hold=self._apply_program_run_hold,
            )
        self._reset_position_identity: dict[str, Any] | None = None
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
            active_tcp_result=self._apply_active_tcp_result,
            scene_pose_source_latch=(
                self._scene.latch_between_nodes if scene_latch else None
            ),
        )
        simulator_control_port = self._simulation_control
        if simulator_control_port is not None:
            simulator_control_port = SimulationResetPoseSourceTaskPort(
                simulator_control_port,
                self._scene if scene_latch else None,
                on_reset=lambda _identity: self._reset_active_tcp(),
            )
        self._assistant = (
            None
            if model_transport is None
            else ProgramAssistantPort(
                model_transport,
                HeadlessProgramSceneSource(self._headless),
                application_start_id=self.application_start_id,
                audit_sink=HeadlessProgramModelAuditSink(self._headless),
                snapshot_source=snapshot_source,
            )
        )
        context_members = [self._native, self._scene]
        if self._assistant is not None:
            context_members.append(self._assistant)
        if self._simulation_control is not None:
            context_members.append(_SelectedSimulationContextPort(
                self._simulation_control,
                lambda context: self._simulation_context_requirement(
                    program=context.program,
                    program_binding=context.program_binding,
                    retained_bundle_id=context.retained_bundle_id,
                    graph_revision=context.graph_revision,
                ),
            ))
        self._interventions = ProgramInterventionRegistry(
            maximum_retained_runs=config.maximum_retained_runs
        )
        self._ports = NodeAdapterPorts(
            kernel_operations=self._native.operations,
            kernel_controls=self._native.controls,
            model_queries=self._native.queries,
            gripper_status=self._native.queries,
            io_signals=self._native.queries,
            io_watches=self._native.io_watches,
            mock_simulation=self._native.mock_simulation,
            scene_transactions=self._scene,
            assistant_requests=self._assistant,
            simulator_control=simulator_control_port,
            program_run_scope=self._native.scope,
            program_run_context=CompositeProgramRunContextPort(*context_members),
            program_interventions=self._interventions,
            persistent_keys=self._persistent_keys,
        )
        self._owner_availability = self._build_owner_availability()
        self._owner = ProgramRunOwner(
            owner_id=self.owner_id,
            application_start_id=self.application_start_id,
            ports=self._ports,
            maximum_retained_runs=config.maximum_retained_runs,
        )
        self._terminal_observation_lock = RLock()
        # Keys of Forced Program Ends no request causes, one per run (class 1,
        # minter runner), so a resend of that end reuses its key. Entries go
        # with the run at Reset. Single dict operations only (atomic in
        # CPython, like the minter's counter), so no lock is taken.
        self._forced_end_keys: dict[int, bytes] = {}
        # The latest run's `ProgramRunEnd` as observed at its end.
        self._run_ends: dict[str, dict[str, Any]] = {}
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
            failure_observer=self._fence_tick_worker_failure,
        )
        if self._journal is not None:
            self._journal.set_change_observer(self._notify_control_state_change)
        # One runner command at a time (OD-144): a command that meets another
        # in progress is refused at once, never queued. The one in progress
        # and the last ended ones are rows of the cycle status's
        # `runner_commands` (OD-214), in memory only. Its nested step (a
        # recovery action's Reset) runs on the same thread. The lock guards
        # only these rows. Stop never enters the runner.
        self._command_lock = Lock()
        self._command: _CommandRow | None = None
        self._ended_commands: deque[_CommandRow] = deque(
            maxlen=MAXIMUM_ENDED_COMMAND_ROWS
        )
        self._started = False
        self._closed = False

    @contextmanager
    def _runner_command(self, kind: str, request_key: str | None) -> Iterator[None]:
        """Hold the runner for one command, or refuse it at once (OD-144).

        The same request key sent again while its first send is admitted is
        answered `execution_control_command_in_progress`; any other command
        meets the runner busy (`execution_control_ingress_busy`, naming the
        kind in progress). Neither waits nor runs later, and neither is a row.
        A command that takes the runner is a row from here to its end
        (OD-214); the end is set when the body returns or raises, so no
        command is ever dropped without one.
        """

        key = format_request_key(_request_key(_KeyedRequest(request_key)))
        thread = get_ident()
        with self._command_lock:
            current = self._command
            if current is not None and current.thread != thread:
                if current.request_key == key:
                    raise ProgramRunnerError(
                        "execution_control_command_in_progress",
                        "the exact command is already in progress",
                    )
                raise ProgramRunnerError(
                    "execution_control_ingress_busy",
                    f"another Program command is in progress: {current.kind}",
                    details={"command_in_progress": current.kind},
                )
            nested = current is not None
            if not nested:
                row = _CommandRow(
                    kind, key, thread, getattr(_carrier_of_thread, "carrier", None)
                )
                self._command = row
        if nested:
            yield
            return
        if row.carrier is not None:
            row.carrier.row = row
        self._notify_control_state_change()
        error: BaseException | None = None
        try:
            yield
        except BaseException as raised:
            error = raised
            raise
        finally:
            end = _command_end(row, error)
            with self._command_lock:
                if row.accepted_at_ns is None and error is None:
                    # Decided and done at once: accepted as it ended.
                    row.accepted_at_ns = end["finished_at_ns"]
                row.end = end
                if row.accepted_at_ns is None and isinstance(error, ProgramRunnerError):
                    row.refusal = error
                self._command = None
                self._ended_commands.appendleft(row)
            if error is not None and not isinstance(error, ProgramRunnerError):
                _LOGGER.error(
                    "program_runner_command_failed kind=%s request_key=%s",
                    kind, key, exc_info=error,
                )
            self._notify_control_state_change()

    def _held_command(self) -> _CommandRow | None:
        """The row of the command this thread holds, if any."""

        row = self._command
        return row if row is not None and row.thread == get_ident() else None

    def _command_accepted(self, step: str) -> None:
        """The command is recorded and will be carried out to its end: its
        reply may be sent now (OD-214). Inside an accepted command (a recovery
        action's nested Reset) it only names the step."""

        row = self._held_command()
        if row is None:
            return
        with self._command_lock:
            newly = row.accepted_at_ns is None
            if newly:
                row.accepted_at_ns = str(time.time_ns())
            row.step = step
        self._notify_control_state_change()
        if newly and row.carrier is not None:
            row.carrier.decided.set()

    def _command_step(self, step: str) -> None:
        """The held command entered one of its real stages."""

        row = self._held_command()
        if row is None:
            return
        with self._command_lock:
            row.step = step
        self._notify_control_state_change()

    def _command_refused_by_journal(self) -> None:
        row = self._held_command()
        if row is not None:
            with self._command_lock:
                row.rejected = True

    def _command_reset_phase(self, cycle: Any) -> None:
        """A Reset's step is the cycle's phase it has just entered."""

        if cycle.reset_phase is not None:
            self._command_step(cycle.reset_phase)

    def _command_in_progress(self) -> str | None:
        command = self._command
        return None if command is None else command.kind

    def _runner_commands(self) -> dict[str, Any]:
        with self._command_lock:
            current = self._command
            return {
                "in_progress": None if current is None else current.view(),
                "ended": [row.view() for row in self._ended_commands],
            }

    def run_command(
        self, request_key: str | None, command: Callable[[], Any]
    ) -> dict[str, Any]:
        """Answer one runner command at acceptance (OD-214).

        The command runs on a thread of its own that holds the runner until
        the command's end; this call returns once it is accepted (the row as
        it is then) or has ended (refused at once: its error is raised).
        A key seen before gets its kept row back and runs nothing again.
        """

        key = format_request_key(_request_key(_KeyedRequest(request_key)))
        with self._command_lock:
            current = self._command
            kept = next(
                (
                    row
                    for row in ((current,) if current is not None else ())
                    + tuple(self._ended_commands)
                    if row.request_key == key
                ),
                None,
            )
            view = None if kept is None else kept.view()
            refusal = None if kept is None else kept.refusal
        if view is not None:
            if view["accepted_at_ns"] is None and view["end"] is None:
                raise ProgramRunnerError(
                    "execution_control_command_in_progress",
                    "the exact command is already in progress",
                )
            if refusal is not None:
                raise ProgramRunnerError(
                    refusal.code, str(refusal),
                    details=refusal.details, producer=refusal.producer,
                )
            return {"schema_version": PROGRAM_COMMAND_REPLY_SCHEMA, "command": view}
        carrier = _CommandCarrier()

        def carry() -> None:
            _carrier_of_thread.carrier = carrier
            try:
                command()
            except BaseException as error:  # noqa: BLE001 - the row has it
                carrier.error = error
            finally:
                carrier.decided.set()

        # One command at a time (OD-144): at most one such thread runs a
        # command; a refused one ends at once. Daemon: at application end the
        # process does not wait for a command (its row is memory only).
        Thread(target=carry, name="manyforge_program_runner_command", daemon=True).start()
        carrier.decided.wait()
        row = carrier.row
        if row is None or (row.accepted_at_ns is None and carrier.error is not None):
            assert carrier.error is not None
            raise carrier.error
        with self._command_lock:
            view = row.view()
        return {"schema_version": PROGRAM_COMMAND_REPLY_SCHEMA, "command": view}

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
        program_run_id = snapshot.capture.program_run_id
        for event in self._owner.take_node_history_events(program_run_id):
            facts = dict(event)
            self._controls(program_run_id).audit_value(facts.pop("kind"), **facts)
        record = self._owner.take_diagnostic(program_run_id)
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
                error.code or "program_runner_manager_liveness_unavailable",
                str(error),
            ) from error

    def _fence_tick_worker_failure(self, error: Exception) -> None:
        """A failed tick worker (OD-267) fences the cell as a Runner fault
        that never clears."""

        if self._liveness is None:
            return
        text = unicodedata.normalize(
            "NFC", f"{type(error).__name__}: {error}"
        ).encode("utf-8")[:1_024].decode("utf-8", errors="ignore")
        fault = {"code": TICK_WORKER_FAULT_CODE, "message": text, "details": {}}
        self._liveness.latch_runner_fault(fault, self._digest(fault))

    def _current_cycle_status_projection(self) -> dict[str, Any]:
        """Project durable cycle facts without claiming current ownership.

        The private liveness probe must remain available while a replacement
        Runner is fenced, otherwise Manager cannot identify the receiver and
        deliver the reconciliation heartbeat.  Manager treats this probe as
        preflight only and publishes readiness after the heartbeat returns a
        stable current status.
        """
        status, accepted_reset = self._cycle_status_projection()
        status["accepted_reset"] = accepted_reset
        return status

    def _kernel_cycle_status(self) -> dict[str, Any]:
        """This runner's current cycle status in the Kernel's six-field shape.

        Reset proofs (the Reset claim and both Reset authorizations) carry it,
        so the Kernel validates them in memory and never reads this runner's
        journal (OD-139). The same facts the liveness probe publishes.
        """
        status, accepted_reset = self._cycle_status_projection()
        return {
            "cycle_generation": status["cycle_generation"],
            "state": status["state"],
            "program_run_id": status["program_run_id"],
            "cycle_claim_generation": status["cycle_claim_generation"],
            "terminal_or_settlement_reference": status["terminal_or_settlement_reference"],
            "accepted_reset": accepted_reset,
        }

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

    def _simulation_node_requirements(
        self, node_id: str, parameters: Mapping[str, Any]
    ) -> tuple[bool, frozenset[tuple[str, str, str]]]:
        entry = self._environment.catalog.get(node_id)
        effective = {
            parameter.id: parameter.default
            for parameter in entry.parameters if parameter.has_default
        }
        effective.update(parameters)
        simulator = False
        targets: set[tuple[str, str, str]] = set()
        for requirement in entry.required_capabilities:
            if (requirement.when_parameter is not None
                and effective.get(requirement.when_parameter) != requirement.when_equals):
                continue
            if (entry.execution.owner_api == "simulator.control"
                and requirement.scope == "cell_global"):
                if requirement.capability_id not in {
                    "simulator.control.reset", "simulator.control.pause",
                    "simulator.control.play", "simulator.control.step",
                    "simulator.entity_state.set",
                }:
                    raise ProgramRunOwnerError(
                        "Configured simulator owner declares an unsupported cell-global capability: "
                        + requirement.capability_id
                    )
                simulator = True
            if requirement.scope == "target_binding":
                reference = effective.get(requirement.target_parameter)
                if not isinstance(reference, Mapping):
                    raise ProgramRunOwnerError("Program simulation dependency has no exact reference")
                component = reference.get("component_id")
                if not isinstance(component, str) or not component:
                    raise ProgramRunOwnerError("Program simulation dependency has an invalid component")
                if set(reference) == {"component_id", "command_target_id"}:
                    kind, identity = "target", reference["command_target_id"]
                elif set(reference) == {"component_id", "signal_name"}:
                    kind, identity = "signal", reference["signal_name"]
                else:
                    raise ProgramRunOwnerError("Program simulation dependency reference is invalid")
                if not isinstance(identity, str) or not identity:
                    raise ProgramRunOwnerError("Program simulation dependency identity is invalid")
                targets.add((kind, component, identity))
        return simulator, frozenset(targets)

    def _simulation_context_requirement(
        self,
        *,
        program: Mapping[str, Any],
        program_binding: Mapping[str, Any],
        retained_bundle_id: str,
        graph_revision: str,
    ) -> tuple[bool, AvailabilityFact]:
        """Separate captured dependency from current typed simulator eligibility.

        The simulator a ROS-hosted component depends on is the Manager's
        deployment-derived ``component_simulator_dependencies`` row, never a
        decoded lane selector, so Mock, Isaac and Real components share one rule.
        """

        unavailable = AvailabilityFact(False, ("program_runtime_owner_unavailable",))
        configured = AvailabilityFact(True) if self._simulation_control is not None else unavailable

        def rows(value: Any) -> Sequence[Mapping[str, Any]]:
            if not isinstance(value, (list, tuple)) or any(
                not isinstance(row, Mapping) for row in value
            ):
                raise ProgramRunOwnerError("Program simulation dependency has invalid rows")
            return value

        tree = program.get("tree")
        if not isinstance(tree, Mapping):
            raise ProgramRunOwnerError("Program simulation dependency has no normalized tree")
        references: set[tuple[str, str, str]] = set()
        simulator_node = False
        pending = [tree]
        while pending:
            node = pending.pop()
            parameters = node.get("parameters", {})
            if not isinstance(parameters, Mapping):
                raise ProgramRunOwnerError("Program simulation dependency has invalid parameters")
            simulator, targets = self._simulation_node_requirements(node["node_id"], parameters)
            simulator_node |= simulator
            references.update(targets)
            pending.extend(rows(node.get("children", ())))
        if not simulator_node and not references:
            return False, unavailable

        projection = self._headless.manager_graph()
        source = projection.get("source")
        if (projection.get("schema_version") != "manyforge.manager_graph_projection.v2"
            or not isinstance(source, Mapping) or source.get("available") is not True
            or source.get("freshness") != "fresh" or source.get("known_gaps") not in ([], ())
            or projection.get("retained_bundle_id") != retained_bundle_id
            or projection.get("graph_revision") != graph_revision):
            raise ProgramRunOwnerError("Program simulation dependency has no fresh captured Manager graph")
        graph = projection.get("cell_graph")
        if not isinstance(graph, Mapping) or graph.get("graph_revision") != graph_revision:
            raise ProgramRunOwnerError("Program simulation dependency has no captured retained graph")
        graph_components = rows(graph.get("controllable_components"))
        units = rows(graph.get("runtime_units"))

        def ros_profiles(unit: Mapping[str, Any]) -> Sequence[Mapping[str, Any]]:
            configuration = unit.get("configuration")
            if not isinstance(configuration, Mapping):
                raise ProgramRunOwnerError("Program simulation dependency has invalid ROS configuration")
            return rows(configuration.get("controller_profiles"))

        dependencies: dict[str, str] | None = None

        def isaac_dependency(component: str) -> bool:
            # Exactly one closed row per ROS controller profile component.
            nonlocal dependencies
            if dependencies is None:
                published = projection.get("component_simulator_dependencies")
                if not isinstance(published, (list, tuple)):
                    raise ProgramRunOwnerError(
                        "Program simulation dependency has no Manager component simulator rows")
                parsed: dict[str, str] = {}
                for row in published:
                    name = row.get("controllable_component_id") if isinstance(row, Mapping) else None
                    if (not isinstance(row, Mapping)
                        or set(row) != {"controllable_component_id", "simulator"}
                        or not isinstance(name, str) or not name or name in parsed
                        or row.get("simulator") not in ("none", "isaac")):
                        raise ProgramRunOwnerError(
                            "Program simulation dependency has invalid component simulator rows")
                    parsed[name] = row["simulator"]
                profiled = [
                    profile.get("controllable_component_id")
                    for unit in units
                    if unit.get("runtime_profile_id") == "manyforge.ros2_control_execution_runtime"
                    for profile in ros_profiles(unit)
                ]
                if any(not isinstance(name, str) for name in profiled) or set(parsed) != set(profiled):
                    raise ProgramRunOwnerError(
                        "Program simulation dependency rows do not match the ROS controller profiles")
                dependencies = parsed
            return dependencies[component] == "isaac"

        if simulator_node:
            # CELL_GLOBAL means the actual applied cell, not unused Program roles.
            eligible = False
            seen: set[str] = set()
            for unit in units:
                if unit.get("runtime_profile_id") != "manyforge.ros2_control_execution_runtime":
                    continue
                for profile in ros_profiles(unit):
                    component = profile.get("controllable_component_id")
                    matches = [row for row in graph_components if row.get("controllable_component_id") == component]
                    if (not isinstance(component, str) or component in seen or len(matches) != 1
                        or unit.get("runtime_unit_id") not in matches[0].get("runtime_unit_ids", ())):
                        raise ProgramRunOwnerError("Program global simulator profile has foreign runtime ownership")
                    seen.add(component)
                    eligible |= isaac_dependency(component)
            return True, configured if eligible else unavailable

        targets = rows(program_binding.get("target_bindings"))
        components = rows(program_binding.get("component_bindings"))
        signals = rows(program_binding.get("signal_bindings"))
        selected: set[str] = set()
        for kind, component, identity in references:
            if kind == "signal":
                matches = [row for row in signals if row.get("reference") == {
                    "component_id": component, "signal_name": identity
                }]
                if len(matches) != 1 or not isinstance(matches[0].get("target"), Mapping):
                    raise ProgramRunOwnerError("Program simulation dependency has ambiguous signal binding")
                target_identity = matches[0]["target"]
            else:
                target_identity = {"component_id": component, "command_target_id": identity}
            if target_identity.get("component_id") != component:
                raise ProgramRunOwnerError("Program simulation dependency signal crosses its component")
            matches = [row for row in targets if row.get("target") == target_identity]
            if len(matches) != 1:
                raise ProgramRunOwnerError("Program simulation dependency has ambiguous target binding")
            matches = [row for row in components if row.get("component_id") == component]
            if (len(matches) != 1 or not isinstance(target_identity.get("command_target_id"), str)
                or not target_identity["command_target_id"]):
                raise ProgramRunOwnerError("Program simulation dependency crosses its component binding")
            selected.add(component)
        # A provider-only I/O component (provider-kind control domain and no
        # controller binding) has no controller profile and no simulator row;
        # its component state gates it. A component that also has a controller
        # binding keeps the controller-profile check.
        io_providers: set[Any] = set()
        for domain in rows(graph.get("control_domains")):
            if domain.get("kind") == "provider":
                provider = domain.get("provider")
                if not isinstance(provider, Mapping):
                    raise ProgramRunOwnerError("Program simulation dependency has invalid provider domain")
                io_providers.add(provider.get("controllable_component_id"))
        io_providers -= {
            row.get("controllable_component_id") for row in rows(graph.get("control_bindings"))
        }
        required = False
        for component in selected:
            matches = [row for row in graph_components if row.get("controllable_component_id") == component]
            if len(matches) != 1:
                raise ProgramRunOwnerError("Program simulation dependency component is not current")
            unit_ids = matches[0].get("runtime_unit_ids")
            if (not isinstance(unit_ids, (list, tuple)) or not unit_ids
                or any(not isinstance(value, str) or not value for value in unit_ids)
                or len(set(unit_ids)) != len(unit_ids)):
                raise ProgramRunOwnerError("Program simulation dependency has invalid runtime ownership")
            seen_profile = False
            for unit_id in unit_ids:
                matches = [row for row in units if row.get("runtime_unit_id") == unit_id]
                if len(matches) != 1:
                    raise ProgramRunOwnerError("Program simulation dependency runtime is not current")
                unit = matches[0]
                profile_id = unit.get("runtime_profile_id")
                if not isinstance(profile_id, str) or not profile_id:
                    raise ProgramRunOwnerError("Program simulation dependency has no runtime profile")
                if profile_id != "manyforge.ros2_control_execution_runtime" or component in io_providers:
                    continue
                matches = [row for row in ros_profiles(unit) if row.get("controllable_component_id") == component]
                # A no-output pneumatic tool (No output, or driven
                # through pneumatic I/O) is lane-free: no controller profile and
                # no simulator row, as for the provider-only I/O component above.
                if not matches and any(
                        target.get("controllable_component_id") == component
                        and target.get("actuation_kind") == "pneumatic"
                        and (target.get("output_mode") == "none" or target.get("pneumatic_io") is not None)
                        for target in rows(unit["configuration"].get("gripper_target_configurations", ()))):
                    continue
                if len(matches) != 1 or seen_profile:
                    raise ProgramRunOwnerError("Program simulation dependency has no unique ROS controller profile")
                seen_profile = True
                required |= isaac_dependency(component)
        return required, configured if required else unavailable

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
        return ProgramRunnerError(
            error.code, str(error), details=error.details,
            producer=error.producer,
        )

    @staticmethod
    def _runner_error(error: Exception) -> ProgramRunnerError:
        if isinstance(error, ProgramRunnerError):
            return error
        code = (
            error.code or "program_run_scheduling_failed"
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

    def _retain_rejection(
        self,
        journal: ProgramCycleJournal,
        *,
        request_key: bytes,
        error: ProgramRunnerError,
    ) -> None:
        # A proven no-effect command: its row ends `refused` (OD-214).
        self._command_refused_by_journal()
        try:
            journal.reject_request(
                request_key=request_key,
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
            cycle.program_run_id != identity.program_run_id
            or cycle.state != "running"
            or cycle.claim is None
        ):
            raise ProgramRunnerError(
                "program_node_parent_not_current",
                "Node transaction has no current running Program claim",
            )
        return {
            "program_run_id": cycle.program_run_id,
            "parent_program_cycle_claim_generation": cycle.claim["claim_generation"],
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
            "owner_api": owner_api,
        }

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
        # An unknown release (the run's release latches before its phase is
        # recorded), or an unknown Reset-effect claim acquire, is settled
        # first from the Kernel's record: receipt or exact replay. A position
        # reset with an unknown effect runs all its steps again.
        unknown_release = cycle.reset_phase == "releasing" and failed_phase in {
            None, "releasing"
        }
        settles_unknown = unknown_release or cycle.reset_phase == "restoring_position" or (
            cycle.reset_phase in {"restoring_scene", "restoring_position"}
            and cycle.claim is None
            and isinstance(cycle.reset_effect_claim, Mapping)
            and cycle.reset_effect_claim.get("claim") is None
        )
        if (
            (allow_reconciliation or not cycle.reconciliation_required or settles_unknown)
            and cycle.reset_phase
            in {"stopping", "settling", "restoring_scene", "restoring_position", "releasing"}
            and (
                failed_phase == cycle.reset_phase
                or (unknown_release and cycle.reconciliation_required)
            )
            and isinstance(failure_code, str)
            and failure_code
        ):
            return cycle.reset_phase
        return None

    @classmethod
    def _reset_retry_phase(cls, cycle: Any) -> str | None:
        """Return the one durable Reset phase safe to resume, if any."""

        return cls._reset_recovery_phase(
            cycle,
            allow_reconciliation=False,
        )

    def _current_cycle_recovery_reset(self, cycle: Any, pending=None) -> bool:
        """Recognize only retained, current-owner command uncertainty."""
        claim = cycle.claim
        intent = claim.get("intent") if isinstance(claim, Mapping) else None
        if not (
            cycle.reconciliation_required
            and cycle.state in {"recovery_required", "terminal_awaiting_reset"}
            and cycle.program_run_id is not None
            and cycle.reset_phase in {None, "stopping", "settling"}
            and cycle.start_settlement is None
            and cycle.acquisition_pending is None
            and cycle.release_intent is None
            and isinstance(intent, Mapping)
            and intent.get("owner_role") == "program_runner"
            and intent.get("claim_kind") == "program_cycle"
            and type(intent.get("owner_transaction_id")) is int
            and intent["owner_transaction_id"] == cycle.program_run_id
            and type(claim.get("claim_generation")) is int
            and 1 <= claim["claim_generation"] <= 9_007_199_254_740_991
        ):
            return False
        journal = self._require_cycle_owner()[1]
        rows = journal.pending_commands() if pending is None else pending
        resets = [row for row in rows if row["kind"] == "reset"]
        if not rows or len(resets) > 1:
            return False
        for row in rows:
            command = row["intent"]
            if row["cycle_generation"] != cycle.cycle_generation:
                return False
            if row["kind"] in {
                "start_motion",
                "resume_motion",
                "resume_halt",
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
                    if resets[0]["request_key"] != with_part(
                        row["request_key"], Part.NESTED_RESET
                    ):
                        return False
            # Any other uncertain command of this cycle, the in-flight start
            # included: Reset's stop, containment and exact Kernel proof settle it.
        return True

    def _prove_reset_recovery(self, cycle) -> None:
        """Join current terminal/tree and exact Kernel-held parent/children."""
        admission, journal = self._require_cycle_owner()
        terminal = self._project_run(self._owner.read(cycle.program_run_id).to_dict())
        claim = cycle.claim
        if (
            terminal.get("active_operation_count") != 0
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
        held = receipt.get("held_claims")
        if not (
            receipt.get("status") == "held_exact"
            and receipt.get("matched_claim") == claim
            and receipt.get("matched_release") is None
            and isinstance(held, list)
            and held.count(claim) == 1
        ):
            raise RuntimeError("Reset parent claim is not exact")
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
        return self._cycle_status_projection()[0]

    def _cycle_status_projection(self) -> tuple[dict[str, Any], dict[str, Any] | None]:
        """Project cycle and Reset context from one dependency-free snapshot."""
        _admission, journal = self._require_cycle_owner()
        cycle, pending_commands = journal.status_snapshot()
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
        idle_reset = self._idle_reset_occurrence(journal, cycle, pending_commands) is not None
        if (
            acquisition_pending
            and (
                cycle.state not in {"starting", "recovery_required"}
                or cycle.claim is not None
                or cycle.program_run_id is None
            )
        ) or (
            not acquisition_pending
            and cycle.claim is None
            and cycle.state != "idle_after_reset"
            and not claimless_no_effect
            and not idle_reset
        ):
            raise ProgramRunnerError(
                "program_cycle_owner_unknown",
                "the acquire response was lost before its exact claim was journaled",
            )
        # K-6: a run number is announced only once the Kernel holds its
        # claim, so every Stop naming it finds the run's fence.
        announced_run_id = None if acquisition_pending else cycle.program_run_id
        command_target = {
            "cycle_generation": cycle.cycle_generation,
            "program_run_id": announced_run_id,
        }
        reset_retry_phase = self._reset_retry_phase(cycle)
        recovery_reset = self._current_cycle_recovery_reset(cycle, pending_commands)
        # Reset first replays the retained acquire whose outcome is unknown.
        acquire_settle = acquisition_pending and cycle.state == "recovery_required"
        allowed_by_state = {
            "program_run": cycle.state == "idle_after_reset" and not (
                cycle.failure and cycle.failure.get("code") == "program_scene_preparation_failed"
            ),
            "program_motion_start": cycle.state == "running",
            "program_motion_stop": cycle.state in {"starting", "running"},
            "program_motion_resume": cycle.state == "running",
            "program_reset": cycle.state
            in {
                "idle_after_reset",
                "running",
                "terminal_awaiting_reset",
            }
            or reset_retry_phase is not None
            or recovery_reset
            or acquire_settle,
            # The dependency-free liveness projection cannot inspect the
            # retained Program tree. The complete cycle_status() projection
            # enables these only after proving the tree is halted.
            "program_resume_halt": False,
            "program_discard_halt": False,
            "program_runtime_blackboard_write": cycle.state == "running",
            "program_manual_result_stage": cycle.state == "running",
            # Only the Kernel-selected held tasks may be retired; the owner
            # rechecks the exact capture and keeps all Program holds intact.
            "program_manual_handover": cycle.state == "running",
            # These are coarse lifecycle prerequisites, not target permission.
            # During a Program, shared ingress requires exact native manual
            # identity/context; the Kernel retains final admission authority.
            "operation_start": cycle.state in {"idle_after_reset", "running"},
            "operation_stop": cycle.state != "resetting",
            "operation_reset": cycle.state != "starting",
            "operation_acknowledge_completed": cycle.state != "starting",
            "stop_all": True,
            "halt": True,
            # Owner recovery offers exist only while the cycle runs.
            "execution_recovery": cycle.state == "running",
            "runtime_scale": cycle.state != "recovery_required",
        }
        if cycle.reconciliation_required:
            allowed_by_state = {
                kind: kind
                in {
                    "stop_all",
                    "halt",
                    # Exact native manual retirement is needed to release an
                    # unresolved child before Program recovery can complete.
                    "operation_reset",
                    "operation_acknowledge_completed",
                }
                or (
                    kind == "execution_recovery"
                    and cycle.state != "recovery_required"
                )
                or (
                    kind == "program_reset"
                    and (reset_retry_phase is not None or recovery_reset or acquire_settle)
                )
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
                        else "program_scene_preparation_failed"
                        if kind == "program_run" and cycle.failure
                        and cycle.failure.get("code") == "program_scene_preparation_failed"
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
                for row in pending_commands
                if row["cycle_generation"] == cycle.cycle_generation
            ]
            if len(pending) == 1:
                failure = self._unknown_control_failure(pending[0]["kind"])
        status = ProgramCycleStatusDTO.model_validate(
            {
                "cycle_generation": cycle.cycle_generation,
                "state": cycle.state,
                "program_run_id": announced_run_id,
                "cycle_claim_generation": (
                    None if cycle.claim is None else cycle.claim.get("claim_generation")
                ),
                "terminal_or_settlement_reference": cycle.terminal_reference,
                "execution_control_revision": cycle.execution_control_revision,
                "causal_request_key": (
                    None
                    if cycle.causal_request_key is None
                    else format_request_key(cycle.causal_request_key)
                ),
                "current_command_kind": cycle.current_command_kind,
                # OD-144/OD-214: the command holding the runner, the last ended ones.
                "runner_commands": self._runner_commands(),
                "failure": failure,
                "reset_phase": cycle.reset_phase,
                "reset_options": self._accepted_reset_options(journal, cycle, pending_commands),
                # Liveness must not depend on an execution/Manager round trip.
                # The public wrapper replaces this unsampled private field.
                "reset_robot_position": {
                    "available": False,
                    "reason": "Position-reset capability is not sampled by the private cycle projection",
                },
                "allowed_commands": allowed_commands,
            }
        )
        return (status.model_dump(mode="json"),
                self._accepted_reset_context(journal, cycle, pending_commands))

    def cycle_status(self) -> dict[str, Any]:
        result = self._cycle_status_core()
        result["reset_robot_position"] = self._reset_robot_position_availability()
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
        result["recovery_offer"] = recovery_offer
        # Why the run ended (None while it runs): the cycle-level
        # notice reads it instead of inferring "finished" from `failure`.
        result["run_end"] = None if run is None else run.get("run_end")
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
        result["runtime_control_revision"] = (
            None if result["runtime_controls"] is None
            else result["runtime_controls"]["runtime_controls"]["runtime_control_revision"]
        )
        # Refine Program-tree-dependent availability in the complete owner
        # projection. The cycle journal remains the lifecycle authority; the
        # retained tree only narrows commands whose meaning depends on its
        # exact halted/runtime state.
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
            "program_motion_start": run_allows(
                "program_motion_start", *stoppable
            ),
            "program_motion_resume": run_allows("program_motion_resume", *stoppable),
            # A nominal held Stop may retain an interrupted task awaiting
            # cleanup. Its intervention state must not prevent that cleanup;
            # the exact Kernel proof still rejects faulted or unsettled work.
            "program_manual_handover": run_allows(
                "program_manual_handover", *stoppable
            ),
            "program_motion_stop": run_allows("program_motion_stop", *stoppable),
            "program_reset": base_permissions.get("program_reset", False)
            and (
                not reconciliation_fenced
                or selections is not None
                # Settling an unknown Kernel exchange needs no resident run.
                or program_run_id is None
                or result["reset_phase"] in {"restoring_scene", "restoring_position", "releasing"}
                # A start whose run the owner never held settles without it.
                or self._start_failed_without_run(self._require_cycle_owner()[1].read())
            )
            and (
                not cycle_running
                or run_state in {"running", "intervention_required", "halted"}
            ),
            "program_resume_halt": (
                not reconciliation_fenced
                and cycle_running
                and run_state == "halted"
            ),
            "program_discard_halt": (
                not reconciliation_fenced
                and cycle_running
                and run_state == "halted"
            ),
            # A permitted runtime key stays writable while a failed attempt
            # is held for intervention; the value store keeps every access,
            # type and lifecycle check, and a write never resumes motion.
            "program_runtime_blackboard_write": run_allows(
                "program_runtime_blackboard_write", *stoppable
            ),
            "program_manual_result_stage": run_allows(
                "program_manual_result_stage", "running"
            ),
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

    def _controls(self, program_run_id: int) -> ProgramRuntimeControls:
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
            recovery_fences = None
            if self._journal is not None:
                cycle = self._journal.read()
                if cycle.program_run_id == program_run_id and isinstance(
                    cycle.capture_identities, Mapping
                ):
                    value = cycle.capture_identities.get("runtime_recovery_fences")
                    if isinstance(value, Mapping):
                        recovery_fences = value
            try:
                controls = ProgramRuntimeControls(
                    capture,
                    recovery_fences=recovery_fences,
                )
            except ValueError as error:
                raise ProgramRunnerError(
                    "program_runtime_controls_invalid",
                    str(error),
                ) from error
            self._runtime_controls[program_run_id] = controls
            return controls

    def _optional_controls(self, program_run_id: int) -> ProgramRuntimeControls | None:
        """Read runtime controls without making legacy terminal cycles unreadable."""

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
        program_run_id: int | None,
        *,
        cycle_generation: int,
        selection_scope=None,
    ) -> dict[str, Any] | None:
        """The focused hold's offer (a failure hold first, else an operator
        Stop hold): the single-offer field current readers use."""
        if program_run_id is None or not self._offer_cycle_current(
            program_run_id, cycle_generation
        ):
            return None
        intervention = self._owner.intervention(program_run_id)
        if intervention is None:
            return None
        return self._offer_for(
            program_run_id,
            intervention,
            cycle_generation=cycle_generation,
            selection_scope=selection_scope,
        )

    def _recovery_offers(
        self,
        program_run_id: int | None,
        *,
        cycle_generation: int,
    ) -> list[dict[str, Any]]:
        """Every active failure hold, one offer each: the
        hold's own motion groups, its captured Stop scope, and its offer."""
        if program_run_id is None or not self._offer_cycle_current(
            program_run_id, cycle_generation
        ):
            return []
        try:
            holds = self._owner.failure_holds(program_run_id)
        except ProgramRunOwnerError:
            return []
        result = []
        for hold in holds:
            offer = self._offer_for(
                program_run_id, hold, cycle_generation=cycle_generation
            )
            if offer is None:
                continue
            groups, scope = self._owner.hold_motion_groups(
                program_run_id, hold.identity.node_instance_id
            )
            result.append(
                {
                    "motion_group_ids": list(groups),
                    "intervention_stop_scope": scope,
                    "offer": offer,
                }
            )
        return result

    def _offer_cycle_current(self, program_run_id: int, cycle_generation: int) -> bool:
        # A node-level recovery offer is valid only while its owning Program
        # cycle is current and free of unresolved control effects.  Keep this
        # check here so status, controls, results, and rejection responses
        # cannot disagree about whether the offer is actionable.
        if self._journal is None:
            return True
        cycle = self._journal.read()
        return (
            cycle.state == "running"
            and not cycle.reconciliation_required
            and cycle.program_run_id == program_run_id
            and cycle.cycle_generation == cycle_generation
        )

    def _offer_for(
        self,
        program_run_id: int,
        intervention,
        *,
        cycle_generation: int,
        selection_scope=None,
    ) -> dict[str, Any] | None:
        # Status projection uses the retained owner assessment. It must not
        # perform owner I/O; Resume revalidates every selected attempt before
        # releasing any hold.
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
        # The capture omits nodes whose policy is the canonical default.
        recovery_policy = dict(
            CANONICAL_DEFAULT_RECOVERY_POLICY
            if policy is None
            else policy.recovery_policy
        )
        hold_revision = None
        if intervention.policy_parameter == "operator_stop_policy":
            if selection_scope is None:
                selection_scope = self._owner.operator_stop_scope(program_run_id)
            if intervention.identity.node_instance_id in selection_scope.held_nodes:
                hold_revision = selection_scope.hold_revision
        return controls.recovery_offer(
            cycle_generation=cycle_generation,
            application_start_id=self.application_start_id,
            intervention=intervention,
            policy=recovery_policy,
            hold_revision=hold_revision,
            containment_pending=(
                intervention.policy_parameter != "operator_stop_policy"
                and self._owner.failure_pause_stop_pending(program_run_id)
            ),
        )

    def _captured_recovery_policy(
        self, program_run_id: int, node_instance_id: str
    ) -> dict[str, Any]:
        """The run-start captured recovery policy of one node, default included."""

        try:
            live = self._owner.read(program_run_id)
        except ProgramRunOwnerError as error:
            raise ProgramRunnerError("program_run_not_found", str(error)) from error
        policy = live.capture.node_policy_by_instance_id.get(node_instance_id)
        return dict(
            CANONICAL_DEFAULT_RECOVERY_POLICY
            if policy is None
            else policy.recovery_policy
        )

    @staticmethod
    def _run_recovery_fences(
        applied: Mapping[str, Any],
        execution: Mapping[str, Any],
        *,
        scene_admission_epoch: int,
    ) -> dict[str, int]:
        source = applied.get("source")
        status = execution.get("identity")
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
        # Program-run status v1 stays a stable owner contract. Runtime-control state is a
        # separate joined projection so adding controls never weakens or
        # silently version-skews existing run consumers.
        projected = deepcopy(dict(value))
        capture = projected.get("capture")
        run_end = self._run_ends.get(
            capture.get("program_run_id") if isinstance(capture, Mapping) else None
        )
        if run_end is not None and projected.get("run_end") is not None:
            # The end observed with the cell latch (`cell_fault`).
            projected["run_end"] = deepcopy(run_end)
        return projected

    def _terminalize_failed_start(
        self,
        journal: ProgramCycleJournal,
        *,
        scene_proof: Mapping[str, Any] | None,
        effect_may_have_started: bool,
        request_key: bytes,
        started_snapshot: Mapping[str, Any] | None = None,
        failure: ProgramRunnerError | None = None,
    ) -> None:
        current = journal.read()
        if current.state == "terminal_awaiting_reset":
            return
        if current.claim is None:
            journal.require_reconciliation(
                expected_generation=current.cycle_generation
            )
            return
        if effect_may_have_started and started_snapshot is None:
            journal.require_reconciliation(expected_generation=current.cycle_generation)
            return
        settlement: dict[str, Any] = {
            "schema_version": "manyforge.program_cycle_start_settlement.v2",
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
            if type(started_run_id) is not int:
                journal.require_reconciliation(
                    expected_generation=current.cycle_generation
                )
                return
            try:
                # The Run's Forced Program End (part 1 of its key).
                stopped = self._host.stop_run(
                    started_run_id,
                    reason="Program Start settlement failed",
                    stop_request_key=with_part(
                        request_key, Part.START_RACE_FORCED_END
                    ),
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
                    # A Reset's acquire replay returned the claim.
                    "recovery_required",
                ),
                state="terminal_awaiting_reset",
                terminal_reference=reference,
                start_settlement=settlement,
                program_run_id=program_run_id,
                run_snapshot=terminal_snapshot,
                failure=None if failure is None else {
                    "code": failure.code,
                    "message": f"Program run {program_run_id} started, then failed: {failure}. "
                               "Reset the Program cycle before another Run.",
                    "details": {**failure.details, "program_run_id": program_run_id,
                                "claim_generation": current.claim["claim_generation"],
                                "needs_reset": True},
                    "failed_phase": "starting",
                    "program_run_id": program_run_id,
                    "claim_generation": current.claim["claim_generation"],
                    "needs_reset": True,
                },
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
        if not isinstance(reference, str) or type(program_run_id) is not int:
            return False
        settlement = cycle.start_settlement
        if settlement is not None:
            prepared_scene = settlement.get("prepared_scene")
            if (
                settlement.get("schema_version")
                != "manyforge.program_cycle_start_settlement.v2"
                or settlement.get("claim") != cycle.claim
                or not isinstance(prepared_scene, Mapping)
                or (prepared_scene != {} and prepared_scene.get("schema_version")
                    != "manyforge.prepared_program_scene.v1")
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
        scene_context: tuple[str, int],
        *,
        program_reset_scene_authorization: Mapping[str, Any] | None = None,
    ) -> tuple[Any, Sequence[Mapping[str, Any]], dict[str, Any]]:
        context_kind, number = scene_context
        context = run_admission.port_context(number, context_kind=context_kind)
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
        context_id: tuple[str, int],
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
        source_document_digest = applied.get("source_document_digest")
        if not isinstance(scenario_id, str) or not isinstance(
            source_document_digest, str
        ):
            raise ProgramRunnerError(
                "program_run_applied_selection_stale",
                "Applied Program omitted its Scenario identity",
            )
        try:
            run_admission = build_applied_program_run_admission(
                applied,
                expected_scenario_id=scenario_id,
                expected_source_document_digest=source_document_digest,
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

    def _current_apply_number(self) -> int:
        """The Manager's Apply number of the current committed activation."""

        return self._headless.committed_scenario_activation()["apply_id"]

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

        if authorization is not None:
            # The Kernel validates the scene restore against this runner's
            # current cycle status, carried in the authorization (OD-139).
            authorization = {
                **authorization, "program_cycle_status": self._kernel_cycle_status()
            }
        try:
            # Scene context (1.16.5): the run this Reset resets, or the
            # current Apply when no run exists.
            self._prepare_current_applied_scene(
                expected_record_digest=None,
                context_id=(
                    ("run", request.program_run_id)
                    if request.program_run_id is not None
                    else ("apply", self._current_apply_number())
                ),
                program_reset_scene_authorization=authorization,
            )
        except ProgramRunnerError as error:
            raise ProgramRunnerError(
                "program_cycle_reset_scene_preparation_failed",
                str(error),
                details={"cause_code": error.code, "cause_producer": error.producer},
            ) from error
        except HeadlessClientError as error:
            raise ProgramRunnerError(
                "program_cycle_reset_scene_preparation_failed",
                str(error),
                details={"cause_code": error.code, "cause_producer": error.producer},
            ) from error
        except (ProgramRunOwnerError, ProgramSchedulingError) as error:
            cause = self._runner_error(error)
            if _scheduling_cause(error) is not None:
                raise cause from error
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
        with self._runner_command("scene_preparation", request.request_key):
            self._require_manager_current()
            admission_client, journal = self._require_cycle_owner()
            cycle = journal.read()
            if (
                cycle.state != "idle_after_reset"
                or cycle.reconciliation_required
                or cycle.acquisition_pending is not None
            ):
                raise ProgramRunnerError(
                    "authoring_not_ready",
                    "Applied Program scene preparation requires authoritative Idle",
                )
            admission = admission_client.get()
            generation = self._admission_generation(admission)
            admission_details = {
                "admission_generation": generation,
                "authoring_allowed": admission.get("authoring_allowed"),
                "expected_admission_generation": request.expected_admission_generation,
            }
            if admission.get("authoring_allowed") is not True:
                raise ProgramRunnerError(
                    "authoring_not_ready",
                    "authoring admission is not ready for scene preparation",
                    details=admission_details,
                )
            if request.clear_empty_scene:
                if request.expected_applied_record_digest is None:
                    def current_empty() -> Mapping[str, Any] | None:
                        manager = self._headless.manager_status()
                        source, status = manager.get("source"), manager.get("status")
                        if (not isinstance(source, Mapping) or source.get("available") is not True
                            or source.get("freshness") != "fresh" or not isinstance(status, Mapping)
                            or status.get("manager_ready") is not True
                            or not isinstance(status.get("runtime"), Mapping)
                            or status["runtime"].get("converged") is not True):
                            raise ProgramRunnerError("manager_unavailable", "Manager status is unavailable")
                        active = status.get("active")
                        if (not isinstance(active, Mapping) or active.get("canonical_empty") is not True
                            or not isinstance(active.get("publication"), Mapping)
                            or active["publication"].get("publication_generation") != request.expected_publication_generation):
                            return None
                        return active

                    try:
                        active = current_empty()
                    except Exception as error:
                        raise ProgramRunnerError("authoring_not_ready",
                                                 "Canonical empty Scene status is unavailable") from error
                    if active is None:
                        raise ProgramRunnerError("program_run_applied_selection_stale", "Applied Scene changed before clearing")
                    applied_status = active

                    def is_current() -> bool:
                        return current_empty() is not None
                else:
                    applied_status = self._headless.program_applied_status()
                    committed = self._headless.committed_scenario_activation()
                    if (
                        committed.get("scene_initialization") != "initialize"
                        or applied_status.get("program_id") is not None
                        or committed.get("record_digest") != request.expected_applied_record_digest
                        or applied_status.get("record_digest") != request.expected_applied_record_digest
                    ):
                        raise ProgramRunnerError("program_run_applied_selection_stale",
                                                 "Applied Scene changed before clearing")

                    def is_current() -> bool:
                        current = self._headless.committed_scenario_activation()
                        return (current.get("apply_id") == committed["apply_id"]
                                and current.get("apply_generation") == committed["apply_generation"]
                                and current.get("record_digest") == committed["record_digest"])

                # Accepted (OD-214) once the scene work starts; it records no
                # request, and its reply stays at completion (Headless).
                self._command_accepted("preparing_scene")
                try:
                    clear = self._scene.clear_applied_scene(
                        retained_bundle_id=applied_status["retained_bundle_id"],
                        graph_revision=applied_status["graph_revision"],
                        is_current=is_current,
                    )
                except Exception as error:
                    clear = {"state": "failed", "reason": "scene_owner_unavailable",
                             "message": str(error) or type(error).__name__,
                             "last_observed_remaining": None}
                cleared = {
                    "schema_version": "manyforge.program_scene_preparation.v1",
                    "status": "prepared", "applied_record_digest": request.expected_applied_record_digest,
                    "program_revision": None,
                    "admission_generation": self._admission_generation(admission_client.get()),
                    "prepared_scene": {"schema_version": "manyforge.empty_scene_clear.v1", **clear},
                }
                # Load (OD-234): idle cycle, this command holds the runner,
                # so no run is scheduled or ticking during the pause.
                settle_heap("load")
                return cleared
            try:
                # Scene context (1.16.5): the Apply number of the Applied
                # record being prepared.
                # Accepted (OD-214) once the scene work starts; it records no
                # request, and its reply stays at completion (Headless).
                self._command_accepted("preparing_scene")
                applied, proof = self._prepare_current_applied_scene(
                    expected_record_digest=request.expected_applied_record_digest,
                    context_id=("apply", self._current_apply_number()),
                )
                resulting_generation = self._admission_generation(
                    admission_client.get()
                )
                journal.complete_scene_preparation(
                    expected_generation=cycle.cycle_generation
                )
                prepared = {
                    "schema_version": "manyforge.program_scene_preparation.v1",
                    "status": "prepared",
                    "applied_record_digest": request.expected_applied_record_digest,
                    "program_revision": applied.get("program_revision"),
                    "admission_generation": resulting_generation,
                    "prepared_scene": deepcopy(proof),
                }
            except Exception as error:
                failure = {
                    "code": "program_scene_preparation_failed",
                    "message": str(error) or type(error).__name__,
                }
                journal.record_scene_preparation_failure(
                    expected_generation=cycle.cycle_generation,
                    failure=failure,
                )
                raise ProgramRunnerError(failure["code"], failure["message"]) from error
            # Load (OD-234): idle cycle, this command holds the runner, so no
            # run is scheduled or ticking during the pause.
            settle_heap("load")
            return prepared

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
        self._start_interrupt_state_follower()
        self._establish_startup_bbkey_source()

    def _establish_startup_bbkey_source(self) -> None:
        """An already Applied Program gets its between-Runs source at Runner
        start, not only at the next Apply or Run (idle cycle only)."""

        if (
            self._journal is None
            or self._persistent_keys is None
            or not callable(getattr(self._execution_transport, "publish_program_bbkeys", None))
        ):
            return  # No between-Runs source can exist here.

        def establish() -> None:
            try:
                if self._journal.read().state != "idle_after_reset":
                    return
                applied = self._headless.program_applied()
                document = applied.get("program")
                if not isinstance(document, Mapping):
                    return
                program = Program.from_dict(deepcopy(dict(document)), self._environment.catalog)
            except Exception:
                _LOGGER.warning(
                    "between-Run BB key source not established at start; "
                    "the next Apply or Run establishes it",
                    exc_info=True,
                )
                return
            self._learn_condition_keys(program)
            self._follow_cell_bbkey_source()

        try:
            self._host.submit_run_work(establish)
        except Exception:
            establish()

    def health(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.program_runner_health.v2",
            "owner_id": self.owner_id,
            "application_start_id": self.application_start_id,
            "started": self._started,
            "closed": self._closed,
            "assistant_model_transport_available": self._assistant is not None,
            "assistant_snapshot_source_available": self._snapshot_source is not None,
            "simulation_control_available": self._simulation_control is not None,
        }

    def node_availability(
        self, request: ProgramNodeAvailabilityRequest
    ) -> dict[str, Any]:
        """Project per-node execution facts; Start checks its full context separately."""

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
            scenario_revision = applied.get("source_document_digest")
            if not isinstance(scenario_id, str) or not isinstance(
                scenario_revision, str
            ):
                raise ProgramRunOwnerError(
                    "Applied Program omitted its Scenario identity"
                )
            owner_availability = self._current_node_owner_availability(
                applied,
                admission_generation=admission_generation,
            )
            admitted = build_applied_program_run_admission(
                applied,
                expected_scenario_id=scenario_id,
                expected_source_document_digest=scenario_revision,
                environment=self._environment,
                owner_availability=owner_availability,
            )
            required_node_ids: set[str] = set()
            simulator_node = False

            def collect(node: Any) -> None:
                nonlocal simulator_node
                required_node_ids.add(node.node_id)
                simulator, _targets = self._simulation_node_requirements(node.node_id, node.parameters)
                simulator_node |= simulator
                for child in node.children:
                    collect(child)

            collect(admitted.program.tree)
            selected_owners = dict(admitted.readiness.owner_by_port)
            selected_owners["simulator_control"] = AvailabilityFact(
                False, ("program_runtime_owner_unavailable",)
            )
            if simulator_node:
                _required, selected_owners["simulator_control"] = self._simulation_context_requirement(
                    program=applied["program"], program_binding=applied["program_binding"],
                    retained_bundle_id=admitted.retained_bundle_id,
                    graph_revision=admitted.graph_revision,
                )
            availability = project_node_execution_availability(
                build_executable_node_resolver(self._environment),
                replace(admitted.readiness, owner_by_port=selected_owners),
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
            "owner_id": self.owner_id,
            "program_binding_id": admitted.program_binding_id,
            "program_id": admitted.program.program_id,
            "program_revision": admitted.program.program_revision,
            "retained_bundle_id": admitted.retained_bundle_id,
            "scenario_id": admitted.scenario_id,
        }
        return {
            "schema_version": "manyforge.program_node_availability.v2",
            "source": {
                **source_value,
                "content_digest": self._digest(
                    {"availability": availability, **source_value}
                ),
            },
            "availability": availability,
        }

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
            "application_start_id": self.application_start_id,
            "active_only": active_only,
            "truncated": live_truncated or history_truncated,
            "runs": values,
        }

    def _reset_rule_activation(self) -> None:
        """Every global rule returns to its `active_at_start` at the end of
        every run (completed, failed, stopped) and at Reset."""

        try:
            self._kernel.reset_rule_activation()
        except Exception:
            _LOGGER.exception("global rule activation could not be reset")

    def _run_end_with_cell_fault(self, run_end: Mapping[str, Any]) -> dict[str, Any]:
        """A run that failed or stopped while the cell is latched ended by a
        cell fault: the latch is the error; the end names its component."""

        if run_end["end_reason"] == "completed":
            return dict(run_end)
        try:
            latch = self._headless.request("GET", "/api/cell/latch")
        except HeadlessClientError:
            _LOGGER.exception("cell latch could not be read for the run end")
            return dict(run_end)
        if latch.get("latched") is not True or not latch.get("causes"):
            return dict(run_end)
        return {
            "end_reason": "cell_fault",
            "details": {"component_id": latch["causes"][0]["component_id"]},
        }

    def _record_terminal_snapshot(self, snapshot: Any) -> None:
        value = (
            snapshot.to_dict()
            if callable(getattr(snapshot, "to_dict", None))
            else snapshot
        )
        if not isinstance(value, Mapping) or value.get("state") not in {
            "stopped",
            "succeeded",
            "failed",
        }:
            return
        capture = value.get("capture")
        if not isinstance(capture, Mapping):
            return
        program_run_id = capture.get("program_run_id")
        if type(program_run_id) is not int:
            return
        if program_run_id not in self._run_ends and value.get("run_end") is not None:
            # Once per run (only the latest run's end is kept): its end with
            # the cell latch, and every rule back to `active_at_start`.
            self._run_ends = {
                program_run_id: self._run_end_with_cell_fault(value["run_end"])
            }
            self._reset_rule_activation()
        value = self._project_run(value)
        if self._journal is None:
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

    def _new_bbkey_publisher(
        self, scope: ProgramBlackboardPublicationScope
    ) -> ProgramBlackboardKeyPublisher:
        publish = getattr(self._execution_transport, "publish_program_bbkeys", None)
        if not callable(publish):
            raise ProgramRunnerError(
                "program_bbkey_publication_unavailable",
                "the execution transport cannot publish BB keys",
            )
        liveness = self._liveness
        created: list[ProgramBlackboardKeyPublisher] = []
        ordinal = next(self._bbkey_source_ordinals)

        def on_fault(fault: Mapping[str, Any], reference: str) -> None:
            # The fence first, for every source: nothing gated by the failed
            # source's last value can be admitted while its fate is unknown.
            token = object()
            with self._bbkey_fence_lock:
                if liveness is not None and liveness.latch_runner_fault(
                    fault, reference
                ):
                    self._bbkey_fence_reference = reference
                self._bbkey_open_losses[token] = ordinal
            # A Run's source ends with its Run (the fault already stopped it).
            # The cell-level source between Runs guards no Run. Drop it
            # locally; the sender has already failed closed in the Kernel: it
            # declared its own continuity lost (an exact-sender sequence gap),
            # so every BB-backed global-rule input is unavailable and gated
            # motion is refused until the next Reset or Run installs a new source.
            # The fence is released only when every such loss is confirmed,
            # or superseded by a successor whose baseline the Kernel ACKed.
            with self._bbkey_publishers_lock:
                if created and self._bbkey_publishers.get(None) is created[0]:
                    self._bbkey_publishers.pop(None)
                    self._bbkey_idle_store = None

            def contain() -> None:
                if created and created[0].loss_confirmed:
                    self._settle_bbkey_losses(
                        lambda key, _ordinal: key is token,
                        "declared lost in the Kernel",
                    )
                    return
                with self._bbkey_publishers_lock:
                    current = self._bbkey_publishers.get(None)
                if current is not None and current.baseline_accepted:
                    self._settle_superseded_bbkey_losses(current.source_ordinal)
                # Otherwise the fence stays; a later ACKed successor settles it.

            try:
                self._host.submit_run_work(contain)
            except Exception:
                contain()

        def on_baseline_accepted() -> None:
            # Only the current between-Runs source proves a handoff: a late
            # ACK of an older, already replaced sender settles nothing.
            if scope.program_run_id is not None:
                return
            with self._bbkey_publishers_lock:
                current = bool(created) and self._bbkey_publishers.get(None) is created[0]
            if current:
                self._settle_superseded_bbkey_losses(ordinal)

        publisher = ProgramBlackboardKeyPublisher(
            scope,
            publish=publish,
            submit=self._host.submit_run_work,
            stop_run=self._stop_for_bbkey_publication_fault,
            on_fault=on_fault,
            fault_reference=self._digest,
            on_baseline_accepted=on_baseline_accepted,
            source_ordinal=ordinal,
        )
        created.append(publisher)
        return publisher

    def _settle_superseded_bbkey_losses(self, successor_ordinal: int) -> None:
        """The Kernel ACKed the current between-Runs source's baseline: it
        replaced every source created before it (lower ordinal)."""

        self._settle_bbkey_losses(
            lambda _key, lost: lost < successor_ordinal,
            "superseded by an ACKed newer source",
        )

    def _settle_bbkey_losses(
        self, settles: Callable[[object, int], bool], outcome: str
    ) -> None:
        """Settle the matching unconfirmed source losses; release the fence
        only when none is left."""

        with self._bbkey_fence_lock:
            settled = [key for key, lost in self._bbkey_open_losses.items() if settles(key, lost)]
            if not settled:
                return
            for key in settled:
                del self._bbkey_open_losses[key]
            reference = self._bbkey_fence_reference
            if self._bbkey_open_losses or reference is None:
                released = False
            else:
                self._bbkey_fence_reference = None
                released = (
                    self._liveness is not None
                    and self._liveness.clear_runner_fault(reference)
                )
        _LOGGER.warning(
            "BB key source %s (%d); fence %s; BB-gated motion waits "
            "for the next Reset or Run",
            outcome,
            len(settled),
            "released" if released else "kept",
        )

    def _bbkey_budget_ns(self) -> int:
        return int(min(self.config.headless_timeout_seconds, 30.0) * 1e9)

    def _start_or_defer_bbkey_source(
        self,
        name: str | None,
        publisher: ProgramBlackboardKeyPublisher,
        store: ProgramValueStore,
        program_keys: tuple[str, ...],
        context: tuple[int, str, str],
        *,
        deadline_ns: int | None = None,
    ) -> None:
        """Send the baseline of the fixed union W = Program keys ∪ the global
        rules' keys only once the catalog of this source's full context
        (publication, bundle, graph) is known and
        the Kernel has installed the source (no ``bb_source_not_ready``).
        Until then the source is pending: its writes only commit, and the next
        liveness observation starts it again with a baseline built from the
        store then. A pending source keeps its original
        ``deadline_ns``."""

        def pend() -> None:
            with self._bbkey_publishers_lock:
                self._bbkey_pending[name] = (
                    publisher, store, program_keys, context,
                    time.monotonic_ns() + self._bbkey_budget_ns()
                    if deadline_ns is None else deadline_ns,
                )

        catalog = self._catalog_for_context(context)
        if catalog is None:
            pend()
            return
        keys = tuple(sorted({*program_keys, *catalog.referenced_program_blackboard_keys}))
        if not keys:
            # The qualified catalog proves no global rule key: nothing to send.
            with self._bbkey_publishers_lock:
                if self._bbkey_publishers.get(name) is publisher:
                    self._bbkey_publishers.pop(name)
            publisher.retire()
            return
        scope = publisher.scope
        # Diagnostic: one bounded line per baseline sender, so two
        # senders of one scope can be told apart (WARNING: the Runner
        # configures no log level, so INFO would not reach the container log).
        _LOGGER.warning(
            "BB baseline sender %x: name=%s thread=%s run=%s claim=%s "
            "cycle=%s publication=%s bundle=%.19s graph=%.19s binding=%.19s "
            "sequence=1 keys=%d",
            id(publisher), name, current_thread().name,
            scope.program_run_id, scope.claim_generation, scope.cycle_generation,
            scope.active_publication_generation, context[1], context[2],
            scope.program_binding_id, len(keys),
        )
        if not publisher.start(store, keys):
            pend()

    def _retry_pending_bbkey_sources(self) -> None:
        with self._bbkey_publishers_lock:
            pending = list(self._bbkey_pending.items())
            self._bbkey_pending.clear()
        for name, (publisher, store, program_keys, context, deadline_ns) in pending:
            with self._bbkey_publishers_lock:
                current = self._bbkey_publishers.get(name) is publisher
            if not current:
                continue  # Retired by a Reset, a new Run or a new publication.
            if time.monotonic_ns() >= deadline_ns:
                publisher.fail(
                    f"the BB key source of publication {context[0]} was not installed "
                    "within the command budget (rule catalog unknown or source not ready)"
                )
                continue
            self._start_or_defer_bbkey_source(
                name, publisher, store, program_keys, context, deadline_ns=deadline_ns
            )

    def _retire_bbkey_publishers(self, *, idle_only: bool = False) -> None:
        with self._bbkey_publishers_lock:
            names = [None] if idle_only else list(self._bbkey_publishers)
            retired = [self._bbkey_publishers.pop(name, None) for name in names]
            if not idle_only:
                self._bbkey_idle_store = None
        if not idle_only:
            with self._active_tcp_stores_lock:
                self._active_tcp_stores.clear()
        for publisher in retired:
            if publisher is not None:
                publisher.retire()

    def _start_bbkey_publication(
        self,
        program_run_id: int,
        *,
        claim_generation: int,
        program_binding_id: str,
        active_publication_generation: int,
        cycle_generation: int,
        keys: tuple[str, ...],
        retained_bundle_id: str,
        graph_revision: str,
        binding: Mapping[str, Any] | None = None,
    ) -> ProgramBlackboardKeyPublisher | None:
        """Subscribe the Run's fixed key union and send its baseline first."""

        with self._bbkey_source_lock:
            return self._start_run_bbkey_publication(
                program_run_id,
                claim_generation=claim_generation,
                program_binding_id=program_binding_id,
                active_publication_generation=active_publication_generation,
                cycle_generation=cycle_generation,
                keys=keys,
                retained_bundle_id=retained_bundle_id,
                graph_revision=graph_revision,
                binding=binding,
            )

    def _start_run_bbkey_publication(
        self,
        program_run_id: int,
        *,
        claim_generation: int,
        program_binding_id: str,
        active_publication_generation: int,
        cycle_generation: int,
        keys: tuple[str, ...],
        retained_bundle_id: str,
        graph_revision: str,
        binding: Mapping[str, Any] | None = None,
    ) -> ProgramBlackboardKeyPublisher | None:
        # Previous sources (a reset Run's, the cell-level one) end here.
        self._retire_bbkey_publishers()
        self._interrupt_state_run_id = program_run_id
        context = (active_publication_generation, retained_bundle_id, graph_revision)
        store = self._owner.value_store(program_run_id)
        if binding is not None:
            self._register_active_tcp_store(program_run_id, store, binding)
        if self._applied_global_rule_ids():
            self._seed_interrupt_states(store, context)
        catalog = self._catalog_for_context(context)
        if not keys and catalog is not None and not catalog.referenced_program_blackboard_keys:
            return None  # No keys: no sender, no baseline.
        publisher = self._new_bbkey_publisher(
            ProgramBlackboardPublicationScope(
                claim_generation=claim_generation,
                program_run_id=program_run_id,
                program_binding_id=program_binding_id,
                active_publication_generation=active_publication_generation,
                cycle_generation=cycle_generation,
            )
        )
        with self._bbkey_publishers_lock:
            self._bbkey_publishers[program_run_id] = publisher
        self._start_or_defer_bbkey_source(
            program_run_id, publisher, self._owner.value_store(program_run_id), keys, context
        )
        return publisher

    def _start_idle_bbkey_publication(self, *, keys: tuple[str, ...]) -> None:
        """The cell-level source between Runs (after Reset or a cold Apply).

        Identity: Applied binding and publication + Program-cycle
        generation. Baseline: persistent keys' stored values,
        run-only keys their declared `initial` (else null), node_outcome.*
        `none`. Every new
        Applied publication is a new source; unchanged identity (a repeated
        Apply notice) keeps the source.
        """

        if self._journal is None or self._persistent_keys is None:
            self._retire_bbkey_publishers(idle_only=True)
            return
        applied = self._headless.program_applied()
        publication_generation = self._applied_publication_generation(applied)
        if publication_generation is None:
            # The host does not publish this Applied occurrence yet (its
            # Apply is still finishing): its notice establishes the source.
            _LOGGER.info("between-Run BB key source waits for the Applied publication")
            return
        context = (
            publication_generation,
            str(applied.get("retained_bundle_id")),
            str(applied.get("graph_revision")),
        )
        cycle_generation = self._journal.read().cycle_generation
        with self._bbkey_publishers_lock:
            current = self._bbkey_publishers.get(None)
        # An unchanged identity keeps its source before anything else.
        # The Kernel keeps the baseline of this exact scope; a local retire
        # followed by a new sender of the same scope (e.g. the no-keys case below on a
        # transient catalog read) would send that baseline again, and the
        # Kernel refuses the repeat (`program_bb_sequence_stale`).
        if (
            current is not None
            and current.scope.program_binding_id == applied["program_binding_id"]
            and current.scope.active_publication_generation == publication_generation
            and current.scope.cycle_generation == cycle_generation
        ):
            return
        catalog = self._catalog_for_context(context)
        no_publisher = not keys and catalog is not None and not catalog.referenced_program_blackboard_keys
        self._retire_bbkey_publishers(idle_only=True)
        program = self._with_global_rule_keys(
            Program.from_dict(deepcopy(applied["program"]), self._environment.catalog),
            retained_bundle_id=applied.get("retained_bundle_id"),
            graph_revision=applied.get("graph_revision"),
        ).with_active_tcp_binding(applied["program_binding"]).with_program_frame_facts(
            ProgramFrameFacts.from_capture(applied["program_frame_facts"], {
                **applied, "applied_record_digest": applied["source"]["record_digest"],
            })
        )
        persistent = {
            key: declaration
            for key, declaration in program.blackboard_keys.items()
            if declaration.persistence == "persistent"
        }
        # Values already live in the register: this store only mirrors them.
        store = ProgramValueStore(
            program,
            restored=self._persistent_keys.restore(applied["project_path"], persistent),
            persistent_commit=lambda _declaration, _value, _metadata: None,
            persistent_forget=lambda _declaration, _metadata: None,
        )
        self._register_active_tcp_store(None, store, applied["program_binding"])
        if no_publisher:
            return  # No keys: Active TCP is mirrored, but no BB source is sent.
        # A fresh store: run-scoped keys hold their declared `initial`, or null
        # when omitted (the value the next Run starts from),
        # node_outcome.* `none`.
        publisher = self._new_bbkey_publisher(
            ProgramBlackboardPublicationScope(
                claim_generation=None,
                program_run_id=None,
                program_binding_id=applied["program_binding_id"],
                active_publication_generation=publication_generation,
                cycle_generation=cycle_generation,
            )
        )
        self._seed_interrupt_states(store, context)
        with self._bbkey_publishers_lock:
            self._bbkey_publishers[None] = publisher
            self._bbkey_idle_store = store
        self._start_or_defer_bbkey_source(None, publisher, store, keys, context)

    def _follow_idle_persistent_edit(self, key: str, value: Any, *, reset: bool) -> None:
        """Mirror an operator edit between Runs into the cell-level source."""

        with self._bbkey_publishers_lock:
            store = self._bbkey_idle_store
        if store is None:
            return
        try:
            if reset:
                store.reset_to_initial(key)
            else:
                store.write(key, value)
        except ProgramValueError as error:
            with self._bbkey_publishers_lock:
                publisher = self._bbkey_publishers.get(None)
            if publisher is not None:
                publisher.fail(f"between-Run edit of {key!r} cannot be published: {error}")

    def _applied_publication_generation(self, applied: Mapping[str, Any]) -> int | None:
        """The host's existing ``active_publication_generation`` for exactly
        this Applied occurrence, or None while the host status still names
        another one."""

        execution = self._kernel.identity()
        status = execution.get("identity")
        if not isinstance(status, Mapping):
            raise ProgramRunnerError(
                "execution_command_response_invalid",
                "host status omitted its identity",
            )
        generation = status.get("active_publication_generation")
        if type(generation) is not int or not 1 <= generation <= 9_007_199_254_740_991:
            raise ProgramRunnerError(
                "execution_command_response_invalid",
                "host status omitted its Applied publication generation",
            )
        if (
            status.get("retained_bundle_id") != applied.get("retained_bundle_id")
            or status.get("graph_revision") != applied.get("graph_revision")
        ):
            return None
        return generation

    def _reset_bbkey_publication(self, program_run_id: int) -> None:
        """Reset retires the Run's source locally: no clearing
        frame; the between-Runs baseline that follows carries run-only keys
        at their declared `initial`, or null when omitted. Keys keep their last value while the
        Run is held or finished, so the sender lives until here."""

        with self._bbkey_publishers_lock:
            publisher = self._bbkey_publishers.pop(program_run_id, None)
        with self._active_tcp_stores_lock:
            self._active_tcp_stores.pop(program_run_id, None)
        if self._interrupt_state_run_id == program_run_id:
            self._interrupt_state_run_id = None
        with self._grasp_lost_mirror_lock:
            if self._carried_object_lost_run_id == program_run_id:
                self._carried_object_lost_run_id = None
                self._carried_object_lost_run_active = False
                self._carried_object_lost_seen_event_ids = set()
        if publisher is not None:
            publisher.retire()

    def _learn_condition_keys(self, program: Program) -> tuple[str, ...]:
        """Learn the Applied Program's condition keys. The source adds the
        global rules' keys of its own publication."""

        self._applied_condition_keys = program.interrupt_condition_keys
        return self._applied_condition_keys

    def _cell_bbkey_union(self) -> tuple[str, ...]:
        """The Program part of the cell-level union:
        the keys the Applied Program's interrupt conditions name, as last
        learned from a cold Apply or a Run start. The global rules'
        ``referenced_program_blackboard_keys`` join when the source starts,
        from the catalog of exactly its publication."""

        return self._applied_condition_keys

    def _follow_cell_bbkey_source(self) -> None:
        """Start the between-Runs source (Reset already advanced the cycle).

        A start failure is logged, never a logged success, and changes no
        source: an unchanged identity keeps its source, which the Kernel
        still holds, and a new Applied publication retires the old source in
        the Kernel itself. The next Reset or Run start establishes the source.
        Startup, Apply notices and Reset reach here on different workers: one
        source creation at a time, so one scope never gets two senders.
        """

        with self._bbkey_source_lock:
            try:
                self._start_idle_bbkey_publication(keys=self._cell_bbkey_union())
            except Exception:
                _LOGGER.exception("cell-level BB key source could not start")

    # Active TCP values belong to robot handles, independent of BB publishing.
    @staticmethod
    def _active_tcp_targets(binding: Mapping[str, Any]) -> dict[str, tuple[str, ...]]:
        targets: dict[str, list[str]] = {}
        for row in binding.get("target_bindings", ()):
            if not isinstance(row, Mapping) or "robot_tcps" not in row:
                continue
            target = row.get("target")
            if not isinstance(target, Mapping):
                continue
            robot, command = target.get("component_id"), target.get("command_target_id")
            if isinstance(robot, str) and robot and isinstance(command, str) and command:
                targets.setdefault(robot, []).append(command)
        return {robot: tuple(sorted(set(names))) for robot, names in targets.items()}

    @staticmethod
    def _write_store_active_tcp(
        store: ProgramValueStore, roles: Mapping[str, tuple[str, ...]],
        robot: str, entry: Mapping[str, Any] | None,
    ) -> None:
        for command_target in roles.get(robot, ()):
            store.write_active_tcp(robot, command_target, entry)

    def _write_active_tcp(self, robot: str, entry: Mapping[str, Any] | None) -> None:
        with self._active_tcp_stores_lock:
            stores = tuple(self._active_tcp_stores.values())
        for store, roles in stores:
            self._write_store_active_tcp(store, roles, robot, entry)

    def _has_active_tcp_follower(self) -> bool:
        follower = self._interrupt_state_follower
        return (
            follower is not None and follower.is_alive()
            and callable(getattr(self._execution_transport, "subscribe_events", None))
        )

    def _apply_active_tcp_reading(self, response: Mapping[str, Any]) -> set[str]:
        readings = decode_active_tcp_rows(response.get("robots"))
        applied_robots: set[str] = set()
        for robot, generation, sequence, entry in readings:
            applied = self._active_tcp_mirror.apply_result_row({
                "component_id": robot,
                "registration_generation": generation, "sequence": sequence,
                "entry": entry,
            })
            if not applied:
                raise ActiveTcpWireError("Active TCP read could not be delivered")
            applied_robots.add(robot)
        return applied_robots

    def _read_active_tcp_ordered(self) -> None:
        # The host orders accepted commands; a connection delayed before its
        # enqueue remains the explicitly approved residual.
        self._apply_active_tcp_reading(self._kernel.read_active_tcp(ordered=True))

    def _refresh_active_tcp(self) -> None:
        try:
            self._read_active_tcp_ordered()
        except Exception:
            if not self._has_active_tcp_follower():
                self._active_tcp_mirror.stream_lost()
            _LOGGER.warning("Active TCP refresh unavailable", exc_info=True)

    def _reset_active_tcp(self) -> None:
        response = self._kernel.reset_active_tcp()
        with self._active_tcp_stores_lock:
            required = {robot for _store, roles in self._active_tcp_stores.values() for robot in roles}
        self._active_tcp_mirror.apply_reset_rows(response.get("robots"), required)

    def _register_active_tcp_store(
        self, name: str | None, store: ProgramValueStore, binding: Mapping[str, Any],
    ) -> None:
        roles = self._active_tcp_targets(binding)
        def register() -> None:
            with self._active_tcp_stores_lock:
                self._active_tcp_stores[name] = (store, roles)

        try:
            ordered = name is not None or not self._has_active_tcp_follower()
            response = self._kernel.read_active_tcp(ordered=ordered)
            read_robots = self._apply_active_tcp_reading(response)
        except Exception:
            # One read only: unavailable keys fail their ordinary Current
            # input until a later confirmed stream or result supplies them.
            _LOGGER.warning("Active TCP baseline read unavailable", exc_info=True)
            self._active_tcp_mirror.seed(lambda _robot, _entry: None, register=register)
            return
        self._active_tcp_mirror.seed(
            lambda robot, entry: (
                self._write_store_active_tcp(store, roles, robot, entry)
                if robot in read_robots else None
            ),
            register=register,
        )

    def _apply_active_tcp_result(self, run_id: int, robot: str, rows: object) -> bool:
        """Apply the result's robot reading before the owner task settles."""

        readings = decode_active_tcp_rows(rows)
        matches = [reading for reading in readings if reading[0] == robot]
        with self._active_tcp_stores_lock:
            registered = self._active_tcp_stores.get(run_id)
        if len(matches) != 1 or registered is None or robot not in registered[1]:
            raise ActiveTcpWireError("Active TCP result has no exact bound robot")
        component, generation, sequence, entry = matches[0]
        store, roles = registered
        applied = self._active_tcp_mirror.apply_result_row(
            {
                "component_id": component,
                "registration_generation": generation, "sequence": sequence,
                "entry": entry,
            },
            write_current=lambda robot_id, current: self._write_store_active_tcp(
                store, roles, robot_id, current
            ),
        )
        if not applied:
            raise ActiveTcpWireError("Active TCP result is not current")
        return True

    # Global-rule state mirror ----------------------------------------------

    def _global_rule_catalog(self) -> RuleCatalog | None:
        """The Kernel's installed global rule catalog (``events`` reply
        ``interrupt_rule_catalog``), independent of the state baseline."""

        mirror = self._interrupt_state_mirror
        if mirror is None:
            return None
        try:
            return mirror.catalog()
        except Exception:
            _LOGGER.exception("interrupt rule catalog could not be read")
            return None

    def _catalog_for_context(self, context: tuple[int, str, str]) -> RuleCatalog | None:
        """The catalog of exactly this full context (publication
        generation, bundle, graph), or None while it is not yet known: null or
        another context is never an empty policy; a
        ``verified_empty`` catalog is the proof of "no global rules". Without a
        rule-state channel there is none to wait for."""

        mirror = self._interrupt_state_mirror
        if mirror is None:
            return _NO_GLOBAL_RULES
        catalog = self._global_rule_catalog()
        if catalog is None or catalog.context != context:
            try:
                mirror.poll()  # One fresh events read before waiting.
            except Exception:
                _LOGGER.exception("interrupt rule catalog could not be read")
            catalog = self._global_rule_catalog()
        if catalog is None or catalog.context != context:
            return None
        return catalog

    def _applied_global_rule_ids(
        self, *, retained_bundle_id: str | None = None, graph_revision: str | None = None
    ) -> tuple[str, ...]:
        """The Applied Deployment's global rule ids: the only declared
        ``interrupt_state.<rule_id>`` keys. Given the Program's bundle and
        graph, a catalog of another publication declares none."""

        catalog = self._global_rule_catalog()
        if catalog is None or (
            retained_bundle_id is not None and catalog.bundle != retained_bundle_id
        ) or (graph_revision is not None and catalog.graph != graph_revision):
            return ()
        return catalog.rule_ids

    def _with_global_rule_keys(
        self, program: Program, *, retained_bundle_id: Any, graph_revision: Any
    ) -> Program:
        """Declare the reserved ``interrupt_state.<rule_id>`` keys."""

        rule_ids = self._applied_global_rule_ids(
            retained_bundle_id=str(retained_bundle_id), graph_revision=str(graph_revision)
        )
        return program.with_interrupt_state_keys(rule_ids) if rule_ids else program

    def _start_interrupt_state_follower(self) -> None:
        """Follow the event subscription on its own thread (where the liveness
        watchdog used to schedule a 250 ms poll), so changes arrive as the host
        streams them; ``shutdown`` stops it within one receive slice."""

        mirror = self._interrupt_state_mirror
        if (
            mirror is None or self._liveness is None or self._interrupt_state_follower is not None
            or not callable(getattr(self._execution_transport, "subscribe_events", None))
        ):
            return
        follower = Thread(
            target=mirror.follow,
            args=(self._interrupt_state_stop,),
            kwargs={
                "on_read": self._on_interrupt_state_read,
                "on_message": self._on_active_tcp_message,
                "on_disconnect": self._on_active_tcp_stream_lost,
            },
            name="interrupt-state-mirror",
            daemon=True,
        )
        self._interrupt_state_follower = follower
        follower.start()

    def _apply_program_run_hold(self, facts: ProgramRunHoldFacts) -> None:
        """Kernel-first, level-triggered: the Kernel's hold facts for the
        claimed run, as the event stream's ``state`` carries them, go to the
        scheduling host on this follower thread. Memory only: the host keeps
        the latest facts and wakes the run; the run's tick reconciles.
        A failure here is a runner defect: one log line names the run."""

        try:
            self._host.apply_hold_facts(facts)
        except Exception as error:
            _LOGGER.error(
                "Program run %s did not take the Kernel hold facts at hold "
                "revision %s: %s",
                facts.program_run_id,
                facts.hold_revision,
                error,
            )

    def _on_active_tcp_stream_lost(self, _error: BaseException) -> None:
        self._active_tcp_mirror.stream_lost()
        # OD-213: the pushed operations are unavailable until the next snapshot.
        self._operation_table.stream_lost()

    def _on_active_tcp_message(self, message: Mapping[str, Any]) -> None:
        kind = message.get("type")
        if kind not in {"snapshot", "state", "event"}:
            return
        if kind in {"snapshot", "state"}:
            state = message.get("state")
            if not isinstance(state, Mapping):
                raise ActiveTcpWireError("Active TCP stream state is invalid")
            # OD-213: the nodes' operations, replaced whole (raises when malformed).
            self._operation_table.apply_state(state)
            self._active_tcp_mirror.apply_rows(state.get("active_tcp_baseline"))
        else:
            event = message.get("event")
            if not isinstance(event, Mapping):
                raise ActiveTcpWireError("Active TCP stream event is invalid")
            self._active_tcp_mirror.apply_event(event)

    def _on_interrupt_state_read(self) -> None:
        # Every read is a reading, also when no state changed: a motion node
        # waiting for a gate rule re-evaluates on it.
        mirror = self._interrupt_state_mirror
        if mirror is not None:
            self._apply_interrupt_states(mirror.current())

    def _interrupt_state_stores(self) -> list[ProgramValueStore]:
        stores: list[ProgramValueStore] = []
        with self._bbkey_publishers_lock:
            if self._bbkey_idle_store is not None:
                stores.append(self._bbkey_idle_store)
        run_id = self._interrupt_state_run_id
        if run_id is not None:
            try:
                stores.append(self._owner.value_store(run_id))
            except ProgramRunOwnerError:
                pass
        return stores

    @staticmethod
    def _write_interrupt_states(
        store: ProgramValueStore, states: Mapping[str, str] | None
    ) -> None:
        """Write every ``interrupt_state.<rule_id>`` key the store already
        declares (its qualified Applied global rules). Unknown states (None,
        e.g. an unknown catalog) make each of them unavailable, never clear;
        a rule absent from known states is unavailable too."""

        prefix = f"{INTERRUPT_STATE_NAMESPACE}."
        for key in store.program.blackboard_keys:
            if is_interrupt_state_key(key):
                rule_id = key[len(prefix):]
                store.write_interrupt_state(
                    rule_id, None if states is None else states.get(rule_id)
                )

    def _apply_interrupt_states(self, states: Mapping[str, str] | None) -> None:
        """Mirror the current states (None = unavailable) into every current
        store: the Run's (also while held or finished) and the between-Runs
        one."""

        for store in self._interrupt_state_stores():
            try:
                self._write_interrupt_states(store, states)
            except Exception:
                _LOGGER.exception("interrupt rule state could not be mirrored")

    @staticmethod
    def _carried_object_loss_event_ids(rows: object) -> set[str] | None:
        """Return IDs from a known, validated last-loss snapshot.

        ``None`` is kept distinct from an empty known snapshot: an unknown
        mirror must not erase the Run's previous event baseline.
        """

        if not isinstance(rows, Sequence) or isinstance(rows, (str, bytes)):
            return None
        event_ids: set[str] = set()
        for entry in rows:
            if not isinstance(entry, Mapping):
                return None
            event_id = entry.get("loss_event_id")
            if (
                type(event_id) is not str
                or not event_id
                or type(entry.get("set_key")) is not str
                or not entry["set_key"]
            ):
                return None
            event_ids.add(event_id)
        return event_ids

    def _begin_carried_object_lost_run(self, program_run_id: int) -> None:
        """Capture an inactive Run's baseline from the known last losses.

        The binding remains inactive until the cycle's admission request is
        fully completed, so a later failed start cannot have written a key.
        """

        with self._grasp_lost_mirror_lock:
            self._carried_object_lost_run_id = program_run_id
            self._carried_object_lost_run_active = False
            self._carried_object_lost_seen_event_ids = set()
            mirror = self._interrupt_state_mirror
            if mirror is None:
                return
            try:
                rows = mirror.carried_object_last_losses()
            except Exception:
                _LOGGER.exception("current carried-object last losses could not be read")
                return
            event_ids = self._carried_object_loss_event_ids(rows)
            if event_ids is not None:
                # All keyed losses already present at Run start are old for
                # this Run.
                self._carried_object_lost_seen_event_ids = event_ids

    def _activate_carried_object_lost_run(self, program_run_id: int) -> None:
        """Activate the admitted Run and re-evaluate the retained loss set."""

        with self._grasp_lost_mirror_lock:
            if self._carried_object_lost_run_id != program_run_id:
                return
            self._carried_object_lost_run_active = True
            mirror = self._interrupt_state_mirror
        if mirror is None:
            return
        try:
            rows = mirror.carried_object_lost_signals()
        except Exception:
            _LOGGER.exception("current carried-object loss rows could not be read")
            return
        if rows is not None:
            try:
                self._mirror_carried_object_lost_signals(rows)
            except Exception:
                _LOGGER.exception("carried-object last losses could not be re-evaluated")

    def _retire_carried_object_lost_run(self, program_run_id: int | None) -> None:
        """Retire only the exact Run whose admission did not complete."""

        if program_run_id is None:
            return
        with self._grasp_lost_mirror_lock:
            if self._carried_object_lost_run_id != program_run_id:
                return
            self._carried_object_lost_run_id = None
            self._carried_object_lost_run_active = False
            self._carried_object_lost_seen_event_ids = set()

    def _mirror_carried_object_lost_signals(self, rows: object) -> None:
        """Mirror each newly observed last loss into the current Run's key.

        ``rows`` is the existing mirror callback's notification that a known
        loss state arrived. The durable per-key loss set is read from the
        mirror because the current rows can clear before this callback runs.
        Unknown state makes no change to the baseline.
        """

        if rows is None:
            return
        mirror = self._interrupt_state_mirror
        if mirror is None:
            return
        try:
            last_losses = mirror.carried_object_last_losses()
        except Exception:
            _LOGGER.exception("carried-object last losses could not be read")
            return
        if last_losses is None:
            return
        event_ids = self._carried_object_loss_event_ids(last_losses)
        if event_ids is None:
            _LOGGER.warning("carried-object last losses are not usable")
            return
        with self._grasp_lost_mirror_lock:
            program_run_id = self._carried_object_lost_run_id
            if program_run_id is None or not self._carried_object_lost_run_active:
                return
            try:
                store = self._owner.value_store(program_run_id)
            except Exception:
                return
            seen_event_ids = self._carried_object_lost_seen_event_ids
            processed_event_ids: set[str] = set()
            for row in last_losses:
                event_id = row["loss_event_id"]
                if event_id in seen_event_ids or event_id in processed_event_ids:
                    continue
                processed_event_ids.add(event_id)
                key = row.get("set_key")
                if key is None:
                    continue
                if type(key) is not str or not key:
                    _LOGGER.warning(
                        "grasp-lost key in loss event %s is not a valid key name",
                        event_id,
                    )
                    continue
                declaration = store.program.blackboard_keys.get(key)
                if (
                    declaration is None
                    or declaration.type != "bool"
                    or declaration.access not in {"write", "read_write"}
                    or declaration.expression is not None
                ):
                    _LOGGER.warning(
                        "grasp-lost key %s in loss event %s is not a writable bool key",
                        key,
                        event_id,
                    )
                    continue
                try:
                    store.write(
                        key,
                        True,
                        metadata={"writer": "grasp_lost", "loss_event_id": event_id},
                    )
                except Exception:
                    _LOGGER.exception("grasp-lost key %s could not be written", key)
                    # Keep the event in the seen baseline below. The mirror
                    # continues with other keys from this known snapshot.
            # The baseline is all rows from the last known snapshot, including
            # new events without a set_key and events whose key was unusable.
            self._carried_object_lost_seen_event_ids = event_ids

    def _seed_interrupt_states(
        self, store: ProgramValueStore, context: tuple[int, str, str]
    ) -> None:
        """A new store sees the current states of exactly its full context
        (publication, bundle, graph) from its first tick; states
        of another context are unavailable for it."""

        mirror = self._interrupt_state_mirror
        self._write_interrupt_states(
            store, None if mirror is None else mirror.current(context)
        )

    def _stop_for_bbkey_publication_fault(self, program_run_id: int) -> None:
        """The Run's own Forced Program End, tried before the health failure.

        No request causes it: a runner key kept per run, so a resend reuses it.
        """

        key = self._forced_end_keys.setdefault(
            program_run_id, RUNNER_KEYS.next_key()
        )
        snapshot = self._host.stop_run(
            program_run_id,
            reason=BBKEY_PUBLICATION_FAULT_CODE,
            stop_request_key=key,
        )
        self._record_terminal_snapshot(snapshot.to_dict())

    def _require_end_effector_tcps(self) -> None:
        """B-run-1: refuse Execute while a robot has no End-effector TCP.

        The execution owner states the fact per robot motion target: its
        Robot Control ``tcp_frame`` is null exactly when the robot has
        no End-effector TCP (the tool on its flange has no default TCP). The
        rule itself is not evaluated here.
        """

        inventory = self._kernel.targets().get("targets")
        components = inventory.get("components") if isinstance(inventory, Mapping) else None
        if not isinstance(components, list):
            raise ProgramRunnerError(
                "execution_command_response_invalid",
                "execution targets omitted their component inventory",
            )
        for component in components:
            for target in component.get("targets", ()):
                if (
                    target.get("presentation") == "motion"
                    and target.get("command", {}).get("tcp_frame") is None
                ):
                    robot = component.get("controllable_component_id")
                    raise ProgramRunnerError(
                        "program_end_effector_tcp_unavailable",
                        f"{robot} has no end-effector TCP: the tool on its flange has no TCP",
                        details={"controllable_component_id": robot},
                    )

    def _require_run_readiness(self, can: str) -> None:
        """Refuse Execute (``can_execute``), a stopped run's Resume (``can_resume``)
        or a held motion group's Run/Resume (``motion_clear``) with
        the first reason of the one ``RunReadiness`` owner
        (``POST /api/program/readiness``), given this runner's current cycle
        facts; the Runner computes none itself."""

        request = {
            "schema_version": RUN_READINESS_REQUEST_SCHEMA,
            "program_cycle": run_cycle_facts(self.cycle_status()),
        }
        try:
            readiness = self._headless.request(
                "POST", "/api/program/readiness", body=request
            )
        except HeadlessClientError as error:
            raise self._headless_error(error) from error
        if readiness.get(can) is True:
            return
        reasons = readiness["blocked_reasons"]
        # A rule hold never clears motion_clear: the kernel refuses its scope.
        reason = next(
            (row for row in reasons if row["code"] != "rule_holds"), reasons[0]
        ) if can == "motion_clear" else reasons[0]
        raise ProgramRunnerError(
            reason["code"],
            reason["message"],
            details={key: value for key, value in reason.items() if key not in {"code", "message"}},
        )

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
        if not isinstance(request, ProgramRunStartRequest):
            raise TypeError("start_run requires one ProgramRunStartRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is not started",
            )
        request_key = _request_key(request)
        self._require_manager_current()
        with self._runner_command("start", request.request_key):
            self._require_manager_current()
            admission_client, journal = self._require_cycle_owner()
            cycle = journal.read()
            if (
                cycle.reconciliation_required
                or cycle.acquisition_pending is not None
            ):
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required",
                    "Program-cycle ownership requires authoritative reconciliation",
                )
            try:
                record = journal.record_request(
                    request_key=request_key,
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
            scene_context_id: tuple[str, int] | None = None
            scene_proof: dict[str, Any] | None = None
            started_snapshot: dict[str, Any] | None = None
            host_start_invoked = False
            carried_object_run_id: int | None = None
            admission_complete = False

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

            try:
                if cycle.state != "idle_after_reset":
                    raise ProgramRunnerError(
                        "program_cycle_reset_required",
                        "Program cycle is not Idle after an explicit Reset",
                    )
                if cycle.failure and cycle.failure.get("code") == "program_scene_preparation_failed":
                    raise ProgramRunnerError(
                        "program_scene_preparation_failed",
                        cycle.failure["message"],
                    )
                try:
                    journal.require_run_history_capacity()
                except OverflowError as error:
                    raise ProgramRunnerError(
                        "program_run_history_full", str(error)
                    ) from error
                # Accepted (OD-214): what follows asks other processes.
                self._command_accepted("admitting")
                owner_admission = admission_client.get()
                generation = self._admission_generation(owner_admission)
                applied = self._headless.program_applied()
                source = applied.get("source")
                if (
                    not isinstance(source, Mapping)
                    or source.get("record_digest")
                    != request.expected_applied_record_digest
                    or applied.get("scenario_id") != request.scenario_id
                    or applied.get("source_document_digest")
                    != request.source_document_digest
                ):
                    raise ProgramRunnerError(
                        "program_run_applied_selection_stale",
                        "Applied Program changed before Run admission",
                    )
                if request.draft_program_revision != applied.get("program_revision"):
                    raise ProgramRunnerError(
                        "program_run_draft_applied_mismatch",
                        "Draft and Applied Program revisions differ",
                    )
                simulation_required, simulation_availability = self._simulation_context_requirement(
                    program=applied["program"], program_binding=applied["program_binding"],
                    retained_bundle_id=applied["retained_bundle_id"], graph_revision=applied["graph_revision"],
                )
                if simulation_required and not simulation_availability.available:
                    raise ProgramRunOwnerError(
                        "Cannot start Program: required simulator is unavailable for the applied cell or configured owner"
                    )
                selected_owners = dict(self._owner_availability)
                selected_owners["simulator_control"] = simulation_availability
                run_admission = build_applied_program_run_admission(
                    applied,
                    expected_scenario_id=request.scenario_id,
                    expected_source_document_digest=(
                        request.source_document_digest
                    ),
                    environment=self._environment,
                    owner_availability=selected_owners,
                )
                declared = self._with_global_rule_keys(
                    run_admission.program,
                    retained_bundle_id=applied["retained_bundle_id"],
                    graph_revision=applied["graph_revision"],
                )
                if declared is not run_admission.program:
                    run_admission = replace(run_admission, program=declared)
                # A `rules.set_active` naming a rule the Deployment lacks
                # blocks Execute.
                unknown_rules = sorted(
                    set(declared.rule_activation_rule_ids)
                    - set(self._applied_global_rule_ids(
                        retained_bundle_id=str(applied["retained_bundle_id"]),
                        graph_revision=str(applied["graph_revision"]),
                    ))
                )
                if unknown_rules:
                    raise ProgramRunnerError(
                        "rule_unknown",
                        f"Rule {unknown_rules[0]} is not in the Deployment",
                        details={"rule_id": unknown_rules[0]},
                    )
                self._require_run_readiness("can_execute")
                self._require_end_effector_tcps()
                pre_claim_dropped: tuple[Any, ...] = ()
                if self._persistent_keys is not None:
                    # Run is refused, not started, while the
                    # register cannot be reconciled. A plain write failure
                    # leaves no remembered fault, so storage is reconciled
                    # here before any claim; the post-claim check below stays
                    # as a backstop for a fault first seen after the claim.
                    _activation, _program, pre_claim = self._reconcile_persistent_keys(
                        self._persistent_keys,
                        expected_record_digest=applied["source"]["record_digest"],
                    )
                    pre_claim_dropped = pre_claim.dropped
                # One identity spans the synchronous Program-cycle claim,
                # scene context, Program owner, and every Kernel operation.
                # The Kernel can therefore fence a Run before its first
                # ordinary motion submission arrives.
                # The run number is the cycle generation; it is announced
                # only once the claim is journaled (K-6).
                program_run_id = cycle.cycle_generation
                scene_context_id = ("run", program_run_id)
                claim_intent = {
                    "claim_kind": "program_cycle",
                    "owner_role": "program_runner",
                    "owner_transaction_id": program_run_id,
                    # The claim is the Run's first Kernel action (1.15.2).
                    "request_key": format_request_key(request_key),
                    "expected_admission_generation": generation,
                    "scope": {"resources": [], "configurations": []},
                    "program_cycle_evidence": {
                        "draft_program_revision": request.draft_program_revision,
                        "applied_program_revision": applied["program_revision"],
                    },
                    "intervention_evidence": None,
                }
                # Run never prepares or compares the Scene. Starting remains
                # backend truth before owner acquisition and capture.
                self._command_step("claiming")
                journal.begin_acquire(
                    expected_generation=cycle.cycle_generation,
                    provisional_run_id=program_run_id,
                    evidence=claim_intent,
                    causal_request_key=request_key,
                )
                starting_committed = True
                scene_context = run_admission.port_context(program_run_id)
                execution_identity = self._kernel.identity()
                # The admission generation read here is claim evidence only:
                # the Kernel admits the claim on its current facts, so a
                # generation moved by this Run's own ``starting`` publication
                # is no refusal.
                owner_admission = admission_client.get()
                generation = self._admission_generation(owner_admission)
                claim_intent["expected_admission_generation"] = generation
                journal.refresh_pending_acquire(
                    expected_generation=cycle.cycle_generation,
                    provisional_run_id=program_run_id,
                    evidence=claim_intent,
                )
                try:
                    acquire_attempted = True
                    claim = admission_client.acquire(claim_intent)
                    acquired_for_request = True
                except CellAdmissionError as error:
                    if error.outcome_unknown:
                        journal.mark_acquire_unknown(
                            expected_generation=cycle.cycle_generation
                        )
                        raise
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
                self._command_step("starting_run")
                # Re-read Applied after the Kernel has made mutation exclusion
                # authoritative, before any capture or execution effect.
                applied_after_claim = self._headless.program_applied()
                if applied_after_claim != applied:
                    raise ProgramRunnerError(
                        "program_run_applied_selection_stale",
                        "Applied Program changed during Run admission",
                    )
                recovery_fences = self._run_recovery_fences(
                    applied_after_claim,
                    execution_identity,
                    scene_admission_epoch=generation,
                )
                persistent_reconciliation: PersistentKeyReconciliation | None = None
                if self._persistent_keys is not None:
                    # Backstop for a committed Apply whose notification never
                    # arrived; the claim excludes any Apply until the run ends.
                    _activation, _program, persistent_reconciliation = (
                        self._reconcile_persistent_keys(
                            self._persistent_keys,
                            expected_record_digest=applied_after_claim["source"][
                                "record_digest"
                            ],
                        )
                    )
                self._kernel.register_program_run_capture(
                    {
                        "schema_version": "manyforge.program_run_execution_capture.v2",
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
                controls = ProgramRuntimeControls(
                    snapshot.capture.to_dict(),
                    recovery_fences=recovery_fences,
                )
                if self._persistent_keys is not None:
                    # Stored values dropped when this run's committed Program
                    # no longer fits them are reported, never silent.
                    dropped = pre_claim_dropped + (
                        ()
                        if persistent_reconciliation is None
                        else persistent_reconciliation.dropped
                    )
                    for notice in (*dropped, *self._persistent_keys.take_notices()):
                        controls.audit_value(
                            "persistent_key_value_dropped",
                            project_path=notice.project_path,
                            key=notice.key,
                            reason=notice.reason,
                            detail=notice.detail,
                        )
                with self._runtime_controls_lock:
                    self._runtime_controls[snapshot.capture.program_run_id] = controls
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
                    "source_document_digest",
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
                # Capture the baseline before the Running CAS, but keep this
                # binding inactive until the start request has fully completed.
                carried_object_run_id = program_run_id
                self._begin_carried_object_lost_run(program_run_id)
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
                # The Run's fixed referenced-key union: the keys
                # its interrupt conditions name plus the installed global
                # rules' condition keys. The baseline must be
                # captured here, before the first tick.
                self._start_bbkey_publication(
                    program_run_id,
                    claim_generation=claim["claim_generation"],
                    program_binding_id=scene_context.program_binding_id,
                    active_publication_generation=recovery_fences["deployment_generation"],
                    cycle_generation=cycle.cycle_generation,
                    keys=self._learn_condition_keys(run_admission.program),
                    retained_bundle_id=applied["retained_bundle_id"],
                    graph_revision=applied["graph_revision"],
                    binding=run_admission.program_binding,
                )
                # The run's tree and per-run objects are built: set them aside
                # (OD-234) so automatic full collections during the run do not
                # scan them; no collection, so Execute gets no pause. Reset's
                # collection frees them.
                freeze_heap("run_start")
                # Capture first, then publish the durable Running owner state,
                # and only then enable the first effectful tree tick.
                self._host.schedule(program_run_id)
                journal.complete_request(
                    request_key=request_key,
                    result=result,
                )
                admission_complete = True
                self._activate_carried_object_lost_run(program_run_id)
            except HeadlessClientError as error:
                outcome = self._headless_error(error)
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        request_key=request_key,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                        failure=outcome,
                    )
                else:
                    retain_unacquired_start_uncertainty()
            except CellAdmissionError as error:
                outcome = ProgramRunnerError(
                    error.code, str(error), details=error.details,
                    producer=error.producer or "program_runner",
                )
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        request_key=request_key,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                        failure=outcome,
                    )
                else:
                    retain_unacquired_start_uncertainty()
            except ProgramRunnerError as error:
                outcome = error
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        request_key=request_key,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                        failure=outcome,
                    )
                else:
                    retain_unacquired_start_uncertainty()
            except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                outcome = self._runner_error(error)
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        request_key=request_key,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                        failure=outcome,
                    )
                else:
                    retain_unacquired_start_uncertainty()
            except Exception as error:
                outcome = ProgramRunnerError("program_run_admission_failed", str(error) or type(error).__name__)
                release_scene_context()
                if acquired_for_request:
                    self._terminalize_failed_start(
                        journal,
                        request_key=request_key,
                        scene_proof=scene_proof,
                        effect_may_have_started=host_start_invoked,
                        started_snapshot=started_snapshot,
                        failure=outcome,
                    )
                else:
                    retain_unacquired_start_uncertainty()
            else:
                return result
            finally:
                if not admission_complete:
                    self._retire_carried_object_lost_run(carried_object_run_id)
            if acquired_for_request:
                failed_cycle = journal.read()
                details = {
                    **outcome.details,
                    "cause_code": outcome.code,
                    "cause_message": str(outcome),
                    "program_run_id": failed_cycle.program_run_id,
                    "claim_generation": claim["claim_generation"],
                    "needs_reset": True,
                }
                if failed_cycle.state == "terminal_awaiting_reset":
                    # A held Program-cycle claim is an accepted start, even
                    # when no host run or ordinary motion was created.
                    outcome = ProgramRunnerError(
                        "program_run_started_then_failed",
                        f"Program run {failed_cycle.program_run_id} started, then failed: "
                        f"{outcome}. Reset the Program cycle before another Run.",
                        details={**details, "cause_producer": outcome.producer},
                    )
                    journal.complete_request(
                        request_key=request_key,
                        result={"error": {"code": outcome.code, "message": str(outcome),
                                          "details": outcome.details}},
                    )
                else:
                    # Unknown settlement stays unresolved and cannot become
                    # an ordinary refusal or a synthetic terminal result.
                    outcome = ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        str(outcome), details={**details, "cause_producer": outcome.producer},
                    )
            else:
                self._retain_rejection(
                    journal,
                    request_key=request_key,
                    error=outcome,
                )
            raise outcome

    def read_run(self, program_run_id: int) -> dict[str, Any]:
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

    def preview_node_correction(
        self,
        program_run_id: int,
        request: ProgramNodeCorrectionPreviewRequest,
    ) -> dict[str, Any]:
        """Build the native preview intent of one held motion node; no effect.

        The node's inputs are resolved as a fresh attempt would resolve them,
        with the proposed paths replaced, and translated by the execution
        builder. Nothing is ticked, submitted, retried or written; the caller
        sends the returned intent to the native preview.
        """

        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is closed"
            )
        request_key = _request_key(request)
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        try:
            live = self._owner.read(program_run_id)
        except ProgramRunOwnerError as error:
            raise ProgramRunnerError("program_run_not_found", str(error)) from error
        capture = live.capture
        if (
            capture.program_id != request.program_id
            or capture.program_revision != request.program_revision
            or capture.program_binding_id != request.program_binding_id
        ):
            raise ProgramRunnerError(
                "program_node_preview_identity_stale",
                "the preview names another captured Program",
            )
        intervention = self._owner.intervention(
            program_run_id, node_attempt_id=request.node_attempt_id
        )
        if (
            intervention is None
            or intervention.identity.node_instance_id != request.node_instance_id
        ):
            raise ProgramRunnerError(
                "program_node_preview_attempt_stale",
                "the preview names no exact held attempt of this node",
            )
        result: dict[str, Any] = {
            "schema_version": PROGRAM_NODE_PREVIEW_INTENT_VERSION,
            "request_key": format_request_key(request_key),
            "program_run_id": program_run_id,
            "run_revision": live.run_revision,
            "program_id": capture.program_id,
            "program_revision": capture.program_revision,
            "program_binding_id": capture.program_binding_id,
            "retained_bundle_id": capture.retained_bundle_id,
            "graph_revision": capture.graph_revision,
            "node_instance_id": request.node_instance_id,
            "node_attempt_id": request.node_attempt_id,
            "node_id": None,
            "proposed_paths": [proposal.path for proposal in request.proposed_inputs],
            "parameters_digest": None,
            "target": None,
            "unavailable_reason": None,
            "operation": None,
        }

        def unavailable(code: str, message: str) -> dict[str, Any]:
            result["unavailable_reason"] = {
                "code": code,
                "message": message[:1024] or code,
            }
            return result

        try:
            node_id, parameters, blackboard = self._owner.preview_node_inputs(
                program_run_id,
                node_instance_id=request.node_instance_id,
                proposed=[
                    (proposal.path, proposal.value)
                    for proposal in request.proposed_inputs
                ],
            )
        except NodeInputProposalError as error:
            cause = error.__cause__
            details = cause.details if isinstance(cause, ProgramFrameReferenceError) else None
            raise ProgramRunnerError(
                "program_node_preview_input_invalid", str(error), details=details
            ) from error
        except ProgramRunOwnerError as error:
            raise ProgramRunnerError("program_run_not_found", str(error)) from error
        except ProgramValueError as error:
            # A sourced input other than the proposal cannot be resolved now.
            return unavailable(input_failure_reason(error), str(error))
        result["node_id"] = node_id
        result["parameters_digest"] = (
            "sha256:" + hashlib.sha256(rfc8785.dumps(dict(parameters))).hexdigest()
        )
        try:
            participant, operation = self._native.motion_preview_intent(
                intervention.identity,
                node_id,
                parameters,
                request_key=request_key,
                active_tcp=blackboard.get("active_tcp"),
            )
        except ProgramMotionPreviewUnavailableError as error:
            return unavailable(error.reason_code, str(error))
        except ProductionPortError as error:
            # Execution would refuse these exact inputs identically.
            raise ProgramRunnerError(
                "program_node_preview_input_invalid", str(error)
            ) from error
        target = participant.target
        result["target"] = target.reference()
        result["operation"] = operation
        return result

    def _runtime_controls_projection(
        self,
        program_run_id: int,
        *,
        live: Mapping[str, Any],
        controls: ProgramRuntimeControls,
        recovery_offer: dict[str, Any] | None,
        execution_selections: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.program_runtime_control_projection.v10",
            "owner_id": self.owner_id,
            "program_run_id": program_run_id,
            "run_revision": live["run_revision"],
            "runtime_controls": controls.snapshot(),
            "recovery_offer": recovery_offer,
            "recovery_offers": self._recovery_offers(
                program_run_id, cycle_generation=self._current_cycle_generation()
            ),
            "execution_selections": deepcopy(dict(execution_selections)),
        }

    def _current_cycle_generation(self) -> int:
        return 1 if self._journal is None else self._journal.read().cycle_generation

    def read_runtime_controls(self, program_run_id: int) -> dict[str, Any]:
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
        request_key = _request_key(request)
        self._require_manager_current()
        with self._runner_command(kind, request.request_key):
            self._require_manager_current()
            _admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            intent = request.model_dump(mode="json")
            try:
                record = journal.record_request(
                    request_key=request_key,
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
                    request_key=request_key,
                    error=error,
                )
                raise error

            if (
                cycle.program_run_id != request.program_run_id
                or cycle.state != "running"
                or cycle.reconciliation_required
                or (
                    kind in {"start_motion", "resume_motion", "prepare_manual_handover"}
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
            # Run/Resume names its run by number and selection revision;
            # tree ticks raise the run revision without changing that.
            if (
                kind not in {"start_motion", "resume_motion"}
                and live.run_revision != request.expected_run_revision
            ):
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
            # Accepted (OD-214): the effect's checks ask other processes.
            self._command_accepted(
                "applying" if kind == "runtime_blackboard_write" else "admitting"
            )

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
                    "schema_version": "manyforge.program_runtime_control_result.v2",
                    "request_key": format_request_key(request_key),
                    "accepted": True,
                    "result": payload,
                    "run": projected,
                    "runtime_controls": self.read_runtime_controls(
                        request.program_run_id
                    ),
                }
                self._complete_control(
                    journal,
                    request_key,
                    result=result,
                    projected=projected,
                    held_failures=live.operation_failures,
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
                code = (
                    getattr(error, "code", None) or "program_runtime_control_rejected"
                )
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
    def _complete_control(
        journal: ProgramCycleJournal,
        request_key: bytes,
        *,
        result: Mapping[str, Any],
        projected: Mapping[str, Any],
        held_failures: Sequence[Mapping[str, Any]],
    ) -> None:
        """Retain one accepted control effect; the one place for every kind.

        An effect that resolved a failure row held before it released that
        failure hold: it is a later accepted control revision, so the hold's
        operation-failure condition clears.
        """

        journal.complete_control_request(
            request_key=request_key,
            result=result,
            run_snapshot=projected,
            advance_execution_control=_released_failure_hold(
                held_failures, projected
            ),
        )

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
                "is blocked until the Program is Reset."
            ),
        }
        if error is not None:
            failure["cause_code"] = getattr(error, "code", type(error).__name__)
        return failure

    def write_runtime_blackboard(
        self, program_run_id: int, request: RuntimeBlackboardWriteRequest
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
                request_key=_request_key(request),
            )
            controls.audit_value("runtime_blackboard_written", key=request.key)
            return {"key": request.key, "status": "written"}

        return self._control_effect(
            request, kind="runtime_blackboard_write", effect=effect
        )

    # Persistent blackboard values belong to a Scenario, not to one run. While
    # a run of that Scenario is live its value store owns them, so edits go
    # through it and stay serialized with node writes; otherwise they go to
    # the register directly. Either way they are validated and audited, and
    # admitted like runtime controls: the request identity is journaled while
    # in flight, and once completed it is kept in the journal's bounded table
    # of completed persistent-value requests, so a resend is refused by name
    # and never applied again. A live run also fences the edit by the
    # revision the operator saw; without a run the last admitted edit wins.

    def _persistent_key_register(self) -> PersistentKeyRegister:
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is not started"
            )
        if self._persistent_keys is None:
            raise ProgramRunnerError(
                "persistent_key_register_unavailable",
                "Program runner has no persistent-key register",
            )
        return self._persistent_keys

    def _committed_projection(self) -> dict[str, Any]:
        """Validate the full committed inventory before interpreting its origin."""

        try:
            projection = self._headless.committed_scenario_activation()
            if (
                not isinstance(projection, Mapping)
                or projection.get("schema_version")
                != "manyforge.committed_scenario_activation.v6"
                or not isinstance(projection.get("origin"), Mapping)
            ):
                raise ProgramRunnerError(
                    "persistent_key_activation_unavailable",
                    "the committed activation lacks the current typed origin",
                )
            return validate_committed_scenario_activation(projection)
        except (HeadlessClientError, ProgramBindingTransportError) as error:
            raise ProgramRunnerError(
                "persistent_key_activation_unavailable",
                f"the committed Scenario activation is unavailable: {error}",
            ) from error

    def _committed_activation(
        self, projection: dict[str, Any] | None = None
    ) -> tuple[CommittedActivation, Program | None]:
        """Read a genuine Scenario from the completely validated inventory."""

        if projection is None:
            projection = self._committed_projection()
        program: Program | None = None
        if projection["program"] is not None:
            try:
                program = Program.from_dict(
                    deepcopy(projection["program"]), self._environment.catalog
                )
                if projection["program_frame_facts"] is not None:
                    wire_facts = projection["program_frame_facts"]
                    program = program.with_program_frame_facts(
                        ProgramFrameFacts.from_capture(wire_facts, {
                            **projection,
                            "applied_record_digest": projection["record_digest"],
                            "program_revision": program.program_revision,
                        })
                    )
            except (KeyError, TypeError, ValueError) as error:
                raise ProgramRunnerError(
                    "persistent_key_program_unavailable",
                    f"Applied Program cannot be read: {error}",
                ) from error
        try:
            activation = CommittedActivation(
                record_digest=projection["record_digest"],
                project_path=projection["origin"]["project_path"],
                declarations={} if program is None else dict(program.blackboard_keys),
                apply_id=projection["apply_id"],
            )
        except PersistentKeyRegisterError as error:
            raise ProgramRunnerError(
                "persistent_key_activation_unavailable", str(error)
            ) from error
        return activation, program

    def _reconcile_persistent_keys(
        self,
        register: PersistentKeyRegister,
        *,
        expected_record_digest: str | None = None,
        projection: dict[str, Any] | None = None,
    ) -> tuple[CommittedActivation, Program | None, PersistentKeyReconciliation]:
        """Make the register follow Manager's current committed activation."""

        if projection is None:
            projection = self._committed_projection()
        # An unreadable Program takes precedence over a stale record.
        activation, program = self._committed_activation(projection)
        if (
            expected_record_digest is not None
            and projection["record_digest"] != expected_record_digest
        ):
            raise ProgramRunnerError(
                "program_run_applied_selection_stale",
                "the committed Scenario activation is not the admitted one",
            )
        reconciliation = register.reconcile(activation)
        return activation, program, reconciliation

    def reconcile_persistent_values(self) -> dict[str, Any]:
        """Follow a committed Apply that Headless observed.

        The notification carries nothing: the runner reads the committed
        activation itself, so a repeated or late notification is a named
        no-op and a lost one is caught by the next reconciliation.
        """

        register = self._persistent_key_register()
        projection = self._committed_projection()
        activation, program, reconciliation = self._reconcile_persistent_keys(
            register, projection=projection
        )
        # A cold Apply changes the Applied binding: the cell-level source
        # follows it (same cycle_generation, new binding); a repeated notice
        # keeps the current source.
        self._applied_condition_keys = (
            () if program is None else program.interrupt_condition_keys
        )
        # No committed Program (e.g. after New): no between-Runs source to ask
        # for (BBSRC; the start would refuse `program_not_selected` anyway).
        if program is not None:
            self._follow_cell_bbkey_source()
        return {
            "schema_version": PERSISTENT_KEY_RECONCILIATION_VERSION,
            "outcome": reconciliation.outcome,
            "record_digest": activation.record_digest,
            "project_path": activation.project_path,
            "dropped": [
                {
                    "project_path": notice.project_path,
                    "key": notice.key,
                    "reason": notice.reason,
                }
                for notice in reconciliation.dropped
            ],
        }

    def _applied_persistent_keys(
        self,
        register: PersistentKeyRegister,
        project_path: str | None = None,
    ) -> tuple[
        Program,
        dict[str, BlackboardKeyDeclaration],
        CommittedActivation,
        str,
    ]:
        """The Applied Scenario's committed Program, after reconciling values.

        Values belong to the Applied Scenario project folder. An edit names
        the folder the operator saw (``project_path``); another one is
        refused. Also returns the Applied Scenario id, for display only.
        """

        projection = self._committed_projection()
        activation, program, _reconciliation = self._reconcile_persistent_keys(
            register, projection=projection
        )
        scenario_id = projection["origin"]["scenario_id"]
        if project_path is not None and activation.project_path != project_path:
            raise ProgramRunnerError(
                "persistent_key_scenario_conflict",
                f"Scenario project {project_path!r} is not the Applied Scenario "
                "project",
            )
        if program is None:
            raise ProgramRunnerError(
                "persistent_key_program_unavailable",
                f"Applied Scenario {scenario_id!r} has no Program",
            )
        return (
            program,
            {
                key: declaration
                for key, declaration in sorted(program.blackboard_keys.items())
                if declaration.persistence == "persistent"
            },
            activation,
            scenario_id,
        )

    def _live_run_id(self, activation: CommittedActivation) -> int | None:
        """The run whose value store currently holds this project's values:
        a live run of this committed activation (its capture names the Apply).
        """

        snapshots, _truncated = self._owner.list_snapshots(
            active_only=True, limit=self.config.maximum_retained_runs
        )
        for snapshot in snapshots:
            capture = snapshot.to_dict()["capture"]
            if capture["applied_record_digest"] == activation.record_digest:
                return capture["program_run_id"]
        return None

    def _persistent_values(
        self,
        register: PersistentKeyRegister,
        scenario_id: str,
        program_revision: str,
        declarations: Mapping[str, BlackboardKeyDeclaration],
        activation: CommittedActivation,
    ) -> dict[str, Any]:
        # Read first: it never counts a change the values omit.
        sequence = register.sequence
        # Only values the register durably reconciled with this committed
        # activation are shown, exactly those a run would restore.
        project_path = activation.project_path
        stored = register.activation
        reconciled = stored is not None and (
            stored["record_digest"],
            stored["project_path"],
        ) == (activation.record_digest, project_path)
        rows = register.values(project_path) if reconciled else {}
        restorable = register.restore(project_path, declarations) if reconciled else {}
        keys = []
        for key, declaration in declarations.items():
            row = rows.get(key) if key in restorable else None
            keys.append(
                {
                    "key": key,
                    "type": declaration.type,
                    "access": declaration.access,
                    "has_initial": declaration.has_initial,
                    "initial": declaration.initial,
                    "stored": row is not None,
                    "value": None if row is None else row["value"],
                    "sequence": None if row is None else row["sequence"],
                }
            )
        return {
            "schema_version": PERSISTENT_KEY_VALUES_VERSION,
            "scenario_id": scenario_id,
            "project_path": project_path,
            "program_revision": program_revision,
            "program_run_id": self._live_run_id(activation),
            "sequence": sequence,
            "keys": keys,
            "edits": register.edits(project_path),
        }

    def read_persistent_values(self) -> dict[str, Any]:
        """The Applied Scenario project's persistent values."""

        register = self._persistent_key_register()
        program, declarations, activation, scenario_id = (
            self._applied_persistent_keys(register)
        )
        return self._persistent_values(
            register,
            scenario_id,
            program.program_revision,
            declarations,
            activation,
        )

    def set_persistent_value(
        self, request: PersistentKeyValueSetRequest
    ) -> dict[str, Any]:
        return self._edit_persistent_value(request, action="set")

    def reset_persistent_value(
        self, request: PersistentKeyValueResetRequest
    ) -> dict[str, Any]:
        return self._edit_persistent_value(request, action="reset")

    def _persistent_edit(
        self,
        request: Any,
        *,
        kind: str,
        effect: Callable[[PersistentKeyRegister], dict[str, Any]],
    ) -> dict[str, Any]:
        """Admit one operator value edit like a runtime control (_control_effect)."""

        request_key = _request_key(request)
        register = self._persistent_key_register()
        self._require_manager_current()
        with self._runner_command(kind, request.request_key):
            self._require_manager_current()
            _admission, journal = self._require_cycle_owner()
            try:
                record = journal.record_request(
                    request_key=request_key,
                    kind=kind,
                    intent=request.model_dump(mode="json"),
                    cycle_generation=journal.read().cycle_generation,
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
            if record.disposition == "completed":
                raise ProgramRunnerError(
                    "persistent_key_request_replayed",
                    f"request {request.request_key} was already applied; "
                    "it is never applied again",
                    details=record.result,
                )
            self._require_new_command(record)
            # Accepted (OD-214).
            self._command_accepted("applying")
            try:
                result = effect(register)
            except ProgramRunnerError as error:
                self._retain_rejection(
                    journal,
                    request_key=request_key,
                    error=error,
                )
                raise
            journal.complete_persistent_value_request(
                request_key=request_key,
                outcome="applied",
            )
            return result

    def _require_persistent_fence(
        self, activation: CommittedActivation, request: Any
    ) -> int | None:
        """Return the live run the edit names, refusing one made on a stale view.

        A live run fences the edit by its revision; without one the last
        admitted edit wins.
        """

        program_run_id = self._live_run_id(activation)
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "persistent_key_values_stale",
                "the run holding these values changed; review them and try again",
                details={
                    "expected_program_run_id": request.program_run_id,
                    "current_program_run_id": program_run_id,
                },
            )
        if program_run_id is None:
            return None
        try:
            live = self._owner.read(program_run_id)
        except ProgramRunOwnerError as error:
            raise ProgramRunnerError("program_run_not_found", str(error)) from error
        if live.run_revision != request.expected_run_revision:
            raise ProgramRunnerError(
                "persistent_key_values_stale",
                "Program run changed before the edit was admitted",
                details={
                    "expected_run_revision": request.expected_run_revision,
                    "current_run_revision": live.run_revision,
                },
            )
        return program_run_id

    def _edit_persistent_value(
        self,
        request: PersistentKeyValueResetRequest,
        *,
        action: str,
    ) -> dict[str, Any]:
        def effect(register: PersistentKeyRegister) -> dict[str, Any]:
            program, declarations, activation, scenario_id = (
                self._applied_persistent_keys(register, request.project_path)
            )
            project_path = activation.project_path
            program_revision = program.program_revision
            if request.expected_program_revision != program_revision:
                raise ProgramRunnerError(
                    "persistent_key_program_stale",
                    "Applied Program changed; review the key before editing it",
                )
            declaration = declarations.get(request.key)
            if declaration is None:
                raise ProgramRunnerError(
                    "persistent_key_not_found",
                    f"Applied Program declares no persistent key {request.key!r}",
                )
            if declaration.access not in OPERATOR_WRITABLE_ACCESS:
                # Set, Reset and Clear are operator value changes; a key the
                # Program declares read-only is changed only by the Program.
                raise ProgramRunnerError(
                    "persistent_key_access_conflict",
                    f"blackboard key {request.key!r} is read-only for the operator",
                )
            if action == "set":
                if (declaration.type == "stored_pose" and isinstance(request.value, Mapping)
                        and isinstance(request.value.get("frame_ref"), Mapping)
                        and request.value["frame_ref"].get("kind") == "target_frame"
                        and getattr(program, "_program_frame_facts", None) is None):
                    raise ProgramRunnerError(
                        "persistent_key_program_unavailable",
                        "Applied Program has no captured frame facts",
                    )
                _validate_persistent_value(program, declaration, request.value)
            program_run_id = self._require_persistent_fence(activation, request)
            if program_run_id is None:
                edit = PersistentKeyEdit(request.request_key, request.source_label)
                try:
                    if action == "set":
                        register.commit(
                            project_path, declaration, request.value, edit=edit
                        )
                    else:
                        register.remove(project_path, request.key, edit=edit)
                except PersistentKeyRegisterError as error:
                    raise _persistent_storage_error(error) from error
                self._follow_idle_persistent_edit(
                    request.key, getattr(request, "value", None), reset=action != "set"
                )
            else:
                controls = self._controls(program_run_id)
                controls.require_event_revisions()
                try:
                    if action == "set":
                        self._owner.write_runtime_blackboard(
                            program_run_id,
                            key=request.key,
                            value=request.value,
                            request_key=_request_key(request),
                        )
                    else:
                        self._owner.reset_runtime_blackboard(
                            program_run_id,
                            key=request.key,
                            request_key=_request_key(request),
                        )
                except ProgramRunOwnerError as error:
                    raise ProgramRunnerError(
                        "persistent_key_run_conflict",
                        f"Program run {program_run_id} refused the edit: {error}",
                    ) from error
                controls.audit_value(
                    f"persistent_value_{'written' if action == 'set' else 'reset'}",
                    key=request.key,
                    request_key=request.request_key,
                )
                self._notify_control_state_change()
            return {
                "schema_version": PERSISTENT_KEY_EDIT_VERSION,
                "request_key": request.request_key,
                "action": action,
                "key": request.key,
                "values": self._persistent_values(
                    register, scenario_id, program_revision, declarations, activation
                ),
            }

        return self._persistent_edit(
            request, kind=f"persistent_value_{action}", effect=effect
        )

    def clear_persistent_values(
        self, request: PersistentKeyValuesClearRequest
    ) -> dict[str, Any]:
        """Reset every writable key of the Applied Scenario project at once.

        Exactly a Reset of each key the Applied Program declares writable:
        read keys keep their stored values.
        """

        def effect(register: PersistentKeyRegister) -> dict[str, Any]:
            _program, declarations, activation, _scenario_id = (
                self._applied_persistent_keys(register, request.project_path)
            )
            project_path = activation.project_path
            program_run_id = self._live_run_id(activation)
            if program_run_id is not None:
                raise ProgramRunnerError(
                    "persistent_key_run_conflict",
                    f"Program run {program_run_id} holds these values; "
                    "reset keys one by one or clear after the run",
                )
            try:
                removed = register.clear(
                    project_path,
                    declarations,
                    edit=PersistentKeyEdit(request.request_key, request.source_label),
                )
            except PersistentKeyRegisterError as error:
                raise _persistent_storage_error(error) from error
            for key, declaration in declarations.items():
                if declaration.access in OPERATOR_WRITABLE_ACCESS:
                    self._follow_idle_persistent_edit(key, None, reset=True)
            return {
                "schema_version": PERSISTENT_KEY_EDIT_VERSION,
                "request_key": request.request_key,
                "action": "clear",
                "project_path": project_path,
                "removed": removed,
            }

        return self._persistent_edit(
            request, kind="persistent_values_clear", effect=effect
        )

    def stage_manual_result(
        self, program_run_id: int, request: ManualResultStageRequest
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
            if intervention.owner_task_id is not None:
                # Offers do not expire: re-read the owner's assessment now,
                # like the Resume and replan presses, before any release.
                previous = intervention.owner_assessment
                try:
                    intervention = self._owner.refresh_failure_assessment(
                        program_run_id, node_attempt_id=request.node_attempt_id
                    )
                    refusal = observed_state_refusal(
                        previous if isinstance(previous, Mapping) else None,
                        intervention.owner_assessment,
                    )
                except Exception as error:
                    raise ProgramRunnerError(
                        "recovery_action_rejected",
                        "manual result: the owner's current assessment is unavailable "
                        f"({str(error) or type(error).__name__})",
                        details={
                            "reason": "owner_assessment_unavailable",
                            "action": "manual_result",
                        },
                    ) from error
                if refusal is not None:
                    raise ProgramRunnerError(
                        "recovery_action_rejected",
                        "manual result: the robot is not observed fresh and at rest now",
                        details={"reason": refusal, "action": "manual_result"},
                    )
            allowed, disabled_reason = manual_result_availability(
                intervention,
                (
                    CANONICAL_DEFAULT_RECOVERY_POLICY
                    if policy is None
                    else policy.recovery_policy
                ),
            )
            if not allowed:
                raise ProgramRunnerError(
                    disabled_reason or "program_manual_result_forbidden",
                    ("manual result is unavailable for the exact held " "attempt"),
                )
            controls.require_event_revisions()
            self._refresh_active_tcp()
            self._command_step("applying")
            snapshot = self._host.resume_recovery(
                program_run_id,
                node_attempt_id=request.node_attempt_id,
                action=(
                    "manual_success"
                    if request.outcome == "succeeded"
                    else "manual_failure"
                ),
                # A failed result has no output; its resolution never carries
                # Continue's marker.
                result=request.output if request.outcome == "succeeded" else None,
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
        request_key = _request_key(request)
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is not started"
            )
        self._require_manager_current()
        with self._runner_command("recovery_action", request.request_key):
            self._require_manager_current()
            _admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            intent = request.model_dump(mode="json")
            try:
                record = journal.record_request(
                    request_key=request_key,
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
                        producer=error.producer,
                    )
                self._retain_rejection(
                    journal,
                    request_key=request_key,
                    error=error,
                )
                raise error

            if (
                cycle.program_run_id is None
                or cycle.state != "running"
            ):
                reject(
                    ProgramRunnerError(
                        "cycle_recovery_offer_stale",
                        "recovery does not name the current Program cycle",
                    )
                )
            program_run_id = cycle.program_run_id
            selection_scope = self._owner.operator_stop_scope(program_run_id)
            # The offer id names one hold: the focused offer or any held node's.
            offer = self._recovery_offer(
                program_run_id,
                cycle_generation=cycle.cycle_generation,
                selection_scope=selection_scope,
            )
            focused = offer is not None and offer.get("offer_id") == request.offer_id
            if not focused:
                offer = next(
                    (
                        row["offer"]
                        for row in self._recovery_offers(
                            program_run_id, cycle_generation=cycle.cycle_generation
                        )
                        if row["offer"].get("offer_id") == request.offer_id
                    ),
                    offer,
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
            intervention = (
                self._owner.intervention(program_run_id)
                if focused
                else self._owner.intervention(
                    program_run_id, node_attempt_id=offer["node_attempt_id"]
                )
            )
            if intervention is None:
                reject(
                    ProgramRunnerError(
                        "cycle_recovery_offer_stale",
                        "held Program attempt is no longer current",
                    )
                )
            # Accepted (OD-214): the checks below ask other processes.
            self._command_accepted("admitting")
            if request.action_id in _MOTION_PRODUCING_RECOVERY_ACTIONS:
                # Held motion continues: like a group's Run/Resume it needs
                # motion_clear, refused before any effect.
                try:
                    self._require_run_readiness("motion_clear")
                except ProgramRunnerError as error:
                    reject(error)
                # The Kernel refuses these while operator-alarm
                # attention gates the held node's resources. Ask it before any
                # effect so a refusal leaves the held attempt untouched; an
                # alarm on disjoint resources does not block this recovery.
                try:
                    self._owner.check_motion_attention(
                        program_run_id,
                        intervention.identity.node_instance_id,
                        request_key,
                    )
                except ProgramRunOwnerError as error:
                    reject(
                        ProgramRunnerError(
                            getattr(error, "code", "cycle_recovery_action_rejected"),
                            str(error),
                            details=getattr(error, "details", None),
                        )
                    )
                except Exception as error:
                    reject(
                        ProgramRunnerError(
                            "cycle_recovery_action_rejected",
                            f"motion attention could not be evaluated: {error}",
                        )
                    )
            controls = self._controls(program_run_id)
            # The run's failure rows before any effect (see _complete_control).
            held_failures = self._owner.read(program_run_id).operation_failures
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
                # Acknowledge, Reset and Continue count no failure Retry.
                counts_retry = not is_non_moving_recovery(
                    request.action_id
                ) and request.action_id != "continue_with_failure"
                controls.require_event_revisions(
                    events=2 if counts_retry else 1,
                    reserved_after=0 if request.action_id == "reset_cycle" else 1,
                )
                if (
                    counts_retry
                    and intervention.policy_parameter != "operator_stop_policy"
                ):
                    # The same captured-policy predicate that built the offer,
                    # revalidated at the exact command; no caller input.
                    controls.require_retry_capacity(
                        intervention.identity.node_instance_id,
                        self._captured_recovery_policy(
                            program_run_id, intervention.identity.node_instance_id
                        ),
                    )
                if request.action_id == "reset_cycle":
                    # The offer only authorizes the action. The ordinary Reset
                    # state machine owns stopping, settlement and restoration.
                    # Do not run a second Stop-before-Reset protocol here.
                    # The nested Reset is this recovery action's: its key is
                    # the action's key with the nested-Reset part, so the
                    # Reset's own parts (Forced Program End, claim) fall on
                    # the action's key too.
                    reset_request = ProgramCycleResetRequest.model_validate(
                        {
                            "request_key": format_request_key(
                                with_part(request_key, Part.NESTED_RESET)
                            ),
                            "source_label": request.source_label,
                            "program_run_id": program_run_id,
                            "expected_cycle_generation": cycle.cycle_generation,
                            "expected_claim_generation": (
                                None
                                if cycle.claim is None
                                else cycle.claim["claim_generation"]
                            ),
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
                            request_key=request_key,
                            error=error,
                        )
                        raise
                    terminal = journal.read_run_snapshot(program_run_id)
                    return {
                        "schema_version": "manyforge.cycle_recovery_result.v3",
                        "request_key": format_request_key(request_key),
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
                elif request.action_id == "continue_with_failure":
                    # Continue: close the held operation, release the hold's
                    # groups like Retry; the held node returns FAILURE.
                    effect_started = True
                    self._command_step("recovering")
                    snapshot = self._host.continue_failure_pause(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        request_key=request_key,
                        reason=request.reason,
                    )
                    owner_effect_accepted = True
                    self._refresh_active_tcp()
                    self._command_step("resuming")
                    snapshot = self._host.resume_recovery(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action="manual_failure",
                        # Marks Continue apart from a staged failed result: on
                        # an unconfirmed-Stop hold it is the Acknowledge.
                        result="continue_with_failure",
                    )
                    owner_result = {"accepted": True, "action": request.action_id}
                    cycle_resumed = True
                elif intervention.policy_parameter != "operator_stop_policy" and not (
                    is_non_moving_recovery(request.action_id)
                ):
                    # Failure pause Retry: recover the interrupted tasks and
                    # the failed step while the hold's motion stays held,
                    # release its groups, then let the tree tick again.
                    effect_started = True
                    self._command_step("recovering")
                    snapshot = self._host.retry_failure_pause(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action=(
                            None
                            if request.action_id == "retry_runtime_step"
                            else request.action_id
                        ),
                        request_key=request_key,
                        reason=request.reason,
                    )
                    owner_effect_accepted = True
                    self._refresh_active_tcp()
                    self._command_step("resuming")
                    snapshot = self._host.resume_recovery(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action="retry",
                    )
                    controls.note_retry(intervention.identity.node_instance_id)
                    owner_result = {"accepted": True, "action": request.action_id}
                    cycle_resumed = True
                elif request.action_id == "retry_runtime_step":
                    effect_started = True
                    self._refresh_active_tcp()
                    self._command_step("resuming")
                    snapshot = self._host.resume_recovery(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action="retry",
                    )
                    cycle_resumed = True
                elif (
                    intervention.policy_parameter == "operator_stop_policy"
                    and intervention.identity.node_instance_id
                    in selection_scope.held_nodes
                    and request.action_id == "replan_from_stopped_state"
                ):
                    effect_started = True
                    self._refresh_active_tcp()
                    self._command_step("recovering")
                    snapshot = self._host.resume_motion(
                        program_run_id,
                        expected_hold_revision=selection_scope.hold_revision,
                        request_key=request_key,
                        reason=request.reason,
                    )
                    owner_effect_accepted = True
                    owner_result = {"accepted": True, "action": request.action_id}
                    cycle_resumed = True
                else:
                    effect_started = True
                    self._command_step("recovering")
                    owner_result = self._owner.apply_owner_recovery(
                        program_run_id,
                        node_attempt_id=intervention.identity.node_attempt_id,
                        action=request.action_id,
                        request_key=request_key,
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
                        producer = (
                            error.get("producer")
                            if isinstance(error, Mapping)
                            else None
                        )
                        # The owner's details carry the typed reason (e.g. a
                        # stale or moving start state found at the press).
                        details = (
                            error.get("details")
                            if isinstance(error, Mapping)
                            else None
                        )
                        reject(ProgramRunnerError(
                            code, message,
                            details=(
                                details if isinstance(details, Mapping) else None
                            ),
                            producer=(
                                producer
                                if isinstance(producer, str) and producer
                                else "program_runner"
                            ),
                        ))
                    owner_effect_accepted = True
                    if request.action_id == "acknowledge_and_continue":
                        snapshot = self._owner.note_runtime_control_change(
                            program_run_id
                        )
                    else:
                        self._refresh_active_tcp()
                        self._command_step("resuming")
                        snapshot = self._host.resume_recovery(
                            program_run_id,
                            node_attempt_id=(intervention.identity.node_attempt_id),
                            action="retry",
                        )
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
                    "schema_version": "manyforge.cycle_recovery_result.v3",
                    "request_key": format_request_key(request_key),
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
                self._complete_control(
                    journal,
                    request_key,
                    result=result,
                    projected=projected,
                    held_failures=held_failures,
                )
                return result
            except ProgramRunnerError as error:
                if effect_started:
                    unresolved(error)
                raise
            except ProgramSchedulingError as error:
                # resume_recovery checks capacity and owner state before its
                # atomic transition, so its direct rejection is a no-effect.
                # After an accepted owner effect (recovery or Retry release),
                # however, the combined outcome is unknown.
                if owner_effect_accepted:
                    unresolved(error)
                reject(
                    ProgramRunnerError(
                        error.code or "program_run_scheduling_failed", str(error)
                    )
                )
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

    def resume_motion(
        self, program_run_id: int, request: ProgramSelectionResumeRequest
    ) -> dict[str, Any]:
        if not isinstance(request, ProgramSelectionResumeRequest):
            raise TypeError("resume_motion requires one ProgramSelectionResumeRequest")
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid", "motion Resume changed its Run"
            )
        request_key = _request_key(request)

        def effect() -> dict[str, Any]:
            self._require_run_readiness("motion_clear")
            try:
                self._refresh_active_tcp()
                self._command_step("applying")
                snapshot = self._host.resume_motion(
                    program_run_id,
                    expected_hold_revision=request.expected_hold_revision,
                    request_key=request_key,
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

    def start_motion(
        self, program_run_id: int, request: ProgramSelectionStartRequest
    ) -> dict[str, Any]:
        if not isinstance(request, ProgramSelectionStartRequest):
            raise TypeError("start_motion requires one ProgramSelectionStartRequest")
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid", "initial motion Run changed its Program"
            )
        request_key = _request_key(request)

        def effect() -> dict[str, Any]:
            self._require_run_readiness("motion_clear")
            self._refresh_active_tcp()
            self._command_step("applying")
            snapshot = self._host.start_motion(
                program_run_id,
                expected_hold_revision=request.expected_hold_revision,
                request_key=request_key,
                group_id=request.group_ids,
                reason=request.reason,
            )
            return {
                "state": snapshot.state.value,
                "run_revision": snapshot.run_revision,
            }

        result = self._control_effect(request, kind="start_motion", effect=effect)
        return self._project_run(result["run"])

    def prepare_manual_handover(
        self,
        program_run_id: int,
        request: ProgramManualHandoverRequest,
    ) -> dict[str, Any]:
        if not isinstance(request, ProgramManualHandoverRequest):
            raise TypeError("manual handover requires one ProgramManualHandoverRequest")
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid", "manual handover changed its Program"
            )
        request_key = _request_key(request)

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
                    self._command_step("applying")
                    snapshot = self._host.prepare_manual_handover(
                        program_run_id,
                        node_indices=tuple(proof.producer_node_indices),
                        expected_run_revision=request.expected_run_revision,
                        expected_hold_revision=proof.context.hold_revision,
                        request_key=request_key,
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

    def resume_halt(
        self,
        program_run_id: int,
        request: ProgramHaltResumeRequest,
    ) -> dict[str, Any]:
        """Execute after Halt: resume one exact preserved tree."""

        if not isinstance(request, ProgramHaltResumeRequest):
            raise TypeError("resume_halt requires one ProgramHaltResumeRequest")
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )

        def effect() -> dict[str, Any]:
            self._require_run_readiness("can_resume")
            self._refresh_active_tcp()
            self._command_step("applying")
            snapshot = self._host.resume_halt(
                program_run_id,
                reason=request.reason,
            )
            return {
                "state": getattr(snapshot.state, "value", snapshot.state),
                "run_revision": snapshot.run_revision,
            }

        result = self._control_effect(
            request,
            kind="resume_halt",
            effect=effect,
        )
        run = result.get("run")
        if not isinstance(run, Mapping):
            raise ProgramRunnerError(
                "program_runner_response_invalid",
                "Execute after Halt omitted its Program-run result",
            )
        return self._project_run(run)

    def _advance_reset_options(
        self, request: ProgramCycleResetRequest, authorization: Mapping[str, Any] | None,
        journal: Any,
    ) -> Any:
        """Run the selected stages once in order, retaining each completed phase."""
        cycle = journal.read()
        if cycle.reset_phase == "restoring_scene":
            try:
                if request.reload_initial_scene:
                    # Keys first: a refused key reset leaves the Scene as it is.
                    self._reset_persistent_keys_to_initial(request)
                    self._prepare_scene_after_reset(request, authorization)
                    try:
                        self._reset_active_tcp()
                    except Exception as error:
                        details = dict(exception_failure_details(error) or {})
                        details["cause_code"] = "active_tcp_reset_failed"
                        raise ProgramRunnerError(
                            "program_cycle_reset_scene_preparation_failed",
                            "Active TCP reset failed",
                            details=details,
                        ) from error
                journal.update_reset_phase(
                    expected_generation=cycle.cycle_generation, program_run_id=request.program_run_id,
                    expected_phase="restoring_scene",
                    phase="restoring_position" if request.reset_robot_position else "releasing",
                    terminal_reference=request.terminal_reference,
                    scene_preparation_completed=request.reload_initial_scene,
                )
                cycle = journal.read()
                self._command_reset_phase(cycle)
            except ProgramRunnerError:
                raise
            except Exception as error:
                raise ProgramRunnerError(
                    "program_cycle_reset_scene_preparation_failed", str(error) or type(error).__name__
                ) from error
        if cycle.reset_phase == "restoring_position":
            try:
                self._reset_robot_position_after_reset(request, authorization)
                journal.update_reset_phase(
                    expected_generation=cycle.cycle_generation, program_run_id=request.program_run_id,
                    expected_phase="restoring_position", phase="releasing",
                    terminal_reference=request.terminal_reference,
                )
                cycle = journal.read()
                self._command_reset_phase(cycle)
            except ProgramRunnerError:
                raise
            except Exception as error:
                raise ProgramRunnerError(
                    "program_cycle_reset_position_failed", str(error) or type(error).__name__
                ) from error
        return cycle

    def _reset_persistent_keys_to_initial(self, request: ProgramCycleResetRequest) -> None:
        """Reload initial scene also resets the Applied Scenario's persistent
        keys: each starts again from `initial:` (or empty), whatever its access.

        The register forgets the stored values in one write, audited with this
        Reset's request; a failure refuses this Reset by name and leaves both
        the values and the Scene as they are (the phase can be retried).
        """

        register = self._persistent_keys
        if register is None:
            return
        try:
            projection = self._committed_projection()
            activation, program, _reconciliation = self._reconcile_persistent_keys(
                register, projection=projection
            )
            if program is None:
                return
            register.reset_to_initial(
                activation.project_path,
                program.blackboard_keys,
                edit=PersistentKeyEdit(
                    request.request_key,
                    "program_reset",
                    program_run_id=request.program_run_id,
                ),
            )
        except (PersistentKeyRegisterError, ProgramRunnerError) as error:
            cause = (
                error
                if isinstance(error, ProgramRunnerError)
                else _persistent_storage_error(error)
            )
            raise ProgramRunnerError(
                "program_cycle_reset_persistent_keys_failed",
                f"persistent keys could not be reset to their initial values: {cause}",
                details={"cause_code": cause.code},
            ) from error

    @staticmethod
    def _accepted_reset_context(
        journal: Any, cycle: Any, pending_commands: tuple[dict[str, Any], ...] | None = None,
    ) -> dict[str, Any] | None:
        pending = journal.pending_commands() if pending_commands is None else pending_commands
        options = ProgramRuntime._accepted_reset_options(journal, cycle, pending)
        if options is None:
            return None
        row = next(row for row in pending if row["kind"] == "reset")
        retained = cycle.reset_effect_claim
        claim = None if retained is None else retained["claim"]
        return {
            "request_key": format_request_key(row["request_key"]),
            "request_payload_digest": ProgramRuntime._digest(row["intent"]),
            # The private Kernel proof keeps exactly its two selectors.
            "reload_initial_scene": options["reload_initial_scene"],
            "reset_robot_position": options["reset_robot_position"],
            "reset_claim_generation": None if claim is None else claim["claim_generation"],
        }

    @staticmethod
    def _accepted_reset_options(
        journal: Any, cycle: Any, pending_commands: tuple[dict[str, Any], ...] | None = None,
    ) -> dict[str, bool] | None:
        if cycle.state not in {"resetting", "recovery_required"}:
            return None
        pending = [row for row in (journal.pending_commands() if pending_commands is None else pending_commands)
                   if row["kind"] == "reset"]
        if len(pending) != 1:
            return None
        row = pending[0]
        if (
            row["cycle_generation"] != cycle.cycle_generation
            or row["intent"].get("program_run_id") != cycle.program_run_id
        ):
            return None
        # Version eight journals own the explicit-option Reset contract. Older
        # implicit-scene journals are rejected intact, never reinterpreted.
        return {
            key: row["intent"].get(key, False)
            for key in ("reload_initial_scene", "reset_robot_position")
        }

    def _reset_robot_position_availability(self) -> dict[str, Any]:
        capability = getattr(self._execution_transport, "reset_robot_position_capability", None)
        if not callable(capability):
            return {"available": False, "reason": "The active Deployment has no qualified simulator position-reset capability"}
        try:
            return capability()["reset_robot_position"]
        except HeadlessApiError as error:
            return {"available": False, "reason": f"Position-reset capability is unavailable ({error.code})"}

    def _require_reset_robot_position_available(self, request: ProgramCycleResetRequest) -> None:
        capability = getattr(self._execution_transport, "reset_robot_position_capability", None)
        try:
            response = None if not callable(capability) else capability()
        except HeadlessApiError as error:
            raise ProgramRunnerError(
                "program_cycle_reset_position_unavailable",
                str(error),
            ) from error
        if response is None or not response["reset_robot_position"]["available"]:
            raise ProgramRunnerError("program_cycle_reset_position_unavailable",
                self._reset_robot_position_availability()["reason"] if response is None
                else response["reset_robot_position"]["reason"])
        self._reset_position_identity = {
            key: response["identity"][key]
            for key in ("application_start_id", "active_publication_generation", "retained_bundle_id", "graph_revision")
        }

    def _reset_robot_position_after_reset(
        self, request: ProgramCycleResetRequest, authorization: Mapping[str, Any] | None
    ) -> None:
        _admission, journal = self._require_cycle_owner()
        cycle = journal.read()
        context = self._accepted_reset_context(journal, cycle)
        if context is None or self._reset_position_identity is None:
            raise ProgramRunnerError("program_cycle_journal_invalid", "Position Reset lost its accepted deployment context")
        body = {
            "authorization": {
                "cycle_generation": cycle.cycle_generation,
                **{key: context[key] for key in ("request_key", "request_payload_digest")},
                "program_run_id": cycle.program_run_id,
                "cycle_claim_generation": None if cycle.claim is None else cycle.claim["claim_generation"],
                "terminal_or_settlement_reference": cycle.terminal_reference,
                "reset_claim_generation": context["reset_claim_generation"],
                # The Kernel validates this in memory (OD-139).
                "program_cycle_status": self._kernel_cycle_status(),
            },
            "expected_deployment_identity": self._reset_position_identity,
        }
        try:
            self._execution_transport.reset_robot_position(body)
        except Exception as error:
            if isinstance(error, HeadlessApiError) and error.details.get("effect_disposition") == "known_no_effect":
                raise ProgramRunnerError(
                    error.code, str(error),
                    producer=error.producer,
                ) from error
            cause_details = {
                "cause_code": getattr(error, "code", type(error).__name__)
            }
            cause_producer = getattr(error, "producer", None)
            if isinstance(cause_producer, str) and cause_producer:
                cause_details["cause_producer"] = cause_producer
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required",
                "Robot position unknown after a failed position reset.",
                details=cause_details,
            ) from error

    @staticmethod
    def _idle_reset_occurrence(
        journal: Any, cycle: Any, pending_commands: tuple[dict[str, Any], ...] | None = None,
    ) -> ProgramCycleResetRequest | None:
        """Recognize selected no-run Reset from its retained accepted intent."""
        if (
            cycle.state not in {"resetting", "recovery_required"}
            or cycle.program_run_id is not None
            or cycle.claim is not None
            or cycle.acquisition_pending is not None
            or cycle.terminal_reference is not None
            or cycle.start_settlement is not None
            or cycle.release_intent is not None
            or cycle.current_command_kind != "program_reset"
            or cycle.causal_request_key is None
            or cycle.reset_phase not in {
                "restoring_scene", "restoring_position", "releasing"
            }
            or (cycle.state == "recovery_required" and (
                not isinstance(cycle.failure, Mapping)
                or cycle.failure.get("failed_phase") != cycle.reset_phase
            ))
        ):
            return None
        pending = journal.pending_commands() if pending_commands is None else pending_commands
        if len(pending) != 1 or pending[0]["kind"] != "reset":
            return None
        row = pending[0]
        try:
            accepted = ProgramCycleResetRequest.model_validate(row["intent"])
        except ValueError:
            return None
        if (
            row["cycle_generation"] != cycle.cycle_generation
            or format_request_key(row["request_key"]) != accepted.request_key
            or accepted.expected_cycle_generation != cycle.cycle_generation
            or accepted.program_run_id is not None
            or accepted.expected_claim_generation is not None
            or accepted.terminal_reference is not None
            or accepted.settlement_evidence_digest is not None
            or not (accepted.reload_initial_scene or accepted.reset_robot_position)
        ):
            return None
        retained = cycle.reset_effect_claim
        if retained is not None:
            claim_key = format_request_key(
                with_part(row["request_key"], Part.RESET_CLAIM)
            )
            intent = retained["intent"]
            if (
                intent.get("claim_kind") != "program_reset"
                or intent.get("owner_role") != "program_runner"
                or intent.get("request_key") != claim_key
                or (retained["claim"] is not None and retained["claim"].get("intent") != intent)
                or (retained["release_intent"] is not None and cycle.reset_phase != "releasing")
            ):
                return None
        return accepted

    @staticmethod
    def _require_same_reset_options(
        request: ProgramCycleResetRequest, accepted: ProgramCycleResetRequest
    ) -> None:
        if (request.reload_initial_scene, request.reset_robot_position) != (
            accepted.reload_initial_scene,
            accepted.reset_robot_position,
        ):
            raise ProgramRunnerError(
                "program_run_request_conflict",
                "Reset options differ from the accepted Reset occurrence",
            )

    def _acquire_reset_effect_claim(self, admission: Any, journal: Any) -> dict[str, Any]:
        cycle = journal.read()
        context = self._accepted_reset_context(journal, cycle)
        if context is None:
            raise ProgramRunnerError("program_cycle_journal_invalid", "Idle Reset lost its accepted occurrence")
        retained = cycle.reset_effect_claim
        if retained is None:
            # The Reset's claim: part 2 of the Reset's key (1.15.2).
            claim_key = format_request_key(
                with_part(parse_request_key(context["request_key"]), Part.RESET_CLAIM)
            )
            intent = {
                "claim_kind": "program_reset", "owner_role": "program_runner",
                "owner_transaction_id": None,
                "request_key": claim_key,
                "expected_admission_generation": self._admission_generation(admission.get()),
                "scope": {"resources": [], "configurations": []},
                "program_cycle_evidence": None, "intervention_evidence": None,
            }
            retained = {"intent": intent, "claim": None, "release_intent": None}
            journal.remember_reset_effect_claim(
                expected_generation=cycle.cycle_generation, expected=None, retained=retained,
            )
        intent = retained["intent"]
        if retained["claim"] is None:
            # A retry after a refused or uncertain acquire replays the retained
            # intent exactly; its admission generation is evidence only.
            # The Kernel proves the Reset claim from this runner's current cycle
            # status, carried beside the intent (OD-139).
            cycle_status = self._kernel_cycle_status()
            try:
                claim = admission.acquire(intent, program_cycle_status=cycle_status)
                if claim.get("intent") != intent:
                    raise RuntimeError("Idle Reset claim receipt does not match its retained intent")
                acquired = {**retained, "claim": claim}
                journal.remember_reset_effect_claim(
                    expected_generation=cycle.cycle_generation, expected=retained, retained=acquired,
                )
                retained = acquired
            except CellAdmissionError as error:
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required" if error.outcome_unknown else error.code,
                    str(error),
                    producer=("program_runner" if error.outcome_unknown
                              else error.producer or "program_runner"),
                ) from error
            except Exception as error:
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required", str(error) or type(error).__name__
                ) from error
        return {
            "cycle_generation": cycle.cycle_generation,
            "reset_claim": {
                key: (retained["claim"]["claim_generation"] if key == "reset_claim_generation" else context[key])
                for key in ("request_key", "request_payload_digest", "reset_claim_generation")
            },
        }

    @staticmethod
    def _retained_release_settled(
        admission: Any, claim: Mapping[str, Any], release: Mapping[str, Any]
    ) -> bool:
        """Read the Kernel's receipt before a retained release is sent again.

        True: that release already took effect; False: the claim is still held."""
        try:
            receipt = admission.recovery_receipt(
                {"intent": claim["intent"], "expected_claim": claim, "expected_release": release}
            )
        except CellAdmissionError as error:
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required" if error.outcome_unknown else error.code,
                str(error),
                producer=("program_runner" if error.outcome_unknown
                          else error.producer or "program_runner"),
            ) from error
        status = receipt.get("status")
        if status not in {"released_exact", "held_exact"}:
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required",
                "Program-cycle release outcome is unresolved",
            )
        return status == "released_exact"

    def _release_reset_effect_claim(
        self, admission: Any, journal: Any, *, settle_unknown: bool = False
    ) -> dict[str, Any]:
        cycle = journal.read()
        retained = cycle.reset_effect_claim
        context = self._accepted_reset_context(journal, cycle)
        if cycle.reset_phase != "releasing" or retained is None or retained["claim"] is None or context is None:
            raise ProgramRunnerError("program_cycle_journal_invalid", "Idle Reset has no exact settled claim")
        claim = retained["claim"]
        release = retained["release_intent"]
        if release is None:
            settlement = {
                "schema_version": "manyforge.program_reset_effect_settlement.v2",
                "cycle_generation": cycle.cycle_generation,
                "request_key": context["request_key"],
                "request_payload_digest": context["request_payload_digest"],
                "claim": claim,
            }
            # The release carries the key of the request that causes it: the
            # Reset (1.15.2).
            release = {
                **{key: retained["intent"][key] for key in ("owner_role", "owner_transaction_id")},
                "request_key": context["request_key"],
                "acquired_admission_generation": claim["acquired_admission_generation"],
                "claim_generation": claim["claim_generation"],
                "settlement": {"settlement_kind": "explicit_reset", "evidence_digest": self._digest(settlement)},
            }
            journal.remember_reset_effect_claim(
                expected_generation=cycle.cycle_generation, expected=retained,
                retained={**retained, "release_intent": release},
            )
        elif settle_unknown and self._retained_release_settled(admission, claim, release):
            return admission.get()
        try:
            return admission.release(release)
        except CellAdmissionError as error:
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required" if error.outcome_unknown else error.code,
                str(error),
                producer=("program_runner" if error.outcome_unknown
                          else error.producer or "program_runner"),
            ) from error
        except Exception as error:
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required", str(error) or type(error).__name__
            ) from error

    def _reset_idle_cycle(
        self, request: ProgramCycleResetRequest, admission: Any, journal: Any, cycle: Any
    ) -> dict[str, Any]:
        accepted = self._idle_reset_occurrence(journal, cycle)
        request_key = _request_key(request)
        if (
            cycle.cycle_generation != request.expected_cycle_generation
            or (cycle.reconciliation_required and self._reset_retry_phase(cycle) is None)
            or cycle.acquisition_pending is not None
            or request.program_run_id is not None
            or request.expected_claim_generation is not None
            or request.terminal_reference is not None
            or request.settlement_evidence_digest is not None
            or (cycle.state != "idle_after_reset" and accepted is None)
        ):
            raise ProgramRunnerError(
                "program_cycle_reset_stale", "Reset does not identify the current idle cycle"
            )
        if accepted is not None:
            self._require_same_reset_options(request, accepted)
        if accepted is None or request.request_key == accepted.request_key:
            try:
                self._require_new_command(journal.record_request(
                    request_key=request_key,
                    kind="reset", intent=request.model_dump(mode="json"),
                    cycle_generation=cycle.cycle_generation,
                ))
            except (ValueError, OverflowError) as error:
                raise ProgramRunnerError(
                    "program_run_request_conflict" if isinstance(error, ValueError)
                    else "execution_control_ingress_busy", str(error)
                ) from error
        # Accepted (OD-214): recorded, or the retained occurrence continues.
        self._command_accepted("admitting")
        selected = request.reload_initial_scene or request.reset_robot_position
        started = False
        try:
            if selected:
                if request.reset_robot_position:
                    self._require_reset_robot_position_available(request)
                if accepted is None:
                    current_admission = admission.get()
                    if current_admission.get("authoring_allowed") is not True:
                        raise ProgramRunnerError(
                            "authoring_not_ready",
                            "authoring admission is not ready for selected Reset effects",
                            details={
                                "admission_generation": self._admission_generation(current_admission),
                                "authoring_allowed": current_admission.get("authoring_allowed"),
                                "expected_admission_generation": self._admission_generation(current_admission),
                            },
                        )
                    if request.reload_initial_scene:
                        # No Applied Scenario, no initial scene: refuse before
                        # the cycle, the claim or the stored keys change.
                        try:
                            self._headless.program_applied()
                        except HeadlessClientError as error:
                            if error.code != "program_applied_selection_not_found":
                                raise
                            raise ProgramRunnerError(
                                "program_applied_selection_not_found",
                                "Reload initial scene needs a loaded Scenario; Load one first",
                            ) from error
                    journal.begin_idle_resetting(
                        expected_generation=cycle.cycle_generation,
                        causal_request_key=request_key,
                        phase="restoring_scene" if request.reload_initial_scene else "restoring_position",
                    )
                else:
                    phase = self._reset_retry_phase(cycle)
                    if phase is None:
                        raise ProgramRunnerError(
                            "program_cycle_reset_stale", "Idle Reset has no resumable phase"
                        )
                    journal.resume_resetting(
                        expected_generation=cycle.cycle_generation, program_run_id=None,
                        expected_failure_code=cycle.failure["code"], phase=phase,
                        causal_request_key=request_key,
                        allow_reconciliation=cycle.reconciliation_required,
                    )
                    request = accepted
                started = True
                self._command_reset_phase(journal.read())
                authorization = self._acquire_reset_effect_claim(admission, journal)
                if request.reset_robot_position:
                    self._require_reset_robot_position_available(request)
                # A latched retry settles a retained release from the receipt.
                settle_unknown = cycle.reconciliation_required
                cycle = self._advance_reset_options(request, authorization, journal)
                released = self._release_reset_effect_claim(
                    admission, journal, settle_unknown=settle_unknown
                )
            result = {
                "schema_version": "manyforge.program_cycle_reset_result.v1",
                "program_run_id": None,
                "reset_cycle_generation": cycle.cycle_generation,
                "idle_cycle_generation": cycle.cycle_generation + 1,
                "settlement_reference": None,
                "admission": released if selected else admission.get(),
                "runtime_controls": None,
            }
            publish = journal.publish_reset if selected else journal.publish_idle_reset
            publish(
                expected_generation=cycle.cycle_generation,
                request_key=parse_request_key(request.request_key),
                result=result,
            )
            self._reset_rule_activation()
            self._follow_cell_bbkey_source()
            return result
        except Exception as error:
            cause = error if isinstance(error, ProgramRunnerError) else ProgramRunnerError(
                "program_cycle_reset_failed", str(error) or type(error).__name__
            )
            if started:
                try:
                    journal.fail_reset(
                        expected_generation=cycle.cycle_generation, program_run_id=None,
                        code=cause.code, message=str(cause),
                        reconciliation_required=cause.code == "program_cycle_reconciliation_required",
                    )
                except Exception:
                    pass
                raise ProgramRunnerError(
                    "program_cycle_recovery_required", str(cause),
                    details={"cause_code": cause.code},
                ) from error
            if accepted is None:
                self._retain_rejection(
                    journal, request_key=request_key, error=cause,
                )
            raise cause

    def _settle_unknown_acquire(
        self, request: ProgramCycleResetRequest, admission: Any, journal: Any, cycle: Any
    ) -> ProgramCycleResetRequest:
        """Replay the retained cycle-claim acquire exactly; one attempt per Reset.

        A returned claim ends the start as a claimed start that never reached
        the host; a definite refusal proves no claim, so the cycle is Idle."""
        if (
            request.expected_cycle_generation != cycle.cycle_generation
            or request.program_run_id is not None
            or request.expected_claim_generation is not None
            or request.terminal_reference is not None
            or request.settlement_evidence_digest is not None
        ):
            raise ProgramRunnerError(
                "program_cycle_reset_stale",
                "Reset does not identify the current Program cycle",
            )
        try:
            claim = admission.acquire(cycle.acquisition_pending)
        except CellAdmissionError as error:
            if error.outcome_unknown:
                raise ProgramRunnerError(
                    "program_cycle_reconciliation_required", str(error),
                    producer="program_runner",
                ) from error
            journal.cancel_rejected_acquire(expected_generation=cycle.cycle_generation)
            return request
        except Exception as error:
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required", str(error) or type(error).__name__
            ) from error
        journal.remember_acquired_claim(
            expected_generation=cycle.cycle_generation,
            provisional_run_id=cycle.program_run_id,
            claim=claim,
        )
        # The start stopped at its acquire, before any capture or host start.
        self._terminalize_failed_start(
            journal,
            scene_proof=None,
            effect_may_have_started=False,
            request_key=cycle.causal_request_key,
        )
        settled = journal.read()
        return request.model_copy(
            update={
                "program_run_id": settled.program_run_id,
                "expected_claim_generation": claim["claim_generation"],
                "terminal_reference": settled.terminal_reference,
                "settlement_evidence_digest": settled.terminal_reference,
            }
        )

    def _start_failed_without_run(self, cycle: Any, pending=None) -> bool:
        """A start that never ran whose run the run owner does not hold: the
        owner adds a run before its first tick, so the start had no effect."""
        return (
            cycle.capture_identities is None
            and self._current_cycle_recovery_reset(cycle, pending)
            and not self._owner.has_run(cycle.program_run_id)
        )

    def _settle_start_without_run(
        self, request: ProgramCycleResetRequest, journal: Any, cycle: Any
    ) -> ProgramCycleResetRequest:
        """End the start as a claimed start without effect; Reset continues it."""
        if (
            request.expected_cycle_generation != cycle.cycle_generation
            or request.program_run_id != cycle.program_run_id
            or request.expected_claim_generation != cycle.claim["claim_generation"]
            or request.terminal_reference is not None
            or request.settlement_evidence_digest is not None
        ):
            raise ProgramRunnerError(
                "program_cycle_reset_stale",
                "Reset does not identify the current Program cycle",
            )
        self._terminalize_failed_start(
            journal,
            scene_proof=None,
            effect_may_have_started=False,
            request_key=cycle.causal_request_key,
        )
        settled = journal.read()
        return request.model_copy(
            update={
                "terminal_reference": settled.terminal_reference,
                "settlement_evidence_digest": settled.terminal_reference,
            }
        )

    def reset_cycle(self, request: ProgramCycleResetRequest) -> dict[str, Any]:
        if not isinstance(request, ProgramCycleResetRequest):
            raise TypeError("reset_cycle requires one ProgramCycleResetRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable", "Program runner is not started"
            )
        self._require_manager_current()
        with self._runner_command("reset", request.request_key):
            self._require_manager_current()
            admission, journal = self._require_cycle_owner()
            cycle = journal.read()
            if cycle.acquisition_pending is not None and cycle.state == "recovery_required":
                request = self._settle_unknown_acquire(request, admission, journal, cycle)
                cycle = journal.read()
            elif self._start_failed_without_run(cycle):
                request = self._settle_start_without_run(request, journal, cycle)
                cycle = journal.read()
            if cycle.claim is None and cycle.program_run_id is None:
                result = self._reset_idle_cycle(request, admission, journal, cycle)
                # Reset (OD-234): no run exists; this command holds the runner.
                settle_heap("reset")
                return result
            request_key = _request_key(request)
            pending = journal.pending_commands()
            requested_intent = request.model_dump(mode="json")
            try:
                if any(row["request_key"] == request_key for row in pending):
                    # An identical unresolved request is not a fresh operator
                    # confirmation; a conflicting identity must not delete it.
                    self._require_new_command(
                        journal.record_request(
                            request_key=request_key,
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
            # This confirmation's Forced Program End: part 1 of its key.
            stop_request_key = with_part(request_key, Part.RESET_FORCED_END)

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
                        request_key=request_key,
                        error=error,
                    )
                raise error

            reset_retry_phase = self._reset_retry_phase(cycle)
            # A latched retry settles a retained release from the receipt.
            settle_unknown = reset_retry_phase is not None and cycle.reconciliation_required
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
                cycle.cycle_generation != request.expected_cycle_generation
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
            if retained_resets:
                self._require_same_reset_options(
                    request, ProgramCycleResetRequest.model_validate(retained_resets[0]["intent"])
                )
            if request.reset_robot_position:
                self._require_reset_robot_position_available(request)
            if recovery_reset:
                try:
                    # Reset recovery requires the current resident Run.
                    self._owner.read(request.program_run_id)
                except Exception as error:
                    reject(
                        ProgramRunnerError(
                            "program_cycle_reconciliation_required", str(error)
                        )
                    )
            if not retained_resets:
                try:
                    record = journal.record_request(
                        request_key=request_key,
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
            # Accepted (OD-214): recorded where the code records a Reset.
            self._command_accepted("admitting")
            try:
                initial_state = cycle.state
                if reset_retry_phase is None:
                    journal.begin_resetting(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=request.program_run_id,
                        causal_request_key=request_key,
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
                        causal_request_key=request_key,
                        allow_reconciliation=cycle.reconciliation_required,
                    )
                reset_accepted = True
                self._command_reset_phase(journal.read())
                if retained_resets:
                    # Restoration and publication belong to the retained
                    # occurrence. Only this explicit confirmation's Stop is
                    # fresh; overlapping containment retains its old deadline.
                    request = request.model_copy(
                        update={
                            "request_key": format_request_key(
                                retained_resets[0]["request_key"]
                            ),
                        }
                    )
                cycle = journal.read()
                if cycle.reset_phase == "stopping" and initial_state in {
                    "running",
                    "recovery_required",
                }:
                    # A run whose cleanup failed gets one more attempt with
                    # each Reset, never on later ticks (OD-279).
                    self._host.retry_cleanup(request.program_run_id)
                    terminal = self._host.stop_run(
                        request.program_run_id,
                        reason=PROGRAM_RESET_STOP_REASON,
                        stop_request_key=stop_request_key,
                    )
                    if getattr(terminal.state, "value", terminal.state) not in {
                        ProgramRunState.STOPPED.value,
                        ProgramRunState.SUCCEEDED.value,
                        ProgramRunState.FAILED.value,
                    }:
                        # No time limit (OD-214): Reset stays `stopping`
                        # until the run settles.
                        terminal = self._host.wait_terminal(request.program_run_id)
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
                        and cycle.start_settlement is None
                    ):
                        self._host.contain_reset_run(
                            request.program_run_id,
                            stop_request_key=stop_request_key,
                        )
                    journal.update_reset_phase(
                        expected_generation=cycle.cycle_generation,
                        program_run_id=request.program_run_id,
                        expected_phase="stopping",
                        phase="settling",
                        terminal_reference=reference,
                    )
                cycle = journal.read()
                self._command_reset_phase(cycle)
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
                        _scheduling_cause(error)
                        or "program_cycle_reconciliation_required",
                        str(error),
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
                    "owner_transaction_id": intent["owner_transaction_id"],
                    # The key of the request that causes the release: this
                    # Reset occurrence (1.15.2).
                    "request_key": request.request_key,
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
            if cycle.reconciliation_required and not recovery_reset:
                reject(
                    ProgramRunnerError(
                        "program_cycle_reconciliation_required",
                        "Reset cannot settle an unreconciled Program-cycle owner",
                    )
                )
            if cycle.reset_phase == "settling":
                retained_run = journal.read_run_snapshot(request.program_run_id)
                try:
                    if retained_run is None and claim is not None:
                        self._native.scope.reset(request.program_run_id)
                    elif retained_run is not None:
                        self._host.reset_run(request.program_run_id)
                        self._reset_bbkey_publication(request.program_run_id)
                    controls = self._optional_controls(request.program_run_id)
                    if controls is not None:
                        controls.mark_reset(
                            request_key=parse_request_key(request.request_key),
                            source_label=request.source_label,
                        )
                    next_phase = (
                        "restoring_scene" if request.reload_initial_scene
                        else "restoring_position" if request.reset_robot_position
                        else "releasing"
                    )
                    if recovery_reset:
                        try:
                            self._prove_reset_recovery(cycle)
                            self._require_manager_current()
                            journal.settle_reset_recovery(
                                expected=cycle, phase=next_phase
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
                            phase=next_phase,
                            terminal_reference=request.terminal_reference,
                        )
                    cycle = journal.read()
                    self._command_reset_phase(cycle)
                except ProgramRunnerError as error:
                    reject(error)
                except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                    reject(
                        ProgramRunnerError(
                            _scheduling_cause(error)
                            or "program_cycle_reset_failed",
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
            elif cycle.reset_phase not in {"restoring_scene", "restoring_position", "releasing"}:
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
                    "program_run_id": request.program_run_id,
                    "cycle_claim_generation": claim["claim_generation"],
                    "terminal_or_settlement_reference": (request.terminal_reference),
                }
            )
            if claim is None and (request.reload_initial_scene or request.reset_robot_position):
                try:
                    reset_scene_authorization = self._acquire_reset_effect_claim(admission, journal)
                except ProgramRunnerError as error:
                    reject(error)
            controls = self._optional_controls(request.program_run_id)
            try:
                if request.reset_robot_position:
                    self._require_reset_robot_position_available(request)
                cycle = self._advance_reset_options(request, reset_scene_authorization, journal)
            except ProgramRunnerError as error:
                reject(error)
            if claim is None:
                try:
                    released = (
                        self._release_reset_effect_claim(
                            admission, journal, settle_unknown=settle_unknown
                        )
                        if journal.read().reset_effect_claim is not None else admission.get()
                    )
                except ProgramRunnerError as error:
                    reject(error)
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
                # A retained release with an unknown outcome: the Kernel's
                # receipt says whether it took effect; it is resent only while held.
                settled = False
                if settle_unknown and cycle.release_intent is not None:
                    try:
                        settled = self._retained_release_settled(admission, claim, release)
                    except ProgramRunnerError as error:
                        reject(error)
                try:
                    released = admission.get() if settled else admission.release(release)
                except CellAdmissionError as error:
                    if error.outcome_unknown:
                        journal.require_reconciliation(
                            expected_generation=cycle.cycle_generation
                        )
                        reject(ProgramRunnerError(
                            error.code, str(error),
                            producer=error.producer or "program_runner",
                        ))
                    reject(ProgramRunnerError(
                        error.code, str(error),
                        producer=error.producer or "program_runner",
                    ))
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
                    request_key=parse_request_key(request.request_key),
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
            self._reset_rule_activation()
            # Idle and the bounded history row are now durable.  The
            # executable tree is no longer current and must not accumulate
            # for the lifetime of this process. This is idempotent when Reset
            # resumed under a replacement Program Runner.
            self._host.forget_reset_run(request.program_run_id)
            with self._runtime_controls_lock:
                self._runtime_controls.pop(request.program_run_id, None)
            self._forced_end_keys.pop(request.program_run_id, None)
            with self._terminal_observation_lock:
                self._pending_terminal_snapshots.pop(
                    request.program_run_id,
                    None,
                )
            self._follow_cell_bbkey_source()
            # Reset (OD-234), after its Stop and once the run is gone: the
            # tick host refused to forget it while scheduled or in flight, and
            # this command holds the runner, so nothing ticks during the pause.
            settle_heap("reset")
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
        self._retire_bbkey_publishers()
        # The subscription loop sees its flag within one receive slice.
        self._interrupt_state_stop.set()
        follower = self._interrupt_state_follower
        if follower is not None:
            follower.join()
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
        # Ticks have stopped and no request is served: write the last state.
        if self._journal is not None:
            try:
                self._journal.close()
            except Exception as error:
                errors.append(error)
        if self._persistent_keys is not None:
            self._persistent_keys.close()
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
        if errors:
            raise ProgramRunnerError(
                "program_runner_shutdown_failed",
                "; ".join(str(error) or type(error).__name__ for error in errors),
            )


__all__ = [
    "PERSISTENT_KEY_EDIT_VERSION",
    "PERSISTENT_KEY_VALUES_VERSION",
    "PROGRAM_RUN_INDEX_VERSION",
    "ProgramRunnerConfig",
    "ProgramRunnerError",
    "ProgramRuntime",
]
