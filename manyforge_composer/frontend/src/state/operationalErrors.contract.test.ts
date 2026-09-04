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
  observedAtMs = 1,
): OperationalErrorState {
  return reduceOperationalErrors(state, {
    kind: 'report',
    source,
    title: 'Viewer response failed',
    message: 'Exact diagnostic',
    attentionPolicy,
    observedAtMs,
  });
}

describe('operational error lifecycle', () => {
  it('auto-resolves a transient error but retains activation and recovery trace', () => {
    const active = report(initialOperationalErrorState(), 'composer.viewer', 'transient');
    const resolved = reduceOperationalErrors(active, {
      kind: 'resolve', source: 'composer.viewer', observedAtMs: 2,
    });

    expect(resolved.records.size).toBe(0);
    expect(resolved.history.map((event) => event.type)).toEqual([
      'composer_client_error_resolved',
      'composer_client_error_activated',
    ]);
    expect(resolved.history[1]?.details).toMatchObject({
      source: 'composer.viewer',
      detail: 'Exact diagnostic',
      attentionPolicy: 'transient',
    });
  });

  it('retains a returned acknowledgement-required error until explicit acknowledgement', () => {
    const active = report(
      initialOperationalErrorState(),
      'composer.full_stop',
      'acknowledgement_required',
    );
    const returned = reduceOperationalErrors(active, {
      kind: 'resolve', source: 'composer.full_stop', observedAtMs: 2,
    });
    const record = [...returned.records.values()][0];

    expect(record).toMatchObject({
      active: false,
      acknowledgementRequired: true,
    });
    const acknowledged = reduceOperationalErrors(returned, {
      kind: 'dismiss', id: record?.id ?? '', observedAtMs: 3,
    });
    expect(acknowledged.records.size).toBe(0);
    expect(acknowledged.history[0]?.type)
      .toBe('composer_client_error_acknowledged');
  });

  it('bounds session trace independently of occurrence count', () => {
    let state = initialOperationalErrorState();
    for (let index = 0; index < 80; index += 1) {
      const source = `composer.source.${index}`;
      state = report(state, source, 'transient', index * 2);
      state = reduceOperationalErrors(state, {
        kind: 'resolve', source, observedAtMs: index * 2 + 1,
      });
    }

    expect(state.records.size).toBe(0);
    expect(state.history).toHaveLength(128);
    expect(state.history[0]?.id).toBe('composer-session:160');
  });
});
