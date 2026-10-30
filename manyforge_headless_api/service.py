"""Owner-neutral projections shared by FastAPI routes and tests."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
import logging
import math
from threading import Lock
import time
from typing import Any, Callable, Mapping, Sequence

import rfc8785
from starlette.responses import JSONResponse

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.skill_loader import default_skills_dir, load_skill_catalog

from .client import MAX_HTTP_RESPONSE_BYTES
from .execution import ExecutionPort
from .manager import ManagerPort
from .history_query import HistorianCaptureRequest, HistorianQueryRequest
from .models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ActivationPreparationRequest,
    CallerAttributionRequest,
    CanonicalEmptyActivationPreparationRequest,
    HistorianReconcileRequest,
    IoSignalReadRequest,
    ManagerApplyRequest,
    ManagerApplyAcknowledgementContext,
    MockIoInputRequest,
    ManagerDeploymentCandidatePreviewRequest,
    AuthoringMutationContext,
    AuthoringMutationRecoveryRequest,
    AuthoringMutationSettlementRequest,
    AuthoringOwnerReconciliationRequest,
    ProgramModelAttemptRequest,
    ProgramNodeCorrectionPreviewRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramScenePreparationRequest,
    ProgramCycleResetRequest,
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    RuntimeBlackboardWriteRequest,
    FullStopIntentRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
    SkillExpansionRequest,
    StopIntentRequest,
    GlobalIoRuleRecoveryIntentRequest,
    StopFaultRecoveryIntentRequest,
    validate_failure_details_and_origin,
)
from .cell_admission import CellAdmissionError
from .component_overview import ComponentOverviewFacts
from .native import HeadlessApiError, HeadlessCorePort, ViewerModelAsset
from .apply_follower import CommittedApplyFollower
from .program_runner_client import ProgramRunnerClientError, ProgramRunnerPort
from .program_cycle import program_operation_failures
from .program_binding_transport import ProgramBindingTransportError
from .program_binding_projection import (
    validate_applied_program_selection,
    validate_committed_scenario_activation,
    validate_prepared_activation_origin,
)


RUNTIME_SNAPSHOT_VERSION = "manyforge.headless_runtime_snapshot.v1"
MANAGER_SOURCE_VERSION = "manyforge.manager_source.v1"
PROGRAM_RUN_INDEX_VERSION = "manyforge.program_run_index.v1"
PROGRAM_NODE_PREVIEW_INTENT_VERSION = (
    "manyforge.program_node_correction_preview_intent.v1"
)
PROGRAM_NODE_PREVIEW_VERSION = "manyforge.program_node_correction_preview.v1"
# What the native motion preview evaluates: it plans a trajectory from the
# current observed start state and collision-validates it against the named
# scene revision. It is not IK-only, not a reservation, and execution always
# plans and validates again.
PROGRAM_NODE_PREVIEW_CHECK = "trajectory_planned_and_collision_validated"
_PROGRAM_NODE_PREVIEW_IDENTITY_KEYS = (
    "request_id",
    "program_run_id",
    "program_id",
    "program_revision",
    "program_binding_id",
    "node_instance_id",
    "node_attempt_id",
)
# Optional debug-export member carrying the native volatile motion trace.
VOLATILE_MOTION_TRACE_MEMBER = "volatile_motion_trace"
# Exact bytes the member adds to the rendered bundle, before its value.
_VOLATILE_MOTION_TRACE_MEMBER_TEXT = f',"{VOLATILE_MOTION_TRACE_MEMBER}":'
# Fixed slack for the {"available":true,"trace":...} wrapper around the native
# trace object; the final render is checked against the limit regardless.
VOLATILE_MOTION_TRACE_MARGIN_BYTES = 1024
# Recommended native trace object cap (fits the 4 MiB execution IPC envelope).
VOLATILE_MOTION_TRACE_MAX_BYTES = 3 * 1024 * 1024
# Below this remaining budget no useful trace fits; skip the native read.
VOLATILE_MOTION_TRACE_MIN_ALLOWANCE_BYTES = 4096
PROGRAM_RUN_STATUS_VERSION = "manyforge.program_run_status.v3"
PROGRAM_RUN_CAPTURE_VERSION = "manyforge.program_run_capture.v2"
SOURCE_MODEL_CONTEXT_VERSION = "manyforge.source_model_context.v1"
ASSISTANT_MODE_CONTEXT_VERSION = "manyforge.assistant_mode_context.v1"
_PROGRAM_RUN_STATES = frozenset(
    {
        "running",
        "intervention_required",
        "full_stopping",
        "full_stopped",
        "stopping",
        "finalizing",
        "stopped",
        "succeeded",
        "failed",
    }
)
_ACTIVE_PROGRAM_RUN_STATES = frozenset(
    {
        "running",
        "intervention_required",
        "full_stopping",
        "full_stopped",
        "stopping",
        "finalizing",
    }
)

_HISTORY_SUMMARY_PAGE_VERSION = "manyforge.historian_summary_page.v2"


def _rendered_response_bytes(content: dict[str, Any]) -> int:
    """Byte length of ``content`` exactly as the route's JSONResponse renders it."""
    return len(JSONResponse(content=content).body)


def _trace_unavailable(reason: str) -> dict[str, Any]:
    return {"available": False, "reason": reason}


def _history_page(page: dict[str, Any], schema: str) -> dict[str, Any]:
    events = page.get("events")
    if (
        page.get("schema_version") != schema
        or not isinstance(events, list)
        or len(events) > 32
        or type(page.get("truncated")) is not bool
    ):
        raise HeadlessApiError(
            "historian_response_invalid",
            "native historian returned an invalid page",
        )
    previous = 9_007_199_254_740_992
    for stored in events:
        sequence = stored.get("sequence") if isinstance(stored, dict) else None
        if type(sequence) is not int or not 0 < sequence < previous:
            raise HeadlessApiError(
                "historian_response_invalid", "historian page sequence is invalid"
            )
        previous = sequence
    expected = events[-1]["sequence"] if page["truncated"] and events else None
    cursor = page.get("next_before_sequence")
    if (
        (page["truncated"] and not events)
        or cursor != expected
        or (cursor is not None and type(cursor) is not int)
    ):
        raise HeadlessApiError(
            "historian_response_invalid", "historian page cursor does not advance"
        )
    return page


_ADMITTED_SPEED_HISTORY_PAGES = 4


def _prepared_motion_admitted_speeds(
    stored: Any, program_run_id: str
) -> list[dict[str, Any]]:
    """Copy one ``operation_prepared`` record's Kernel speed diagnostics.

    Rows are matched to their exact participant target. Anything malformed or
    from another run yields no row rather than a guessed value.
    """

    event = stored.get("event") if isinstance(stored, dict) else None
    payload = event.get("payload") if isinstance(event, dict) else None
    journal = payload.get("journal_payload") if isinstance(payload, dict) else None
    if not isinstance(journal, dict) or journal.get("record_kind") != "operation_prepared":
        return []
    state = journal.get("operation_state")
    intent = journal.get("intent")
    if (
        not isinstance(state, dict)
        or state.get("program_run_id") != program_run_id
        or not isinstance(intent, dict)
    ):
        return []
    participants = {
        participant.get("participant_id"): participant
        for participant in state.get("participants") or ()
        if isinstance(participant, dict)
    }
    rows: list[dict[str, Any]] = []
    for diagnostic in intent.get("prepared_motion_diagnostics") or ():
        if not isinstance(diagnostic, dict):
            continue
        participant = participants.get(diagnostic.get("participant_id"))
        admitted = diagnostic.get("admitted_tcp_linear_speed_mps")
        if (
            participant is None
            or not _bounded_text(participant.get("controllable_component_id"), maximum_bytes=1024)
            or not _bounded_text(participant.get("command_target_id"), maximum_bytes=1024)
            or type(admitted) not in (int, float)
            or not math.isfinite(admitted)
            or admitted <= 0
        ):
            continue
        rows.append(
            {
                "controllable_component_id": participant["controllable_component_id"],
                "command_target_id": participant["command_target_id"],
                "operation_id": state.get("operation_id"),
                "occurred_at_wall": event.get("occurred_at_wall"),
                "requested_tcp_linear_speed_mps": diagnostic.get(
                    "requested_tcp_linear_speed_mps"
                ),
                "deployment_maximum_tcp_linear_speed_mps": diagnostic.get(
                    "deployment_maximum_tcp_linear_speed_mps"
                ),
                "admitted_tcp_linear_speed_mps": admitted,
            }
        )
    return rows


def _bounded_text(value: object, *, maximum_bytes: int) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and len(value.encode("utf-8")) <= maximum_bytes
    )


def _sha256_identity(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 71
        and value.startswith("sha256:")
        and all(character in "0123456789abcdef" for character in value[7:])
    )


@lru_cache(maxsize=1)
def _build_skills():
    """One installed build input, never a mutable runtime skill registry."""
    environment = build_node_environment()
    return environment, load_skill_catalog(
        default_skills_dir(), catalog=environment.catalog
    )


@lru_cache(maxsize=1)
def _palette_environment():
    return build_node_environment()


def _palette_missing_capability(entry: Any, offers: set[str]) -> str | None:
    """Evaluate unconditional requirements and each parameter's alternatives."""
    conditional: dict[str, dict[object, list[str]]] = {}
    for requirement in entry.required_capabilities:
        if requirement.when_parameter is None:
            if requirement.capability_id not in offers:
                return requirement.capability_id
        else:
            conditional.setdefault(requirement.when_parameter, {}).setdefault(
                requirement.when_equals, []
            ).append(requirement.capability_id)
    for alternatives in conditional.values():
        if not any(
            all(capability in offers for capability in required)
            for required in alternatives.values()
        ):
            return next(
                capability
                for required in alternatives.values()
                for capability in required
                if capability not in offers
            )
    return None


def _palette_model_configured(applied: Mapping[str, Any]) -> bool:
    assistant = applied.get("assistant_projection")
    if not isinstance(assistant, Mapping):
        return False
    models = assistant.get("ai_models")
    endpoints = assistant.get("model_endpoint_aliases")
    if not isinstance(models, list) or not isinstance(endpoints, list):
        return False
    components = {
        row["controllable_component_id"]
        for row in models
        if isinstance(row, Mapping)
        and isinstance(row.get("controllable_component_id"), str)
    }
    return any(
        isinstance(row, Mapping)
        and row.get("external_resource_alias") in components
        for row in endpoints
    )


def _strip_palette_offers(projection: dict[str, Any]) -> Any:
    offers = projection.pop("capability_offers", None)
    availability = projection.get("availability")
    if isinstance(availability, dict):
        for row in availability.get("implementations", []):
            row["reasons"] = []
    return offers


@dataclass(frozen=True, slots=True)
class TrustedAttribution:
    principal_id: str | None = None
    assurance: str = "unverified"


class HeadlessService:
    """Select fields from immutable owner snapshots; own no runtime state."""

    def __init__(
        self,
        core: HeadlessCorePort,
        manager: ManagerPort,
        execution: ExecutionPort | None = None,
        program_runner: ProgramRunnerPort | None = None,
        cell_admission: Any | None = None,
        cclc: Any | None = None,
        activation_preparer: Any | None = None,
    ) -> None:
        self.core = core
        self.manager = manager
        self.execution = execution
        self.program_runner = program_runner
        self.cell_admission = cell_admission
        self.cclc = cclc
        self.activation_preparer = activation_preparer
        # The motion-attention projection the Kernel acknowledged holding
        # (OperatorAlarmAttentionPublisher.kernel_held_attention); None in a
        # composition without the publisher.
        self.kernel_attention_source: Callable[[], dict[str, Any] | None] | None = None
        # Admission bound only: exact node attempts whose advisory native
        # preview is in flight (see program_node_correction_preview).
        self._apply_scene_completion_lock = Lock()
        self._apply_scene_attempted: tuple[str, int, str] | None = None
        self._node_previews_lock = Lock()
        self._node_previews_in_flight: set[tuple[str, str]] = set()
        # Item 38: set when this service ran the Kernel's Force close step
        # (armed Full Stop, Close retirement). It lives for that attempt only:
        # the next preparation of any kind consumes it, success or failure, and
        # the attempt's step `end` (SX-616) or a new arm clears it. Callers
        # cannot set it.
        self._force_close_in_progress = False
        # Each Apply Headless forwards is followed to its committed outcome,
        # which wakes the Program runner (see apply_follower).
        self.apply_follower: CommittedApplyFollower | None = (
            None
            if program_runner is None
            else CommittedApplyFollower(
                observe_apply=lambda apply_id, generation: self.manager.observe_apply(
                    apply_id, generation
                ),
                committed_activation=lambda: self.committed_scenario_activation(),
                wake=lambda: self.notify_committed_apply(),
            )
        )

    def close(self) -> None:
        """Stop following forwarded Applies; owners are untouched."""

        if self.apply_follower is not None:
            self.apply_follower.close()

    def _cell_admission(self) -> Any:
        if self.cell_admission is None:
            raise HeadlessApiError(
                "authoring_not_ready",
                "Kernel cell-admission owner is not configured",
            )
        return self.cell_admission

    @staticmethod
    def _admission_error(error: CellAdmissionError) -> HeadlessApiError:
        return HeadlessApiError(
            error.code, str(error), details=error.details,
            producer=error.producer or "headless_api",
        )

    def authoring_admission(self) -> dict[str, Any]:
        try:
            return self._cell_admission().get()
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    def acquire_authoring_mutation(
        self, request: AuthoringMutationContext
    ) -> dict[str, Any]:
        intent = {
            "claim_kind": "authoring_mutation",
            "owner_role": "authoring_owner",
            "owner_epoch": request.request_owner_epoch,
            "owner_transaction_id": request.owner_transaction_id,
            "request_id": request.request_id,
            "request_payload_digest": request.request_payload_digest,
            "expected_admission_generation": request.expected_admission_generation,
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        }
        try:
            return self._cell_admission().acquire(intent)
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    def settle_authoring_mutation(
        self, request: AuthoringMutationSettlementRequest
    ) -> dict[str, Any]:
        claim = request.claim
        intent = claim.get("intent") if isinstance(claim, dict) else None
        if (
            not isinstance(intent, dict)
            or intent.get("claim_kind") != "authoring_mutation"
            or intent.get("owner_role") != "authoring_owner"
        ):
            raise HeadlessApiError(
                "authoring_claim_invalid",
                "settlement does not identify an authoring claim",
            )
        release = {
            "owner_role": intent["owner_role"],
            "owner_epoch": intent["owner_epoch"],
            "owner_transaction_id": intent["owner_transaction_id"],
            "request_id": intent["request_id"],
            "request_payload_digest": intent["request_payload_digest"],
            "acquired_admission_generation": claim.get("acquired_admission_generation"),
            "claim_generation": claim.get("claim_generation"),
            "settlement": {
                "settlement_kind": request.settlement_kind,
                "evidence_digest": request.evidence_digest,
            },
        }
        try:
            return self._cell_admission().release(release)
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    def recover_authoring_mutation(
        self, request: AuthoringMutationRecoveryRequest
    ) -> dict[str, Any]:
        intent = request.intent
        if (
            intent.get("claim_kind") != "authoring_mutation"
            or intent.get("owner_role") != "authoring_owner"
        ):
            raise HeadlessApiError(
                "authoring_claim_invalid",
                "recovery query does not identify a Composer authoring claim",
            )
        try:
            return self._cell_admission().recovery_receipt(
                request.model_dump(mode="json")
            )
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    def reconcile_authoring_owner(
        self, request: AuthoringOwnerReconciliationRequest
    ) -> dict[str, Any]:
        owners = request.current_owner_epochs
        authoring = [
            item for item in owners if item.get("owner_role") == "authoring_owner"
        ]
        if len(authoring) != 1:
            raise HeadlessApiError(
                "authoring_reconciliation_invalid",
                "reconciliation must name exactly one authoring owner epoch",
            )
        try:
            return self._cell_admission().reconcile(request.model_dump(mode="json"))
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    @staticmethod
    def _request_digest(value: dict[str, Any]) -> str:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        return f"sha256:{hashlib.sha256(encoded).hexdigest()}"

    def _acquire_owner_claim(
        self,
        *,
        claim_kind: str,
        owner_role: str,
        owner_epoch: str,
        owner_transaction_id: str,
        request_id: str,
        expected_generation: int,
        payload: dict[str, Any],
    ) -> dict[str, Any]:
        intent = {
            "claim_kind": claim_kind,
            "owner_role": owner_role,
            "owner_epoch": owner_epoch,
            "owner_transaction_id": owner_transaction_id,
            "request_id": request_id,
            "request_payload_digest": self._request_digest(payload),
            "expected_admission_generation": expected_generation,
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        }
        try:
            return self._cell_admission().acquire(intent)
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    def _release_owner_claim(
        self,
        claim: dict[str, Any],
        *,
        committed: bool,
        result: dict[str, Any],
    ) -> None:
        intent = claim["intent"]
        release = {
            "owner_role": intent["owner_role"],
            "owner_epoch": intent["owner_epoch"],
            "owner_transaction_id": intent["owner_transaction_id"],
            "request_id": intent["request_id"],
            "request_payload_digest": intent["request_payload_digest"],
            "acquired_admission_generation": claim["acquired_admission_generation"],
            "claim_generation": claim["claim_generation"],
            "settlement": {
                "settlement_kind": (
                    "owner_commit" if committed else "proven_no_effect"
                ),
                "evidence_digest": self._request_digest(result),
            },
        }
        try:
            self._cell_admission().release(release)
        except CellAdmissionError as error:
            raise self._admission_error(error) from error

    def _execution(self) -> ExecutionPort:
        if self.execution is None:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "native execution owner is not configured",
            )
        return self.execution

    def execution_diagnostics(self) -> dict[str, Any]:
        """Return the explicit, cold-path aggregate owner projection."""
        return self._execution().diagnostics()

    def execution_identity(self) -> dict[str, Any]:
        return self._execution().identity()

    def execution_targets(self) -> dict[str, Any]:
        return self._execution().targets()

    def execution_control_state(self) -> dict[str, Any]:
        return self._execution().control_state()

    def execution_observation(self) -> dict[str, Any]:
        return self._execution().observation()

    def execution_read_io_signal(self, request: IoSignalReadRequest) -> dict[str, Any]:
        """One effect-free `query_target(kind=io_signal)` read."""
        return self._execution().query_target(request.target_query())

    def execution_set_mock_io_input(self, request: MockIoInputRequest) -> dict[str, Any]:
        """Set one installed Mock input; the host refuses every other source."""
        return self._execution().set_mock_io_input(request)

    def open_execution_observation_session(self) -> Any:
        return self._execution().open_observation_session()

    def execution_submit(
        self,
        request: OperationIntentRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().submit(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_preview_motion(
        self,
        request: OperationIntentRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().preview_motion(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_read_manual_handover(
        self,
        request: OperationIntentRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        response = self._execution().read_manual_handover(
            request, trusted_principal_id=attribution.principal_id
        )
        if response.get("ok") is not True:
            return response
        # Captured producer indices are private Runner cleanup input. The
        # browser needs only eligibility and the opaque current precondition,
        # not a potentially 100,000-node list on each control-state change.
        handover = response["handover"]
        return {
            **{key: value for key, value in response.items() if key != "handover"},
            "manual_handover": None if handover is None else handover["context"],
        }

    def execution_preview_capability_command(
        self,
        request: OperationIntentRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().preview_capability_command(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_read(self, operation_id: str) -> dict[str, Any]:
        return self._execution().read(operation_id)

    def execution_plan_pick(
        self,
        request: PickSequencePlanningRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().plan_pick(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_plan_place(
        self,
        request: PlaceSequencePlanningRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().plan_place(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_reset(
        self,
        request: OperationControlRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().reset(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_acknowledge_completed(
        self,
        request: OperationControlRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().acknowledge_completed(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_stop(
        self,
        request: StopIntentRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().stop(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_full_stop(
        self,
        request: FullStopIntentRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().full_stop(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_recognized_malformed_stop(
        self,
        *,
        request_id: str,
        recognized_route: str,
        reason: str,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().recognized_malformed_stop(
            request_id=request_id,
            recognized_route=recognized_route,
            reason=reason,
            trusted_principal_id=attribution.principal_id,
        )

    def execution_stop_fault(self) -> dict[str, Any]:
        return self._execution().stop_fault()

    def execution_recover_stop_fault(
        self,
        request: StopFaultRecoveryIntentRequest | GlobalIoRuleRecoveryIntentRequest,
    ) -> dict[str, Any]:
        return self._execution().recover_stop_fault(request)

    def scenario_force_close_arm(self, stop_generation: int) -> dict[str, Any]:
        """Item 37: the Kernel confirms the operator's Full Stop for Force close."""

        self._force_close_in_progress = False
        return self._execution().force_close(step="arm", stop_generation=stop_generation)

    def scenario_force_close_operations(
        self, context: CallerAttributionRequest
    ) -> dict[str, Any]:
        """Item 37: the Kernel step, then one History row of what stays unknown."""

        response = self._execution().force_close(step="close_operations")
        if response.get("ok") is not True:
            return response
        self._force_close_in_progress = True
        result = response["result"]
        record = {
            "schema_version": "manyforge.scenario_force_close.v1",
            "reason_code": "cell_force_closed_by_operator",
            "request_id": context.request_id,
            "source_label": context.source_label,
            "stop_generation": result["stop_generation"],
            "operations": result["operations"],
            "released_orphan_resources": result["released_orphan_resources"],
            # FIX1: the cell-wide motion block of an unresolved closure ended.
            "motion_block_cleared": result.get("motion_block_cleared") is True,
        }
        try:
            history = self.core.record_scenario_force_close(record)
        except HeadlessApiError as error:
            # The Kernel step already happened; a lost History row is logged.
            logging.getLogger(__name__).warning(
                "Force close History row was not recorded: %s", error.code
            )
            history = {"event_id": None, "recorded": False}
        return {**response, "history": history}

    def scenario_force_close_end(self) -> dict[str, Any]:
        """SX-616: the caller's Force close sequence ended; its exception ends."""

        self._force_close_in_progress = False
        return {"ok": True, "command": "force_close", "result": {"step": "end", "operations": []}}

    def execution_set_runtime_scale_control(
        self,
        request: RuntimeScaleControlRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().set_runtime_scale_control(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_get_runtime_scale_control(
        self, selector: RuntimeScaleControlSelectorRequest
    ) -> dict[str, Any]:
        return self._execution().get_runtime_scale_control(selector)

    def runtime(self) -> dict[str, Any]:
        snapshot = self.core.runtime_snapshot()
        if snapshot.get("schema_version") != RUNTIME_SNAPSHOT_VERSION:
            raise HeadlessApiError(
                "headless_runtime_projection_invalid",
                "runtime snapshot version is unsupported",
            )
        return snapshot

    def component_overview_facts(
        self,
        *,
        program_cycle: dict[str, Any] | None = None,
        cancelled: Callable[[], bool] | None = None,
    ) -> ComponentOverviewFacts:
        """Capture each owner exactly once for one Component Overview reduction."""

        projections: dict[str, dict[str, Any] | None] = {}
        errors: dict[str, dict[str, Any]] = {}

        def capture(name: str, callback) -> None:
            if cancelled is not None and cancelled():
                raise InterruptedError("control owner capture cancelled")
            try:
                value = callback()
                if not isinstance(value, dict):
                    raise HeadlessApiError(
                        f"{name}_projection_invalid",
                        f"{name.replace('_', ' ')} projection is not an object",
                    )
                projections[name] = value
            except HeadlessApiError as error:
                projections[name] = None
                errors[name] = {
                    "code": error.code,
                    "message": str(error),
                }

        capture("runtime", self.runtime)
        capture(
            "alarms",
            lambda: self.alarms(limit=32, ordering="attention_newest"),
        )
        capture("authoring_admission", self.authoring_admission)
        if program_cycle is None:
            capture("program_cycle", self.program_cycle_status)
        else:
            projections["program_cycle"] = program_cycle
        capture("applied_program", self.program_applied)
        capture("source_model", self.source_model)
        capture("execution_identity", self.execution_identity)
        capture("execution_targets", self.execution_targets)
        capture("execution_control", self.execution_control_state)
        return ComponentOverviewFacts(
            runtime=projections["runtime"],
            alarms=projections["alarms"],
            authoring_admission=projections["authoring_admission"],
            program_cycle=projections["program_cycle"],
            applied_program=projections["applied_program"],
            source_model=projections["source_model"],
            execution_identity_response=projections["execution_identity"],
            execution_targets_response=projections["execution_targets"],
            execution_control_response=projections["execution_control"],
            errors=errors,
        )

    def component_overview(self) -> dict[str, Any]:
        """Return one pure reduction over one exact read of each owner."""

        return self.component_overview_facts().reduce()

    def scene_resource_poses(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]:
        return self.manager.resolve_scene_resources(request)

    def _source_section(self, name: str) -> tuple[dict[str, Any], dict[str, Any]]:
        runtime = self.runtime()
        sections = runtime.get("sections")
        section = sections.get(name) if isinstance(sections, dict) else None
        if not isinstance(section, dict):
            raise HeadlessApiError(
                "headless_runtime_projection_invalid",
                f"runtime snapshot has no {name} source section",
            )
        payload = section.get("payload")
        if not isinstance(payload, dict):
            raise HeadlessApiError(
                f"{name}_source_unavailable",
                f"{name} source has no current or last-valid payload",
            )
        return runtime, section

    @staticmethod
    def _source_metadata(
        runtime: dict[str, Any],
        section: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "projection_generation": runtime.get("projection_generation"),
            "projection_owner_epoch": runtime.get("projection_owner_epoch"),
            "available": section.get("available"),
            "freshness": section.get("freshness"),
            "known_gaps": section.get("known_gaps"),
            "receiver_age_ns": section.get("receiver_age_ns"),
            "source_generation": section.get("source_generation"),
            "source_owner_epoch": section.get("source_owner_epoch"),
            "source_owner_id": section.get("source_owner_id"),
            "source_content_digest": section.get("source_content_digest"),
        }

    def canonical_empty_selection(self) -> dict[str, Any]:
        return self.manager.canonical_empty_selection()

    def manager_status(self) -> dict[str, Any]:
        runtime, section = self._source_section("manager")
        payload = section["payload"]
        if payload.get("api_version") != MANAGER_SOURCE_VERSION:
            raise HeadlessApiError(
                "manager_source_invalid",
                "Manager source payload version is unsupported",
            )
        active = payload.get("active")
        if not isinstance(active, dict) or not isinstance(
            active.get("canonical_empty"), bool
        ):
            raise HeadlessApiError(
                "manager_source_invalid",
                "Manager source active.canonical_empty must be a boolean",
            )
        return {
            "schema_version": "manyforge.manager_status_projection.v1",
            "source": self._source_metadata(runtime, section),
            "status": payload,
        }

    def manager_graph(self) -> dict[str, Any]:
        status = self.manager_status()
        active = status["status"].get("active")
        graph = active.get("cell_graph") if isinstance(active, dict) else None
        if not isinstance(graph, dict):
            raise HeadlessApiError(
                "manager_source_invalid",
                "Manager source has no active graph",
            )
        projection = {
            "schema_version": "manyforge.manager_graph_projection.v1",
            "source": status["source"],
            "graph_revision": active.get("graph_revision"),
            "retained_bundle_id": active.get("retained_bundle_id"),
            "cell_graph": graph,
        }
        # CP8 step 1: the Manager-derived simulator per ROS-hosted component.
        if "component_simulator_dependencies" in active:
            projection["component_simulator_dependencies"] = active[
                "component_simulator_dependencies"
            ]
        return projection

    def manager_diff(self) -> dict[str, Any]:
        status = self.manager_status()
        runtime = status["status"].get("runtime")
        if not isinstance(runtime, dict):
            raise HeadlessApiError(
                "manager_source_invalid",
                "Manager source has no runtime convergence projection",
            )
        return {
            "schema_version": "manyforge.manager_diff_projection.v1",
            "source": status["source"],
            "converged": runtime.get("converged"),
            "desired_units": runtime.get("desired_units"),
            "observed_units": runtime.get("observed_units"),
        }

    def skills(self) -> dict[str, Any]:
        environment, skills = _build_skills()
        selected = self.core.node_environment_projection()
        if (
            selected.get("schema_version") != "manyforge.node_environment_projection.v2"
            or selected.get("environment_digest") != environment.environment_digest
        ):
            raise HeadlessApiError(
                "skill_environment_unavailable",
                "Installed skills do not match the selected retained environment",
            )
        return {
            "schema_version": "manyforge.skill_catalog.v1",
            "environment_digest": environment.environment_digest,
            "retained_bundle_id": selected["retained_bundle_id"],
            "graph_revision": selected["graph_revision"],
            "source": deepcopy(selected["source"]),
            "skills": skills.artifacts(),
        }

    def expand_skill(self, request: SkillExpansionRequest) -> dict[str, Any]:
        selection = self.skills()
        if request.expected_environment_digest != selection["environment_digest"]:
            raise HeadlessApiError(
                "skill_environment_stale", "Skill expansion selected an old environment"
            )
        try:
            result = _build_skills()[1].expand(
                **request.model_dump(
                    exclude={
                        "schema_version",
                        "expected_environment_digest",
                    }
                )
            )
        except ValueError as error:
            raise HeadlessApiError("skill_expansion_invalid", str(error)) from error
        return {
            **result,
            "environment_digest": selection["environment_digest"],
            "retained_bundle_id": selection["retained_bundle_id"],
        }

    def node_environment(self) -> dict[str, Any]:
        projection = self.core.node_environment_projection()
        source = projection.get("source")
        if projection.get(
            "schema_version"
        ) != "manyforge.node_environment_projection.v2" or not isinstance(source, dict):
            raise HeadlessApiError(
                "node_environment_projection_invalid",
                "native core returned an unsupported node environment projection",
            )
        offers = _strip_palette_offers(projection)
        if (
            not isinstance(offers, list)
            or any(not isinstance(value, str) for value in offers)
            or offers != sorted(set(offers))
        ):
            raise HeadlessApiError(
                "node_environment_projection_invalid",
                "native core returned invalid Deployment capability offers",
            )
        if self.program_runner is None:
            return projection
        try:
            applied = self.program_applied()
            scenario_id = applied.get("scenario_id")
            if not isinstance(scenario_id, str):
                raise HeadlessApiError(
                    "program_applied_selection_invalid",
                    "Applied Program omitted its Scenario identity",
                )
        except HeadlessApiError as error:
            # Catalog authoring is independent of an Applied Program. Never
            # join a Runner offer when the local Applied identity is absent.
            # A non-executable Applied Program (H1a) is not a Runner source
            # either; authoring stays available and joins no Runner offer.
            if error.code in {
                "program_applied_selection_not_found",
                "program_applied_non_executable",
                "program_not_selected",
            }:
                return projection
            raise
        try:
            runner_projection = self.program_runner.node_availability(
                {"applied": applied}
            )
        except ProgramRunnerClientError:
            # The immutable catalog remains authorable while the independent
            # Runner is absent or incompatible. Its existing unavailable
            # projection is already fail-closed and carries no false claim.
            return projection
        if (
            not isinstance(runner_projection, dict)
            or set(runner_projection) != {"availability", "schema_version", "source"}
            or runner_projection.get("schema_version")
            != "manyforge.program_node_availability.v1"
            or not isinstance(runner_projection.get("source"), dict)
            or not isinstance(runner_projection.get("availability"), dict)
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned an invalid node-availability projection",
            )
        runner_source = runner_projection["source"]
        runner_availability = runner_projection["availability"]
        applied_source = applied.get("source")
        core_availability = projection.get("availability")
        implementations = runner_availability.get("implementations")
        core_implementations = (
            core_availability.get("implementations")
            if isinstance(core_availability, dict)
            else None
        )
        source_without_digest = {
            key: value
            for key, value in runner_source.items()
            if key != "content_digest"
        }
        try:
            expected_availability_digest = (
                "sha256:"
                + hashlib.sha256(
                    rfc8785.dumps(
                        {
                            "availability": runner_availability,
                            **source_without_digest,
                        }
                    )
                ).hexdigest()
            )
        except (rfc8785.CanonicalizationError, TypeError, ValueError):
            expected_availability_digest = None
        if (
            set(runner_source)
            != {
                "admission_generation",
                "applied_record_digest",
                "content_digest",
                "cycle_generation",
                "graph_revision",
                "node_environment_digest",
                "owner_epoch",
                "owner_id",
                "program_binding_id",
                "program_id",
                "program_revision",
                "retained_bundle_id",
                "scenario_id",
            }
            or runner_source.get("content_digest") != expected_availability_digest
            or not isinstance(applied_source, dict)
            or type(runner_source.get("admission_generation")) is not int
            or type(runner_source.get("cycle_generation")) is not int
            or runner_availability.get("schema_version")
            != "manyforge.node_execution_availability.v1"
            or not isinstance(implementations, list)
            or not isinstance(core_implementations, list)
            or len(implementations) != len(core_implementations)
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program node availability does not identify the active environment",
            )
        stale_fields = {
            field
            for field, expected in (
                ("graph_revision", projection.get("graph_revision")),
                (
                    "node_environment_digest",
                    projection.get("environment_digest"),
                ),
                ("retained_bundle_id", projection.get("retained_bundle_id")),
            )
            if runner_source.get(field) != expected
        }
        stale_fields.update(
            field
            for field in (
                "graph_revision",
                "node_environment_digest",
                "program_binding_id",
                "program_id",
                "program_revision",
                "retained_bundle_id",
                "scenario_id",
            )
            if runner_source.get(field) != applied.get(field)
        )
        if runner_source.get("applied_record_digest") != applied_source.get(
            "record_digest"
        ):
            stale_fields.add("applied_record_digest")
        if runner_availability.get("environment_digest") != projection.get(
            "environment_digest"
        ):
            stale_fields.add("availability.environment_digest")
        if runner_availability.get("program_binding_id") != runner_source.get(
            "program_binding_id"
        ):
            stale_fields.add("availability.program_binding_id")
        if stale_fields:
            ordered_stale_fields = sorted(stale_fields)
            raise HeadlessApiError(
                "program_node_availability_stale",
                "Program node availability has not reached the active environment: "
                + ", ".join(ordered_stale_fields),
                details={
                    "mismatched_fields": ordered_stale_fields,
                    "node_environment_digest": {
                        "active": projection.get("environment_digest"),
                        "applied": applied.get("node_environment_digest"),
                        "runner": runner_source.get("node_environment_digest"),
                    },
                },
            )
        projected_rows: list[dict[str, Any]] = []
        for runner_row, core_row in zip(implementations, core_implementations):
            if (
                not isinstance(runner_row, dict)
                or not isinstance(core_row, dict)
                or any(
                    runner_row.get(field) != core_row.get(field)
                    for field in (
                        "adapter_build_qualified",
                        "authoring_available",
                        "engine_id",
                        "executable_package_id",
                        "implementation_id",
                        "implementation_identity_resolved",
                        "node_id",
                        "restoration_gates",
                    )
                )
            ):
                raise HeadlessApiError(
                    "program_runner_response_invalid",
                    "Program node availability disagrees with the active registry",
                )
            projected_rows.append(
                {field: runner_row[field] for field in core_row if field != "reasons"}
            )
        environment = _palette_environment()
        if environment.environment_digest != projection["environment_digest"]:
            raise HeadlessApiError(
                "program_node_availability_stale",
                "Installed palette catalog does not match the active environment",
            )
        catalog = environment.catalog
        offer_set = set(offers)
        required_nodes = set(runner_availability["required_node_ids"])
        model_configured = _palette_model_configured(applied)
        palette_reasons: set[str] = set()
        for row in projected_rows:
            entry = catalog.get(row["node_id"])
            missing = _palette_missing_capability(entry, offer_set)
            reasons = []
            if missing is not None:
                reasons.append({
                    "code": "capability_not_offered",
                    "message": f"No component offers {missing}",
                    "details": {"capability": missing},
                })
            if (
                entry.execution.owner_api == "assistant.model_request"
                and not model_configured
            ):
                reasons.append({
                    "code": "model_not_configured",
                    "message": "No model is configured",
                    "details": {},
                })
            row["reasons"] = reasons
            if reasons:
                row["binding_available"] = False
                row["execution_available"] = False
                row["reason_codes"] = sorted(
                    set(row["reason_codes"]) | {reason["code"] for reason in reasons}
                )
                if row["node_id"] in required_nodes:
                    palette_reasons.update(reason["code"] for reason in reasons)
        projection["availability"] = {
            "binding_context": "applied",
            "execution_available": (
                runner_availability.get("execution_available") and not palette_reasons
            ),
            "implementations": projected_rows,
            "manager_runtime_converged": core_availability.get(
                "manager_runtime_converged"
            ),
            "portable_authoring_available": runner_availability.get(
                "portable_authoring_available"
            ),
            "reason_codes": sorted(
                set(runner_availability.get("reason_codes", [])) | palette_reasons
            ),
            "restoration_gates": sorted(
                {
                    gate
                    for row in projected_rows
                    for gate in row.get("restoration_gates", [])
                    if isinstance(gate, str)
                }
            ),
        }
        return projection

    def source_model(self) -> dict[str, Any]:
        projection = self.core.source_model_projection()
        source = projection.get("source")
        if projection.get(
            "schema_version"
        ) != "manyforge.source_model_projection.v3" or not isinstance(source, dict):
            raise HeadlessApiError(
                "source_model_projection_invalid",
                "native core returned an unsupported source model projection",
            )
        return projection

    def source_model_context(self) -> dict[str, Any]:
        context = self.core.source_model_context()
        if (
            not isinstance(context, dict)
            or set(context) != {"node_environment", "schema_version", "source_model"}
            or context.get("schema_version") != SOURCE_MODEL_CONTEXT_VERSION
            or not isinstance(context.get("node_environment"), dict)
            or not isinstance(context.get("source_model"), dict)
            or context["node_environment"].get("schema_version")
            != "manyforge.node_environment_projection.v2"
            or context["source_model"].get("schema_version")
            != "manyforge.source_model_projection.v3"
        ):
            raise HeadlessApiError(
                "source_model_context_invalid",
                "native core returned an unsupported source-model context",
            )
        _strip_palette_offers(context["node_environment"])
        return context

    def assistant_mode_context(self, mode_id: str) -> dict[str, Any]:
        context = self.core.assistant_mode_context(mode_id)
        if (
            not isinstance(context, dict)
            or set(context)
            != {
                "assistant_mode",
                "node_environment",
                "schema_version",
                "source_model",
            }
            or context.get("schema_version") != ASSISTANT_MODE_CONTEXT_VERSION
            or not isinstance(context.get("assistant_mode"), dict)
            or not isinstance(context.get("node_environment"), dict)
            or not isinstance(context.get("source_model"), dict)
            or context["assistant_mode"].get("schema_version")
            != "manyforge.assistant_mode_projection.v1"
            or context["assistant_mode"].get("mode") != mode_id
            or context["node_environment"].get("schema_version")
            != "manyforge.node_environment_projection.v2"
            or context["source_model"].get("schema_version")
            != "manyforge.source_model_projection.v3"
        ):
            raise HeadlessApiError(
                "assistant_mode_context_invalid",
                "native core returned an unsupported assistant-mode context",
            )
        _strip_palette_offers(context["node_environment"])
        return context

    def program_applied(self) -> dict[str, Any]:
        projection = self.core.applied_program_selection()
        if not isinstance(projection, dict):
            raise HeadlessApiError(
                "program_applied_selection_invalid",
                "native core returned an invalid Applied Program projection",
            )
        try:
            validate_applied_program_selection(projection)
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessApiError(
                "program_applied_selection_invalid",
                "native core returned an unsupported Applied Program selection",
            ) from error
        return projection

    def program_applied_status(self) -> dict[str, Any]:
        """Derived executable/non-executable status of the Applied Program.

        Read on demand from the durable active Apply; for a non-executable
        Program it carries the preserved authored Program and the named
        unresolved references, and never a binding.
        """

        projection = self.core.applied_program_status()
        if not isinstance(projection, dict):
            raise HeadlessApiError(
                "program_applied_selection_invalid",
                "native core returned an unsupported Applied Program status",
            )
        executable = projection.get("executable")
        try:
            origin = validate_prepared_activation_origin(projection.get("origin"))
        except ProgramBindingTransportError as error:
            raise HeadlessApiError("program_applied_selection_invalid", "native core returned an invalid Applied origin") from error
        expected_fields = {
            "deployment_artifact_id",
            "deployment_revision",
            "executable",
            "graph_revision",
            "message",
            "non_executable",
            "program",
            "program_binding_id",
            "program_id",
            "program_revision",
            "record_digest",
            "retained_bundle_id",
            "origin",
            "schema_version",
        }
        if (
            set(projection) != expected_fields
            or projection.get("schema_version") != "manyforge.applied_program_status.v2"
            or type(executable) is not bool
            or (projection.get("program_binding_id") is None) is executable
            or (
                projection.get("program_id") is None
                and (
                    executable or projection.get("program_revision") is not None
                    or projection.get("non_executable") is not None
                    or projection.get("program") is not None
                )
            )
            or (
                projection.get("program_id") is not None
                and (
                    (projection.get("non_executable") is None) is not executable
                    or (projection.get("program") is None) is not executable
                )
            )
            or not _bounded_text(
                projection.get("deployment_artifact_id"), maximum_bytes=256
            )
            or not isinstance(projection.get("deployment_revision"), str)
            or not _sha256_identity(projection["deployment_revision"])
            or (origin["kind"] == "sealed_canonical_empty" and (
                projection.get("program_id") is not None or executable
                or any(origin[field] != projection.get(field) for field in (
                    "deployment_artifact_id", "deployment_revision", "retained_bundle_id", "graph_revision",
                ))
            ))
        ):
            raise HeadlessApiError(
                "program_applied_selection_invalid",
                "native core returned an unsupported Applied Program status",
            )
        return projection

    def committed_scenario_activation(self) -> dict[str, Any]:
        """Manager's current committed Scenario activation and its history.

        The Program runner reconciles its persistent-key register against it;
        Draft edits and Applies that never published are never part of it.
        """

        projection = self.core.committed_scenario_activation()
        try:
            return validate_committed_scenario_activation(projection)
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessApiError(
                "program_activation_invalid",
                "native core returned an unsupported committed Scenario activation",
            ) from error

    def notify_committed_apply(self) -> None:
        """Reconcile values and initialize Scene only for an explicit Apply purpose.

        Repeated status reads do not repeat placement or retry a failed
        placement. The existing idle Reset with reload is the explicit retry path.
        The attempt latch is memory-only: a replacement Headless instance may
        attempt the current initializing Apply again on its next notification.
        """

        if self.program_runner is None:
            return
        try:
            self.program_runner.reconcile_persistent_values()
        except ProgramRunnerClientError as error:
            logging.getLogger(__name__).warning(
                "Program runner did not reconcile persistent values after a "
                "committed Apply: %s: %s",
                error.code,
                error,
            )
        try:
            committed = self.committed_scenario_activation()
            applied = self.program_applied_status()
            if (
                committed["scene_initialization"] != "initialize"
                or applied["origin"]["kind"] != "scenario"
                or applied["program_id"] is None
                or applied["record_digest"] != committed["record_digest"]
            ):
                return
            identity = (
                committed["apply_id"], committed["apply_generation"],
                committed["record_digest"],
            )
            with self._apply_scene_completion_lock:
                if self._apply_scene_attempted == identity:
                    return
                self._apply_scene_attempted = identity
            cycle = self.program_cycle_status()
            admission = self.authoring_admission()
            # The existing preparation path rechecks the current selection
            # and admission before the Runner performs any Scene effect.
            self.program_scene_prepare(ProgramScenePreparationRequest(
                request_id="apply-scene." + identity[2][7:],
                request_owner_epoch=cycle["program_runner_owner_epoch"],
                source_label="headless_apply_completion",
                expected_applied_record_digest=identity[2],
                expected_admission_generation=admission["admission_generation"],
            ))
        except (HeadlessApiError, ProgramRunnerClientError) as error:
            logging.getLogger(__name__).warning(
                "Program scene preparation after committed Apply failed: %s: %s",
                error.code,
                error,
            )

    def follow_committed_apply(self, apply_id: str, apply_generation: int) -> None:
        """Wake the Program runner for an Apply observed committed through a
        status read, once Manager's published source names it.

        Manager answers Apply status from its journal and publishes its source
        just after, so a reader of the source can briefly still see the Apply
        in progress. The follower waits that out (bounded) instead of waking
        the runner into a refusal; a repeated identity is followed once.
        """

        if self.apply_follower is not None:
            self.apply_follower.follow(apply_id, apply_generation)

    def program_model_attempt(
        self,
        request: ProgramModelAttemptRequest,
    ) -> dict[str, Any]:
        return self.core.commit_program_model_attempt(request.model_dump(mode="json"))

    def _program_runner(self) -> ProgramRunnerPort:
        if self.program_runner is None:
            raise HeadlessApiError(
                "program_runner_unavailable",
                "independent Program runner is not configured",
            )
        return self.program_runner

    def program_diagnostic_health(self) -> dict[str, Any]:
        try:
            return self._program_runner().diagnostic_health()
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    @staticmethod
    def _runner_error(error: ProgramRunnerClientError) -> HeadlessApiError:
        return HeadlessApiError(
            error.code,
            str(error),
            details=error.details,
            producer=error.producer,
        )

    @staticmethod
    def _validate_program_run_snapshot(
        projection: dict[str, Any],
        *,
        expected_program_run_id: str | None = None,
        expected_owner_epoch: str | None = None,
    ) -> dict[str, Any]:
        expected_fields = {
            "active_operation_count",
            "capture",
            "details",
            "message",
            "node_status_revision",
            "node_statuses",
            "origin",
            "owner_epoch",
            "owner_id",
            "reason_code",
            "run_revision",
            "schema_version",
            "state",
            "tree_status",
        }
        capture_fields = {
            "applied_record_digest",
            "graph_revision",
            "node_environment_digest",
            "node_policies",
            "node_policy_digest",
            "owner_identities",
            "program_binding_id",
            "program_id",
            "program_revision",
            "program_run_id",
            "retained_bundle_id",
            "source_document_digest",
            "scenario_id",
            "schema_version",
        }
        capture = projection.get("capture")
        owner_id = projection.get("owner_id")
        owner_epoch = projection.get("owner_epoch")
        state = projection.get("state")
        active_count = projection.get("active_operation_count")
        reason = projection.get("reason_code")
        node_statuses = projection.get("node_statuses")
        owner_identities = (
            capture.get("owner_identities") if isinstance(capture, dict) else None
        )
        node_policies = (
            capture.get("node_policies") if isinstance(capture, dict) else None
        )
        try:
            calculated_policy_digest = (
                "sha256:" + hashlib.sha256(rfc8785.dumps(node_policies)).hexdigest()
                if isinstance(node_policies, list)
                else None
            )
        except (rfc8785.CanonicalizationError, TypeError, ValueError):
            calculated_policy_digest = None
        if (
            # `operation_failures` is always present (empty when none).
            set(projection) != expected_fields | {"operation_failures"}
            or projection.get("schema_version") != PROGRAM_RUN_STATUS_VERSION
            or not _bounded_text(owner_id, maximum_bytes=512)
            or not _bounded_text(owner_epoch, maximum_bytes=512)
            or type(projection.get("run_revision")) is not int
            or not 1 <= projection["run_revision"] <= 9_007_199_254_740_991
            or not isinstance(state, str)
            or state not in _PROGRAM_RUN_STATES
            or not isinstance(projection.get("tree_status"), str)
            or projection.get("tree_status")
            not in {"invalid", "running", "success", "failure"}
            or type(projection.get("node_status_revision")) is not int
            or not 1 <= projection["node_status_revision"] <= 9_007_199_254_740_991
            or not isinstance(node_statuses, dict)
            or len(node_statuses) > 100_000
            or any(
                not _bounded_text(instance_id, maximum_bytes=512)
                or not isinstance(status, str)
                or status
                not in {"idle", "running", "held", "success", "failure", "interrupted"}
                for instance_id, status in node_statuses.items()
            )
            or (
                active_count is not None
                and (type(active_count) is not int or not 0 <= active_count <= 4_096)
            )
            or (reason is not None and not _bounded_text(reason, maximum_bytes=256))
            or not isinstance(projection.get("message"), str)
            or len(projection["message"].encode("utf-8")) > 4_096
            or not isinstance(capture, dict)
            or set(capture) != capture_fields
            or capture.get("schema_version") != PROGRAM_RUN_CAPTURE_VERSION
            or not _bounded_text(capture.get("program_run_id"), maximum_bytes=512)
            or not _bounded_text(capture.get("scenario_id"), maximum_bytes=128)
            or not _bounded_text(capture.get("program_id"), maximum_bytes=512)
            or any(
                not _sha256_identity(capture.get(field_name))
                for field_name in (
                    "applied_record_digest",
                    "source_document_digest",
                    "program_revision",
                    "program_binding_id",
                    "retained_bundle_id",
                    "graph_revision",
                    "node_environment_digest",
                    "node_policy_digest",
                )
            )
            or not isinstance(node_policies, list)
            or len(node_policies) > 100_000
            or calculated_policy_digest != capture.get("node_policy_digest")
            or not isinstance(owner_identities, list)
            or not 1 <= len(owner_identities) <= 4_096
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned an invalid run status",
            )
        try:
            validate_failure_details_and_origin(
                projection["details"], projection["origin"]
            )
            if state == "succeeded" and projection["origin"] is not None:
                raise ValueError("a succeeded run carries no failure origin")
            program_operation_failures(projection)
        except ValueError as exc:
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned invalid run failure details or origin",
            ) from exc
        roles: list[str] = []
        runner_identity: dict[str, Any] | None = None
        for identity in owner_identities:
            if (
                not isinstance(identity, dict)
                or set(identity) != {"owner_epoch", "owner_id", "owner_role"}
                or not _bounded_text(identity.get("owner_role"), maximum_bytes=128)
                or not _bounded_text(identity.get("owner_id"), maximum_bytes=512)
                or not _bounded_text(identity.get("owner_epoch"), maximum_bytes=512)
            ):
                raise HeadlessApiError(
                    "program_runner_response_invalid",
                    "Program runner returned an invalid owner identity",
                )
            roles.append(identity["owner_role"])
            if identity["owner_role"] == "program_runner":
                runner_identity = identity
        if (
            roles != sorted(set(roles))
            or runner_identity is None
            or runner_identity["owner_id"] != owner_id
            or runner_identity["owner_epoch"] != owner_epoch
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program-run status and captured owner identities disagree",
            )
        if (
            expected_program_run_id is not None
            and capture["program_run_id"] != expected_program_run_id
        ) or (expected_owner_epoch is not None and owner_epoch != expected_owner_epoch):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned a divergent run identity",
            )
        return projection

    def program_runs(self, *, active_only: bool, limit: int) -> dict[str, Any]:
        if type(active_only) is not bool or type(limit) is not int:
            raise HeadlessApiError(
                "program_run_index_request_invalid",
                "Program-run index filter is invalid",
            )
        if not 1 <= limit <= 4_096:
            raise HeadlessApiError(
                "program_run_index_request_invalid",
                "Program-run index limit is outside its bound",
            )
        try:
            projection = self._program_runner().runs(
                active_only=active_only,
                limit=limit,
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        runs = projection.get("runs")
        owner_id = projection.get("owner_id")
        owner_epoch = projection.get("owner_epoch")
        if (
            set(projection)
            != {
                "active_only",
                "owner_epoch",
                "owner_id",
                "runs",
                "schema_version",
                "truncated",
            }
            or projection.get("schema_version") != PROGRAM_RUN_INDEX_VERSION
            or projection.get("active_only") is not active_only
            or not _bounded_text(owner_id, maximum_bytes=512)
            or not _bounded_text(owner_epoch, maximum_bytes=512)
            or not isinstance(runs, list)
            or len(runs) > limit
            or type(projection.get("truncated")) is not bool
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned an invalid run index",
            )
        program_run_ids: list[str] = []
        for row in runs:
            if not isinstance(row, dict):
                raise HeadlessApiError(
                    "program_runner_response_invalid",
                    "Program runner returned an invalid run index member",
                )
            self._validate_program_run_snapshot(
                row,
                expected_owner_epoch=(
                    owner_epoch
                    if row.get("state") in _ACTIVE_PROGRAM_RUN_STATES
                    else None
                ),
            )
            program_run_ids.append(row["capture"]["program_run_id"])
            if row["owner_id"] != owner_id or (
                active_only and row["state"] not in _ACTIVE_PROGRAM_RUN_STATES
            ):
                raise HeadlessApiError(
                    "program_runner_response_invalid",
                    "Program-run index and member identities disagree",
                )
        if program_run_ids != sorted(set(program_run_ids)):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program-run index must be strictly identity-sorted and unique",
            )
        return projection

    def program_run_start(
        self,
        request: ProgramRunStartRequest,
    ) -> dict[str, Any]:
        applied = self.program_applied()
        source = applied.get("source")
        if (
            not isinstance(source, dict)
            or source.get("record_digest") != request.expected_applied_record_digest
            or applied.get("scenario_id") != request.scenario_id
            or applied.get("source_document_digest")
            != request.source_document_digest
        ):
            raise HeadlessApiError(
                "program_run_applied_selection_stale",
                "Applied Program changed before Run admission",
            )
        if applied.get("program_revision") != request.draft_program_revision:
            raise HeadlessApiError(
                "program_run_draft_applied_mismatch",
                "authoritative Draft and Applied Program revisions differ",
            )
        # Verify the exact Applied/Runner/environment join before the effect.
        # A transition may leave a well-formed older Runner offer briefly
        # visible; rejecting it here keeps retry entirely pre-admission.
        current_environment = self.node_environment()
        try:
            projection = self._program_runner().start_run(
                request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        snapshot = self._validate_program_run_snapshot(projection)
        capture = snapshot["capture"]
        binding = applied.get("program_binding")
        applied_node_policies = (
            binding.get("node_policies") if isinstance(binding, dict) else None
        )
        if (
            capture["applied_record_digest"] != source.get("record_digest")
            or capture["node_policies"] != applied_node_policies
            or capture["node_environment_digest"]
            != current_environment.get("environment_digest")
            or any(
                capture[capture_field] != applied.get(applied_field)
                for capture_field, applied_field in (
                    ("scenario_id", "scenario_id"),
                    ("source_document_digest", "source_document_digest"),
                    ("program_id", "program_id"),
                    ("program_revision", "program_revision"),
                    ("program_binding_id", "program_binding_id"),
                    ("retained_bundle_id", "retained_bundle_id"),
                    ("graph_revision", "graph_revision"),
                )
            )
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner started another Applied Program selection",
            )
        return snapshot

    def program_scene_prepare(
        self, request: ProgramScenePreparationRequest
    ) -> dict[str, Any]:
        applied = self.program_applied()
        source = applied.get("source")
        if (
            not isinstance(source, dict)
            or source.get("record_digest") != request.expected_applied_record_digest
        ):
            raise HeadlessApiError(
                "program_run_applied_selection_stale",
                "Applied Program changed before scene preparation",
            )
        try:
            result = self._program_runner().prepare_applied_scene(
                request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        proof = result.get("prepared_scene")
        generation = result.get("admission_generation")
        if (
            set(result)
            != {
                "admission_generation",
                "applied_record_digest",
                "prepared_scene",
                "program_revision",
                "schema_version",
                "status",
            }
            or result.get("schema_version") != "manyforge.program_scene_preparation.v1"
            or result.get("status") != "prepared"
            or result.get("applied_record_digest")
            != request.expected_applied_record_digest
            or result.get("program_revision") != applied.get("program_revision")
            or type(generation) is not int
            or not 1 <= generation <= 9_007_199_254_740_991
            or not isinstance(proof, dict)
            or proof.get("schema_version") != "manyforge.prepared_program_scene.v1"
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned an invalid scene-preparation result",
            )
        return result

    def program_run_read(self, program_run_id: str) -> dict[str, Any]:
        try:
            projection = self._program_runner().read_run(program_run_id)
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
        )

    def program_run_stop(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().stop_run(
                program_run_id,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        if projection.get("schema_version") == ("manyforge.program_stop_acceptance.v1"):
            if (
                set(projection)
                != {
                    "schema_version",
                    "program_run_id",
                    "cycle_generation",
                    "state",
                    "execution_control_revision",
                }
                or projection.get("program_run_id") != program_run_id
                or projection.get("state")
                not in {"stopping", "resetting", "terminal_awaiting_reset"}
                or type(projection.get("cycle_generation")) is not int
                or type(projection.get("execution_control_revision")) is not int
            ):
                raise HeadlessApiError(
                    "program_runner_response_invalid",
                    "Program runner returned an invalid Stop acceptance",
                )
            return dict(projection)
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_motion_stop(
        self,
        program_run_id: str,
        request: ProgramSelectionStopRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().stop_motion(
                program_run_id,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_motion_resume(
        self,
        program_run_id: str,
        request: ProgramSelectionResumeRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().resume_motion(
                program_run_id,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_motion_start(
        self,
        program_run_id: str,
        request: ProgramSelectionStartRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().start_motion(
                program_run_id,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_manual_handover(
        self,
        program_run_id: str,
        request: ProgramManualHandoverRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().prepare_manual_handover(
                program_run_id, request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_full_stop(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().full_stop(
                program_run_id,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_resume_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResumeRequest,
    ) -> dict[str, Any]:
        try:
            projection = self._program_runner().resume_full_stop(
                program_run_id,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_run_reset_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResetRequest,
    ) -> dict[str, Any]:
        """Adapt the retained REST shape to one owner-managed Reset command."""

        if request.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        reset = ProgramCycleResetRequest.model_validate(
            {
                "request_id": request.request_id,
                "request_owner_epoch": request.request_owner_epoch,
                "source_label": request.source_label,
                "correlation_id": request.correlation_id,
                "session_id": request.session_id,
                "client_sequence": request.client_sequence,
                "program_run_id": program_run_id,
                "expected_owner_epoch": request.expected_owner_epoch,
                "expected_cycle_generation": request.expected_cycle_generation,
                "expected_claim_generation": request.expected_claim_generation,
                "terminal_reference": None,
                "settlement_evidence_digest": None,
            }
        )
        return self.program_cycle_reset(reset)

    def program_cycle_status(self) -> dict[str, Any]:
        try:
            return self._program_runner().cycle_status()
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_cycle_reset(self, request: ProgramCycleResetRequest) -> dict[str, Any]:
        """Forward one Reset; Program Runner owns every lifecycle phase."""

        try:
            return self._program_runner().reset_cycle(request.model_dump(mode="json"))
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_runtime_controls(self, program_run_id: str) -> dict[str, Any]:
        try:
            return self._program_runner().runtime_controls(program_run_id)
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_node_correction_preview(
        self,
        program_run_id: str,
        request: ProgramNodeCorrectionPreviewRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        """Preview one held motion node with proposed inputs; no effect.

        The Program runner builds the request with the node's own execution
        builder; the native owner previews it through the same path as a
        manual move. The result is advisory: it grants no dispatch, Retry,
        Reset or Apply authority.

        Native preview planning is synchronous and uncancellable. At most one
        preview per exact (Program run, node attempt) is in flight; a
        concurrent duplicate is refused before any owner read, never queued.
        """

        attempt = (program_run_id, request.node_attempt_id)
        with self._node_previews_lock:
            if attempt in self._node_previews_in_flight:
                raise HeadlessApiError(
                    "program_node_preview_busy",
                    "A correction preview for this node attempt is already "
                    "running; retry after it completes",
                )
            self._node_previews_in_flight.add(attempt)
        try:
            return self._preview_node_correction(program_run_id, request, attribution)
        finally:
            with self._node_previews_lock:
                self._node_previews_in_flight.discard(attempt)

    def _preview_node_correction(
        self,
        program_run_id: str,
        request: ProgramNodeCorrectionPreviewRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        try:
            built = self._program_runner().preview_node_correction(
                program_run_id, request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        expected_keys = {
            "schema_version",
            *_PROGRAM_NODE_PREVIEW_IDENTITY_KEYS,
            "program_runner_owner_epoch",
            "run_revision",
            "retained_bundle_id",
            "graph_revision",
            "node_id",
            "proposed_paths",
            "parameters_digest",
            "target",
            "unavailable_reason",
            "operation",
        }
        operation = built.get("operation")
        unavailable = built.get("unavailable_reason")
        if (
            set(built) != expected_keys
            or built.get("schema_version") != PROGRAM_NODE_PREVIEW_INTENT_VERSION
            or any(
                built.get(key) != getattr(request, key)
                for key in _PROGRAM_NODE_PREVIEW_IDENTITY_KEYS
            )
            or built.get("program_runner_owner_epoch") != request.expected_owner_epoch
            or built.get("proposed_paths")
            != [proposal.path for proposal in request.proposed_inputs]
            or (operation is None) == (unavailable is None)
            or (
                unavailable is not None
                and (
                    not isinstance(unavailable, dict)
                    or set(unavailable) != {"code", "message"}
                    or not isinstance(unavailable.get("code"), str)
                    or not unavailable["code"]
                )
            )
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned an invalid node-correction preview",
            )
        result = {
            **{key: value for key, value in built.items() if key != "operation"},
            "schema_version": PROGRAM_NODE_PREVIEW_VERSION,
            "preview_check": None,
            "preview": None,
        }
        if operation is None:
            return result
        target = built.get("target")
        try:
            intent = OperationIntentRequest.model_validate(operation)
        except ValueError as error:
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner returned an invalid preview intent",
            ) from error
        participant = intent.participants[0] if len(intent.participants) == 1 else None
        if (
            participant is None
            or not isinstance(target, dict)
            or intent.request_context.request_id != request.request_id
            or intent.program_run_id != program_run_id
            or intent.program_id != request.program_id
            or intent.program_revision != request.program_revision
            or intent.program_binding_id != request.program_binding_id
            or intent.expected_retained_bundle_id != built["retained_bundle_id"]
            or intent.expected_graph_revision != built["graph_revision"]
            or intent.manual_handover is not None
            or participant.action_span is not None
            or participant.payload.kind != "motion"
            or participant.target_role_id != target.get("target_role_id")
            or participant.component_role_id != target.get("component_role_id")
            or participant.required_capability_id != "execution.trajectory.execute"
        ):
            raise HeadlessApiError(
                "program_runner_response_invalid",
                "Program runner preview intent differs from the named node",
            )
        result["preview_check"] = PROGRAM_NODE_PREVIEW_CHECK
        result["preview"] = self._execution().preview_motion(
            intent, trusted_principal_id=attribution.principal_id
        )
        return result

    def program_runtime_control_action(
        self,
        program_run_id: str,
        suffix: str,
        request: Any,
    ) -> dict[str, Any]:
        try:
            return self._program_runner().runtime_control_action(
                program_run_id,
                suffix,
                request.model_dump(mode="json"),
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_persistent_values(self) -> dict[str, Any]:
        try:
            return self._program_runner().persistent_values()
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_persistent_value_action(
        self, action: str, request: Any
    ) -> dict[str, Any]:
        try:
            return self._program_runner().persistent_value_action(
                action, request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def cycle_recovery_action(
        self, request: CycleRecoveryActionRequest
    ) -> dict[str, Any]:
        try:
            return self._program_runner().apply_recovery_action(
                request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def component_process_stop_offer(self, component_id: str) -> dict[str, Any]:
        return self.manager.component_process_stop_offer(component_id)

    def stop_component_process(self, request: Any) -> dict[str, Any]:
        return self.manager.stop_component_process(request)

    def component_process_stop_result(self, request: Any) -> dict[str, Any]:
        return self.manager.component_process_stop_result(request)

    def component_lifecycle_submit(self, request: Any) -> dict[str, Any]:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        payload = request.model_dump(
            mode="json", exclude={"confirmed"}, exclude_none=True
        )
        return self.cclc.submit_lifecycle(payload)

    def component_lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        return self.cclc.lifecycle_status(transaction_id)

    def component_lifecycle_request_result(self, request: Any) -> dict[str, Any] | None:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        return self.cclc.lifecycle_request_result(
            request.model_dump(mode="json", exclude={"confirmed"}, exclude_none=True)
        )

    def route_selection_submit(self, request: Any) -> dict[str, Any]:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        payload = request.model_dump(
            mode="json",
            exclude={"confirmed"},
            exclude_none=True,
        )
        return self.cclc.submit_route(payload)

    def route_selection_request_result(self, request: Any) -> dict[str, Any] | None:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        # Preserve the original intent, including absent fields on legacy
        # requests. Only Core may decide admission or replay membership.
        return self.cclc.route_request_result(
            request.model_dump(mode="json", exclude={"confirmed"}, exclude_none=True)
        )

    @staticmethod
    def _verified_viewer_asset(
        asset: ViewerModelAsset,
        *,
        expected_kind: str,
        expected_retained_bundle_id: str | None = None,
        expected_logical_uri: str | None = None,
    ) -> ViewerModelAsset:
        if (
            asset.asset_kind != expected_kind
            or asset.product_id != "cell_viewer_model"
            or asset.product_contract_id != "manyforge.viewer_model"
            or not asset.cell_id
            or not asset.graph_revision
            or not asset.viewer_model_revision
            or not asset.content_digest
            or asset.byte_size != len(asset.bytes)
        ):
            raise HeadlessApiError(
                "viewer_model_asset_invalid",
                "native core returned an incoherent viewer-model asset",
            )
        if expected_kind == "urdf":
            if asset.logical_uri is not None or asset.media_type != "application/xml":
                raise HeadlessApiError(
                    "viewer_model_asset_invalid",
                    "native core returned an invalid viewer URDF asset",
                )
        elif (
            asset.logical_uri != expected_logical_uri
            or asset.retained_bundle_id != expected_retained_bundle_id
        ):
            raise HeadlessApiError(
                "viewer_model_asset_invalid",
                "native core returned a different viewer resource",
            )
        return asset

    def viewer_model_urdf(self) -> ViewerModelAsset:
        return self._verified_viewer_asset(
            self.core.viewer_model_urdf(),
            expected_kind="urdf",
        )

    def viewer_model_resource(
        self,
        retained_bundle_id: str,
        logical_uri: str,
    ) -> ViewerModelAsset:
        return self._verified_viewer_asset(
            self.core.viewer_model_resource(retained_bundle_id, logical_uri),
            expected_kind="resource",
            expected_retained_bundle_id=retained_bundle_id,
            expected_logical_uri=logical_uri,
        )

    def assistant_mode(self, mode_id: str) -> dict[str, Any]:
        projection = self.core.assistant_mode_projection(mode_id)
        source = projection.get("source")
        if (
            projection.get("schema_version") != "manyforge.assistant_mode_projection.v1"
            or projection.get("mode") != mode_id
            or not isinstance(source, dict)
        ):
            raise HeadlessApiError(
                "assistant_mode_projection_invalid",
                "native core returned an unsupported assistant mode projection",
            )
        return projection

    def assistant_modes(self) -> dict[str, Any]:
        projection = self.core.assistant_mode_index()
        source = projection.get("source")
        if (
            projection.get("schema_version") != "manyforge.assistant_mode_index.v1"
            or not isinstance(projection.get("modes"), list)
            or not isinstance(source, dict)
        ):
            raise HeadlessApiError(
                "assistant_mode_projection_invalid",
                "native core returned an unsupported assistant mode index",
            )
        return projection

    def scene(self) -> dict[str, Any]:
        runtime = self.runtime()
        sections = runtime["sections"]
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "projection_generation": runtime["projection_generation"],
            "logical_scene": sections["logical_scene"],
            "scene_runtime": sections["scene_runtime"],
        }

    def alarms(
        self,
        *,
        limit: int | None = 32,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]:
        snapshot = self.core.alarm_snapshot(
            limit=limit,
            states=states,
            ordering=ordering,
        )
        return self._with_kernel_attention(snapshot)

    def _with_kernel_attention(self, snapshot: dict[str, Any]) -> dict[str, Any]:
        """Add the Kernel-held attention copy to an owner snapshot.

        The owner's `reconciled` reaches the Kernel only up to `P + D` later
        (spec 530 §11), and the Kernel admits by its held copy alone. The
        `kernel_attention` member names that copy (owner epoch, generation,
        `reconciled`, `active_configuration`) as the Kernel last acknowledged
        it, or null when none is known to be current, so a client can wait
        for the Kernel's admission state instead of retrying refusals.
        """
        source = self.kernel_attention_source
        held = None
        if source is not None:
            try:
                held = source()
            except Exception:  # noqa: BLE001 - presentation only; fail closed
                held = None
        return {**snapshot, "kernel_attention": held}

    def acknowledge_alarm(
        self,
        condition_id: str,
        request: AcknowledgeAlarmRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self.core.acknowledge_alarm(
            condition_id=condition_id,
            activation_id=request.activation_id,
            expected_record_revision=request.expected_record_revision,
            request_id=request.request_id,
            request_owner_epoch=request.request_owner_epoch,
            source_label=request.source_label,
            correlation_id=request.correlation_id,
            transport_kind="http",
            adapter_id="manyforge.headless.http.v1",
            assurance=attribution.assurance,
            trusted_principal_id=attribution.principal_id,
            reason=request.reason,
            confirmed=request.confirmed,
        )

    def acknowledge_all(
        self,
        request: AcknowledgeAllAlarmsRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self.core.acknowledge_all(
            members=[
                (item.condition_id, item.activation_id, item.record_revision)
                for item in request.members
            ],
            expected_generation=request.expected_generation,
            request_id=request.request_id,
            request_owner_epoch=request.request_owner_epoch,
            source_label=request.source_label,
            correlation_id=request.correlation_id,
            transport_kind="http",
            adapter_id="manyforge.headless.http.v1",
            assurance=attribution.assurance,
            trusted_principal_id=attribution.principal_id,
            reason=request.reason,
            confirmed=request.confirmed,
        )

    def history(
        self,
        query: HistorianQueryRequest,
    ) -> dict[str, Any]:
        arguments = query.native_arguments(now_utc_ns=time.time_ns())
        page = _history_page(
            self.core.history(**arguments), _HISTORY_SUMMARY_PAGE_VERSION
        )
        # Keep a relative window fixed across Older/Newer navigation. These
        # are the query's actual server UTC bounds, not another source clock.
        return {
            **page,
            "time_window": {
                key: None if arguments[key] is None else str(arguments[key])
                for key in ("start_utc_ns", "end_utc_ns")
            },
        }

    def history_detail(self, sequence: int) -> dict[str, Any]:
        return self.core.history_detail(sequence)

    def history_captures(self, query: HistorianCaptureRequest) -> dict[str, Any]:
        page = self.core.history_captures(
            before_capture_id=query.before_capture_id, limit=query.limit
        )
        captures = page.get("captures")
        if (
            page.get("schema_version") != "manyforge.historian_capture_page.v1"
            or not isinstance(captures, list)
            or len(captures) > query.limit
            or type(page.get("truncated")) is not bool
        ):
            raise HeadlessApiError(
                "historian_response_invalid",
                "native historian returned an invalid capture page",
            )
        return page

    def history_export(self, query: HistorianQueryRequest) -> dict[str, Any]:
        arguments = query.native_arguments(now_utc_ns=time.time_ns())
        page = _history_page(
            self.core.history_export(**arguments), "manyforge.historian_export_page.v1"
        )
        return {
            **page,
            "time_window": {
                key: None if arguments[key] is None else str(arguments[key])
                for key in ("start_utc_ns", "end_utc_ns")
            },
        }

    def program_run_admitted_speeds(self, program_run_id: str) -> dict[str, Any]:
        """Return the owner-admitted TCP speed of each robot's latest prepared motion.

        Execution evidence only: each row copies, unchanged, the Kernel's
        ``prepared_motion_diagnostics`` row journaled when the motion was
        prepared (requested, Deployment maximum and admitted speed). Nothing
        is computed here. The scan reads at most
        ``_ADMITTED_SPEED_HISTORY_PAGES`` newest history pages of this run, so
        a robot whose last prepared motion is older (or pruned) has no row.
        """

        rows: dict[tuple[str, str], dict[str, Any]] = {}
        before_sequence: int | None = None
        for _ in range(_ADMITTED_SPEED_HISTORY_PAGES):
            page = self.history_export(
                HistorianQueryRequest(
                    program_run_id=program_run_id,
                    event_types=["operation_prepared"],
                    before_sequence=before_sequence,
                )
            )
            for stored in page["events"]:  # newest first: the first row wins
                for row in _prepared_motion_admitted_speeds(stored, program_run_id):
                    key = (row["controllable_component_id"], row["command_target_id"])
                    rows.setdefault(key, row)
            if not page["truncated"]:
                break
            before_sequence = page["next_before_sequence"]
        return {
            "schema_version": "manyforge.program_run_admitted_speeds.v1",
            "program_run_id": program_run_id,
            "targets": [rows[key] for key in sorted(rows)],
        }

    def debug_export(
        self,
        query: HistorianQueryRequest,
        *,
        include_volatile_motion_trace: bool = True,
    ) -> dict[str, Any]:
        """Return the forensic bundle, optionally with the volatile motion trace.

        The historian bundle is unchanged. When requested, an optional
        ``volatile_motion_trace`` member is appended (see
        ``_attach_volatile_motion_trace``).
        """
        from .history_diagnostics import forensic_bundle

        page = self.history_export(query)
        events = list(page["events"])
        current = query.model_copy(
            update={
                "recent_seconds": None,
                **{
                    key: int(value) if value is not None else None
                    for key, value in page["time_window"].items()
                },
            }
        )
        for _ in range(7):
            if not page["truncated"] or page["next_before_sequence"] is None:
                break
            before = page["next_before_sequence"]
            if (
                not events
                or type(before) is not int
                or before != events[-1]["sequence"]
            ):
                raise HeadlessApiError(
                    "historian_response_invalid", "export cursor does not advance"
                )
            current = current.model_copy(update={"before_sequence": before})
            page = self.history_export(current)
            if page["events"] and page["events"][0]["sequence"] >= before:
                raise HeadlessApiError(
                    "historian_response_invalid",
                    "export cursor repeated a previous page",
                )
            events.extend(page["events"])
        bundle = forensic_bundle(
            {**page, "events": events},
            program_run_id=current.program_run_id,
            exported_at_ns=time.time_ns(),
        )
        if include_volatile_motion_trace:
            self._attach_volatile_motion_trace(bundle)
        return bundle

    def _attach_volatile_motion_trace(self, bundle: dict[str, Any]) -> None:
        """Append the optional diagnostic ``volatile_motion_trace`` member.

        Member shape:
        - ``{"available": true, "trace": <native trace, unchanged>}``, or
        - ``{"available": false, "reason": <stable code>}`` where reason is
          ``export_limit``, ``execution_unavailable``, the native/adapter
          ``HeadlessApiError`` code, or ``read_failed``.

        The trace receives only the HTTP budget the unchanged bundle leaves:
        ``min(3 MiB, MAX_HTTP_RESPONSE_BYTES - baseline - member overhead)``.
        If the final render still exceeds the limit the member becomes the
        ``export_limit`` marker; if even that does not fit, the member is
        omitted so the response equals the baseline. Trace goals are never
        pruned, and a failed diagnostic read never fails the export.
        """
        limit = MAX_HTTP_RESPONSE_BYTES
        baseline = _rendered_response_bytes(bundle)
        allowance = min(
            VOLATILE_MOTION_TRACE_MAX_BYTES,
            limit
            - baseline
            - len(_VOLATILE_MOTION_TRACE_MEMBER_TEXT.encode("utf-8"))
            - VOLATILE_MOTION_TRACE_MARGIN_BYTES,
        )
        if allowance < VOLATILE_MOTION_TRACE_MIN_ALLOWANCE_BYTES:
            member = _trace_unavailable("export_limit")
        elif self.execution is None:
            member = _trace_unavailable("execution_unavailable")
        else:
            try:
                member = {
                    "available": True,
                    "trace": self.execution.read_volatile_motion_trace(allowance),
                }
            except HeadlessApiError as error:
                code = error.code
                member = _trace_unavailable(
                    code
                    if isinstance(code, str) and 0 < len(code) <= 128
                    else "read_failed"
                )
            except Exception:  # Diagnostics never fail the export.
                logging.getLogger(__name__).warning(
                    "volatile motion trace read failed", exc_info=True
                )
                member = _trace_unavailable("read_failed")
        for candidate in (member, _trace_unavailable("export_limit")):
            bundle[VOLATILE_MOTION_TRACE_MEMBER] = candidate
            if _rendered_response_bytes(bundle) <= limit:
                return
        del bundle[VOLATILE_MOTION_TRACE_MEMBER]

    def record_client_incident(self, **kwargs: Any) -> dict[str, Any]:
        """Non-blockingly enqueue one strict client-incident transition."""

        return self.core.record_client_incident(**kwargs)

    def historian_reconcile(
        self,
        request: HistorianReconcileRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self.core.reconcile_historian_health(
            expected_generation=request.expected_health_generation,
            request_id=request.request_id,
            request_owner_epoch=request.request_owner_epoch,
            source_label=request.source_label,
            correlation_id=request.correlation_id,
            transport_kind="http",
            adapter_id="manyforge.headless.http.v1",
            assurance=attribution.assurance,
            trusted_principal_id=attribution.principal_id,
        )

    def manager_deployment_candidate_preview(
        self, request: ManagerDeploymentCandidatePreviewRequest
    ) -> dict[str, Any]:
        if self.activation_preparer is None:
            raise HeadlessApiError(
                "activation_preparer_unavailable",
                "Deployment preview requires an exact prepared activation",
            )
        prepared = self.activation_preparer.prepared_value(
            request.prepared_activation_id
        )
        if (
            prepared is None
            or prepared.get("deployment_artifact_id")
            != request.deployment_artifact_id
            or prepared.get("deployment_revision") != request.deployment_revision
            or prepared.get("retained_bundle_id")
            != request.candidate_retained_bundle_id
            or prepared.get("graph_revision") != request.candidate_graph_revision
        ):
            raise HeadlessApiError(
                "prepared_activation_mismatch",
                "Deployment preview does not match the retained prepared activation",
            )
        return self.manager.preview_deployment_candidate(request)

    def _scenario_closing(self) -> bool:
        """The Kernel reports authoring closed *only* by the closing scope.

        `stop_fault_recovery_closing` on the Stop-fault offer means every other
        admission condition holds (SX-370); `_not_ready` or no reason does not.
        A latch whose Stop has not certified yet is a typed wait.
        """

        try:
            response = self._execution().stop_fault()
        except HeadlessApiError:
            return False
        offer = response.get("stop_fault") if isinstance(response, Mapping) else None
        if not isinstance(offer, Mapping):
            return False
        latch = offer.get("latch")
        if isinstance(latch, Mapping) and latch.get("stop_certificate") is None:
            raise HeadlessApiError(
                "authoring_not_ready",
                "The cell Stop is still settling; retry Close in a moment",
            )
        return offer.get("recovery_unavailable_reason") == "stop_fault_recovery_closing"

    def _require_no_carried_objects(self) -> None:
        """Close refuses while a part is attached: the caller detaches first.

        The canonical-empty Apply preserves the Scene, so an attachment would
        outlive its carrier and could no longer be detached.
        """

        _runtime, section = self._source_section("logical_scene")
        snapshot = section["payload"].get("snapshot")
        attachments = (
            snapshot.get("carried_object_attachments")
            if isinstance(snapshot, Mapping) else None
        )
        if not attachments:
            return
        raise HeadlessApiError(
            "scenario_close_attachment_present",
            "Detach every carried object before Close",
            details={"carried_objects": [
                {
                    key: row.get(key)
                    for key in (
                        "scene_resource_id",
                        "controllable_component_id",
                        "command_target_id",
                    )
                }
                for row in attachments
                if isinstance(row, Mapping)
            ]},
        )

    def prepare_activation(
        self, request: ActivationPreparationRequest | CanonicalEmptyActivationPreparationRequest
    ) -> dict[str, Any]:
        if self.activation_preparer is None:
            raise HeadlessApiError(
                "activation_preparer_unavailable",
                "Activation preparation is not configured",
            )
        canonical_empty = isinstance(request, CanonicalEmptyActivationPreparationRequest)
        # Item 38: one preparation consumes the Force close exception, whatever
        # its outcome (SX-616).
        force_close_bypass = canonical_empty and self._force_close_in_progress
        self._force_close_in_progress = False

        def admitted() -> bool:
            try:
                admission = self._cell_admission().get()
            except CellAdmissionError as error:
                raise self._admission_error(error) from error
            # The request's admission generation is evidence only: preparation
            # follows the current admission facts.
            if admission.get("authoring_allowed") is True:
                return True
            # D6 (SX-366/370): while Scenario Close runs, authoring stays
            # closed; only the verified canonical-empty preparation, from any
            # caller, is admitted, and only when the Kernel reports that the
            # closing scope is the sole blocker.
            return canonical_empty and self._scenario_closing()

        if not admitted():
            raise HeadlessApiError(
                "authoring_not_ready",
                "Activation preparation requires the current idle admission",
            )
        if canonical_empty:
            # Item 38 (b): during Force close a part whose detach was refused
            # (the robot's process is gone) may stay attached.
            if not force_close_bypass:
                self._require_no_carried_objects()
            selection = self.manager.canonical_empty_selection()
            origin = request.origin.model_dump(mode="json")
            selected_origin = {
                "kind": "sealed_canonical_empty", "cell_id": selection["cell_id"],
                "deployment_artifact_id": selection["deployment_artifact_id"],
                "deployment_revision": selection["deployment_revision"],
                "retained_bundle_id": selection["candidate_retained_bundle_id"],
                "graph_revision": selection["candidate_graph_revision"],
                "admission_digest": selection["admission_digest"],
            }
            if selection["request_owner_epoch"] != request.request_owner_epoch or selected_origin != origin:
                raise HeadlessApiError("stale_generation", "Canonical-empty selection changed before preparation")
            receipt = self.activation_preparer.prepare_canonical_empty(selection)
            try:
                after = self.manager.canonical_empty_selection()
                if after != selection or not admitted():
                    raise HeadlessApiError("stale_generation", "Canonical-empty selection or admission changed during preparation")
            except Exception:
                self.activation_preparer.release(receipt["prepared_activation_id"])
                raise
            return {"prepared_activation": receipt}
        return {
            "prepared_activation": self.activation_preparer.prepare(
                source_document_digest=request.source_document_digest,
                scenario_id=request.scenario_id,
                project_path=request.project_path,
                captured_input_digest=request.captured_input_digest,
                deployment=request.deployment,
                model_source_closure=request.model_source_closure,
                program=request.program,
                program_role_assignment=request.program_role_assignment,
            )
        }

    def manager_apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        response = self.manager.apply(request)
        apply_id = response.get("apply_id")
        apply_generation = response.get("apply_generation")
        if (
            self.apply_follower is not None
            and response.get("ok") is True
            and isinstance(apply_id, str)
            and type(apply_generation) is int
        ):
            self.apply_follower.follow(apply_id, apply_generation)
        return response

    def manager_apply_status(
        self, apply_id: str, apply_generation: int
    ) -> dict[str, Any]:
        return self.manager.observe_apply(apply_id, apply_generation)

    def manager_apply_acknowledge(
        self,
        apply_id: str,
        apply_generation: int,
        request_context: ManagerApplyAcknowledgementContext,
    ) -> dict[str, Any]:
        return self.manager.acknowledge_apply(
            apply_id, apply_generation, request_context
        )

    def scene_commit(
        self,
        request: SceneTransactionRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        authored = request.operation.kind in {
            "add_object",
            "upsert_object",
            "remove_object",
            "update_object_pose",
            "set_collision_policy",
        }
        if request.program_node_evidence is not None:
            if (
                request.expected_admission_generation is None
                or request.program_reset_scene_authorization is not None
            ):
                raise HeadlessApiError(
                    "invalid_request",
                    "runtime Scene transaction needs exact child admission",
                )
        elif authored:
            if (request.expected_admission_generation is None) == (
                request.program_reset_scene_authorization is None
            ):
                raise HeadlessApiError(
                    "authoring_not_ready",
                    "authored-scene mutation requires exactly one ordinary "
                    "or Program Reset authorization",
                )
        elif request.program_reset_scene_authorization is not None:
            raise HeadlessApiError(
                "invalid_request",
                "runtime scene mutation cannot carry Program Reset authorization",
            )
        response = self.manager.commit_scene_transaction(
            request,
            trusted_principal_id=attribution.principal_id,
        )
        return response

    def scene_transaction_status(
        self, request: SceneTransactionRequest
    ) -> dict[str, Any]:
        return self.manager.scene_transaction_status(request)

    def health(self) -> dict[str, Any]:
        runtime = self.runtime()
        alarms = self.core.alarm_snapshot(limit=0, states=())
        historian = self.core.historian_health()
        try:
            node_environment = self.node_environment()
            node_environment_source = node_environment.get("source")
            node_environment_available = bool(
                isinstance(node_environment_source, dict)
                and node_environment_source.get("available") is True
                and node_environment_source.get("freshness") == "fresh"
                and node_environment_source.get("known_gaps") == []
            )
            node_environment_health: dict[str, Any] = {
                "available": node_environment_available,
                "source": node_environment_source,
            }
        except HeadlessApiError as error:
            node_environment_available = False
            node_environment_health = {
                "available": False,
                "reason_code": error.code,
            }
        alarm_health = alarms.get("health")
        alarm_available = (
            isinstance(alarm_health, dict) and alarm_health.get("state") == "available"
        )
        projection_ready = bool(
            runtime.get("complete")
            and alarms.get("reconciled")
            and alarm_available
            and historian.get("available")
            and node_environment_available
        )
        return {
            "schema_version": "manyforge.headless_health.v1",
            **({"workbench_previous_session_registration": registration}
               if (registration := getattr(
                   self.core, "workbench_previous_session_registration", None
               )) is not None else {}),
            "status_scope": "headless_projection",
            "status": "ready" if projection_ready else "degraded",
            "headless_projection_ready": projection_ready,
            "components": {
                "runtime_projection": {
                    "complete": runtime.get("complete"),
                    "reasons": runtime.get("reasons"),
                },
                "alarm_owner": {
                    "reconciled": alarms.get("reconciled"),
                    "health": alarm_health,
                },
                "historian": historian,
                "node_environment": node_environment_health,
            },
        }
