import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { PersistentKeyValues } from '../types/api';
import {
  PersistentKeyResetList,
  PersistentKeyValuesView,
  persistentKeyResetRows,
} from './PersistentKeyValues';

const START = '3f9a000000000001';
/** An earlier application start: its run numbers are another start's. */
const EARLIER_START = 'b07c000000000002';

function values(programRunId: number | null = null): PersistentKeyValues {
  return {
    scenarioId: 'cell-pick',
    projectPath: '/projects/cell-pick.scenario',
    programRevision: `sha256:${'a'.repeat(64)}`,
    programRunId,
    sequence: 5,
    keys: [
      {
        key: 'layer', type: 'int64', access: 'read_write',
        initial: 0, stored: true, value: 3, sequence: 4,
      },
      {
        key: 'limit', type: 'float64', access: 'read',
        initial: undefined, stored: false, value: null, sequence: null,
      },
    ],
    edits: [
      {
        sequence: 4, key: 'layer', action: 'set', applicationStartId: START,
        requestKey: 'a'.repeat(32), sourceLabel: 'operator_ui', programRunId: null, nodeInstanceId: null, nodeAttemptId: null, reason: null, detail: null,
      },
      {
        sequence: 5, key: 'layer', action: 'set', applicationStartId: START,
        requestKey: null, sourceLabel: 'program_node', programRunId: 3, nodeInstanceId: 'set_layer', nodeAttemptId: 2,
        reason: null, detail: null,
      },
    ],
  };
}

function render(
  current: PersistentKeyValues,
  error: string | null = null,
  applicationStartId: string | null = START,
) {
  return renderToStaticMarkup(
    <PersistentKeyValuesView
      values={current}
      applicationStartId={applicationStartId}
      sceneResourceIds={[]}
      pending={false}
      error={error}
      onSet={vi.fn()}
      onReset={vi.fn()}
      onClear={vi.fn()}
      onRefresh={vi.fn()}
    />,
  );
}

describe('stored persistent values', () => {
  it('offers Edit initial only where the declaration can be opened', () => {
    expect(render(values())).not.toContain('Edit initial');
    const markup = renderToStaticMarkup(
      <PersistentKeyValuesView
        values={values()}
        applicationStartId={START}
        sceneResourceIds={[]}
        pending={false}
        error={null}
        onSet={vi.fn()}
        onReset={vi.fn()}
        onClear={vi.fn()}
        onRefresh={vi.fn()}
        onEditInitial={vi.fn()}
      />,
    );
    expect(markup).toMatch(/data-stored-key="layer".*?>Edit value<\/button>.*?>Edit initial<\/button>.*?>Reset to initial<\/button>/);
  });

  it('shows each key with its stored or starting value and its value edits', () => {
    const markup = render(values());

    expect(markup).toContain('Stored values');
    expect(markup).toContain('Kept between runs · Scenario cell-pick');
    expect(markup).toContain('<code>layer</code> · int64 · stored <strong>3</strong>');
    expect(markup).toContain('<code>limit</code> · float64 · not stored, starts from empty');
    // Read-only keys refuse every operator change: Edit and Reset alike.
    expect(markup).toMatch(
      /data-stored-key="limit".*?<button type="button" disabled="" title="Declared read-only: operator writes are refused.">Edit value<\/button>/,
    );
    expect(markup).toMatch(
      /data-stored-key="limit".*?disabled="" title="Declared read-only: operator changes are refused.">Reset to initial<\/button>/,
    );
    expect(markup).toMatch(/<button type="button">Clear stored values<\/button>/);
    expect(markup).toContain('Recent stored-value edits (2)');
    expect(markup).toContain('<code>4</code> set layer · operator_ui</li>');
    // A node write names the node and run that made it, the run in the
    // display form of the start that wrote it (D4).
    expect(markup).toContain(
      '<code>5</code> set layer · program_node · node <code>set_layer</code> · run <code>3f9a/run3</code>',
    );
    // The run is the edit's own start's, also while the current one is unknown.
    expect(render(values(), null, null)).toContain(' · run <code>3f9a/run3</code>');
  });

  it('shows a value written under an earlier start with that start, not the current one', () => {
    const current = values();
    const markup = render({
      ...current,
      edits: [{ ...current.edits[1], applicationStartId: EARLIER_START }],
    });

    expect(markup).toContain(
      '<code>5</code> set layer · program_node · node <code>set_layer</code> · run <code>b07c/run3</code>',
    );
    expect(markup).not.toContain('3f9a/run3');
  });

  it('routes edits through a live run and keeps Clear for after the run', () => {
    const markup = render(values(3), 'persistent_key_run_conflict: refused');

    expect(markup).toContain('Run <code>3f9a/run3</code> holds these values; edits go through it.');
    expect(markup).toContain('role="alert">persistent_key_run_conflict: refused</p>');
    // A writable key's Reset goes through the live run.
    expect(markup).toMatch(/data-stored-key="layer".*?<button type="button">Reset to initial<\/button>/);
    expect(markup).toMatch(
      /disabled="" title="A run holds these values: reset keys one by one, or clear after the run.">Clear stored values/,
    );
  });

  it('clears writable keys only: a cleared key starts from its initial value, a read key keeps its row', () => {
    const current = values();
    // After Clear: the writable key is no longer stored; the read key's row stays.
    const cleared = render({
      ...current,
      keys: [
        { ...current.keys[0], stored: false, value: null, sequence: null },
        { ...current.keys[1], initial: 0.5, stored: true, value: 0.7, sequence: 3 },
      ],
    });
    expect(cleared).toContain('<code>layer</code> · int64 · not stored, starts from 0</span>');
    expect(cleared).toContain('<code>limit</code> · float64 · stored <strong>0.7</strong>');
    // Only a read key is stored: Clear has nothing to reset.
    expect(cleared).toMatch(
      /<button type="button" disabled="" title="No writable key has a stored value.">Clear stored values<\/button>/,
    );
  });

  it('names dropped values', () => {
    const current = values();
    const markup = render({
      ...current,
      keys: current.keys.map((row) => ({ ...row, stored: false, value: null, sequence: null })),
      edits: [{
        sequence: 6, key: null, action: 'dropped', applicationStartId: START,
        requestKey: null, sourceLabel: 'committed_apply',
        programRunId: null, nodeInstanceId: null, nodeAttemptId: null,
        reason: 'key_not_declared_persistent', detail: null,
      }],
    });

    expect(markup).toContain(
      '<code>6</code> dropped all keys · committed_apply · <code>key_not_declared_persistent</code>',
    );
  });
});

describe('Reset with Reload initial scene: the stored values it resets', () => {
  it('lists each stored value with its current and new value; unstored keys are not listed', () => {
    const current = values();
    const rows = persistentKeyResetRows({
      ...current,
      keys: [
        ...current.keys,
        // A read key is reset too; without `initial:` it becomes empty.
        {
          key: 'pallets', type: 'int64', access: 'read',
          initial: undefined, stored: true, value: 12, sequence: 5,
        },
      ],
    });

    expect(rows).toEqual([
      { key: 'layer', current: '3', next: '0' },
      { key: 'pallets', current: '12', next: 'empty' },
    ]);
    const markup = renderToStaticMarkup(<PersistentKeyResetList rows={rows} />);
    expect(markup).toContain('Stored values reset to their initial values:');
    expect(markup).toContain('<li><code>layer</code>: 3 → 0</li>');
    expect(markup).toContain('<li><code>pallets</code>: 12 → empty</li>');
  });

  it('shows nothing when no stored value would change', () => {
    const current = values();
    const rows = persistentKeyResetRows({
      ...current,
      // Already at `initial:`, or not stored at all.
      keys: [{ ...current.keys[0], value: 0 }, current.keys[1]],
    });

    expect(rows).toEqual([]);
    expect(renderToStaticMarkup(<PersistentKeyResetList rows={rows} />)).toBe('');
  });
});
