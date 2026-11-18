"""Keep-alive pairing of ManyForge's internal HTTP hops.

A pooled client may reuse an idle connection only while its server still
keeps it open. Uvicorn arms its keep-alive timer when it has sent a response;
the client starts its idle time later, when it has read that response. With
equal times (both libraries default to 5 s) the server closes first, and a
request sent on the connection in the last milliseconds fails. Every internal
client therefore drops an idle connection well before every internal server
closes it. The 3 s margin covers the client's read of a response finishing
late (thread scheduling and the GIL under load); reconnecting after 2 s idle
costs one local TCP connect. Neither side depends on a library default.
"""

from __future__ import annotations

from typing import Final

import httpx

# uvicorn ``timeout_keep_alive`` of every ManyForge server (its default value).
INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS: Final = 5
# httpx ``keepalive_expiry`` of every ManyForge internal client.
INTERNAL_HTTP_CLIENT_KEEP_ALIVE_EXPIRY_SECONDS: Final = 2.0
# httpx's default pool sizes, with the client expiry above.
INTERNAL_HTTP_CLIENT_LIMITS: Final = httpx.Limits(
    max_connections=100,
    max_keepalive_connections=20,
    keepalive_expiry=INTERNAL_HTTP_CLIENT_KEEP_ALIVE_EXPIRY_SECONDS,
)

__all__ = [
    "INTERNAL_HTTP_CLIENT_KEEP_ALIVE_EXPIRY_SECONDS",
    "INTERNAL_HTTP_CLIENT_LIMITS",
    "INTERNAL_HTTP_SERVER_KEEP_ALIVE_SECONDS",
]
