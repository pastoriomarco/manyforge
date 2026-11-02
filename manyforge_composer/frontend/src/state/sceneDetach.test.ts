import { describe, expect, it } from 'vitest';

import type { CarriedObjectAttachment, ProjectionRevision } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { SceneOwnerFacts } from './sceneAuthoring';
import { sceneDetachReceiptAccepted, sceneDetachRequest, sceneDetachUnavailableReason } from './sceneDetach';

const revision: ProjectionRevision = {
  retained_bundle_id: 'bundle.1',
  graph_revision: 'graph.1',
  scene_revision: { scene_id: 'scene.1', revision_id: 7, content_hash: 'scene.hash.7' },
};

const attachment: CarriedObjectAttachment = {
  carried_object_attachment_id: 'attachment.1',
  carried_object_attachment_revision: 'attachment.revision.4',
  scene_resource_id: 'part.1',
  controllable_component_id: 'robot.1',
  command_target_id: 'gripper.1',
  carrier_frame_ref: {
    kind: 'component',
    controllable_component_id: 'robot.1',
    model_instance_id: 'robot.model',
    frame_role_id: 'flange',
  },
  pose_in_carrier_frame: {
    position_m: { x: 0, y: 0, z: 0 },
    orientation_xyzw: [0, 0, 0, 1],
  },
  touch_body_roles: [],
};

const facts = {
  sourceOwnerEpoch: 'scene.owner.1',
  logicalScene: {
    access: { active_projection: revision, scene_writes_open: true, projection_leases_open: true },
    snapshot: { carried_object_attachments: [attachment] },
  },
} as unknown as SceneOwnerFacts;

const currentControl = {
  transportStatus: 'connected' as const,
  semanticStatus: 'current' as const,
  transportError: null,
};

function snapshot(state: ComposerHeadlessState['programCycle']['state'], runId: string | null,
  motionHeld: boolean | null): ComposerHeadlessState {
  return {
    programCycle: {
      state,
      programRunId: runId,
      runtimeControls: motionHeld === null ? null : {
        executionSelections: { motionHeld },
      },
    },
  } as ComposerHeadlessState;
}

describe('live carried-object detach', () => {
  it('uses the owner Program motion-held projection without Robot Control readiness gates', () => {
    expect(sceneDetachUnavailableReason(snapshot('idle_after_reset', null, null), facts, attachment, currentControl)).toBeNull();
    expect(sceneDetachUnavailableReason(snapshot('running', 'run.1', true), facts, attachment, currentControl)).toBeNull();
    expect(sceneDetachUnavailableReason(snapshot('running', 'run.1', false), facts, attachment, currentControl))
      .toBe('Not available while the Program is moving.');
    expect(sceneDetachUnavailableReason(snapshot('running', 'run.1', null), facts, attachment, currentControl))
      .toBe('Not available while the Program is moving.');
  });

  it('requires connected transport, current semantics, and no transport error even with a cached held snapshot', () => {
    const cachedHeld = snapshot('running', 'run.1', true);
    expect(sceneDetachUnavailableReason(cachedHeld, facts, attachment, {
      ...currentControl,
      transportStatus: 'disconnected',
    })).toBe('Program motion transport is not connected; wait for current state before detaching.');
    expect(sceneDetachUnavailableReason(cachedHeld, facts, attachment, {
      ...currentControl,
      transportStatus: 'delayed',
    })).toBe('Program motion transport is not connected; wait for current state before detaching.');
    expect(sceneDetachUnavailableReason(cachedHeld, facts, attachment, {
      ...currentControl,
      transportStatus: 'stale',
    })).toBe('Program motion transport is not connected; wait for current state before detaching.');
    expect(sceneDetachUnavailableReason(cachedHeld, facts, attachment, {
      ...currentControl,
      semanticStatus: 'synchronizing',
    })).toBe('Program motion state is not semantically current; wait before detaching.');
    expect(sceneDetachUnavailableReason(cachedHeld, facts, attachment, {
      ...currentControl,
      transportError: 'Control heartbeat failed.',
    })).toBe('Program motion transport reports an error; wait before detaching.');
  });

  it('builds the direct Headless transaction from the current attachment and projection', () => {
    expect(sceneDetachRequest(attachment, facts, 'request.detach.1')).toEqual({
      request_context: {
        request_id: 'request.detach.1',
        request_owner_epoch: 'scene.owner.1',
        source_label: 'composer_ui',
        correlation_id: 'request.detach.1',
      },
      expected_projection_revision: revision,
      operation: {
        kind: 'detach_carried_object_at_current_pose',
        carried_object_attachment_id: 'attachment.1',
        expected_carried_object_attachment_revision: 'attachment.revision.4',
        expected_scene_resource_id: 'part.1',
        expected_controllable_component_id: 'robot.1',
        expected_command_target_id: 'gripper.1',
      },
    });
  });

  it('accepts only the matching direct postcommit receipt for a successor Scene revision', () => {
    const response = {
      request_id: 'request.detach.1',
      request_owner_epoch: 'scene.owner.1',
      committed_projection_revision: {
        ...revision,
        scene_revision: { scene_id: 'scene.1', revision_id: 8, content_hash: 'scene.hash.8' },
      },
    } as Parameters<typeof sceneDetachReceiptAccepted>[0];
    expect(sceneDetachReceiptAccepted(response, 'request.detach.1', 'scene.owner.1', revision)).toBe(true);
    expect(sceneDetachReceiptAccepted(response, 'request.other', 'scene.owner.1', revision)).toBe(false);
    expect(sceneDetachReceiptAccepted({
      ...response,
      committed_projection_revision: revision,
    }, 'request.detach.1', 'scene.owner.1', revision)).toBe(false);
  });
});
