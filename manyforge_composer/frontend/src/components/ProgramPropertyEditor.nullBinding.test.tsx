import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { statusFixture } from '../testFixtures/robotControl';
import { trialEAssignment, trialEDeployment, trialEProgram } from '../testFixtures/mockLite6SignalMoves';
import type {
  CatalogEntry, CatalogParameter, DeploymentWorkspaceResponse, JsonValue, ProgramTreeNode,
} from '../types/api';
import { IO_LIVENESS_UNPROVEN_WARNING } from '../utils/ioSignalConfiguration';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';

/** The workspace while editing a Draft: no compiled binding, the Draft document only. */
const workspace = (document: Record<string, JsonValue>) => (
  { binding: null, draft: { document } } as unknown as DeploymentWorkspaceResponse);

const param = (id: string, schema: CatalogParameter['schema'] = { type: 'string' }): CatalogParameter => (
  { id, required: false, schema, schemaHash: `sha256:${id}`, hasDefault: false });

const entry = (id: string, parameters: CatalogParameter[]): CatalogEntry => ({
  id, name: id, category: 'logic', description: id, stability: 'stable', kind: 'leaf',
  childrenMin: 0, childrenMax: 0, parameters, requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'program' },
  runners: { py_trees: id }, tags: [], minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] }, ownerContext: null, outcomes: {}, eventFamily: null,
});

const roleParams = [
  param('target_role', { type: 'string', 'x-manyforge-semantic-type': 'target_role' }),
  param('resource_role', { type: 'string', 'x-manyforge-semantic-type': 'resource_role' }),
];

/** The signal_moves Program with probe_contact as a change-only ROS Bool input. */
function changeOnlyProbeDeployment(): Record<string, JsonValue> {
  const document = trialEDeployment();
  const station = (document.components as Array<Record<string, JsonValue>>)[1];
  const rows = ((station.configuration as Record<string, JsonValue>).runtime_configuration as
    Record<string, JsonValue>).io_signals as Array<Record<string, JsonValue>>;
  const probe = rows.find((row) => row.resource_role_id === 'probe_contact')!;
  probe.provider = { profile: 'ros_bool', topic: '/probe', liveness: { mode: 'change_only' } };
  return document;
}

function render(
  node: ProgramTreeNode,
  catalog: CatalogEntry,
  assignment: Record<string, JsonValue> | null,
  extra: Partial<Parameters<typeof ProgramPropertyEditor>[0]> = {},
  document: Record<string, JsonValue> = trialEDeployment(),
): string {
  return renderToStaticMarkup(<ProgramPropertyEditor
    roots={[node]} selectedId={node.instance_id} catalogEntries={[catalog]}
    program={trialEProgram([node])} deploymentWorkspace={workspace(document)}
    programRoleAssignment={assignment}
    busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()} {...extra}
  />);
}

describe('ProgramPropertyEditor without a compiled binding', () => {
  it('offers gripper input keys from the selected target in the Deployment Draft', () => {
    const deployment = trialEDeployment();
    const robot = (deployment.components as Array<Record<string, JsonValue>>)[0];
    const configuration = (robot.configuration as Record<string, JsonValue>).runtime_configuration as Record<string, JsonValue>;
    configuration.gripper_target_configurations = [{
      controllable_component_id: 'lite6_robot', command_target_id: 'gripper',
      actuation_kind: 'pneumatic', output_mode: 'commanded', pneumatic_io: {
        outputs: [{ signal: { controllable_component_id: 'station_io', signal_name: 'clamp' }, open_value: false, close_value: true }],
        inputs: { workpiece_grasped: { signal: { controllable_component_id: 'station_io', signal_name: 'probe_contact' } } },
      },
    }];
    const catalog = entry('gripper.command_position', [param('target_role'), param('required_inputs', {
      type: 'array', maxItems: 4, items: { type: 'object', additionalProperties: false,
        required: ['input', 'value'], properties: { input: { type: 'string', enum: ['open', 'closed', 'pressure', 'workpiece_grasped'] }, value: { type: 'boolean' } } },
    })]);
    const node: ProgramTreeNode = { node_id: 'gripper.command_position', instance_id: 'close', children: [],
      parameters: { target_role: 'gripper', required_inputs: [{ input: 'pressure', value: false }] } };
    const html = render(node, catalog, trialEAssignment, {}, deployment);
    expect(html).toContain('Workpiece grasped');
    expect(html).toContain('Pressure (not declared for this gripper)');
    expect(html).toContain('read again to confirm');
    expect(html).not.toContain('Open expected value');
  });

  it('shows the Deployment speed maximum of the resolved robot target', () => {
    const descriptor = { ...statusFixture().directControl!.targetDescriptor,
      controllableComponentId: 'lite6_robot', commandTargetId: 'arm',
      operationalLimit: { maximumTcpLinearSpeedMps: 0.15 } };
    const catalog = entry('timer', [param('max_tcp_linear_speed_mps', { type: 'number', exclusiveMinimum: 0 })]);
    const node: ProgramTreeNode = { node_id: 'timer', instance_id: 'move', children: [],
      parameters: { target_role: 'arm', max_tcp_linear_speed_mps: 0.25 } };
    const resolved = render(node, catalog, trialEAssignment, { motionDescriptors: [descriptor] });
    expect(resolved).toContain('Currently applied Deployment maximum for lite6_robot/arm: 0.15 m/s');
    expect(resolved).toContain('effective ceiling is 0.15 m/s');
    const missing = render(node, catalog, null, { motionDescriptors: [descriptor] });
    expect(missing).toContain('Optional speed ceiling');
  });

  it('offers the qualified Stop stages of the resolved robot target', () => {
    const catalog = entry('motion.move_until_signal', [
      param('target_role', { type: 'string', 'x-manyforge-semantic-type': 'target_role' }),
      param('stop_entry_stage_id'),
    ]);
    const node: ProgramTreeNode = { node_id: 'motion.move_until_signal', instance_id: 'seek', children: [],
      parameters: { target_role: 'arm' } };
    const stopChains = [{ controllableComponentId: 'lite6_robot', commandTargetId: 'arm', stopResponse: {
      controlBindingId: 'c', controlBindingRevision: 'r', controlRouteId: 'route', nominalStopMode: null,
      stages: [{ stageId: 'motion_controlled_stop', stageKind: 'nominal', commandKind: 'controlled_stop' }],
    } }];
    const resolved = render(node, catalog, trialEAssignment, { stopChains });
    expect(resolved).toContain('1. Nominal stop (controlled stop)');
    expect(resolved).not.toContain('available after Apply');
    expect(render(node, catalog, null, { stopChains })).toContain('available after Apply');
  });

  it('offers io nodes the resolved signals, the no-signal hint and the liveness warning', () => {
    const wait = entry('io.wait_for_input', roleParams);
    const node: ProgramTreeNode = { node_id: 'io.wait_for_input', instance_id: 'wait', children: [],
      parameters: { target_role: 'station', resource_role: 'probe_contact' } };
    const resolved = render(node, wait, trialEAssignment, {}, changeOnlyProbeDeployment());
    expect(resolved).toContain('probe_contact · probe_contact (input)');
    expect(resolved).toContain('station input signals');
    expect(resolved).not.toContain('curtain_blocked');
    expect(resolved).toContain(IO_LIVENESS_UNPROVEN_WARNING.slice(0, 40));
    const missing = render(node, wait, null, {}, changeOnlyProbeDeployment());
    expect(missing).not.toContain('(input)');
    expect(missing).not.toContain(IO_LIVENESS_UNPROVEN_WARNING.slice(0, 40));
    // clamp is declared but not a required resource role of station.
    const setOutput = entry('io.set_output', [...roleParams, param('value', { type: 'boolean' })]);
    const output: ProgramTreeNode = { node_id: 'io.set_output', instance_id: 'clamp', children: [],
      parameters: { target_role: 'station', resource_role: 'clamp', value: true } };
    expect(render(output, setOutput, trialEAssignment))
      .toContain('The current binding has no output signal for station.');
    expect(render(output, setOutput, null)).not.toContain('The current binding has no output signal');
  });
});
