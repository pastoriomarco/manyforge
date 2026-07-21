"""python -m manyforge_composer --catalog-path <path> [--host 0.0.0.0] [--port 9000]"""
from __future__ import annotations

import argparse
from dataclasses import replace
import logging
import math
import os
from pathlib import Path
from typing import Any

import py_trees
import uvicorn

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_behavior.model_paths import (
    ResolvedRobotModel,
    default_package_paths,
    resolve_robot_model,
)
from manyforge_behavior.node_environment import (
    build_node_environment,
    configured_node_roots,
)
from manyforge_behavior.robot_model_runtime import (
    attach_robot_model_metadata,
    prepare_roboplan_runtime_model_view,
)
from manyforge_behavior.scenarios import configured_scenario_root_values
from manyforge_composer.backend.logging_config import setup_logging
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.assistant_provider import (
    AssistantProviderError,
    build_assistant_provider,
)
from manyforge_composer.backend.security import SecurityConfig

logger = logging.getLogger(__name__)

_ROS2_CONTROL_EXECUTION_ADAPTER_IDS = {
    "execution.ros2_control_fjt",
    "execution.ros2_control_follow_joint_trajectory",
}


def _node_environment_factory(catalog_path: str):
    roots = configured_node_roots()
    strict = os.environ.get("MANYFORGE_NODE_STRICT", "").lower() in {
        "1", "true", "yes", "on"
    }

    def build(generation: int):
        return build_node_environment(
            catalog_path,
            external_roots=roots,
            strict=strict,
            generation=generation,
        )

    return build


def _scenario_roots() -> list[str]:
    configured = configured_scenario_root_values()
    if configured:
        return list(configured)
    return [str(Path(__file__).resolve().parents[1] / "examples")]


def _configure_assistant_provider(
    state: ComposerState,
    provider_id: str | None,
    *,
    endpoint_url: str | None = None,
    timeout_s: float | None = None,
    api_token: str | None = None,
) -> None:
    configured = (provider_id or os.environ.get("MANYFORGE_ASSISTANT_PROVIDER", "")).strip()
    try:
        state.assistant_provider = build_assistant_provider(
            configured,
            endpoint_url=endpoint_url,
            timeout_s=timeout_s,
            api_token=api_token,
        )
    except AssistantProviderError as exc:
        logger.error("%s", exc)
        raise SystemExit(2) from exc
    if state.assistant_provider is not None:
        logger.info("Assistant provider configured: %s", state.assistant_provider.provider_id)

def _get_simulator_initial_joint_positions(state: ComposerState) -> list[float] | None:
    """Return explicit simulator-only startup joint positions from deployment config."""
    if state.deployment is None:
        return None
    execution = state.deployment.adapters.get("execution")
    if execution is None:
        return None
    if execution.adapter_id != "execution.roboplan_native":
        return None
    if execution.config.get("mode") != "simulated":
        return None
    positions = execution.config.get("initial_joint_positions_rad")
    if not isinstance(positions, list) or not positions:
        return None
    return [float(v) for v in positions]


def _default_public_api_base_url(host: str, port: int) -> str:
    override = os.environ.get("MANYFORGE_PUBLIC_API_BASE_URL", "").strip()
    if override:
        return override.rstrip("/")
    resolved_host = "localhost" if host in {"0.0.0.0", "::"} else host
    return f"http://{resolved_host}:{port}"


def _optional_float_env(name: str) -> float | None:
    value = os.environ.get(name, "").strip()
    return float(value) if value else None


def _optional_int_env(name: str) -> int | None:
    value = os.environ.get(name, "").strip()
    return int(value) if value else None


def _bool_env(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _resolved_robot_model_from_deployment(
    deployment: Deployment | None,
) -> ResolvedRobotModel | None:
    robot_config = getattr(deployment, "robot_config", None)
    if deployment is None or robot_config is None:
        return None
    if not robot_config.srdf_path:
        return None
    extra_package_paths = (
        []
        if robot_config.package_paths
        else default_package_paths()
    )
    return resolve_robot_model(
        urdf_path=robot_config.urdf_path,
        srdf_path=robot_config.srdf_path,
        package_paths=robot_config.package_paths,
        deployment_path=getattr(deployment, "source_path", None),
        extra_package_paths=extra_package_paths,
    )


def build_program_loaded_callback():
    """Build the Composer program-load callback for runtime visual state.

    The native viewer path is always active: behavior-tree scene mutations
    update ComposerState's visual object cache directly so attach/detach
    effects are visible without any external viewer server.
    """
    def on_program_loaded(st: ComposerState):
        if (
            st.cycle_manager is not None
            and hasattr(st.cycle_manager, "_runner")
            and st._resolve_scene_snapshot_provider() is not None
        ):
            st.cycle_manager._runner.set_scene_snapshot_provider(
                st._resolve_scene_snapshot_provider()
            )
            st.cycle_manager._runner.set_scene_snapshot_url_publisher(
                lambda image_bytes, width, height, format, **kwargs: st.publish_scene_snapshot(
                    image_bytes,
                    width=width,
                    height=height,
                    format=format,
                    fetch_base_url=kwargs.get("fetch_base_url"),
                    model_base_url=kwargs.get("model_base_url"),
                )
            )

        def on_scene_edit(operation: str, request):
            visual_request = request
            if getattr(request, "operation", operation) != operation:
                try:
                    visual_request = replace(request, operation=operation)
                except Exception:
                    visual_request = request
            visual_update = st.apply_visual_scene_edit_success(visual_request)
            if visual_update.get("needs_refresh"):
                st.request_visual_scene_objects_refresh(
                    reason=f"scene_edit_callback_{operation}"
                )
            logger.debug(
                "Scene invalidation requested after %s '%s'",
                operation,
                request.object_id,
                extra={
                    "event": "manyforge.visual.scene_edit_callback",
                    "operation": operation,
                    "object_id": request.object_id,
                    "visual_cache_updated": bool(visual_update.get("updated")),
                    "visual_refresh_requested": bool(visual_update.get("needs_refresh")),
                },
            )

        bb = py_trees.blackboard.Client(name="composer_scene_sync")
        bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.WRITE)
        bb.scene_edit_callback = on_scene_edit

        def _refresh_runtime_visuals():
            st.clear_visual_scene_objects_snapshot()
            st.refresh_visual_scene_objects_sync(reason="run_from_idle")
            st.request_visual_state_publish(reason="run_from_idle")

        st.cycle_manager._on_run_from_idle = _refresh_runtime_visuals
        # Program loading has already seeded the authored scene under the
        # scene-initialization barrier. Do not run a second Reset here: besides
        # being redundant, it would clear the ready scene after the barrier had
        # opened and could turn a concurrent pose observation into a false
        # post-ready apply fault. The first Run builds the behavior tree from
        # the seeded IDLE state, as CycleManager.seed_authored_scene() intends.
        initial_positions = _get_simulator_initial_joint_positions(st)
        runtime_bundle = getattr(st, "kernel_bundle", None)
        if runtime_bundle is not None and initial_positions:
            runtime_bundle.set_joint_positions(initial_positions)
            logger.info("Applied simulator-only initial joint positions from deployment")
        st.refresh_visual_scene_objects_sync(reason="program_loaded")
        st.request_visual_state_publish(reason="program_loaded")
        logger.info("Native visual runtime callbacks installed")

    return on_program_loaded


def _collision_monitor_period_ns_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get("collision_monitor_period_ms")
    if raw_value is None:
        return None
    period_ms = float(raw_value)
    if period_ms <= 0.0:
        raise ValueError("orchestrator.config.collision_monitor_period_ms must be > 0")
    return int(period_ms * 1_000_000.0)


def _runtime_collision_immediate_query_budget_ns_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get(
        "runtime_collision_immediate_query_budget_ms"
    )
    if raw_value is None:
        return None
    timeout_ms = float(raw_value)
    if timeout_ms <= 0.0:
        raise ValueError(
            "orchestrator.config.runtime_collision_immediate_query_budget_ms must be > 0"
        )
    return int(timeout_ms * 1_000_000.0)


def _validation_collision_query_timeout_ns_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get(
        "validation_collision_query_timeout_ms"
    )
    if raw_value is None:
        return None
    timeout_ms = float(raw_value)
    if timeout_ms <= 0.0:
        raise ValueError(
            "orchestrator.config.validation_collision_query_timeout_ms must be > 0"
        )
    return int(timeout_ms * 1_000_000.0)


def _validation_collision_wall_timeout_ns_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get(
        "validation_collision_wall_timeout_ms"
    )
    if raw_value is None:
        return None
    timeout_ms = float(raw_value)
    if timeout_ms <= 0.0:
        raise ValueError(
            "orchestrator.config.validation_collision_wall_timeout_ms must be > 0"
        )
    return int(timeout_ms * 1_000_000.0)


def _validation_collision_workers_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get("validation_collision_workers")
    if raw_value is None:
        return None
    if isinstance(raw_value, bool) or not isinstance(raw_value, int):
        raise ValueError(
            "orchestrator.config.validation_collision_workers must be an integer"
        )
    if raw_value < 1 or raw_value > 64:
        raise ValueError(
            "orchestrator.config.validation_collision_workers must be in [1, 64]"
        )
    return raw_value


def _runtime_collision_lookahead_from_deployment(
    deployment: Deployment | None,
) -> tuple[int | None, int | None, int | None, int | None, int | None]:
    if deployment is None or deployment.orchestrator is None:
        return None, None, None, None, None
    config = deployment.orchestrator.config
    for removed_key in (
        "runtime_collision_lookahead_period_ms",
        "runtime_collision_lookahead_max_waypoints",
        "runtime_collision_query_timeout_ms",
    ):
        if removed_key in config:
            raise ValueError(
                f"orchestrator.config.{removed_key} was removed by RT-8C.3"
            )
    # Deployment objects materialize these defaults. The fallback keeps
    # lightweight test/composition objects behaviorally identical to a parsed
    # production deployment instead of accidentally disabling lookahead.
    raw_cycle_budget_ms = config.get(
        "runtime_collision_lookahead_cycle_budget_ms", 200.0
    )
    raw_horizon_s = config.get("runtime_collision_lookahead_s", 2.0)
    raw_query_budget_ms = config.get(
        "runtime_collision_lookahead_query_budget_ms", 75.0
    )
    if isinstance(raw_cycle_budget_ms, bool) or not isinstance(
        raw_cycle_budget_ms, (int, float)
    ):
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_cycle_budget_ms "
            "must be a number"
        )
    if isinstance(raw_horizon_s, bool) or not isinstance(
        raw_horizon_s, (int, float)
    ):
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_s must be a number"
        )
    if isinstance(raw_query_budget_ms, bool) or not isinstance(
        raw_query_budget_ms, (int, float)
    ):
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_query_budget_ms "
            "must be a number"
        )
    cycle_budget_ms = float(raw_cycle_budget_ms)
    horizon_s = float(raw_horizon_s)
    query_budget_ms = float(raw_query_budget_ms)
    immediate_budget_ms = config.get(
        "runtime_collision_immediate_query_budget_ms", 50.0
    )
    monitor_period_ms = config.get("collision_monitor_period_ms", 50.0)
    for key, value in (
        ("runtime_collision_immediate_query_budget_ms", immediate_budget_ms),
        ("collision_monitor_period_ms", monitor_period_ms),
    ):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(f"orchestrator.config.{key} must be a number")
        if not math.isfinite(float(value)) or float(value) <= 0.0:
            raise ValueError(
                f"orchestrator.config.{key} must be finite and > 0"
            )
    if float(immediate_budget_ms) > float(monitor_period_ms):
        raise ValueError(
            "orchestrator.config.runtime_collision_immediate_query_budget_ms "
            "must be <= collision_monitor_period_ms"
        )
    max_samples = config.get("runtime_collision_lookahead_max_samples", 32)
    workers = config.get("runtime_collision_lookahead_workers", 2)
    if not math.isfinite(cycle_budget_ms) or cycle_budget_ms <= 0.0:
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_cycle_budget_ms "
            "must be finite and > 0"
        )
    if not math.isfinite(horizon_s) or horizon_s <= 0.0:
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_s "
            "must be finite and > 0"
        )
    if not math.isfinite(query_budget_ms) or query_budget_ms <= 0.0:
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_query_budget_ms "
            "must be finite and > 0"
        )
    if query_budget_ms > cycle_budget_ms:
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_query_budget_ms must be "
            "<= runtime_collision_lookahead_cycle_budget_ms"
        )
    if (
        isinstance(max_samples, bool)
        or not isinstance(max_samples, int)
        or not 2 <= max_samples <= 4096
    ):
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_max_samples "
            "must be in [2, 4096]"
        )
    if (
        isinstance(workers, bool)
        or not isinstance(workers, int)
        or not 1 <= workers <= 64
    ):
        raise ValueError(
            "orchestrator.config.runtime_collision_lookahead_workers "
            "must be in [1, 64]"
        )
    return (
        int(cycle_budget_ms * 1_000_000.0),
        int(horizon_s * 1_000_000_000.0),
        max_samples,
        workers,
        int(query_budget_ms * 1_000_000.0),
    )


def _runtime_dynamic_body_max_age_cap_ns_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get(
        "runtime_dynamic_body_max_age_cap_ms"
    )
    if raw_value is None:
        return None
    cap_ms = float(raw_value)
    # Zero is a sentinel: disable the kernel-side cap while keeping body-local
    # freshness checks active. Negative values are invalid.
    if cap_ms < 0.0:
        raise ValueError(
            "orchestrator.config.runtime_dynamic_body_max_age_cap_ms must be >= 0"
        )
    return int(cap_ms * 1_000_000.0)


def _kernel_config_from_deployment(
    deployment: Deployment | None,
    *,
    collision_monitor_period_ns: int | None,
    runtime_collision_immediate_query_budget_ns: int | None,
    validation_collision_query_timeout_ns: int | None,
    validation_collision_wall_timeout_ns: int | None,
    runtime_dynamic_body_max_age_cap_ns: int | None,
    validation_collision_workers: int | None = None,
    runtime_collision_lookahead_cycle_budget_ns: int | None = None,
    runtime_collision_lookahead_query_budget_ns: int | None = None,
    runtime_collision_lookahead_horizon_ns: int | None = None,
    runtime_collision_lookahead_max_samples: int | None = None,
    runtime_collision_lookahead_workers: int | None = None,
) -> dict[str, Any] | None:
    config: dict[str, Any] = {}
    if collision_monitor_period_ns is not None:
        config["collision_monitor_period_ns"] = collision_monitor_period_ns
    if runtime_collision_immediate_query_budget_ns is not None:
        config["runtime_collision_immediate_query_budget_ns"] = (
            runtime_collision_immediate_query_budget_ns
        )
    if validation_collision_query_timeout_ns is not None:
        config["validation_collision_query_timeout_ns"] = (
            validation_collision_query_timeout_ns
        )
    if validation_collision_wall_timeout_ns is not None:
        config["validation_collision_wall_timeout_ns"] = (
            validation_collision_wall_timeout_ns
        )
    if runtime_dynamic_body_max_age_cap_ns is not None:
        config["runtime_dynamic_body_max_age_cap_ns"] = (
            runtime_dynamic_body_max_age_cap_ns
        )
    if validation_collision_workers is not None:
        config["validation_collision_workers"] = validation_collision_workers
    if runtime_collision_lookahead_cycle_budget_ns is not None:
        config["runtime_collision_lookahead_cycle_budget_ns"] = (
            runtime_collision_lookahead_cycle_budget_ns
        )
    if runtime_collision_lookahead_query_budget_ns is not None:
        config["runtime_collision_lookahead_query_budget_ns"] = (
            runtime_collision_lookahead_query_budget_ns
        )
    if runtime_collision_lookahead_horizon_ns is not None:
        config["runtime_collision_lookahead_horizon_ns"] = (
            runtime_collision_lookahead_horizon_ns
        )
    if runtime_collision_lookahead_max_samples is not None:
        config["runtime_collision_lookahead_max_samples"] = (
            runtime_collision_lookahead_max_samples
        )
    if runtime_collision_lookahead_workers is not None:
        config["runtime_collision_lookahead_workers"] = (
            runtime_collision_lookahead_workers
        )

    execution = deployment.adapters.get("execution") if deployment is not None else None
    execution_config = dict(execution.config or {}) if execution is not None else {}
    planning = deployment.adapters.get("planning") if deployment is not None else None
    planning_config = dict(planning.config or {}) if planning is not None else {}
    for key in (
        "native_reset_completion_timeout_s",
        "native_reset_completion_timeout_ms",
        "single_shot_ik_timeout_s",
        "single_shot_ik_timeout_ms",
    ):
        if key in execution_config:
            config[key] = execution_config[key]
    for key in (
        "planner_max_planning_time_s",
        "planner_max_planning_time_ms",
        "rng_seed",
    ):
        if key in planning_config:
            config[key] = planning_config[key]
        elif key in execution_config:
            config[key] = execution_config[key]

    if deployment is not None:
        baseline_provider = getattr(
            deployment,
            "baseline_collision_policy_snapshot",
            None,
        )
        baseline_policy = baseline_provider() if callable(baseline_provider) else None
        if baseline_policy is not None:
            config["baseline_collision_policy_snapshot"] = baseline_policy

    # Pass the deployment-declared fault-stop and operator-stop profiles
    # through to the C++ kernel. Both are optional; deployments not
    # opting in see no behavior change (legacy backend.stop() path
    # is preserved when these are unset). Recommended deployment-
    # YAML shape under `execution.config`:
    #
    #   default_fault_stop_profile:
    #     profile_id: "default_fault_spring_back"
    #     stop_mode: "spring_back_hold"
    #     deceleration_time_s: 0.3
    #   operator_stop_profile:
    #     profile_id: "operator_soft_on_path"
    #     stop_mode: "soft_on_path"
    #     deceleration_time_s: 1.0
    #     per_joint_max_deceleration_rad_s2:
    #       shoulder_pan_joint: 8.0
    #       ...
    for key in ("default_fault_stop_profile", "operator_stop_profile"):
        if key in execution_config and execution_config[key] is not None:
            config[key] = execution_config[key]

    return config or None


def _execution_adapter_id_from_deployment(deployment: Deployment | None) -> str:
    if deployment is None:
        return "execution.roboplan_native"
    execution = deployment.adapters.get("execution")
    if execution is None:
        return "execution.roboplan_native"
    return execution.adapter_id


def _native_model_and_group_from_deployment(
    deployment: Deployment | None,
) -> tuple[str, str]:
    robot_config = getattr(deployment, "robot_config", None)
    if deployment is None or robot_config is None:
        return "ur5_gripper", "arm"
    model_name = Path(robot_config.urdf_path).stem or "ur5_gripper"
    group_name = robot_config.group_id or "arm"
    return model_name, group_name


def _make_ros2_control_kernel(
    deployment: Deployment,
    *,
    collision_monitor_period_ns: int | None,
    runtime_collision_immediate_query_budget_ns: int | None,
    validation_collision_query_timeout_ns: int | None,
    validation_collision_wall_timeout_ns: int | None,
    runtime_dynamic_body_max_age_cap_ns: int | None,
    validation_collision_workers: int | None,
    runtime_collision_lookahead_cycle_budget_ns: int | None,
    runtime_collision_lookahead_query_budget_ns: int | None,
    runtime_collision_lookahead_horizon_ns: int | None,
    runtime_collision_lookahead_max_samples: int | None,
    runtime_collision_lookahead_workers: int | None,
):
    """Create the ROS 2 controller-backed kernel.

    The implementation intentionally lives outside Composer.  Composer owns
    deployment selection and UI/server wiring; the ROS runtime package owns the
    ROS-specific backend that satisfies the core ``IExecutionBackend`` port.
    """
    # Local import to avoid a top-level cycle and keep this module self-contained
    # when the ROS adapter isn't installed in this image at all.
    from .backend.runtime_requirements import (
        RuntimeFactoryRequirementsError,
        detect_current_lane,
    )

    adapter_id = _execution_adapter_id_from_deployment(deployment)
    try:
        from manyforge_ros.ros2_control_kernel_factory import (
            create_ros2_control_kernel,
        )
    except Exception as exc:
        # Adapter not present in this image — fundamental lane mismatch. Surface
        # a structured rejection so the HTTP layer can show the operator what
        # to launch instead of a generic message.
        raise RuntimeFactoryRequirementsError(
            reason_code="ros_adapter_missing",
            message=(
                f"Deployment selected ROS 2 controller execution ({adapter_id}), "
                "but the ROS runtime kernel factory is not available in this "
                "Composer image. Start Composer in the ROS-capable lane "
                "(isaac-sim) before loading this deployment."
            ),
            missing=("ros_adapter",),
            expected_lane="isaac-sim",
            current_lane=detect_current_lane(),
            suggested_command=(
                "scripts/launch.sh --lane isaac-sim "
                "--scenario isaac-ur10e-light-scene restart"
            ),
            original_error=str(exc),
        ) from exc

    try:
        return create_ros2_control_kernel(
            deployment=deployment,
            collision_monitor_period_ns=collision_monitor_period_ns,
            runtime_collision_immediate_query_budget_ns=runtime_collision_immediate_query_budget_ns,
            validation_collision_query_timeout_ns=(
                validation_collision_query_timeout_ns
            ),
            validation_collision_wall_timeout_ns=(
                validation_collision_wall_timeout_ns
            ),
            runtime_dynamic_body_max_age_cap_ns=runtime_dynamic_body_max_age_cap_ns,
            validation_collision_workers=validation_collision_workers,
            runtime_collision_lookahead_cycle_budget_ns=(
                runtime_collision_lookahead_cycle_budget_ns
            ),
            runtime_collision_lookahead_query_budget_ns=(
                runtime_collision_lookahead_query_budget_ns
            ),
            runtime_collision_lookahead_horizon_ns=(
                runtime_collision_lookahead_horizon_ns
            ),
            runtime_collision_lookahead_max_samples=(
                runtime_collision_lookahead_max_samples
            ),
            runtime_collision_lookahead_workers=(
                runtime_collision_lookahead_workers
            ),
        )
    except Exception as exc:
        # Adapter installed but the runtime build failed — check ROS endpoint
        # availability, controller config, and the adapter build itself. The
        # cause is included verbatim so the operator can see what JTC / topic /
        # action lookup actually threw.
        raise RuntimeFactoryRequirementsError(
            reason_code="ros_factory_runtime_failed",
            message=(
                "ROS 2 controller execution runtime factory failed while building "
                "the Composer kernel. Check the ROS adapter build, deployment "
                "controller config, and ROS endpoint availability."
            ),
            missing=(),
            expected_lane="isaac-sim",
            current_lane=detect_current_lane(),
            suggested_command=(
                "scripts/launch.sh --lane isaac-sim "
                "--scenario isaac-ur10e-light-scene restart"
            ),
            original_error=str(exc),
        ) from exc


def _make_kernel(
    deployment: Deployment | None = None,
    *,
    kernel_mode: str = "auto",
    mock_execute_duration_s: float = 1.5,
):
    """Try NativeKernelClient backed by real RoboPlan C++; fall back to Mock.

    Returns:
        (kernel, bundle) — bundle is None when using MockKernelClient.
    """
    if kernel_mode == "mock":
        logger.info(
            "Using MockKernelClient (forced), execute_duration_s=%.3f",
            mock_execute_duration_s,
        )
        # This profile exists solely for deterministic control-flow
        # qualification. It does not claim physical stop-dynamics evidence and
        # is never selected by auto/production kernel discovery.
        mock_stop_capability = {
            "supported_stop_modes": [
                "spring_back_hold",
                "soft_on_path",
                "hard_deceleration",
            ],
            "capability_profile_id": "manyforge_mock_control_flow_v1",
            "hard_stop_level": "mock_immediate_terminal",
            "derivative_proof_level": "test_double_no_physical_dynamics",
            "scaling_source_visibility": "not_applicable",
            "scaling_source_probe_status": "not_applicable",
            "unsupported_reasons": {},
        }
        return (
            MockKernelClient(
                execute_duration_s=mock_execute_duration_s,
                motion_interrupt_stop_capability=mock_stop_capability,
            ),
            None,
        )
    execution_adapter_id = _execution_adapter_id_from_deployment(deployment)
    collision_monitor_period_ns = _collision_monitor_period_ns_from_deployment(
        deployment
    )
    runtime_collision_immediate_query_budget_ns = (
        _runtime_collision_immediate_query_budget_ns_from_deployment(deployment)
    )
    validation_collision_query_timeout_ns = (
        _validation_collision_query_timeout_ns_from_deployment(deployment)
    )
    validation_collision_wall_timeout_ns = (
        _validation_collision_wall_timeout_ns_from_deployment(deployment)
    )
    validation_collision_workers = _validation_collision_workers_from_deployment(
        deployment
    )
    (
        runtime_collision_lookahead_cycle_budget_ns,
        runtime_collision_lookahead_horizon_ns,
        runtime_collision_lookahead_max_samples,
        runtime_collision_lookahead_workers,
        runtime_collision_lookahead_query_budget_ns,
    ) = _runtime_collision_lookahead_from_deployment(deployment)
    runtime_dynamic_body_max_age_cap_ns = (
        _runtime_dynamic_body_max_age_cap_ns_from_deployment(deployment)
    )
    if execution_adapter_id in _ROS2_CONTROL_EXECUTION_ADAPTER_IDS:
        if deployment is None:
            raise RuntimeError(
                "ROS 2 controller execution requires a loaded deployment"
            )
        return _make_ros2_control_kernel(
            deployment,
            collision_monitor_period_ns=collision_monitor_period_ns,
            runtime_collision_immediate_query_budget_ns=runtime_collision_immediate_query_budget_ns,
            validation_collision_query_timeout_ns=(
                validation_collision_query_timeout_ns
            ),
            validation_collision_wall_timeout_ns=(
                validation_collision_wall_timeout_ns
            ),
            runtime_dynamic_body_max_age_cap_ns=runtime_dynamic_body_max_age_cap_ns,
            validation_collision_workers=validation_collision_workers,
            runtime_collision_lookahead_cycle_budget_ns=(
                runtime_collision_lookahead_cycle_budget_ns
            ),
            runtime_collision_lookahead_query_budget_ns=(
                runtime_collision_lookahead_query_budget_ns
            ),
            runtime_collision_lookahead_horizon_ns=(
                runtime_collision_lookahead_horizon_ns
            ),
            runtime_collision_lookahead_max_samples=(
                runtime_collision_lookahead_max_samples
            ),
            runtime_collision_lookahead_workers=(
                runtime_collision_lookahead_workers
            ),
        )
    if execution_adapter_id not in {"execution.roboplan_native", "execution.test"}:
        raise RuntimeError(
            f"Unsupported execution adapter '{execution_adapter_id}'. "
            "Supported Composer runtime adapters are execution.roboplan_native "
            "and execution.ros2_control_follow_joint_trajectory."
        )
    model_name, group_name = _native_model_and_group_from_deployment(deployment)
    robot_config = getattr(deployment, "robot_config", None)
    logical_robot_id = str(
        getattr(robot_config, "robot_id", None) or model_name
    )
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient

        kernel_config = _kernel_config_from_deployment(
            deployment,
            collision_monitor_period_ns=collision_monitor_period_ns,
            runtime_collision_immediate_query_budget_ns=runtime_collision_immediate_query_budget_ns,
            validation_collision_query_timeout_ns=(
                validation_collision_query_timeout_ns
            ),
            validation_collision_wall_timeout_ns=(
                validation_collision_wall_timeout_ns
            ),
            runtime_dynamic_body_max_age_cap_ns=runtime_dynamic_body_max_age_cap_ns,
            validation_collision_workers=validation_collision_workers,
            runtime_collision_lookahead_cycle_budget_ns=(
                runtime_collision_lookahead_cycle_budget_ns
            ),
            runtime_collision_lookahead_query_budget_ns=(
                runtime_collision_lookahead_query_budget_ns
            ),
            runtime_collision_lookahead_horizon_ns=(
                runtime_collision_lookahead_horizon_ns
            ),
            runtime_collision_lookahead_max_samples=(
                runtime_collision_lookahead_max_samples
            ),
            runtime_collision_lookahead_workers=(
                runtime_collision_lookahead_workers
            ),
        )
        resolved_model = _resolved_robot_model_from_deployment(deployment)
        if resolved_model is not None:
            runtime_model = prepare_roboplan_runtime_model_view(
                resolved_model=resolved_model,
                grippers=getattr(deployment, "grippers", {}),
            )
            bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
                runtime_model.model_name,
                group_name,
                runtime_model.urdf_path,
                runtime_model.srdf_path,
                runtime_model.package_paths,
                kernel_config,
                logical_robot_id,
            )
        else:
            runtime_model = None
            bundle = manyforge_roboplan_py.create_native_kernel(
                model_name,
                group_name,
                kernel_config,
                logical_robot_id,
            )
        if runtime_model is not None:
            kernel = NativeKernelClient(
                bundle,
                robot_model_authority_metadata=runtime_model.metadata,
                robot_model_authority_temp_dir=runtime_model._temp_dir,
            )
            attach_robot_model_metadata(kernel, runtime_model)
            attach_robot_model_metadata(bundle, runtime_model)
            logger.info(
                "Robot model authority view prepared: canonical=%s view=%s changed=%s",
                runtime_model.authority.canonical_fingerprint,
                runtime_model.import_view.view_fingerprint,
                runtime_model.import_view.changed,
            )
        else:
            kernel = NativeKernelClient(bundle)
        if collision_monitor_period_ns is not None:
            logger.info(
                "Using NativeKernelClient (real RoboPlan planning), collision monitor period=%d ns",
                collision_monitor_period_ns,
            )
        else:
            logger.info("Using NativeKernelClient (real RoboPlan planning)")
        return kernel, bundle
    except Exception as e:
        if deployment is None or execution_adapter_id == "execution.test":
            logger.info("NativeKernelClient unavailable (%s), using MockKernelClient", e)
            return MockKernelClient(execute_duration_s=mock_execute_duration_s), None
        from .backend.runtime_requirements import (
            RuntimeFactoryRequirementsError,
            detect_current_lane,
        )

        raise RuntimeFactoryRequirementsError(
            reason_code="native_factory_runtime_failed",
            message=(
                "Native RoboPlan execution runtime factory failed while building "
                "the Composer kernel. Check the native RoboPlan build, robot "
                "model paths, and deployment configuration."
            ),
            missing=("native_roboplan_runtime",),
            expected_lane="manyforge-only",
            current_lane=detect_current_lane(),
            suggested_command="scripts/launch.sh --lane manyforge-only restart",
            original_error=str(e),
        ) from e


def _run_mcp(args):
    """Run ManyForge as an MCP server over stdio."""
    from manyforge_composer.backend.mcp_server import create_mcp_server

    security = SecurityConfig.from_env()
    kernel, bundle = _make_kernel()
    environment_factory = _node_environment_factory(args.catalog_path)
    state = ComposerState(
        node_environment=environment_factory(1),
        node_environment_factory=environment_factory,
        kernel=kernel,
        kernel_factory=_make_kernel,
        kernel_bundle=bundle,
        trace_dir=args.trace_dir,
        failure_archive_dir=args.failure_archive_dir,
        failure_archive_enabled=not args.disable_failure_archive,
        failure_archive_max_age_s=args.failure_archive_max_age_s,
        failure_archive_max_bytes=args.failure_archive_max_bytes,
        trace_max_age_s=args.trace_max_age_s,
        trace_max_bytes=args.trace_max_bytes,
        allowed_fs_roots=security.allowed_fs_roots,
        scenario_roots=_scenario_roots(),
    )
    # ComposerState is the sole runtime owner. Keeping the initial bundle in
    # this long-lived stack frame would retain its scheduler/ROS resources
    # after a deployment-specific kernel replaces it.
    bundle = None
    _configure_assistant_provider(
        state,
        args.assistant_provider,
        endpoint_url=args.assistant_endpoint,
        timeout_s=args.assistant_timeout_s,
        api_token=args.assistant_api_token,
    )
    state.public_api_base_url = _default_public_api_base_url(args.host, args.port)
    try:
        mcp_server = create_mcp_server(state)
    except RuntimeError as exc:
        logger.error("%s", exc)
        raise SystemExit(2) from exc
    logger.info("Starting ManyForge MCP server (stdio transport)")
    mcp_server.run(transport="stdio")


def main():
    parser = argparse.ArgumentParser(description="ManyForge Composer Backend")
    parser.add_argument("--catalog-path", required=True, help="Path to node_catalog.yaml")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument(
        "--static-dir",
        default=None,
        help="Explicit built frontend dist directory to serve. When set, no fallback dist is used.",
    )
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    parser.add_argument(
        "--kernel-mode",
        default="auto",
        choices=["auto", "mock"],
        help="Kernel selection mode for REST/qualification runs",
    )
    parser.add_argument(
        "--mock-execute-duration-s",
        type=float,
        default=1.5,
        help="Per-move execution duration for mock-kernel runs and native fallback",
    )
    parser.add_argument(
        "--assistant-provider",
        default=None,
        choices=[
            "mock",
            "none",
            "deterministic",
            "deterministic-scene-authoring",
            "direct",
            "nemoclaw",  # legacy alias for "direct"
            "openclaw",
            "hermes",  # registered but inert until Phase 4
        ],
        help=(
            "Optional assistant provider. Canonical lane ids are 'direct', "
            "'openclaw', and 'hermes' (per THREE-LANE-MIGRATION-PLAN). "
            "'nemoclaw' is accepted as a legacy alias for 'direct'. Defaults "
            "to MANYFORGE_ASSISTANT_PROVIDER or mock-only mode."
        ),
    )
    parser.add_argument(
        "--assistant-endpoint",
        default=None,
        help=(
            "HTTP endpoint for the NemoClaw/OpenClaw assistant provider. "
            "Defaults to MANYFORGE_ASSISTANT_ENDPOINT_URL."
        ),
    )
    parser.add_argument(
        "--assistant-timeout-s",
        type=float,
        default=None,
        help=(
            "Timeout for live assistant provider calls. Defaults to "
            "MANYFORGE_ASSISTANT_TIMEOUT_S or 30s."
        ),
    )
    parser.add_argument(
        "--assistant-api-token",
        default=None,
        help=(
            "Bearer token for the live assistant provider. Defaults to "
            "MANYFORGE_ASSISTANT_API_TOKEN."
        ),
    )
    parser.add_argument("--mcp", action="store_true",
                        help="Run as MCP server over stdio (no HTTP)")
    parser.add_argument("--mcp-http", action="store_true",
                        help="Enable HTTP/SSE MCP transport on /api/mcp (alongside REST)")
    parser.add_argument(
        "--trace-dir",
        default=os.environ.get("MANYFORGE_TRACE_DIR", "/tmp/manyforge-traces"),
        help=(
            "Directory for durable execution traces (per-run JSONL, incident "
            "snapshots, failure archives). Defaults to MANYFORGE_TRACE_DIR or "
            "/tmp/manyforge-traces. The /tmp fallback does NOT survive a "
            "reboot — deployments must point this at persistent storage "
            "(the compose/launcher lanes set /workspace/.manyforge-var/traces "
            "under the repo bind mount)."
        ),
    )
    parser.add_argument(
        "--failure-archive-dir",
        default=os.environ.get("MANYFORGE_FAILURE_ARCHIVE_DIR"),
        help=(
            "Directory for file-backed execution failure archives. Defaults to "
            "<trace-dir>/failure-archives."
        ),
    )
    parser.add_argument(
        "--failure-archive-max-age-s",
        type=float,
        default=_optional_float_env("MANYFORGE_FAILURE_ARCHIVE_MAX_AGE_S"),
        help=(
            "Archive retention age in seconds. Defaults to "
            "MANYFORGE_FAILURE_ARCHIVE_MAX_AGE_S or 2592000 (30 days). "
            "Older archives are deleted FIFO by directory mtime."
        ),
    )
    parser.add_argument(
        "--failure-archive-max-bytes",
        type=int,
        default=_optional_int_env("MANYFORGE_FAILURE_ARCHIVE_MAX_BYTES"),
        help=(
            "Archive disk budget in bytes. Defaults to "
            "MANYFORGE_FAILURE_ARCHIVE_MAX_BYTES or 1073741824 (1 GiB). "
            "Oldest archives are deleted until the budget is satisfied."
        ),
    )
    parser.add_argument(
        "--trace-max-age-s",
        type=float,
        default=_optional_float_env("MANYFORGE_TRACE_MAX_AGE_S"),
        help=(
            "Per-run trace JSONL retention age in seconds. Defaults to "
            "MANYFORGE_TRACE_MAX_AGE_S or 2592000 (30 days)."
        ),
    )
    parser.add_argument(
        "--trace-max-bytes",
        type=int,
        default=_optional_int_env("MANYFORGE_TRACE_MAX_BYTES"),
        help=(
            "Trace directory disk budget in bytes. Defaults to "
            "MANYFORGE_TRACE_MAX_BYTES or 2147483648 (2 GiB). Oldest "
            "INFO-only runs are pruned first; ERROR/CRITICAL runs are "
            "preferred-kept."
        ),
    )
    parser.add_argument(
        "--disable-failure-archive",
        action="store_true",
        default=_bool_env("MANYFORGE_DISABLE_FAILURE_ARCHIVE", False),
        help="Disable file-backed failure archives.",
    )
    args = parser.parse_args()

    setup_logging(args.log_level)

    if str(args.trace_dir).startswith("/tmp"):
        logger.warning(
            "Execution traces at %s are on VOLATILE storage and will not "
            "survive a reboot — the incident that forces a power-cycle "
            "erases its own forensics. Set --trace-dir/MANYFORGE_TRACE_DIR "
            "to a persistent path for deployments.",
            args.trace_dir,
        )

    if args.mcp and args.mcp_http:
        logger.error("--mcp (stdio) and --mcp-http cannot be used together")
        raise SystemExit(2)

    if args.mcp:
        _run_mcp(args)
        return

    security = SecurityConfig.from_env()
    kernel, bundle = _make_kernel(
        kernel_mode=args.kernel_mode,
        mock_execute_duration_s=args.mock_execute_duration_s,
    )
    environment_factory = _node_environment_factory(args.catalog_path)

    state = ComposerState(
        node_environment=environment_factory(1),
        node_environment_factory=environment_factory,
        kernel=kernel,
        kernel_factory=lambda deployment=None: _make_kernel(
            deployment,
            kernel_mode=args.kernel_mode,
            mock_execute_duration_s=args.mock_execute_duration_s,
        ),
        kernel_bundle=bundle,
        trace_dir=args.trace_dir,
        failure_archive_dir=args.failure_archive_dir,
        failure_archive_enabled=not args.disable_failure_archive,
        failure_archive_max_age_s=args.failure_archive_max_age_s,
        failure_archive_max_bytes=args.failure_archive_max_bytes,
        trace_max_age_s=args.trace_max_age_s,
        trace_max_bytes=args.trace_max_bytes,
        allowed_fs_roots=security.allowed_fs_roots,
        scenario_roots=_scenario_roots(),
    )
    _configure_assistant_provider(
        state,
        args.assistant_provider,
        endpoint_url=args.assistant_endpoint,
        timeout_s=args.assistant_timeout_s,
        api_token=args.assistant_api_token,
    )
    state.public_api_base_url = _default_public_api_base_url(args.host, args.port)

    state._on_program_loaded = build_program_loaded_callback()
    # The state owns the active bundle. Do not retain the unconfigured startup
    # bundle in main() after a deployment load replaces and closes it.
    bundle = None

    if not security.api_token and args.host != "127.0.0.1":
        logger.warning(
            "No MANYFORGE_API_TOKEN set — API is unauthenticated on %s:%d",
            args.host, args.port,
        )
    app = create_app(
        state,
        static_dir=args.static_dir,
        security=security,
        enable_mcp_http=args.mcp_http,
    )
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
