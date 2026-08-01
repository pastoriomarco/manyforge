"""Cutover gate for v2 runtime requirement and availability projection."""

import pytest

from manyforge_composer.backend.runtime_requirements import (
    RUNTIME_REQUIREMENTS_RESTORATION_GATES,
    RuntimeRequirementsClient,
    RuntimeRequirementsClientUnavailableError,
    assess_runtime_requirements,
)


class _PoisonInput:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"runtime-requirements input was accessed: {name}")


def test_client_fails_before_manager_or_deployment_access() -> None:
    poison = _PoisonInput()
    with pytest.raises(
        RuntimeRequirementsClientUnavailableError,
        match="composable_cell_runtime_requirements_client_unavailable",
    ):
        RuntimeRequirementsClient(poison, environment=poison)


def test_assessment_fails_before_legacy_inference() -> None:
    poison = _PoisonInput()
    with pytest.raises(
        RuntimeRequirementsClientUnavailableError,
        match="composable_cell_runtime_requirements_client_unavailable",
    ):
        assess_runtime_requirements(poison, lane=poison)

    assert RUNTIME_REQUIREMENTS_RESTORATION_GATES == ("I5", "H0", "I6", "N")
