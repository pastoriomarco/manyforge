import { describe, expect, it } from 'vitest';

import type { AppliedProgramSelection, ProgramBindingDocument, ProgramTreeNode } from '../types/api';
import type { ExecutionOwnerStatus } from '../types/robotControl';
import { manualFillPreviewValues } from './manualFillPreview';
import { observedMoveToNodePreview } from './observedMoveToNode';

const armTarget = { component_id: 'robot', command_target_id: 'arm' };
const baseFrame = { component_id: 'robot', frame_id: 'base' };
const tcpFrame = { component_id: 'robot', frame_id: 'tcp' };
const tcpEntry = { target: armTarget, frame: tcpFrame };
const directTcpEntry = { componentId: 'robot', targetId: 'arm', frameComponentId: 'robot', frameRoleId: 'tcp' };
const binding = {
  target_bindings: [{
    target: armTarget,
    frame_references: [baseFrame, tcpFrame],
    robot_tcps: [{ entry: tcpEntry, frame: { reference: tcpFrame, compiled_frame_name: 'tool0',
      model_instance_id: 'model', model_revision: 'r1' }, end_effector: true, component_default: true }],
    default_tcp: tcpEntry,
  }],
  frame_bindings: [{ reference: baseFrame, compiled_frame_name: 'base',
    model_instance_id: 'model', model_revision: 'r1' }],
} as unknown as ProgramBindingDocument;
const applied = {
  graphRevision: 'graph',
  retainedBundleId: 'bundle',
  programBinding: binding,
} as AppliedProgramSelection;

function node(nodeId: ProgramTreeNode['node_id'], parameters: ProgramTreeNode['parameters']) {
  return { instance_id: 'move', node_id: nodeId, children: [], parameters,
    value_sources: { goal: { kind: 'program_parameter', reference: 'pose' } } } as ProgramTreeNode;
}

describe('observed move field preview', () => {
  it('maps current joints into the selected joint target and retains unrelated fields', () => {
    const status = {
      graphRevision: 'graph', retainedBundleId: 'bundle',
      directControlTargets: [{
        controllableComponentId: 'robot', commandTargetId: 'arm',
        operationCapabilityId: 'execution.trajectory.execute', targetDescriptor: { tcps: [], frames: [] },
      }],
      observedMotions: [{
        controllableComponentId: 'robot', commandTargetId: 'arm',
        complete: true, sourceJointNames: ['joint1', 'joint2'], positionsRad: [0.2, -0.4],
      }],
    } as unknown as ExecutionOwnerStatus;
    const selected = node('motion.move_to_joint_target', {
      target: armTarget, planner: 'keep', goal: [],
    });
    const preview = observedMoveToNodePreview(status, applied, selected);

    expect(preview.ok, preview.ok ? undefined : preview.reason).toBe(true);
    if (!preview.ok) return;
    expect(preview.referenceSummary).toBe('current joint positions');
    expect(preview.parameters).toEqual({
      target: armTarget, planner: 'keep', goal: [
        { joint_name: 'joint1', position_rad: 0.2 },
        { joint_name: 'joint2', position_rad: -0.4 },
      ],
    });
    expect(preview.mutation).toEqual({ instance_id: 'move', observed_kind: 'joint' });
  });

  it('previews the selected TCP in the Applied base frame and resolves relative fields to absolute', () => {
    const pose = {
      position_m: { x_m: 0.4, y_m: -0.2, z_m: 0.1 },
      orientation: { x: 0, y: 0, z: 0.5, w: 0.5 },
    };
    const status = {
      graphRevision: 'graph', retainedBundleId: 'bundle',
      directControlTargets: [{
        controllableComponentId: 'robot', commandTargetId: 'arm',
        operationCapabilityId: 'execution.trajectory.execute', baseFrameRoleId: 'base',
        baseFrame: { componentId: 'robot', frameId: 'base' },
        activeTcp: { entry: directTcpEntry },
        targetDescriptor: {
          frames: [{ frameComponentId: 'robot', frameRoleId: 'base', compiledFrameName: 'base' }],
          tcps: [{ entry: directTcpEntry, mountedHere: true, modelInstanceId: 'model', modelRevision: 'r1', compiledFrameName: 'tool0' }],
        },
      }],
      observedMotions: [{
        controllableComponentId: 'robot', commandTargetId: 'arm', complete: true,
        sourceJointNames: ['joint1'], positionsRad: [0.1],
        tcpPoses: { [JSON.stringify(['robot', 'arm', 'robot', 'tcp'])]: pose },
      }],
    } as unknown as ExecutionOwnerStatus;
    const selected = node('motion.move_to_pose', {
      target: armTarget, base_frame: baseFrame, tcp_endpoint: { kind: 'current' },
      target_mode: 'relative', target_reference: { type: 'cell_frame' },
      offset_reference: { type: 'cell_frame' }, goal: { position_m: [0, 0, 0] },
    });
    const preview = observedMoveToNodePreview(status, applied, selected);

    expect(preview.ok, preview.ok ? undefined : preview.reason).toBe(true);
    if (!preview.ok) return;
    expect(preview.referenceSummary).toBe('Active TCP');
    expect(preview.parameters).toEqual({
      target: armTarget, base_frame: baseFrame, tcp_endpoint: { kind: 'current' },
      target_mode: 'absolute',
      goal: {
        frame_ref: { kind: 'target_frame', frame: baseFrame },
        position_m: [0.4, -0.2, 0.1], orientation: { xyzw: [0, 0, 0.5, 0.5] },
      },
    });
    expect(preview.valueSources).toEqual({});
    const changes = manualFillPreviewValues(selected, preview.parameters, preview.valueSources);
    expect(changes.get('target_mode')).toBe('absolute');
    expect(changes.get('goal.frame_ref')).toBe('robot · base frame');
    expect(changes.get('goal.position_m[0]')).toBe('0.4');
  });

  it('resolves direct default-of-target and entry TCP selectors through the Applied binding', () => {
    const pose = { position_m: { x_m: 0.1, y_m: 0.2, z_m: 0.3 }, orientation: { x: 0, y: 0, z: 0, w: 1 } };
    const status = {
      graphRevision: 'graph', retainedBundleId: 'bundle',
      directControlTargets: [{
        controllableComponentId: 'robot', commandTargetId: 'arm',
        operationCapabilityId: 'execution.trajectory.execute', baseFrame: { componentId: 'robot', frameId: 'base' },
        activeTcp: null,
        targetDescriptor: {
          frames: [{ frameComponentId: 'robot', frameRoleId: 'base', compiledFrameName: 'base' }],
          tcps: [{ entry: directTcpEntry, mountedHere: true, modelInstanceId: 'model', modelRevision: 'r1', compiledFrameName: 'tool0' }],
        },
      }],
      observedMotions: [{
        controllableComponentId: 'robot', commandTargetId: 'arm', complete: true,
        sourceJointNames: ['joint1'], positionsRad: [0.1],
        tcpPoses: { [JSON.stringify(['robot', 'arm', 'robot', 'tcp'])]: pose },
      }],
    } as unknown as ExecutionOwnerStatus;
    const preview = (tcp_endpoint: ProgramTreeNode['parameters'][string]) => observedMoveToNodePreview(status, applied,
      node('motion.move_to_pose', { target: armTarget, base_frame: baseFrame, tcp_endpoint }));
    const byDefault = preview({ kind: 'default_of_target', target: armTarget });
    expect(byDefault.ok && byDefault.referenceSummary).toBe('robot · arm default TCP');
    const byEntry = preview({ kind: 'entry', target: armTarget, frame: tcpFrame });
    expect(byEntry.ok && byEntry.referenceSummary).toBe('robot · tcp');
    expect(preview({ kind: 'entry', target: armTarget, frame: { component_id: 'robot', frame_id: 'other' } }))
      .toEqual({ ok: false, reason: 'The selected TCP has no current pose on the Applied robot.' });
    expect(preview({ kind: 'current' }))
      .toEqual({ ok: false, reason: 'The selected TCP has no current pose on the Applied robot.' });
  });

  it('does not invent a pose when the Applied binding no longer matches the owner', () => {
    expect(observedMoveToNodePreview(
      { graphRevision: 'changed', retainedBundleId: 'bundle' } as ExecutionOwnerStatus,
      applied,
      node('motion.move_to_pose', { target: armTarget, base_frame: baseFrame }),
    )).toEqual({ ok: false, reason: 'The Applied target binding is unavailable.' });
  });
});
