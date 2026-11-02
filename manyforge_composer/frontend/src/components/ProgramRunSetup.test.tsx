import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import type { AppliedProgramSelection } from '../types/api';
import type { DirectControlMotionStatus } from '../types/robotControl';
import { parseProgramRunAdmittedSpeeds } from '../api/parsers';
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
    parameters: [{ parameter_id: 'max_tcp_linear_speed_mps', type: 'float64', default: 0.15 }],
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
        admitted={{ kind: 'no_run' }}
      />,
    );
    expect(markup).toContain('<h6>lite6_arm<span class="program-run-setup-component">lite6_robot</span></h6>');
    expect(markup).toContain('<span class="program-run-setup-stat-label">Deployment max</span><span class="program-run-setup-stat-value">0.25 m/s</span><span class="program-run-setup-stat-note">TCP speed limit</span>');
    expect(markup).toContain('<div class="program-run-setup-stat" title="No Program run, so no motion has been admitted.">');
    expect(markup).toContain('<span class="program-run-setup-stat-label">Program request</span><span class="program-run-setup-stat-value">0.15 m/s</span><span class="program-run-setup-stat-note">default of max_tcp_linear_speed_mps</span>');
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
        admitted={{ kind: 'loading' }}
      />,
    );
    expect(markup).toContain('<h6>lite6_arm<span class="program-run-setup-component">cell</span></h6>');
    expect(markup).toContain('<h6>ur5e_arm<span class="program-run-setup-component">cell</span></h6>');
  });

  it('shows only the owner-admitted speed per exact robot, with a reason when absent', () => {
    const speeds = parseProgramRunAdmittedSpeeds({
      schema_version: 'manyforge.program_run_admitted_speeds.v1',
      program_run_id: 'run-1',
      targets: [{
        controllable_component_id: 'lite6_robot',
        command_target_id: 'arm',
        operation_id: 'op-7',
        occurred_at_wall: '2026-10-04T10:00:00Z',
        requested_tcp_linear_speed_mps: 0.15,
        deployment_maximum_tcp_linear_speed_mps: 0.1,
        admitted_tcp_linear_speed_mps: 0.1,
      }],
    });
    const markup = renderToStaticMarkup(
      <ProgramRunSetup
        applied={applied}
        targets={[target('lite6_robot', 0.1), target('ur5e_robot', 0.25)]}
        groups={[]}
        admitted={{ kind: 'ready', speeds }}
      />,
    );
    expect(markup).toContain('<span class="program-run-setup-stat-label" title="TCP speed the execution owner admitted for this robot&#x27;s most recent prepared motion in this run (the request capped by the applied Deployment maximum), read from operation history. Motions not yet prepared have no admitted value; nothing is estimated here.">Last admitted</span><span class="program-run-setup-stat-value">0.1 m/s</span><span class="program-run-setup-stat-note">by the Kernel</span>');
    expect(markup).toContain('requested 0.15 m/s, Deployment maximum 0.1 m/s.">');
    // The UR5e has a Deployment maximum and a request but no prepared motion:
    // the GUI shows no estimate, only the reason.
    expect(markup).toMatch(/<div class="program-run-setup-stat" title="No motion of this robot has been prepared in this run yet[^"]*">/);
    expect(markup).not.toContain('Last admitted</span><span class="program-run-setup-stat-value">0.25 m/s');
  });

  it('rejects an admitted-speed response that is not exactly the owner rows', () => {
    expect(() => parseProgramRunAdmittedSpeeds({
      schema_version: 'manyforge.program_run_admitted_speeds.v1',
      program_run_id: 'run-1',
      targets: [{ controllable_component_id: 'lite6_robot', command_target_id: 'arm', admitted_tcp_linear_speed_mps: 0.1 }],
    })).toThrow();
  });
});
