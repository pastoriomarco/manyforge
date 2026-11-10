"""E1: one constrained regeneration of a call that fails argument validation."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from fastapi.testclient import TestClient
import httpx
from jsonschema import Draft202012Validator

from manyforge_assistant_bridge.bridge import create_app
from manyforge_assistant_bridge.constrained import (
    GUIDANCE_OPTIONS,
    MAXIMUM_REGENERATION_TOKENS,
    regeneration_tokens,
    call_schema,
    regenerates,
)

from test_bridge import (
    _CATALOG,
    _INSTRUMENTS,
    _gripper_insert,
    _no_composer,
    _projection,
    _request,
    _service,
    _terminal,
)

_TOOLS = (
    "blackboard_draft_upsert_keys",
    "scene_draft_add_object",
    "tree_draft_insert_node",
    "tree_draft_update_node_params",
)


def _mode(*, constrained_regeneration: bool, reasoning: str | None = None) -> dict[str, Any]:
    native = _projection()
    native["constrained_regeneration"] = constrained_regeneration
    if reasoning is not None:
        native["model_endpoint"]["request_contract"]["reasoning"] = reasoning
    for field in ("catalog_hash", "instrument_registry_digest", "nodes"):
        native.pop(field)
    native["source"].pop("source_content_digest")
    native["tools"] = [
        {"effect": "composer_draft_mutating", "tool_id": tool_id} for tool_id in _TOOLS
    ]
    return _INSTRUMENTS.compose_mode(native, [entry.id for entry in _INSTRUMENTS.entries()])


def _upstream(replies: list[dict[str, Any] | str], seen: list[dict[str, Any]]):
    """Reply i: a dict is one insert call with those arguments, a str is content."""

    async def upstream(request: httpx.Request) -> httpx.Response:
        seen.append(json.loads(request.content))
        index = len(seen) - 1
        reply = replies[index] if index < len(replies) else "done"
        if isinstance(reply, tuple) and reply[0] == "oversized":
            return httpx.Response(200, content=b'{"choices":[{"message":{"content":"'
                                  + b" " * (9 * 1024 * 1024) + b'"}}]}', request=request)
        if isinstance(reply, tuple) and reply[0] == "status":
            # vLLM refusing the grammar, as measured with llguidance.
            return httpx.Response(reply[1], json={"error": {"message": "Grammar error: panic"}},
                                  request=request)
        if isinstance(reply, tuple) and reply[0] == "timeout":
            raise httpx.ReadTimeout("timed out", request=request)
        if isinstance(reply, tuple) and reply[0] == "length":
            return httpx.Response(200, json={"choices": [{"message": {"content": reply[1]},
                                                          "finish_reason": "length"}]},
                                  request=request)
        if isinstance(reply, str):
            message: dict[str, Any] = {"content": reply}
        else:
            message = {"content": "", "tool_calls": [{
                "id": "call.0", "type": "function",
                "function": {"name": "tree_draft_insert_node", "arguments": json.dumps(reply)},
            }]}
        return httpx.Response(200, json={"choices": [{"message": message}]}, request=request)

    return upstream


async def _composer(request: httpx.Request) -> httpx.Response:
    body = json.loads(request.content)
    return httpx.Response(200, json={
        "success": True,
        "mutated": True,
        "draftMutated": True,
        "requestId": body["requestId"],
        "requestOwnerEpoch": body["requestOwnerEpoch"],
        "sourceLabel": body["sourceLabel"],
        "correlationId": body["correlationId"],
        "nextAdmissionGeneration": body["expectedAdmissionGeneration"],
        "nextWorkspaceRevision": body["expectedWorkspaceRevision"] + 1,
        "result": {"kind": "composer_draft_mutation", "draftRevision": 2, "details": {}},
    }, request=request)


def _run(tmp_path: Path, replies: list[Any], *, on: bool = True, composer=_composer,
         reasoning: str | None = None):
    projection = _mode(constrained_regeneration=on, reasoning=reasoning)
    seen: list[dict[str, Any]] = []
    service = _service(tmp_path, projection, upstream_handler=_upstream(replies, seen),
                       composer_handler=composer, node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    audit = [json.loads(line) for line in (tmp_path / "audit.jsonl").read_text().splitlines()]
    return status, seen, audit, projection


_BAD = _gripper_insert("open")
_GOOD = _gripper_insert({"kind": "named_preset", "name": "open"})


def _refs(value: Any) -> set[str]:
    if isinstance(value, dict):
        found = {value["$ref"]} if isinstance(value.get("$ref"), str) else set()
        return found.union(*(_refs(item) for item in value.values()))
    if isinstance(value, list):
        return set().union(*(_refs(item) for item in value))
    return set()


def _assert_resolved(schema: dict[str, Any]) -> None:
    Draft202012Validator.check_schema(schema)
    for reference in _refs(schema):
        assert reference.startswith("#/$defs/"), reference
        assert reference.removeprefix("#/$defs/") in schema["$defs"], reference


def _tool(projection: dict[str, Any], tool_id: str) -> dict[str, Any]:
    return next(row for row in projection["tools"] if row["tool_id"] == tool_id)


def test_an_invalid_call_is_regenerated_once_with_its_full_schema_and_executes(
    tmp_path: Path,
) -> None:
    status, seen, audit, projection = _run(tmp_path, [_BAD, json.dumps(_GOOD)])
    assert status["state"] == "completed", status
    assert len(seen) == 3
    normal, extra, final = seen
    assert "response_format" not in normal and "response_format" not in final
    # The extra request: same tools and conversation, the errors, the schema.
    assert extra["tools"] == normal["tools"] and extra["tool_choice"] == "none"
    assert extra["messages"][: len(normal["messages"])] == normal["messages"]
    failed = extra["messages"][len(normal["messages"])]
    assert json.loads(failed["tool_calls"][0]["function"]["arguments"]) == _BAD
    tool_result, instruction = extra["messages"][-2:]
    assert tool_result["role"] == "tool" and tool_result["tool_call_id"] == "call.0"
    assert json.loads(tool_result["content"])["errors"][0]["path"] == "node.parameters.position"
    assert instruction["role"] == "user" and "tree_draft_insert_node" in instruction["content"]
    schema = extra["response_format"]["json_schema"]["schema"]
    assert extra["response_format"]["type"] == "json_schema"
    assert extra["response_format"]["json_schema"]["strict"] is True
    # The full schema, sent lenient (llguidance skips `not`; the bridge
    # validator still checks it).
    assert schema == {**call_schema(
        _tool(projection, "tree_draft_insert_node"), _BAD,
        types=projection["types"], nodes=projection["nodes"], allowed_orientation_fields=None,
    ), "x-guidance": GUIDANCE_OPTIONS}
    assert GUIDANCE_OPTIONS["whitespace_flexible"] is False
    assert extra["max_tokens"] == regeneration_tokens(_BAD, {
        "max_output_tokens": normal["max_tokens"]})
    assert schema["properties"]["node"]["properties"]["node_id"] == {
        "const": "gripper.command_position"}

    call = status["response"]["toolCalls"][0]
    assert len(status["response"]["toolCalls"]) == 1
    assert call["status"] == "completed"
    assert call["arguments"]["node"]["parameters"]["position"] == {
        "kind": "named_preset", "name": "open"}
    assert call["result"]["regenerated"] is True
    # The conversation carries the regenerated arguments with their result.
    assistant = final["messages"][-2]
    assert json.loads(assistant["tool_calls"][0]["function"]["arguments"]) == _GOOD
    assert [row.get("regenerated") for row in audit if row["kind"] == "tool_call"] == [True]


def test_with_reasoning_on_only_the_regeneration_turns_thinking_off(tmp_path: Path) -> None:
    for reasoning, normal_kwargs, extra_kwargs in (
        (None, None, None),
        ("off", {"enable_thinking": False}, {"enable_thinking": False}),
        ("on", {"enable_thinking": True}, {"enable_thinking": False}),
    ):
        status, seen, _, _ = _run(tmp_path, [_BAD, json.dumps(_GOOD)], reasoning=reasoning)
        assert status["state"] == "completed", status
        normal, extra, final = seen
        assert normal.get("chat_template_kwargs") == normal_kwargs
        assert final.get("chat_template_kwargs") == normal_kwargs
        assert extra.get("chat_template_kwargs") == extra_kwargs


def test_a_still_invalid_regeneration_returns_typed_errors_without_a_second_one(
    tmp_path: Path,
) -> None:
    status, seen, audit, _ = _run(
        tmp_path, [_BAD, json.dumps(_BAD)], composer=_no_composer
    )
    assert len(seen) == 3
    assert ["response_format" in body for body in seen] == [False, True, False]
    call = status["response"]["toolCalls"][0]
    assert call["status"] == "failed"
    # The invalid regeneration is discarded: the original call and error stand.
    assert "regenerated" not in call["result"]
    assert call["result"]["regenerationFailed"] == "invalid"
    assert json.loads(seen[2]["messages"][-2]["tool_calls"][0]["function"]["arguments"]) == _BAD
    detail = call["result"]["detail"]
    assert detail["code"] == "assistant_tool_arguments_invalid"
    assert detail["path"] == "node.parameters.position"
    assert json.loads(seen[2]["messages"][-1]["content"])["errors"][0]["path"] == (
        "node.parameters.position")


def test_with_the_setting_off_an_invalid_call_is_not_regenerated(tmp_path: Path) -> None:
    status, seen, audit, _ = _run(tmp_path, [_BAD], on=False, composer=_no_composer)
    assert len(seen) == 2
    assert not any("response_format" in body for body in seen)
    call = status["response"]["toolCalls"][0]
    assert call["status"] == "failed" and "regenerated" not in call["result"]
    assert all("regenerated" not in row for row in audit)


def test_a_valid_call_is_not_regenerated(tmp_path: Path) -> None:
    status, seen, _, _ = _run(tmp_path, [_GOOD])
    assert len(seen) == 2 and not any("response_format" in body for body in seen)
    assert "regenerated" not in status["response"]["toolCalls"][0]["result"]


def test_the_insert_schema_is_the_full_per_node_schema() -> None:
    projection = _mode(constrained_regeneration=True)
    nodes = {row["node_id"]: row for row in projection["nodes"]}
    move = {"node_id": "motion.move_to_pose", "instance_id": "approach", "parameters": {}}
    arguments = {
        "parent_instance_id": "main",
        "node": {"node_id": "sequence", "instance_id": "main", "parameters": {},
                 "children": [move]},
    }
    schema = call_schema(_tool(projection, "tree_draft_insert_node"), arguments,
                         types=projection["types"], nodes=projection["nodes"],
                         allowed_orientation_fields=None)
    _assert_resolved(schema)
    node = schema["properties"]["node"]
    assert node["properties"]["node_id"] == {"const": "sequence"}
    children = node["properties"]["children"]
    # The container's bound stays with the validator; a leaf keeps 0.
    assert nodes["sequence"]["children"]["max"] > 0 and "maxItems" not in children
    assert children["items"] == {"$ref": "#/$defs/generic__node"}
    child = children["prefixItems"][0]
    expected = nodes["motion.move_to_pose"]["node_schema"]
    # Every parameter of the node is there, with its own (hoisted) schema.
    assert child["properties"]["parameters"]["properties"].keys() == (
        expected["properties"]["parameters"]["properties"].keys())
    assert child["properties"]["node_id"] == {"const": "motion.move_to_pose"}
    assert child["properties"]["children"] == {"type": "array", "maxItems": 0}
    assert len({name for name in schema["$defs"] if name.startswith("motion_move_to_pose")}) == (
        len(expected["$defs"]))
    # The placement rule of the tool stays.
    assert schema["oneOf"] == _tool(projection, "tree_draft_insert_node")["input_schema"]["oneOf"]
    validator = Draft202012Validator(schema)
    good = _gripper_insert({"kind": "named_preset", "name": "open"})
    good_schema = call_schema(_tool(projection, "tree_draft_insert_node"), good,
                              types=projection["types"], nodes=projection["nodes"],
                              allowed_orientation_fields=None)
    assert not list(Draft202012Validator(good_schema).iter_errors(good))
    assert list(Draft202012Validator(good_schema).iter_errors(_BAD))
    assert list(validator.iter_errors({**arguments, "node": {**arguments["node"],
                                                              "node_id": "parallel"}}))
    # An unknown node_id falls back to the compact generic node.
    unknown = call_schema(_tool(projection, "tree_draft_insert_node"),
                          {"node": {"node_id": "no.such.node"}},
                          types=projection["types"], nodes=projection["nodes"],
                          allowed_orientation_fields=None)
    _assert_resolved(unknown)
    assert unknown["properties"]["node"] == {"$ref": "#/$defs/generic__node"}


def _node_defs(schema: dict[str, Any], projection: dict[str, Any]) -> set[str]:
    """The node ids whose own schemas the call schema carries."""

    text = json.dumps(schema)
    return {row["node_id"] for row in projection["nodes"]
            if f'"const": "{row["node_id"]}"' in text}


def test_an_unknown_node_and_further_children_get_the_compact_generic_node() -> None:
    projection = _mode(constrained_regeneration=True)
    tool = _tool(projection, "tree_draft_insert_node")
    node_type = projection["types"]["node"]
    # The model named the field wrong: no node is known.
    wrong = {"parent_instance_id": "main", "node": {
        "expected_node_id": "gripper.command_position", "instance_id": "grip", "parameters": {}}}
    schema = call_schema(tool, wrong, types=projection["types"], nodes=projection["nodes"],
                         allowed_orientation_fields=None)
    _assert_resolved(schema)
    assert _node_defs(schema, projection) == set()
    generic = schema["$defs"]["generic__node"]
    assert generic["additionalProperties"] is False
    assert generic["properties"].keys() == node_type["properties"].keys()
    assert generic["required"] == node_type["required"]
    assert set(generic["properties"]["node_id"]["enum"]) == {
        row["node_id"] for row in projection["nodes"]}
    assert generic["properties"]["parameters"] == {"type": "object"}
    assert generic["properties"]["value_sources"] == {"type": "object"}
    assert generic["properties"]["children"] == {
        "type": "array", "items": {"$ref": "#/$defs/generic__node"}}
    assert len(json.dumps(schema)) < 4096
    # The grammar is wider than the validator; it still rejects the call.
    assert list(Draft202012Validator(schema).iter_errors(wrong))

    # A container carries only the nodes the model gave.
    move = {"node_id": "motion.move_to_pose", "instance_id": "approach", "parameters": {}}
    grip = {"node_id": "gripper.command_position", "instance_id": "grip", "parameters": {}}
    inner = {"node_id": "sequence", "instance_id": "inner", "parameters": {},
             "children": [move, grip]}
    tree = {"parent_instance_id": "main", "node": {
        "node_id": "sequence", "instance_id": "main", "parameters": {}, "children": [inner, move]}}
    schema = call_schema(tool, tree, types=projection["types"], nodes=projection["nodes"],
                         allowed_orientation_fields=None)
    _assert_resolved(schema)
    assert _node_defs(schema, projection) == {
        "sequence", "motion.move_to_pose", "gripper.command_position"}
    assert not list(Draft202012Validator(schema).iter_errors(tree))
    rows = {row["node_id"]: row for row in projection["nodes"]}
    given = sum(len(json.dumps(rows[node_id]["node_schema"])) for node_id in (
        "sequence", "sequence", "motion.move_to_pose", "motion.move_to_pose",
        "gripper.command_position"))
    assert len(json.dumps(schema)) < given + 4096


def _children_bounds(value: Any) -> list[Any]:
    if isinstance(value, dict):
        children = value.get("properties", {}).get("children")
        found = [children.get("maxItems")] if isinstance(children, dict) else []
        return found + [bound for item in value.values() for bound in _children_bounds(item)]
    if isinstance(value, list):
        return [bound for item in value for bound in _children_bounds(item)]
    return []


def test_nested_containers_carry_no_children_bound_in_the_grammar() -> None:
    # Three nested sequences made llguidance refuse the grammar (symbol limit):
    # every `maxItems` 4096 expands into thousands of rules.
    projection = _mode(constrained_regeneration=True)
    move = {"node_id": "motion.move_to_pose", "instance_id": "approach", "parameters": {}}
    grip = {"node_id": "gripper.command_position", "instance_id": "grip", "parameters": {}}

    def sequence(name: str, *children: dict[str, Any]) -> dict[str, Any]:
        return {"node_id": "sequence", "instance_id": name, "parameters": {},
                "children": list(children)}

    tree = {"parent_instance_id": "main", "node": sequence(
        "a", sequence("b", sequence("c", move, grip), grip), sequence("d", move))}
    schema = call_schema(_tool(projection, "tree_draft_insert_node"), tree,
                         types=projection["types"], nodes=projection["nodes"],
                         allowed_orientation_fields=None)
    _assert_resolved(schema)
    bounds = _children_bounds(schema)
    # Four sequences and the generic node unbounded; four leaves at 0.
    assert sorted(bounds, key=str) == [0] * 4 + [None] * 5
    assert len(json.dumps(schema)) < 64 * 1024
    assert not list(Draft202012Validator(schema).iter_errors(tree))


def test_typed_argument_schemas_resolve_their_defs() -> None:
    projection = _mode(constrained_regeneration=True)
    keys = call_schema(_tool(projection, "blackboard_draft_upsert_keys"), {},
                       types=projection["types"], nodes=projection["nodes"],
                       allowed_orientation_fields=None)
    _assert_resolved(keys)
    assert projection["types"]["blackboard_key"].get("$defs")
    assert keys["properties"]["keys"]["items"] == {"$ref": "#/$defs/type__blackboard_key"}
    assert keys["properties"]["keys"]["minItems"] == 1
    validator = Draft202012Validator(keys)
    key = {"key": "ready", "type": "bool", "access": "read_write", "persistence": "run"}
    assert not list(validator.iter_errors({"keys": [key]}))
    assert list(validator.iter_errors({"keys": [{**key, "initial": "yes"}]}))

    scene = call_schema(_tool(projection, "scene_draft_add_object"), {},
                        types=projection["types"], nodes=projection["nodes"],
                        allowed_orientation_fields=None)
    _assert_resolved(scene)
    assert scene["properties"]["scene_resource"] == {"$ref": "#/$defs/type__scene_resource"}


def test_an_update_resolves_the_expected_node_parameters() -> None:
    projection = _mode(constrained_regeneration=True)
    row = next(r for r in projection["nodes"] if r["node_id"] == "gripper.command_position")
    schema = call_schema(
        _tool(projection, "tree_draft_update_node_params"),
        {"instance_id": "close_gripper", "expected_node_id": "gripper.command_position",
         "parameters": {"position": "open"}},
        types=projection["types"], nodes=projection["nodes"], allowed_orientation_fields=None,
    )
    _assert_resolved(schema)
    assert schema["properties"]["expected_node_id"] == {"const": "gripper.command_position"}
    assert schema["properties"]["parameters"]["properties"].keys() == (
        row["update_schema"]["properties"]["parameters"]["properties"].keys())
    validator = Draft202012Validator(schema)
    base = {"instance_id": "close_gripper", "expected_node_id": "gripper.command_position"}
    assert list(validator.iter_errors({**base, "parameters": {"position": "open"}}))
    assert not list(validator.iter_errors(
        {**base, "parameters": {"position": {"kind": "named_preset", "name": "open"}}}))


# Every tool the instruments offer, with its effect as composed for a mode.
_ALL_TOOLS = (
    "alarms_read", "blackboard_draft_remove_keys", "blackboard_draft_remove_namespaces",
    "blackboard_draft_upsert_keys", "blackboard_draft_upsert_namespaces", "catalog_read",
    "program_draft_remove_contact_policies", "program_draft_remove_parameters",
    "program_draft_upsert_contact_policies", "program_draft_upsert_parameters", "program_read",
    "program_validate", "scene_draft_add_object", "scene_draft_remove_objects",
    "scene_draft_update_object", "scene_draft_upsert_objects", "scene_inspect",
    "tree_draft_change_node_kind", "tree_draft_delete_node", "tree_draft_insert_node",
    "tree_draft_move_node", "tree_draft_rename_node", "tree_draft_replace_subtree",
    "tree_draft_update_node_params", "tree_draft_wrap_node",
)


def _xgrammar_unsupported(value: Any) -> bool:
    """vLLM 0.33 `has_xgrammar_unsupported_json_features`, the rule our schemas hit:
    a string with `pattern` (or `format`) and a length bound."""

    if isinstance(value, dict):
        kind = value.get("type")
        kinds = set(kind) if isinstance(kind, list) else {kind} if isinstance(kind, str) else set()
        if ("string" in kinds and ("pattern" in value or "format" in value)
                and ("minLength" in value or "maxLength" in value)):
            return True
        return any(_xgrammar_unsupported(item) for item in value.values())
    if isinstance(value, list):
        return any(_xgrammar_unsupported(item) for item in value)
    return False


def test_every_regenerated_tool_schema_resolves_to_the_guidance_backend() -> None:
    native = _projection()
    for field in ("catalog_hash", "instrument_registry_digest", "nodes"):
        native.pop(field)
    native["source"].pop("source_content_digest")
    native["tools"] = [
        {"effect": "composer_draft_mutating" if "_draft_" in tool_id else "read_only",
         "tool_id": tool_id}
        for tool_id in _ALL_TOOLS
    ]
    projection = _INSTRUMENTS.compose_mode(native, [e.id for e in _INSTRUMENTS.entries()])
    regenerated = [row for row in projection["tools"] if regenerates(row)]
    assert {row["tool_id"] for row in regenerated} == {
        tool_id for tool_id in _ALL_TOOLS if "_draft_" in tool_id}
    for row in regenerated:
        schema = call_schema(row, {}, types=projection["types"], nodes=projection["nodes"],
                             allowed_orientation_fields=None)
        assert _xgrammar_unsupported(schema), row["tool_id"]
    assert not regenerates(_tool(projection, "program_validate"))


def test_an_invalid_read_is_not_regenerated(tmp_path: Path) -> None:
    seen: list[dict[str, Any]] = []

    async def read_call(request: httpx.Request) -> httpx.Response:
        seen.append(json.loads(request.content))
        if len(seen) > 1:
            return httpx.Response(200, json={"choices": [{"message": {"content": "done"}}]},
                                  request=request)
        return httpx.Response(200, json={"choices": [{"message": {"content": "", "tool_calls": [{
            "id": "call.0", "type": "function",
            "function": {"name": "catalog_read", "arguments": json.dumps({"bogus": 1})},
        }]}}]}, request=request)

    native = _projection()
    native["constrained_regeneration"] = True
    for field in ("catalog_hash", "instrument_registry_digest", "nodes"):
        native.pop(field)
    native["source"].pop("source_content_digest")
    native["tools"] = [{"effect": "read_only", "tool_id": "catalog_read"}]
    projection = _INSTRUMENTS.compose_mode(native, [e.id for e in _INSTRUMENTS.entries()])
    service = _service(tmp_path, projection, upstream_handler=read_call,
                       composer_handler=_no_composer, node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert len(seen) == 2 and not any("response_format" in body for body in seen)
    call = status["response"]["toolCalls"][0]
    assert call["status"] == "failed" and "regenerated" not in call["result"]



import pytest  # noqa: E402


@pytest.mark.parametrize(("reply", "reason"), [
    (("length", "{" + " " * 4000), "truncated"),
    (("oversized",), "upstream_response_oversized"),
    ("this is not json", "not_json"),
    ("{" + " " * 70_000 + "}", "upstream_response_oversized"),
    (("status", 400), "upstream_http_error"),
    (("timeout",), "upstream_timeout"),
])
def test_a_failed_regeneration_returns_the_original_error_and_the_request_goes_on(
    tmp_path: Path, reply: Any, reason: str,
) -> None:
    status, seen, audit, _ = _run(tmp_path, [_BAD, reply], composer=_no_composer)
    assert status["state"] == "completed", status
    call = status["response"]["toolCalls"][0]
    assert call["status"] == "failed"
    assert call["result"]["regenerationFailed"] == reason
    assert "regenerated" not in call["result"]
    assert call["result"]["detail"]["path"] == "node.parameters.position"
    # The model gets the original error, as with E1 off, and answers.
    assert json.loads(seen[2]["messages"][-1]["content"])["errors"][0]["path"] == (
        "node.parameters.position")



def test_the_regeneration_budget_follows_the_size_of_the_repaired_call() -> None:
    contract = {"max_output_tokens": 8192}
    small = {"instance_id": "a"}
    assert regeneration_tokens(small, contract) == 256
    medium = {"node": {"x": "y" * 580}}  # ~600 characters, ~200 tokens
    assert regeneration_tokens(medium, contract) == 4 * (len(json.dumps(
        medium, separators=(",", ":"))) // 3)
    large = {"node": {"x": "y" * 9000}}
    assert regeneration_tokens(large, contract) == MAXIMUM_REGENERATION_TOKENS
    assert regeneration_tokens(medium, {"max_output_tokens": 512}) == 512
    # With reasoning on the thinking shares the budget: the fixed cap stays.
    assert regeneration_tokens(small, {**contract, "reasoning": "on"}) == (
        MAXIMUM_REGENERATION_TOKENS)


def test_a_nested_interrupt_condition_resolves_in_the_call_schema() -> None:
    from test_bridge import _nested_condition

    projection = _mode(constrained_regeneration=True)
    seek = {"node_id": "motion.move_until_signal", "instance_id": "seek",
            "parameters": {"interrupt_condition": _nested_condition()}, "children": []}
    arguments = {"parent_instance_id": "main", "node": {
        "node_id": "sequence", "instance_id": "main", "parameters": {}, "children": [seek]}}
    schema = call_schema(_tool(projection, "tree_draft_insert_node"), arguments,
                         types=projection["types"], nodes=projection["nodes"],
                         allowed_orientation_fields=None)
    _assert_resolved(schema)
    validator = Draft202012Validator(schema)
    assert not list(validator.iter_errors(arguments))
    seek["parameters"]["interrupt_condition"] = _nested_condition("not a bool") | {"all": []}
    assert list(validator.iter_errors(arguments))
