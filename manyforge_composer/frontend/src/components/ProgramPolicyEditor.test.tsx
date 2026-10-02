import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramTreeNode } from '../types/api';
import { inheritedProgramExecutionGroups } from '../utils/programAuthoring';
import {
  ALLOWED_SKILLS_UNAVAILABLE_MESSAGE,
  effectiveProgramPolicy,
  ProgramPolicyEditor,
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

  it('renders omitted fields as effective canonical defaults', () => {
    const form = effectiveProgramPolicy(node);
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={node} busy={false} onUpdate={vi.fn()} />,
    );

    expect(form.modification_policy).toEqual({
      runtime_override: 'forbidden',
      persistent_edit: 'forbidden',
      intervention_delegate: 'forbidden',
    });
    expect(form.recovery_policy).toEqual({
      allow_intervention: false,
      allowed_recovery_actions: [],
      max_retries: 0,
      allowed_skills: [],
      persistence_ceiling: 'session_only',
    });
    expect(form.operator_stop_policy).toBe('inherit');
    expect(markup).toContain('Defaults are saved by omission.');
    expect(markup).not.toContain('Save node policy to Draft');
    expect(markup).toContain('Hold pauses this subtree');
    expect(markup).toContain('leaves default to Hold');
    expect(markup).toContain('the root to Continue');
    expect(markup).toContain('Allowed recovery skills are unavailable: each skill requires an exact revision-qualified retained referent. Leave Allowed skills empty.');
    expect(markup).not.toContain('>Apply<');
  });

  it('blocks imported skills without inventing a retained referent', () => {
    const imported: ProgramTreeNode = {
      ...node,
      recovery_policy: {
        allow_intervention: true,
        allowed_recovery_actions: ['intervention'],
        max_retries: 0,
        allowed_skills: ['legacy.skill'],
        persistence_ceiling: 'session_only',
      },
    };
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={imported} busy={false} onUpdate={vi.fn()} />,
    );

    expect(markup).toContain(ALLOWED_SKILLS_UNAVAILABLE_MESSAGE);
    expect(markup).toContain('Imported authored Allowed skills (legacy.skill)');
    expect(markup).toContain('blocking applicability error');
    expect(markup).not.toContain('id="policy-settle-allowed_skills"');
    expect(markup).toContain('aria-invalid="true"');
    expect(markup).not.toContain('Apply-ready');
  });

  it('explains every canonical cross-field constraint before submit', () => {
    const invalid = effectiveProgramPolicy(node);
    invalid.recovery_policy.allow_intervention = true;
    invalid.recovery_policy.allowed_recovery_actions = ['runtime_override'];
    invalid.recovery_policy.persistence_ceiling = 'draft_patch';
    invalid.recovery_policy.max_retries = 256;
    const errors = validateProgramPolicy(invalid);

    expect(errors['recovery_policy.allow_intervention']).toContain('match');
    expect(errors['recovery_policy.allowed_recovery_actions']).toContain('requires');
    expect(errors['modification_policy.runtime_override']).toContain('non-forbidden');
    expect(errors['modification_policy.persistent_edit']).toContain('persistent edit');
    expect(errors['recovery_policy.persistence_ceiling']).toContain('Draft-patch');
    expect(errors['recovery_policy.max_retries']).toContain('0 to 255');
  });
});
