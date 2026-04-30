"""Assistant provider boundary for model-backed Composer workflows.

The provider boundary intentionally deals in reviewable proposals and tool-call
records only.  Providers must not mutate Composer state directly.
"""
from __future__ import annotations

import os
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol

from .assistant_tools import (
    AssistantToolError,
    propose_scene_object_nodes,
    propose_scene_object_resources,
)


class AssistantProviderError(RuntimeError):
    """Raised when an assistant provider cannot complete a request."""


@dataclass(frozen=True)
class AssistantProviderContext:
    """Server-side context passed to assistant providers."""

    conversation_id: str
    program_loaded: bool
    cycle_state: str
    assistant_mode: str | None = None
    schema_version: str = "0.1.0"
    tool_catalog: list[dict[str, Any]] = field(default_factory=list)
    visible_skills: list[str] = field(default_factory=list)
    visible_nodes: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class AssistantProviderResult:
    """Normalized provider result returned to the REST layer."""

    mode: str
    message: str
    proposals: list[dict[str, Any]] = field(default_factory=list)
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    requires_review: bool = True
    mutated: bool = False
    timestamp_ns: int = field(default_factory=time.monotonic_ns)


class AssistantProvider(Protocol):
    """Protocol implemented by mock, fake, and future NemoClaw providers."""

    @property
    def provider_id(self) -> str:
        """Stable provider identifier for diagnostics."""

    def run(self, request: Any, context: AssistantProviderContext) -> AssistantProviderResult:
        """Run the provider and return normalized, non-mutating proposals."""


_SIMULATION_TERMS = (
    "isaac",
    "sim",
    "simulation",
    "scene",
    "collider",
    "object",
    "soup",
    "grasp",
)


class MockAssistantProvider:
    """Deterministic default provider used when no model provider is configured."""

    provider_id = "mock"

    def run(self, request: Any, context: AssistantProviderContext) -> AssistantProviderResult:
        proposals = _mock_proposals(request, context)
        return AssistantProviderResult(
            mode="mock",
            message=_mock_message(proposals),
            proposals=proposals,
            tool_calls=[],
            warnings=[
                "Assistant scaffold is running in mock mode; no model or external tool was invoked.",
                "Returned proposals are drafts and require explicit user review before application.",
            ],
            mutated=False,
            requires_review=True,
        )


class DeterministicSceneAuthoringProvider:
    """Local deterministic provider for testing the assistant tool loop.

    This provider is not an LLM.  It is a strict stand-in for the future
    NemoClaw/OpenClaw provider so Composer can validate tool-loop contracts,
    proposal staging, and review/apply behavior without a live model service.
    """

    provider_id = "deterministic-scene-authoring"

    def run(self, request: Any, context: AssistantProviderContext) -> AssistantProviderResult:
        report = _context_value(
            request.context,
            "isaacSceneReport",
            "isaac_scene_report",
            "sceneReport",
            "scene_report",
            "colliderReport",
            "collider_report",
        )
        source_roots = _string_list_context(
            request.context,
            "sourceRoots",
            "source_roots",
        )
        object_id_prefix = str(
            _context_value(
                request.context,
                "objectIdPrefix",
                "object_id_prefix",
                default="",
            )
            or ""
        )
        max_proposals = _optional_int_context(
            request.context,
            "maxProposals",
            "max_proposals",
        )
        propose_tree_nodes = bool(
            _context_value(
                request.context,
                "proposeTreeNodes",
                "propose_tree_nodes",
                default=False,
            )
        )
        node_operations = _string_list_context(
            request.context,
            "nodeOperations",
            "node_operations",
            default=[],
        )
        link_frame = str(
            _context_value(request.context, "linkFrame", "link_frame", default="tool0")
            or "tool0"
        )
        gripper_id_value = _context_value(request.context, "gripperId", "gripper_id")
        gripper_id = str(gripper_id_value).strip() if gripper_id_value else None

        tool_calls: list[dict[str, Any]] = []
        proposals: list[dict[str, Any]] = []
        warnings: list[str] = [
            "Deterministic provider mode is a local test stand-in; no model was invoked.",
        ]

        if report is None:
            tool_calls.append(
                {
                    "name": "inspect_isaac_scene",
                    "status": "skipped",
                    "arguments": {},
                    "error": (
                        "No Isaac scene report was provided in assistant context. "
                        "Live inspection is intentionally not performed by the "
                        "deterministic provider."
                    ),
                }
            )
            proposals = _mock_proposals(request, context)
            warnings.append(
                "Provide context.isaacSceneReport or use the inspect_isaac_scene tool before requesting scene proposals."
            )
            return AssistantProviderResult(
                mode="provider",
                message=(
                    "I need an Isaac scene inspection report before I can propose "
                    "planning objects in deterministic provider mode."
                ),
                proposals=proposals,
                tool_calls=tool_calls,
                warnings=warnings,
                mutated=False,
                requires_review=True,
            )

        if not isinstance(report, dict):
            raise AssistantProviderError("assistant context Isaac scene report must be an object")

        tool_calls.append(
            {
                "name": "inspect_isaac_scene",
                "status": "completed",
                "arguments": {"source": "request.context"},
                "result": {
                    "summary": report.get("summary", {}),
                    "objectCount": len(report.get("objects", []))
                    if isinstance(report.get("objects"), list)
                    else None,
                },
            }
        )

        propose_args = {
            "sourceRoots": source_roots,
            "objectIdPrefix": object_id_prefix,
            "maxProposals": max_proposals,
        }
        try:
            propose_result = propose_scene_object_resources(
                report,
                source_roots=source_roots,
                object_id_prefix=object_id_prefix,
                max_proposals=max_proposals,
            )
        except AssistantToolError as exc:
            tool_calls.append(
                {
                    "name": "propose_scene_objects",
                    "status": "failed",
                    "arguments": propose_args,
                    "error": str(exc),
                }
            )
            raise AssistantProviderError(str(exc)) from exc

        proposals = list(propose_result.get("proposals") or [])
        proposal_count = len(proposals)
        tool_calls.append(
            {
                "name": "propose_scene_objects",
                "status": "completed",
                "arguments": propose_args,
                "result": {
                    "proposalCount": proposal_count,
                    "sceneResourceCount": len(propose_result.get("sceneResources") or []),
                    "warnings": list(propose_result.get("warnings") or []),
                },
            }
        )
        warnings.extend(str(item) for item in propose_result.get("warnings") or [])

        if propose_tree_nodes and propose_result.get("sceneResources"):
            node_args = {
                "operations": node_operations or ["add_collision_object"],
                "linkFrame": link_frame,
                "gripperId": gripper_id,
            }
            try:
                node_result = propose_scene_object_nodes(
                    list(propose_result.get("sceneResources") or []),
                    operations=node_operations or ["add_collision_object"],
                    link_frame=link_frame,
                    gripper_id=gripper_id,
                )
            except AssistantToolError as exc:
                tool_calls.append(
                    {
                        "name": "propose_scene_object_nodes",
                        "status": "failed",
                        "arguments": node_args,
                        "error": str(exc),
                    }
                )
                raise AssistantProviderError(str(exc)) from exc
            node_proposals = list(node_result.get("proposals") or [])
            proposals.extend(node_proposals)
            warnings.extend(str(item) for item in node_result.get("warnings") or [])
            tool_calls.append(
                {
                    "name": "propose_scene_object_nodes",
                    "status": "completed",
                    "arguments": node_args,
                    "result": {
                        "proposalCount": len(node_proposals),
                        "treeNodeCount": len(node_result.get("treeNodes") or []),
                        "warnings": list(node_result.get("warnings") or []),
                    },
                }
            )

        return AssistantProviderResult(
            mode="provider",
            message=(
                f"Prepared {proposal_count} reviewable scene-object proposal"
                f"{'' if proposal_count == 1 else 's'} from the provided Isaac scene report."
            ),
            proposals=proposals,
            tool_calls=tool_calls,
            warnings=warnings,
            mutated=False,
            requires_review=True,
        )


class NemoClawAssistantProvider:
    """HTTP adapter for a NemoClaw/OpenClaw-backed assistant provider.

    The endpoint is expected to run the model/tool loop externally and return
    normalized ManyForge proposals.  This adapter remains fail-closed: it
    rejects mutation claims and malformed proposal/tool-call records before the
    Composer routes see them.
    """

    def __init__(
        self,
        *,
        endpoint_url: str,
        provider_id: str = "nemoclaw",
        timeout_s: float = 30.0,
        api_token: str | None = None,
        request_fn: Callable[[dict[str, Any]], dict[str, Any]] | None = None,
    ) -> None:
        normalized_endpoint = endpoint_url.strip().rstrip("/")
        if not normalized_endpoint:
            raise AssistantProviderError("NemoClaw assistant endpoint URL is required")
        if timeout_s <= 0:
            raise AssistantProviderError("NemoClaw assistant timeout must be > 0")
        self._endpoint_url = normalized_endpoint
        self._provider_id = provider_id.strip() or "nemoclaw"
        self._timeout_s = float(timeout_s)
        self._api_token = api_token.strip() if api_token else None
        self._request_fn = request_fn

    @property
    def provider_id(self) -> str:
        return self._provider_id

    @property
    def endpoint_url(self) -> str:
        return self._endpoint_url

    def run(self, request: Any, context: AssistantProviderContext) -> AssistantProviderResult:
        payload = self._request_payload(request, context)
        try:
            response_payload = (
                self._request_fn(payload)
                if self._request_fn is not None
                else self._post_payload(payload)
            )
        except AssistantProviderError:
            raise
        except Exception as exc:
            raise AssistantProviderError(f"NemoClaw assistant request failed: {exc}") from exc

        return self._normalize_response(response_payload, context=context)

    def _request_payload(
        self,
        request: Any,
        context: AssistantProviderContext,
    ) -> dict[str, Any]:
        payload = {
            "version": "manyforge.assistant.provider_request.v0",
            "requestId": f"mf-{uuid.uuid4().hex[:12]}",
            "providerId": self.provider_id,
            "conversationId": context.conversation_id,
            "message": request.message,
            "requestedTools": list(request.requested_tools),
            "context": dict(request.context),
            "runtime": {
                "programLoaded": context.program_loaded,
                "cycleState": context.cycle_state,
            },
            "tools": list(context.tool_catalog),
            "constraints": {
                "mutatesState": False,
                "requiresReview": True,
                "proposalStatus": "draft",
                "allowedToolCallStatuses": [
                    "proposed",
                    "skipped",
                    "completed",
                    "failed",
                ],
            },
        }
        if context.assistant_mode:
            tool_ids = [_tool_identifier(tool) for tool in context.tool_catalog]
            payload.update(
                {
                    "schemaVersion": context.schema_version,
                    "assistantMode": context.assistant_mode,
                    "skills": list(context.visible_skills),
                    "nodes": list(context.visible_nodes),
                    "catalog": {
                        "skills": list(context.visible_skills),
                        "tools": [tool_id for tool_id in tool_ids if tool_id],
                        "nodes": list(context.visible_nodes),
                    },
                }
            )
        return payload

    def _post_payload(self, payload: dict[str, Any]) -> dict[str, Any]:
        try:
            import httpx
        except ModuleNotFoundError as exc:
            raise AssistantProviderError(
                "httpx is required for the NemoClaw assistant provider"
            ) from exc

        headers = {"Content-Type": "application/json"}
        if self._api_token:
            headers["Authorization"] = f"Bearer {self._api_token}"
        try:
            response = httpx.post(
                self.endpoint_url,
                json=payload,
                headers=headers,
                timeout=self._timeout_s,
            )
            response.raise_for_status()
            data = response.json()
        except httpx.TimeoutException as exc:
            raise AssistantProviderError(
                f"NemoClaw assistant timed out after {self._timeout_s:.3f}s"
            ) from exc
        except httpx.HTTPStatusError as exc:
            raise AssistantProviderError(
                "NemoClaw assistant returned HTTP "
                f"{exc.response.status_code}: {exc.response.text[:500]}"
            ) from exc
        except ValueError as exc:
            raise AssistantProviderError(
                "NemoClaw assistant returned a non-JSON response"
            ) from exc
        if not isinstance(data, dict):
            raise AssistantProviderError("NemoClaw assistant response must be an object")
        return data

    def _normalize_response(
        self,
        data: dict[str, Any],
        *,
        context: AssistantProviderContext,
    ) -> AssistantProviderResult:
        if not isinstance(data, dict):
            raise AssistantProviderError("NemoClaw assistant response must be an object")
        provider_error = data.get("error")
        if provider_error is not None:
            if isinstance(provider_error, dict):
                code = str(provider_error.get("code") or "provider_error").strip()
                detail = str(provider_error.get("detail") or "").strip()
            else:
                code = "provider_error"
                detail = str(provider_error).strip()
            message = f"NemoClaw assistant rejected request: {code}"
            if detail:
                message += f": {detail}"
            raise AssistantProviderError(message)
        if bool(data.get("mutated")):
            raise AssistantProviderError(
                "NemoClaw assistant responses must not report state mutation"
            )

        message = str(data.get("message") or data.get("response") or "").strip()
        if not message:
            raise AssistantProviderError("NemoClaw assistant response.message is required")

        proposals = _normalize_provider_proposals(data.get("proposals", []))
        tool_calls = _normalize_provider_tool_calls(data.get("toolCalls", data.get("tool_calls", [])))
        _validate_provider_tool_calls_in_catalog(tool_calls, context)
        warnings = _normalize_string_list(data.get("warnings", []), field_name="warnings")
        requires_review = bool(data.get("requiresReview", data.get("requires_review", True)))
        if proposals and not requires_review:
            warnings.append(
                "Provider attempted to mark proposals as not requiring review; ManyForge forced review."
            )
            requires_review = True

        return AssistantProviderResult(
            mode="provider",
            message=message,
            proposals=proposals,
            tool_calls=tool_calls,
            warnings=warnings,
            requires_review=requires_review,
            mutated=False,
        )


def build_assistant_provider(
    provider_id: str | None,
    *,
    endpoint_url: str | None = None,
    timeout_s: float | None = None,
    api_token: str | None = None,
) -> AssistantProvider | None:
    """Build a configured assistant provider by id.

    Empty, ``mock``, and ``none`` keep Composer in its default mock-only mode.
    """
    normalized = (provider_id or "").strip().lower()
    if normalized in {"", "none", "mock"}:
        return None
    if normalized in {"deterministic", "deterministic-scene-authoring"}:
        return DeterministicSceneAuthoringProvider()
    if normalized in {"nemoclaw", "openclaw"}:
        return NemoClawAssistantProvider(
            endpoint_url=endpoint_url or _assistant_endpoint_from_env(),
            provider_id=normalized,
            timeout_s=timeout_s if timeout_s is not None else _assistant_timeout_from_env(),
            api_token=api_token or os.environ.get("MANYFORGE_ASSISTANT_API_TOKEN"),
        )
    raise AssistantProviderError(f"Unsupported assistant provider: {provider_id}")


def assistant_provider_catalog(configured_provider: AssistantProvider | None) -> list[dict[str, Any]]:
    configured_id = configured_provider.provider_id if configured_provider is not None else None
    return [
        {
            "id": "mock",
            "description": "Built-in mock provider. Returns scaffolding proposals only.",
            "configured": configured_id in {None, "mock"},
            "requiresLiveModel": False,
            "requiresLiveIsaacSim": False,
        },
        {
            "id": DeterministicSceneAuthoringProvider.provider_id,
            "description": (
                "Deterministic local scene-authoring provider. Consumes an Isaac "
                "scene report from request context and calls validated backend tools."
            ),
            "configured": configured_id == DeterministicSceneAuthoringProvider.provider_id,
            "requiresLiveModel": False,
            "requiresLiveIsaacSim": False,
        },
        {
            "id": "nemoclaw",
            "description": (
                "External NemoClaw assistant provider. Calls a configured "
                "HTTP endpoint and normalizes response proposals into Composer's "
                "review/apply flow."
            ),
            "configured": configured_id == "nemoclaw",
            "requiresLiveModel": True,
            "requiresLiveIsaacSim": False,
        },
        {
            "id": "openclaw",
            "description": (
                "External OpenClaw assistant provider using the same ManyForge "
                "assistant HTTP contract as NemoClaw."
            ),
            "configured": configured_id == "openclaw",
            "requiresLiveModel": True,
            "requiresLiveIsaacSim": False,
        },
    ]


def _assistant_endpoint_from_env() -> str:
    endpoint = (
        os.environ.get("MANYFORGE_ASSISTANT_ENDPOINT_URL")
        or os.environ.get("MANYFORGE_NEMOCLAW_ASSISTANT_URL")
        or os.environ.get("MANYFORGE_OPENCLAW_ASSISTANT_URL")
        or ""
    )
    if not endpoint.strip():
        raise AssistantProviderError(
            "MANYFORGE_ASSISTANT_ENDPOINT_URL is required for the NemoClaw assistant provider"
        )
    return endpoint


def _assistant_timeout_from_env() -> float:
    raw_timeout = os.environ.get("MANYFORGE_ASSISTANT_TIMEOUT_S", "").strip()
    if not raw_timeout:
        return 30.0
    try:
        timeout_s = float(raw_timeout)
    except ValueError as exc:
        raise AssistantProviderError(
            "MANYFORGE_ASSISTANT_TIMEOUT_S must be a number"
        ) from exc
    if timeout_s <= 0:
        raise AssistantProviderError("MANYFORGE_ASSISTANT_TIMEOUT_S must be > 0")
    return timeout_s


def _tool_identifier(tool: dict[str, Any]) -> str:
    identifier = tool.get("id", tool.get("name", ""))
    return str(identifier).strip()


def _validate_provider_tool_calls_in_catalog(
    tool_calls: list[dict[str, Any]],
    context: AssistantProviderContext,
) -> None:
    if not context.assistant_mode:
        return
    allowed_tool_ids = {
        tool_id
        for tool_id in (_tool_identifier(tool) for tool in context.tool_catalog)
        if tool_id
    }
    out_of_catalog = sorted(
        {
            str(call.get("name", "")).strip()
            for call in tool_calls
            if str(call.get("name", "")).strip() not in allowed_tool_ids
        }
    )
    if out_of_catalog:
        raise AssistantProviderError(
            "NemoClaw assistant response referenced tools outside assistantMode "
            f"'{context.assistant_mode}': {out_of_catalog}"
        )


def _normalize_provider_proposals(raw: Any) -> list[dict[str, Any]]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise AssistantProviderError("NemoClaw assistant response.proposals must be a list")
    proposals: list[dict[str, Any]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise AssistantProviderError(f"proposal[{index}] must be an object")
        proposal = dict(item)
        for key in ["id", "type", "title", "summary"]:
            value = proposal.get(key)
            if not isinstance(value, str) or not value.strip():
                raise AssistantProviderError(f"proposal[{index}].{key} must be a non-empty string")
            proposal[key] = value.strip()
        proposal["status"] = "draft"
        proposal["reviewRequired"] = True
        payload = proposal.get("payload", {})
        if not isinstance(payload, dict):
            raise AssistantProviderError(f"proposal[{index}].payload must be an object")
        proposal["payload"] = payload
        proposal["warnings"] = _normalize_string_list(
            proposal.get("warnings", []),
            field_name=f"proposal[{index}].warnings",
        )
        source = proposal.get("source", {})
        if source is None:
            source = {}
        if not isinstance(source, dict):
            raise AssistantProviderError(f"proposal[{index}].source must be an object")
        source.setdefault("kind", "nemoclaw")
        proposal["source"] = source
        proposals.append(proposal)
    return proposals


def _normalize_provider_tool_calls(raw: Any) -> list[dict[str, Any]]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise AssistantProviderError("NemoClaw assistant response.toolCalls must be a list")
    tool_calls: list[dict[str, Any]] = []
    allowed_statuses = {"proposed", "skipped", "completed", "failed"}
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise AssistantProviderError(f"toolCalls[{index}] must be an object")
        call = dict(item)
        name = call.get("name")
        if not isinstance(name, str) or not name.strip():
            raise AssistantProviderError(f"toolCalls[{index}].name must be a non-empty string")
        status = call.get("status", "proposed")
        if status not in allowed_statuses:
            raise AssistantProviderError(
                f"toolCalls[{index}].status must be one of {sorted(allowed_statuses)}"
            )
        arguments = call.get("arguments", {})
        if arguments is None:
            arguments = {}
        if not isinstance(arguments, dict):
            raise AssistantProviderError(f"toolCalls[{index}].arguments must be an object")
        normalized = {
            "name": name.strip(),
            "status": status,
            "arguments": arguments,
        }
        result = call.get("result")
        if result is not None:
            if not isinstance(result, dict):
                raise AssistantProviderError(f"toolCalls[{index}].result must be an object")
            normalized["result"] = result
        error = call.get("error")
        if error is not None:
            normalized["error"] = str(error)
        tool_calls.append(normalized)
    return tool_calls


def _normalize_string_list(raw: Any, *, field_name: str) -> list[str]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        raise AssistantProviderError(f"NemoClaw assistant {field_name} must be a list")
    return [str(item) for item in raw]


def _mock_proposals(
    request: Any,
    context: AssistantProviderContext,
) -> list[dict[str, Any]]:
    message = request.message.lower()
    context_keys = set(request.context.keys())
    scene_related = any(term in message for term in _SIMULATION_TERMS) or bool(
        context_keys & {"isaac_scene_report", "scene_report", "collider_report"}
    )

    if scene_related:
        return [
            {
                "id": f"proposal-{context.conversation_id[:12]}-scene-review",
                "type": "scene_authoring_review",
                "title": "Inspect simulator scene before authoring objects",
                "summary": (
                    "Use the Isaac scene inspection tool to gather collider/object data, "
                    "then propose explicit ManyForge scene draft edits for user review."
                ),
                "status": "draft",
                "reviewRequired": True,
                "payload": {
                    "recommendedTool": "inspect_isaac_scene",
                    "requestedTools": request.requested_tools,
                    "nextActions": [
                        "inspect simulator colliders with configured include/exclude filters",
                        "ask the user which objects should become planning geometry",
                        "propose add/update/remove scene draft edits without applying them",
                    ],
                    "programLoaded": context.program_loaded,
                    "cycleState": context.cycle_state,
                },
                "warnings": [
                    "Do not infer safety-critical planning geometry without user approval.",
                    "Do not mutate Composer state from the assistant response path.",
                ],
                "source": {
                    "kind": "mock_assistant_scaffold",
                    "mode": request.mode,
                },
            }
        ]

    return [
        {
            "id": f"proposal-{context.conversation_id[:12]}-assistant-scaffold",
            "type": "assistant_scaffold",
            "title": "No model-backed assistant configured",
            "summary": (
                "The assistant API is available, but this route currently returns only "
                "reviewable mock proposals and performs no actions."
            ),
            "status": "draft",
            "reviewRequired": True,
            "payload": {
                "requestedTools": request.requested_tools,
                "programLoaded": context.program_loaded,
                "cycleState": context.cycle_state,
            },
            "warnings": [
                "Configure a bounded model/tool provider before relying on assistant output.",
            ],
            "source": {
                "kind": "mock_assistant_scaffold",
                "mode": request.mode,
            },
        }
    ]


def _mock_message(proposals: list[dict[str, Any]]) -> str:
    proposal_type = proposals[0]["type"] if proposals else "none"
    if proposal_type == "scene_authoring_review":
        return (
            "I can help with simulator-backed scene authoring, but this scaffold only "
            "returns draft proposals. It did not inspect Isaac Sim or change the program."
        )
    return (
        "Assistant scaffold is active in mock mode. It did not call a model, invoke tools, "
        "or change Composer state."
    )


def _context_value(context: dict[str, Any], *keys: str, default: Any = None) -> Any:
    for key in keys:
        if key in context:
            return context[key]
    return default


def _string_list_context(
    context: dict[str, Any],
    *keys: str,
    default: list[str] | None = None,
) -> list[str]:
    value = _context_value(context, *keys, default=default or [])
    if value is None:
        return []
    if not isinstance(value, list):
        raise AssistantProviderError(f"{keys[0]} must be a list of strings")
    result: list[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise AssistantProviderError(f"{keys[0]} must be a list of non-empty strings")
        result.append(item.strip())
    return result


def _optional_int_context(context: dict[str, Any], *keys: str) -> int | None:
    value = _context_value(context, *keys, default=None)
    if value is None:
        return None
    if not isinstance(value, int) or value < 1:
        raise AssistantProviderError(f"{keys[0]} must be a positive integer")
    return value
