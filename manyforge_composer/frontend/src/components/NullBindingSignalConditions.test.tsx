import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { reactPropsHost } from '../test/reactPropsHost';
import {
  TRIAL_E_CONDITION, trialEAssignment, trialEDeployment, trialEProgram,
} from '../testFixtures/trialESignal';
import type {
  CatalogEntry, DeploymentWorkspaceResponse, JsonValue, ProgramTreeNode,
} from '../types/api';
import type { CarriedObjectLostSignalChoice } from '../utils/systemSignalAuthoring';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';

afterEach(() => vi.unstubAllGlobals());

const base: Omit<CatalogEntry, 'id' | 'parameters'> = {
  name: 'Node', category: 'motion', description: 'Node.', stability: 'stable',
  kind: 'leaf', childrenMin: 0, childrenMax: 0, requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'kernel.operation' },
  runners: { py_trees: 'node_v1' }, tags: ['motion'],
  minProgramProfile: 'composable-cell-v1', blackboard: { reads: [], writes: [] },
  ownerContext: null, outcomes: {}, eventFamily: null,
};
const targetRole = {
  id: 'target_role', required: true,
  schema: { type: 'string', 'x-manyforge-semantic-type': 'target_role' },
  schemaHash: 'sha256:t', hasDefault: false,
};
const until: CatalogEntry = { ...base, id: 'motion.move_until_signal', parameters: [
  targetRole,
  { id: 'interrupt_condition', required: true,
    schema: { type: 'object', 'x-manyforge-condition-grammar': 'interrupt_condition' },
    schemaHash: 'sha256:c', hasDefault: false },
] };
const probeEntry: CatalogEntry = { ...base, id: 'motion.probe_search', parameters: [
  targetRole,
  { id: 'stop_mode', required: true,
    schema: { type: 'string', enum: ['spring_back_hold', 'soft_on_path', 'hard_deceleration', 'spring_back_fast'] },
    schemaHash: 'sha256:m', hasDefault: false },
] };

// The live Composer workspace: a Deployment Draft and no compiled binding.
const workspace = (document: Record<string, JsonValue>) => (
  { draft: { document }, binding: null } as unknown as DeploymentWorkspaceResponse
);

const searchTrip: ProgramTreeNode = {
  node_id: 'motion.move_until_signal', instance_id: 'search_trip', children: [],
  parameters: { target_role: 'arm', interrupt_condition: TRIAL_E_CONDITION },
};

async function mountSearchTrip(assignment: Record<string, JsonValue> | null) {
  const { host, find, all } = reactPropsHost();
  // Card panel layout: no frame is ever run and no observer fires here.
  class Observer { observe() {} disconnect() {} }
  vi.stubGlobal('requestAnimationFrame', () => 0);
  vi.stubGlobal('cancelAnimationFrame', () => undefined);
  vi.stubGlobal('ResizeObserver', Observer);
  vi.stubGlobal('MutationObserver', Observer);
  const onInvalidEditChange = vi.fn();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ProgramPropertyEditor
    roots={[searchTrip]} selectedId="search_trip" catalogEntries={[until]}
    program={trialEProgram([searchTrip])} deploymentWorkspace={workspace(trialEDeployment())}
    programRoleAssignment={assignment} busy={false} onMutate={vi.fn()}
    onUpdateNodePolicies={vi.fn()} onInvalidEditChange={onInvalidEditChange}
  />));
  return { find, all, onInvalidEditChange, unmount: () => act(() => root.unmount()) };
}

describe('interrupt conditions with binding null', () => {
  it('resolves the Trial E signal from the Draft facts: no invalid edit, operand listed', async () => {
    const mounted = await mountSearchTrip(trialEAssignment);
    try {
      expect(mounted.onInvalidEditChange).not.toHaveBeenCalledWith(true);
      expect(mounted.all((props) => props.role === 'alert')).toEqual([]);
      const operand = mounted.find((props) => props['aria-label'] === 'condition operand');
      expect(operand.value).toBe(JSON.stringify(
        { signal: { resource_role: 'probe_contact', target_role: 'station' } },
      ));
    } finally { await mounted.unmount(); }
    const markup = renderToStaticMarkup(<ProgramPropertyEditor
      roots={[searchTrip]} selectedId="search_trip" catalogEntries={[until]}
      program={trialEProgram([searchTrip])} deploymentWorkspace={workspace(trialEDeployment())}
      programRoleAssignment={trialEAssignment} busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()}
    />);
    expect(markup).toContain('station · probe_contact (probe_contact, input)');
    expect(markup).not.toContain('is not a bound input signal');
  });

  it('keeps the current refusal without a role assignment', async () => {
    const mounted = await mountSearchTrip(null);
    try {
      expect(mounted.onInvalidEditChange).toHaveBeenLastCalledWith(true);
    } finally { await mounted.unmount(); }
  });

  it('lists the generated system input for an assigned required role before Apply', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_until_signal', instance_id: 'watch_loss', children: [],
      parameters: { target_role: 'gripper', interrupt_condition: {
        compare: { operand: { signal: { target_role: 'gripper', resource_role: 'carried_object_lost' } },
          op: '==', value: true },
      } },
    };
    const program = {
      ...trialEProgram([node]),
      target_roles: { gripper: { component_role: 'tool', required_resource_roles: ['carried_object_lost'] } },
    };
    const assignment = {
      component_roles: [{ component_role_id: 'tool', controllable_component_id: 'robot' }],
      target_roles: [{ target_role_id: 'gripper', command_target_id: 'gripper' }],
    } as unknown as Record<string, JsonValue>;
    const systemSignal: CarriedObjectLostSignalChoice = {
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', resourceRoleId: 'carried_object_lost',
      direction: 'input', valueType: 'bool', provenance: 'last_apply',
    };
    const markup = renderToStaticMarkup(<ProgramPropertyEditor
      roots={[node]} selectedId="watch_loss" catalogEntries={[until]} program={program}
      deploymentWorkspace={workspace(trialEDeployment())} programRoleAssignment={assignment}
      systemSignalChoices={[systemSignal]} busy={false} onMutate={vi.fn()}
      onUpdateNodePolicies={vi.fn()} />);

    expect(markup).toContain('gripper · carried_object_lost (carried_object_lost.gripper, input) · from last Apply');
    expect(markup).not.toContain('is not a bound input signal');
  });

  it('tells the user to Apply when the current Draft has no deterministic signal table', () => {
    const markup = renderToStaticMarkup(<ProgramPropertyEditor
      roots={[searchTrip]} selectedId="search_trip" catalogEntries={[until]}
      program={trialEProgram([searchTrip])} deploymentWorkspace={workspace(trialEDeployment())}
      programRoleAssignment={trialEAssignment} systemSignalsNeedApply busy={false}
      onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()} />);

    expect(markup).toContain('Apply to list system signals.');
  });

  it('shows the probe Stop warning through the same resolver', () => {
    const probe: ProgramTreeNode = {
      node_id: 'motion.probe_search', instance_id: 'probe', children: [],
      parameters: { target_role: 'arm', stop_mode: 'soft_on_path' },
    };
    const render = (assignment: Record<string, JsonValue> | null) => renderToStaticMarkup(<ProgramPropertyEditor
      roots={[probe]} selectedId="probe" catalogEntries={[probeEntry]} program={trialEProgram([probe])}
      deploymentWorkspace={workspace(trialEDeployment(true))} programRoleAssignment={assignment}
      busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()}
    />);
    expect(render(trialEAssignment))
      .toContain('This probe may finish a short remaining path instead of stopping on contact.');
    expect(render(null)).not.toContain('This probe may finish a short remaining path');
  });
});
