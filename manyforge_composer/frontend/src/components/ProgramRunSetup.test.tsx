import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import type { AppliedProgramSelection } from '../types/api';
import type { DirectControlMotionStatus } from '../types/robotControl';
import { ProgramRunSetup } from './ProgramRunSetup';

const motion = (id: string, role: string) => ({
  node_id: 'motion.move_to_joint_target',
  instance_id: id,
  parameters: { target_role: role },
  value_sources: {
    max_tcp_linear_speed_mps: { kind: 'program_parameter', reference: 'max_tcp_linear_speed_mps' },
  },
  children: [],
});

const applied = {
  programId: 'dual',
  scenarioId: 'dual_cell',
  programRevision: 'sha256:0123456789abcdef',
  program: {
    parameters: [{ parameter_id: 'max_tcp_linear_speed_mps', type: 'float64', default: 0.15, runtime_mutability: 'before_run' }],
    tree: {
      node_id: 'parallel', instance_id: 'root', parameters: {},
      children: [motion('a', 'lite6_arm'), motion('b', 'ur5e_arm')],
    },
  },
  programBinding: {
    // Exact rows; the generic component role alone cannot tell robots apart.
    target_role_bindings: [
      { target_role_id: 'lite6_arm', component_role_id: 'controlled_component', controllable_component_id: 'lite6_robot', command_target_id: 'arm' },
      { target_role_id: 'ur5e_arm', component_role_id: 'controlled_component', controllable_component_id: 'ur5e_robot', command_target_id: 'arm' },
    ],
  },
} as unknown as AppliedProgramSelection;

const target = (component: string, maximum: number, commandTarget = 'arm') => ({
  controllableComponentId: component,
  commandTargetId: commandTarget,
  componentRoleId: 'controlled_component',
  targetRoleId: 'command_target',
  targetDescriptor: { operationalLimit: { maximumTcpLinearSpeedMps: maximum } },
}) as unknown as DirectControlMotionStatus;

describe('ProgramRunSetup', () => {
  it('shows each robot with its Deployment maximum and the Program request', () => {
    const markup = renderToStaticMarkup(
      <ProgramRunSetup
        applied={applied}
        targets={[target('lite6_robot', 0.15), target('ur5e_robot', 0.25)]}
        groups={[{ groupId: 'all', nodeCount: 3, state: 'unavailable', velocity: '—' }]}
      />,
    );
    expect(markup).toContain('<td>lite6_arm</td><td>lite6_robot</td><td>0.15 m/s</td><td>0.15 m/s (default of max_tcp_linear_speed_mps)</td>');
    expect(markup).toContain('<td>ur5e_arm</td><td>ur5e_robot</td><td>0.25 m/s</td><td>0.15 m/s (default of max_tcp_linear_speed_mps)</td>');
    expect(markup).toContain('no active run');
  });

  it('matches exact command targets, not just the component', () => {
    const twoTargets = {
      ...applied,
      programBinding: {
        target_role_bindings: [
          { target_role_id: 'lite6_arm', component_role_id: 'controlled_component', controllable_component_id: 'cell', command_target_id: 'left' },
          { target_role_id: 'ur5e_arm', component_role_id: 'controlled_component', controllable_component_id: 'cell', command_target_id: 'right' },
        ],
      },
    } as unknown as AppliedProgramSelection;
    const markup = renderToStaticMarkup(
      <ProgramRunSetup
        applied={twoTargets}
        targets={[target('cell', 0.15, 'left'), target('cell', 0.25, 'right')]}
        groups={[]}
      />,
    );
    expect(markup).toContain('<td>lite6_arm</td><td>cell</td><td>0.15 m/s</td>');
    expect(markup).toContain('<td>ur5e_arm</td><td>cell</td><td>0.25 m/s</td>');
  });
});
