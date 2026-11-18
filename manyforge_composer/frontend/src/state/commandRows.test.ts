import { describe, expect, it } from 'vitest';

import type { ApplyCommandRow, CommandEnd, CommandRow } from '../types/api';
import { authoringOperation, busyReason } from './busyOperation';
import {
  awaitCommandSuccess,
  CommandEndedError,
  commandEndText,
  commandProgressText,
  CommandRowWaiter,
  withApplyStep,
} from './commandRows';

const KEY = 'a'.repeat(32);

function row(overrides: Partial<CommandRow> = {}): CommandRow {
  return { kind: 'start', requestKey: KEY, step: 'claiming', acceptedAtNs: 1, end: null, ...overrides };
}

function ended(outcome: CommandEnd['outcome'], code: string | null, details: CommandEnd['details'] = null): CommandEnd {
  return { outcome, code, details, finishedAtNs: 2 };
}

describe('what the operator reads from a command row', () => {
  it('names the command and its step; no step yet is starting', () => {
    expect(commandProgressText(row())).toBe('Execute, claiming the cell');
    expect(commandProgressText(row({ kind: 'reset', step: 'restoring_scene' })))
      .toBe('Reset, restoring the Scene');
    expect(commandProgressText(row({ step: null, acceptedAtNs: null }))).toBe('Execute, starting');
  });

  it('gives the catalogue text for an end other than succeeded, never the raw code', () => {
    expect(commandEndText(row({ end: ended('succeeded', null) }))).toBeNull();
    expect(commandEndText(row())).toBeNull();
    const failed = commandEndText(row({ end: ended('failed', 'program_run_started_then_failed') }));
    expect(failed).toBe('Execute: Run failed. The Program run started and then failed. Reset is required.');
    const unknown = commandEndText(row({ kind: 'start_motion', end: ended('unknown', 'program_cycle_reconciliation_required') }));
    expect(unknown).toContain('Run: Program outcome not confirmed.');
    expect(unknown).not.toContain('program_cycle_reconciliation_required');
    const uncatalogued = commandEndText(row({ end: ended('refused', 'no_such_code_in_catalogue') }));
    expect(uncatalogued).toBe('Execute: Ended without a known reason.');
    expect(uncatalogued).not.toContain('no_such_code');
  });

  it('keeps the typed code and scalar details on the error for code-based handling', () => {
    const error = new CommandEndedError(row({ kind: 'start_motion',
      end: ended('refused', 'rule_holds', { rule_id: 'door', nested: { a: 1 } }) }));
    expect(error.structuredError).toEqual({
      code: 'rule_holds', message: '', details: { rule_id: 'door' }, origin: null,
    });
    expect(error.message).toContain('Run: ');
  });

  it('adds the published step to the busy tooltip of this page\'s Apply-level command', () => {
    const applying: ApplyCommandRow = {
      kind: 'deployment_apply', requestKey: KEY, step: 'disabling',
      stepSource: { status: 'component_action', componentId: 'arm' }, acceptedAtNs: 1, end: null,
    };
    const rows = { inProgress: applying, ended: [] };
    expect(busyReason(withApplyStep(authoringOperation('apply'), rows)))
      .toBe('Apply is in progress: disabling components.');
    expect(busyReason(withApplyStep(authoringOperation('load'), { inProgress: { ...applying, step: null, acceptedAtNs: null }, ended: [] })))
      .toBe('Load is in progress: starting.');
    // A Draft edit is no Apply-level command; nothing in progress adds nothing.
    expect(busyReason(withApplyStep(authoringOperation('edit'), rows))).toBe('A Draft edit is in progress.');
    expect(busyReason(withApplyStep(authoringOperation('apply'), { inProgress: null, ended: [] })))
      .toBe('Apply is in progress.');
    expect(withApplyStep(null, rows)).toBeNull();
  });
});

describe('waiting for a command row to end', () => {
  it('settles when its row ends, at once when it already has, and keeps waiting without rows', async () => {
    const waiter = new CommandRowWaiter();
    const pending = waiter.wait(KEY);
    let settled: CommandRow | null | undefined;
    void pending.then((value) => { settled = value; });
    waiter.observe({ inProgress: row(), ended: [] });
    // A lost connection publishes no rows: the wait stays.
    waiter.observe(null);
    await Promise.resolve();
    expect(settled).toBeUndefined();
    const done = row({ end: ended('succeeded', null) });
    waiter.observe({ inProgress: null, ended: [done] });
    await expect(pending).resolves.toEqual(done);
    await expect(waiter.wait(KEY)).resolves.toEqual(done);
  });

  it('resolves success, throws the end otherwise, and settles abandoned waits with null', async () => {
    const waiter = new CommandRowWaiter();
    await expect(awaitCommandSuccess(row({ end: ended('succeeded', null) }), waiter)).resolves.toMatchObject({ kind: 'start' });
    await expect(awaitCommandSuccess(row({ end: ended('refused', 'rule_holds') }), waiter))
      .rejects.toBeInstanceOf(CommandEndedError);
    const waiting = awaitCommandSuccess(row({ requestKey: 'b'.repeat(32) }), waiter);
    waiter.abandon();
    await expect(waiting).resolves.toBeNull();
  });
});
