"""Owner-neutral projections shared by FastAPI routes and tests."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
from typing import Any, Sequence

import rfc8785

from manyforge_behavior.catalog import (
    CatalogValidationError,
    NodeCatalog,
)
from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    verify_content_identity,
)
from manyforge_behavior.node_environment import BUILTIN_CATALOG_PATH
from manyforge_behavior.program import Program, ProgramValidationError

from .execution import ExecutionPort
from .manager import ManagerPort
from .models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    HistorianReconcileRequest,
    ManagerApplyRequest,
    AuthoringMutationContext,
    AuthoringMutationRecoveryRequest,
    AuthoringMutationSettlementRequest,
    AuthoringOwnerReconciliationRequest,
    ProgramApplyRequest,
    ProgramModelAttemptRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramScenePreparationRequest,
    ProgramCycleResetRequest,
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
    FullStopIntentRequest,
    OperationControlRequest,
    OperationRecoveryActionRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
    StopIntentRequest,
    StopFaultRecoveryIntentRequest,
)
from .cell_admission import CellAdmissionError
from .native import HeadlessApiError, HeadlessCorePort, ViewerModelAsset
from .program_runner_client import ProgramRunnerClientError, ProgramRunnerPort
from .program_binding_transport import ProgramBindingTransportError
from .program_binding_projection import (
    validate_applied_program_selection,
    validate_program_apply_result,
    validate_scenario_program_selection,
)


RUNTIME_SNAPSHOT_VERSION = "manyforge.headless_runtime_snapshot.v1"
MANAGER_SOURCE_VERSION = "manyforge.manager_source.v1"
PROGRAM_APPLY_RESULT_VERSION = "manyforge.program_apply_result.v2"
PROGRAM_RUN_INDEX_VERSION = "manyforge.program_run_index.v1"
PROGRAM_RUN_STATUS_VERSION = "manyforge.program_run_status.v1"
PROGRAM_RUN_CAPTURE_VERSION = "manyforge.program_run_capture.v1"
SOURCE_MODEL_CONTEXT_VERSION = "manyforge.source_model_context.v1"
ASSISTANT_MODE_CONTEXT_VERSION = "manyforge.assistant_mode_context.v1"
_PROGRAM_RUN_STATES = frozenset(
    {
        "running",
        "intervention_required",
        "stopping",
        "finalizing",
        "stopped",
        "succeeded",
        "failed",
    }
)
_ACTIVE_PROGRAM_RUN_STATES = frozenset(
    {"running", "intervention_required", "stopping", "finalizing"}
)
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
def _program_catalog() -> NodeCatalog:
    """Load the build-owned authoring catalog once per headless process."""

    return NodeCatalog(BUILTIN_CATALOG_PATH)


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
    ) -> None:
        self.core = core
        self.manager = manager
        self.execution = execution
        self.program_runner = program_runner
        self.cell_admission = cell_admission
        self.cclc = cclc

    def _cell_admission(self) -> Any:
        if self.cell_admission is None:
            raise HeadlessApiError(
                "authoring_not_ready",
                "Kernel cell-admission owner is not configured",
            )
        return self.cell_admission

    @staticmethod
    def _admission_error(error: CellAdmissionError) -> HeadlessApiError:
        return HeadlessApiError(error.code, str(error))

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
            "acquired_admission_generation": claim.get(
                "acquired_admission_generation"
            ),
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
            item
            for item in owners
            if item.get("owner_role") == "authoring_owner"
        ]
        if len(authoring) != 1:
            raise HeadlessApiError(
                "authoring_reconciliation_invalid",
                "reconciliation must name exactly one authoring owner epoch",
            )
        try:
            return self._cell_admission().reconcile(
                request.model_dump(mode="json")
            )
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
            "acquired_admission_generation": claim[
                "acquired_admission_generation"
            ],
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

    def execution_status(self) -> dict[str, Any]:
        return self._execution().status()

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

    def execution_operation_recovery(
        self, operation_id: str
    ) -> dict[str, Any]:
        return self._execution().operation_recovery(operation_id)

    def execution_apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        request: OperationRecoveryActionRequest,
    ) -> dict[str, Any]:
        return self._execution().apply_operation_recovery(
            operation_id,
            expected_operation_revision,
            request,
        )

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

    def execution_start_pick(
        self,
        request: PickSequencePlanningRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().start_pick(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_start_place(
        self,
        request: PlaceSequencePlanningRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().start_place(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_pick_place_sequence(
        self, sequence_id: str
    ) -> dict[str, Any]:
        return self._execution().read_pick_place_sequence(sequence_id)

    def execution_pause(
        self,
        request: OperationControlRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().pause(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_resume(
        self,
        request: OperationControlRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().resume(
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
        request: StopFaultRecoveryIntentRequest,
    ) -> dict[str, Any]:
        return self._execution().recover_stop_fault(request)

    def execution_set_runtime_scale(
        self,
        request: RuntimeScaleRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
        return self._execution().set_runtime_scale(
            request, trusted_principal_id=attribution.principal_id
        )

    def execution_get_runtime_scale(
        self, operation_id: str
    ) -> dict[str, Any]:
        return self._execution().get_runtime_scale(operation_id)

    def runtime(self) -> dict[str, Any]:
        snapshot = self.core.runtime_snapshot()
        if snapshot.get("schema_version") != RUNTIME_SNAPSHOT_VERSION:
            raise HeadlessApiError(
                "headless_runtime_projection_invalid",
                "runtime snapshot version is unsupported",
            )
        return snapshot

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
        return {
            "schema_version": "manyforge.manager_graph_projection.v1",
            "source": status["source"],
            "graph_revision": active.get("graph_revision"),
            "retained_bundle_id": active.get("retained_bundle_id"),
            "cell_graph": graph,
        }

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

    def node_environment(self) -> dict[str, Any]:
        projection = self.core.node_environment_projection()
        source = projection.get("source")
        if (
            projection.get("schema_version")
            != "manyforge.node_environment_projection.v2"
            or not isinstance(source, dict)
        ):
            raise HeadlessApiError(
                "node_environment_projection_invalid",
                "native core returned an unsupported node environment projection",
            )
        return projection

    def source_model(self) -> dict[str, Any]:
        projection = self.core.source_model_projection()
        source = projection.get("source")
        if (
            projection.get("schema_version")
            != "manyforge.source_model_projection.v1"
            or not isinstance(source, dict)
        ):
            raise HeadlessApiError(
                "source_model_projection_invalid",
                "native core returned an unsupported source model projection",
            )
        return projection

    def source_model_context(self) -> dict[str, Any]:
        context = self.core.source_model_context()
        if (
            not isinstance(context, dict)
            or set(context)
            != {"node_environment", "schema_version", "source_model"}
            or context.get("schema_version") != SOURCE_MODEL_CONTEXT_VERSION
            or not isinstance(context.get("node_environment"), dict)
            or not isinstance(context.get("source_model"), dict)
            or context["node_environment"].get("schema_version")
            != "manyforge.node_environment_projection.v2"
            or context["source_model"].get("schema_version")
            != "manyforge.source_model_projection.v1"
        ):
            raise HeadlessApiError(
                "source_model_context_invalid",
                "native core returned an unsupported source-model context",
            )
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
            != "manyforge.source_model_projection.v1"
        ):
            raise HeadlessApiError(
                "assistant_mode_context_invalid",
                "native core returned an unsupported assistant-mode context",
            )
        return context

    def scenarios(self) -> dict[str, Any]:
        projection = self.core.scenario_catalog_projection()
        if (
            set(projection)
            != {"api_version", "scenario_catalog_revision", "scenarios"}
            or projection.get("api_version") != "manyforge.scenario_catalog.v1"
            or not isinstance(projection.get("scenario_catalog_revision"), str)
            or not isinstance(projection.get("scenarios"), list)
        ):
            raise HeadlessApiError(
                "scenario_catalog_projection_invalid",
                "native core returned an unsupported scenario catalog",
            )
        return projection

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        projection = self.core.scenario_program_selection(scenario_id)
        if not isinstance(projection, dict):
            raise HeadlessApiError(
                "scenario_program_selection_invalid",
                "native core returned an unsupported scenario Program selection",
            )
        try:
            validate_scenario_program_selection(
                projection,
                scenario_id=scenario_id,
            )
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessApiError(
                "scenario_program_selection_invalid",
                "native core returned an unsupported scenario Program selection",
            ) from error
        return projection

    def scenario_load_selection(self, scenario_id: str) -> dict[str, Any]:
        projection = self.core.scenario_load_selection(scenario_id)
        if not isinstance(projection, dict):
            raise HeadlessApiError(
                "scenario_program_selection_invalid",
                "native core returned an unsupported Scenario Load selection",
            )
        try:
            validate_scenario_program_selection(
                projection,
                scenario_id=scenario_id,
                label="scenario_load_selection",
            )
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessApiError(
                "scenario_program_selection_invalid",
                "native core returned an unsupported Scenario Load selection",
            ) from error
        return projection

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
        self._validate_applied_program_semantics(
            projection,
            error_code="program_applied_selection_invalid",
            message="native core returned an unsupported Applied Program selection",
        )
        return projection

    def program_model_attempt(
        self,
        request: ProgramModelAttemptRequest,
    ) -> dict[str, Any]:
        return self.core.commit_program_model_attempt(
            request.model_dump(mode="json")
        )

    def _program_runner(self) -> ProgramRunnerPort:
        if self.program_runner is None:
            raise HeadlessApiError(
                "program_runner_unavailable",
                "independent Program runner is not configured",
            )
        return self.program_runner

    @staticmethod
    def _runner_error(error: ProgramRunnerClientError) -> HeadlessApiError:
        return HeadlessApiError(
            error.code,
            str(error),
            details=error.details,
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
            "message",
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
            "scenario_catalog_revision",
            "scenario_id",
            "schema_version",
        }
        capture = projection.get("capture")
        owner_id = projection.get("owner_id")
        owner_epoch = projection.get("owner_epoch")
        state = projection.get("state")
        active_count = projection.get("active_operation_count")
        reason = projection.get("reason_code")
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
            set(projection) != expected_fields
            or projection.get("schema_version") != PROGRAM_RUN_STATUS_VERSION
            or not _bounded_text(owner_id, maximum_bytes=512)
            or not _bounded_text(owner_epoch, maximum_bytes=512)
            or type(projection.get("run_revision")) is not int
            or not 1 <= projection["run_revision"] <= 9_007_199_254_740_991
            or state not in _PROGRAM_RUN_STATES
            or projection.get("tree_status")
            not in {"invalid", "running", "success", "failure"}
            or (
                active_count is not None
                and (
                    type(active_count) is not int
                    or not 0 <= active_count <= 4_096
                )
            )
            or (
                reason is not None
                and not _bounded_text(reason, maximum_bytes=256)
            )
            or not isinstance(projection.get("message"), str)
            or len(projection["message"].encode("utf-8")) > 4_096
            or not isinstance(capture, dict)
            or set(capture) != capture_fields
            or capture.get("schema_version") != PROGRAM_RUN_CAPTURE_VERSION
            or not _bounded_text(
                capture.get("program_run_id"), maximum_bytes=512
            )
            or not _bounded_text(capture.get("scenario_id"), maximum_bytes=128)
            or not _bounded_text(capture.get("program_id"), maximum_bytes=512)
            or any(
                not _sha256_identity(capture.get(field_name))
                for field_name in (
                    "applied_record_digest",
                    "scenario_catalog_revision",
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
        roles: list[str] = []
        runner_identity: dict[str, Any] | None = None
        for identity in owner_identities:
            if (
                not isinstance(identity, dict)
                or set(identity) != {"owner_epoch", "owner_id", "owner_role"}
                or not _bounded_text(
                    identity.get("owner_role"), maximum_bytes=128
                )
                or not _bounded_text(identity.get("owner_id"), maximum_bytes=512)
                or not _bounded_text(
                    identity.get("owner_epoch"), maximum_bytes=512
                )
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
            (
                expected_program_run_id is not None
                and capture["program_run_id"] != expected_program_run_id
            )
            or (
                expected_owner_epoch is not None
                and owner_epoch != expected_owner_epoch
            )
        ):
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
            or source.get("record_digest")
            != request.expected_applied_record_digest
            or applied.get("scenario_id") != request.scenario_id
            or applied.get("scenario_catalog_revision")
            != request.scenario_catalog_revision
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
        try:
            projection = self._program_runner().start_run(
                request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error
        snapshot = self._validate_program_run_snapshot(projection)
        capture = snapshot["capture"]
        binding = applied.get("program_binding")
        current_environment = self.node_environment()
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
                    ("scenario_catalog_revision", "scenario_catalog_revision"),
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
            or source.get("record_digest")
            != request.expected_applied_record_digest
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
            or result.get("schema_version")
            != "manyforge.program_scene_preparation.v1"
            or result.get("status") != "prepared"
            or result.get("applied_record_digest")
            != request.expected_applied_record_digest
            or result.get("program_revision") != applied.get("program_revision")
            or type(generation) is not int
            or not 1 <= generation <= 9_007_199_254_740_991
            or not isinstance(proof, dict)
            or proof.get("schema_version")
            != "manyforge.prepared_program_scene.v1"
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
        return self._validate_program_run_snapshot(
            projection,
            expected_program_run_id=program_run_id,
            expected_owner_epoch=request.expected_owner_epoch,
        )

    def program_cycle_status(self) -> dict[str, Any]:
        try:
            return self._program_runner().cycle_status()
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_cycle_reset(
        self, request: ProgramCycleResetRequest
    ) -> dict[str, Any]:
        try:
            return self._program_runner().reset_cycle(
                request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def program_runtime_controls(self, program_run_id: str) -> dict[str, Any]:
        try:
            return self._program_runner().runtime_controls(program_run_id)
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

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

    def cycle_recovery_action(
        self, request: CycleRecoveryActionRequest
    ) -> dict[str, Any]:
        try:
            return self._program_runner().apply_recovery_action(
                request.model_dump(mode="json")
            )
        except ProgramRunnerClientError as error:
            raise self._runner_error(error) from error

    def component_lifecycle_submit(
        self, request: Any
    ) -> dict[str, Any]:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        payload = request.model_dump(
            mode="json", exclude={"confirmed"}, exclude_none=True
        )
        return self.cclc.submit_lifecycle(payload)

    def component_lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any] | None:
        if self.cclc is None:
            raise HeadlessApiError(
                "cclc_open_unavailable", "CCLC owner is not configured"
            )
        return self.cclc.lifecycle_status(transaction_id)

    def _program_apply_manager_selection(
        self,
    ) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
        status = self.manager_status()
        source = status.get("source")
        payload = status.get("status")
        active = payload.get("active") if isinstance(payload, dict) else None
        runtime = payload.get("runtime") if isinstance(payload, dict) else None
        if (
            not isinstance(source, dict)
            or not isinstance(payload, dict)
            or not isinstance(active, dict)
            or not isinstance(runtime, dict)
            or source.get("available") is not True
            or source.get("freshness") != "fresh"
            or source.get("known_gaps") != []
            or not isinstance(active.get("retained_bundle_id"), str)
            or not isinstance(active.get("graph_revision"), str)
            or not isinstance(source.get("source_owner_epoch"), str)
        ):
            raise HeadlessApiError(
                "manager_source_invalid",
                "Manager source has no exact active Program Apply selection",
            )
        return source, payload, active

    @staticmethod
    def _fence_program_apply_for_package_restart(
        payload: dict[str, Any],
    ) -> None:
        apply_state = payload.get("apply")
        if (
            isinstance(apply_state, dict)
            and apply_state.get("unavailable_reason_code")
            == "full_stack_restart_required"
        ):
            raise HeadlessApiError(
                "full_stack_restart_required",
                "Program Apply requires a full stack restart",
            )

    @staticmethod
    def _program_apply_proven_no_effect(
        error: HeadlessApiError,
    ) -> HeadlessApiError:
        """Attach owner-issued proof that this Apply did not select Program."""

        return HeadlessApiError(
            error.code,
            str(error),
            details={
                **error.details,
                "effect_disposition": "proven_no_effect",
            },
        )

    def _program_apply_replay_result(
        self, replay: dict[str, Any]
    ) -> dict[str, Any]:
        preparation = replay.get("preparation")
        applied = replay.get("applied")
        if (
            set(replay) != {"applied", "preparation"}
            or not isinstance(preparation, dict)
            or not isinstance(applied, dict)
        ):
            raise HeadlessApiError(
                "program_apply_response_invalid",
                "native Program Apply replay omitted its exact outcome",
            )
        return self._program_apply_result(preparation, applied)

    def program_apply(self, request: ProgramApplyRequest) -> dict[str, Any]:
        try:
            claim = request.authoring_claim
            intent = claim.get("intent") if isinstance(claim, dict) else None
            if (
                not isinstance(intent, dict)
                or intent.get("claim_kind") != "authoring_mutation"
                or intent.get("owner_role") != "authoring_owner"
            ):
                raise HeadlessApiError(
                    "authoring_not_ready",
                    "Program Apply requires an exact held authoring claim",
                )
            replay = self._cell_admission().acquire(intent)
        except CellAdmissionError as error:
            translated = self._admission_error(error)
            raise self._program_apply_proven_no_effect(translated) from error
        except (KeyError, TypeError, ValueError) as error:
            translated = HeadlessApiError(
                "authoring_claim_invalid",
                "Program Apply authoring claim is malformed",
            )
            raise self._program_apply_proven_no_effect(translated) from error
        except HeadlessApiError as error:
            raise self._program_apply_proven_no_effect(error) from error
        if replay != claim:
            error = HeadlessApiError(
                "stale_generation",
                "Program Apply authoring claim is no longer exact",
            )
            raise self._program_apply_proven_no_effect(error)
        return self._program_apply(request)

    def _program_apply(self, request: ProgramApplyRequest) -> dict[str, Any]:
        """Select a Program only for the already-converged deployment."""

        try:
            program = Program.from_dict(
                deepcopy(request.program),
                _program_catalog(),
                path="<headless-program-apply>",
            )
        except (CatalogValidationError, ProgramValidationError) as error:
            translated = HeadlessApiError(
                "program_apply_program_invalid",
                f"Program Apply candidate is invalid: {error}",
            )
            raise self._program_apply_proven_no_effect(translated) from error
        program_document = program.to_dict()
        claim_intent = request.authoring_claim.get("intent")
        request_owner_epoch = (
            claim_intent.get("owner_epoch")
            if isinstance(claim_intent, dict)
            else None
        )
        request_id = (
            claim_intent.get("request_id")
            if isinstance(claim_intent, dict)
            else None
        )
        if (
            not isinstance(request_owner_epoch, str)
            or not request_owner_epoch
            or not isinstance(request_id, str)
            or not request_id
        ):
            error = HeadlessApiError(
                "authoring_claim_invalid",
                "Program Apply authoring claim has no request identity",
            )
            raise self._program_apply_proven_no_effect(error)
        try:
            replay = self.core.replay_program_apply(
                request.scenario_id,
                request.scenario_catalog_revision,
                str(program_document["program_id"]),
                str(program_document["program_revision"]),
                request.expected_applied_record_digest,
                request_owner_epoch,
                request_id,
            )
        except HeadlessApiError as error:
            if error.code == "program_apply_request_conflict":
                raise self._program_apply_proven_no_effect(error) from error
            raise
        if replay is not None:
            return self._program_apply_replay_result(replay)

        try:
            _source, payload, _active = self._program_apply_manager_selection()
            self._fence_program_apply_for_package_restart(payload)
            runtime = payload.get("runtime")
            if (
                payload.get("manager_ready") is not True
                or not isinstance(runtime, dict)
                or runtime.get("converged") is not True
            ):
                raise HeadlessApiError(
                    "program_apply_deployment_not_active",
                    "Program Apply requires one already-converged Manager selection",
                )

            preparation = self.core.prepare_program_apply(
                request.scenario_id,
                request.scenario_catalog_revision,
                program_document,
            )
            target_bundle = preparation.get("retained_bundle_id")
            target_graph = preparation.get("graph_revision")
            if not all(
                isinstance(value, str) and value
                for value in (
                    target_bundle,
                    target_graph,
                    preparation.get("preparation_digest"),
                )
            ):
                raise HeadlessApiError(
                    "program_apply_preparation_invalid",
                    "native Program Apply preparation omitted exact identities",
                )

            _source, payload, active = self._program_apply_manager_selection()
            self._fence_program_apply_for_package_restart(payload)
            runtime = payload.get("runtime")
            manager_matches = (
                payload.get("manager_ready") is True
                and isinstance(runtime, dict)
                and runtime.get("converged") is True
                and active["retained_bundle_id"] == target_bundle
                and active["graph_revision"] == target_graph
            )
            if not manager_matches:
                raise HeadlessApiError(
                    "program_apply_deployment_not_active",
                    "Program Apply target is no longer the converged Manager selection",
                )
        except HeadlessApiError as error:
            raise self._program_apply_proven_no_effect(error) from error

        try:
            applied = self.core.commit_program_apply(
                preparation,
                request.expected_applied_record_digest,
                request_owner_epoch,
                request_id,
            )
        except HeadlessApiError as error:
            try:
                recovered = self.core.replay_program_apply(
                    request.scenario_id,
                    request.scenario_catalog_revision,
                    str(program_document["program_id"]),
                    str(program_document["program_revision"]),
                    request.expected_applied_record_digest,
                    request_owner_epoch,
                    request_id,
                )
            except HeadlessApiError:
                raise error
            if recovered is not None:
                return self._program_apply_replay_result(recovered)
            raise self._program_apply_proven_no_effect(error) from error
        return self._program_apply_result(preparation, applied)

    def _program_apply_result(
        self, preparation: dict[str, Any], applied: dict[str, Any]
    ) -> dict[str, Any]:
        result = {
            "schema_version": PROGRAM_APPLY_RESULT_VERSION,
            "status": "applied",
            "preparation": preparation,
            "applied": applied,
        }
        try:
            validate_program_apply_result(result)
            self._validate_applied_program_semantics(
                applied,
                error_code="program_apply_response_invalid",
                message="native Program Apply returned an invalid Applied selection",
            )
        except (
            ProgramBindingTransportError,
            TypeError,
            ValueError,
            HeadlessApiError,
        ) as error:
            if isinstance(error, HeadlessApiError):
                raise
            raise HeadlessApiError(
                "program_apply_response_invalid",
                "native Program Apply returned an invalid result",
            ) from error
        return result

    @staticmethod
    def _validate_applied_program_semantics(
        applied: object,
        *,
        error_code: str,
        message: str,
    ) -> None:
        if not isinstance(applied, dict):
            raise HeadlessApiError(error_code, message)
        program = applied["program"]
        try:
            identity = verify_content_identity(
                program,
                own_field="program_revision",
                identity_domain="program-revision",
                api_version="manyforge.program.v1",
                label="applied_program.program",
            )
            validated = Program.from_dict(
                program,
                _program_catalog(),
                path="<headless-applied-program>",
            )
        except (ProgramValidationError, StrictArtifactError, TypeError, ValueError):
            raise HeadlessApiError(error_code, message)
        if (
            identity != applied["program_revision"]
            or validated.to_dict() != program
        ):
            raise HeadlessApiError(error_code, message)

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
            projection.get("schema_version")
            != "manyforge.assistant_mode_projection.v1"
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
            projection.get("schema_version")
            != "manyforge.assistant_mode_index.v1"
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
        return self.core.alarm_snapshot(
            limit=limit,
            states=states,
            ordering=ordering,
        )

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
        *,
        start_ns: int | None,
        end_ns: int | None,
        components: Sequence[str],
        event_types: Sequence[str],
        before_sequence: int | None,
        limit: int,
    ) -> dict[str, Any]:
        return self.core.history(
            start_ns=start_ns,
            end_ns=end_ns,
            components=list(components),
            event_types=list(event_types),
            before_sequence=before_sequence,
            limit=limit,
        )

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

    def manager_apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        return self.manager.apply(request)

    def manager_apply_status(
        self, apply_id: str, apply_generation: int
    ) -> dict[str, Any]:
        return self.manager.observe_apply(apply_id, apply_generation)

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
        if authored:
            if request.expected_admission_generation is None:
                raise HeadlessApiError(
                    "authoring_not_ready",
                    "authored-scene mutation requires admission generation",
                )
        return self.manager.commit_scene_transaction(
            request,
            trusted_principal_id=attribution.principal_id,
        )

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
            isinstance(alarm_health, dict)
            and alarm_health.get("state") == "available"
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
            "status_scope": "headless_projection",
            "status": "ready" if projection_ready else "degraded",
            "headless_projection_ready": projection_ready,
            "robot_readiness": {
                "available": False,
                "state": None,
                "source_owner_id": None,
                "reason_code": "robot_readiness_owner_unavailable",
            },
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
