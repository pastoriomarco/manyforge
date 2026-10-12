"""Isaac/ROS glue. Disabled by default; authored scene cutover is prerequisite."""
from __future__ import annotations

from threading import Event, Thread
import time
from types import SimpleNamespace
from typing import Any

import omni.ext

from .runtime import GuardRuntime, RuntimeConfig, status_fields


def _ros_bindings() -> Any:
    # The declared ROS bridge dependency selects the installed Jazzy bindings.
    import rclpy
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.node import Node
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from sensor_msgs.msg import JointState
    from builtin_interfaces.msg import Time
    from diagnostic_msgs.msg import DiagnosticStatus, KeyValue
    return SimpleNamespace(rclpy=rclpy, Context=Context, Executor=SingleThreadedExecutor,
                           Node=Node, QoSProfile=QoSProfile, Reliable=ReliabilityPolicy.RELIABLE,
                           Volatile=DurabilityPolicy.VOLATILE,
                           TransientLocal=DurabilityPolicy.TRANSIENT_LOCAL,
                           JointState=JointState, Time=Time, Status=DiagnosticStatus, KeyValue=KeyValue)


class RosSession:
    """Own context and callbacks; retains the same pure guard across stage close."""

    def __init__(self, config: RuntimeConfig, bindings: Any = None,
                 clock: Any = time.monotonic_ns) -> None:
        self._b = bindings if bindings is not None else _ros_bindings()
        self._stop = Event()
        self._worker: Thread | None = None
        self._monitor: Thread | None = None
        self._executor: Any = None
        self._authorization: Any = None
        self._context = self._b.Context()
        self._b.rclpy.init(context=self._context)
        self._node = None
        try:
            self._initialize_resources(config, clock)
        except Exception:
            if self._node is not None:
                self._node.destroy_node()
            self._b.rclpy.shutdown(context=self._context)
            raise

    def _initialize_resources(self, config: RuntimeConfig, clock: Any) -> None:
        self._node = self._b.Node("manyforge_isaac_command_guard", context=self._context)
        self._command_qos = self._b.QoSProfile(
            depth=1, reliability=self._b.Reliable, durability=self._b.Volatile)
        status_qos = self._b.QoSProfile(
            depth=1, reliability=self._b.Reliable, durability=self._b.TransientLocal)
        self._publisher = self._node.create_publisher(self._b.Status, config.guard_status_topic,
                                                     status_qos)
        self.runtime = GuardRuntime(config, clock, self._publish)
        self._command = self._node.create_subscription(
            self._b.JointState, config.command_topic, self._on_command, self._command_qos)
        # Derived only from the explicitly supplied age; no policy default.
        self._interval = config.max_command_age_ns / 2_000_000_000
        self._timer = self._node.create_timer(self._interval, self._on_status)
        self.runtime.publish_status()  # Truthful closed, not SDK readiness.

    def _publish(self, status: Any) -> None:
        message = self._b.Status()
        message.name = "manyforge.isaac.command_guard"
        message.hardware_id = self.runtime.config.articulation_prim_path
        message.level = (self._b.Status.ERROR if status.tripped else
                         self._b.Status.OK if status.authorized else self._b.Status.WARN)
        message.message = "tripped" if status.tripped else "open" if status.authorized else "closed"
        message.values = [self._b.KeyValue(key=key, value=value)
                          for key, value in status_fields(status, self.runtime.config.joint_names)]
        self._publisher.publish(message)

    def _on_command(self, message: Any) -> None:
        try:
            self.runtime.receive_command(message)
        except (ValueError, TypeError, AttributeError) as error:
            self._node.get_logger().error(f"Invalid arm JointState refused: {error}")

    def _on_authorization(self, message: Any) -> None:
        try:
            self.runtime.receive_authorization(message)
        except (ValueError, TypeError, AttributeError) as error:
            self._node.get_logger().error(f"Invalid authorization Time refused: {error}")

    def _on_status(self) -> None:
        try:
            self.runtime.publish_status()
        except Exception as error:
            self._node.get_logger().error(f"Guard status publication failed; delivery unknown: {error}")

    def bind_writer(self, writer: Any) -> None:
        self.runtime.bind_writer(writer)
        try:
            # Volatile Time subscription exists only AFTER actual writer binding.
            self._authorization = self._node.create_subscription(
                self._b.Time, self.runtime.config.authorization_topic,
                self._on_authorization, self._command_qos)
            self._executor = self._b.Executor(context=self._context)
            self._executor.add_node(self._node)
            self._stop.clear()
            self._worker = Thread(target=self._spin, name="manyforge-guard-ros", daemon=True)
            self._monitor = Thread(target=self._monitor_loop, name="manyforge-guard-age", daemon=True)
            self._worker.start()
            self._monitor.start()
        except Exception as error:
            try:
                self.close_binding()
            except Exception as cleanup_error:
                error.add_note(f"Guard partial binding cleanup failed: {cleanup_error}")
            raise

    def _spin(self) -> None:
        try:
            self._executor.spin()
        except Exception as error:
            if self._stop.is_set():
                return  # Owned executor shutdown is ordinary quiescence.
            self.runtime.close()
            self._stop.set()
            self._node.get_logger().error(f"Guard ROS executor failed: {error}")

    def _monitor_loop(self) -> None:
        while not self._stop.wait(self._interval):
            self.runtime.monitor()  # No publish, SDK or publication-lock wait.

    def close_binding(self) -> None:
        try:
            self.runtime.close()  # Discard frame/Time; retain T, ordering and trip.
        finally:
            self._stop.set()
            if self._executor is not None:
                self._executor.shutdown()  # Explicit callback quiescence, no timeout proof.
            for thread in (self._worker, self._monitor):
                if thread is not None and thread.ident is not None:
                    thread.join()
            if self._authorization is not None:
                self._node.destroy_subscription(self._authorization)
            self._authorization = self._executor = self._worker = self._monitor = None

    def shutdown(self) -> None:
        try:
            self.close_binding()
        finally:
            self._node.destroy_node()
            self._b.rclpy.shutdown(context=self._context)  # Never bridge/default context.


class Extension(omni.ext.IExt):
    def on_startup(self, ext_id: str) -> None:
        self._session: RosSession | None = None
        self._callback: int | None = None
        self._stage_subscription: Any = None
        self._articulation: Any = None
        self._failed = False
        self._writer_error: Exception | None = None
        import carb
        settings = carb.settings.get_settings()
        prefix = "/exts/manyforge.isaac.command_guard/"
        if not settings.get(prefix + "enabled"):
            return
        command_topic = settings.get(prefix + "command_topic")
        config = RuntimeConfig(
            settings.get(prefix + "articulation_prim_path"),
            tuple(settings.get(prefix + "joint_names") or ()),
            command_topic, f"{command_topic}/authorization", f"{command_topic}/guard_status",
            settings.get(prefix + "max_command_age_ns"))
        # No configured age default, startup motion, CCLC, or Reset here.
        from isaacsim.core.simulation_manager import IsaacEvents, SimulationManager
        import omni.usd
        try:
            self._session = RosSession(config)
            self._callback = SimulationManager.register_callback(
                self._on_physics_step, IsaacEvents.PRE_PHYSICS_STEP,
                name="manyforge-isaac-command-guard")
            self._stage_subscription = omni.usd.get_context().get_stage_event_stream().create_subscription_to_pop(
                self._on_stage_event, name="manyforge-isaac-command-guard-stage")
        except Exception as error:
            try:
                self.on_shutdown()
            except Exception as cleanup_error:
                error.add_note(f"Guard partial startup cleanup failed: {cleanup_error}")
            raise

    def _on_physics_step(self, step: Any) -> None:
        if self._session is None or self._failed:
            return
        try:
            from isaacsim.core.prims import SingleArticulation
            from isaacsim.core.utils.types import ArticulationAction
            import omni.usd
            if self._articulation is None:
                config = self._session.runtime.config
                stage = omni.usd.get_context().get_stage()
                if stage is None or not stage.GetPrimAtPath(config.articulation_prim_path).IsValid():
                    raise RuntimeError("configured articulation prim unavailable")
                articulation = SingleArticulation(config.articulation_prim_path,
                                                  reset_xform_properties=False)
                articulation.initialize()
                if not articulation.handles_initialized:
                    raise RuntimeError("articulation physics handle unavailable")
                available = tuple(articulation.dof_names)
                if any(available.count(name) != 1 for name in config.joint_names):
                    raise RuntimeError("configured arm DOF map unavailable or ambiguous")
                indices = tuple(articulation.get_dof_index(name) for name in config.joint_names)

                def apply_positions(positions: tuple[float, ...]) -> None:
                    if not articulation.handles_initialized:
                        raise RuntimeError("articulation physics handle lost")
                    articulation.apply_action(ArticulationAction(
                        joint_positions=list(positions), joint_indices=list(indices)))

                self._articulation = articulation
                self._session.bind_writer(apply_positions)
            self._session.runtime.step()
        except Exception as error:
            self._failed = True
            self._writer_error = error
            import carb
            carb.log_error(f"ManyForge guard writer unavailable: {error}")
            try:
                self._session.runtime.close()
            except Exception as close_error:
                error.add_note(f"Guard close publication delivery unknown: {close_error}")
                carb.log_error(f"ManyForge guard close publication failed; delivery unknown: {close_error}")

    def _on_stage_event(self, event: Any) -> None:
        import omni.usd
        if event.type in (int(omni.usd.StageEventType.CLOSING), int(omni.usd.StageEventType.CLOSED)):
            if self._session is not None:
                self._session.close_binding()
            self._articulation = None
            # Existing guard/floors survive. No automatic fault recovery.

    def on_shutdown(self) -> None:
        try:
            if self._callback is not None:
                from isaacsim.core.simulation_manager import SimulationManager
                SimulationManager.deregister_callback(self._callback)
        finally:
            self._callback = self._stage_subscription = None
            try:
                if self._session is not None:
                    self._session.shutdown()
            finally:
                self._session = self._articulation = None
