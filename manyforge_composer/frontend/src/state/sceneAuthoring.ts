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
  SceneTransactionRequest,
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
    || object.collision_shapes.some((shape) => shape.type === 'mesh')
  ) return null;
  return { object, pose: object.placement.pose_in_cell };
}

export function scenePoseTransaction(
  facts: SceneOwnerFacts,
  object: LogicalSceneObject,
  pose: LogicalScenePose,
  requestId: string,
): SceneTransactionRequest | null {
  const editable = editableLogicalSceneObject(object);
  const projection = facts.logicalScene.access.active_projection;
  const snapshot = facts.logicalScene.snapshot;
  if (
    !facts.writeAvailable
    || editable === null
    || projection === null
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
  return {
    request_context: {
      request_id: requestId,
      request_owner_epoch: facts.sourceOwnerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
    },
    expected_projection_revision: projection,
    operation: {
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
    },
  };
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
