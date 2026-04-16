"""python -m manyforge_composer --catalog-path <path> [--host 0.0.0.0] [--port 9000] [--hmi-port 8081]"""
from __future__ import annotations

import argparse
import logging
import os
from pathlib import Path

import py_trees
import uvicorn

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.logging_config import setup_logging
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig

logger = logging.getLogger(__name__)

# UR5 joint names
JOINT_NAMES = [
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint", "wrist_2_joint", "wrist_3_joint",
]


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

        # Stop previous HMI if reloading a program
        if _hmi_server is not None:
            _hmi_server.stop()
            st.hmi_server = None

        _hmi_server = hmi_server_factory(
            cycle_manager=st.cycle_manager,
            host=host,
            port=hmi_port,
            urdf_path=urdf_path,
            joint_names=JOINT_NAMES,
            package_dirs=package_dirs or None,
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
            _hmi_server.request_scene_sync(
                reason="scene_edit_callback",
                operation=operation,
                object_id=request.object_id,
            )
            logger.debug("Scene invalidation requested after %s '%s'", operation, request.object_id)

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


def _native_model_and_group_from_deployment(
    deployment: Deployment | None,
) -> tuple[str, str]:
    if deployment is None or deployment.robot_config is None:
        return "ur5_gripper", "arm"
    model_name = Path(deployment.robot_config.urdf_path).stem or "ur5_gripper"
    group_name = deployment.robot_config.group_id or "arm"
    return model_name, group_name


def _make_kernel(deployment: Deployment | None = None):
    """Try NativeKernelClient backed by real RoboPlan C++; fall back to Mock.

    Returns:
        (kernel, bundle) — bundle is None when using MockKernelClient.
    """
    model_name, group_name = _native_model_and_group_from_deployment(deployment)
    collision_monitor_period_ns = _collision_monitor_period_ns_from_deployment(
        deployment
    )
    try:
        import manyforge_roboplan_py
        from native_kernel_client import NativeKernelClient

        kernel_config = None
        if collision_monitor_period_ns is not None:
            kernel_config = {
                "collision_monitor_period_ns": collision_monitor_period_ns,
            }
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
        return MockKernelClient(execute_duration_s=1.5), None


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
        allowed_fs_roots=security.allowed_fs_roots,
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
    parser.add_argument("--mcp", action="store_true",
                        help="Run as MCP server over stdio (no HTTP)")
    parser.add_argument("--mcp-http", action="store_true",
                        help="Enable HTTP/SSE MCP transport on /api/mcp (alongside REST)")
    args = parser.parse_args()

    setup_logging(args.log_level)

    if args.mcp and args.mcp_http:
        logger.error("--mcp (stdio) and --mcp-http cannot be used together")
        raise SystemExit(2)

    if args.mcp:
        _run_mcp(args)
        return

    security = SecurityConfig.from_env()
    kernel, bundle = _make_kernel()

    # Auto-resolve URDF and package dirs from roboplan if not specified via CLI
    urdf_path = args.urdf
    package_dirs = args.package_dir or []
    if not urdf_path or not package_dirs:
        roboplan_urdf, roboplan_share = _resolve_roboplan_paths()
        if not urdf_path and roboplan_urdf:
            urdf_path = roboplan_urdf
            logger.info("URDF auto-resolved from roboplan: %s", urdf_path)
        if not package_dirs and roboplan_share:
            package_dirs = [roboplan_share]
            logger.info("Package dir auto-resolved from roboplan: %s", roboplan_share)

    state = ComposerState(
        catalog_path=args.catalog_path,
        kernel=kernel,
        kernel_factory=_make_kernel,
        kernel_bundle=bundle,
        trace_dir="/tmp/manyforge-traces",
        allowed_fs_roots=security.allowed_fs_roots,
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
