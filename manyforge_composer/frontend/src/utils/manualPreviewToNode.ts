import { Quaternion, Vector3 } from 'three';
import { robotControlPreviewApprovable } from '../state/robotControl';
import type { RobotControlState } from '../state/robotControl';
import type { JsonValue, ProgramBindingDocument, ProgramTreeNode } from '../types/api';
import type { ExecutionOwnerStatus, Pose3Value } from '../types/robotControl';
import { hasBoundProgramTarget } from './moveTo';
import { parseValuePath } from './valuePaths';

type Path = (string | number)[];
/** What the copy changes or keeps, shown in the Fill from box. */
export interface ManualFillNotice { tone: 'warning' | 'note'; text: string }
export type ManualPreviewNodeCopy =
  | { ok: false; reason: string }
  | {
    ok: true;
    args: Record<string, JsonValue>;
    notices: ManualFillNotice[];
    /** Set when the node moves another robot: copying needs the operator's confirmation. */
    robotChange: { from: string; to: string } | null;
  };

type TargetRef = { component_id: string; command_target_id: string };
type FrameRef = { component_id: string; frame_id: string };

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}
function targetRef(value: JsonValue | undefined): TargetRef | null {
  const row = record(value);
  return row && typeof row.component_id === 'string' && typeof row.command_target_id === 'string'
    ? { component_id: row.component_id, command_target_id: row.command_target_id } : null;
}
function frameRef(value: JsonValue | undefined): FrameRef | null {
  const row = record(value);
  return row && typeof row.component_id === 'string' && typeof row.frame_id === 'string'
    ? { component_id: row.component_id, frame_id: row.frame_id } : null;
}
const targetLabel = (target: TargetRef) => `${target.component_id} · ${target.command_target_id}`;
const frameLabel = (frame: FrameRef) => `${frame.component_id} / ${frame.frame_id}`;
const sameTarget = (a: TargetRef, b: TargetRef) =>
  a.component_id === b.component_id && a.command_target_id === b.command_target_id;
const sameFrame = (a: FrameRef, b: FrameRef) =>
  a.component_id === b.component_id && a.frame_id === b.frame_id;

function overlaps(a: Path, b: Path): boolean {
  return a.slice(0, Math.min(a.length, b.length)).every((part, index) => part === b[index]);
}
function absolutePose(pose: Pose3Value, baseFrame: { component_id: string; frame_id: string }): JsonValue {
  return {
    frame_ref: { kind: 'target_frame', frame: { ...baseFrame } },
    position_m: [pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m],
    orientation: { xyzw: [pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w] },
  };
}

/** One ordinary Draft edit from the currently shown successful preview. */
export function manualPreviewToNode(
  state: RobotControlState,
  selectedStatus: ExecutionOwnerStatus | null,
  node: ProgramTreeNode | null,
  binding: ProgramBindingDocument | null,
): ManualPreviewNodeCopy {
  if (state.mode === 'cartesian_path'
      || (state.mode === 'move_to' && state.moveToKind === 'pose' && state.moveToPath === 'linear')) {
    return { ok: false, reason: "Cartesian moves can't be copied: only end poses, relative poses and joint targets can." };
  }
  if (!robotControlPreviewApprovable(state) || state.poseDragging || state.intent === null) {
    return { ok: false, reason: 'Show a successful motion preview first.' };
  }
  const payload = state.intent.participants[0].payload;
  if (payload.kind !== 'motion') return { ok: false, reason: 'Choose a motion preview.' };
  const jointMove = payload.motion.kind === 'joint_goal';
  const expectedNode = jointMove ? 'motion.move_to_joint_target' : 'motion.move_to_pose';
  if (node === null || node.node_id !== expectedNode) {
    return { ok: false, reason: `Select a compatible node in the Program (${jointMove ? 'joint target' : 'pose'})` };
  }
  const motion = selectedStatus?.directControl;
  if (!motion || binding === null) return { ok: false, reason: 'The applied target binding is unavailable.' };
  const receipt = state.preview;
  if (receipt === null || receipt.schemaVersion !== 'manyforge.direct_control_motion_preview.v4') {
    return { ok: false, reason: 'Preview again: this preview does not record its TCP.' };
  }
  if (receipt.graphRevision !== selectedStatus?.graphRevision
    || receipt.retainedBundleId !== selectedStatus?.retainedBundleId) {
    return { ok: false, reason: 'Preview again: the Applied configuration changed since this preview.' };
  }
  if (!hasBoundProgramTarget(binding, motion.controllableComponentId, motion.commandTargetId)) {
    return { ok: false, reason: 'The preview target is not bound in the Applied Program.' };
  }
  // The manual move is the design: the node takes its robot, base frame and
  // TCP meaning; what changes is said, and a robot change is confirmed.
  const target = { component_id: motion.controllableComponentId, command_target_id: motion.commandTargetId };
  const previewTcpTarget = {
    component_id: receipt.resolvedTcp.entry.target.componentId,
    command_target_id: receipt.resolvedTcp.entry.target.commandTargetId,
  };
  const previewTcpFrame = {
    component_id: receipt.resolvedTcp.entry.frame.componentId,
    frame_id: receipt.resolvedTcp.entry.frame.frameId,
  };
  if (!hasBoundProgramTarget(binding, previewTcpTarget.component_id, previewTcpTarget.command_target_id)) {
    return { ok: false, reason: 'The preview TCP is not bound in the Applied Program.' };
  }
  const params: Record<string, JsonValue> = { ...node.parameters };
  const copied: Path[] = [];
  const notices: ManualFillNotice[] = [];
  const set = (name: string, value: JsonValue) => { params[name] = value; copied.push([name]); };
  const clear = (name: string) => { delete params[name]; copied.push([name]); };
  const nodeTarget = targetRef(node.parameters.target);
  const robotChange = nodeTarget !== null && !sameTarget(nodeTarget, target)
    ? { from: targetLabel(nodeTarget), to: targetLabel(target) } : null;
  if (robotChange) notices.push({ tone: 'warning', text: `Robot changed: ${robotChange.from} → ${robotChange.to}` });
  set('target', target);
  const nodeBase = frameRef(node.parameters.base_frame);
  if (robotChange === null && nodeBase !== null && !sameFrame(nodeBase, payload.base_frame)) {
    notices.push({ tone: 'warning', text: `Base frame changed: ${frameLabel(nodeBase)} → ${frameLabel(payload.base_frame)}` });
  }
  set('base_frame', { ...payload.base_frame });
  const previewTcp = frameLabel(previewTcpFrame);
  const nodeTcp = record(node.parameters.tcp_endpoint);
  const nodeTcpFrame = frameRef(nodeTcp?.frame);
  if (nodeTcp?.kind === 'entry') {
    const nodeTcpTarget = targetRef(nodeTcp.target);
    if (nodeTcpTarget === null || nodeTcpFrame === null
      || !sameTarget(nodeTcpTarget, previewTcpTarget) || !sameFrame(nodeTcpFrame, previewTcpFrame)) {
      set('tcp_endpoint', { kind: 'entry', target: { ...previewTcpTarget }, frame: { ...previewTcpFrame } });
      notices.push({ tone: 'warning',
        text: `TCP changed: ${nodeTcpFrame === null ? 'unset' : frameLabel(nodeTcpFrame)} → ${previewTcp}` });
    }
  } else if (nodeTcp?.kind === 'default_of_target') {
    set('tcp_endpoint', { kind: 'current' });
    notices.push({ tone: 'warning', text: `TCP changed: arm's own default → Current (now ${previewTcp})` });
  } else {
    notices.push({ tone: 'note', text: `Uses the Active TCP at run time (now ${previewTcp})` });
  }
  // Only the goal is copied: speed and dynamics stay as the node has them.
  if (payload.motion.kind === 'joint_goal') {
    const { joint_names, positions_rad } = payload.motion;
    set('goal', joint_names.map((joint_name, index) => ({ joint_name, position_rad: positions_rad[index] })));
    set('target_mode', 'absolute');
  } else if (payload.motion.kind === 'pose_goal') {
    const pose = payload.motion.goal;
    const provenance = state.mode === 'move_to' && state.moveToDraft?.source.kind === 'pose'
      ? state.moveToDraft.source.provenance : undefined;
    if (state.mode === 'move_to' && provenance === undefined) {
      return { ok: false, reason: 'The Move To reference is unavailable.' };
    }
    if (provenance?.kind === 'relative') {
      const reference = new Quaternion(...provenance.offsetPose.quaternion).normalize().invert();
      const position = new Vector3(pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m)
        .sub(new Vector3(...provenance.targetPose.position)).applyQuaternion(reference);
      const orientation = reference.clone().multiply(new Quaternion(
        pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w,
      )).normalize();
      set('goal', { position_m: [position.x, position.y, position.z],
        orientation: { xyzw: [orientation.x, orientation.y, orientation.z, orientation.w] } });
      set('target_mode', 'relative');
      set('target_reference', provenance.targetReference);
      if (provenance.offsetReference === undefined) clear('offset_reference');
      else set('offset_reference', provenance.offsetReference);
    } else {
      set('goal', absolutePose(pose, payload.base_frame));
      set('target_mode', 'absolute');
      clear('target_reference');
      clear('offset_reference');
    }
  } else {
    return { ok: false, reason: "Cartesian moves can't be copied: only end poses, relative poses and joint targets can." };
  }
  const sources = Object.fromEntries(Object.entries(node.value_sources ?? {}).filter(([key]) => {
    const path = parseValuePath(key);
    return path === null || !copied.some((field) => overlaps(path, field));
  }));
  return { ok: true, notices, robotChange, args: { instance_id: node.instance_id, expected_node_id: node.node_id,
    merge: false, parameters: params, value_sources: sources as unknown as JsonValue } };
}
