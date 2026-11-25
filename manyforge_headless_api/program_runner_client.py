"""Headless's link to the independently restartable Program runner.

A thin adapter over the core's ``ProgramRunnerLink`` (``program-command.sock``
and ``program-status.sock`` in the runner's socket root): one command per
call, answered at the command's acceptance (OD-214), and the pushed Program
control state. All Program-run meaning remains in the runner.

A command, and the cycle status read that decides whether a command is
forwarded, wait for the runner's answer with only the connect limited; every
other read ends within ``timeout_seconds`` as a whole
(``program_runner_unavailable``, ``transport_error`` ``read_timeout``).
"""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from pathlib import Path
from typing import Any, Protocol

from pydantic import ValidationError

from .program_cycle import ProgramCommandReplyDTO


class ProgramRunnerClientError(RuntimeError):
    """The configured Program runner is unavailable or rejected a request."""

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


_PERSISTENT_VALUE_ACTION_SUFFIXES = {"set": "", "reset": "/reset", "clear": "/clear"}


def persistent_values_path(action: str | None = None) -> str:
    """Headless's path of the Applied Scenario's persistent blackboard values,
    or of one edit.

    Values belong to the Applied Scenario project; the Program runner resolves
    it, and an edit names the project folder the operator saw in its body.
    """

    if action is not None and action not in _PERSISTENT_VALUE_ACTION_SUFFIXES:
        raise ValueError("persistent-value action is unsupported")
    suffix = "" if action is None else _PERSISTENT_VALUE_ACTION_SUFFIXES[action]
    return f"/api/program/persistent-values{suffix}"


class ProgramStatusSubscriptionPort(Protocol):
    def next(self, timeout_s: float) -> dict[str, Any] | None: ...

    def close(self) -> None: ...


class ProgramRunnerPort(Protocol):
    def subscribe_status(self) -> ProgramStatusSubscriptionPort: ...

    def node_availability(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]: ...

    def diagnostic_health(self) -> dict[str, Any]: ...

    def start_run(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def prepare_applied_scene(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def read_run(self, program_run_id: int) -> dict[str, Any]: ...

    def preview_node_correction(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def start_motion(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def resume_halt(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def resume_motion(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def prepare_manual_handover(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def cycle_status(self, *, before_command: bool = False) -> dict[str, Any]: ...

    def reset_cycle(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def runtime_controls(self, program_run_id: int) -> dict[str, Any]: ...

    def runtime_blackboard_values(self, program_run_id: int) -> dict[str, Any]: ...

    def runtime_control_action(
        self, program_run_id: int, suffix: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def apply_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def persistent_values(self) -> dict[str, Any]: ...

    def persistent_value_action(
        self, action: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def reconcile_persistent_values(self) -> dict[str, Any]: ...

    def close(self) -> None: ...


def _client_error(error: Any) -> ProgramRunnerClientError:
    """The core's GatewayError as the client error."""

    return ProgramRunnerClientError(
        error.code,
        error.message,
        details=error.details,
        producer=error.producer or "program_runner",
    )


class _StatusSubscription:
    """One pushed Program control-state subscription; errors are typed."""

    def __init__(self, subscription: Any, gateway_error: type[Exception]) -> None:
        self._subscription = subscription
        self._gateway_error = gateway_error

    def next(self, timeout_s: float) -> dict[str, Any] | None:
        """The next frame ``{event_class, program_cycle, schema_version,
        server_sequence}``, or None after ``timeout_s``."""

        try:
            return self._subscription.next(timeout_s)
        except self._gateway_error as error:
            raise _client_error(error) from error

    def close(self) -> None:
        self._subscription.close()


_RUN_ACTIONS = {
    "blackboard-values": "write_runtime_blackboard",
    "manual-result": "stage_manual_result",
}
_PERSISTENT_VALUE_ACTIONS = {
    "set": "set_persistent_value",
    "reset": "reset_persistent_value",
    "clear": "clear_persistent_values",
}


class ProgramRunnerClient:
    """Transport only; all Program-run meaning remains in the remote owner."""

    def __init__(
        self,
        socket_root: Path | str,
        *,
        timeout_seconds: float = 30.0,
        link: Any | None = None,
    ) -> None:
        if not 0 < timeout_seconds <= 60:
            raise ValueError("Program-runner timeout must be in (0, 60] seconds")
        import manyforge_headless_py as core  # type: ignore[import-not-found]

        self._gateway_error: type[Exception] = core.GatewayError
        # A command answers at its acceptance and publishes its end in its
        # row (OD-214): only the connect is limited, never the wait for the
        # answer (the Manager's liveness probe detects a lost runner). So is
        # the cycle status read before a command is forwarded; other reads
        # keep the whole limit.
        self._read_limit_seconds = timeout_seconds
        if link is None:
            try:
                link = core.ProgramRunnerLink(str(socket_root), timeout_seconds)
            except core.GatewayError as error:
                raise _client_error(error) from error
        self._link = link

    def subscribe_status(self) -> _StatusSubscription:
        """The pushed Program control state (``program-status.sock``)."""

        try:
            subscription = self._link.subscribe_status()
        except self._gateway_error as error:
            raise _client_error(error) from error
        return _StatusSubscription(subscription, self._gateway_error)

    def _call(
        self,
        command: str,
        request: Mapping[str, Any],
        *,
        command_reply: bool = False,
        wait_for_answer: bool = False,
    ) -> dict[str, Any]:
        limit_s = None if command_reply or wait_for_answer else self._read_limit_seconds
        try:
            reply = self._link.call(command, dict(request), limit_s=limit_s)
        except self._gateway_error as error:
            raise _client_error(error) from error
        if not reply["ok"]:
            error = reply["error"]
            raise ProgramRunnerClientError(
                error["code"],
                error["message"] or "Program runner refused the request",
                details=error["details"],
                producer=error["producer"],
            )
        value = reply["value"]
        if not isinstance(value, dict):
            raise ProgramRunnerClientError(
                "program_runner_response_invalid",
                "Program runner returned an invalid response",
            )
        if command_reply:
            try:
                ProgramCommandReplyDTO.model_validate(value)
            except ValidationError as exc:
                raise ProgramRunnerClientError(
                    "program_runner_response_invalid",
                    "Program runner returned an invalid command reply",
                ) from exc
        return value

    def _command(self, command: str, body: Mapping[str, Any]) -> dict[str, Any]:
        """One runner command: its ``program_command_reply.v1`` at acceptance."""

        return self._call(command, body, command_reply=True)

    @staticmethod
    def _run_id(program_run_id: int) -> int:
        if type(program_run_id) is not int or not 1 <= program_run_id <= 9_007_199_254_740_991:
            raise ValueError("program_run_id is invalid")
        return program_run_id

    def _run_command(
        self, command: str, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._command(
            command, {"program_run_id": self._run_id(program_run_id), "body": dict(body)}
        )

    def health(self) -> dict[str, Any]:
        return self._call("health", {})

    def runs(
        self,
        *,
        active_only: bool = False,
        limit: int = 128,
    ) -> dict[str, Any]:
        if type(active_only) is not bool:
            raise ValueError("active_only must be boolean")
        if type(limit) is not int or not 1 <= limit <= 4_096:
            raise ValueError("Program-run index limit is outside its bound")
        return self._call("runs", {"active_only": active_only, "limit": limit})

    def node_availability(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._call("node_availability", body)

    def diagnostic_health(self) -> dict[str, Any]:
        return self._call("diagnostic_health", {})

    def start_run(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._command("start_run", body)

    def prepare_applied_scene(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._call("prepare_applied_scene", body)

    def read_run(self, program_run_id: int) -> dict[str, Any]:
        return self._call("read_run", {"program_run_id": self._run_id(program_run_id)})

    def preview_node_correction(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._call(
            "preview_node_correction",
            {"program_run_id": self._run_id(program_run_id), "body": dict(body)},
        )

    def start_motion(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._run_command("start_motion", program_run_id, body)

    def resume_motion(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._run_command("resume_motion", program_run_id, body)

    def prepare_manual_handover(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._run_command("prepare_manual_handover", program_run_id, body)

    def resume_halt(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._run_command("resume_halt", program_run_id, body)

    def cycle_status(self, *, before_command: bool = False) -> dict[str, Any]:
        """The cycle status; ``before_command``: the read that decides whether
        a command is forwarded, which waits for the runner's answer."""

        return self._call("cycle_status", {}, wait_for_answer=before_command)

    def reset_cycle(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._command("reset_cycle", body)

    def runtime_controls(self, program_run_id: int) -> dict[str, Any]:
        return self._call(
            "read_runtime_controls", {"program_run_id": self._run_id(program_run_id)}
        )

    def runtime_blackboard_values(self, program_run_id: int) -> dict[str, Any]:
        return self._call(
            "read_runtime_blackboard", {"program_run_id": self._run_id(program_run_id)}
        )

    def runtime_control_action(
        self,
        program_run_id: int,
        suffix: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        if suffix not in _RUN_ACTIONS:
            raise ValueError("runtime-control action is unsupported")
        return self._run_command(_RUN_ACTIONS[suffix], program_run_id, body)

    def apply_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._command("apply_recovery_action", body)

    def persistent_values(self) -> dict[str, Any]:
        return self._call("read_persistent_values", {})

    def persistent_value_action(
        self, action: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        if action not in _PERSISTENT_VALUE_ACTIONS:
            raise ValueError("persistent-value action is unsupported")
        return self._command(_PERSISTENT_VALUE_ACTIONS[action], body)

    def reconcile_persistent_values(self) -> dict[str, Any]:
        return self._call("reconcile_persistent_values", {})

    def close(self) -> None:
        """Nothing to release: each call opens and closes its own connection."""

    def __enter__(self) -> "ProgramRunnerClient":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


__all__ = [
    "persistent_values_path",
    "ProgramRunnerClient",
    "ProgramRunnerClientError",
    "ProgramRunnerPort",
    "ProgramStatusSubscriptionPort",
]
