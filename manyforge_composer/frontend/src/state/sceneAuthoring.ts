import * as THREE from 'three';

import {
  parseLogicalSceneProjection,
  parseSceneRuntimeProjection,
} from '../api/parsers';
import type {
  LogicalSceneObject,
  LogicalSceneProjection,
  NodeEnvironmentResponse,
  ProgramSceneResource,
  SceneRuntimeProjection,
  SourceModelProjection,
} from '../types/api';
import type { HeadlessRuntimeSnapshot } from '../types/headless';
import type { Pose3Value } from '../types/robotControl';

export interface SceneOwnerFacts {
  logicalScene: LogicalSceneProjection;
  sceneRuntime: SceneRuntimeProjection;
  sourceGeneration: number;
  sourceOwnerEpoch: string;
  runtimeSourceGeneration: number;
  runtimeSourceOwnerEpoch: string;
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
  return {
    available: true,
    facts: {
      logicalScene,
      sceneRuntime,
      sourceGeneration: logicalSection.sourceGeneration as number,
      sourceOwnerEpoch: logicalSection.sourceOwnerEpoch as string,
      runtimeSourceGeneration: runtimeSection.sourceGeneration as number,
      runtimeSourceOwnerEpoch: runtimeSection.sourceOwnerEpoch as string,
    },
  };
}

export interface SceneObjectMutationAvailability {
  duplicate: boolean;
  remove: boolean;
  transform: boolean;
  shape: boolean;
  duplicateReason: string | null;
  removeReason: string | null;
  transformReason: string | null;
  shapeReason: string | null;
}

export type CandidateTcpCellPoseResult =
  | { available: true; pose: Pose3Value }
  | { available: false; reason: string };

export interface CandidateTcpViewerBase {
  modelInstanceId: string;
  viewerLinkName: string;
  poseInCell: Pose3Value;
}

const IDENTITY_POSE: Pose3Value = {
  position_m: { x_m: 0, y_m: 0, z_m: 0 },
  orientation: { x: 0, y: 0, z: 0, w: 1 },
};

function finitePose(pose: Pose3Value): boolean {
  return [
    pose.position_m.x_m,
    pose.position_m.y_m,
    pose.position_m.z_m,
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w,
  ].every(Number.isFinite) && (
    (pose.orientation.x ** 2)
    + (pose.orientation.y ** 2)
    + (pose.orientation.z ** 2)
    + (pose.orientation.w ** 2)
  ) > 1e-18;
}

function composePose(parent: Pose3Value, local: Pose3Value): Pose3Value | null {
  if (!finitePose(parent) || !finitePose(local)) return null;
  const parentOrientation = new THREE.Quaternion(
    parent.orientation.x,
    parent.orientation.y,
    parent.orientation.z,
    parent.orientation.w,
  ).normalize();
  const position = new THREE.Vector3(
    local.position_m.x_m,
    local.position_m.y_m,
    local.position_m.z_m,
  ).applyQuaternion(parentOrientation).add(new THREE.Vector3(
    parent.position_m.x_m,
    parent.position_m.y_m,
    parent.position_m.z_m,
  ));
  const orientation = parentOrientation.multiply(new THREE.Quaternion(
    local.orientation.x,
    local.orientation.y,
    local.orientation.z,
    local.orientation.w,
  ).normalize()).normalize();
  return {
    position_m: { x_m: position.x, y_m: position.y, z_m: position.z },
    orientation: {
      x: orientation.x,
      y: orientation.y,
      z: orientation.z,
      w: orientation.w,
    },
  };
}

function relativePose(parent: Pose3Value, child: Pose3Value): Pose3Value | null {
  if (!finitePose(parent) || !finitePose(child)) return null;
  const inverseParent = new THREE.Quaternion(
    parent.orientation.x,
    parent.orientation.y,
    parent.orientation.z,
    parent.orientation.w,
  ).normalize().invert();
  const position = new THREE.Vector3(
    child.position_m.x_m - parent.position_m.x_m,
    child.position_m.y_m - parent.position_m.y_m,
    child.position_m.z_m - parent.position_m.z_m,
  ).applyQuaternion(inverseParent);
  const orientation = inverseParent.multiply(new THREE.Quaternion(
    child.orientation.x,
    child.orientation.y,
    child.orientation.z,
    child.orientation.w,
  ).normalize()).normalize();
  return {
    position_m: { x_m: position.x, y_m: position.y, z_m: position.z },
    orientation: {
      x: orientation.x,
      y: orientation.y,
      z: orientation.z,
      w: orientation.w,
    },
  };
}

function candidateTcpBaseInCell(
  sourceModel: SourceModelProjection | null,
  componentId: string | null,
  targetId: string | null,
  baseFrameRoleId: string | null,
  viewerBase: CandidateTcpViewerBase | null,
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
  if (baseFrame.frame_kind === 'cell_frame') {
    return { available: true, pose: IDENTITY_POSE };
  }
  if (
    baseFrame.model_instance_id === null
    || baseFrame.source_link_name === null
    || viewerBase === null
    || viewerBase.modelInstanceId !== baseFrame.model_instance_id
  ) {
    return {
      available: false,
      reason: 'The current base-to-cell pose is unavailable.',
    };
  }
  const occurrence = sourceModel.occurrences.find(
    (item) => item.model_instance_id === baseFrame.model_instance_id,
  );
  const link = occurrence?.links.find(
    (item) => item.source_link_name === baseFrame.source_link_name,
  );
  if (link === undefined || link.viewer_link_name !== viewerBase.viewerLinkName) {
    return {
      available: false,
      reason: 'The preview base frame does not match the current retained viewer model.',
    };
  }
  return finitePose(viewerBase.poseInCell)
    ? { available: true, pose: viewerBase.poseInCell }
    : { available: false, reason: 'The current base-to-cell pose is invalid.' };
}

export function candidateTcpPoseInCell(
  sourceModel: SourceModelProjection | null,
  componentId: string | null,
  targetId: string | null,
  baseFrameRoleId: string | null,
  pose: Pose3Value,
  viewerBase: CandidateTcpViewerBase | null = null,
): CandidateTcpCellPoseResult {
  const base = candidateTcpBaseInCell(
    sourceModel,
    componentId,
    targetId,
    baseFrameRoleId,
    viewerBase,
  );
  if (!base.available) return base;
  const result = composePose(base.pose, pose);
  return result === null
    ? { available: false, reason: 'The candidate TCP pose is invalid.' }
    : { available: true, pose: result };
}

export function candidateTcpPoseFromCell(
  sourceModel: SourceModelProjection | null,
  componentId: string | null,
  targetId: string | null,
  baseFrameRoleId: string | null,
  pose: Pose3Value,
  viewerBase: CandidateTcpViewerBase | null = null,
): CandidateTcpCellPoseResult {
  const base = candidateTcpBaseInCell(
    sourceModel,
    componentId,
    targetId,
    baseFrameRoleId,
    viewerBase,
  );
  if (!base.available) return base;
  const result = relativePose(base.pose, pose);
  return result === null
    ? { available: false, reason: 'The edited TCP pose is invalid.' }
    : { available: true, pose: result };
}

export interface RetainedSceneOwnerPresentation {
  productIdentity: string;
  facts: SceneOwnerFacts;
}

/**
 * Select the visual scene without turning retained presentation into authority.
 *
 * A current coherent owner join always wins.  During a short cross-owner join
 * gap, the last coherent scene may remain visible only while the retained
 * product identity is still compatible.  Callers must continue to use the
 * current (possibly absent) facts for every edit or control decision.
 */
export function sceneOwnerPresentation(
  current: SceneOwnerFacts | null,
  retained: RetainedSceneOwnerPresentation | null,
  currentProductIdentity: string | null,
): SceneOwnerFacts | null {
  if (current !== null) return current;
  if (retained === null) return null;
  return currentProductIdentity === null
    || retained.productIdentity === currentProductIdentity
    ? retained.facts
    : null;
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
