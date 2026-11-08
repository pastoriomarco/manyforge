import { describe, expect, it } from 'vitest';
import { initialRobotControlState } from '../state/robotControl';
import type { RobotControlState } from '../state/robotControl';
import type { ProgramBindingDocument, ProgramTreeNode } from '../types/api';
import type { ExecutionOwnerStatus } from '../types/robotControl';
import { manualPreviewToNode } from './manualPreviewToNode';
import { resolveStoredProgramPose } from './moveTo';

const entry = { componentId: 'tool', targetId: 'gripper', frameComponentId: 'tool', frameRoleId: 'tcp' };
const status = { graphRevision: 'graph-1', retainedBundleId: 'bundle-1', directControl: {
  controllableComponentId: 'robot', commandTargetId: 'arm',
  activeTcp: { entry }, targetDescriptor: {
    joints: [{ sourceJointName: 'joint1' }], tcps: [{ entry, isDefault: true }],
  },
} } as unknown as ExecutionOwnerStatus;
const binding = { target_bindings: [
  { target: { component_id: 'robot', command_target_id: 'arm' } },
  { target: { component_id: 'tool', command_target_id: 'gripper' } },
] } as unknown as ProgramBindingDocument;
const payload = { kind: 'motion', base_frame: { component_id: 'robot', frame_id: 'base' },
  speed_profile: { velocity_scale: 0.5, acceleration_scale: 0.25, max_tcp_linear_speed_mps: null },
  per_joint_dynamics_limits: { joint1: { max_velocity_rad_s: 2 } },
  motion: { kind: 'joint_goal', joint_names: ['joint1'], positions_rad: [0.75] },
};
const receipt = { schemaVersion: 'manyforge.direct_control_motion_preview.v4',
  graphRevision: 'graph-1', retainedBundleId: 'bundle-1',
  resolvedTcp: { entry: { target: { componentId: 'tool', commandTargetId: 'gripper' },
    frame: { componentId: 'tool', frameId: 'tcp' } },
  source: 'active_tcp', ownerEpoch: 'owner-1', registrationGeneration: 1, sequence: 1 },
};
const state = { ...initialRobotControlState, phase: 'ready', preview: receipt, status: {},
  intent: { participants: [{ payload }] },
} as unknown as RobotControlState;
const node = { node_id: 'motion.move_to_joint_target', instance_id: 'move', children: [],
  parameters: { target: { component_id: 'robot', command_target_id: 'arm' }, planner: 'keep', acceleration_scale: 0.3,
    per_joint_dynamics_limits: {
      joint1: { max_velocity_rad_s: 1, max_jerk_rad_s3: 9 }, other: { max_velocity_rad_s: 4 },
    } }, value_sources: { goal: { kind: 'parameter', name: 'old' }, planner: { kind: 'parameter', name: 'keep' },
    velocity_scale: { kind: 'program_parameter', reference: 'velocity_scale' },
    'per_joint_dynamics_limits.joint1.max_jerk_rad_s3': { kind: 'parameter', name: 'jerk' } },
} as unknown as ProgramTreeNode;
const withParams = (parameters: Record<string, unknown>) =>
  ({ ...node, parameters: { ...node.parameters, ...parameters } }) as unknown as ProgramTreeNode;
const params = (copy: ReturnType<typeof manualPreviewToNode>) =>
  (copy.ok ? copy.args.parameters : null) as Record<string, unknown> | null;
const toolEntry = { kind: 'entry', target: { component_id: 'tool', command_target_id: 'gripper' },
  frame: { component_id: 'tool', frame_id: 'tcp' } };

describe('copy manual preview to a Program node', () => {
  it('copies only the goal: speed, dynamics and their bindings stay as the node has them', () => {
    const copy = manualPreviewToNode(state, status, node, binding);
    expect(copy.ok).toBe(true);
    if (!copy.ok) return;
    expect(copy.args).toMatchObject({ instance_id: 'move', merge: false, parameters: {
      target: { component_id: 'robot', command_target_id: 'arm' },
      base_frame: { component_id: 'robot', frame_id: 'base' },
      goal: [{ joint_name: 'joint1', position_rad: 0.75 }], planner: 'keep', acceleration_scale: 0.3,
      per_joint_dynamics_limits: { joint1: { max_velocity_rad_s: 1, max_jerk_rad_s3: 9 }, other: { max_velocity_rad_s: 4 } },
    } });
    const parameters = copy.args.parameters as Record<string, unknown>;
    expect(parameters.tcp_endpoint).toBeUndefined();
    expect(copy.robotChange).toBeNull();
    expect(copy.notices).toEqual([{ tone: 'note', text: 'Uses the Active TCP at run time (now tool / tcp)' }]);
    expect(parameters.velocity_scale).toBeUndefined();
    expect(parameters.max_tcp_linear_speed_mps).toBeUndefined();
    expect(copy.args.value_sources).toEqual({ planner: { kind: 'parameter', name: 'keep' },
      velocity_scale: { kind: 'program_parameter', reference: 'velocity_scale' },
      'per_joint_dynamics_limits.joint1.max_jerk_rad_s3': { kind: 'parameter', name: 'jerk' } });
  });
  it('keeps an exact TCP entry the preview used and keeps a Current TCP dynamic', () => {
    const same = manualPreviewToNode(state, status, withParams({ tcp_endpoint: toolEntry }), binding);
    expect(params(same)?.tcp_endpoint).toEqual(toolEntry);
    expect(same.ok && same.notices).toEqual([]);
    const current = manualPreviewToNode(state, status, withParams({ tcp_endpoint: { kind: 'current' } }), binding);
    expect(params(current)?.tcp_endpoint).toEqual({ kind: 'current' });
    expect(current.ok && current.notices).toEqual([{ tone: 'note', text: 'Uses the Active TCP at run time (now tool / tcp)' }]);
  });
  it('transfers the preview TCP and warns when the node named another entry or the arm default', () => {
    const other = { kind: 'entry', target: { component_id: 'robot', command_target_id: 'arm' },
      frame: { component_id: 'robot', frame_id: 'flange' } };
    const changed = manualPreviewToNode(state, status, withParams({ tcp_endpoint: other }), binding);
    expect(params(changed)?.tcp_endpoint).toEqual(toolEntry);
    expect(changed.ok && changed.notices).toEqual([{ tone: 'warning', text: 'TCP changed: robot / flange → tool / tcp' }]);
    const fixedDefault = manualPreviewToNode(state, status, withParams({ tcp_endpoint:
      { kind: 'default_of_target', target: { component_id: 'robot', command_target_id: 'arm' } } }), binding);
    expect(params(fixedDefault)?.tcp_endpoint).toEqual({ kind: 'current' });
    expect(fixedDefault.ok && fixedDefault.notices)
      .toEqual([{ tone: 'warning', text: "TCP changed: arm's own default → Current (now tool / tcp)" }]);
  });
  it('asks before moving the node to another robot and warns about a changed base frame', () => {
    const otherRobot = manualPreviewToNode(state, status, withParams({
      target: { component_id: 'old', command_target_id: 'arm2' }, base_frame: { component_id: 'old', frame_id: 'base' },
    }), binding);
    expect(otherRobot.ok && otherRobot.robotChange).toEqual({ from: 'old · arm2', to: 'robot · arm' });
    expect(params(otherRobot)?.target).toEqual({ component_id: 'robot', command_target_id: 'arm' });
    expect(otherRobot.ok && otherRobot.notices[0])
      .toEqual({ tone: 'warning', text: 'Robot changed: old · arm2 → robot · arm' });
    expect(otherRobot.ok && otherRobot.notices.some((notice) => notice.text.startsWith('Base frame'))).toBe(false);
    const otherBase = manualPreviewToNode(state, status, withParams({
      base_frame: { component_id: 'robot', frame_id: 'table' } }), binding);
    expect(otherBase.ok && otherBase.robotChange).toBeNull();
    expect(otherBase.ok && otherBase.notices[0])
      .toEqual({ tone: 'warning', text: 'Base frame changed: robot / table → robot / base' });
  });
  it('refuses a preview without its TCP, from another Applied configuration, or with unbound targets', () => {
    const oldReceipt = { ...state, preview: { ...receipt, schemaVersion: 'manyforge.direct_control_motion_preview.v3' } } as unknown as RobotControlState;
    expect(manualPreviewToNode(oldReceipt, status, node, binding))
      .toEqual({ ok: false, reason: 'Preview again: this preview does not record its TCP.' });
    const reapplied = { ...status, graphRevision: 'graph-2' } as ExecutionOwnerStatus;
    expect(manualPreviewToNode(state, reapplied, node, binding))
      .toEqual({ ok: false, reason: 'Preview again: the Applied configuration changed since this preview.' });
    const robotOnly = { target_bindings: [binding.target_bindings[0]] } as unknown as ProgramBindingDocument;
    expect(manualPreviewToNode(state, status, node, robotOnly))
      .toEqual({ ok: false, reason: 'The preview TCP is not bound in the Applied Program.' });
    const toolOnly = { target_bindings: [binding.target_bindings[1]] } as unknown as ProgramBindingDocument;
    expect(manualPreviewToNode(state, status, node, toolOnly))
      .toEqual({ ok: false, reason: 'The preview target is not bound in the Applied Program.' });
  });
  it('keeps a relative Move To reference and reconstructs the local offset', () => {
    const posePayload = { ...payload, motion: { kind: 'pose_goal', goal: {
      position_m: { x_m: 1, y_m: 3, z_m: 0 }, orientation: { x: 0, y: 0, z: Math.SQRT1_2, w: Math.SQRT1_2 },
    } } };
    const relative = { ...state, mode: 'move_to', intent: { participants: [{ payload: posePayload }] },
      moveToDraft: { source: { kind: 'pose', provenance: { kind: 'relative',
        targetReference: { type: 'scene_resource', scene_resource_id: 'box' },
        offsetReference: { type: 'component_frame', frame: { component_id: 'robot', frame_id: 'rotated' } },
        targetPose: { position: [1, 2, 0], quaternion: [0, 0, 0, 1] },
        offsetPose: { position: [0, 0, 0], quaternion: [0, 0, Math.SQRT1_2, Math.SQRT1_2] },
      } } },
    } as unknown as RobotControlState;
    const poseNode = { ...node, node_id: 'motion.move_to_pose' };
    const copy = manualPreviewToNode(relative, status, poseNode, binding);
    expect(copy.ok).toBe(true);
    if (!copy.ok) return;
    const params = copy.args.parameters as Record<string, unknown> & {
      goal: { position_m: number[]; orientation: { xyzw: number[] } };
    };
    expect(params.target_mode).toBe('relative');
    expect(params.target_reference).toEqual({ type: 'scene_resource', scene_resource_id: 'box' });
    expect(params.offset_reference).toEqual({ type: 'component_frame', frame: { component_id: 'robot', frame_id: 'rotated' } });
    expect(params.goal.position_m[0]).toBeCloseTo(1);
    expect(params.goal.position_m[1]).toBeCloseTo(0);
    expect(params.goal.orientation.xyzw[3]).toBeCloseTo(1);
    const roundtrip = resolveStoredProgramPose(params, {
      baseFrame: { component_id: 'robot', frame_id: 'base' },
      sceneResourcePoses: new Map([['box', { position: [1, 2, 0], quaternion: [0, 0, 0, 1] }]]),
      framePoses: new Map([['robot/rotated', { position: [0, 0, 0], quaternion: [0, 0, Math.SQRT1_2, Math.SQRT1_2] }]]),
    });
    expect(roundtrip?.position[0]).toBeCloseTo(1);
    expect(roundtrip?.position[1]).toBeCloseTo(3);
    expect(roundtrip?.quaternion[2]).toBeCloseTo(Math.SQRT1_2);
    expect(roundtrip?.quaternion[3]).toBeCloseTo(Math.SQRT1_2);

  });
  it('writes an absolute pose goal in the preview base frame', () => {
    const posePayload = { ...payload, motion: { kind: 'pose_goal', goal: {
      position_m: { x_m: 0.1, y_m: 0.2, z_m: 0.3 }, orientation: { x: 0, y: 0, z: 0, w: 1 },
    } } };
    const absolute = { ...state, intent: { participants: [{ payload: posePayload }] } } as unknown as RobotControlState;
    const copy = manualPreviewToNode(absolute, status, { ...node, node_id: 'motion.move_to_pose' }, binding);
    expect(copy.ok).toBe(true);
    if (!copy.ok) return;
    expect((copy.args.parameters as Record<string, unknown>).goal).toEqual({
      frame_ref: { kind: 'target_frame', frame: { component_id: 'robot', frame_id: 'base' } },
      position_m: [0.1, 0.2, 0.3], orientation: { xyzw: [0, 0, 0, 1] },
    });
    expect((copy.args.parameters as Record<string, unknown>).target_mode).toBe('absolute');
  });
  it('refuses an incompatible selected node', () => {
    const copy = manualPreviewToNode(state, status, { ...node, node_id: 'motion.move_to_pose' }, binding);
    expect(copy.ok).toBe(false);
    if (!copy.ok) expect(copy.reason).toBe('Select a compatible node in the Program (joint target)');
    const missing = manualPreviewToNode(state, status, null, binding);
    expect(missing).toEqual({ ok: false, reason: 'Select a compatible node in the Program (joint target)' });
    const poseState = { ...state, intent: { participants: [{ payload: { ...payload, motion: { kind: 'pose_goal' } } }] } } as unknown as RobotControlState;
    expect(manualPreviewToNode(poseState, status, null, binding))
      .toEqual({ ok: false, reason: 'Select a compatible node in the Program (pose)' });
  });
  it('disables Cartesian copies with the approved reason', () => {
    expect(manualPreviewToNode({ ...state, mode: 'cartesian_path' }, status, node, binding))
      .toEqual({ ok: false, reason: "Cartesian moves can't be copied: only end poses, relative poses and joint targets can." });
    expect(manualPreviewToNode({ ...state, mode: 'move_to', moveToKind: 'pose', moveToPath: 'linear' }, status, node, binding))
      .toEqual({ ok: false, reason: "Cartesian moves can't be copied: only end poses, relative poses and joint targets can." });
  });
});
