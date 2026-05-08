"""Protocol-level helpers shared between Composer and the assistant bridge.

This module is the single source of truth for any helper whose contract
must stay byte-identical across the manifest emitter (Composer) and the
manifest consumer (the assistant bridge). Anything here that changes
shape, ordering, or normalisation rules MUST treat the change as a wire
breaking change — every running deployment will recompute hashes,
session keys, etc., from this code.

Add helpers here only when they're shared across the protocol boundary.
Logic that lives entirely on one side does not belong here.
"""
from __future__ import annotations

import hashlib
import json


def compute_catalog_hash(catalog: dict[str, list[str]], mode: str) -> str:
    """Deterministic SHA-256 of the resolved catalog and active mode.

    The hash gates the catalog-freshness contract:

      1. The Composer manifest endpoint emits this hash on
         /api/assistant/modes/{mode}.
      2. The MCP wrapper echoes it on every tools/call.
      3. The bridge endpoint validates that the echoed hash matches
         the currently-resolved hash; mismatch returns 409, catching
         deployment reloads that happened mid-conversation.
      4. The direct-lane bridge recomputes the hash from the envelope
         it received, so it can detect tampering or skew.

    Both sides MUST produce byte-identical output for the same input.
    The previous duplicated implementations in
    ``manyforge_composer/backend/routes_assistant.py`` and
    ``manyforge_assistant_bridge/bridge.py`` were collapsed into this
    one canonical helper to remove the dual-maintenance hazard.

    The input is the resolved bare-id catalog dict (skills/tools/nodes
    visible in the active mode). The hash is computed on internal
    Composer state, not on the wire envelope — so the wire-format
    cleanup that dropped the bare-id lists from the envelope did not
    change what's hashed. Changing what is hashed would invalidate
    every running session's cached hash; the contract is conservative
    by design.
    """
    payload = {
        "mode": mode,
        "skills": sorted(catalog.get("skills", []) or []),
        "tools": sorted(catalog.get("tools", []) or []),
        "nodes": sorted(catalog.get("nodes", []) or []),
    }
    blob = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()
