import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramTreeNode } from '../types/api';
import { inheritedProgramExecutionGroups } from '../utils/programAuthoring';
import {
  effectiveProgramPolicy,
  ProgramPolicyEditor,
  programRecoveryPolicy,
  retryLimitLabel,
  validateProgramPolicy,
} from './ProgramPolicyEditor';

const node: ProgramTreeNode = {
  node_id: 'timer',
  instance_id: 'settle',
  parameters: {},
  children: [],
};

describe('ProgramPolicyEditor', () => {
  it('shows inherited groups as checked but disabled, with a creation shortcut', () => {
    const leaf = { ...node };
    const parent: ProgramTreeNode = {
      ...node, instance_id: 'branch', execution_groups: ['all_grippers', 'robot_a'], children: [leaf],
    };
    expect(inheritedProgramExecutionGroups([parent], leaf.instance_id)).toEqual(['all_grippers', 'robot_a']);
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={leaf} inheritedGroups={parent.execution_groups as string[]}
        availableGroups={['all_grippers', 'robot_a']} onCreateGroup={vi.fn()}
        busy={false} onUpdate={vi.fn()} />,
    );
    expect(markup).toContain('Draft memberships: All + all_grippers, robot_a (inherited).');
    expect(markup).toContain('<legend>Control groups</legend>');
    expect(markup).toContain('Inherit groups from parent');
    expect(markup).toContain('running membership cannot change');
    expect(markup).toContain('type="checkbox" disabled="" checked=""/>all_grippers');
    expect(markup).toContain('Create group…');
    expect(markup).not.toContain('placeholder="robot_a, all_grippers"');
    parent.execution_groups = 'all';
    expect(inheritedProgramExecutionGroups([parent], leaf.instance_id)).toEqual([]);
  });

  it('offers only existing named groups and keeps empty explicit membership in All', () => {
    const explicit = renderToStaticMarkup(
      <ProgramPolicyEditor node={{ ...node, execution_groups: ['robot_a', 'robot_b'] }}
        availableGroups={['robot_a', 'robot_b', 'robot_c']} busy={false} onUpdate={vi.fn()} />,
    );
    expect(explicit).toContain('type="checkbox" checked=""/>robot_a');
    expect(explicit).toContain('type="checkbox" checked=""/>robot_b');
    expect(explicit).toContain('type="checkbox"/>robot_c');
    const allOnly = renderToStaticMarkup(
      <ProgramPolicyEditor node={{ ...node, execution_groups: 'all' }}
        availableGroups={['robot_a']} busy={false} onUpdate={vi.fn()} />,
    );
    expect(allOnly).toContain('Draft memberships: All only.');
    expect(allOnly).toContain('type="checkbox"/>robot_a');
    expect(allOnly).not.toContain('>All only</option>');
  });

  it('keeps undeclared memberships visible as missing, and branch hints mention inheriting children', () => {
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={{ ...node, execution_groups: ['ghost', 'robot_a'], children: [{ ...node, instance_id: 'child' }] }}
        availableGroups={['robot_a', 'robot_b']} busy={false} onUpdate={vi.fn()} />,
    );
    expect(markup).toContain('type="checkbox" checked=""/>ghost — missing');
    expect(markup).toContain('Draft memberships: All + ghost, robot_a.');
    expect(markup).toContain('Child nodes that inherit follow later changes here.');
    expect(markup.indexOf('<legend>Control groups</legend>'))
      .toBeLessThan(markup.indexOf('<legend>Operator Stop policy</legend>'));
  });

  it('validates named groups before submitting the existing policy form', () => {
    const form = effectiveProgramPolicy({ ...node, execution_groups: ['robot_a'] });
    expect(form.execution_group_mode).toBe('named');
    expect(form.execution_group_names).toBe('robot_a');
    expect(validateProgramPolicy(form).execution_groups).toBeUndefined();
    for (const names of ['', 'all', 'inherit', 'Robot A', 'robot_a, robot_a', 'robot_a,']) {
      expect(validateProgramPolicy({ ...form, execution_group_names: names }).execution_groups).toBeTruthy();
    }
    expect(validateProgramPolicy({ ...form, execution_group_names: 'robot_a, all_grippers' }).execution_groups).toBeUndefined();
  });

  it('renders omitted fields as effective defaults with only the two recovery controls', () => {
    const form = effectiveProgramPolicy(node);
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={node} busy={false} onUpdate={vi.fn()} />,
    );

    expect(programRecoveryPolicy(form)).toEqual({ allow_intervention: false, max_retries: 0 });
    expect(form.retry_limit_mode).toBe('unlimited');
    expect(form.operator_stop_policy).toBe('inherit');
    expect(markup).toContain('defaults are saved by omission');
    expect(markup).toContain('Allow intervention');
    expect(markup).toContain('Retry limit');
    expect(markup).toContain('<option value="unlimited" selected="">Unlimited</option>');
    // Unlimited is never presented as a numeric zero.
    expect(markup).not.toContain('value="0"');
    expect(markup).not.toContain('id="policy-settle-max_retries"');
    for (const removed of [
      'Runtime override', 'Persistent edit', 'Intervention delegate',
      'Allowed recovery actions', 'Persistence ceiling', 'Allowed skills', 'LLM',
    ]) {
      expect(markup).not.toContain(removed);
    }
    expect(markup).toContain('Hold pauses this subtree');
    expect(markup).toContain('leaves default to Hold');
    expect(markup).toContain('the root to Continue');
    expect(markup).not.toContain('>Apply<');
  });

  it('shows a finite authored limit as N and zero as Unlimited', () => {
    const limited: ProgramTreeNode = {
      ...node,
      recovery_policy: { allow_intervention: true, max_retries: 3 },
    };
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={limited} busy={false} onUpdate={vi.fn()} />,
    );
    expect(markup).toContain('<option value="limited" selected="">Limit to</option>');
    expect(markup).toContain('value="3"');
    expect(retryLimitLabel(0)).toBe('Unlimited');
    expect(retryLimitLabel(1)).toBe('1 retry');
    expect(retryLimitLabel(3)).toBe('3 retries');
  });

  it('accepts a finite limit of 1..255 and explains anything else', () => {
    const form = effectiveProgramPolicy(node);
    expect(validateProgramPolicy(form)).toEqual({});
    for (const retryLimit of [1, 255]) {
      expect(validateProgramPolicy({
        ...form, retry_limit_mode: 'limited', retry_limit: retryLimit,
      })).toEqual({});
    }
    for (const retryLimit of [0, 256, 1.5, Number.NaN]) {
      expect(validateProgramPolicy({
        ...form, retry_limit_mode: 'limited', retry_limit: retryLimit,
      })['recovery_policy.max_retries']).toContain('Choose Unlimited');
    }
    // The finite draft is ignored while Unlimited is chosen.
    expect(programRecoveryPolicy({ ...form, retry_limit: 7 })).toEqual({
      allow_intervention: false, max_retries: 0,
    });
  });
});
