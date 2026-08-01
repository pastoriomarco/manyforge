"""Assistant bridge cutover entrypoint coverage."""

import pytest

from manyforge_assistant_bridge import bridge


def test_bridge_entrypoint_returns_unavailable_without_starting_server() -> None:
    assert bridge.main() == 78


def test_bridge_activation_is_typed_and_fenced() -> None:
    with pytest.raises(
        bridge.AssistantBridgeUnavailableError,
        match="composable_cell_assistant_bridge_unavailable",
    ):
        bridge.require_assistant_bridge()
