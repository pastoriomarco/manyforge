import { describe, expect, it } from 'vitest';
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
  ExecutionOwnerStatus,
  ManualControlMotionDescriptor,
  MotionPreviewReceipt,
} from '../types/robotControl';
import type { ExecutionObservationFrame } from '../types/viewerObservation';
import {
  attachedScenePresentation,
  candidateRobotPresentation,
  candidateToolPresentation,
  candidateObservationEpoch,
  createExecutionRobotPresentationReader,
  executionObservationRobotPresentation,
  executionRobotPresentation,
  measuredLinkTransforms,
  runtimeScenePresentation,
} from './runtimeScenePresentation';

function executionStatus(): ExecutionOwnerStatus {
  return {
    ownerEpoch: 'owner-1',
    cellId: 'cell-1',
    graphRevision: 'graph-1',
    retainedBundleId: 'bundle-1',
    directControl: {
      controllableComponentId: 'robot-1', commandTargetId: 'arm',
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
    occurrences: [{
      component_id: 'robot-1',
      model_instance_id: 'model-instance-1',
      model_revision: 'model-revision-1',
      coordinates: [
        { source_joint_name: 'source-joint', viewer_joint_name: 'joint-1', coordinate_kind: 'angular', unit: 'radians' },
        { source_joint_name: 'joint-1' },
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
    schemaVersion: 'manyforge.manual_control_motion_descriptor.v3',
    controllableComponentId: 'robot-1',
    commandTargetId: 'arm',
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
    quality: 'good',
    source_epoch: 'epoch-1',
    source_epoch_order: 1,
    source_id: 'observer-1',
    source_sequence: 1,
    ...overrides,
  } as SceneRuntimePoseSource;
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

function runtime(sources: SceneRuntimePoseSource[]): SceneRuntimeProjection {
  return {
    api_version: 'manyforge.scene_runtime_source.v1',
    cell_id: 'cell-1',
    state: {
      api_version: 'manyforge.scene_runtime_state.v1',
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

  it('projects and caches compact owner observations once per frame', () => {
    let frame: ExecutionObservationFrame | null = {
      schemaVersion: 'manyforge.execution_observation_frame.v2',
      ownerEpoch: 'owner-1',
      cellId: 'cell-1',
      retainedBundleId: 'bundle-1',
      graphRevision: 'graph-1',
      sourceEpoch: 'native-1',
      sourceSequence: 5,
      receivedAtMonotonicUs: 123,
      targets: [{
        controllableComponentId: 'robot-1', commandTargetId: 'arm',
        sourceEpoch: 'native-1', sourceSequence: 5,
        sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'],
        coordinateKinds: ['angular'], positionsSi: [Math.PI / 2],
      }],
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
      schemaVersion: 'manyforge.execution_observation_frame.v2', ownerEpoch: 'owner-1',
      cellId: 'cell-1', retainedBundleId: 'bundle-1', graphRevision: 'graph-1',
      sourceEpoch: 'render-1', sourceSequence: 10, receivedAtMonotonicUs: 123,
      targets: [
        { controllableComponentId: 'robot-1', commandTargetId: 'arm', sourceEpoch: 'arm-1', sourceSequence: 5,
          sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'], coordinateKinds: ['angular'], positionsSi: [0] },
        { controllableComponentId: 'robot-1', commandTargetId: 'gripper', sourceEpoch: 'gripper-1', sourceSequence: 8,
          sourceJointNames: ['finger'], compiledJointNames: ['finger-master'], coordinateKinds: ['linear'], positionsSi: [0.025] },
      ],
    };
    const open = executionObservationRobotPresentation(frame, model, geometry, motionDescriptor(), null);
    const authoringStatus = executionStatus();
    authoringStatus.directControlGripper = {
      controllableComponentId: 'robot-1', commandTargetId: 'gripper', sourceKind: 'model_role', sourceCoordinateName: 'finger',
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
      joint_names: ['joint-1'],
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
      quality: 'unknown',
      source_sequence: 4,
    });

    const presentation = runtimeScenePresentation(
      runtime([jointSource, tcp, object, unknown]),
      logicalScene(),
      sourceModel(),
      manifest(),
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

  it('joins a gripper-carried object to the exact shared model frame and excludes a duplicate pose overlay', () => {
    const scene = attachedLogicalScene();
    const joined = attachedScenePresentation(
      scene,
      sourceModel(),
      manifest(),
      motionDescriptor(),
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
      motionDescriptor(),
    );
    expect(presentation.attachedObjects).toHaveLength(1);
    expect(presentation.measuredObjects).toEqual([]);
    expect(presentation.attachmentDiagnostics).toEqual([]);
  });

  it('follows the exact carrier transform and detaches at the same cell pose without a jump', () => {
    const scene = attachedLogicalScene();
    const attached = attachedScenePresentation(
      scene,
      sourceModel(),
      manifest(),
      motionDescriptor(),
    ).objects[0];
    expect(attached).toBeDefined();

    const cellPose = (positionRad: number) => {
      const frame: ExecutionObservationFrame = {
        schemaVersion: 'manyforge.execution_observation_frame.v2',
        ownerEpoch: 'owner-1',
        cellId: 'cell-1',
        retainedBundleId: 'bundle-1',
        graphRevision: 'graph-1',
        sourceEpoch: 'native-1',
        sourceSequence: positionRad === 0 ? 1 : 2,
        receivedAtMonotonicUs: 123,
        targets: [{
          controllableComponentId: 'robot-1', commandTargetId: 'arm',
          sourceEpoch: 'native-1', sourceSequence: positionRad === 0 ? 1 : 2,
          sourceJointNames: ['source-joint'], compiledJointNames: ['joint-1'],
          coordinateKinds: ['angular'], positionsSi: [positionRad],
        }],
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
      detached, sourceModel(), manifest(), motionDescriptor(),
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
      motionDescriptor(),
    );
    expect(joined.objects).toEqual([]);
    expect(joined.diagnostics.map((item) => item.code)).toEqual([
      'attachment_orphaned',
      'attachment_identity_mismatch',
    ]);
  });

  it('fails closed instead of cross-driving an unkeyed multi-occurrence viewer', () => {
    const model = sourceModel();
    model.occurrences.push({
      ...model.occurrences[0],
      component_id: 'robot-2',
      model_instance_id: 'model-2',
    });
    const presentation = runtimeScenePresentation(
      runtime([baseSource({
        api_version: 'manyforge.ros_pose_observation.v2',
        joint_names: ['joint-1'],
        kind: 'joint_state',
        positions: [0.5],
        source_stamp_ns: null,
        topic: '/isaac_joint_states',
      })]),
      logicalScene(),
      model,
      manifest(),
    );

    expect(presentation.measuredRobot).toBeNull();
    expect(presentation.status.warnings[0]).toContain('occurrence-keyed');
  });

  it('fails closed on identity drift and excludes ineligible object poses', () => {
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
    expect(withoutEligibleObject.measuredObjects).toEqual([]);

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
      schemaVersion: 'manyforge.direct_control_motion_preview.v1',
      receiptDigest: 'receipt-1',
      retainedBundleId: 'bundle-1',
      graphRevision: 'graph-1',
      componentRoleId: 'controlled_component',
      targetRoleId: 'command_target',
      sourceJointNames: ['source-joint'],
      finalPositionsRad: [Math.PI / 2],
    } as MotionPreviewReceipt;
    const descriptor = {
      ...executionStatus().directControl.targetDescriptor,
      componentRoleId: 'controlled_component', targetRoleId: 'command_target',
    };
    const measured = measuredLinkTransforms(manifest(), new Map())!;
    const presentation = candidateRobotPresentation(receipt, manifest(), descriptor, measured);
    expect(presentation?.receiptDigest).toBe('receipt-1');
    expect(presentation?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);

    expect(candidateRobotPresentation({
      ...receipt,
      retainedBundleId: 'other-bundle',
    }, manifest(), descriptor, measured)).toBeNull();
    expect(candidateRobotPresentation({
      ...receipt,
      sourceJointNames: ['missing'],
    }, manifest(), descriptor, measured)).toBeNull();
    expect(candidateRobotPresentation({
      ...receipt, targetRoleId: 'another-target',
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

  it('marks a rejected TCP target with only its exact tool, without inventing an IK solution', () => {
    const source = sourceModel();
    source.occurrences[0].links = [{ source_link_name: 'source-tip', viewer_link_name: 'tip' }];
    source.targets[1].model_groups[0].model_instance_id = 'model-instance-1';
    const status = executionStatus();
    status.directControl.tcpFrameRoleId = 'tcp';
    status.directControl.targetDescriptor = motionDescriptor();
    status.directControlGripper = { targetDescriptor: {
      controllableComponentId: 'robot-1', commandTargetId: 'gripper', touchLinks: ['source-tip'],
    } } as ExecutionOwnerStatus['directControlGripper'];
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
