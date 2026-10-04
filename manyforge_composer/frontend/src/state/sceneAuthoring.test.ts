import { describe, expect, it } from 'vitest';

import {
  candidateTcpPoseFromCell,
  candidateTcpPoseInCell,
  programResourceWithLivePose,
  sceneOwnerPresentation,
  type SceneOwnerFacts,
} from './sceneAuthoring';
import type {
  LogicalSceneObject,
  LogicalSceneProjection,
  SceneRuntimeProjection,
  SourceModelProjection,
} from '../types/api';

const object: LogicalSceneObject = {
  scene_resource_id: 'part',
  collision_shapes: [{ type: 'sphere', radius_m: 0.02 }],
  placement: {
    kind: 'cell_fixed',
    cell_id: 'cell-a',
    pose_in_cell: {
      position_m: { x: 0.4, y: 0.1, z: 0.2 },
      orientation_xyzw: [0, 0, 0, 1],
    },
  },
  provider_evidence: null,
  support_contacts: [],
};

function facts(): SceneOwnerFacts {
  const projection = {
    retained_bundle_id: 'bundle-1',
    graph_revision: 'graph-1',
    scene_revision: { scene_id: 'scene-1', revision_id: 3, content_hash: 'scene-hash' },
  };
  return {
    logicalScene: {
      api_version: 'manyforge.logical_scene_source.v1',
      access: {
        active_projection: projection,
        phase: 'finalized',
        projection_leases_open: true,
        scene_writes_open: true,
      },
      snapshot: {
        carried_object_attachments: [],
        cell_id: 'cell-a',
        configured_collision_policy: {},
        objects: [object],
        policy_epoch: 1,
        revision: projection.scene_revision,
        topology_epoch: 1,
        transaction_sequence: 4,
      },
    } satisfies LogicalSceneProjection,
    sceneRuntime: {
      api_version: 'manyforge.scene_runtime_source.v1',
      cell_id: 'cell-a',
      state: {
        api_version: 'manyforge.scene_runtime_state.v1',
        active_replay_id: null,
        counters: {},
        deferred: false,
        error: null,
        initialization_generation: 1,
        initialization_token: 'token-1',
        pose_sources: [],
        reason: 'ready',
        runtime_identity: null,
        started_at_ns: 1,
        state: 'open',
        updated_at_ns: 2,
      },
    } satisfies SceneRuntimeProjection,
    sourceGeneration: 1,
    sourceOwnerEpoch: 'scene-owner-1',
    runtimeSourceGeneration: 1,
    runtimeSourceOwnerEpoch: 'runtime-owner-1',
  };
}

describe('scene authoring intents', () => {
  it('retains only a coherent visual scene for the same product identity', () => {
    const coherent = facts();
    const retained = {
      productIdentity: 'bundle-1\u0000graph-1',
      facts: coherent,
    };

    expect(sceneOwnerPresentation(null, null, null)).toBeNull();
    expect(sceneOwnerPresentation(coherent, null, 'bundle-1\u0000graph-1'))
      .toBe(coherent);
    expect(sceneOwnerPresentation(null, retained, 'bundle-1\u0000graph-1'))
      .toBe(coherent);
    expect(sceneOwnerPresentation(null, retained, null)).toBe(coherent);
    expect(sceneOwnerPresentation(null, retained, 'bundle-2\u0000graph-2'))
      .toBeNull();
  });

  it('copies a live cell pose into Program Draft without changing its shape', () => {
    expect(programResourceWithLivePose({
      scene_resource_id: 'part',
      shape: { type: 'box', box_dimensions_m: [0.1, 0.2, 0.3] },
      pose: {
        frame_ref: { kind: 'cell_frame' },
        position_m: [0, 0, 0],
        orientation: { xyzw: [0, 0, 0, 1] },
      },
    }, object)).toMatchObject({
      shape: { type: 'box', box_dimensions_m: [0.1, 0.2, 0.3] },
      pose: { position_m: [0.4, 0.1, 0.2] },
    });
  });

  it('places a cell-framed preview directly and requires an exact base mapping otherwise', () => {
    const pose = {
      position_m: { x_m: 0.1, y_m: 0.2, z_m: 0.3 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    };
    const source = {
      targets: [{
        component_id: 'robot-1',
        target_id: 'arm',
        frames: [{
          frame_kind: 'cell_frame',
          frame_role_id: 'base',
          model_instance_id: null,
          source_link_name: null,
        }],
      }],
    } as unknown as SourceModelProjection;
    expect(candidateTcpPoseInCell(source, 'robot-1', 'arm', 'base', pose)).toEqual({
      available: true,
      pose,
    });

    const componentLocal = structuredClone(source);
    componentLocal.targets[0].frames[0].frame_kind = 'component_model_frame';
    componentLocal.targets[0].frames[0].model_instance_id = 'model-1';
    componentLocal.targets[0].frames[0].source_link_name = 'base_link';
    expect(candidateTcpPoseInCell(
      componentLocal,
      'robot-1',
      'arm',
      'base',
      pose,
    )).toMatchObject({ available: false });

    const mappedComponentLocal = {
      ...componentLocal,
      occurrences: [{
        model_instance_id: 'model-1',
        links: [{
          source_link_name: 'base_link',
          viewer_link_name: 'viewer_base_link',
        }],
      }],
    } as unknown as SourceModelProjection;
    const viewerBase = {
      modelInstanceId: 'model-1',
      viewerLinkName: 'viewer_base_link',
      poseInCell: {
        position_m: { x_m: 1, y_m: 2, z_m: 3 },
        orientation: { x: 0, y: 0, z: Math.SQRT1_2, w: Math.SQRT1_2 },
      },
    };
    const placed = candidateTcpPoseInCell(
      mappedComponentLocal,
      'robot-1',
      'arm',
      'base',
      pose,
      viewerBase,
    );
    expect(placed.available).toBe(true);
    if (!placed.available) throw new Error(placed.reason);
    expect(placed.pose.position_m.x_m).toBeCloseTo(0.8);
    expect(placed.pose.position_m.y_m).toBeCloseTo(2.1);
    expect(placed.pose.position_m.z_m).toBeCloseTo(3.3);

    const roundTrip = candidateTcpPoseFromCell(
      mappedComponentLocal,
      'robot-1',
      'arm',
      'base',
      placed.pose,
      viewerBase,
    );
    expect(roundTrip.available).toBe(true);
    if (!roundTrip.available) throw new Error(roundTrip.reason);
    expect(roundTrip.pose.position_m.x_m).toBeCloseTo(pose.position_m.x_m);
    expect(roundTrip.pose.position_m.y_m).toBeCloseTo(pose.position_m.y_m);
    expect(roundTrip.pose.position_m.z_m).toBeCloseTo(pose.position_m.z_m);
    expect(roundTrip.pose.orientation.x).toBeCloseTo(pose.orientation.x);
    expect(roundTrip.pose.orientation.y).toBeCloseTo(pose.orientation.y);
    expect(roundTrip.pose.orientation.z).toBeCloseTo(pose.orientation.z);
    expect(roundTrip.pose.orientation.w).toBeCloseTo(pose.orientation.w);
  });
});
