"""Private HTTP surface of the independently restartable Program runner."""

from __future__ import annotations

from contextlib import asynccontextmanager
from typing import Annotated, Any, Protocol, runtime_checkable

from fastapi import FastAPI, Path, Query, Request
from fastapi.responses import JSONResponse

from manyforge_headless_api.app import RequestBodyLimitMiddleware
from manyforge_headless_api.models import (
    ProgramRunStartRequest,
    ProgramRunStopRequest,
)

from .models import ProgramCycleResetRequest
from .runtime import ProgramRunnerError


@runtime_checkable
class ProgramRunnerApplicationPort(Protocol):
    """Small process surface implemented by the Program semantic owner."""

    def start(self) -> None: ...

    def shutdown(self) -> None: ...

    def health(self) -> dict[str, Any]: ...

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]: ...

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]: ...

    def read_run(self, program_run_id: str) -> dict[str, Any]: ...

    def stop_run(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]: ...

    def reset_cycle(self, request: ProgramCycleResetRequest) -> dict[str, Any]: ...

    def cycle_status(self) -> dict[str, Any]: ...


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
    ):
        return 503
    if code.endswith("_invalid") or code.endswith("_failed"):
        return 400
    return 500


def create_app(runtime: ProgramRunnerApplicationPort) -> FastAPI:
    if not isinstance(runtime, ProgramRunnerApplicationPort):
        raise TypeError("Program-runner app requires one application port")

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        runtime.start()
        try:
            yield
        finally:
            runtime.shutdown()

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
                    "message": str(error),
                }
            },
        )

    @app.get("/api/program/health")
    def health() -> dict[str, Any]:
        return runtime.health()

    @app.get("/api/program/runs")
    def runs(
        active_only: bool = False,
        limit: Annotated[int, Query(ge=1, le=4_096)] = 128,
    ) -> dict[str, Any]:
        return runtime.runs(active_only=active_only, limit=limit)

    @app.post("/api/program/runs")
    def start_run(request: ProgramRunStartRequest) -> dict[str, Any]:
        return runtime.start_run(request)

    @app.get("/api/program/runs/{program_run_id}")
    def read_run(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return runtime.read_run(program_run_id)

    @app.post("/api/program/runs/{program_run_id}/stop")
    def stop_run(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if request.program_run_id != program_run_id:
            raise ProgramRunnerError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return runtime.stop_run(program_run_id, request)

    @app.post("/api/program/cycle/reset")
    def reset_cycle(request: ProgramCycleResetRequest) -> dict[str, Any]:
        return runtime.reset_cycle(request)

    @app.get("/api/program/cycle/status")
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
