import type {
  CarriedObjectAttachment,
  ProjectionRevision,
  SceneTransactionRequest,
  SceneTransactionResponse,
} from '../types/api';
import type { ControlSemanticStatus, ControlTransportStatus } from '../hooks/useHeadlessState';
import type { ComposerHeadlessState } from '../types/headless';
import type { SceneOwnerFacts } from './sceneAuthoring';

export interface SceneDetachControlFreshness {
  transportStatus: ControlTransportStatus;
  semanticStatus: ControlSemanticStatus;
  transportError: string | null;
}

function sameAttachmentAddress(left: CarriedObjectAttachment, right: CarriedObjectAttachment): boolean {
  return left.carried_object_attachment_id === right.carried_object_attachment_id
    && left.scene_resource_id === right.scene_resource_id
    && left.controllable_component_id === right.controllable_component_id
    && left.command_target_id === right.command_target_id;
}

export function sceneDetachUnavailableReason(
  snapshot: ComposerHeadlessState | null,
  facts: SceneOwnerFacts | null,
  attachment: CarriedObjectAttachment | null,
  control: SceneDetachControlFreshness,
): string | null {
  if (control.transportStatus !== 'connected') {
    return 'Program motion transport is not connected; wait for current state before detaching.';
  }
  if (control.semanticStatus !== 'current') {
    return 'Program motion state is not semantically current; wait before detaching.';
  }
  if (control.transportError !== null) {
    return 'Program motion transport reports an error; wait before detaching.';
  }
  if (snapshot === null) return 'The current Program motion state is unavailable.';
  const cycle = snapshot.programCycle;
  const idle = cycle.state === 'idle_after_reset' && cycle.programRunId === null;
  const held = cycle.programRunId !== null
    && cycle.runtimeControls?.executionSelections.motionHeld === true;
  if (!idle && !held) {
    return 'Not available while the Program is moving.';
  }
  if (facts === null) return 'The current logical Scene owner is unavailable.';
  if (attachment === null) return 'The carried-object attachment is unavailable in the current Scene.';
  const scene = facts.logicalScene.snapshot;
  if (scene === null || !scene.carried_object_attachments.some((row) => sameAttachmentAddress(row, attachment))) {
    return 'The carried-object attachment is no longer current in the Scene.';
  }
  if (facts.logicalScene.access.active_projection === null) return 'The logical Scene has no active projection.';
  if (!facts.logicalScene.access.scene_writes_open || !facts.logicalScene.access.projection_leases_open) {
    return 'The logical Scene owner currently fences detachment.';
  }
  return null;
}

export function sceneDetachRequest(
  attachment: CarriedObjectAttachment,
  facts: SceneOwnerFacts,
  requestId: string,
): SceneTransactionRequest {
  const expectedProjection = facts.logicalScene.access.active_projection;
  if (expectedProjection === null) throw new Error('The logical Scene has no active projection.');
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: facts.sourceOwnerEpoch,
      source_label: 'composer_ui',
      correlation_id: requestId,
    },
    expected_projection_revision: expectedProjection,
    operation: {
      kind: 'detach_carried_object_at_current_pose',
      carried_object_attachment_id: attachment.carried_object_attachment_id,
      expected_carried_object_attachment_revision: attachment.carried_object_attachment_revision,
      expected_scene_resource_id: attachment.scene_resource_id,
      expected_controllable_component_id: attachment.controllable_component_id,
      expected_command_target_id: attachment.command_target_id,
    },
  };
}

export function sceneDetachReceiptAccepted(
  response: SceneTransactionResponse,
  requestId: string,
  requestOwnerEpoch: string,
  expected: ProjectionRevision,
): boolean {
  const committed = response.committed_projection_revision;
  return response.request_id === requestId
    && response.request_owner_epoch === requestOwnerEpoch
    && committed.retained_bundle_id === expected.retained_bundle_id
    && committed.graph_revision === expected.graph_revision
    && committed.scene_revision.scene_id === expected.scene_revision.scene_id
    && committed.scene_revision.revision_id > expected.scene_revision.revision_id;
}
