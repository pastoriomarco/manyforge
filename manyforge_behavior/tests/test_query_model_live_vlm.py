"""The old direct live-VLM node path is unavailable until I5/H0 and I6/N."""

import pytest

from manyforge_behavior.nodes.model import ModelNodeUnavailableError, _call_model


def test_live_provider_path_is_gated_before_environment_or_network_access() -> None:
    with pytest.raises(
        ModelNodeUnavailableError,
        match="composable_cell_model_node_unavailable",
    ):
        _call_model(provider="local", model_id="unqualified")
