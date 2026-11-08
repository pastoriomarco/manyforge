import { describe, expect, it } from 'vitest';
import { parseApplyPreview } from './components';

const preview = () => ({
  components: [], scope: {},
  program: { executable: false, program_id: 'program', unresolved_references: [{
    target: { component_id: 'robot', command_target_id: 'arm' },
    reason: 'component_missing', detail: 'component is absent',
  }], affected_nodes: ['move'], affected_execution_groups: [] },
  attachment_changed_components: [], detached_components: [],
});

describe('direct deployment-preview references', () => {
  it('preserves exact target and typed refusal evidence', () => {
    const input = preview();
    expect(parseApplyPreview(input).program.unresolved_references).toEqual(input.program.unresolved_references);
  });

  it('rejects legacy role coordinates and unknown refusal reasons', () => {
    const old = preview();
    old.program.unresolved_references[0] = {
      component_role_id: 'robot', target_role_id: 'arm', controllable_component_id: 'robot',
      command_target_id: 'arm', reason: 'component_missing', detail: 'component is absent',
    } as unknown as typeof old.program.unresolved_references[0];
    expect(() => parseApplyPreview(old)).toThrow(/unresolved_references/);
    const future = preview();
    future.program.unresolved_references[0].reason = 'unknown_reason';
    expect(() => parseApplyPreview(future)).toThrow(/reason/);
  });
});
