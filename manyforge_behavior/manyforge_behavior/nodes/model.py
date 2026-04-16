"""Non-blocking model query node for ManyForge behavior tree (OP-7).

QueryModelNode is a leaf node that queries a deployment-configured model
endpoint using a versioned prompt template.  It is designed for 100Hz tick
loop compatibility: the actual model call runs in a background thread via
ThreadPoolExecutor, and update() checks the future status without blocking.

Blackboard writes are atomic at the namespace level — all validated output
fields are applied together or not at all.
"""

from __future__ import annotations

import base64
import copy
import hashlib
import json
import logging
import math
import os
import re
import time
from concurrent.futures import CancelledError, Future, ThreadPoolExecutor
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.blackboard_namespace import (
    read_blackboard_binding_value,
    write_namespace_bundle,
)

logger = logging.getLogger(__name__)

_FENCED_JSON_RE = re.compile(
    r"^\s*```(?:json)?\s*(.*?)\s*```\s*$",
    re.IGNORECASE | re.DOTALL,
)

# Shared executor — max 2 workers to avoid thread explosion in the tick loop
_MODEL_EXECUTOR: Optional[ThreadPoolExecutor] = None

# Shared httpx client — reuses TCP connections across model calls.
# Timeout is set per-request via the ``timeout`` kwarg on ``client.post()``.
_HTTP_CLIENT: Optional["httpx.Client"] = None
_HTTP_CLIENT_LOCK = __import__("threading").Lock()
_SCENE_SNAPSHOT_WAIT_TIMEOUT_S = 2.0
_PARSE_RETRY_SUFFIX = (
    "\n\nThe previous response did not satisfy the required structured output contract. "
    "Return ONLY valid structured output for this request. Do not include markdown "
    "fences, prose, or any extra text."
)


def _get_executor() -> ThreadPoolExecutor:
    global _MODEL_EXECUTOR
    if _MODEL_EXECUTOR is None:
        _MODEL_EXECUTOR = ThreadPoolExecutor(
            max_workers=2,
            thread_name_prefix="manyforge_model",
        )
    return _MODEL_EXECUTOR


def _get_http_client() -> "httpx.Client":
    """Return a module-level httpx.Client for connection pooling."""
    global _HTTP_CLIENT
    if _HTTP_CLIENT is None:
        with _HTTP_CLIENT_LOCK:
            if _HTTP_CLIENT is None:
                import httpx
                _HTTP_CLIENT = httpx.Client(
                    timeout=60.0,  # generous default; per-request overrides below
                    limits=httpx.Limits(
                        max_connections=4,
                        max_keepalive_connections=2,
                    ),
                )
    return _HTTP_CLIENT


def _call_model(
    *,
    provider: str,
    model_id: str,
    base_url: Optional[str],
    api_key_env: Optional[str],
    prompt: str,
    max_tokens: int,
    timeout_s: float,
    output_schema: Optional[dict] = None,
    response_tool: Optional[dict[str, Any]] = None,
    scene_snapshot_bytes: Optional[bytes] = None,
    scene_snapshot_url: Optional[str] = None,
    scene_snapshot_format: str = "jpeg",
) -> dict[str, Any]:
    """Execute a model call synchronously (runs in thread pool).

    Returns a dict with:
      - "text": raw response text
      - "parsed": parsed JSON if response is valid JSON, else None
      - "model_version": model version string if available
      - "usage": token usage dict if available
    """
    api_key = os.environ.get(api_key_env, "") if api_key_env else ""

    if provider in ("openai", "local", "anthropic"):
        import httpx
        headers: dict[str, str] = {"Content-Type": "application/json"}
        body: dict[str, Any] = {
            "model": model_id,
            "max_tokens": max_tokens,
        }
        media_type = "image/png" if scene_snapshot_format.lower() == "png" else "image/jpeg"
        image_b64 = (
            base64.b64encode(scene_snapshot_bytes).decode("ascii")
            if scene_snapshot_bytes is not None and scene_snapshot_url is None
            else None
        )

        if provider in ("openai", "local"):
            url = (base_url or "https://api.openai.com/v1") + "/chat/completions"
            if api_key:
                headers["Authorization"] = f"Bearer {api_key}"
            if image_b64 is not None or scene_snapshot_url is not None:
                body["messages"] = [{
                    "role": "user",
                    "content": [
                        {"type": "text", "text": prompt},
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": (
                                    scene_snapshot_url
                                    if scene_snapshot_url is not None
                                    else f"data:{media_type};base64,{image_b64}"
                                ),
                            },
                        },
                    ],
                }]
            else:
                body["messages"] = [{"role": "user", "content": prompt}]
            if response_tool is not None:
                body["tools"] = [{
                    "type": "function",
                    "function": response_tool,
                }]
                body["tool_choice"] = "required"
            elif output_schema:
                body["response_format"] = {"type": "json_object"}
        else:  # anthropic
            url = (base_url or "https://api.anthropic.com/v1") + "/messages"
            headers["x-api-key"] = api_key
            headers["anthropic-version"] = "2023-06-01"
            if response_tool is not None:
                raise ValueError("response_tool is only supported for openai/local providers")
            if scene_snapshot_url is not None:
                raise ValueError(
                    "scene_snapshot_url transport is only supported for openai/local providers"
                )
            if image_b64 is not None:
                body["messages"] = [{
                    "role": "user",
                    "content": [
                        {
                            "type": "image",
                            "source": {
                                "type": "base64",
                                "media_type": media_type,
                                "data": image_b64,
                            },
                        },
                        {"type": "text", "text": prompt},
                    ],
                }]
            else:
                body["messages"] = [{"role": "user", "content": prompt}]

        client = _get_http_client()
        resp = client.post(
            url, json=body, headers=headers,
            timeout=timeout_s,
        )
        resp.raise_for_status()
        data = resp.json()

        # Extract text from response
        if provider in ("openai", "local"):
            message = data.get("choices", [{}])[0].get("message", {})
            text = message.get("content", "")
            tool_calls = message.get("tool_calls", [])
            model_version = data.get("model", model_id)
            usage = data.get("usage", {})
        else:
            content_blocks = data.get("content", [])
            text = "".join(
                b.get("text", "") for b in content_blocks if b.get("type") == "text"
            )
            tool_calls = []
            model_version = data.get("model", model_id)
            usage = data.get("usage", {})

        tool_call_payload: Optional[dict[str, Any]] = None
        if response_tool is not None and tool_calls:
            first_call = tool_calls[0]
            function_payload = first_call.get("function", {})
            arguments_text = function_payload.get("arguments", "")
            parsed = _parse_json_text(arguments_text)
            tool_call_payload = {
                "name": function_payload.get("name", ""),
                "arguments_text": arguments_text,
                "id": first_call.get("id"),
                "raw": first_call,
            }
        else:
            parsed = _parse_json_text(text)

        return {
            "text": text,
            "parsed": parsed,
            "model_version": model_version,
            "tool_call": tool_call_payload,
            "usage": {
                "input_tokens": usage.get("input_tokens") or usage.get("prompt_tokens", 0),
                "output_tokens": usage.get("output_tokens") or usage.get("completion_tokens", 0),
            },
        }
    else:
        raise ValueError(f"Unsupported model provider: {provider}")


def _parse_json_text(text: Any) -> Optional[Any]:
    """Parse JSON text, tolerating fenced ```json wrappers from compliant-ish models."""
    if not isinstance(text, str):
        return None

    candidates = [text]
    fenced = _FENCED_JSON_RE.match(text)
    if fenced:
        candidates.append(fenced.group(1))

    for candidate in candidates:
        try:
            return json.loads(candidate)
        except (json.JSONDecodeError, TypeError):
            continue
    return None


def _stringify_template_value(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=True)
    return str(value)


def _render_prompt_template(template_text: str, template_variables: dict[str, str]) -> str:
    """Render ``{{ variable }}`` placeholders with optional inner whitespace."""
    rendered = template_text
    for var_name, var_value in template_variables.items():
        pattern = re.compile(r"\{\{\s*" + re.escape(var_name) + r"\s*\}\}")
        rendered = pattern.sub(var_value, rendered)
    return rendered


def _resolve_response_tool_template(
    response_tool: Optional[dict[str, Any]],
    template_variables: dict[str, Any],
) -> Optional[dict[str, Any]]:
    if response_tool is None:
        return None
    if not isinstance(response_tool, dict):
        raise ValueError("response_tool must be a mapping")

    resolved = copy.deepcopy(response_tool)
    name = resolved.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError("response_tool.name must be a non-empty string")

    description = resolved.get("description", "")
    if description is not None and not isinstance(description, str):
        raise ValueError("response_tool.description must be a string")

    parameters = resolved.get("parameters")
    if parameters is not None and not isinstance(parameters, dict):
        raise ValueError("response_tool.parameters must be a mapping when provided")

    resolved["parameters"] = _resolve_output_schema_template(
        parameters,
        template_variables,
    )
    return resolved


def _resolve_output_schema_template(
    schema: Optional[dict[str, Any]],
    template_variables: dict[str, Any],
) -> Optional[dict[str, Any]]:
    if schema is None:
        return None

    resolved = copy.deepcopy(schema)

    def _walk(node: Any) -> Any:
        if isinstance(node, dict):
            resolved_node = {key: _walk(value) for key, value in node.items()}
            enum_var = resolved_node.pop("x-manyforge-enum-template-variable", None)
            allow_empty_string = bool(
                resolved_node.pop("x-manyforge-enum-allow-empty-string", False)
            )
            if enum_var is not None:
                enum_source = template_variables.get(str(enum_var))
                if not isinstance(enum_source, list):
                    raise ValueError(
                        "x-manyforge-enum-template-variable must resolve to a list, "
                        f"got {type(enum_source).__name__}"
                    )
                if not enum_source:
                    raise ValueError(
                        "x-manyforge-enum-template-variable resolved to an empty list"
                    )
                enum_values: list[Any] = []
                for item in enum_source:
                    if isinstance(item, (str, int, float, bool)) and not (
                        isinstance(item, float) and not math.isfinite(item)
                    ):
                        enum_values.append(item)
                    else:
                        raise ValueError(
                            "x-manyforge-enum-template-variable items must be finite "
                            "JSON scalars"
                        )
                if allow_empty_string and "" not in enum_values:
                    enum_values.append("")
                resolved_node["enum"] = enum_values
            return resolved_node
        if isinstance(node, list):
            return [_walk(item) for item in node]
        return node

    return _walk(resolved)


class QueryModelNode(py_trees.behaviour.Behaviour):
    """Non-blocking leaf node that queries a model endpoint.

    Constructor args are injected by the runner at build time.
    """

    def __init__(
        self,
        name: str,
        *,
        model_endpoint_alias: str,
        prompt_template_id: str,
        template_variables: Optional[dict[str, Any]] = None,
        output_key_prefix: str,
        timeout_s: float = 30.0,
        min_confidence: float = 0.0,
        retry_on_parse_failure: bool = False,
        include_scene_snapshot: bool = False,
        scene_snapshot_transport: str = "inline_data_url",
        scene_snapshot_width: int = 640,
        scene_snapshot_height: int = 480,
        scene_snapshot_format: str = "jpeg",
        # Injected by runner — not from program params
        endpoint: Any = None,
        template: Any = None,
        event_recorder: Any = None,
        audit_policy: Any = None,
        scene_snapshot_provider: Optional[
            Callable[[int, int, str], Optional[bytes]]
        ] = None,
        scene_snapshot_url_publisher: Optional[
            Callable[..., Optional[dict[str, Any]]]
        ] = None,
        # Execution context — injected by runner for audit correlation
        run_id: Optional[str] = None,
        session_id: Optional[str] = None,
        principal_id: Optional[str] = None,
        parent_run_id: Optional[str] = None,
    ):
        super().__init__(name=name)
        self._model_endpoint_alias = model_endpoint_alias
        self._prompt_template_id = prompt_template_id
        self._template_variables = dict(template_variables or {})
        self._output_key_prefix = output_key_prefix
        self._timeout_s = timeout_s
        self._min_confidence = min_confidence
        self._retry_on_parse_failure = retry_on_parse_failure
        self._include_scene_snapshot = include_scene_snapshot
        self._scene_snapshot_transport = scene_snapshot_transport
        self._scene_snapshot_width = scene_snapshot_width
        self._scene_snapshot_height = scene_snapshot_height
        self._scene_snapshot_format = scene_snapshot_format
        self._endpoint = endpoint
        self._template = template
        self._event_recorder = event_recorder
        self._audit_policy = audit_policy
        self._scene_snapshot_provider = scene_snapshot_provider
        self._scene_snapshot_url_publisher = scene_snapshot_url_publisher
        self._run_id = run_id
        self._session_id = session_id
        self._principal_id = principal_id
        self._parent_run_id = parent_run_id
        self._future: Optional[Future] = None
        self._call_start_ns: int = 0
        self._resolved_prompt: str = ""
        self._resolved_output_schema: Optional[dict[str, Any]] = None
        self._resolved_response_tool: Optional[dict[str, Any]] = None
        self._resolved_template_vars_raw: dict[str, Any] = {}
        self._awaiting_scene_snapshot = False
        self._scene_snapshot_wait_started_ns: int = 0
        self._skip_snapshot_retry_once = False
        self._parse_retry_attempted = False
        self._call_generation = 0
        self._active_call_generation: Optional[int] = None
        self._terminated_call_generation: Optional[int] = None
        self._last_scene_snapshot_bytes: Optional[bytes] = None
        self._last_scene_snapshot_url: Optional[str] = None
        self._last_scene_snapshot_metadata: Optional[dict[str, Any]] = None
        self._bb = py_trees.blackboard.Client(name=f"query_model_{name}")
        self._output_keys_registered = False

    def _ensure_output_keys(self) -> None:
        if self._output_keys_registered:
            return
        self._bb.register_key(
            key=self._output_key_prefix,
            access=py_trees.common.Access.WRITE,
        )
        self._bb.register_key(
            key="stop_requested",
            access=py_trees.common.Access.READ,
        )
        self._output_keys_registered = True

    def _stop_requested(self) -> bool:
        try:
            return bool(self._bb.get("stop_requested"))
        except KeyError:
            return False

    def _event_context(self) -> dict[str, Any]:
        """Return execution context kwargs for record_model_event calls."""
        ctx: dict[str, Any] = {}
        if self._run_id is not None:
            ctx["run_id"] = self._run_id
        if self._session_id is not None:
            ctx["session_id"] = self._session_id
        if self._principal_id is not None:
            ctx["principal_id"] = self._principal_id
        if self._parent_run_id is not None:
            ctx["parent_run_id"] = self._parent_run_id
        return ctx

    def _maybe_prepare_scene_snapshot(
        self,
    ) -> tuple[Optional[bytes], Optional[str], Optional[dict[str, Any]], Optional[str], bool]:
        """Try to prepare an optional scene snapshot without blocking for availability."""
        if not self._include_scene_snapshot:
            return None, None, None, None, True

        provider = self._scene_snapshot_provider
        if provider is None:
            return (
                None,
                None,
                None,
                "scene snapshot requested but no HMI render provider configured",
                False,
            )

        try:
            scene_snapshot_bytes = provider(
                self._scene_snapshot_width,
                self._scene_snapshot_height,
                self._scene_snapshot_format,
            )
        except Exception as exc:
            return (
                None,
                None,
                None,
                f"scene_snapshot_unavailable: {exc}",
                False,
            )
        if scene_snapshot_bytes is None:
            return None, None, None, None, False

        scene_snapshot_metadata: dict[str, Any] = {
            "format": self._scene_snapshot_format,
            "width": self._scene_snapshot_width,
            "height": self._scene_snapshot_height,
            "sha256": hashlib.sha256(scene_snapshot_bytes).hexdigest(),
            "byte_size": len(scene_snapshot_bytes),
            "transport": self._scene_snapshot_transport,
        }
        scene_snapshot_url: Optional[str] = None
        if self._scene_snapshot_transport == "http_url":
            publisher = self._scene_snapshot_url_publisher
            if publisher is None:
                return (
                    None,
                    None,
                    None,
                    "scene_snapshot_url_publisher_unavailable",
                    False,
                )
            try:
                published = publisher(
                    scene_snapshot_bytes,
                    self._scene_snapshot_width,
                    self._scene_snapshot_height,
                    self._scene_snapshot_format,
                    fetch_base_url=getattr(self._endpoint, "image_fetch_base_url", None),
                    model_base_url=getattr(self._endpoint, "base_url", None),
                )
            except TypeError:
                published = publisher(
                    scene_snapshot_bytes,
                    self._scene_snapshot_width,
                    self._scene_snapshot_height,
                    self._scene_snapshot_format,
                )
            except Exception as exc:
                return None, None, None, f"scene_snapshot_publish_failed: {exc}", False
            if not isinstance(published, dict):
                return None, None, None, "scene_snapshot_publish_failed", False
            scene_snapshot_url = str(published.get("url", "") or "")
            if not scene_snapshot_url:
                return None, None, None, "scene_snapshot_publish_failed", False
            scene_snapshot_metadata["url"] = scene_snapshot_url
            if published.get("snapshot_id"):
                scene_snapshot_metadata["snapshot_id"] = published["snapshot_id"]
        elif self._scene_snapshot_transport != "inline_data_url":
            return (
                None,
                None,
                None,
                f"unsupported_scene_snapshot_transport: {self._scene_snapshot_transport}",
                False,
            )

        return scene_snapshot_bytes, scene_snapshot_url, scene_snapshot_metadata, None, True

    def _submit_model_call(
        self,
        *,
        scene_snapshot_bytes: Optional[bytes],
        scene_snapshot_url: Optional[str],
        scene_snapshot_metadata: Optional[dict[str, Any]],
        prompt_override: Optional[str] = None,
    ) -> None:
        self._call_start_ns = time.monotonic_ns()
        self._call_generation += 1
        self._active_call_generation = self._call_generation
        self._last_scene_snapshot_bytes = scene_snapshot_bytes
        self._last_scene_snapshot_url = scene_snapshot_url
        self._last_scene_snapshot_metadata = (
            dict(scene_snapshot_metadata)
            if scene_snapshot_metadata is not None
            else None
        )
        prompt_text = prompt_override if prompt_override is not None else self._resolved_prompt
        if self._event_recorder is not None:
            audit_arguments = dict(self._resolved_template_vars_raw)
            if scene_snapshot_metadata is not None:
                audit_arguments["_scene_snapshot"] = scene_snapshot_metadata
            self._event_recorder.record_model_event(
                action="started",
                model_endpoint_alias=self._model_endpoint_alias,
                prompt_template_id=self._prompt_template_id,
                prompt_template_hash=self._template.content_hash,
                prompt_text=prompt_text,
                argument_keys=list(audit_arguments.keys()),
                argument_values=audit_arguments,
                node_instance=self.name,
                audit_policy=self._audit_policy,
                **self._event_context(),
            )

        self._future = _get_executor().submit(
            _call_model,
            provider=self._endpoint.provider,
            model_id=self._endpoint.model_id,
            base_url=self._endpoint.base_url,
            api_key_env=self._endpoint.api_key_env,
            prompt=prompt_text,
            max_tokens=self._endpoint.max_tokens,
            timeout_s=self._timeout_s,
            output_schema=self._resolved_output_schema,
            response_tool=self._resolved_response_tool,
            scene_snapshot_bytes=scene_snapshot_bytes,
            scene_snapshot_url=scene_snapshot_url,
            scene_snapshot_format=self._scene_snapshot_format,
        )

    def _advance_preflight(self) -> py_trees.common.Status | None:
        scene_snapshot_bytes, scene_snapshot_url, scene_snapshot_metadata, error, ready = (
            self._maybe_prepare_scene_snapshot()
        )
        if error is not None:
            self.feedback_message = error
            self._write_namespace_output(value=None, status="failed")
            return py_trees.common.Status.FAILURE
        if not ready:
            if self._scene_snapshot_wait_started_ns == 0:
                self._scene_snapshot_wait_started_ns = time.monotonic_ns()
            elapsed_s = (
                time.monotonic_ns() - self._scene_snapshot_wait_started_ns
            ) / 1_000_000_000
            if elapsed_s >= _SCENE_SNAPSHOT_WAIT_TIMEOUT_S:
                self.feedback_message = (
                    f"scene_snapshot_unavailable_after_{_SCENE_SNAPSHOT_WAIT_TIMEOUT_S:.1f}s"
                )
                self._write_namespace_output(value=None, status="failed")
                return py_trees.common.Status.FAILURE
            self._awaiting_scene_snapshot = True
            self.feedback_message = "waiting_for_scene_snapshot"
            self._write_namespace_output(
                value=None,
                status="waiting_for_scene_snapshot",
            )
            return py_trees.common.Status.RUNNING

        self._awaiting_scene_snapshot = False
        self.feedback_message = ""
        self._submit_model_call(
            scene_snapshot_bytes=scene_snapshot_bytes,
            scene_snapshot_url=scene_snapshot_url,
            scene_snapshot_metadata=scene_snapshot_metadata,
        )
        return None

    def initialise(self) -> None:
        self._ensure_output_keys()
        self._future = None
        self._call_start_ns = 0
        self._resolved_output_schema = None
        self._resolved_response_tool = None
        self._resolved_template_vars_raw = {}
        self._awaiting_scene_snapshot = False
        self._scene_snapshot_wait_started_ns = 0
        self._skip_snapshot_retry_once = False
        self._parse_retry_attempted = False
        self._terminated_call_generation = None
        self._active_call_generation = None
        self._last_scene_snapshot_bytes = None
        self._last_scene_snapshot_url = None
        self._last_scene_snapshot_metadata = None

        if self._endpoint is None or self._template is None:
            logger.error(
                "%s: model endpoint or template not configured", self.name
            )
            return

        # Resolve template variables from blackboard
        resolved_vars_raw: dict[str, Any] = {}
        resolved_vars_prompt: dict[str, str] = {}
        for var_name, binding in self._template_variables.items():
            try:
                value = read_blackboard_binding_value(self._bb, binding)
                resolved_vars_raw[var_name] = value
                resolved_vars_prompt[var_name] = _stringify_template_value(value)
            except KeyError:
                resolved_vars_raw[var_name] = ""
                resolved_vars_prompt[var_name] = ""

        try:
            self._resolved_output_schema = _resolve_output_schema_template(
                self._template.output_schema,
                resolved_vars_raw,
            )
            self._resolved_response_tool = _resolve_response_tool_template(
                getattr(self._template, "response_tool", None),
                resolved_vars_raw,
            )
        except ValueError as exc:
            logger.error("%s: failed to resolve output contract: %s", self.name, exc)
            self.feedback_message = f"output_contract_resolution_failed: {exc}"
            return

        # Build prompt from template
        self._resolved_prompt = _render_prompt_template(
            self._template.template_text,
            resolved_vars_prompt,
        )
        self._resolved_template_vars_raw = dict(resolved_vars_raw)
        preflight_status = self._advance_preflight()
        if preflight_status == py_trees.common.Status.RUNNING:
            self._skip_snapshot_retry_once = True

    def _record_failed_model_event(
        self,
        *,
        result: Optional[dict[str, Any]],
        error: str,
        latency_ms: float,
    ) -> None:
        if self._event_recorder is None:
            return
        self._event_recorder.record_model_event(
            action="failed",
            model_endpoint_alias=self._model_endpoint_alias,
            prompt_template_id=self._prompt_template_id,
            prompt_template_hash=self._template.content_hash if self._template else None,
            response_text=result.get("text") if isinstance(result, dict) else None,
            error=error,
            latency_ms=latency_ms,
            node_instance=self.name,
            audit_policy=self._audit_policy,
            **self._event_context(),
        )

    def _retry_parse_failure(
        self,
        *,
        result: dict[str, Any],
        latency_ms: float,
        reason: str,
    ) -> bool:
        if not self._retry_on_parse_failure or self._parse_retry_attempted:
            return False
        self._parse_retry_attempted = True
        logger.warning("%s: %s; retrying once with repair prompt", self.name, reason)
        self._record_failed_model_event(
            result=result,
            error=f"{reason}; retrying_once",
            latency_ms=latency_ms,
        )
        self.feedback_message = f"{reason}; retrying_once"
        self._submit_model_call(
            scene_snapshot_bytes=self._last_scene_snapshot_bytes,
            scene_snapshot_url=self._last_scene_snapshot_url,
            scene_snapshot_metadata=self._last_scene_snapshot_metadata,
            prompt_override=self._resolved_prompt + _PARSE_RETRY_SUFFIX,
        )
        return True

    def _write_namespace_output(self, *, value: Any, status: str, **meta: Any) -> None:
        write_namespace_bundle(
            self._bb,
            self._output_key_prefix,
            value=value,
            status=status,
            **meta,
        )

    def update(self) -> py_trees.common.Status:
        if self._stop_requested():
            self.feedback_message = "stop_requested"
            if self._future is not None and not self._future.done():
                self._future.cancel()
            self._future = None
            self._active_call_generation = None
            self._write_namespace_output(value=None, status="failed")
            return py_trees.common.Status.FAILURE

        if self._future is None:
            if self._awaiting_scene_snapshot:
                if self._skip_snapshot_retry_once:
                    self._skip_snapshot_retry_once = False
                    return py_trees.common.Status.RUNNING
                status = self._advance_preflight()
                if status is not None:
                    return status
            if self._future is None:
                # Configuration or preflight error in initialise/update
                self._write_namespace_output(value=None, status="error")
                return py_trees.common.Status.FAILURE

        if self._future is None:
            self._write_namespace_output(value=None, status="error")
            return py_trees.common.Status.FAILURE

        if not self._future.done():
            return py_trees.common.Status.RUNNING

        latency_ms = (time.monotonic_ns() - self._call_start_ns) / 1_000_000
        active_generation = self._active_call_generation

        try:
            if self._future.cancelled():
                self.feedback_message = "model_call_cancelled"
                self._write_namespace_output(value=None, status="failed")
                return py_trees.common.Status.FAILURE
            result = self._future.result()
        except CancelledError:
            self.feedback_message = "model_call_cancelled"
            self._write_namespace_output(value=None, status="failed")
            return py_trees.common.Status.FAILURE
        except Exception as exc:
            logger.error("%s: model call failed: %s", self.name, exc)
            self._write_namespace_output(value=None, status="failed")
            self._record_failed_model_event(
                result=None,
                error=str(exc),
                latency_ms=latency_ms,
            )
            return py_trees.common.Status.FAILURE

        if (
            active_generation is None
            or self._active_call_generation != active_generation
            or self._terminated_call_generation == active_generation
        ):
            logger.info(
                "%s: dropping stale model result for generation %s",
                self.name,
                active_generation,
            )
            self._future = None
            return py_trees.common.Status.FAILURE

        # Validate response against output contract if provided
        parsed = result.get("parsed")
        tool_call = result.get("tool_call")
        if self._resolved_response_tool is not None:
            expected_tool_name = str(self._resolved_response_tool.get("name", ""))
            actual_tool_name = ""
            if isinstance(tool_call, dict):
                actual_tool_name = str(tool_call.get("name", "") or "")
            if actual_tool_name != expected_tool_name:
                self.feedback_message = (
                    f"tool_call_missing_or_mismatched: expected {expected_tool_name!r}, "
                    f"got {actual_tool_name!r}"
                )
                logger.error("%s: %s", self.name, self.feedback_message)
                self._write_namespace_output(value=None, status="tool_call_failed")
                self._record_failed_model_event(
                    result=result,
                    error=self.feedback_message,
                    latency_ms=latency_ms,
                )
                return py_trees.common.Status.FAILURE
            if parsed is None:
                self.feedback_message = "tool_call_arguments_not_valid_json"
                logger.error("%s: %s", self.name, self.feedback_message)
                if self._retry_parse_failure(
                    result=result,
                    latency_ms=latency_ms,
                    reason=self.feedback_message,
                ):
                    self._write_namespace_output(
                        value=None,
                        status="retrying_after_parse_failure",
                    )
                    return py_trees.common.Status.RUNNING
                self._write_namespace_output(value=None, status="tool_call_parse_failed")
                self._record_failed_model_event(
                    result=result,
                    error=self.feedback_message,
                    latency_ms=latency_ms,
                )
                return py_trees.common.Status.FAILURE
            try:
                parameters_schema = self._resolved_response_tool.get("parameters")
                if parameters_schema is not None:
                    _validate_json_schema(parsed, parameters_schema)
            except ValueError as exc:
                self.feedback_message = f"tool_schema_validation: {exc}"
                logger.error(
                    "%s: tool arguments schema validation failed: %s",
                    self.name,
                    exc,
                )
                self._write_namespace_output(value=None, status="validation_failed")
                self._record_failed_model_event(
                    result=result,
                    error=f"tool_schema_validation: {exc}",
                    latency_ms=latency_ms,
                )
                return py_trees.common.Status.FAILURE
        elif self._resolved_output_schema is not None and parsed is not None:
            try:
                _validate_json_schema(parsed, self._resolved_output_schema)
            except ValueError as exc:
                self.feedback_message = f"schema_validation: {exc}"
                logger.error(
                    "%s: response JSON schema validation failed: %s",
                    self.name,
                    exc,
                )
                self._write_namespace_output(value=None, status="validation_failed")
                self._record_failed_model_event(
                    result=result,
                    error=f"schema_validation: {exc}",
                    latency_ms=latency_ms,
                )
                return py_trees.common.Status.FAILURE
        elif self._resolved_output_schema is not None and parsed is None:
            self.feedback_message = "response is not valid JSON"
            logger.error(
                "%s: response is not valid JSON but output_schema requires it",
                self.name,
            )
            if self._retry_parse_failure(
                result=result,
                latency_ms=latency_ms,
                reason=self.feedback_message,
            ):
                self._write_namespace_output(
                    value=None,
                    status="retrying_after_parse_failure",
                )
                return py_trees.common.Status.RUNNING
            self._write_namespace_output(value=None, status="parse_failed")
            self._record_failed_model_event(
                result=result,
                error="response is not valid JSON",
                latency_ms=latency_ms,
            )
            return py_trees.common.Status.FAILURE

        if (
            self._stop_requested()
            or self._terminated_call_generation == active_generation
            or self._active_call_generation != active_generation
        ):
            self.feedback_message = "model_call_result_discarded"
            self._future = None
            return py_trees.common.Status.FAILURE

        output = parsed if parsed is not None else result.get("text", "")
        confidence = self._derive_confidence(parsed)
        write_status = "succeeded" if confidence >= self._min_confidence else "failed"
        try:
            self._write_namespace_output(
                value=output,
                status=write_status,
                confidence=confidence,
                model_version=result.get("model_version", ""),
            )
        except Exception as exc:
            self.feedback_message = f"output_commit_failed: {exc}"
            logger.error("%s: %s", self.name, self.feedback_message)
            self._record_failed_model_event(
                result=result,
                error=self.feedback_message,
                latency_ms=latency_ms,
            )
            return py_trees.common.Status.FAILURE

        # Record completed event
        if self._event_recorder is not None:
            self._event_recorder.record_model_event(
                action="completed",
                model_endpoint_alias=self._model_endpoint_alias,
                prompt_template_id=self._prompt_template_id,
                prompt_template_hash=self._template.content_hash if self._template else None,
                response_text=result.get("text"),
                response_fields=parsed if isinstance(parsed, dict) else None,
                model_version=result.get("model_version"),
                latency_ms=latency_ms,
                token_counts=result.get("usage"),
                node_instance=self.name,
                audit_policy=self._audit_policy,
                **self._event_context(),
            )

        if confidence < self._min_confidence:
            message_parts = [
                f"confidence {confidence:.2f} below threshold {self._min_confidence:.2f}"
            ]
            if isinstance(parsed, dict):
                outcome = parsed.get("outcome")
                if isinstance(outcome, str) and outcome:
                    message_parts.append(f"outcome={outcome}")
                reason = parsed.get("reason")
                if isinstance(reason, str) and reason:
                    message_parts.append(f"reason={reason}")
                request_interpretation = parsed.get("request_interpretation")
                if isinstance(request_interpretation, str) and request_interpretation:
                    message_parts.append(
                        f"request_interpretation={request_interpretation}"
                    )
            self.feedback_message = " | ".join(message_parts)
            logger.warning("%s: %s", self.name, self.feedback_message)
            return py_trees.common.Status.FAILURE

        return py_trees.common.Status.SUCCESS

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if self._active_call_generation is not None:
            self._terminated_call_generation = self._active_call_generation
        if (
            self._future is not None
            and not self._future.done()
            and new_status != py_trees.common.Status.RUNNING
        ):
            self._future.cancel()
            self._future = None
            self._active_call_generation = None

    def _derive_confidence(self, parsed: Any) -> float:
        if isinstance(parsed, dict):
            raw_confidence = parsed.get("confidence")
            if isinstance(raw_confidence, (int, float)) and not isinstance(raw_confidence, bool):
                return float(raw_confidence)
        return 1.0 if parsed is not None else 0.5


def _validate_json_schema(data: Any, schema: dict[str, Any]) -> None:
    """Minimal JSON schema validation for structured model outputs.

    Validates type, required fields, and basic property types without
    pulling in a heavy jsonschema dependency.
    """
    schema_type = schema.get("type")
    if schema_type == "object":
        if not isinstance(data, dict):
            raise ValueError(f"Expected object, got {type(data).__name__}")
        required = schema.get("required", [])
        for field_name in required:
            if field_name not in data:
                raise ValueError(f"Missing required field: {field_name}")
        properties = schema.get("properties", {})
        for prop_name, prop_schema in properties.items():
            if prop_name in data:
                _validate_json_schema(data[prop_name], prop_schema)
    elif schema_type == "array":
        if not isinstance(data, list):
            raise ValueError(f"Expected array, got {type(data).__name__}")
        items_schema = schema.get("items")
        if items_schema:
            for i, item in enumerate(data):
                try:
                    _validate_json_schema(item, items_schema)
                except ValueError as exc:
                    raise ValueError(f"Item [{i}]: {exc}") from exc
    elif schema_type == "string":
        if not isinstance(data, str):
            raise ValueError(f"Expected string, got {type(data).__name__}")
    elif schema_type == "number":
        if not isinstance(data, (int, float)) or isinstance(data, bool):
            raise ValueError(f"Expected number, got {type(data).__name__}")
    elif schema_type == "integer":
        if not isinstance(data, int) or isinstance(data, bool):
            raise ValueError(f"Expected integer, got {type(data).__name__}")
    elif schema_type == "boolean":
        if not isinstance(data, bool):
            raise ValueError(f"Expected boolean, got {type(data).__name__}")

    if "enum" in schema and data not in schema["enum"]:
        raise ValueError(f"Expected one of {schema['enum']}, got {data!r}")
