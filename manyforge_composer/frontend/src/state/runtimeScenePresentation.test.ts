import { describe, expect, it } from 'vitest';

import type {
  LogicalSceneProjection,
  SceneRuntimePoseSource,
  SceneRuntimeProjection,
  SceneUrdfJoint,
  SceneUrdfManifest,
  SourceModelProjection,
} from '../types/api';
import type { ExecutionOwnerStatus, MotionPreviewReceipt } from '../types/robotControl';
import type { ExecutionObservationFrame } from '../types/viewerObservation';
import {
  candidateRobotPresentation,
  createExecutionRobotPresentationReader,
  executionObservationRobotPresentation,
  executionRobotPresentation,
  measuredLinkTransforms,
  runtimeScenePresentation,
} from './runtimeScenePresentation';

function executionStatus(): ExecutionOwnerStatus {
  return {
    cellId: 'cell-1',
    graphRevision: 'graph-1',
    retainedBundleId: 'bundle-1',
    directControl: {
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
      coordinates: [
        { source_joint_name: 'source-joint' },
        { source_joint_name: 'joint-1' },
      ],
    }],
    targets: [{
      component_id: 'robot-1',
      target_id: 'arm',
      model_groups: [{
        commandable_source_joint_names: ['source-joint'],
      }],
    }],
  } as unknown as SourceModelProjection;
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
    api_version: 'manyforge.ros_pose_observation.v1' as const,
    kind,
    pose: {
      frame_id: 'cell',
      frame_kind: 'cell_frame' as const,
      orientation_xyzw: [0, 0, 0, 1] as [number, number, number, number],
      position_m: [0.4, 0.2, 0.1] as [number, number, number],
      source_stamp_s: 12,
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
      schemaVersion: 'manyforge.execution_observation_frame.v1',
      ownerEpoch: 'owner-1',
      cellId: 'cell-1',
      retainedBundleId: 'bundle-1',
      graphRevision: 'graph-1',
      controllableComponentId: 'robot-1',
      commandTargetId: 'arm',
      sourceEpoch: 'native-1',
      sourceSequence: 5,
      receivedAtMonotonicUs: 123,
      sourceJointNames: ['source-joint'],
      compiledJointNames: ['joint-1'],
      positionsRad: [Math.PI / 2],
    };
    const projected = executionObservationRobotPresentation(
      frame, sourceModel(), manifest(), 2_000_000,
    );
    expect(projected).toMatchObject({
      objectId: 'robot-1',
      sourceSequence: 5,
      observedAtNs: 2_000_000,
    });
    expect(projected?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);

    let reads = 0;
    const reader = createExecutionRobotPresentationReader({
      getLatestExecutionObservation: () => { reads += 1; return frame; },
      subscribeExecutionObservation: () => () => undefined,
    }, sourceModel(), manifest());
    const first = reader.readMeasuredRobotPresentation();
    const second = reader.readMeasuredRobotPresentation();
    expect(first).toBe(second);
    expect(reads).toBe(2);

    frame = { ...frame, graphRevision: 'crossed-graph' };
    expect(reader.readMeasuredRobotPresentation()).toBeNull();
  });

  it('computes retained-model FK and exact measured object/TCP overlays', () => {
    const jointSource = baseSource({
      api_version: 'manyforge.ros_pose_observation.v1',
      joint_names: ['joint-1'],
      kind: 'joint_state',
      positions: [Math.PI / 2],
      source_stamp_s: 11,
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
      api_version: 'manyforge.ros_pose_observation.v1',
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

  it('fails closed instead of cross-driving an unkeyed multi-occurrence viewer', () => {
    const model = sourceModel();
    model.occurrences.push({
      ...model.occurrences[0],
      component_id: 'robot-2',
      model_instance_id: 'model-2',
    });
    const presentation = runtimeScenePresentation(
      runtime([baseSource({
        api_version: 'manyforge.ros_pose_observation.v1',
        joint_names: ['joint-1'],
        kind: 'joint_state',
        positions: [0.5],
        source_stamp_s: null,
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
      sourceJointNames: ['joint-1'],
      finalPositionsRad: [Math.PI / 2],
    } as MotionPreviewReceipt;
    const presentation = candidateRobotPresentation(receipt, manifest());
    expect(presentation?.receiptDigest).toBe('receipt-1');
    expect(presentation?.linkTransforms.tip.position_m[1]).toBeCloseTo(1);

    expect(candidateRobotPresentation({
      ...receipt,
      retainedBundleId: 'other-bundle',
    }, manifest())).toBeNull();
    expect(candidateRobotPresentation({
      ...receipt,
      sourceJointNames: ['missing'],
    }, manifest())).toBeNull();
  });
});
