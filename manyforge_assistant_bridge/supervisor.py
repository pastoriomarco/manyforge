"""Pure assistant observation reducer plus the superseded fixed supervisor.

The complete effectful supervisor is pinned at source blob
``8c0ecee5ad79b553063c0f171740a3ccf617e8d7`` (sha256
``febb08a34f5b13491cbc7675656a51135b0f360dca71c8c97be47d95eab40978``).
Its HTTP probes (lines 77-142), Composer-v0 status envelope (164-230), status
POST (232-256), polling/signal loop, and shutdown post (264-304) are
acceptance inputs, not a callable fallback. The Component Manager
owns process convergence and publication; the current bridge therefore does
not revive this second launcher.
"""

from __future__ import annotations

import logging
import sys
from dataclasses import dataclass
from typing import Final


logger = logging.getLogger(__name__)
MAX_ID_CHARS: Final = 256
MAX_LABEL_CHARS: Final = 512
MAX_DETAIL_CHARS: Final = 2048
MAX_COMPONENTS: Final = 16
SUPERVISOR_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_assistant_supervisor_unavailable"
)
SUPERVISOR_DISPOSITION: Final = "superseded_by_component_manager"


class AssistantSupervisorUnavailableError(RuntimeError):
    """Raised before publication to a headless status owner."""


@dataclass(frozen=True)
class SupervisorConfig:
    """Owner-neutral inputs needed to reduce already-observed facts."""

    model_profile: str = "unconfigured"
    assistant_provider: str = "unconfigured"
    start_vllm_proxy: bool = False

    def __post_init__(self) -> None:
        _bounded_text(self.model_profile, label="model_profile", limit=MAX_ID_CHARS)
        _bounded_text(
            self.assistant_provider,
            label="assistant_provider",
            limit=MAX_ID_CHARS,
        )
        _exact_bool(self.start_vllm_proxy, label="start_vllm_proxy")


@dataclass(frozen=True)
class ComponentObservation:
    component_id: str
    label: str
    kind: str
    available: bool
    detail: str

    def __post_init__(self) -> None:
        _bounded_text(self.component_id, label="component_id", limit=MAX_ID_CHARS)
        _bounded_text(self.label, label="component label", limit=MAX_LABEL_CHARS)
        _bounded_text(self.kind, label="component kind", limit=MAX_ID_CHARS)
        _exact_bool(self.available, label="component available")
        _bounded_text(
            self.detail,
            label="component detail",
            limit=MAX_DETAIL_CHARS,
            allow_empty=True,
        )


@dataclass(frozen=True)
class AssistantStackObservation:
    """Transport-neutral availability facts; it does not grant execution."""

    available: bool
    message: str
    components: tuple[ComponentObservation, ...]
    assistant_provider: str
    model_profile: str

    def __post_init__(self) -> None:
        _exact_bool(self.available, label="stack available")
        _bounded_text(self.message, label="stack message", limit=MAX_DETAIL_CHARS)
        if not isinstance(self.components, tuple):
            raise ValueError("components must be a tuple")
        if len(self.components) > MAX_COMPONENTS:
            raise ValueError(f"components exceeds the {MAX_COMPONENTS}-item bound")
        if any(not isinstance(item, ComponentObservation) for item in self.components):
            raise ValueError("components must contain ComponentObservation values")
        _bounded_text(
            self.assistant_provider,
            label="assistant_provider",
            limit=MAX_ID_CHARS,
        )
        _bounded_text(self.model_profile, label="model_profile", limit=MAX_ID_CHARS)


def reduce_status_payload(
    config: SupervisorConfig,
    *,
    vllm_result: tuple[bool, str],
    proxy_result: tuple[bool, str],
    bridge_result: tuple[bool, str],
) -> AssistantStackObservation:
    """Reduce already-observed probes without producing an old runtime wire."""
    if not isinstance(config, SupervisorConfig):
        raise ValueError("config must be a SupervisorConfig")
    vllm_available, vllm_detail = _validated_probe_result(
        vllm_result, label="vllm_result"
    )
    proxy_available, proxy_detail = _validated_probe_result(
        proxy_result, label="proxy_result"
    )
    bridge_available, bridge_detail = _validated_probe_result(
        bridge_result, label="bridge_result"
    )
    components = [
        ComponentObservation(
            component_id="model",
            label=f"Model ({config.model_profile})",
            kind="model",
            available=vllm_available,
            detail=vllm_detail,
        )
    ]
    if config.start_vllm_proxy:
        components.append(
            ComponentObservation(
                component_id="model_proxy",
                label="Model proxy",
                kind="model",
                available=proxy_available,
                detail=proxy_detail,
            )
        )
    components.append(
        ComponentObservation(
            component_id="assistant_bridge",
            label=f"Assistant bridge ({config.assistant_provider})",
            kind="assistant",
            available=bridge_available,
            detail=bridge_detail,
        )
    )
    available = all(component.available for component in components)
    missing = [component.label for component in components if not component.available]
    return AssistantStackObservation(
        available=available,
        message=(
            "Assistant stack is available."
            if available
            else "Assistant stack is waiting on: " + ", ".join(missing)
        ),
        components=tuple(components),
        assistant_provider=config.assistant_provider,
        model_profile=config.model_profile,
    )


def _validated_probe_result(
    value: object, *, label: str
) -> tuple[bool, str]:
    if not isinstance(value, tuple) or len(value) != 2:
        raise ValueError(f"{label} must be an (available, detail) tuple")
    available = _exact_bool(value[0], label=f"{label}.available")
    detail = _bounded_text(
        value[1],
        label=f"{label}.detail",
        limit=MAX_DETAIL_CHARS,
        allow_empty=True,
    )
    return (available, detail)


def _exact_bool(value: object, *, label: str) -> bool:
    if type(value) is not bool:
        raise ValueError(f"{label} must be a boolean")
    return value


def _bounded_text(
    value: object,
    *,
    label: str,
    limit: int,
    allow_empty: bool = False,
) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise ValueError(f"{label} must be {qualifier}")
    if len(value) > limit:
        raise ValueError(f"{label} exceeds the {limit}-character bound")
    return value


def post_status(
    config: SupervisorConfig,
    payload: AssistantStackObservation,
) -> None:
    """Fail before opening a request or resolving a publication endpoint."""
    del config, payload
    raise AssistantSupervisorUnavailableError(
        f"{SUPERVISOR_UNAVAILABLE_REASON_CODE}: fixed status publication is "
        "superseded by Component Manager-owned process convergence"
    )


def main() -> int:
    """Fail before configuration, network probes, publication, or looping."""

    logger.error(
        "%s: the fixed assistant supervisor is superseded by Component Manager",
        SUPERVISOR_UNAVAILABLE_REASON_CODE,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
