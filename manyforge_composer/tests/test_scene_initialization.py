"""Fail-before-state tests for the permanent Composer-owner fence."""

import pytest

from manyforge_composer.backend import scene_initialization
from manyforge_composer.backend.scene_initialization import (
    SCENE_INITIALIZATION_REPLACEMENT_GATE,
    SceneInitializationClient,
    SceneInitializationClientUnavailableError,
    require_scene_initialization_client,
)


class _PoisonState:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"scene-initialization gate accessed state: {name}")


def test_client_fails_before_scene_manager_or_backend_access() -> None:
    poison = _PoisonState()
    with pytest.raises(
        SceneInitializationClientUnavailableError,
        match="composable_cell_scene_initialization_client_unavailable",
    ):
        SceneInitializationClient(poison, manager=poison, backend=poison)


def test_gate_fails_before_token_or_observation_access() -> None:
    poison = _PoisonState()
    with pytest.raises(SceneInitializationClientUnavailableError):
        require_scene_initialization_client(poison, token=poison)

    assert SCENE_INITIALIZATION_REPLACEMENT_GATE == "I5/H0"


def test_composer_barrier_and_v1_replay_dto_are_absent() -> None:
    assert not hasattr(scene_initialization, "SceneInitializationBarrier")
    assert not hasattr(scene_initialization, "SceneInitializationReplayBatch")
