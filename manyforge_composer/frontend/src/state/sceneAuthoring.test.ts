import { describe, expect, it } from 'vitest';

import {
  candidateTcpPoseFromCell,
  candidateTcpPoseInCell,
  componentAttachmentFenceFromSnapshot,
  editableLogicalSceneObject,
  editableProgramShapeFromObject,
  livePoseFromProgramResource,
  programResourceWithLivePose,
  sceneMutationFenceFromSnapshot,
  sceneObjectMutationAvailability,
  sceneObjectUpdateTransaction,
  sceneOwnerPresentation,
  scenePoseTransaction,
  sceneDuplicateTransaction,
  sceneDetachTransaction,
  type SceneOwnerFacts,
} from './sceneAuthoring';
import type {
  LogicalSceneObject,
  LogicalSceneProjection,
  SceneRuntimeProjection,
  SourceModelProjection,
} from '../types/api';
import type { ComposerHeadlessState, HeadlessRuntimeSnapshot } from '../types/headless';

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

const fence = {
  admissionGeneration: 7,
  draftId: 'draft-1',
  draftRevision: 4,
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
    runtimeWriteAvailable: true,
    runtimeWriteUnavailableReason: null,
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

  it('forms an authored fence only from one current admitted Draft snapshot', () => {
    const snapshot = {
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1',
        workspaceRevision: 9,
        programDraftId: 'draft-1',
        programDraftRevision: 4,
        deploymentDraftId: 'deployment-draft-1',
        deploymentDraftRevision: 1,
      },
    } as ComposerHeadlessState;
    expect(sceneMutationFenceFromSnapshot(snapshot, 'draft-1', 4)).toEqual({
      available: true,
      fence,
    });
    expect(sceneMutationFenceFromSnapshot(snapshot, 'draft-1', 5)).toMatchObject({
      available: false,
    });
    expect(sceneMutationFenceFromSnapshot({
      ...snapshot,
      authoringAdmission: { authoringAllowed: false, admissionGeneration: 7 },
    }, 'draft-1', 4)).toMatchObject({ available: false });
  });

  it('uses the exact current Manager snapshot as the attachment fence', () => {
    const runtime = {
      complete: true,
      sections: {
        manager: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          sourceGeneration: 3,
          sourceOwnerEpoch: 'manager-owner-1',
          payload: {
            active: {
              retained_bundle_id: 'bundle-1',
              graph_revision: 'graph-1',
              cell_graph: {
                component_attachments: [
                  { component_attachment_revision: 'revision-b' },
                  { component_attachment_revision: 'revision-a' },
                ],
              },
            },
          },
        },
      },
    } as unknown as HeadlessRuntimeSnapshot;
    expect(componentAttachmentFenceFromSnapshot(
      runtime,
      facts().logicalScene.access.active_projection,
    )).toEqual({
      available: true,
      revisions: ['revision-a', 'revision-b'],
    });
    expect(componentAttachmentFenceFromSnapshot(runtime, {
      ...facts().logicalScene.access.active_projection!,
      graph_revision: 'other-graph',
    })).toMatchObject({ available: false });
  });

  it('commits exactly one current-revision, cell-framed pose update', () => {
    expect(scenePoseTransaction(facts(), fence, object, object.placement.kind === 'cell_fixed'
      ? object.placement.pose_in_cell
      : neverPose(), 'request-1')).toEqual({
      request_context: {
        request_id: 'request-1',
        request_owner_epoch: 'scene-owner-1',
        source_label: 'composer_ui',
        correlation_id: null,
      },
      expected_projection_revision: facts().logicalScene.access.active_projection,
      expected_admission_generation: 7,
      expected_draft_id: 'draft-1',
      expected_draft_revision: 4,
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

  it('rejects provider-owned, carried, and write-fenced edits while allowing owner meshes', () => {
    expect(editableLogicalSceneObject({
      ...object,
      provider_evidence: { provider_instance_id: 'camera' },
    })).toBeNull();
    expect(editableLogicalSceneObject({
      ...object,
      collision_shapes: [{ type: 'mesh', resource_uri: 'package://part', scale: { x: 1, y: 1, z: 1 } }],
    })).not.toBeNull();
    const fenced = { ...facts(), writeAvailable: false };
    expect(scenePoseTransaction(
      fenced,
      fence,
      object,
      object.placement.kind === 'cell_fixed' ? object.placement.pose_in_cell : neverPose(),
      'request-2',
    )).toBeNull();
  });

  it('keeps pose editing independent from single-shape actions', () => {
    const shapeless = { ...object, collision_shapes: [] };
    expect(editableLogicalSceneObject(shapeless)).not.toBeNull();
    expect(sceneObjectMutationAvailability(facts(), fence, shapeless)).toMatchObject({
      transform: true,
      shape: false,
      duplicate: false,
      remove: true,
    });
  });

  it('commits one exact owner update for staged mesh, scale, and pose', () => {
    const shape = editableProgramShapeFromObject({
      ...object,
      collision_shapes: [{
        type: 'mesh',
        resource_uri: 'package://part.glb',
        scale: { x: 1, y: 2, z: 3 },
      }],
    });
    expect(shape).not.toBeNull();
    expect(sceneObjectUpdateTransaction(
      facts(),
      fence,
      { ...object, collision_shapes: [{
        type: 'mesh',
        resource_uri: 'package://part.glb',
        scale: { x: 1, y: 2, z: 3 },
      }] },
      object.placement.kind === 'cell_fixed'
        ? object.placement.pose_in_cell
        : neverPose(),
      shape!,
      'request-shape',
    )?.operation).toMatchObject({
      kind: 'upsert_object',
      shape: {
        type: 'mesh',
        mesh_resource_uri: 'package://part.glb',
        mesh_scale: { x_m: 1, y_m: 2, z_m: 3 },
      },
    });
  });

  it('builds owner-fenced duplicate and detach transactions without browser effects', () => {
    expect(sceneDuplicateTransaction(
      facts(), fence, object, 'part_copy', 'request-copy',
    )?.operation).toMatchObject({ kind: 'add_object', scene_resource_id: 'part_copy' });
    const detached = sceneDetachTransaction(facts(), {
      carried_object_attachment_id: 'attachment.1',
      carried_object_attachment_revision: 'sha256:' + 'a'.repeat(64),
      scene_resource_id: 'part',
      controllable_component_id: 'robot',
      command_target_id: 'arm',
      carrier_frame_ref: {
        kind: 'component',
        controllable_component_id: 'robot',
        model_instance_id: 'robot.model',
        frame_role_id: 'tcp',
      },
      pose_in_carrier_frame: {
        position_m: { x: 0, y: 0, z: 0 },
        orientation_xyzw: [0, 0, 0, 1],
      },
      touch_body_roles: [],
    }, 'request-detach');
    expect(detached).not.toHaveProperty('expected_admission_generation');
    expect(detached).not.toHaveProperty('expected_draft_id');
    expect(detached?.operation).toMatchObject({
      kind: 'detach_carried_object_at_current_pose',
      expected_scene_resource_id: 'part',
    });
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

function neverPose(): never {
  throw new Error('fixture is not cell-fixed');
}
