"""Typed gate for the removed deployment-owned runtime-resource registry.

The former module selected arbitrary Python provider factories from raw
``runtime_resources`` mappings, opened handles, owned rollback/close state, and
served alias-based resources to behavior nodes.  That is runtime binding and
lifecycle authority, not portable authoring logic.  The headless owner now projects the
verified retained implementation identity and runtime availability without
constructing it; native resource-addressed behavior binding is not available.
The raw v0 registry is never restored.
"""

from __future__ import annotations

from typing import Final, NoReturn


RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_runtime_resource_binding_unavailable"
)


class RuntimeResourceBindingUnavailableError(RuntimeError):
    """Raised before provider lookup, handle creation, or lifecycle effects."""


def _raise_runtime_resource_binding_unavailable() -> NoReturn:
    raise RuntimeResourceBindingUnavailableError(
        f"{RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE}: the headless owner provides "
        "read-only implementation availability; resource-addressed native "
        "behavior binding is not available"
    )


class RuntimeResourceFactoryRegistry:
    """Unavailable former Python provider-factory owner."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_resource_binding_unavailable()


class RuntimeResourceRegistry:
    """Unavailable former mutable handle and close-state owner."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_runtime_resource_binding_unavailable()

    @classmethod
    def empty(cls) -> NoReturn:
        _raise_runtime_resource_binding_unavailable()


def build_runtime_resource_registry(*_args: object, **_kwargs: object) -> NoReturn:
    """Reject the old raw mapping before iteration or provider effects."""

    _raise_runtime_resource_binding_unavailable()


__all__ = [
    "RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE",
    "RuntimeResourceBindingUnavailableError",
    "RuntimeResourceFactoryRegistry",
    "RuntimeResourceRegistry",
    "build_runtime_resource_registry",
]
