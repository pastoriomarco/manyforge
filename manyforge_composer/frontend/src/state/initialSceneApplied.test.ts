import { describe, expect, it } from 'vitest';

import type { AppliedProgramSelection, ProgramDocument } from '../types/api';
import {
  INITIAL_SCENE_TRACKING,
  acknowledgeReloadNotSent,
  dropInitialSceneExpectation,
  expectInitialScene,
  initialSceneReloadOffered,
  initialSceneReloadReady,
  observeAppliedScene,
  observeDeploymentApplyEnd,
  observeInitialSceneCycle,
  type InitialSceneTracking,
} from './initialSceneApplied';

const program = (x: number): ProgramDocument => ({
  scene: [{ resource_id: 'box', pose: { position: [x, 0, 0] } }],
} as unknown as ProgramDocument);

function selection(revision: string, x: number): AppliedProgramSelection {
  return {
    recordDigest: `record-${revision}-${x}`, programRevision: revision, program: program(x),
  } as unknown as AppliedProgramSelection;
}

const start = () => observeAppliedScene(INITIAL_SCENE_TRACKING, selection('a', 1), false);

/** Applied revision `a`, then the operator's Program Apply to `b`. */
function applied(reload: boolean, x = 2): InitialSceneTracking {
  return observeAppliedScene(
    expectInitialScene(start(), { target: 'program', targetProgramRevision: 'b', reload }), selection('b', x), false,
  );
}

const ready = {
  cycle: { state: 'idle_after_reset' as const, programRunId: null },
  resetAllowed: true, runnerBusy: false, commandPending: false, authoringAllowed: true,
};

describe('Reload initial scene after a Program Apply or Revert', () => {
  it('waits for the new Applied Program, then for Idle, admission and an allowed Reset', () => {
    const expecting = expectInitialScene(start(), { target: 'program', targetProgramRevision: 'b', reload: true });
    // Never before the Apply made the new Program Applied.
    expect(initialSceneReloadReady(expecting, ready)).toBe(false);
    const tracking = observeAppliedScene(expecting, selection('b', 2), false);
    expect(tracking.reload).toEqual({ kind: 'waiting' });
    expect(initialSceneReloadReady(tracking, { ...ready, authoringAllowed: false })).toBe(false);
    expect(initialSceneReloadReady(tracking, { ...ready, resetAllowed: false })).toBe(false);
    expect(initialSceneReloadReady(tracking, { ...ready, runnerBusy: true })).toBe(false);
    expect(initialSceneReloadReady(tracking, ready)).toBe(true);
  });

  it('is not sent unchecked, or when the Applied initial scene did not change', () => {
    expect(applied(false).reload).toBeNull();
    expect(applied(true, 1).reload).toBeNull();
  });

  it('ignores a failed Applied read, which proves nothing', () => {
    const expecting = expectInitialScene(start(), { target: 'program', targetProgramRevision: 'b', reload: true });
    const afterGap = observeAppliedScene(observeAppliedScene(expecting, null, false), selection('b', 2), false);
    expect(afterGap.reload).toEqual({ kind: 'waiting' });
  });

  it('is not sent for an Applied Program this browser did not Apply', () => {
    const expecting = expectInitialScene(start(), { target: 'program', targetProgramRevision: 'b', reload: true });
    const other = observeAppliedScene(expecting, selection('c', 5), false);
    expect(other).toMatchObject({ expectation: null, reload: null });
  });

  it('is not sent when a run or another command came first', () => {
    const run = observeInitialSceneCycle(applied(true), { state: 'running' });
    expect(run.reload).toEqual({ kind: 'not_sent' });
    expect(initialSceneReloadReady(run, ready)).toBe(false);
    expect(acknowledgeReloadNotSent(run).reload).toBeNull();
    expect(observeInitialSceneCycle(applied(true), { state: 'idle_after_reset' }).reload).toEqual({ kind: 'waiting' });
  });
});

describe('A refused or failed Apply or Revert', () => {
  it('drops only its own expectation', () => {
    const own = { target: 'program' as const, targetProgramRevision: 'b', reload: true };
    const tracking = expectInitialScene(INITIAL_SCENE_TRACKING, own);
    expect(dropInitialSceneExpectation(tracking, own).expectation).toBeNull();
    const newer = expectInitialScene(tracking, { ...own });
    expect(dropInitialSceneExpectation(newer, own)).toBe(newer);
  });
});

describe('Reload initial scene after a Deployment Apply or Revert', () => {
  const expecting = (reload: boolean) => expectInitialScene(start(), { target: 'deployment', reload });

  it('waits for its Apply command to end without error, whatever the Applied Program', () => {
    const tracking = expecting(true);
    expect(initialSceneReloadReady(tracking, ready)).toBe(false);
    // The Applied Program may change (or not) before the command's end.
    const observed = observeAppliedScene(tracking, selection('b', 1), false);
    expect(observed.expectation).toEqual({ target: 'deployment', reload: true });
    expect(observed.reload).toBeNull();
    const ended = observeDeploymentApplyEnd(observed, true);
    expect(ended).toMatchObject({ expectation: null, reload: { kind: 'waiting' } });
    expect(initialSceneReloadReady(ended, ready)).toBe(true);
    // The Applied observation arriving after the end keeps the reload.
    expect(observeAppliedScene(ended, selection('c', 1), false).reload).toEqual({ kind: 'waiting' });
  });

  it('is not sent when the Apply failed or was refused, or unchecked', () => {
    expect(observeDeploymentApplyEnd(expecting(true), false)).toMatchObject({ expectation: null, reload: null });
    expect(observeDeploymentApplyEnd(expecting(false), true)).toMatchObject({ expectation: null, reload: null });
    // An end without a Deployment expectation changes nothing.
    expect(observeDeploymentApplyEnd(start(), true)).toEqual(start());
  });
});

describe('Reload initial scene offer', () => {
  const base = {
    target: 'program' as const,
    draftProgram: program(2),
    appliedProgram: program(1),
    savedDiffersFromApplied: false,
  };
  it('is offered with a Program Apply that changes the initial scene', () => {
    expect(initialSceneReloadOffered({ ...base, action: 'apply' })).toBe(true);
    expect(initialSceneReloadOffered({ ...base, action: 'apply', draftProgram: program(1) })).toBe(false);
    expect(initialSceneReloadOffered({ ...base, action: 'apply', appliedProgram: null })).toBe(false);
  });
  it('is offered with a Program Revert that changes the Applied Program', () => {
    expect(initialSceneReloadOffered({ ...base, action: 'revert', savedDiffersFromApplied: true })).toBe(true);
    expect(initialSceneReloadOffered({ ...base, action: 'revert' })).toBe(false);
    expect(initialSceneReloadOffered({ ...base, action: 'discard', savedDiffersFromApplied: true })).toBe(false);
  });
  it('is offered with every Deployment Apply and Revert', () => {
    const deployment = { ...base, target: 'deployment' as const, draftProgram: program(1), appliedProgram: null };
    expect(initialSceneReloadOffered({ ...deployment, action: 'apply' })).toBe(true);
    expect(initialSceneReloadOffered({ ...deployment, action: 'revert' })).toBe(true);
    expect(initialSceneReloadOffered({ ...deployment, action: 'discard' })).toBe(false);
  });
});
