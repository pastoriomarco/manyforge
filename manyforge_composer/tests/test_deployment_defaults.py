"""Backend-owned Deployment defaults and effective values."""

from manyforge_composer.backend.deployment_defaults import effective_fields, with_defaults


def _document() -> dict:
    return {
        "api_version": "manyforge.deployment_intent.v2",
        "components": [
            {
                "component_id": "lite6_robot",
                "lane": "real",
                "configuration": {
                    "runtime_kind": "ros2_control",
                    "runtime_configuration": {"execution_tick_hz": 250},
                },
            },
            {
                "component_id": "lite_gripper",
                "configuration": {"runtime_kind": "ros2_control", "runtime_configuration": {}},
            },
            {
                "component_id": "plc_io",
                "configuration": {"runtime_kind": "io_provider", "runtime_configuration": {}},
            },
            {
                "component_id": "ur5e_robot",
                "lane": "mock",
                "configuration": {"runtime_kind": "ros2_control", "runtime_configuration": {}},
            },
        ],
        "attachments": [
            {
                "parent": {"component_id": "lite6_robot", "interface": "flange"},
                "child": {"component_id": "lite_gripper", "interface": "mount"},
            }
        ],
        "policies": {
            "interrupt_rules": [
                {"rule_id": "door_open", "on_trip": "stop", "active_at_start": True},
                {"rule_id": "part_seen", "on_trip": "notify"},
            ]
        },
    }


def test_effective_values_name_value_default_and_whether_set() -> None:
    fields = effective_fields(_document())["fields"]
    runtime = "components.0.configuration.runtime_configuration"
    assert fields[f"{runtime}.execution_tick_hz"] == {"value": 250, "default": 100, "set": True}
    assert fields[f"{runtime}.collision_monitor_period_ms"] == {
        "value": 50.0, "default": 50.0, "set": False,
    }
    assert fields[f"{runtime}.planner_rng_seed"]["set"] is False
    assert fields[f"{runtime}.collision_check_step_rad"]["default"] == 0.05
    assert fields[f"{runtime}.collision_clearance_m"]["default"] == 0.001
    # robot_defaults.tsv: the fresh-plan pre-dispatch check is on by default.
    assert fields[f"{runtime}.collision_check_fresh_plans"] == {
        "value": True, "default": True, "set": False,
    }
    # The planner block's Deployment default.
    assert fields[f"{runtime}.planner_search"] == {
        "value": "rrt_connect", "default": "rrt_connect", "set": False,
    }
    assert fields[f"{runtime}.planner_optimize"]["default"] == "first"
    assert fields[f"{runtime}.planner_n"]["default"] == 4
    assert fields[f"{runtime}.planner_max_nodes"]["default"] == 5000
    assert fields[f"{runtime}.cartesian_timing"] == {
        "value": "constant_speed", "default": "constant_speed", "set": False,
    }
    # Joint and pose goals are timed smooth unless the robot says fast.
    assert fields[f"{runtime}.motion_timing"] == {
        "value": "smooth", "default": "smooth", "set": False,
    }
    assert fields[f"{runtime}.stop_lead_in_anchor"]["value"] == "commanded"
    assert fields["policies.interrupt_rules.0.active_at_start"] == {
        "value": True, "default": False, "set": True,
    }
    assert fields["policies.interrupt_rules.0.after_stop"] == {
        "value": "hold", "default": "hold", "set": False,
    }
    # `after_stop` exists only for `on_trip: stop`.
    assert "policies.interrupt_rules.1.after_stop" not in fields
    assert fields["policies.interrupt_rules.1.active_at_start"]["value"] is False
    # Robots only: not the mounted tool or the I/O device; Real-only fields
    # only on the Real robot.
    assert not any(path.startswith(("components.1.", "components.2.")) for path in fields)
    mock = "components.3.configuration.runtime_configuration"
    assert f"{mock}.execution_tick_hz" in fields
    assert f"{mock}.stop_lead_in_anchor" not in fields


def test_no_document_has_no_fields() -> None:
    assert effective_fields(None) == {"fields": {}}


def test_required_defaults_are_filled_before_compile_without_touching_the_draft() -> None:
    document = _document()
    filled = with_defaults(document)
    runtime = filled["components"][0]["configuration"]["runtime_configuration"]
    assert runtime == {
        "execution_tick_hz": 250,
        "collision_monitor_period_ms": 50.0,
        "planner_rng_seed": 12345,
    }
    assert filled["components"][1]["configuration"]["runtime_configuration"] == {}
    assert filled["components"][2]["configuration"]["runtime_configuration"] == {}
    # Rule defaults are optional in the compiled schema (absent = default).
    assert filled["policies"] == document["policies"]
    assert document == _document()


def test_lifecycle_budget_is_reported_for_every_component_with_its_own_lifecycle() -> None:
    document = {
        "components": [
            {
                "component_id": "lite6",
                "component_profile_id": "ufactory.lite6_machine",
                "lane": "real",
                "configuration": {"device_address_ref": "lite6_main"},
            },
            {
                "component_id": "lite6_robot",
                "owner": "lite6",
                "role": "arm",
                "target_settings": {},
                "configuration": {
                    "runtime_kind": "ros2_control",
                    "lifecycle_command_timeout_s": 30.0,
                    "runtime_configuration": {},
                },
            },
            {"component_id": "lite6_robot_tool_io", "owner": "lite6", "role": "tool_io"},
            {
                "component_id": "llm",
                "target_settings": {},
                "configuration": {"runtime_kind": "ai_model", "runtime_configuration": {}},
            },
            {
                "component_id": "plc_io",
                "target_settings": {},
                "configuration": {"runtime_kind": "io_provider", "runtime_configuration": {}},
            },
        ]
    }
    fields = effective_fields(document)["fields"]
    budgets = {
        path: row for path, row in fields.items() if path.endswith(".lifecycle_command_timeout_s")
    }
    assert budgets == {
        "components.1.configuration.lifecycle_command_timeout_s": {
            "value": 30.0, "default": 10.0, "set": True,
        },
        "components.3.configuration.lifecycle_command_timeout_s": {
            "value": 10.0, "default": 10.0, "set": False,
        },
    }
    # Optional in the compiled schema: never filled in before compile.
    assert "lifecycle_command_timeout_s" not in with_defaults(document)["components"][3]["configuration"]


def test_takeover_planning_budget_defaults_to_four_fifths_of_the_total() -> None:
    assert effective_fields({"policies": {}})["fields"] == {}
    fields = effective_fields({"policies": {"motion_takeover_preparation_budget_s": 0.5}})["fields"]
    assert fields["policies.motion_takeover_planning_budget_s"] == {
        "value": 0.4, "default": 0.4, "set": False,
    }
    fields = effective_fields({"policies": {
        "motion_takeover_preparation_budget_s": 0.5,
        "motion_takeover_planning_budget_s": 0.3,
    }})["fields"]
    assert fields["policies.motion_takeover_planning_budget_s"] == {
        "value": 0.3, "default": 0.4, "set": True,
    }


def _sensor_document(runtime: dict) -> dict:
    return {
        "api_version": "manyforge.deployment_intent.v2",
        "components": [{
            "component_id": "front_points",
            "component_profile_id": "manyforge.pointcloud_source",
            "target_settings": {},
            "configuration": {"runtime_kind": "sensor", "runtime_configuration": runtime},
        }],
        "attachments": [],
        "policies": {},
    }


def test_pointcloud_source_effective_values_without_frame_default() -> None:
    runtime = "components.0.configuration.runtime_configuration"
    settings = {"topic": "/p", "source_frame_id": "camera_link", "max_points": 5000}
    fields = effective_fields(_sensor_document(settings))["fields"]
    assert fields[f"{runtime}.max_points"] == {"value": 5000, "default": 200000, "set": True}
    assert fields[f"{runtime}.minimum_period_s"] == {"value": 0.1, "default": 0.1, "set": False}
    # Topic and source frame are required settings without a default (PC-1).
    assert f"{runtime}.source_frame_id" not in fields
    assert f"{runtime}.topic" not in fields
    filled = with_defaults(_sensor_document({"topic": "/p", "source_frame_id": "f"}))
    assert filled["components"][0]["configuration"]["runtime_configuration"] == {
        "topic": "/p", "source_frame_id": "f", "max_points": 200000, "minimum_period_s": 0.1,
    }


def test_a_mounted_sensor_keeps_its_defaults_and_lifecycle_time() -> None:
    document = _sensor_document({"topic": "/p"})
    document["components"].insert(0, {
        "component_id": "robot",
        "lane": "isaac",
        "target_settings": {},
        "configuration": {"runtime_kind": "ros2_control", "runtime_configuration": {}},
    })
    document["attachments"] = [{
        "parent": {"component_id": "robot", "interface": "flange"},
        "child": {"component_id": "front_points", "interface": "mount"},
    }]
    fields = effective_fields(document)["fields"]
    sensor = "components.1.configuration"
    assert fields[f"{sensor}.lifecycle_command_timeout_s"] == {
        "value": 10.0, "default": 10.0, "set": False,
    }
    assert fields[f"{sensor}.runtime_configuration.max_points"]["default"] == 200000
    assert f"{sensor}.runtime_configuration.execution_tick_hz" not in fields
    filled = with_defaults(document)["components"][1]["configuration"]["runtime_configuration"]
    assert filled == {"topic": "/p", "max_points": 200000, "minimum_period_s": 0.1}
