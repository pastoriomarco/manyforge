import { describe, expect, it, vi } from 'vitest';
import * as THREE from 'three';

import type {
  LogicalSceneProjection,
  SceneRuntimePoseSource,
  SceneRuntimeProjection,
  SceneUrdfJoint,
  SceneUrdfManifest,
  SourceModelProjection,
} from '../types/api';
import type {
  CapabilityCommandPreviewReceipt,
  ExecutionOwnerStatus,
  ManualControlMotionDescriptor,
  MotionPreviewReceipt,
} from '../types/robotControl';
import type { ExecutionObservationFrame } from '../types/viewerObservation';
import {
  previewBodyIdentity,
  previewContactPresentation,
  sampledPreviewRobotPresentation,
  attachedScenePresentation,
  candidateGripperPresentation,
  candidateRobotPresentation,
  candidateToolPresentation,
  candidateObservationEpoch,
  createExecutionRobotPresentationReader,
  createMeasuredComparisonPresentationReader,
  createMeasuredOverlayFrameWarningsReader,
  executionObservationRobotPresentation,
  executionRobotPresentation,
  measuredLinkTransforms,
  runtimeScenePresentation,
} from './runtimeScenePresentation';
import type {
  MeasuredRobotPresentation,
  RuntimeScenePresentation,
} from './runtimeScenePresentation';

function executionStatus(): ExecutionOwnerStatus {
  const status = {
    ownerEpoch: 'owner-1',
    cellId: 'cell-1',
    graphRevision: 'graph-1',
    retainedBundleId: 'bundle-1',
    directControl: {
      controllableComponentId: 'robot-1', commandTargetId: 'arm', operationCapabilityId: 'execution.trajectory.execute',
      targetDescriptor: {
        controllableComponentId: 'robot-1',
        commandTargetId: 'arm',
        joints: [{ sourceJointName: 'source-joint', compiledJointName: 'joint-1' }],
      },
    },
    observedMotion: {
      controllableComponentId: 'robot-1',
      commandTargetId: 'arm',
      sourceEpoch: 'native-1',
      sequence: 4,
      sourceJointNames: ['source-joint'],
      positionsRad: [Math.PI / 2],
      complete: true,
    },
  } as unknown as ExecutionOwnerStatus;
  // The IF.2 inventory: one component owning the arm target.
  status.components = [{
    controllableComponentId: 'robot-1',
    commandEligible: true,
    unavailableReason: null,
    unsupportedTargets: [],
  }];
  status.directControlTargets = [status.directControl!];
  status.directControlGripperTargets = [];
  status.observedMotions = [status.observedMotion!];
  status.observedGrippers = [];
  return status;
}

function joint(
  name: string,
  type: string,
  parent: string,
  child: string,
  position: [number, number, number],
): SceneUrdfJoint {
  return {
    name,
    type,
    axis: [0, 0, 1],
    parent_link: parent,
    child_link: child,
    origin: {
      relative_to: parent,
      position_m: position,
      orientation_rpy_rad: [0, 0, 0],
      orientation_xyzw: [0, 0, 0, 1],
    },
    coordinate_unit: type === 'fixed' ? null : 'rad',
    neutral_position: type === 'fixed' ? null : 0,
    limit_lower: null,
    limit_upper: null,
    effort_limit: null,
    velocity_limit: null,
    mimic: null,
    is_mimic_master: false,
  };
}

function manifest(): SceneUrdfManifest {
  return {
    cell_id: 'cell-1',
    graph_revision: 'graph-1',
    retained_bundle_id: 'bundle-1',
    viewer_model_revision: 'viewer-1',
    model_key: 'model-1',
    root_link: 'base',
    projection: {
      kind: 'retained_neutral',
      source: 'active_retained_viewer_model',
      root_frame: 'base',
      position_unit: 'm',
      angle_unit: 'rad',
      quaternion_order: 'xyzw',
      joint_coordinate_policy: 'zero_independent_with_retained_mimic',
      operational_validity: 'not_asserted',
      runtime_state_used: false,
    },
    links: ['base', 'link-1', 'tip'].map((name) => ({
      name,
      neutral_transform: {
        relative_to: 'base',
        position_m: [0, 0, 0],
        orientation_xyzw: [0, 0, 0, 1],
      },
      visuals: [],
      collisions: [],
    })),
    joints: [
      joint('joint-1', 'revolute', 'base', 'link-1', [1, 0, 0]),
      joint('fixed-tip', 'fixed', 'link-1', 'tip', [1, 0, 0]),
    ],
  };
}

function sourceModel(): SourceModelProjection {
  return {
    cell_id: 'cell-1',
    graph_revision: 'graph-1',
    retained_bundle_id: 'bundle-1',
    components: [{ component_id: 'robot-1', mounted_on: null }],
    occurrences: [{
      component_id: 'robot-1',
      model_instance_id: 'model-instance-1',
      model_revision: 'model-revision-1',
      coordinates: [
        { source_joint_name: 'source-joint', viewer_joint_name: 'joint-1', coordinate_kind: 'angular', unit: 'radians' },
      ],
      links: [
        { source_link_name: 'base', viewer_link_name: 'base' },
        { source_link_name: 'link-1', viewer_link_name: 'link-1' },
        { source_link_name: 'tip', viewer_link_name: 'tip' },
      ],
    }],
    targets: [
      {
        component_id: 'robot-1',
        target_id: 'arm',
        frames: [{
          frame_kind: 'component_model_frame',
          frame_role_id: 'tcp',
          model_instance_id: 'model-instance-1',
          source_link_name: 'tip',
        }],
        model_groups: [{
          commandable_source_joint_names: ['source-joint'],
          model_instance_id: 'model-instance-1',
        }],
      },
      {
        component_id: 'robot-1',
        target_id: 'gripper',
        frames: [{
          frame_kind: 'component_model_frame',
          frame_role_id: 'tcp',
          model_instance_id: 'model-instance-1',
          source_link_name: 'tip',
        }, {
          frame_kind: 'component_model_frame',
          frame_role_id: 'gripper_tcp',
          model_instance_id: 'model-instance-1',
          source_link_name: 'tip',
        }],
        model_groups: [{
          commandable_source_joint_names: [],
        }],
      },
    ],
  } as unknown as SourceModelProjection;
}

function motionDescriptor(): ManualControlMotionDescriptor {
  return {
    schemaVersion: 'manyforge.manual_control_motion_descriptor.v8',
    ordinaryJerk: null,
    controllableComponentId: 'robot-1',
    endEffectorComponentId: null,
    commandTargetId: 'arm',
    tcps: [{
      entry: { componentId: 'robot-1', targetId: 'arm', frameComponentId: 'robot-1', frameRoleId: 'tcp' },
      controllableComponentId: 'robot-1',
      commandTargetId: 'arm',
      frameRoleId: 'tcp',
      qualifiedName: 'robot-1.tcp',
      compiledFrameName: 'tip',
      modelInstanceId: 'model-instance-1',
      modelRevision: 'model-revision-1',
      isDefault: true,
    }],
    frames: [{
      frameRoleId: 'tcp',
      compiledFrameName: 'tip',
      modelInstanceId: 'model-instance-1',
      modelRevision: 'model-revision-1',
    }],
    joints: [{
      sourceJointName: 'source-joint',
      compiledJointName: 'joint-1',
      unit: 'rad',
    }],
    operationalJointLimits: [],
    operationalLimit: null,
  } as unknown as ManualControlMotionDescriptor;
}

function attachedLogicalScene(): LogicalSceneProjection {
  const scene = logicalScene();
  const snapshot = scene.snapshot;
  if (snapshot === null) throw new Error('fixture must have a snapshot');
  snapshot.objects[0].placement = {
    kind: 'carried',
    carried_object_attachment_id: 'attachment-1',
    carried_object_attachment_revision: 'attachment-revision-1',
  };
  snapshot.carried_object_attachments = [{
    carried_object_attachment_id: 'attachment-1',
    carried_object_attachment_revision: 'attachment-revision-1',
    scene_resource_id: 'part',
    controllable_component_id: 'robot-1',
    command_target_id: 'gripper',
    carrier_frame_ref: {
      kind: 'component',
      controllable_component_id: 'robot-1',
      model_instance_id: 'model-instance-1',
      frame_role_id: 'gripper_tcp',
    },
    pose_in_carrier_frame: {
      position_m: { x: 0.01, y: -0.02, z: 0.03 },
      orientation_xyzw: [0, 0, 0, 1],
    },
    touch_body_roles: ['tool'],
  }];
  return scene;
}

function logicalScene(): LogicalSceneProjection {
  return {
    api_version: 'manyforge.logical_scene_source.v1',
    access: {
      active_projection: {
        retained_bundle_id: 'bundle-1',
        graph_revision: 'graph-1',
        scene_revision: {
          scene_id: 'scene-1',
          revision_id: 1,
          content_hash: 'hash-1',
        },
      },
      phase: 'finalized',
      projection_leases_open: true,
      scene_writes_open: true,
    },
    snapshot: {
      carried_object_attachments: [],
      cell_id: 'cell-1',
      configured_collision_policy: {},
      objects: [{
        scene_resource_id: 'part',
        collision_shapes: [{ type: 'sphere', radius_m: 0.02 }],
        placement: {
          kind: 'cell_fixed',
          cell_id: 'cell-1',
          pose_in_cell: {
            position_m: { x: 0, y: 0, z: 0 },
            orientation_xyzw: [0, 0, 0, 1],
          },
        },
        provider_evidence: null,
        support_contacts: [],
      }],
      policy_epoch: 1,
      revision: { scene_id: 'scene-1', revision_id: 1, content_hash: 'hash-1' },
      topology_epoch: 1,
      transaction_sequence: 1,
    },
  };
}

function baseSource(
  payload: SceneRuntimePoseSource['payload'],
  overrides: Partial<SceneRuntimePoseSource> = {},
): SceneRuntimePoseSource {
  return {
    buffered: false,
    eligible: false,
    expires_at_ns: 2_000_000_000,
    fresh: true,
    object_id: 'robot-1',
    observed_at_ns: 1_000_000_000,
    payload,
    policy_id: 'viewer.measured_robot',
    payload_quality: 'good',
    source_epoch: 'epoch-1',
    source_epoch_order: 1,
    source_id: 'observer-1',
    source_sequence: 1,
    ...overrides,
  } as SceneRuntimePoseSource;
}

function viewerFrameWithSources(
  sources: SceneRuntimePoseSource[],
  frame?: ExecutionObservationFrame | null,
): ExecutionObservationFrame {
  return {
    ...(frame ?? {
      schemaVersion: 'manyforge.execution_observation_frame.v4', ownerEpoch: 'owner-1',
      cellId: 'cell-1', retainedBundleId: 'bundle-1', graphRevision: 'graph-1',
      sourceEpoch: 'native-1', sourceSequence: 1, receivedAtMonotonicUs: 123,
      targets: [], measuredTargets: [],
    }),
    outsidePoseSources: sources.map((source) => ({
      source, ageAtDeliveryNs: 0, freshnessBudgetNs: 1_000_000_000,
    })),
  };
}

function posePayload(kind: 'simulation_entity_pose' | 'detection3d_pose') {
  const common = {
    api_version: 'manyforge.ros_pose_observation.v2' as const,
    kind,
    pose: {
      frame_id: 'cell',
      frame_kind: 'cell_frame' as const,
      orientation_xyzw: [0, 0, 0, 1] as [number, number, number, number],
      position_m: [0.4, 0.2, 0.1] as [number, number, number],
      source_stamp_ns: 12000000000,
    },
  };
  return kind === 'simulation_entity_pose'
    ? { ...common, kind, source: { entity: '/part' } }
    : {
        ...common,
        kind,
        source: {
          class_id: '',
          confidence: 0.9,
          detection_id: '',
          max_position_variance_m2: 0.001,
          topic: '/output',
        },
      };
}

function attachedPosePayload() {
  return {
    api_version: 'manyforge.ros_pose_observation.v2' as const,
    kind: 'simulation_entity_pose_with_references.v1' as const,
    pose: {
      frame_id: 'cell',
      frame_kind: 'cell_frame' as const,
      orientation_xyzw: [0, 0, 0, 1] as [number, number, number, number],
      position_m: [0.4, 0.2, 0.1] as [number, number, number],
      source_stamp_ns: 12_000_000_000,
    },
    references: [{
      controllable_component_id: 'robot-1',
      command_target_id: 'gripper',
      gripper_link: 'tool',
      entity: '/World/robot/tool',
      link: 'tool_base_link',
      source_header_frame_id: 'tool_header',
      coherent: true,
      pose_in_reference: {
        orientation_xyzw: [0, 0, 0, 1] as [number, number, number, number],
        position_m: [0.02, -0.01, 0.25] as [number, number, number],
      },
    }],
    source: { entity: '/part' },
  };
}

function runtime(sources: SceneRuntimePoseSource[]): SceneRuntimeProjection {
  return {
    api_version: 'manyforge.scene_runtime_source.v1',
    cell_id: 'cell-1',
    state: {
      api_version: 'manyforge.scene_runtime_state.v2',
      active_replay_id: null,
      counters: {},
      deferred: false,
      error: null,
      initialization_generation: 1,
      initialization_token: null,
      pose_sources: sources,
      reason: 'ready',
      runtime_identity: {
        deployment_generation: 1,
        graph_revision: 'graph-1',
        program_generation: 1,
        retained_bundle_id: 'bundle-1',
        runtime_generation: 1,
      },
      started_at_ns: 1,
      state: 'open',
      updated_at_ns: 2,
    },
  };
}

describe('runtime scene presentation', () => {
  it('projects exact native Execution-owner joints without inventing time', () => {
    const presentation = executionRobotPresentation(
      executionStatus(),
      sourceModel(),
      manifest(),
    );

    expect(presentation).toMatchObject({
      objectId: 'robot-1',
      sourceId: 'execution:arm',
      sourceEpoch: 'native-1',
      sourceSequence: 4,
      observedAtNs: null,
    });
    expect(presentation?.linkTransforms.tip.position_m[0]).toBeCloseTo(1);
    expect(presentation?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);

    const mismatched = executionStatus();
    mismatched.graphRevision = 'another-graph';
    expect(executionRobotPresentation(mismatched, sourceModel(), manifest())).toBeNull();
  });

  it('skips a model-free I/O device component (not drawn in the 3D scene)', () => {
    const status = executionStatus();
    status.components = [...status.components, {
      controllableComponentId: 'station_io',
      commandEligible: true,
      unavailableReason: null,
      unsupportedTargets: [],
    }];
    expect(executionRobotPresentation(status, sourceModel(), manifest()))
      .toEqual(executionRobotPresentation(executionStatus(), sourceModel(), manifest()));
    expect(executionRobotPresentation(status, sourceModel(), manifest())?.objectId).toBe('robot-1');
  });

  it('projects every observed robot instead of fabricating unselected robots at neutral', () => {
    const status = executionStatus();
    const secondDescriptor = {
      ...status.directControl!.targetDescriptor,
      controllableComponentId: 'robot-2',
      commandTargetId: 'arm-2',
      joints: [{
        ...status.directControl!.targetDescriptor.joints[0],
        sourceJointName: 'source-joint-2',
        compiledJointName: 'joint-2',
      }],
    };
    status.directControlTargets = [status.directControl!, {
      ...status.directControl!,
      controllableComponentId: 'robot-2',
      commandTargetId: 'arm-2',
      targetDescriptor: secondDescriptor,
    }];
    status.observedMotions = [status.observedMotion!, {
      ...status.observedMotion!,
      controllableComponentId: 'robot-2',
      commandTargetId: 'arm-2',
      sourceJointNames: ['source-joint-2'],
      positionsRad: [-Math.PI / 2],
    }];
    const model = sourceModel();
    model.occurrences.push({
      ...model.occurrences[0],
      component_id: 'robot-2',
      model_instance_id: 'model-instance-2',
      coordinates: [{
        ...model.occurrences[0].coordinates[0],
        source_joint_name: 'source-joint-2',
        viewer_joint_name: 'joint-2',
      }],
    });
    const cellManifest = manifest();
    cellManifest.links.push(
      { ...cellManifest.links[1], name: 'link-2' },
      { ...cellManifest.links[2], name: 'tip-2' },
    );
    cellManifest.joints.push(
      joint('joint-2', 'revolute', 'base', 'link-2', [10, 0, 0]),
      joint('fixed-tip-2', 'fixed', 'link-2', 'tip-2', [1, 0, 0]),
    );

    const presentation = executionRobotPresentation(status, model, cellManifest);

    expect(presentation?.linkTransforms.tip.position_m).toEqual(expect.arrayContaining([
      expect.closeTo(1), expect.closeTo(1), expect.closeTo(0),
    ]));
    expect(presentation?.linkTransforms['tip-2'].position_m).toEqual(expect.arrayContaining([
      expect.closeTo(10), expect.closeTo(-1), expect.closeTo(0),
    ]));
  });

  it('projects and caches compact owner observations once per frame', () => {
    let frame: ExecutionObservationFrame | null = {
      schemaVersion: 'manyforge.execution_observation_frame.v4',
      ownerEpoch: 'owner-1',
      cellId: 'cell-1',
      retainedBundleId: 'bundle-1',
      graphRevision: 'graph-1',
      sourceEpoch: 'native-1',
      sourceSequence: 5,
      receivedAtMonotonicUs: 123,
      targets: [{
        controllableComponentId: 'robot-1', commandTargetId: 'arm', operationCapabilityId: 'execution.trajectory.execute',
        sourceEpoch: 'native-1', sourceSequence: 5,
        sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'],
        coordinateKinds: ['angular'], positionsSi: [Math.PI / 2],
      }],
      measuredTargets: [],
    };
    const projected = executionObservationRobotPresentation(
      frame, sourceModel(), manifest(), motionDescriptor(), 2_000_000,
    );
    expect(projected).toMatchObject({
      objectId: 'robot-1',
      sourceSequence: 5,
      observedAtNs: 2_000_000,
    });
    expect(projected?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    // The viewer remains live even when Robot-control has no selected target
    // (for example immediately after Apply or while the inspector is closed).
    expect(executionObservationRobotPresentation(
      frame, sourceModel(), manifest(), null, 2_000_000,
    )).toMatchObject({ objectId: 'robot-1', sourceSequence: 5 });

    // Source-model units are retained-map enums, not URDF-manifest labels.
    const wrongUnits = sourceModel();
    wrongUnits.occurrences[0].coordinates[0].unit = 'rad';
    expect(executionObservationRobotPresentation(
      frame, wrongUnits, manifest(), motionDescriptor(), null,
    )).toBeNull();

    let reads = 0;
    const rejected: ExecutionObservationFrame[] = [];
    const reader = createExecutionRobotPresentationReader({
      getLatestExecutionObservation: () => { reads += 1; return frame; },
      subscribeExecutionObservation: () => () => undefined,
      markExecutionObservationApplied: () => undefined,
      rejectExecutionObservation: (value) => { rejected.push(value); },
    }, sourceModel(), manifest(), motionDescriptor());
    const first = reader.readMeasuredRobotPresentation();
    const second = reader.readMeasuredRobotPresentation();
    expect(first).toBe(second);
    expect(first?.observedAtNs).toBe('123000');
    expect(reads).toBe(2);

    const referenceEpoch = candidateObservationEpoch(frame, executionStatus());
    expect(referenceEpoch).not.toBeNull();
    expect(candidateObservationEpoch(frame, { ...executionStatus(), ownerEpoch: 'owner-2' })).toBeNull();
    expect(candidateObservationEpoch({ ...frame, graphRevision: 'other' }, executionStatus())).toBeNull();
    expect(candidateObservationEpoch({ ...frame, sourceEpoch: 'restarted' }, executionStatus())).not.toBe(referenceEpoch);
    expect(candidateObservationEpoch({ ...frame, targets: [{ ...frame.targets[0], sourceEpoch: 'restarted-arm' }] }, executionStatus())).not.toBe(referenceEpoch);

    frame = { ...frame, sourceSequence: 6, receivedAtMonotonicUs: 9_007_199_254_741 };
    expect(candidateObservationEpoch(frame, executionStatus())).toBe(referenceEpoch);
    expect(reader.readMeasuredRobotPresentation()?.observedAtNs).toBe('9007199254741000');

    frame = { ...frame, targets: [{ ...frame.targets[0], compiledJointNames: ['fixed-tip'] }] };
    expect(reader.readMeasuredRobotPresentation()).toBeNull();
    expect(reader.readMeasuredRobotPresentation()).toBeNull();
    expect(rejected).toEqual([frame]);

    frame = { ...frame, graphRevision: 'crossed-graph' };
    expect(reader.readMeasuredRobotPresentation()).toBeNull();
    expect(rejected).toEqual([rejected[0], frame]);
  });

  it('projects measured rows through the solid robot path without crediting frames', () => {
    const commanded = {
      controllableComponentId: 'robot-1', commandTargetId: 'arm', operationCapabilityId: 'execution.trajectory.execute',
      sourceEpoch: 'native-1', sourceSequence: 5,
      sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'],
      coordinateKinds: ['angular' as const], positionsSi: [0],
    };
    let frame: ExecutionObservationFrame | null = {
      schemaVersion: 'manyforge.execution_observation_frame.v4',
      ownerEpoch: 'owner-1', cellId: 'cell-1', retainedBundleId: 'bundle-1', graphRevision: 'graph-1',
      sourceEpoch: 'native-1', sourceSequence: 5, receivedAtMonotonicUs: 123,
      targets: [commanded],
      measuredTargets: [{ ...commanded, sourceEpoch: 'measured-1', sourceSequence: 40, positionsSi: [Math.PI / 2] }],
    };
    const credits: unknown[] = [];
    const reader = createMeasuredComparisonPresentationReader({
      getLatestExecutionObservation: () => frame,
      subscribeExecutionObservation: () => () => undefined,
      markExecutionObservationApplied: (value) => { credits.push(value); },
      rejectExecutionObservation: (value) => { credits.push(value); },
    }, sourceModel(), manifest());
    const measured = reader.readMeasuredRobotPresentation();
    expect(measured).toMatchObject({
      objectId: 'robot-1', sourceId: 'measured:arm', sourceSequence: 5, observedAtNs: '123000',
    });
    expect(reader.readMeasuredRobotPresentation()).toBe(measured);
    // Same retained-model projection as the solid robot at that coordinate.
    expect(measured?.linkTransforms).toEqual(executionObservationRobotPresentation(
      { ...frame, targets: frame.measuredTargets, measuredTargets: [] },
      sourceModel(), manifest(), null, '123000',
    )?.linkTransforms);
    expect(measured?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    reader.markMeasuredRobotPresentationApplied(measured!);
    expect(credits).toEqual([]);

    frame = { ...frame, sourceSequence: 6, measuredTargets: [] };
    expect(reader.readMeasuredRobotPresentation()).toBeNull();
    frame = { ...frame, sourceSequence: 7, measuredTargets: [{ ...commanded, commandTargetId: 'other' }] };
    expect(reader.readMeasuredRobotPresentation()).toBeNull();
    expect(credits).toEqual([]);
  });

  it('moves model-backed gripper master and mimic fingers without an arm update', () => {
    const model = sourceModel();
    const geometry = manifest();
    geometry.links.push(...['finger-left', 'finger-right'].map((name) => ({ ...geometry.links[0], name })));
    geometry.joints.push(
      { ...joint('finger-master', 'prismatic', 'tip', 'finger-left', [0, 0, 0]),
        axis: [0, 1, 0], coordinate_unit: 'm', is_mimic_master: true },
      { ...joint('finger-mimic', 'prismatic', 'tip', 'finger-right', [0, 0, 0]),
        axis: [0, 1, 0], coordinate_unit: 'm',
        mimic: { master: 'finger-master', multiplier: -1, offset: 0 } },
    );
    model.occurrences.push({
      ...model.occurrences[0], model_instance_id: 'gripper-model',
      coordinates: [{ ...model.occurrences[0].coordinates[0],
        source_joint_name: 'finger', viewer_joint_name: 'finger-master', coordinate_kind: 'linear', unit: 'meters' }],
    });
    model.targets[1].model_groups = [{ ...model.targets[0].model_groups[0],
      model_instance_id: 'gripper-model', commandable_source_joint_names: ['finger'] }];
    const frame: ExecutionObservationFrame = {
      schemaVersion: 'manyforge.execution_observation_frame.v4', ownerEpoch: 'owner-1',
      cellId: 'cell-1', retainedBundleId: 'bundle-1', graphRevision: 'graph-1',
      sourceEpoch: 'render-1', sourceSequence: 10, receivedAtMonotonicUs: 123,
      targets: [
        { controllableComponentId: 'robot-1', commandTargetId: 'arm', operationCapabilityId: 'execution.trajectory.execute', sourceEpoch: 'arm-1', sourceSequence: 5,
          sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'], coordinateKinds: ['angular'], positionsSi: [0] },
        { controllableComponentId: 'robot-1', commandTargetId: 'gripper', operationCapabilityId: 'tool.gripper.command', sourceEpoch: 'gripper-1', sourceSequence: 8,
          sourceJointNames: ['finger'], compiledJointNames: ['finger-master'], coordinateKinds: ['linear'], positionsSi: [0.025] },
      ],
      measuredTargets: [],
    };
    const open = executionObservationRobotPresentation(frame, model, geometry, motionDescriptor(), null);
    const authoringStatus = executionStatus();
    authoringStatus.directControlGripper = {
      controllableComponentId: 'robot-1', commandTargetId: 'gripper', operationCapabilityId: 'tool.gripper.command', sourceKind: 'model_role', sourceCoordinateName: 'finger',
    } as ExecutionOwnerStatus['directControlGripper'];
    expect(candidateObservationEpoch(frame, authoringStatus)).not.toBeNull();
    expect(candidateObservationEpoch({ ...frame, targets: [frame.targets[0]] }, authoringStatus)).toBeNull();
    expect(open?.linkTransforms['finger-left'].position_m[1]).toBeCloseTo(0.025);
    expect(open?.linkTransforms['finger-right'].position_m[1]).toBeCloseTo(-0.025);
    frame.sourceSequence += 1;
    frame.targets[1] = { ...frame.targets[1], sourceSequence: 9, positionsSi: [0.005] };
    const closed = executionObservationRobotPresentation(frame, model, geometry, motionDescriptor(), null);
    expect(closed?.linkTransforms.tip).toEqual(open?.linkTransforms.tip);
    expect(closed?.linkTransforms['finger-left'].position_m[1]).toBeCloseTo(0.005);
    expect(closed?.linkTransforms['finger-right'].position_m[1]).toBeCloseTo(-0.005);
    model.occurrences[1].coordinates[0].unit = 'm';
    expect(executionObservationRobotPresentation(frame, model, geometry, motionDescriptor(), null)).toBeNull();
    model.occurrences[1].coordinates[0].unit = 'meters';
    frame.targets[1].coordinateKinds = ['angular'];
    expect(executionObservationRobotPresentation(frame, model, geometry, motionDescriptor(), null)).toBeNull();
    frame.targets[1].coordinateKinds = ['linear'];
    model.occurrences[1].model_instance_id = 'foreign-model';
    expect(executionObservationRobotPresentation(frame, model, geometry, motionDescriptor(), null)).toBeNull();
  });

  it('computes retained-model FK and exact measured object/TCP overlays', () => {
    const jointSource = baseSource({
      api_version: 'manyforge.ros_pose_observation.v2',
      joint_names: ['source-joint'],
      kind: 'joint_state',
      positions: [Math.PI / 2],
      source_stamp_ns: 11000000000,
      topic: '/isaac_joint_states',
    });
    const tcp = baseSource(posePayload('simulation_entity_pose'), {
      policy_id: 'viewer.measured_tcp',
      source_sequence: 2,
    });
    const object = baseSource(posePayload('simulation_entity_pose'), {
      eligible: true,
      object_id: 'part',
      policy_id: 'continuous_unattached',
      source_sequence: 3,
    });
    const unknown = baseSource({
      api_version: 'manyforge.ros_pose_observation.v2',
      kind: 'detection3d_pose',
      source_error: {
        code: 'detection_not_found',
        detail: 'no match',
        source_key: '/output',
      },
    }, {
      eligible: false,
      fresh: false,
      object_id: 'other',
      policy_id: 'before_planning',
      payload_quality: 'unknown',
      source_sequence: 4,
    });

    const presentation = runtimeScenePresentation(
      runtime([jointSource, tcp, object, unknown]),
      logicalScene(),
      sourceModel(),
      manifest(),
      viewerFrameWithSources([jointSource]),
    );

    expect(presentation.measuredRobot?.linkTransforms['link-1'].position_m)
      .toEqual([1, 0, 0]);
    expect(presentation.measuredRobot?.linkTransforms.tip.position_m[0])
      .toBeCloseTo(1);
    expect(presentation.measuredRobot?.linkTransforms.tip.position_m[1])
      .toBeCloseTo(1);
    expect(presentation.measuredRobot).toMatchObject({
      sourceEpoch: 'epoch-1',
      sourceSequence: 1,
      observedAtNs: 1_000_000_000,
    });
    expect(presentation.measuredTcp?.policyId).toBe('viewer.measured_tcp');
    expect(presentation.measuredTcp).toMatchObject({
      sourceEpoch: 'epoch-1',
      sourceSequence: 2,
      observedAtNs: 1_000_000_000,
    });
    expect(presentation.measuredObjects.map((item) => item.objectId))
      .toEqual(['part']);
    expect(presentation.measuredObjects[0]).toMatchObject({
      sourceEpoch: 'epoch-1',
      sourceSequence: 3,
      observedAtNs: 1_000_000_000,
    });
    expect(presentation.status).toMatchObject({
      total: 4,
      freshGood: 3,
      staleGood: 0,
      unknown: 1,
      buffered: 0,
    });
  });

  it('withholds every measured overlay when its good scene-runtime samples are stale', () => {
    const jointSource = baseSource({
      api_version: 'manyforge.ros_pose_observation.v2',
      joint_names: ['source-joint'],
      kind: 'joint_state',
      positions: [Math.PI / 2],
      source_stamp_ns: 11000000000,
      topic: '/isaac_joint_states',
    }, { fresh: false });
    const tcp = baseSource(posePayload('simulation_entity_pose'), {
      fresh: false,
      policy_id: 'viewer.measured_tcp',
      source_sequence: 2,
    });
    const object = baseSource(posePayload('simulation_entity_pose'), {
      eligible: true,
      fresh: false,
      object_id: 'part',
      policy_id: 'continuous_unattached',
      source_sequence: 3,
    });

    const presentation = runtimeScenePresentation(
      runtime([jointSource, tcp, object]),
      logicalScene(),
      sourceModel(),
      manifest(),
    );

    expect(presentation).toMatchObject({
      measuredRobot: null,
      measuredTcp: null,
      measuredObjects: [],
      status: { total: 3, freshGood: 0, staleGood: 3 },
    });
  });

  it('joins the exact carrier while retaining independent measured drift evidence', () => {
    const scene = attachedLogicalScene();
    const joined = attachedScenePresentation(
      scene,
      sourceModel(),
      manifest(),
    );
    expect(joined.diagnostics).toEqual([]);
    expect(joined.objects).toHaveLength(1);
    expect(joined.objects[0]).toMatchObject({
      attachmentId: 'attachment-1',
      attachmentRevision: 'attachment-revision-1',
      carrierComponentId: 'robot-1',
      commandTargetId: 'gripper',
      carrierFrameRoleId: 'gripper_tcp',
      carrierCompiledFrameName: 'tip',
      poseInCarrierFrame: {
        position_m: { x: 0.01, y: -0.02, z: 0.03 },
      },
    });

    const presentation = runtimeScenePresentation(
      runtime([baseSource(posePayload('simulation_entity_pose'), {
        eligible: true,
        object_id: 'part',
        policy_id: 'continuous_unattached',
      })]),
      scene,
      sourceModel(),
      manifest(),
    );
    expect(presentation.attachedObjects).toHaveLength(1);
    expect(presentation.measuredObjects).toHaveLength(1);
    expect(presentation.measuredObjects[0].objectId).toBe('part');
    expect(presentation.attachmentDiagnostics).toEqual([]);
  });

  it('joins attached pose evidence by object, holder, and carrier frame while preserving freshness', () => {
    const scene = attachedLogicalScene();
    const sample = baseSource(attachedPosePayload(), {
      eligible: true,
      object_id: 'part',
      policy_id: 'continuous_relative_to_attached',
      holder: {
        controllable_component_id: 'robot-1', command_target_id: 'gripper', gripper_link: 'tool',
      },
      measured_against: { entity: '/World/robot/tool', link: 'tool_base_link' },
      source_header_frame_id: 'tool_header',
      frame: {
        kind: 'component',
        controllable_component_id: 'robot-1',
        model_instance_id: 'model-instance-1',
        frame_role_id: 'gripper_tcp',
      },
      current_offset: {
        position_m: { x: 0.02, y: -0.01, z: 0.25 },
        orientation_xyzw: [0, 0, 0, 1],
      },
      baseline_offset: {
        position_m: { x: 0.01, y: -0.01, z: 0.24 },
        orientation_xyzw: [0, 0, 0, 1],
      },
      drift: { translation_m: 0.0141, rotation_rad: 0.02 },
    });
    const presentation = runtimeScenePresentation(
      runtime([sample]),
      scene,
      sourceModel(),
      manifest(),
    );

    expect(presentation.attachedObjects[0]?.poseMeasurement).toMatchObject({
      holder: {
        controllable_component_id: 'robot-1', command_target_id: 'gripper', gripper_link: 'tool',
      },
      measured_against: { entity: '/World/robot/tool', link: 'tool_base_link' },
      frame: {
        controllable_component_id: 'robot-1',
        model_instance_id: 'model-instance-1',
        frame_role_id: 'gripper_tcp',
      },
      current_offset: { position_m: { x: 0.02, y: -0.01, z: 0.25 } },
      baseline_offset: { position_m: { x: 0.01, y: -0.01, z: 0.24 } },
      drift: { translation_m: 0.0141, rotation_rad: 0.02 },
      fresh: true,
    });

    const stale = runtimeScenePresentation(
      runtime([{ ...sample, fresh: false }]),
      scene,
      sourceModel(),
      manifest(),
    );
    expect(stale.attachedObjects[0]?.poseMeasurement).toMatchObject({
      current_offset: { position_m: { x: 0.02, y: -0.01, z: 0.25 } },
      fresh: false,
    });

    const wrongFrame = runtimeScenePresentation(
      runtime([{
        ...sample,
        frame: {
          kind: 'component',
          controllable_component_id: 'robot-1',
          model_instance_id: 'another-model',
          frame_role_id: 'gripper_tcp',
        },
      }]),
      scene,
      sourceModel(),
      manifest(),
    );
    expect(wrongFrame.attachedObjects[0]?.poseMeasurement).toBeNull();

    const continuous = runtimeScenePresentation(
      runtime([baseSource(posePayload('simulation_entity_pose'), {
        eligible: true,
        object_id: 'part',
        policy_id: 'continuous',
        measured_against: 'cell frame',
        frame: {
          kind: 'component',
          controllable_component_id: 'robot-1',
          model_instance_id: 'model-instance-1',
          frame_role_id: 'gripper_tcp',
        },
        current_offset: {
          position_m: { x: 0.02, y: -0.01, z: 0.25 },
          orientation_xyzw: [0, 0, 0, 1],
        },
        baseline_offset: {
          position_m: { x: 0.01, y: -0.01, z: 0.24 },
          orientation_xyzw: [0, 0, 0, 1],
        },
        drift: { translation_m: 0.0141, rotation_rad: 0.02 },
      })]),
      scene,
      sourceModel(),
      manifest(),
    );
    expect(continuous.attachedObjects[0]?.poseMeasurement).toMatchObject({
      measured_against: 'cell frame',
      current_offset: { position_m: { x: 0.02, y: -0.01, z: 0.25 } },
      drift: { translation_m: 0.0141, rotation_rad: 0.02 },
    });
    expect(continuous.attachedObjects[0]?.poseMeasurement).not.toHaveProperty('holder');
  });

  it('follows the exact carrier transform and detaches at the same cell pose without a jump', () => {
    const scene = attachedLogicalScene();
    const attached = attachedScenePresentation(
      scene,
      sourceModel(),
      manifest(),
    ).objects[0];
    expect(attached).toBeDefined();

    const cellPose = (positionRad: number) => {
      const frame: ExecutionObservationFrame = {
        schemaVersion: 'manyforge.execution_observation_frame.v4',
        ownerEpoch: 'owner-1',
        cellId: 'cell-1',
        retainedBundleId: 'bundle-1',
        graphRevision: 'graph-1',
        sourceEpoch: 'native-1',
        sourceSequence: positionRad === 0 ? 1 : 2,
        receivedAtMonotonicUs: 123,
        targets: [{
          controllableComponentId: 'robot-1', commandTargetId: 'arm', operationCapabilityId: 'execution.trajectory.execute',
          sourceEpoch: 'native-1', sourceSequence: positionRad === 0 ? 1 : 2,
          sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'],
          coordinateKinds: ['angular'], positionsSi: [positionRad],
        }],
        measuredTargets: [],
      };
      const robot = executionObservationRobotPresentation(
        frame, sourceModel(), manifest(), motionDescriptor(), 2_000_000,
      );
      const carrier = robot?.linkTransforms[attached!.carrierCompiledFrameName];
      expect(carrier).toBeDefined();
      const carrierMatrix = new THREE.Matrix4().compose(
        new THREE.Vector3(...carrier!.position_m),
        new THREE.Quaternion(...carrier!.orientation_xyzw),
        new THREE.Vector3(1, 1, 1),
      );
      const relative = attached!.poseInCarrierFrame;
      const objectMatrix = new THREE.Matrix4().compose(
        new THREE.Vector3(
          relative.position_m.x,
          relative.position_m.y,
          relative.position_m.z,
        ),
        new THREE.Quaternion(...relative.orientation_xyzw),
        new THREE.Vector3(1, 1, 1),
      );
      const position = new THREE.Vector3();
      const orientation = new THREE.Quaternion();
      carrierMatrix.multiply(objectMatrix).decompose(
        position, orientation, new THREE.Vector3(),
      );
      return { position, orientation };
    };

    const initial = cellPose(0);
    expect(initial.position.toArray()).toEqual([2.01, -0.02, 0.03]);
    const followed = cellPose(Math.PI / 2);
    expect(followed.position.x).toBeCloseTo(1.02);
    expect(followed.position.y).toBeCloseTo(1.01);
    expect(followed.position.z).toBeCloseTo(0.03);

    const detached = logicalScene();
    const detachedSnapshot = detached.snapshot;
    if (detachedSnapshot === null) throw new Error('fixture must have a snapshot');
    const detachedPose = detachedSnapshot.objects[0].placement;
    if (detachedPose.kind !== 'cell_fixed') throw new Error('fixture must be cell fixed');
    detachedPose.pose_in_cell = {
      position_m: {
        x: followed.position.x,
        y: followed.position.y,
        z: followed.position.z,
      },
      orientation_xyzw: followed.orientation.toArray(),
    };
    expect(attachedScenePresentation(
      detached, sourceModel(), manifest(),
    ).objects).toEqual([]);
    expect(detachedPose.pose_in_cell.position_m).toEqual({
      x: followed.position.x,
      y: followed.position.y,
      z: followed.position.z,
    });
    expect(detachedPose.pose_in_cell.orientation_xyzw)
      .toEqual(followed.orientation.toArray());
  });

  it('reports crossed and orphaned attachment identities without inventing a pose', () => {
    const scene = attachedLogicalScene();
    const snapshot = scene.snapshot;
    if (snapshot === null) throw new Error('fixture must have a snapshot');
    const placement = snapshot.objects[0].placement;
    if (placement.kind !== 'carried') throw new Error('fixture must be carried');
    placement.carried_object_attachment_revision = 'crossed-revision';
    snapshot.carried_object_attachments.push({
      ...snapshot.carried_object_attachments[0],
      carried_object_attachment_id: 'attachment-orphan',
      scene_resource_id: 'orphan',
    });

    const joined = attachedScenePresentation(
      scene,
      sourceModel(),
      manifest(),
    );
    expect(joined.objects).toEqual([]);
    expect(joined.diagnostics.map((item) => item.code)).toEqual([
      'attachment_orphaned',
      'attachment_identity_mismatch',
    ]);
  });

  describe('G2: measured overlay with a mounted tool', () => {
    // robot-1 carries tool-1 on its flange; the tool has one movable finger.
    const toolCell = (fingerType = 'prismatic') => {
      const model = sourceModel();
      model.components = [
        { component_id: 'robot-1', mounted_on: null },
        { component_id: 'tool-1', mounted_on: { component_id: 'robot-1', interface_id: 'flange' } },
      ];
      model.occurrences.push({
        ...model.occurrences[0],
        component_id: 'tool-1',
        model_instance_id: 'tool-model',
        coordinates: [{
          ...model.occurrences[0].coordinates[0],
          joint_type: fingerType,
          mimic: null,
          source_joint_name: 'finger',
          viewer_joint_name: 'finger-joint',
          coordinate_kind: 'linear',
          unit: 'meters',
        }],
        links: [{ source_link_name: 'finger', viewer_link_name: 'finger' }],
      });
      model.targets.push({
        ...model.targets[1],
        component_id: 'tool-1',
        target_id: 'gripper',
        frames: [],
        model_groups: [{
          ...model.targets[0].model_groups[0],
          model_instance_id: 'tool-model',
          commandable_source_joint_names: ['finger'],
        }],
      });
      const geometry = manifest();
      geometry.links.push({ ...geometry.links[2], name: 'finger' });
      geometry.joints.push({
        ...joint('finger-joint', 'prismatic', 'tip', 'finger', [0, 0, 0]),
        axis: [0, 1, 0],
        coordinate_unit: 'm',
      });
      return { model, geometry };
    };
    const measuredArm = (positions = [Math.PI / 2]) => baseSource({
      api_version: 'manyforge.ros_pose_observation.v2',
      joint_names: ['source-joint'],
      kind: 'joint_state',
      positions,
      source_stamp_ns: null,
      topic: '/isaac_joint_states',
    });
    const toolFrame = (finger: number): ExecutionObservationFrame => ({
      schemaVersion: 'manyforge.execution_observation_frame.v4', ownerEpoch: 'owner-1',
      cellId: 'cell-1', retainedBundleId: 'bundle-1', graphRevision: 'graph-1',
      sourceEpoch: 'native-1', sourceSequence: 1, receivedAtMonotonicUs: 123,
      targets: [
        { controllableComponentId: 'robot-1', commandTargetId: 'arm', operationCapabilityId: 'execution.trajectory.execute',
          sourceEpoch: 'arm-1', sourceSequence: 1, sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'],
          coordinateKinds: ['angular'], positionsSi: [0] },
        { controllableComponentId: 'tool-1', commandTargetId: 'gripper', operationCapabilityId: 'tool.gripper.command',
          sourceEpoch: 'tool-1', sourceSequence: 1, sourceJointNames: ['finger'], compiledJointNames: ['finger-joint'],
          coordinateKinds: ['linear'], positionsSi: [finger] },
      ],
      measuredTargets: [],
    });

    // The Scene fixture still supplies object facts; explicitly copy its robot
    // observations into a viewer frame to exercise the new transport contract.
    const present = (
      scene: SceneRuntimeProjection, logical: LogicalSceneProjection,
      model: SourceModelProjection, geometry: SceneUrdfManifest,
      frame?: ExecutionObservationFrame | null,
    ) => runtimeScenePresentation(
      scene, logical, model, geometry,
      scene.state.pose_sources.some((source) => source.policy_id === 'viewer.measured_robot')
        ? viewerFrameWithSources(scene.state.pose_sources.filter(
          (source) => source.policy_id === 'viewer.measured_robot',
        ), frame)
        : frame,
    );

    it('drives the arm from the measured source and the tool from its own state', () => {
      const { model, geometry } = toolCell();
      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry, toolFrame(0.02),
      );

      expect(presentation.status.warnings).toEqual([]);
      expect(presentation.measuredRobot).toMatchObject({ objectId: 'robot-1', sourceId: 'observer-1' });
      // The measured joint, not the frame's commanded 0, moves the arm.
      expect(presentation.measuredRobot?.linkTransforms['link-1'].position_m).toEqual([1, 0, 0]);
      expect(presentation.measuredRobot?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
      // The finger sits 0.02 m from the tip: the tool's own state.
      const tip = new THREE.Vector3(...presentation.measuredRobot!.linkTransforms.tip.position_m);
      const finger = new THREE.Vector3(...presentation.measuredRobot!.linkTransforms.finger.position_m);
      expect(finger.distanceTo(tip)).toBeCloseTo(0.02);
    });

    it('takes a measured tool row over its commanded row', () => {
      const { model, geometry } = toolCell();
      const frame = toolFrame(0.02);
      frame.measuredTargets = [{ ...frame.targets[1], positionsSi: [0.03] }];
      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry, frame,
      );
      const tip = new THREE.Vector3(...presentation.measuredRobot!.linkTransforms.tip.position_m);
      const finger = new THREE.Vector3(...presentation.measuredRobot!.linkTransforms.finger.position_m);
      expect(finger.distanceTo(tip)).toBeCloseTo(0.03);
    });

    it('never draws a tool joint at neutral as if measured', () => {
      const { model, geometry } = toolCell();
      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry, null,
      );

      expect(presentation.measuredRobot).toBeNull();
      expect(presentation.status.warnings).toEqual([
        'Parts of tool-1 are hidden in the measured overlay until their joint state arrives.',
      ]);
      expect(presentation.status.warnings.join(' ')).not.toContain('occurrence-keyed');
    });

    it('needs no tool state for a jointless tool', () => {
      const { model, geometry } = toolCell('fixed');
      geometry.joints[geometry.joints.length - 1] = joint('finger-joint', 'fixed', 'tip', 'finger', [0, 0, 0]);
      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry, null,
      );

      expect(presentation.status.warnings).toEqual([]);
      expect(presentation.measuredRobot?.linkTransforms['link-1'].position_m).toEqual([1, 0, 0]);
    });

    it('says the overlay requires execution only with no live position at all', () => {
      const { model, geometry } = toolCell();
      expect(present(
        runtime([]), logicalScene(), model, geometry, null,
      ).status.warnings).toEqual(['Measured robot overlay requires execution.']);
      const live = present(
        runtime([]), logicalScene(), model, geometry, toolFrame(0.02),
      );
      expect(live.measuredRobot).toBeNull();
      expect(live.status.warnings).toEqual([]);
      // A frame of another applied model is no live position of this one.
      expect(present(
        runtime([]), logicalScene(), model, geometry, { ...toolFrame(0.02), graphRevision: 'graph-old' },
      ).status.warnings).toEqual(['Measured robot overlay requires execution.']);
    });

    it('still refuses a measured source that is not the robot occurrence', () => {
      const { model, geometry } = toolCell();
      const presentation = present(
        runtime([{ ...measuredArm(), object_id: 'tool-1' } as SceneRuntimePoseSource]),
        logicalScene(), model, geometry, toolFrame(0.02),
      );
      expect(presentation.measuredRobot).toBeNull();
      expect(presentation.status.warnings).toEqual([
        'The measured robot data does not match this cell\'s robot model; the measured overlay is off.',
      ]);
    });

    // The displayed ghost: the comparison reader, fed per owner frame.
    const ghost = (
      sources: RuntimeScenePresentation['measuredRobotSources'],
      frame: () => ExecutionObservationFrame | null,
      model: SourceModelProjection,
      geometry: SceneUrdfManifest,
    ) => createMeasuredComparisonPresentationReader({
      // Existing selection cases supply their source fixture to this helper;
      // deliver it on the viewer frame as production now does.
      getLatestExecutionObservation: () => {
        const current = frame();
        if (current?.outsidePoseSources !== undefined) return current;
        if (sources.length === 0) return current;
        return {
          ...(current ?? { ...toolFrame(0), targets: [] }),
          outsidePoseSources: sources.map((source) => ({
            source, ageAtDeliveryNs: 0, freshnessBudgetNs: 1_000_000_000,
          })),
        };
      },
      subscribeExecutionObservation: () => () => undefined,
      markExecutionObservationApplied: () => undefined,
      rejectExecutionObservation: () => undefined,
    }, model, geometry, sources);
    const fingerFromTip = (presentation: MeasuredRobotPresentation) => (
      new THREE.Vector3(...presentation.linkTransforms.finger.position_m)
        .distanceTo(new THREE.Vector3(...presentation.linkTransforms.tip.position_m))
    );

    it('selects only current delivered outside samples, independently of Scene attachments', () => {
      const clock = vi.spyOn(performance, 'now');
      try {
        clock.mockReturnValue(100);
        const { model, geometry } = toolCell();
        const staleControl = runtime([measuredArm([0])]);
        const sources = present(staleControl, logicalScene(), model, geometry).measuredRobotSources;
        const row = (source: SceneRuntimePoseSource, ageAtDeliveryNs = 200_000_000) => ({
          source, ageAtDeliveryNs, freshnessBudgetNs: 1_000_000_000,
        });
        let frame: ExecutionObservationFrame = {
          ...toolFrame(0.02), deliverySequence: 1, outsideReceivedAtMonotonicMs: 100,
          outsidePoseSources: [row(measuredArm([0]))],
        };
        const reader = ghost(sources, () => frame, model, geometry);
        expect(reader.readMeasuredRobotPresentation()?.linkTransforms.tip.position_m[0]).toBeCloseTo(2);

        // Same execution/control state; only the viewer sample advances.
        frame = { ...frame, deliverySequence: 2, outsidePoseSources: [row(measuredArm([Math.PI / 2]))] };
        expect(reader.readMeasuredRobotPresentation()?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
        // The same viewer frame is selected across carried and detached Scene facts.
        for (const scene of [logicalScene(), attachedLogicalScene(), logicalScene()]) {
          const selected = runtimeScenePresentation(staleControl, scene, model, geometry, frame);
          expect(selected.measuredRobotSources).toHaveLength(1);
          expect(selected.measuredRobotSources[0].source_id).toBe('observer-1');
        }
        const detachedReader = ghost([], () => frame, model, geometry);
        expect(detachedReader.readMeasuredRobotPresentation()?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);

        clock.mockReturnValue(900); // exactly the source's budget
        expect(reader.readMeasuredRobotPresentation()).not.toBeNull();
        clock.mockReturnValue(901); // no new execution or viewer frame
        expect(reader.readMeasuredRobotPresentation()).toBeNull();
        clock.mockReturnValue(100);
        frame = { ...frame, deliverySequence: 3, outsidePoseSources: [row({ ...measuredArm(), fresh: false })] };
        expect(reader.readMeasuredRobotPresentation()).toBeNull();
        frame = { ...frame, deliverySequence: 4, outsidePoseSources: [] };
        expect(reader.readMeasuredRobotPresentation()).toBeNull();
        frame = { ...frame, deliverySequence: 5, graphRevision: 'old-graph', outsidePoseSources: [row(measuredArm())] };
        expect(reader.readMeasuredRobotPresentation()).toBeNull();
        frame = { ...frame, graphRevision: 'graph-1', deliverySequence: 6,
          outsidePoseSources: [row({ ...measuredArm([0]), source_epoch: 'epoch-2', source_epoch_order: 2 })] };
        expect(reader.readMeasuredRobotPresentation()?.linkTransforms.tip.position_m[0]).toBeCloseTo(2);
      } finally {
        clock.mockRestore();
      }
    });

    it('re-evaluates source ambiguity and falls back to measured execution as samples expire', () => {
      const clock = vi.spyOn(performance, 'now');
      try {
        clock.mockReturnValue(100);
        const { model, geometry } = toolCell();
        const frame = viewerFrameWithSources([measuredArm(), {
          ...measuredArm([0]), source_id: 'observer-2',
        }], toolFrame(0.02));
        frame.outsideReceivedAtMonotonicMs = 100;
        frame.outsidePoseSources![1].ageAtDeliveryNs = 500_000_000;
        frame.measuredTargets = [{ ...frame.targets[0], positionsSi: [0], sourceEpoch: 'measured' }];
        const reader = ghost([], () => frame, model, geometry);
        expect(reader.readMeasuredRobotPresentation()).toBeNull(); // two fresh outside sources
        clock.mockReturnValue(601); // one expires; no new frame
        expect(reader.readMeasuredRobotPresentation()?.sourceId).toBe('observer-1');
        clock.mockReturnValue(1101); // both expire; measured execution remains
        expect(reader.readMeasuredRobotPresentation()?.sourceId).toBe('measured:arm');
      } finally {
        clock.mockRestore();
      }
    });

    it('draws the ghost from the outside source with no measured execution rows', () => {
      const { model, geometry } = toolCell();
      const { measuredRobotSources } = present(
        runtime([measuredArm()]), logicalScene(), model, geometry,
      );
      let frame = toolFrame(0.02);
      const reader = ghost(measuredRobotSources, () => frame, model, geometry);

      const first = reader.readMeasuredRobotPresentation();
      expect(frame.measuredTargets).toEqual([]);
      expect(first).toMatchObject({ objectId: 'robot-1', sourceId: 'observer-1', hiddenLinkNames: [] });
      expect(first?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
      expect(fingerFromTip(first!)).toBeCloseTo(0.02);

      // A later owner frame moves the tool in the same reader.
      frame = toolFrame(0.04);
      const next = reader.readMeasuredRobotPresentation();
      expect(fingerFromTip(next!)).toBeCloseTo(0.04);
      expect(next?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    });

    it('takes the outside source over a disagreeing execution row', () => {
      const { model, geometry } = toolCell();
      const { measuredRobotSources } = present(
        runtime([measuredArm()]), logicalScene(), model, geometry,
      );
      const frame = toolFrame(0.02);
      frame.measuredTargets = [{ ...frame.targets[0], sourceEpoch: 'measured-1', positionsSi: [0] }];

      const presentation = ghost(measuredRobotSources, () => frame, model, geometry)
        .readMeasuredRobotPresentation();
      // The source's PI/2, not the measured row's 0, turns the arm.
      expect(presentation?.linkTransforms.tip.position_m[0]).toBeCloseTo(1);
      expect(presentation?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    });

    it('keeps following the measured execution rows with no outside source', () => {
      const { model, geometry } = toolCell();
      const { measuredRobotSources } = present(
        runtime([]), logicalScene(), model, geometry,
      );
      expect(measuredRobotSources).toEqual([]);
      let frame = toolFrame(0.02);
      const reader = ghost(measuredRobotSources, () => frame, model, geometry);
      expect(reader.readMeasuredRobotPresentation()).toBeNull();

      frame = toolFrame(0.02);
      frame.measuredTargets = [{ ...frame.targets[0], sourceEpoch: 'measured-1', positionsSi: [0] }];
      const presentation = reader.readMeasuredRobotPresentation();
      expect(presentation).toMatchObject({ objectId: 'robot-1', sourceId: 'measured:arm' });
      expect(presentation?.hiddenLinkNames).toBeUndefined();
      expect(presentation?.linkTransforms.tip.position_m[0]).toBeCloseTo(2);
      expect(presentation?.linkTransforms).toEqual(
        createMeasuredComparisonPresentationReader({
          getLatestExecutionObservation: () => frame,
          subscribeExecutionObservation: () => () => undefined,
          markExecutionObservationApplied: () => undefined,
          rejectExecutionObservation: () => undefined,
        }, model, geometry).readMeasuredRobotPresentation()?.linkTransforms,
      );
    });

    it('turns the ghost off with more than one outside source', () => {
      const { model, geometry } = toolCell();
      const { measuredRobotSources } = present(
        runtime([measuredArm(), { ...measuredArm(), source_id: 'observer-2' } as SceneRuntimePoseSource]),
        logicalScene(), model, geometry,
      );
      expect(measuredRobotSources).toHaveLength(2);
      expect(ghost(measuredRobotSources, () => toolFrame(0.02), model, geometry)
        .readMeasuredRobotPresentation()).toBeNull();
    });

    it('G2-b: draws the elements with data while one stream has none yet', () => {
      const { model, geometry } = toolCell();
      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry,
      );
      // The tool streams but its row has not arrived; the arm still draws.
      const armOnly = { ...toolFrame(0.02), targets: [toolFrame(0.02).targets[0]] };
      for (const frame of [armOnly, null]) {
        const drawn = ghost(presentation.measuredRobotSources, () => frame, model, geometry)
          .readMeasuredRobotPresentation();
        expect(drawn).toMatchObject({ objectId: 'robot-1', hiddenLinkNames: ['finger'] });
        expect(drawn?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
      }
      // The solid model keeps waiting for the tool; the panel names it hidden.
      expect(present(
        runtime([measuredArm()]), logicalScene(), model, geometry, armOnly,
      ).status.warnings).toEqual(['Parts of tool-1 are hidden in the measured overlay until their joint state arrives.']);
    });

    it('updates the panel line per frame while the transport stays live', () => {
      const { model, geometry } = toolCell();
      const scene = runtime([measuredArm()]);
      let frame: ExecutionObservationFrame | null = { ...toolFrame(0.02), targets: [toolFrame(0.02).targets[0]] };
      const listeners = new Set<() => void>();
      const lines = createMeasuredOverlayFrameWarningsReader({
        getLatestExecutionObservation: () => viewerFrameWithSources([measuredArm()], frame),
        subscribeExecutionObservation: (listener) => {
          listeners.add(listener);
          return () => { listeners.delete(listener); };
        },
        markExecutionObservationApplied: () => undefined,
        rejectExecutionObservation: () => undefined,
      }, scene, model);
      const notified: (readonly string[])[] = [];
      const unsubscribe = lines.subscribeFrameWarnings(() => { notified.push(lines.readFrameWarnings()); });
      const hidden = ['Parts of tool-1 are hidden in the measured overlay until their joint state arrives.'];
      const panel = () => present(scene, logicalScene(), model, geometry, frame).status.warnings;

      const first = lines.readFrameWarnings();
      expect(first).toEqual(hidden);
      expect(panel()).toEqual(hidden);
      // Another arm-only sample: same lines, same identity, so no re-render.
      frame = { ...frame!, sourceSequence: 2 };
      listeners.forEach((listener) => listener());
      expect(notified[0]).toBe(first);
      // The tool's data arrives on the same live stream: the line goes.
      frame = { ...toolFrame(0.02), sourceSequence: 3 };
      listeners.forEach((listener) => listener());
      expect(notified[1]).toEqual([]);
      expect(panel()).toEqual([]);
      unsubscribe();
      expect(listeners.size).toBe(0);
    });

    it('notifies the overlay warning reader as quiet outside sources become ambiguous and expire', () => {
      const clock = vi.spyOn(performance, 'now');
      try {
        clock.mockReturnValue(100);
        const { model } = toolCell();
        let frame = viewerFrameWithSources([measuredArm()], toolFrame(0.02));
        frame.outsideReceivedAtMonotonicMs = 100;
        const listeners = new Set<() => void>();
        const reader = createMeasuredOverlayFrameWarningsReader({
          getLatestExecutionObservation: () => frame,
          subscribeExecutionObservation: (listener) => {
            listeners.add(listener);
            return () => { listeners.delete(listener); };
          },
          markExecutionObservationApplied: () => undefined,
          rejectExecutionObservation: () => undefined,
        }, runtime([]), model);
        const changes: (readonly string[])[] = [];
        const unsubscribe = reader.subscribeFrameWarnings(() => { changes.push(reader.readFrameWarnings()); });
        expect(reader.readFrameWarnings()).toEqual([]);
        frame = viewerFrameWithSources([measuredArm(), {
          ...measuredArm([0]), source_id: 'observer-2',
        }], frame);
        frame.outsideReceivedAtMonotonicMs = 100;
        frame.outsidePoseSources![1].ageAtDeliveryNs = 500_000_000;
        listeners.forEach((listener) => listener());
        expect(changes[0]).toEqual([
          'More than one measured robot source is configured; the measured overlay is off.',
        ]);
        // The transport's expiry notification can arrive without an execution frame.
        clock.mockReturnValue(601);
        listeners.forEach((listener) => listener());
        expect(changes[1]).toEqual([]);
        unsubscribe();
        expect(listeners.size).toBe(0);
      } finally {
        clock.mockRestore();
      }
    });

    it('G2-b: leaves components without a stream and static fixtures out of the ghost', () => {
      const { model, geometry } = toolCell('fixed');
      geometry.joints[geometry.joints.length - 1] = joint('finger-joint', 'fixed', 'tip', 'finger', [0, 0, 0]);
      model.components.push({ component_id: 'fixture-1', mounted_on: null });
      model.occurrences.push({
        ...model.occurrences[0],
        component_id: 'fixture-1',
        model_instance_id: 'fixture-model',
        coordinates: [],
        links: [{ source_link_name: 'table', viewer_link_name: 'table' }],
      });
      geometry.links.push({ ...geometry.links[0], name: 'table' });
      geometry.joints.push(joint('table-joint', 'fixed', 'base', 'table', [0, 0, -1]));
      const { measuredRobotSources } = present(
        runtime([measuredArm()]), logicalScene(), model, geometry,
      );
      const armOnly = { ...toolFrame(0), targets: [toolFrame(0).targets[0]] };

      const drawn = ghost(measuredRobotSources, () => armOnly, model, geometry)
        .readMeasuredRobotPresentation();
      expect(drawn?.linkTransforms.table).toBeDefined();
      expect(drawn?.hiddenLinkNames).toEqual(['finger', 'table']);
      expect(drawn?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    });

    it('G2-b: hides only the links downstream of a missing tool joint', () => {
      const { model, geometry } = toolCell();
      const tool = model.occurrences[1];
      tool.coordinates.push({
        ...tool.coordinates[0], source_joint_name: 'finger-2', viewer_joint_name: 'finger-joint-2',
      });
      tool.links.push({ source_link_name: 'finger-2', viewer_link_name: 'finger-2' });
      model.targets.push({
        ...model.targets[2],
        target_id: 'gripper-2',
        model_groups: [{ ...model.targets[2].model_groups[0], commandable_source_joint_names: ['finger-2'] }],
      });
      geometry.links.push({ ...geometry.links[3], name: 'finger-2' });
      geometry.joints.push({
        ...joint('finger-joint-2', 'prismatic', 'tip', 'finger-2', [0, 0, 0]),
        axis: [0, 1, 0],
        coordinate_unit: 'm',
      });
      // Only the first finger's row arrived: the second finger alone is hidden.
      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry, toolFrame(0.02),
      );
      expect(presentation.measuredRobot).toBeNull();
      expect(presentation.status.warnings).toEqual([
        'Parts of tool-1 are hidden in the measured overlay until their joint state arrives.',
      ]);
      const partial = ghost(presentation.measuredRobotSources, () => toolFrame(0.02), model, geometry)
        .readMeasuredRobotPresentation();
      expect(partial?.hiddenLinkNames).toEqual(['finger-2']);
      expect(fingerFromTip(partial!)).toBeCloseTo(0.02);

      const both = toolFrame(0.02);
      both.targets.push({
        ...both.targets[1], commandTargetId: 'gripper-2',
        sourceJointNames: ['finger-2'], compiledJointNames: ['finger-joint-2'],
      });
      expect(ghost(presentation.measuredRobotSources, () => both, model, geometry)
        .readMeasuredRobotPresentation()?.hiddenLinkNames).toEqual([]);
    });

    it('G2-b: hides the arm after a joint missing from the sample, and the tool on it, with one line', () => {
      const { model, geometry } = toolCell();
      const robot = model.occurrences[0];
      robot.coordinates = [
        { ...robot.coordinates[0], joint_type: 'revolute', mimic: null },
        {
          ...robot.coordinates[0], joint_type: 'revolute', mimic: null,
          source_joint_name: 'source-joint-b', viewer_joint_name: 'joint-b',
        },
      ];
      robot.links.push({ source_link_name: 'link-b', viewer_link_name: 'link-b' });
      geometry.links.push({ ...geometry.links[1], name: 'link-b' });
      geometry.joints.push(joint('joint-b', 'revolute', 'base', 'link-b', [0, 1, 0]));
      // The sample carries joint-b only; joint-1 is in the robot's frame row
      // alone, which never counts: link-1, the tip and the tool on it depend
      // on joint-1.
      const scene = runtime([baseSource({
        api_version: 'manyforge.ros_pose_observation.v2',
        joint_names: ['source-joint-b'],
        kind: 'joint_state',
        positions: [0.3],
        source_stamp_ns: null,
        topic: '/isaac_joint_states',
      })]);
      const frame = toolFrame(0.02);

      const presentation = present(scene, logicalScene(), model, geometry, frame);
      expect(presentation.measuredRobot).toBeNull();
      expect(presentation.status.warnings).toEqual([
        'Parts of robot-1 are hidden in the measured overlay until their joint state arrives.',
      ]);
      const partial = ghost(presentation.measuredRobotSources, () => frame, model, geometry)
        .readMeasuredRobotPresentation();
      expect(partial?.hiddenLinkNames).toEqual(['link-1', 'tip', 'finger']);
    });

    it('G2-b: hides only the built-in finger the topic does not carry', () => {
      const { model, geometry } = toolCell();
      const robot = model.occurrences[0];
      robot.coordinates = [
        { ...robot.coordinates[0], joint_type: 'revolute', mimic: null },
        {
          ...robot.coordinates[0], joint_type: 'prismatic', mimic: null,
          source_joint_name: 'robot-finger', viewer_joint_name: 'robot-finger-joint',
          coordinate_kind: 'linear', unit: 'meters',
        },
      ];
      robot.links.push({ source_link_name: 'robot-finger', viewer_link_name: 'robot-finger' });
      geometry.links.push({ ...geometry.links[2], name: 'robot-finger' });
      geometry.joints.push({
        ...joint('robot-finger-joint', 'prismatic', 'tip', 'robot-finger', [0, 0, 0]),
        axis: [0, 1, 0],
        coordinate_unit: 'm',
      });

      const presentation = present(
        runtime([measuredArm()]), logicalScene(), model, geometry, toolFrame(0.02),
      );
      expect(presentation.status.warnings).toEqual([
        'Parts of robot-1 are hidden in the measured overlay until their joint state arrives.',
      ]);
      const drawn = ghost(presentation.measuredRobotSources, () => toolFrame(0.02), model, geometry)
        .readMeasuredRobotPresentation();
      expect(drawn?.hiddenLinkNames).toEqual(['robot-finger']);
      expect(drawn?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
      expect(fingerFromTip(drawn!)).toBeCloseTo(0.02);
    });
  });

  it('fails closed on identity drift while retaining observation-only ghost poses', () => {
    const object = baseSource(posePayload('simulation_entity_pose'), {
      eligible: false,
      object_id: 'part',
      policy_id: 'continuous_unattached',
    });
    const withoutEligibleObject = runtimeScenePresentation(
      runtime([object]),
      logicalScene(),
      sourceModel(),
      manifest(),
    );
    expect(withoutEligibleObject.measuredObjects).toHaveLength(1);
    expect(withoutEligibleObject.measuredObjects[0].objectId).toBe('part');

    const staleModel = sourceModel();
    staleModel.graph_revision = 'graph-stale';
    const mismatched = runtimeScenePresentation(
      runtime([]),
      logicalScene(),
      staleModel,
      manifest(),
    );
    expect(mismatched.measuredRobot).toBeNull();
    expect(mismatched.status.warnings).toEqual([
      'Measured presentation identities do not match.',
    ]);
  });

  it('rejects unsupported joints, mimic cycles, and disconnected trees', () => {
    expect(measuredLinkTransforms(manifest(), new Map([['missing', 0]])))
      .toBeNull();
    const cyclic = manifest();
    cyclic.joints[0].mimic = {
      master: 'joint-1',
      multiplier: 1,
      offset: 0,
    };
    expect(measuredLinkTransforms(cyclic, new Map([['joint-1', 0]])))
      .toBeNull();
    const disconnected = manifest();
    disconnected.joints[1].parent_link = 'missing-parent';
    expect(measuredLinkTransforms(disconnected, new Map([['joint-1', 0]])))
      .toBeNull();
  });

  it('materializes only an exact retained-model motion-preview ghost', () => {
    const receipt = {
      schemaVersion: 'manyforge.direct_control_motion_preview.v4',
      receiptDigest: 'receipt-1',
      retainedBundleId: 'bundle-1',
      graphRevision: 'graph-1',
      target: { componentId: 'robot-1', commandTargetId: 'arm' },
      resolvedTcp: {
        entry: {
          target: { componentId: 'robot-1', commandTargetId: 'arm' },
          frame: { componentId: 'robot-1', frameId: 'tcp' },
        },
        source: 'active_tcp', ownerEpoch: 'owner-1', registrationGeneration: 1, sequence: 1,
      },
      sourceJointNames: ['source-joint'],
      finalPositionsRad: [Math.PI / 2],
    } as MotionPreviewReceipt;
    const descriptor = {
      ...executionStatus().directControl!.targetDescriptor,
    };
    const measured = measuredLinkTransforms(manifest(), new Map())!;
    const presentation = candidateRobotPresentation(receipt, manifest(), descriptor, measured);
    expect(presentation?.receiptDigest).toBe('receipt-1');
    expect(presentation?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    const start = sampledPreviewRobotPresentation(['source-joint'], [0], null, manifest(), descriptor, measured);
    const end = sampledPreviewRobotPresentation(['source-joint'], [Math.PI / 2], null, manifest(), descriptor, measured);
    expect(start?.linkTransforms.tip.position_m[1]).toBeCloseTo(0);
    expect(end?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    expect(sampledPreviewRobotPresentation(['different'], [0], null, manifest(), descriptor, measured)).toBeNull();

    expect(candidateRobotPresentation({
      ...receipt,
      retainedBundleId: 'other-bundle',
    }, manifest(), descriptor, measured)).toBeNull();
    expect(candidateRobotPresentation({
      ...receipt,
      sourceJointNames: ['missing'],
    }, manifest(), descriptor, measured)).toBeNull();
    expect(candidateRobotPresentation({
      ...receipt, target: { ...receipt.target, commandTargetId: 'another-target' },
    }, manifest(), descriptor, measured)).toBeNull();
    expect(presentation?.linkTransforms.base).toBeUndefined();

    const placed = manifest();
    placed.links.push({ ...placed.links[2], name: 'other-robot' },
      { ...placed.links[2], name: 'finger' }, { ...placed.links[2], name: 'cross-branch-mimic' });
    placed.joints.push(joint('other-joint', 'revolute', 'base', 'other-robot', [3, 0, 0]),
      joint('finger-joint', 'prismatic', 'tip', 'finger', [0, 0, 0]),
      { ...joint('mimic-joint', 'revolute', 'base', 'cross-branch-mimic', [0, 0, 0]),
        mimic: { master: 'joint-1', multiplier: 1, offset: 0 } });
    const observed = measuredLinkTransforms(placed, new Map([['finger-joint', 0.025], ['other-joint', 1]]))!;
    // The complete occurrence sits on a displaced moving base. The preview
    // changes only its declared arm coordinates, retaining measured tool state.
    const placement = new THREE.Matrix4().makeRotationZ(Math.PI / 2).setPosition(10, 0, 0);
    for (const transform of Object.values(observed)) {
      const p = new THREE.Vector3(...transform.position_m).applyMatrix4(placement);
      const q = new THREE.Quaternion().setFromRotationMatrix(placement)
        .multiply(new THREE.Quaternion(...transform.orientation_xyzw));
      transform.position_m = [p.x, p.y, p.z];
      transform.orientation_xyzw = [q.x, q.y, q.z, q.w];
    }
    const scoped = candidateRobotPresentation(receipt, placed, descriptor, observed)!;
    expect(scoped.linkTransforms['other-robot']).toBeUndefined();
    expect(scoped.linkTransforms.tip.position_m[0]).toBeCloseTo(9);
    expect(scoped.linkTransforms.tip.position_m[1]).toBeCloseTo(1);
    expect(scoped.linkTransforms.finger.position_m[2]).toBeCloseTo(0.025);
    expect(Math.abs(scoped.linkTransforms['cross-branch-mimic'].orientation_xyzw[2])).toBeCloseTo(1);
    expect(candidateRobotPresentation(receipt, placed, descriptor, {})).toBeNull();
  });

  it('materializes an exact gripper preview on the addressed tool only', () => {
    const placed = manifest();
    placed.links.push({ ...placed.links[2], name: 'finger' });
    placed.joints.push(joint('finger-joint', 'prismatic', 'tip', 'finger', [0, 0, 0]));
    const source = sourceModel();
    source.occurrences[0].coordinates.push({
      source_joint_name: 'source-finger', viewer_joint_name: 'finger-joint',
      coordinate_kind: 'linear', unit: 'meters',
    } as SourceModelProjection['occurrences'][number]['coordinates'][number]);
    source.occurrences[0].links = [{
      source_link_name: 'source-finger-link', viewer_link_name: 'finger',
    }];
    source.targets[1].model_groups[0] = {
      ...source.targets[1].model_groups[0],
      model_instance_id: 'model-instance-1',
      commandable_source_joint_names: ['source-finger'],
    };
    source.targets[1].collision_bodies = [{
      collision_body_role_id: 'gripper_collision',
      model_instance_id: 'model-instance-1',
      source_link_names: ['source-finger-link'],
    }];
    const status = executionStatus();
    status.directControlGripper = {
      controllableComponentId: 'robot-1', commandTargetId: 'gripper', operationCapabilityId: 'tool.gripper.command',
      commandSchemaId: 'gripper.command_position', commandSchemaRevision: 'schema-1',
      sourceCoordinateName: 'source-finger', sourceKind: 'model_role',
      coordinateKind: 'linear_m', minPosition: 0, maxPosition: 0.04,
      programId: 'direct-control-gripper', programRevision: 'schema-1',
      programBindingId: 'gripper-binding', programRunId: 'gripper-run',
      targetDescriptor: {} as NonNullable<ExecutionOwnerStatus['directControlGripper']>['targetDescriptor'],
    } as NonNullable<ExecutionOwnerStatus['directControlGripper']>;
    const receipt = {
      schemaVersion: 'manyforge.direct_control_capability_command_preview.v2',
      receiptDigest: 'gripper-receipt-1', retainedBundleId: 'bundle-1',
      graphRevision: 'graph-1', target: { componentId: 'robot-1', commandTargetId: 'gripper' },
      requiredCapabilityId: 'tool.gripper.command',
      commandSchemaId: 'gripper.command_position', commandSchemaRevision: 'schema-1',
      sourceCoordinateName: 'source-finger', sourceKind: 'model_role',
      coordinateKind: 'linear_m', targetPosition: 0.03,
      minPosition: 0, maxPosition: 0.04,
    } as CapabilityCommandPreviewReceipt;
    const measured = measuredLinkTransforms(placed, new Map([['finger-joint', 0.01]]))!;
    const presentation = candidateGripperPresentation(
      receipt, status, source, placed, measured,
    );
    expect(presentation?.receiptDigest).toBe('gripper-receipt-1');
    expect(Object.keys(presentation?.linkTransforms ?? {})).toEqual(['finger']);
    expect(presentation?.linkTransforms.finger.position_m[2]).toBeCloseTo(0.03);
    expect(candidateGripperPresentation(
      { ...receipt, targetPosition: 0.05 }, status, source, placed, measured,
    )).toBeNull();
  });

  it('marks a rejected TCP target with only its exact tool, without inventing an IK solution', () => {
    const source = sourceModel();
    source.occurrences[0].links = [{ source_link_name: 'source-tip', viewer_link_name: 'tip' }];
    source.targets[1].model_groups[0].model_instance_id = 'model-instance-1';
    const status = executionStatus();
    status.directControl!.tcpFrameRoleId = 'tcp';
    status.directControl!.commandTargetId = 'arm';
    status.directControl!.targetDescriptor = motionDescriptor();
    // The marker follows the owner's Active TCP.
    status.directControl!.activeTcp = { entry: { componentId: 'robot-1', targetId: 'arm', frameComponentId: 'robot-1', frameRoleId: 'tcp' } } as
      NonNullable<ExecutionOwnerStatus['directControl']>['activeTcp'];
    status.directControlGripper = { targetDescriptor: {
      controllableComponentId: 'robot-1', commandTargetId: 'gripper', operationCapabilityId: 'tool.gripper.command', touchLinks: ['source-tip'],
    } } as unknown as NonNullable<ExecutionOwnerStatus['directControlGripper']>;
    const pose = { position_m: { x_m: 7, y_m: 8, z_m: 9 }, orientation: { x: 0, y: 0, z: 0, w: 1 } };
    const measured = measuredLinkTransforms(manifest(), new Map())!;
    const marker = candidateToolPresentation(pose, status, source, manifest(), measured)!;
    expect(marker.receiptDigest).toBeNull();
    expect(Object.keys(marker.linkTransforms)).toEqual(['tip']);
    expect(marker.linkTransforms.tip.position_m).toEqual([7, 8, 9]);
    expect(candidateToolPresentation(pose, status, { ...source, graph_revision: 'other' }, manifest(), measured)).toBeNull();
    source.occurrences[0].links.push({ source_link_name: 'source-tip', viewer_link_name: 'other-tip' });
    expect(candidateToolPresentation(pose, status, source, manifest(), measured)).toBeNull();
  });
});


describe('preview collision identity', () => {
  it('joins the retained role index to exactly one viewer link', () => {
    const model = sourceModel();
    model.targets[0].collision_bodies = [{ collision_body_role_id: 'arm', model_instance_id: 'model-instance-1', source_link_names: ['base', 'link-1'] }];
    model.targets[1].collision_bodies = [];
    const body = 'component/robot-1/model/model-instance-1/body/arm/collision/1';
    expect(previewBodyIdentity(body, model, [])).toEqual({ kind: 'link', name: 'link-1' });
    model.occurrences[0].links.push({ source_link_name: 'link-1', viewer_link_name: 'other' });
    expect(previewBodyIdentity(body, model, [])).toBeNull();
    expect(previewBodyIdentity('runtime_object/part/shape/0', model, [])).toBeNull();
  });
});


describe('sample contact presentation', () => {
  it('maps link and Scene identities, applies refused over unknown over allowed, and never invents a mapping', () => {
    const model = sourceModel();
    model.targets[0].collision_bodies = [{ collision_body_role_id: 'arm', model_instance_id: 'model-instance-1', source_link_names: ['link-1'] }];
    model.targets[1].collision_bodies = [];
    const objects = [{ scene_resource_id: 'part', collision_shapes: [{}] }] as unknown as import('../types/api').LogicalSceneObject[];
    const arm = 'component/robot-1/model/model-instance-1/body/arm/collision/0';
    const path = { sourceJointNames: ['source-joint'], pairs: [
      { bodyA: arm, bodyB: 'scene/part/shape/0' },
      { bodyA: arm, bodyB: 'unknown-body' },
      { bodyA: arm, bodyB: 'scene/part/shape/0' },
    ], samples: [{ timeFromStartS: 0, positionsRad: [0], status: 'checked' as const, contacts: [
      { pairIndex: 0, disposition: 'allowed' as const }, { pairIndex: 1, disposition: 'allowed' as const },
      { pairIndex: 2, disposition: 'refused' as const },
    ] }] };
    const output = previewContactPresentation(path, 0, model, objects);
    expect(output.defaultDisposition).toBe('unknown');
    expect(output.links['link-1']).toBe('refused');
    expect(output.objects.part).toBe('refused');
    expect(output.links.base).toBeUndefined();
    path.samples[0].contacts.pop();
    expect(previewContactPresentation(path, 0, model, objects).links['link-1']).toBe('unknown');
  });

  it('uses the shown sample and never treats unknown or unresolvable evidence as clear', () => {
    const model = sourceModel();
    model.targets[0].collision_bodies = [{ collision_body_role_id: 'arm', model_instance_id: 'model-instance-1', source_link_names: ['base', 'link-1'] }];
    model.targets[1].collision_bodies = [];
    const objects = [{ scene_resource_id: 'part', collision_shapes: [{}] }] as unknown as import('../types/api').LogicalSceneObject[];
    const path = { sourceJointNames: ['source-joint'], pairs: [
      { bodyA: 'component/robot-1/model/model-instance-1/body/arm/collision/1', bodyB: 'scene/part/shape/0' },
      { bodyA: 'unmapped', bodyB: 'scene/part/shape/0' },
    ], samples: [
      { timeFromStartS: 0, positionsRad: [0], status: 'checked' as const, contacts: [] },
      { timeFromStartS: 1, positionsRad: [0], status: 'checked' as const, contacts: [{ pairIndex: 0, disposition: 'allowed' as const }] },
      { timeFromStartS: 2, positionsRad: [0], status: 'checked' as const, contacts: [{ pairIndex: 0, disposition: 'refused' as const }] },
      { timeFromStartS: 3, positionsRad: [0], status: 'unknown' as const, contacts: [] },
      { timeFromStartS: 4, positionsRad: [0], status: 'checked' as const, contacts: [{ pairIndex: 1, disposition: 'allowed' as const }] },
      { timeFromStartS: 5, positionsRad: [0], status: 'checked' as const, contacts: [{ pairIndex: 0, disposition: 'unknown' as const }] },
    ] };
    expect(previewContactPresentation(path, 0, model, objects).defaultDisposition).toBe('clear');
    expect(previewContactPresentation(path, 1, model, objects).links['link-1']).toBe('allowed');
    expect(previewContactPresentation(path, 2, model, objects).links['link-1']).toBe('refused');
    expect(previewContactPresentation(path, 3, model, objects).defaultDisposition).toBe('unknown');
    expect(previewContactPresentation(path, 4, model, objects).defaultDisposition).toBe('unknown');
    expect(previewContactPresentation(path, 5, model, objects).defaultDisposition).toBe('unknown');
    expect(previewContactPresentation(path, 99, model, objects).defaultDisposition).toBe('unknown');
  });
});


it('keeps v2 uncertainty local and preserves proved violations over incomplete geometry', () => {
  const model = sourceModel();
  model.targets[0].collision_bodies = [{ collision_body_role_id: 'arm', model_instance_id: 'model-instance-1', source_link_names: ['link-1'] }];
  model.targets[1].collision_bodies = [];
  const objects = [{ scene_resource_id: 'part', collision_shapes: [{}] }] as unknown as import('../types/api').LogicalSceneObject[];
  const path: import('../types/robotControl').MotionPreviewPath = {
    schemaVersion: 'manyforge.motion_preview_diagnostic.v2', kind: 'validated_path', reason: 'collision',
    sourceJointNames: ['joint'], pairs: [{ bodyA: 'component/robot-1/model/model-instance-1/body/arm/collision/0', bodyB: 'scene/part/shape/0' }],
    samples: [{ timeFromStartS: 0, positionsRad: [0], status: 'checked', contacts: [{ pairIndex: 0, disposition: 'unknown' }], clearanceViolations: [] }],
  };
  expect(previewContactPresentation(path, 0, model, objects)).toMatchObject({ defaultDisposition: 'clear', links: { 'link-1': 'unknown' } });
  path.samples[0].status = 'unknown';
  path.samples[0].clearanceViolations = [{ pairIndex: 0, distanceM: .001, requiredClearanceM: .002 }];
  expect(previewContactPresentation(path, 0, model, objects)).toMatchObject({ defaultDisposition: 'unknown', links: { 'link-1': 'clearance' } });
  path.samples[0].contacts[0].disposition = 'refused';
  expect(previewContactPresentation(path, 0, model, objects).links['link-1']).toBe('refused');
});
