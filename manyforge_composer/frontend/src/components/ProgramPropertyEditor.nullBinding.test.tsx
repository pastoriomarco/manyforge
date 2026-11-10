import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { statusFixture } from '../testFixtures/robotControl';
import { trialEDeployment, trialEProgram } from '../testFixtures/mockLite6SignalMoves';
import type {
  CatalogEntry, CatalogParameter, DeploymentWorkspaceResponse, JsonValue, ProgramTreeNode,
} from '../types/api';
import { IO_LIVENESS_UNPROVEN_WARNING } from '../utils/ioSignalConfiguration';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import type { ReferenceSelectorFacts } from '../utils/referenceValidation';

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

function render(
  node: ProgramTreeNode,
  catalog: CatalogEntry,
  extra: Partial<Parameters<typeof ProgramPropertyEditor>[0]> = {},
  document: Record<string, JsonValue> = trialEDeployment(),
): string {
  return renderToStaticMarkup(<ProgramPropertyEditor
    roots={[node]} selectedId={node.instance_id} catalogEntries={[catalog]}
    program={trialEProgram([node])} deploymentWorkspace={workspace(document)}
    busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()} {...extra}
  />);
}

describe('ProgramPropertyEditor without a compiled binding', () => {
  it('offers qualified direct targets and disables a target missing a required capability', () => {
    const catalog = entry('motion.move_to_pose', [param('target', { type: 'object', 'x-manyforge-semantic-type': 'target_reference' })]);
    catalog.requiredCapabilities = [{ capabilityId: 'execution.trajectory.execute', scope: 'target_binding', effectful: true, targetParameter: 'target' }];
    const node: ProgramTreeNode = { node_id: catalog.id, instance_id: 'direct', children: [], parameters: {} };
    const facts: ReferenceSelectorFacts = { components: [], rules: [], frames: [], bodies: [], signals: [], targets: [
      { target: { component_id: 'arm', command_target_id: 'motion' }, capabilities: [{ capability_id: 'execution.trajectory.execute', available: true, reason: null }], source_joint_names: [], tcp_entries: [], default_tcp: null, stop_routes: [] },
      { target: { component_id: 'io', command_target_id: 'port' }, capabilities: [], source_joint_names: [], tcp_entries: [], default_tcp: null, stop_routes: [] },
    ] };
    const html = render(node, catalog, { selectorFacts: facts });
    expect(html).toContain('arm · motion');
    expect(html).toContain('io · port · Missing execution.trajectory.execute');
    expect(html).toContain('disabled=""');
  });
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
    const catalog = entry('gripper.command_position', [param('target', { type: 'object', 'x-manyforge-semantic-type': 'target_reference' }), param('required_inputs', {
      type: 'array', maxItems: 4, items: { type: 'object', additionalProperties: false,
        required: ['input', 'value'], properties: { input: { type: 'string', enum: ['open', 'closed', 'pressure', 'workpiece_grasped'] }, value: { type: 'boolean' } } },
    })]);
    const node: ProgramTreeNode = { node_id: 'gripper.command_position', instance_id: 'close', children: [],
      parameters: { target: { component_id: 'lite6_robot', command_target_id: 'gripper' },
        required_inputs: [{ input: 'pressure', value: false }] } };
    const html = render(node, catalog, {}, deployment);
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
      parameters: { target: { component_id: 'lite6_robot', command_target_id: 'arm' }, max_tcp_linear_speed_mps: 0.25 } };
    const resolved = render(node, catalog, { motionDescriptors: [descriptor] });
    expect(resolved).toContain('Currently applied Deployment maximum for lite6_robot/arm: 0.15 m/s');
    expect(resolved).toContain('effective ceiling is 0.15 m/s');
    const missing = render({ ...node, parameters: { max_tcp_linear_speed_mps: 0.25 } }, catalog, { motionDescriptors: [descriptor] });
    expect(missing).toContain('Optional speed ceiling');
  });

  it('offers the qualified Stop stages of the resolved robot target', () => {
    const catalog = entry('motion.move_until_signal', [
      param('target', { type: 'object', 'x-manyforge-semantic-type': 'target_reference' }),
      param('stop_entry_stage_id'),
    ]);
    catalog.requiredCapabilities = [{ capabilityId: 'execution.trajectory.execute', scope: 'target_binding', effectful: true, targetParameter: 'target' }];
    const node: ProgramTreeNode = { node_id: 'motion.move_until_signal', instance_id: 'seek', children: [],
      parameters: { target: { component_id: 'lite6_robot', command_target_id: 'arm' } } };
    const facts: ReferenceSelectorFacts = { components: [], rules: [], signals: [], frames: [], bodies: [], targets: [{
      target: { component_id: 'lite6_robot', command_target_id: 'arm' }, capabilities: [], source_joint_names: [], tcp_entries: [], default_tcp: null,
      stop_routes: [{ capability_id: 'execution.trajectory.execute', control_binding_id: 'c', control_binding_revision: 'r', control_route_id: 'route', available: true, reason: null,
        stages: [{ stage_id: 'motion_controlled_stop', stage_kind: 'nominal', command_kind: 'controlled_stop', stop_action_class: 'controlled_stop' }] }],
    }] };
    const resolved = render(node, catalog, { selectorFacts: facts });
    expect(resolved).toContain('1. Nominal stop (controlled stop)');
    expect(render(node, catalog)).toContain('Qualified Draft target or capability is unavailable.');
    const failed = structuredClone(facts);
    failed.targets[0].stop_routes[0] = { ...failed.targets[0].stop_routes[0], available: false,
      reason: { code: 'route_unavailable', message: 'This route cannot stop safely.', details: {} }, stages: [] };
    expect(render(node, catalog, { selectorFacts: failed })).toContain('This route cannot stop safely.');
    const ambiguous = structuredClone(facts);
    ambiguous.targets[0].stop_routes.push({ ...ambiguous.targets[0].stop_routes[0], control_route_id: 'other' });
    expect(render(node, catalog, { selectorFacts: ambiguous })).toContain('Multiple Stop routes match this capability.');
  });

  it('offers a carried object only the touch bodies moving with the selected target', () => {
    const attach = entry('scene.attach_carried_object', [
      param('target', { type: 'object', 'x-manyforge-semantic-type': 'target_reference' }),
      param('touch_bodies', { type: 'array', minItems: 1,
        items: { type: 'object', 'x-manyforge-semantic-type': 'collision_body_reference' } }),
    ]);
    const arm = { component_id: 'robot', command_target_id: 'arm' };
    const gripper = { component_id: 'tool', command_target_id: 'gripper' };
    const body = (componentId: string, bodyId: string, moving: typeof arm[]) => (
      { reference: { component_id: componentId, collision_body_id: bodyId }, ambiguous: false, moving_targets: moving });
    const facts: ReferenceSelectorFacts = { components: [], rules: [], targets: [], signals: [], frames: [], bodies: [
      body('robot', 'arm_collision', [arm]),
      body('tool', 'finger', [arm, gripper]),
      body('tool', 'gripper_collision', [arm, gripper]),
    ] };
    const node: ProgramTreeNode = { node_id: attach.id, instance_id: 'attach', children: [],
      parameters: { target: gripper, touch_bodies: [{ component_id: 'tool', collision_body_id: 'gripper_collision' }] } };
    const scoped = render(node, attach, { selectorFacts: facts });
    expect(scoped).toContain('tool · gripper_collision');
    expect(scoped).toContain('tool · finger');
    expect(scoped).not.toContain('arm_collision');
    // The robot target also moves its mounted tool's bodies.
    const onArm = render({ ...node, parameters: { ...node.parameters, target: arm } }, attach, { selectorFacts: facts });
    expect(onArm).toContain('robot · arm_collision');
    expect(onArm).toContain('tool · finger');
  });

  it('offers io nodes the resolved signals, the no-signal hint and the liveness warning', () => {
    const wait = entry('io.wait_for_input', [param('signal', { type: 'object', 'x-manyforge-semantic-type': 'signal_reference' })]);
    const node: ProgramTreeNode = { node_id: 'io.wait_for_input', instance_id: 'wait', children: [],
      parameters: { signal: { component_id: 'station_io', signal_name: 'probe_contact' } } };
    const facts: ReferenceSelectorFacts = { components: [], rules: [], targets: [], frames: [], bodies: [], signals: [
      { reference: { component_id: 'station_io', signal_name: 'probe_contact' }, ambiguous: false, target: { component_id: 'station_io', command_target_id: 'gpio' }, value_type: 'bool', direction: 'input', transition_capability: null, update_mode: null, liveness: 'change_only', provider_profile: 'ros_bool' },
      { reference: { component_id: 'station_io', signal_name: 'clamp' }, ambiguous: false, target: { component_id: 'station_io', command_target_id: 'gpio' }, value_type: 'bool', direction: 'output', transition_capability: null, update_mode: null, liveness: null, provider_profile: 'ros_bool' },
    ] };
    const resolved = render(node, wait, { selectorFacts: facts });
    expect(resolved).toContain('station_io · probe_contact');
    expect(resolved).toContain('station_io · clamp · This node requires an input signal.');
    expect(resolved).toContain(IO_LIVENESS_UNPROVEN_WARNING.slice(0, 40));
    const missing = render(node, wait);
    expect(missing).toContain('unavailable in the current Draft candidate');
    expect(missing).not.toContain(IO_LIVENESS_UNPROVEN_WARNING.slice(0, 40));
    const clear = { ...node, node_id: 'io.clear_outside_change', parameters: {} };
    const clearCatalog = entry(clear.node_id, [param('signal', { type: 'object', 'x-manyforge-semantic-type': 'signal_reference' })]);
    expect(render(clear, clearCatalog, { selectorFacts: facts })).toContain('This output does not have a uniquely qualified changed-outside signal.');
  });
});
