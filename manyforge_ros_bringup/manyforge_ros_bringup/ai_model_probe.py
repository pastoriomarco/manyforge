"""Reachability probe of one ManyForge ``ai_model`` component (stream health).

The execution host writes the site document from the Deployment row and
supervises ``ai_model.launch.py``. Every probe period this process GETs
``{base_url}/models`` and records one ``endpoint`` stream sample only when the
server answers 2xx and lists the configured model; otherwise the stream goes
stale (a warning, never a fault) and the reason is logged when it changes
(also when the token variable named by ``api_key_env`` is unset). A bad site
document exits with status 78.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import http.client
import json
import os
from pathlib import Path
import re
import sys
from typing import Any
import urllib.request

from manyforge_ros.component_stream_health import ComponentStreamSink, valid_stream_id

SITE_API_VERSION = "manyforge.ai_model_probe_site.v1"
CONFIG_INVALID_EXIT_STATUS = 78
UNREACHABLE = "ai_model_unreachable"
NOT_SERVED = "ai_model_not_served"
TOKEN_UNAVAILABLE = "ai_model_token_unavailable"
_ENV_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,127}")
_MAX_BODY_BYTES = 4 * 1024 * 1024

# (url, headers, timeout_s) -> (status, body)
Fetch = Callable[[str, Mapping[str, str], float], tuple[int, bytes]]


@dataclass(frozen=True)
class ProbeSite:
    component_id: str
    stream_id: str
    base_url: str
    model: str
    api_key_env: str | None
    probe_period_s: float


def load_site(path: Path) -> ProbeSite:
    """The validated site document; ``ValueError``/``OSError`` when invalid."""

    doc = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict) or doc.get("api_version") != SITE_API_VERSION:
        raise ValueError("site config api_version is unsupported")
    texts = {
        name: doc.get(name) for name in ("component_id", "stream_id", "base_url", "model")
    }
    for name, value in texts.items():
        if type(value) is not str or not value:
            raise ValueError(f"site config {name} must be a non-empty string")
    if not valid_stream_id(texts["stream_id"]):
        raise ValueError("site config stream_id is invalid")
    if not texts["base_url"].startswith(("http://", "https://")):
        raise ValueError("site config base_url must be an http(s) URL")
    api_key_env = doc.get("api_key_env")
    if api_key_env is not None and (
        type(api_key_env) is not str or not _ENV_NAME.fullmatch(api_key_env)
    ):
        raise ValueError("site config api_key_env must be an environment variable name")
    period_ms = doc.get("probe_period_ms")
    if type(period_ms) is not int or period_ms <= 0:
        raise ValueError("site config probe_period_ms must be a positive integer")
    return ProbeSite(
        texts["component_id"], texts["stream_id"], texts["base_url"].rstrip("/"),
        texts["model"], api_key_env, period_ms / 1000.0,
    )


def bearer_token(site: ProbeSite, environ: Mapping[str, str]) -> str | None:
    """The token named by ``api_key_env``; ``ValueError`` when it is unset."""

    if site.api_key_env is None:
        return None
    token = environ.get(site.api_key_env)
    if not token:
        raise ValueError(f"environment variable {site.api_key_env} is not set")
    return token


def http_get(url: str, headers: Mapping[str, str], timeout_s: float) -> tuple[int, bytes]:
    request = urllib.request.Request(url, headers=dict(headers))
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return response.status, response.read(_MAX_BODY_BYTES)


def probe_endpoint(
    site: ProbeSite, token: str | None, fetch: Fetch = http_get
) -> tuple[str | None, str]:
    """``(None, "")`` when the endpoint lists the model, else ``(reason, detail)``."""

    headers = {"Accept": "application/json"}
    if token is not None:
        headers["Authorization"] = f"Bearer {token}"
    try:
        status, body = fetch(f"{site.base_url}/models", headers, site.probe_period_s)
    except (OSError, http.client.HTTPException, ValueError) as error:
        return UNREACHABLE, type(error).__name__
    if not 200 <= status < 300:
        return UNREACHABLE, f"http_status={status}"
    try:
        listed = {entry.get("id") for entry in json.loads(body)["data"]}
    except (ValueError, KeyError, TypeError, AttributeError):
        return NOT_SERVED, "model list is not an OpenAI list"
    return (None, "") if site.model in listed else (NOT_SERVED, "model is not listed")


class ProbeLoop:
    """One probe per ``step``; logs only when the outcome changes."""

    def __init__(self, site: ProbeSite, environ: Mapping[str, str], sink: Any, logger: Any,
                 fetch: Fetch = http_get) -> None:
        self._site, self._environ, self._sink = site, environ, sink
        self._logger, self._fetch = logger, fetch
        self._last: str | None = ""  # nothing logged yet

    def step(self) -> None:
        try:
            token = bearer_token(self._site, self._environ)
        except ValueError:
            reason, detail = TOKEN_UNAVAILABLE, f"api_key_env={self._site.api_key_env}"
        else:
            reason, detail = probe_endpoint(self._site, token, self._fetch)
        if reason is None:
            self._sink.record()
        if reason != self._last:
            if reason is None:
                self._logger.info(f"ai_model_served: component={self._site.component_id}")
            else:
                self._logger.warning(
                    f"{reason}: component={self._site.component_id} "
                    f"url={self._site.base_url}/models model={self._site.model} {detail}"
                )
        self._last = reason


def main(args: list[str] | None = None) -> int:
    import rclpy
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor

    context = Context()
    rclpy.init(args=args, context=context)
    node = rclpy.create_node("manyforge_ai_model_probe", context=context)
    executor = SingleThreadedExecutor(context=context)
    result = 0
    try:
        node.declare_parameter("site_config_path", "")
        try:
            site = load_site(Path(str(node.get_parameter("site_config_path").value)))
        except (OSError, ValueError) as error:
            print(f"ai_model_probe_failed: {error}", file=sys.stderr)
            return CONFIG_INVALID_EXIT_STATUS
        sink = ComponentStreamSink(node, site.component_id, site.stream_id)
        loop = ProbeLoop(site, os.environ, sink, node.get_logger())
        loop.step()  # Probe at once: the stream is live within one period.
        node.create_timer(site.probe_period_s, loop.step)
        executor.add_node(node)
        executor.spin()
    except KeyboardInterrupt:
        result = 0
    finally:
        executor.shutdown(timeout_sec=2.0)
        node.destroy_node()
        if context.ok():
            rclpy.shutdown(context=context)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
