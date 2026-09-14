"""Applied-Program adapter for the sole CCLC lifecycle and route owner."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from hashlib import sha256
from threading import RLock
from types import MappingProxyType
from typing import Any, Protocol
import unicodedata

from .orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerInvocationHeldError,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)


_MAX_IDENTITIES = 4_096
_MAX_IDENTITY_BYTES = 256
_LIFECYCLE_INTENTS = MappingProxyType(
    {
        "lifecycle.prepare_component": "prepare",
        "lifecycle.enable_component": "enable",
        "lifecycle.disable_component": "disable",
        "lifecycle.recover_component": "recover",
    }
)


class CCLCProgramClient(Protocol):
    """Thin Headless client surface consumed by the Program owner."""

    def cclc_owner_identity(self) -> dict[str, Any]: ...

    def authoring_admission(self) -> dict[str, Any]: ...

    def component_lifecycle_request_result(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def cclc_component(self, component_id: str) -> dict[str, Any]: ...

    def cclc_route_templates(self, component_id: str) -> dict[str, Any]: ...

    def submit_component_lifecycle(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def component_lifecycle_status(self, transaction_id: str) -> dict[str, Any]: ...

    def submit_route_selection(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def route_selection_status(self, transaction_id: str) -> dict[str, Any]: ...

    def route_selection_request_result(self, body: Mapping[str, Any]) -> dict[str, Any]: ...


class CCLCProgramPortError(RuntimeError):
    """One Program/Binding/CCLC join is absent or contradictory."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class _Target:
    component_id: str
    control_binding_ids: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class _BoundContext:
    program_run_id: str
    program_id: str
    retained_bundle_id: str
    graph_revision: str
    components: Mapping[str, str]
    targets: Mapping[str, _Target]


@dataclass(slots=True)
class _Task:
    program_run_id: str
    kind: str
    transaction_id: str | None
    snapshot: OwnerTaskSnapshot
    request: Mapping[str, Any] | None = None
    start_args: tuple[str, NodeAttemptIdentity, OwnerNodeInputs] | None = None


def _text(value: Any, *, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > _MAX_IDENTITY_BYTES
    ):
        raise CCLCProgramPortError(
            "cclc_program_binding_invalid",
            f"{field} must be bounded NFC text",
        )
    return value


def _mapping(value: Any, *, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise CCLCProgramPortError(
            "cclc_projection_invalid", f"{field} must be an object"
        )
    if any(not isinstance(key, str) or not key for key in value):
        raise CCLCProgramPortError(
            "cclc_projection_invalid", f"{field} has an invalid member name"
        )
    return value


def _sequence(value: Any, *, field: str, nonempty: bool = True) -> Sequence[Any]:
    if (
        not isinstance(value, (list, tuple))
        or (nonempty and not value)
        or len(value) > _MAX_IDENTITIES
    ):
        raise CCLCProgramPortError(
            "cclc_projection_invalid", f"{field} violates its array bound"
        )
    return value


def _positive_integer(value: Any, *, field: str) -> int:
    if type(value) is not int or not 1 <= value <= 9_007_199_254_740_991:
        raise CCLCProgramPortError(
            "cclc_projection_invalid", f"{field} is not a safe positive integer"
        )
    return value


def _thaw(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _thaw(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_thaw(nested) for nested in value]
    return value


def _request_id(node_id: str, identity: NodeAttemptIdentity) -> str:
    digest = sha256(
        b"manyforge-cclc-program-request-v1\0"
        + node_id.encode("utf-8")
        + b"\0"
        + identity.program_run_id.encode("utf-8")
        + b"\0"
        + identity.node_attempt_id.encode("utf-8")
    ).hexdigest()
    return f"cclc-{digest}"


def _failure(task_id: str, code: str, message: str) -> OwnerTaskSnapshot:
    return OwnerTaskSnapshot(
        task_id=task_id,
        state=OwnerTaskState.FAILED,
        reason_code=code,
        message=message,
        settlement_proven=True,
    )


def _known_rejection(
    task_id: str, error: Mapping[str, Any]
) -> OwnerTaskSnapshot | None:
    details = error.get("details")
    if not isinstance(details, Mapping) or details.get("effect") != "not_started":
        return None
    code = error.get("code")
    message = error.get("message")
    if not isinstance(code, str) or not code or not isinstance(message, str):
        return None
    if (
        code == "cclc_request_admission_deferred"
        and details.get("reason") == "request_namespace_busy"
    ) or (
        code == "cclc_request_namespace_refreshed"
        and details.get("reason") == "settled_request_namespace_checkpoint"
    ):
        return OwnerTaskSnapshot(
            task_id,
            OwnerTaskState.ACTIVE,
            metadata={"admission_deferred": True},
            message=message,
        )
    revision = details.get("selection_revision")
    if (
        code == "invalid_state_transition"
        and details.get("reason") == "program_node_stop_fence_active"
    ):
        if (
            not isinstance(revision, str)
            or not revision.isascii()
            or not revision.isdecimal()
            or int(revision) > 9_007_199_254_740_991
        ):
            return None
        raise OwnerInvocationHeldError(int(revision))
    return _failure(task_id, code, message)


def _error_payload(error: Exception) -> dict[str, Any]:
    response = getattr(error, "response", None)
    failure = response.get("error") if isinstance(response, Mapping) else None
    return {
        "code": (
            failure.get("code")
            if isinstance(failure, Mapping)
            else getattr(error, "code", None)
        ),
        "details": (
            failure.get("details")
            if isinstance(failure, Mapping)
            else getattr(error, "details", None)
        ),
        "message": str(error),
    }


class CCLCProgramTaskPort:
    """One bounded Program-run context port and CCLC task adapter.

    The authored Program node is itself the explicit request.  The adapter
    therefore sets the Headless confirmation bit, but every semantic field is
    copied from an exact Program Binding plus a fresh CCLC-owned projection.
    """

    def __init__(
        self,
        client: CCLCProgramClient,
        *,
        maximum_retained_tasks: int = _MAX_IDENTITIES,
        program_node_evidence: (
            Callable[[NodeAttemptIdentity, str], Mapping[str, Any]] | None
        ) = None,
    ) -> None:
        if not all(
            callable(getattr(client, method, None))
            for method in (
                "cclc_owner_identity",
                "cclc_component",
                "cclc_route_templates",
                "submit_component_lifecycle",
                "component_lifecycle_status",
                "submit_route_selection",
                "route_selection_status",
            )
        ):
            raise TypeError("client must expose the complete CCLC Headless surface")
        self._client = client
        self._program_node_evidence = program_node_evidence
        if program_node_evidence is not None and not all(
            callable(getattr(client, name, None))
            for name in ("authoring_admission", "component_lifecycle_request_result", "route_selection_request_result")
        ):
            raise TypeError(
                "Runtime CCLC requires admission and exact request-result lookup"
            )
        if (
            type(maximum_retained_tasks) is not int
            or not 1 <= maximum_retained_tasks <= _MAX_IDENTITIES
        ):
            raise ValueError("maximum_retained_tasks must be in [1, 4096]")
        self._maximum_retained_tasks = maximum_retained_tasks
        self._contexts: dict[str, _BoundContext] = {}
        self._tasks: dict[str, _Task] = {}
        self._reserved_tasks: dict[str, str] = {}
        self._lock = RLock()

    @staticmethod
    def _parse_context(context: ProgramRunPortContext) -> _BoundContext:
        binding = context.program_binding
        component_rows = _sequence(
            binding.get("component_role_bindings"),
            field="component_role_bindings",
        )
        components: dict[str, str] = {}
        for index, raw_row in enumerate(component_rows):
            row = _mapping(raw_row, field=f"component_role_bindings[{index}]")
            role = _text(
                row.get("component_role_id"),
                field=f"component_role_bindings[{index}].component_role_id",
            )
            component = _text(
                row.get("controllable_component_id"),
                field=(
                    f"component_role_bindings[{index}]." "controllable_component_id"
                ),
            )
            if role in components:
                raise CCLCProgramPortError(
                    "cclc_program_binding_invalid",
                    "Program Binding repeats a component role",
                )
            components[role] = component

        target_rows = _sequence(
            binding.get("target_role_bindings", ()),
            field="target_role_bindings",
            nonempty=False,
        )
        targets: dict[str, _Target] = {}
        for index, raw_row in enumerate(target_rows):
            row = _mapping(raw_row, field=f"target_role_bindings[{index}]")
            target_role = _text(
                row.get("target_role_id"),
                field=f"target_role_bindings[{index}].target_role_id",
            )
            component_role = _text(
                row.get("component_role_id"),
                field=f"target_role_bindings[{index}].component_role_id",
            )
            component_id = _text(
                row.get("controllable_component_id"),
                field=(f"target_role_bindings[{index}]." "controllable_component_id"),
            )
            if components.get(component_role) != component_id:
                raise CCLCProgramPortError(
                    "cclc_program_binding_invalid",
                    "target and component-role bindings disagree",
                )
            binding_ids: set[str] = set()
            capabilities = _sequence(
                row.get("capability_bindings", ()),
                field=f"target_role_bindings[{index}].capability_bindings",
                nonempty=False,
            )
            for capability_index, raw_capability in enumerate(capabilities):
                capability = _mapping(
                    raw_capability,
                    field=(
                        f"target_role_bindings[{index}]."
                        f"capability_bindings[{capability_index}]"
                    ),
                )
                control = capability.get("control_binding")
                if control is None:
                    continue
                control_row = _mapping(
                    control,
                    field=(
                        f"target_role_bindings[{index}]."
                        f"capability_bindings[{capability_index}].control_binding"
                    ),
                )
                binding_ids.add(
                    _text(
                        control_row.get("control_binding_id"),
                        field="control_binding_id",
                    )
                )
            if target_role in targets:
                raise CCLCProgramPortError(
                    "cclc_program_binding_invalid",
                    "Program Binding repeats a target role",
                )
            targets[target_role] = _Target(component_id, tuple(sorted(binding_ids)))
        return _BoundContext(
            context.program_run_id,
            context.program_id,
            context.retained_bundle_id,
            context.graph_revision,
            MappingProxyType(components),
            MappingProxyType(targets),
        )

    def bind(self, context: ProgramRunPortContext) -> None:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("context must be a ProgramRunPortContext")
        parsed = self._parse_context(context)
        with self._lock:
            if context.program_run_id in self._contexts:
                raise CCLCProgramPortError(
                    "cclc_program_context_conflict",
                    "Program run is already bound to the CCLC adapter",
                )
            self._contexts[context.program_run_id] = parsed

    def unbind(self, program_run_id: str) -> None:
        program_run_id = _text(program_run_id, field="program_run_id")
        with self._lock:
            self._contexts.pop(program_run_id, None)
            self._tasks = {
                key: value
                for key, value in self._tasks.items()
                if value.program_run_id != program_run_id
            }
            self._reserved_tasks = {
                key: value
                for key, value in self._reserved_tasks.items()
                if value != program_run_id
            }

    def _context(self, program_run_id: str) -> _BoundContext:
        with self._lock:
            try:
                return self._contexts[program_run_id]
            except KeyError as exc:
                raise CCLCProgramPortError(
                    "cclc_program_context_missing",
                    "Program run is not bound to the CCLC adapter",
                ) from exc

    def _reserve(self, task_id: str, program_run_id: str) -> None:
        with self._lock:
            if task_id in self._tasks or task_id in self._reserved_tasks:
                raise CCLCProgramPortError(
                    "cclc_task_identity_conflict",
                    "CCLC task identity is already retained",
                )
            if (
                len(self._tasks) + len(self._reserved_tasks)
                >= self._maximum_retained_tasks
            ):
                raise CCLCProgramPortError(
                    "cclc_task_capacity_exhausted",
                    "CCLC Program task capacity is exhausted",
                )
            self._reserved_tasks[task_id] = program_run_id

    @staticmethod
    def _request_context(
        request_id: str, identity: NodeAttemptIdentity
    ) -> dict[str, Any]:
        correlation = identity.node_attempt_id
        if len(correlation.encode("utf-8")) > _MAX_IDENTITY_BYTES:
            correlation = f"sha256:{sha256(correlation.encode()).hexdigest()}"
        return {
            "request_id": request_id,
            "request_owner_epoch": "",
            "source_label": "manyforge_behavior.program",
            "correlation_id": correlation,
        }

    @staticmethod
    def _expected_domains(snapshot: Mapping[str, Any]) -> list[dict[str, Any]]:
        domains = _sequence(snapshot.get("domains"), field="domains")
        result: list[dict[str, Any]] = []
        previous: str | None = None
        for index, raw_domain in enumerate(domains):
            domain = _mapping(raw_domain, field=f"domains[{index}]")
            domain_id = _text(
                domain.get("control_domain_id"),
                field=f"domains[{index}].control_domain_id",
            )
            if previous is not None and domain_id <= previous:
                raise CCLCProgramPortError(
                    "cclc_projection_invalid",
                    "component domains are not canonical",
                )
            previous = domain_id
            bindings = _sequence(
                domain.get("current_control_bindings"),
                field=f"domains[{index}].current_control_bindings",
            )
            expected_bindings: list[dict[str, str]] = []
            previous_binding: str | None = None
            for binding_index, raw_binding in enumerate(bindings):
                binding = _mapping(
                    raw_binding,
                    field=(
                        f"domains[{index}]."
                        f"current_control_bindings[{binding_index}]"
                    ),
                )
                binding_id = _text(
                    binding.get("control_binding_id"),
                    field="control_binding_id",
                )
                if previous_binding is not None and binding_id <= previous_binding:
                    raise CCLCProgramPortError(
                        "cclc_projection_invalid",
                        "current control bindings are not canonical",
                    )
                previous_binding = binding_id
                expected_bindings.append(
                    {
                        "control_binding_id": binding_id,
                        "control_binding_revision": _text(
                            binding.get("control_binding_revision"),
                            field="control_binding_revision",
                        ),
                        "control_route_id": _text(
                            binding.get("control_route_id"),
                            field="control_route_id",
                        ),
                    }
                )
            active = domain.get("active_control_configuration")
            if active is None:
                expected_active: dict[str, Any] = {
                    "presence": "absent",
                    "active_control_configuration_id": None,
                    "active_control_configuration_generation": None,
                }
            else:
                active_row = _mapping(
                    active,
                    field=f"domains[{index}].active_control_configuration",
                )
                expected_active = {
                    "presence": "present",
                    "active_control_configuration_id": _text(
                        active_row.get("active_control_configuration_id"),
                        field="active_control_configuration_id",
                    ),
                    "active_control_configuration_generation": _positive_integer(
                        active_row.get("active_control_configuration_generation"),
                        field="active_control_configuration_generation",
                    ),
                }
            result.append(
                {
                    "control_domain_id": domain_id,
                    "control_domain_revision": _text(
                        domain.get("control_domain_revision"),
                        field="control_domain_revision",
                    ),
                    "controller_topology_id": _text(
                        domain.get("controller_topology_id"),
                        field="controller_topology_id",
                    ),
                    "topology_revision": _text(
                        domain.get("topology_revision"),
                        field="topology_revision",
                    ),
                    "expected_active_control_configuration": expected_active,
                    "expected_control_bindings": expected_bindings,
                }
            )
        return result

    def _start_lifecycle(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
        context: _BoundContext,
        request_id: str,
    ) -> tuple[str, dict[str, Any]]:
        component_role = _text(
            inputs.parameters.get("component_role"), field="component_role"
        )
        try:
            component_id = context.components[component_role]
        except KeyError as exc:
            raise CCLCProgramPortError(
                "cclc_component_role_unbound",
                "lifecycle node names an unbound component role",
            ) from exc
        snapshot = _mapping(
            self._client.cclc_component(component_id),
            field="component snapshot",
        )
        if (
            snapshot.get("controllable_component_id") != component_id
            or snapshot.get("retained_bundle_id") != context.retained_bundle_id
            or snapshot.get("graph_revision") != context.graph_revision
        ):
            raise CCLCProgramPortError(
                "cclc_program_context_stale",
                "component snapshot differs from the Applied Program context",
            )
        request_context = self._request_context(request_id, identity)
        owner = _mapping(
            self._client.cclc_owner_identity(), field="CCLC owner identity"
        )
        request_context["request_owner_epoch"] = _text(
            owner.get("request_owner_epoch"), field="request_owner_epoch"
        )
        body = {
            "request_context": request_context,
            "controllable_component_id": component_id,
            "intent": _LIFECYCLE_INTENTS[node_id],
            "expected_graph_revision": context.graph_revision,
            "expected_component_lifecycle_generation": _positive_integer(
                snapshot.get("component_lifecycle_generation"),
                field="component_lifecycle_generation",
            ),
            "expected_control_domains": self._expected_domains(snapshot),
            "reason": "applied-program-node",
            "confirmed": True,
        }
        if self._program_node_evidence is None:
            raise CCLCProgramPortError(
                "program_node_parent_missing",
                "Runtime lifecycle has no exact Program parent",
            )
        body["program_node_evidence"] = dict(
            self._program_node_evidence(identity, "cclc.lifecycle")
        )
        body["expected_admission_generation"] = _positive_integer(
            self._client.authoring_admission().get("admission_generation"),
            field="admission_generation",
        )
        return "lifecycle", body

    def _start_route(
        self,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
        context: _BoundContext,
        request_id: str,
    ) -> tuple[str, dict[str, Any]]:
        target_role = _text(inputs.parameters.get("target_role"), field="target_role")
        route_role = _text(inputs.parameters.get("route_role"), field="route_role")
        try:
            target = context.targets[target_role]
        except KeyError as exc:
            raise CCLCProgramPortError(
                "cclc_target_role_unbound",
                "route node names an unbound target role",
            ) from exc
        if not target.control_binding_ids:
            raise CCLCProgramPortError(
                "cclc_control_binding_unbound",
                "route target has no bound control-domain capability",
            )
        template_set = _mapping(
            self._client.cclc_route_templates(target.component_id),
            field="route template set",
        )
        if (
            template_set.get("controllable_component_id") != target.component_id
            or template_set.get("retained_bundle_id") != context.retained_bundle_id
            or template_set.get("graph_revision") != context.graph_revision
        ):
            raise CCLCProgramPortError(
                "cclc_program_context_stale",
                "route templates differ from the Applied Program context",
            )
        matches: list[Mapping[str, Any]] = []
        for index, raw_template in enumerate(
            _sequence(
                template_set.get("templates"),
                field="route templates",
                nonempty=False,
            )
        ):
            template = _mapping(raw_template, field=f"route templates[{index}]")
            desired = _mapping(
                template.get("desired_configuration"),
                field=f"route templates[{index}].desired_configuration",
            )
            routes = _sequence(
                desired.get("binding_routes"),
                field=f"route templates[{index}].binding_routes",
            )
            if any(
                isinstance(route, Mapping)
                and route.get("control_binding_id") in target.control_binding_ids
                and route.get("control_route_id") == route_role
                for route in routes
            ):
                matches.append(template)
        if len(matches) != 1:
            raise CCLCProgramPortError(
                "cclc_route_template_ambiguous",
                "route role does not select exactly one retained/current template",
            )
        selected = matches[0]
        request_context = self._request_context(request_id, identity)
        owner = _mapping(
            self._client.cclc_owner_identity(), field="CCLC owner identity"
        )
        request_context["request_owner_epoch"] = _text(
            owner.get("request_owner_epoch"), field="request_owner_epoch"
        )
        body = {
            "request_context": request_context,
            "expected_retained_bundle_id": context.retained_bundle_id,
            "expected_graph_revision": context.graph_revision,
            "desired_configuration": _thaw(selected.get("desired_configuration")),
            "expected_active_control_configuration": _thaw(
                selected.get("expected_active_control_configuration")
            ),
            "component_expectations": _thaw(selected.get("component_expectations")),
            "confirmed": True,
        }
        if self._program_node_evidence is None:
            raise CCLCProgramPortError(
                "program_node_parent_missing", "Runtime route selection has no exact Program parent"
            )
        body["program_node_evidence"] = dict(
            self._program_node_evidence(identity, "cclc.control_route")
        )
        body["expected_admission_generation"] = _positive_integer(
            self._client.authoring_admission().get("admission_generation"),
            field="admission_generation",
        )
        return "route", body

    @staticmethod
    def _snapshot_from_result(
        task_id: str,
        kind: str,
        request_id: str,
        result: Mapping[str, Any],
    ) -> tuple[str | None, OwnerTaskSnapshot]:
        if result.get("rejection") is not None:
            if (
                not isinstance(result["rejection"], Mapping)
                or result.get("replay_status") not in {"accepted_new", "replayed_exact"}
                or result.get("acceptance") is not None
                or result.get("status") is not None
            ):
                raise CCLCProgramPortError(
                    "cclc_projection_invalid", "Contradictory no-admission result"
                )
            rejected = _known_rejection(task_id, result["rejection"])
            if rejected is None:
                raise CCLCProgramPortError(
                    "cclc_projection_invalid", "Invalid no-admission result"
                )
            return None, rejected
        replay_status = result.get("replay_status")
        if replay_status not in {
            "accepted_new",
            "replayed_exact",
            "request_result_expired",
            "request_id_conflict",
            "request_ledger_full",
            "stale_request_owner_epoch",
        }:
            raise CCLCProgramPortError(
                "cclc_projection_invalid",
                "CCLC admission has an unknown replay status",
            )
        acceptance = result.get("acceptance")
        status = result.get("status")
        if acceptance is None:
            if status is not None or replay_status in {
                "accepted_new",
                "replayed_exact",
            }:
                raise CCLCProgramPortError(
                    "cclc_projection_invalid",
                    "CCLC admission omits its accepted transaction",
                )
            return None, _failure(
                task_id,
                f"cclc_{replay_status}",
                "CCLC did not accept the Program request",
            )
        acceptance_row = _mapping(acceptance, field="CCLC acceptance")
        if acceptance_row.get("request_id") != request_id:
            raise CCLCProgramPortError(
                "cclc_projection_invalid",
                "CCLC acceptance names another request",
            )
        transaction_field = (
            "lifecycle_transaction_id"
            if kind == "lifecycle"
            else "route_selection_transaction_id"
        )
        transaction_id = _text(
            acceptance_row.get(transaction_field), field=transaction_field
        )
        if status is not None:
            return transaction_id, CCLCProgramTaskPort._status_snapshot(
                task_id, kind, transaction_id, status
            )
        return transaction_id, OwnerTaskSnapshot(
            task_id,
            OwnerTaskState.ACTIVE,
            metadata={"transaction_id": transaction_id, "kind": kind},
        )

    @staticmethod
    def _status_snapshot(
        task_id: str,
        kind: str,
        transaction_id: str,
        raw_status: Any,
    ) -> OwnerTaskSnapshot:
        status = _mapping(raw_status, field="CCLC status")
        acceptance = _mapping(status.get("acceptance"), field="status.acceptance")
        transaction_field = (
            "lifecycle_transaction_id"
            if kind == "lifecycle"
            else "route_selection_transaction_id"
        )
        if acceptance.get(transaction_field) != transaction_id:
            raise CCLCProgramPortError(
                "cclc_projection_invalid", "CCLC status names another transaction"
            )
        terminal = status.get("terminal_result")
        metadata = {
            "transaction_id": transaction_id,
            "kind": kind,
            "phase": status.get("phase"),
            "reconciliation_required": status.get("reconciliation_required"),
        }
        if terminal is None:
            return OwnerTaskSnapshot(task_id, OwnerTaskState.ACTIVE, metadata=metadata)
        if terminal == "succeeded":
            return OwnerTaskSnapshot(
                task_id,
                OwnerTaskState.SUCCEEDED,
                output=_thaw(status),
                metadata=metadata,
                settlement_proven=True,
            )
        if terminal not in {"failed_known_no_effect", "failed_partial_effect"}:
            raise CCLCProgramPortError(
                "cclc_projection_invalid",
                "CCLC status has an unknown terminal result",
            )
        error = status.get("error")
        message = (
            error.get("message")
            if isinstance(error, Mapping)
            and isinstance(error.get("message"), str)
            and error["message"]
            else f"CCLC transaction ended as {terminal}"
        )
        return OwnerTaskSnapshot(
            task_id,
            OwnerTaskState.FAILED,
            metadata=metadata,
            settlement_proven=True,
            reason_code=f"cclc_{terminal}",
            message=message,
        )

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        return self._start(node_id, identity, inputs)

    def _start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
        *,
        previous_task: _Task | None = None,
    ) -> OwnerTaskSnapshot:
        if not isinstance(identity, NodeAttemptIdentity) or not isinstance(
            inputs, OwnerNodeInputs
        ):
            raise TypeError("CCLC start requires exact node identity and inputs")
        if node_id not in _LIFECYCLE_INTENTS and node_id != "controller.select_route":
            raise CCLCProgramPortError(
                "cclc_node_unsupported", "node is not owned by the CCLC adapter"
            )
        context = self._context(identity.program_run_id)
        request_id = _request_id(node_id, identity)
        task_id = request_id
        if previous_task is not None and previous_task.request is not None:
            request_id = previous_task.request["request_context"]["request_id"]
        with self._lock:
            if previous_task is not None:
                if self._tasks.get(task_id) is not previous_task:
                    raise OwnerTaskObservationUnavailableError(
                        "CCLC admission is already being reconciled"
                    )
                self._tasks.pop(task_id)
            self._reserve(task_id, identity.program_run_id)
        body: dict[str, Any] | None = None
        submitted = False
        try:
            for refresh in range(2):
                if node_id in _LIFECYCLE_INTENTS:
                    kind, body = self._start_lifecycle(
                        node_id, identity, inputs, context, request_id
                    )
                else:
                    kind, body = self._start_route(
                        identity, inputs, context, request_id
                    )
                submitted = True
                try:
                    raw_result = (
                        self._client.submit_component_lifecycle(body)
                        if kind == "lifecycle"
                        else self._client.submit_route_selection(body)
                    )
                    break
                except Exception as error:
                    failure = _error_payload(error)
                    if (
                        refresh == 0
                        and failure["code"] == "stale_generation"
                        and isinstance(failure["details"], Mapping)
                        and failure["details"].get("effect") == "not_started"
                        and failure["details"].get("reason")
                        == "expected_generation_stale"
                    ):
                        # The owner proved no admission. Reuse the bounded
                        # refresh, with a new receipt identity for changed input.
                        request_id = f"{task_id}-admission-{body['expected_admission_generation']}"
                        submitted = False
                        continue
                    # The receiver explicitly proved this request unstarted
                    # before retiring its settled namespace. Re-read all
                    # binding/admission facts once; never retry a lost reply,
                    # expired result, generic stale epoch, or unknown effect.
                    if (
                        refresh == 0
                        and failure["code"] == "cclc_request_namespace_refreshed"
                        and isinstance(failure["details"], Mapping)
                        and failure["details"].get("effect") == "not_started"
                        and failure["details"].get("reason")
                        == "settled_request_namespace_checkpoint"
                    ):
                        submitted = False
                        continue
                    raise
            result = _mapping(raw_result, field="CCLC admission")
            transaction_id, snapshot = self._snapshot_from_result(
                task_id, kind, request_id, result
            )
        except OwnerInvocationHeldError:
            with self._lock:
                self._reserved_tasks.pop(task_id, None)
            raise
        except Exception as error:
            try:
                rejected = _known_rejection(task_id, _error_payload(error))
            except OwnerInvocationHeldError:
                with self._lock:
                    self._reserved_tasks.pop(task_id, None)
                raise
            kind = "route" if node_id == "controller.select_route" else "lifecycle"
            transaction_id = None
            snapshot = rejected or (
                OwnerTaskSnapshot(
                    task_id,
                    OwnerTaskState.ACTIVE,
                    metadata={"kind": kind, "outcome_unconfirmed": True},
                    message="Lifecycle transaction outcome remains unconfirmed",
                )
                if submitted
                else _failure(
                    task_id,
                    getattr(error, "code", "cclc_request_failed"),
                    str(error) or type(error).__name__,
                )
            )
        with self._lock:
            self._reserved_tasks.pop(task_id, None)
            if identity.program_run_id in self._contexts:
                self._tasks[task_id] = _Task(
                    identity.program_run_id,
                    kind,
                    transaction_id,
                    snapshot,
                    body if submitted else None,
                    (node_id, identity, inputs) if transaction_id is None else None,
                )
        return snapshot

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        task_id = _text(task_id, field="task_id")
        with self._lock:
            try:
                task = self._tasks[task_id]
            except KeyError as exc:
                raise CCLCProgramPortError(
                    "cclc_task_missing", "CCLC Program task is not retained"
                ) from exc
            if task.snapshot.state is not OwnerTaskState.ACTIVE:
                return task.snapshot
            transaction_id = task.transaction_id
            kind = task.kind
        if (
            task.snapshot.metadata.get("admission_deferred")
            and task.start_args is not None
        ):
            # This is not replay of uncertain work. The prior owner result
            # proves no admission. Reuse this task's existing observation wake
            # for one fresh attempt; Kernel still orders it against Stop.
            return self._start(*task.start_args, previous_task=task)
        try:
            if transaction_id is None:
                if task.request is None:
                    raise OwnerTaskObservationUnavailableError(
                        "CCLC submission outcome is unknown"
                    )
                response = (
                    self._client.component_lifecycle_request_result(task.request)
                    if kind == "lifecycle"
                    else self._client.route_selection_request_result(task.request)
                )
                result = _mapping(response, field="CCLC result response").get("result")
                if not isinstance(result, Mapping) or result.get(
                    "replay_status"
                ) not in {"accepted_new", "replayed_exact"}:
                    raise OwnerTaskObservationUnavailableError(
                        "CCLC submission outcome is unknown"
                    )
                transaction_id, snapshot = self._snapshot_from_result(
                    task_id, kind, task.request["request_context"]["request_id"], result
                )
                with self._lock:
                    if self._tasks.get(task_id) is task:
                        task.transaction_id = transaction_id
                        task.snapshot = snapshot
                return snapshot
            response = (
                self._client.component_lifecycle_status(transaction_id)
                if kind == "lifecycle"
                else self._client.route_selection_status(transaction_id)
            )
            raw_status = _mapping(response, field="CCLC status response").get("status")
            if raw_status is None:
                raise OwnerTaskObservationUnavailableError(
                    "Accepted CCLC status is unavailable"
                )
            else:
                snapshot = self._status_snapshot(
                    task_id, kind, transaction_id, raw_status
                )
        except OwnerInvocationHeldError:
            with self._lock:
                self._tasks.pop(task_id, None)
            raise
        except Exception as error:
            try:
                rejected = _known_rejection(task_id, _error_payload(error))
            except OwnerInvocationHeldError:
                with self._lock:
                    self._tasks.pop(task_id, None)
                raise
            if rejected is not None:
                with self._lock:
                    if self._tasks.get(task_id) is task:
                        task.snapshot = rejected
                return rejected
            raise OwnerTaskObservationUnavailableError(
                "CCLC transaction outcome remains unconfirmed"
            ) from error
        with self._lock:
            current = self._tasks.get(task_id)
            if current is not None and current.transaction_id == transaction_id:
                current.snapshot = snapshot
        return snapshot

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        """CCLC transactions are non-cancellable; this performs no owner call."""

        task_id = _text(task_id, field="task_id")
        if not isinstance(identity, NodeAttemptIdentity):
            raise TypeError("identity must be a NodeAttemptIdentity")
        with self._lock:
            task = self._tasks.get(task_id)
            if task is None or task.program_run_id != identity.program_run_id:
                raise CCLCProgramPortError(
                    "cclc_task_missing", "CCLC Program task is not retained"
                )

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        task_id = _text(task_id, field="task_id")
        if not isinstance(identity, NodeAttemptIdentity):
            raise TypeError("identity must be a NodeAttemptIdentity")
        with self._lock:
            task = self._tasks.get(task_id)
            if (
                task is None
                or task.program_run_id != identity.program_run_id
                or not task.snapshot.settlement_proven
            ):
                raise CCLCProgramPortError(
                    "cclc_task_not_acknowledgeable",
                    "CCLC task is absent, foreign, or lacks settlement evidence",
                )
            del self._tasks[task_id]


__all__ = [
    "CCLCProgramClient",
    "CCLCProgramPortError",
    "CCLCProgramTaskPort",
]
