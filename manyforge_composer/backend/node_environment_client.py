"""Independent validation of the Manager-selected headless node environment."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from manyforge_behavior.catalog import CatalogValidationError, NodeCatalog
from manyforge_behavior.executable_node_resolver import build_executable_node_resolver

if TYPE_CHECKING:
    from manyforge_behavior.node_environment import (
        NodeEnvironment,
        NodeEnvironmentManifest,
    )

    from .state import ComposerState


_PROJECTION_VERSION = "manyforge.node_environment_projection.v1"
_PROJECTION_FIELDS = {
    "availability",
    "catalog",
    "catalog_digest",
    "catalog_id",
    "content_version",
    "environment_digest",
    "graph_revision",
    "manifest",
    "retained_bundle_id",
    "schema_version",
    "source",
}
_AVAILABILITY_FIELDS = {
    "binding_context",
    "execution_available",
    "implementations",
    "manager_runtime_converged",
    "portable_authoring_available",
    "reason_codes",
    "restoration_gates",
}
_IMPLEMENTATION_FIELDS = {
    "adapter_build_qualified",
    "authoring_available",
    "binding_available",
    "engine_id",
    "executable_package_id",
    "execution_available",
    "implementation_id",
    "implementation_identity_resolved",
    "node_id",
    "owner_health_available",
    "reason_codes",
    "restoration_gates",
}


class ActiveNodeEnvironmentProjectionError(RuntimeError):
    """The active headless projection is unavailable or not independently valid."""

    def __init__(
        self,
        code: str,
        message: str,
        status_code: int = 503,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


@dataclass(frozen=True, slots=True)
class VerifiedActiveNodeEnvironment:
    payload: dict[str, Any]
    catalog: NodeCatalog
    manifest: "NodeEnvironmentManifest"


def _invalid(message: str) -> ActiveNodeEnvironmentProjectionError:
    return ActiveNodeEnvironmentProjectionError(
        "node_environment_projection_invalid",
        message,
    )


def _object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise _invalid(f"{label} must be an object")
    return value


def _closed(value: dict[str, Any], fields: set[str], label: str) -> None:
    if set(value) != fields:
        raise _invalid(f"{label} has an unsupported shape")


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
        raise _invalid(f"{label} must be a non-empty canonical string")
    return value


def _text_list(value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or len(value) > 4096:
        raise _invalid(f"{label} must be a bounded array")
    result = [_text(item, f"{label}[{index}]") for index, item in enumerate(value)]
    if len(result) != len(set(result)):
        raise _invalid(f"{label} contains duplicates")
    return result


def _validate_source(value: Any) -> None:
    source = _object(value, "node environment source")
    if source.get("available") is not True or source.get("freshness") != "fresh":
        raise ActiveNodeEnvironmentProjectionError(
            "node_environment_projection_unavailable",
            "the active node environment source is unavailable or stale",
        )
    known_gaps = source.get("known_gaps")
    if not isinstance(known_gaps, list) or known_gaps:
        raise ActiveNodeEnvironmentProjectionError(
            "node_environment_projection_unavailable",
            "the active node environment source is incomplete",
        )
    for field in (
        "projection_owner_epoch",
        "source_owner_epoch",
        "source_owner_id",
        "source_content_digest",
    ):
        _text(source.get(field), f"node environment source.{field}")
    for field in (
        "projection_generation",
        "receiver_age_ns",
        "source_generation",
    ):
        item = source.get(field)
        if type(item) is not int or item < 0:
            raise _invalid(f"node environment source.{field} must be non-negative")


def _validate_availability(
    value: Any,
    environment: "NodeEnvironment",
) -> None:
    availability = _object(value, "node environment availability")
    _closed(availability, _AVAILABILITY_FIELDS, "node environment availability")
    _text(availability["binding_context"], "availability.binding_context")
    for field in (
        "execution_available",
        "manager_runtime_converged",
        "portable_authoring_available",
    ):
        if not isinstance(availability[field], bool):
            raise _invalid(f"availability.{field} must be a boolean")
    _text_list(availability["reason_codes"], "availability.reason_codes")
    _text_list(availability["restoration_gates"], "availability.restoration_gates")

    implementations = availability["implementations"]
    if not isinstance(implementations, list):
        raise _invalid("availability.implementations must be an array")
    expected = environment.manifest.executable_registry
    dispositions = {
        item.row: item
        for item in build_executable_node_resolver(environment).dispositions
    }
    if len(implementations) != len(expected):
        raise _invalid("availability does not cover the complete executable registry")
    for index, (raw, registry_row) in enumerate(zip(implementations, expected)):
        row = _object(raw, f"availability.implementations[{index}]")
        _closed(
            row,
            _IMPLEMENTATION_FIELDS,
            f"availability.implementations[{index}]",
        )
        for field in (
            "adapter_build_qualified",
            "authoring_available",
            "binding_available",
            "execution_available",
            "implementation_identity_resolved",
            "owner_health_available",
        ):
            if not isinstance(row[field], bool):
                raise _invalid(
                    f"availability.implementations[{index}].{field} must be a boolean"
                )
        if (
            row["node_id"],
            row["engine_id"],
            row["implementation_id"],
            row["executable_package_id"],
        ) != (
            registry_row.node_id,
            registry_row.engine_id,
            registry_row.implementation_id,
            registry_row.executable_package_id,
        ):
            raise _invalid("availability implementation identity is not registry-bound")
        reasons = _text_list(
            row["reason_codes"],
            f"availability.implementations[{index}].reason_codes",
        )
        gates = _text_list(
            row["restoration_gates"],
            f"availability.implementations[{index}].restoration_gates",
        )
        if row["execution_available"] and not all(
            row[field]
            for field in (
                "adapter_build_qualified",
                "binding_available",
                "implementation_identity_resolved",
                "owner_health_available",
            )
        ):
            raise _invalid("availability execution claim lacks a required dimension")
        if row["adapter_build_qualified"] == bool(gates):
            raise _invalid(
                "availability adapter disposition and restoration gates disagree"
            )
        disposition = dispositions[registry_row]
        if row[
            "adapter_build_qualified"
        ] is not disposition.adapter_build_qualified or gates != list(
            disposition.restoration_gates
        ):
            raise _invalid(
                "availability adapter disposition does not match the local "
                "build-qualified resolver"
            )
        if (
            disposition.unavailable_reason_code is not None
            and disposition.unavailable_reason_code not in reasons
        ):
            raise _invalid("availability omits the build-qualified adapter reason")
        if (
            disposition.adapter_build_qualified
            and disposition.owner_port_name is None
            and row["owner_health_available"] is not True
        ):
            raise _invalid(
                "owner-neutral adapter cannot report an unavailable semantic owner"
            )


def verify_active_node_environment(
    state: "ComposerState",
    payload: Any,
) -> VerifiedActiveNodeEnvironment:
    """Verify one fresh projection against the same build-owned schema inputs."""

    projection = _object(payload, "node environment projection")
    _closed(projection, _PROJECTION_FIELDS, "node environment projection")
    if projection.get("schema_version") != _PROJECTION_VERSION:
        raise _invalid("node environment projection version is unsupported")
    _validate_source(projection["source"])

    raw_catalog = _object(projection["catalog"], "node environment catalog")
    try:
        catalog = NodeCatalog.from_data(
            raw_catalog,
            source="<active-headless-node-catalog>",
        )
    except CatalogValidationError as error:
        raise _invalid(f"active node catalog failed validation: {error}") from error

    local_environment = state.node_environment
    local_manifest = local_environment.manifest
    raw_manifest = _object(projection["manifest"], "node environment manifest")
    if raw_catalog != local_environment.catalog.raw_data:
        raise _invalid("active catalog does not match the build-qualified catalog")
    if raw_manifest != local_manifest.to_dict():
        raise _invalid("active manifest does not match the build-qualified environment")
    if (
        projection["catalog_id"] != catalog.catalog_id
        or projection["content_version"] != catalog.content_version
        or projection["catalog_digest"] != catalog.catalog_digest
        or projection["environment_digest"] != local_manifest.environment_digest
    ):
        raise _invalid("active node environment identities disagree")
    _text(projection["retained_bundle_id"], "retained_bundle_id")
    _text(projection["graph_revision"], "graph_revision")
    _validate_availability(projection["availability"], local_environment)
    return VerifiedActiveNodeEnvironment(
        payload=projection,
        catalog=catalog,
        manifest=local_manifest,
    )


def read_active_node_environment(
    state: "ComposerState",
) -> VerifiedActiveNodeEnvironment:
    client = state.headless_client
    if client is None:
        raise _invalid("Composer has no configured headless client")
    try:
        payload = client.node_environment()
    except Exception as error:
        code = getattr(error, "code", None)
        if isinstance(code, str) and code:
            status_code = getattr(error, "status_code", None)
            raise ActiveNodeEnvironmentProjectionError(
                code,
                str(error),
                status_code if isinstance(status_code, int) else 503,
            ) from error
        raise
    return verify_active_node_environment(state, payload)


__all__ = [
    "ActiveNodeEnvironmentProjectionError",
    "VerifiedActiveNodeEnvironment",
    "read_active_node_environment",
    "verify_active_node_environment",
]
