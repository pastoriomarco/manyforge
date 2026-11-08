import type {
  AppliedProgramSelection,
  JsonValue,
  ProgramBindingDocument,
  ProgramFrameReference,
  ProgramTargetReference,
  ProgramTreeNode,
} from '../types/api';
import {
  tcpEntryIdentityKey,
  type DirectControlMotionStatus,
  type ExecutionOwnerStatus,
  type ObservedMotionTarget,
  type Pose3Value,
  type TcpEntryIdentity,
} from '../types/robotControl';
import type { ProgramTcpEntry } from '../types/programBindingTransport.generated';

type JsonObject = Record<string, JsonValue>;
type TargetBinding = ProgramBindingDocument['target_bindings'][number];

export type ObservedMoveToNodePreview =
  | { ok: false; reason: string }
  | {
    ok: true;
    kind: 'tcp_pose' | 'joint';
    referenceSummary: string;
    parameters: JsonObject;
    valueSources: Record<string, JsonValue>;
    mutation: { instance_id: string; observed_kind: 'tcp_pose' | 'joint' };
  };

function record(value: JsonValue | undefined): Record<string, JsonValue> {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function sameEntry(left: TcpEntryIdentity, right: TcpEntryIdentity): boolean {
  return tcpEntryIdentityKey(left) === tcpEntryIdentityKey(right);
}

function sameProgramEntry(left: ProgramTcpEntry, right: TcpEntryIdentity): boolean {
  return left.target.component_id === right.componentId
    && left.target.command_target_id === right.targetId
    && left.frame.component_id === right.frameComponentId
    && left.frame.frame_id === right.frameRoleId;
}

function targetReference(value: JsonValue | undefined): ProgramTargetReference | null {
  const reference = record(value);
  return Object.keys(reference).length === 2
    && typeof reference.component_id === 'string' && reference.component_id !== ''
    && typeof reference.command_target_id === 'string' && reference.command_target_id !== ''
    ? { component_id: reference.component_id, command_target_id: reference.command_target_id } : null;
}

function frameReference(value: JsonValue | undefined): ProgramFrameReference | null {
  const reference = record(value);
  return Object.keys(reference).length === 2
    && typeof reference.component_id === 'string' && reference.component_id !== ''
    && typeof reference.frame_id === 'string' && reference.frame_id !== ''
    ? { component_id: reference.component_id, frame_id: reference.frame_id } : null;
}

function sameTarget(left: ProgramTargetReference, right: ProgramTargetReference): boolean {
  return left.component_id === right.component_id && left.command_target_id === right.command_target_id;
}

function sameFrame(left: ProgramFrameReference, right: ProgramFrameReference): boolean {
  return left.component_id === right.component_id && left.frame_id === right.frame_id;
}

function boundTarget(binding: ProgramBindingDocument, target: ProgramTargetReference): TargetBinding | null {
  const bound = binding.target_bindings.filter((row) => sameTarget(row.target, target));
  return bound.length === 1 ? bound[0] : null;
}

function finitePose(value: Pose3Value | undefined): value is Pose3Value {
  if (value === undefined) return false;
  const numbers = [
    value.position_m.x_m,
    value.position_m.y_m,
    value.position_m.z_m,
    value.orientation.x,
    value.orientation.y,
    value.orientation.z,
    value.orientation.w,
  ];
  return numbers.every(Number.isFinite)
    && Math.hypot(value.orientation.x, value.orientation.y, value.orientation.z, value.orientation.w) > 1e-12;
}

function observedTargetFor(
  status: ExecutionOwnerStatus,
  direct: DirectControlMotionStatus,
): ObservedMotionTarget | undefined {
  const matches = status.observedMotions.filter((row) => (
    row.controllableComponentId === direct.controllableComponentId
    && row.commandTargetId === direct.commandTargetId
    && row.complete
  ));
  return matches.length === 1 ? matches[0] : undefined;
}

function directTargetFor(
  status: ExecutionOwnerStatus,
  binding: ProgramBindingDocument,
  reference: ProgramTargetReference,
): { direct: DirectControlMotionStatus; bindingTarget: TargetBinding } | null {
  const target = boundTarget(binding, reference);
  if (target === null) return null;
  const directs = status.directControlTargets.filter((row) => (
    row.controllableComponentId === target.target.component_id
    && row.commandTargetId === target.target.command_target_id
    && row.operationCapabilityId === 'execution.trajectory.execute'
  ));
  return directs.length === 1 ? { direct: directs[0], bindingTarget: target } : null;
}

function tcpEntryFor(
  selector: JsonValue | undefined,
  movingTarget: TargetBinding,
  binding: ProgramBindingDocument,
  direct: DirectControlMotionStatus,
  observed: ObservedMotionTarget,
): { entry: TcpEntryIdentity; summary: string } | null {
  const tcpSelector: Record<string, JsonValue> = selector === undefined
    ? { kind: 'current' } : record(selector);
  const kind = tcpSelector.kind;
  const selectorFields = kind === 'current' ? ['kind']
    : kind === 'default_of_target' ? ['kind', 'target']
      : kind === 'entry' ? ['kind', 'target', 'frame'] : [];
  if (selectorFields.length === 0
    || Object.keys(tcpSelector).length !== selectorFields.length
    || selectorFields.some((field) => !Object.hasOwn(tcpSelector, field))) return null;
  let requested: TcpEntryIdentity | null = null;
  let label = '';
  if (kind === 'current') {
    requested = direct.activeTcp?.entry ?? null;
    label = 'Active TCP';
  } else if (kind === 'default_of_target') {
    const owner = targetReference(tcpSelector.target);
    const target = owner === null ? null : boundTarget(binding, owner);
    const entry = target?.default_tcp;
    if (owner !== null && entry) {
      requested = { componentId: entry.target.component_id, targetId: entry.target.command_target_id,
        frameComponentId: entry.frame.component_id, frameRoleId: entry.frame.frame_id };
      label = `${owner.component_id} · ${owner.command_target_id} default TCP`;
    }
  } else if (kind === 'entry') {
    const owner = targetReference(tcpSelector.target);
    const frame = frameReference(tcpSelector.frame);
    const target = owner === null ? null : boundTarget(binding, owner);
    if (owner !== null && frame !== null && target !== null
      && target.frame_references.some((row) => sameFrame(row, frame))) {
      requested = { componentId: owner.component_id, targetId: owner.command_target_id,
        frameComponentId: frame.component_id, frameRoleId: frame.frame_id };
      label = `${frame.component_id} · ${frame.frame_id}`;
    }
  }
  if (requested === null) return null;

  const bindingTcp = (movingTarget.robot_tcps ?? []).find((row) => sameProgramEntry(row.entry, requested!));
  const directTcp = direct.targetDescriptor.tcps.find((row) => sameEntry(row.entry, requested!));
  if (bindingTcp === undefined || directTcp === undefined || !directTcp.mountedHere
    || directTcp.modelInstanceId !== bindingTcp.frame.model_instance_id
    || directTcp.modelRevision !== bindingTcp.frame.model_revision
    || directTcp.compiledFrameName !== bindingTcp.frame.compiled_frame_name) return null;
  const pose = observed.tcpPoses[tcpEntryIdentityKey(requested)];
  if (!finitePose(pose)) return null;
  return { entry: requested, summary: label };
}

function withoutGoalSources(valueSources: ProgramTreeNode['value_sources']): Record<string, JsonValue> {
  return Object.fromEntries(Object.entries(valueSources ?? {}).filter(([path]) => path !== 'goal'
    && !path.startsWith('goal.') && !path.startsWith('goal['))) as unknown as Record<string, JsonValue>;
}

/** Predict the selected observed-move action for hover feedback; the click still uses the owner mutation. */
export function observedMoveToNodePreview(
  status: ExecutionOwnerStatus | null,
  applied: AppliedProgramSelection | null,
  node: ProgramTreeNode | null,
): ObservedMoveToNodePreview {
  if (node === null || (node.node_id !== 'motion.move_to_pose'
    && node.node_id !== 'motion.move_to_joint_target')) {
    return { ok: false, reason: 'Select a Move to pose or Move to joint target node.' };
  }
  const kind = node.node_id === 'motion.move_to_pose' ? 'tcp_pose' : 'joint';
  const targetValue = targetReference(node.parameters.target);
  if (targetValue === null) {
    return { ok: false, reason: 'Choose the target first.' };
  }
  if (status === null || applied === null
    || applied.graphRevision !== status.graphRevision
    || applied.retainedBundleId !== status.retainedBundleId) {
    return { ok: false, reason: 'The Applied target binding is unavailable.' };
  }
  const target = directTargetFor(status, applied.programBinding, targetValue);
  if (target === null) {
    return { ok: false, reason: `Target ${targetValue.component_id} · ${targetValue.command_target_id} is unavailable in the Applied Program.` };
  }
  const observed = observedTargetFor(status, target.direct);
  if (observed === undefined) return { ok: false, reason: 'Current observed motion is unavailable for this target.' };

  let goal: JsonValue;
  let referenceSummary: string;
  const parameters: JsonObject = { ...node.parameters };
  if (kind === 'joint') {
    if (observed.sourceJointNames.length === 0
      || observed.sourceJointNames.length !== observed.positionsRad.length
      || new Set(observed.sourceJointNames).size !== observed.sourceJointNames.length
      || observed.positionsRad.some((value) => !Number.isFinite(value))) {
      return { ok: false, reason: 'Current joint positions are incomplete for this target.' };
    }
    goal = observed.sourceJointNames.map((joint_name, index) => ({
      joint_name,
      position_rad: observed.positionsRad[index],
    }));
    referenceSummary = 'current joint positions';
  } else {
    if (Object.keys(node.value_sources ?? {}).some((path) => (
      ['target_mode', 'target_reference', 'offset_reference'].some((field) => (
        path === field || path.startsWith(`${field}.`) || path.startsWith(`${field}[`)
      ))
    ))) {
      return { ok: false, reason: 'Pose preview is unavailable while target mode or reference fields are sourced.' };
    }
    const baseFrame = frameReference(node.parameters.base_frame);
    if (baseFrame === null) {
      return { ok: false, reason: 'Choose the base frame first.' };
    }
    const bindingBase = target.bindingTarget.frame_references.some((row) => sameFrame(row, baseFrame))
      ? applied.programBinding.frame_bindings.find((row) => sameFrame(row.reference, baseFrame)) : undefined;
    const directBase = target.direct.targetDescriptor.frames.find((row) => (
      row.frameComponentId === target.direct.baseFrame.componentId
      && row.frameRoleId === target.direct.baseFrame.frameId
    ));
    if (bindingBase === undefined || directBase === undefined
      || bindingBase.compiled_frame_name !== directBase.compiledFrameName) {
      return { ok: false, reason: 'Choose the robot’s base frame to capture its current pose.' };
    }
    const tcp = tcpEntryFor(node.parameters.tcp_endpoint, target.bindingTarget,
      applied.programBinding, target.direct, observed);
    if (tcp === null) {
      return { ok: false, reason: 'The selected TCP has no current pose on the Applied robot.' };
    }
    const pose = observed.tcpPoses[tcpEntryIdentityKey(tcp.entry)];
    if (!finitePose(pose)) return { ok: false, reason: 'Current TCP pose is unavailable.' };
    goal = {
      frame_ref: { kind: 'target_frame', frame: { ...baseFrame } },
      position_m: [pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m],
      orientation: { xyzw: [pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w] },
    };
    const previousMode = parameters.target_mode;
    for (const field of ['target_mode', 'target_reference', 'offset_reference']) delete parameters[field];
    // The owner removes target_mode on capture; the node's effective default is absolute.
    // Show that effective value only when the selected node currently differs from it.
    if (previousMode !== undefined && previousMode !== 'absolute') parameters.target_mode = 'absolute';
    referenceSummary = tcp.summary;
  }
  parameters.goal = goal;
  return {
    ok: true,
    kind,
    referenceSummary,
    parameters,
    valueSources: withoutGoalSources(node.value_sources),
    mutation: { instance_id: node.instance_id, observed_kind: kind },
  };
}
