"""Typed same-host execution transport for the Program owner."""

from __future__ import annotations

from collections.abc import Mapping
import math
from pathlib import Path
import re
from typing import Annotated, Any, Literal, TypeVar
import unicodedata

from pydantic import BaseModel, Field, ValidationError, field_validator, model_validator

from manyforge_behavior.orchestration_ports import ProgramSelectionReleaseRejectedError
from manyforge_headless_api.execution import (
    EXECUTION_COMMAND_VERSION,
    EXECUTION_RESPONSE_VERSION,
    ExecutionCommandClient,
)
from manyforge_headless_api.models import (
    MAX_OPERATION_PARTICIPANTS,
    StrictRequest,
    OperationControlRequest,
    OperationRuntimeScaleRequest,
    OperationIntentRequest,
    OperationParticipantRequest,
    OperationRecoveryActionRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    ProgramRunStopIntentRequest,
    ProgramOperationQuery,
    FullStopIntentRequest,
    StopIntentRequest,
    TargetObservationQuery,
    TcpEntryRequest,
    IoWatchResult,
    SHA256_PATTERN,
)
from manyforge_headless_api.program_cycle import ProgramResetPositionAvailabilityDTO
from manyforge_headless_api.native import HeadlessApiError


_Request = TypeVar("_Request", bound=BaseModel)
_Revision = Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
_Digest = Annotated[str, Field(pattern=SHA256_PATTERN)]
_Identity = Annotated[str, Field(min_length=1, max_length=256)]


class _ResetPositionAuthorization(StrictRequest):
    program_runner_owner_epoch: _Identity
    cycle_generation: _Revision
    request_owner_epoch: _Identity
    request_id: _Identity
    request_payload_digest: _Digest
    program_run_id: _Identity | None
    cycle_claim_generation: _Revision | None
    terminal_or_settlement_reference: _Digest | None
    reset_claim_generation: _Revision | None

    @model_validator(mode="after")
    def exact_claim_shape(self):
        if self.cycle_claim_generation is not None:
            valid = self.program_run_id is not None and self.terminal_or_settlement_reference is not None and self.reset_claim_generation is None
        else:
            valid = self.reset_claim_generation is not None and (
                (self.program_run_id is None) == (self.terminal_or_settlement_reference is None)
            )
        if not valid:
            raise ValueError("position Reset requires its exact run or Reset claim shape")
        return self


class _ResetPositionDeploymentIdentity(StrictRequest):
    owner_epoch: _Identity
    active_publication_generation: _Revision
    retained_bundle_id: _Digest
    graph_revision: _Digest


class _ResetPositionRequest(StrictRequest):
    authorization: _ResetPositionAuthorization
    expected_deployment_identity: _ResetPositionDeploymentIdentity


class _ResetPositionExecutionIdentity(_ResetPositionDeploymentIdentity):
    schema_version: Literal["manyforge.execution_identity.v2"]
    cell_id: _Identity
    control_management_mode: Literal["component_lifecycle", "pending"]


class _ResetPositionSuccessResponse(StrictRequest):
    api_version: Literal[EXECUTION_RESPONSE_VERSION]
    ok: Literal[True]

    @field_validator("ok", mode="before")
    @classmethod
    def exact_success(cls, value):
        if value is not True:
            raise ValueError("position Reset success must be a strict true boolean")
        return value


class _ResetPositionCapabilityResponse(_ResetPositionSuccessResponse):
    command: Literal["reset_robot_position_capability"]
    reset_robot_position: ProgramResetPositionAvailabilityDTO
    identity: _ResetPositionExecutionIdentity


class _ResetPositionResponse(_ResetPositionSuccessResponse):
    command: Literal["reset_robot_position"]


class _ActiveTcpReading(StrictRequest):
    component_id: _Identity
    entry: TcpEntryRequest | None
    owner_epoch: _Identity
    registration_generation: _Revision
    sequence: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]


class _ActiveTcpResponse(StrictRequest):
    api_version: Literal[EXECUTION_RESPONSE_VERSION]
    command: Literal["read_active_tcp", "reset_active_tcp"]
    ok: Literal[True]
    robots: list[_ActiveTcpReading]


class _ProgramNodeExecutionScope(StrictRequest):
    """Private Runner attribution, never accepted from a public operation DTO."""

    node_instance_id: str
    node_attempt_id: str

    @field_validator("node_instance_id", "node_attempt_id")
    @classmethod
    def validate_scope_identity(cls, value: str, info) -> str:
        limit = 128 if info.field_name == "node_instance_id" else 512
        if not value or "\0" in value or len(value.encode("utf-8")) > limit:
            raise ValueError("Program execution scope identity exceeds its bounds")
        return value


class _ProgramParticipant(OperationParticipantRequest):
    execution_scope: _ProgramNodeExecutionScope


class _ProgramControl(OperationControlRequest):
    execution_scope: _ProgramNodeExecutionScope


class _ProgramOperation(OperationIntentRequest):
    participants: Annotated[
        list[_ProgramParticipant],
        Field(min_length=1, max_length=MAX_OPERATION_PARTICIPANTS),
    ]


class _ProgramPick(PickSequencePlanningRequest):
    approach_operation: _ProgramOperation
    descend_operation: _ProgramOperation
    tool_operation: _ProgramOperation
    retreat_operation: _ProgramOperation


class _ProgramPlace(PlaceSequencePlanningRequest):
    approach_operation: _ProgramOperation
    descend_operation: _ProgramOperation
    tool_operation: _ProgramOperation
    retreat_operation: _ProgramOperation


class _IoWatchCondition(StrictRequest):
    # Edge and compound conditions are refused before this boundary.
    kind: Literal["level"]
    expected_value: bool


class _IoWatchRequest(StrictRequest):
    """`manyforge.io_watch_request.v1`: one node attempt's level watch."""

    schema_version: Literal["manyforge.io_watch_request.v1"]
    owner_epoch: _Identity
    program_binding_id: _Digest
    program_runner_owner_epoch: _Identity
    claim_generation: _Revision
    program_run_id: _Identity
    node_instance_id: Annotated[str, Field(min_length=1, max_length=128)]
    node_attempt_id: Annotated[str, Field(min_length=1, max_length=512)]
    target_role_id: _Identity
    resource_role_id: _Identity
    condition: _IoWatchCondition


class _IoWatchNode(StrictRequest):
    node_instance_id: Annotated[str, Field(min_length=1, max_length=128)]
    node_attempt_id: Annotated[str, Field(min_length=1, max_length=512)]


class _IoWatchScope(StrictRequest):
    program_runner_owner_epoch: _Identity
    program_run_id: _Identity
    node: _IoWatchNode


class _IoWatchScopeRelease(StrictRequest):
    """`manyforge.io_watch_scope_release.v1`: release (and fence) one exact
    node attempt's watch when its registration reply was never known."""

    schema_version: Literal["manyforge.io_watch_scope_release.v1"]
    owner_epoch: _Identity
    program_binding_id: _Digest
    program_runner_owner_epoch: _Identity
    claim_generation: _Revision
    program_run_id: _Identity
    node_instance_id: Annotated[str, Field(min_length=1, max_length=128)]
    node_attempt_id: Annotated[str, Field(min_length=1, max_length=512)]


class _IoWatchRegistration(StrictRequest):
    """The exact Core registration token; read/release send it unchanged."""

    owner_epoch: _Identity
    catalog_generation: _Identity
    registration_id: _Identity
    scope: _IoWatchScope


_UnsignedDecimal = Annotated[str, Field(pattern=r"^(0|[1-9][0-9]{0,19})$")]


# Decision 133 (interface freeze01): `start_mock_io_node_simulation`.
_MOCK_SIMULATION_COMMAND: Literal["start_mock_io_node_simulation"] = (
    "start_mock_io_node_simulation"
)
_PositiveDecimal = Annotated[str, Field(pattern=r"^[1-9][0-9]{0,19}$")]


class _MockSimulationSignal(StrictRequest):
    target_role: _Identity
    resource_role: _Identity


class _MockSimulationChange(StrictRequest):
    after_ms: Annotated[int, Field(ge=0, le=2**64 - 1)]
    value: bool


class _MockSimulationRow(StrictRequest):
    signal: _MockSimulationSignal
    start_value: bool
    # Required and explicit: an authoring omission is normalized to null.
    change: _MockSimulationChange | None


class _MockSimulationReceipt(StrictRequest):
    signal: _MockSimulationSignal
    status: Literal["applied", "inactive"]


def _selectors(rows) -> list[tuple[str, str]]:
    return [(row.signal.target_role, row.signal.resource_role) for row in rows]


class _MockSimulationOccurrence(StrictRequest):
    owner_epoch: _Identity
    program_binding_id: _Digest
    program_runner_owner_epoch: _Identity
    claim_generation: _Revision
    program_run_id: _Identity
    node_instance_id: Annotated[str, Field(min_length=1, max_length=128)]
    node_attempt_id: Annotated[str, Field(min_length=1, max_length=512)]
    # Positive full-width uint64, canonical decimal text (no 53-bit limit).
    node_start_index: _PositiveDecimal

    @field_validator("node_start_index")
    @classmethod
    def full_width_index(cls, value: str) -> str:
        if int(value) > 2**64 - 1:
            raise ValueError("node_start_index exceeds uint64")
        return value


class _MockSimulationEnd(_MockSimulationOccurrence):
    """`manyforge.mock_io_node_end.v1` (decision 135): the start's exact
    occurrence; its still-pending change is cancelled."""

    schema_version: Literal["manyforge.mock_io_node_end.v1"]


class _MockSimulationEndAck(_MockSimulationOccurrence):
    api_version: Literal[EXECUTION_RESPONSE_VERSION]
    command: Literal["end_mock_io_node_simulation"]
    ok: Literal[True]


class _MockRunPauseOccurrence(StrictRequest):
    owner_epoch: _Identity
    program_binding_id: _Digest
    program_runner_owner_epoch: _Identity
    claim_generation: _Revision
    program_run_id: _Identity
    # The Runner's existing run_revision: positive full uint64 decimal text.
    run_revision: _PositiveDecimal
    paused: bool
    # Decision 138 (snapshot138 interface01): nodes whose nearest Hold
    # wrapper is wholly held, sorted unique; mandatory, possibly empty.
    held_node_instance_ids: list[Annotated[str, Field(min_length=1, max_length=128)]]

    @field_validator("run_revision")
    @classmethod
    def full_width_revision(cls, value: str) -> str:
        if int(value) > 2**64 - 1:
            raise ValueError("run_revision exceeds uint64")
        return value

    @field_validator("held_node_instance_ids")
    @classmethod
    def canonical_mask(cls, value: list[str]) -> list[str]:
        if value != sorted(set(value)):
            raise ValueError("held_node_instance_ids is not sorted and unique")
        return value


class _MockRunPause(_MockRunPauseOccurrence):
    """`manyforge.mock_io_run_pause.v1` (SX-2411, decision 138): the Run's
    Halt state and held-node mask under one run_revision."""

    schema_version: Literal["manyforge.mock_io_run_pause.v1"]


class _MockRunPauseAck(_MockRunPauseOccurrence):
    api_version: Literal[EXECUTION_RESPONSE_VERSION]
    command: Literal["set_mock_io_run_paused"]
    ok: Literal[True]


class _MockSimulationRequest(_MockSimulationOccurrence):
    """`manyforge.mock_io_node_start.v1`: one node start's literal rows."""

    schema_version: Literal["manyforge.mock_io_node_start.v1"]
    signals: Annotated[list[_MockSimulationRow], Field(min_length=1)]

    @model_validator(mode="after")
    def canonical_rows(self):
        selectors = _selectors(self.signals)
        if selectors != sorted(set(selectors)):
            raise ValueError("Mock simulation rows are not unique and canonically sorted")
        return self


class _MockSimulationAck(_MockSimulationOccurrence):
    """The exact flat 14-field success reply; no wrapper."""

    api_version: Literal[EXECUTION_RESPONSE_VERSION]
    command: Literal["start_mock_io_node_simulation"]
    ok: Literal[True]
    status: Literal["accepted"]
    # The original host monotonic receipt: nonnegative int64 decimal text.
    started_at_ns: _UnsignedDecimal
    signals: list[_MockSimulationReceipt]

    @model_validator(mode="after")
    def bounded_anchor(self):
        selectors = _selectors(self.signals)
        if int(self.started_at_ns) > 2**63 - 1 or selectors != sorted(set(selectors)):
            raise ValueError("Mock simulation acknowledgement is not canonical")
        return self


class _ProgramBlackboardValue(StrictRequest):
    name: Annotated[str, Field(min_length=1, max_length=256)]
    value_type: Literal["bool", "int64", "float64", "string"]
    value: bool | str | float | None

    @model_validator(mode="after")
    def typed_value(self):
        value = self.value
        if value is None:
            valid = True  # The key is actually unset.
        elif self.value_type == "bool":
            valid = type(value) is bool
        elif self.value_type == "int64":
            valid = (
                isinstance(value, str)
                and re.fullmatch(r"0|-?[1-9][0-9]{0,18}", value) is not None
                and -(2**63) <= int(value) <= 2**63 - 1
            )
        elif self.value_type == "float64":
            valid = type(value) is float and math.isfinite(value)
        else:
            valid = isinstance(value, str) and unicodedata.normalize("NFC", value) == value
        if not valid:
            raise ValueError("BB key value does not match its value type")
        return self


class _ProgramBlackboardPublication(StrictRequest):
    """`manyforge.program_bbkey_publication.v1`: one ordered BB key frame."""

    schema_version: Literal["manyforge.program_bbkey_publication.v1"]
    owner_epoch: _Identity
    program_runner_owner_epoch: _Identity
    # Both null for the cell-level source between Runs.
    claim_generation: _Revision | None
    program_run_id: _Identity | None
    program_binding_id: _Digest
    # bb-wire-shapes03: the host's existing Applied publication counter.
    active_publication_generation: _Revision
    cycle_generation: _Revision
    kind: Literal["baseline", "update"]
    message_sequence: _UnsignedDecimal
    previous_sequence: _UnsignedDecimal
    values: list[_ProgramBlackboardValue]

    @model_validator(mode="after")
    def ordered_frame(self):
        sequence = int(self.message_sequence)
        names = [row.name for row in self.values]
        if (
            not 1 <= sequence <= 2**64 - 1
            or int(self.previous_sequence) != sequence - 1
            or (self.kind == "baseline") != (sequence == 1)
            or names != sorted(set(names))
            or not names
            or (self.claim_generation is None) != (self.program_run_id is None)
        ):
            raise ValueError("BB key publication frame is not ordered and canonical")
        return self


class _ProgramBlackboardPublicationAck(StrictRequest):
    """The actual Core admission reply (bb-wire-shapes03), closed.

    `accepted`: this frame was admitted (no ACK for enqueue). `superseded`:
    the frame's occurrence was retired by an authoritative Reset/new Run/
    Apply replacement; `accepted_sequence` is null and the occurrence fields
    describe the installed replacement. Mismatches stay ErrorDTOs.
    """

    api_version: Literal[EXECUTION_RESPONSE_VERSION]
    command: Literal["publish_program_bbkeys"]
    ok: Literal[True]
    status: Literal["accepted", "superseded"]
    owner_epoch: _Identity
    program_binding_id: _Digest
    active_publication_generation: _Revision
    cycle_generation: _Revision
    # Null between Runs.
    program_run_id: _Identity | None
    source_epoch: _Identity
    accepted_sequence: _UnsignedDecimal | None

    @model_validator(mode="after")
    def sequence_matches_status(self):
        if (self.status == "accepted") != (self.accepted_sequence is not None):
            raise ValueError("BB publication reply sequence does not match its status")
        return self


class ProgramRunnerExecutionTransport(ExecutionCommandClient):
    """Validate Program mappings over the existing private owner client."""

    def __init__(self, run_root: Path, *, timeout_seconds: float) -> None:
        super().__init__(
            run_root,
            timeout_seconds=timeout_seconds,
            attribution_transport="internal",
        )

    def resolve_attach_active_tcp(self, binding_id: str, target_role: str) -> str:
        response = self.require_success(self._exchange({
            "api_version": EXECUTION_COMMAND_VERSION,
            "command": "resolve_attach_active_tcp",
            "program_binding_id": binding_id,
            "target_role_id": target_role,
        }))
        frame = response.get("frame_role_id")
        if response.get("command") != "resolve_attach_active_tcp" or not isinstance(frame, str) or not frame:
            raise HeadlessApiError("execution_command_response_invalid", "Invalid Active TCP holder response")
        return frame

    @staticmethod
    def _request(model: type[_Request], body: Mapping[str, Any]) -> _Request:
        try:
            return model.model_validate(body)
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_invalid",
                "Program Runner execution request is invalid",
            ) from error

    @staticmethod
    def _reset_response(model: type[_Request], body: Mapping[str, Any]) -> _Request:
        try:
            return model.model_validate(body)
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid", "Native position Reset response is invalid"
            ) from error

    def reset_robot_position_capability(self) -> dict[str, Any]:
        response = self._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                   "command": "reset_robot_position_capability"})
        return self._reset_response(_ResetPositionCapabilityResponse, response).model_dump(mode="json")

    def reset_robot_position(self, body: Mapping[str, Any]) -> None:
        request = self._request(_ResetPositionRequest, body)
        response = self._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                  "command": "reset_robot_position", **request.model_dump(mode="json")})
        self._reset_response(_ResetPositionResponse, response)

    def _active_tcp(self, command: Literal["read_active_tcp", "reset_active_tcp"],
                    *, ordered: bool = False) -> dict[str, Any]:
        request = {"api_version": EXECUTION_COMMAND_VERSION, "command": command}
        if command == "reset_active_tcp":
            request["attribution"] = self._request_attribution(None)
        elif ordered:
            request["ordered"] = True
        response = self._exchange(request)
        try:
            parsed = _ActiveTcpResponse.model_validate(response)
            if parsed.command != command:
                raise ValueError("Active TCP response names another command")
        except (ValidationError, ValueError) as error:
            raise HeadlessApiError(
                "execution_command_response_invalid", "Active TCP response is invalid"
            ) from error
        return response

    def read_active_tcp(self, *, ordered: bool = False) -> dict[str, Any]:
        if type(ordered) is not bool:
            raise TypeError("ordered must be boolean")
        return self._active_tcp("read_active_tcp", ordered=ordered)

    def reset_active_tcp(self) -> dict[str, Any]:
        return self._active_tcp("reset_active_tcp")

    def _exchange_on(
        self, command: dict[str, Any], socket_path: Path
    ) -> dict[str, Any]:
        response = super()._exchange_on(command, socket_path)
        try:
            return self.require_success(response)
        except HeadlessApiError as error:
            if command.get("command") == "submit" and error.details.get("immediate_step") == "admitted":
                rows = response.get("active_tcp")
                try:
                    if type(rows) is not list:
                        raise ValueError("Active TCP rows are missing")
                    for row in rows:
                        _ActiveTcpReading.model_validate(row)
                except (ValidationError, ValueError) as invalid:
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "admitted immediate step response has invalid Active TCP rows",
                    ) from invalid
                error.active_tcp = rows
            if command.get("command") in {
                "start_program_selection",
                "release_program_selection",
                "check_program_node_motion_attention",
            } and error.details.get("reason") in {
                "program_selection_stale",
                "program_selection_unknown",
                "program_node_unknown",
                "program_selection_revision_stale",
                "program_selection_not_ready",
                "program_selection_not_releasable",
                "program_selection_recovery_required",
                "program_children_unsettled",
                "program_run_admission_in_flight",
                # Operator-alarm motion attention (spec 450 §5.2).
                "operator_alarm_interlock_active",
                "operator_alarm_state_unavailable",
            }:
                # These closed Kernel rejections all precede release. A lost,
                # malformed or exceptional reply remains outcome-unknown.
                raise ProgramSelectionReleaseRejectedError(
                    str(error), code=error.code, details=error.details
                ) from error
            raise

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().submit(
            self._request(_ProgramOperation, body),
            trusted_principal_id=None,
        )

    def register_program_run_capture(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Send only identities; the native product boundary supplies the table.

        This is an internal Run-owner call, not an external REST operation or
        a preparation side effect. Native decoding and Kernel admission remain
        responsible for the closed shape and the exact current claim.
        """

        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "register_program_run_capture",
                "capture": dict(body),
            }
        )
        if (
            response.get("command") != "register_program_run_capture"
            or type(response.get("claim_generation")) is not int
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Program capture response has the wrong kind or shape",
            )
        return response

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().query_target(self._request(TargetObservationQuery, body))

    def _io_watch_result(self, response: Mapping[str, Any], command: str) -> dict[str, Any]:
        if response.get("command") != command:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                f"{command} response does not name its command",
            )
        try:
            return IoWatchResult.model_validate(response.get("result")).model_dump(mode="json")
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid", f"{command} result is invalid"
            ) from error

    def register_io_watch(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Register one zero-claim level watch for one exact node attempt."""

        request = self._request(_IoWatchRequest, body)
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "register_io_watch",
                "attribution": self._request_attribution(None),
                "request": request.model_dump(mode="json"),
            }
        )
        result = self._io_watch_result(response, "register_io_watch")
        try:
            registration = _IoWatchRegistration.model_validate(response.get("registration"))
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid", "I/O watch registration is invalid"
            ) from error
        return {"registration": registration.model_dump(mode="json"), "result": result}

    def _io_watch_registration(self, registration: Mapping[str, Any]) -> dict[str, Any]:
        return self._request(_IoWatchRegistration, registration).model_dump(mode="json")

    def read_io_watch(self, registration: Mapping[str, Any]) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read_io_watch",
                "attribution": self._request_attribution(None),
                "registration": self._io_watch_registration(registration),
            }
        )
        return self._io_watch_result(response, "read_io_watch")

    def release_io_watch(self, registration: Mapping[str, Any]) -> None:
        """Exact-token idempotent release; a released token stays released."""

        self._release_io_watch(
            {"registration": self._io_watch_registration(registration)}
        )

    def release_io_watch_scope(self, scope_request: Mapping[str, Any]) -> None:
        """Tokenless release of one exact node attempt's watch scope.

        The same command with `scope_request` instead of `registration`; the
        host ACKs only after fencing the attempt and releasing any watch it
        holds, with no result.
        """

        self._release_io_watch(
            {
                "scope_request": self._request(
                    _IoWatchScopeRelease, scope_request
                ).model_dump(mode="json")
            }
        )

    def _release_io_watch(self, selector: dict[str, Any]) -> None:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "release_io_watch",
                "attribution": self._request_attribution(None),
                **selector,
            }
        )
        if response.get("command") != "release_io_watch":
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "release_io_watch response does not name its command",
            )

    def start_mock_io_node_simulation(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Start one node attempt's Mock simulation; only a well-formed accepted
        reply ACKs it.

        A host ErrorResponse keeps its code and details. Resending the same
        request is answered with the host's retained original result.
        """

        request = self._request(_MockSimulationRequest, body)
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": _MOCK_SIMULATION_COMMAND,
                "attribution": self._request_attribution(None),
                "request": request.model_dump(mode="json"),
            }
        )
        if response.get("ok") is False:
            self._raise_error_response(response)
        try:
            ack = _MockSimulationAck.model_validate(response)
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Mock simulation acknowledgement is invalid",
            ) from error
        return ack.model_dump(mode="json")

    def _mock_exchange(
        self,
        command: str,
        request_model: type[_Request],
        ack_model: type[_Request],
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        """One closed Mock lifetime command; a host ErrorResponse keeps its
        code."""

        request = self._request(request_model, body)
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": command,
                "attribution": self._request_attribution(None),
                "request": request.model_dump(mode="json"),
            }
        )
        if response.get("ok") is False:
            self._raise_error_response(response)
        try:
            ack = ack_model.model_validate(response)
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid", f"{command} reply is invalid"
            ) from error
        return ack.model_dump(mode="json")

    def end_mock_io_node_simulation(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Decision 135: end one node attempt's Mock simulation (idempotent)."""

        return self._mock_exchange(
            "end_mock_io_node_simulation", _MockSimulationEnd, _MockSimulationEndAck, body
        )

    def set_mock_io_run_paused(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Decision 135 / SX-2411: the Run's Halt state for Mock timers."""

        return self._mock_exchange(
            "set_mock_io_run_paused", _MockRunPause, _MockRunPauseAck, body
        )

    def publish_program_bbkeys(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Send one ordered BB key frame; only a well-formed success reply ACKs it."""

        request = self._request(_ProgramBlackboardPublication, body)
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "publish_program_bbkeys",
                "attribution": self._request_attribution(None),
                "request": request.model_dump(mode="json"),
            }
        )
        if response.get("ok") is False:
            # The owner's ErrorDTO keeps its code (e.g. bb_source_not_ready).
            self._raise_error_response(response)
        try:
            ack = _ProgramBlackboardPublicationAck.model_validate(response)
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "BB key publication reply is invalid",
            ) from error
        return ack.model_dump(mode="json")

    def read_interrupt_rule_state_events(
        self, owner_epoch: str, after_sequence: int
    ) -> dict[str, Any]:
        """One page of the existing ``events`` command, returned whole for the
        global-rule state mirror (contract 64): its outer
        ``interrupt_rule_state_baseline`` and its sparse
        ``interrupt_rule_state_changed`` rows, read strictly by the mirror."""

        if type(after_sequence) is not int or not 0 <= after_sequence <= 9_007_199_254_740_991:
            raise ValueError("operation event query exceeds its fixed bounds")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "after_sequence": after_sequence,
                "command": "events",
                "limit": 16,
                "owner_epoch": owner_epoch,
            }
        )
        if response.get("ok") is not True:
            self._raise_error_response(response)
        return response

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read_program_execution_scope",
                "program_run_id": program_run_id,
            }
        )

    def read_manual_handover(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().read_manual_handover(
            self._request(OperationIntentRequest, body), trusted_principal_id=None
        )

    def stop_program_selection(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "stop_program_selection",
                "attribution": self._request_attribution(None),
                "request": dict(body),
            }
        )

    def release_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "release_program_selection",
                "attribution": self._request_attribution(None),
                "request": dict(body),
                "expected_selection_revision": expected_selection_revision,
            }
        )

    def start_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "start_program_selection",
                "attribution": self._request_attribution(None),
                "request": dict(body),
                "expected_selection_revision": expected_selection_revision,
            }
        )

    def check_program_node_motion_attention(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "check_program_node_motion_attention",
                "attribution": self._request_attribution(None),
                "request": dict(body),
            }
        )

    def query_operation(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().query_operation(self._request(ProgramOperationQuery, body))

    def full_stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().full_stop(
            self._request(FullStopIntentRequest, body), trusted_principal_id=None
        )

    def set_operation_runtime_scale(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().set_operation_runtime_scale(
            self._request(OperationRuntimeScaleRequest, body), trusted_principal_id=None
        )

    def apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return super().apply_operation_recovery(
            operation_id,
            expected_operation_revision,
            self._request(OperationRecoveryActionRequest, body),
        )

    def _mapped_control(
        self,
        method: str,
        operation_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        request = self._request(
            _ProgramControl if "execution_scope" in body else OperationControlRequest,
            body,
        )
        if request.operation_id != operation_id:
            raise HeadlessApiError(
                "execution_operation_path_invalid",
                "operation identity and request body must match",
            )
        return getattr(super(), method)(request, trusted_principal_id=None)

    def reset(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._mapped_control("reset", operation_id, body)

    def acknowledge_completed(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._mapped_control("acknowledge_completed", operation_id, body)

    def stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().stop(
            self._request(StopIntentRequest, body),
            trusted_principal_id=None,
        )

    def stop_program_run(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().stop_program_run(
            self._request(ProgramRunStopIntentRequest, body)
        )

    def set_rule_active(self, rule_id: str, active: bool) -> dict[str, Any]:
        """Kernel `set_rule_active` (node `rules.set_active`, CONTRACTS §8)."""

        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "set_rule_active",
                "rule_id": rule_id,
                "active": active,
            }
        )

    def reset_rule_activation(self) -> dict[str, Any]:
        """Kernel `reset_rule_activation`: every rule to `active_at_start`."""

        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "reset_rule_activation",
            }
        )

    def clear_outside_change(
        self, controllable_component_id: str, signal_name: str
    ) -> dict[str, Any]:
        """Kernel `clear_outside_change` (node `io.clear_outside_change`)."""

        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "clear_outside_change",
                "controllable_component_id": controllable_component_id,
                "signal_name": signal_name,
            }
        )

    def _pick_place(
        self,
        method: str,
        model: type[PickSequencePlanningRequest] | type[PlaceSequencePlanningRequest],
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return getattr(super(), method)(
            self._request(model, body),
            trusted_principal_id=None,
        )

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("plan_pick", _ProgramPick, body)

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("plan_place", _ProgramPlace, body)

__all__ = ["ProgramRunnerExecutionTransport"]
