import { describe, expect, it } from 'vitest';

import {
  deploymentApplyResetsRun,
  nextResetApplyStep,
  resetApplyContext,
  resetApplyPhaseText,
  type ResetApplyFacts,
  type ResetApplyPlan,
  type ResetApplySequence,
} from './resetApply';

const digest = (fill: string) => `sha256:${fill.repeat(64)}`;

function plan(patch: Partial<ResetApplyPlan> = {}): ResetApplyPlan {
  return {
    correlationId: 'reset-apply.1',
    document: 'program',
    programRunId: 1,
    cycleGeneration: 7,
    draftId: 'draft-1',
    draftRevision: 3,
    draftContentRevision: digest('f'),
    appliedRevision: digest('a'),
    scope: null,
    ...patch,
  };
}

// The Reset is a WebSocket command: session 4, client sequence 118.
const reset = {
  command: { controlSessionNumber: 4, clientSequence: '118' },
  programRunId: 1, cycleGeneration: 7,
};
// Class 0 keys: session number in bytes 1-7, client sequence in bytes 8-15.
const byReset = { causalRequestKey: '00000000000000040000000000000076' };
const bySomeoneElse = { causalRequestKey: '00000000000000040000000000000077' };

const held = {
  state: 'running' as const, cycleGeneration: 7,
  programRunId: 1, causalRequestKey: null, failure: null,
};
const idle = {
  ...held, state: 'idle_after_reset' as const, cycleGeneration: 8, programRunId: null,
  ...byReset,
};

function facts(patch: Partial<ResetApplyFacts> = {}): ResetApplyFacts {
  return {
    cycle: held,
    applyAvailable: false,
    workspacePending: false,
    draft: { draftId: 'draft-1', draftRevision: 3 },
    appliedRevision: digest('a'),
    activation: null,
    ...patch,
  };
}

describe('Apply after Reset sequence', () => {
  const awaiting: ResetApplySequence = {
    plan: plan(), phase: { kind: 'awaiting_idle', reset, delivery: 'confirmed' },
  };

  it('records only bounded identities, never Draft content', () => {
    expect(resetApplyContext(plan())).toEqual({
      schemaVersion: 'manyforge.composer_reset_apply_context.v2',
      correlationId: 'reset-apply.1',
      document: 'program',
      programRunId: 1,
      cycleGeneration: 7,
      draftId: 'draft-1',
      draftRevision: 3,
      draftContentRevision: digest('f'),
      appliedRevision: digest('a'),
    });
  });

  it('treats a Reset receipt as insufficient until the cycle Idle it caused', () => {
    expect(nextResetApplyStep(awaiting, facts())).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaiting, facts({ cycle: null }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaiting, facts({ cycle: idle }))).toEqual({
      kind: 'phase', phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false },
    });
  });

  it('refuses an Idle reached by another request or a failed Reset', () => {
    expect(nextResetApplyStep(awaiting, facts({
      cycle: { ...idle, ...bySomeoneElse },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'reset', unknown: false } });
    // The same client sequence on another control session is another command.
    expect(nextResetApplyStep(awaiting, facts({
      cycle: { ...idle, causalRequestKey: '00000000000000050000000000000076' },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'reset', unknown: false } });
    expect(nextResetApplyStep(awaiting, facts({
      cycle: { ...idle, state: 'recovery_required', cycleGeneration: 7, failure: { message: 'stop fault latched' } },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'reset', message: expect.stringContaining('stop fault latched') } });
  });

  it('reconciles a Reset whose reply was lost only from the settlement it caused', () => {
    const lost: ResetApplySequence = {
      plan: plan(), phase: { kind: 'awaiting_idle', reset, delivery: 'unconfirmed' },
    };
    expect(resetApplyPhaseText(lost.phase)).toContain('Reset delivery unknown: reconciling');
    expect(nextResetApplyStep(lost, facts({
      cycle: { ...idle, state: 'recovery_required', cycleGeneration: 7, causalRequestKey: null },
    }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(lost, facts({ applyAvailable: true }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(lost, facts({ cycle: idle }))).toEqual({
      kind: 'phase', phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false },
    });
  });

  it('Applies only the exact confirmed Draft once authoring is admitted', () => {
    const applying: ResetApplySequence = { plan: plan(), phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false } };
    expect(nextResetApplyStep(applying, facts({ cycle: idle }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(applying, facts({ cycle: idle, applyAvailable: true, workspacePending: true })))
      .toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(applying, facts({ cycle: idle, applyAvailable: true, appliedRevision: null })))
      .toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(applying, facts({ cycle: idle, applyAvailable: true })))
      .toEqual({ kind: 'send_apply' });
    // Another browser edited the Draft after Reset: no Apply.
    expect(nextResetApplyStep(applying, facts({
      cycle: idle, applyAvailable: true, draft: { draftId: 'draft-1', draftRevision: 4 },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'apply', unknown: false } });
    expect(nextResetApplyStep(applying, facts({
      cycle: idle, applyAvailable: true, appliedRevision: digest('d'),
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'apply' } });
    expect(nextResetApplyStep({ ...applying, phase: { ...applying.phase, sent: true } as never },
      facts({ cycle: idle, applyAvailable: true }))).toEqual({ kind: 'wait' });
  });

  it('rechecks the settled Reset identity while waiting and immediately before Apply', () => {
    const applying: ResetApplySequence = {
      plan: plan(), phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false },
    };
    // Waiting for admission does not skip the identity check.
    for (const cycle of [
      { ...idle, cycleGeneration: 9 },
      { ...idle, ...bySomeoneElse },
      { ...idle, state: 'running' as const, programRunId: 2 },
      { ...idle, programRunId: 2 },
    ]) {
      for (const applyAvailable of [false, true]) {
        const step = nextResetApplyStep(applying, facts({ cycle, applyAvailable }));
        expect(step).toMatchObject({ phase: { kind: 'failed', stage: 'apply', unknown: false } });
        expect(step).not.toEqual({ kind: 'send_apply' });
      }
    }
    expect(nextResetApplyStep(applying, facts({
      cycle: idle, applyAvailable: true,
    }))).toEqual({ kind: 'send_apply' });
    expect(nextResetApplyStep(applying, facts({ cycle: { ...idle, cycleGeneration: 9 } })))
      .toMatchObject({ phase: { message: expect.stringContaining('cycle changed after Reset reached Idle') } });
    // An absent observation proves nothing: wait, never send.
    expect(nextResetApplyStep(applying, facts({ cycle: null, applyAvailable: true }))).toEqual({ kind: 'wait' });
  });

  it('follows its exact activation and never claims success early', () => {
    const apply = { applyId: 1, applyGeneration: 3 };
    const awaitingApply: ResetApplySequence = { plan: plan(), phase: { kind: 'awaiting_apply', apply } };
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: { applyId: 0, applyGeneration: 3, state: 'succeeded', failure: null },
    }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: { ...apply, state: 'pending', failure: null },
    }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: { ...apply, state: 'succeeded', failure: null },
    }))).toEqual({ kind: 'phase', phase: { kind: 'applied', apply } });
    expect(resetApplyPhaseText({ kind: 'applied', apply }))
      .toBe('Applied; awaiting Execute/Run; not saved to Scenario.');
    expect(resetApplyPhaseText(awaitingApply.phase, '3f9a0123456789ab'))
      .toBe('Apply 3f9a/apply1 accepted; waiting for activation to settle.');
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: {
        ...apply, state: 'unknown_effect',
        failure: {
          code: 'manager_lost', message: 'reply lost', phase: 'accepted', effectDisposition: 'unknown',
          causeCode: 'manager_lost', causeDetails: {}, producer: 'component_manager', effectBoundary: 'unknown',
        },
      },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'apply', unknown: true, apply } });
  });
});

describe('Deployment Reset and Apply', () => {
  const snapshot = (state: string) => ({ programCycle: { state } }) as never;
  it('resets only a stopped or held run; the server Apply resets a finished one itself', () => {
    expect(deploymentApplyResetsRun(null)).toBe(false);
    expect(deploymentApplyResetsRun(snapshot('idle_after_reset'))).toBe(false);
    expect(deploymentApplyResetsRun(snapshot('terminal_awaiting_reset'))).toBe(false);
    expect(deploymentApplyResetsRun(snapshot('running'))).toBe(true);
  });
  it('records the Deployment document in the Apply context', () => {
    expect(resetApplyContext(plan({ document: 'deployment', scope: { r: 1 } })).document).toBe('deployment');
  });
});
