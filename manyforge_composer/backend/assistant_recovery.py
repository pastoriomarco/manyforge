"""Recovery hints appended to bridge tool-call error responses.

The model sees the bridge's 4xx response body as the tool-call result.
Without a recovery framing, error messages are diagnostic only — they
state what is wrong but not what to do next. Adding a class-specific
"Action: ..." clause and a rotating "step back and re-evaluate" nudge
gives even thinking-off models a useful recovery surface and breaks
repetition loops where the model would otherwise re-emit the same
broken call.

The nudges are intentionally factual (not first-person) and roughly
equivalent in meaning, just rephrased — what matters is that
consecutive failures produce different textual prefixes for the model,
not that the nudges escalate or diverge in content.

The recovery wrapper is composed at the bridge endpoint, not at the
validator, so validators stay state-free.

Layered nudges augment the base recovery envelope with
context-specific suggestions when the generic Action is not enough:

* context_aware_action — overrides the generic Action with a tailored
  one for known intent traps. The canonical example is
  arity_insufficient on @root with an empty decorator/composite,
  which is almost always a wrap-intent expressed as insert; the
  override redirects the caller at wrap_node instead of the generic
  "add the missing children" advice.
* truncation_pattern_nudge — fires when consecutive stringified-
  payload parse failures land at the same byte offset, indicating
  the caller is hitting a per-tool-call argument size limit. Points
  the caller at lighter-payload tools (wrap_node, swap_node) or at
  splitting the operation into smaller calls.
* format_stuck_nudge — broader sibling of the truncation nudge.
  Fires when the same tool produces the same error class three times
  in a row, regardless of detail. Tells the caller that the call
  shape is not the issue — the chosen tool or approach is — and to
  switch rather than retry.
* meta_nudge — fires once a conversation accumulates enough failed
  calls overall (META_NUDGE_AFTER_FAILURES). Surfaces the running
  tool/error-class distribution so the caller sees the thrash
  pattern explicitly and can step back, rather than only seeing
  the latest single error.

These compose: the truncation/format-stuck pair is mutually exclusive
(truncation wins when both match) and meta_nudge stacks on top.
"""
from __future__ import annotations

import re
from typing import Any


# Equal-weight rephrasings of the same instruction:
# re-read the request + the catalogs + the structural rules,
# then either retry with a corrected payload or pick a different tool.
# Each is self-contained so the model can act on any one of them on its own.
NUDGES: tuple[str, ...] = (
    "Re-read the original request, the tools manifest, and the node catalog; then either retry with a corrected payload or choose a different tool.",
    "Re-examine the request alongside the catalog and the structural rules before the next call; a different tool or a different shape may be needed.",
    "Consult the catalog and the program tree again, then either fix the payload and retry or pick a tool whose contract matches the request more closely.",
    "Step back: review the original request, the manifest, and the catalog before retrying; the correct move may be a different tool entirely.",
    "Cross-check the request against the manifest's tools and the catalog entries' rules; retry only after confirming this tool and shape can satisfy the goal.",
    "Re-evaluate from the original request: which tool fits, what the catalog requires, and whether the structural rules allow the intended change.",
)


def nudge_for_index(index: int) -> str:
    """Return the rotating nudge for a given failure-counter index.

    The counter is 1-based at the call site; we map it onto the 0-based
    NUDGES tuple via modulo so wraparound is graceful at any failure
    count.
    """
    return NUDGES[(max(0, index - 1)) % len(NUDGES)]


def append_recovery(
    detail: Any,
    action: str | None,
    nudge: str,
    *,
    extra_lines: list[str] | None = None,
) -> Any:
    """Compose the final error envelope the model sees.

    Up to four kinds of clauses on a single line:

        <diagnostic>. Action: <class-specific recovery>. <rotating nudge>. <extra lines...>

    Extra lines are layered context-specific nudges (truncation
    pattern, format-stuck, meta-summary) appended after the rotating
    nudge. They are optional and only fire when their detectors
    return non-None.

    For string details, this is straight concatenation. For dict
    details, the structured keys (action, recoveryNudge, extraNudges)
    are also added so structured consumers can read them individually
    while the model sees the same composed string under `message`.
    """
    extras = [s for s in (extra_lines or []) if s]
    if isinstance(detail, str):
        parts: list[str] = [detail.rstrip(". ")]
        if action:
            parts.append(f"Action: {action.rstrip('. ')}")
        if nudge:
            parts.append(nudge.rstrip(". "))
        for extra in extras:
            parts.append(extra.rstrip(". "))
        return ". ".join(parts) + "."
    if isinstance(detail, dict):
        composed = dict(detail)
        message = composed.get("message")
        if isinstance(message, str):
            composed["message"] = append_recovery(
                message, action, nudge, extra_lines=extras
            )
        if action:
            composed["action"] = action
        if nudge:
            composed["recoveryNudge"] = nudge
        if extras:
            composed["extraNudges"] = list(extras)
        return composed
    return detail


# After this many consecutive failures on a single conversation, the
# bridge attaches a fresh program-tree snapshot to the error envelope so
# the model can see the current state without spending a tool call on
# program_read. Counter resets on success.
ATTACH_SNAPSHOT_AFTER_FAILURES: int = 3


# Hard cap on consecutive failures per conversation. When reached, the
# bridge stops accepting tool calls for the in-flight assistant request
# and surfaces a structural-impasse response — the model is expected to
# halt the agent loop and reply to the user with an explanation, a
# request for direction, and a suggested simpler command.
IMPASSE_FAILURE_THRESHOLD: int = 15


# meta_nudge fires after this many failed tool calls on a turn.
# Tuned to roughly one third of IMPASSE_FAILURE_THRESHOLD so the
# caller sees the tool/error-class distribution well before hitting
# the hard cap, with room to switch approach afterwards.
META_NUDGE_AFTER_FAILURES: int = 5


# truncation_pattern_nudge and format_stuck_nudge both trigger when
# this many consecutive failures share a defining property (same
# parse-error column for the truncation case; same tool + error
# class for the broader format-stuck case). Three is the smallest
# count where "the model keeps emitting the same shape" becomes
# a confident pattern.
FORMAT_STUCK_REPEAT_THRESHOLD: int = 3


# Regex for extracting the byte column from a stringified-JSON parse
# failure. Matches both Python's json.JSONDecodeError messages and
# common variants. None when the column can't be extracted.
_PARSE_COLUMN_RE = re.compile(r"line\s+\d+\s+column\s+(\d+)")


def _extract_parse_column(error_text: Any) -> int | None:
    if isinstance(error_text, dict):
        return _extract_parse_column(error_text.get("message"))
    if not isinstance(error_text, str):
        return None
    m = _PARSE_COLUMN_RE.search(error_text)
    if m is None:
        return None
    try:
        return int(m.group(1))
    except (TypeError, ValueError):
        return None


def context_aware_action(
    *,
    error_class: str | None,
    tool_id: str | None,
    arguments: dict[str, Any] | None,
) -> str | None:
    """Return a context-tailored Action clause when one applies.

    Falls back to None — callers should then look up the generic
    ACTION_BY_CLASS entry. The point of this function is to override
    the generic Action when the (tool_id, arguments) shape strongly
    suggests a specific intent that the generic clause would steer
    away from.

    Currently handles:

    * arity_insufficient + tree_draft_insert_node + parentName
      resolves to root + new node has no children + new node is a
      decorator or composite kind → wrap intent. Surface a wrap_node
      pointer instead of the generic "add the missing children"
      action, because the model almost certainly wanted to introduce
      this kind as a new parent above the existing root, not to add
      an empty composite/decorator as a sibling.
    """
    if not error_class or not tool_id or not isinstance(arguments, dict):
        return None
    if error_class == "arity_insufficient" and tool_id == "tree_draft_insert_node":
        parent_name_raw = arguments.get("parentName") or arguments.get("parent_name") or ""
        parent_is_root = (
            isinstance(parent_name_raw, str)
            and parent_name_raw.strip().lower() in {"@root", "root"}
        )
        node = arguments.get("node")
        # Accept stringified payloads that may have been normalized.
        if isinstance(node, dict):
            new_children = node.get("children") or []
            has_no_children = isinstance(new_children, list) and len(new_children) == 0
        else:
            has_no_children = False
        if parent_is_root and has_no_children:
            return (
                "If the goal is to introduce this kind as a new parent "
                "above the current root, use tree_draft_wrap_node with "
                "targetName='@root' and a wrapper carrying the new id and "
                "name — wrap_node inserts the existing subtree as the "
                "wrapper's only child automatically, so you do not need "
                "to re-emit any children. Use insert_node only if you "
                "genuinely want this node added as a sibling and you can "
                "supply the required minimum children inline."
            )
    return None


def truncation_pattern_nudge(history: list[dict[str, Any]]) -> str | None:
    """Detect repeated parse failures at the same byte column.

    History is the conversation's recent tool-call summaries (oldest
    first). When the last FORMAT_STUCK_REPEAT_THRESHOLD failures all
    share the same parse-error column from a stringified-payload
    parse, return the targeted size-limit nudge.
    """
    if len(history) < FORMAT_STUCK_REPEAT_THRESHOLD:
        return None
    columns: list[int] = []
    for entry in history[-FORMAT_STUCK_REPEAT_THRESHOLD:]:
        if entry.get("status") != "failed":
            return None
        col = _extract_parse_column(entry.get("error"))
        if col is None:
            return None
        columns.append(col)
    if len(set(columns)) != 1:
        return None
    return (
        "Your stringified payload is parse-failing at the same byte "
        "offset on every retry, indicating a per-tool-call argument "
        "size limit. Stop trying to inline the entire subtree — switch "
        "to a tool that does not require re-emitting children "
        "(tree_draft_wrap_node to add a parent, tree_draft_swap_node "
        "to change a node's id), or break the operation into multiple "
        "smaller calls."
    )


def format_stuck_nudge(history: list[dict[str, Any]]) -> str | None:
    """Detect same-tool same-error-class repetition.

    When the last FORMAT_STUCK_REPEAT_THRESHOLD failures used the same
    tool and produced the same error class, the model is stuck in a
    format/shape loop — the same call shape keeps producing the same
    rejection. Return a nudge suggesting an approach change rather
    than another retry.
    """
    if len(history) < FORMAT_STUCK_REPEAT_THRESHOLD:
        return None
    last = history[-FORMAT_STUCK_REPEAT_THRESHOLD:]
    if any(entry.get("status") != "failed" for entry in last):
        return None
    tools = {entry.get("tool") for entry in last}
    classes = {entry.get("error_class") for entry in last}
    if len(tools) != 1 or len(classes) != 1:
        return None
    cls = next(iter(classes))
    if cls is None:
        return None
    tool = next(iter(tools)) or "this tool"
    return (
        f"The same call shape on {tool} has produced the same error "
        f"class ({cls}) on every recent retry. The payload format is "
        "not the issue; the chosen approach or tool is. Switch to a "
        "different tool or restructure the operation before the next "
        "call."
    )


def meta_nudge(history: list[dict[str, Any]], *, threshold: int) -> str | None:
    """Surface a tool/error-class distribution summary when warranted.

    Fires when the conversation has accumulated at least `threshold`
    failed tool calls. Surfaces what the model has been trying so it
    can recognise its own thrash pattern and switch approach.
    """
    failed = [entry for entry in history if entry.get("status") == "failed"]
    if len(failed) < threshold:
        return None
    tool_counts: dict[str, int] = {}
    class_counts: dict[str, int] = {}
    for entry in failed:
        tool = entry.get("tool") or "unknown"
        tool_counts[tool] = tool_counts.get(tool, 0) + 1
        cls = entry.get("error_class")
        if cls:
            class_counts[cls] = class_counts.get(cls, 0) + 1
    tool_summary = ", ".join(
        f"{tool}×{count}"
        for tool, count in sorted(tool_counts.items(), key=lambda kv: -kv[1])[:4]
    )
    class_summary = ", ".join(
        f"{cls}×{count}"
        for cls, count in sorted(class_counts.items(), key=lambda kv: -kv[1])[:4]
    ) or "uncategorised"
    return (
        f"You have accumulated {len(failed)} failed calls on this turn "
        f"(tools: {tool_summary}; classes: {class_summary}). Sustained "
        "failures usually indicate a different tool or a simpler "
        "approach is needed; review the manifest and pick a tool whose "
        "contract matches the request more directly."
    )


def impasse_message(
    *,
    failure_count: int,
    tool_history: list[dict[str, Any]],
) -> str:
    """Build the structural-impasse response the model sees on the cap.

    Tells the agent loop to stop calling tools, summarises what was
    attempted, and instructs the agent to explain the impasse to the
    user, ask for direction, and propose a simpler command. The
    tool_history is a list of {tool, status, error_class} entries
    capturing what the agent has tried in this turn.
    """
    tool_counts: dict[str, int] = {}
    error_counts: dict[str, int] = {}
    for entry in tool_history:
        tool = entry.get("tool") or "unknown"
        tool_counts[tool] = tool_counts.get(tool, 0) + 1
        cls = entry.get("error_class")
        if cls:
            error_counts[cls] = error_counts.get(cls, 0) + 1
    tool_summary = ", ".join(
        f"{tool}×{count}"
        for tool, count in sorted(tool_counts.items(), key=lambda kv: -kv[1])
    ) or "none"
    error_summary = ", ".join(
        f"{cls}×{count}"
        for cls, count in sorted(error_counts.items(), key=lambda kv: -kv[1])
    ) or "none"
    return (
        f"Tool-call budget exhausted on this turn after {failure_count} "
        "consecutive failed attempts. "
        f"Tools attempted: {tool_summary}. "
        f"Error classes encountered: {error_summary}. "
        "Stop calling tools now. In your reply to the user: explain "
        "what you tried, what went wrong (focus on the dominant error "
        "class above), ask the user for direction on how to proceed, "
        "and propose a simpler version of the request that you believe "
        "could succeed in one or two tool calls. Do not retry the same "
        "approach without the user's input."
    )


# Mapping of error-class identifiers to their class-specific Action
# clauses. The bridge-endpoint exception handler classifies the failed
# call based on the HTTPException it caught, picks the matching action
# here, and composes the final envelope via append_recovery.
ACTION_BY_CLASS: dict[str, str] = {
    "unknown_parent_name":
        "Pick an existing name from validParentNames and retry, or use the rootAlias for the root.",
    "arity_saturated":
        "Restructure first by introducing a multi-child parent under the saturated decorator; do not retry the same shape.",
    "arity_insufficient":
        "Add the required minimum children to satisfy this node's arity, or pick a different node kind whose minimum arity matches the current child count.",
    "arity_leaf_no_children":
        "Choose a parent whose catalog kind accepts children; this kind never accepts any.",
    "missing_required_param":
        "Add the missing parameter from the expected schema; consult the catalog entry if the accepted shape is unclear.",
    "unrecognized_param":
        "Remove parameters not declared by the catalog entry and retry; consult the catalog for the accepted parameter set.",
    "duplicate_node_name":
        "Pick a different unique instance name and retry; do not retry with the same name.",
    "tool_outside_mode":
        "Pick a tool listed in this assistant mode's allowed-tools manifest.",
    "catalog_hash_drift":
        "Re-read the manifest to obtain the current catalogHash, then retry the call.",
    "stringified_payload_unparseable":
        "Send the payload as a structured object instead of a JSON-encoded string.",
    "unknown_node_name":
        "Pick a name that currently exists in the program tree and retry; consult program_read if needed.",
    "unknown_tool":
        "Pick a tool listed in this assistant mode's manifest; consult catalog_read for tool ids.",
    "node_kind_not_in_mode":
        "Pick a node id from the response's allowedNodeKinds list, or use a different tool whose semantics fit the request without recreating the rejected node ids.",
    "delete_root_forbidden":
        "Use replace_subtree or wrap_node on the root instead of delete_node; the root cannot be removed in isolation.",
    "swap_arity_incompatible":
        "The new node id's arity does not accept the current child count. Adjust the children first or pick a different new id whose arity matches.",
    "swap_param_invalid":
        "Provide the new id's required params explicitly under params, or set paramsMode to merge to inherit overlapping params from the old node.",
    "swap_class_mismatch":
        "The new id is not in the same swap class as the current node. Pick a swap-compatible id, or pass force=true if the cross-class swap is intentional.",
    "swap_capability_missing":
        "Pick a new id whose required capabilities are present in the active deployment.",
}


# Heuristic classification of plain-string error details. The bridge
# endpoint uses these substring matches to attach a class-specific
# action without requiring every raise site to carry an explicit class
# tag. Matched in order; the first hit wins. Sites that need
# unambiguous classification can call append_recovery() directly with
# an explicit action string.
_DETAIL_CLASSIFIERS: tuple[tuple[str, str], ...] = (
    ("Duplicate node instance name", "duplicate_node_name"),
    ("missing required parameter", "missing_required_param"),
    ("allows at most 0 children", "arity_leaf_no_children"),
    ("allows at most", "arity_saturated"),
    ("requires at least", "arity_insufficient"),
    ("could not be parsed as JSON", "stringified_payload_unparseable"),
    ("string parsed to", "stringified_payload_unparseable"),
    ("catalogHash does not match", "catalog_hash_drift"),
    ("is outside assistantMode", "tool_outside_mode"),
    ("Parent node not found", "unknown_parent_name"),
    ("Node not found:", "unknown_node_name"),
    ("not allowed by", "node_kind_not_in_mode"),
    ("cannot delete the root", "delete_root_forbidden"),
    ("swap class mismatch", "swap_class_mismatch"),
    ("swap arity", "swap_arity_incompatible"),
    ("swap params", "swap_param_invalid"),
    ("swap capability", "swap_capability_missing"),
    ("unknown tool", "unknown_tool"),
)


def classify_detail(detail: Any) -> str | None:
    """Best-effort error-class for a freeform detail. Returns None when no rule matches."""
    if isinstance(detail, dict):
        message = detail.get("message")
        if isinstance(message, str):
            return classify_detail(message)
        return None
    if not isinstance(detail, str):
        return None
    for needle, cls in _DETAIL_CLASSIFIERS:
        if needle in detail:
            return cls
    return None


def action_for_class(error_class: str | None) -> str | None:
    """Return the Action clause for an error class, or None when unknown."""
    if error_class is None:
        return None
    return ACTION_BY_CLASS.get(error_class)
