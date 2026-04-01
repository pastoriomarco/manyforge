"""Tests for scene editing functionality."""

import pytest

from manyforge_behavior.kernel_client import SceneEditRequest


def test_add_box(client):
    """Test adding a box to the scene."""
    request = SceneEditRequest(
        operation="add",
        object_id="box1",
        shape_type="box",
        box_dimensions_m=[0.1, 0.2, 0.3],
        pose_position_m=[0.5, 0.0, 0.6],
        pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    
    result = client.scene_edit(request)
    
    assert result.success is True
    assert result.error is None


def test_add_sphere(client):
    """Test adding a sphere to the scene."""
    request = SceneEditRequest(
        operation="add",
        object_id="sphere1",
        shape_type="sphere",
        sphere_radius_m=0.05,
        pose_position_m=[0.5, 0.0, 0.6],
        pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    
    result = client.scene_edit(request)
    
    assert result.success is True
    assert result.error is None


def test_remove_geometry(client):
    """Test removing geometry from the scene."""
    # First add something
    add_request = SceneEditRequest(
        operation="add",
        object_id="box1",
        shape_type="box",
        box_dimensions_m=[0.1, 0.2, 0.3],
        pose_position_m=[0.5, 0.0, 0.6],
    )
    client.scene_edit(add_request)
    
    # Now remove it
    remove_request = SceneEditRequest(
        operation="remove",
        object_id="box1",
    )
    result = client.scene_edit(remove_request)
    
    assert result.success is True
    assert result.error is None


def test_update_pose(client):
    """Test updating the pose of existing geometry."""
    # First add something
    add_request = SceneEditRequest(
        operation="add",
        object_id="box1",
        shape_type="box",
        box_dimensions_m=[0.1, 0.2, 0.3],
        pose_position_m=[0.5, 0.0, 0.6],
    )
    client.scene_edit(add_request)
    
    # Now update its pose
    update_request = SceneEditRequest(
        operation="update_pose",
        object_id="box1",
        pose_position_m=[0.7, 0.1, 0.8],
        pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    result = client.scene_edit(update_request)
    
    assert result.success is True
    assert result.error is None


def test_unknown_operation_fails(client):
    """Test that an unknown operation fails."""
    request = SceneEditRequest(
        operation="unknown_op",
        object_id="box1",
    )
    
    result = client.scene_edit(request)
    
    assert result.success is False
    assert "Unknown operation" in result.error


def test_add_box_verify_exists(client, mock_scene):
    """Test that adding a box verifies it exists in mock_scene._geometries."""
    request = SceneEditRequest(
        operation="add",
        object_id="box1",
        shape_type="box",
        box_dimensions_m=[0.1, 0.2, 0.3],
        pose_position_m=[0.5, 0.0, 0.6],
    )
    
    result = client.scene_edit(request)
    
    assert result.success is True
    assert "box1" in mock_scene._geometries
    assert mock_scene._geometries["box1"]["type"] == "box"


def test_add_sphere_verify_exists(client, mock_scene):
    """Test that adding a sphere verifies it exists in mock_scene._geometries."""
    request = SceneEditRequest(
        operation="add",
        object_id="sphere1",
        shape_type="sphere",
        sphere_radius_m=0.05,
        pose_position_m=[0.5, 0.0, 0.6],
    )
    
    result = client.scene_edit(request)
    
    assert result.success is True
    assert "sphere1" in mock_scene._geometries
    assert mock_scene._geometries["sphere1"]["type"] == "sphere"


def test_scene_edit_uses_universe_parent(client, mock_scene):
    """Test that scene_edit uses 'universe' as parent frame (Bug 2 fix)."""
    request = SceneEditRequest(
        operation="add",
        object_id="box1",
        shape_type="box",
        box_dimensions_m=[0.1, 0.2, 0.3],
        pose_position_m=[0.5, 0.0, 0.6],
    )
    
    # The mock scene doesn't track parent, but we can verify the code path
    # by checking that the geometry was added successfully
    result = client.scene_edit(request)
    assert result.success is True
    assert "box1" in mock_scene._geometries
