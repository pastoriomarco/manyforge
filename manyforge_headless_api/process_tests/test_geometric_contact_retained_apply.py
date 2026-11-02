"""First geometric input added across a real retained ROS Mock Apply."""

from copy import deepcopy
from pathlib import Path
import time

import pytest

from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.process_tests.test_interrupt_signal_nodes_process import (
    REPOSITORY_ROOT,
    SCENARIO_ID,
    _load,
    _signal_stack,
    _skip_reason,
)
from manyforge_headless_api.process_tests.test_ui6i_grouped_execution import (
    _activate_loaded_components,
    _disable_for_reload,
    _wait_components_reached_requested_target,
)


def _geometric_candidate(project: Path) -> Path:
    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        load_scenario_artifact,
        save_scenario_artifact,
    )

    directory = project.parent
    original, _ = load_scenario_artifact(str(project), allowed_roots=[str(directory)])
    deployment = deepcopy(original["deployment_draft"])
    station = next(row for row in deployment["components"] if row["component_id"] == "station_io")
    signals = station["configuration"]["runtime_configuration"]["io_signals"]
    assert all(row["provider"]["profile"] != "geometric_contact" for row in signals)
    contact = next(row for row in signals if row["resource_role_id"] == "probe_contact")
    contact["provider"] = {
        "profile": "geometric_contact",
        "ingress_depth": 4,
        "robot_component_id": "lite6_robot",
        "target_scene_resource_id": "probe_block",
    }
    contact["update_mode"] = "sampled"
    contact["freshness"] = {"mode": "sample_age", "maximum_age_ns": 5_000_000_000}
    candidate = build_scenario_document(
        scenario_id=SCENARIO_ID,
        name=SCENARIO_ID,
        description="Adds the first geometric input to a retained robot cell.",
        program_draft=original["program_draft"],
        deployment_draft=deployment,
        program_role_assignment=original["program_role_assignment"],
        manyforge_version=(REPOSITORY_ROOT / "VERSION").read_text(encoding="utf-8").strip(),
        last_known_status="unknown",
    )
    # Same Scenario identity at a second path: folder, id and name are one name.
    target = directory / "geometric-apply" / f"{SCENARIO_ID}.scenario"
    target.parent.mkdir()
    save_scenario_artifact(
        candidate, str(target), allowed_roots=[str(directory)], overwrite=False,
        source_project_path=str(project),
    )
    return target


@pytest.mark.skipif(_skip_reason() is not None, reason=str(_skip_reason()))
def test_first_geometric_profile_recollects_retained_cell_sources() -> None:
    with _signal_stack() as (stack, project):
        before_apply = _load(stack, project, activate_components=False)
        components = _activate_loaded_components(stack)
        assert {"lite6_robot", "lite_gripper", "station_io"}.issubset(components)
        _wait_components_reached_requested_target(stack, components)

        # Load is Close, then Apply of the file (CONTRACTS §11): every
        # component comes back prepared and is enabled again.
        candidate = _geometric_candidate(project)
        _disable_for_reload(stack, "station_io")
        applied = _load(stack, candidate, activate_components=False)
        components = _activate_loaded_components(stack)
        _wait_components_reached_requested_target(stack, components)
        assert applied["retained_bundle_id"] != before_apply["retained_bundle_id"]

        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            owner = headless.execution_identity()["identity"]["owner_epoch"]
            request = {
                "schema_version": "manyforge.target_observation_query.v1",
                "owner_epoch": owner,
                "program_binding_id": applied["program_binding_id"],
                "target_role_id": "station",
                "kind": "io_signal",
                "resource_role_id": "probe_contact",
            }
            deadline = time.monotonic() + 10.0
            while True:
                observation = headless.read_io_signal(request)["target_observation"]["value"]
                error = observation.get("error") or {}
                if (observation["quality"] == "unavailable"
                        and error.get("code") == "io_source_unavailable"
                        and error.get("details", {}).get("reason") == "observed_state_missing_or_stale"):
                    break
                assert time.monotonic() < deadline, observation
                time.sleep(0.02)
            # The maintained fixture's split gripper is pneumatic, so the
            # collected coordinate exists but has unavailable authority.
            assert observation["value"] is None, observation
            assert observation["source_id"] == "station_io.probe_contact", observation
