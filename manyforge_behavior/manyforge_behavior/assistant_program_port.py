"""Bounded Program model/selection owner over retained assistant contracts.

This port is deliberately distinct from interactive Composer assistants.  It
binds one exact Applied Program, retained assistant projection, and scene-owner
lineage before a tree is constructed.  Model transports resolve concrete site
configuration outside this module and receive only the compiler-qualified
endpoint/template request.
"""

from __future__ import annotations

import base64
import binascii
from collections.abc import Mapping, Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from copy import deepcopy
from dataclasses import dataclass, field
from datetime import datetime, timezone
from hashlib import sha256
import json
import math
from threading import Event, RLock
import time
import unicodedata
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
from manyforge_behavior.json_scalars import nonnegative_int64_value


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
_MAX_MODEL_USAGE_BYTES: Final = 64 * 1024
_MAX_MODEL_AUDIT_RECORD_BYTES: Final = 256 * 1024
_MAX_MODEL_AUDIT_CANDIDATE_BYTES: Final = _MAX_MODEL_TEXT_BYTES + 512 * 1024
_MAX_INLINE_IMAGE_BYTES: Final = 2 * 1024 * 1024
_MAX_SCENE_POSE_QUERY_RESOURCES: Final = 128
_MAX_SAFE_INTEGER: Final = 9_007_199_254_740_991
_PARSE_RETRY_SUFFIX: Final = (
    "\n\nThe previous response did not satisfy the required structured output "
    "contract. Return ONLY valid structured output for this request. Do not "
    "include markdown fences, prose, or any extra text."
)
_DIGEST_PREFIX: Final = "sha256:"


def _utc_wall_time_text() -> str:
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="microseconds")
        .replace("+00:00", "Z")
    )


def _bounded_nfc_text(value: str, *, maximum_bytes: int) -> str:
    normalized = unicodedata.normalize("NFC", value)
    encoded = normalized.encode("utf-8")
    if len(encoded) <= maximum_bytes:
        return normalized
    return encoded[:maximum_bytes].decode("utf-8", errors="ignore")


def _thaw(value: Any) -> Any:
    """Copy one recursively frozen JSON value into ordinary containers."""

    if isinstance(value, Mapping):
        return {key: _thaw(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_thaw(nested) for nested in value]
    return deepcopy(value)


def _bounded_json_copy(
    value: Any,
    *,
    field: str,
    maximum_bytes: int = _MAX_MODEL_TEXT_BYTES,
) -> Any:
    """Copy transport-owned JSON after enforcing one fixed byte bound."""

    try:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (RecursionError, TypeError, UnicodeError, ValueError) as exc:
        raise ValueError(f"{field} must be finite acyclic JSON data") from exc
    if len(encoded) > maximum_bytes:
        raise ValueError(f"{field} exceeds its fixed byte bound")
    return json.loads(encoded)


class ProgramAssistantPortError(RuntimeError):
    """An assistant contract, scene projection, or task request is invalid."""


class ProgramModelTransportError(RuntimeError):
    """A bounded model transport failed before returning a valid response."""


class ProgramSnapshotSourceError(RuntimeError):
    """A snapshot source failed with one safe Program-facing reason."""

    def __init__(self, reason_code: str, message: str) -> None:
        if (
            not isinstance(reason_code, str)
            or not reason_code
            or len(reason_code.encode("utf-8")) > 256
        ):
            raise ValueError("snapshot source reason code must be bounded text")
        super().__init__(_bounded_nfc_text(str(message), maximum_bytes=4_096))
        self.reason_code = reason_code


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
                else MappingProxyType(
                    _bounded_json_copy(
                        self.response_tool,
                        field="model response tool",
                    )
                )
            ),
        )
        attachment = (
            None
            if self.attachment is None
            else _bounded_json_copy(
                self.attachment,
                field="model attachment",
            )
        )
        if isinstance(attachment, Mapping) and attachment.get("kind") == (
            "snapshot_image"
        ):
            attachment = validate_program_snapshot_attachment(attachment)
        object.__setattr__(
            self,
            "attachment",
            None if attachment is None else MappingProxyType(attachment),
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
                    _bounded_json_copy(
                        self.usage,
                        field="model usage",
                        maximum_bytes=_MAX_MODEL_USAGE_BYTES,
                    )
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


class ProgramSnapshotSource(Protocol):
    """Transport-neutral bounded source for one owner-attributed image."""

    def capture_snapshot(
        self,
        *,
        width: int,
        height: int,
        image_format: str,
        transport: str,
        timeout_s: float,
        cancel_event: Event,
    ) -> Mapping[str, Any]: ...


class ProgramModelAuditSink(Protocol):
    """Synchronous durable boundary for one external model attempt."""

    def commit(self, record: Mapping[str, Any]) -> None: ...


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
    external_attempts: dict[int, "_ExternalAttempt"] = field(
        default_factory=dict
    )
    audit_lock: RLock = field(default_factory=RLock, repr=False)


@dataclass(slots=True)
class _ExternalAttempt:
    ordinal: int
    started_at_s: float
    record: Mapping[str, Any]
    store_full_response: bool
    pending_terminal_record: Mapping[str, Any] | None = None
    terminal_outcome: str | None = None


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


def validate_program_snapshot_attachment(
    value: Mapping[str, Any],
) -> dict[str, Any]:
    """Validate and copy one exact inline snapshot attachment."""

    attachment = dict(_mapping(value, field="snapshot attachment"))
    _exact(
        attachment,
        {"data_url", "kind", "media_type", "source", "transport"},
        path="snapshot attachment",
    )
    media_type = attachment.get("media_type")
    if (
        attachment.get("kind") != "snapshot_image"
        or attachment.get("transport") != "inline_data_url"
        or media_type not in {"image/jpeg", "image/png"}
    ):
        raise ProgramAssistantPortError("snapshot attachment contract is invalid")
    data_url = attachment.get("data_url")
    prefix = f"data:{media_type};base64,"
    if (
        not isinstance(data_url, str)
        or not data_url.startswith(prefix)
        or len(data_url.encode("ascii", errors="ignore")) != len(data_url)
        or len(data_url) > len(prefix) + 4 * ((_MAX_INLINE_IMAGE_BYTES + 2) // 3)
    ):
        raise ProgramAssistantPortError("snapshot data URL is invalid or oversized")
    encoded_image = data_url[len(prefix) :]
    try:
        image = base64.b64decode(encoded_image, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ProgramAssistantPortError("snapshot data URL is not canonical base64") from exc
    if (
        not image
        or len(image) > _MAX_INLINE_IMAGE_BYTES
        or base64.b64encode(image).decode("ascii") != encoded_image
        or (media_type == "image/jpeg" and not image.startswith(b"\xff\xd8\xff"))
        or (
            media_type == "image/png"
            and not image.startswith(b"\x89PNG\r\n\x1a\n")
        )
    ):
        raise ProgramAssistantPortError("snapshot image payload is invalid")
    source = dict(_mapping(attachment.get("source"), field="snapshot source"))
    _exact(
        source,
        {
            "height",
            "image_sha256",
            "schema_version",
            "viewer_generation",
            "viewer_owner_epoch",
            "width",
        },
        path="snapshot source",
    )
    owner_epoch = source.get("viewer_owner_epoch")
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-"
    if (
        source.get("schema_version") != "manyforge.program_snapshot_source.v1"
        or not isinstance(owner_epoch, str)
        or not owner_epoch
        or len(owner_epoch.encode("utf-8")) > 256
        or owner_epoch[0]
        not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        or any(character not in allowed for character in owner_epoch)
        or type(source.get("viewer_generation")) is not int
        or not 0 <= source["viewer_generation"] <= _MAX_SAFE_INTEGER
        or type(source.get("width")) is not int
        or not 64 <= source["width"] <= 1_920
        or type(source.get("height")) is not int
        or not 64 <= source["height"] <= 1_080
        or source["image_sha256"]
        != _DIGEST_PREFIX + sha256(image).hexdigest()
    ):
        raise ProgramAssistantPortError("snapshot source identity is invalid")
    return {
        "data_url": data_url,
        "kind": "snapshot_image",
        "media_type": media_type,
        "source": source,
        "transport": "inline_data_url",
    }


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
        elif declared_type == "stored_pose":
            schema = {
                "type": "object",
                "properties": {
                    "frame_ref": {
                        "type": "object",
                        "properties": {
                            "kind": {
                                "type": "string",
                                "enum": [
                                    "cell_frame",
                                    "target_frame",
                                    "scene_resource",
                                ],
                            },
                            "target_role": {"type": "string"},
                            "frame_role": {"type": "string"},
                            "scene_resource_id": {"type": "string"},
                        },
                        "required": ["kind"],
                        "additionalProperties": False,
                    },
                    "position_m": {
                        "type": "array",
                        "items": {"type": "number"},
                        "minItems": 3,
                        "maxItems": 3,
                    },
                    "orientation": {
                        "type": "object",
                        "properties": {
                            "xyzw": {
                                "type": "array",
                                "items": {"type": "number"},
                                "minItems": 4,
                                "maxItems": 4,
                            },
                            "rpy_deg": {
                                "type": "array",
                                "items": {"type": "number"},
                                "minItems": 3,
                                "maxItems": 3,
                            },
                        },
                        "required": [],
                        "additionalProperties": False,
                    },
                },
                "required": ["frame_ref", "position_m", "orientation"],
                "additionalProperties": False,
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


def _validate_program_model_outputs(
    parsed: Any,
    declaration: Mapping[str, Any],
    program: Mapping[str, Any],
) -> None:
    """Close Program-identity semantics outside the JSON-schema subset."""

    if not isinstance(parsed, Mapping):
        return
    target_roles = _mapping(program.get("target_roles"), field="target_roles")
    scene_ids = {
        _text(
            _mapping(row, field="Program scene row").get("scene_resource_id"),
            field="scene_resource_id",
        )
        for row in _sequence(program.get("scene", ()), field="Program scene")
    }
    fields = _sequence(declaration.get("fields"), field="output declaration fields")
    for index, raw in enumerate(fields):
        field = _mapping(raw, field=f"output declaration fields[{index}]")
        name = _text(field.get("field"), field="output field")
        if field.get("type") == "scene_resource":
            value = parsed.get(name)
            empty_selection = (
                value == ""
                and parsed.get("outcome") in {"no_match", "ambiguous"}
            )
            if not empty_selection and value not in scene_ids:
                raise ModelOutputValidationError(
                    f"$.{name}: undeclared scene resource"
                )
        if field.get("type") != "stored_pose":
            continue
        pose = _mapping(parsed.get(name), field=f"model output {name}")
        frame = _mapping(pose.get("frame_ref"), field=f"model output {name}.frame_ref")
        kind = frame.get("kind")
        if kind == "cell_frame":
            expected_frame_fields = {"kind"}
        elif kind == "target_frame":
            expected_frame_fields = {"kind", "target_role", "frame_role"}
            target_role = _text(
                frame.get("target_role"),
                field=f"model output {name}.frame_ref.target_role",
            )
            frame_role = _text(
                frame.get("frame_role"),
                field=f"model output {name}.frame_ref.frame_role",
            )
            try:
                target = _mapping(
                    target_roles[target_role],
                    field=f"target role {target_role}",
                )
            except KeyError as exc:
                raise ModelOutputValidationError(
                    f"$.{name}.frame_ref.target_role: undeclared target role"
                ) from exc
            required_frames = _sequence(
                target.get("required_frame_roles", ()),
                field=f"target role {target_role}.required_frame_roles",
            )
            if frame_role not in required_frames:
                raise ModelOutputValidationError(
                    f"$.{name}.frame_ref.frame_role: undeclared frame role"
                )
        elif kind == "scene_resource":
            expected_frame_fields = {"kind", "scene_resource_id"}
            scene_resource_id = _text(
                frame.get("scene_resource_id"),
                field=f"model output {name}.frame_ref.scene_resource_id",
            )
            if scene_resource_id not in scene_ids:
                raise ModelOutputValidationError(
                    f"$.{name}.frame_ref.scene_resource_id: undeclared scene resource"
                )
        else:
            raise ModelOutputValidationError(
                f"$.{name}.frame_ref.kind: unsupported pose frame"
            )
        if set(frame) != expected_frame_fields:
            raise ModelOutputValidationError(
                f"$.{name}.frame_ref: fields disagree with frame kind"
            )
        orientation = _mapping(
            pose.get("orientation"),
            field=f"model output {name}.orientation",
        )
        if set(orientation) not in ({"xyzw"}, {"rpy_deg"}):
            raise ModelOutputValidationError(
                f"$.{name}.orientation: exactly one representation is required"
            )
        if "xyzw" in orientation:
            quaternion = _sequence(
                orientation["xyzw"],
                field=f"model output {name}.orientation.xyzw",
            )
            try:
                normalized_quaternion = [float(value) for value in quaternion]
                squared_norm = sum(
                    value * value for value in normalized_quaternion
                )
            except OverflowError as exc:
                raise ModelOutputValidationError(
                    f"$.{name}.orientation.xyzw: must be finite"
                ) from exc
            if not math.isfinite(squared_norm) or abs(squared_norm - 1.0) > 1e-6:
                raise ModelOutputValidationError(
                    f"$.{name}.orientation.xyzw: must be a unit quaternion"
                )


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
        audit_sink: ProgramModelAuditSink | None = None,
        snapshot_source: ProgramSnapshotSource | None = None,
        maximum_workers: int = 4,
        maximum_tasks: int = _MAX_TASKS,
        clock_ns: Any = time.monotonic_ns,
        wall_clock: Any = _utc_wall_time_text,
    ) -> None:
        for method in ("validate_endpoint", "complete"):
            if not callable(getattr(model_transport, method, None)):
                raise TypeError(f"model transport has no callable {method}")
        if not isinstance(scene_source, HeadlessProgramSceneSource):
            raise TypeError("scene_source must be one headless Program scene source")
        if audit_sink is not None and not callable(
            getattr(audit_sink, "commit", None)
        ):
            raise TypeError("audit_sink must commit durable model attempts")
        if snapshot_source is not None and not callable(
            getattr(snapshot_source, "capture_snapshot", None)
        ):
            raise TypeError("snapshot_source must capture bounded snapshots")
        if (
            type(maximum_workers) is not int
            or not 1 <= maximum_workers <= _MAX_WORKERS
            or type(maximum_tasks) is not int
            or not 1 <= maximum_tasks <= _MAX_TASKS
            or not callable(clock_ns)
            or not callable(wall_clock)
        ):
            raise ValueError("assistant port bounds are invalid")
        self._model_transport = model_transport
        self._scene_source = scene_source
        self._audit_sink = audit_sink
        self._snapshot_source = snapshot_source
        self._executor = ThreadPoolExecutor(
            max_workers=maximum_workers,
            thread_name_prefix="manyforge-program-model",
        )
        self._maximum_tasks = maximum_tasks
        self._clock_ns = clock_ns
        self._wall_clock = wall_clock
        self._contexts: dict[str, _BoundContext] = {}
        self._tasks: dict[str, _Task] = {}
        self._lock = RLock()
        self._closed = False

    def _clock(self) -> float:
        """Second-based timeout budgets share the exact record clock."""
        return self._clock_ns() / 1_000_000_000

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
            if self._audit_sink is None:
                raise ProgramAssistantPortError(
                    "query_model has no durable Program model-attempt audit sink"
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
            tasks = tuple(
                (task_id, task)
                for task_id, task in self._tasks.items()
                if task.identity.program_run_id == program_run_id
            )
        errors: list[Exception] = []
        for _task_id, task in tasks:
            task.cancel_event.set()
            if task.future is not None:
                task.future.cancel()
            try:
                self._commit_active_model_attempt(
                    task,
                    outcome="cancelled",
                    error_code="model_request_cancelled",
                    error_message="Program run released the model request",
                )
            except Exception as error:
                errors.append(error)
        if errors:
            detail = "; ".join(
                str(error) or type(error).__name__ for error in errors
            )
            raise ProgramAssistantPortError(
                f"Program model-attempt cleanup was not durable: {detail}"
            ) from errors[0]
        with self._lock:
            for task_id, task in tasks:
                if self._tasks.get(task_id) is task:
                    self._tasks.pop(task_id)
            self._contexts.pop(program_run_id, None)

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
                "scene-selection context accepts authoritative scene data only",
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
        *,
        deadline_s: float,
        cancel_event: Event,
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
            if self._snapshot_source is None:
                raise _TaskFailure(
                    "model_snapshot_image_source_unavailable",
                    "no Program snapshot source is configured",
                )
            if cancel_event.is_set():
                raise _TaskFailure(
                    "model_request_cancelled",
                    "model request was cancelled",
                )
            remaining_s = deadline_s - self._clock()
            if remaining_s <= 0.0:
                raise _TaskFailure(
                    "model_request_timeout",
                    "model request timed out before snapshot capture",
                )
            try:
                attachment = self._snapshot_source.capture_snapshot(
                    width=inputs.parameters.get("scene_snapshot_width", 640),
                    height=inputs.parameters.get("scene_snapshot_height", 480),
                    image_format=inputs.parameters.get(
                        "scene_snapshot_format", "jpeg"
                    ),
                    transport=inputs.parameters.get(
                        "scene_snapshot_transport", "inline_data_url"
                    ),
                    timeout_s=remaining_s,
                    cancel_event=cancel_event,
                )
            except ProgramSnapshotSourceError as exc:
                raise _TaskFailure(exc.reason_code, str(exc)) from exc
            except Exception as exc:
                raise _TaskFailure(
                    "model_snapshot_capture_failed",
                    "Program snapshot capture failed",
                ) from exc
            if cancel_event.is_set():
                raise _TaskFailure(
                    "model_request_cancelled",
                    "model request was cancelled",
                )
            try:
                return validate_program_snapshot_attachment(attachment)
            except ProgramAssistantPortError as exc:
                raise _TaskFailure(
                    "model_snapshot_capture_invalid",
                    str(exc),
                ) from exc
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

    def _begin_model_attempt(
        self,
        task: _Task,
        *,
        ordinal: int,
        context: _BoundContext,
        endpoint_alias: str,
        endpoint: Mapping[str, Any],
        template_id: str,
        template: Mapping[str, Any],
        prompt: str,
        raw_variables: Mapping[str, Any],
        attachment: Mapping[str, Any] | None,
    ) -> None:
        attempt_id = "model-attempt:" + sha256(
            "\x00".join(
                (
                    task.identity.program_run_id,
                    task.identity.node_instance_id,
                    task.identity.node_attempt_id,
                    str(ordinal),
                )
            ).encode("utf-8")
        ).hexdigest()
        record = {
            "schema_version": "manyforge.program_model_attempt.v1",
            "model_attempt_id": attempt_id,
            "program_run_id": task.identity.program_run_id,
            "node_instance_id": task.identity.node_instance_id,
            "node_attempt_id": task.identity.node_attempt_id,
            "external_attempt_index": ordinal,
            "assistant_projection_digest": (
                context.assistant.projection_digest
                if context.assistant is not None
                else None
            ),
            "model_endpoint_alias": endpoint_alias,
            "external_resource_alias": endpoint["external_resource_alias"],
            "model_id": endpoint["model_id"],
            "prompt_template_id": template_id,
            "prompt_template_version": template["version"],
            "prompt_template_digest": template["content_digest"],
            "prompt_digest": _DIGEST_PREFIX
            + sha256(prompt.encode("utf-8")).hexdigest(),
            "argument_digests": {
                variable_id: _sha256_json(
                    value,
                    field=f"template variable {variable_id!r}",
                )
                for variable_id, value in raw_variables.items()
            },
            "attachment_digest": (
                None
                if attachment is None
                else _sha256_json(attachment, field="model attachment")
            ),
            "audit_policy": dict(context.assistant.audit_policy),
            "prompt_text": (
                prompt
                if context.assistant.audit_policy["store_full_prompts"]
                else None
            ),
            "argument_values": (
                None
                if context.assistant.audit_policy["redact_argument_values"]
                else _thaw(raw_variables)
            ),
        }
        try:
            frozen_record = MappingProxyType(
                _bounded_json_copy(
                    record,
                    field="Program model-attempt initial record",
                    maximum_bytes=_MAX_MODEL_AUDIT_RECORD_BYTES,
                )
            )
        except ValueError as exc:
            raise _TaskFailure(
                "model_audit_record_invalid",
                str(exc),
            ) from exc
        with task.audit_lock:
            if ordinal in task.external_attempts:
                raise _TaskFailure(
                    "model_audit_identity_conflict",
                    "external model attempt ordinal was reused",
                )
            task.external_attempts[ordinal] = _ExternalAttempt(
                ordinal,
                self._clock(),
                frozen_record,
                context.assistant.audit_policy["store_full_responses"],
            )

    def _commit_model_attempt(
        self,
        task: _Task,
        ordinal: int,
        *,
        outcome: str,
        error_code: str | None = None,
        error_message: str = "",
        response: ProgramModelResponse | None = None,
        raw_response: Any = None,
        validated_output: Any = None,
    ) -> bool:
        allowed_outcomes = {
            "success",
            "timeout",
            "parse_error",
            "schema_error",
            "confidence_below_threshold",
            "cancelled",
            "transport_error",
            "invalid_response",
        }
        if outcome not in allowed_outcomes:
            raise _TaskFailure(
                "model_audit_outcome_invalid",
                "external model attempt outcome is unsupported",
            )
        with task.audit_lock:
            try:
                attempt = task.external_attempts[ordinal]
            except KeyError as exc:
                raise _TaskFailure(
                    "model_audit_identity_missing",
                    "external model attempt was not registered",
                ) from exc
            if attempt.terminal_outcome is not None:
                return False
            if attempt.pending_terminal_record is None:
                completed_at_ns = self._clock_ns()
                try:
                    occurred_at_ns = nonnegative_int64_value(completed_at_ns)
                except ValueError as exc:
                    raise _TaskFailure(
                        "model_audit_record_invalid",
                        "model-attempt terminal time is outside its bound",
                    ) from exc
                completed_at_s = completed_at_ns / 1_000_000_000
                try:
                    wall_time = self._wall_clock()
                    if not isinstance(wall_time, str) or not wall_time:
                        raise ValueError("wall clock returned no text")
                    occurred_at_wall = _bounded_nfc_text(
                        wall_time,
                        maximum_bytes=128,
                    )
                except (TypeError, UnicodeError, ValueError) as exc:
                    raise _TaskFailure(
                        "model_audit_record_invalid",
                        "model-attempt wall time is invalid",
                    ) from exc
                response_digest = None
                if raw_response is not None:
                    response_digest = _DIGEST_PREFIX + sha256(
                        _raw_response_bytes(raw_response)
                    ).hexdigest()
                attempt.pending_terminal_record = MappingProxyType(
                    _bounded_json_copy(
                        {
                            **dict(attempt.record),
                            "outcome": outcome,
                            "error_code": error_code,
                            "error_message": _bounded_nfc_text(
                                error_message,
                                maximum_bytes=4_096,
                            ),
                            "response_digest": response_digest,
                            "validated_output_digest": (
                                None
                                if validated_output is None
                                else _sha256_json(
                                    validated_output,
                                    field="validated model output",
                                )
                            ),
                            "model_version": _bounded_nfc_text(
                                "" if response is None else response.model_version,
                                maximum_bytes=_MAX_MODEL_VERSION_BYTES,
                            ),
                            "occurred_at_ns": occurred_at_ns,
                            "occurred_at_wall": occurred_at_wall,
                            "latency_ms": min(
                                _MAX_TIMEOUT_S * 1_000.0,
                                max(
                                    0.0,
                                    (
                                        completed_at_s - attempt.started_at_s
                                    )
                                    * 1_000.0,
                                ),
                            ),
                            "usage": (
                                None
                                if response is None or response.usage is None
                                else dict(response.usage)
                            ),
                            "response_value": (
                                _thaw(raw_response)
                                if attempt.store_full_response
                                and raw_response is not None
                                else None
                            ),
                        },
                        field="Program model-attempt terminal record",
                        maximum_bytes=_MAX_MODEL_AUDIT_CANDIDATE_BYTES,
                    )
                )
            record = attempt.pending_terminal_record
            if self._audit_sink is not None:
                try:
                    self._audit_sink.commit(record)
                except Exception as exc:
                    raise _TaskFailure(
                        "model_audit_commit_failed",
                        str(exc) or type(exc).__name__,
                    ) from exc
            attempt.terminal_outcome = record["outcome"]
            return True

    def _commit_active_model_attempt(
        self,
        task: _Task,
        *,
        outcome: str,
        error_code: str,
        error_message: str,
    ) -> bool | None:
        with task.audit_lock:
            active = [
                ordinal
                for ordinal, attempt in task.external_attempts.items()
                if attempt.terminal_outcome is None
            ]
            has_attempts = bool(task.external_attempts)
        if active:
            ordinal = max(active)
            with task.audit_lock:
                pending = task.external_attempts[ordinal].pending_terminal_record
            return self._commit_model_attempt(
                task,
                ordinal,
                outcome=(outcome if pending is None else pending["outcome"]),
                error_code=(
                    error_code if pending is None else pending.get("error_code")
                ),
                error_message=(
                    error_message
                    if pending is None
                    else pending.get("error_message", "")
                ),
            )
        return False if has_attempts else None

    def _query_model(
        self,
        context: _BoundContext,
        task: _Task,
        started_at_s: float,
        deadline_s: float,
    ) -> _TaskResult:
        inputs = task.inputs
        cancel_event = task.cancel_event
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
        attachment = self._attachment(
            context,
            inputs,
            deadline_s=deadline_s,
            cancel_event=cancel_event,
        )
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
            ordinal = attempt + 1
            self._begin_model_attempt(
                task,
                ordinal=ordinal,
                context=context,
                endpoint_alias=endpoint_alias,
                endpoint=endpoint,
                template_id=template_id,
                template=template,
                prompt=prompt_attempt,
                raw_variables=raw_variables,
                attachment=attachment,
            )
            try:
                response = self._model_transport.complete(request, cancel_event)
            except Exception as exc:
                if cancel_event.is_set():
                    self._commit_model_attempt(
                        task,
                        ordinal,
                        outcome="cancelled",
                        error_code="model_request_cancelled",
                        error_message="model request was cancelled",
                    )
                    raise _TaskFailure(
                        "model_request_cancelled", "model request was cancelled"
                    ) from exc
                timed_out = self._clock() >= deadline_s or "timed out" in str(
                    exc
                ).lower()
                self._commit_model_attempt(
                    task,
                    ordinal,
                    outcome="timeout" if timed_out else "transport_error",
                    error_code=(
                        "model_request_timeout"
                        if timed_out
                        else "model_request_failed"
                    ),
                    error_message=str(exc),
                )
                raise _TaskFailure(
                    "model_request_timeout" if timed_out else "model_request_failed",
                    str(exc),
                ) from exc
            if not isinstance(response, ProgramModelResponse):
                self._commit_model_attempt(
                    task,
                    ordinal,
                    outcome="invalid_response",
                    error_code="model_transport_response_invalid",
                    error_message="model transport returned an invalid response type",
                )
                raise _TaskFailure(
                    "model_transport_response_invalid",
                    "model transport returned an invalid response type",
                )
            raw_response = (
                response.tool_arguments
                if response_mode == "tool_call"
                else response.text
            )
            if isinstance(raw_response, Mapping):
                raw_response = _thaw(raw_response)
            if cancel_event.is_set():
                self._commit_model_attempt(
                    task,
                    ordinal,
                    outcome="cancelled",
                    error_code="model_request_cancelled",
                    error_message="model request was cancelled",
                    response=response,
                    raw_response=raw_response,
                )
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
                self._commit_model_attempt(
                    task,
                    ordinal,
                    outcome="parse_error",
                    error_code=exc.reason_code,
                    error_message=str(exc),
                    response=response,
                    raw_response=raw_response,
                )
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
            _validate_program_model_outputs(
                parsed,
                declaration,
                context.capture.program,
            )
        except (ModelOutputValidationError, ProgramAssistantPortError) as exc:
            reason = (
                "model_confidence_below_threshold"
                if "below minimum" in str(exc)
                else "model_response_schema_invalid"
            )
            self._commit_model_attempt(
                task,
                attempt + 1,
                outcome=(
                    "confidence_below_threshold"
                    if reason == "model_confidence_below_threshold"
                    else "schema_error"
                ),
                error_code=reason,
                error_message=str(exc),
                response=response,
                raw_response=raw_response,
            )
            raise _TaskFailure(reason, str(exc)) from exc
        completed_at = self._clock()
        if completed_at > deadline_s:
            self._commit_model_attempt(
                task,
                attempt + 1,
                outcome="timeout",
                error_code="model_request_timeout",
                error_message="model request timed out",
                response=response,
                raw_response=raw_response,
            )
            raise _TaskFailure("model_request_timeout", "model request timed out")
        self._commit_model_attempt(
            task,
            attempt + 1,
            outcome="success",
            response=response,
            raw_response=raw_response,
            validated_output=parsed,
        )
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
            if attachment.get("kind") == "snapshot_image":
                metadata["snapshot_source"] = _thaw(attachment["source"])
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
                try:
                    audit_state = self._commit_active_model_attempt(
                        task,
                        outcome="timeout",
                        error_code="model_request_timeout",
                        error_message="model request timed out",
                    )
                except _TaskFailure as exc:
                    task.cancel_event.set()
                    task.future.cancel()
                    task.terminal = self._failure_snapshot(
                        task_id,
                        exc.reason_code,
                        str(exc),
                        task=task,
                    )
                else:
                    if audit_state is False:
                        # The external attempt already reached its durable
                        # terminal outcome before this observation raced its
                        # worker return. Let that exact result settle.
                        return OwnerTaskSnapshot(task_id, OwnerTaskState.ACTIVE)
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
                task,
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
            try:
                self._commit_active_model_attempt(
                    task,
                    outcome="cancelled",
                    error_code="model_request_cancelled",
                    error_message="model request was cancelled",
                )
            except _TaskFailure as exc:
                task.terminal = self._failure_snapshot(
                    task_id,
                    exc.reason_code,
                    str(exc),
                    task=task,
                )
            else:
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
            if self._closed and not self._tasks:
                return
            self._closed = True
            program_run_ids = tuple(
                sorted(
                    {
                        *self._contexts,
                        *(task.identity.program_run_id for task in self._tasks.values()),
                    }
                )
            )
        errors: list[Exception] = []
        for program_run_id in program_run_ids:
            try:
                self.unbind(program_run_id)
            except Exception as error:
                errors.append(error)
        self._executor.shutdown(wait=False, cancel_futures=True)
        if errors:
            detail = "; ".join(
                str(error) or type(error).__name__ for error in errors
            )
            raise ProgramAssistantPortError(
                f"assistant port cleanup was incomplete: {detail}"
            ) from errors[0]


__all__ = [
    "HeadlessProgramSceneSource",
    "ProgramAssistantPort",
    "ProgramAssistantPortError",
    "ProgramModelRequest",
    "ProgramModelResponse",
    "ProgramModelAuditSink",
    "ProgramModelTransport",
    "ProgramModelTransportError",
    "ProgramSnapshotSource",
    "ProgramSnapshotSourceError",
    "validate_program_snapshot_attachment",
]
