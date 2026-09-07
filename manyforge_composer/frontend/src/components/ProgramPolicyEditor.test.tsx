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
  it('previews inherited or replaced Draft membership without a group registry', () => {
    const leaf = { ...node };
    const parent: ProgramTreeNode = {
      ...node, instance_id: 'branch', execution_groups: ['all_grippers', 'robot_a'], children: [leaf],
    };
    expect(inheritedProgramExecutionGroups([parent], leaf.instance_id)).toEqual(['all_grippers', 'robot_a']);
    const markup = renderToStaticMarkup(
      <ProgramPolicyEditor node={leaf} inheritedGroups={parent.execution_groups as string[]}
        busy={false} onUpdate={vi.fn()} />,
    );
    expect(markup).toContain('all, all_grippers, robot_a');
    expect(markup).toContain('running membership cannot change');
    parent.execution_groups = 'all';
    expect(inheritedProgramExecutionGroups([parent], leaf.instance_id)).toEqual([]);
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
