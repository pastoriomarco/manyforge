"""Owner-neutral projections shared by FastAPI routes and tests."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
import logging
import math
from threading import Event, Lock, Thread
import time
from typing import Any, Callable, Sequence

import rfc8785
from starlette.responses import JSONResponse

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.skill_loader import default_skills_dir, load_skill_catalog

from .assistant_instruments import (
    INDEX_VERSION as ASSISTANT_MODE_INDEX_VERSION,
    MODE_PROJECTION_VERSION as ASSISTANT_MODE_PROJECTION_VERSION,
    AssistantInstruments,
)
from .activity_history import apply_record, component_action_record, record_activity
from .client import MAX_HTTP_RESPONSE_BYTES
from .execution import ExecutionPort
from .manager import ManagerPort
from .history_query import HistorianCaptureRequest, HistorianQueryRequest
from .models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ActivationPreparationRequest,
    ProgramReferenceValidationRequest,
    CallerAttributionRequest,
    ApplyOutcomeRequest,
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
    ProgramTargetReferenceRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
    SkillExpansionRequest,
    StopIntentRequest,
    validate_failure_details_and_origin,
)
from .cell_admission import CellAdmissionError
from .component_model import (
    ComponentModel,
    ComponentState,
    SignalSample,
    DEFAULT_LIFECYCLE_TIMEOUT_S,
    action_wait_s,
    group_action_order,
    launch_settle_bound_s,
    lifecycle_timeouts_s,
    parse_component_model,
    manual_operation_resident,
    run_readiness,
    wait_for_action,
)
from .component_overview import ComponentOverviewFacts
from .native import HeadlessApiError, HeadlessCorePort, ViewerModelAsset
from .activation_preparer import _program_catalog
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
PROGRAM_RUN_INDEX_VERSION = "manyforge.program_run_index.v2"
PROGRAM_NODE_PREVIEW_INTENT_VERSION = (
    "manyforge.program_node_correction_preview_intent.v2"
)
PROGRAM_NODE_PREVIEW_VERSION = "manyforge.program_node_correction_preview.v2"
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
PROGRAM_RUN_STATUS_VERSION = "manyforge.program_run_status.v4"
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


_RUN_END_REASONS = frozenset({"completed", "failed", "stopped", "cell_fault"})
_RUN_END_DETAIL_FIELDS = frozenset(
    {"node_instance_id", "error", "stopped_by", "rule_id", "component_id"}
)


def _valid_run_end(value: object) -> bool:
    """`{end_reason, details}`; absent while the run goes on."""

    if value is None:
        return True
    if (
        not isinstance(value, dict)
        or set(value) != {"end_reason", "details"}
        or value["end_reason"] not in _RUN_END_REASONS
        or not isinstance(value["details"], dict)
        or not set(value["details"]) <= _RUN_END_DETAIL_FIELDS
    ):
        return False
    details = value["details"]
    error = details.get("error")
    return (
        details.get("stopped_by") in (None, "operator", "rule")
        and (
            error is None
            or (isinstance(error, dict) and isinstance(error.get("code"), str))
        )
        and all(
            details.get(name) is None or _bounded_text(details[name], maximum_bytes=512)
            for name in ("node_instance_id", "rule_id", "component_id")
        )
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
    environment = _palette_environment()
    return environment, load_skill_catalog(
        default_skills_dir(), catalog=environment.catalog
    )


@lru_cache(maxsize=1)
def _palette_environment():
    return build_node_environment()


@lru_cache(maxsize=1)
def _assistant_instruments() -> AssistantInstruments:
    """Assistant node and tool descriptors, built once from the loaded catalog."""
    return AssistantInstruments(_palette_environment().catalog)


def _assistant_mode(
    native: dict[str, Any], offers: Any
) -> dict[str, Any]:
    """Join the catalog part: the stable nodes this Deployment can run."""
    instruments = _assistant_instruments()
    offer_set = set(offers)
    return instruments.compose_mode(
        native,
        (
            entry.id
            for entry in instruments.entries()
            if _palette_missing_capability(entry, offer_set) is None
        ),
        offer_set,
    )


def warm_builtin_node_environment() -> None:
    """Build the immutable built-in node environment once, before serving.

    Building it parses and validates the node catalog in pure Python (about
    a CPU-second). Built lazily, it ran right after the first Apply on
    request threads, once per concurrent request (``lru_cache`` doesn't make
    concurrent first callers wait), holding the GIL for seconds while the
    machines were prepared: the alarm-attention publisher and the
    execution-socket clients then missed their deadlines. A build that fails
    here stays uncached, and the requests that need it report it as before.
    """

    try:
        _build_skills()
        _program_catalog()
        _assistant_instruments()
    except (OSError, ValueError) as error:  # unreadable or invalid build inputs
        logging.getLogger(__name__).warning(
            "built-in node environment is unavailable: %s", error
        )


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
        activation_preparer: Any | None = None,
    ) -> None:
        self.core = core
        self.manager = manager
        self.execution = execution
        self.program_runner = program_runner
        self.cell_admission = cell_admission
        self.activation_preparer = activation_preparer
        # The motion-attention projection the Kernel acknowledged holding
        # (OperatorAlarmAttentionPublisher.kernel_held_attention); None in a
        # composition without the publisher.
        self.kernel_attention_source: Callable[[], dict[str, Any] | None] | None = None
        # Admission bound only: exact node attempts whose advisory native
        # preview is in flight (see program_node_correction_preview).
        self._apply_scene_stop = Event()
        self._apply_scene_completion_lock = Lock()
        self._empty_scene_execution_lock = Lock()
        self._apply_scene_attempted: tuple[str, int, str] | None = None
        self._empty_scene_applies: set[tuple[str, int]] = set()
        self._direct_scene_clears: set[tuple[str, str]] = set()
        self._scene_clear: dict[str, Any] | None = None
        self._initial_scene_failure: dict[str, Any] | None = None
        self.initial_scene_observer: Callable[[], None] | None = None
        self._node_previews_lock = Lock()
        self._node_previews_in_flight: set[tuple[str, str]] = set()
        # One group action runs at a time; held by its thread.
        self._group_action_lock = Lock()
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

        self._apply_scene_stop.set()
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

    # Component model v2 ------------------

    def component_model(self) -> ComponentModel:
        response = self._execution().component_model()
        if response.get("ok") is not True:
            error = response.get("error") if isinstance(response.get("error"), dict) else {}
            raise HeadlessApiError(
                str(error.get("code") or "execution_command_response_invalid"),
                str(error.get("message") or "component model is unavailable"),
            )
        return parse_component_model(response)

    def components(self) -> list[ComponentState]:
        return self.component_model().component_states

    def cell_latch(self) -> dict[str, Any]:
        return dict(self.component_model().cell_latch)

    def io_signals(self) -> list[SignalSample]:
        return self.component_model().signal_samples

    def run_readiness(self) -> dict[str, Any]:
        """The one owner of RunReadiness."""

        model = self.component_model()
        try:
            cycle: dict[str, Any] | None = self.program_cycle_status()
        except HeadlessApiError:
            cycle = None
        try:
            status: dict[str, Any] | None = self.program_applied_status()
        except HeadlessApiError:
            status = None
        return dict(
            run_readiness(
                model,
                cycle,
                program_status=status,
                manual_operation_in_progress=manual_operation_resident(
                    self.execution_control_state()
                ),
            )
        )

    def _action_refusal(self, response: dict[str, Any]) -> HeadlessApiError:
        error = response.get("error") if isinstance(response.get("error"), dict) else {}
        return HeadlessApiError(
            str(error.get("code") or "execution_command_response_invalid"),
            str(error.get("message") or "component action was refused"),
            details=error.get("details") if isinstance(error.get("details"), dict) else None,
        )

    def component_action(self, component_id: str, action: str) -> dict[str, Any]:
        """Hand one action to the coordinator; its result is `last_action`."""

        try:
            response = self._execution().component_action(component_id, action)
            if response.get("ok") is not True:
                raise self._action_refusal(response)
        except HeadlessApiError as refusal:
            # Never dispatched, so the host publishes no `last_action` for it.
            record_activity(self.core, component_action_record(
                component_id, action, "refused",
                {"code": refusal.code, "message": str(refusal)},
            ))
            raise
        return {"action": action}

    def component_recovery_preflight(self, component_id: str) -> dict[str, Any]:
        response = self._execution().component_recovery_preflight(component_id)
        if response.get("ok") is not True:
            raise self._action_refusal(response)
        return response["preflight"]

    def component_recover_enable(self, component_id: str, confirmation: dict[str, Any]) -> dict[str, Any]:
        self._dispatch_recover_enable(component_id, confirmation)
        return {"action": "enable"}

    def _dispatch_recover_enable(self, component_id: str, confirmation: dict[str, Any]) -> int:
        try:
            response = self._execution().component_recover_enable(component_id, confirmation)
        except HeadlessApiError as error:
            record_activity(self.core, component_action_record(
                component_id, "enable", "unknown",
                {"code": error.code, "message": str(error)},
            ))
            raise
        if response.get("ok") is not True:
            refusal = self._action_refusal(response)
            record_activity(self.core, component_action_record(
                component_id, "enable", "refused", {"code": refusal.code, "message": str(refusal)},
            ))
            raise refusal
        accepted = response.get("accepted")
        if (not isinstance(accepted, dict) or accepted.get("component_id") != component_id
                or accepted.get("action") != "enable"
                or type(accepted.get("started_at_ns")) is not int
                or accepted["started_at_ns"] < 0):
            error = HeadlessApiError("execution_command_response_invalid", "recovery acceptance is malformed")
            record_activity(self.core, component_action_record(
                component_id, "enable", "unknown", {"code": error.code, "message": str(error)},
            ))
            raise error
        return accepted["started_at_ns"]

    def component_recover_enable_group(self, targets: list[dict[str, Any]]) -> dict[str, Any]:
        """Use the native composite for each confirmed member; stop on uncertainty."""
        states = self.component_model().component_states
        order = group_action_order(states, "enable")
        by_id = {target["component_id"]: target for target in targets}
        missing = set(by_id) - set(order)
        if missing:
            component_id = sorted(missing)[0]
            raise HeadlessApiError("component_not_found", f"component {component_id} is not in the cell")
        selected = [component_id for component_id in order if component_id in by_id]
        timeouts = lifecycle_timeouts_s(self.manager_graph()["cell_graph"])
        owner_of = {s["component_id"]: s.get("owner_id") for s in states}
        stop_generation = self._stop_generation()
        logger = logging.getLogger(__name__)

        def run() -> None:
            try:
                recovered_owners: set[str] = set()
                for component_id in selected:
                    if self._stop_generation() != stop_generation:
                        return
                    target = by_id[component_id]
                    confirmed = target["recovery_preflight"]
                    current = self.component_recovery_preflight(component_id)
                    actual = [{"component_id": row["component_id"],
                               "cause_code": (row["cause"] or {}).get("code"),
                               "vendor_code": (row["cause"] or {}).get("vendor_code"),
                               "stop_code": (row.get("stop") or {}).get("code"),
                               "stop_vendor_code": (row.get("stop") or {}).get("vendor_code"),
                               "eligibility": row["eligibility"]} for row in current["components"]]
                    original = confirmed["confirmed"]
                    owner_id = owner_of.get(component_id)
                    owner_recovered = owner_id in recovered_owners if owner_id else False
                    fact_keys = ("cause_code", "vendor_code", "stop_code", "stop_vendor_code")
                    cleared_owner = (
                        len(original) == 2 and len(actual) == 2 and owner_recovered
                        and original[0]["component_id"] == owner_id
                        and actual[0] == {"component_id": owner_id, "cause_code": None,
                                          "vendor_code": None, "stop_code": None,
                                          "stop_vendor_code": None, "eligibility": "ready"}
                        and original[1]["component_id"] == actual[1]["component_id"]
                        and actual[1]["eligibility"] in ("ready", "recover")
                        and (all(actual[1][key] == original[1][key] for key in fact_keys)
                             or all(actual[1][key] is None for key in fact_keys))
                    )
                    if (owner_recovered and not cleared_owner) or (not owner_recovered and actual != original):
                        logger.warning("component_recovery_confirmation_changed", extra={"component_id": component_id})
                        record_activity(self.core, component_action_record(
                            component_id, "enable", "refused", {
                                "code": "component_recovery_confirmation_changed",
                                "message": "Recovery facts changed; review and confirm them again.",
                            },
                        ))
                        return
                    dispatched = {"snapshot_id": current["snapshot_id"], "confirmed": actual}
                    before = next((s for s in self.component_model().component_states
                                   if s["component_id"] == component_id), None)
                    if before is None:
                        return
                    previous = before.get("last_action")
                    started = self._dispatch_recover_enable(component_id, dispatched)
                    if previous is not None and previous.get("started_at_ns") == started:
                        logger.warning("component_recovery_outcome_unknown", extra={"component_id": component_id})
                        return
                    owner = owner_of.get(component_id)
                    member_timeout = timeouts.get(component_id, DEFAULT_LIFECYCLE_TIMEOUT_S)
                    owner_timeout = timeouts.get(owner, DEFAULT_LIFECYCLE_TIMEOUT_S) if owner else 0.0
                    deadline = time.monotonic() + 2 * member_timeout + owner_timeout
                    while True:
                        state = next((s for s in self.component_model().component_states
                                      if s["component_id"] == component_id), None)
                        if state is None or self._stop_generation() != stop_generation:
                            return
                        result = state.get("last_action")
                        if result and result.get("started_at_ns") == started and "action" not in state:
                            if result.get("action") != "enable" or result.get("outcome") != "succeeded":
                                return
                            expected_phases = [
                                (row["component_id"], "recover") for row in actual
                                if row["eligibility"] == "recover"
                            ] + [(component_id, "enable")]
                            phases = result.get("phases")
                            if (not isinstance(phases, list) or
                                    [(p.get("component_id"), p.get("action")) for p in phases if isinstance(p, dict)] != expected_phases or
                                    any(not isinstance(p, dict) or p.get("outcome") != "succeeded" for p in phases)):
                                logger.warning("component_recovery_phases_invalid", extra={"component_id": component_id})
                                return
                            if (owner_id and actual[0]["component_id"] == owner_id
                                    and actual[0]["eligibility"] == "recover"):
                                recovered_owners.add(owner_id)
                            break
                        if time.monotonic() >= deadline:
                            logger.warning("component_recovery_outcome_unknown", extra={"component_id": component_id})
                            return
                        time.sleep(0.1)
            except HeadlessApiError as error:
                logger.warning("component_recovery_group_refused", extra={"error_code": error.code})
            finally:
                self._group_action_lock.release()

        if not self._group_action_lock.acquire(blocking=False):
            raise HeadlessApiError("component_action_in_progress", "A group action is still running; wait for it to finish")
        Thread(target=run, name="component-recover-enable-group", daemon=True).start()
        return {"action": "enable"}

    def _stop_generation(self) -> Any:
        """The cell's Stop generation; it changes with every new Stop."""

        control = self.execution_control_state().get("control_state")
        stop = control.get("cell_stop_action") if isinstance(control, dict) else None
        return stop.get("expected_stop_generation") if isinstance(stop, dict) else None

    def component_actions(self, action: str, component_ids: list[str]) -> dict[str, Any]:
        """One action on the named components, one at a time.

        Order: `prepare` runs owners before their members, `disable`,
        `power_off` and `reboot` run members before their owner, anything
        else keeps cell order (`group_action_order`). `enable` first prepares
        a target that isn't prepared (its machine first, when that isn't
        prepared either) and recovers a target's machine that reports an
        e-stop (the explicit clear), and a machine's `recover` then prepares its members
        that aren't prepared, so Enable all after an e-stop or a failed Load
        brings every component up. Each action is
        dispatched after the previous one finished, bounded as the host bounds
        it (`action_wait_s`): the component's authored lifecycle timeout from
        the active cell graph; a `prepare` of a component with no action since
        its launch also waits up to the launch bound, the cell's lifecycle
        timeouts summed (a result that doesn't come back is `unknown`). A
        refusal or failure of one doesn't stop the rest (each result is that
        component's `last_action`), except: a Stop ends the group, and the
        members of an owner whose `prepare` failed are skipped
        (`owner_not_prepared`, logged).
        A group request while another group runs is refused
        (`component_action_in_progress`).
        """

        states = self.component_model().component_states
        order = group_action_order(states, action)
        unknown = sorted(set(component_ids) - set(order))
        if unknown:
            raise HeadlessApiError(
                "component_not_found",
                f"component {unknown[0]} is not in the cell",
                details={"component_id": unknown[0]},
            )
        selected = [component_id for component_id in order if component_id in component_ids]
        owner_of = {
            state["component_id"]: state["owner_id"] for state in states if "owner_id" in state
        }
        previous = {
            state["component_id"]: state.get("last_action") for state in states
        }
        lifecycle = {state["component_id"]: state.get("lifecycle") for state in states}
        stopped = {state["component_id"] for state in states if state.get("stop") is not None}

        def unprepared(component_id: str) -> bool:
            return lifecycle.get(component_id) in ("not_prepared", "offline")

        steps: list[tuple[str, str]] = []
        for component_id in selected:
            owner = owner_of.get(component_id)
            if action == "enable" and owner is not None and owner in stopped \
                    and (owner, "recover") not in steps:
                # Enable all is the explicit clear of a machine's latched
                # e-stop error: its Recover first (refused typed while the
                # e-stop stays, and then its members aren't tried).
                steps.append((owner, "recover"))
            if action == "enable" and unprepared(component_id):
                if owner is not None and unprepared(owner) and (owner, "prepare") not in steps \
                        and (owner, "recover") not in steps:
                    steps.append((owner, "prepare"))
                steps.append((component_id, "prepare"))
            steps.append((component_id, action))
            if action == "recover":
                steps.extend(
                    (member, "prepare") for member in order
                    if owner_of.get(member) == component_id and unprepared(member)
                    and member not in selected
                )
        timeouts = lifecycle_timeouts_s(self.manager_graph()["cell_graph"])
        launch_bound_s = launch_settle_bound_s(
            timeouts.get(component_id, DEFAULT_LIFECYCLE_TIMEOUT_S) for component_id in order
        )
        stop_generation = self._stop_generation()
        logger = logging.getLogger(__name__)

        def read_states() -> list[ComponentState]:
            return self.component_model().component_states

        def run() -> None:
            try:
                run_group()
            finally:
                self._group_action_lock.release()

        def run_group() -> None:
            failed_owners: set[str] = set()
            for component_id, step_action in steps:
                if self._stop_generation() != stop_generation:
                    logger.warning(
                        "component_group_action_stopped",
                        extra={"component_id": component_id, "action": step_action,
                               "error_code": "component_action_interrupted_by_stop"},
                    )
                    return
                # A target whose own prepare failed isn't enabled either.
                if component_id in failed_owners and step_action == action:
                    continue
                if owner_of.get(component_id) in failed_owners:
                    logger.warning(
                        "component_group_action_step_skipped",
                        extra={"component_id": component_id, "action": step_action,
                               "error_code": "owner_not_prepared"},
                    )
                    record_activity(self.core, component_action_record(
                        component_id, step_action, "refused",
                        {"code": "owner_not_prepared",
                         "message": f"{component_id} wasn't tried: its machine "
                         f"{owner_of[component_id]} didn't prepare or recover"},
                    ))
                    continue
                try:
                    self.component_action(component_id, step_action)
                    result = wait_for_action(
                        read_states,
                        component_id,
                        step_action,  # type: ignore[arg-type]
                        previous[component_id],
                        timeout_s=action_wait_s(
                            step_action,
                            timeouts.get(component_id, DEFAULT_LIFECYCLE_TIMEOUT_S),
                            launch_bound_s,
                            # The host starts a launched component without a
                            # `last_action`; its first Prepare awaits the launch.
                            launch_settled=previous[component_id] is not None,
                        ),
                    )
                except HeadlessApiError as error:
                    logger.warning(
                        "component_group_action_step_refused",
                        extra={
                            "component_id": component_id,
                            "action": step_action,
                            "error_code": error.code,
                        },
                    )
                    result = None
                if result is not None:
                    previous[component_id] = result
                if result is not None and result.get("outcome") == "succeeded":
                    continue
                if step_action in ("prepare", "recover"):
                    failed_owners.add(component_id)
                error_code = (result or {}).get("error", {}).get("code")
                if error_code == "component_action_interrupted_by_stop":
                    return

        if not self._group_action_lock.acquire(blocking=False):
            raise HeadlessApiError(
                "component_action_in_progress",
                "A group action is still running; wait for it to finish",
            )
        Thread(target=run, name="component-group-action", daemon=True).start()
        return {"action": action}

    def set_io_output(self, signal_id: str, value: bool | float) -> dict[str, Any]:
        response = self._execution().set_io_output(signal_id, value, origin="manual")
        if response.get("ok") is not True:
            raise self._action_refusal(response)
        return dict(response["result"])

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
        # Headless-owned condition: available without querying Program Runner.
        return {**snapshot, **self.initial_scene_status()}

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
        capture("applied_program_status", self.program_applied_status)
        capture("source_model", self.source_model)
        capture("execution_identity", self.execution_identity)
        capture("execution_targets", self.execution_targets)
        capture("execution_control", self.execution_control_state)
        component_model: ComponentModel | None = None
        try:
            component_model = self.component_model()
        except HeadlessApiError as error:
            errors["component_model"] = {"code": error.code, "message": str(error)}
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
            component_model=component_model,
            errors=errors,
            applied_program_status=projections["applied_program_status"],
        )

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
        # The Manager-derived simulator per ROS-hosted component.
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
            # A non-executable Applied Program is not a Runner source
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
            != ASSISTANT_MODE_PROJECTION_VERSION
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
        offers = _strip_palette_offers(context["node_environment"])
        context["assistant_mode"] = _assistant_mode(context["assistant_mode"], offers)
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
            validate_prepared_activation_origin(projection.get("origin"))
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

        Repeated status reads do not repeat placement. A typed Runner
        authoring-readiness refusal is retried briefly; other failures remain
        terminal, and idle Reset with reload remains the explicit retry path.
        The attempt latch is memory-only: a replacement Headless instance may
        attempt the current initializing Apply again on its next notification.
        """

        try:
            self._program_runner().reconcile_persistent_values()
        except (HeadlessApiError, ProgramRunnerClientError) as error:
            logging.getLogger(__name__).warning(
                "Program runner did not reconcile persistent values after a "
                "committed Apply: %s: %s",
                error.code,
                error,
            )
        identity = None
        dispatched = False
        def mark_dispatched() -> None:
            nonlocal dispatched
            dispatched = True
        try:
            committed = self.committed_scenario_activation()
            if committed["scene_initialization"] != "initialize":
                return
            identity = (committed["apply_id"], committed["apply_generation"],
                        committed["record_digest"])
            applied = self.program_applied_status()
            if (committed["scene_initialization"] == "initialize"
                and applied["program_id"] is None
                and applied["record_digest"] == identity[2]):
                self._clear_empty_applied(identity)
                return
            if (
                committed["scene_initialization"] != "initialize"
                or applied["program_id"] is None
                or applied["record_digest"] != committed["record_digest"]
            ):
                return
            with self._apply_scene_completion_lock:
                current = self.committed_scenario_activation()
                if (current["apply_id"], current["apply_generation"],
                    current["record_digest"]) != identity:
                    return
                if self._initial_scene_failure is not None and self._initial_scene_failure["identity"] != identity:
                    self._initial_scene_failure = None
                if self._apply_scene_attempted == identity:
                    return
                self._apply_scene_attempted = identity
            deadline = time.monotonic() + 30.0
            last_refusal = None
            owner_epoch = None
            while not self._apply_scene_stop.is_set():
                cycle = self.program_cycle_status()
                current_owner = cycle["program_runner_owner_epoch"]
                if owner_epoch is not None and current_owner != owner_epoch:
                    raise HeadlessApiError("program_runner_unavailable", "Program runner changed during initial scene preparation")
                owner_epoch = current_owner
                admission = self.authoring_admission()
                current = self.committed_scenario_activation()
                if (current["apply_id"], current["apply_generation"],
                    current["record_digest"]) != identity:
                    return
                with self._apply_scene_completion_lock:
                    if self._apply_scene_attempted != identity:
                        return
                if self._apply_scene_stop.is_set():
                    return
                request = ProgramScenePreparationRequest(
                    request_id="apply-scene." + identity[2][7:],
                    request_owner_epoch=owner_epoch,
                    source_label="headless_apply_completion",
                    expected_applied_record_digest=identity[2],
                    expected_admission_generation=admission["admission_generation"],
                )
                # Readiness checks can be slow; no attempt may start after the bound.
                if time.monotonic() >= deadline:
                    if last_refusal is not None:
                        raise last_refusal
                    raise HeadlessApiError(
                        "program_scene_preparation_failed",
                        "Program scene preparation deadline expired before dispatch",
                    )
                try:
                    # Runner rechecks selection and admission before effects.
                    self.program_scene_prepare(request, on_dispatch=mark_dispatched)
                    return
                except HeadlessApiError as error:
                    # Only the typed Runner refusal is known to precede effects.
                    if (error.code != "authoring_not_ready"
                        or not isinstance(error.__cause__, ProgramRunnerClientError)):
                        raise
                    dispatched = False  # Runner explicitly refused before effects.
                    last_refusal = error
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise
                    if self._apply_scene_stop.wait(min(0.25, remaining)):
                        return
        except (HeadlessApiError, ProgramRunnerClientError) as error:
            if identity is not None:
                self._record_initial_scene_failure(identity, error, dispatched=dispatched)
            logging.getLogger(__name__).warning(
                "Program scene preparation after committed Apply failed: %s: %s",
                error.code,
                error,
            )

    def _notify_initial_scene(self) -> None:
        if self.initial_scene_observer is not None:
            try:
                self.initial_scene_observer()
            except Exception:
                logging.getLogger(__name__).exception("Initial scene status wake failed")

    def _current_apply_identity(self) -> tuple[str, int, str]:
        committed = self.committed_scenario_activation()
        return (committed["apply_id"], committed["apply_generation"], committed["record_digest"])

    def _record_initial_scene_failure(self, identity: tuple[str, int, str], error: Exception, *, dispatched: bool) -> None:
        # Unconfirmed or superseded identity must not modify the condition.
        try:
            with self._apply_scene_completion_lock:
                if self._current_apply_identity() != identity:
                    return
                # After submission, even a typed failure may follow partial Scene effects.
                unknown = dispatched
                # Last confirmed arrival wins. Rare reordered RPC
                # replies can temporarily report an older outcome for this Apply.
                self._initial_scene_failure = {
                    "identity": identity, "state": "unknown" if unknown else "failed",
                    "reason": {"code": getattr(error, "code", "program_scene_preparation_failed"),
                               "message": str(error) or type(error).__name__},
                }
            self._notify_initial_scene()
        except (HeadlessApiError, ProgramRunnerClientError):
            return

    def _clear_initial_scene_failure(self, identity: tuple[str, int, str]) -> None:
        try:
            with self._apply_scene_completion_lock:
                if self._current_apply_identity() == identity and self._initial_scene_failure is not None and self._initial_scene_failure["identity"] == identity:
                    self._initial_scene_failure = None
            self._notify_initial_scene()
        except (HeadlessApiError, ProgramRunnerClientError):
            pass

    def initial_scene_status(self) -> dict[str, Any]:
        # Independent of Runner availability. A new confirmed Apply supersedes the old condition.
        with self._apply_scene_completion_lock:
            try:
                identity = self._current_apply_identity()
            except (HeadlessApiError, ProgramRunnerClientError):
                identity = None
            if identity is not None and self._initial_scene_failure is not None and self._initial_scene_failure["identity"] != identity:
                self._initial_scene_failure = None
            condition = deepcopy(self._initial_scene_failure)
        if condition is not None:
            apply_id, generation, digest = condition.pop("identity")
            condition.update(apply_id=apply_id, apply_generation=generation, record_digest=digest)
        return {"initial_scene_failure": condition}

    def _clear_empty_applied(self, identity: tuple[str, int, str]) -> None:
        with self._empty_scene_execution_lock:
            self._clear_empty_applied_once(identity)

    def _clear_empty_applied_once(self, identity: tuple[str, int, str]) -> None:
        with self._apply_scene_completion_lock:
            if identity[:2] not in self._empty_scene_applies or self._apply_scene_attempted == identity:
                return

            def publish_unstarted(result: dict[str, Any]) -> None:
                # A stale follower cannot replace another clear's evidence.
                if (self._scene_clear is None
                    or (self._scene_clear.get("apply_id"),
                        self._scene_clear.get("apply_generation")) == identity[:2]):
                    self._scene_clear = {"apply_id": identity[0], "apply_generation": identity[1],
                                         **result}

            try:
                current = self.committed_scenario_activation()
                current_identity = (current["apply_id"], current["apply_generation"],
                                    current["record_digest"])
            except Exception as error:
                logging.getLogger(__name__).warning(
                    "Cannot confirm committed Apply for Scene clear: %s", error
                )
                return
            self._empty_scene_applies.discard(identity[:2])
            if current_identity != identity:
                publish_unstarted({"state": "superseded", "remaining": None, "reason": None})
                return
            self._apply_scene_attempted = identity
            self._scene_clear = {"apply_id": identity[0], "apply_generation": identity[1],
                                 "state": "pending", "remaining": None, "reason": None}

        sent = False
        try:
            cycle = self._program_runner().cycle_status()
            admission = self.authoring_admission()
            current = self.committed_scenario_activation()
            if (current["apply_id"], current["apply_generation"], current["record_digest"]) != identity:
                result = {"state": "superseded", "remaining": None, "reason": None}
            else:
                request = ProgramScenePreparationRequest(
                    request_id="apply-scene." + identity[2][7:],
                    request_owner_epoch=cycle["program_runner_owner_epoch"],
                    source_label="headless_apply_completion",
                    expected_applied_record_digest=identity[2],
                    expected_admission_generation=admission["admission_generation"],
                    clear_empty_scene=True,
                )
                sent = True
                response = self._program_runner().prepare_applied_scene(request.model_dump(mode="json"))
                result = self._scene_clear_result(response, identity[2])
        except Exception as error:
            refused_before_effect = isinstance(error, ProgramRunnerClientError) and error.code in {
                "authoring_not_ready", "program_run_applied_selection_stale",
            }
            unknown = sent and not refused_before_effect
            result = {"state": "unknown" if unknown else "failed", "remaining": None,
                      "reason": {"code": "scene_clear_unknown" if unknown else "scene_clear_failed",
                                 "message": str(error) or type(error).__name__,
                                 "details": {"reason": getattr(error, "code", "scene_owner_unavailable")}}}
        with self._apply_scene_completion_lock:
            if (self._scene_clear is not None
                and (self._scene_clear["apply_id"], self._scene_clear["apply_generation"]) == identity[:2]
                and self._scene_clear["state"] == "pending"):
                self._scene_clear.update(result)

    @staticmethod
    def _scene_clear_result(response: dict[str, Any], expected_digest: str | None) -> dict[str, Any]:
        proof = response.get("prepared_scene")
        if (not isinstance(proof, dict)
            or response.get("schema_version") != "manyforge.program_scene_preparation.v1"
            or response.get("status") != "prepared"
            or response.get("applied_record_digest") != expected_digest
            or response.get("program_revision") is not None
            or proof.get("schema_version") != "manyforge.empty_scene_clear.v1"
            or proof.get("state") not in {"succeeded", "partial", "failed", "unknown", "superseded"}
            or (proof.get("last_observed_remaining") is not None
                and (not isinstance(proof["last_observed_remaining"], list)
                     or not all(isinstance(item, str) and item for item in proof["last_observed_remaining"])))
            or (proof.get("state") == "succeeded" and
                (proof.get("last_observed_remaining") != [] or proof.get("refused") != []))
            or (proof.get("state") == "partial" and
                (not proof.get("last_observed_remaining") or not proof.get("refused")))
            or not isinstance(proof.get("refused", []), list)
            or not all(isinstance(item, dict)
                       and all(isinstance(item.get(key), str) and item[key]
                       for key in ("resource_id", "code", "message"))
                       for item in proof.get("refused", []))
            or (proof.get("state") in {"failed", "unknown"} and
                (not isinstance(proof.get("reason"), str)
                 or not isinstance(proof.get("message"), str)))):
            raise ValueError("Program runner returned invalid Scene clear evidence")
        reason = None
        if proof["state"] in {"partial", "failed", "unknown"}:
            reason = {"code": "scene_clear_" + proof["state"],
                      "message": proof.get("message", "Some Scene objects remain"),
                      "details": {"reason": proof.get("reason"),
                          "resource_id": proof.get("resource_id"),
                          "refused": proof.get("refused", [])}}
            if "cause_code" in proof:
                reason["details"]["cause_code"] = proof["cause_code"]
                if "cause_details" in proof:
                    reason["details"]["cause_details"] = proof["cause_details"]
        return {"state": proof["state"], "remaining": proof["last_observed_remaining"],
                "reason": reason}

    def _clear_current_empty_scene(self, request: ProgramScenePreparationRequest) -> dict[str, Any]:
        key = (request.request_owner_epoch, request.request_id)
        with self._empty_scene_execution_lock:
            with self._apply_scene_completion_lock:
                if key in self._direct_scene_clears:
                    raise HeadlessApiError("scene_clear_failed", "This Scene clear request was already attempted")
                self._direct_scene_clears.add(key)
                self._scene_clear = {"request_id": request.request_id, "state": "pending",
                                     "remaining": None, "reason": None}
            sent = False
            try:
                manager = self.manager_status()
                source, status = manager["source"], manager["status"]
                active = status.get("active", {})
                publication = active.get("publication", {})
                if (source.get("available") is not True or source.get("freshness") != "fresh"
                    or status.get("manager_ready") is not True
                    or not isinstance(status.get("runtime"), dict)
                    or status["runtime"].get("converged") is not True
                    or active.get("canonical_empty") is not True
                    or publication.get("publication_generation") != request.expected_publication_generation):
                    raise HeadlessApiError("program_run_applied_selection_stale", "Canonical empty Scene changed before clearing")
                cycle = self._program_runner().cycle_status()
                if cycle.get("program_runner_owner_epoch") != request.request_owner_epoch:
                    raise HeadlessApiError("program_runner_unavailable", "Program runner changed before clearing")
                sent = True
                response = self._program_runner().prepare_applied_scene(request.model_dump(mode="json"))
                result = self._scene_clear_result(response, None)
            except Exception as error:
                refused_before_effect = isinstance(error, ProgramRunnerClientError) and error.code in {
                    "authoring_not_ready", "program_run_applied_selection_stale",
                }
                unknown = sent and not refused_before_effect
                result = {"state": "unknown" if unknown else "failed", "remaining": None,
                          "reason": {"code": "scene_clear_unknown" if unknown else "scene_clear_failed",
                                     "message": str(error) or type(error).__name__,
                                     "details": {"reason": getattr(error, "code", "scene_owner_unavailable")}}}
            with self._apply_scene_completion_lock:
                self._scene_clear.update(result)
                return {"scene_clear": deepcopy(self._scene_clear)}

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
            # `operation_failures` is always present (empty when none);
            # `run_end` (ProgramRunEnd) once the run has ended.
            set(projection) - {"run_end"} != expected_fields | {"operation_failures"}
            or not _valid_run_end(projection.get("run_end"))
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
        self, request: ProgramScenePreparationRequest, *, on_dispatch: Callable[[], None] | None = None
    ) -> dict[str, Any]:
        if request.clear_empty_scene and request.expected_applied_record_digest is None:
            return self._clear_current_empty_scene(request)
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
            preparation_identity = self._current_apply_identity()
        except (HeadlessApiError, ProgramRunnerClientError):
            preparation_identity = None
        try:
            runner = self._program_runner()
            body = request.model_dump(mode="json")
            if on_dispatch is not None:
                on_dispatch()
            result = runner.prepare_applied_scene(body)
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
        if (preparation_identity is not None
            and preparation_identity[2] == request.expected_applied_record_digest):
            self._clear_initial_scene_failure(preparation_identity)
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

    def program_cycle_status(self, *, include_scene_clear: bool = False) -> dict[str, Any]:
        try:
            result = self._program_runner().cycle_status()
            if include_scene_clear:
                with self._apply_scene_completion_lock:
                    result["scene_clear"] = deepcopy(self._scene_clear)
            return result
        except ProgramRunnerClientError as error:
            if include_scene_clear:
                with self._apply_scene_completion_lock:
                    clear = deepcopy(self._scene_clear)
                if clear is not None:
                    raise HeadlessApiError(error.code, str(error),
                                           details={**error.details, "scene_clear": clear},
                                           producer=error.producer) from error
            raise self._runner_error(error) from error

    def program_cycle_reset(self, request: ProgramCycleResetRequest) -> dict[str, Any]:
        """Forward one Reset; Program Runner owns every lifecycle phase."""

        try:
            identity = None
            if request.reload_initial_scene:
                try:
                    identity = self._current_apply_identity()
                except (HeadlessApiError, ProgramRunnerClientError):
                    pass
            result = self._program_runner().reset_cycle(request.model_dump(mode="json"))
            if (identity is not None
                and result.get("schema_version") == "manyforge.program_cycle_reset_result.v1"
                and result.get("program_run_id") == request.program_run_id
                and result.get("reset_cycle_generation") == request.expected_cycle_generation
                and result.get("idle_cycle_generation") == request.expected_cycle_generation + 1):
                self._clear_initial_scene_failure(identity)
            return result
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
        target = built.get("target")
        if target is not None:
            try:
                ProgramTargetReferenceRequest.model_validate(target)
            except ValueError as error:
                raise HeadlessApiError(
                    "program_runner_response_invalid",
                    "Program runner returned an invalid preview target",
                ) from error
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
            or participant.target.component_id != target.get("component_id")
            or participant.target.command_target_id != target.get("command_target_id")
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
            projection.get("schema_version") != ASSISTANT_MODE_PROJECTION_VERSION
            or projection.get("mode") != mode_id
            or not isinstance(source, dict)
        ):
            raise HeadlessApiError(
                "assistant_mode_projection_invalid",
                "native core returned an unsupported assistant mode projection",
            )
        offers = self.core.node_environment_projection().get("capability_offers")
        return _assistant_mode(projection, offers)

    def assistant_modes(self) -> dict[str, Any]:
        projection = self.core.assistant_mode_index()
        source = projection.get("source")
        if (
            projection.get("schema_version") != ASSISTANT_MODE_INDEX_VERSION
            or not isinstance(projection.get("modes"), list)
            or not isinstance(source, dict)
        ):
            raise HeadlessApiError(
                "assistant_mode_projection_invalid",
                "native core returned an unsupported assistant mode index",
            )
        return _assistant_instruments().compose_index(projection)

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

        The owner's `reconciled` reaches the Kernel only up to `P + D` later,
        and the Kernel admits by its held copy alone. The
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

    def record_apply_outcome(self, request: ApplyOutcomeRequest) -> dict[str, Any]:
        """One Load/New/Apply/Revert terminal into History (``activity_history``)."""

        body = request.model_dump(mode="json", exclude_none=True)
        return self.core.record_activity(apply_record(
            body["apply_kind"], body["outcome"], body.get("error"), body["components"]
        ))

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

    def validate_program_references(self, request: ProgramReferenceValidationRequest) -> dict[str, Any]:
        if self.activation_preparer is None:
            raise HeadlessApiError("activation_preparer_unavailable", "Reference validation is not configured")
        diagnostics = self.activation_preparer.validate_program_references(
            deployment=request.deployment, model_source_closure=request.model_source_closure,
            model_source_closure_digest=request.model_source_closure_digest,
            program=request.program, program_content_digest=request.program_content_digest,
            draft=request.draft,
        )
        result = {"schema_version": "manyforge.program_reference_validation_response.v2",
                  "workspace_generation": request.workspace_generation.model_dump(mode="json"),
                  "program_content_digest": request.program_content_digest, **diagnostics}
        actual_bytes = len(rfc8785.dumps(result))
        bound_bytes = 16 * 1024 * 1024
        if actual_bytes > bound_bytes:
            raise HeadlessApiError("draft_candidate_response_too_large", "Draft candidate response exceeds its bound",
                                   details={"bound_bytes": bound_bytes, "actual_bytes": actual_bytes})
        return result

    def prepare_activation(self, request: ActivationPreparationRequest) -> dict[str, Any]:
        if self.activation_preparer is None:
            raise HeadlessApiError(
                "activation_preparer_unavailable",
                "Activation preparation is not configured",
            )
        # The request's admission generation is evidence only: preparation
        # follows the current admission facts. A momentarily closed admission
        # (a Reset's Idle reaching the Kernel, the committed Apply's initial
        # Scene placement, a readiness transition) is waited for, within the
        # floor of every Apply's bound (momentarily busy never fails a Load).
        self._await_idle_admission()
        return {
            "prepared_activation": self.activation_preparer.prepare(
                source_document_digest=request.source_document_digest,
                scenario_id=request.scenario_id,
                project_path=request.project_path,
                captured_input_digest=request.captured_input_digest,
                deployment=request.deployment,
                model_source_closure=request.model_source_closure,
                program=request.program,
            )
        }

    # The wait for idle admission: one default lifecycle timeout, the floor of
    # every Apply's bound, which fits inside the callers' Headless HTTP timeout.
    _ADMISSION_WAIT_S = DEFAULT_LIFECYCLE_TIMEOUT_S
    _ADMISSION_POLL_S = 0.1

    def _await_idle_admission(self) -> None:
        deadline = time.monotonic() + self._ADMISSION_WAIT_S
        while True:
            try:
                admission = self._cell_admission().get()
            except CellAdmissionError as error:
                raise self._admission_error(error) from error
            if admission.get("authoring_allowed") is True:
                return
            if time.monotonic() >= deadline:
                raise HeadlessApiError(
                    "authoring_not_ready",
                    "Activation preparation requires the current idle admission; "
                    f"it didn't open within {self._ADMISSION_WAIT_S:g} s",
                )
            time.sleep(self._ADMISSION_POLL_S)

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
            if (response.get("replay_status") == "accepted_new"
                and request.scene_initialization == "initialize"
                and request.prepared_activation.program_outcome["kind"] == "no_program"):
                with self._apply_scene_completion_lock:
                    self._empty_scene_applies.add((apply_id, apply_generation))
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

    def manager_apply_release_for_new(
        self, request_context: ManagerApplyAcknowledgementContext
    ) -> dict[str, Any]:
        return self.manager.release_failed_apply_for_new(request_context)

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
