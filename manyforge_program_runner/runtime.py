"""Independent Program orchestration owner assembled from focused clients."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from threading import RLock
from typing import Any, Final
import unicodedata
from urllib.parse import urlsplit
from uuid import uuid4

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
    owner_id: str = "program-runner"
    owner_epoch: str | None = None
    headless_timeout_seconds: float = 30.0
    tick_interval_seconds: float = 0.01
    tick_workers: int = 4
    maximum_retained_runs: int = 4_096
    maximum_replay_records: int = _MAX_REPLAY_RECORDS

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
        if self.owner_epoch is not None and (
            not self.owner_epoch
            or unicodedata.normalize("NFC", self.owner_epoch) != self.owner_epoch
            or len(self.owner_epoch.encode("utf-8")) > 256
        ):
            raise ValueError("owner_epoch must be bounded non-empty text")
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


@dataclass(frozen=True, slots=True)
class _ReplayRecord:
    kind: str
    request: Mapping[str, Any]
    program_run_id: str


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
            request_owner_epoch=self.owner_epoch,
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
        self._host = ProgramRunTickHost(
            self._owner,
            tick_interval_s=config.tick_interval_seconds,
            maximum_scheduled_runs=config.maximum_retained_runs,
            maximum_workers=config.tick_workers,
        )
        self._replay: dict[tuple[str, str], _ReplayRecord] = {}
        self._replay_lock = RLock()
        # Serialize effectful Program lifecycle admission around replay checks.
        # The owner has its own per-run transition locks, but this process-level
        # fence also prevents a full replay ledger from being discovered only
        # after a concurrent Stop has already issued its effect.
        self._transition_lock = RLock()
        self._started = False
        self._closed = False

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
    def _request_key(
        request: ProgramRunStartRequest | ProgramRunStopRequest,
    ) -> tuple[str, str]:
        return request.request_owner_epoch, request.request_id

    @staticmethod
    def _request_value(
        request: ProgramRunStartRequest | ProgramRunStopRequest,
    ) -> dict[str, Any]:
        return request.model_dump(mode="json")

    def _existing_replay(
        self,
        kind: str,
        request: ProgramRunStartRequest | ProgramRunStopRequest,
    ) -> _ReplayRecord | None:
        key = self._request_key(request)
        value = self._request_value(request)
        with self._replay_lock:
            current = self._replay.get(key)
            if current is None:
                return None
            if current.kind != kind or dict(current.request) != value:
                raise ProgramRunnerError(
                    "program_run_request_conflict",
                    "request identity was reused with different Program-run intent",
                )
            return current

    def _commit_replay(
        self,
        kind: str,
        request: ProgramRunStartRequest | ProgramRunStopRequest,
        program_run_id: str,
    ) -> None:
        key = self._request_key(request)
        value = self._request_value(request)
        with self._replay_lock:
            current = self._replay.get(key)
            candidate = _ReplayRecord(kind, value, program_run_id)
            if current is not None:
                if current != candidate:
                    raise ProgramRunnerError(
                        "program_run_request_conflict",
                        "request identity was reused with different Program-run intent",
                    )
                return
            if len(self._replay) >= self.config.maximum_replay_records:
                raise ProgramRunnerError(
                    "program_run_request_ledger_full",
                    "Program-run request replay retention is full",
                )
            self._replay[key] = candidate

    def _require_replay_capacity(self) -> None:
        with self._replay_lock:
            if len(self._replay) >= self.config.maximum_replay_records:
                raise ProgramRunnerError(
                    "program_run_request_ledger_full",
                    "Program-run request replay retention is full",
                )

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

    def start(self) -> None:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is closed",
            )
        if self._started:
            return
        self._host.start()
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

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is closed",
            )
        snapshots, truncated = self._owner.list_snapshots(
            active_only=active_only,
            limit=limit,
        )
        return {
            "schema_version": PROGRAM_RUN_INDEX_VERSION,
            "owner_id": self.owner_id,
            "owner_epoch": self.owner_epoch,
            "active_only": active_only,
            "truncated": truncated,
            "runs": [snapshot.to_dict() for snapshot in snapshots],
        }

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
        if not isinstance(request, ProgramRunStartRequest):
            raise TypeError("start_run requires one ProgramRunStartRequest")
        if self._closed or not self._started:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is not started",
            )
        with self._transition_lock:
            replay = self._existing_replay("start", request)
            if replay is not None:
                return self._owner.read(replay.program_run_id).to_dict()
            self._require_replay_capacity()
            active, _truncated = self._owner.list_snapshots(
                active_only=True,
                limit=1,
            )
            if active:
                raise ProgramRunnerError(
                    "program_run_already_active",
                    "the current product gate permits one active Program run",
                )
            try:
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
                admission = build_applied_program_run_admission(
                    applied,
                    live,
                    expected_scenario_id=request.scenario_id,
                    expected_scenario_catalog_revision=(
                        request.scenario_catalog_revision
                    ),
                    environment=self._environment,
                    owner_availability=self._owner_availability,
                )
                snapshot = self._host.start_run(admission)
            except HeadlessClientError as error:
                raise self._headless_error(error) from error
            except ProgramRunnerError:
                raise
            except (ProgramRunOwnerError, ProgramSchedulingError) as error:
                raise self._runner_error(error) from error
            except Exception as error:
                raise ProgramRunnerError(
                    "program_run_admission_failed",
                    str(error) or type(error).__name__,
                ) from error
            self._commit_replay(
                "start",
                request,
                snapshot.capture.program_run_id,
            )
            return snapshot.to_dict()

    def read_run(self, program_run_id: str) -> dict[str, Any]:
        if self._closed:
            raise ProgramRunnerError(
                "program_runner_unavailable",
                "Program runner is closed",
            )
        try:
            return self._owner.read(program_run_id).to_dict()
        except ProgramRunOwnerError as error:
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
        with self._transition_lock:
            replay = self._existing_replay("stop", request)
            if replay is not None:
                return self.read_run(replay.program_run_id)
            self._require_replay_capacity()
            try:
                current = self._owner.read(program_run_id)
                if current.owner_epoch != request.expected_owner_epoch:
                    raise ProgramRunnerError(
                        "program_run_owner_stale",
                        "Program-run owner epoch changed before Stop",
                    )
                snapshot = self._host.stop_run(
                    program_run_id,
                    reason=request.reason,
                )
            except ProgramRunOwnerError as error:
                raise ProgramRunnerError(
                    "program_run_not_found",
                    str(error),
                ) from error
            except ProgramSchedulingError as error:
                raise self._runner_error(error) from error
            self._commit_replay("stop", request, program_run_id)
            return snapshot.to_dict()

    def shutdown(self) -> None:
        if self._closed:
            return
        self._closed = True
        errors: list[Exception] = []
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
