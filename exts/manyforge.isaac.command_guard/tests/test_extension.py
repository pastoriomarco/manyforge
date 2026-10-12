from __future__ import annotations

import importlib
import json
from pathlib import Path
import sys
from threading import Event, Thread
from types import ModuleType, SimpleNamespace as NS
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
omni = ModuleType("omni")
omni.ext = ModuleType("omni.ext")
omni.ext.IExt = object
with patch.dict(sys.modules, {"omni": omni, "omni.ext": omni.ext}):
    extension = importlib.import_module("manyforge.isaac.command_guard.extension")

from manyforge.isaac.command_guard.runtime import RuntimeConfig


class FakeBindings:
    def __init__(self) -> None:
        self.events = []
        self.messages = []
        self.subscriptions = []
        self.fail_publish = False
        self.fail_node = False
        self.contexts = []
        owner = self

        class Context:
            pass

        class Status:
            # Jazzy DiagnosticStatus octet constants are one-byte Python bytes;
            # the native converter asserts on an int even though setter accepts it.
            OK, WARN, ERROR = b"\x00", b"\x01", b"\x02"

            @property
            def level(self) -> bytes:
                return self._level

            @level.setter
            def level(self, value: bytes) -> None:
                if type(value) is not bytes or len(value) != 1:
                    raise TypeError("native DiagnosticStatus octet requires one byte")
                self._level = value

        class Node:
            def __init__(self, name: str, context: object) -> None:
                owner.events.append(("node", context))
                if owner.fail_node:
                    raise RuntimeError("node creation failure")

            def create_publisher(self, kind: object, topic: str, qos: object) -> object:
                owner.events.append(("publisher", topic, qos))
                return NS(publish=self.publish)

            def publish(self, message: object) -> None:
                if owner.fail_publish:
                    raise RuntimeError("publisher failure")
                owner.messages.append(message)

            def create_subscription(self, kind: object, topic: str, callback: object,
                                    qos: object) -> object:
                subscription = NS(kind=kind, topic=topic, callback=callback, qos=qos)
                owner.subscriptions.append(subscription)
                owner.events.append(("subscribe", topic))
                return subscription

            def destroy_subscription(self, subscription: object) -> None:
                owner.events.append(("destroy_subscription", subscription.topic))
                owner.subscriptions.remove(subscription)

            def create_timer(self, interval: float, callback: object) -> object:
                owner.events.append(("timer", interval))
                return NS(callback=callback)

            def get_logger(self) -> object:
                return NS(error=lambda text: owner.events.append(("error", text)))

            def destroy_node(self) -> None:
                owner.events.append(("destroy_node",))

        class Executor:
            def __init__(self, context: object) -> None:
                owner.events.append(("executor", context))
                self.stop = Event()

            def add_node(self, node: object) -> None:
                owner.events.append(("add_node",))

            def spin(self) -> None:
                self.stop.wait(2)
                owner.events.append(("spin_exit",))

            def shutdown(self) -> None:
                owner.events.append(("executor_shutdown",))
                self.stop.set()

        self.Context, self.Node, self.Executor = Context, Node, Executor
        self.rclpy = NS(init=lambda *, context: self.events.append(("init", context)),
                        shutdown=lambda *, context: self.events.append(("context_shutdown", context)))
        self.QoSProfile = lambda **values: NS(**values)
        self.Reliable, self.Volatile, self.TransientLocal = "reliable", "volatile", "transient_local"
        self.JointState, self.Time = object(), object()
        self.Status = Status
        self.KeyValue = lambda **values: NS(**values)


def config() -> RuntimeConfig:
    return RuntimeConfig("/World/arm", ("a", "b"), "/command", "/authorize", "/status",
                         100_000_000_000)


class ExtensionTest(unittest.TestCase):
    def test_scene_settings_derive_scoped_topics_from_one_explicit_base(self) -> None:
        configs, requested = [], []
        values = dict(enabled=True, articulation_prim_path="/World/arm", joint_names=("b", "a"),
                      command_topic="/cell/arm/joint_commands", max_command_age_ns=123)

        def get(key: str) -> object:
            field = key.rsplit("/", 1)[-1]
            requested.append(field)
            return values.get(field)

        simulation = NS(IsaacEvents=NS(PRE_PHYSICS_STEP="pre"), SimulationManager=NS(
            register_callback=lambda *args, **kw: 99, deregister_callback=lambda key: None))
        modules = {"isaacsim": ModuleType("isaacsim"), "isaacsim.core": ModuleType("isaacsim.core"),
                   "isaacsim.core.simulation_manager": simulation, "omni": omni, "omni.ext": omni.ext,
                   "omni.usd": NS(get_context=lambda: NS(get_stage_event_stream=lambda: NS(
                       create_subscription_to_pop=lambda *args, **kw: object()))),
                   "carb": NS(settings=NS(get_settings=lambda: NS(get=get)))}
        ext = extension.Extension()
        session = NS(shutdown=lambda: None)
        with patch.dict(sys.modules, modules), patch.object(
                omni, "usd", modules["omni.usd"], create=True), patch.object(
                    extension, "RosSession", lambda cfg: (configs.append(cfg) or session)):
            ext.on_startup("guard")
            ext.on_shutdown()
        self.assertEqual(configs[0].joint_names, ("b", "a"))
        self.assertEqual(configs[0].authorization_topic, "/cell/arm/joint_commands/authorization")
        self.assertEqual(configs[0].guard_status_topic, "/cell/arm/joint_commands/guard_status")
        self.assertEqual(set(requested), {"enabled", "articulation_prim_path", "joint_names",
                                          "command_topic", "max_command_age_ns"})

    def test_disabled_scene_needs_no_age_but_enabled_missing_age_refuses_before_ros(self) -> None:
        values = dict(enabled=False, articulation_prim_path="/World/arm", joint_names=("a", "b"),
                      command_topic="/command")
        carb = NS(settings=NS(get_settings=lambda: NS(
            get=lambda key: values.get(key.rsplit("/", 1)[-1]))))
        with patch.dict(sys.modules, {"carb": carb}), patch.object(extension, "RosSession") as session:
            ext = extension.Extension()
            ext.on_startup("guard")
            self.assertIsNone(ext._session)
            session.assert_not_called()
            values["enabled"] = True
            with self.assertRaises(ValueError):
                ext.on_startup("guard")
            session.assert_not_called()

    def test_jazzy_status_byte_constants_for_closed_open_and_trip(self) -> None:
        bindings = FakeBindings()
        now = [10]
        session = extension.RosSession(config(), bindings, lambda: now[0])
        self.assertEqual(bindings.messages[-1].level, b"\x01")
        session.bind_writer(lambda positions: None)
        session._on_authorization(NS(sec=0, nanosec=1000))
        session.runtime.step()
        self.assertEqual(bindings.messages[-1].level, b"\x00")
        now[0] += config().max_command_age_ns
        session.runtime.monitor()
        session.runtime.publish_status()
        self.assertEqual(bindings.messages[-1].level, b"\x02")
        for message in bindings.messages:
            self.assertEqual(json.loads(dict((v.key, v.value) for v in message.values)["joint_names"]),
                             list(config().joint_names))
        session.shutdown()

    def test_partial_worker_start_failure_quiesces_only_started_threads(self) -> None:
        for fail_at in (1, 2):
            with self.subTest(fail_at=fail_at):
                bindings = FakeBindings()
                session = extension.RosSession(config(), bindings, lambda: 10)
                created = []

                def create_thread(**fields: object) -> Thread:
                    thread = Thread(**fields)
                    created.append(thread)
                    if len(created) == fail_at:
                        def fail_start() -> None:
                            raise RuntimeError("thread start failure")
                        thread.start = fail_start
                    return thread

                with patch.object(extension, "Thread", create_thread), self.assertRaisesRegex(
                        RuntimeError, "thread start failure"):
                    session.bind_writer(lambda positions: None)
                self.assertFalse(session.runtime.ready)
                self.assertTrue(all(not thread.is_alive() for thread in created))
                self.assertEqual([s.topic for s in bindings.subscriptions], ["/command"])
                self.assertIsNone(session._executor)
                self.assertIsNone(session._worker)
                self.assertIsNone(session._monitor)
                session.shutdown()
                self.assertEqual(bindings.events[-1][0], "context_shutdown")

    def test_kit_registration_or_stage_subscription_failure_unwinds_owned_resources(self) -> None:
        for failure in ("physics", "stage"):
            with self.subTest(failure=failure):
                bindings, callbacks = FakeBindings(), []
                values = dict(enabled=True, articulation_prim_path="/World/arm", joint_names=("a", "b"),
                              command_topic="/command", authorization_topic="/authorize",
                              guard_status_topic="/status", max_command_age_ns=100_000_000_000)

                def register(*args: object, **kw: object) -> int:
                    if failure == "physics":
                        raise RuntimeError("physics registration failure")
                    callbacks.append("register")
                    return 99

                def stage_subscribe(*args: object, **kw: object) -> object:
                    raise RuntimeError("stage subscription failure")

                simulation = NS(IsaacEvents=NS(PRE_PHYSICS_STEP="pre"), SimulationManager=NS(
                    register_callback=register, deregister_callback=lambda key: callbacks.append(("remove", key))))
                modules = {"isaacsim": ModuleType("isaacsim"), "isaacsim.core": ModuleType("isaacsim.core"),
                           "isaacsim.core.simulation_manager": simulation, "omni": omni,
                           "omni.ext": omni.ext, "omni.usd": NS(get_context=lambda: NS(
                               get_stage_event_stream=lambda: NS(create_subscription_to_pop=stage_subscribe))),
                           "carb": NS(settings=NS(get_settings=lambda: NS(
                               get=lambda key: values[key.rsplit("/", 1)[-1]])))}
                original_session = extension.RosSession
                ext = extension.Extension()
                with patch.dict(sys.modules, modules), patch.object(omni, "usd", modules["omni.usd"],
                                                                   create=True), patch.object(
                        extension, "RosSession", lambda cfg: original_session(cfg, bindings, lambda: 10)), \
                        self.assertRaisesRegex(RuntimeError, failure):
                    ext.on_startup("test-extension")
                self.assertIsNone(ext._session)
                self.assertIsNone(ext._callback)
                self.assertEqual(bindings.events[-2][0], "destroy_node")
                self.assertEqual(bindings.events[-1][0], "context_shutdown")
                self.assertEqual(callbacks, [] if failure == "physics" else ["register", ("remove", 99)])

    def test_own_context_qos_auth_after_binding_and_quiescent_teardown(self) -> None:
        bindings = FakeBindings()
        session = extension.RosSession(config(), bindings, lambda: 10)
        context = next(event[1] for event in bindings.events if event[0] == "init")
        self.assertIs(next(event[1] for event in bindings.events if event[0] == "node"), context)
        self.assertEqual([s.topic for s in bindings.subscriptions], ["/command"])
        self.assertEqual(bindings.messages[0].message, "closed")
        self.assertEqual(bindings.messages[0].level, b"\x01")
        self.assertEqual([v.key for v in bindings.messages[0].values], ["state", "joint_names"])
        session.bind_writer(lambda positions: None)
        self.assertIs(next(event[1] for event in bindings.events if event[0] == "executor"), context)
        self.assertEqual([s.topic for s in bindings.subscriptions], ["/command", "/authorize"])
        self.assertEqual(bindings.subscriptions[-1].qos.durability, "volatile")
        status_qos = next(event[2] for event in bindings.events if event[0] == "publisher")
        self.assertEqual((status_qos.reliability, status_qos.durability),
                         ("reliable", "transient_local"))
        session.shutdown()
        names = [event[0] for event in bindings.events]
        self.assertLess(names.index("executor_shutdown"), names.index("spin_exit"))
        self.assertLess(names.index("spin_exit"), names.index("destroy_subscription"))
        self.assertLess(names.index("destroy_subscription"), names.index("destroy_node"))
        self.assertLess(names.index("destroy_node"), names.index("context_shutdown"))
        self.assertIs(bindings.events[-1][1], context)

    def test_invalid_ros_callback_refuses_without_sdk_or_age_refresh(self) -> None:
        bindings = FakeBindings()
        session = extension.RosSession(config(), bindings, lambda: 10)
        writes = []
        session.bind_writer(writes.append)
        session._on_authorization(NS(sec=0, nanosec=1000))
        session.runtime.step()
        self.assertEqual(bindings.messages[-1].level, b"\x00")
        session._on_command(NS(header=NS(stamp=NS(sec=0, nanosec=1001)), name=("a",),
                               position=(0.1,), velocity=(), effort=()))
        self.assertEqual(writes, [])
        self.assertEqual(session.runtime.monitor().progress_receipt_ns, 10)
        self.assertTrue(any(event[0] == "error" for event in bindings.events))
        session.shutdown()

    def test_stage_close_retains_guard_and_removes_auth_before_rebind(self) -> None:
        bindings = FakeBindings()
        session = extension.RosSession(config(), bindings, lambda: 10)
        guard = session.runtime.guard
        session.bind_writer(lambda positions: None)
        session._on_authorization(NS(sec=0, nanosec=1000))
        session.runtime.step()
        session.close_binding()
        self.assertIs(session.runtime.guard, guard)
        self.assertEqual(guard.monitor().authorization_stamp_ns, 1000)
        self.assertFalse(guard.monitor().authorized)
        self.assertEqual([s.topic for s in bindings.subscriptions], ["/command"])
        session.bind_writer(lambda positions: None)
        session._on_authorization(NS(sec=0, nanosec=1000))
        session.runtime.step()
        self.assertFalse(guard.monitor().authorized)
        session.shutdown()

    def test_partial_start_failure_closes_only_owned_context(self) -> None:
        bindings = FakeBindings()
        bindings.fail_node = True
        with self.assertRaisesRegex(RuntimeError, "node creation"):
            extension.RosSession(config(), bindings, lambda: 10)
        self.assertEqual([event[0] for event in bindings.events], ["init", "node", "context_shutdown"])
        self.assertIs(bindings.events[0][1], bindings.events[-1][1])

    def test_initial_publish_failure_destroys_node_context_and_claims_no_delivery(self) -> None:
        bindings = FakeBindings()
        bindings.fail_publish = True
        with self.assertRaisesRegex(RuntimeError, "publisher failure"):
            extension.RosSession(config(), bindings, lambda: 10)
        self.assertEqual(bindings.events[-2][0], "destroy_node")
        self.assertEqual(bindings.events[-1][0], "context_shutdown")

    def test_publication_error_does_not_skip_shutdown_quiescence(self) -> None:
        bindings = FakeBindings()
        session = extension.RosSession(config(), bindings, lambda: 10)
        session.bind_writer(lambda positions: None)
        session._on_authorization(NS(sec=0, nanosec=1000))
        session.runtime.step()
        bindings.fail_publish = True
        with self.assertRaisesRegex(RuntimeError, "publisher failure"):
            session.shutdown()
        self.assertFalse(session.runtime.ready)
        names = [event[0] for event in bindings.events]
        self.assertIn("spin_exit", names)
        self.assertEqual(names[-2:], ["destroy_node", "context_shutdown"])

    def test_sdk_binding_checks_ready_and_scopes_position_only_named_dofs(self) -> None:
        writes, bound = [], []
        articulation = NS(handles_initialized=True, dof_names=("finger", "b", "a"),
                          initialize=lambda: None, get_dof_index=lambda name: {"a": 2, "b": 1}[name],
                          apply_action=writes.append)
        modules = {"isaacsim": ModuleType("isaacsim"), "isaacsim.core": ModuleType("isaacsim.core"),
                   "isaacsim.core.prims": NS(SingleArticulation=lambda *args, **kw: articulation),
                   "isaacsim.core.utils": ModuleType("isaacsim.core.utils"),
                   "isaacsim.core.utils.types": NS(ArticulationAction=lambda **values: NS(**values)),
                   "omni": omni, "omni.ext": omni.ext,
                   "omni.usd": NS(get_context=lambda: NS(get_stage=lambda: NS(
                       GetPrimAtPath=lambda path: NS(IsValid=lambda: True))))}
        ext = extension.Extension()
        ext._failed = False
        ext._articulation = None
        ext._session = NS(runtime=NS(config=config(), step=lambda: bound[-1]((0.1, 0.2))),
                          bind_writer=bound.append)
        with patch.dict(sys.modules, modules), patch.object(omni, "usd", modules["omni.usd"], create=True):
            ext._on_physics_step(None)
        self.assertEqual(len(bound), 1)
        self.assertEqual(writes[0].joint_positions, [0.1, 0.2])
        self.assertEqual(writes[0].joint_indices, [2, 1])
        self.assertFalse(hasattr(writes[0], "joint_velocities"))
        self.assertFalse(hasattr(writes[0], "joint_efforts"))

    def test_invalid_prim_handle_or_dof_map_never_binds_or_admits_authorization(self) -> None:
        for prim_valid, handle_valid, names in ((False, True, ("a", "b")),
                                               (True, False, ("a", "b")),
                                               (True, True, ("a", "finger")),
                                               (True, True, ("a", "a", "b"))):
            with self.subTest(prim=prim_valid, handle=handle_valid, names=names):
                bound, closed, errors = [], [], []
                articulation = NS(handles_initialized=handle_valid, dof_names=names,
                                  initialize=lambda: None)
                modules = {"isaacsim": ModuleType("isaacsim"),
                           "isaacsim.core": ModuleType("isaacsim.core"),
                           "isaacsim.core.prims": NS(SingleArticulation=lambda *a, **kw: articulation),
                           "isaacsim.core.utils": ModuleType("isaacsim.core.utils"),
                           "isaacsim.core.utils.types": NS(ArticulationAction=object),
                           "omni": omni, "omni.ext": omni.ext,
                           "omni.usd": NS(get_context=lambda: NS(get_stage=lambda: NS(
                               GetPrimAtPath=lambda path: NS(IsValid=lambda: prim_valid)))),
                           "carb": NS(log_error=errors.append)}
                ext = extension.Extension()
                ext._failed, ext._articulation = False, None
                ext._session = NS(runtime=NS(config=config(), close=lambda: closed.append(True)),
                                  bind_writer=bound.append)
                with patch.dict(sys.modules, modules), patch.object(omni, "usd", modules["omni.usd"],
                                                                   create=True):
                    ext._on_physics_step(None)
                self.assertTrue(ext._failed)
                self.assertEqual(bound, [])
                self.assertEqual(closed, [True])
                self.assertEqual(len(errors), 1)

    def test_original_sdk_failure_survives_secondary_close_publication_error(self) -> None:
        errors = []
        original_error = RuntimeError("SDK effect outcome unknown")

        def fail_step() -> None:
            raise original_error

        def fail_close() -> None:
            raise RuntimeError("DDS close delivery unknown")

        modules = {"isaacsim": ModuleType("isaacsim"), "isaacsim.core": ModuleType("isaacsim.core"),
                   "isaacsim.core.prims": NS(SingleArticulation=object),
                   "isaacsim.core.utils": ModuleType("isaacsim.core.utils"),
                   "isaacsim.core.utils.types": NS(ArticulationAction=object),
                   "omni": omni, "omni.ext": omni.ext, "omni.usd": NS(),
                   "carb": NS(log_error=errors.append)}
        ext = extension.Extension()
        ext._failed, ext._articulation = False, object()
        ext._session = NS(runtime=NS(step=fail_step, close=fail_close))
        with patch.dict(sys.modules, modules), patch.object(omni, "usd", modules["omni.usd"], create=True):
            ext._on_physics_step(None)
        self.assertTrue(ext._failed)
        self.assertIs(ext._writer_error, original_error)
        self.assertIn("SDK effect outcome unknown", errors[0])
        self.assertIn("DDS close delivery unknown", errors[1])
        self.assertIn("DDS close delivery unknown", original_error.__notes__[0])


if __name__ == "__main__":
    unittest.main()
