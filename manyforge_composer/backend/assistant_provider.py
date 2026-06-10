"""Assistant provider boundary for model-backed Composer workflows.

The provider boundary intentionally deals in reviewable proposals and tool-call
records only. Providers must not mutate runtime or saved state directly.
Composer-authorized callbacks may update the in-memory Composer draft and report
that separately from provider mutation.
"""
from __future__ import annotations

import logging
import os
import re
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol
from urllib.parse import quote

from .assistant_tools import (
    AssistantToolError,
    propose_scene_object_nodes,
    propose_scene_object_resources,
)

logger = logging.getLogger(__name__)

# A Node.js process warning emitted via process.emitWarning() always renders in
# a fixed, documented shape: "(node:<pid>) [CODE] <Type>Warning: <message>", e.g.
# "(node:1610) [UNDICI-EHPA] Warning: EnvHttpProxyAgent is experimental ...".
# The OpenClaw lane runs as a Node subprocess and the bridge forwards its raw
# stderr into the assistant warnings channel (prefixed "OpenClaw stderr: ..."),
# so these benign experimental/deprecation notices show up as user-facing
# warnings. The "(node:<pid>)" token is written ONLY by Node's warning writer
# and never by a ManyForge-emitted (English-sentence) warning, so matching on
# this STRUCTURE — rather than on message keywords like "UNDICI"/"experimental",
# which would rot as Node's messages change — reliably separates runtime noise
# from actionable assistant warnings, and catches future Node notices too.
_NODE_RUNTIME_WARNING_RE = re.compile(
    r"\(node:\d+\)\s*"        # process-warning prefix (emitWarning only)
    r"(?:\[[\w.+-]+\]\s*)?"   # optional warning code: [UNDICI-EHPA], [DEP0040], ...
    r"\w*Warning:"            # Warning: / ExperimentalWarning: / DeprecationWarning:
)


def _partition_runtime_noise(warnings: list[str]) -> tuple[list[str], list[str]]:
    """Split actionable assistant warnings from benign Node-subprocess noise.

    Returns (kept, dropped). A warning is dropped only when it matches the Node
    process-warning grammar (see _NODE_RUNTIME_WARNING_RE) — an allowlist of
    noise with default-keep, so anything unexpected on the subprocess stderr (a
    real trace or error) is NOT matched and still surfaces to the user. Callers
    should log the dropped lines rather than discard them silently.
    """
    kept: list[str] = []
    dropped: list[str] = []
    for warning in warnings:
        if _NODE_RUNTIME_WARNING_RE.search(warning):
            dropped.append(warning)
        else:
            kept.append(warning)
    return kept, dropped


class AssistantProviderError(RuntimeError):
    """Raised when an assistant provider cannot complete a request."""

    def __init__(
        self,
        message: str,
        *,
        code: str = "provider_error",
        detail: str | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code.strip() or "provider_error"
        self.detail = (detail or message).strip()


@dataclass(frozen=True)
class AssistantProviderContext:
    """Server-side context passed to assistant providers."""

    conversation_id: str
    program_loaded: bool
    cycle_state: str
    request_id: str = ""
    assistant_mode: str | None = None
    schema_version: str = "0.1.0"
    tool_catalog: list[dict[str, Any]] = field(default_factory=list)
    field_policy: dict[str, Any] = field(default_factory=dict)
    # Rich per-node descriptors (id, kind, childrenMin, childrenMax,
    # description, parameters, requiredCapabilities, ...) for nodes
    # visible in this assistant mode. The same shape `catalog_read`
    # returns. Forwarded to the assistant provider as `nodeCatalog`
    # so the model can plan from the same data structure it gets back
    # from a tool call — single source of truth between the preamble
    # and live tool calls.
    visible_node_catalog: list[dict[str, Any]] = field(default_factory=list)
    # Full skill payloads (the same shape skills_read returns, with
    # bodies). Forwarded to the assistant provider as `skillCatalog`.
    # Same single-source-of-truth principle as visible_node_catalog.
    visible_skill_catalog: list[dict[str, Any]] = field(default_factory=list)
    # Snapshots of the current draft program tree and scene resources at
    # request-time. Auto-injected so the LLM can plan straight from the
    # actual state without round-tripping through `program_read` and
    # `scene_inspect` first — saves 2 turns on every request, big win for
    # thinking-on (~30s/turn) and noticeable for thinking-off too.
    # Models are still expected to call program_read / scene_inspect
    # explicitly if they need to refresh state mid-task (e.g. after a
    # mutation in this same request); the schema descriptions are
    # updated to make that pattern clear.
    # Optional — None when no program/scene is loaded; consumers fall
    # back to the existing tool-call path. Added 2026-05-06.
    program_snapshot: dict[str, Any] | None = None
    scene_snapshot: dict[str, Any] | None = None


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
    draft_mutated: bool = False
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
        request_id = context.request_id or f"mf-{uuid.uuid4().hex[:12]}"
        payload = {
            "version": "manyforge.assistant.provider_request.v0",
            "requestId": request_id,
            "providerId": self.provider_id,
            "conversationId": context.conversation_id,
            "message": request.message,
            "requestedTools": list(request.requested_tools),
            "fieldPolicy": dict(context.field_policy),
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
                "fieldPolicy": dict(context.field_policy),
                "allowedToolCallStatuses": [
                    "proposed",
                    "skipped",
                    "completed",
                    "failed",
                ],
            },
        }
        if context.assistant_mode:
            # The wire envelope carries rich payloads only. Three
            # parallel rich lists at the catalog level:
            #
            #   tools         — the top-level `tools` field above
            #                   (full descriptors with id, description,
            #                   effect, inputSchema). Consumers derive
            #                   ids from `tools[*].id`.
            #   nodeCatalog   — full catalog entries for nodes visible
            #                   in this mode. Consumers derive ids
            #                   from `nodeCatalog[*].id`.
            #   skillCatalog  — full skill payloads (with bodies). Same
            #                   convention.
            #
            # The legacy bare-id lists (`catalog.tools`, `catalog.nodes`,
            # `catalog.skills`, top-level `nodes`, top-level `skills`)
            # were dropped to remove the source-of-truth duplication.
            # programSnapshot + sceneSnapshot carry the live program
            # tree and scene state at request-time so the LLM can plan
            # without round-tripping through `program_read` /
            # `scene_inspect`.
            # Wire envelope keeps catalog metadata at the top level
            # only. The legacy nested `catalog` block was retained for
            # one transitional cycle as `{nodeCatalog, skillCatalog}`,
            # but since both fields are byte-identical to their
            # top-level counterparts, the nested block was pure
            # duplication. Dropped here for single-source-of-truth.
            payload.update(
                {
                    "schemaVersion": context.schema_version,
                    "assistantMode": context.assistant_mode,
                    "nodeCatalog": list(context.visible_node_catalog),
                    "skillCatalog": list(context.visible_skill_catalog),
                    "programSnapshot": context.program_snapshot,
                    "sceneSnapshot": context.scene_snapshot,
                }
            )
        return payload

    def cancel(self, request_id: str) -> dict[str, Any]:
        """Ask the external bridge to cancel a running request."""
        normalized_request_id = request_id.strip()
        if not normalized_request_id:
            raise AssistantProviderError(
                "NemoClaw assistant cancel request_id is required",
                code="invalid_request",
            )
        try:
            import httpx
        except ModuleNotFoundError as exc:
            raise AssistantProviderError(
                "httpx is required for the NemoClaw assistant provider",
                code="missing_dependency",
            ) from exc

        headers = {"Content-Type": "application/json"}
        if self._api_token:
            headers["Authorization"] = f"Bearer {self._api_token}"
        cancel_url = f"{self.endpoint_url}/{quote(normalized_request_id, safe='')}/cancel"
        try:
            response = httpx.post(
                cancel_url,
                headers=headers,
                timeout=min(self._timeout_s, 5.0),
            )
            response.raise_for_status()
            data = response.json()
        except httpx.TimeoutException as exc:
            raise AssistantProviderError(
                f"NemoClaw assistant cancel timed out after {min(self._timeout_s, 5.0):.3f}s",
                code="timeout",
            ) from exc
        except httpx.HTTPStatusError as exc:
            raise AssistantProviderError(
                "NemoClaw assistant cancel returned HTTP "
                f"{exc.response.status_code}: {exc.response.text[:500]}",
                code="http_error",
            ) from exc
        except ValueError as exc:
            raise AssistantProviderError(
                "NemoClaw assistant cancel returned a non-JSON response",
                code="malformed_response",
            ) from exc
        if not isinstance(data, dict):
            raise AssistantProviderError(
                "NemoClaw assistant cancel response must be an object",
                code="malformed_response",
            )
        return data

    def _post_payload(self, payload: dict[str, Any]) -> dict[str, Any]:
        try:
            import httpx
        except ModuleNotFoundError as exc:
            raise AssistantProviderError(
                "httpx is required for the NemoClaw assistant provider",
                code="missing_dependency",
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
                f"NemoClaw assistant timed out after {self._timeout_s:.3f}s",
                code="timeout",
            ) from exc
        except httpx.HTTPStatusError as exc:
            provider_error = _error_from_response_json(exc.response)
            if provider_error is not None:
                raise provider_error from exc
            raise AssistantProviderError(
                "NemoClaw assistant returned HTTP "
                f"{exc.response.status_code}: {exc.response.text[:500]}",
                code="http_error",
            ) from exc
        except ValueError as exc:
            raise AssistantProviderError(
                "NemoClaw assistant returned a non-JSON response",
                code="malformed_response",
            ) from exc
        if not isinstance(data, dict):
            raise AssistantProviderError(
                "NemoClaw assistant response must be an object",
                code="malformed_response",
            )
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
            raise AssistantProviderError(message, code=code, detail=detail or message)
        if bool(data.get("mutated")):
            raise AssistantProviderError(
                "NemoClaw assistant responses must not report state mutation"
            )

        raw_message = data.get("message") or data.get("response") or ""
        message = _flatten_provider_message(raw_message).strip()
        if not message:
            raise AssistantProviderError("NemoClaw assistant response.message is required")

        proposals = _normalize_provider_proposals(data.get("proposals", []))
        tool_calls = _normalize_provider_tool_calls(data.get("toolCalls", data.get("tool_calls", [])))
        _validate_provider_tool_calls_in_catalog(tool_calls, context)
        warnings = _normalize_string_list(data.get("warnings", []), field_name="warnings")
        warnings, runtime_noise = _partition_runtime_noise(warnings)
        if runtime_noise:
            logger.debug(
                "Suppressed %d benign Node-runtime warning(s) from the assistant "
                "warnings channel: %s",
                len(runtime_noise),
                runtime_noise,
            )
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
            draft_mutated=bool(data.get("draftMutated", data.get("draft_mutated", False))),
        )


# =========================================================================
# LANE_REGISTRY — per THREE-LANE-MIGRATION-PLAN.md §8 Phase 1.
# =========================================================================
# Single source of truth for which provider ids the Composer accepts.
# Keyed by provider id (lower-case); each entry describes the lane.
# Phase 1 keeps the existing two providers fully wired; ``hermes`` is
# registered but marked ``inert=True`` until Phase 4 lands the bridge.
# Eliminates the "unknown provider falls back silently" foot-gun — any
# id outside this registry now raises AssistantProviderError with a
# clear message including the accepted ids.
# Structured metadata enums for LANE_REGISTRY entries. These are stable
# fields callers (tests, UI, docs, audit pipelines) can rely on without
# parsing prose. New lanes MUST pick a value from these sets — typos
# raise ImportError at module load instead of producing silently-wrong
# behavior. Reference: external LLM review of three-lane-migration
# branch on 2026-06-03 (suggestion: "tests assert metadata, not prose").
_LANE_RUNTIMES: frozenset[str] = frozenset({"nemoclaw"})
_LANE_ROUTINGS: frozenset[str] = frozenset(
    {"direct-vllm", "openclaw-gateway", "hermes-sessions"}
)
_LANE_ISOLATIONS: frozenset[str] = frozenset({"sandbox"})


def _validate_lane_entry(lane_id: str, entry: dict[str, Any]) -> None:
    """Validate a LANE_REGISTRY entry's structured-metadata fields.

    Called once at module import for every entry. Catches enum typos
    (e.g. "sandobx") immediately rather than letting them surface
    later as confusing test assertion failures or silent UI bugs.
    """
    runtime = entry.get("runtime")
    routing = entry.get("routing")
    isolation = entry.get("isolation")
    if runtime not in _LANE_RUNTIMES:
        raise ValueError(
            f"LANE_REGISTRY[{lane_id!r}].runtime={runtime!r} not in {sorted(_LANE_RUNTIMES)}"
        )
    if routing not in _LANE_ROUTINGS:
        raise ValueError(
            f"LANE_REGISTRY[{lane_id!r}].routing={routing!r} not in {sorted(_LANE_ROUTINGS)}"
        )
    if isolation not in _LANE_ISOLATIONS:
        raise ValueError(
            f"LANE_REGISTRY[{lane_id!r}].isolation={isolation!r} not in {sorted(_LANE_ISOLATIONS)}"
        )


LANE_REGISTRY: dict[str, dict[str, Any]] = {
    # Canonical id for the Direct lane is "direct" per the
    # THREE-LANE-MIGRATION-PLAN. The "direct" name refers to direct
    # vLLM endpoint dispatch (bypassing the OpenClaw agent path), NOT
    # direct host execution — components still run inside the NemoClaw
    # sandbox. ``runtime`` and ``isolation`` carry that safety signal
    # so it is queryable by tests/UI/audit, not hidden in prose.
    # ``nemoclaw`` remains an accepted alias for the same lane (see
    # LANE_ID_ALIASES below) — meaningful, not a tombstone, because
    # the sandbox-isolation property it described still holds.
    "direct": {
        "default_bridge_port": 8100,
        "transport_class": "NemoClawAssistantProvider",
        "lane_name": "direct",
        "inert": False,
        "aliases": ("nemoclaw",),
        "runtime": "nemoclaw",
        "routing": "direct-vllm",
        "isolation": "sandbox",
        "description": (
            "Direct-vLLM routing lane. Runs inside the NemoClaw sandbox "
            "(same isolation boundary as the OpenClaw lane); 'direct' "
            "refers to direct vLLM endpoint dispatch, NOT direct host "
            "execution. Bridge on :8100."
        ),
    },
    "openclaw": {
        "default_bridge_port": 8200,
        "transport_class": "NemoClawAssistantProvider",
        "lane_name": "openclaw",
        "inert": False,
        "aliases": (),
        "runtime": "nemoclaw",
        "routing": "openclaw-gateway",
        "isolation": "sandbox",
        "description": (
            "OpenClaw gateway lane with discovery-protocol skill. "
            "Runs inside the NemoClaw sandbox. Bridge on :8200."
        ),
    },
    "hermes": {
        "default_bridge_port": 8300,
        "transport_class": "HermesAssistantProvider",  # Phase 4 — landed
        "lane_name": "hermes",
        # Phase 4 landed: the Hermes bridge (manyforge/lanes/hermes/) implements
        # the AssistantTransport against Hermes' native /v1/runs API. Activated
        # at the launcher behind HERMES_LANE_PHASE4_ENABLED (the empirical
        # bake-off gate is run with that flag set).
        "inert": False,
        "aliases": (),
        "runtime": "nemoclaw",
        "routing": "hermes-sessions",
        "isolation": "sandbox",
        "description": (
            "Hermes Agents lane with native mcp_servers + /v1/runs sessions "
            "API (memory/skills/cron/todo/delegation enabled). Runs inside the "
            "NemoClaw sandbox. Bridge on :8300."
        ),
    },
}

# Validate every entry at module import. Enum typos fail loudly here
# rather than silently passing through to UI/tests.
for _lane_id, _lane_entry in LANE_REGISTRY.items():
    _validate_lane_entry(_lane_id, _lane_entry)

# Legacy alias map: alias → canonical lane id. The launcher's
# scripts/lib/assistant.sh mirrors this map. Aliases carry meaningful
# semantics (e.g. "nemoclaw" → "direct" because the lane runs inside
# the NemoClaw sandbox), not deprecation — see LANE_REGISTRY[...].aliases
# for the inverse relation and the lane's structured metadata for the
# property the alias was naming.
LANE_ID_ALIASES: dict[str, str] = {
    "nemoclaw": "direct",
}


class HermesAssistantProvider(NemoClawAssistantProvider):
    """Hermes lane provider (Phase 4).

    The Hermes bridge (NemoClaw-Thor ``manyforge/lanes/hermes/``) speaks the
    same ManyForge provider-request envelope as the Direct/OpenClaw bridges, so
    the Composer-side HTTP transport is identical — this subclass exists for
    audit clarity (``provider_id="hermes"``) and a longer default client
    timeout: the Hermes bridge runs a *full agent loop* (memory/skills/tools)
    per turn over ``/v1/runs``, not a single round-trip, so the default timeout
    is higher than the per-turn lanes'. The deployment's
    ``model_endpoints.<alias>.default_timeout_s`` still overrides when set.
    """

    def __init__(
        self,
        *,
        endpoint_url: str,
        provider_id: str = "hermes",
        timeout_s: float = 600.0,
        api_token: str | None = None,
        request_fn: Callable[[dict[str, Any]], dict[str, Any]] | None = None,
    ) -> None:
        super().__init__(
            endpoint_url=endpoint_url,
            provider_id=provider_id,
            timeout_s=timeout_s,
            api_token=api_token,
            request_fn=request_fn,
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

    Other provider ids are resolved against ``LANE_REGISTRY``; unknown
    ids raise AssistantProviderError naming the accepted set, so a typo
    surfaces immediately instead of silently falling back to Direct
    (the pre-Phase-1 behavior at launcher's ``assistant.sh``).
    """
    normalized = (provider_id or "").strip().lower()
    if normalized in {"", "none", "mock"}:
        return None
    if normalized in {"deterministic", "deterministic-scene-authoring"}:
        return DeterministicSceneAuthoringProvider()

    # Resolve legacy aliases (e.g. nemoclaw → direct) before consulting
    # the registry. The original provider_id is preserved in the
    # NemoClawAssistantProvider for audit-log compatibility.
    resolved = LANE_ID_ALIASES.get(normalized, normalized)
    lane_entry = LANE_REGISTRY.get(resolved)
    if lane_entry is not None and lane_entry.get("inert"):
        raise AssistantProviderError(
            f"Provider '{provider_id}' is registered but inert in this build "
            f"({lane_entry.get('description', '')}). Implement the transport class "
            f"before selecting it as the active provider."
        )
    if resolved in {"direct", "openclaw"}:
        # NemoClawAssistantProvider handles both the Direct lane (bridge
        # on :8100) and the OpenClaw lane (bridge on :8200). The
        # endpoint URL differs but the transport class is the same.
        return NemoClawAssistantProvider(
            endpoint_url=endpoint_url or _assistant_endpoint_from_env(),
            provider_id=normalized,  # preserve original id for audit
            timeout_s=timeout_s if timeout_s is not None else _assistant_timeout_from_env(),
            api_token=api_token or os.environ.get("MANYFORGE_ASSISTANT_API_TOKEN"),
        )
    if resolved == "hermes":
        # Hermes lane (bridge on :8300). Same envelope/transport as above;
        # HermesAssistantProvider just carries the hermes provider_id and a
        # longer default timeout for Hermes' per-turn agent loop. The endpoint
        # URL comes from the deployment's model_endpoint (or the env default).
        return HermesAssistantProvider(
            endpoint_url=endpoint_url or _assistant_endpoint_from_env(),
            timeout_s=timeout_s if timeout_s is not None else _assistant_timeout_from_env(),
            api_token=api_token or os.environ.get("MANYFORGE_ASSISTANT_API_TOKEN"),
        )
    accepted = sorted(
        set(LANE_REGISTRY)
        | set(LANE_ID_ALIASES)
        | {"mock", "none", "deterministic", "deterministic-scene-authoring"}
    )
    raise AssistantProviderError(
        f"Unsupported assistant provider: {provider_id!r}. "
        f"Accepted ids: {accepted}"
    )


def _error_from_response_json(response: Any) -> AssistantProviderError | None:
    try:
        data = response.json()
    except ValueError:
        return None
    if not isinstance(data, dict):
        return None
    provider_error = data.get("error")
    if not isinstance(provider_error, dict):
        return None
    code = str(provider_error.get("code") or "provider_error").strip()
    detail = str(provider_error.get("detail") or "").strip()
    message = f"NemoClaw assistant rejected request: {code}"
    if detail:
        message += f": {detail}"
    return AssistantProviderError(
        message,
        code=code,
        detail=detail or message,
    )


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
        # The three engine-backed lanes below carry structured metadata
        # fields (runtime/routing/isolation/aliases) sourced from
        # LANE_REGISTRY. Tests and UI should assert on these, NOT on
        # description prose — prose drifts as wording is polished, the
        # metadata is the stable contract. See LANE_REGISTRY in this
        # module for the source of truth and _LANE_RUNTIMES/_LANE_ROUTINGS/
        # _LANE_ISOLATIONS for the allowed enum values.
        {
            "id": "direct",
            "aliases": list(LANE_REGISTRY["direct"]["aliases"]),
            "runtime": LANE_REGISTRY["direct"]["runtime"],
            "routing": LANE_REGISTRY["direct"]["routing"],
            "isolation": LANE_REGISTRY["direct"]["isolation"],
            "description": LANE_REGISTRY["direct"]["description"] + (
                " The in-tree manyforge_assistant_bridge runs the agent "
                "loop in-process against the configured vLLM endpoint. "
                "Sub-2s warm latency; current known-good demo baseline "
                "and fallback when the OpenClaw lane is unhealthy."
            ),
            # Match against both the canonical id and every registered
            # alias so `configured_id="nemoclaw"` and `"direct"` both
            # resolve true. Single catalog row keeps the contract
            # unambiguous (one row per lane), while aliases preserve
            # caller intent. configuredProviderId at the API surface
            # still returns the original requested id for audit.
            "configured": configured_id == "direct"
            or configured_id in LANE_REGISTRY["direct"]["aliases"],
            "requiresLiveModel": True,
            "requiresLiveIsaacSim": False,
        },
        {
            "id": "openclaw",
            "aliases": list(LANE_REGISTRY["openclaw"]["aliases"]),
            "runtime": LANE_REGISTRY["openclaw"]["runtime"],
            "routing": LANE_REGISTRY["openclaw"]["routing"],
            "isolation": LANE_REGISTRY["openclaw"]["isolation"],
            "description": LANE_REGISTRY["openclaw"]["description"] + (
                " The NemoClaw-Thor openclaw_assistant_bridge translates "
                "the ManyForge assistant HTTP contract into "
                "/v1/chat/completions calls against the OpenClaw gateway. "
                "The gateway runs the agent loop and the manyforge MCP "
                "server; ManyForge still enforces every tool call via "
                "/api/assistant/bridge/tools/{toolId}. Warm latency 5-15s."
            ),
            "configured": configured_id == "openclaw",
            "requiresLiveModel": True,
            "requiresLiveIsaacSim": False,
        },
        {
            "id": "hermes",
            "aliases": list(LANE_REGISTRY["hermes"]["aliases"]),
            "runtime": LANE_REGISTRY["hermes"]["runtime"],
            "routing": LANE_REGISTRY["hermes"]["routing"],
            "isolation": LANE_REGISTRY["hermes"]["isolation"],
            "description": LANE_REGISTRY["hermes"]["description"] + (
                " The NemoClaw-Thor lanes/hermes bridge submits each turn as a "
                "Hermes /v1/runs run and observes its lifecycle event stream; "
                "Hermes owns the agent loop and fires manyforge MCP tools, which "
                "ManyForge still enforces via /api/assistant/bridge/tools/{toolId}."
            ),
            "configured": configured_id == "hermes",
            "requiresLiveModel": True,
            "requiresLiveIsaacSim": False,
            # Sourced from the registry so the row tracks activation state
            # rather than hardcoding it (flipped to False when Phase 4 landed).
            "inert": LANE_REGISTRY["hermes"]["inert"],
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


def _flatten_provider_message(value: Any) -> str:
    """Coerce a provider's ``message`` field into plain text.

    Providers can send the message as a flat string, as a list of typed
    content blocks (Anthropic-style ``[{"type": "text", "text": "..."}]``),
    or as a single block dict. Without normalisation, ``str(value)`` on
    a list emitted the Python ``repr`` of the list verbatim into the
    chat surface — single quotes, embedded ``\\n`` literals, and all.
    Mirrors the bridge-side flattener in ``openclaw_assistant_bridge``;
    the duplication is intentional so a non-OpenClaw provider that
    emits the same shape is also handled.
    """
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        text = value.get("text")
        if isinstance(text, str):
            return text
        return ""
    if isinstance(value, list):
        parts: list[str] = []
        for block in value:
            if isinstance(block, str):
                parts.append(block)
                continue
            if not isinstance(block, dict):
                continue
            block_type = block.get("type")
            text = block.get("text")
            if isinstance(text, str) and block_type in (None, "text"):
                parts.append(text)
        return "".join(parts)
    return ""


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
