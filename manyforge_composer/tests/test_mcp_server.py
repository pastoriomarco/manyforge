"""Tests for ManyForge MCP Server."""
from __future__ import annotations

import asyncio
import json
import logging
import time
from pathlib import Path

import pytest
import yaml

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.mcp_server import create_mcp_server
from manyforge_composer.backend.state import ComposerState

logger = logging.getLogger(__name__)

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml"
)
_DEPLOYMENT_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml"
)


def _write_program_with_runtime_policy(
    tmp_path: Path,
    *,
    runtime_override: str = "llm_allowed",
    persistent_edit: str = "human_approval",
    intervention_delegate: str = "forbidden",
) -> str:
    data = yaml.safe_load(Path(_PROGRAM_PATH).read_text(encoding="utf-8"))
    move_node = data["tree"]["children"][0]
    move_node["modification_policy"] = {
        "runtime_override": runtime_override,
        "persistent_edit": persistent_edit,
        "intervention_delegate": intervention_delegate,
    }
    target = tmp_path / "runtime_override.program.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)


def _make_state(execute_duration_s: float = 0.0) -> ComposerState:
    kernel = MockKernelClient(execute_duration_s=execute_duration_s)
    return ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=["/workspace", "/tmp"],
    )


def _call(mcp, tool_name: str, arguments: dict | None = None) -> dict:
    """Synchronously call an MCP tool and parse the JSON result."""
    contents, _raw = asyncio.run(mcp.call_tool(tool_name, arguments or {}))
    return json.loads(contents[0].text)


# =============================================================================
# Tool Discovery
# =============================================================================

class TestToolDiscovery:

    def test_all_tools_registered(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        tools = asyncio.run(mcp.list_tools())
        tool_names = sorted(t.name for t in tools)
        expected = sorted([
            "manyforge_scene_state",
            "manyforge_scene_edit",
            "manyforge_program_load",
            "manyforge_program_tree",
            "manyforge_program_validate",
            "manyforge_runtime_override",
            "manyforge_cycle_control",
            "manyforge_capabilities",
            "manyforge_skills",
            "manyforge_intervention",
            "manyforge_execution_status",
            "manyforge_debug_export",
            "manyforge_catalog",
            "manyforge_scene_snapshot",
            "manyforge_program_save",
            "manyforge_files",
            "manyforge_deployment",
            "manyforge_kernel",
            "manyforge_session",
        ])
        assert tool_names == expected

    def test_tools_have_descriptions(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        tools = asyncio.run(mcp.list_tools())
        for tool in tools:
            assert tool.description, f"Tool {tool.name} has no description"

    def test_tools_have_input_schemas(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        tools = asyncio.run(mcp.list_tools())
        for tool in tools:
            assert tool.inputSchema is not None, f"Tool {tool.name} has no input schema"


# =============================================================================
# Scene State
# =============================================================================

class TestSceneState:

    def test_scene_state_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_scene_state")
        assert "timestamp_ns" in result
        assert result["objects"] == []

    def test_scene_state_with_program(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_scene_state")
        assert result["robot"] is not None
        assert isinstance(result["objects"], list)


# =============================================================================
# Scene Edit
# =============================================================================

class TestSceneEdit:

    def test_add_object(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_scene_edit", {
            "operation": "add",
            "object_id": "test_box",
            "shape_type": "box",
            "box_dimensions_m": [0.05, 0.05, 0.05],
            "pose_position_m": [0.3, 0.0, 0.15],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        })
        assert result["success"] is True

    def test_remove_nonexistent_object(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_scene_edit", {
            "operation": "remove",
            "object_id": "nonexistent",
        })
        # Mock kernel returns success=False with an error for missing objects
        assert "success" in result


# =============================================================================
# Program Load
# =============================================================================

class TestProgramLoad:

    def test_load_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_load", {
            "program_path": _PROGRAM_PATH,
            "deployment_path": _DEPLOYMENT_PATH,
        })
        assert "error" not in result
        assert result["name"] is not None
        assert result["source_path"] == _PROGRAM_PATH

    def test_load_nonexistent_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_load", {
            "program_path": "/workspace/nonexistent/program.yaml",
        })
        assert "error" in result

    def test_load_program_path_traversal_blocked(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_load", {
            "program_path": "/etc/passwd",
        })
        assert "error" in result
        assert "outside allowed roots" in result["error"]

    def test_load_deployment_path_traversal_blocked(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_load", {
            "program_path": _PROGRAM_PATH,
            "deployment_path": "/etc/shadow",
        })
        assert "error" in result
        assert "outside allowed roots" in result["error"]


# =============================================================================
# Program Tree
# =============================================================================

class TestProgramTree:

    def test_read_tree_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_tree", {"action": "read"})
        assert result.get("error") == "No program loaded"

    def test_read_tree(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_tree", {"action": "read"})
        assert "tree" in result
        assert result["tree"]["id"] is not None

    def test_write_tree(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        # Read current tree
        current = _call(mcp, "manyforge_program_tree", {"action": "read"})
        tree = current["tree"]

        # Write it back (no-op change)
        result = _call(mcp, "manyforge_program_tree", {
            "action": "write",
            "tree": tree,
        })
        assert result.get("status") == "ok"

    def test_write_tree_no_body(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_tree", {"action": "write"})
        assert "error" in result

    def test_read_tree_reflects_active_runtime_override(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        override_result = _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        assert override_result["has_active"] is True

        tree = _call(mcp, "manyforge_program_tree", {"action": "read"})["tree"]
        assert tree["children"][0]["params"]["planning_timeout_s"] == 3.5
        assert tree["children"][0]["modification_policy"]["runtime_override"] == "llm_allowed"


# =============================================================================
# Program Validate
# =============================================================================

class TestProgramValidate:

    def test_validate_valid_tree(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        tree = _call(mcp, "manyforge_program_tree", {"action": "read"})["tree"]
        result = _call(mcp, "manyforge_program_validate", {"tree": tree})
        assert result["valid"] is True
        assert result["errors"] == []

    def test_validate_invalid_tree(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_validate", {
            "tree": {"id": "nonexistent_node", "name": "bad", "params": {}, "children": []},
        })
        assert result["valid"] is False
        assert len(result["errors"]) > 0


# =============================================================================
# Cycle Control
# =============================================================================

class TestCycleControl:

    def test_status_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_cycle_control", {"command": "status"})
        assert result["state"] == "idle"

    def test_run_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_cycle_control", {"command": "run"})
        assert "error" in result

    def test_run_stop_reset_lifecycle(self):
        state = _make_state(execute_duration_s=5.0)
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        result = _call(mcp, "manyforge_cycle_control", {"command": "run"})
        assert result["state"] == "running"

        result = _call(mcp, "manyforge_cycle_control", {"command": "stop"})
        assert result["state"] == "stopped"

        result = _call(mcp, "manyforge_cycle_control", {"command": "reset"})
        assert result["state"] == "idle"

    def test_velocity_scale(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_cycle_control", {
            "command": "velocity",
            "velocity_scale": 0.5,
        })
        assert result["velocity_scale"] == 0.5

    def test_unknown_command(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_cycle_control", {"command": "explode"})
        assert "error" in result

    def test_velocity_nan_rejected(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_cycle_control", {
            "command": "velocity",
            "velocity_scale": float("nan"),
        })
        assert "error" in result

    def test_velocity_inf_rejected(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_cycle_control", {
            "command": "velocity",
            "velocity_scale": float("inf"),
        })
        assert "error" in result


# =============================================================================
# Capabilities
# =============================================================================

class TestCapabilities:

    def test_capabilities_no_deployment(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_capabilities")
        assert "error" in result

    def test_capabilities_with_deployment(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_capabilities")
        assert "planning" in result
        assert "execution" in result
        assert "program_profile" in result


# =============================================================================
# Skills
# =============================================================================

class TestSkills:

    def test_skills_catalog(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_skills")
        assert "skills" in result
        assert isinstance(result["skills"], list)
        assert len(result["skills"]) > 0

    def test_skill_has_required_fields(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_skills")
        skill = result["skills"][0]
        for field in ("id", "name", "description", "parameters", "body", "expands_to"):
            assert field in skill, f"Skill missing field: {field}"


# =============================================================================
# Execution Status
# =============================================================================

class TestExecutionStatus:

    def test_digest_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_execution_status", {"include": "digest"})
        assert "digest" in result
        assert result["digest"]["state"] == "idle"

    def test_blackboard(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_execution_status", {"include": "blackboard"})
        assert "blackboard" in result
        assert "keys" in result["blackboard"]

    def test_history(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_execution_status", {"include": "history"})
        assert "history" in result

    def test_all(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_execution_status", {"include": "all"})
        assert "digest" in result
        assert "history" in result
        assert "blackboard" in result

    def test_unknown_include(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_execution_status", {"include": "invalid"})
        assert "error" in result


# =============================================================================
# Debug Export
# =============================================================================

class TestDebugExport:

    def test_debug_export(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_debug_export")
        assert "exported_at_ns" in result
        assert "kernel_snapshot" in result
        assert "execution_digest" in result
        assert "scene_state" in result


# =============================================================================
# Catalog
# =============================================================================

class TestCatalog:

    def test_full_catalog(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_catalog")
        assert "entries" in result
        assert len(result["entries"]) > 0
        assert "version" in result

    def test_single_entry(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_catalog", {"entry_id": "sequence"})
        assert result["id"] == "sequence"
        assert "parameters" in result

    def test_single_entry_includes_blackboard_metadata(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_catalog", {"entry_id": "set_key_bool_value"})
        assert result["bb_reader_params"] == []
        assert result["bb_writer_params"] == ["key"]

    def test_missing_entry(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_catalog", {"entry_id": "nonexistent"})
        assert "error" in result


# =============================================================================
# Intervention
# =============================================================================

class TestIntervention:

    def test_intervention_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_intervention", {"action": "status"})
        assert "error" in result

    def test_intervention_execute_and_status(self):
        state = _make_state(execute_duration_s=0.0)
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        tree = {
            "id": "timer",
            "name": "test_wait",
            "params": {"duration_s": 0.01},
            "children": [],
        }
        result = _call(mcp, "manyforge_intervention", {
            "action": "execute",
            "tree": tree,
            "session_kind": "manual_intervention",
            "authority_source": "human",
            "principal_id": "human:test-operator",
            "approval_mode": "human_approved",
        })
        assert "session_id" in result
        assert result["session_kind"] == "manual_intervention"
        assert result["authority_source"] == "human"
        assert result["principal_id"] == "human:test-operator"
        assert result["grant_type"] == "human_approved"
        assert result["status"] in ("running", "completed")

    def test_intervention_execute_from_idle_requires_human_approval(self):
        state = _make_state(execute_duration_s=0.0)
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        tree = {
            "id": "timer",
            "name": "test_wait",
            "params": {"duration_s": 0.01},
            "children": [],
        }
        result = _call(mcp, "manyforge_intervention", {
            "action": "execute",
            "tree": tree,
            "session_kind": "manual_intervention",
            "authority_source": "llm",
            "principal_id": "agent:test-llm",
            "grant_type": "policy_granted",
        })
        assert "error" in result
        assert "human-approved manual intervention" in result["error"]

    def test_intervention_no_tree(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_intervention", {"action": "execute"})
        assert "error" in result

    def test_intervention_abort_no_session(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_intervention", {"action": "abort"})
        assert "error" in result

    def test_intervention_unknown_action(self):
        state = _make_state()
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_intervention", {"action": "explode"})
        assert "error" in result

    def test_pending_delegated_recovery_request_exposed(self, tmp_path):
        state = _make_state()
        state.load_program(
            _write_program_with_runtime_policy(
                tmp_path,
                intervention_delegate="llm_allowed",
                persistent_edit="llm_allowed",
            ),
            _DEPLOYMENT_PATH,
        )
        state.kernel.fail_next_execute = True
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_cycle_control", {"command": "run"})
        time.sleep(0.2)

        pending = _call(mcp, "manyforge_intervention", {"action": "pending_recovery"})
        assert pending["trigger_node_instance"] == "move_to_pick"
        assert pending["trigger_reason"] == "execute_failed"
        assert pending["delegate_policy"] == "llm_allowed"

    def test_execute_delegated_recovery_from_pending_request(self, tmp_path):
        state = _make_state()
        state.load_program(
            _write_program_with_runtime_policy(tmp_path, intervention_delegate="llm_allowed"),
            _DEPLOYMENT_PATH,
        )
        state.kernel.fail_next_execute = True
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_cycle_control", {"command": "run"})
        time.sleep(0.2)

        result = _call(mcp, "manyforge_intervention", {
            "action": "execute",
            "tree": {
                "id": "timer",
                "name": "delegated_pause",
                "params": {"duration_s": 0.01},
                "children": [],
            },
            "session_kind": "delegated_recovery",
            "authority_source": "llm",
            "principal_id": "agent:test-llm",
            "grant_type": "policy_granted",
            "resume_target": "stop",
        })
        assert result["session_kind"] == "delegated_recovery"
        assert result["trigger_node_instance"] == "move_to_pick"
        assert result["principal_id"] == "agent:test-llm"
        assert result["grant_type"] == "policy_granted"

    def test_intervention_promote_blackboard_value(self):
        state = _make_state(execute_duration_s=0.0)
        state.load_program(_PROGRAM_PATH, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        result = _call(mcp, "manyforge_intervention", {
            "action": "execute",
            "tree": {
                "id": "set_key_bool_value",
                "name": "publish_detection",
                "params": {"key": "detected_ok", "value": True},
                "children": [],
            },
            "session_kind": "manual_intervention",
            "authority_source": "human",
            "principal_id": "human:test-operator",
            "approval_mode": "human_approved",
        })
        assert "session_id" in result

        promoted = _call(mcp, "manyforge_intervention", {
            "action": "promote",
            "promote_keys": {"detected_ok": "recovery.detected_ok"},
        })
        assert promoted["promotions"]

        blackboard = _call(mcp, "manyforge_execution_status", {"include": "blackboard"})
        assert blackboard["blackboard"]["keys"]["recovery/detected_ok"]["value_json"] is True


# =============================================================================
# Runtime Overrides
# =============================================================================

class TestRuntimeOverrides:

    def test_status_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_runtime_override", {"action": "status"})
        assert "error" in result

    def test_apply_and_clear(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        result = _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 4.0},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        assert result["has_active"] is True
        assert result["active"][0]["node_name"] == "move_to_pick"

        cleared = _call(mcp, "manyforge_runtime_override", {"action": "clear"})
        assert cleared["has_active"] is False
        assert cleared["active_count"] == 0

    def test_staged_override_requires_reset_before_stopped_resume(self, tmp_path):
        state = _make_state(execute_duration_s=5.0)
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        state.cycle_manager._state = CycleState.DIRECT_CONTROL

        result = _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 4.0},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        assert result["has_staged"] is True

        state.cycle_manager._state = CycleState.STOPPED
        run_result = _call(mcp, "manyforge_cycle_control", {"command": "run"})
        assert "error" in run_result
        assert "reset the cycle first" in run_result["error"]

    def test_materialized_tree_can_include_staged_overrides(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)
        state.cycle_manager._state = CycleState.DIRECT_CONTROL

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 4.0},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        current_tree = _call(mcp, "manyforge_runtime_override", {
            "action": "materialized_tree",
        })
        staged_tree = _call(mcp, "manyforge_runtime_override", {
            "action": "materialized_tree",
            "include_staged": True,
        })

        assert current_tree["tree"]["children"][0]["params"]["planning_timeout_s"] == 2.0
        assert staged_tree["tree"]["children"][0]["params"]["planning_timeout_s"] == 4.0

    def test_promote_loaded_program_materializes_and_clears_ledger(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 4.0},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        result = _call(mcp, "manyforge_runtime_override", {
            "action": "promote",
            "target": "loaded_program",
        })

        assert result["status"] == "ok"
        assert result["runtime_override_status"]["has_active"] is False
        tree = _call(mcp, "manyforge_program_tree")
        assert tree["tree"]["children"][0]["params"]["planning_timeout_s"] == 4.0

    def test_promote_save_requires_persistent_edit_policy(self, tmp_path):
        state = _make_state()
        state.load_program(
            _write_program_with_runtime_policy(tmp_path, persistent_edit="forbidden"),
            _DEPLOYMENT_PATH,
        )
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 4.0},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        result = _call(mcp, "manyforge_runtime_override", {
            "action": "promote",
            "target": "save",
            "path": str(tmp_path / "saved_materialized"),
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        assert "does not allow persistent edits" in result["error"]


# =============================================================================
# Runtime Override Revision Binding (OP-5)
# =============================================================================


class TestRuntimeOverrideRevisionBinding:

    def test_override_status_includes_base_program_hash(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        result = _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })
        assert "base_program_hash" in result
        assert len(result["base_program_hash"]) == 64
        assert result["active"][0]["base_program_hash"] == result["base_program_hash"]
        assert result["active"][0]["is_stale"] is False
        assert result["stale_count"] == 0
        assert result["has_stale"] is False

    def test_load_program_rejects_with_existing_overrides(self, tmp_path):
        state = _make_state()
        path = _write_program_with_runtime_policy(tmp_path)
        state.load_program(path, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        result = _call(mcp, "manyforge_program_load", {
            "program_path": path,
            "deployment_path": _DEPLOYMENT_PATH,
        })
        assert "error" in result
        assert "runtime overrides exist" in result["error"].lower()

    def test_load_program_with_force_discard_overrides(self, tmp_path):
        state = _make_state()
        path = _write_program_with_runtime_policy(tmp_path)
        state.load_program(path, _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        result = _call(mcp, "manyforge_program_load", {
            "program_path": path,
            "deployment_path": _DEPLOYMENT_PATH,
            "force_discard_overrides": True,
        })
        assert "error" not in result
        assert result["name"]

    def test_revalidate_action_with_no_stale(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        result = _call(mcp, "manyforge_runtime_override", {
            "action": "revalidate",
        })
        assert result["stale_count"] == 0
        assert result["revalidation_results"] == {}

    def test_stale_detected_after_hash_change(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        state._program_tree_hash = "0" * 64

        result = _call(mcp, "manyforge_runtime_override", {"action": "status"})
        assert result["has_stale"] is True
        assert result["stale_count"] == 1
        assert result["stale"][0]["is_stale"] is True

    def test_stale_override_blocks_materialized_tree(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        state._program_tree_hash = "0" * 64

        result = _call(mcp, "manyforge_runtime_override", {
            "action": "materialized_tree",
        })
        assert "error" in result
        assert "stale" in result["error"].lower()

    def test_revalidation_succeeds_for_valid_override(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        real_hash = state._program_tree_hash
        for record in state._runtime_overrides.values():
            record.base_program_hash = "0" * 64

        result = _call(mcp, "manyforge_runtime_override", {"action": "revalidate"})
        assert result["revalidation_results"]["move_to_pick"] == "revalidated"
        assert result["stale_count"] == 0
        assert result["active"][0]["base_program_hash"] == real_hash

    def test_invalidation_removes_unknown_node(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        for record in state._runtime_overrides.values():
            record.base_program_hash = "0" * 64
        state.program.tree.children[0].name = "renamed_node"

        result = _call(mcp, "manyforge_runtime_override", {"action": "revalidate"})
        assert result["revalidation_results"]["move_to_pick"] == "invalid"
        assert result["active_count"] == 0

    def test_tree_write_rejects_with_overrides(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        tree = _call(mcp, "manyforge_program_tree", {"action": "read"})["tree"]
        result = _call(mcp, "manyforge_program_tree", {
            "action": "write",
            "tree": tree,
        })
        assert "error" in result
        assert "runtime overrides exist" in result["error"].lower()

    def test_tree_write_with_force_discard_overrides(self, tmp_path):
        state = _make_state()
        state.load_program(_write_program_with_runtime_policy(tmp_path), _DEPLOYMENT_PATH)
        mcp = create_mcp_server(state)

        _call(mcp, "manyforge_runtime_override", {
            "action": "apply",
            "node_name": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authority_source": "llm",
            "approval_mode": "not_required",
        })

        tree = _call(mcp, "manyforge_program_tree", {"action": "read"})["tree"]
        result = _call(mcp, "manyforge_program_tree", {
            "action": "write",
            "tree": tree,
            "force_discard_overrides": True,
        })
        assert result.get("status") == "ok"

        status = _call(mcp, "manyforge_runtime_override", {"action": "status"})
        assert status["active_count"] == 0


# =============================================================================
# Scene Snapshot
# =============================================================================

class TestSceneSnapshot:

    def test_no_hmi_server(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_scene_snapshot")
        assert "error" in result
        assert "HMI" in result["error"]

    def test_invalid_format(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_scene_snapshot", {"format": "bmp"})
        assert "error" in result
        assert "format" in result["error"]


# =============================================================================
# Program Save
# =============================================================================

class TestProgramSave:

    def test_no_program_loaded(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_program_save")
        assert "error" in result
        assert "No program" in result["error"]

    def test_save_loaded_program(self, tmp_path):
        state = _make_state()
        mcp = create_mcp_server(state)
        _call(mcp, "manyforge_program_load", {"program_path": _PROGRAM_PATH})
        target = str(tmp_path / "saved.program.yaml")
        result = _call(mcp, "manyforge_program_save", {"path": target})
        assert result["status"] == "ok"
        assert result["path"] == target
        assert Path(target).exists()


# =============================================================================
# File Browser
# =============================================================================

class TestFileBrowser:

    def test_browse_default(self, tmp_path):
        state = ComposerState(
            catalog_path=_CATALOG_PATH,
            kernel=MockKernelClient(),
            allowed_fs_roots=[str(tmp_path)],
        )
        # Create a test program file
        (tmp_path / "test.program.yaml").write_text("tree: {}")
        (tmp_path / "subdir").mkdir()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_files", {"path": str(tmp_path)})
        assert "entries" in result
        names = [e["name"] for e in result["entries"]]
        assert "subdir" in names
        assert "test.program.yaml" in names

    def test_browse_kind_filter(self, tmp_path):
        state = ComposerState(
            catalog_path=_CATALOG_PATH,
            kernel=MockKernelClient(),
            allowed_fs_roots=[str(tmp_path)],
        )
        (tmp_path / "test.program.yaml").write_text("tree: {}")
        (tmp_path / "test.deployment.yaml").write_text("name: test")
        mcp = create_mcp_server(state)

        # Program kind should not include deployment files
        result = _call(mcp, "manyforge_files", {"path": str(tmp_path), "kind": "program"})
        file_names = [e["name"] for e in result["entries"] if e["type"] == "file"]
        assert "test.program.yaml" in file_names
        assert "test.deployment.yaml" not in file_names

        # Deployment kind should not include program files
        result = _call(mcp, "manyforge_files", {"path": str(tmp_path), "kind": "deployment"})
        file_names = [e["name"] for e in result["entries"] if e["type"] == "file"]
        assert "test.deployment.yaml" in file_names
        assert "test.program.yaml" not in file_names

    def test_browse_invalid_kind(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_files", {"kind": "invalid"})
        assert "error" in result

    def test_browse_outside_roots(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_files", {"path": "/etc"})
        assert "error" in result


# =============================================================================
# Deployment Reader
# =============================================================================

class TestDeploymentReader:

    def test_no_deployment_loaded(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_deployment")
        assert "error" in result
        assert "No deployment" in result["error"]

    def test_read_deployment(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        _call(mcp, "manyforge_program_load", {
            "program_path": _PROGRAM_PATH,
            "deployment_path": _DEPLOYMENT_PATH,
        })
        result = _call(mcp, "manyforge_deployment")
        assert "error" not in result
        assert "name" in result or "robot_config" in result


class TestKernel:
    """Tests for manyforge_kernel tool."""

    def test_kernel_snapshot_no_kernel(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_kernel", {"include": "snapshot"})
        assert "snapshot" in result
        assert result["snapshot"]["state"] == "idle"

    def test_kernel_snapshot_with_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        _call(mcp, "manyforge_program_load", {"program_path": _PROGRAM_PATH})
        result = _call(mcp, "manyforge_kernel", {"include": "snapshot"})
        assert "snapshot" in result

    def test_kernel_metrics(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        _call(mcp, "manyforge_program_load", {"program_path": _PROGRAM_PATH})
        result = _call(mcp, "manyforge_kernel", {"include": "metrics"})
        assert "metrics" in result

    def test_kernel_all(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_kernel", {"include": "all"})
        assert "snapshot" in result

    def test_kernel_invalid_include(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_kernel", {"include": "invalid"})
        assert "error" in result


class TestSession:
    """Tests for manyforge_session tool."""

    def test_session_status_no_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_session", {"action": "status"})
        assert result["program_loaded"] is False
        assert result["cycle_state"] == "none"

    def test_session_status_with_program(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        _call(mcp, "manyforge_program_load", {"program_path": _PROGRAM_PATH})
        result = _call(mcp, "manyforge_session", {"action": "status"})
        assert result["program_loaded"] is True
        assert result["program_name"] is not None

    def test_session_reset(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        _call(mcp, "manyforge_program_load", {"program_path": _PROGRAM_PATH})
        result = _call(mcp, "manyforge_session", {"action": "reset"})
        assert result["status"] == "ok"

    def test_session_invalid_action(self):
        state = _make_state()
        mcp = create_mcp_server(state)
        result = _call(mcp, "manyforge_session", {"action": "invalid"})
        assert "error" in result
