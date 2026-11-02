import { describe, expect, it } from 'vitest';
import { parseComponentOverviewProjection } from './parsers/headless';
import { parseProgramCycleStatus } from './parsers/programRuntime';

/**
 * Backend contract fixtures: snapshots built by the real backend producers
 * (manyforge_composer/tests/backend_snapshot_fixtures.py, which also fails
 * when a committed file is stale). Each must pass the real strict parser, so
 * a backend field the frontend does not know fails here, not on a live
 * control socket.
 */
const fixtures = import.meta.glob<unknown>(
  '../testFixtures/backendSnapshots/*.json',
  { eager: true, import: 'default' },
);

const parsers: Record<string, (value: unknown) => unknown> = {
  'component-overview': (value) => parseComponentOverviewProjection(
    value,
    'headlessState.component_overview',
  ),
  'program-cycle': parseProgramCycleStatus,
};

const entries = Object.entries(fixtures).map(([path, value]) => {
  const name = path.split('/').pop() as string;
  return { name, record: name.split('.')[0], value };
});

describe('backend snapshot contract fixtures', () => {
  it('names a known record type in every fixture and covers every record type', () => {
    expect(entries.map(({ record }) => record).filter((record) => !(record in parsers)))
      .toEqual([]);
    expect(new Set(entries.map(({ record }) => record))).toEqual(new Set(Object.keys(parsers)));
  });

  it.each(entries)('$name passes its strict parser', ({ record, value }) => {
    expect(() => parsers[record](value)).not.toThrow();
  });

  it('keeps the held recovery offer and hold Stop scope that once closed the socket', () => {
    const held = entries.find(({ name }) => name === 'program-cycle.held-failures.json');
    const cycle = parseProgramCycleStatus(held?.value);
    const projection = cycle.runtimeControls;
    expect(projection?.recoveryHolds.map((hold) => hold.interventionStopScope))
      .toEqual(['own_groups', 'own_groups']);
    expect(projection?.recoveryOffer?.policy.interventionStopScope).toBe('own_groups');
  });
});
