import { describe, expect, it } from 'vitest';

import {
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
    programRunId: 'run.1',
    programRunnerOwnerEpoch: 'runner.epoch.1',
    cycleGeneration: 7,
    draftId: 'draft-1',
    draftRevision: 3,
    draftContentRevision: digest('f'),
    appliedRevision: digest('a'),
    activationScope: null,
    ...patch,
  };
}

const reset = {
  requestId: 'program-cycle-reset.1', correlationId: 'reset-apply.1',
  programRunId: 'run.1', programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 7,
};

const held = {
  state: 'running' as const, programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 7,
  programRunId: 'run.1', causalCorrelationId: null, failure: null,
};
const idle = {
  ...held, state: 'idle_after_reset' as const, cycleGeneration: 8, programRunId: null,
  causalCorrelationId: 'reset-apply.1',
};

function facts(patch: Partial<ResetApplyFacts> = {}): ResetApplyFacts {
  return {
    cycle: held,
    applyAvailable: false,
    workspacePending: false,
    draft: { draftId: 'draft-1', draftRevision: 3 },
    appliedRevision: digest('a'),
    activation: null,
    activationScope: null,
    correlatedApply: null,
    ...patch,
  };
}

describe('Apply after Reset sequence', () => {
  const awaiting: ResetApplySequence = {
    plan: plan(), phase: { kind: 'awaiting_idle', reset, delivery: 'confirmed' },
  };

  it('records only bounded identities, never Draft content', () => {
    expect(resetApplyContext(plan())).toEqual({
      schemaVersion: 'manyforge.composer_reset_apply_context.v1',
      correlationId: 'reset-apply.1',
      document: 'program',
      programRunId: 'run.1',
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 7,
      draftId: 'draft-1',
      draftRevision: 3,
      draftContentRevision: digest('f'),
      appliedRevision: digest('a'),
    });
  });

  it('treats a Reset receipt as insufficient until its correlated cycle is Idle', () => {
    expect(nextResetApplyStep(awaiting, facts())).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaiting, facts({ cycle: null }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaiting, facts({ cycle: idle }))).toEqual({
      kind: 'phase', phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false },
    });
  });

  it('refuses an Idle reached by another request, a failed Reset and an owner restart', () => {
    expect(nextResetApplyStep(awaiting, facts({
      cycle: { ...idle, causalCorrelationId: 'someone-else' },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'reset', unknown: false } });
    expect(nextResetApplyStep(awaiting, facts({
      cycle: { ...idle, state: 'recovery_required', cycleGeneration: 7, failure: { message: 'stop fault latched' } },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'reset', message: expect.stringContaining('stop fault latched') } });
    expect(nextResetApplyStep(awaiting, facts({
      cycle: { ...idle, programRunnerOwnerEpoch: 'runner.epoch.2' },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'reset', unknown: true } });
  });

  it('reconciles a Reset whose reply was lost only from its correlated settlement', () => {
    const lost: ResetApplySequence = {
      plan: plan(), phase: { kind: 'awaiting_idle', reset, delivery: 'unconfirmed' },
    };
    expect(resetApplyPhaseText(lost.phase)).toContain('Reset delivery unknown — reconciling');
    expect(nextResetApplyStep(lost, facts({
      cycle: { ...idle, state: 'recovery_required', cycleGeneration: 7, causalCorrelationId: null },
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
    // A Deployment with no compiled Applied revision is not waited for.
    expect(nextResetApplyStep(
      { plan: plan({ document: 'deployment', appliedRevision: null }), phase: applying.phase },
      facts({ cycle: idle, applyAvailable: true, appliedRevision: null }),
    )).toEqual({ kind: 'send_apply' });
  });

  it('rechecks the settled Reset identity while waiting and immediately before Apply', () => {
    const applying: ResetApplySequence = {
      plan: plan(), phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false },
    };
    // Waiting for admission does not skip the identity check.
    for (const cycle of [
      { ...idle, programRunnerOwnerEpoch: 'runner.epoch.2' },
      { ...idle, cycleGeneration: 9 },
      { ...idle, causalCorrelationId: 'someone-else' },
      { ...idle, state: 'running' as const, programRunId: 'run.2' },
      { ...idle, programRunId: 'run.2' },
    ]) {
      for (const applyAvailable of [false, true]) {
        const step = nextResetApplyStep(applying, facts({ cycle, applyAvailable }));
        expect(step).toMatchObject({ phase: { kind: 'failed', stage: 'apply', unknown: false } });
        expect(step).not.toEqual({ kind: 'send_apply' });
      }
    }
    expect(nextResetApplyStep(applying, facts({ cycle: { ...idle, programRunnerOwnerEpoch: 'runner.epoch.2' } })))
      .toMatchObject({ phase: { message: expect.stringContaining('runner restarted after Reset settled') } });
    expect(nextResetApplyStep(applying, facts({ cycle: { ...idle, cycleGeneration: 9 } })))
      .toMatchObject({ phase: { message: expect.stringContaining('cycle changed after Reset reached Idle') } });
    // An absent observation proves nothing: wait, never send.
    expect(nextResetApplyStep(applying, facts({ cycle: null, applyAvailable: true }))).toEqual({ kind: 'wait' });
  });

  it('refuses a Deployment Apply whose confirmed activation scope changed', () => {
    const scope = {
      deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 2, deploymentRevision: digest('e'),
      programDraftId: 'draft-1', programDraftRevision: 3, programRevision: digest('f'),
      sourceDocumentDigest: digest('s'), programRoleAssignment: { component_roles: [], target_roles: [] },
    };
    const applying: ResetApplySequence = {
      plan: plan({ document: 'deployment', activationScope: scope, draftId: 'deployment-draft-1', draftRevision: 2 }),
      phase: { kind: 'applying', reset, idle: { cycleGeneration: 8 }, sent: false },
    };
    const ready = { cycle: idle, applyAvailable: true, draft: { draftId: 'deployment-draft-1', draftRevision: 2 } };
    expect(nextResetApplyStep(applying, facts({
      ...ready, activationScope: { ...scope, programRoleAssignment: { target_roles: [], component_roles: [] } },
    }))).toEqual({ kind: 'send_apply' });
    for (const changed of [
      { ...scope, programDraftRevision: 4 },
      { ...scope, programRevision: digest('9') },
      { ...scope, sourceDocumentDigest: digest('8') },
      { ...scope, programRoleAssignment: { component_roles: [{ role: 'arm' }], target_roles: [] } },
      { ...scope, deploymentRevision: digest('7') },
    ]) {
      expect(nextResetApplyStep(applying, facts({ ...ready, activationScope: changed })))
        .toMatchObject({ phase: { kind: 'failed', stage: 'apply', message: expect.stringContaining('Draft changed after confirmation; review again') } });
    }
    expect(nextResetApplyStep(applying, facts({ ...ready, activationScope: null })))
      .toMatchObject({ phase: { kind: 'failed', stage: 'apply' } });
  });

  it('follows the Apply the operator confirmed after a disruptive impact, by correlation', () => {
    const apply = { applyId: 'apply-2', applyGeneration: 4 };
    const handed: ResetApplySequence = { plan: plan({ document: 'deployment' }), phase: { kind: 'confirmation_required' } };
    expect(nextResetApplyStep(handed, facts())).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(handed, facts({ correlatedApply: apply })))
      .toEqual({ kind: 'phase', phase: { kind: 'awaiting_apply', apply } });
  });

  it('follows its exact activation and never claims success early', () => {
    const apply = { applyId: 'apply-1', applyGeneration: 3 };
    const awaitingApply: ResetApplySequence = { plan: plan(), phase: { kind: 'awaiting_apply', apply } };
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: { applyId: 'apply-0', applyGeneration: 3, state: 'succeeded', failure: null },
    }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: { ...apply, state: 'pending', failure: null },
    }))).toEqual({ kind: 'wait' });
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: { ...apply, state: 'succeeded', failure: null },
    }))).toEqual({ kind: 'phase', phase: { kind: 'applied', apply } });
    expect(resetApplyPhaseText({ kind: 'applied', apply }))
      .toBe('Applied; awaiting Execute/Run; not saved to Scenario.');
    expect(nextResetApplyStep(awaitingApply, facts({
      activation: {
        ...apply, state: 'unknown_effect',
        failure: { code: 'manager_lost', message: 'reply lost', phase: 'accepted', effectDisposition: 'unknown' },
      },
    }))).toMatchObject({ phase: { kind: 'failed', stage: 'apply', unknown: true, apply } });
  });
});
