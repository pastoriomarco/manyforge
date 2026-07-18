"""Validated descriptor boundary for trusted external orchestration nodes."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from manyforge_behavior.catalog import (
    CatalogValidationError,
    NodeCatalog,
    SEMVER_RE,
)


EXTERNAL_NODE_API_VERSION = "manyforge.external_node.v0"
_NODE_ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_OPERATOR_STOP_POLICIES = {"hold", "continue", "inherit"}
_DESCRIPTOR_FIELDS = {
    "api_version",
    "id",
    "name",
    "version",
    "category",
    "description",
    "stability",
    "kind",
    "children",
    "profiles",
    "runtime",
    "required_capabilities",
    "runtime_resources",
    "parameters",
    "operator_stop_policy",
    "tags",
    "bb_reader_params",
    "bb_writer_params",
    "swap_class",
    "swap_param_aliases",
    "execution",
    "observability",
    "assistant_visibility_hint",
}
_RUNTIME_FIELDS = {
    "language",
    "file",
    "class",
    "py_trees",
    "constructor",
    "tick_thread_policy",
}
_RUNTIME_RESOURCE_FIELDS = {
    "id",
    "type",
    "access",
    "required",
    "alias_parameter",
}
_RUNTIME_RESOURCE_ACCESS = {"read", "write", "read_write"}


class NodeDescriptorError(ValueError):
    """A node folder is not a valid trusted extension descriptor."""


@dataclass(frozen=True)
class NodeRuntimeBinding:
    file: str
    class_name: str
    py_trees: str
    constructor: str
    tick_thread_policy: str


@dataclass(frozen=True)
class NodeDescriptor:
    node_id: str
    version: str
    folder: Path
    descriptor_path: Path
    source_path: Path
    descriptor_text: str
    source_text: str
    source_bytes: bytes
    runtime: NodeRuntimeBinding
    catalog_entry: dict[str, Any]
    content_hash: str


def _required_string(
    data: dict[str, Any],
    key: str,
    *,
    field: str | None = None,
) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise NodeDescriptorError(
            f"{field or key} must be a non-empty string"
        )
    return value.strip()


def _contained_file(folder: Path, relative: str, *, field: str) -> Path:
    candidate = folder / relative
    if Path(relative).is_absolute():
        raise NodeDescriptorError(f"{field} must be relative to the node folder")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(folder)
    except (FileNotFoundError, ValueError) as exc:
        raise NodeDescriptorError(
            f"{field} must resolve to a file inside the node folder"
        ) from exc
    if not resolved.is_file():
        raise NodeDescriptorError(f"{field} must identify a regular file")
    return resolved


def _validate_string_list(value: Any, *, field: str) -> None:
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item.strip() for item in value
    ):
        raise NodeDescriptorError(f"{field} must be a list of non-empty strings")
    if len(value) != len(set(value)):
        raise NodeDescriptorError(f"{field} must not contain duplicates")


def _validate_optional_metadata(
    raw: dict[str, Any],
    *,
    parameter_names: set[str],
) -> None:
    if "tags" in raw:
        _validate_string_list(raw["tags"], field="tags")

    execution = raw.get("execution")
    if execution is not None:
        if not isinstance(execution, dict):
            raise NodeDescriptorError("execution must be a mapping")
        unknown = sorted(set(execution) - {"blocking", "cancellable", "timeout_param"})
        if unknown:
            raise NodeDescriptorError(f"execution contains unknown fields: {unknown}")
        for field in ("blocking", "cancellable"):
            if not isinstance(execution.get(field), bool):
                raise NodeDescriptorError(f"execution.{field} must be a boolean")
        timeout_param = execution.get("timeout_param")
        if timeout_param is not None:
            if not isinstance(timeout_param, str) or not timeout_param.strip():
                raise NodeDescriptorError(
                    "execution.timeout_param must be a non-empty string"
                )
            if timeout_param not in parameter_names:
                raise NodeDescriptorError(
                    "execution.timeout_param references unknown parameter "
                    f"'{timeout_param}'"
                )

    observability = raw.get("observability")
    if observability is not None:
        if not isinstance(observability, dict):
            raise NodeDescriptorError("observability must be a mapping")
        unknown = sorted(
            set(observability) - {"emits_events", "consumes_events"}
        )
        if unknown:
            raise NodeDescriptorError(
                f"observability contains unknown fields: {unknown}"
            )
        for field in ("emits_events", "consumes_events"):
            if field in observability:
                _validate_string_list(
                    observability[field], field=f"observability.{field}"
                )

    hint = raw.get("assistant_visibility_hint")
    if hint is not None:
        if not isinstance(hint, dict):
            raise NodeDescriptorError("assistant_visibility_hint must be a mapping")
        unknown = sorted(set(hint) - {"recommended_modes", "rationale"})
        if unknown:
            raise NodeDescriptorError(
                f"assistant_visibility_hint contains unknown fields: {unknown}"
            )
        if "recommended_modes" in hint:
            _validate_string_list(
                hint["recommended_modes"],
                field="assistant_visibility_hint.recommended_modes",
            )
        if "rationale" in hint and (
            not isinstance(hint["rationale"], str)
            or not hint["rationale"].strip()
        ):
            raise NodeDescriptorError(
                "assistant_visibility_hint.rationale must be a non-empty string"
            )


def load_node_descriptor(folder: str | Path) -> NodeDescriptor:
    """Parse one node folder without importing its executable source."""
    root = Path(folder).resolve(strict=True)
    if not root.is_dir():
        raise NodeDescriptorError("node folder must be a directory")
    descriptor_path = _contained_file(root, "node.yaml", field="node.yaml")
    try:
        descriptor_text = descriptor_path.read_text(encoding="utf-8")
        raw = yaml.safe_load(descriptor_text)
    except (OSError, yaml.YAMLError) as exc:
        raise NodeDescriptorError(f"cannot parse node.yaml: {exc}") from exc
    if not isinstance(raw, dict):
        raise NodeDescriptorError("node.yaml must be a mapping")
    unknown = sorted(set(raw) - _DESCRIPTOR_FIELDS)
    if unknown:
        raise NodeDescriptorError(f"node.yaml contains unknown fields: {unknown}")
    if raw.get("api_version") != EXTERNAL_NODE_API_VERSION:
        raise NodeDescriptorError(
            f"api_version must be {EXTERNAL_NODE_API_VERSION}"
        )

    node_id = _required_string(raw, "id")
    if not _NODE_ID_RE.fullmatch(node_id):
        raise NodeDescriptorError("id must match ^[a-z][a-z0-9_]*$")
    if root.name != node_id:
        raise NodeDescriptorError(
            f"folder name '{root.name}' must match descriptor id '{node_id}'"
        )
    version = _required_string(raw, "version")
    if not SEMVER_RE.fullmatch(version):
        raise NodeDescriptorError("version must match X.Y.Z")
    for required in ("name", "category", "description"):
        _required_string(raw, required)
    if raw.get("kind") != "leaf":
        raise NodeDescriptorError("external node v0 supports kind: leaf only")
    children = raw.get("children")
    if children != {"min": 0, "max": 0}:
        raise NodeDescriptorError("external leaf children must be {min: 0, max: 0}")
    if "operator_stop_policy" not in raw:
        raise NodeDescriptorError("operator_stop_policy is required")
    operator_stop_policy = raw["operator_stop_policy"]
    if operator_stop_policy not in _OPERATOR_STOP_POLICIES:
        raise NodeDescriptorError(
            "operator_stop_policy must be hold, continue, or inherit"
        )

    parameters = raw.get("parameters")
    if not isinstance(parameters, list):
        raise NodeDescriptorError("parameters must be a list")
    parameter_names = {
        item.get("name")
        for item in parameters
        if isinstance(item, dict) and isinstance(item.get("name"), str)
    }
    _validate_optional_metadata(raw, parameter_names=parameter_names)
    if "required_capabilities" not in raw:
        raise NodeDescriptorError("required_capabilities is required")
    _validate_string_list(
        raw["required_capabilities"], field="required_capabilities"
    )
    runtime_resources = raw.get("runtime_resources", [])
    if not isinstance(runtime_resources, list):
        raise NodeDescriptorError("runtime_resources must be a list")
    resource_ids: set[str] = set()
    for index, requirement in enumerate(runtime_resources):
        path = f"runtime_resources[{index}]"
        if not isinstance(requirement, dict):
            raise NodeDescriptorError(f"{path} must be a mapping")
        missing_resource_fields = sorted(
            _RUNTIME_RESOURCE_FIELDS - set(requirement)
        )
        if missing_resource_fields:
            raise NodeDescriptorError(
                f"{path} is missing required fields: {missing_resource_fields}"
            )
        unknown_resource_fields = sorted(
            set(requirement) - _RUNTIME_RESOURCE_FIELDS
        )
        if unknown_resource_fields:
            raise NodeDescriptorError(
                f"{path} contains unknown fields: {unknown_resource_fields}"
            )
        resource_id = requirement.get("id")
        resource_type = requirement.get("type")
        alias_parameter = requirement.get("alias_parameter")
        if not isinstance(resource_id, str) or not resource_id.strip():
            raise NodeDescriptorError(f"{path}.id must be a non-empty string")
        if resource_id in resource_ids:
            raise NodeDescriptorError(
                f"runtime_resources contains duplicate id '{resource_id}'"
            )
        resource_ids.add(resource_id)
        if not isinstance(resource_type, str) or not resource_type.strip():
            raise NodeDescriptorError(f"{path}.type must be a non-empty string")
        if requirement.get("access", "read") not in _RUNTIME_RESOURCE_ACCESS:
            raise NodeDescriptorError(
                f"{path}.access must be read, write, or read_write"
            )
        if not isinstance(requirement.get("required", True), bool):
            raise NodeDescriptorError(f"{path}.required must be a boolean")
        if not isinstance(alias_parameter, str) or not alias_parameter.strip():
            raise NodeDescriptorError(
                f"{path}.alias_parameter must name the node parameter that "
                "selects the deployment-owned alias"
            )
        if alias_parameter not in parameter_names:
            raise NodeDescriptorError(
                f"{path}.alias_parameter references unknown parameter "
                f"'{alias_parameter}'"
            )

    runtime = raw.get("runtime")
    if not isinstance(runtime, dict):
        raise NodeDescriptorError("runtime must be a mapping")
    unknown_runtime = sorted(set(runtime) - _RUNTIME_FIELDS)
    if unknown_runtime:
        raise NodeDescriptorError(
            f"runtime contains unknown fields: {unknown_runtime}"
        )
    if runtime.get("language") != "python":
        raise NodeDescriptorError("runtime.language must be python")
    runtime_file = _required_string(runtime, "file", field="runtime.file")
    if runtime_file != "node.py":
        raise NodeDescriptorError("runtime.file must be node.py in external_node.v0")
    source_path = _contained_file(root, runtime_file, field="runtime.file")
    try:
        source_bytes = source_path.read_bytes()
        source_text = source_bytes.decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise NodeDescriptorError(
            "runtime.file must be readable UTF-8 Python source"
        ) from exc
    class_name = _required_string(runtime, "class", field="runtime.class")
    if not class_name.isidentifier():
        raise NodeDescriptorError("runtime.class must be a Python identifier")
    py_trees_constraint = _required_string(
        runtime,
        "py_trees",
        field="runtime.py_trees",
    )
    if py_trees_constraint != "==2.4.0":
        raise NodeDescriptorError("runtime.py_trees must be exactly ==2.4.0")
    constructor = str(runtime.get("constructor", "name_kwargs"))
    if constructor != "name_kwargs":
        raise NodeDescriptorError("runtime.constructor must be name_kwargs")
    tick_policy = str(runtime.get("tick_thread_policy", "nonblocking"))
    if tick_policy != "nonblocking":
        raise NodeDescriptorError(
            "runtime.tick_thread_policy must be nonblocking"
        )

    catalog_entry = {
        key: value
        for key, value in raw.items()
        if key
        not in {
            "api_version",
            "version",
            "runtime",
            "assistant_visibility_hint",
        }
    }
    catalog_entry["runners"] = {
        "py_trees": {
            "implementation_kind": "external_python",
            "class": class_name,
        }
    }
    try:
        descriptor_catalog = NodeCatalog.from_data(
            {
                "api_version": "manyforge.node_catalog.v1",
                "catalog_version": "0.0.0",
                "schema_types": {},
                "compound_skills": [],
                "entries": [catalog_entry],
            }
        )
    except (CatalogValidationError, KeyError, TypeError) as exc:
        raise NodeDescriptorError(f"catalog metadata is invalid: {exc}") from exc

    descriptor_entry = descriptor_catalog.get(node_id)
    declared_parameters = {
        parameter.name: parameter for parameter in descriptor_entry.parameters
    }
    for index, requirement in enumerate(runtime_resources):
        alias_parameter = requirement["alias_parameter"]
        parameter = declared_parameters[alias_parameter]
        if parameter.schema is None or (
            descriptor_catalog.schema_registry.resolve(parameter.schema).schema_type
            != "string"
        ):
            raise NodeDescriptorError(
                f"runtime_resources[{index}].alias_parameter must reference a "
                "string-schema parameter"
            )
    canonical = json.dumps(raw, sort_keys=True, separators=(",", ":")).encode()
    digest = hashlib.sha256(canonical + b"\0" + source_bytes).hexdigest()
    return NodeDescriptor(
        node_id=node_id,
        version=version,
        folder=root,
        descriptor_path=descriptor_path,
        source_path=source_path,
        descriptor_text=descriptor_text,
        source_text=source_text,
        source_bytes=source_bytes,
        runtime=NodeRuntimeBinding(
            file=runtime_file,
            class_name=class_name,
            py_trees=py_trees_constraint,
            constructor=constructor,
            tick_thread_policy=tick_policy,
        ),
        catalog_entry=catalog_entry,
        content_hash=f"sha256:{digest}",
    )
