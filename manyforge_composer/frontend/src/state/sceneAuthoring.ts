import {
  parseLogicalSceneProjection,
  parseSceneRuntimeProjection,
} from '../api/parsers';
import type {
  LogicalSceneObject,
  LogicalScenePose,
  LogicalSceneProjection,
  NodeEnvironmentResponse,
  ProgramSceneResource,
  SceneRuntimeProjection,
  SceneTransactionOperation,
  SceneTransactionRequest,
  SceneShapeRequest,
  SourceModelProjection,
} from '../types/api';
import type { HeadlessRuntimeSnapshot } from '../types/headless';
import type { Pose3Value } from '../types/robotControl';
import { quaternionFromRpyDegrees } from './robotControl';

export interface SceneOwnerFacts {
  logicalScene: LogicalSceneProjection;
  sceneRuntime: SceneRuntimeProjection;
  sourceGeneration: number;
  sourceOwnerEpoch: string;
  runtimeSourceGeneration: number;
  runtimeSourceOwnerEpoch: string;
  writeAvailable: boolean;
  writeUnavailableReason: string | null;
  runtimeWriteAvailable: boolean;
  runtimeWriteUnavailableReason: string | null;
}

export type SceneOwnerFactsResult =
  | { available: true; facts: SceneOwnerFacts }
  | { available: false; reason: string };

function unavailable(reason: string): SceneOwnerFactsResult {
  return { available: false, reason };
}

export function sceneOwnerFacts(
  runtime: HeadlessRuntimeSnapshot | null,
  sourceModel: SourceModelProjection | null,
  environment: NodeEnvironmentResponse | null,
): SceneOwnerFactsResult {
  if (runtime === null) return unavailable('The headless projection is unavailable.');
  if (sourceModel === null) return unavailable('The active source-model projection is unavailable.');
  if (environment?.active === null || environment === null) {
    return unavailable('The active node-environment identity is unavailable.');
  }
  const logicalSection = runtime.sections.logicalScene;
  const runtimeSection = runtime.sections.sceneRuntime;
  for (const [label, section] of [
    ['logical scene', logicalSection],
    ['scene runtime', runtimeSection],
  ] as const) {
    if (!section.available || section.freshness !== 'fresh' || section.payload === null) {
      return unavailable(`The ${label} owner projection is unavailable or stale.`);
    }
    if (
      section.knownGaps.length > 0
      || section.sourceGeneration === null
      || section.sourceOwnerEpoch === null
    ) {
      return unavailable(`The ${label} owner projection is incomplete.`);
    }
  }
  let logicalScene: LogicalSceneProjection;
  let sceneRuntime: SceneRuntimeProjection;
  try {
    logicalScene = parseLogicalSceneProjection(logicalSection.payload);
    sceneRuntime = parseSceneRuntimeProjection(runtimeSection.payload);
  } catch (error) {
    return unavailable(
      error instanceof Error
        ? `The scene owner contract is invalid: ${error.message}`
        : 'The scene owner contract is invalid.',
    );
  }
  const active = logicalScene.access.active_projection;
  const snapshot = logicalScene.snapshot;
  if (active === null || snapshot === null) {
    return unavailable('The logical scene has no active projection.');
  }
  if (
    snapshot.cell_id !== sourceModel.cell_id
    || sceneRuntime.cell_id !== sourceModel.cell_id
  ) {
    return unavailable('The source-model, logical-scene, and runtime cells disagree.');
  }
  if (
    active.retained_bundle_id !== sourceModel.retained_bundle_id
    || active.graph_revision !== sourceModel.graph_revision
    || active.retained_bundle_id !== environment.active.retainedBundleId
    || active.graph_revision !== environment.active.graphRevision
  ) {
    return unavailable('The source-model, node environment, and scene identities disagree.');
  }
  const runtimeIdentity = sceneRuntime.state.runtime_identity;
  if (runtimeIdentity === null) {
    return unavailable('The scene runtime has no active retained-product identity.');
  }
  if (
    runtimeIdentity.retained_bundle_id !== active.retained_bundle_id
    || runtimeIdentity.graph_revision !== active.graph_revision
  ) {
    return unavailable('The scene-runtime identity does not select the active scene graph.');
  }
  const runtimeOpen = sceneRuntime.state.state === 'open';
  const writeAvailable = logicalScene.access.scene_writes_open && runtimeOpen;
  return {
    available: true,
    facts: {
      logicalScene,
      sceneRuntime,
      sourceGeneration: logicalSection.sourceGeneration as number,
      sourceOwnerEpoch: logicalSection.sourceOwnerEpoch as string,
      runtimeSourceGeneration: runtimeSection.sourceGeneration as number,
      runtimeSourceOwnerEpoch: runtimeSection.sourceOwnerEpoch as string,
      writeAvailable,
      writeUnavailableReason: writeAvailable
        ? null
        : runtimeOpen
          ? 'The logical-scene owner has fenced scene writes.'
          : `The scene runtime is ${sceneRuntime.state.state}.`,
      runtimeWriteAvailable: runtimeOpen,
      runtimeWriteUnavailableReason: runtimeOpen
        ? null
        : `The scene runtime is ${sceneRuntime.state.state}.`,
    },
  };
}

export interface EditableLogicalSceneObject {
  object: LogicalSceneObject;
  pose: LogicalScenePose;
}

export function editableLogicalSceneObject(
  object: LogicalSceneObject,
): EditableLogicalSceneObject | null {
  if (
    object.placement.kind !== 'cell_fixed'
    || object.provider_evidence !== null
    || object.collision_shapes.length === 0
  ) return null;
  return { object, pose: object.placement.pose_in_cell };
}

export interface SceneMutationFence {
  admissionGeneration: number;
  draftId: string;
  draftRevision: number;
}

type RuntimeOwnedSceneOperation = Extract<
  SceneTransactionOperation,
  { kind: 'attach_carried_object' | 'detach_carried_object_at_current_pose' }
>;
type AuthoredSceneOperation = Exclude<
  SceneTransactionOperation,
  RuntimeOwnedSceneOperation
>;

export function sceneTransaction(
  facts: SceneOwnerFacts,
  fence: SceneMutationFence,
  operation: AuthoredSceneOperation,
  requestId: string,
): SceneTransactionRequest | null {
  const projection = facts.logicalScene.access.active_projection;
  if (
    !facts.writeAvailable
    || projection === null
    || !requestId.trim()
    || !fence.draftId.trim()
    || !Number.isSafeInteger(fence.draftRevision)
    || fence.draftRevision < 0
    || !Number.isSafeInteger(fence.admissionGeneration)
    || fence.admissionGeneration < 1
  ) return null;
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: facts.sourceOwnerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    expected_projection_revision: projection,
    expected_admission_generation: fence.admissionGeneration,
    expected_draft_id: fence.draftId,
    expected_draft_revision: fence.draftRevision,
    operation,
  };
}

function runtimeSceneTransaction(
  facts: SceneOwnerFacts,
  operation: RuntimeOwnedSceneOperation,
  requestId: string,
): SceneTransactionRequest | null {
  const projection = facts.logicalScene.access.active_projection;
  if (
    !facts.runtimeWriteAvailable
    || projection === null
    || !requestId.trim()
  ) return null;
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: facts.sourceOwnerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    expected_projection_revision: projection,
    operation,
  };
}

export function scenePoseTransaction(
  facts: SceneOwnerFacts,
  fence: SceneMutationFence,
  object: LogicalSceneObject,
  pose: LogicalScenePose,
  requestId: string,
): SceneTransactionRequest | null {
  const editable = editableLogicalSceneObject(object);
  const snapshot = facts.logicalScene.snapshot;
  if (
    !facts.writeAvailable
    || editable === null
    || snapshot === null
    || !requestId.trim()
  ) return null;
  const quaternion = pose.orientation_xyzw;
  if (
    ![
      pose.position_m.x,
      pose.position_m.y,
      pose.position_m.z,
      ...quaternion,
    ].every(Number.isFinite)
    || Math.abs(Math.hypot(...quaternion) - 1) > 1e-6
  ) return null;
  return sceneTransaction(facts, fence, {
      kind: 'update_object_pose',
      scene_resource_id: object.scene_resource_id,
      pose: {
        position_m: {
          x_m: pose.position_m.x,
          y_m: pose.position_m.y,
          z_m: pose.position_m.z,
        },
        orientation: {
          x: quaternion[0],
          y: quaternion[1],
          z: quaternion[2],
          w: quaternion[3],
        },
      },
      frame_ref: { kind: 'cell_frame', cell_id: snapshot.cell_id },
      provider_evidence: null,
      expected_attached: false,
  }, requestId);
}

function scaledSceneShape(
  object: LogicalSceneObject,
  scale: [number, number, number],
): SceneShapeRequest | null {
  if (object.collision_shapes.length !== 1) return null;
  const [x, y, z] = scale;
  if (![x, y, z].every((value) => Number.isFinite(value) && value > 0)) return null;
  const shape = object.collision_shapes[0];
  if (shape.type === 'box') return {
    type: 'box',
    box_dimensions_m: {
      x_m: shape.dimensions_m.x * x,
      y_m: shape.dimensions_m.y * y,
      z_m: shape.dimensions_m.z * z,
    },
  };
  if (shape.type === 'sphere') {
    if (Math.max(x, y, z) - Math.min(x, y, z) > 1e-6) return null;
    return { type: 'sphere', sphere_radius_m: shape.radius_m * x };
  }
  if (shape.type === 'cylinder') {
    if (Math.abs(x - y) > 1e-6) return null;
    return {
      type: 'cylinder',
      cylinder_radius_m: shape.radius_m * x,
      cylinder_height_m: shape.height_m * z,
    };
  }
  return {
    type: 'mesh',
    mesh_resource_uri: shape.resource_uri,
    mesh_scale: {
      x_m: shape.scale.x * x,
      y_m: shape.scale.y * y,
      z_m: shape.scale.z * z,
    },
  };
}

export function sceneObjectTransformTransaction(
  facts: SceneOwnerFacts,
  fence: SceneMutationFence,
  object: LogicalSceneObject,
  pose: LogicalScenePose,
  scale: [number, number, number],
  requestId: string,
): SceneTransactionRequest | null {
  const scaleChanged = scale.some((value) => Math.abs(value - 1) > 1e-6);
  if (!scaleChanged) {
    return scenePoseTransaction(facts, fence, object, pose, requestId);
  }
  const shape = scaledSceneShape(object, scale);
  const snapshot = facts.logicalScene.snapshot;
  if (
    editableLogicalSceneObject(object) === null
    || shape === null
    || snapshot === null
  ) return null;
  return sceneTransaction(facts, fence, {
    kind: 'upsert_object',
    scene_resource_id: object.scene_resource_id,
    shape,
    pose: {
      position_m: {
        x_m: pose.position_m.x,
        y_m: pose.position_m.y,
        z_m: pose.position_m.z,
      },
      orientation: {
        x: pose.orientation_xyzw[0],
        y: pose.orientation_xyzw[1],
        z: pose.orientation_xyzw[2],
        w: pose.orientation_xyzw[3],
      },
    },
    frame_ref: { kind: 'cell_frame', cell_id: snapshot.cell_id },
    support_contacts: object.support_contacts,
  }, requestId);
}

export function sceneRemoveTransaction(
  facts: SceneOwnerFacts,
  fence: SceneMutationFence,
  object: LogicalSceneObject,
  requestId: string,
): SceneTransactionRequest | null {
  if (object.provider_evidence !== null) return null;
  return sceneTransaction(facts, fence, {
    kind: 'remove_object',
    scene_resource_id: object.scene_resource_id,
  }, requestId);
}

export function sceneDuplicateTransaction(
  facts: SceneOwnerFacts,
  fence: SceneMutationFence,
  object: LogicalSceneObject,
  duplicateId: string,
  requestId: string,
): SceneTransactionRequest | null {
  const snapshot = facts.logicalScene.snapshot;
  const shape = scaledSceneShape(object, [1, 1, 1]);
  const pose = object.placement.kind === 'cell_fixed'
    ? object.placement.pose_in_cell
    : null;
  if (
    snapshot === null || shape === null || pose === null
    || !duplicateId.trim()
    || snapshot.objects.some((candidate) => candidate.scene_resource_id === duplicateId)
  ) return null;
  return sceneTransaction(facts, fence, {
    kind: 'add_object',
    scene_resource_id: duplicateId,
    shape,
    pose: {
      position_m: {
        x_m: pose.position_m.x + 0.05,
        y_m: pose.position_m.y + 0.05,
        z_m: pose.position_m.z,
      },
      orientation: {
        x: pose.orientation_xyzw[0],
        y: pose.orientation_xyzw[1],
        z: pose.orientation_xyzw[2],
        w: pose.orientation_xyzw[3],
      },
    },
    frame_ref: { kind: 'cell_frame', cell_id: snapshot.cell_id },
    support_contacts: object.support_contacts,
  }, requestId);
}

export function sceneDetachTransaction(
  facts: SceneOwnerFacts,
  attachment: NonNullable<LogicalSceneProjection['snapshot']>['carried_object_attachments'][number],
  requestId: string,
): SceneTransactionRequest | null {
  return runtimeSceneTransaction(facts, {
    kind: 'detach_carried_object_at_current_pose',
    carried_object_attachment_id: attachment.carried_object_attachment_id,
    expected_carried_object_attachment_revision:
      attachment.carried_object_attachment_revision,
    expected_scene_resource_id: attachment.scene_resource_id,
    expected_controllable_component_id: attachment.controllable_component_id,
    expected_command_target_id: attachment.command_target_id,
  }, requestId);
}

export function sceneAttachTransaction(
  facts: SceneOwnerFacts,
  object: LogicalSceneObject,
  target: {
    componentId: string;
    targetId: string;
    frameRoleId: string;
    touchBodyRoles: string[];
  },
  expectedComponentAttachmentRevisions: string[],
  requestId: string,
): SceneTransactionRequest | null {
  if (
    object.placement.kind !== 'cell_fixed'
    || object.provider_evidence !== null
    || !target.componentId.trim()
    || !target.targetId.trim()
    || !target.frameRoleId.trim()
  ) return null;
  const attachmentId = `carried.${object.scene_resource_id}`;
  return runtimeSceneTransaction(facts, {
    kind: 'attach_carried_object',
    carried_object_attachment_id: attachmentId,
    scene_resource_id: object.scene_resource_id,
    controllable_component_id: target.componentId,
    command_target_id: target.targetId,
    frame_role_id: target.frameRoleId,
    touch_body_roles: [...new Set(target.touchBodyRoles)].sort(),
    expected_component_attachment_revisions:
      [...new Set(expectedComponentAttachmentRevisions)].sort(),
  }, requestId);
}

export type CandidateTcpCellPoseResult =
  | { available: true; pose: Pose3Value }
  | { available: false; reason: string };

export function candidateTcpPoseInCell(
  sourceModel: SourceModelProjection | null,
  componentId: string | null,
  targetId: string | null,
  baseFrameRoleId: string | null,
  pose: Pose3Value,
): CandidateTcpCellPoseResult {
  if (
    sourceModel === null
    || componentId === null
    || targetId === null
    || baseFrameRoleId === null
  ) {
    return {
      available: false,
      reason: 'The preview base frame cannot be joined to the active source model.',
    };
  }
  const target = sourceModel.targets.find(
    (item) => item.component_id === componentId && item.target_id === targetId,
  );
  const baseFrame = target?.frames.find(
    (frame) => frame.frame_role_id === baseFrameRoleId,
  );
  if (baseFrame === undefined) {
    return {
      available: false,
      reason: 'The preview base-frame role is absent from the selected target.',
    };
  }
  if (baseFrame.frame_kind !== 'cell_frame') {
    return {
      available: false,
      reason: 'The preview is base-local and no owner-issued base-to-cell transform is available in this slice.',
    };
  }
  return { available: true, pose };
}

export function programResourceWithLivePose(
  programResource: ProgramSceneResource,
  liveObject: LogicalSceneObject,
): ProgramSceneResource | null {
  if (liveObject.placement.kind !== 'cell_fixed') return null;
  const pose = liveObject.placement.pose_in_cell;
  return {
    ...programResource,
    pose: {
      frame_ref: { kind: 'cell_frame' },
      position_m: [pose.position_m.x, pose.position_m.y, pose.position_m.z],
      orientation: { xyzw: [...pose.orientation_xyzw] },
    },
  };
}

export function livePoseFromProgramResource(
  programResource: ProgramSceneResource,
): LogicalScenePose | null {
  if (programResource.pose.frame_ref.kind !== 'cell_frame') return null;
  const orientation = 'xyzw' in programResource.pose.orientation
    ? {
        x: programResource.pose.orientation.xyzw[0],
        y: programResource.pose.orientation.xyzw[1],
        z: programResource.pose.orientation.xyzw[2],
        w: programResource.pose.orientation.xyzw[3],
      }
    : quaternionFromRpyDegrees(...programResource.pose.orientation.rpy_deg);
  return {
    position_m: {
      x: programResource.pose.position_m[0],
      y: programResource.pose.position_m[1],
      z: programResource.pose.position_m[2],
    },
    orientation_xyzw: [
      orientation.x,
      orientation.y,
      orientation.z,
      orientation.w,
    ],
  };
}
