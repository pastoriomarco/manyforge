"""Observe configured Isaac ROS sources through the Manager-owned runtime.

The process is a thin producer. It normalizes depth-one JointState and
Detection3D subscriptions plus bounded ``simulation_interfaces`` reads, then
hands immutable observations to Manager. It owns no freshness, scene apply,
lifecycle, command admission, or Composer protocol.
"""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import sys
import threading
import time
from typing import Any, Protocol

import rclpy
from rclpy.context import Context
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from rclpy.time import Time

from manyforge_ros.isaac_pose_observer import (
    Detection3dBinding,
    IsaacPoseObserverSiteConfig,
    JointStateBinding,
    ObservationDispatchEvent,
    ObservationDispatchSnapshot,
    ObservationDispatcher,
    SimulationEntityBinding,
    detection3d_observation_intent,
    joint_state_observation_intent,
    load_isaac_pose_observer_site_config,
    simulation_entity_observation_intent,
    unknown_observation_intent,
)
from manyforge_ros.manager_scene_runtime import PoseObservationIntent
from manyforge_ros.pose_source_sidecar import (
    AdapterFailure,
    DepthOneSubscriptionAdapter,
    ObservationNormalizationError,
    capture_simulation_entity_poses,
    normalize_detection3d_array_message,
    normalize_joint_state_message,
)
from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient


class _Dispatcher(Protocol):
    def start(self) -> None: ...

    def submit(self, intent: PoseObservationIntent) -> None: ...

    def close(self, *, timeout_s: float = 6.0) -> bool: ...

    def snapshot(self) -> ObservationDispatchSnapshot: ...


DispatcherFactory = Callable[
    [Path, IsaacPoseObserverSiteConfig, Callable[[ObservationDispatchEvent], None]],
    _Dispatcher,
]
SimulationClientFactory = Callable[[Node, float], Any]


def _default_dispatcher_factory(
    manager_run_root: Path,
    config: IsaacPoseObserverSiteConfig,
    callback: Callable[[ObservationDispatchEvent], None],
) -> ObservationDispatcher:
    return ObservationDispatcher.for_manager_root(
        manager_run_root=manager_run_root,
        config=config,
        event_callback=callback,
    )


def _default_simulation_client_factory(node: Node, timeout_s: float) -> Any:
    return SimulationSceneMirrorClient(node, service_timeout_s=timeout_s)


class PoseSourceSnapshotPublisher(Node):
    """Mechanical ROS producer for the Manager scene-runtime owner."""

    def __init__(
        self,
        *,
        site_config_path: Path | None = None,
        manager_run_root: Path | None = None,
        dispatcher_factory: DispatcherFactory = _default_dispatcher_factory,
        simulation_client_factory: SimulationClientFactory = (
            _default_simulation_client_factory
        ),
        context: Context | None = None,
    ) -> None:
        super().__init__("manyforge_pose_source_snapshot_publisher", context=context)
        self.declare_parameter("site_config_path", "")
        self.declare_parameter("manager_run_root", "")
        self.declare_parameter("manager_timeout_s", 2.0)
        self.declare_parameter("simulation_service_timeout_s", 2.0)
        self.declare_parameter("retry_log_period_s", 5.0)

        configured_site_path = (
            Path(site_config_path)
            if site_config_path is not None
            else _required_path_parameter(self, "site_config_path")
        )
        configured_manager_root = (
            Path(manager_run_root)
            if manager_run_root is not None
            else _required_path_parameter(self, "manager_run_root")
        )
        self._config = load_isaac_pose_observer_site_config(configured_site_path)
        self._manager_timeout_s = _bounded_parameter(
            self, "manager_timeout_s", minimum=0.05, maximum=5.0
        )
        self._simulation_service_timeout_s = _bounded_parameter(
            self,
            "simulation_service_timeout_s",
            minimum=0.05,
            maximum=30.0,
        )
        self._retry_log_period_s = _bounded_parameter(
            self, "retry_log_period_s", minimum=0.1, maximum=300.0
        )
        if dispatcher_factory is _default_dispatcher_factory:
            self._dispatcher = ObservationDispatcher.for_manager_root(
                manager_run_root=configured_manager_root,
                config=self._config,
                event_callback=self._on_dispatch_event,
                timeout_s=self._manager_timeout_s,
            )
        else:
            self._dispatcher = dispatcher_factory(
                configured_manager_root,
                self._config,
                self._on_dispatch_event,
            )
        self._simulation_client_factory = simulation_client_factory
        # Do not shadow rclpy.Node._subscriptions: executors consume that
        # internal collection as transport entities.
        self._source_adapters: list[DepthOneSubscriptionAdapter] = []
        self._tf_buffer: Any | None = None
        self._tf_listener: Any | None = None
        self._simulation_node: Node | None = None
        self._simulation_client: Any | None = None
        self._simulation_thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._started = False
        self._stopped = False
        self._last_retry_log_s = 0.0
        self._last_dispatch_error = ""
        self._configure_joint_state()
        self._configure_detections()
        self._configure_simulation()

    @property
    def auxiliary_nodes(self) -> tuple[Node, ...]:
        return () if self._simulation_node is None else (self._simulation_node,)

    def start(self) -> None:
        if self._started:
            raise RuntimeError("pose-source publisher was already started")
        self._started = True
        self._dispatcher.start()
        if self._simulation_client is not None:
            self._simulation_thread = threading.Thread(
                target=self._simulation_loop,
                name="manyforge-isaac-simulation-pose-poll",
                daemon=True,
            )
            self._simulation_thread.start()
        self.get_logger().info(
            "isaac_pose_observer_started "
            + json.dumps(
                {
                    "cell_id": self._config.cell_id,
                    "detection_sources": len(self._config.detection3d_sources),
                    "joint_state": self._config.joint_state is not None,
                    "simulation_entities": len(self._config.simulation_entities),
                    "source_id": self._config.source_id,
                },
                separators=(",", ":"),
                sort_keys=True,
            )
        )

    def stop(self) -> bool:
        if self._stopped:
            return True
        self._stopped = True
        self._stop_event.set()
        for adapter in self._source_adapters:
            adapter.close()
        self._source_adapters.clear()
        thread_stopped = True
        if self._simulation_thread is not None:
            self._simulation_thread.join(
                timeout=self._simulation_service_timeout_s + 1.0
            )
            thread_stopped = not self._simulation_thread.is_alive()
        dispatcher_stopped = self._dispatcher.close(
            timeout_s=self._manager_timeout_s + 1.0
        )
        if not thread_stopped or not dispatcher_stopped:
            self.get_logger().error(
                "isaac_pose_observer_shutdown_incomplete "
                + json.dumps(
                    {
                        "dispatcher_stopped": dispatcher_stopped,
                        "simulation_thread_stopped": thread_stopped,
                    },
                    separators=(",", ":"),
                    sort_keys=True,
                )
            )
        return thread_stopped and dispatcher_stopped

    def _configure_joint_state(self) -> None:
        binding = self._config.joint_state
        if binding is None:
            return
        from sensor_msgs.msg import JointState

        self._source_adapters.append(
            DepthOneSubscriptionAdapter(
                node=self,
                message_type=JointState,
                topic=binding.topic,
                normalize=lambda message: normalize_joint_state_message(
                    message, topic=binding.topic
                ),
                on_observation=lambda observation: self._submit_joint(
                    binding, observation
                ),
                on_error=lambda error: self._submit_unknown(
                    binding,
                    error=error,
                    source_key=binding.topic,
                ),
            )
        )

    def _configure_detections(self) -> None:
        if not self._config.detection3d_sources:
            return
        from vision_msgs.msg import Detection3DArray

        self._ensure_tf()
        for binding in self._config.detection3d_sources:
            self._source_adapters.append(
                DepthOneSubscriptionAdapter(
                    node=self,
                    message_type=Detection3DArray,
                    topic=binding.topic,
                    normalize=lambda message, selected=binding: (
                        normalize_detection3d_array_message(
                            message,
                            selector=selected.selector,
                            fallback_frame_id=selected.fallback_frame_id,
                            target_frame_id=selected.target_frame_id,
                            tf_buffer=self._tf_buffer,
                            lookup_time=Time(),
                        )
                    ),
                    on_observation=lambda observation, selected=binding: (
                        self._submit_detection(selected, observation)
                    ),
                    on_error=lambda error, selected=binding: self._submit_unknown(
                        selected,
                        error=error,
                        source_key=selected.topic,
                    ),
                )
            )

    def _configure_simulation(self) -> None:
        if not self._config.simulation_entities:
            return
        if any(
            binding.request.target_frame_id
            for binding in self._config.simulation_entities
        ):
            self._ensure_tf()
        # The entrypoint deliberately owns a private rclpy Context.  Every
        # auxiliary node must join that same context; falling back to rclpy's
        # uninitialized global context makes the UR10 simulation-entity
        # observer fail during construction while joint-only Lite6 profiles
        # happen to remain unaffected.
        self._simulation_node = Node(
            "manyforge_isaac_pose_service_client", context=self.context
        )
        self._simulation_client = self._simulation_client_factory(
            self._simulation_node,
            self._simulation_service_timeout_s,
        )

    def _ensure_tf(self) -> None:
        if self._tf_buffer is not None:
            return
        from tf2_ros import Buffer, TransformListener

        self._tf_buffer = Buffer()
        self._tf_listener = TransformListener(self._tf_buffer, self)

    def _submit_joint(self, binding: JointStateBinding, observation: Any) -> None:
        try:
            self._dispatcher.submit(
                joint_state_observation_intent(binding, observation)
            )
        except ObservationNormalizationError as error:
            self._submit_unknown(binding, error=error, source_key=binding.topic)
        except Exception as error:
            self._log_adapter_failure(binding, error)

    def _submit_detection(
        self,
        binding: Detection3dBinding,
        observation: Any,
    ) -> None:
        try:
            self._dispatcher.submit(
                detection3d_observation_intent(binding, observation)
            )
        except ObservationNormalizationError as error:
            self._submit_unknown(binding, error=error, source_key=binding.topic)
        except Exception as error:
            self._log_adapter_failure(binding, error)

    def _submit_simulation(
        self,
        binding: SimulationEntityBinding,
        observation: Any,
    ) -> None:
        try:
            self._dispatcher.submit(
                simulation_entity_observation_intent(binding, observation)
            )
        except ObservationNormalizationError as error:
            self._submit_unknown(
                binding,
                error=error,
                source_key=binding.request.entity,
            )
        except Exception as error:
            self._log_adapter_failure(binding, error)

    def _submit_unknown(
        self,
        binding: JointStateBinding | SimulationEntityBinding | Detection3dBinding,
        *,
        error: ObservationNormalizationError | AdapterFailure,
        source_key: str,
    ) -> None:
        try:
            self._dispatcher.submit(
                unknown_observation_intent(
                    binding,
                    code=error.code,
                    detail=error.detail,
                    source_key=source_key,
                )
            )
        except Exception as submit_error:
            self._log_adapter_failure(binding, submit_error)

    def _simulation_loop(self) -> None:
        period_s = self._config.simulation_poll_period_ms / 1000.0
        bindings_by_entity = {
            binding.request.entity: binding
            for binding in self._config.simulation_entities
        }
        requests = tuple(
            binding.request for binding in self._config.simulation_entities
        )
        while not self._stop_event.is_set():
            # ``start()`` runs immediately before the entrypoint begins
            # spinning its already-attached executor.  Do not let the
            # background service client enter rclpy's global-executor fallback
            # during that narrow window: this process deliberately owns only a
            # private Context.  Once spinning, the normal client path observes
            # the attached executor and waits for its future without competing
            # for node ownership.
            executor = (
                None
                if self._simulation_node is None
                else self._simulation_node.executor
            )
            if executor is not None and not executor.is_spinning:
                self._stop_event.wait(min(0.01, period_s))
                continue
            started = time.monotonic()
            try:
                batch = capture_simulation_entity_poses(
                    self._simulation_client,
                    requests,
                    tf_buffer=self._tf_buffer,
                    lookup_time=Time(),
                )
                observed_entities: set[str] = set()
                failed_entities: set[str] = set()
                for observation in batch.observations:
                    observed_entities.add(observation.entity)
                    binding = bindings_by_entity.get(observation.entity)
                    if binding is not None:
                        self._submit_simulation(binding, observation)
                for failure in batch.failures:
                    binding = bindings_by_entity.get(failure.source_key)
                    if binding is None:
                        self._log_source_failure(failure)
                        continue
                    failed_entities.add(failure.source_key)
                    self._submit_unknown(
                        binding,
                        error=failure,
                        source_key=failure.source_key,
                    )
                for entity, binding in bindings_by_entity.items():
                    if (
                        entity not in observed_entities
                        and entity not in failed_entities
                    ):
                        self._submit_unknown(
                            binding,
                            error=AdapterFailure(
                                entity,
                                "source_result_missing",
                                "simulator batch omitted a configured result",
                            ),
                            source_key=entity,
                        )
            except Exception as error:
                normalized = ObservationNormalizationError(
                    "source_error", str(error)[:512] or "simulation poll failed"
                )
                for binding in self._config.simulation_entities:
                    self._submit_unknown(
                        binding,
                        error=normalized,
                        source_key=binding.request.entity,
                    )
            remaining = max(0.0, period_s - (time.monotonic() - started))
            self._stop_event.wait(remaining)

    def _on_dispatch_event(self, event: ObservationDispatchEvent) -> None:
        if event.kind == "delivered":
            if self._last_dispatch_error:
                self.get_logger().info(
                    "isaac_pose_observer_recovered "
                    + self._event_json(event, self._dispatcher.snapshot())
                )
                self._last_dispatch_error = ""
            return
        now = time.monotonic()
        self._last_dispatch_error = event.code
        if now - self._last_retry_log_s < self._retry_log_period_s:
            return
        self._last_retry_log_s = now
        self.get_logger().warning(
            "isaac_pose_observer_delivery "
            + self._event_json(event, self._dispatcher.snapshot())
        )

    @staticmethod
    def _event_json(
        event: ObservationDispatchEvent,
        snapshot: ObservationDispatchSnapshot,
    ) -> str:
        return json.dumps(
            {
                "code": event.code,
                "delivered": snapshot.delivered,
                "detail": event.detail,
                "expired": snapshot.expired,
                "joins": snapshot.joins,
                "kind": event.kind,
                "object_id": event.object_id,
                "pending": snapshot.pending,
                "policy_id": event.policy_id,
                "retries": snapshot.retries,
                "scene_application_status": event.scene_application_status,
                "scene_request_id": event.scene_request_id,
                "scene_runtime_source_generation": (
                    event.scene_runtime_source_generation
                ),
                "scene_update_status": event.scene_update_status,
                "source_sequence": event.source_sequence,
                "superseded": snapshot.superseded,
            },
            separators=(",", ":"),
            sort_keys=True,
        )

    def _log_adapter_failure(self, binding: Any, error: Exception) -> None:
        self.get_logger().error(
            "isaac_pose_observer_adapter_failure "
            + json.dumps(
                {
                    "detail": str(error)[:512],
                    "object_id": binding.object_id,
                    "policy_id": binding.policy_id,
                },
                separators=(",", ":"),
                sort_keys=True,
            )
        )

    def _log_source_failure(self, failure: AdapterFailure) -> None:
        now = time.monotonic()
        if now - self._last_retry_log_s < self._retry_log_period_s:
            return
        self._last_retry_log_s = now
        self.get_logger().warning(
            "isaac_pose_observer_source_failure "
            + json.dumps(
                {
                    "code": failure.code,
                    "detail": failure.detail,
                    "source_key": failure.source_key,
                },
                separators=(",", ":"),
                sort_keys=True,
            )
        )


def _required_path_parameter(node: Node, name: str) -> Path:
    value = str(node.get_parameter(name).value).strip()
    if not value:
        raise ValueError(f"{name} is required")
    return Path(value)


def _bounded_parameter(
    node: Node,
    name: str,
    *,
    minimum: float,
    maximum: float,
) -> float:
    value = node.get_parameter(name).value
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be numeric")
    parsed = float(value)
    if not minimum <= parsed <= maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    return parsed


def main(args: list[str] | None = None) -> int:
    context = Context()
    rclpy.init(args=args, context=context)
    node: PoseSourceSnapshotPublisher | None = None
    executor: MultiThreadedExecutor | None = None
    result = 0
    try:
        node = PoseSourceSnapshotPublisher(context=context)
        executor = MultiThreadedExecutor(num_threads=2, context=context)
        executor.add_node(node)
        for auxiliary in node.auxiliary_nodes:
            executor.add_node(auxiliary)
        node.start()
        executor.spin()
    except KeyboardInterrupt:
        result = 0
    except Exception as error:
        print(f"isaac_pose_observer_failed: {error}", file=sys.stderr)
        result = 78
    finally:
        if node is not None:
            if not node.stop() and result == 0:
                result = 1
            if executor is not None:
                for auxiliary in node.auxiliary_nodes:
                    executor.remove_node(auxiliary)
                    auxiliary.destroy_node()
                executor.remove_node(node)
            else:
                for auxiliary in node.auxiliary_nodes:
                    auxiliary.destroy_node()
            node.destroy_node()
        if executor is not None:
            executor.shutdown(timeout_sec=2.0)
        if context.ok():
            rclpy.shutdown(context=context)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
