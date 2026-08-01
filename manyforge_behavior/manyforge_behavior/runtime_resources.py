"""Typed gate for the removed deployment-owned runtime-resource registry.

The former module selected arbitrary Python provider factories from raw
``runtime_resources`` mappings, opened handles, owned rollback/close state, and
served alias-based resources to behavior nodes.  That is runtime binding and
lifecycle authority, not portable authoring logic.  Dynamic implementation
availability returns through the headless resolver at I5/H0; native
resource-addressed behavior binding returns at I6/N.  The raw v0 registry is
never restored.
"""

from __future__ import annotations

from typing import Final, NoReturn


RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_runtime_resource_binding_unavailable"
)
RUNTIME_RESOURCE_BINDING_RESTORATION_GATES: Final = ("I5", "H0", "I6", "N")


class RuntimeResourceBindingUnavailableError(RuntimeError):
    """Raised before provider lookup, handle creation, or lifecycle effects."""


def _raise_runtime_resource_binding_unavailable() -> NoReturn:
    raise RuntimeResourceBindingUnavailableError(
        f"{RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE}: dynamic "
        "implementation availability returns at I5/H0 and resource-addressed "
        "native behavior binding returns at I6/N"
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
    "RUNTIME_RESOURCE_BINDING_RESTORATION_GATES",
    "RUNTIME_RESOURCE_BINDING_UNAVAILABLE_REASON_CODE",
    "RuntimeResourceBindingUnavailableError",
    "RuntimeResourceFactoryRegistry",
    "RuntimeResourceRegistry",
    "build_runtime_resource_registry",
]
