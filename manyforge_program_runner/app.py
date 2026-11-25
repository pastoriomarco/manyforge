"""The Program runner's private command and status sockets.

Headless reaches the runner through two core-served sockets in the socket
root (``manyforge_headless_py.ProgramCommandServer`` on
``program-command.sock``, ``ProgramStatusPublisher`` on
``program-status.sock``). This module dispatches each received command to the
runtime and publishes the Program control state; the core moves the bytes.

- Commands (Execute, Run, Resume, Reset, recovery, handover, value writes,
  manual result) are answered at their acceptance through
  ``runtime.run_command`` (OD-214), on the bounded ordinary lane: two
  workers, two admitted, a third refused at once with
  ``execution_control_ingress_busy``. The scene preparation and the
  persistent-value reconciliation use the same lane.
- Reads run concurrently on their own pool.
- Status: one publisher thread follows the read requests of the core's
  publisher (each runtime control-state wake, and a subscriber that connects
  while the state is withdrawn), classifies each cycle against the last one
  published, holds a ``node_progress``-only change until 50 ms after the last
  publication unless a critical change comes, completes the cycle with the
  Reset-position availability (one execution-host read, only for a cycle it
  publishes) and hands it to the core, which sends the newest value to each
  subscriber. A state it cannot read is withdrawn: the subscribers it reaches
  end with its error, and each reconnecting one asks for a fresh read.
- A socket another runner serves (or one that cannot be bound) refuses the
  start with ``program_runner_socket_unavailable``.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import logging
from pathlib import Path
from threading import BoundedSemaphore, Thread
import time
from typing import Any, Protocol, runtime_checkable

from pydantic import BaseModel, ValidationError

from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlLane,
)
from manyforge_headless_api.program_cycle import program_cycle_event_class
from manyforge_headless_api.models import (
    ProgramHaltResumeRequest,
    ProgramRunStartRequest,
)

from .models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
    ProgramNodeCorrectionPreviewRequest,
    ProgramScenePreparationRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    PersistentKeyValueResetRequest,
    PersistentKeyValueSetRequest,
    PersistentKeyValuesClearRequest,
    RuntimeBlackboardWriteRequest,
)
from .heap_settle import settle_heap
from .runtime import ProgramRunnerError


_LOGGER = logging.getLogger(__name__)
_NODE_PROGRESS_PERIOD_NS = 50_000_000
_MAXIMUM_PROGRAM_RUN_ID = 9_007_199_254_740_991
# The ordinary lane of the former HTTP surface (ExecutionControlClient's
# default ordinary lane): two workers, two admitted, no waiting.
_LANE_WORKERS = 2
# Concurrent reads: the worker bound the former HTTP surface ran its plain
# reads on (the default thread limit of its server).
_READ_WORKERS = 40
# Headless's code and text for a request the runner refuses as malformed.
_MALFORMED_REQUEST_CODE = "program_runner_http_error"
_MALFORMED_REQUEST_MESSAGE = "Program runner returned HTTP 422"


class _MalformedRequest(Exception):
    """A request that does not fit its command (the former HTTP 422)."""


@runtime_checkable
class ProgramRunnerApplicationPort(Protocol):
    """Small process surface implemented by the Program semantic owner."""

    def start(self) -> None: ...

    def shutdown(self) -> None: ...

    def health(self) -> dict[str, Any]: ...

    def diagnostic_health(self) -> dict[str, Any]: ...

    def node_availability(
        self, request: ProgramNodeAvailabilityRequest
    ) -> dict[str, Any]: ...

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]: ...

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]: ...

    def prepare_applied_scene(
        self, request: ProgramScenePreparationRequest
    ) -> dict[str, Any]: ...

    def read_run(self, program_run_id: int) -> dict[str, Any]: ...

    def preview_node_correction(
        self, program_run_id: int, request: ProgramNodeCorrectionPreviewRequest
    ) -> dict[str, Any]: ...

    def resume_motion(
        self, program_run_id: int, request: ProgramSelectionResumeRequest
    ) -> dict[str, Any]: ...

    def start_motion(
        self, program_run_id: int, request: ProgramSelectionStartRequest
    ) -> dict[str, Any]: ...

    def prepare_manual_handover(
        self,
        program_run_id: int,
        request: ProgramManualHandoverRequest,
    ) -> dict[str, Any]: ...

    def resume_halt(
        self,
        program_run_id: int,
        request: ProgramHaltResumeRequest,
    ) -> dict[str, Any]: ...

    def reset_cycle(self, request: ProgramCycleResetRequest) -> dict[str, Any]: ...

    def cycle_status(self) -> dict[str, Any]: ...

    def read_runtime_controls(self, program_run_id: int) -> dict[str, Any]: ...

    def read_runtime_blackboard(self, program_run_id: int) -> dict[str, Any]: ...

    def write_runtime_blackboard(
        self, program_run_id: int, request: RuntimeBlackboardWriteRequest
    ) -> dict[str, Any]: ...

    def stage_manual_result(
        self, program_run_id: int, request: ManualResultStageRequest
    ) -> dict[str, Any]: ...

    def read_persistent_values(self) -> dict[str, Any]: ...

    def set_persistent_value(
        self, request: PersistentKeyValueSetRequest
    ) -> dict[str, Any]: ...

    def reset_persistent_value(
        self, request: PersistentKeyValueResetRequest
    ) -> dict[str, Any]: ...

    def clear_persistent_values(
        self, request: PersistentKeyValuesClearRequest
    ) -> dict[str, Any]: ...

    def reconcile_persistent_values(self) -> dict[str, Any]: ...

    def run_command(
        self, request_key: str | None, command: Callable[[], Any]
    ) -> dict[str, Any]: ...

    def apply_recovery_action(
        self, request: CycleRecoveryActionRequest
    ) -> dict[str, Any]: ...


# ---------------------------------------------------------------------------
# Request decoding: each command's members, checked as the former routes did.
# ---------------------------------------------------------------------------


def _members(request: Mapping[str, Any], names: set[str]) -> None:
    if set(request) != names:
        raise _MalformedRequest()


def _program_run_id(request: Mapping[str, Any]) -> int:
    value = request.get("program_run_id")
    if type(value) is not int or not 1 <= value <= _MAXIMUM_PROGRAM_RUN_ID:
        raise _MalformedRequest()
    return value


def _body(model: type[BaseModel]) -> Callable[[Mapping[str, Any]], Any]:
    def decode(request: Mapping[str, Any]) -> Any:
        return model.model_validate(request)

    return decode


def _run_body(
    model: type[BaseModel], mismatch: str | None = None
) -> Callable[[Mapping[str, Any]], Any]:
    """``{program_run_id, body}`` of a run-scoped request; ``mismatch``: the
    body names its run too, and another one is refused with this text."""

    def decode(request: Mapping[str, Any]) -> Any:
        _members(request, {"program_run_id", "body"})
        run_id = _program_run_id(request)
        body = model.model_validate(request["body"])
        if mismatch is not None and body.program_run_id != run_id:
            raise ProgramRunnerError("program_run_path_invalid", mismatch)
        return run_id, body

    return decode


def _run_only(request: Mapping[str, Any]) -> int:
    _members(request, {"program_run_id"})
    return _program_run_id(request)


def _nothing(request: Mapping[str, Any]) -> None:
    _members(request, set())


def _runs_query(request: Mapping[str, Any]) -> tuple[bool, int]:
    if not set(request) <= {"active_only", "limit"}:
        raise _MalformedRequest()
    active_only = request.get("active_only", False)
    limit = request.get("limit", 128)
    if type(active_only) is not bool or type(limit) is not int or not 1 <= limit <= 4_096:
        raise _MalformedRequest()
    return active_only, limit


@dataclass(frozen=True, slots=True)
class _Route:
    """One command: how its request decodes and what answers it.

    ``kind``: ``read`` (the read pool), ``lane`` (the ordinary lane, answered
    after the work) or ``command`` (the ordinary lane, answered at the
    command's acceptance through ``runtime.run_command``).
    """

    kind: str
    decode: Callable[[Mapping[str, Any]], Any]
    call: Callable[[Any, Any], Any]


_PATH_MISMATCH = "Program-run path and body must match"


def _routes() -> dict[str, _Route]:
    def scoped(method: str):
        def call(runtime: Any, decoded: Any) -> Any:
            run_id, body = decoded
            return body, lambda: getattr(runtime, method)(run_id, body)

        return call

    def unscoped(method: str):
        def call(runtime: Any, body: Any) -> Any:
            return body, lambda: getattr(runtime, method)(body)

        return call

    return {
        "health": _Route("read", _nothing, lambda runtime, _: runtime.health()),
        "diagnostic_health": _Route(
            "read", _nothing, lambda runtime, _: runtime.diagnostic_health()
        ),
        "node_availability": _Route(
            "read",
            _body(ProgramNodeAvailabilityRequest),
            lambda runtime, body: runtime.node_availability(body),
        ),
        "runs": _Route(
            "read",
            _runs_query,
            lambda runtime, query: runtime.runs(active_only=query[0], limit=query[1]),
        ),
        "start_run": _Route(
            "command", _body(ProgramRunStartRequest), unscoped("start_run")
        ),
        "prepare_applied_scene": _Route(
            "lane",
            _body(ProgramScenePreparationRequest),
            lambda runtime, body: runtime.prepare_applied_scene(body),
        ),
        "read_run": _Route(
            "read", _run_only, lambda runtime, run_id: runtime.read_run(run_id)
        ),
        # Effect-free preview intent of one held motion node; not a control.
        "preview_node_correction": _Route(
            "read",
            _run_body(ProgramNodeCorrectionPreviewRequest),
            lambda runtime, decoded: runtime.preview_node_correction(*decoded),
        ),
        "resume_motion": _Route(
            "command",
            _run_body(ProgramSelectionResumeRequest, _PATH_MISMATCH),
            scoped("resume_motion"),
        ),
        "start_motion": _Route(
            "command",
            _run_body(ProgramSelectionStartRequest, _PATH_MISMATCH),
            scoped("start_motion"),
        ),
        "prepare_manual_handover": _Route(
            "command",
            _run_body(ProgramManualHandoverRequest, _PATH_MISMATCH),
            scoped("prepare_manual_handover"),
        ),
        "resume_halt": _Route(
            "command",
            _run_body(
                ProgramHaltResumeRequest,
                "Program-run identity in path and body must match",
            ),
            scoped("resume_halt"),
        ),
        "read_runtime_controls": _Route(
            "read",
            _run_only,
            lambda runtime, run_id: runtime.read_runtime_controls(run_id),
        ),
        "read_runtime_blackboard": _Route(
            "read",
            _run_only,
            lambda runtime, run_id: runtime.read_runtime_blackboard(run_id),
        ),
        "write_runtime_blackboard": _Route(
            "command",
            _run_body(RuntimeBlackboardWriteRequest),
            scoped("write_runtime_blackboard"),
        ),
        "stage_manual_result": _Route(
            "command",
            _run_body(ManualResultStageRequest),
            scoped("stage_manual_result"),
        ),
        # Values of the Applied Scenario project; an edit names the project
        # folder the operator saw (`project_path`) and another one is refused.
        "read_persistent_values": _Route(
            "read", _nothing, lambda runtime, _: runtime.read_persistent_values()
        ),
        "set_persistent_value": _Route(
            "command",
            _body(PersistentKeyValueSetRequest),
            unscoped("set_persistent_value"),
        ),
        "reset_persistent_value": _Route(
            "command",
            _body(PersistentKeyValueResetRequest),
            unscoped("reset_persistent_value"),
        ),
        "clear_persistent_values": _Route(
            "command",
            _body(PersistentKeyValuesClearRequest),
            unscoped("clear_persistent_values"),
        ),
        # Headless observed a committed Apply; the runner reads it itself.
        "reconcile_persistent_values": _Route(
            "lane",
            _nothing,
            lambda runtime, _: runtime.reconcile_persistent_values(),
        ),
        "apply_recovery_action": _Route(
            "command",
            _body(CycleRecoveryActionRequest),
            unscoped("apply_recovery_action"),
        ),
        "reset_cycle": _Route(
            "command", _body(ProgramCycleResetRequest), unscoped("reset_cycle")
        ),
        "cycle_status": _Route(
            "read", _nothing, lambda runtime, _: runtime.cycle_status()
        ),
    }


PROGRAM_RUNNER_COMMANDS = frozenset(_routes())


def _error_reply(code: str, message: str, details: Mapping[str, Any], producer: str):
    return {
        "ok": False,
        "error": {
            "code": code,
            "details": dict(details),
            "message": message,
            "producer": producer or "program_runner",
        },
    }


def _reply_for(error: Exception) -> dict[str, Any]:
    if isinstance(error, ProgramRunnerError):
        return _error_reply(error.code, str(error), error.details, error.producer)
    if isinstance(error, (ValidationError, _MalformedRequest)):
        return _error_reply(
            _MALFORMED_REQUEST_CODE, _MALFORMED_REQUEST_MESSAGE, {}, "program_runner"
        )
    # An unexpected failure: Headless reports the runner's answer invalid,
    # as it did for the HTTP surface's plain-text 500.
    _LOGGER.error("Program-runner command failed", exc_info=error)
    return _error_reply(
        "program_runner_response_invalid",
        "Program runner returned an invalid response",
        {},
        "program_runner",
    )


# ---------------------------------------------------------------------------
# Status publication.
# ---------------------------------------------------------------------------


class _ProgramStatusPump:
    """Publishes the Program control state on each read request (see module)."""

    def __init__(self, runtime: Any, publisher: Any, *, clock_ns=time.monotonic_ns) -> None:
        self._runtime = runtime
        self._publisher = publisher
        self._clock_ns = clock_ns
        # One bound callable: the runtime's observer set removes the same one.
        self._wake = publisher.request_read
        self._stop = False
        self._thread = Thread(
            target=self._run, name="manyforge_program_runner_status", daemon=True
        )

    def start(self) -> None:
        self._runtime.add_control_state_observer(self._wake)
        self._wake()
        self._thread.start()

    def close(self) -> None:
        self._runtime.remove_control_state_observer(self._wake)
        self._stop = True
        self._wake()
        if self._thread.is_alive():
            self._thread.join()

    def _snapshot(self) -> tuple[dict[str, Any], int]:
        """The cycle for classification; it lacks the Reset-position read."""

        return self._runtime.cycle_projection(), self._clock_ns()

    def _run(self) -> None:
        previous: dict[str, Any] | None = None
        last_publication_ns = 0
        while True:
            # False only once the publisher is closed.
            if not self._publisher.wait_read_request() or self._stop:
                return
            try:
                cycle, observed_ns = self._snapshot()
                event_class = program_cycle_event_class(previous, cycle)
                remaining_ns = _NODE_PROGRESS_PERIOD_NS - (
                    observed_ns - last_publication_ns
                )
                if event_class == "node_progress" and remaining_ns > 0:
                    deadline_ns = observed_ns + remaining_ns
                    node_only = True
                    while node_only:
                        timeout = (deadline_ns - self._clock_ns()) / 1_000_000_000
                        if timeout <= 0 or not self._publisher.wait_read_request(
                            timeout
                        ):
                            break
                        if self._stop:
                            return
                        cycle, observed_ns = self._snapshot()
                        node_only = (
                            program_cycle_event_class(previous, cycle)
                            == "node_progress"
                        )
                    cycle, observed_ns = self._snapshot()
                    event_class = program_cycle_event_class(previous, cycle)
                cycle = self._runtime.add_reset_robot_position(cycle)
                self._publisher.publish(cycle, event_class == "critical")
            except Exception as error:
                # Subscribers learn that the state is not readable; the next
                # read request (a runtime wake, or a subscriber connecting
                # while withdrawn) reads it again. ``previous`` stays the last
                # published.
                if not isinstance(error, ProgramRunnerError):
                    _LOGGER.exception("Program control-state read failed")
                self._publisher.withdraw(
                    getattr(error, "code", "program_runner_response_invalid"),
                    str(error) or type(error).__name__,
                )
                continue
            previous = cycle
            last_publication_ns = observed_ns


# ---------------------------------------------------------------------------
# The application.
# ---------------------------------------------------------------------------


class ProgramRunnerApplication:
    """Serves the runtime on the core's Program command and status sockets."""

    def __init__(self, runtime: ProgramRunnerApplicationPort, socket_root: Path) -> None:
        if not isinstance(runtime, ProgramRunnerApplicationPort):
            raise TypeError("Program-runner app requires one application port")
        import manyforge_headless_py as core  # type: ignore[import-not-found]

        self._gateway_error: type[Exception] = core.GatewayError
        self._runtime = runtime
        self._routes = _routes()
        self._commands = core.ProgramCommandServer(str(socket_root))
        observable = callable(getattr(runtime, "add_control_state_observer", None))
        self._status = core.ProgramStatusPublisher(str(socket_root)) if observable else None
        self._pump = (
            _ProgramStatusPump(runtime, self._status) if self._status is not None else None
        )
        self._lane_capacity = BoundedSemaphore(_LANE_WORKERS)
        self._lane = ThreadPoolExecutor(
            max_workers=_LANE_WORKERS,
            thread_name_prefix="manyforge_program_runner_control_ordinary",
        )
        self._reads = ThreadPoolExecutor(
            max_workers=_READ_WORKERS, thread_name_prefix="manyforge_program_runner_read"
        )
        self._intake = Thread(
            target=self._serve_commands,
            name="manyforge_program_runner_commands",
            daemon=True,
        )
        self._stopping = False
        self._started = False

    def start(self) -> None:
        """Start the runtime, then serve (the sockets appear after the start)."""

        self._runtime.start()
        self._started = True
        # Started and not yet serving (OD-234): no run exists, so the pause
        # of the full collection falls before any command.
        settle_heap("start")
        try:
            if self._status is not None and self._pump is not None:
                self._status.start()
                self._pump.start()
            self._commands.start()
        except self._gateway_error as error:
            raise ProgramRunnerError(
                error.code, error.message, details=error.details, producer=error.producer
            ) from error
        self._intake.start()

    def close(self) -> None:
        """Stop serving, let started work end, then shut the runtime down."""

        self._stopping = True
        self._commands.close()
        if self._intake.is_alive():
            self._intake.join()
        try:
            self._lane.shutdown(wait=True)
            self._reads.shutdown(wait=True)
            if self._pump is not None:
                self._pump.close()
            if self._started:
                self._runtime.shutdown()
        finally:
            if self._status is not None:
                self._status.close()

    def _serve_commands(self) -> None:
        while not self._stopping:
            received = self._commands.next(3600.0)
            if received is None:
                continue
            token, envelope = received
            self._dispatch(token, envelope["command"], envelope["request"])

    def _reply(self, token: int, reply: dict[str, Any]) -> None:
        try:
            self._commands.reply(token, reply)
        except (TypeError, ValueError) as error:
            # The value has no JSON form: answer the failure instead.
            self._commands.reply(token, _reply_for(error))

    def _dispatch(self, token: int, command: str, request: dict[str, Any]) -> None:
        route = self._routes.get(command)
        if route is None:
            self._reply(token, _reply_for(_MalformedRequest()))
            return
        if route.kind == "read":
            self._reads.submit(self._answer_read, token, route, request)
            return
        try:
            decoded = route.decode(request)
        except Exception as error:
            self._reply(token, _reply_for(error))
            return
        if not self._lane_capacity.acquire(blocking=False):
            busy = ExecutionControlBusyError(ExecutionControlLane.ORDINARY, _LANE_WORKERS)
            self._reply(
                token,
                _reply_for(
                    ProgramRunnerError(
                        "execution_control_ingress_busy",
                        str(busy),
                        details={
                            "lane": busy.lane.value,
                            "maximum_admitted": busy.maximum_admitted,
                        },
                    )
                ),
            )
            return
        try:
            self._lane.submit(self._answer_lane, token, route, decoded)
        except BaseException:
            self._lane_capacity.release()
            raise

    def _answer_read(self, token: int, route: _Route, request: dict[str, Any]) -> None:
        try:
            reply = {"ok": True, "value": route.call(self._runtime, route.decode(request))}
        except Exception as error:
            reply = _reply_for(error)
        self._reply(token, reply)

    def _answer_lane(self, token: int, route: _Route, decoded: Any) -> None:
        try:
            try:
                if route.kind == "command":
                    body, command = route.call(self._runtime, decoded)
                    # OD-214: answered once the command is accepted (or ended
                    # at once); the command runs on to its end under the
                    # runner's slot.
                    value = self._runtime.run_command(body.request_key, command)
                else:
                    value = route.call(self._runtime, decoded)
                reply = {"ok": True, "value": value}
            except Exception as error:
                reply = _reply_for(error)
            self._reply(token, reply)
        finally:
            self._lane_capacity.release()


__all__ = [
    "PROGRAM_RUNNER_COMMANDS",
    "ProgramRunnerApplication",
    "ProgramRunnerApplicationPort",
]
