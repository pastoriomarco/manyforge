import { act, type ReactElement } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { BlackboardKeyDeclaration, JsonValue } from '../types/api';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import type { ConditionOperandChoice } from '../utils/interruptAuthoring';
import { globalRuleChoices } from '../utils/interruptAuthoring';
import {
  draftCarriedObjectLostSignals,
  resolveCarriedObjectLostSignalChoices,
  type CarriedObjectLostSignalChoice,
} from '../utils/systemSignalAuthoring';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
import { InterruptConditionEditor } from './InterruptConditionEditor';
import { StopStagePicker } from './StopStagePicker';

afterEach(() => vi.unstubAllGlobals());

const choice = (key: string, valueType: ConditionOperandChoice['valueType'], values?: string[]): ConditionOperandChoice => ({
  id: JSON.stringify({ key }), operand: { key }, label: key, key, valueType, ...(values ? { values } : {}),
});
const signal: ConditionOperandChoice = {
  id: JSON.stringify({ signal: { resource_role: 'part_present', target_role: 'arm' } }),
  operand: { signal: { target_role: 'arm', resource_role: 'part_present' } },
  label: 'arm · part_present', key: '', valueType: 'bool',
};
const choices = [signal, choice('cell.count', 'int64'), choice('node_outcome.seek', 'string', ['completed', 'signal_trip'])];

async function render(element: ReactElement) {
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(element));
  const control = (label: string) => find((props) => props['aria-label'] === label);
  return {
    root, find, all, control,
    type: (label: string, value: string) => act(() => control(label).onChange?.({ target: { value } })),
    click: (text: string) => act(() => find((props) => props.type === 'button' && props.children === text).onClick?.()),
    unmount: () => act(() => root.unmount()),
  };
}

const options = (props: MountedProps) => (function flatten(children: unknown): string[] {
  if (Array.isArray(children)) return children.flatMap(flatten);
  const element = children as { props?: { value?: unknown; children?: unknown } } | null;
  if (element?.props === undefined) return [];
  return element.props.value !== undefined ? [String(element.props.value)] : flatten(element.props.children);
})(props.children);

describe('InterruptConditionEditor', () => {
  it('builds a condition: comparison, typed operators, groups; commits only valid trees', async () => {
    const onCommit = vi.fn();
    const onInvalid = vi.fn();
    const mounted = await render(<InterruptConditionEditor label="c" value={undefined} choices={choices}
      disabled={false} onCommit={onCommit} onInvalidChange={onInvalid} />);
    try {
      await mounted.click('Add condition');
      expect(onCommit).toHaveBeenLastCalledWith({ compare: { operand: signal.operand, op: '==', value: true } });
      // A bool operand offers only equality.
      expect(options(mounted.control('condition comparison'))).toEqual(['==', '!=']);
      await mounted.type('condition operand', choices[1].id);
      expect(onCommit).toHaveBeenLastCalledWith({ compare: { operand: { key: 'cell.count' }, op: '==', value: 0 } });
      expect(options(mounted.control('condition comparison'))).toHaveLength(6);
      await mounted.type('condition comparison', '>=');
      // A string key with closed values offers them as a select.
      await mounted.type('condition operand', choices[2].id);
      expect(options(mounted.control('condition value'))).toEqual(['completed', 'signal_trip']);
      expect(onCommit).toHaveBeenLastCalledWith({ compare: { operand: { key: 'node_outcome.seek' }, op: '==', value: 'completed' } });
      // Wrap into AND and add a second comparison.
      await mounted.type('condition operator', 'all');
      await mounted.click('Add condition');
      const last = onCommit.mock.lastCall?.[0] as { all: JsonValue[] };
      expect(last.all).toHaveLength(2);
      // Removing every child makes the tree invalid: it is not committed.
      const calls = onCommit.mock.calls.length;
      await act(() => mounted.control('Remove condition.all[1]').onClick?.());
      await act(() => mounted.control('Remove condition.all[0]').onClick?.());
      expect(onCommit.mock.calls.length).toBe(calls + 1);
      expect(onInvalid).toHaveBeenLastCalledWith(expect.stringContaining('at least one'));
    } finally { await mounted.unmount(); }
  });

  it('keeps an unknown operand visible and invalid', async () => {
    const onInvalid = vi.fn();
    const mounted = await render(<InterruptConditionEditor label="c"
      value={{ compare: { operand: { key: 'gone' }, op: '==', value: true } }} choices={choices}
      disabled={false} onCommit={vi.fn()} onInvalidChange={onInvalid} />);
    try {
      expect(options(mounted.control('condition operand'))[0]).toBe('');
      expect(onInvalid).toHaveBeenLastCalledWith(expect.stringContaining('not a bound'));
    } finally { await mounted.unmount(); }
  });

  it('hints when a != node outcome test is not guarded against none (contract 105)', async () => {
    const outcome = choice('node_outcome.pick', 'string', ['completed', 'failed', 'none']);
    const notCompleted = { compare: { operand: outcome.operand, op: '!=', value: 'completed' } };
    const hinted = await render(<InterruptConditionEditor label="c" value={notCompleted} choices={[outcome]}
      disabled={false} onCommit={vi.fn()} />);
    try {
      const parts = hinted.control('c none hint').children as unknown[];
      expect(parts.filter((part) => typeof part === 'string').join('')).toContain('has no result yet in this Run');
    } finally { await hinted.unmount(); }
    const guarded = await render(<InterruptConditionEditor label="c" choices={[outcome]} disabled={false} onCommit={vi.fn()}
      value={{ all: [notCompleted, { compare: { operand: outcome.operand, op: '!=', value: 'none' } }] }} />);
    try {
      expect(guarded.all((props) => props['aria-label'] === 'c none hint')).toHaveLength(0);
    } finally { await guarded.unmount(); }
  });
});

describe('StopStagePicker', () => {
  it('shows the three stage labels as available after Apply and never invents an id', async () => {
    const onChange = vi.fn();
    const mounted = await render(<StopStagePicker label="Stop entry stage" value="software_hard" stages={null}
      omittedLabel="First" disabled={false} onChange={onChange} />);
    try {
      const labels = mounted.all((props) => props['aria-disabled'] === 'true').map((props) => props.children);
      expect(labels).toEqual(['Controlled stop (incl. spring-back)', 'Controller goal cancel', 'Software hard stop']);
      expect(mounted.all((props) => typeof props.children === 'string' && props.children.includes('available after Apply')))
        .toHaveLength(1);
      await mounted.click('Clear');
      expect(onChange).toHaveBeenCalledWith(undefined);
    } finally { await mounted.unmount(); }
  });

  it('stores the qualified row id when stages are known', async () => {
    const onChange = vi.fn();
    const mounted = await render(<StopStagePicker label="s" value={undefined}
      stages={[{ id: 'motion_controlled_stop', label: 'Controlled stop' }]} omittedLabel="First"
      disabled={false} onChange={onChange} />);
    try {
      await mounted.type('s', 'motion_controlled_stop');
      expect(onChange).toHaveBeenCalledWith('motion_controlled_stop');
    } finally { await mounted.unmount(); }
  });
});

const station = {
  component_id: 'station',
  component_profile_id: 'io_device_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'io_provider',
    runtime_configuration: {
      io_provider_devices: [{ controllable_component_id: 'station' }],
      io_signals: [
        { controllable_component_id: 'station', command_target_id: 'io', resource_role_id: 'part_present',
          signal_name: 'part_present', source_id: 'm', value_type: 'bool', direction: 'input',
          update_mode: 'change_only', transition_capability: 'sampled_only',
          freshness: { mode: 'provider_validity' }, provider: { profile: 'mock' } },
        { controllable_component_id: 'station', command_target_id: 'io', resource_role_id: 'clamp',
          signal_name: 'clamp', source_id: 'clamp_service', value_type: 'bool', direction: 'output',
          provider: { profile: 'ros_set_bool', service: '/cell/clamp/set' } },
      ],
    },
  },
};

async function mountDeployment(
  policies: Record<string, JsonValue>,
  executionGroupIds: string[] | null = null,
  programKeys: BlackboardKeyDeclaration[] | null = [],
  systemSignalChoices: CarriedObjectLostSignalChoice[] = [],
  systemSignalsNeedApply = false,
) {
  const controller = {
    workspace: { draft: { document: {
      api_version: 'manyforge.deployment_intent.v2', deployment_artifact_id: 'cell', name: 'Cell',
      components: [station], attachments: [], policies,
    } } },
    mutationAvailable: true,
    busy: false,
    mutate: vi.fn().mockResolvedValue({}),
    updateMotionTargetLimit: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
  const element = (choices: CarriedObjectLostSignalChoice[], needApply: boolean) => (
    <DeploymentFocusedEditor controller={controller}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]}
      executionGroupIds={executionGroupIds} programKeys={programKeys}
      systemSignalChoices={choices} systemSignalsNeedApply={needApply} />
  );
  const mounted = await render(element(systemSignalChoices, systemSignalsNeedApply));
  return {
    ...mounted,
    controller,
    rerenderSignals: async (choices: CarriedObjectLostSignalChoice[], needApply: boolean) => (
      act(() => mounted.root.render(element(choices, needApply)))
    ),
  };
}

describe('Deployment interrupt rules and output acknowledgement', () => {
  const operand = { signal: { controllable_component_id: 'station', signal_name: 'part_present' } };

  it('adds, edits and removes global rules as the whole policies.interrupt_rules list', async () => {
    const mounted = await mountDeployment({});
    try {
      await mounted.click('Add global rule');
      const rule = { rule_id: 'rule_1', condition: { compare: { operand, op: '==', value: true } }, scope: 'all_motion', priority: 0,
        on_trip: 'stop' };
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies', 'interrupt_rules'], 'set', [rule]);
    } finally { await mounted.unmount(); }
    const edited = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
    }] }, ['right', 'left']);
    try {
      // A stored row without on_trip shows stop, and any edit writes it (CONTRACTS §8).
      expect(edited.control('part_lost on trip').value).toBe('stop');
      // Until the backend reports the effective values the select shows "default".
      expect(edited.control('part_lost after stop').value).toBe('');
      expect(edited.control('part_lost active at start').checked).toBe(false);
      await edited.type('part_lost after stop', 'fail_node');
      expect(edited.controller.mutate).toHaveBeenLastCalledWith(['policies', 'interrupt_rules'], 'set', [{
        rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
        after_stop: 'fail_node', on_trip: 'stop',
      }]);
      // Contract 73: execution groups come from the Applied Program; or `everything`.
      await edited.type('part_lost scope', 'execution_groups');
      expect((edited.controller.mutate as ReturnType<typeof vi.fn>).mock.lastCall?.[2][0].scope)
        .toEqual({ execution_groups: ['left'] });
      await edited.type('part_lost scope', 'everything');
      expect((edited.controller.mutate as ReturnType<typeof vi.fn>).mock.lastCall?.[2][0].scope).toBe('everything');
      // The stop entry stage ids are not projected yet.
      expect(edited.all((props) => props['aria-label'] === 'part_lost stop entry stage stages')).toHaveLength(1);
      await edited.click('Remove rule');
      expect(edited.controller.mutate).toHaveBeenLastCalledWith(['policies', 'interrupt_rules'], 'remove');
    } finally { await edited.unmount(); }
  });

  it('shows short trip labels and the selected option\'s meaning under its field', async () => {
    const mounted = await mountDeployment({ interrupt_rules: [
      { rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion',
        priority: 5, on_trip: 'stop', after_stop: 'fail_node' },
      { rule_id: 'door_open', condition: { compare: { operand, op: '==', value: true } }, scope: 'all_motion',
        priority: 0, on_trip: 'notify' },
    ] });
    try {
      const optionLabels = (value: string) => mounted.all((props) => props.value === value
        && typeof props.children === 'string').map((props) => props.children);
      expect(optionLabels('stop')).toEqual(['Stop and hold', 'Stop and hold']);
      expect(optionLabels('notify')).toEqual(['Notify only', 'Notify only']);
      expect(optionLabels('hold')).toEqual(['Hold the run']);
      expect(optionLabels('fail_node')).toEqual(['Fail the node']);
      const hint = (text: string) => mounted.all((props) => props.children === text);
      expect(hint('Stop, then hold the scope while the condition holds.')).toHaveLength(1);
      expect(hint('Notify: the Program reads the state; no Stop.')).toHaveLength(1);
      expect(hint('Fail the stopped node; the run continues.')).toHaveLength(1);
      expect(hint('Hold: Resume from where it stopped.')).toHaveLength(0);
      // Labels only: the stored values are unchanged.
      expect(mounted.control('part_lost on trip').value).toBe('stop');
      expect(mounted.control('part_lost after stop').value).toBe('fail_node');
      expect(mounted.control('door_open on trip').value).toBe('notify');
    } finally { await mounted.unmount(); }
  });

  it('keeps last-Apply choices across Draft edits and hides rows from a stale owner', async () => {
    const systemSignal = {
      controllableComponentId: 'robot', commandTargetId: 'gripper',
      signalName: 'carried_object_lost.gripper', resourceRoleId: 'carried_object_lost',
      direction: 'input', valueType: 'bool',
    } as const;
    const appliedOwner = { graphRevision: 'graph.1', retainedBundleId: 'bundle.1' };
    const appliedRows = [systemSignal];
    const initialDraft = {
      api_version: 'manyforge.deployment_intent.v2', components: [station], attachments: [], policies: {},
    } as unknown as Record<string, JsonValue>;
    const initial = resolveCarriedObjectLostSignalChoices(
      draftCarriedObjectLostSignals(initialDraft), appliedRows,
      'manyforge.deployment_intent.v2', true, appliedOwner, appliedOwner,
    );
    const mounted = await mountDeployment({}, null, [], initial.choices, initial.needApply);
    try {
      await mounted.click('Add global rule');
      const created = (mounted.controller.mutate as ReturnType<typeof vi.fn>).mock.lastCall?.[2] as JsonValue[];
      expect(created).toHaveLength(1);
      const workspace = mounted.controller.workspace;
      if (workspace === null) throw new Error('Deployment test workspace is missing.');
      const changedDraft = {
        ...initialDraft,
        policies: { interrupt_rules: created },
      };
      (workspace as unknown as { draft: { document: Record<string, JsonValue> | null } }).draft.document = changedDraft;
      const changed = resolveCarriedObjectLostSignalChoices(
        draftCarriedObjectLostSignals(changedDraft), appliedRows,
        'manyforge.deployment_intent.v2', false, appliedOwner, appliedOwner,
      );
      expect(changed.needApply).toBe(false);
      await mounted.rerenderSignals(changed.choices, changed.needApply);

      const optionId = JSON.stringify({ signal: {
        controllable_component_id: 'robot', signal_name: 'carried_object_lost.gripper',
      } });
      const labels = mounted.all((props) => typeof props['aria-label'] === 'string')
        .map((props) => String(props['aria-label']));
      expect(labels).toContain('condition operand');
      const renderedAppliedOption = mounted.all((props) => props.value === optionId
        && typeof props.children === 'string');
      expect(renderedAppliedOption).toHaveLength(1);
      expect(renderedAppliedOption[0].children).toContain('from last Apply');
      const displayedChoice = globalRuleChoices(
        changedDraft,
        [],
        created[0] !== null && typeof created[0] === 'object' && !Array.isArray(created[0])
          ? created[0].condition : undefined,
        changed.choices,
      ).find((item) => item.id === optionId);
      expect(displayedChoice?.label).toContain('from last Apply');
      // The mounted select's handler only commits values from its current choices.
      // Selecting the applied candidate here verifies the full add/edit path.
      await mounted.type('condition operand', optionId);
      const rules = (mounted.controller.mutate as ReturnType<typeof vi.fn>).mock.lastCall?.[2] as JsonValue[];
      expect((rules[0] as { condition: { compare: { operand: JsonValue } } }).condition.compare.operand).toEqual({ signal: {
        controllable_component_id: 'robot', signal_name: 'carried_object_lost.gripper',
      } });
    } finally { await mounted.unmount(); }

    const changedDraft = {
      api_version: 'manyforge.deployment_intent.v2', components: [station], attachments: [], policies: {},
    } as unknown as Record<string, JsonValue>;
    const staleOwner = resolveCarriedObjectLostSignalChoices(
      draftCarriedObjectLostSignals(changedDraft), appliedRows,
      'manyforge.deployment_intent.v2', false,
      { graphRevision: 'graph.2', retainedBundleId: 'bundle.1' }, appliedOwner,
    );
    expect(staleOwner).toEqual({ choices: [], needApply: true });
    const stale = await mountDeployment({}, null, [], staleOwner.choices, staleOwner.needApply);
    try {
      await stale.click('Add global rule');
      const optionId = JSON.stringify({ signal: {
        controllable_component_id: 'robot', signal_name: 'carried_object_lost.gripper',
      } });
      expect(stale.all((props) => props.value === optionId
        && typeof props.children === 'string')).toHaveLength(0);
      expect(stale.all((props) => typeof props.children === 'string'
        && props.children.includes('Apply to list system signals.'))).toHaveLength(1);
    } finally { await stale.unmount(); }
  });

  it('shows the Apply hint when system signals cannot be derived from this Draft', async () => {
    const mounted = await mountDeployment({}, null, [], [], true);
    try {
      expect(mounted.all((props) => typeof props.children === 'string'
        && props.children.includes('Apply to list system signals.'))).toHaveLength(1);
    } finally { await mounted.unmount(); }
  });

  it('shows the effective after-Stop and active-at-start values and resets them to default (CONTRACTS §11)', async () => {
    const effective = vi.spyOn(api.cellApply, 'effective').mockResolvedValue({ fields: {
      'policies.interrupt_rules.0.after_stop': { value: 'fail_node', default: 'hold', set: true },
      'policies.interrupt_rules.0.active_at_start': { value: false, default: false, set: false },
    } });
    const mounted = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
      on_trip: 'stop', after_stop: 'fail_node',
    }] });
    try {
      await act(async () => { await Promise.resolve(); });
      expect(effective).toHaveBeenCalled();
      expect(mounted.control('part_lost after stop').value).toBe('fail_node');
      expect(options(mounted.control('part_lost after stop'))).toEqual(['hold', 'fail_node']);
      expect(mounted.control('part_lost active at start').checked).toBe(false);
      // Only a set field offers Reset to default.
      expect(mounted.all((props) => props['aria-label'] === 'Reset part_lost active at start to default')).toHaveLength(0);
      await act(() => mounted.control('Reset part_lost after stop to default').onClick?.());
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['policies', 'interrupt_rules', 0, 'after_stop'], 'remove');
    } finally { await mounted.unmount(); effective.mockRestore(); }
  });

  it('edits after the Stop and active at start, writing the chosen value', async () => {
    const stop = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
      on_trip: 'stop', after_stop: 'fail_node',
    }] });
    try {
      expect(options(stop.control('part_lost on trip'))).toEqual(['stop', 'notify']);
      expect(options(stop.control('part_lost after stop'))).toEqual(['hold', 'fail_node']);
      expect(stop.control('part_lost after stop').value).toBe('fail_node');
      await stop.type('part_lost after stop', 'hold');
      expect(stop.controller.mutate).toHaveBeenLastCalledWith(['policies', 'interrupt_rules'], 'set', [{
        rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
        on_trip: 'stop', after_stop: 'hold',
      }]);
      await act(() => stop.control('part_lost active at start').onChange?.({ target: { checked: true } }));
      expect((stop.controller.mutate as ReturnType<typeof vi.fn>).mock.lastCall?.[2][0].active_at_start).toBe(true);
    } finally { await stop.unmount(); }
    const active = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
      on_trip: 'stop', active_at_start: true,
    }] });
    try {
      expect(active.control('part_lost active at start').checked).toBe(true);
      await act(() => active.control('part_lost active at start').onChange?.({ target: { checked: false } }));
      expect(active.controller.mutate).toHaveBeenLastCalledWith(['policies', 'interrupt_rules'], 'set', [{
        rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
        on_trip: 'stop', active_at_start: false,
      }]);
    } finally { await active.unmount(); }
  });

  it('offers publish-only notify without after-Stop or Stop-stage controls', async () => {
    const mounted = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
      on_trip: 'stop', after_stop: 'fail_node', stop_entry_stage_id: 'motion_controlled_stop',
    }] });
    try {
      await mounted.type('part_lost on trip', 'notify');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(['policies', 'interrupt_rules'], 'set', [{
        rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
        on_trip: 'notify',
      }]);
    } finally { await mounted.unmount(); }

    // The mocked mutation does not update the Deployment draft. Render the
    // applied notify value separately to verify its publish-only controls.
    const notify = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
      on_trip: 'notify',
    }] });
    try {
      expect(notify.control('part_lost on trip').value).toBe('notify');
      expect(notify.all((props) => props['aria-label'] === 'part_lost after stop')).toHaveLength(0);
      expect(notify.all((props) => props['aria-label'] === 'part_lost stop entry stage stages')).toHaveLength(0);
      expect(notify.all((props) => props['aria-label'] === 'part_lost active at start')).toHaveLength(1);
    } finally { await notify.unmount(); }

    const malformed = await mountDeployment({ interrupt_rules: [{
      rule_id: 'part_lost', condition: { compare: { operand, op: '==', value: false } }, scope: 'all_motion', priority: 5,
      on_trip: 'notify', after_stop: 'hold',
    }] });
    try {
      const problem = malformed.all((props) => props['aria-label'] === 'part_lost notify settings');
      expect(problem).toHaveLength(1);
      expect(problem[0].children).toContain('does not apply to on_trip notify');
    } finally { await malformed.unmount(); }
  });

  it('offers changed outside under the outputs that hold one', () => {
    const document = { components: [station] } as unknown as Record<string, JsonValue>;
    const labels = (eligible: ReadonlySet<string>) => globalRuleChoices(document, [], undefined, [], eligible)
      .map((choice) => choice.label);
    expect(labels(new Set(['station/clamp']))).toContain('station · clamp → changed outside');
    expect(labels(new Set())).not.toContain('station · clamp → changed outside');
  });

  it('loads a stop rule (all_motion, priority 0) without flags', async () => {
    const trial = { rule_id: 'light_curtain', condition: { compare: { operand, op: '==', value: true } },
      scope: 'all_motion', priority: 0, on_trip: 'stop', active_at_start: true };
    const mounted = await mountDeployment({ interrupt_rules: [trial] });
    try {
      expect(mounted.control('light_curtain scope').value).toBe('all_motion');
      expect(options(mounted.control('light_curtain scope'))).toEqual(['all_motion', 'execution_groups', 'everything']);
      expect(mounted.all((props) => props['aria-label'] === 'light_curtain execution groups')).toHaveLength(0);
      expect(mounted.all((props) => props.role === 'alert')).toHaveLength(0);
      // Without an Applied Program, execution groups are unavailable.
      await mounted.type('light_curtain scope', 'execution_groups');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); }
    const grouped = await mountDeployment({ interrupt_rules: [{ ...trial, scope: { execution_groups: ['left'] } }] },
      ['left', 'right']);
    try {
      await act(() => grouped.control('light_curtain group right').onChange?.({ target: { checked: true } }));
      expect((grouped.controller.mutate as ReturnType<typeof vi.fn>).mock.lastCall?.[2][0].scope)
        .toEqual({ execution_groups: ['left', 'right'] });
    } finally { await grouped.unmount(); }
  });

  it('flags a global rule reading a key without initial (contract 87)', async () => {
    const keyRule = (key: string) => ({ rule_id: 'armed', scope: 'all_motion',
      condition: { compare: { operand: { key }, op: '==', value: true } } });
    const keys = [
      { key: 'cell.ready', type: 'bool', access: 'read_write', persistence: 'run' },
      { key: 'cell.armed', type: 'bool', access: 'read_write', persistence: 'run', initial: false },
    ] as BlackboardKeyDeclaration[];
    const flagged = await mountDeployment({ interrupt_rules: [keyRule('cell.ready')] }, null, keys);
    try {
      expect(String(flagged.control('armed key initial').children)).toContain('interrupt_key_initial_required');
    } finally { await flagged.unmount(); }
    const clean = await mountDeployment({ interrupt_rules: [keyRule('cell.armed')] }, null, keys);
    try {
      expect(clean.all((props) => props['aria-label'] === 'armed key initial')).toHaveLength(0);
    } finally { await clean.unmount(); }
  });

  it('flags global-rule user keys (106, 110) and refuses rule-state operands (111)', async () => {
    const door = { compare: { operand: { key: 'cell.door_open' }, op: '==', value: true } };
    const mixed = { any: [{ compare: { operand, op: '==', value: true } }, door] };
    const state = { compare: { operand: { key: 'interrupt_state.door' }, op: '==', value: 'tripped' } };
    const rules = [
      { rule_id: 'door', scope: 'all_motion', condition: door },
      { rule_id: 'mixed', scope: 'all_motion', condition: mixed },
      { rule_id: 'chained', scope: 'all_motion', condition: state },
    ];
    const waiting = await mountDeployment({ interrupt_rules: rules }, null, null);
    try {
      expect(JSON.stringify(waiting.control('door key waiting').children)).toContain('Waiting for a Program that declares');
      expect(waiting.all((props) => props['aria-label'] === 'door key undeclared')).toHaveLength(0);
      expect(waiting.all((props) => props['aria-label'] === 'door mixed operands')).toHaveLength(0);
      expect(String(waiting.control('mixed mixed operands').children)).toContain('use a separate signal-only rule');
      expect(JSON.stringify(waiting.control('chained state operand').children)).toContain('global_rule_state_operand_unsupported');
    } finally { await waiting.unmount(); }
    const undeclared = await mountDeployment({ interrupt_rules: [rules[0]] }, null, []);
    try {
      expect(JSON.stringify(undeclared.control('door key undeclared').children)).toContain('global_rule_key_undeclared');
      // A pending user key stays editable: its condition is not refused.
      expect(undeclared.all((props) => props.role === 'alert')).toHaveLength(0);
    } finally { await undeclared.unmount(); }
  });

  it('shows the output acknowledgement timeout only with ROS SetBool outputs, prefilled 0.5 s', async () => {
    const mounted = await mountDeployment({});
    try {
      expect(mounted.control('station output acknowledgement timeout').value).toBe('0.5');
      expect(mounted.all((props) => props.className === 'field-help' && Array.isArray(props.children)
        && props.children.some((part) => typeof part === 'string' && part.includes('provisional')))).toHaveLength(1);
      await mounted.type('station output acknowledgement timeout', '0.8');
      await act(() => (mounted.control('station output acknowledgement timeout').onBlur as () => void)());
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['components', 0, 'configuration', 'runtime_configuration', 'io_provider_devices'], 'set',
        [{ controllable_component_id: 'station', lifecycle_command_timeout_s: 10, output_source_ack_timeout_s: 0.8 }],
      );
    } finally { await mounted.unmount(); }
    const mockOnly = structuredClone(station);
    mockOnly.configuration.runtime_configuration.io_signals.pop();
    const { host, all } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const controller = {
      workspace: { draft: { document: {
        api_version: 'manyforge.deployment_intent.v2', deployment_artifact_id: 'cell', name: 'Cell',
        components: [mockOnly], attachments: [], policies: {},
      } } },
      mutationAvailable: true, busy: false, mutate: vi.fn(), updateMotionTargetLimit: vi.fn(),
    } as unknown as DeploymentWorkspaceController;
    await act(() => root.render(<DeploymentFocusedEditor controller={controller}
      sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
    expect(all((props) => props['aria-label'] === 'station output acknowledgement timeout')).toHaveLength(0);
    await act(() => root.unmount());
  });
});

describe('Deployment motion takeover timing', () => {
  const enter = async (mounted: Awaited<ReturnType<typeof mountDeployment>>, label: string, value: string) => {
    await mounted.type(label, value);
    await act(() => (mounted.control(label).onBlur as (() => void) | undefined)?.());
  };
  const total = 'Takeover total preparation time';
  const planning = 'Takeover planning budget';

  it('keeps the planning budget unavailable while takeover is off', async () => {
    const mounted = await mountDeployment({});
    try {
      expect(mounted.control(planning).disabled).toBe(true);
      await enter(mounted, total, '0.5');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['policies', 'motion_takeover_preparation_budget_s'], 'set', 0.5);
    } finally { await mounted.unmount(); }
  });

  it('shows the planning budget default the backend reports (CONTRACTS §11)', async () => {
    const effective = vi.spyOn(api.cellApply, 'effective').mockResolvedValue({ fields: {
      'policies.motion_takeover_planning_budget_s': { value: 0.4, default: 0.4, set: false },
    } });
    const mounted = await mountDeployment({ motion_takeover_preparation_budget_s: 0.5 });
    try {
      await act(async () => { await Promise.resolve(); });
      expect(mounted.control(planning).placeholder).toBe('default 0.4');
    } finally { await mounted.unmount(); effective.mockRestore(); }
  });

  it('writes the planning budget only strictly below the total', async () => {
    const mounted = await mountDeployment({ motion_takeover_preparation_budget_s: 0.5 });
    try {
      await enter(mounted, planning, '0.5');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await enter(mounted, planning, '0.35');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['policies', 'motion_takeover_planning_budget_s'], 'set', 0.35);
    } finally { await mounted.unmount(); }
  });

  it('refuses a total at or below the planning budget and turning takeover off before clearing planning', async () => {
    const mounted = await mountDeployment({
      motion_takeover_preparation_budget_s: 0.5, motion_takeover_planning_budget_s: 0.3,
    });
    try {
      await enter(mounted, total, '0.3');
      await enter(mounted, total, '');
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
      await enter(mounted, planning, '');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['policies', 'motion_takeover_planning_budget_s'], 'remove');
      await enter(mounted, total, '0.8');
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(
        ['policies', 'motion_takeover_preparation_budget_s'], 'set', 0.8);
    } finally { await mounted.unmount(); }
  });
});

describe('Deployment motion takeover refusal feedback', () => {
  it('leaves an owner refusal of a well-formed value to the yellow notice row, without calling it malformed', async () => {
    const mounted = await mountDeployment({ motion_takeover_preparation_budget_s: 0.5 });
    try {
      vi.mocked(mounted.controller.mutate).mockResolvedValueOnce(null);
      await mounted.type('Takeover planning budget', '0.4');
      await act(() => (mounted.control('Takeover planning budget').onBlur as (() => void) | undefined)?.());
      const texts = mounted.all((props) => typeof props.children === 'string').map((props) => props.children);
      expect(mounted.controller.mutate).toHaveBeenCalledOnce();
      // No red inline repeat of the refusal.
      expect(texts).not.toContain('The Deployment owner rejected this timing change.');
      expect(texts).not.toContain('Value was rejected.');
      expect(texts.some((text) => String(text).startsWith('Enter a positive value'))).toBe(false);
    } finally { await mounted.unmount(); }
  });
});

describe('Deployment motion takeover default planning budget', () => {
  it('shows the 80% default the backend reports for an omitted planning budget and writes nothing', async () => {
    const effective = vi.spyOn(api.cellApply, 'effective').mockResolvedValue({ fields: {
      'policies.motion_takeover_planning_budget_s': { value: 0.4, default: 0.4, set: false },
    } });
    const mounted = await mountDeployment({ motion_takeover_preparation_budget_s: 0.5 });
    try {
      await act(async () => { await Promise.resolve(); });
      expect(mounted.control('Takeover planning budget').placeholder).toBe('default 0.4');
      expect(mounted.all((props) => Array.isArray(props.children)
        && props.children.includes('Default planning budget: 0.4 s (80% of the total). '))).toHaveLength(1);
      expect(mounted.controller.mutate).not.toHaveBeenCalled();
    } finally { await mounted.unmount(); effective.mockRestore(); }
  });
});
