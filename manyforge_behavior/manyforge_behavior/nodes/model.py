"""Non-blocking model query node for ManyForge behavior tree (OP-7).

QueryModelNode is a leaf node that queries a deployment-configured model
endpoint using a versioned prompt template.  It is designed for 100Hz tick
loop compatibility: the actual model call runs in a background thread via
ThreadPoolExecutor, and update() checks the future status without blocking.

Blackboard writes are atomic at the namespace level — all validated output
fields are applied together or not at all.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import time
from concurrent.futures import Future, ThreadPoolExecutor
from typing import Any, Optional

import py_trees

logger = logging.getLogger(__name__)

# Shared executor — max 2 workers to avoid thread explosion in the tick loop
_MODEL_EXECUTOR: Optional[ThreadPoolExecutor] = None

# Shared httpx client — reuses TCP connections across model calls.
# Timeout is set per-request via the ``timeout`` kwarg on ``client.post()``.
_HTTP_CLIENT: Optional["httpx.Client"] = None
_HTTP_CLIENT_LOCK = __import__("threading").Lock()


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
) -> dict[str, Any]:
    """Execute a model call synchronously (runs in thread pool).

    Returns a dict with:
      - "text": raw response text
      - "parsed": parsed JSON if response is valid JSON, else None
      - "model_version": model version string if available
      - "usage": token usage dict if available
    """
    api_key = os.environ.get(api_key_env, "") if api_key_env else ""

    if provider in ("openai", "anthropic"):
        import httpx
        headers: dict[str, str] = {"Content-Type": "application/json"}
        body: dict[str, Any] = {
            "model": model_id,
            "max_tokens": max_tokens,
        }

        if provider == "openai":
            url = (base_url or "https://api.openai.com/v1") + "/chat/completions"
            headers["Authorization"] = f"Bearer {api_key}"
            body["messages"] = [{"role": "user", "content": prompt}]
            if output_schema:
                body["response_format"] = {"type": "json_object"}
        else:  # anthropic
            url = (base_url or "https://api.anthropic.com/v1") + "/messages"
            headers["x-api-key"] = api_key
            headers["anthropic-version"] = "2023-06-01"
            body["messages"] = [{"role": "user", "content": prompt}]

        client = _get_http_client()
        resp = client.post(
            url, json=body, headers=headers,
            timeout=timeout_s,
        )
        resp.raise_for_status()
        data = resp.json()

        # Extract text from response
        if provider == "openai":
            text = data.get("choices", [{}])[0].get("message", {}).get("content", "")
            model_version = data.get("model", model_id)
            usage = data.get("usage", {})
        else:
            content_blocks = data.get("content", [])
            text = "".join(
                b.get("text", "") for b in content_blocks if b.get("type") == "text"
            )
            model_version = data.get("model", model_id)
            usage = data.get("usage", {})

        parsed = None
        try:
            parsed = json.loads(text)
        except (json.JSONDecodeError, TypeError):
            pass

        return {
            "text": text,
            "parsed": parsed,
            "model_version": model_version,
            "usage": {
                "input_tokens": usage.get("input_tokens") or usage.get("prompt_tokens", 0),
                "output_tokens": usage.get("output_tokens") or usage.get("completion_tokens", 0),
            },
        }
    else:
        raise ValueError(f"Unsupported model provider: {provider}")


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
        template_variables: Optional[dict[str, str]] = None,
        output_key_prefix: str,
        timeout_s: float = 30.0,
        min_confidence: float = 0.0,
        # Injected by runner — not from program params
        endpoint: Any = None,
        template: Any = None,
        event_recorder: Any = None,
        audit_policy: Any = None,
        # Execution context — injected by runner for audit correlation
        run_id: Optional[str] = None,
        session_id: Optional[str] = None,
        principal_id: Optional[str] = None,
    ):
        super().__init__(name=name)
        self._model_endpoint_alias = model_endpoint_alias
        self._prompt_template_id = prompt_template_id
        self._template_variables = dict(template_variables or {})
        self._output_key_prefix = output_key_prefix
        self._timeout_s = timeout_s
        self._min_confidence = min_confidence
        self._endpoint = endpoint
        self._template = template
        self._event_recorder = event_recorder
        self._audit_policy = audit_policy
        self._run_id = run_id
        self._session_id = session_id
        self._principal_id = principal_id
        self._future: Optional[Future] = None
        self._call_start_ns: int = 0
        self._resolved_prompt: str = ""
        self._bb = py_trees.blackboard.Client(name=f"query_model_{name}")
        self._output_keys_registered = False

    def _ensure_output_keys(self) -> None:
        if self._output_keys_registered:
            return
        prefix = self._output_key_prefix
        for key in (
            f"{prefix}/response",
            f"{prefix}/confidence",
            f"{prefix}/timestamp_ns",
            f"{prefix}/model_version",
            f"{prefix}/status",
        ):
            self._bb.register_key(
                key=key,
                access=py_trees.common.Access.WRITE,
            )
        # Register read access for template variables
        for bb_key in self._template_variables.values():
            self._bb.register_key(
                key=bb_key,
                access=py_trees.common.Access.READ,
            )
        self._output_keys_registered = True

    def _event_context(self) -> dict[str, Any]:
        """Return execution context kwargs for record_model_event calls."""
        ctx: dict[str, Any] = {}
        if self._run_id is not None:
            ctx["run_id"] = self._run_id
        if self._session_id is not None:
            ctx["session_id"] = self._session_id
        if self._principal_id is not None:
            ctx["principal_id"] = self._principal_id
        return ctx

    def initialise(self) -> None:
        self._ensure_output_keys()
        self._future = None
        self._call_start_ns = 0

        if self._endpoint is None or self._template is None:
            logger.error(
                "%s: model endpoint or template not configured", self.name
            )
            return

        # Resolve template variables from blackboard
        resolved_vars: dict[str, str] = {}
        for var_name, bb_key in self._template_variables.items():
            try:
                value = self._bb.get(bb_key)
                resolved_vars[var_name] = str(value) if value is not None else ""
            except KeyError:
                resolved_vars[var_name] = ""

        # Build prompt from template
        prompt = self._template.template_text
        for var_name, var_value in resolved_vars.items():
            prompt = prompt.replace("{{" + var_name + "}}", var_value)
        self._resolved_prompt = prompt

        # Record started event
        self._call_start_ns = time.monotonic_ns()
        if self._event_recorder is not None:
            self._event_recorder.record_model_event(
                action="started",
                model_endpoint_alias=self._model_endpoint_alias,
                prompt_template_id=self._prompt_template_id,
                prompt_template_hash=self._template.content_hash,
                prompt_text=self._resolved_prompt,
                argument_keys=list(resolved_vars.keys()),
                argument_values=resolved_vars,
                node_instance=self.name,
                audit_policy=self._audit_policy,
                **self._event_context(),
            )

        # Submit to thread pool
        self._future = _get_executor().submit(
            _call_model,
            provider=self._endpoint.provider,
            model_id=self._endpoint.model_id,
            base_url=self._endpoint.base_url,
            api_key_env=self._endpoint.api_key_env,
            prompt=self._resolved_prompt,
            max_tokens=self._endpoint.max_tokens,
            timeout_s=self._timeout_s,
            output_schema=self._template.output_schema,
        )

    def update(self) -> py_trees.common.Status:
        if self._future is None:
            # Configuration error in initialise
            self._write_output("status", "error")
            return py_trees.common.Status.FAILURE

        if not self._future.done():
            return py_trees.common.Status.RUNNING

        latency_ms = (time.monotonic_ns() - self._call_start_ns) / 1_000_000

        try:
            result = self._future.result()
        except Exception as exc:
            logger.error("%s: model call failed: %s", self.name, exc)
            self._write_output("status", "failed")
            if self._event_recorder is not None:
                self._event_recorder.record_model_event(
                    action="failed",
                    model_endpoint_alias=self._model_endpoint_alias,
                    prompt_template_id=self._prompt_template_id,
                    prompt_template_hash=self._template.content_hash if self._template else None,
                    error=str(exc),
                    latency_ms=latency_ms,
                    node_instance=self.name,
                    audit_policy=self._audit_policy,
                    **self._event_context(),
                )
            return py_trees.common.Status.FAILURE

        # Validate response against output schema if provided
        parsed = result.get("parsed")
        if self._template and self._template.output_schema and parsed is not None:
            try:
                _validate_json_schema(parsed, self._template.output_schema)
            except ValueError as exc:
                logger.error(
                    "%s: response JSON schema validation failed: %s",
                    self.name,
                    exc,
                )
                self._write_output("status", "validation_failed")
                if self._event_recorder is not None:
                    self._event_recorder.record_model_event(
                        action="failed",
                        model_endpoint_alias=self._model_endpoint_alias,
                        prompt_template_id=self._prompt_template_id,
                        prompt_template_hash=self._template.content_hash,
                        response_text=result.get("text"),
                        error=f"schema_validation: {exc}",
                        latency_ms=latency_ms,
                        node_instance=self.name,
                        audit_policy=self._audit_policy,
                        **self._event_context(),
                    )
                return py_trees.common.Status.FAILURE
        elif self._template and self._template.output_schema and parsed is None:
            logger.error(
                "%s: response is not valid JSON but output_schema requires it",
                self.name,
            )
            self._write_output("status", "parse_failed")
            if self._event_recorder is not None:
                self._event_recorder.record_model_event(
                    action="failed",
                    model_endpoint_alias=self._model_endpoint_alias,
                    prompt_template_id=self._prompt_template_id,
                    prompt_template_hash=self._template.content_hash,
                    response_text=result.get("text"),
                    error="response is not valid JSON",
                    latency_ms=latency_ms,
                    node_instance=self.name,
                    audit_policy=self._audit_policy,
                    **self._event_context(),
                )
            return py_trees.common.Status.FAILURE

        # Atomic blackboard writes
        output = parsed if parsed is not None else result.get("text", "")
        self._write_output("response", output)
        self._write_output("timestamp_ns", time.monotonic_ns())
        self._write_output("model_version", result.get("model_version", ""))
        self._write_output("status", "succeeded")
        # confidence is derived from presence of valid structured output
        confidence = 1.0 if parsed is not None else 0.5
        self._write_output("confidence", confidence)

        # Record completed event
        if self._event_recorder is not None:
            self._event_recorder.record_model_event(
                action="completed",
                model_endpoint_alias=self._model_endpoint_alias,
                prompt_template_id=self._prompt_template_id,
                prompt_template_hash=self._template.content_hash if self._template else None,
                response_text=result.get("text"),
                response_fields=parsed,
                model_version=result.get("model_version"),
                latency_ms=latency_ms,
                token_counts=result.get("usage"),
                node_instance=self.name,
                audit_policy=self._audit_policy,
                **self._event_context(),
            )

        if confidence < self._min_confidence:
            logger.warning(
                "%s: confidence %.2f below threshold %.2f",
                self.name,
                confidence,
                self._min_confidence,
            )
            return py_trees.common.Status.FAILURE

        return py_trees.common.Status.SUCCESS

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if (
            self._future is not None
            and not self._future.done()
            and new_status != py_trees.common.Status.RUNNING
        ):
            self._future.cancel()
            self._future = None

    def _write_output(self, key: str, value: Any) -> None:
        try:
            self._bb.set(f"{self._output_key_prefix}/{key}", value)
        except Exception:
            logger.debug(
                "%s: failed to write blackboard key %s/%s",
                self.name,
                self._output_key_prefix,
                key,
            )


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
