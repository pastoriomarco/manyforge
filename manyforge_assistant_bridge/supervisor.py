"""Assistant-stack supervisor for the ManyForge Composer monitor popup.

Mirrors `manyforge_ros_bringup/isaac_demo_supervisor.py`. Probes the AI
stack components (vLLM, optional mutator proxy, OpenClaw gateway-backed
bridge, assistant bridge) and POSTs a runtime-status envelope to
``${COMPOSER_URL}/api/runtime/external/assistant_supervisor`` every
``poll_period_s`` seconds. Composer aggregates the components into the
runtime status surface so the popup shows assistant-stack readiness
without the operator having to inspect each component manually.

This module deliberately uses only the stdlib (``urllib.request``) — the
supervisor must be runnable from any Python 3 install, including
volume-mounted demo environments where the venv may not be ready yet.
"""

from __future__ import annotations

import json
import logging
import os
import signal
import socket
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlsplit

logging.basicConfig(
    format="%(asctime)s [assistant_supervisor] %(levelname)s %(message)s",
    level=os.environ.get("LOG_LEVEL", "INFO"),
)
logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

PROVIDER_ID = "assistant_supervisor"


def _env_bool(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class SupervisorConfig:
    composer_url: str = os.environ.get("COMPOSER_URL", "http://127.0.0.1:9000")
    vllm_direct_url: str = os.environ.get("VLLM_DIRECT_URL", "http://127.0.0.1:8000/v1")
    upstream_base_url: str = os.environ.get("UPSTREAM_BASE_URL", "http://127.0.0.1:8000/v1")
    start_vllm_proxy: bool = _env_bool("START_VLLM_PROXY", True)
    assistant_provider: str = os.environ.get("ASSISTANT_PROVIDER", "openclaw")
    bridge_url: str = os.environ.get("BRIDGE_URL", "http://127.0.0.1:8200")
    model_profile: str = os.environ.get("MODEL_PROFILE", "cosmos-reason2-8b")
    poll_period_s: float = float(os.environ.get("ASSISTANT_SUPERVISOR_PERIOD_S", "1.0"))
    http_timeout_s: float = float(os.environ.get("ASSISTANT_SUPERVISOR_HTTP_TIMEOUT_S", "2.0"))


# ---------------------------------------------------------------------------
# Probes
# ---------------------------------------------------------------------------


@dataclass
class ProbeResult:
    ok: bool
    detail: str
    raw: dict[str, Any] | None = field(default=None)


def _probe_get_json(url: str, timeout_s: float) -> ProbeResult:
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=timeout_s) as response:
            body = response.read()
            if not body:
                return ProbeResult(ok=True, detail=f"{url} → empty body", raw=None)
            try:
                parsed = json.loads(body.decode("utf-8"))
            except json.JSONDecodeError:
                return ProbeResult(ok=True, detail=f"{url} → non-JSON 2xx", raw=None)
            return ProbeResult(ok=True, detail=f"{url} → 2xx", raw=parsed)
    except urllib.error.HTTPError as exc:
        return ProbeResult(ok=False, detail=f"{url} → HTTP {exc.code}")
    except (urllib.error.URLError, TimeoutError, socket.timeout) as exc:
        return ProbeResult(ok=False, detail=f"{url} unreachable ({exc})")
    except OSError as exc:
        return ProbeResult(ok=False, detail=f"{url} OSError ({exc})")


def _vllm_models_url(base_url: str) -> str:
    return base_url.rstrip("/") + "/models"


def _bridge_health_url(base_url: str) -> str:
    return base_url.rstrip("/") + "/healthz"


def probe_vllm(cfg: SupervisorConfig) -> tuple[bool, str]:
    result = _probe_get_json(_vllm_models_url(cfg.vllm_direct_url), cfg.http_timeout_s)
    if not result.ok:
        return False, result.detail
    served = ""
    if isinstance(result.raw, dict):
        data = result.raw.get("data") or []
        if data and isinstance(data[0], dict):
            served = str(data[0].get("id") or "")
    if served:
        if served != cfg.model_profile:
            return False, f"vLLM serves {served!r} (expected {cfg.model_profile!r})"
        return True, f"serving {served}"
    return True, "reachable (no model id)"


def probe_vllm_proxy(cfg: SupervisorConfig) -> tuple[bool, str]:
    if not cfg.start_vllm_proxy:
        # Proxy disabled by deployment; treat as N/A.
        return True, "proxy disabled (START_VLLM_PROXY=false)"
    parts = urlsplit(cfg.upstream_base_url)
    host = parts.hostname or "127.0.0.1"
    port = parts.port or 8000
    url = f"http://{host}:{port}/v1/models"
    result = _probe_get_json(url, cfg.http_timeout_s)
    return result.ok, result.detail


def probe_bridge(cfg: SupervisorConfig) -> tuple[bool, str]:
    result = _probe_get_json(_bridge_health_url(cfg.bridge_url), cfg.http_timeout_s)
    if result.ok:
        return True, f"{cfg.bridge_url}/healthz → 2xx"
    return False, result.detail


# ---------------------------------------------------------------------------
# Payload + POST
# ---------------------------------------------------------------------------


def _component(
    component_id: str,
    label: str,
    kind: str,
    ready: bool,
    detail: str,
    *,
    required_for: tuple[str, ...] = ("execute",),
) -> dict[str, Any]:
    return {
        "id": component_id,
        "label": label,
        "kind": kind,
        "status": "ready" if ready else "waiting",
        "requiredFor": list(required_for),
        "detail": detail,
    }


def build_status_payload(cfg: SupervisorConfig) -> dict[str, Any]:
    vllm_ok, vllm_detail = probe_vllm(cfg)
    proxy_ok, proxy_detail = probe_vllm_proxy(cfg)
    bridge_ok, bridge_detail = probe_bridge(cfg)

    components: list[dict[str, Any]] = [
        _component(
            "vllm",
            f"vLLM ({cfg.model_profile})",
            "model",
            vllm_ok,
            vllm_detail,
        ),
    ]
    if cfg.start_vllm_proxy:
        components.append(
            _component(
                "vllm_proxy",
                "vLLM mutator proxy",
                "model",
                proxy_ok,
                proxy_detail,
            )
        )
    # OpenClaw gateway readiness is derived from bridge health: the bridge
    # cannot pass /healthz unless the gateway it talks to (via openshell)
    # is also up. This avoids piling an openshell CLI dependency on the
    # supervisor.
    if cfg.assistant_provider == "openclaw":
        components.append(
            _component(
                "openclaw_gateway",
                "OpenClaw gateway",
                "assistant",
                bridge_ok,
                "Reachable via bridge" if bridge_ok else "Bridge is unreachable; gateway state unknown",
            )
        )
    components.append(
        _component(
            "assistant_bridge",
            f"Assistant bridge ({cfg.assistant_provider})",
            "assistant",
            bridge_ok,
            bridge_detail,
        )
    )

    all_ready = all(c["status"] == "ready" for c in components)
    missing = [c["label"] for c in components if c["status"] != "ready"]
    if all_ready:
        readiness = "ready"
        user_message = "Assistant stack is ready."
    else:
        readiness = "waiting"
        user_message = "Assistant stack is waiting on: " + ", ".join(missing)

    return {
        "providerLabel": "Assistant supervisor",
        "readiness": readiness,
        "canExecute": all_ready,
        "userMessage": user_message,
        "components": components,
        "assistantProvider": cfg.assistant_provider,
        "modelProfile": cfg.model_profile,
    }


def post_status(cfg: SupervisorConfig, payload: dict[str, Any]) -> None:
    url = f"{cfg.composer_url.rstrip('/')}/api/runtime/external/{PROVIDER_ID}"
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=cfg.http_timeout_s) as response:
            if not (200 <= response.status < 300):
                logger.debug("Runtime status post returned HTTP %s", response.status)
    except (OSError, urllib.error.URLError) as exc:
        # Composer may not be up yet; log at debug to avoid spam during boot.
        logger.debug("Runtime status post failed: %s", exc)


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------


_RUNNING = True


def _handle_signal(signum: int, _frame: Any) -> None:
    global _RUNNING
    logger.info("Received signal %d; shutting down", signum)
    _RUNNING = False


def main() -> int:
    cfg = SupervisorConfig()
    logger.info(
        "Starting assistant supervisor: composer=%s vllm=%s proxy=%s provider=%s bridge=%s",
        cfg.composer_url,
        cfg.vllm_direct_url,
        "on" if cfg.start_vllm_proxy else "off",
        cfg.assistant_provider,
        cfg.bridge_url,
    )
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    last_readiness: str | None = None
    while _RUNNING:
        payload = build_status_payload(cfg)
        post_status(cfg, payload)
        readiness = payload["readiness"]
        if readiness != last_readiness:
            logger.info(
                "Assistant readiness %s → %s (missing: %s)",
                last_readiness or "(none)",
                readiness,
                ", ".join(c["label"] for c in payload["components"] if c["status"] != "ready") or "none",
            )
            last_readiness = readiness
        time.sleep(cfg.poll_period_s)

    # Best-effort final post indicating shutdown.
    try:
        shutdown_payload = build_status_payload(cfg)
        shutdown_payload["readiness"] = "offline"
        shutdown_payload["userMessage"] = "Assistant supervisor stopping."
        for component in shutdown_payload["components"]:
            component["status"] = "disabled"
            component["detail"] = "Supervisor stopping"
        post_status(cfg, shutdown_payload)
    except Exception:  # pragma: no cover - shutdown best-effort
        pass
    logger.info("Assistant supervisor stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
