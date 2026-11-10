"""Constrained regeneration of a tool call that failed argument validation (E1).

EXPERIMENTAL, mode setting ``constrained_regeneration`` (off by default).

When a call fails with ``assistant_tool_arguments_invalid``, the bridge makes
one extra model call for the same tool. That request repeats the conversation,
adds the failed call's errors and asks for corrected arguments only. Its
OpenAI-standard ``response_format`` (``json_schema``) carries the full JSON
Schema of that exact call:

- every typed argument (``scene_resource``, ``blackboard_key[]``, ``node``,
  ...) is its full type schema, with the type's ``$defs`` moved to the root;
- a tree tool's node is resolved by its chosen ``node_id`` to that node's own
  schema (``node_schema``, ``replacement_schema`` or ``update_schema``), and
  each given child likewise, by position;
- a tree node that is not known (no valid ``node_id``) and further children
  get the compact generic node: the node type's closed keys, ``node_id`` one
  of its accepted ids, ``parameters`` and ``value_sources`` plain objects and
  ``children`` unbounded arrays of the same shape;
- a container's ``children`` carry no ``maxItems`` (a leaf keeps 0):
  llguidance expands a bound such as 4096 into thousands of grammar symbols.
  The validator enforces the bound.

The regular prompt and tools stay unchanged; the schema travels only in that
one request. The regenerated arguments are validated and executed like any
call; a call is regenerated at most once. Any failure of the regeneration,
including a typed upstream failure of its one model call, leaves the original
call and its error, as with the setting off.

The schema is sent with llguidance's `"x-guidance": {"lenient": true}`: the
grammar then skips the keywords it does not implement (`not`, measured on
Thor with vLLM 0.33). The bridge validator checks the regenerated arguments
against the complete schemas afterwards, so whatever the lenient grammar
skips stays enforced.

Only Draft-editing tools are regenerated. Each of their call schemas has an
identifier string with `pattern` and a length bound, which xgrammar does not
support, so vLLM's `auto` backend resolves it to guidance, the backend the
served engine is bound to (a schema resolving to xgrammar is refused there).
`test_constrained` checks this for every Draft-editing tool.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
import re
from typing import Any, Final, Mapping

from .catalog import _narrow_schema
from .node_catalog import _deref

RESPONSE_FORMAT_NAME: Final = "tool_arguments"
# llguidance JSON compile options (JsonCompileOptions, llguidance 1.7.6 in the
# vLLM 0.33 image): lenient skips unimplemented keywords; whitespace_flexible
# defaults to true there and lets the model emit free whitespace, so the
# output is compact with the standard separators.
GUIDANCE_OPTIONS: Final = {
    "lenient": True,
    "whitespace_flexible": False,
    "item_separator": ",",
    "key_separator": ":",
}
# A call's arguments are small; the regeneration never needs the turn budget.
MAXIMUM_REGENERATION_TOKENS: Final = 2048
MINIMUM_REGENERATION_TOKENS: Final = 256
# The corrected arguments are about the size of the rejected ones; four times
# their token estimate (about 3 characters per token) leaves room for the fix
# and cuts a grammar-bound structural loop early.
REGENERATION_SIZE_FACTOR: Final = 4
CHARACTERS_PER_TOKEN: Final = 3
# Typed upstream failures of the one regeneration call (HTTP status such as a
# refused grammar, timeout, transport, reply); each falls back.
UPSTREAM_FAILURES: Final = frozenset({
    "upstream_http_error",
    "upstream_response_invalid",
    "upstream_response_oversized",
    "upstream_timeout",
    "upstream_unavailable",
})


def regeneration_tokens(arguments: Mapping[str, Any], request_contract: Mapping[str, Any]) -> int:
    """The output bound for one regeneration, from the size of the call it repairs.

    With reasoning on, the model's thinking shares the same output budget, so
    the size-derived bound would cut it; the regeneration then keeps the fixed
    cap.
    """

    cap = min(request_contract["max_output_tokens"], MAXIMUM_REGENERATION_TOKENS)
    if request_contract.get("reasoning") == "on":
        return cap
    estimate = len(json.dumps(arguments, separators=(",", ":"))) // CHARACTERS_PER_TOKEN
    return max(MINIMUM_REGENERATION_TOKENS, min(REGENERATION_SIZE_FACTOR * estimate, cap))


class RegenerationFailed(Exception):
    """The regeneration produced no usable arguments; the original error stands."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason

# Tree-tool arguments resolved by their node_id: argument -> per-node schema.
_NODE_ARGUMENTS: Final = {
    ("tree_draft_insert_node", "node"): "tree",
    ("tree_draft_replace_subtree", "subtree"): "tree",
    ("tree_draft_wrap_node", "wrapper"): "node_schema",
    ("tree_draft_change_node_kind", "replacement"): "replacement_schema",
}
_UNSAFE: Final = re.compile(r"[^A-Za-z0-9_]")


def regeneration_instruction(tool_id: str) -> str:
    return (
        f"The {tool_id} call above was rejected. Reply with only the corrected "
        f"JSON arguments for {tool_id}; fix every listed error and keep everything "
        "else as intended."
    )


def failed_call_result(detail: Mapping[str, Any]) -> dict[str, Any]:
    """The rejected call's errors, as the model sees a failed tool result."""

    view: dict[str, Any] = {"ok": False, "errors": deepcopy(detail.get("errors", []))}
    if isinstance(detail.get("shape_of"), dict):
        view["shape_of"] = deepcopy(detail["shape_of"])
    return view


def regenerates(descriptor: Mapping[str, Any] | None) -> bool:
    """Whether a failed call of this tool gets a constrained regeneration."""

    return descriptor is not None and descriptor["effect"] == "composer_draft_mutating"


def response_format(schema: dict[str, Any]) -> dict[str, Any]:
    # Lenient: llguidance skips keywords it does not implement instead of
    # refusing the grammar; the bridge validator still enforces them.
    return {
        "type": "json_schema",
        "json_schema": {
            "name": RESPONSE_FORMAT_NAME,
            "schema": {**schema, "x-guidance": GUIDANCE_OPTIONS},
            "strict": True,
        },
    }


class _Schemas:
    """Builds one call schema; every `$defs` entry is moved to its root."""

    def __init__(
        self,
        types: Mapping[str, Any],
        nodes: Mapping[str, Mapping[str, Any]],
        allowed_orientation_fields: list[str] | None,
    ) -> None:
        self._types = types
        self._nodes = nodes
        self._allowed = allowed_orientation_fields
        self.defs: dict[str, Any] = {}

    def hoist(self, schema: Mapping[str, Any], source: str) -> dict[str, Any]:
        """A copy of `schema` whose `$defs` live in the root under `source`."""

        prefix = _UNSAFE.sub("_", source) + "__"

        def rewrite(value: Any) -> Any:
            if isinstance(value, dict):
                return {
                    key: (
                        "#/$defs/" + prefix + item.removeprefix("#/$defs/")
                        if key == "$ref" and isinstance(item, str) and item.startswith("#/$defs/")
                        else rewrite(item)
                    )
                    for key, item in value.items()
                    if key != "$defs"
                }
            if isinstance(value, list):
                return [rewrite(item) for item in value]
            return value

        for name, definition in schema.get("$defs", {}).items():
            if prefix + name not in self.defs:
                self.defs[prefix + name] = rewrite(definition)
        return rewrite(schema)

    def type_schema(self, type_name: str) -> dict[str, Any]:
        """The full named type, as a `$ref` to its root definition."""

        name = "type__" + type_name
        if name not in self.defs:
            schema = self._types[type_name]
            if self._allowed is not None:
                # The request's orientation field policy narrows the type,
                # as in the bridge's typed-argument check.
                schema = _narrow_schema(schema, set(self._allowed))
            self.defs[name] = self.hoist(schema, name)
        return {"$ref": "#/$defs/" + name}

    def generic_node(self) -> dict[str, Any]:
        """The node type's closed keys, without per-node schemas, as a `$ref`."""

        name = "generic__node"
        if name not in self.defs:
            root = self._types["node"]
            properties: dict[str, Any] = {}
            for key, schema in root["properties"].items():
                if key in ("parameters", "value_sources"):
                    properties[key] = {"type": "object"}
                elif key == "children":
                    properties[key] = {"type": "array", "items": {"$ref": "#/$defs/" + name}}
                else:
                    properties[key] = deepcopy(_deref(schema, root))
            self.defs[name] = {
                "type": "object",
                "additionalProperties": False,
                "properties": properties,
                "required": list(root.get("required", [])),
            }
        return {"$ref": "#/$defs/" + name}

    def node_id_of(self, value: Any, type_name: str) -> str | None:
        """The value's node_id when the type accepts it and the node is known."""

        node_id = value.get("node_id") if isinstance(value, dict) else None
        if not isinstance(node_id, str) or node_id not in self._nodes:
            return None
        root = self._types[type_name]
        accepted = _deref(root.get("properties", {}).get("node_id", {}), root)
        if isinstance(accepted, dict) and "enum" in accepted and node_id not in accepted["enum"]:
            return None
        return node_id

    def tree(self, value: Any) -> dict[str, Any]:
        node_id = self.node_id_of(value, "node")
        if node_id is None:
            return self.generic_node()
        row = self._nodes[node_id]
        schema = self.hoist(row["node_schema"], node_id + "__node")
        maximum = row["children"]["max"]
        # A container's bound stays with the validator: llguidance expands
        # `maxItems` into one grammar rule per item.
        children: dict[str, Any] = {"type": "array"}
        if maximum == 0:
            children["maxItems"] = 0
        else:
            given = value.get("children")
            given = given[:maximum] if isinstance(given, list) else []
            if given:
                children["prefixItems"] = [self.tree(child) for child in given]
            children["items"] = self.generic_node()
        schema["properties"] = {**schema.get("properties", {}), "children": children}
        return schema

    def node(self, value: Any, type_name: str, schema_field: str) -> dict[str, Any]:
        node_id = self.node_id_of(value, type_name)
        if node_id is None:
            return self.type_schema(type_name)
        return self.hoist(self._nodes[node_id][schema_field], node_id + "__" + schema_field)


def call_schema(
    descriptor: Mapping[str, Any],
    arguments: Mapping[str, Any],
    *,
    types: Mapping[str, Any],
    nodes: list[Mapping[str, Any]],
    allowed_orientation_fields: list[str] | None,
) -> dict[str, Any]:
    """The full JSON Schema of one call of `descriptor`, resolved by its arguments."""

    tool_id = descriptor["tool_id"]
    builder = _Schemas(types, {row["node_id"]: row for row in nodes}, allowed_orientation_fields)
    root = builder.hoist(descriptor["input_schema"], "tool")
    properties = root.setdefault("properties", {})
    for name, type_name in descriptor["argument_types"].items():
        base = type_name.removesuffix("[]")
        kind = _NODE_ARGUMENTS.get((tool_id, name))
        if kind == "tree":
            full = builder.tree(arguments.get(name))
        elif kind is not None:
            full = builder.node(arguments.get(name), base, kind)
        else:
            full = builder.type_schema(base)
        if type_name.endswith("[]"):
            properties[name] = {**properties.get(name, {"type": "array"}), "items": full}
        else:
            properties[name] = full
    if tool_id == "tree_draft_update_node_params":
        node_id = arguments.get("expected_node_id")
        accepted = properties.get("expected_node_id", {}).get("enum", [])
        if isinstance(node_id, str) and node_id in accepted and node_id in builder._nodes:
            update = builder.hoist(builder._nodes[node_id]["update_schema"], node_id + "__update")
            properties["expected_node_id"] = {"const": node_id}
            for field, schema in update.get("properties", {}).items():
                properties[field] = schema
            if update.get("allOf"):
                root["allOf"] = [*root.get("allOf", []), *update["allOf"]]
    if builder.defs:
        root["$defs"] = builder.defs
    return root


async def regenerate_call(
    service: Any,
    call: Any,
    detail: Mapping[str, Any],
    *,
    verified: Any,
    messages: list[dict[str, Any]],
    tools: list[dict[str, Any]],
    tool_by_id: dict[str, dict[str, Any]],
    request_contract: Mapping[str, Any],
) -> Any:
    """One constrained model call for the failed `call`; returns the new call.

    The new call keeps the failed call's id and position and is marked
    `regenerated`. Raises RegenerationFailed when the model call fails
    upstream, or the reply is truncated, oversized or not one JSON object; the
    caller then keeps the failed call and its error, as without regeneration.
    Cancellation and request-level errors propagate. `adopt` puts an accepted call's
    arguments into the conversation.
    """

    schema = call_schema(
        call.descriptor,
        call.arguments,
        types=verified.active_projection["types"],
        nodes=verified.catalog.nodes,
        allowed_orientation_fields=verified.field_policy.get("allowedOrientationFields"),
    )
    try:
        upstream = await service._call_upstream(
            endpoint=verified.active_projection["model_endpoint"],
            messages=[
                *messages,
                {
                    "role": "tool",
                    "tool_call_id": call.call_id,
                    "content": json.dumps(failed_call_result(detail), ensure_ascii=False,
                                          separators=(",", ":")),
                },
                {"role": "user", "content": regeneration_instruction(call.tool_id)},
            ],
            tools=tools,
            maximum_output_tokens=regeneration_tokens(call.arguments, request_contract),
            timeout_seconds=request_contract["default_timeout_s"],
            response_format=response_format(schema),
            # The small structured retry does not think, even with reasoning on.
            thinking=False,
        )
    except Exception as error:  # noqa: BLE001 - typed upstream failures of this one call
        code = getattr(error, "code", None)
        if code in UPSTREAM_FAILURES:
            raise RegenerationFailed(code) from error
        raise
    choices = upstream.get("choices") if isinstance(upstream, dict) else None
    if (isinstance(choices, list) and choices and isinstance(choices[0], dict)
            and choices[0].get("finish_reason") == "length"):
        raise RegenerationFailed("truncated")
    try:
        content, _, _ = service._parse_upstream(upstream, tool_by_id)
    except Exception as error:  # noqa: BLE001 - a malformed reply of this one call
        code = getattr(error, "code", None)
        if code in ("upstream_response_invalid", "upstream_response_oversized"):
            raise RegenerationFailed(code) from error
        raise
    try:
        decoded = json.loads(content or "")
    except ValueError as error:
        raise RegenerationFailed("not_json") from error
    if not isinstance(decoded, dict):
        raise RegenerationFailed("not_json")
    # The reply is the arguments object: parsed exactly like a tool call's.
    _, message, (regenerated,) = service._parse_upstream(
        {"choices": [{"message": {"content": "", "tool_calls": [{
            "id": call.call_id,
            "type": "function",
            "function": {"name": call.tool_id, "arguments": content},
        }]}}]},
        tool_by_id,
    )
    return replace(regenerated, position=call.position, regenerated=True,
                   regenerated_function=message["tool_calls"][0]["function"])


def adopt(messages: list[dict[str, Any]], call: Any) -> None:
    """The conversation's assistant message carries the accepted arguments."""

    for row in next(
        item for item in reversed(messages)
        if item["role"] == "assistant"
        and any(entry["id"] == call.call_id for entry in item.get("tool_calls", []))
    )["tool_calls"]:
        if row["id"] == call.call_id:
            row["function"] = call.regenerated_function
