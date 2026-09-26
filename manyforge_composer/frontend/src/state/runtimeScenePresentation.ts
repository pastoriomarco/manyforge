import * as THREE from 'three';
import type { Nanoseconds } from '../types/nanoseconds';

import type {
  CarriedObjectAttachment,
  LogicalSceneProjection,
  LogicalSceneObject,
  QuaternionXyzwTuple,
  SceneRuntimeGoodObservationPayload,
  SceneRuntimeObservationPose,
  SceneRuntimePoseSource,
  SceneRuntimeProjection,
  SceneUrdfJoint,
  SceneUrdfManifest,
  SourceModelProjection,
  Vector3Tuple,
} from '../types/api';
import type {
  CapabilityCommandPreviewReceipt,
  MotionPreviewReceipt,
} from '../types/robotControl';
import type {
  ExecutionOwnerStatus,
  ManualControlMotionDescriptor,
  Pose3Value,
} from '../types/robotControl';
import type {
  ExecutionObservationFrame,
  ExecutionObservationReader,
} from '../types/viewerObservation';

export interface RuntimeLinkTransform {
  position_m: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
}

/** Exact authoring join; the viewer may separately retain older display data. */
export function candidateObservationEpoch(
  frame: ExecutionObservationFrame | null,
  status: ExecutionOwnerStatus | null,
): string | null {
  if (frame === null || status === null || frame.ownerEpoch !== status.ownerEpoch
    || frame.cellId !== status.cellId || frame.retainedBundleId !== status.retainedBundleId
    || frame.graphRevision !== status.graphRevision) return null;
  // A selection may have no motion target (gripper-only, H1a-IF.2).
  const motion = status.directControl;
  const arm = motion === null ? null : frame.targets.find((target) => (
    target.controllableComponentId === motion.controllableComponentId
    && target.commandTargetId === motion.commandTargetId));
  const gripper = status.directControlGripper;
  const tool = gripper?.sourceKind !== 'model_role' ? null : frame.targets.find((target) => (
    target.controllableComponentId === gripper.controllableComponentId
    && target.commandTargetId === gripper.commandTargetId
    && target.sourceJointNames.includes(gripper.sourceCoordinateName)
  ));
  if (arm === undefined || tool === undefined) return null;
  return JSON.stringify([frame.ownerEpoch, frame.sourceEpoch, arm?.sourceEpoch, tool?.sourceEpoch]);
}

export interface MeasuredRobotPresentation {
  objectId: string;
  sourceId: string;
  sourceEpoch: string;
  sourceSequence: number;
  observedAtNs: Nanoseconds | null;
  linkTransforms: Record<string, RuntimeLinkTransform>;
}

export interface MeasuredRobotPresentationReader {
  readMeasuredRobotPresentation: () => MeasuredRobotPresentation | null;
  subscribeMeasuredRobotPresentation: (listener: () => void) => () => void;
  markMeasuredRobotPresentationApplied: (
    presentation: MeasuredRobotPresentation,
  ) => void;
}

export interface MeasuredPosePresentation {
  objectId: string;
  policyId: string;
  sourceId: string;
  sourceEpoch: string;
  sourceSequence: number;
  observedAtNs: Nanoseconds;
  pose: SceneRuntimeObservationPose;
}

export interface RuntimeScenePresentation {
  measuredRobot: MeasuredRobotPresentation | null;
  measuredTcp: MeasuredPosePresentation | null;
  measuredObjects: MeasuredPosePresentation[];
  attachedObjects: AttachedObjectPresentation[];
  attachmentDiagnostics: AttachmentPresentationDiagnostic[];
  status: {
    total: number;
    freshGood: number;
    staleGood: number;
    unknown: number;
    buffered: number;
    warnings: string[];
  };
}

export interface AttachedObjectPresentation {
  object: LogicalSceneObject;
  attachmentId: string;
  attachmentRevision: string;
  carrierComponentId: string;
  commandTargetId: string;
  carrierFrameRoleId: string;
  carrierCompiledFrameName: string;
  poseInCarrierFrame: CarriedObjectAttachment['pose_in_carrier_frame'];
}

export interface AttachmentPresentationDiagnostic {
  code:
    | 'attachment_missing'
    | 'attachment_ambiguous'
    | 'attachment_identity_mismatch'
    | 'carrier_descriptor_unavailable'
    | 'carrier_target_mismatch'
    | 'carrier_frame_missing'
    | 'carrier_frame_ambiguous'
    | 'carrier_model_mismatch'
    | 'carrier_viewer_link_missing'
    | 'attachment_orphaned';
  sceneResourceId: string;
  message: string;
}

export interface CandidateRobotPresentation {
  receiptDigest: string | null;
  linkTransforms: Record<string, RuntimeLinkTransform>;
}

/**
 * Project exact Execution-owner joint observations onto the retained model.
 * This is the native-lane measured robot source; it never plans or infers a
 * command and snaps rather than inventing a source timestamp.
 */
export function executionRobotPresentation(
  status: ExecutionOwnerStatus | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
): MeasuredRobotPresentation | null {
  if (
    status === null
    || sourceModel === null
    || manifest === null
    || status.cellId !== sourceModel.cell_id
    || status.cellId !== manifest.cell_id
    || status.retainedBundleId !== sourceModel.retained_bundle_id
    || status.retainedBundleId !== manifest.retained_bundle_id
    || status.graphRevision !== sourceModel.graph_revision
    || status.graphRevision !== manifest.graph_revision
  ) return null;
  const positions = new Map<string, number>();
  const setPosition = (compiledJointName: string, position: number): boolean => {
    const previous = positions.get(compiledJointName);
    if (!Number.isFinite(position) || (previous !== undefined && previous !== position)) return false;
    positions.set(compiledJointName, position);
    return true;
  };
  const observedMotions = status.observedMotions;
  const motionDescriptors = status.directControlTargets.map((target) => target.targetDescriptor);
  for (const observed of observedMotions) {
    const descriptor = motionDescriptors.find((candidate) => (
      candidate.controllableComponentId === observed.controllableComponentId
      && candidate.commandTargetId === observed.commandTargetId
    ));
    const occurrence = sourceModel.occurrences.find(
      (candidate) => candidate.component_id === observed.controllableComponentId,
    );
    if (
      !observed.complete
      || descriptor === undefined
      || occurrence === undefined
      || descriptor.joints.length !== observed.sourceJointNames.length
      || observed.positionsRad.length !== observed.sourceJointNames.length
    ) return null;
    const occurrenceCoordinates = new Set(
      occurrence.coordinates.map((coordinate) => coordinate.source_joint_name),
    );
    for (const [index, sourceJointName] of observed.sourceJointNames.entries()) {
      const joint = descriptor.joints[index];
      if (
        joint === undefined
        || joint.sourceJointName !== sourceJointName
        || !occurrenceCoordinates.has(sourceJointName)
        || !setPosition(joint.compiledJointName, observed.positionsRad[index])
      ) return null;
    }
  }
  const observedGrippers = status.observedGrippers?.length
    ? status.observedGrippers
    : status.observedGripper == null ? [] : [status.observedGripper];
  const gripperTargets = status.directControlGripperTargets?.length
    ? status.directControlGripperTargets
    : status.directControlGripper === null ? [] : [status.directControlGripper];
  for (const observed of observedGrippers) {
    if (!observed.complete) return null;
    const target = gripperTargets.find((candidate) => (
      (observed.controllableComponentId === undefined
        || candidate.controllableComponentId === observed.controllableComponentId)
      && (observed.commandTargetId === undefined
        || candidate.commandTargetId === observed.commandTargetId)
      && candidate.sourceCoordinateName === observed.sourceCoordinateName
    ));
    if (target === undefined) return null;
    const occurrence = sourceModel.occurrences.find((candidate) => (
      candidate.component_id === target.controllableComponentId
      && candidate.coordinates.some(
        (coordinate) => coordinate.source_joint_name === observed.sourceCoordinateName,
      )
    ));
    const coordinate = occurrence?.coordinates.find(
      (candidate) => candidate.source_joint_name === observed.sourceCoordinateName,
    );
    if (coordinate === undefined || !setPosition(coordinate.viewer_joint_name, observed.position)) {
      return null;
    }
  }
  const linkTransforms = measuredLinkTransforms(manifest, positions);
  // The presentation is sourced from the selected observation: motion when
  // present, otherwise the first observed tool (gripper-only cell).
  const motionSource = status.observedMotion;
  const toolSource = status.observedGrippers[0];
  const observed = motionSource !== null
    ? {
      controllableComponentId: motionSource.controllableComponentId,
      commandTargetId: motionSource.commandTargetId,
      sourceEpoch: motionSource.sourceEpoch,
      sequence: motionSource.sequence,
    }
    : toolSource?.controllableComponentId !== undefined
      && toolSource.commandTargetId !== undefined
      ? {
        controllableComponentId: toolSource.controllableComponentId,
        commandTargetId: toolSource.commandTargetId,
        sourceEpoch: toolSource.sourceEpoch,
        sequence: toolSource.sequence,
      }
      : null;
  if (observed === null) return null;
  return linkTransforms === null ? null : {
    objectId: observed.controllableComponentId,
    sourceId: `execution:${observed.commandTargetId}`,
    sourceEpoch: observed.sourceEpoch,
    sourceSequence: observed.sequence,
    observedAtNs: null,
    linkTransforms,
  };
}

/**
 * Project one compact owner observation through the retained source/model map.
 * The frame contains presentation facts only; every identity is checked again
 * before it can move a retained Three.js link.
 */
export function executionObservationRobotPresentation(
  frame: ExecutionObservationFrame | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
  descriptor: ManualControlMotionDescriptor | null = null,
  observedAtNs: Nanoseconds | null = null,
): MeasuredRobotPresentation | null {
  if (
    frame === null
    || sourceModel === null
    || manifest === null
    || frame.cellId !== sourceModel.cell_id
    || frame.cellId !== manifest.cell_id
    || frame.retainedBundleId !== sourceModel.retained_bundle_id
    || frame.retainedBundleId !== manifest.retained_bundle_id
    || frame.graphRevision !== sourceModel.graph_revision
    || frame.graphRevision !== manifest.graph_revision
  ) return null;
  // Viewer pose admission must not depend on the optional Robot-control
  // selection. After Apply (and whenever the inspector is closed) there may
  // be no selected descriptor, while the execution owner still publishes the
  // complete current state. Use any observed target as the presentation
  // identity and validate a descriptor only when one is explicitly selected.
  const motion = descriptor === null
    ? frame.targets[0]
    : frame.targets.find((target) => (
        target.controllableComponentId === descriptor.controllableComponentId
        && target.commandTargetId === descriptor.commandTargetId
      ));
  if (motion === undefined) return null;
  if (descriptor !== null && (
    motion.sourceJointNames.length !== descriptor.joints.length
    || descriptor.joints.some((joint, index) => (
      joint.sourceJointName !== motion.sourceJointNames[index]
      || joint.compiledJointName !== motion.compiledJointNames[index]
    ))
  )) return null;
  const compiledPositions = new Map<string, number>();
  for (const observed of frame.targets) {
    const target = sourceModel.targets.find((candidate) => (
      candidate.component_id === observed.controllableComponentId
      && candidate.target_id === observed.commandTargetId
    ));
    if (target === undefined || observed.sourceJointNames.length !== observed.compiledJointNames.length
      || observed.sourceJointNames.length !== observed.positionsSi.length
      || observed.sourceJointNames.length !== observed.coordinateKinds.length) return null;
    const groups = target.model_groups.filter((group) => (
      group.commandable_source_joint_names.length === observed.sourceJointNames.length
      && group.commandable_source_joint_names.every((name, index) => name === observed.sourceJointNames[index])
    ));
    if (groups.length !== 1) return null;
    const occurrence = sourceModel.occurrences.find((candidate) => (
      candidate.component_id === observed.controllableComponentId
      && candidate.model_instance_id === groups[0].model_instance_id
    ));
    if (occurrence === undefined) return null;
    for (const [index, sourceName] of observed.sourceJointNames.entries()) {
      const compiledName = observed.compiledJointNames[index];
      const value = observed.positionsSi[index];
      const coordinates = occurrence.coordinates.filter((coordinate) => (
        coordinate.source_joint_name === sourceName && coordinate.viewer_joint_name === compiledName
      ));
      if (coordinates.length !== 1 || coordinates[0].coordinate_kind !== observed.coordinateKinds[index]
        || coordinates[0].unit !== (observed.coordinateKinds[index] === 'angular' ? 'radians' : 'meters')
        || !Number.isFinite(value) || compiledPositions.has(compiledName)) return null;
      compiledPositions.set(compiledName, value);
    }
  }
  const linkTransforms = measuredLinkTransforms(manifest, compiledPositions);
  return linkTransforms === null ? null : {
    objectId: motion.controllableComponentId,
    sourceId: `execution:${motion.commandTargetId}`,
    sourceEpoch: `${frame.ownerEpoch}:${frame.sourceEpoch}`,
    sourceSequence: frame.sourceSequence,
    observedAtNs,
    linkTransforms,
  };
}

/**
 * Capacity-one render reader. Forward kinematics runs once per admitted owner
 * sample, regardless of how many retained links consume that sample.
 */
export function createExecutionRobotPresentationReader(
  reader: ExecutionObservationReader,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
  descriptor: ManualControlMotionDescriptor | null,
): MeasuredRobotPresentationReader {
  let cachedFrame: ExecutionObservationFrame | null | undefined;
  let cachedPresentation: MeasuredRobotPresentation | null = null;
  return {
    readMeasuredRobotPresentation: () => {
      const frame = reader.getLatestExecutionObservation();
      if (frame === cachedFrame) return cachedPresentation;
      cachedFrame = frame;
      cachedPresentation = executionObservationRobotPresentation(
        frame,
        sourceModel,
        manifest,
        descriptor,
        frame === null ? null : (BigInt(frame.receivedAtMonotonicUs) * 1_000n).toString(),
      );
      if (frame !== null && cachedPresentation === null
        && sourceModel !== null && manifest !== null && descriptor !== null
        && sourceModel.cell_id === manifest.cell_id
        && sourceModel.retained_bundle_id === manifest.retained_bundle_id
        && sourceModel.graph_revision === manifest.graph_revision) {
        // A frame for a model that is not currently projectable cannot move
        // the retained links. The transport rejects it in place (with a
        // credit) so a model refresh does not create a reconnect storm. An
        // incomplete metadata join remains a bootstrap wait.
        reader.rejectExecutionObservation(frame);
      }
      return cachedPresentation;
    },
    subscribeMeasuredRobotPresentation: reader.subscribeExecutionObservation,
    markMeasuredRobotPresentationApplied: (presentation) => {
      if (
        cachedFrame !== null
        && cachedFrame !== undefined
        && cachedPresentation === presentation
      ) {
        reader.markExecutionObservationApplied(cachedFrame);
      }
    },
  };
}

const EMPTY_PRESENTATION: RuntimeScenePresentation = {
  measuredRobot: null,
  measuredTcp: null,
  measuredObjects: [],
  attachedObjects: [],
  attachmentDiagnostics: [],
  status: {
    total: 0,
    freshGood: 0,
    staleGood: 0,
    unknown: 0,
    buffered: 0,
    warnings: [],
  },
};

interface AttachedScenePresentationResult {
  objects: AttachedObjectPresentation[];
  diagnostics: AttachmentPresentationDiagnostic[];
}

function attachmentDiagnostic(
  code: AttachmentPresentationDiagnostic['code'],
  sceneResourceId: string,
  message: string,
): AttachmentPresentationDiagnostic {
  return { code, sceneResourceId, message };
}

/**
 * Join one owner attachment to one exact retained target/frame/viewer link.
 * No source or compiled name is guessed: a missing or crossed join remains a
 * visible diagnostic and the object is not rendered at an invented pose.
 */
export function attachedScenePresentation(
  logicalScene: LogicalSceneProjection | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
  descriptor: ManualControlMotionDescriptor | null,
): AttachedScenePresentationResult {
  const snapshot = logicalScene?.snapshot;
  if (snapshot === null || snapshot === undefined) {
    return { objects: [], diagnostics: [] };
  }
  const objects: AttachedObjectPresentation[] = [];
  const diagnostics: AttachmentPresentationDiagnostic[] = [];
  const referencedAttachments = new Set<string>();
  const viewerLinks = new Set(manifest?.links.map((link) => link.name) ?? []);

  for (const object of snapshot.objects) {
    if (object.placement.kind !== 'carried') continue;
    const placement = object.placement;
    const matches = snapshot.carried_object_attachments.filter(
      (attachment) => attachment.carried_object_attachment_id
        === placement.carried_object_attachment_id,
    );
    if (matches.length === 0) {
      diagnostics.push(attachmentDiagnostic(
        'attachment_missing',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has no matching owner attachment.`,
      ));
      continue;
    }
    if (matches.length !== 1) {
      diagnostics.push(attachmentDiagnostic(
        'attachment_ambiguous',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has an ambiguous owner attachment.`,
      ));
      continue;
    }
    const attachment = matches[0];
    referencedAttachments.add(attachment.carried_object_attachment_id);
    if (
      attachment.carried_object_attachment_revision
        !== placement.carried_object_attachment_revision
      || attachment.scene_resource_id !== object.scene_resource_id
      || attachment.carrier_frame_ref.controllable_component_id
        !== attachment.controllable_component_id
    ) {
      diagnostics.push(attachmentDiagnostic(
        'attachment_identity_mismatch',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} crosses its owner identity or revision.`,
      ));
      continue;
    }
    if (descriptor === null || sourceModel === null || manifest === null) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_descriptor_unavailable',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} is waiting for its retained carrier descriptor.`,
      ));
      continue;
    }
    if (descriptor.controllableComponentId !== attachment.controllable_component_id) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_target_mismatch',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} does not address the displayed carrier component.`,
      ));
      continue;
    }
    const sourceTargets = sourceModel.targets.filter((target) => (
      target.component_id === attachment.controllable_component_id
      && target.target_id === attachment.command_target_id
    ));
    if (sourceTargets.length !== 1) {
      diagnostics.push(attachmentDiagnostic(
        sourceTargets.length === 0 ? 'carrier_frame_missing' : 'carrier_frame_ambiguous',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has no singular retained carrier target.`,
      ));
      continue;
    }
    const sourceFrames = sourceTargets[0].frames.filter((frame) => (
      frame.model_instance_id === attachment.carrier_frame_ref.model_instance_id
      && frame.source_link_name !== null
    ));
    const descriptorTargets = sourceModel.targets.filter((target) => (
      target.component_id === descriptor.controllableComponentId
      && target.target_id === descriptor.commandTargetId
    ));
    if (sourceFrames.length === 0 || descriptorTargets.length !== 1) {
      diagnostics.push(attachmentDiagnostic(
        sourceFrames.length === 0 || descriptorTargets.length === 0
          ? 'carrier_frame_missing'
          : 'carrier_frame_ambiguous',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has no singular retained carrier frame.`,
      ));
      continue;
    }
    const descriptorFrames = descriptor.frames.filter((frame) => {
      if (frame.modelInstanceId !== attachment.carrier_frame_ref.model_instance_id) {
        return false;
      }
      const descriptorSourceFrames = descriptorTargets[0].frames.filter((sourceFrame) => (
        sourceFrame.frame_role_id === frame.frameRoleId
        && sourceFrame.model_instance_id === frame.modelInstanceId
        && sourceFrame.source_link_name !== null
      ));
      return descriptorSourceFrames.length === 1 && sourceFrames.some((sourceFrame) => (
        sourceFrame.model_instance_id === descriptorSourceFrames[0].model_instance_id
        && sourceFrame.source_link_name === descriptorSourceFrames[0].source_link_name
      ));
    });
    const uniqueDescriptorFrames = [...new Map(descriptorFrames.map((frame) => [
      `${frame.modelInstanceId}\u0000${frame.modelRevision}\u0000${frame.compiledFrameName}`,
      frame,
    ])).values()];
    if (uniqueDescriptorFrames.length !== 1) {
      diagnostics.push(attachmentDiagnostic(
        uniqueDescriptorFrames.length === 0
          ? 'carrier_frame_missing'
          : 'carrier_frame_ambiguous',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has no singular retained carrier frame.`,
      ));
      continue;
    }
    const carrierFrame = uniqueDescriptorFrames[0];
    const occurrence = sourceModel.occurrences.filter((candidate) => (
      candidate.component_id === attachment.controllable_component_id
      && candidate.model_instance_id === attachment.carrier_frame_ref.model_instance_id
    ));
    if (
      occurrence.length !== 1
      || occurrence[0].model_revision !== carrierFrame.modelRevision
    ) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_model_mismatch',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} crosses its retained carrier model revision.`,
      ));
      continue;
    }
    if (!viewerLinks.has(carrierFrame.compiledFrameName)) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_viewer_link_missing',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} carrier frame is absent from the active viewer model.`,
      ));
      continue;
    }
    objects.push({
      object,
      attachmentId: attachment.carried_object_attachment_id,
      attachmentRevision: attachment.carried_object_attachment_revision,
      carrierComponentId: attachment.controllable_component_id,
      commandTargetId: attachment.command_target_id,
      carrierFrameRoleId: attachment.carrier_frame_ref.frame_role_id,
      carrierCompiledFrameName: carrierFrame.compiledFrameName,
      poseInCarrierFrame: attachment.pose_in_carrier_frame,
    });
  }

  for (const attachment of snapshot.carried_object_attachments) {
    if (referencedAttachments.has(attachment.carried_object_attachment_id)) continue;
    diagnostics.push(attachmentDiagnostic(
      'attachment_orphaned',
      attachment.scene_resource_id,
      `Owner attachment ${attachment.carried_object_attachment_id} has no matching Attached object.`,
    ));
  }
  objects.sort((left, right) => left.object.scene_resource_id.localeCompare(
    right.object.scene_resource_id,
  ));
  diagnostics.sort((left, right) => (
    left.sceneResourceId.localeCompare(right.sceneResourceId)
    || left.code.localeCompare(right.code)
  ));
  return { objects, diagnostics };
}

type GoodObservationSource = Extract<
  SceneRuntimePoseSource,
  { quality: 'good' }
>;
type JointObservationSource = GoodObservationSource & {
  payload: Extract<
    SceneRuntimeGoodObservationPayload,
    { kind: 'joint_state' }
  >;
};
type PoseObservationSource = GoodObservationSource & {
  payload: Extract<
    SceneRuntimeGoodObservationPayload,
    { kind: 'simulation_entity_pose' | 'detection3d_pose' }
  >;
};

function isFreshGood(source: SceneRuntimePoseSource): source is GoodObservationSource {
  return source.quality === 'good' && source.fresh;
}

function isJointObservation(
  source: GoodObservationSource,
): source is JointObservationSource {
  return source.payload.kind === 'joint_state';
}

function isPoseObservation(
  source: GoodObservationSource,
): source is PoseObservationSource {
  return source.payload.kind === 'simulation_entity_pose'
    || source.payload.kind === 'detection3d_pose';
}

/**
 * Project fresh Manager-owned observations onto one retained viewer model.
 *
 * The retained viewer artifact currently carries no occurrence identity, so a
 * measured robot overlay is admitted only for a one-occurrence cell. A
 * multi-occurrence cell remains visible at retained-neutral coordinates and
 * reports the missing exact mapping instead of cross-driving lookalike models.
 */
export function runtimeScenePresentation(
  sceneRuntime: SceneRuntimeProjection | null,
  logicalScene: LogicalSceneProjection | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
  motionDescriptor: ManualControlMotionDescriptor | null = null,
): RuntimeScenePresentation {
  if (
    sceneRuntime === null
    || logicalScene === null
    || logicalScene.snapshot === null
    || sourceModel === null
    || manifest === null
  ) return EMPTY_PRESENTATION;

  const warnings: string[] = [];
  const attached = attachedScenePresentation(
    logicalScene,
    sourceModel,
    manifest,
    motionDescriptor,
  );
  warnings.push(...attached.diagnostics.map((diagnostic) => diagnostic.message));
  const identity = sceneRuntime.state.runtime_identity;
  const projection = logicalScene.access.active_projection;
  if (
    identity === null
    || projection === null
    || sceneRuntime.state.state !== 'open'
    || sceneRuntime.cell_id !== sourceModel.cell_id
    || sceneRuntime.cell_id !== logicalScene.snapshot.cell_id
    || sceneRuntime.cell_id !== manifest.cell_id
    || identity.retained_bundle_id !== sourceModel.retained_bundle_id
    || identity.graph_revision !== sourceModel.graph_revision
    || identity.retained_bundle_id !== manifest.retained_bundle_id
    || identity.graph_revision !== manifest.graph_revision
    || identity.retained_bundle_id !== projection.retained_bundle_id
    || identity.graph_revision !== projection.graph_revision
  ) {
    return {
      ...EMPTY_PRESENTATION,
      status: {
        ...EMPTY_PRESENTATION.status,
        warnings: ['Measured presentation identities do not match.'],
      },
    };
  }

  const sources = sceneRuntime.state.pose_sources;
  const fresh = sources.filter(isFreshGood);
  const measuredRobotSources = fresh.filter(
    (source): source is JointObservationSource => isJointObservation(source)
      && source.policy_id === 'viewer.measured_robot',
  );
  let measuredRobot: MeasuredRobotPresentation | null = null;
  if (measuredRobotSources.length > 1) {
    warnings.push('Measured robot observation is ambiguous.');
  } else if (measuredRobotSources.length === 1) {
    const source = measuredRobotSources[0];
    if (sourceModel.occurrences.length !== 1) {
      warnings.push(
        'Measured robot overlay requires an occurrence-keyed viewer artifact.',
      );
    } else {
      const occurrence = sourceModel.occurrences[0];
      const coordinateNames = new Set(
        occurrence.coordinates.map((coordinate) => coordinate.source_joint_name),
      );
      if (
        occurrence.component_id !== source.object_id
        || source.payload.joint_names.some((name) => !coordinateNames.has(name))
      ) {
        warnings.push('Measured robot observation does not match the retained occurrence.');
      } else {
        const positions = new Map(
          source.payload.joint_names.map((name, index) => [
            name,
            source.payload.positions[index],
          ]),
        );
        const linkTransforms = measuredLinkTransforms(manifest, positions);
        if (linkTransforms === null) {
          warnings.push('Measured robot coordinates do not resolve on the viewer model.');
        } else {
          measuredRobot = {
            objectId: source.object_id,
            sourceId: source.source_id,
            sourceEpoch: source.source_epoch,
            sourceSequence: source.source_sequence,
            observedAtNs: source.observed_at_ns,
            linkTransforms,
          };
        }
      }
    }
  }

  const sceneObjectIds = new Set(
    logicalScene.snapshot.objects.map((object) => object.scene_resource_id),
  );
  const attachedObjectIds = new Set(
    attached.objects.map((object) => object.object.scene_resource_id),
  );
  const objectSources = fresh.filter(
    (source): source is PoseObservationSource => isPoseObservation(source)
      && source.eligible
      && sceneObjectIds.has(source.object_id)
      && !attachedObjectIds.has(source.object_id),
  );
  const measuredObjects: MeasuredPosePresentation[] = [];
  for (const objectId of [...new Set(objectSources.map((source) => source.object_id))]
    .sort()) {
    const candidates = objectSources.filter((source) => source.object_id === objectId);
    if (candidates.length !== 1) {
      warnings.push(`Measured object ${objectId} has ambiguous pose policies.`);
      continue;
    }
    const source = candidates[0];
    measuredObjects.push({
      objectId,
      policyId: source.policy_id,
      sourceId: source.source_id,
      sourceEpoch: source.source_epoch,
      sourceSequence: source.source_sequence,
      observedAtNs: source.observed_at_ns,
      pose: source.payload.pose,
    });
  }

  const tcpSources = fresh.filter(
    (source): source is PoseObservationSource => isPoseObservation(source)
      && source.policy_id === 'viewer.measured_tcp',
  );
  let measuredTcp: MeasuredPosePresentation | null = null;
  if (tcpSources.length > 1) {
    warnings.push('Measured TCP observation is ambiguous.');
  } else if (tcpSources.length === 1) {
    const source = tcpSources[0];
    measuredTcp = {
      objectId: source.object_id,
      policyId: source.policy_id,
      sourceId: source.source_id,
      sourceEpoch: source.source_epoch,
      sourceSequence: source.source_sequence,
      observedAtNs: source.observed_at_ns,
      pose: source.payload.pose,
    };
  }

  return {
    measuredRobot,
    measuredTcp,
    measuredObjects,
    attachedObjects: attached.objects,
    attachmentDiagnostics: attached.diagnostics,
    status: {
      total: sources.length,
      freshGood: fresh.length,
      staleGood: sources.filter(
        (source) => source.quality === 'good' && !source.fresh,
      ).length,
      unknown: sources.filter((source) => source.quality === 'unknown').length,
      buffered: sources.filter((source) => source.buffered).length,
      warnings,
    },
  };
}

/**
 * Materialize only an exact, artifact-matched motion preview. The browser does
 * no planning or name inference; it evaluates the receipt's source joint
 * vector against the already-retained viewer model and otherwise shows no
 * robot ghost.
 */
export function candidateRobotPresentation(
  receipt: MotionPreviewReceipt,
  manifest: SceneUrdfManifest,
  descriptor: ManualControlMotionDescriptor,
  measured: Readonly<Record<string, RuntimeLinkTransform>>,
): CandidateRobotPresentation | null {
  if (
    receipt.retainedBundleId !== manifest.retained_bundle_id
    || receipt.graphRevision !== manifest.graph_revision
    || receipt.sourceJointNames.length !== receipt.finalPositionsRad.length
    || receipt.componentRoleId !== descriptor.componentRoleId
    || receipt.targetRoleId !== descriptor.targetRoleId
    || receipt.sourceJointNames.length !== descriptor.joints.length
    || new Set(receipt.sourceJointNames).size !== receipt.sourceJointNames.length
    || receipt.finalPositionsRad.some((value) => !Number.isFinite(value))
  ) return null;
  const positions = new Map<string, number>();
  for (const [index, joint] of descriptor.joints.entries()) {
    if (joint.sourceJointName !== receipt.sourceJointNames[index]
      || positions.has(joint.compiledJointName)) return null;
    positions.set(joint.compiledJointName, receipt.finalPositionsRad[index]);
  }
  const transforms = measuredLinkTransforms(
    manifest,
    positions,
    measured,
  );
  // Only the commanded chain and its carried tool are a candidate. Other
  // components retain their measured pose and must not acquire a neutral ghost.
  const changedCoordinates = new Set(positions.keys());
  const affected = new Set<string>();
  for (let pass = 0; pass < manifest.joints.length; pass += 1) {
    const count = affected.size + changedCoordinates.size;
    for (const joint of manifest.joints) {
      if (changedCoordinates.has(joint.name)
        || (joint.mimic !== null && changedCoordinates.has(joint.mimic.master))) {
        changedCoordinates.add(joint.name);
        affected.add(joint.child_link);
      }
      if (affected.has(joint.parent_link)) affected.add(joint.child_link);
    }
    if (count === affected.size + changedCoordinates.size) break;
  }
  return transforms === null ? null : {
    receiptDigest: receipt.receiptDigest,
    linkTransforms: Object.fromEntries(Object.entries(transforms)
      .filter(([link]) => affected.has(link))),
  };
}

/**
 * Materialize an exact capability-preview target on the selected tool only.
 * The backend receipt remains the command authority; the browser merely maps
 * its validated source coordinate through the retained model for display.
 */
export function candidateGripperPresentation(
  receipt: CapabilityCommandPreviewReceipt,
  status: ExecutionOwnerStatus,
  sourceModel: SourceModelProjection,
  manifest: SceneUrdfManifest,
  measured: Readonly<Record<string, RuntimeLinkTransform>>,
): CandidateRobotPresentation | null {
  const gripper = status.directControlGripper;
  if (
    gripper === null
    || status.cellId !== sourceModel.cell_id
    || status.cellId !== manifest.cell_id
    || receipt.retainedBundleId !== sourceModel.retained_bundle_id
    || receipt.retainedBundleId !== manifest.retained_bundle_id
    || receipt.graphRevision !== sourceModel.graph_revision
    || receipt.graphRevision !== manifest.graph_revision
    || receipt.componentRoleId !== gripper.componentRoleId
    || receipt.targetRoleId !== gripper.targetRoleId
    || receipt.requiredCapabilityId !== gripper.operationCapabilityId
    || receipt.commandSchemaId !== gripper.commandSchemaId
    || receipt.commandSchemaRevision !== gripper.commandSchemaRevision
    || receipt.sourceCoordinateName !== gripper.sourceCoordinateName
    || receipt.sourceKind !== gripper.sourceKind
    || receipt.coordinateKind !== gripper.coordinateKind
    || !Number.isFinite(receipt.targetPosition)
    || (receipt.minPosition !== null && receipt.targetPosition < receipt.minPosition)
    || (receipt.maxPosition !== null && receipt.targetPosition > receipt.maxPosition)
  ) return null;
  const targets = sourceModel.targets.filter((target) => (
    target.component_id === gripper.controllableComponentId
    && target.target_id === gripper.commandTargetId
  ));
  if (targets.length !== 1) return null;
  const groups = targets[0].model_groups.filter((group) => (
    group.commandable_source_joint_names.includes(receipt.sourceCoordinateName)
  ));
  if (groups.length !== 1) return null;
  const occurrences = sourceModel.occurrences.filter((occurrence) => (
    occurrence.component_id === gripper.controllableComponentId
    && occurrence.model_instance_id === groups[0].model_instance_id
  ));
  if (occurrences.length !== 1) return null;
  const occurrence = occurrences[0];
  const coordinates = occurrence.coordinates.filter((coordinate) => (
    coordinate.source_joint_name === receipt.sourceCoordinateName
  ));
  const coordinateKind = receipt.coordinateKind === 'angular_rad' ? 'angular' : 'linear';
  const unit = receipt.coordinateKind === 'angular_rad' ? 'radians' : 'meters';
  if (coordinates.length !== 1
    || coordinates[0].coordinate_kind !== coordinateKind
    || coordinates[0].unit !== unit) return null;
  const sourceLinks = new Set(targets[0].collision_bodies
    .filter((body) => body.model_instance_id === groups[0].model_instance_id)
    .flatMap((body) => body.source_link_names));
  if (sourceLinks.size === 0) return null;
  const viewerLinks = occurrence.links.filter((link) => sourceLinks.has(link.source_link_name));
  if (viewerLinks.length !== sourceLinks.size
    || new Set(viewerLinks.map((link) => link.source_link_name)).size !== sourceLinks.size) return null;
  const transforms = measuredLinkTransforms(
    manifest,
    new Map([[coordinates[0].viewer_joint_name, receipt.targetPosition]]),
    measured,
  );
  if (transforms === null) return null;
  const linkTransforms: Record<string, RuntimeLinkTransform> = {};
  for (const link of viewerLinks) {
    const transform = transforms[link.viewer_link_name];
    if (transform === undefined) return null;
    linkTransforms[link.viewer_link_name] = transform;
  }
  return {
    receiptDigest: receipt.receiptDigest,
    linkTransforms,
  };
}

function linkMatrix(transform: RuntimeLinkTransform): THREE.Matrix4 {
  return new THREE.Matrix4().compose(
    new THREE.Vector3(...transform.position_m),
    new THREE.Quaternion(...transform.orientation_xyzw).normalize(),
    new THREE.Vector3(1, 1, 1),
  );
}

/** An unvalidated tool target marker, never an IK solution or a motion receipt. */
export function candidateToolPresentation(
  poseInCell: Pose3Value,
  status: ExecutionOwnerStatus,
  sourceModel: SourceModelProjection,
  manifest: SceneUrdfManifest,
  measured: Readonly<Record<string, RuntimeLinkTransform>>,
): CandidateRobotPresentation | null {
  if (status.cellId !== sourceModel.cell_id || status.cellId !== manifest.cell_id
    || status.retainedBundleId !== sourceModel.retained_bundle_id
    || status.retainedBundleId !== manifest.retained_bundle_id
    || status.graphRevision !== sourceModel.graph_revision
    || status.graphRevision !== manifest.graph_revision) return null;
  // A carried object needs a motion target's TCP.
  const motion = status.directControl;
  if (motion === null) return null;
  const tcpFrames = motion.targetDescriptor.frames.filter(
    (frame) => frame.frameRoleId === motion.tcpFrameRoleId,
  );
  // An arm-only component carries nothing through a gripper.
  if (status.directControlGripper === null) return null;
  const gripper = status.directControlGripper.targetDescriptor;
  const targets = sourceModel.targets.filter((target) => (
    target.component_id === gripper.controllableComponentId
    && target.target_id === gripper.commandTargetId
  ));
  if (tcpFrames.length !== 1 || targets.length !== 1) return null;
  const tcp = measured[tcpFrames[0].compiledFrameName];
  if (tcp === undefined) return null;
  const instances = new Set(targets[0].model_groups.map((group) => group.model_instance_id));
  const links = sourceModel.occurrences.filter((occurrence) => (
    occurrence.component_id === gripper.controllableComponentId
    && instances.has(occurrence.model_instance_id)
  )).flatMap((occurrence) => occurrence.links);
  const mapped = gripper.touchLinks.map((sourceName) => links.filter(
    (link) => link.source_link_name === sourceName,
  ));
  if (mapped.length === 0 || mapped.some((matches) => matches.length !== 1)) return null;
  const delta = linkMatrix({
    position_m: [poseInCell.position_m.x_m, poseInCell.position_m.y_m, poseInCell.position_m.z_m],
    orientation_xyzw: [poseInCell.orientation.x, poseInCell.orientation.y, poseInCell.orientation.z, poseInCell.orientation.w],
  }).multiply(linkMatrix(tcp).invert());
  const linkTransforms: Record<string, RuntimeLinkTransform> = {};
  for (const [link] of mapped) {
    const observed = measured[link.viewer_link_name];
    if (observed === undefined || !manifest.links.some((row) => row.name === link.viewer_link_name)) return null;
    const matrix = delta.clone().multiply(linkMatrix(observed));
    const position = new THREE.Vector3().setFromMatrixPosition(matrix);
    const orientation = new THREE.Quaternion().setFromRotationMatrix(matrix).normalize();
    if (![...position, ...orientation].every(Number.isFinite)) return null;
    linkTransforms[link.viewer_link_name] = {
      position_m: [position.x, position.y, position.z],
      orientation_xyzw: [orientation.x, orientation.y, orientation.z, orientation.w],
    };
  }
  return { receiptDigest: null, linkTransforms };
}

export function measuredLinkTransforms(
  manifest: SceneUrdfManifest,
  observedPositions: ReadonlyMap<string, number>,
  baseline?: Readonly<Record<string, RuntimeLinkTransform>>,
): Record<string, RuntimeLinkTransform> | null {
  const linkNames = new Set(manifest.links.map((link) => link.name));
  const jointByName = new Map(manifest.joints.map((joint) => [joint.name, joint]));
  if (
    !linkNames.has(manifest.root_link)
    || [...observedPositions].some(([name, value]) => (
      !jointByName.has(name) || !Number.isFinite(value)
    ))
  ) return null;

  const coordinates = new Map<string, number>();
  const resolving = new Set<string>();
  const overridden = baseline === undefined ? undefined : new Set(observedPositions.keys());
  const coordinate = (joint: SceneUrdfJoint): number | null => {
    const cached = coordinates.get(joint.name);
    if (cached !== undefined) return cached;
    if (resolving.has(joint.name)) return null;
    resolving.add(joint.name);
    let value = observedPositions.get(joint.name)
      ?? joint.neutral_position
      ?? 0;
    if (joint.mimic !== null) {
      const master = jointByName.get(joint.mimic.master);
      const masterValue = master === undefined ? null : coordinate(master);
      if (masterValue === null) return null;
      value = (masterValue * joint.mimic.multiplier) + joint.mimic.offset;
      if (overridden?.has(joint.mimic.master)) overridden.add(joint.name);
    }
    resolving.delete(joint.name);
    if (!Number.isFinite(value)) return null;
    coordinates.set(joint.name, value);
    return value;
  };
  for (const joint of manifest.joints) {
    if (coordinate(joint) === null) return null;
  }

  const matrices = new Map<string, THREE.Matrix4>();
  if (baseline !== undefined && manifest.links.some((link) => baseline[link.name] === undefined)) {
    return null;
  }
  matrices.set(manifest.root_link, baseline === undefined
    ? new THREE.Matrix4().identity() : linkMatrix(baseline[manifest.root_link]));
  const pending = [...manifest.joints];
  for (let pass = 0; pass <= manifest.joints.length && pending.length > 0; pass += 1) {
    let progressed = false;
    for (let index = pending.length - 1; index >= 0; index -= 1) {
      const joint = pending[index];
      const parent = matrices.get(joint.parent_link);
      if (parent === undefined) continue;
      const motion = jointMotion(joint, coordinates.get(joint.name) ?? 0);
      if (motion === null || !linkNames.has(joint.child_link)) return null;
      const origin = new THREE.Matrix4().compose(
        new THREE.Vector3(...joint.origin.position_m),
        new THREE.Quaternion(...joint.origin.orientation_xyzw).normalize(),
        new THREE.Vector3(1, 1, 1),
      );
      const local = baseline !== undefined && !overridden?.has(joint.name)
        ? linkMatrix(baseline[joint.parent_link]).invert().multiply(linkMatrix(baseline[joint.child_link]))
        : origin.multiply(motion);
      matrices.set(
        joint.child_link,
        parent.clone().multiply(local),
      );
      pending.splice(index, 1);
      progressed = true;
    }
    if (!progressed) break;
  }
  if (pending.length > 0) return null;

  const result: Record<string, RuntimeLinkTransform> = {};
  for (const link of manifest.links) {
    const matrix = matrices.get(link.name);
    if (matrix === undefined) return null;
    const position = new THREE.Vector3();
    const orientation = new THREE.Quaternion();
    const scale = new THREE.Vector3();
    matrix.decompose(position, orientation, scale);
    if (![
      position.x,
      position.y,
      position.z,
      orientation.x,
      orientation.y,
      orientation.z,
      orientation.w,
    ].every(Number.isFinite)) return null;
    result[link.name] = {
      position_m: [position.x, position.y, position.z],
      orientation_xyzw: [
        orientation.x,
        orientation.y,
        orientation.z,
        orientation.w,
      ],
    };
  }
  return result;
}

function jointMotion(joint: SceneUrdfJoint, value: number): THREE.Matrix4 | null {
  const axis = new THREE.Vector3(...joint.axis);
  if (joint.type === 'revolute' || joint.type === 'continuous') {
    if (axis.lengthSq() < 1e-24) return null;
    return new THREE.Matrix4().makeRotationFromQuaternion(
      new THREE.Quaternion().setFromAxisAngle(axis.normalize(), value),
    );
  }
  if (joint.type === 'prismatic') {
    if (axis.lengthSq() < 1e-24) return null;
    axis.normalize().multiplyScalar(value);
    return new THREE.Matrix4().makeTranslation(axis.x, axis.y, axis.z);
  }
  if (joint.type === 'fixed') return new THREE.Matrix4().identity();
  return null;
}
