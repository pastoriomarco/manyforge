# Copyright 2026 Marco Pastorio
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""
Real sequencer action with native-message stand-ins, never GPU qualification.

The empty standard container is started first. The test establishes its own
construction-response stand-in before loading the real sequencer with the
existing LoadNode service. Returned node names are fixture data, not proof
that NVIDIA components were constructed or engines are ready.
"""

from array import array
import importlib.util
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

from action_msgs.msg import GoalStatus
from composition_interfaces.srv import ListNodes, LoadNode, UnloadNode
from launch import LaunchContext, LaunchDescription
from launch_ros.actions import ComposableNodeContainer
import launch_testing
from manyforge_perception_interfaces.action import EstimateInstances
import pytest
import rclpy
from rclpy.action import ActionClient
from rclpy.parameter import Parameter
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CameraInfo, Image
from vision_msgs.msg import Detection2DArray


@pytest.mark.launch_test
def generate_test_description():
    container = ComposableNodeContainer(
        name='action_fixture', namespace='', package='rclcpp_components',
        executable='component_container_mt', output='screen',
        composable_node_descriptions=[])
    return LaunchDescription([container, launch_testing.actions.ReadyToTest()])


class TestActionContract(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('action_contract_fixture')
        cls.construction = cls.node.create_service(
            ListNodes, '/standin_construction/_container/list_nodes', cls.list_nodes)
        cls.native_requests = []
        cls.native_sub = cls.node.create_subscription(
            Image, '/native/detect/image', cls.native_requests.append, 10)
        cls.native_pub = cls.node.create_publisher(Detection2DArray, '/native/detections', 10)
        cls.rgb_pub = cls.node.create_publisher(Image, '/image_rect', qos_profile_sensor_data)
        cls.depth_pub = cls.node.create_publisher(Image, '/depth', qos_profile_sensor_data)
        cls.camera_pub = cls.node.create_publisher(
            CameraInfo, '/camera_info', qos_profile_sensor_data)
        cls.loader = cls.node.create_client(LoadNode, '/action_fixture/_container/load_node')
        cls.unloader = cls.node.create_client(UnloadNode, '/action_fixture/_container/unload_node')
        if not cls.loader.wait_for_service(timeout_sec=10.0):
            raise AssertionError('standard fixture container did not appear')
        # Arbitrary explicit fixture tolerances; these are not Orin settings.
        cls.provider_values = {
            'component_container': '/standin_construction',
            'model_id': 'fixture', 'model_revision': 'fixture-revision',
            'settle_frames': 0, 'duplicate_iou': 0.8, 'crop_side': 2, 'max_reuse': 2,
            'prior_iou': 0.8, 'prior_center_px': 1.0, 'prior_size_ratio': 0.1,
            'unchanged_tol_m': 0.01, 'unchanged_max_fraction': 0.0,
            'small_change_max_fraction': 0.5, 'small_change_max_m': 0.1,
            'colour_tol': 0.1, 'colour_unchanged_max_fraction': 0.0,
            'colour_small_change_max_fraction': 0.5,
            'refine_max_translation': 0.05, 'refine_max_rotation': 0.2,
            'asset_symmetry_axes': ['x_full'],
            'asset_symmetry_quaternions': [0.0, 0.0, 0.0, 1.0],
            'completion_grace_s': 0.1}
        response = cls.load_provider()
        if not response.success:
            raise AssertionError(response.error_message)
        cls.action = ActionClient(cls.node, EstimateInstances, '/estimate_instances')
        cls.until(lambda: cls.action.server_is_ready())
        cls.until(lambda: all(
            pub.get_subscription_count() for pub in
            (cls.rgb_pub, cls.depth_pub, cls.camera_pub, cls.native_pub)))
        cls.capture_counter = 0

    @classmethod
    def load_provider(cls, namespace='', overrides=None, missing=()):
        request = LoadNode.Request()
        request.package_name = 'manyforge_perception_provider'
        request.plugin_name = 'manyforge::perception::InstancePoseSequencer'
        request.node_name = 'instance_pose_sequencer'
        request.node_namespace = namespace
        values = {**cls.provider_values, **(overrides or {})}
        for name in missing:
            del values[name]
        request.parameters = [Parameter(name, value=value).to_parameter_msg()
                              for name, value in values.items()]
        return cls.wait(cls.loader.call_async(request))

    @classmethod
    def unload_provider(cls, unique_id):
        request = UnloadNode.Request()
        request.unique_id = unique_id
        response = cls.wait(cls.unloader.call_async(request))
        if not response.success:
            raise AssertionError(response.error_message)

    @classmethod
    def tearDownClass(cls):
        cls.action.destroy()
        cls.node.destroy_node()
        rclpy.shutdown()

    @staticmethod
    def list_nodes(_, response):
        response.full_node_names = [
            '/encoder/resize_node', '/encoder/image_format_converter_node', '/encoder/crop_node',
            '/encoder/image_to_tensor', '/encoder/normalize_node',
            '/encoder/interleaved_to_planar_node',
            '/encoder/reshape_node', '/yolov8_tensor_rt', '/yolov8_decoder', '/refine', '/score',
            '/tracking_refine', '/foundationpose', '/tracking', '/instance_pose_sequencer']
        response.unique_ids = list(range(1, 16))
        return response

    @classmethod
    def until(cls, predicate, timeout=10.0):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise AssertionError('fixture condition did not arrive')
            rclpy.spin_once(cls.node, timeout_sec=0.02)

    @classmethod
    def wait(cls, future):
        cls.until(future.done)
        return future.result()

    def goal(self, name, preempt=False):
        message = EstimateInstances.Goal()
        message.request_id = name
        message.model_id = 'fixture'
        message.model_revision = 'fixture-revision'
        message.work_budget.sec = 1
        message.max_instances = 5
        message.min_detection_score = 0.5
        message.preempt = preempt
        return message

    def accept(self, name, preempt=False):
        feedback = []
        handle = self.wait(self.action.send_goal_async(
            self.goal(name, preempt), feedback_callback=lambda msg: feedback.append(msg.feedback)))
        self.assertTrue(handle.accepted)
        return handle, feedback

    def capture(self):
        type(self).capture_counter += 1
        rgb = Image()
        rgb.header.stamp.sec = 100 + self.capture_counter
        rgb.header.frame_id = 'camera_optical'
        rgb.width, rgb.height, rgb.step, rgb.encoding = 4, 2, 12, 'rgb8'
        rgb.data = array('B', [128] * 24)
        depth = Image()
        depth.header = rgb.header
        depth.width, depth.height, depth.step, depth.encoding = 4, 2, 16, '32FC1'
        depth.data = array('B', struct.pack('<8f', *([1.0] * 8)))
        camera = CameraInfo()
        camera.header = rgb.header
        camera.width, camera.height = 4, 2
        camera.k = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
        old_count = len(self.native_requests)
        self.rgb_pub.publish(rgb)
        self.depth_pub.publish(depth)
        self.camera_pub.publish(camera)
        self.until(lambda: len(self.native_requests) > old_count)
        return rgb.header, self.native_requests[-1]

    def empty(self, native, frame=None):
        message = Detection2DArray()
        message.header = native.header
        if frame is not None:
            message.header.frame_id = frame
        self.native_pub.publish(message)

    def test_empty_feedback_and_real_capture_metadata(self):
        handle, feedback = self.accept('valid-empty')
        future = handle.get_result_async()
        source, native = self.capture()
        self.assertNotEqual(source.stamp, native.header.stamp)
        self.empty(native)
        terminal = self.wait(future)
        self.assertEqual(terminal.status, GoalStatus.STATUS_SUCCEEDED)
        self.assertEqual(terminal.result.outcome, EstimateInstances.Result.VALID_EMPTY)
        self.assertEqual(terminal.result.capture, source)
        self.until(lambda: bool(feedback))
        self.assertEqual(feedback[0].kind, EstimateInstances.Feedback.KIND_DETECTIONS)
        self.assertEqual(feedback[0].selected_total, 0)

    def test_timeout_allows_immediate_next(self):
        handle, _ = self.accept('no-capture')
        terminal = self.wait(handle.get_result_async())
        self.assertEqual(terminal.status, GoalStatus.STATUS_ABORTED)
        self.assertEqual(terminal.result.reason, 'no_fresh_capture')
        next_handle, _ = self.accept('after-timeout')
        _, native = self.capture()
        self.empty(native)
        self.assertEqual(self.wait(next_handle.get_result_async()).result.outcome,
                         EstimateInstances.Result.VALID_EMPTY)

    def test_client_cancel_and_late_output_do_not_retire_next(self):
        handle, _ = self.accept('client-cancel')
        old_result = handle.get_result_async()
        _, old_native = self.capture()
        self.assertTrue(self.wait(handle.cancel_goal_async()).goals_canceling)
        terminal = self.wait(old_result)
        self.assertEqual(terminal.status, GoalStatus.STATUS_CANCELED)
        self.assertEqual(terminal.result.outcome, EstimateInstances.Result.CANCELLED)
        next_handle, _ = self.accept('after-cancel')
        next_result = next_handle.get_result_async()
        _, current = self.capture()
        self.assertNotEqual(current.header.stamp, old_native.header.stamp)
        self.empty(old_native)
        self.empty(current)
        terminal = self.wait(next_result)
        self.assertEqual(terminal.result.request_id, 'after-cancel')
        self.assertEqual(terminal.result.outcome, EstimateInstances.Result.VALID_EMPTY)
        self.assertGreaterEqual(terminal.result.timing.late_outputs, 1)

    def test_server_preemption_and_current_malformed_native(self):
        old, _ = self.accept('server-preempt')
        old_result = old.get_result_async()
        self.capture()
        current, _ = self.accept('preempting', preempt=True)
        terminal = self.wait(old_result)
        self.assertEqual(terminal.status, GoalStatus.STATUS_ABORTED)
        self.assertEqual(terminal.result.outcome, EstimateInstances.Result.CANCELLED)
        _, native = self.capture()
        self.empty(native, frame='wrong_frame')
        terminal = self.wait(current.get_result_async())
        self.assertEqual(terminal.status, GoalStatus.STATUS_ABORTED)
        self.assertEqual(terminal.result.reason, 'invalid_native_detection')

    def test_integer_required_numbers_construct_real_wrapper(self):
        response = self.load_provider('/integer_fixture', {'prior_center_px': 4})
        self.assertTrue(response.success, response.error_message)
        action = ActionClient(self.node, EstimateInstances, '/integer_fixture/estimate_instances')
        try:
            self.until(action.server_is_ready)
        finally:
            action.destroy()
            self.unload_provider(response.unique_id)

    def test_required_numbers_refuse_non_numeric_absent_and_invalid_range(self):
        for value in (True, '4', -1, float('inf')):
            with self.subTest(value=value):
                response = self.load_provider(
                    '/invalid_number_fixture', {'prior_center_px': value})
                self.assertFalse(response.success)
        response = self.load_provider('/missing_number_fixture', missing=('prior_center_px',))
        self.assertFalse(response.success)

    def test_construction_mismatch_stays_unavailable_without_retry(self, proc_output):
        requests = []
        valid_reply = [False]

        def construction(request, response):
            requests.append(request)
            response = self.list_nodes(request, response)
            if not valid_reply[0]:
                response.full_node_names[0] = '/wrong_encoder'
            return response

        service = self.node.create_service(
            ListNodes, '/mismatch_construction/_container/list_nodes', construction)
        response = self.load_provider(
            '/mismatch_fixture', {'component_container': '/mismatch_construction'})
        self.assertTrue(response.success, response.error_message)
        action = ActionClient(self.node, EstimateInstances, '/mismatch_fixture/estimate_instances')
        try:
            self.until(lambda: len(requests) == 1)
            proc_output.assertWaitFor(
                'Provider startup unavailable: native composition differs', timeout=10)
            # A subsequent corrected reply could enable the action if a query
            # were retried. Observe the real callback for ten timer periods.
            valid_reply[0] = True
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                rclpy.spin_once(self.node, timeout_sec=0.02)
                self.assertFalse(action.server_is_ready())
            self.assertEqual(len(requests), 1)
        finally:
            action.destroy()
            self.unload_provider(response.unique_id)
            self.node.destroy_service(service)

    def test_launch_shares_node_specific_asset_symmetry(self):
        launch_path = Path(__file__).resolve().parents[1] / 'launch/phase1_orin.launch.py'
        spec = importlib.util.spec_from_file_location('phase1_launch_fixture', launch_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            settings = Path(directory) / 'fixture.yaml'
            settings.write_text(
                '/**:\n  ros__parameters:\n    asset_symmetry_axes: [x_full]\n'
                'instance_pose_sequencer:\n  ros__parameters:\n'
                '    asset_symmetry_axes: [y_full]\n')
            context = LaunchContext()
            context.launch_configurations.update({
                'settings': str(settings), 'yolov8_engine': 'fixture-yolo',
                'refine_engine': 'fixture-refine', 'score_engine': 'fixture-score',
                'mesh': 'fixture-mesh', 'rgb_topic': '/image_rect',
                'depth_topic': '/depth', 'camera_info_topic': '/camera_info'})
            # Exercise standard ROS file resolution and the actual assembly,
            # replacing description constructors only; no NVIDIA is loaded.
            with (
                patch.object(module, 'ComposableNode', side_effect=lambda **args: args),
                patch.object(module, 'ComposableNodeContainer', side_effect=lambda **args: args)
            ):
                container = module.assemble(context)[0]
            nodes = {node['name']: node for node in container['composable_node_descriptions']}
            native_axes = nodes['foundationpose']['parameters'][0]['symmetry_axes']
            provider_axes = nodes['instance_pose_sequencer']['parameters'][1][
                'asset_symmetry_axes']
            self.assertEqual(tuple(native_axes), ('y_full',))
            self.assertEqual(tuple(provider_axes), tuple(native_axes))
