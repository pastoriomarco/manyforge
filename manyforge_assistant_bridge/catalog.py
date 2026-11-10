"""Independent assistant-mode verification and narrowing for the bridge."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import math
import re
from typing import Any, Final, Mapping, Sequence
import unicodedata

import rfc8785


MODE_PROJECTION_VERSION: Final = "manyforge.assistant_mode_projection.v2"
MODE_CATALOG_VERSION: Final = "manyforge.assistant_mode_catalog.v1"
CONTENT_IDENTITY_PREFIX: Final = b"manyforge.content-identity.v1"
MAXIMUM_ROWS: Final = 4_096
MAXIMUM_TEXT_BYTES: Final = 16_384
TOOL_ID_PATTERN: Final = re.compile(r"^[a-z][a-z0-9_]*$")
ENV_NAME_PATTERN: Final = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,127}$")
ORIENTATION_FIELDS: Final = frozenset(
    {
        "xyzw",
        "rpy_deg",
        "rotate_local_rpy_deg",
        "rotate_world_rpy_deg",
        "rpy_delta_deg",
        "orientation_from",
    }
)
_PROJECTION_FIELDS: Final = frozenset(
    {
        "assistant_component_id",
        "assistant_projection_digest",
        "catalog_hash",
        "compact_history",
        "constrained_regeneration",
        "description",
        "grant_policy",
        "graph_revision",
        "instrument_registry_digest",
        "mode",
        "model_endpoint",
        "node_environment",
        "nodes",
        "retained_bundle_id",
        "runs_in",
        "schema_version",
        "skills",
        "source",
        "stuck_loop_recovery",
        "tools",
        "types",
    }
)
_NODE_FIELDS: Final = frozenset(
    {
        "children",
        "node_id",
        "node_schema",
        "replacement_schema",
        "summary",
        "update_schema",
    }
)
_SOURCE_FIELDS: Final = frozenset(
    {
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
)


class AssistantCatalogError(ValueError):
    """An assistant projection or narrowing request is invalid or stale."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class ResolvedAssistantCatalog:
    catalog_hash: str
    grant_policy: dict[str, Any]
    nodes: list[dict[str, Any]]
    skills: list[dict[str, Any]]
    tools: list[dict[str, Any]]

    def to_wire(self) -> dict[str, Any]:
        return {
            "catalogHash": self.catalog_hash,
            "grantPolicy": deepcopy(self.grant_policy),
            "nodes": deepcopy(self.nodes),
            "skills": deepcopy(self.skills),
            "tools": deepcopy(self.tools),
        }


def _fail(code: str, message: str) -> None:
    raise AssistantCatalogError(code, message)


def _object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict or any(type(key) is not str for key in value):
        _fail("assistant_projection_invalid", f"{label} must be a built-in object")
    return value


def _closed(value: dict[str, Any], fields: set[str] | frozenset[str], label: str) -> None:
    if set(value) != set(fields):
        _fail("assistant_projection_invalid", f"{label} has an unsupported shape")


def _text(value: Any, label: str, *, maximum_bytes: int = MAXIMUM_TEXT_BYTES) -> str:
    if (
        type(value) is not str
        or not value
        or value != value.strip()
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        _fail("assistant_projection_invalid", f"{label} must be bounded NFC text")
    return value


def _digest(value: Any, label: str) -> str:
    result = _text(value, label, maximum_bytes=71)
    if (
        len(result) != 71
        or not result.startswith("sha256:")
        or any(character not in "0123456789abcdef" for character in result[7:])
    ):
        _fail("assistant_projection_invalid", f"{label} must be an exact SHA-256 digest")
    return result


def _array(value: Any, label: str) -> list[Any]:
    if type(value) is not list or len(value) > MAXIMUM_ROWS:
        _fail("assistant_projection_invalid", f"{label} must be a bounded array")
    return value


def canonical_json(value: Any) -> bytes:
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as error:
        _fail("assistant_projection_invalid", f"value is not canonical JSON: {error}")


def raw_digest(value: Any) -> str:
    return f"sha256:{hashlib.sha256(canonical_json(value)).hexdigest()}"


def framed_digest(domain: str, version: str, value: Any) -> str:
    content = b"\0".join(
        (
            CONTENT_IDENTITY_PREFIX,
            domain.encode("ascii"),
            version.encode("ascii"),
            canonical_json(value),
        )
    )
    return f"sha256:{hashlib.sha256(content).hexdigest()}"


def _validate_projection_rows(projection: dict[str, Any]) -> None:
    previous = ""
    tools = _array(projection["tools"], "tools")
    for index, raw in enumerate(tools):
        path = f"tools[{index}]"
        tool = _object(raw, path)
        _closed(
            tool,
            {
                "argument_types",
                "description",
                "effect",
                "input_schema",
                "input_schema_hash",
                "tool_id",
            },
            path,
        )
        argument_types = _object(tool["argument_types"], f"{path}.argument_types")
        types = _object(projection["types"], "types")
        for name, type_name in argument_types.items():
            if name not in tool["input_schema"].get("properties", {}) or (
                type(type_name) is not str or type_name.removesuffix("[]") not in types
            ):
                _fail("assistant_projection_invalid", f"{path}.argument_types is not closed")
        tool_id = _text(tool["tool_id"], f"{path}.tool_id", maximum_bytes=256)
        if TOOL_ID_PATTERN.fullmatch(tool_id) is None or tool_id <= previous:
            _fail(
                "assistant_projection_invalid",
                "tool IDs must be canonical underscore IDs in strict sorted order",
            )
        previous = tool_id
        _text(tool["description"], f"{path}.description")
        if tool["effect"] not in {"read_only", "composer_draft_mutating"}:
            _fail("assistant_projection_invalid", f"{path}.effect is unsupported")
        schema = _object(tool["input_schema"], f"{path}.input_schema")
        if _digest(tool["input_schema_hash"], f"{path}.input_schema_hash") != raw_digest(schema):
            _fail("assistant_projection_invalid", f"{path} schema hash disagrees")

    previous = ""
    node_ids: set[str] = set()
    for index, raw in enumerate(_array(projection["nodes"], "nodes")):
        path = f"nodes[{index}]"
        row = _object(raw, path)
        _closed(row, _NODE_FIELDS, path)
        node_id = _text(row["node_id"], f"{path}.node_id", maximum_bytes=256)
        if node_id <= previous:
            _fail("assistant_projection_invalid", "nodes must be strictly sorted")
        previous = node_id
        node_ids.add(node_id)
        _text(row["summary"], f"{path}.summary")
        children = _object(row["children"], f"{path}.children")
        _closed(children, {"max", "min"}, f"{path}.children")
        if any(type(children[field]) is not int for field in ("max", "min")):
            _fail("assistant_projection_invalid", f"{path}.children bounds are invalid")
        for field in ("node_schema", "replacement_schema", "update_schema"):
            _object(row[field], f"{path}.{field}")

    previous = ""
    tool_ids = {row["tool_id"] for row in tools}
    for index, raw in enumerate(_array(projection["skills"], "skills")):
        path = f"skills[{index}]"
        row = _object(raw, path)
        _closed(row, {"requires_tools", "skill_id"}, path)
        skill_id = _text(row["skill_id"], f"{path}.skill_id", maximum_bytes=256)
        if skill_id <= previous:
            _fail("assistant_projection_invalid", "skills must be strictly sorted")
        previous = skill_id
        requirements = [
            _text(item, f"{path}.requires_tools", maximum_bytes=256)
            for item in _array(row["requires_tools"], f"{path}.requires_tools")
        ]
        if requirements != sorted(set(requirements)) or not set(requirements) <= tool_ids:
            _fail("assistant_projection_invalid", f"{path} is not closed over tools")


def _validate_model_request_contract(value: Any) -> dict[str, Any]:
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
        }
        # EXPERIMENTAL optional `reasoning: on|off`; optional `sampling`.
        | ({"reasoning", "sampling"} & set(contract)),
        "model_endpoint.request_contract",
    )
    if "sampling" in contract:
        sampling = _object(contract["sampling"], "model sampling")
        if not sampling or not set(sampling) <= {"temperature", "top_k", "top_p"}:
            _fail("assistant_projection_invalid", "model sampling has an unsupported shape")
        if "temperature" in sampling and not (
            type(sampling["temperature"]) in {int, float}
            and math.isfinite(sampling["temperature"])
            and 0 <= sampling["temperature"] <= 2
        ):
            _fail("assistant_projection_invalid", "model sampling temperature is outside its bound")
        if "top_p" in sampling and not (
            type(sampling["top_p"]) in {int, float}
            and math.isfinite(sampling["top_p"])
            and 0 < sampling["top_p"] <= 1
        ):
            _fail("assistant_projection_invalid", "model sampling top_p is outside its bound")
        if "top_k" in sampling and not (
            type(sampling["top_k"]) is int and 1 <= sampling["top_k"] <= 1_048_576
        ):
            _fail("assistant_projection_invalid", "model sampling top_k is outside its bound")
    if contract.get("reasoning", "on") not in {"on", "off"}:
        _fail("assistant_projection_invalid", "model reasoning must be on or off")
    timeout = contract["default_timeout_s"]
    if (
        type(timeout) not in {int, float}
        or not math.isfinite(timeout)
        or not 0 < timeout <= 3_600
    ):
        _fail("assistant_projection_invalid", "model timeout is outside its bound")
    tokens = contract["max_output_tokens"]
    if type(tokens) is not int or not 1 <= tokens <= 1_048_576:
        _fail("assistant_projection_invalid", "model output-token limit is invalid")
    for field in (
        "supports_image_input",
        "supports_json_object",
        "supports_response_tools",
    ):
        if type(contract[field]) is not bool:
            _fail("assistant_projection_invalid", f"model {field} must be boolean")
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
        _fail("assistant_projection_invalid", "model image transports are inconsistent")
    if contract["tool_call_family"] not in {
        "openai_chat_tools",
        "qwen3_coder",
        "qwen3_xml",
    }:
        _fail("assistant_projection_invalid", "model tool-call family is unsupported")
    return contract


def verify_active_mode_projection(payload: Any, *, expected_mode: str) -> dict[str, Any]:
    """Verify one fresh closed projection and both of its content identities."""

    projection = _object(payload, "assistant mode projection")
    _closed(projection, _PROJECTION_FIELDS, "assistant mode projection")
    if projection["schema_version"] != MODE_PROJECTION_VERSION:
        _fail("assistant_projection_invalid", "assistant mode projection version is unsupported")
    if _text(projection["mode"], "mode", maximum_bytes=256) != expected_mode:
        _fail("assistant_projection_stale", "assistant mode selection changed")
    for field in (
        "assistant_projection_digest",
        "catalog_hash",
        "instrument_registry_digest",
    ):
        _digest(projection[field], field)
    for field in ("description", "graph_revision", "retained_bundle_id"):
        _text(projection[field], field)

    source = _object(projection["source"], "source")
    _closed(source, _SOURCE_FIELDS, "source")
    if (
        source["available"] is not True
        or source["freshness"] != "fresh"
        or source["known_gaps"] != []
    ):
        _fail("assistant_projection_unavailable", "assistant mode projection is not fresh and complete")
    if source["source_owner_kind"] != "assistant_mode_projection":
        _fail("assistant_projection_invalid", "assistant mode projection owner kind is invalid")
    for field in (
        "projection_owner_epoch",
        "selection_source_owner_epoch",
        "selection_source_owner_id",
        "source_owner_epoch",
        "source_owner_id",
    ):
        _text(source[field], f"source.{field}")
    for field in ("selection_source_content_digest", "source_content_digest"):
        _digest(source[field], f"source.{field}")
    for field in (
        "projection_generation",
        "receiver_age_ns",
        "selection_projection_generation",
        "selection_source_generation",
        "source_generation",
    ):
        if type(source[field]) is not int or source[field] < 0:
            _fail("assistant_projection_invalid", f"source.{field} must be non-negative")

    _text(projection["assistant_component_id"], "assistant_component_id", maximum_bytes=256)
    runs_in = _array(projection["runs_in"], "runs_in")
    if not runs_in:
        _fail("assistant_projection_invalid", "runs_in must name at least one cell state")
    for index, state in enumerate(runs_in):
        _text(state, f"runs_in[{index}]", maximum_bytes=256)
    model = _object(projection["model_endpoint"], "model_endpoint")
    _closed(
        model,
        {
            "ai_model_component_id",
            "api_key_env",
            "base_url",
            "model_endpoint_alias",
            "model_id",
            "request_contract",
        },
        "model_endpoint",
    )
    _text(model["model_endpoint_alias"], "model_endpoint.model_endpoint_alias", maximum_bytes=256)
    _text(model["model_id"], "model_endpoint.model_id", maximum_bytes=1024)
    _text(model["ai_model_component_id"], "model_endpoint.ai_model_component_id", maximum_bytes=256)
    base_url = _text(model["base_url"], "model_endpoint.base_url", maximum_bytes=4_096)
    if not base_url.startswith(("http://", "https://")):
        _fail("assistant_projection_invalid", "model_endpoint.base_url must be an http(s) URL")
    if model["api_key_env"] is not None and (
        type(model["api_key_env"]) is not str
        or ENV_NAME_PATTERN.fullmatch(model["api_key_env"]) is None
    ):
        _fail("assistant_projection_invalid", "model_endpoint.api_key_env must be an env var name")
    request_contract = _validate_model_request_contract(model["request_contract"])
    if request_contract["tool_call_family"] != "openai_chat_tools":
        _fail(
            "assistant_provider_unavailable",
            "the bridge requires the openai_chat_tools parser family",
        )
    if projection["tools"] and request_contract["supports_response_tools"] is not True:
        _fail(
            "assistant_provider_unavailable",
            "selected model endpoint does not support response tools",
        )
    environment = _object(projection["node_environment"], "node_environment")
    _closed(
        environment,
        {"catalog_digest", "catalog_id", "content_version", "environment_digest"},
        "node_environment",
    )
    _digest(environment["catalog_digest"], "node_environment.catalog_digest")
    _digest(environment["environment_digest"], "node_environment.environment_digest")
    _text(environment["catalog_id"], "node_environment.catalog_id")
    _text(environment["content_version"], "node_environment.content_version")
    policy = _object(projection["grant_policy"], "grant_policy")
    _closed(policy, {"approval_mode", "review_policy", "session_kind"}, "grant_policy")
    if policy["review_policy"] not in {
        "required",
        "bounded_by_recovery_profile",
        "not_required",
    }:
        _fail("assistant_projection_invalid", "grant policy review mode is unsupported")
    for field in ("approval_mode", "session_kind"):
        if policy[field] is not None:
            _text(policy[field], f"grant_policy.{field}")

    for flag in ("stuck_loop_recovery", "compact_history", "constrained_regeneration"):
        if type(projection[flag]) is not bool:
            _fail("assistant_projection_invalid", f"{flag} must be boolean")
    _validate_projection_rows(projection)
    catalog = {
        "grant_policy": policy,
        "mode": projection["mode"],
        "nodes": projection["nodes"],
        "skills": projection["skills"],
        "tools": projection["tools"],
    }
    if projection["catalog_hash"] != framed_digest(
        "assistant-mode-catalog", MODE_CATALOG_VERSION, catalog
    ):
        _fail("assistant_projection_invalid", "catalog hash does not match the projection")
    semantic = {name: value for name, value in projection.items() if name != "source"}
    if source["source_content_digest"] != framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        MODE_PROJECTION_VERSION,
        semantic,
    ):
        _fail("assistant_projection_invalid", "projection source digest does not match")
    return deepcopy(projection)


def normalize_requested_tools(value: Sequence[str] | None) -> tuple[str, ...]:
    if value is None:
        return ()
    if type(value) not in {list, tuple} or len(value) > MAXIMUM_ROWS:
        _fail("assistant_request_invalid", "requestedTools must be a bounded array")
    result: list[str] = []
    for index, item in enumerate(value):
        tool_id = _text(item, f"requestedTools[{index}]", maximum_bytes=256)
        if TOOL_ID_PATTERN.fullmatch(tool_id) is None:
            _fail("assistant_request_invalid", "requestedTools contains a noncanonical tool ID")
        result.append(tool_id)
    if result != sorted(set(result)):
        _fail("assistant_request_invalid", "requestedTools must be sorted and unique")
    return tuple(result)


def normalize_field_policy(value: Mapping[str, Any] | None) -> dict[str, Any]:
    if value is None or value == {}:
        return {}
    policy = _object(value, "fieldPolicy")
    _closed(policy, {"allowedOrientationFields"}, "fieldPolicy")
    raw = policy["allowedOrientationFields"]
    if type(raw) is not list or not raw or len(raw) > len(ORIENTATION_FIELDS):
        _fail("assistant_request_invalid", "allowedOrientationFields must be a bounded non-empty array")
    fields = [
        _text(item, f"allowedOrientationFields[{index}]", maximum_bytes=64)
        for index, item in enumerate(raw)
    ]
    if fields != sorted(set(fields)) or not set(fields) <= ORIENTATION_FIELDS:
        _fail("assistant_request_invalid", "allowedOrientationFields is not canonical or supported")
    return {"allowedOrientationFields": fields}


def _narrow_schema(schema: dict[str, Any], allowed: set[str]) -> dict[str, Any]:
    disallowed = set(ORIENTATION_FIELDS) - allowed
    if not ({"xyzw", "rpy_deg"} & allowed):
        disallowed.add("orientation")

    def impossible_branch(value: Any) -> bool:
        if type(value) is not dict:
            return False
        required = value.get("required")
        return type(required) is list and any(item in disallowed for item in required)

    def narrow(value: Any) -> Any:
        if type(value) is list:
            return [narrow(item) for item in value if not impossible_branch(item)]
        if type(value) is not dict:
            return deepcopy(value)
        result: dict[str, Any] = {}
        for key, item in value.items():
            if key == "properties" and type(item) is dict:
                result[key] = {
                    name: narrow(child)
                    for name, child in item.items()
                    if name not in disallowed
                }
            elif key == "required" and type(item) is list:
                result[key] = [name for name in item if name not in disallowed]
            else:
                result[key] = narrow(item)
        return result

    return narrow(schema)


def resolve_catalog(
    projection: Mapping[str, Any],
    *,
    requested_tools: Sequence[str] | None,
    field_policy: Mapping[str, Any] | None,
) -> ResolvedAssistantCatalog:
    """Apply request/tool/field narrowing without inventing any descriptor."""

    mode = _text(projection.get("mode"), "mode", maximum_bytes=256)
    requested = normalize_requested_tools(requested_tools)
    policy = normalize_field_policy(field_policy)
    selected = set(requested) if requested else {row["tool_id"] for row in projection["tools"]}
    available = {row["tool_id"] for row in projection["tools"]}
    unknown = sorted(selected - available)
    if unknown:
        _fail("assistant_tool_out_of_mode", "requestedTools contains tools outside the selected mode")

    allowed_orientation = set(policy.get("allowedOrientationFields", ORIENTATION_FIELDS))
    tools: list[dict[str, Any]] = []
    for source in projection["tools"]:
        if source["tool_id"] not in selected:
            continue
        tool = deepcopy(source)
        if policy:
            tool["input_schema"] = _narrow_schema(tool["input_schema"], allowed_orientation)
            tool["input_schema_hash"] = raw_digest(tool["input_schema"])
        tools.append(tool)
    catalog = {
        "grant_policy": deepcopy(projection["grant_policy"]),
        "mode": mode,
        "nodes": deepcopy(projection["nodes"]),
        "skills": deepcopy(projection["skills"]),
        "tools": tools,
    }
    return ResolvedAssistantCatalog(
        catalog_hash=framed_digest(
            "assistant-mode-catalog", MODE_CATALOG_VERSION, catalog
        ),
        grant_policy=catalog["grant_policy"],
        nodes=catalog["nodes"],
        skills=catalog["skills"],
        tools=tools,
    )


__all__ = [
    "AssistantCatalogError",
    "ORIENTATION_FIELDS",
    "ResolvedAssistantCatalog",
    "normalize_field_policy",
    "normalize_requested_tools",
    "resolve_catalog",
    "verify_active_mode_projection",
]
