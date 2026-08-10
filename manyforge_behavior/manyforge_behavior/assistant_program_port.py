"""Bounded Program model/selection owner over retained assistant contracts.

This port is deliberately distinct from interactive Composer assistants.  It
binds one exact Applied Program, retained assistant projection, and scene-owner
lineage before a tree is constructed.  Model transports resolve concrete site
configuration outside this module and receive only the compiler-qualified
endpoint/template request.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from copy import deepcopy
from dataclasses import dataclass
from hashlib import sha256
import json
import math
from threading import Event, RLock
import time
from types import MappingProxyType
from typing import Any, Final, Protocol

from manyforge_behavior.nodes.model import (
    ModelOutputValidationError,
    _parse_json_text,
    _render_prompt_template,
    _resolve_output_schema_template,
    _resolve_response_tool_template,
    _validate_model_output,
)
from manyforge_behavior.nodes.selection import (
    SceneSelectionError,
    build_scene_selection_context,
    validate_scene_selection,
)
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)


_RETAINED_ASSISTANT_API: Final = "manyforge.retained_assistant.v1"
_SCENE_SOURCE_API: Final = "manyforge.logical_scene_source.v1"
_SCENE_POSE_QUERY_API: Final = "manyforge.scene_resource_pose_query.v1"
_SCENE_POSE_RESPONSE_API: Final = "manyforge.scene_resource_pose_response.v1"
_MAX_CONTEXTS: Final = 4_096
_MAX_TASKS: Final = 4_096
_MAX_WORKERS: Final = 8
_MAX_TIMEOUT_S: Final = 3_600.0
_MAX_MODEL_TEXT_BYTES: Final = 4 * 1024 * 1024
_MAX_MODEL_VERSION_BYTES: Final = 4_096
_MAX_SCENE_POSE_QUERY_RESOURCES: Final = 128
_PARSE_RETRY_SUFFIX: Final = (
    "\n\nThe previous response did not satisfy the required structured output "
    "contract. Return ONLY valid structured output for this request. Do not "
    "include markdown fences, prose, or any extra text."
)
_DIGEST_PREFIX: Final = "sha256:"


def _thaw(value: Any) -> Any:
    """Copy one recursively frozen JSON value into ordinary containers."""

    if isinstance(value, Mapping):
        return {key: _thaw(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_thaw(nested) for nested in value]
    return deepcopy(value)


def _bounded_json_copy(value: Any, *, field: str) -> Any:
    """Copy transport-owned JSON after enforcing one fixed byte bound."""

    try:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (RecursionError, TypeError, ValueError) as exc:
        raise ValueError(f"{field} must be finite acyclic JSON data") from exc
    if len(encoded) > _MAX_MODEL_TEXT_BYTES:
        raise ValueError(f"{field} exceeds its fixed byte bound")
    return json.loads(encoded)


class ProgramAssistantPortError(RuntimeError):
    """An assistant contract, scene projection, or task request is invalid."""


class ProgramModelTransportError(RuntimeError):
    """A bounded model transport failed before returning a valid response."""


@dataclass(frozen=True, slots=True)
class ProgramModelRequest:
    """One exact compiler-qualified request passed to a site-owned transport."""

    model_endpoint_alias: str
    external_resource_alias: str
    model_id: str
    prompt_template_id: str
    prompt_template_version: str
    prompt_template_digest: str
    prompt: str
    timeout_s: float
    max_output_tokens: int
    response_mode: str
    tool_call_family: str
    response_tool: Mapping[str, Any] | None
    attachment: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        for field_name in (
            "model_endpoint_alias",
            "external_resource_alias",
            "model_id",
            "prompt_template_id",
            "prompt_template_version",
        ):
            value = getattr(self, field_name)
            if not isinstance(value, str) or not value:
                raise ValueError(f"{field_name} must be non-empty text")
        _digest(self.prompt_template_digest, field="prompt_template_digest")
        if (
            not isinstance(self.prompt, str)
            or not self.prompt
            or len(self.prompt.encode("utf-8")) > _MAX_MODEL_TEXT_BYTES
        ):
            raise ValueError("model prompt is empty or oversized")
        if (
            isinstance(self.timeout_s, bool)
            or not isinstance(self.timeout_s, (int, float))
            or not math.isfinite(float(self.timeout_s))
            or not 0.0 < float(self.timeout_s) <= _MAX_TIMEOUT_S
        ):
            raise ValueError("model timeout is outside its fixed bound")
        if (
            type(self.max_output_tokens) is not int
            or not 1 <= self.max_output_tokens <= 1_048_576
        ):
            raise ValueError("model output-token bound is invalid")
        if self.response_mode not in {"json_object", "tool_call"}:
            raise ValueError("model response mode is invalid")
        if self.tool_call_family not in {
            "openai_chat_tools",
            "qwen3_coder",
            "qwen3_xml",
        }:
            raise ValueError("model tool-call family is invalid")
        if self.response_mode == "tool_call" and not isinstance(
            self.response_tool, Mapping
        ):
            raise ValueError("tool-call response requires one exact tool")
        if self.response_mode == "json_object" and self.response_tool is not None:
            raise ValueError("JSON-object response cannot carry a tool")
        object.__setattr__(
            self,
            "response_tool",
            (
                None
                if self.response_tool is None
                else MappingProxyType(_thaw(self.response_tool))
            ),
        )
        object.__setattr__(
            self,
            "attachment",
            (
                None
                if self.attachment is None
                else MappingProxyType(_thaw(self.attachment))
            ),
        )


@dataclass(frozen=True, slots=True)
class ProgramModelResponse:
    """Transport-neutral response before Program schema validation."""

    text: str | None = None
    tool_name: str | None = None
    tool_arguments: str | Mapping[str, Any] | None = None
    model_version: str = ""
    usage: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        if self.text is not None and (
            not isinstance(self.text, str)
            or len(self.text.encode("utf-8")) > _MAX_MODEL_TEXT_BYTES
        ):
            raise ValueError("model response text is invalid or oversized")
        if self.tool_name is not None and (
            not isinstance(self.tool_name, str) or not self.tool_name
        ):
            raise ValueError("model tool name must be absent or non-empty text")
        if self.tool_arguments is not None and not isinstance(
            self.tool_arguments, (str, Mapping)
        ):
            raise TypeError("model tool arguments must be JSON text or a mapping")
        if isinstance(self.tool_arguments, str) and len(
            self.tool_arguments.encode("utf-8")
        ) > _MAX_MODEL_TEXT_BYTES:
            raise ValueError("model tool arguments exceed their fixed byte bound")
        if (
            not isinstance(self.model_version, str)
            or len(self.model_version.encode("utf-8")) > _MAX_MODEL_VERSION_BYTES
        ):
            raise ValueError("model_version must be bounded text")
        if self.usage is not None and not isinstance(self.usage, Mapping):
            raise TypeError("model usage must be absent or a mapping")
        object.__setattr__(
            self,
            "tool_arguments",
            (
                _bounded_json_copy(
                    self.tool_arguments,
                    field="model tool arguments",
                )
                if isinstance(self.tool_arguments, Mapping)
                else self.tool_arguments
            ),
        )
        object.__setattr__(
            self,
            "usage",
            (
                None
                if self.usage is None
                else MappingProxyType(
                    _bounded_json_copy(self.usage, field="model usage")
                )
            ),
        )


class ProgramModelTransport(Protocol):
    """Site-owned transport that resolves config without runtime discovery."""

    def validate_endpoint(
        self,
        *,
        model_endpoint_alias: str,
        external_resource_alias: str,
        model_id: str,
        request_contract: Mapping[str, Any],
    ) -> None: ...

    def complete(
        self,
        request: ProgramModelRequest,
        cancel_event: Event,
    ) -> ProgramModelResponse: ...


class _SceneTransport(Protocol):
    def scene(self) -> dict[str, Any]: ...


@dataclass(frozen=True, slots=True)
class _AssistantContract:
    projection_digest: str
    audit_policy: Mapping[str, bool]
    endpoints: Mapping[str, Mapping[str, Any]]
    templates: Mapping[str, Mapping[str, Any]]


@dataclass(frozen=True, slots=True)
class _BoundContext:
    capture: ProgramRunPortContext
    assistant: _AssistantContract | None
    blackboard: Mapping[str, Mapping[str, Any]]


@dataclass(frozen=True, slots=True)
class _TaskResult:
    output: Any
    metadata: Mapping[str, Any]
    completed_at_s: float


@dataclass(slots=True)
class _Task:
    identity: NodeAttemptIdentity
    node_id: str
    inputs: OwnerNodeInputs
    cancel_event: Event
    deadline_s: float | None = None
    future: Future[_TaskResult] | None = None
    terminal: OwnerTaskSnapshot | None = None


class _TaskFailure(RuntimeError):
    def __init__(self, reason_code: str, message: str) -> None:
        super().__init__(message)
        self.reason_code = reason_code


def _digest(value: object, *, field: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 71
        or not value.startswith(_DIGEST_PREFIX)
        or any(character not in "0123456789abcdef" for character in value[7:])
    ):
        raise ProgramAssistantPortError(f"{field} must be an exact SHA-256 identity")
    return value


def _mapping(value: object, *, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ProgramAssistantPortError(f"{field} must be an object")
    return value


def _sequence(value: object, *, field: str) -> Sequence[Any]:
    if isinstance(value, (str, bytes)) or not isinstance(value, Sequence):
        raise ProgramAssistantPortError(f"{field} must be an array")
    return value


def _text(value: object, *, field: str, maximum_bytes: int = 512) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        raise ProgramAssistantPortError(f"{field} must be bounded non-empty text")
    return value


def _exact(value: Mapping[str, Any], fields: set[str], *, path: str) -> None:
    if set(value) != fields:
        raise ProgramAssistantPortError(f"{path} has wrong fields")


def _parse_assistant(value: Mapping[str, Any]) -> _AssistantContract:
    root = dict(value)
    _exact(
        root,
        {
            "api_version",
            "projection_digest",
            "audit_policy",
            "model_endpoint_aliases",
            "provider_aliases",
            "modes",
            "prompt_templates",
            "instruments",
        },
        path="assistant_projection",
    )
    if root["api_version"] != _RETAINED_ASSISTANT_API:
        raise ProgramAssistantPortError("retained assistant version is unsupported")
    projection_digest = _digest(
        root["projection_digest"], field="assistant_projection.projection_digest"
    )
    audit = dict(_mapping(root["audit_policy"], field="assistant audit_policy"))
    _exact(
        audit,
        {"redact_argument_values", "store_full_prompts", "store_full_responses"},
        path="assistant audit_policy",
    )
    if any(type(value) is not bool for value in audit.values()):
        raise ProgramAssistantPortError("assistant audit policy must contain booleans")

    endpoints: dict[str, Mapping[str, Any]] = {}
    previous = ""
    for index, raw in enumerate(
        _sequence(root["model_endpoint_aliases"], field="model endpoints")
    ):
        row = dict(_mapping(raw, field=f"model endpoints[{index}]"))
        _exact(
            row,
            {
                "model_endpoint_alias",
                "external_resource_alias",
                "model_id",
                "request_contract",
            },
            path=f"model endpoints[{index}]",
        )
        alias = _text(row["model_endpoint_alias"], field="model_endpoint_alias")
        if alias <= previous:
            raise ProgramAssistantPortError("model endpoints are not canonical")
        previous = alias
        _text(row["external_resource_alias"], field="external_resource_alias")
        _text(row["model_id"], field="model_id", maximum_bytes=4_096)
        contract = dict(_mapping(row["request_contract"], field="request_contract"))
        _exact(
            contract,
            {
                "default_timeout_s",
                "max_output_tokens",
                "supported_image_transports",
                "supports_image_input",
                "supports_json_object",
                "supports_response_tools",
                "tool_call_family",
            },
            path=f"model endpoints[{index}].request_contract",
        )
        timeout = contract["default_timeout_s"]
        tokens = contract["max_output_tokens"]
        if (
            isinstance(timeout, bool)
            or not isinstance(timeout, (int, float))
            or not math.isfinite(float(timeout))
            or not 0.0 < float(timeout) <= _MAX_TIMEOUT_S
            or type(tokens) is not int
            or not 1 <= tokens <= 1_048_576
            or any(
                type(contract[name]) is not bool
                for name in (
                    "supports_image_input",
                    "supports_json_object",
                    "supports_response_tools",
                )
            )
        ):
            raise ProgramAssistantPortError("model request contract is invalid")
        transports = list(
            _sequence(
                contract["supported_image_transports"],
                field="supported_image_transports",
            )
        )
        if (
            transports != sorted(set(transports))
            or any(value not in {"http_url", "inline_data_url"} for value in transports)
            or bool(transports) != contract["supports_image_input"]
            or contract["tool_call_family"]
            not in {"openai_chat_tools", "qwen3_coder", "qwen3_xml"}
        ):
            raise ProgramAssistantPortError("model image/tool contract is invalid")
        endpoints[alias] = MappingProxyType(_thaw(row))

    templates: dict[str, Mapping[str, Any]] = {}
    previous = ""
    for index, raw in enumerate(
        _sequence(root["prompt_templates"], field="prompt templates")
    ):
        row = dict(_mapping(raw, field=f"prompt templates[{index}]"))
        _exact(
            row,
            {
                "content_digest",
                "prompt_template_id",
                "response_contract",
                "template_text",
                "variable_ids",
                "version",
            },
            path=f"prompt templates[{index}]",
        )
        template_id = _text(row["prompt_template_id"], field="prompt_template_id")
        if template_id <= previous:
            raise ProgramAssistantPortError("prompt templates are not canonical")
        previous = template_id
        _digest(row["content_digest"], field="prompt template content_digest")
        _text(row["version"], field="prompt template version")
        _text(
            row["template_text"],
            field="prompt template text",
            maximum_bytes=16 * 1024,
        )
        variable_ids = list(_sequence(row["variable_ids"], field="variable_ids"))
        if (
            variable_ids != sorted(set(variable_ids))
            or any(not isinstance(value, str) or not value for value in variable_ids)
        ):
            raise ProgramAssistantPortError(
                "prompt template variables are not canonical"
            )
        response = dict(
            _mapping(row["response_contract"], field="prompt response_contract")
        )
        mode = response.get("response_mode")
        if mode == "json_object":
            _exact(response, {"response_mode"}, path="prompt response_contract")
        elif mode == "tool_call":
            _exact(
                response,
                {"response_mode", "tool_name", "tool_description"},
                path="prompt response_contract",
            )
            _text(response["tool_name"], field="response tool_name")
            _text(
                response["tool_description"],
                field="response tool_description",
                maximum_bytes=16 * 1024,
            )
        else:
            raise ProgramAssistantPortError("prompt response mode is invalid")
        templates[template_id] = MappingProxyType(_thaw(row))
    if not endpoints or not templates:
        raise ProgramAssistantPortError(
            "Program assistant projection has no endpoint/template pair"
        )
    return _AssistantContract(
        projection_digest,
        MappingProxyType(audit),
        MappingProxyType(endpoints),
        MappingProxyType(templates),
    )


def _blackboard_declarations(
    program: Mapping[str, Any],
) -> Mapping[str, Mapping[str, Any]]:
    raw_blackboard = program.get("blackboard")
    if raw_blackboard is None:
        return MappingProxyType({})
    blackboard = _mapping(raw_blackboard, field="Program blackboard")
    result: dict[str, Mapping[str, Any]] = {}
    previous = ""
    for index, raw in enumerate(
        _sequence(blackboard.get("keys"), field="blackboard keys")
    ):
        row = dict(_mapping(raw, field=f"blackboard keys[{index}]"))
        key = _text(row.get("key"), field="blackboard key")
        if key <= previous:
            raise ProgramAssistantPortError("Program blackboard keys are not canonical")
        previous = key
        result[key] = MappingProxyType(_thaw(row))
    return MappingProxyType(result)


def _walk_tree(root: Mapping[str, Any]) -> tuple[Mapping[str, Any], ...]:
    rows: list[Mapping[str, Any]] = []
    stack: list[Mapping[str, Any]] = [root]
    while stack:
        row = stack.pop()
        rows.append(row)
        children = _sequence(row.get("children"), field="Program tree children")
        for child in reversed(children):
            stack.append(_mapping(child, field="Program tree child"))
    return tuple(rows)


def _assistant_node_ids(context: ProgramRunPortContext) -> frozenset[str]:
    tree = _mapping(context.program.get("tree"), field="Program tree")
    result: set[str] = set()
    for node in _walk_tree(tree):
        node_id = node.get("node_id")
        if isinstance(node_id, str) and node_id.startswith("assistant."):
            result.add(node_id)
    return frozenset(result)


def _validate_query_nodes(
    context: ProgramRunPortContext,
    assistant: _AssistantContract,
    blackboard: Mapping[str, Mapping[str, Any]],
    model_transport: ProgramModelTransport,
) -> None:
    tree = _mapping(context.program.get("tree"), field="Program tree")
    validated_endpoints: set[str] = set()
    for node in _walk_tree(tree):
        if node.get("node_id") != "assistant.query_model":
            continue
        parameters = _mapping(node.get("parameters"), field="query_model parameters")
        endpoint_alias = _text(
            parameters.get("model_endpoint_alias"), field="model_endpoint_alias"
        )
        template_id = _text(
            parameters.get("prompt_template_id"), field="prompt_template_id"
        )
        try:
            endpoint = assistant.endpoints[endpoint_alias]
            template = assistant.templates[template_id]
        except KeyError as exc:
            raise ProgramAssistantPortError(
                "query_model references an unretained endpoint or template"
            ) from exc
        variable_ids = [
            _text(
                _mapping(raw, field="template variable").get("variable_id"),
                field="template variable_id",
            )
            for raw in _sequence(
                parameters.get("template_variables", ()),
                field="template_variables",
            )
        ]
        if variable_ids != list(template["variable_ids"]):
            raise ProgramAssistantPortError(
                "query_model variables differ from the retained template"
            )
        output_key = _text(parameters.get("output_key"), field="output_key")
        declaration = blackboard.get(output_key)
        if declaration is None or declaration.get("type") != "model_response":
            raise ProgramAssistantPortError(
                "query_model output key is not one declared model_response"
            )
        fields = declaration.get("fields")
        if not isinstance(fields, (list, tuple)) or not fields:
            raise ProgramAssistantPortError(
                "query_model output key has no closed response fields"
            )
        schema_template = _json_schema(declaration, {})
        confidence_schema = schema_template["properties"].get("confidence")
        minimum_confidence = parameters.get("min_confidence", 0.0)
        if (
            minimum_confidence != 0.0
            and (
                not isinstance(confidence_schema, Mapping)
                or confidence_schema.get("type") not in {"integer", "number"}
            )
        ):
            raise ProgramAssistantPortError(
                "query_model confidence threshold has no numeric output field"
            )
        response_mode = template["response_contract"]["response_mode"]
        request_contract = endpoint["request_contract"]
        if (
            response_mode == "json_object"
            and request_contract["supports_json_object"] is not True
        ) or (
            response_mode == "tool_call"
            and request_contract["supports_response_tools"] is not True
        ):
            raise ProgramAssistantPortError(
                "query_model response mode is unsupported by its endpoint"
            )
        attachment = parameters.get("scene_attachment", "none")
        if attachment == "snapshot_image":
            transport = parameters.get(
                "scene_snapshot_transport", "inline_data_url"
            )
            if (
                request_contract["supports_image_input"] is not True
                or transport
                not in request_contract["supported_image_transports"]
            ):
                raise ProgramAssistantPortError(
                    "query_model image transport is unsupported by its endpoint"
                )
        elif attachment not in {"none", "state_json"}:
            raise ProgramAssistantPortError("query_model scene attachment is invalid")
        if endpoint_alias not in validated_endpoints:
            try:
                model_transport.validate_endpoint(
                    model_endpoint_alias=endpoint_alias,
                    external_resource_alias=endpoint["external_resource_alias"],
                    model_id=endpoint["model_id"],
                    request_contract=request_contract,
                )
            except Exception as exc:
                raise ProgramAssistantPortError(
                    f"model endpoint {endpoint_alias!r} is not available to "
                    "the selected site transport"
                ) from exc
            validated_endpoints.add(endpoint_alias)


def _json_schema(
    declaration: Mapping[str, Any],
    template_variables: Mapping[str, Any],
) -> dict[str, Any]:
    fields = _sequence(declaration.get("fields"), field="output declaration fields")
    properties: dict[str, Any] = {}
    required: list[str] = []
    for index, raw in enumerate(fields):
        field = _mapping(raw, field=f"output declaration fields[{index}]")
        name = _text(field.get("field"), field="output field")
        declared_type = field.get("type")
        if declared_type == "bool":
            schema: dict[str, Any] = {"type": "boolean"}
        elif declared_type == "float64":
            schema = {"type": "number"}
        elif declared_type == "int64":
            schema = {"type": "integer"}
        elif declared_type in {"scene_resource", "string"}:
            schema = {"type": "string"}
        elif declared_type == "string_list":
            schema = {
                "type": "array",
                "items": {"type": "string"},
                "maxItems": 4_096,
            }
        else:
            raise ProgramAssistantPortError(
                f"model response field {name!r} has unsupported type {declared_type!r}"
            )
        if (
            declared_type == "scene_resource"
            and name == "selected_object_id"
            and isinstance(template_variables.get("candidate_object_ids"), list)
        ):
            schema["x-manyforge-enum-template-variable"] = "candidate_object_ids"
            schema["x-manyforge-enum-allow-empty-string"] = True
        properties[name] = schema
        required.append(name)
    return {
        "type": "object",
        "properties": properties,
        "required": required,
        "additionalProperties": False,
    }


def _stringify_template_value(value: Any) -> str:
    if isinstance(value, str):
        return value
    try:
        return json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    except (TypeError, ValueError) as exc:
        raise ProgramAssistantPortError(
            "template variable is not bounded JSON data"
        ) from exc


def _json_bytes(value: Any, *, field: str) -> bytes:
    try:
        return json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise ProgramAssistantPortError(f"{field} is not bounded JSON data") from exc


def _sha256_json(value: Any, *, field: str) -> str:
    return _DIGEST_PREFIX + sha256(_json_bytes(value, field=field)).hexdigest()


def _raw_response_bytes(value: Any) -> bytes:
    if isinstance(value, str):
        result = value.encode("utf-8")
    else:
        result = _json_bytes(value, field="raw model response")
    if len(result) > _MAX_MODEL_TEXT_BYTES:
        raise ProgramAssistantPortError("raw model response exceeds its fixed bound")
    return result


class HeadlessProgramSceneSource:
    """Read-only adapter over fresh logical-scene owner projection.

    Fixed-object poses are already cell-frame owner truth.  A carried object
    requires the optional native ``resolve_scene_resources`` transport method;
    this adapter never composes a carrier transform in Python.
    """

    def __init__(self, transport: _SceneTransport) -> None:
        if not callable(getattr(transport, "scene", None)):
            raise TypeError("scene transport has no callable scene")
        self._transport = transport

    def _snapshot(
        self, context: ProgramRunPortContext
    ) -> tuple[str, Mapping[str, Any], Mapping[str, Any]]:
        projection = self._transport.scene()
        if (
            not isinstance(projection, dict)
            or projection.get("schema_version")
            != "manyforge.scene_projection.v1"
        ):
            raise ProgramAssistantPortError(
                "headless scene read returned an unsupported projection"
            )
        logical = _mapping(projection.get("logical_scene"), field="logical_scene")
        if (
            logical.get("available") is not True
            or logical.get("freshness") != "fresh"
            or list(_sequence(logical.get("known_gaps"), field="known_gaps"))
        ):
            raise ProgramAssistantPortError("logical-scene owner truth is not fresh")
        payload = _mapping(logical.get("payload"), field="logical_scene.payload")
        if payload.get("api_version") != _SCENE_SOURCE_API:
            raise ProgramAssistantPortError(
                "logical-scene payload version is unsupported"
            )
        access = _mapping(payload.get("access"), field="logical_scene.access")
        active = _mapping(access.get("active_projection"), field="active_projection")
        if (
            active.get("retained_bundle_id") != context.retained_bundle_id
            or active.get("graph_revision") != context.graph_revision
            or access.get("projection_leases_open") is not True
        ):
            raise ProgramAssistantPortError(
                "logical scene differs from the captured Program deployment"
            )
        snapshot = _mapping(payload.get("snapshot"), field="logical_scene.snapshot")
        if dict(_mapping(snapshot.get("revision"), field="scene revision")) != dict(
            _mapping(active.get("scene_revision"), field="active scene revision")
        ):
            raise ProgramAssistantPortError(
                "logical scene snapshot and active projection disagree"
            )
        cell_id = _text(snapshot.get("cell_id"), field="cell_id")
        return cell_id, active, snapshot

    def state_json(self, context: ProgramRunPortContext) -> str:
        _cell_id, active, snapshot = self._snapshot(context)
        return json.dumps(
            {
                "projection_revision": _thaw(active),
                "snapshot": _thaw(snapshot),
            },
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )

    def _resolve_carried_poses(
        self,
        active: Mapping[str, Any],
        carried: Sequence[str],
    ) -> dict[str, Any]:
        resolver = getattr(self._transport, "resolve_scene_resources", None)
        if not callable(resolver):
            raise ProgramAssistantPortError(
                "native carried-object pose projection is unavailable"
            )
        expected_revision = _thaw(active)
        ordered = sorted(carried)
        pose_by_id: dict[str, Any] = {}
        for start in range(0, len(ordered), _MAX_SCENE_POSE_QUERY_RESOURCES):
            batch = ordered[start : start + _MAX_SCENE_POSE_QUERY_RESOURCES]
            query = {
                "api_version": _SCENE_POSE_QUERY_API,
                "expected_projection_revision": expected_revision,
                "scene_resource_ids": batch,
            }
            response = resolver(query)
            if not isinstance(response, Mapping):
                raise ProgramAssistantPortError(
                    "native scene pose projection is not an object"
                )
            _exact(
                response,
                {
                    "api_version",
                    "poses",
                    "projection_revision",
                },
                path="native scene pose projection",
            )
            if (
                response["api_version"] != _SCENE_POSE_RESPONSE_API
                or response["projection_revision"] != expected_revision
            ):
                raise ProgramAssistantPortError(
                    "native scene pose projection changed owner revision"
                )
            rows = _sequence(response.get("poses"), field="resolved scene poses")
            batch_poses: dict[str, Any] = {}
            previous = ""
            for index, raw_pose in enumerate(rows):
                pose = _mapping(raw_pose, field=f"resolved scene poses[{index}]")
                _exact(
                    pose,
                    {"pose_in_cell", "scene_resource_id"},
                    path=f"resolved scene poses[{index}]",
                )
                resource_id = _text(
                    pose.get("scene_resource_id"),
                    field="resolved scene_resource_id",
                )
                if resource_id <= previous:
                    raise ProgramAssistantPortError(
                        "native scene pose projection is not canonical"
                    )
                previous = resource_id
                batch_poses[resource_id] = _thaw(pose.get("pose_in_cell"))
            if set(batch_poses) != set(batch):
                raise ProgramAssistantPortError(
                    "native scene pose projection is incomplete"
                )
            pose_by_id.update(batch_poses)
        return pose_by_id

    def selection_objects(
        self,
        context: ProgramRunPortContext,
        requested_ids: Sequence[str],
    ) -> tuple[str, list[dict[str, Any]]]:
        cell_id, active, snapshot = self._snapshot(context)
        raw_requested = _sequence(requested_ids, field="requested scene resources")
        if len(raw_requested) > 4_096:
            raise ProgramAssistantPortError(
                "requested scene-resource set exceeds its fixed bound"
            )
        requested_rows = [
            _text(value, field=f"requested scene resources[{index}]")
            for index, value in enumerate(raw_requested)
        ]
        if len(requested_rows) != len(set(requested_rows)):
            raise ProgramAssistantPortError(
                "requested scene resources contain duplicates"
            )
        requested = set(requested_rows)
        objects: list[dict[str, Any]] = []
        carried: list[str] = []
        seen_resources: set[str] = set()
        for raw in _sequence(snapshot.get("objects"), field="scene objects"):
            row = dict(_mapping(raw, field="scene object"))
            resource_id = _text(row.get("scene_resource_id"), field="scene_resource_id")
            if resource_id in seen_resources:
                raise ProgramAssistantPortError(
                    "logical-scene projection repeats a scene resource"
                )
            seen_resources.add(resource_id)
            if requested and resource_id not in requested:
                continue
            placement = _mapping(row.get("placement"), field="scene object placement")
            kind = placement.get("kind")
            if kind == "cell_fixed":
                if placement.get("cell_id") != cell_id:
                    raise ProgramAssistantPortError(
                        "scene resource is fixed in another cell"
                    )
                row["pose_in_cell"] = _thaw(placement.get("pose_in_cell"))
                row["attached"] = False
            elif kind == "carried":
                carried.append(resource_id)
                row["attached"] = True
            else:
                raise ProgramAssistantPortError("scene resource placement is invalid")
            objects.append(row)

        if carried:
            pose_by_id = self._resolve_carried_poses(active, carried)
            for row in objects:
                resource_id = row["scene_resource_id"]
                if resource_id in pose_by_id:
                    row["pose_in_cell"] = pose_by_id[resource_id]
        return cell_id, objects


class ProgramAssistantPort:
    """One bounded OwnerTaskPort for Program-only model and selection nodes."""

    def __init__(
        self,
        model_transport: ProgramModelTransport,
        scene_source: HeadlessProgramSceneSource,
        *,
        maximum_workers: int = 4,
        maximum_tasks: int = _MAX_TASKS,
        clock: Any = time.monotonic,
    ) -> None:
        for method in ("validate_endpoint", "complete"):
            if not callable(getattr(model_transport, method, None)):
                raise TypeError(f"model transport has no callable {method}")
        if not isinstance(scene_source, HeadlessProgramSceneSource):
            raise TypeError("scene_source must be one headless Program scene source")
        if (
            type(maximum_workers) is not int
            or not 1 <= maximum_workers <= _MAX_WORKERS
            or type(maximum_tasks) is not int
            or not 1 <= maximum_tasks <= _MAX_TASKS
            or not callable(clock)
        ):
            raise ValueError("assistant port bounds are invalid")
        self._model_transport = model_transport
        self._scene_source = scene_source
        self._executor = ThreadPoolExecutor(
            max_workers=maximum_workers,
            thread_name_prefix="manyforge-program-model",
        )
        self._maximum_tasks = maximum_tasks
        self._clock = clock
        self._contexts: dict[str, _BoundContext] = {}
        self._tasks: dict[str, _Task] = {}
        self._lock = RLock()
        self._closed = False

    @staticmethod
    def _task_id(identity: NodeAttemptIdentity) -> str:
        digest = sha256(
            "\x00".join(
                (
                    identity.program_run_id,
                    identity.node_instance_id,
                    identity.node_attempt_id,
                )
            ).encode("utf-8")
        ).hexdigest()
        return f"assistant-task:{digest}"

    def bind(self, context: ProgramRunPortContext) -> None:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("assistant bind requires one Program-run context")
        if not context.program:
            raise ProgramAssistantPortError("assistant bind has no exact Program")
        assistant_nodes = _assistant_node_ids(context)
        supported_nodes = {
            "assistant.build_scene_selection",
            "assistant.query_model",
            "assistant.validate_scene_selection",
        }
        unknown_nodes = assistant_nodes - supported_nodes
        if unknown_nodes:
            raise ProgramAssistantPortError(
                f"Program contains unsupported assistant nodes {sorted(unknown_nodes)}"
            )
        assistant: _AssistantContract | None = None
        if "assistant.query_model" in assistant_nodes:
            if context.assistant_projection is None:
                raise ProgramAssistantPortError(
                    "query_model has no retained assistant projection"
                )
            assistant = _parse_assistant(context.assistant_projection)
        blackboard = _blackboard_declarations(context.program)
        if assistant is not None:
            _validate_query_nodes(
                context,
                assistant,
                blackboard,
                self._model_transport,
            )
        parsed = _BoundContext(context, assistant, blackboard)
        with self._lock:
            if self._closed:
                raise ProgramAssistantPortError("assistant port is closed")
            current = self._contexts.get(context.program_run_id)
            if current is not None:
                if current != parsed:
                    raise ProgramAssistantPortError("assistant context was rebound")
                return
            if len(self._contexts) >= _MAX_CONTEXTS:
                raise ProgramAssistantPortError("assistant context retention is full")
            self._contexts[context.program_run_id] = parsed

    def unbind(self, program_run_id: str) -> None:
        with self._lock:
            self._contexts.pop(program_run_id, None)
            task_ids = tuple(
                task_id
                for task_id, task in self._tasks.items()
                if task.identity.program_run_id == program_run_id
            )
            tasks = tuple(self._tasks.pop(task_id) for task_id in task_ids)
        for task in tasks:
            task.cancel_event.set()
            if task.future is not None:
                task.future.cancel()

    def _context(self, program_run_id: str) -> _BoundContext:
        with self._lock:
            try:
                return self._contexts[program_run_id]
            except KeyError as exc:
                raise ProgramAssistantPortError(
                    "Program-run assistant context is not bound"
                ) from exc

    def _build_selection(
        self,
        context: _BoundContext,
        inputs: OwnerNodeInputs,
    ) -> _TaskResult:
        parameters = inputs.parameters
        if parameters.get("object_source", "backend_scene") != "backend_scene":
            raise _TaskFailure(
                "selection_object_source_unsupported",
                "only the authoritative backend scene is supported",
            )
        if parameters.get("image_source", "none") != "none":
            raise _TaskFailure(
                "selection_image_source_unavailable",
                "viewer snapshots return at I9; no image was requested from Composer",
            )
        candidate_ids = list(
            _sequence(
                parameters.get("candidate_object_ids", ()),
                field="candidate_object_ids",
            )
        )
        try:
            frame_id, scene_objects = self._scene_source.selection_objects(
                context.capture, candidate_ids
            )
        except ProgramAssistantPortError as exc:
            raise _TaskFailure("selection_scene_unavailable", str(exc)) from exc
        try:
            output = build_scene_selection_context(
                selection_request_text=parameters.get("selection_request_text"),
                scene_objects=scene_objects,
                candidate_object_ids=candidate_ids,
                frame_id=frame_id,
            )
        except SceneSelectionError as exc:
            raise _TaskFailure(exc.reason_code, str(exc)) from exc
        return _TaskResult(
            output,
            {
                **(
                    {}
                    if context.assistant is None
                    else {
                        "assistant_projection_digest": (
                            context.assistant.projection_digest
                        )
                    }
                ),
                "scene_frame_id": frame_id,
            },
            self._clock(),
        )

    def _validate_selection(self, inputs: OwnerNodeInputs) -> _TaskResult:
        try:
            output = validate_scene_selection(
                selection_context=_mapping(
                    inputs.blackboard.get("selection_context_key"),
                    field="selection context",
                ),
                selection_response=_mapping(
                    inputs.blackboard.get("selection_response_key"),
                    field="selection response",
                ),
                allow_no_match=inputs.parameters.get("allow_no_match", False),
                allow_ambiguous=inputs.parameters.get("allow_ambiguous", False),
            )
        except SceneSelectionError as exc:
            raise _TaskFailure(exc.reason_code, str(exc)) from exc
        return _TaskResult(output, {}, self._clock())

    @staticmethod
    def _template_variables(
        inputs: OwnerNodeInputs,
    ) -> tuple[dict[str, Any], dict[str, str]]:
        raw_values: dict[str, Any] = {}
        prompt_values: dict[str, str] = {}
        for index, raw in enumerate(
            _sequence(
                inputs.parameters.get("template_variables", ()),
                field="template_variables",
            )
        ):
            binding = _mapping(raw, field=f"template_variables[{index}]")
            variable_id = _text(binding.get("variable_id"), field="variable_id")
            source_path = f"/template_variables/{index}/source"
            if source_path not in inputs.embedded_values:
                raise ProgramAssistantPortError(
                    f"template variable {variable_id!r} has no captured value"
                )
            if variable_id in raw_values:
                raise ProgramAssistantPortError("template variables contain duplicates")
            value = _thaw(inputs.embedded_values[source_path])
            if isinstance(value, tuple):
                value = list(value)
            raw_values[variable_id] = value
            prompt_values[variable_id] = _stringify_template_value(value)
        return raw_values, prompt_values

    def _attachment(
        self,
        context: _BoundContext,
        inputs: OwnerNodeInputs,
    ) -> Mapping[str, Any] | None:
        kind = inputs.parameters.get("scene_attachment", "none")
        if kind == "none":
            return None
        if kind == "state_json":
            return {
                "kind": "state_json",
                "state_json": self._scene_source.state_json(context.capture),
            }
        if kind == "snapshot_image":
            raise _TaskFailure(
                "model_snapshot_image_unavailable",
                "viewer snapshot transport returns at I9",
            )
        raise ProgramAssistantPortError("model scene_attachment is invalid")

    def _parse_model_response(
        self,
        response: ProgramModelResponse,
        *,
        response_mode: str,
        response_tool: Mapping[str, Any] | None,
    ) -> Any:
        if response_mode == "tool_call":
            assert response_tool is not None
            expected_name = response_tool["name"]
            if response.tool_name != expected_name:
                raise _TaskFailure(
                    "model_tool_call_mismatched",
                    f"expected model tool {expected_name!r}",
                )
            if isinstance(response.tool_arguments, Mapping):
                return _thaw(response.tool_arguments)
            parsed = _parse_json_text(response.tool_arguments)
        else:
            parsed = _parse_json_text(response.text)
        if parsed is None:
            raise _TaskFailure(
                "model_response_parse_failed",
                "model response is not valid bounded JSON",
            )
        return parsed

    def _query_model(
        self,
        context: _BoundContext,
        inputs: OwnerNodeInputs,
        cancel_event: Event,
        started_at_s: float,
        deadline_s: float,
    ) -> _TaskResult:
        parameters = inputs.parameters
        if context.assistant is None:
            raise _TaskFailure(
                "model_contract_unavailable",
                "query_model has no retained assistant contract",
            )
        endpoint_alias = _text(
            parameters.get("model_endpoint_alias"), field="model_endpoint_alias"
        )
        template_id = _text(
            parameters.get("prompt_template_id"), field="prompt_template_id"
        )
        endpoint = context.assistant.endpoints[endpoint_alias]
        template = context.assistant.templates[template_id]
        raw_variables, prompt_variables = self._template_variables(inputs)
        if list(raw_variables) != list(template["variable_ids"]):
            raise _TaskFailure(
                "model_template_variables_mismatched",
                "captured template variables differ from the retained template",
            )
        output_key = _text(parameters.get("output_key"), field="output_key")
        declaration = context.blackboard[output_key]
        schema_template = _json_schema(declaration, raw_variables)
        try:
            output_schema = _resolve_output_schema_template(
                schema_template, raw_variables
            )
            prompt = _render_prompt_template(
                template["template_text"], prompt_variables
            )
        except (ModelOutputValidationError, TypeError, ValueError) as exc:
            raise _TaskFailure("model_output_contract_invalid", str(exc)) from exc
        response_contract = template["response_contract"]
        response_mode = response_contract["response_mode"]
        response_tool = None
        if response_mode == "tool_call":
            try:
                response_tool = _resolve_response_tool_template(
                    {
                        "name": response_contract["tool_name"],
                        "description": response_contract["tool_description"],
                        "parameters": schema_template,
                    },
                    raw_variables,
                )
            except (ModelOutputValidationError, TypeError, ValueError) as exc:
                raise _TaskFailure("model_output_contract_invalid", str(exc)) from exc
        attachment = self._attachment(context, inputs)
        timeout_s = deadline_s - started_at_s
        minimum_confidence = parameters.get("min_confidence", 0.0)
        confidence_declared = "confidence" in output_schema["properties"]
        retry = parameters.get("retry_on_parse_failure", False) is True
        prompt_attempt = prompt
        response: ProgramModelResponse | None = None
        parsed: Any = None
        for attempt in range(2 if retry else 1):
            if cancel_event.is_set():
                raise _TaskFailure(
                    "model_request_cancelled",
                    "model request was cancelled",
                )
            remaining_s = min(timeout_s, deadline_s - self._clock())
            if remaining_s <= 0.0:
                raise _TaskFailure("model_request_timeout", "model request timed out")
            request = ProgramModelRequest(
                model_endpoint_alias=endpoint_alias,
                external_resource_alias=endpoint["external_resource_alias"],
                model_id=endpoint["model_id"],
                prompt_template_id=template_id,
                prompt_template_version=template["version"],
                prompt_template_digest=template["content_digest"],
                prompt=prompt_attempt,
                timeout_s=remaining_s,
                max_output_tokens=endpoint["request_contract"]["max_output_tokens"],
                response_mode=response_mode,
                tool_call_family=endpoint["request_contract"]["tool_call_family"],
                response_tool=response_tool,
                attachment=attachment,
            )
            try:
                response = self._model_transport.complete(request, cancel_event)
            except Exception as exc:
                if cancel_event.is_set():
                    raise _TaskFailure(
                        "model_request_cancelled", "model request was cancelled"
                    ) from exc
                raise _TaskFailure("model_request_failed", str(exc)) from exc
            if not isinstance(response, ProgramModelResponse):
                raise _TaskFailure(
                    "model_transport_response_invalid",
                    "model transport returned an invalid response type",
                )
            if cancel_event.is_set():
                raise _TaskFailure(
                    "model_request_cancelled",
                    "model request was cancelled",
                )
            try:
                parsed = self._parse_model_response(
                    response,
                    response_mode=response_mode,
                    response_tool=response_tool,
                )
            except _TaskFailure as exc:
                if (
                    exc.reason_code != "model_response_parse_failed"
                    or attempt != 0
                    or not retry
                ):
                    raise
                prompt_attempt = prompt + _PARSE_RETRY_SUFFIX
                continue
            break
        assert response is not None
        try:
            confidence = _validate_model_output(
                parsed,
                output_schema,
                min_confidence=(
                    minimum_confidence if confidence_declared else None
                ),
            )
        except ModelOutputValidationError as exc:
            reason = (
                "model_confidence_below_threshold"
                if "below minimum" in str(exc)
                else "model_response_schema_invalid"
            )
            raise _TaskFailure(reason, str(exc)) from exc
        completed_at = self._clock()
        if completed_at > deadline_s:
            raise _TaskFailure("model_request_timeout", "model request timed out")
        raw_response = (
            response.tool_arguments
            if response_mode == "tool_call"
            else response.text
        )
        if isinstance(raw_response, Mapping):
            raw_response = _thaw(raw_response)
        response_bytes = _raw_response_bytes(raw_response)
        audit = context.assistant.audit_policy
        metadata: dict[str, Any] = {
            "assistant_projection_digest": context.assistant.projection_digest,
            "argument_digests": {
                variable_id: _sha256_json(
                    value,
                    field=f"template variable {variable_id!r}",
                )
                for variable_id, value in raw_variables.items()
            },
            "confidence": confidence,
            "latency_ms": max(
                0.0,
                (completed_at - started_at_s) * 1_000.0,
            ),
            "model_call_attempt_count": attempt + 1,
            "model_endpoint_alias": endpoint_alias,
            "model_response_digest": (
                _DIGEST_PREFIX + sha256(response_bytes).hexdigest()
            ),
            "model_version": response.model_version,
            "prompt_digest": _DIGEST_PREFIX
            + sha256(prompt_attempt.encode("utf-8")).hexdigest(),
            "prompt_template_digest": template["content_digest"],
            "prompt_template_id": template_id,
            "prompt_template_version": template["version"],
        }
        if attempt > 0:
            metadata["initial_prompt_digest"] = _DIGEST_PREFIX + sha256(
                prompt.encode("utf-8")
            ).hexdigest()
        if attachment is not None:
            metadata["attachment_digest"] = _sha256_json(
                attachment,
                field="model attachment",
            )
        if audit["redact_argument_values"] is not True:
            metadata["template_variables"] = _thaw(raw_variables)
        if audit["store_full_prompts"] is True:
            metadata["prompt"] = prompt_attempt
            if attempt > 0:
                metadata["initial_prompt"] = prompt
        if audit["store_full_responses"] is True:
            metadata["raw_response"] = _thaw(raw_response)
        if response.usage is not None:
            metadata["usage"] = dict(response.usage)
        return _TaskResult(parsed, metadata, completed_at)

    @staticmethod
    def _authored_failure_policy(
        task: _Task,
        reason_code: str,
    ) -> Mapping[str, Any]:
        parameter: str | None = None
        if task.node_id == "assistant.query_model":
            parameter = (
                "model_low_confidence_policy"
                if reason_code == "model_confidence_below_threshold"
                else "model_runtime_failure_policy"
            )
        elif task.node_id == "assistant.build_scene_selection" and reason_code in {
            "selection_candidates_unavailable",
            "selection_object_pose_unavailable",
            "selection_scene_unavailable",
        }:
            parameter = "unavailable_policy"
        if parameter is None:
            return {}
        policy = task.inputs.parameters.get(parameter, "pause_for_operator")
        if policy not in {"pause_for_operator", "condition_failure"}:
            return {}
        return {
            "authored_failure_policy": policy,
            "authored_failure_policy_parameter": parameter,
        }

    def _failure_snapshot(
        self,
        task_id: str,
        reason_code: str,
        message: str,
        *,
        task: _Task | None = None,
    ) -> OwnerTaskSnapshot:
        return OwnerTaskSnapshot(
            task_id,
            OwnerTaskState.FAILED,
            metadata=(
                {}
                if task is None
                else self._authored_failure_policy(task, reason_code)
            ),
            reason_code=reason_code,
            message=message,
        )

    def _refresh(self, task_id: str, task: _Task) -> OwnerTaskSnapshot:
        if task.terminal is not None:
            return task.terminal
        if task.future is None:
            raise ProgramAssistantPortError("assistant task has no result")
        if not task.future.done():
            if task.deadline_s is not None and self._clock() >= task.deadline_s:
                task.cancel_event.set()
                task.future.cancel()
                task.terminal = self._failure_snapshot(
                    task_id,
                    "model_request_timeout",
                    "model request timed out",
                    task=task,
                )
                return task.terminal
            return OwnerTaskSnapshot(task_id, OwnerTaskState.ACTIVE)
        try:
            result = task.future.result()
        except _TaskFailure as exc:
            task.terminal = self._failure_snapshot(
                task_id,
                exc.reason_code,
                str(exc),
                task=task,
            )
        except Exception as exc:
            task.terminal = self._failure_snapshot(
                task_id,
                "assistant_task_failed",
                str(exc),
                task=task,
            )
        else:
            if task.cancel_event.is_set():
                task.terminal = OwnerTaskSnapshot(
                    task_id,
                    OwnerTaskState.INTERRUPTED,
                    reason_code="assistant_task_cancelled",
                    message="assistant task was cancelled",
                )
            elif (
                task.deadline_s is not None
                and result.completed_at_s > task.deadline_s
            ):
                task.terminal = self._failure_snapshot(
                    task_id,
                    "model_request_timeout",
                    "model request timed out",
                    task=task,
                )
            else:
                task.terminal = OwnerTaskSnapshot(
                    task_id,
                    OwnerTaskState.SUCCEEDED,
                    output=result.output,
                    metadata=result.metadata,
                )
        return task.terminal

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        if node_id not in {
            "assistant.build_scene_selection",
            "assistant.query_model",
            "assistant.validate_scene_selection",
        }:
            raise ProgramAssistantPortError(f"unsupported assistant node {node_id!r}")
        if not isinstance(identity, NodeAttemptIdentity) or not isinstance(
            inputs, OwnerNodeInputs
        ):
            raise TypeError("assistant task identity or inputs are invalid")
        context = self._context(identity.program_run_id)
        task_id = self._task_id(identity)
        with self._lock:
            existing = self._tasks.get(task_id)
            if existing is not None:
                if (
                    existing.identity != identity
                    or existing.node_id != node_id
                    or existing.inputs != inputs
                ):
                    raise ProgramAssistantPortError(
                        "assistant task identity was reused"
                    )
                return self._refresh(task_id, existing)
            if self._closed:
                raise ProgramAssistantPortError("assistant port is closed")
            if len(self._tasks) >= self._maximum_tasks:
                raise ProgramAssistantPortError("assistant task retention is full")
            task = _Task(identity, node_id, inputs, Event())
            self._tasks[task_id] = task
        try:
            if node_id == "assistant.build_scene_selection":
                result = self._build_selection(context, inputs)
                task.terminal = OwnerTaskSnapshot(
                    task_id,
                    OwnerTaskState.SUCCEEDED,
                    output=result.output,
                    metadata=result.metadata,
                )
                return task.terminal
            if node_id == "assistant.validate_scene_selection":
                result = self._validate_selection(inputs)
                task.terminal = OwnerTaskSnapshot(
                    task_id,
                    OwnerTaskState.SUCCEEDED,
                    output=result.output,
                    metadata=result.metadata,
                )
                return task.terminal
            if context.assistant is None:
                raise _TaskFailure(
                    "model_contract_unavailable",
                    "query_model has no retained assistant contract",
                )
            endpoint_alias = _text(
                inputs.parameters.get("model_endpoint_alias"),
                field="model_endpoint_alias",
            )
            try:
                default_timeout = context.assistant.endpoints[endpoint_alias][
                    "request_contract"
                ]["default_timeout_s"]
            except KeyError as exc:
                raise _TaskFailure(
                    "model_contract_unavailable",
                    "query_model endpoint is absent from the captured contract",
                ) from exc
            timeout = inputs.parameters.get("timeout_s", default_timeout)
            if (
                isinstance(timeout, bool)
                or not isinstance(timeout, (int, float))
                or not math.isfinite(float(timeout))
                or not 0.0 < float(timeout) <= _MAX_TIMEOUT_S
            ):
                raise _TaskFailure(
                    "model_timeout_invalid", "model timeout is outside its bound"
                )
            started_at_s = self._clock()
            task.deadline_s = started_at_s + float(timeout)
            task.future = self._executor.submit(
                self._query_model,
                context,
                inputs,
                task.cancel_event,
                started_at_s,
                task.deadline_s,
            )
            return OwnerTaskSnapshot(task_id, OwnerTaskState.ACTIVE)
        except _TaskFailure as exc:
            task.terminal = self._failure_snapshot(
                task_id,
                exc.reason_code,
                str(exc),
                task=task,
            )
            return task.terminal
        except (ProgramAssistantPortError, SceneSelectionError) as exc:
            task.terminal = self._failure_snapshot(
                task_id,
                "assistant_task_invalid",
                str(exc),
                task=task,
            )
            return task.terminal

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        with self._lock:
            try:
                task = self._tasks[task_id]
            except KeyError as exc:
                raise ProgramAssistantPortError("assistant task is unknown") from exc
            return self._refresh(task_id, task)

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        with self._lock:
            try:
                task = self._tasks[task_id]
            except KeyError as exc:
                raise ProgramAssistantPortError("assistant task is unknown") from exc
            if task.identity != identity:
                raise ProgramAssistantPortError(
                    "assistant cancellation identity differs"
                )
            if task.terminal is not None:
                return
            task.cancel_event.set()
            if task.future is not None:
                task.future.cancel()
            task.terminal = OwnerTaskSnapshot(
                task_id,
                OwnerTaskState.INTERRUPTED,
                reason_code="assistant_task_cancelled",
                message="assistant task was cancelled",
            )

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        with self._lock:
            try:
                task = self._tasks[task_id]
            except KeyError as exc:
                raise ProgramAssistantPortError("assistant task is unknown") from exc
            if task.identity != identity:
                raise ProgramAssistantPortError(
                    "assistant acknowledgement identity differs"
                )
            snapshot = self._refresh(task_id, task)
            if snapshot.state is OwnerTaskState.ACTIVE:
                raise ProgramAssistantPortError(
                    "active assistant task cannot be acknowledged"
                )
            del self._tasks[task_id]

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            tasks = tuple(self._tasks.values())
            self._contexts.clear()
            self._tasks.clear()
        for task in tasks:
            task.cancel_event.set()
            if task.future is not None:
                task.future.cancel()
        self._executor.shutdown(wait=False, cancel_futures=True)


__all__ = [
    "HeadlessProgramSceneSource",
    "ProgramAssistantPort",
    "ProgramAssistantPortError",
    "ProgramModelRequest",
    "ProgramModelResponse",
    "ProgramModelTransport",
    "ProgramModelTransportError",
]
