"""Private HTTP surface of the independently restartable Program runner."""

from __future__ import annotations

import asyncio
from contextlib import suppress
from contextlib import asynccontextmanager
from dataclasses import dataclass
import json
from threading import Lock
import time
from typing import Annotated, Any, Protocol, runtime_checkable

from fastapi import FastAPI, Path, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from manyforge_headless_api.app import RequestBodyLimitMiddleware
from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
)
from manyforge_headless_api.models import (
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
)

from .models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
    ProgramScenePreparationRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
)
from .runtime import ProgramRunnerError


_PROGRAM_CONTROL_STATE_SCHEMA = "manyforge.program_control_state.v1"
_MAXIMUM_PROGRAM_CONTROL_CLIENTS = 4
_MAXIMUM_PROGRAM_CONTROL_MESSAGE_BYTES = 16 * 1024 * 1024
_NODE_PROGRESS_PERIOD_NS = 50_000_000


@dataclass(eq=False)
class _ProgramControlSubscription:
    wake: asyncio.Event
    closed: bool = False


class ProgramControlStateSource:
    """Thread-safe capacity-one wake source over current Program owner state."""

    def __init__(self, runtime: Any, *, clock_ns=time.monotonic_ns) -> None:
        add = getattr(runtime, "add_control_state_observer", None)
        remove = getattr(runtime, "remove_control_state_observer", None)
        if not callable(add) or not callable(remove):
            raise TypeError("Program runtime has no control-state observation port")
        self._runtime = runtime
        self._add = add
        self._remove = remove
        self._clock_ns = clock_ns
        self._lock = Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._subscriptions: set[_ProgramControlSubscription] = set()
        self._registered = False
        self._closed = False

    def subscribe(self) -> _ProgramControlSubscription:
        loop = asyncio.get_running_loop()
        with self._lock:
            if self._closed:
                raise RuntimeError("Program control-state source is closed")
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("Program control-state source crossed event loops")
            if len(self._subscriptions) >= _MAXIMUM_PROGRAM_CONTROL_CLIENTS:
                raise RuntimeError("Program control-state client limit reached")
            self._loop = loop
            subscription = _ProgramControlSubscription(asyncio.Event())
            self._subscriptions.add(subscription)
            register = not self._registered
            if register:
                self._registered = True
        if register:
            self._add(self.notify)
        subscription.wake.set()
        return subscription

    def unsubscribe(self, subscription: _ProgramControlSubscription) -> None:
        with self._lock:
            subscription.closed = True
            subscription.wake.set()
            self._subscriptions.discard(subscription)
            unregister = self._registered and not self._subscriptions
            if unregister:
                self._registered = False
                self._loop = None
        if unregister:
            self._remove(self.notify)

    def notify(self) -> None:
        with self._lock:
            loop = self._loop
            subscriptions = tuple(self._subscriptions)
        if loop is None:
            return
        for subscription in subscriptions:
            if not subscription.closed:
                loop.call_soon_threadsafe(subscription.wake.set)

    def snapshot(self) -> tuple[dict[str, Any], int]:
        return self._runtime.cycle_status(), self._clock_ns()

    def clock_ns(self) -> int:
        return self._clock_ns()

    def close(self) -> None:
        with self._lock:
            self._closed = True
            subscriptions = tuple(self._subscriptions)
        for subscription in subscriptions:
            self.unsubscribe(subscription)


async def _serve_program_control_state(
    websocket: WebSocket,
    source: ProgramControlStateSource,
) -> None:
    await websocket.accept()
    try:
        subscription = source.subscribe()
    except RuntimeError:
        # The peer may already have abandoned the handshake while overload is
        # being rejected. Rejection must remain bounded and must not turn that
        # ordinary race into an ASGI exception storm.
        with suppress(RuntimeError, WebSocketDisconnect):
            await websocket.close(
                code=1013,
                reason="Program control-state unavailable",
            )
        return
    sequence = 0
    last_control_revision: int | None = None
    last_node_send_ns = 0
    disconnect = asyncio.create_task(websocket.receive())
    try:
        while not subscription.closed:
            wake = asyncio.create_task(subscription.wake.wait())
            ready, _pending = await asyncio.wait(
                (wake, disconnect),
                return_when=asyncio.FIRST_COMPLETED,
            )
            if disconnect in ready:
                if not wake.done():
                    wake.cancel()
                    with suppress(asyncio.CancelledError):
                        await wake
                message = disconnect.result()
                if message.get("type") != "websocket.disconnect":
                    await websocket.close(
                        code=1008,
                        reason="Program control state is server-only",
                    )
                return
            subscription.wake.clear()
            cycle, observed_ns = await asyncio.to_thread(source.snapshot)
            control_revision = cycle.get("execution_control_revision")
            node_only = (
                last_control_revision is not None
                and control_revision == last_control_revision
            )
            remaining_ns = _NODE_PROGRESS_PERIOD_NS - (
                observed_ns - last_node_send_ns
            )
            if node_only and remaining_ns > 0:
                deadline_ns = observed_ns + remaining_ns
                while node_only:
                    timeout = max(
                        0.0,
                        (deadline_ns - source.clock_ns()) / 1_000_000_000,
                    )
                    if timeout == 0.0:
                        break
                    try:
                        await asyncio.wait_for(subscription.wake.wait(), timeout)
                    except asyncio.TimeoutError:
                        break
                    subscription.wake.clear()
                    cycle, observed_ns = await asyncio.to_thread(source.snapshot)
                    control_revision = cycle.get("execution_control_revision")
                    node_only = control_revision == last_control_revision
                cycle, observed_ns = await asyncio.to_thread(source.snapshot)
                control_revision = cycle.get("execution_control_revision")
            sequence += 1
            payload = json.dumps(
                {
                    "schema_version": _PROGRAM_CONTROL_STATE_SCHEMA,
                    "server_sequence": str(sequence),
                    "program_cycle": cycle,
                },
                allow_nan=False,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
            if len(payload.encode("utf-8")) > _MAXIMUM_PROGRAM_CONTROL_MESSAGE_BYTES:
                await websocket.close(code=1009, reason="Program control state oversized")
                return
            await websocket.send_text(payload)
            last_control_revision = (
                control_revision if isinstance(control_revision, int) else None
            )
            last_node_send_ns = observed_ns
    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        if not disconnect.done():
            disconnect.cancel()
            with suppress(asyncio.CancelledError):
                await disconnect
        source.unsubscribe(subscription)


@runtime_checkable
class ProgramRunnerApplicationPort(Protocol):
    """Small process surface implemented by the Program semantic owner."""

    def start(self) -> None: ...

    def shutdown(self) -> None: ...

    def health(self) -> dict[str, Any]: ...

    def node_availability(
        self, request: ProgramNodeAvailabilityRequest
    ) -> dict[str, Any]: ...

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]: ...

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]: ...

    def prepare_applied_scene(
        self, request: ProgramScenePreparationRequest
    ) -> dict[str, Any]: ...

    def read_run(self, program_run_id: str) -> dict[str, Any]: ...

    def stop_run(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]: ...

    def stop_motion(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]: ...

    def full_stop(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]: ...

    def resume_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResumeRequest,
    ) -> dict[str, Any]: ...

    def discard_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResetRequest,
    ) -> dict[str, Any]: ...

    def reset_cycle(self, request: ProgramCycleResetRequest) -> dict[str, Any]: ...

    def cycle_status(self) -> dict[str, Any]: ...

    def read_runtime_controls(self, program_run_id: str) -> dict[str, Any]: ...

    def stage_runtime_override(
        self, program_run_id: str, request: RuntimeOverrideStageRequest
    ) -> dict[str, Any]: ...

    def activate_runtime_override(
        self, program_run_id: str, request: RuntimeOverrideActionRequest
    ) -> dict[str, Any]: ...

    def clear_runtime_override(
        self, program_run_id: str, request: RuntimeOverrideActionRequest
    ) -> dict[str, Any]: ...

    def promote_runtime_override(
        self, program_run_id: str, request: RuntimeOverridePromotionRequest
    ) -> dict[str, Any]: ...

    def write_runtime_blackboard(
        self, program_run_id: str, request: RuntimeBlackboardWriteRequest
    ) -> dict[str, Any]: ...

    def stage_manual_result(
        self, program_run_id: str, request: ManualResultStageRequest
    ) -> dict[str, Any]: ...

    def apply_recovery_action(
        self, request: CycleRecoveryActionRequest
    ) -> dict[str, Any]: ...


def _status(code: str) -> int:
    if code.endswith("_not_found"):
        return 404
    if code.endswith("_stale") or code.endswith("_conflict") or code in {
        "authoring_not_ready",
        "program_cycle_reset_required",
        "program_run_already_active",
        "program_run_draft_applied_mismatch",
        "stale_generation",
    }:
        return 409
    if (
        "unavailable" in code
        or code.endswith("_unknown")
        or code.endswith("_in_progress")
        or code.endswith("_ledger_full")
        or code == "program_cycle_reconciliation_required"
        or code == "execution_control_ingress_busy"
    ):
        return 503
    if code.endswith("_invalid") or code.endswith("_failed"):
        return 400
    return 500


def create_app(runtime: ProgramRunnerApplicationPort) -> FastAPI:
    if not isinstance(runtime, ProgramRunnerApplicationPort):
        raise TypeError("Program-runner app requires one application port")

    execution_controls = ExecutionControlClient(
        thread_name_prefix="manyforge_program_runner_control",
    )
    try:
        control_state_source: ProgramControlStateSource | None = (
            ProgramControlStateSource(runtime)
        )
    except TypeError:
        control_state_source = None

    async def control_call(callback, *, stop: bool = False):
        try:
            lane = execution_controls.stop if stop else execution_controls.ordinary
            # Keep JSON traversal/encoding in the bounded transport worker.
            # Otherwise FastAPI performs it after the await on the shared ASGI
            # loop, where unrelated work can delay an accepted Stop response.
            return await lane(lambda: JSONResponse(content=callback()))
        except ExecutionControlBusyError as error:
            raise ProgramRunnerError(
                "execution_control_ingress_busy",
                str(error),
                details={
                    "lane": error.lane.value,
                    "maximum_admitted": error.maximum_admitted,
                },
            ) from error

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        runtime.start()
        try:
            yield
        finally:
            try:
                runtime.shutdown()
            finally:
                if control_state_source is not None:
                    control_state_source.close()
                execution_controls.close()

    app = FastAPI(
        title="ManyForge Program Runner",
        version="0.1.0",
        lifespan=lifespan,
    )
    app.add_middleware(
        RequestBodyLimitMiddleware,
        maximum_bytes=1024 * 1024,
        error_code="program_runner_request_body_oversized",
        error_message="Program-runner request body exceeds its byte bound",
    )

    @app.exception_handler(ProgramRunnerError)
    async def program_runner_error(
        _request: Request,
        error: ProgramRunnerError,
    ) -> JSONResponse:
        return JSONResponse(
            status_code=_status(error.code),
            content={
                "detail": {
                    "code": error.code,
                    "details": error.details,
                    "message": str(error),
                }
            },
        )

    @app.get("/api/program/health")
    def health() -> dict[str, Any]:
        return runtime.health()

    @app.post("/api/program/node-availability")
    def node_availability(
        request: ProgramNodeAvailabilityRequest,
    ) -> dict[str, Any]:
        return runtime.node_availability(request)

    @app.get("/api/program/runs")
    def runs(
        active_only: bool = False,
        limit: Annotated[int, Query(ge=1, le=4_096)] = 128,
    ) -> dict[str, Any]:
        return runtime.runs(active_only=active_only, limit=limit)

    @app.post("/api/program/runs")
    async def start_run(request: ProgramRunStartRequest) -> dict[str, Any]:
        return await control_call(lambda: runtime.start_run(request))

    @app.post("/api/program/scene-preparations")
    async def prepare_applied_scene(
        request: ProgramScenePreparationRequest,
    ) -> dict[str, Any]:
        return await control_call(lambda: runtime.prepare_applied_scene(request))

    @app.get("/api/program/runs/{program_run_id}")
    def read_run(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return runtime.read_run(program_run_id)

    @app.post("/api/program/runs/{program_run_id}/stop")
    async def stop_run(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await control_call(
            lambda: runtime.stop_run(program_run_id, request),
            stop=True,
        )

    @app.post("/api/program/runs/{program_run_id}/motion-stop")
    async def stop_motion(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await control_call(
            lambda: runtime.stop_motion(program_run_id, request),
            stop=True,
        )

    @app.post("/api/program/runs/{program_run_id}/full-stop")
    async def full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await control_call(
            lambda: runtime.full_stop(program_run_id, request),
            stop=True,
        )

    @app.post("/api/program/runs/{program_run_id}/resume-full-stop")
    async def resume_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ProgramFullStopResumeRequest,
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await control_call(
            lambda: runtime.resume_full_stop(program_run_id, request)
        )

    @app.post("/api/program/runs/{program_run_id}/discard-full-stop")
    async def discard_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ProgramFullStopResetRequest,
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await control_call(
            lambda: runtime.discard_full_stop(program_run_id, request)
        )

    @app.get("/api/program/runs/{program_run_id}/runtime-controls")
    def read_runtime_controls(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return runtime.read_runtime_controls(program_run_id)

    @app.post("/api/program/runs/{program_run_id}/runtime-overrides")
    async def stage_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: RuntimeOverrideStageRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: runtime.stage_runtime_override(program_run_id, request)
        )

    @app.post(
        "/api/program/runs/{program_run_id}/runtime-overrides/activate"
    )
    async def activate_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: RuntimeOverrideActionRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: runtime.activate_runtime_override(program_run_id, request)
        )

    @app.post("/api/program/runs/{program_run_id}/runtime-overrides/clear")
    async def clear_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: RuntimeOverrideActionRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: runtime.clear_runtime_override(program_run_id, request)
        )

    @app.post(
        "/api/program/runs/{program_run_id}/runtime-overrides/promote"
    )
    async def promote_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: RuntimeOverridePromotionRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: runtime.promote_runtime_override(program_run_id, request)
        )

    @app.post("/api/program/runs/{program_run_id}/blackboard-values")
    async def write_runtime_blackboard(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: RuntimeBlackboardWriteRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: runtime.write_runtime_blackboard(program_run_id, request)
        )

    @app.post("/api/program/runs/{program_run_id}/manual-result")
    async def stage_manual_result(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ManualResultStageRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: runtime.stage_manual_result(program_run_id, request)
        )

    @app.post("/api/cycle/recovery-actions")
    async def apply_recovery_action(
        request: CycleRecoveryActionRequest,
    ) -> dict[str, Any]:
        return await control_call(lambda: runtime.apply_recovery_action(request))

    @app.post("/api/program/cycle/reset")
    async def reset_cycle(request: ProgramCycleResetRequest) -> dict[str, Any]:
        return await control_call(lambda: runtime.reset_cycle(request))

    @app.get("/api/cycle/status")
    def cycle_status() -> dict[str, Any]:
        return runtime.cycle_status()

    @app.websocket("/ws/program/control-state")
    async def program_control_state(websocket: WebSocket) -> None:
        if control_state_source is None:
            await websocket.close(code=1008, reason="Program control state unavailable")
            return
        await _serve_program_control_state(websocket, control_state_source)

    @app.get("/api/program/liveness/status")
    def liveness_status() -> dict[str, Any]:
        read = getattr(runtime, "liveness_status", None)
        if not callable(read):
            raise ProgramRunnerError(
                "program_runner_manager_liveness_unavailable",
                "Manager-presence liveness status is unavailable",
            )
        return read()

    return app


__all__ = [
    "ProgramControlStateSource",
    "ProgramRunnerApplicationPort",
    "create_app",
]
