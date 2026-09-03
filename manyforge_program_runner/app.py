"""Private HTTP surface of the independently restartable Program runner."""

from __future__ import annotations

from contextlib import asynccontextmanager
from typing import Annotated, Any, Protocol, runtime_checkable

from fastapi import FastAPI, Path, Query, Request
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


__all__ = ["ProgramRunnerApplicationPort", "create_app"]
