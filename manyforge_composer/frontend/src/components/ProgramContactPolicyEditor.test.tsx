import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { JsonValue, ProgramAuthoringDocument } from '../types/api';
import type { ReferenceSelectorFacts } from '../utils/referenceValidation';
import { reactPropsHost } from '../test/reactPropsHost';
import { ProgramContactPolicyEditor } from './ProgramContactPolicyEditor';

afterEach(() => vi.unstubAllGlobals());

const endpointPair = {
  body_a: { kind: 'motion_endpoint' },
  body_b: { kind: 'runtime_object', runtime_object_id: 'trial.probe_object' },
};

// The signal_moves withdrawal: start in contact up to 2 mm, then separate.
const releasePolicy = {
  api_version: 'manyforge.contact_policy.v1',
  policy_id: 'withdrawal_contact_policy',
  mode: 'contact_release',
  sensing_mode: 'geometry_only',
  default_unmatched_response: 'hard_stop',
  zones: [{
    zone_id: 'probe_departure',
    body_pairs: [endpointPair],
    violation_response: 'hard_stop',
    release_evidence_mode: 'signed_penetration',
    enforce_max_start_penetration: true,
    max_start_penetration_m: 0.002,
    min_release_progress_m: 0,
    release_tolerance_m: 0.00001,
    require_strictly_decreasing_penetration: true,
    require_separation_by_end: true,
    allow_recontact: false,
  }],
};

function program(policies: JsonValue[]): ProgramAuthoringDocument {
  return {
    target_roles: {},
    blackboard: { keys: [{ key: 'trial.probe_object', type: 'scene_resource' }] },
    contact_policies: policies,
  } as unknown as ProgramAuthoringDocument;
}

async function mount(policies: JsonValue[], selectorFacts: ReferenceSelectorFacts | null = null) {
  const { host, find } = reactPropsHost();
  const mutate = vi.fn().mockResolvedValue({});
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(async () => {
    root.render(<ProgramContactPolicyEditor
      program={program(policies)}
      selectorFacts={selectorFacts}
      draftRevision={1}
      busy={false}
      mutate={mutate as unknown as ProgramWorkspaceController['mutate']}
    />);
  });
  return { find, mutate, unmount: () => act(async () => { root.unmount(); }) };
}

describe('ProgramContactPolicyEditor', () => {
  it('preserves a direct component body on an existing policy', async () => {
    const explicitPolicy = {
      ...releasePolicy,
      zones: [{ ...releasePolicy.zones[0], body_pairs: [{
        ...endpointPair,
        body_a: { kind: 'component_model_body', body: { component_id: 'tool', collision_body_id: 'jaw' } },
      }] }],
    };
    const facts: ReferenceSelectorFacts = { components: [], targets: [], signals: [], frames: [], bodies: [
      { reference: { component_id: 'tool', collision_body_id: 'jaw' }, ambiguous: false, moving_targets: [] },
    ] };
    const mounted = await mount([explicitPolicy as unknown as JsonValue], facts);
    const startCap = mounted.find((props) => props.type === 'number' && props.value === 0.002);
    await act(async () => { (startCap.onBlur as () => void)(); });
    const written = mounted.mutate.mock.calls.at(-1);
    expect((written?.[1] as { contact_policies: unknown[] }).contact_policies).toEqual([explicitPolicy]);
    await mounted.unmount();
  });

  it('disables a body identity shared by multiple model occurrences', () => {
    const facts: ReferenceSelectorFacts = { components: [], targets: [], signals: [], frames: [], bodies: [
      { reference: { component_id: 'tool', collision_body_id: 'jaw' }, ambiguous: true, moving_targets: [] },
    ] };
    const markup = renderToStaticMarkup(<ProgramContactPolicyEditor
      program={program([])} selectorFacts={facts} draftRevision={1} busy={false}
      mutate={vi.fn() as unknown as ProgramWorkspaceController['mutate']}
    />);
    expect(markup).toContain('Ambiguous collision body identity.');
    expect(markup).toContain('disabled=""');
  });
  it('shows a release policy as Release with its own envelope, never as a grasp', () => {
    const markup = renderToStaticMarkup(<ProgramContactPolicyEditor
      program={program([releasePolicy as unknown as JsonValue])}
      draftRevision={1}
      busy={false}
      mutate={vi.fn() as unknown as ProgramWorkspaceController['mutate']}
    />);

    expect(markup).toContain('<option value="contact_release" selected="">');
    expect(markup).toContain('Release evidence');
    expect(markup).toContain('Maximum starting penetration (m)');
    expect(markup).toContain('value="0.002"');
    expect(markup).not.toContain('Terminal window start');
    expect(markup).not.toContain('Maximum penetration (m)');
  });

  it('round-trips an existing release policy unchanged', async () => {
    const mounted = await mount([releasePolicy as unknown as JsonValue]);
    // Leaving a field without a change re-materializes exactly the authored policy.
    const startCap = mounted.find((props) => props.type === 'number' && props.value === 0.002);
    await act(async () => { (startCap.onBlur as () => void)(); });

    const written = mounted.mutate.mock.calls.at(-1);
    expect(written?.[0]).toBe('program_draft_upsert_contact_policies');
    expect((written?.[1] as { contact_policies: unknown[] }).contact_policies).toEqual([releasePolicy]);
    await mounted.unmount();
  });

  it('writes a binary release with only its neutral, implicit metrics', async () => {
    const mounted = await mount([releasePolicy as unknown as JsonValue]);
    const evidence = mounted.find((props) => props.value === 'signed_penetration'
      && typeof props.onChange === 'function');
    await act(async () => { evidence.onChange?.({ target: { value: 'binary_contact' } }); });

    const written = mounted.mutate.mock.calls.at(-1);
    const zone = (written?.[1] as { contact_policies: Array<{ zones: unknown[] }> })
      .contact_policies[0].zones[0];
    expect(zone).toEqual({
      zone_id: 'probe_departure',
      body_pairs: [endpointPair],
      violation_response: 'hard_stop',
      release_evidence_mode: 'binary_contact',
      require_separation_by_end: true,
      allow_recontact: false,
    });
    await mounted.unmount();
  });
});
