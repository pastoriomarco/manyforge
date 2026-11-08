import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { reactPropsHost } from '../test/reactPropsHost';
import {
  TRIAL_E_CONDITION, trialEDeployment, trialEProgram,
} from '../testFixtures/mockLite6SignalMoves';
import type {
  CatalogEntry, DeploymentWorkspaceResponse, JsonValue, ProgramTreeNode,
} from '../types/api';
import type { ReferenceSelectorFacts } from '../utils/referenceValidation';
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
  id: 'target', required: true,
  schema: { type: 'object', 'x-manyforge-semantic-type': 'target_reference' },
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
  parameters: {
    target: { component_id: 'lite6_robot', command_target_id: 'arm' },
    interrupt_condition: TRIAL_E_CONDITION,
  },
};

type SignalFact = ReferenceSelectorFacts['signals'][number];

const inputFact = (componentId: string, signalName: string, commandTargetId: string): SignalFact => ({
  reference: { component_id: componentId, signal_name: signalName }, ambiguous: false,
  target: { component_id: componentId, command_target_id: commandTargetId },
  value_type: 'bool', direction: 'input', transition_capability: 'sampled_only',
  update_mode: 'change_only', liveness: null, provider_profile: 'mock',
});

type TargetFact = ReferenceSelectorFacts['targets'][number];

const targetFact = (componentId: string, commandTargetId: string): TargetFact => ({
  target: { component_id: componentId, command_target_id: commandTargetId },
  capabilities: [], source_joint_names: [], tcp_entries: [], default_tcp: null, stop_routes: [],
});

/** The qualified Draft candidate's facts for the Trial E Deployment Draft (no binding). */
const trialEFacts = (extra: SignalFact[] = [], extraTargets: TargetFact[] = []): ReferenceSelectorFacts => ({
  components: [{ component_id: 'lite6_robot' }, { component_id: 'station_io' }],
  targets: [targetFact('lite6_robot', 'arm'), targetFact('station_io', 'io'), ...extraTargets],
  frames: [], bodies: [],
  signals: [
    inputFact('station_io', 'curtain_blocked', 'io'),
    inputFact('station_io', 'edge_sensor', 'io'),
    inputFact('station_io', 'probe_contact', 'io'),
    ...extra,
  ],
});

async function mountSearchTrip(facts: ReferenceSelectorFacts | null) {
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
    selectorFacts={facts} busy={false} onMutate={vi.fn()}
    onUpdateNodePolicies={vi.fn()} onInvalidEditChange={onInvalidEditChange}
  />));
  return { find, all, onInvalidEditChange, unmount: () => act(() => root.unmount()) };
}

describe('interrupt conditions with binding null', () => {
  it('resolves the signal from the Draft facts: no invalid edit, operand listed', async () => {
    const mounted = await mountSearchTrip(trialEFacts());
    try {
      expect(mounted.onInvalidEditChange).not.toHaveBeenCalledWith(true);
      expect(mounted.all((props) => props.role === 'alert')).toEqual([]);
      const operand = mounted.find((props) => props['aria-label'] === 'condition operand');
      expect(operand.value).toBe(JSON.stringify(
        { signal: { component_id: 'station_io', signal_name: 'probe_contact' } },
      ));
    } finally { await mounted.unmount(); }
    const markup = renderToStaticMarkup(<ProgramPropertyEditor
      roots={[searchTrip]} selectedId="search_trip" catalogEntries={[until]}
      program={trialEProgram([searchTrip])} deploymentWorkspace={workspace(trialEDeployment())}
      selectorFacts={trialEFacts()} busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()}
    />);
    expect(markup).toContain('station_io · probe_contact (input)');
    expect(markup).not.toContain('is not a candidate signal');
  });

  it('keeps the current refusal without qualified Draft facts', async () => {
    const mounted = await mountSearchTrip(null);
    try {
      expect(mounted.onInvalidEditChange).toHaveBeenLastCalledWith(true);
    } finally { await mounted.unmount(); }
  });

  it('lists the generated system input of the qualified Draft candidate before Apply', () => {
    const operand = { signal: { component_id: 'robot', signal_name: 'carried_object_lost.gripper' } };
    const node: ProgramTreeNode = {
      node_id: 'motion.move_until_signal', instance_id: 'watch_loss', children: [],
      parameters: {
        target: { component_id: 'robot', command_target_id: 'gripper' },
        interrupt_condition: { compare: { operand, op: '==', value: true } },
      },
    };
    const facts = trialEFacts([inputFact('robot', 'carried_object_lost.gripper', 'gripper')],
      [targetFact('robot', 'gripper')]);
    const markup = renderToStaticMarkup(<ProgramPropertyEditor
      roots={[node]} selectedId="watch_loss" catalogEntries={[until]} program={trialEProgram([node])}
      deploymentWorkspace={workspace(trialEDeployment())} selectorFacts={facts}
      busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()} />);

    expect(markup).toContain(`<option value="${JSON.stringify(operand).replaceAll('"', '&quot;')}" selected="">`
      + 'robot · carried_object_lost.gripper (input)</option>');
    expect(markup).not.toContain('is not a candidate signal');
  });

  it('tells the user when the current Draft has no qualified signal table', () => {
    const render = (facts: ReferenceSelectorFacts | null) => renderToStaticMarkup(<ProgramPropertyEditor
      roots={[searchTrip]} selectedId="search_trip" catalogEntries={[until]}
      program={trialEProgram([searchTrip])} deploymentWorkspace={workspace(trialEDeployment())}
      selectorFacts={facts} systemSignalsNeedApply={facts === null}
      busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()} />);

    expect(render(null)).toContain('Qualified Draft system signals are unavailable.');
    expect(render(trialEFacts())).not.toContain('Qualified Draft system signals are unavailable.');
  });

  it('shows the probe Stop warning through the same resolver', () => {
    const probe: ProgramTreeNode = {
      node_id: 'motion.probe_search', instance_id: 'probe', children: [],
      parameters: { target: { component_id: 'lite6_robot', command_target_id: 'arm' }, stop_mode: 'soft_on_path' },
    };
    const render = (node: ProgramTreeNode) => renderToStaticMarkup(<ProgramPropertyEditor
      roots={[node]} selectedId="probe" catalogEntries={[probeEntry]} program={trialEProgram([node])}
      deploymentWorkspace={workspace(trialEDeployment(true))}
      busy={false} onMutate={vi.fn()} onUpdateNodePolicies={vi.fn()}
    />);
    expect(render(probe))
      .toContain('This probe may finish a short remaining path instead of stopping on contact.');
    // A target the Deployment Draft does not have resolves no Stop settings.
    const unresolved = { ...probe, parameters: {
      ...probe.parameters, target: { component_id: 'other_robot', command_target_id: 'arm' } } };
    expect(render(unresolved)).not.toContain('This probe may finish a short remaining path');
  });
});
