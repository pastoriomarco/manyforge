import * as THREE from 'three';
import type { Nanoseconds } from '../types/nanoseconds';
import { selectedTcpOf } from './robotControl';

import type {
  CarriedObjectAttachment,
  LogicalSceneProjection,
  LogicalSceneObject,
  QuaternionXyzwTuple,
  SceneRuntimeCellPoseMeasurement,
  SceneRuntimeAttachedPoseReference,
  SceneRuntimePoseMeasurement,
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
  ExecutionObservationTarget,
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
  /** G2-b: links the measured ghost does not draw (no stream, or no data yet). */
  hiddenLinkNames?: readonly string[];
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
  /**
   * G2: the fresh outside measured robot sources (`viewer.measured_robot`).
   * With none, the measured ghost follows the execution owner's measured rows.
   */
  measuredRobotSources: JointObservationSource[];
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
  carrierModelInstanceId: string;
  carrierFrameRoleId: string;
  carrierCompiledFrameName: string;
  poseInCarrierFrame: CarriedObjectAttachment['pose_in_carrier_frame'];
  poseMeasurement: AttachedObjectPoseMeasurement | null;
}

export type AttachedObjectPoseMeasurement = SceneRuntimePoseMeasurement & {
  fresh: boolean;
  observedAtNs: Nanoseconds;
  sourceId: string;
  sourceEpoch: string;
  sourceSequence: number;
};

export interface AttachmentPresentationDiagnostic {
  code:
    | 'attachment_missing'
    | 'attachment_ambiguous'
    | 'attachment_identity_mismatch'
    | 'carrier_model_unavailable'
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
 * Viewer-joint positions of observed execution targets, each mapped through
 * its own target's model group onto its owner's occurrence. Null when any
 * row crosses its retained join.
 */
function observedTargetPositions(
  targets: readonly ExecutionObservationTarget[],
  sourceModel: SourceModelProjection,
): Map<string, number> | null {
  const compiledPositions = new Map<string, number>();
  for (const observed of targets) {
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
  return compiledPositions;
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
  const compiledPositions = observedTargetPositions(frame.targets, sourceModel);
  if (compiledPositions === null) return null;
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

/**
 * Render-only measured comparison robot from the same viewer-stream frame and
 * the same retained-model projection as the solid robot. Each measured row
 * replaces its commanded row; every other coordinate keeps the solid robot's
 * value, so the comparison differs only where measurement does. A frame with
 * no measured row has no comparison robot. With an outside measured source
 * (G2) it draws that source's robot and each other element's own state in the
 * frame instead. This reader never credits or rejects frames: the solid robot
 * reader owns the capacity-one stream.
 */
export function createMeasuredComparisonPresentationReader(
  reader: ExecutionObservationReader,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
  _measuredRobotSources: readonly JointObservationSource[] = [],
): MeasuredRobotPresentationReader {
  let cachedFrame: ExecutionObservationFrame | null | undefined;
  let cachedPresentation: MeasuredRobotPresentation | null = null;
  return {
    readMeasuredRobotPresentation: () => {
      const frame = reader.getLatestExecutionObservation();
      if (frame === cachedFrame && !outsideExpired(frame)) return cachedPresentation;
      cachedFrame = frame;
      cachedPresentation = null;
      const measuredRobotSources = measuredRobotSourcesOfFrame(sourceModel === null ? null : frameOfModel(frame, sourceModel));
      // G2 (C-G2): with an outside measured source the ghost follows it and
      // every other element's own state in this frame; more than one is off.
      if (measuredRobotSources.length > 0) {
        if (measuredRobotSources.length === 1 && sourceModel !== null && manifest !== null) {
          cachedPresentation = outsideMeasuredRobot(
            measuredRobotSources[0], frameOfModel(frame, sourceModel), sourceModel, manifest,
          ).presentation;
        }
        return cachedPresentation;
      }
      const measured = frame?.measuredTargets[0];
      if (frame === null || measured === undefined) return null;
      const sameTarget = (left: ExecutionObservationTarget, right: ExecutionObservationTarget) => (
        left.controllableComponentId === right.controllableComponentId
        && left.commandTargetId === right.commandTargetId
      );
      if (!frame.measuredTargets.every((row) => frame.targets.some((target) => sameTarget(row, target)))) {
        return null;
      }
      const presentation = executionObservationRobotPresentation(
        {
          ...frame,
          targets: frame.targets.map((target) => (
            frame.measuredTargets.find((row) => sameTarget(row, target)) ?? target
          )),
        },
        sourceModel,
        manifest,
        null,
        (BigInt(frame.receivedAtMonotonicUs) * 1_000n).toString(),
      );
      cachedPresentation = presentation === null ? null : {
        ...presentation,
        objectId: measured.controllableComponentId,
        sourceId: `measured:${measured.commandTargetId}`,
      };
      return cachedPresentation;
    },
    subscribeMeasuredRobotPresentation: reader.subscribeExecutionObservation,
    markMeasuredRobotPresentationApplied: () => undefined,
  };
}

/**
 * A reader frozen at one measured-robot presentation. A retained (not
 * verified) robot model keeps its last coherent pose while the control
 * transport is delayed or stale; it neither follows nor credits live samples.
 */
export function frozenMeasuredRobotPresentationReader(
  presentation: MeasuredRobotPresentation | null,
): MeasuredRobotPresentationReader {
  return {
    readMeasuredRobotPresentation: () => presentation,
    subscribeMeasuredRobotPresentation: () => () => undefined,
    markMeasuredRobotPresentationApplied: () => undefined,
  };
}

const EMPTY_PRESENTATION: RuntimeScenePresentation = {
  measuredRobot: null,
  measuredRobotSources: [],
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
 *
 * G4 (C-P5): the part is always drawn at its holder's frame, whatever Robot
 * control selects. The holder's target names the frame's source link, and
 * the holder's own occurrence maps that link to its viewer link; no
 * Robot-control descriptor takes part.
 */
export function attachedScenePresentation(
  logicalScene: LogicalSceneProjection | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
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
    if (sourceModel === null || manifest === null) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_model_unavailable',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} is waiting for the retained model of its holder.`,
      ));
      continue;
    }
    const frameRef = attachment.carrier_frame_ref;
    // The same join as the scene's collision world: the holder's command
    // target lists the frame by owner, model occurrence and role.
    const holderTargets = sourceModel.targets.filter((target) => (
      target.component_id === attachment.controllable_component_id
      && target.target_id === attachment.command_target_id
    ));
    const holderFrames = holderTargets.length !== 1 ? [] : holderTargets[0].frames.filter((frame) => (
      frame.frame_role_id === frameRef.frame_role_id
      && frame.model_instance_id === frameRef.model_instance_id
      && frame.source_link_name !== null
    ));
    if (holderTargets.length > 1 || holderFrames.length > 1) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_frame_ambiguous',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has no singular retained carrier frame.`,
      ));
      continue;
    }
    if (holderFrames.length === 0) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_frame_missing',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} has no singular retained carrier frame.`,
      ));
      continue;
    }
    const occurrences = sourceModel.occurrences.filter((candidate) => (
      candidate.model_instance_id === frameRef.model_instance_id
    ));
    if (occurrences.length !== 1 || occurrences[0].component_id !== attachment.controllable_component_id) {
      diagnostics.push(attachmentDiagnostic(
        'carrier_model_mismatch',
        object.scene_resource_id,
        `Attached object ${object.scene_resource_id} names a model its holder does not own.`,
      ));
      continue;
    }
    const links = occurrences[0].links.filter((link) => (
      link.source_link_name === holderFrames[0].source_link_name
    ));
    if (links.length !== 1 || !viewerLinks.has(links[0].viewer_link_name)) {
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
      carrierModelInstanceId: frameRef.model_instance_id,
      carrierFrameRoleId: frameRef.frame_role_id,
      carrierCompiledFrameName: links[0].viewer_link_name,
      poseInCarrierFrame: attachment.pose_in_carrier_frame,
      poseMeasurement: null,
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

type ResolvedPoseMeasurementSource = SceneRuntimePoseSource & {
  measured_against: SceneRuntimeAttachedPoseReference['measured_against']
    | SceneRuntimeCellPoseMeasurement['measured_against'];
  frame: SceneRuntimeAttachedPoseReference['frame'];
  current_offset: SceneRuntimeAttachedPoseReference['current_offset'];
};

function hasResolvedPoseMeasurement(source: SceneRuntimePoseSource): source is ResolvedPoseMeasurementSource {
  return source.payload_quality === 'good'
    && source.measured_against !== undefined
    && source.frame !== undefined
    && source.current_offset !== undefined;
}

function attachedPoseMeasurement(
  attached: AttachedObjectPresentation,
  sources: readonly SceneRuntimePoseSource[],
): AttachedObjectPoseMeasurement | null {
  const matches = sources.filter((source) => (
    source.object_id === attached.object.scene_resource_id
    && hasResolvedPoseMeasurement(source)
    && source.frame.controllable_component_id === attached.carrierComponentId
    && source.frame.model_instance_id === attached.carrierModelInstanceId
    && source.frame.frame_role_id === attached.carrierFrameRoleId
  ));
  const source = matches[0];
  if (matches.length !== 1 || source === undefined || !hasResolvedPoseMeasurement(source)) return null;
  // Night change 9: the row's policy_id is its observer binding, not the
  // apply policy, so it does not gate the measurement.
  if (source.measured_against === 'cell frame') {
    if (source.holder !== undefined) return null;
    return {
      measured_against: 'cell frame',
      frame: source.frame,
      current_offset: source.current_offset,
      ...(source.baseline_offset === undefined ? {} : { baseline_offset: source.baseline_offset }),
      ...(source.drift === undefined ? {} : { drift: source.drift }),
      fresh: source.fresh,
      observedAtNs: source.observed_at_ns,
      sourceId: source.source_id,
      sourceEpoch: source.source_epoch,
      sourceSequence: source.source_sequence,
    };
  }
  if (
    source.holder === undefined
    || source.holder.controllable_component_id !== attached.carrierComponentId
    || source.holder.command_target_id !== attached.commandTargetId
  ) return null;
  return {
    holder: source.holder,
    measured_against: source.measured_against,
    ...(source.source_header_frame_id === undefined
      ? {}
      : { source_header_frame_id: source.source_header_frame_id }),
    frame: source.frame,
    current_offset: source.current_offset,
    ...(source.baseline_offset === undefined ? {} : { baseline_offset: source.baseline_offset }),
    ...(source.drift === undefined ? {} : { drift: source.drift }),
    fresh: source.fresh,
    observedAtNs: source.observed_at_ns,
    sourceId: source.source_id,
    sourceEpoch: source.source_epoch,
    sourceSequence: source.source_sequence,
  };
}

type GoodObservationSource = Extract<
  SceneRuntimePoseSource,
  { payload_quality: 'good' }
>;
export type JointObservationSource = GoodObservationSource & {
  payload: Extract<
    SceneRuntimeGoodObservationPayload,
    { kind: 'joint_state' }
  >;
};
type PoseObservationSource = GoodObservationSource & {
  payload: Extract<
    SceneRuntimeGoodObservationPayload,
    {
      kind:
        | 'simulation_entity_pose'
        | 'simulation_entity_pose_with_references.v1'
        | 'detection3d_pose';
    }
  >;
};

function isFreshGood(source: SceneRuntimePoseSource): source is GoodObservationSource {
  return source.payload_quality === 'good' && source.fresh;
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
    || source.payload.kind === 'simulation_entity_pose_with_references.v1'
    || source.payload.kind === 'detection3d_pose';
}

/** The execution frame when it belongs to this retained model, else null. */
function frameOfModel(
  frame: ExecutionObservationFrame | null,
  sourceModel: SourceModelProjection,
): ExecutionObservationFrame | null {
  return frame !== null
    && frame.cellId === sourceModel.cell_id
    && frame.retainedBundleId === sourceModel.retained_bundle_id
    && frame.graphRevision === sourceModel.graph_revision
    ? frame
    : null;
}

/** The fresh outside measured robot sources (`viewer.measured_robot`). */
function outsideExpired(frame: ExecutionObservationFrame | null): boolean {
  if (frame?.outsideReceivedAtMonotonicMs === undefined) return false;
  const elapsed = Math.max(0, performance.now() - frame.outsideReceivedAtMonotonicMs) * 1_000_000;
  return (frame.outsidePoseSources ?? []).some((row) =>
    row.ageAtDeliveryNs + elapsed > row.freshnessBudgetNs);
}

function measuredRobotSourcesOfFrame(frame: ExecutionObservationFrame | null): JointObservationSource[] {
  const elapsed = frame?.outsideReceivedAtMonotonicMs === undefined ? 0
    : Math.max(0, performance.now() - frame.outsideReceivedAtMonotonicMs) * 1_000_000;
  return (frame?.outsidePoseSources ?? [])
    .filter((row) => row.ageAtDeliveryNs + elapsed <= row.freshnessBudgetNs)
    .map((row) => row.source).filter(isFreshGood).filter(
    (source): source is JointObservationSource => isJointObservation(source)
      && source.policy_id === 'viewer.measured_robot',
  );
}

const MEASURED_ROBOT_REQUIRES_EXECUTION = 'Measured robot overlay requires execution.';
const AMBIGUOUS_MEASURED_ROBOT = 'More than one measured robot source is configured; the measured overlay is off.';

function hiddenUntilJointState(componentId: string): string {
  return `Parts of ${componentId} are hidden in the measured overlay until their joint state arrives.`;
}

/**
 * Source joint to viewer joint over the measured robot's own occurrences;
 * null when the source does not match them.
 */
function measuredRobotJointMap(
  source: JointObservationSource,
  sourceModel: SourceModelProjection,
): Map<string, string> | null {
  const robotOccurrences = sourceModel.occurrences.filter(
    (occurrence) => occurrence.component_id === source.object_id,
  );
  const viewerJointNames = new Map(robotOccurrences.flatMap(
    (occurrence) => occurrence.coordinates.map((coordinate) => [
      coordinate.source_joint_name, coordinate.viewer_joint_name,
    ] as const),
  ));
  return robotOccurrences.length === 0
    || source.payload.joint_names.some((name) => !viewerJointNames.has(name))
    ? null
    : viewerJointNames;
}

/**
 * Each component's own state in the frame; the measured rows replace their
 * commanded rows, as in the measured comparison robot.
 */
function ownStatePositions(
  frame: ExecutionObservationFrame | null,
  sourceModel: SourceModelProjection,
): Map<string, number> {
  return (frame === null ? null : observedTargetPositions(
    frame.targets.map((target) => frame.measuredTargets.find((row) => (
      row.controllableComponentId === target.controllableComponentId
      && row.commandTargetId === target.commandTargetId
    )) ?? target),
    sourceModel,
  )) ?? new Map<string, number>();
}

/**
 * Viewer-joint positions of the measured overlay: each other component's own
 * state, and the measured robot's joints only from the outside sample (never
 * its frame row, which may be commanded).
 */
function measuredPositions(
  source: JointObservationSource,
  viewerJointNames: ReadonlyMap<string, string>,
  ownState: ReadonlyMap<string, number>,
): Map<string, number> {
  const positions = new Map(ownState);
  for (const viewerName of viewerJointNames.values()) positions.delete(viewerName);
  for (const [index, name] of source.payload.joint_names.entries()) {
    positions.set(viewerJointNames.get(name)!, source.payload.positions[index]);
  }
  return positions;
}

/** Components with a movable joint of their own that is not measured (G2-b). */
function componentsAwaitingJointState(
  positions: ReadonlyMap<string, number>,
  sourceModel: SourceModelProjection,
): string[] {
  return [...new Set(sourceModel.occurrences.flatMap((occurrence) => (
    occurrence.component_id !== null
      && occurrence.coordinates.some((coordinate) => (
        coordinate.joint_type !== 'fixed'
        && coordinate.mimic === null
        && !positions.has(coordinate.viewer_joint_name)
      ))
      ? [occurrence.component_id]
      : []
  )))];
}

/**
 * G2 (C-G2, G2-b): the measured overlay with one outside measured source.
 * The source drives its robot's own occurrences; every other component that
 * streams joints takes them from its own state in the execution owner's frame
 * (measured rows where present). Only those elements are drawn, and a link
 * only when every movable joint between it and the root is measured (a mimic
 * joint follows its master): links downstream of a missing joint, of a
 * component with no stream, and of static fixtures are hidden, never drawn at
 * neutral as if measured. Each component with a missing joint of its own gets
 * one warning line; the presentation still draws everything else.
 */
function outsideMeasuredRobot(
  source: JointObservationSource,
  frame: ExecutionObservationFrame | null,
  sourceModel: SourceModelProjection,
  manifest: SceneUrdfManifest,
): { presentation: MeasuredRobotPresentation | null; warnings: string[] } {
  const viewerJointNames = measuredRobotJointMap(source, sourceModel);
  if (viewerJointNames === null) {
    return {
      presentation: null,
      warnings: ['The measured robot data does not match this cell\'s robot model; the measured overlay is off.'],
    };
  }
  const ownState = ownStatePositions(frame, sourceModel);
  const positions = measuredPositions(source, viewerJointNames, ownState);
  const warnings = componentsAwaitingJointState(positions, sourceModel).map(hiddenUntilJointState);
  const linkTransforms = measuredLinkTransforms(manifest, positions);
  if (linkTransforms === null) {
    return {
      presentation: null,
      warnings: warnings.length > 0 ? warnings : ['Measured robot coordinates do not resolve on the viewer model.'],
    };
  }
  // G2-b: only the measured robot and the components with own state stream.
  const drawn = new Set([source.object_id, ...sourceModel.occurrences.flatMap((occurrence) => (
    occurrence.component_id !== null
      && occurrence.coordinates.some((coordinate) => ownState.has(coordinate.viewer_joint_name))
      ? [occurrence.component_id]
      : []
  ))]);
  const drawnLinks = new Set(sourceModel.occurrences.flatMap((occurrence) => (
    occurrence.component_id !== null && drawn.has(occurrence.component_id)
      ? occurrence.links.map((link) => link.viewer_link_name)
      : []
  )));
  // The FK above resolved this tree, so the walk to the root terminates.
  const jointByName = new Map(manifest.joints.map((joint) => [joint.name, joint]));
  const parentJoint = new Map(manifest.joints.map((joint) => [joint.child_link, joint]));
  const jointMeasured = (joint: SceneUrdfJoint): boolean => (joint.mimic !== null
    ? jointMeasured(jointByName.get(joint.mimic.master)!)
    : joint.type === 'fixed' || positions.has(joint.name));
  const chainMeasured = (linkName: string): boolean => {
    const joint = parentJoint.get(linkName);
    return joint === undefined || (jointMeasured(joint) && chainMeasured(joint.parent_link));
  };
  return {
    presentation: {
      objectId: source.object_id,
      sourceId: source.source_id,
      sourceEpoch: source.source_epoch,
      sourceSequence: source.source_sequence,
      observedAtNs: source.observed_at_ns,
      linkTransforms,
      hiddenLinkNames: manifest.links.map((link) => link.name)
        .filter((name) => !drawnLinks.has(name) || !chainMeasured(name)),
    },
    warnings,
  };
}

/**
 * The measured-overlay lines that follow the owner frames, as
 * `runtimeScenePresentation` words them: execution is required with no live
 * position at all, and each component waiting for its joint state is hidden.
 * No FK; cheap enough per frame.
 */
function measuredOverlayFrameWarnings(
  measuredRobotSources: readonly JointObservationSource[],
  executionFrame: ExecutionObservationFrame | null,
  sourceModel: SourceModelProjection,
): string[] {
  const frame = frameOfModel(executionFrame, sourceModel);
  if (measuredRobotSources.length === 0) return frame === null ? [MEASURED_ROBOT_REQUIRES_EXECUTION] : [];
  if (measuredRobotSources.length > 1) return [AMBIGUOUS_MEASURED_ROBOT];
  const source = measuredRobotSources[0];
  const viewerJointNames = measuredRobotJointMap(source, sourceModel);
  if (viewerJointNames === null) return [];
  return componentsAwaitingJointState(
    measuredPositions(source, viewerJointNames, ownStatePositions(frame, sourceModel)), sourceModel,
  ).map(hiddenUntilJointState);
}

/**
 * G2: the panel's frame-dependent overlay lines, read through the owner frame
 * subscription. The result keeps its identity until the lines change, so a
 * steady stream does not re-render the panel.
 */
export function createMeasuredOverlayFrameWarningsReader(
  reader: ExecutionObservationReader,
  _sceneRuntime: SceneRuntimeProjection | null,
  sourceModel: SourceModelProjection | null,
): {
  readFrameWarnings: () => readonly string[];
  subscribeFrameWarnings: (listener: () => void) => () => void;
} {
  let cached: readonly string[] = [];
  return {
    readFrameWarnings: () => {
      const next = sourceModel === null ? [] : measuredOverlayFrameWarnings(
        measuredRobotSourcesOfFrame(frameOfModel(reader.getLatestExecutionObservation(), sourceModel)),
        reader.getLatestExecutionObservation(), sourceModel,
      );
      if (next.length !== cached.length || next.some((line, index) => line !== cached[index])) {
        cached = next;
      }
      return cached;
    },
    subscribeFrameWarnings: reader.subscribeExecutionObservation,
  };
}

/**
 * Project fresh Manager-owned observations onto one retained viewer model.
 *
 * G2 (C-G2): the outside measured source (`viewer.measured_robot`) drives the
 * joints of its robot's own occurrences. Every other component's joints come
 * from that component's own state, the execution owner's latest frame
 * (measured rows where present); a component with a movable joint but no own
 * state holds `measuredRobot` back rather than being drawn at neutral as if
 * measured. The measured ghost follows the same pose per frame and draws each
 * element that has data (`createMeasuredComparisonPresentationReader`). With
 * no live position at all the overlay requires execution.
 */
export function runtimeScenePresentation(
  sceneRuntime: SceneRuntimeProjection | null,
  logicalScene: LogicalSceneProjection | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
  executionFrame: ExecutionObservationFrame | null = null,
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
  const attachedObjects = attached.objects.map((item) => ({
    ...item,
    poseMeasurement: attachedPoseMeasurement(item, sources),
  }));
  const fresh = sources.filter(isFreshGood);
  const measuredRobotSources = measuredRobotSourcesOfFrame(
    sourceModel === null ? null : frameOfModel(executionFrame, sourceModel),
  );
  const frame = frameOfModel(executionFrame, sourceModel);
  let measuredRobot: MeasuredRobotPresentation | null = null;
  if (measuredRobotSources.length > 1) {
    warnings.push(AMBIGUOUS_MEASURED_ROBOT);
  } else if (measuredRobotSources.length === 0) {
    if (frame === null) warnings.push(MEASURED_ROBOT_REQUIRES_EXECUTION);
  } else {
    // The solid model takes the overlay only once every element has data.
    const outside = outsideMeasuredRobot(measuredRobotSources[0], frame, sourceModel, manifest);
    if (outside.warnings.length === 0) measuredRobot = outside.presentation;
    warnings.push(...outside.warnings);
  }

  const sceneObjectIds = new Set(
    logicalScene.snapshot.objects.map((object) => object.scene_resource_id),
  );
  const objectSources = fresh.filter(
    (source): source is PoseObservationSource => isPoseObservation(source)
      && sceneObjectIds.has(source.object_id),
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
    measuredRobotSources,
    measuredTcp,
    measuredObjects,
    attachedObjects,
    attachmentDiagnostics: attached.diagnostics,
    status: {
      total: sources.length,
      freshGood: fresh.length,
      staleGood: sources.filter(
        (source) => source.payload_quality === 'good' && !source.fresh,
      ).length,
      unknown: sources.filter((source) => source.payload_quality === 'unknown').length,
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
  // A manual preview is planned through the manual-control binding whose
  // roles the descriptor carries.
  if (
    receipt.componentRoleId !== descriptor.componentRoleId
    || receipt.targetRoleId !== descriptor.targetRoleId
  ) return null;
  return resolvedPreviewRobotPresentation(receipt, manifest, descriptor, measured);
}

/**
 * The robot ghost of a receipt whose binding identity the caller already
 * resolved to this physical target's descriptor, for example a Program move
 * resolved through the Applied Program binding, whose role ids need not be
 * the manual-control binding's. The artifact and joint checks still apply.
 */
export function resolvedPreviewRobotPresentation(
  receipt: MotionPreviewReceipt,
  manifest: SceneUrdfManifest,
  descriptor: ManualControlMotionDescriptor,
  measured: Readonly<Record<string, RuntimeLinkTransform>>,
): CandidateRobotPresentation | null {
  if (
    receipt.retainedBundleId !== manifest.retained_bundle_id
    || receipt.graphRevision !== manifest.graph_revision
    || receipt.sourceJointNames.length !== receipt.finalPositionsRad.length
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
  // TCP-C: the selected TCP (the robot's or its mounted tool's).
  const selectedTcp = selectedTcpOf(motion);
  const tcpFrames = selectedTcp === null ? [] : [selectedTcp];
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
