import { describe, expect, it } from 'vitest';
import { referenceResult } from './referenceValidation';

describe('reference validation capture', () => {
  const generation = { workspace_epoch: 'e', workspace_revision: 1, program_draft_id: 'p',
    program_draft_revision: 2, deployment_draft_id: 'd', deployment_draft_revision: 3 };
  const digest = `sha256:${'a'.repeat(64)}`;
  const row = { code: 'frame_missing', location: { document: 'program', path: '/forest/0/parameters/frame',
    node_instance_id: 'move', parameter: 'frame' }, payload: { component_id: 'robot', frame_id: 'tool' } };
  const response = { reference_validation: { schema_version: 'manyforge.program_reference_validation_response.v2',
    workspace_generation: generation, program_content_digest: digest, diagnostics: [row], total_count: 2, truncated: true, selector_facts: { components: [], targets: [], signals: [], frames: [], bodies: [], rules: [] } } };
  it('preserves diagnostics and bounded count for the same capture', () => {
    expect(referenceResult(response, digest, generation)).toEqual({ issues: [row], total: 2, truncated: true, selectorFacts: { components: [], targets: [], signals: [], frames: [], bodies: [], rules: [] } });
  });
  it('rejects stale generation and mismatched body digest', () => {
    expect(() => referenceResult(response, digest, { ...generation, workspace_revision: 2 })).toThrow();
    expect(() => referenceResult(response, `sha256:${'b'.repeat(64)}`, generation)).toThrow();
  });
});
