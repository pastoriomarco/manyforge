"""Public names for the current strict assistant-provider wire.

The removed v0/v0.1 ``compute_catalog_hash`` helper deliberately does not
return: the current hash is framed over exact verified descriptors, policy,
nodes, and skills after narrowing, not over caller-supplied ID lists.
"""

from .catalog import (
    AssistantCatalogError,
    ResolvedAssistantCatalog,
    resolve_catalog,
    verify_active_mode_projection,
)
from .contracts import (
    PROVIDER_REQUEST_VERSION,
    PROVIDER_RESPONSE_VERSION,
    PROVIDER_STATUS_VERSION,
    ProviderRequest,
)


__all__ = [
    "AssistantCatalogError",
    "PROVIDER_REQUEST_VERSION",
    "PROVIDER_RESPONSE_VERSION",
    "PROVIDER_STATUS_VERSION",
    "ProviderRequest",
    "ResolvedAssistantCatalog",
    "resolve_catalog",
    "verify_active_mode_projection",
]
