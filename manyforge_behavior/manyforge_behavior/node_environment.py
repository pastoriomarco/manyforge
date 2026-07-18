"""Discovery and immutable publication of catalog/registry environments."""

from __future__ import annotations

import hashlib
import json
import os
import time
from dataclasses import dataclass, replace
from pathlib import Path
from types import MappingProxyType
from typing import Any, Iterable, Mapping

from manyforge_behavior.catalog import CatalogValidationError, NodeCatalog
from manyforge_behavior.node_descriptor import (
    NodeDescriptor,
    NodeDescriptorError,
    load_node_descriptor,
)
from manyforge_behavior.node_registry import (
    NodeFactory,
    NodeRegistry,
    NodeRegistryError,
    build_node_registry,
    load_external_node_factory,
)


DEFAULT_EXTERNAL_NODE_ROOTS = (
    Path("/workspace/manyforge_nodes"),
    Path.home() / ".manyforge" / "nodes",
)


@dataclass(frozen=True)
class NodeDiscoveryDiagnostic:
    path: str
    status: str
    node_id: str | None = None
    version: str | None = None
    error_code: str | None = None
    message: str = ""


@dataclass(frozen=True)
class NodeDiscoveryReport:
    attempted_at_ns: int
    roots: tuple[str, ...]
    strict: bool
    accepted_count: int
    rejected_count: int
    diagnostics: tuple[NodeDiscoveryDiagnostic, ...]
    effective_catalog_hash: str | None
    previous_catalog_hash: str | None = None
    added_count: int = 0
    changed_count: int = 0
    removed_count: int = 0
    unchanged_count: int = 0
    activated: bool = False
    conflict: str | None = None


@dataclass(frozen=True)
class NodeSource:
    node_id: str
    kind: str
    path: str
    content_hash: str
    version: str


@dataclass(frozen=True)
class NodeEnvironment:
    catalog: NodeCatalog
    registry: NodeRegistry
    effective_catalog_hash: str
    sources: tuple[NodeSource, ...]
    descriptors: Mapping[str, NodeDescriptor]
    report: NodeDiscoveryReport
    generation: int = 1


class NodeEnvironmentBuildError(ValueError):
    def __init__(self, message: str, report: NodeDiscoveryReport):
        super().__init__(message)
        self.report = report


def validate_environment_availability(
    environment: NodeEnvironment,
    deployment: Any,
    program: Any | None = None,
) -> None:
    """Fail before run when external execution/resources are unavailable."""
    if deployment.external_nodes.strict:
        unavailable = [
            diagnostic
            for diagnostic in environment.report.diagnostics
            if diagnostic.status != "accepted"
        ]
        if unavailable:
            summary = "; ".join(
                f"{item.path}: {item.message or item.status}"
                for item in unavailable
            )
            raise ValueError(
                "external_nodes.strict rejects incomplete node discovery: " + summary
            )
    configured_enabled = deployment.external_nodes.enabled
    enabled = (
        set(environment.descriptors)
        if configured_enabled is None
        else set(configured_enabled)
    )
    unknown_enabled = sorted(enabled - set(environment.descriptors))
    if unknown_enabled:
        raise ValueError(
            f"external_nodes.enabled references undiscovered nodes {unknown_enabled}"
        )

    used: set[str] = set()
    used_nodes: list[Any] = []
    if program is not None and program.tree is not None:
        def visit(node: Any) -> None:
            used.add(node.id)
            used_nodes.append(node)
            for child in node.children:
                visit(child)
        visit(program.tree)
    disabled = sorted((used & set(environment.descriptors)) - enabled)
    if disabled:
        raise ValueError(
            f"program uses external nodes not enabled by deployment: {disabled}"
        )

    resources = dict(deployment.runtime_resources)
    for node in used_nodes:
        node_id = node.id
        if node_id not in environment.descriptors:
            continue
        entry = environment.catalog.get(node_id)
        for requirement in entry.runtime_resources:
            if not requirement.get("required", True):
                continue
            resource_type = requirement.get("type")
            if not isinstance(resource_type, str) or not resource_type:
                raise ValueError(
                    f"external node '{node_id}' has invalid runtime resource type"
                )
            if resource_type not in resources and f"{resource_type}s" not in resources:
                raise ValueError(
                    f"external node '{node_id}' requires runtime resource type "
                    f"'{resource_type}'"
                )
            aliases = resources.get(resource_type)
            if aliases is None:
                aliases = resources.get(f"{resource_type}s")
            if not isinstance(aliases, Mapping):
                raise ValueError(
                    f"runtime resource type '{resource_type}' must map aliases"
                )
            alias_parameter = requirement.get("alias_parameter")
            declared = {parameter.name: parameter for parameter in entry.parameters}
            parameter = declared.get(alias_parameter)
            alias = node.params.get(
                alias_parameter,
                parameter.default if parameter is not None else None,
            )
            if not isinstance(alias, str) or not alias.strip():
                raise ValueError(
                    f"external node '{node_id}' must select a non-empty "
                    f"runtime resource alias with parameter '{alias_parameter}'"
                )
            if alias not in aliases:
                raise ValueError(
                    f"external node '{node_id}' requires unavailable runtime "
                    f"resource '{resource_type}:{alias}'"
                )

    for mode_id, mode in deployment.assistant_modes.items():
        disabled_mode_nodes = sorted(
            (set(mode.catalog.nodes) & set(environment.descriptors)) - enabled
        )
        if disabled_mode_nodes:
            raise ValueError(
                f"assistant mode '{mode_id}' references external nodes not enabled "
                f"by deployment: {disabled_mode_nodes}"
            )


def configured_node_roots(value: str | None = None) -> tuple[Path, ...]:
    raw = os.environ.get("MANYFORGE_NODE_ROOTS", "") if value is None else value
    if not raw.strip():
        return DEFAULT_EXTERNAL_NODE_ROOTS
    roots = tuple(Path(item).expanduser() for item in raw.split(os.pathsep) if item.strip())
    return roots or DEFAULT_EXTERNAL_NODE_ROOTS


def _report(
    *,
    roots: tuple[Path, ...],
    strict: bool,
    diagnostics: list[NodeDiscoveryDiagnostic],
    effective_hash: str | None,
) -> NodeDiscoveryReport:
    return NodeDiscoveryReport(
        attempted_at_ns=time.time_ns(),
        roots=tuple(str(root) for root in roots),
        strict=strict,
        accepted_count=sum(item.status == "accepted" for item in diagnostics),
        rejected_count=sum(item.status == "rejected" for item in diagnostics),
        diagnostics=tuple(diagnostics),
        effective_catalog_hash=effective_hash,
    )


def build_node_environment(
    builtin_catalog_path: str | Path,
    *,
    external_roots: Iterable[str | Path] = (),
    strict: bool = False,
    generation: int = 1,
) -> NodeEnvironment:
    """Build a complete candidate without mutating any active runtime state."""
    builtin_path = Path(builtin_catalog_path).resolve(strict=True)
    roots = tuple(Path(root).expanduser() for root in external_roots)
    diagnostics: list[NodeDiscoveryDiagnostic] = []
    descriptors: dict[str, NodeDescriptor] = {}
    external_factories: dict[str, NodeFactory] = {}
    duplicate_external_ids: set[str] = set()

    for root in roots:
        if not root.exists():
            status = "rejected" if strict else "warning"
            diagnostics.append(
                NodeDiscoveryDiagnostic(
                    path=str(root), status=status, message="node root does not exist"
                )
            )
            continue
        if not root.is_dir():
            diagnostics.append(
                NodeDiscoveryDiagnostic(
                    path=str(root), status="rejected", message="node root is not a directory"
                )
            )
            continue
        for folder in sorted(path for path in root.iterdir() if path.is_dir()):
            try:
                descriptor = load_node_descriptor(folder)
                if descriptor.node_id in descriptors:
                    duplicate_external_ids.add(descriptor.node_id)
                    diagnostics.append(
                        NodeDiscoveryDiagnostic(
                            path=str(folder),
                            status="rejected",
                            node_id=descriptor.node_id,
                            version=descriptor.version,
                            error_code="duplicate_node_id",
                            message=(
                                f"duplicate external node id '{descriptor.node_id}'"
                            ),
                        )
                    )
                    continue
                try:
                    factory = load_external_node_factory(descriptor)
                except NodeRegistryError as exc:
                    diagnostics.append(
                        NodeDiscoveryDiagnostic(
                            path=str(folder),
                            status="rejected",
                            node_id=descriptor.node_id,
                            version=descriptor.version,
                            error_code="runtime_import_failed",
                            message=str(exc),
                        )
                    )
                    continue
                descriptors[descriptor.node_id] = descriptor
                external_factories[descriptor.node_id] = factory
                diagnostics.append(
                    NodeDiscoveryDiagnostic(
                        path=str(folder),
                        status="accepted",
                        node_id=descriptor.node_id,
                        version=descriptor.version,
                    )
                )
            except (OSError, NodeDescriptorError) as exc:
                diagnostics.append(
                    NodeDiscoveryDiagnostic(
                        path=str(folder),
                        status="rejected",
                        error_code="invalid_descriptor",
                        message=str(exc),
                    )
                )

    if duplicate_external_ids:
        for node_id in duplicate_external_ids:
            descriptors.pop(node_id, None)
            external_factories.pop(node_id, None)
        diagnostics = [
            replace(
                diagnostic,
                status="rejected",
                error_code="duplicate_node_id",
                message=f"duplicate external node id '{diagnostic.node_id}'",
            )
            if diagnostic.node_id in duplicate_external_ids
            else diagnostic
            for diagnostic in diagnostics
        ]
        report = _report(
            roots=roots, strict=strict, diagnostics=diagnostics, effective_hash=None
        )
        raise NodeEnvironmentBuildError(
            f"duplicate external node ids {sorted(duplicate_external_ids)}", report
        )

    if strict and any(item.status == "rejected" for item in diagnostics):
        report = _report(
            roots=roots, strict=strict, diagnostics=diagnostics, effective_hash=None
        )
        raise NodeEnvironmentBuildError("strict node discovery failed", report)

    builtin = NodeCatalog(builtin_path)
    data = builtin.raw_data
    entries = list(data.get("entries", []))
    builtin_ids = {str(entry.get("id")) for entry in entries}
    duplicate_ids = sorted(builtin_ids.intersection(descriptors))
    if duplicate_ids:
        diagnostics.append(
            NodeDiscoveryDiagnostic(
                path=str(builtin_path),
                status="rejected",
                error_code="duplicate_node_id",
                message=f"duplicate built-in/external node ids {duplicate_ids}",
            )
        )
        report = _report(
            roots=roots, strict=strict, diagnostics=diagnostics, effective_hash=None
        )
        raise NodeEnvironmentBuildError("effective catalog has duplicate ids", report)
    entries.extend(
        descriptor.catalog_entry
        for descriptor in sorted(descriptors.values(), key=lambda item: item.node_id)
    )
    data["entries"] = entries

    try:
        catalog = NodeCatalog.from_data(data)
        registry = build_node_registry(
            catalog,
            descriptors,
            external_factories=external_factories,
        )
    except (CatalogValidationError, NodeRegistryError) as exc:
        diagnostics.append(
            NodeDiscoveryDiagnostic(
                path="<effective-catalog>",
                status="rejected",
                error_code="effective_catalog_invalid",
                message=str(exc),
            )
        )
        report = _report(
            roots=roots, strict=strict, diagnostics=diagnostics, effective_hash=None
        )
        raise NodeEnvironmentBuildError(str(exc), report) from exc

    builtin_entries = {
        str(entry["id"]): entry
        for entry in builtin.raw_data.get("entries", [])
    }
    sources = [
        NodeSource(
            node_id=node_id,
            kind="builtin",
            path=str(builtin_path),
            content_hash=(
                "sha256:"
                + hashlib.sha256(
                    json.dumps(
                        entry,
                        sort_keys=True,
                        separators=(",", ":"),
                    ).encode()
                ).hexdigest()
            ),
            version=builtin.version,
        )
        for node_id, entry in sorted(builtin_entries.items())
    ]
    sources.extend(
        NodeSource(
            node_id=descriptor.node_id,
            kind="external_python",
            path=str(descriptor.folder),
            content_hash=descriptor.content_hash,
            version=descriptor.version,
        )
        for descriptor in sorted(descriptors.values(), key=lambda item: item.node_id)
    )
    hash_payload = json.dumps(
        {
            "catalog": catalog.raw_data,
            "sources": [source.__dict__ for source in sources],
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    effective_hash = f"sha256:{hashlib.sha256(hash_payload).hexdigest()}"
    report = _report(
        roots=roots,
        strict=strict,
        diagnostics=diagnostics,
        effective_hash=effective_hash,
    )
    return NodeEnvironment(
        catalog=catalog,
        registry=registry,
        effective_catalog_hash=effective_hash,
        sources=tuple(sources),
        descriptors=MappingProxyType(dict(descriptors)),
        report=report,
        generation=generation,
    )
