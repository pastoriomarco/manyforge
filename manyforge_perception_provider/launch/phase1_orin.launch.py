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
One constructed graph; request-driven admissions, no continuous tracking.

Encoder topology/parameters follow NVIDIA isaac_ros_dnn_image_encoder5.0.
This owns only assembly of existing plugins; no NVIDIA code/model modification.
The sequencer performs one existing ListNodes construction read before exposing
its action. Node names establish construction, not engine execution readiness.
"""

from pathlib import Path

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import ComposableNodeContainer
from launch_ros.descriptions import ComposableNode
from launch_ros.utilities import to_parameters_list


def assemble(context):
    """Keep all known nodes in a single standard sequential load list."""
    def value(name):
        return LaunchConfiguration(name).perform(context)

    # Resolve the same node-specific/wildcard settings as standard LoadNode.
    # Pin the required asset value for both consumers, even if the file changes
    # between assembly and component loading; no native symmetry default.
    settings = to_parameters_list(
        context, 'instance_pose_sequencer', '', [Path(value('settings'))])
    axes = next((setting.value for setting in settings
                 if setting.name == 'asset_symmetry_axes'), None)
    if axes is None:
        raise ValueError('asset_symmetry_axes is required in the provider settings')

    def node(name, package, plugin, parameters, remappings, namespace=''):
        return ComposableNode(
            name=name, namespace=namespace, package=package, plugin=plugin,
            parameters=[parameters], remappings=remappings,
            extra_arguments=[{'use_intra_process_comms': True}])

    image = 'nvidia::isaac_ros::image_proc::'
    tensor = 'nvidia::isaac_ros::dnn_inference::'
    encoder = [
        node('resize_node', 'isaac_ros_image_proc', image + 'ResizeNode',
             {'input_qos': 'DEFAULT', 'output_width': 640, 'output_height': 640,
              'num_blocks': 40, 'keep_aspect_ratio': True,
              'encoding_desired': 'rgb8', 'interp_type': 'linear'},
             [('image', '/native/detect/image'),
              ('camera_info', '/native/detect/camera_info')], 'encoder'),
        node('image_format_converter_node', 'isaac_ros_image_proc',
             image + 'ImageFormatConverterNode',
             {'image_width': 640, 'image_height': 640, 'encoding_desired': 'rgb8'},
             [('image_raw', 'resize/image'), ('image', 'converted/image')], 'encoder'),
        node('crop_node', 'isaac_ros_image_proc', image + 'CropNode',
             {'input_width': 640, 'input_height': 640,
              'crop_width': 640, 'crop_height': 640, 'num_blocks': 40,
              'crop_mode': 'CENTER', 'roi_top_left_x': 0, 'roi_top_left_y': 0},
             [('image', 'converted/image'), ('camera_info', 'resize/camera_info')], 'encoder'),
        node('image_to_tensor', 'isaac_ros_tensor_proc', tensor + 'ImageToTensorNode',
             {'scale': True, 'tensor_name': 'image'},
             [('image', 'crop/image'), ('tensor', 'image_tensor')], 'encoder'),
        node('normalize_node', 'isaac_ros_tensor_proc', tensor + 'ImageTensorNormalizeNode',
             {'mean': [0.0, 0.0, 0.0], 'stddev': [1.0, 1.0, 1.0],
              'input_tensor_name': 'image', 'output_tensor_name': 'image'},
             [('tensor', 'image_tensor')], 'encoder'),
        node('interleaved_to_planar_node', 'isaac_ros_tensor_proc',
             tensor + 'InterleavedToPlanarNode',
             {'input_tensor_shape': [640, 640, 3], 'num_blocks': 40},
             [('interleaved_tensor', 'normalized_tensor')], 'encoder'),
        node('reshape_node', 'isaac_ros_tensor_proc', tensor + 'ReshapeNode',
             {'output_qos': 'DEFAULT', 'output_tensor_name': 'input_tensor',
              'input_tensor_layout': 'CHW', 'output_tensor_layout': 'NCHW',
              'input_tensor_shape': [3, 640, 640], 'output_tensor_shape': [1, 3, 640, 640],
              'num_blocks': 40},
             [('tensor', 'planar_tensor'), ('reshaped_tensor', '/yolov8/tensor')], 'encoder')]

    def trt(name, plan, prefix, outputs, bindings, batch):
        return node(name, 'isaac_ros_tensor_rt', tensor + 'TensorRTNode',
                    {'engine_file_path': plan,
                     'input_tensor_names': ['input_tensor1', 'input_tensor2'],
                     'input_binding_names': ['input1', 'input2'], 'output_tensor_names': outputs,
                     'output_binding_names': bindings, 'max_batch_size': batch,
                     'force_engine_update': False, 'verbose': False},
                    [('tensor_pub', prefix + '/tensor_pub'),
                     ('tensor_sub', prefix + '/tensor_sub')])

    nodes = encoder + [
        node('yolov8_tensor_rt', 'isaac_ros_tensor_rt', tensor + 'TensorRTNode',
             {'engine_file_path': value('yolov8_engine'), 'input_tensor_names': ['input_tensor'],
              'input_binding_names': ['images'], 'output_tensor_names': ['output_tensor'],
              'output_binding_names': ['output0'], 'force_engine_update': False,
              'verbose': False},
             [('tensor_sub', '/yolov8/tensor'), ('tensor_pub', '/yolov8/output')]),
        node('yolov8_decoder', 'isaac_ros_yolov8',
             'nvidia::isaac_ros::yolov8::YoloV8DecoderNode',
             {'confidence_threshold': 0.25, 'nms_threshold': 0.45, 'num_classes': 1},
             [('tensor_sub', '/yolov8/output'), ('detections_output', '/native/detections')]),
        trt('refine', value('refine_engine'), 'refine',
            ['output_tensor1', 'output_tensor2'], ['output1', 'output2'], 42),
        trt('score', value('score_engine'), 'score', ['output_tensor'], ['output1'], 252),
        trt('tracking_refine', value('refine_engine'), 'tracking_refine',
            ['output_tensor1', 'output_tensor2'], ['output1', 'output2'], 1),
        node('foundationpose', 'isaac_ros_foundationpose',
             'nvidia::isaac_ros::foundationpose::FoundationPoseNode',
             {'mesh_file_path': value('mesh'), 'symmetry_axes': axes,
              'refine_input_tensor_names': ['input_tensor1', 'input_tensor2'],
              'score_input_tensor_names': ['input_tensor1', 'input_tensor2']},
             [('pose_estimation/image', '/native/full/image'),
              ('pose_estimation/depth_image', '/native/full/depth'),
              ('pose_estimation/camera_info', '/native/full/camera_info'),
              ('pose_estimation/segmentation', '/native/full/mask'),
              ('pose_estimation/output', '/native/full/pose'),
              ('pose_estimation/pose_matrix_output', '/native/full/matrix')]),
        node('tracking', 'isaac_ros_foundationpose',
             'nvidia::isaac_ros::foundationpose::FoundationPoseTrackingNode',
             {'mesh_file_path': value('mesh'),
              'refine_input_tensor_names': ['input_tensor1', 'input_tensor2']},
             [('tracking/image', '/native/refine/image'),
              ('tracking/depth_image', '/native/refine/depth'),
              ('tracking/camera_info', '/native/refine/camera_info'),
              ('tracking/pose_input', '/native/refine/seed'),
              ('tracking/output', '/native/refine/pose'),
              ('tracking/pose_matrix_output', '/native/refine/matrix')]),
        ComposableNode(name='instance_pose_sequencer', namespace='',
                       package='manyforge_perception_provider',
                       plugin='manyforge::perception::InstancePoseSequencer',
                       parameters=[value('settings'), {'asset_symmetry_axes': axes}],
                       remappings=[('image_rect', value('rgb_topic')),
                                   ('depth', value('depth_topic')),
                                   ('camera_info', value('camera_info_topic'))],
                       extra_arguments=[{'use_intra_process_comms': True}])]
    return [ComposableNodeContainer(
        name='phase1_components', namespace='', package='rclcpp_components',
        executable='component_container_mt', composable_node_descriptions=nodes,
        output='screen')]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument('settings', description='Explicit reviewed provider parameter file'),
        DeclareLaunchArgument('yolov8_engine', description='Existing recorded Orin YOLO plan'),
        DeclareLaunchArgument('refine_engine', description='Existing recorded Orin refine plan'),
        DeclareLaunchArgument('score_engine', description='Existing recorded Orin score plan'),
        DeclareLaunchArgument('mesh', description='Existing recorded trocar_short.obj mesh'),
        DeclareLaunchArgument('rgb_topic', default_value='/image_rect'),
        DeclareLaunchArgument('depth_topic', default_value='/depth'),
        DeclareLaunchArgument('camera_info_topic', default_value='/camera_info'),
        OpaqueFunction(function=assemble)])
