"""Test fixtures for manyforge_planning."""

import pytest
import numpy as np

from manyforge_planning.roboplan_mock import (
    MockScene, MockRRT, MockPathShortcutter, MockTOPPRA, MockSimpleIk, MockRoboPlan
)
from manyforge_planning.roboplan_client import RoboPlanKernelClient


@pytest.fixture
def mock_scene():
    """Create a mock scene with default 6 joints."""
    return MockScene()


@pytest.fixture
def mock_rrt(mock_scene):
    """Create a mock RRT planner."""
    return MockRRT(mock_scene)


@pytest.fixture
def mock_shortcutter(mock_scene):
    """Create a mock path shortcutter."""
    return MockPathShortcutter(mock_scene, "arm")


@pytest.fixture
def mock_topptra(mock_scene):
    """Create a mock TOPPRA time parameterizer."""
    return MockTOPPRA(mock_scene)


@pytest.fixture
def mock_ik(mock_scene):
    """Create a mock IK solver."""
    return MockSimpleIk(mock_scene)


@pytest.fixture
def roboplan_module():
    """Provide the MockRoboPlan module."""
    return MockRoboPlan


@pytest.fixture
def client(mock_scene, mock_rrt, mock_shortcutter, mock_topptra, mock_ik, roboplan_module):
    """Create a RoboPlanKernelClient with all mock components."""
    return RoboPlanKernelClient(
        scene=mock_scene,
        rrt=mock_rrt,
        shortcutter=mock_shortcutter,
        toppra=mock_topptra,
        ik=mock_ik,
        roboplan_module=roboplan_module,
        group_name="arm"
    )
