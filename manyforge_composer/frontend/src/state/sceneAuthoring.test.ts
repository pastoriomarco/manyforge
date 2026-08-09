import { describe, expect, it } from 'vitest';

import {
  candidateTcpPoseInCell,
  editableLogicalSceneObject,
  livePoseFromProgramResource,
  programResourceWithLivePose,
  scenePoseTransaction,
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
    writeAvailable: true,
    writeUnavailableReason: null,
  };
}

describe('scene authoring intents', () => {
  it('commits exactly one current-revision, cell-framed pose update', () => {
    expect(scenePoseTransaction(facts(), object, object.placement.kind === 'cell_fixed'
      ? object.placement.pose_in_cell
      : neverPose(), 'request-1')).toEqual({
      request_context: {
        request_id: 'request-1',
        request_owner_epoch: 'scene-owner-1',
        source_label: 'composer_ui',
        correlation_id: null,
      },
      expected_projection_revision: facts().logicalScene.access.active_projection,
      operation: {
        kind: 'update_object_pose',
        scene_resource_id: 'part',
        pose: {
          position_m: { x_m: 0.4, y_m: 0.1, z_m: 0.2 },
          orientation: { x: 0, y: 0, z: 0, w: 1 },
        },
        frame_ref: { kind: 'cell_frame', cell_id: 'cell-a' },
        provider_evidence: null,
        expected_attached: false,
      },
    });
  });

  it('rejects provider-owned, carried, mesh, and write-fenced edits', () => {
    expect(editableLogicalSceneObject({
      ...object,
      provider_evidence: { provider_instance_id: 'camera' },
    })).toBeNull();
    expect(editableLogicalSceneObject({
      ...object,
      collision_shapes: [{ type: 'mesh', resource_uri: 'package://part', scale: { x: 1, y: 1, z: 1 } }],
    })).toBeNull();
    const fenced = { ...facts(), writeAvailable: false };
    expect(scenePoseTransaction(
      fenced,
      object,
      object.placement.kind === 'cell_fixed' ? object.placement.pose_in_cell : neverPose(),
      'request-2',
    )).toBeNull();
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

  it('stages an exact cell-frame Program pose without mutating the live owner', () => {
    const staged = livePoseFromProgramResource({
      scene_resource_id: 'part',
      shape: { type: 'sphere', sphere_radius_m: 0.02 },
      pose: {
        frame_ref: { kind: 'cell_frame' },
        position_m: [0.2, 0.3, 0.4],
        orientation: { rpy_deg: [0, 0, 90] },
      },
    });
    expect(staged?.position_m).toEqual({ x: 0.2, y: 0.3, z: 0.4 });
    expect(staged?.orientation_xyzw[0]).toBeCloseTo(0);
    expect(staged?.orientation_xyzw[1]).toBeCloseTo(0);
    expect(staged?.orientation_xyzw[2]).toBeCloseTo(Math.SQRT1_2);
    expect(staged?.orientation_xyzw[3]).toBeCloseTo(Math.SQRT1_2);
    expect(livePoseFromProgramResource({
      scene_resource_id: 'part',
      shape: { type: 'sphere', sphere_radius_m: 0.02 },
      pose: {
        frame_ref: {
          kind: 'component_frame',
          component_role: 'robot',
          model_role: 'arm',
          frame_role: 'base',
        },
        position_m: [0.2, 0.3, 0.4],
        orientation: { xyzw: [0, 0, 0, 1] },
      },
    })).toBeNull();
  });

  it('renders a preview in the cell only when its exact base role is the cell frame', () => {
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
    expect(candidateTcpPoseInCell(
      componentLocal,
      'robot-1',
      'arm',
      'base',
      pose,
    )).toMatchObject({ available: false });
  });
});

function neverPose(): never {
  throw new Error('fixture is not cell-fixed');
}
