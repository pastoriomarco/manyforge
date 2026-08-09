"""Independent verification of one active retained assistant-mode projection."""

from __future__ import annotations

from dataclasses import dataclass
import math
import unicodedata
from typing import TYPE_CHECKING, Any

from manyforge_behavior.canonical_artifact import (
    CONTENT_IDENTITY_PREFIX,
    canonical_json_bytes,
    raw_sha256_digest,
)

from .node_environment_client import VerifiedActiveNodeEnvironment

if TYPE_CHECKING:
    from .state import ComposerState


_PROJECTION_VERSION = "manyforge.assistant_mode_projection.v1"
_CATALOG_VERSION = "manyforge.assistant_mode_catalog.v1"
_PROJECTION_FIELDS = {
    "assistant_projection_digest",
    "catalog_hash",
    "description",
    "grant_policy",
    "graph_revision",
    "instrument_registry_digest",
    "mode",
    "model_endpoint",
    "node_environment",
    "nodes",
    "provider",
    "retained_bundle_id",
    "schema_version",
    "skills",
    "source",
    "tools",
}
_SOURCE_FIELDS = {
    "available",
    "freshness",
    "known_gaps",
    "projection_generation",
    "projection_owner_epoch",
    "receiver_age_ns",
    "selection_projection_generation",
    "selection_source_content_digest",
    "selection_source_generation",
    "selection_source_owner_epoch",
    "selection_source_owner_id",
    "source_content_digest",
    "source_generation",
    "source_owner_epoch",
    "source_owner_id",
    "source_owner_kind",
}


class ActiveAssistantModeProjectionError(RuntimeError):
    """The selected assistant mode is unavailable or not independently valid."""

    def __init__(self, code: str, message: str, status_code: int = 503) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


@dataclass(frozen=True, slots=True)
class VerifiedActiveAssistantMode:
    payload: dict[str, Any]
    tools: dict[str, dict[str, Any]]


def _invalid(message: str) -> ActiveAssistantModeProjectionError:
    return ActiveAssistantModeProjectionError(
        "assistant_mode_projection_invalid",
        message,
    )


def _object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict or any(type(key) is not str for key in value):
        raise _invalid(f"{label} must be a built-in object")
    return value


def _closed(value: dict[str, Any], fields: set[str], label: str) -> None:
    if set(value) != fields:
        raise _invalid(f"{label} has an unsupported shape")


def _text(value: Any, label: str, *, maximum_bytes: int = 4096) -> str:
    if (
        type(value) is not str
        or not value
        or value != value.strip()
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        raise _invalid(f"{label} must be bounded non-empty NFC text")
    return value


def _digest(value: Any, label: str) -> str:
    result = _text(value, label, maximum_bytes=71)
    if (
        len(result) != 71
        or not result.startswith("sha256:")
        or any(character not in "0123456789abcdef" for character in result[7:])
    ):
        raise _invalid(f"{label} must be an exact SHA-256 digest")
    return result


def _array(value: Any, label: str, *, maximum: int = 4096) -> list[Any]:
    if type(value) is not list or len(value) > maximum:
        raise _invalid(f"{label} must be a bounded array")
    return value


def _framed_digest(domain: str, api_version: str, value: dict[str, Any]) -> str:
    preimage = b"\0".join(
        (
            CONTENT_IDENTITY_PREFIX,
            domain.encode("ascii"),
            api_version.encode("ascii"),
            canonical_json_bytes(value),
        )
    )
    return raw_sha256_digest(preimage)


def _source(value: Any, node_environment: VerifiedActiveNodeEnvironment) -> dict:
    source = _object(value, "assistant mode source")
    _closed(source, _SOURCE_FIELDS, "assistant mode source")
    if source["available"] is not True or source["freshness"] != "fresh":
        raise ActiveAssistantModeProjectionError(
            "assistant_mode_projection_unavailable",
            "the active assistant mode projection is unavailable or stale",
        )
    if source["known_gaps"] != []:
        raise ActiveAssistantModeProjectionError(
            "assistant_mode_projection_unavailable",
            "the active assistant mode projection is incomplete",
        )
    for field in (
        "projection_owner_epoch",
        "selection_source_owner_epoch",
        "selection_source_owner_id",
        "source_owner_epoch",
        "source_owner_id",
        "source_owner_kind",
    ):
        _text(source[field], f"source.{field}")
    if source["source_owner_kind"] != "assistant_mode_projection":
        raise _invalid("source.source_owner_kind is not assistant_mode_projection")
    for field in (
        "selection_source_content_digest",
        "source_content_digest",
    ):
        _digest(source[field], f"source.{field}")
    for field in (
        "projection_generation",
        "receiver_age_ns",
        "selection_projection_generation",
        "selection_source_generation",
        "source_generation",
    ):
        if type(source[field]) is not int or source[field] < 0:
            raise _invalid(f"source.{field} must be a non-negative integer")
    node_source = node_environment.payload["source"]
    for field in (
        "selection_projection_generation",
        "selection_source_content_digest",
        "selection_source_generation",
        "selection_source_owner_epoch",
        "selection_source_owner_id",
    ):
        if source[field] != node_source.get(field):
            raise _invalid("assistant mode and node environment selections disagree")
    return source


def _grant_policy(value: Any, mode: str) -> dict[str, Any]:
    policy = _object(value, "grant_policy")
    _closed(
        policy,
        {"approval_mode", "review_policy", "session_kind"},
        "grant_policy",
    )
    review = _text(policy["review_policy"], "grant_policy.review_policy")
    for field in ("approval_mode", "session_kind"):
        if policy[field] is not None:
            _text(policy[field], f"grant_policy.{field}")
    if mode == "composer-assistant" and policy != {
        "approval_mode": None,
        "review_policy": "required",
        "session_kind": None,
    }:
        raise _invalid("composer-assistant grant/review policy is not exact")
    if review not in {"required", "bounded_by_recovery_profile", "not_required"}:
        raise _invalid("grant_policy.review_policy is unsupported")
    return policy


def _tools(value: Any, mode: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    previous = ""
    for index, raw in enumerate(_array(value, "tools")):
        path = f"tools[{index}]"
        tool = _object(raw, path)
        _closed(
            tool,
            {"description", "effect", "input_schema", "input_schema_hash", "tool_id"},
            path,
        )
        tool_id = _text(tool["tool_id"], f"{path}.tool_id", maximum_bytes=256)
        if tool_id <= previous:
            raise _invalid("tools must be unique and canonical-sorted")
        previous = tool_id
        _text(tool["description"], f"{path}.description", maximum_bytes=16_384)
        effect = _text(tool["effect"], f"{path}.effect")
        if effect not in {
            "read_only",
            "proposal",
            "composer_draft_mutating",
            "runtime_mutating",
        }:
            raise _invalid(f"{path}.effect is unsupported")
        if mode == "composer-assistant" and effect == "runtime_mutating":
            raise _invalid("composer-assistant exposes a runtime-mutating tool")
        schema = _object(tool["input_schema"], f"{path}.input_schema")
        declared = _digest(tool["input_schema_hash"], f"{path}.input_schema_hash")
        if declared != raw_sha256_digest(canonical_json_bytes(schema)):
            raise _invalid(f"{path}.input_schema_hash does not match its schema")
        result[tool_id] = tool
    return result


def _nodes(value: Any, node_environment: VerifiedActiveNodeEnvironment) -> list[dict]:
    result: list[dict] = []
    previous = ""
    catalog_ids = {entry.id for entry in node_environment.catalog.entries}
    for index, raw in enumerate(_array(value, "nodes")):
        row = _object(raw, f"nodes[{index}]")
        _closed(row, {"node_id"}, f"nodes[{index}]")
        node_id = _text(row["node_id"], f"nodes[{index}].node_id")
        if node_id <= previous or node_id not in catalog_ids:
            raise _invalid("nodes are unsorted, duplicated, or absent from the catalog")
        previous = node_id
        result.append(row)
    return result


def _skills(value: Any, tools: dict[str, dict]) -> list[dict]:
    result: list[dict] = []
    previous = ""
    for index, raw in enumerate(_array(value, "skills")):
        path = f"skills[{index}]"
        row = _object(raw, path)
        _closed(row, {"requires_tools", "skill_id"}, path)
        skill_id = _text(row["skill_id"], f"{path}.skill_id")
        if skill_id <= previous:
            raise _invalid("skills must be unique and canonical-sorted")
        previous = skill_id
        requirements = [
            _text(item, f"{path}.requires_tools[{offset}]")
            for offset, item in enumerate(_array(row["requires_tools"], f"{path}.requires_tools"))
        ]
        if requirements != sorted(set(requirements)) or not set(requirements) <= set(tools):
            raise _invalid(f"{path}.requires_tools is not closed over selected tools")
        result.append(row)
    return result


def _model_request_contract(value: Any) -> dict[str, Any]:
    contract = _object(value, "model_endpoint.request_contract")
    _closed(
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
        "model_endpoint.request_contract",
    )
    timeout = contract["default_timeout_s"]
    if (
        type(timeout) not in {int, float}
        or not math.isfinite(timeout)
        or not 0 < timeout <= 3_600
    ):
        raise _invalid("model timeout is outside its bound")
    tokens = contract["max_output_tokens"]
    if type(tokens) is not int or not 1 <= tokens <= 1_048_576:
        raise _invalid("model output-token limit is invalid")
    for field in (
        "supports_image_input",
        "supports_json_object",
        "supports_response_tools",
    ):
        if type(contract[field]) is not bool:
            raise _invalid(f"model {field} must be boolean")
    transports = [
        _text(item, "model image transport", maximum_bytes=64)
        for item in _array(
            contract["supported_image_transports"], "model image transports"
        )
    ]
    if (
        transports != sorted(set(transports))
        or not set(transports) <= {"http_url", "inline_data_url"}
        or bool(transports) != contract["supports_image_input"]
    ):
        raise _invalid("model image transports are inconsistent")
    if contract["tool_call_family"] not in {
        "openai_chat_tools",
        "qwen3_coder",
        "qwen3_xml",
    }:
        raise _invalid("model tool-call family is unsupported")
    return contract


def verify_active_assistant_mode(
    payload: Any,
    *,
    expected_mode: str,
    node_environment: VerifiedActiveNodeEnvironment,
) -> VerifiedActiveAssistantMode:
    projection = _object(payload, "assistant mode projection")
    _closed(projection, _PROJECTION_FIELDS, "assistant mode projection")
    if projection["schema_version"] != _PROJECTION_VERSION:
        raise _invalid("assistant mode projection version is unsupported")
    mode = _text(projection["mode"], "mode")
    if mode != expected_mode:
        raise _invalid("assistant mode projection selected a different mode")
    source = _source(projection["source"], node_environment)
    for field in (
        "assistant_projection_digest",
        "catalog_hash",
        "instrument_registry_digest",
    ):
        _digest(projection[field], field)
    _text(projection["description"], "description", maximum_bytes=16_384)
    _text(projection["graph_revision"], "graph_revision")
    _text(projection["retained_bundle_id"], "retained_bundle_id")
    if (
        projection["retained_bundle_id"]
        != node_environment.payload["retained_bundle_id"]
        or projection["graph_revision"] != node_environment.payload["graph_revision"]
    ):
        raise _invalid("assistant mode and node environment select different bundles")

    environment = _object(projection["node_environment"], "node_environment")
    _closed(
        environment,
        {"catalog_digest", "catalog_id", "content_version", "environment_digest"},
        "node_environment",
    )
    for field in environment:
        expected = node_environment.payload[field]
        if environment[field] != expected:
            raise _invalid("assistant mode node environment identities disagree")

    provider = _object(projection["provider"], "provider")
    _closed(provider, {"provider_alias", "provider_kind"}, "provider")
    _text(provider["provider_alias"], "provider.provider_alias")
    _text(provider["provider_kind"], "provider.provider_kind")
    model = _object(projection["model_endpoint"], "model_endpoint")
    _closed(
        model,
        {"model_endpoint_alias", "model_id", "request_contract"},
        "model_endpoint",
    )
    _text(model["model_endpoint_alias"], "model_endpoint.model_endpoint_alias")
    _text(model["model_id"], "model_endpoint.model_id")
    request_contract = _model_request_contract(model["request_contract"])
    if request_contract["tool_call_family"] != "openai_chat_tools":
        raise _invalid("direct bridge requires the openai_chat_tools parser family")

    policy = _grant_policy(projection["grant_policy"], mode)
    tools = _tools(projection["tools"], mode)
    if tools and request_contract["supports_response_tools"] is not True:
        raise _invalid("selected model endpoint does not support response tools")
    nodes = _nodes(projection["nodes"], node_environment)
    skills = _skills(projection["skills"], tools)
    catalog = {
        "grant_policy": policy,
        "mode": mode,
        "nodes": nodes,
        "skills": skills,
        "tools": list(tools.values()),
    }
    expected_catalog_hash = _framed_digest(
        "assistant-mode-catalog", _CATALOG_VERSION, catalog
    )
    if projection["catalog_hash"] != expected_catalog_hash:
        raise _invalid("catalog_hash does not match the exact selected mode")

    semantic = {key: value for key, value in projection.items() if key != "source"}
    expected_source_digest = _framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        _PROJECTION_VERSION,
        semantic,
    )
    if source["source_content_digest"] != expected_source_digest:
        raise _invalid("assistant mode source digest does not match the projection")
    return VerifiedActiveAssistantMode(payload=projection, tools=tools)


def read_active_assistant_mode(
    state: "ComposerState",
    *,
    mode: str,
    node_environment: VerifiedActiveNodeEnvironment,
) -> VerifiedActiveAssistantMode:
    client = state.headless_client
    if client is None:
        raise _invalid("Composer has no configured headless client")
    try:
        payload = client.assistant_mode(mode)
    except Exception as error:
        code = getattr(error, "code", None)
        if isinstance(code, str) and code:
            status_code = getattr(error, "status_code", None)
            raise ActiveAssistantModeProjectionError(
                code,
                str(error),
                status_code if isinstance(status_code, int) else 503,
            ) from error
        raise
    return verify_active_assistant_mode(
        payload,
        expected_mode=mode,
        node_environment=node_environment,
    )


__all__ = [
    "ActiveAssistantModeProjectionError",
    "VerifiedActiveAssistantMode",
    "read_active_assistant_mode",
    "verify_active_assistant_mode",
]
