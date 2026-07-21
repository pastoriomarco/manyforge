from __future__ import annotations

import pytest

from manyforge_ros_bringup.scene_initialization_handshake import (
    deferred_program_load_payload,
    scene_initialization_reset_payload,
    scene_initialization_token_from_program_response,
)


def test_deferred_load_payload_requests_generation_fenced_initialization() -> None:
    assert deferred_program_load_payload(
        program_path="/program.program.yaml",
        deployment_path="/deployment.deployment.yaml",
    ) == {
        "path": "/program.program.yaml",
        "deploymentPath": "/deployment.deployment.yaml",
        "forceDiscardOverrides": True,
        "deferInitialRuntimeReset": True,
    }


def test_program_response_requires_closed_barrier_with_nonempty_token() -> None:
    assert (
        scene_initialization_token_from_program_response(
            {
                "sceneInitialization": {
                    "state": "closed",
                    "token": "scene-init-123",
                }
            }
        )
        == "scene-init-123"
    )

    with pytest.raises(ValueError, match="not closed"):
        scene_initialization_token_from_program_response(
            {
                "sceneInitialization": {
                    "state": "open",
                    "token": "scene-init-123",
                }
            }
        )
    with pytest.raises(ValueError, match="no scene initialization token"):
        scene_initialization_token_from_program_response(
            {"sceneInitialization": {"state": "closed", "token": ""}}
        )


def test_reset_payload_echoes_exact_token_and_rejects_empty_value() -> None:
    assert scene_initialization_reset_payload("scene-init-123") == {
        "sceneInitializationToken": "scene-init-123"
    }
    with pytest.raises(ValueError, match="token is required"):
        scene_initialization_reset_payload("")
