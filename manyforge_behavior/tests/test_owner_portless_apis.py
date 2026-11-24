"""The resolver's owner-port-less adapters agree with the native projection.

The Composer refuses an active node environment whose native projection
reports an unavailable owner for an adapter the Python resolver builds
without an owner port (``node_environment_client.py``). Both sides must
therefore classify the same owner APIs as having no separate semantic owner.
"""

from __future__ import annotations

from pathlib import Path
import re

from manyforge_behavior.executable_node_resolver import build_executable_node_resolver
from manyforge_behavior.node_environment import build_node_environment

_ROOT = Path(__file__).resolve().parents[2]
_NATIVE = _ROOT / "manyforge_headless/src/node_environment_projection.cpp"


def _native_owner_neutral_apis() -> frozenset[str]:
    source = _NATIVE.read_text(encoding="utf-8")
    body = re.search(
        r"bool IsOwnerNeutralOwnerApi\([^)]*\) noexcept \{(.*?)\n\}", source, re.S
    )
    assert body is not None, "IsOwnerNeutralOwnerApi not found"
    return frozenset(re.findall(r'owner_api == "([^"]+)"', body.group(1)))


def test_owner_port_less_adapters_match_the_native_owner_neutral_apis() -> None:
    environment = build_node_environment()
    resolver = build_executable_node_resolver(environment)
    portless = {
        environment.catalog.get(disposition.row.node_id).execution.owner_api
        for disposition in resolver.dispositions
        if disposition.adapter_build_qualified and disposition.owner_port_name is None
    }
    with_port = {
        environment.catalog.get(disposition.row.node_id).execution.owner_api
        for disposition in resolver.dispositions
        if disposition.owner_port_name is not None
    }
    native = _native_owner_neutral_apis()
    assert portless == native
    assert not (with_port & native)
    # OD-342: the Program run owner serves these itself.
    assert {"orchestration.operator", "orchestration.selection_stop"} <= native
