"""Fake execution gateways for unit tests.

``KernelClient`` talks to the core's ``manyforge_headless_py.Gateway``: one
method per host command, named as the wire command, taking the request
members and returning the host's reply. ``TransportGateway`` serves those
calls from a test double written per Kernel call (``submit(operation)``,
``read(operation_id)``, ``release_halt(run, revision)``, ...): it unpacks the
members the gateway would send, calls the double's method of that name and
returns its value as a reply. A command whose double method is missing is
missing on the gateway too. An exception the double raises with an owner's
code becomes that refusal reply (``ok: false``), as the host answers it; a
transport code, or any other exception, propagates like the core's
``GatewayError``.
"""

from __future__ import annotations

from typing import Any, Callable, Mapping

from manyforge_behavior.kernel_client import EXECUTION_TRANSPORT_FAILURE_CODES


def _request(members: Mapping[str, Any]) -> tuple[Any, ...]:
    return (members["request"],)


# command -> (double method, members -> its positional arguments);
# read_active_tcp, set_io_output and release_io_watch are unpacked below.
_CALLS: dict[str, tuple[str, Callable[[Mapping[str, Any]], tuple[Any, ...]]]] = {
    "identity": ("identity", lambda _m: ()),
    "targets": ("targets", lambda _m: ()),
    "control_state": ("control_state", lambda _m: ()),
    "observation": ("observation", lambda _m: ()),
    "reset_active_tcp": ("reset_active_tcp", lambda _m: ()),
    "reset_rule_activation": ("reset_rule_activation", lambda _m: ()),
    "reset_robot_position_capability": ("reset_robot_position_capability", lambda _m: ()),
    "reset_robot_position": ("reset_robot_position", lambda m: (dict(m),)),
    "submit": ("submit", lambda m: (m["operation"],)),
    "read_manual_handover": ("read_manual_handover", lambda m: (m["operation"],)),
    "plan_pick": ("plan_pick", lambda m: (m["intent"],)),
    "plan_place": ("plan_place", lambda m: (m["intent"],)),
    "read_planning_job": ("read_planning_job", lambda m: (m["job_id"],)),
    "cancel_planning_job": ("cancel_planning_job", lambda m: (m["job_id"],)),
    "release_planning_job": ("release_planning_job", lambda m: (m["job_id"],)),
    "read": ("read", lambda m: (m["operation_id"],)),
    "get_operation_recovery": ("operation_recovery", lambda m: (m["operation_id"],)),
    "apply_operation_recovery": (
        "apply_operation_recovery",
        lambda m: (m["operation_id"], m["expected_operation_revision"], m["request"]),
    ),
    "reset": ("reset", lambda m: (m["request"]["operation_id"], m["request"])),
    "acknowledge_completed": (
        "acknowledge_completed", lambda m: (m["request"]["operation_id"], m["request"]),
    ),
    "stop": ("stop", _request),
    "stop_program_run": ("stop_program_run", _request),
    "cell_stop": ("cell_stop", _request),
    "stop_program_selection": ("stop_program_selection", _request),
    "check_program_node_motion_attention": ("check_program_node_motion_attention", _request),
    "release_program_selection": (
        "release_program_selection", lambda m: (m["request"], m["expected_hold_revision"]),
    ),
    "start_program_selection": (
        "start_program_selection", lambda m: (m["request"], m["expected_hold_revision"]),
    ),
    "release_halt": (
        "release_halt", lambda m: (m["program_run_id"], m["expected_hold_revision"]),
    ),
    "read_stop": ("read_stop", lambda m: (m["event_id"],)),
    "set_rule_active": ("set_rule_active", lambda m: (m["rule_id"], m["active"])),
    "clear_outside_change": (
        "clear_outside_change", lambda m: (m["controllable_component_id"], m["signal_name"]),
    ),
    "query_target": ("query_target", lambda m: (m["query"],)),
    "query_operation": ("query_operation", lambda m: (m["query"],)),
    "register_io_watch": ("register_io_watch", _request),
    "read_io_watch": ("read_io_watch", lambda m: (m["registration"],)),
    "start_mock_io_node_simulation": ("start_mock_io_node_simulation", _request),
    "end_mock_io_node_simulation": ("end_mock_io_node_simulation", _request),
    "set_mock_io_run_paused": ("set_mock_io_run_paused", _request),
    "publish_program_bbkeys": ("publish_program_bbkeys", _request),
    "register_program_run_capture": ("register_program_run_capture", lambda m: (m["capture"],)),
    "read_program_execution_scope": (
        "read_program_execution_scope", lambda m: (m["program_run_id"],),
    ),
    "resolve_attach_active_tcp": (
        "resolve_attach_active_tcp", lambda m: (m["program_binding_id"], m["target"]),
    ),
    "events": ("read_interrupt_rule_state_events", lambda m: (m["after_sequence"],)),
}


def _reply(command: str, value: Any) -> Any:
    """A double's value as the host's reply."""

    if value is None:
        return {"ok": True, "command": command}
    if command == "read_io_watch" and isinstance(value, dict) and "state" in value:
        return {"ok": True, "command": command, "result": value}
    if command == "resolve_attach_active_tcp" and isinstance(value, dict) and "ok" not in value:
        return {"ok": True, "command": command, "frame": value}
    if isinstance(value, dict) and "ok" not in value:
        return {"ok": True, "command": command, **value}
    return value


class TransportGateway:
    """A gateway over a per-call test double (see the module text)."""

    def __init__(self, double: Any) -> None:
        self.double = double
        self.sent: list[tuple[str, dict[str, Any]]] = []

    def __getattr__(self, command: str) -> Callable[..., Any]:
        if command == "read_active_tcp":
            method = getattr(self.double, "read_active_tcp")

            def read(members: Mapping[str, Any] = {}, principal_id: str | None = None) -> Any:
                del principal_id
                return self._call(
                    command, members, lambda: method(ordered=True) if members.get("ordered") else method()
                )

            return read
        if command == "set_io_output":
            method = getattr(self.double, "set_io_output")

            def write(members: Mapping[str, Any] = {}, principal_id: str | None = None) -> Any:
                del principal_id
                request = members["request"]
                return self._call(
                    command,
                    members,
                    lambda: method(request["signal_id"], request["value"], origin=request["origin"]),
                )

            return write
        if command == "release_io_watch":
            release = getattr(self.double, "release_io_watch", None)
            scope = getattr(self.double, "release_io_watch_scope", None)
            if release is None and scope is None:
                raise AttributeError(command)

            def release_watch(members: Mapping[str, Any] = {}, principal_id: str | None = None) -> Any:
                del principal_id
                if "scope_request" in members:
                    return self._call(command, members, lambda: scope(members["scope_request"]))
                return self._call(command, members, lambda: release(members["registration"]))

            return release_watch
        if command not in _CALLS:
            raise AttributeError(command)
        name, arguments = _CALLS[command]
        method = getattr(self.double, name)

        def call(members: Mapping[str, Any] = {}, principal_id: str | None = None) -> Any:
            del principal_id
            return self._call(command, members, lambda: method(*arguments(members)))

        return call

    def _call(self, command: str, members: Mapping[str, Any], call: Callable[[], Any]) -> Any:
        self.sent.append((command, dict(members)))
        try:
            value = call()
        except Exception as error:
            code = getattr(error, "code", None)
            if not isinstance(code, str) or not code or code in EXECUTION_TRANSPORT_FAILURE_CODES:
                raise
            reply: dict[str, Any] = {
                "ok": False,
                "command": command,
                "error": {
                    "code": code,
                    "message": str(error) or code,
                    "details": dict(getattr(error, "details", None) or {}),
                },
            }
            producer = getattr(error, "producer", None)
            if isinstance(producer, str) and producer:
                reply["error"]["producer"] = producer
            active_tcp = getattr(error, "active_tcp", None)
            if active_tcp is not None:
                reply["active_tcp"] = active_tcp
            return reply
        return _reply(command, value)


def transport_gateway(double: Any) -> TransportGateway:
    return TransportGateway(double)
