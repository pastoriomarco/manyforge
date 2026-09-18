"""Validated adapter over the pybind11-owned H0 semantic core."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import json
from pathlib import Path
import re
from typing import Any, Mapping, Protocol, Sequence
import unicodedata
from urllib.parse import urlsplit

import rfc8785


MAX_NATIVE_RESPONSE_BYTES = 64 * 1024 * 1024
# Shared native/HTTP bound: two independently bounded projection children plus
# only a fixed, bounded allowance for the context response/envelope framing.
MAX_SOURCE_MODEL_CONTEXT_ENVELOPE_BYTES = 64 * 1024
MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES = (
    2 * MAX_NATIVE_RESPONSE_BYTES + MAX_SOURCE_MODEL_CONTEXT_ENVELOPE_BYTES
)
MAX_ASSISTANT_MODE_CONTEXT_RESPONSE_BYTES = (
    3 * MAX_NATIVE_RESPONSE_BYTES + MAX_SOURCE_MODEL_CONTEXT_ENVELOPE_BYTES
)
MAX_VIEWER_MODEL_URDF_BYTES = 16 * 1024 * 1024
MAX_VIEWER_MODEL_RESOURCE_BYTES = 256 * 1024 * 1024
MAX_VIEWER_MODEL_RESOURCE_URI_BYTES = 4096
MAX_STANDARD_REQUEST_BODY_BYTES = 1024 * 1024
MAX_PROGRAM_APPLY_REQUEST_BYTES = 17 * 1024 * 1024
MAX_DEPLOYMENT_CANDIDATE_REQUEST_BYTES = 34 * 1024 * 1024
MAX_PROGRAM_MODEL_ATTEMPT_BYTES = 256 * 1024
DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS = 5.0
MAX_MANAGER_COMMAND_TIMEOUT_SECONDS = 60.0
_SHA256_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")


class HeadlessApiError(RuntimeError):
    """Stable failure returned by a headless owner or projection adapter."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = deepcopy(dict(details or {}))


@dataclass(frozen=True, slots=True)
class HeadlessApiConfig:
    projection_root: Path
    install_root: Path
    artifact_root: Path
    program_selection_state_root: Path
    node_catalog_path: Path
    node_environment_manifest_path: Path
    alarm_catalog_path: Path
    historian_database_path: Path
    manager_run_root: Path
    execution_run_root: Path
    cell_id: str
    resource_ids: tuple[str, ...] = ()
    owner_id: str = "headless-api"
    # Deterministic override for in-process qualification only. Production
    # launchers leave this unset so every owner restart mints a new epoch.
    owner_epoch: str | None = None
    freshness_milliseconds: int = 2_000
    historian_queue_capacity: int = 1_024
    historian_record_retention: int = 100_000
    # Apply and scene commits are durable Manager transactions, not health
    # probes. Keep their local transport wait bounded but long enough for the
    # owner to return the authoritative result instead of reporting an
    # ambiguous timeout after the mutation has committed.
    manager_timeout_seconds: float = DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS
    execution_timeout_seconds: float = 10.0
    cclc_timeout_seconds: float = 5.0
    execution_event_poll_milliseconds: int = 100
    maximum_request_body_bytes: int = MAX_STANDARD_REQUEST_BODY_BYTES
    program_runner_url: str | None = None
    program_runner_timeout_seconds: float = 30.0
    deployment_compiler_path: Path | None = None
    deployment_compiler_workspace_root: Path | None = None
    deployment_compiler_prepared_roots: tuple[tuple[str, Path], ...] = ()
    deployment_compiler_timeout_seconds: float = 120.0

    def __post_init__(self) -> None:
        path_values = (
            self.projection_root,
            self.install_root,
            self.artifact_root,
            self.program_selection_state_root,
            self.node_catalog_path,
            self.node_environment_manifest_path,
            self.alarm_catalog_path,
            self.historian_database_path,
            self.manager_run_root,
            self.execution_run_root,
        )
        if any(not value.is_absolute() for value in path_values):
            raise ValueError("Headless filesystem capabilities must be absolute")
        try:
            manager_run = self.manager_run_root.resolve(strict=False)
            execution_run = self.execution_run_root.resolve(strict=False)
        except (OSError, RuntimeError) as error:
            raise ValueError("Headless run capabilities cannot be resolved") from error
        if (
            manager_run == execution_run
            or manager_run in execution_run.parents
            or execution_run in manager_run.parents
        ):
            raise ValueError("manager_run_root and execution_run_root must be disjoint trees")
        durable_paths = path_values[:-2]
        for durable_path in durable_paths:
            try:
                resolved = durable_path.resolve(strict=False)
            except (OSError, RuntimeError) as error:
                raise ValueError("Headless durable capabilities cannot be resolved") from error
            if any(
                resolved == run_root or run_root in resolved.parents or resolved in run_root.parents
                for run_root in (manager_run, execution_run)
            ):
                raise ValueError("durable Headless paths must be outside disposable run roots")
        for label, value in (("cell_id", self.cell_id), ("owner_id", self.owner_id)):
            if (
                not value
                or unicodedata.normalize("NFC", value) != value
                or len(value.encode("utf-8")) > 256
            ):
                raise ValueError(f"{label} must be bounded non-empty NFC text")
        if self.owner_epoch is not None and (
            not self.owner_epoch
            or unicodedata.normalize("NFC", self.owner_epoch) != self.owner_epoch
            or len(self.owner_epoch.encode("utf-8")) > 256
        ):
            raise ValueError("owner_epoch must be bounded non-empty NFC text")
        if tuple(sorted(set(self.resource_ids))) != self.resource_ids:
            raise ValueError("resource_ids must be strictly sorted and unique")
        if len(self.resource_ids) > 4_096:
            raise ValueError("resource_ids exceeds the 4096-resource bound")
        for resource_id in self.resource_ids:
            if (
                not resource_id
                or unicodedata.normalize("NFC", resource_id) != resource_id
                or len(resource_id.encode("utf-8")) > 1_024
            ):
                raise ValueError("resource_ids must contain bounded non-empty NFC text")
        if self.freshness_milliseconds < 1:
            raise ValueError("freshness_milliseconds must be positive")
        if not 0 < self.manager_timeout_seconds <= MAX_MANAGER_COMMAND_TIMEOUT_SECONDS:
            raise ValueError("manager_timeout_seconds must be in (0, 60]")
        if not 0 < self.execution_timeout_seconds <= 30:
            raise ValueError("execution_timeout_seconds must be in (0, 30]")
        if not 0 < self.cclc_timeout_seconds <= 30:
            raise ValueError("cclc_timeout_seconds must be in (0, 30]")
        if not 10 <= self.execution_event_poll_milliseconds <= 5_000:
            raise ValueError("execution_event_poll_milliseconds must be between 10 and 5000")
        if self.program_runner_url is not None:
            parsed_runner_url = urlsplit(self.program_runner_url)
            if (
                parsed_runner_url.scheme not in {"http", "https"}
                or not parsed_runner_url.netloc
                or parsed_runner_url.username is not None
                or parsed_runner_url.password is not None
                or parsed_runner_url.path not in {"", "/"}
                or bool(parsed_runner_url.query)
                or bool(parsed_runner_url.fragment)
            ):
                raise ValueError(
                    "program_runner_url must be an absolute credential-free HTTP(S) URL"
                )
        if not 0 < self.program_runner_timeout_seconds <= 60:
            raise ValueError("program_runner_timeout_seconds must be in (0, 60]")
        compiler_parts = (
            self.deployment_compiler_path is not None,
            self.deployment_compiler_workspace_root is not None,
            bool(self.deployment_compiler_prepared_roots),
        )
        compiler_configured = all(compiler_parts)
        if any(compiler_parts) and not compiler_configured:
            raise ValueError(
                "Deployment compiler requires an executable, workspace, and "
                "at least one prepared model root"
            )
        if compiler_configured:
            assert self.deployment_compiler_path is not None
            assert self.deployment_compiler_workspace_root is not None
            compiler_paths = (
                self.deployment_compiler_path,
                self.deployment_compiler_workspace_root,
                *(path for _, path in self.deployment_compiler_prepared_roots),
            )
            if any(not path.is_absolute() for path in compiler_paths):
                raise ValueError("Deployment compiler capabilities must be absolute")
            family_ids = tuple(
                family_id for family_id, _ in self.deployment_compiler_prepared_roots
            )
            if family_ids != tuple(sorted(set(family_ids))):
                raise ValueError("Deployment compiler prepared families must be sorted and unique")
        if not 0 < self.deployment_compiler_timeout_seconds <= 300:
            raise ValueError("deployment_compiler_timeout_seconds must be in (0, 300]")
        if not 1_024 <= self.maximum_request_body_bytes <= 1024 * 1024:
            raise ValueError("maximum_request_body_bytes must be between 1024 and 1048576")


@dataclass(frozen=True, slots=True)
class ViewerModelAsset:
    asset_kind: str
    cell_id: str
    graph_revision: str
    retained_bundle_id: str
    product_id: str
    product_contract_id: str
    product_contract_revision: str
    viewer_model_revision: str
    logical_uri: str | None
    media_type: str
    byte_size: int
    content_digest: str
    bytes: bytes


class HeadlessCorePort(Protocol):
    def runtime_snapshot(self) -> dict[str, Any]: ...

    def node_environment_projection(self) -> dict[str, Any]: ...

    def source_model_projection(self) -> dict[str, Any]: ...

    def source_model_context(self) -> dict[str, Any]: ...

    def assistant_mode_context(self, mode_id: str) -> dict[str, Any]: ...

    def scenario_catalog_projection(self) -> dict[str, Any]: ...

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]: ...

    def scenario_load_selection(self, scenario_id: str) -> dict[str, Any]: ...

    def prepare_program_apply(
        self,
        scenario_id: str,
        scenario_catalog_revision: str,
        program: dict[str, Any],
    ) -> dict[str, Any]: ...

    def replay_program_apply(
        self,
        scenario_id: str,
        scenario_catalog_revision: str,
        program_id: str,
        program_revision: str,
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any] | None: ...

    def commit_program_apply(
        self,
        preparation: dict[str, Any],
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any]: ...

    def applied_program_selection(self) -> dict[str, Any]: ...

    def viewer_model_urdf(self) -> ViewerModelAsset: ...

    def viewer_model_resource(
        self,
        expected_retained_bundle_id: str,
        logical_uri: str,
    ) -> ViewerModelAsset: ...

    def assistant_mode_index(self) -> dict[str, Any]: ...

    def assistant_mode_projection(self, mode_id: str) -> dict[str, Any]: ...

    def alarm_snapshot(
        self,
        limit: int | None = 32,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]: ...

    def alarm_metrics(self) -> dict[str, Any]: ...

    def acknowledge_alarm(self, **kwargs: Any) -> dict[str, Any]: ...

    def acknowledge_all(self, **kwargs: Any) -> dict[str, Any]: ...

    def history(self, **kwargs: Any) -> dict[str, Any]: ...
    def history_export(self, **kwargs: Any) -> dict[str, Any]: ...
    def history_discovery(self, *, kind: str, after: str, limit: int) -> dict[str, Any]: ...
    def history_detail(self, sequence: int) -> dict[str, Any]: ...
    def record_client_incident(self, **kwargs: Any) -> dict[str, Any]: ...

    def record_program_diagnostic(self, record: Mapping[str, Any]) -> dict[str, Any]: ...

    def record_gripper_confirmation(self, record: Mapping[str, Any]) -> dict[str, Any]: ...

    def historian_health(self) -> dict[str, Any]: ...

    def reconcile_historian_health(
        self,
        **kwargs: Any,
    ) -> dict[str, Any]: ...

    def commit_program_model_attempt(
        self,
        record: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]: ...

    def complete_operation_event_snapshot(
        self,
        **kwargs: Any,
    ) -> dict[str, Any]: ...

    def mark_operation_event_source_unknown(
        self,
        **kwargs: Any,
    ) -> dict[str, Any]: ...

    def shutdown(self) -> None: ...


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value} is forbidden")


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def strict_json_object(
    payload: str,
    *,
    label: str,
    maximum_bytes: int = MAX_NATIVE_RESPONSE_BYTES,
) -> dict[str, Any]:
    if len(payload.encode("utf-8")) > maximum_bytes:
        raise HeadlessApiError(
            "headless_projection_response_oversized",
            f"{label} exceeds the native response byte bound",
        )
    try:
        value = json.loads(
            payload,
            parse_constant=_reject_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except (UnicodeError, ValueError, TypeError) as exc:
        raise HeadlessApiError(
            "headless_projection_response_invalid",
            f"{label} is not strict JSON: {exc}",
        ) from exc
    if not isinstance(value, dict):
        raise HeadlessApiError(
            "headless_projection_response_invalid",
            f"{label} root must be an object",
        )
    return value


class NativeHeadlessCore:
    """Decode the canonical bridge envelope without recreating owner state."""

    def __init__(self, config: HeadlessApiConfig) -> None:
        try:
            import manyforge_headless_py  # type: ignore[import-not-found]
        except ImportError as exc:
            raise HeadlessApiError(
                "headless_native_binding_unavailable",
                "manyforge_headless_py is not importable",
            ) from exc
        try:
            self._native = manyforge_headless_py.HeadlessApiCore(
                str(config.projection_root),
                str(config.install_root),
                str(config.artifact_root),
                str(config.program_selection_state_root),
                str(config.node_catalog_path),
                str(config.node_environment_manifest_path),
                str(config.alarm_catalog_path),
                str(config.historian_database_path),
                config.cell_id,
                list(config.resource_ids),
                config.owner_id,
                config.owner_epoch,
                config.freshness_milliseconds,
                config.historian_queue_capacity,
                config.historian_record_retention,
            )
        except Exception as exc:
            raise HeadlessApiError(
                "headless_native_initialization_failed",
                f"headless native core initialization failed: {exc}",
            ) from exc

    def _invoke(self, method: str, *args: Any, **kwargs: Any) -> Any:
        payload = getattr(self._native, method)(*args, **kwargs)
        if not isinstance(payload, str):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} returned a non-text envelope",
            )
        maximum_bytes = {
            "assistant_mode_context": MAX_ASSISTANT_MODE_CONTEXT_RESPONSE_BYTES,
            "source_model_context": MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES,
        }.get(method, MAX_NATIVE_RESPONSE_BYTES)
        envelope = strict_json_object(
            payload,
            label=f"native {method} response",
            maximum_bytes=maximum_bytes,
        )
        ok = envelope.get("ok")
        if ok is True and set(envelope) == {"ok", "value"}:
            return envelope["value"]
        if ok is False and set(envelope) == {"error", "ok"}:
            error = envelope["error"]
            if (
                isinstance(error, dict)
                and set(error) == {"code", "message"}
                and isinstance(error["code"], str)
                and error["code"]
                and isinstance(error["message"], str)
                and error["message"]
            ):
                raise HeadlessApiError(error["code"], error["message"])
        raise HeadlessApiError(
            "headless_projection_response_invalid",
            f"native {method} returned an invalid envelope",
        )

    def _invoke_viewer_asset(
        self,
        method: str,
        *args: Any,
    ) -> ViewerModelAsset:
        envelope = getattr(self._native, method)(*args)
        if not isinstance(envelope, dict):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} returned a non-object asset envelope",
            )
        ok = envelope.get("ok")
        if ok is False and set(envelope) == {"error", "ok"}:
            error = envelope["error"]
            if (
                isinstance(error, dict)
                and set(error) == {"code", "message"}
                and isinstance(error.get("code"), str)
                and error["code"]
                and isinstance(error.get("message"), str)
                and error["message"]
            ):
                raise HeadlessApiError(error["code"], error["message"])
        if ok is not True or set(envelope) != {"bytes", "metadata", "ok"}:
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} returned an invalid asset envelope",
            )
        raw_bytes = envelope["bytes"]
        metadata_text = envelope["metadata"]
        if not isinstance(raw_bytes, bytes) or not isinstance(metadata_text, str):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} returned invalid asset fields",
            )
        metadata = strict_json_object(
            metadata_text,
            label=f"native {method} metadata",
        )
        expected_fields = {
            "asset_kind",
            "byte_size",
            "cell_id",
            "content_digest",
            "graph_revision",
            "logical_uri",
            "media_type",
            "product_contract_id",
            "product_contract_revision",
            "product_id",
            "retained_bundle_id",
            "schema_version",
            "viewer_model_revision",
        }
        if (
            set(metadata) != expected_fields
            or metadata.get("schema_version") != "manyforge.viewer_model_asset.v1"
        ):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} returned unsupported asset metadata",
            )
        asset_kind = metadata["asset_kind"]
        logical_uri = metadata["logical_uri"]
        if asset_kind == "urdf":
            if logical_uri is not None:
                raise HeadlessApiError(
                    "headless_projection_response_invalid",
                    "viewer URDF metadata unexpectedly names a resource URI",
                )
            maximum_bytes = MAX_VIEWER_MODEL_URDF_BYTES
        elif asset_kind == "resource":
            if (
                not isinstance(logical_uri, str)
                or not logical_uri
                or len(logical_uri.encode("utf-8")) > MAX_VIEWER_MODEL_RESOURCE_URI_BYTES
            ):
                raise HeadlessApiError(
                    "headless_projection_response_invalid",
                    "viewer resource metadata has an invalid logical URI",
                )
            maximum_bytes = MAX_VIEWER_MODEL_RESOURCE_BYTES
        else:
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                "viewer asset kind is unsupported",
            )
        byte_size = metadata["byte_size"]
        if (
            isinstance(byte_size, bool)
            or not isinstance(byte_size, int)
            or byte_size <= 0
            or byte_size > maximum_bytes
            or byte_size != len(raw_bytes)
        ):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                "viewer asset byte size is invalid",
            )
        text_fields = (
            "cell_id",
            "graph_revision",
            "media_type",
            "product_contract_id",
            "product_id",
        )
        if any(
            not isinstance(metadata[field], str) or not metadata[field] for field in text_fields
        ):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                "viewer asset metadata has an invalid text identity",
            )
        for digest_field in (
            "content_digest",
            "product_contract_revision",
            "retained_bundle_id",
            "viewer_model_revision",
        ):
            value = metadata[digest_field]
            if not isinstance(value, str) or _SHA256_PATTERN.fullmatch(value) is None:
                raise HeadlessApiError(
                    "headless_projection_response_invalid",
                    f"viewer asset {digest_field} is not a SHA-256 digest",
                )
        return ViewerModelAsset(
            asset_kind=asset_kind,
            cell_id=metadata["cell_id"],
            graph_revision=metadata["graph_revision"],
            retained_bundle_id=metadata["retained_bundle_id"],
            product_id=metadata["product_id"],
            product_contract_id=metadata["product_contract_id"],
            product_contract_revision=metadata["product_contract_revision"],
            viewer_model_revision=metadata["viewer_model_revision"],
            logical_uri=logical_uri,
            media_type=metadata["media_type"],
            byte_size=byte_size,
            content_digest=metadata["content_digest"],
            bytes=raw_bytes,
        )

    @staticmethod
    def _require_object(value: Any, *, method: str) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise HeadlessApiError(
                "headless_projection_response_invalid",
                f"native {method} value must be an object",
            )
        return value

    def runtime_snapshot(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("runtime_snapshot"),
            method="runtime_snapshot",
        )

    def node_environment_projection(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("node_environment_projection"),
            method="node_environment_projection",
        )

    def source_model_projection(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("source_model_projection"),
            method="source_model_projection",
        )

    def source_model_context(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("source_model_context"),
            method="source_model_context",
        )

    def assistant_mode_context(self, mode_id: str) -> dict[str, Any]:
        return self._require_object(
            self._invoke("assistant_mode_context", mode_id),
            method="assistant_mode_context",
        )

    def scenario_catalog_projection(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("scenario_catalog_projection"),
            method="scenario_catalog_projection",
        )

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        return self._require_object(
            self._invoke("scenario_program_selection", scenario_id),
            method="scenario_program_selection",
        )

    def scenario_load_selection(self, scenario_id: str) -> dict[str, Any]:
        return self._require_object(
            self._invoke("scenario_load_selection", scenario_id),
            method="scenario_load_selection",
        )

    @staticmethod
    def _canonical_object(value: dict[str, Any], *, label: str) -> str:
        try:
            encoded = rfc8785.dumps(value)
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
            raise HeadlessApiError(
                "program_apply_request_invalid",
                f"{label} is not canonically encodable: {exc}",
            ) from exc
        if len(encoded) > MAX_PROGRAM_APPLY_REQUEST_BYTES:
            raise HeadlessApiError(
                "program_apply_request_oversized",
                f"{label} exceeds the Program Apply byte bound",
            )
        return encoded.decode("utf-8", errors="strict")

    def prepare_program_apply(
        self,
        scenario_id: str,
        scenario_catalog_revision: str,
        program: dict[str, Any],
    ) -> dict[str, Any]:
        return self._require_object(
            self._invoke(
                "prepare_program_apply",
                scenario_id,
                scenario_catalog_revision,
                self._canonical_object(program, label="Program Apply candidate"),
            ),
            method="prepare_program_apply",
        )

    def replay_program_apply(
        self,
        scenario_id: str,
        scenario_catalog_revision: str,
        program_id: str,
        program_revision: str,
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any] | None:
        value = self._invoke(
            "replay_program_apply",
            scenario_id,
            scenario_catalog_revision,
            program_id,
            program_revision,
            expected_applied_record_digest,
            request_owner_epoch,
            request_id,
        )
        if value is None:
            return None
        return self._require_object(value, method="replay_program_apply")

    def commit_program_apply(
        self,
        preparation: dict[str, Any],
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any]:
        return self._require_object(
            self._invoke(
                "commit_program_apply",
                self._canonical_object(
                    preparation,
                    label="Program Apply preparation",
                ),
                expected_applied_record_digest,
                request_owner_epoch,
                request_id,
            ),
            method="commit_program_apply",
        )

    def applied_program_selection(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("applied_program_selection"),
            method="applied_program_selection",
        )

    def viewer_model_urdf(self) -> ViewerModelAsset:
        return self._invoke_viewer_asset("viewer_model_urdf")

    def viewer_model_resource(
        self,
        expected_retained_bundle_id: str,
        logical_uri: str,
    ) -> ViewerModelAsset:
        return self._invoke_viewer_asset(
            "viewer_model_resource",
            expected_retained_bundle_id,
            logical_uri,
        )

    def assistant_mode_index(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("assistant_mode_index"),
            method="assistant_mode_index",
        )

    def assistant_mode_projection(self, mode_id: str) -> dict[str, Any]:
        return self._require_object(
            self._invoke("assistant_mode_projection", mode_id),
            method="assistant_mode_projection",
        )

    def alarm_snapshot(
        self,
        limit: int | None = 32,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]:
        return self._require_object(
            self._invoke("alarm_snapshot", limit, list(states), ordering),
            method="alarm_snapshot",
        )

    def alarm_metrics(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("alarm_metrics"),
            method="alarm_metrics",
        )

    def acknowledge_alarm(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("acknowledge_alarm", **kwargs),
            method="acknowledge_alarm",
        )

    def acknowledge_all(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("acknowledge_all", **kwargs),
            method="acknowledge_all",
        )

    def history(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            # The list endpoint never needs full payloads. Project its bounded
            # rows while the native call has released the Python GIL; exact
            # payloads remain available through history_detail().
            self._invoke("history_query", **kwargs),
            method="history",
        )

    def history_export(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("history_query", full_records=True, **kwargs),
            method="history_export",
        )

    def history_detail(self, sequence: int) -> dict[str, Any]:
        return self._require_object(
            self._invoke("history_detail", sequence),
            method="history_detail",
        )

    def history_discovery(self, *, kind: str, after: str, limit: int) -> dict[str, Any]:
        return self._require_object(
            self._invoke("history_discovery", kind=kind, after=after, limit=limit),
            method="history_discovery",
        )

    def record_client_incident(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("record_client_incident", **kwargs),
            method="record_client_incident",
        )

    def record_program_diagnostic(self, record: Mapping[str, Any]) -> dict[str, Any]:
        encoded = rfc8785.dumps(dict(record))
        if len(encoded) > 64 * 1024:
            raise HeadlessApiError(
                "program_diagnostic_invalid",
                "Program diagnostic exceeds its byte bound",
            )
        return self._require_object(
            self._invoke("record_program_diagnostic", encoded.decode("utf-8")),
            method="record_program_diagnostic",
        )

    def record_gripper_confirmation(self, record: Mapping[str, Any]) -> dict[str, Any]:
        encoded = rfc8785.dumps(dict(record))
        if len(encoded) > 8192:
            raise HeadlessApiError(
                "gripper_confirmation_trace_invalid",
                "Gripper confirmation trace exceeds its byte bound",
            )
        return self._require_object(
            self._invoke("record_gripper_confirmation", encoded.decode("utf-8")),
            method="record_gripper_confirmation",
        )

    def historian_health(self) -> dict[str, Any]:
        return self._require_object(
            self._invoke("historian_health"),
            method="historian_health",
        )

    def reconcile_historian_health(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("reconcile_historian_health", **kwargs),
            method="reconcile_historian_health",
        )

    def commit_program_model_attempt(
        self,
        record: Mapping[str, Any],
    ) -> dict[str, Any]:
        if not isinstance(record, Mapping):
            raise TypeError("Program model-attempt record must be a mapping")
        try:
            encoded = rfc8785.dumps(dict(record))
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
            raise HeadlessApiError(
                "headless_program_model_attempt_invalid",
                f"Program model-attempt record is not canonical JSON: {exc}",
            ) from exc
        if len(encoded) > MAX_PROGRAM_MODEL_ATTEMPT_BYTES:
            raise HeadlessApiError(
                "headless_program_model_attempt_invalid",
                "Program model-attempt record exceeds its byte bound",
            )
        return self._require_object(
            self._invoke(
                "commit_program_model_attempt",
                encoded.decode("utf-8", errors="strict"),
            ),
            method="commit_program_model_attempt",
        )

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]:
        return self._require_object(
            self._invoke("ingest_operation_event", **kwargs),
            method="ingest_operation_event",
        )

    def complete_operation_event_snapshot(
        self,
        **kwargs: Any,
    ) -> dict[str, Any]:
        return self._require_object(
            self._invoke("complete_operation_event_snapshot", **kwargs),
            method="complete_operation_event_snapshot",
        )

    def mark_operation_event_source_unknown(
        self,
        **kwargs: Any,
    ) -> dict[str, Any]:
        return self._require_object(
            self._invoke("mark_operation_event_source_unknown", **kwargs),
            method="mark_operation_event_source_unknown",
        )

    def shutdown(self) -> None:
        self._native.shutdown()
