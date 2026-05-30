"""python -m manyforge_composer --catalog-path <path> [--host 0.0.0.0] [--port 9000] [--hmi-port 8081]"""
from __future__ import annotations

import argparse
from dataclasses import replace
import logging
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
    normalize_package_paths,
    resolve_resource_path,
    resolve_robot_model,
)
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
    if deployment is None or deployment.robot_config is None:
        return None
    if not deployment.robot_config.srdf_path:
        return None
    extra_package_paths = (
        []
        if deployment.robot_config.package_paths
        else default_package_paths()
    )
    return resolve_robot_model(
        urdf_path=deployment.robot_config.urdf_path,
        srdf_path=deployment.robot_config.srdf_path,
        package_paths=deployment.robot_config.package_paths,
        deployment_path=deployment.source_path,
        extra_package_paths=extra_package_paths,
    )


def _resolved_hmi_robot_paths(
    deployment: Deployment | None,
    *,
    default_urdf_path: str | None,
    default_package_dirs: list[str],
) -> tuple[str | None, list[str]]:
    try:
        resolved_model = _resolved_robot_model_from_deployment(deployment)
    except FileNotFoundError as exc:
        logger.warning(
            "Could not resolve deployment robot model for HMI; falling back to "
            "launcher defaults: %s",
            exc,
        )
        resolved_model = None
    if resolved_model is not None:
        return resolved_model.urdf_path, resolved_model.package_paths
    return default_urdf_path, default_package_dirs


def build_program_loaded_callback(
    *,
    host: str,
    hmi_port: int,
    urdf_path: str | None,
    package_dirs: list[str],
    bundle,
    hmi_server_factory=None,
):
    """Build the Composer program-load callback that wires Viser HMI state.

    Extracted for testability so backend tests can validate the callback
    without spawning uvicorn or a real Viser server.
    """
    if hmi_server_factory is None:
        from manyforge_hmi.server import ViserHmiServer
        hmi_server_factory = ViserHmiServer

    _hmi_server = None

    def _snapshot_provider_for_runtime(hmi_server):
        return (
            getattr(hmi_server, "get_cached_scene_snapshot", None)
            or getattr(hmi_server, "render_scene", None)
        )

    def on_program_loaded(st: ComposerState):
        nonlocal _hmi_server

        runtime_urdf_path, runtime_package_dirs = _resolved_hmi_robot_paths(
            st.deployment,
            default_urdf_path=urdf_path,
            default_package_dirs=package_dirs,
        )

        # Stop previous HMI if reloading a program
        if _hmi_server is not None:
            _hmi_server.stop()
            st.hmi_server = None

        _hmi_server = hmi_server_factory(
            cycle_manager=st.cycle_manager,
            host=host,
            port=hmi_port,
            urdf_path=runtime_urdf_path,
            joint_names=None,
            package_dirs=runtime_package_dirs or None,
        )
        st.hmi_server = _hmi_server
        st.hmi_public_port = hmi_port
        if (
            st.cycle_manager is not None
            and hasattr(st.cycle_manager, "_runner")
            and _snapshot_provider_for_runtime(_hmi_server) is not None
        ):
            st.cycle_manager._runner.set_scene_snapshot_provider(
                _snapshot_provider_for_runtime(_hmi_server)
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
        _hmi_server.load_program(st.effective_program or st.program)

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
            _hmi_server.request_scene_sync(
                reason="scene_edit_callback",
                operation=operation,
                object_id=request.object_id,
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

        def _sync_runtime_views(force_robot: bool = False):
            if _hmi_server and hasattr(_hmi_server, "sync_scene_from_backend"):
                _hmi_server.sync_scene_from_backend()
            if (
                _hmi_server
                and hasattr(_hmi_server, "sync_robot_from_backend")
            ):
                _hmi_server.sync_robot_from_backend(force=force_robot)

        # Reset Viser scene on each run from IDLE (e.g. after reset)
        def _refresh_viser_scene():
            if _hmi_server and _hmi_server.scene_renderer:
                _hmi_server.scene_renderer.clear()
                _sync_runtime_views(force_robot=True)

        st.cycle_manager._on_run_from_idle = _refresh_viser_scene
        st.cycle_manager.reset()
        initial_positions = _get_simulator_initial_joint_positions(st)
        runtime_bundle = getattr(st, "kernel_bundle", None) or bundle
        if runtime_bundle is not None and initial_positions:
            runtime_bundle.set_joint_positions(initial_positions)
            logger.info("Applied simulator-only initial joint positions from deployment")
        _sync_runtime_views(force_robot=True)

        _hmi_server.start()
        _sync_runtime_views(force_robot=True)
        logger.info("Viser HMI started on port %d", hmi_port)

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


def _runtime_collision_query_timeout_ns_from_deployment(
    deployment: Deployment | None,
) -> int | None:
    if deployment is None or deployment.orchestrator is None:
        return None
    raw_value = deployment.orchestrator.config.get(
        "runtime_collision_query_timeout_ms"
    )
    if raw_value is None:
        return None
    timeout_ms = float(raw_value)
    if timeout_ms <= 0.0:
        raise ValueError(
            "orchestrator.config.runtime_collision_query_timeout_ms must be > 0"
        )
    return int(timeout_ms * 1_000_000.0)


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
    runtime_collision_query_timeout_ns: int | None,
    runtime_dynamic_body_max_age_cap_ns: int | None,
) -> dict[str, Any] | None:
    config: dict[str, Any] = {}
    if collision_monitor_period_ns is not None:
        config["collision_monitor_period_ns"] = collision_monitor_period_ns
    if runtime_collision_query_timeout_ns is not None:
        config["runtime_collision_query_timeout_ns"] = (
            runtime_collision_query_timeout_ns
        )
    if runtime_dynamic_body_max_age_cap_ns is not None:
        config["runtime_dynamic_body_max_age_cap_ns"] = (
            runtime_dynamic_body_max_age_cap_ns
        )

    execution = deployment.adapters.get("execution") if deployment is not None else None
    execution_config = dict(execution.config or {}) if execution is not None else {}
    for key in (
        "native_reset_completion_timeout_s",
        "native_reset_completion_timeout_ms",
        "single_shot_ik_timeout_s",
        "single_shot_ik_timeout_ms",
    ):
        if key in execution_config:
            config[key] = execution_config[key]

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
    #     deceleration_time_s: 0.5
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
    if deployment is None or deployment.robot_config is None:
        return "ur5_gripper", "arm"
    model_name = Path(deployment.robot_config.urdf_path).stem or "ur5_gripper"
    group_name = deployment.robot_config.group_id or "arm"
    return model_name, group_name


def _make_ros2_control_kernel(
    deployment: Deployment,
    *,
    collision_monitor_period_ns: int | None,
    runtime_collision_query_timeout_ns: int | None,
    runtime_dynamic_body_max_age_cap_ns: int | None,
):
    """Create the ROS 2 controller-backed kernel.

    The implementation intentionally lives outside Composer.  Composer owns
    deployment selection and UI/server wiring; the ROS runtime package owns the
    ROS-specific backend that satisfies the core ``IExecutionBackend`` port.
    """
    try:
        from manyforge_ros.ros2_control_kernel_factory import (
            create_ros2_control_kernel,
        )
    except Exception as exc:
        raise RuntimeError(
            "Deployment selected ROS 2 controller execution "
            f"({_execution_adapter_id_from_deployment(deployment)}), but the "
            "ROS runtime kernel factory is not available. Start Composer in the "
            "ROS-capable image and build/install the manyforge_ros runtime "
            "adapter before loading this deployment."
        ) from exc

    try:
        return create_ros2_control_kernel(
            deployment=deployment,
            collision_monitor_period_ns=collision_monitor_period_ns,
            runtime_collision_query_timeout_ns=runtime_collision_query_timeout_ns,
            runtime_dynamic_body_max_age_cap_ns=runtime_dynamic_body_max_age_cap_ns,
        )
    except Exception as exc:
        raise RuntimeError(
            "ROS 2 controller execution runtime factory failed while building "
            "the Composer kernel. Check the ROS adapter build, deployment "
            "controller config, and ROS endpoint availability."
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
        return MockKernelClient(execute_duration_s=mock_execute_duration_s), None
    execution_adapter_id = _execution_adapter_id_from_deployment(deployment)
    collision_monitor_period_ns = _collision_monitor_period_ns_from_deployment(
        deployment
    )
    runtime_collision_query_timeout_ns = (
        _runtime_collision_query_timeout_ns_from_deployment(deployment)
    )
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
            runtime_collision_query_timeout_ns=runtime_collision_query_timeout_ns,
            runtime_dynamic_body_max_age_cap_ns=runtime_dynamic_body_max_age_cap_ns,
        )
    if execution_adapter_id not in {"execution.roboplan_native", "execution.test"}:
        raise RuntimeError(
            f"Unsupported execution adapter '{execution_adapter_id}'. "
            "Supported Composer runtime adapters are execution.roboplan_native "
            "and execution.ros2_control_follow_joint_trajectory."
        )
    model_name, group_name = _native_model_and_group_from_deployment(deployment)
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient

        kernel_config = _kernel_config_from_deployment(
            deployment,
            collision_monitor_period_ns=collision_monitor_period_ns,
            runtime_collision_query_timeout_ns=runtime_collision_query_timeout_ns,
            runtime_dynamic_body_max_age_cap_ns=runtime_dynamic_body_max_age_cap_ns,
        )
        resolved_model = _resolved_robot_model_from_deployment(deployment)
        if resolved_model is not None:
            bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
                resolved_model.model_name,
                group_name,
                resolved_model.urdf_path,
                resolved_model.srdf_path,
                resolved_model.package_paths,
                kernel_config,
            )
        else:
            bundle = manyforge_roboplan_py.create_native_kernel(
                model_name, group_name, kernel_config
            )
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
        logger.info("NativeKernelClient unavailable (%s), using MockKernelClient", e)
        return MockKernelClient(execute_duration_s=mock_execute_duration_s), None


def _resolve_roboplan_paths():
    """Get URDF path and package share dir from installed roboplan.

    Returns:
        (urdf_path, package_share_dir) or (None, None) if not available.
    """
    try:
        from roboplan import example_models

        models_dir = example_models.get_package_models_dir()
        share_dir = example_models.get_package_share_dir()
        urdf_path = str(models_dir / "ur_robot_model" / "ur5_gripper.urdf")
        return urdf_path, str(share_dir)
    except Exception:
        return None, None


def _run_mcp(args):
    """Run ManyForge as an MCP server over stdio."""
    from manyforge_composer.backend.mcp_server import create_mcp_server

    security = SecurityConfig.from_env()
    kernel, bundle = _make_kernel()
    state = ComposerState(
        catalog_path=args.catalog_path,
        kernel=kernel,
        kernel_factory=_make_kernel,
        kernel_bundle=bundle,
        trace_dir="/tmp/manyforge-traces",
        failure_archive_dir=args.failure_archive_dir,
        failure_archive_enabled=not args.disable_failure_archive,
        failure_archive_max_age_s=args.failure_archive_max_age_s,
        failure_archive_max_bytes=args.failure_archive_max_bytes,
        allowed_fs_roots=security.allowed_fs_roots,
    )
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
    parser.add_argument("--hmi-port", type=int, default=None,
                        help="Enable Viser 3D HMI on this port (e.g. 8081)")
    parser.add_argument("--urdf", default=None,
                        help="Path to robot URDF (auto-resolved from roboplan if omitted)")
    parser.add_argument("--package-dir", action="append", default=[],
                        help="Directory for package:// URI resolution (repeatable)")
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
            "nemoclaw",
            "openclaw",
        ],
        help=(
            "Optional assistant provider. Defaults to MANYFORGE_ASSISTANT_PROVIDER "
            "or mock-only mode."
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
        "--disable-failure-archive",
        action="store_true",
        default=_bool_env("MANYFORGE_DISABLE_FAILURE_ARCHIVE", False),
        help="Disable file-backed failure archives.",
    )
    args = parser.parse_args()

    setup_logging(args.log_level)

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

    # Auto-resolve URDF and package dirs from roboplan if not specified via CLI
    urdf_path = args.urdf
    package_dirs = normalize_package_paths(
        args.package_dir or [],
        base_dir=Path.cwd(),
    )
    if not urdf_path or not package_dirs:
        roboplan_urdf, roboplan_share = _resolve_roboplan_paths()
        if not urdf_path and roboplan_urdf:
            urdf_path = roboplan_urdf
            logger.info("URDF auto-resolved from roboplan: %s", urdf_path)
        if not package_dirs and roboplan_share:
            package_dirs = [roboplan_share]
            logger.info("Package dir auto-resolved from roboplan: %s", roboplan_share)
    if urdf_path and package_dirs:
        try:
            urdf_path = resolve_resource_path(
                urdf_path,
                base_dir=Path.cwd(),
                package_paths=package_dirs,
            )
        except FileNotFoundError:
            pass

    state = ComposerState(
        catalog_path=args.catalog_path,
        kernel=kernel,
        kernel_factory=lambda deployment=None: _make_kernel(
            deployment,
            kernel_mode=args.kernel_mode,
            mock_execute_duration_s=args.mock_execute_duration_s,
        ),
        kernel_bundle=bundle,
        trace_dir="/tmp/manyforge-traces",
        failure_archive_dir=args.failure_archive_dir,
        failure_archive_enabled=not args.disable_failure_archive,
        failure_archive_max_age_s=args.failure_archive_max_age_s,
        failure_archive_max_bytes=args.failure_archive_max_bytes,
        allowed_fs_roots=security.allowed_fs_roots,
    )
    _configure_assistant_provider(
        state,
        args.assistant_provider,
        endpoint_url=args.assistant_endpoint,
        timeout_s=args.assistant_timeout_s,
        api_token=args.assistant_api_token,
    )
    state.public_api_base_url = _default_public_api_base_url(args.host, args.port)

    # Wire up Viser HMI if --hmi-port is set
    if args.hmi_port is not None:
        state._on_program_loaded = build_program_loaded_callback(
            host=args.host,
            hmi_port=args.hmi_port,
            urdf_path=urdf_path,
            package_dirs=package_dirs,
            bundle=bundle,
        )

    if not security.api_token and args.host != "127.0.0.1":
        logger.warning(
            "No MANYFORGE_API_TOKEN set — API is unauthenticated on %s:%d",
            args.host, args.port,
        )
    app = create_app(state, security=security, enable_mcp_http=args.mcp_http)
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
