"""Owner-neutral projections shared by FastAPI routes and tests."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from functools import lru_cache
import hashlib
from typing import Any, Sequence

from manyforge_behavior.catalog import (
    CatalogValidationError,
    NodeCatalog,
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
    ProgramApplyRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
    StopIntentRequest,
)
from .native import HeadlessApiError, HeadlessCorePort, ViewerModelAsset


RUNTIME_SNAPSHOT_VERSION = "manyforge.headless_runtime_snapshot.v1"
MANAGER_SOURCE_VERSION = "manyforge.manager_source.v1"
PROGRAM_APPLY_RESULT_VERSION = "manyforge.program_apply_result.v1"
APPLIED_PROGRAM_SELECTION_VERSION = "manyforge.applied_program_selection.v1"


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
    ) -> None:
        self.core = core
        self.manager = manager
        self.execution = execution

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
            != "manyforge.node_environment_projection.v1"
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

    def program_applied(self) -> dict[str, Any]:
        projection = self.core.applied_program_selection()
        if (
            projection.get("schema_version")
            != APPLIED_PROGRAM_SELECTION_VERSION
            or not isinstance(projection.get("program_revision"), str)
            or not isinstance(projection.get("program_binding_id"), str)
            or not isinstance(projection.get("retained_bundle_id"), str)
            or not isinstance(projection.get("graph_revision"), str)
            or not isinstance(projection.get("source"), dict)
        ):
            raise HeadlessApiError(
                "program_applied_selection_invalid",
                "native core returned an unsupported Applied Program selection",
            )
        return projection

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

    def program_apply(self, request: ProgramApplyRequest) -> dict[str, Any]:
        """Prepare, converge only a differing deployment, then CAS Applied."""

        try:
            program = Program.from_dict(
                deepcopy(request.program),
                _program_catalog(),
                path="<headless-program-apply>",
            )
        except (CatalogValidationError, ProgramValidationError) as error:
            raise HeadlessApiError(
                "program_apply_program_invalid",
                f"Program Apply candidate is invalid: {error}",
            ) from error
        preparation = self.core.prepare_program_apply(
            request.scenario_id,
            request.scenario_catalog_revision,
            program.to_dict(),
        )
        target_bundle = preparation.get("retained_bundle_id")
        target_graph = preparation.get("graph_revision")
        preparation_digest = preparation.get("preparation_digest")
        if not all(
            isinstance(value, str) and value
            for value in (target_bundle, target_graph, preparation_digest)
        ):
            raise HeadlessApiError(
                "program_apply_preparation_invalid",
                "native Program Apply preparation omitted exact identities",
            )

        source, payload, active = self._program_apply_manager_selection()
        deployment_apply_required = (
            active["retained_bundle_id"] != target_bundle
            or active["graph_revision"] != target_graph
        )
        manager_response: dict[str, Any] | None = None
        if deployment_apply_required:
            apply_state = payload.get("apply")
            if (
                not isinstance(apply_state, dict)
                or apply_state.get("mutation_available") is not True
            ):
                reason = (
                    apply_state.get("unavailable_reason_code")
                    if isinstance(apply_state, dict)
                    else None
                )
                raise HeadlessApiError(
                    reason
                    if isinstance(reason, str) and reason
                    else "manager_apply_unavailable",
                    "Program Apply requires a deployment change, but Manager Apply "
                    "is unavailable",
                )
            request_identity = "\0".join(
                (
                    preparation_digest,
                    active["retained_bundle_id"],
                    active["graph_revision"],
                )
            )
            request_suffix = hashlib.sha256(
                request_identity.encode("ascii")
            ).hexdigest()[:24]
            manager_response = self.manager.apply(
                ManagerApplyRequest(
                    request_id=f"program-apply-{request_suffix}",
                    request_owner_epoch=source["source_owner_epoch"],
                    source_label="headless_program_apply",
                    correlation_id=None,
                    deployment_artifact_id=preparation["deployment_artifact_id"],
                    deployment_revision=preparation["deployment_revision"],
                    expected_current={
                        "retained_bundle_id": active["retained_bundle_id"],
                        "graph_revision": active["graph_revision"],
                    },
                    reject_if_affected_busy=True,
                )
            )
            if manager_response.get("ok") is not True:
                error = manager_response.get("error")
                code = error.get("code") if isinstance(error, dict) else None
                message = error.get("message") if isinstance(error, dict) else None
                raise HeadlessApiError(
                    code
                    if isinstance(code, str) and code
                    else "manager_apply_rejected",
                    message
                    if isinstance(message, str) and message
                    else "Manager rejected the Program deployment Apply",
                )
            _source, payload, active = self._program_apply_manager_selection()

        runtime = payload.get("runtime")
        manager_matches = (
            payload.get("manager_ready") is True
            and isinstance(runtime, dict)
            and runtime.get("converged") is True
            and active["retained_bundle_id"] == target_bundle
            and active["graph_revision"] == target_graph
        )
        if not manager_matches:
            return {
                "schema_version": PROGRAM_APPLY_RESULT_VERSION,
                "status": "pending_deployment",
                "deployment_apply_required": deployment_apply_required,
                "preparation": preparation,
                "manager_response": manager_response,
                "applied": None,
            }

        applied = self.core.commit_program_apply(
            preparation,
            request.expected_applied_record_digest,
        )
        return {
            "schema_version": PROGRAM_APPLY_RESULT_VERSION,
            "status": "applied",
            "deployment_apply_required": deployment_apply_required,
            "preparation": preparation,
            "manager_response": manager_response,
            "applied": applied,
        }

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

    def scene_commit(
        self,
        request: SceneTransactionRequest,
        attribution: TrustedAttribution,
    ) -> dict[str, Any]:
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
