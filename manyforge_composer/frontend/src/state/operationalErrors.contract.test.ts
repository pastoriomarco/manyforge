import { describe, expect, it } from 'vitest';

import {
  initialOperationalErrorState,
  reduceOperationalErrors,
  type OperationalErrorState,
} from './operationalErrors';

function report(
  state: OperationalErrorState,
  source: string,
  attentionPolicy: 'transient' | 'acknowledgement_required',
): OperationalErrorState {
  return reduceOperationalErrors(state, {
    kind: 'report',
    source,
    title: 'Viewer response failed',
    message: 'Exact diagnostic',
    attentionPolicy,
  });
}

describe('operational error lifecycle', () => {
  it('auto-resolves a transient error while durable trace remains backend-owned', () => {
    const active = report(initialOperationalErrorState(), 'composer.viewer', 'transient');
    const resolved = reduceOperationalErrors(active, {
      kind: 'resolve', source: 'composer.viewer',
    });

    expect(resolved.records.size).toBe(0);
    expect(resolved.sequence).toBe(2);
  });

  it('retains a returned acknowledgement-required error until explicit acknowledgement', () => {
    const active = report(
      initialOperationalErrorState(),
      'composer.full_stop',
      'acknowledgement_required',
    );
    const returned = reduceOperationalErrors(active, {
      kind: 'resolve', source: 'composer.full_stop',
    });
    const record = [...returned.records.values()][0];

    expect(record).toMatchObject({
      active: false,
      acknowledgementRequired: true,
    });
    const acknowledged = reduceOperationalErrors(returned, {
      kind: 'dismiss', id: record?.id ?? '',
    });
    expect(acknowledged.records.size).toBe(0);
    expect(acknowledged.sequence).toBe(3);
  });

  it('clears every message at Reset or Scenario New', () => {
    const state = report(
      report(initialOperationalErrorState(), 'composer.a', 'acknowledgement_required'),
      'composer.b', 'acknowledgement_required',
    );
    const cleared = reduceOperationalErrors(state, { kind: 'clear' });
    expect(cleared.records.size).toBe(0);
    expect(reduceOperationalErrors(cleared, { kind: 'clear' })).toBe(cleared);
  });

  it('retains no browser-side history while issuing stable unique identities', () => {
    let state = initialOperationalErrorState();
    for (let index = 0; index < 80; index += 1) {
      const source = `composer.source.${index}`;
      state = report(state, source, 'transient');
      state = reduceOperationalErrors(state, {
        kind: 'resolve', source,
      });
    }

    expect(state.records.size).toBe(0);
    expect(state.sequence).toBe(160);
  });
});
