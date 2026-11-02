import { describe, expect, it, vi } from 'vitest';

import { activationFailureNotice } from './activationNotices';

/** The notice, asserted present (an acknowledged failure returns null). */
function shownNotice(...args: Parameters<typeof activationFailureNotice>) {
  const notice = activationFailureNotice(...args);
  if (notice === null) throw new Error('Expected an activation notice.');
  return notice;
}

describe('activation failure notice', () => {
  it('keeps the owner failure phase, code and effect disposition', () => {
    const notice = shownNotice(
      'activation.a:1',
      'apply.a',
      {
        state: 'unknown_effect',
        failureAcknowledgement: 'unavailable',
        failure: {
          code: 'runtime_composition_publication_verification_failed',
          message: 'native candidate routes are not ready.',
          phase: 'runtime_converged',
          effectDisposition: 'unknown',
          causeCode: 'typed_cause_code',
          causeDetails: {},
          producer: 'component_manager',
          effectBoundary: 'unknown',
        },
      },
      "/workspace/scenarios/operator's test cell.scenario",
      false,
    );
    expect(notice).toMatchObject({
      severity: 'error', domain: 'activation', title: 'Activation outcome unknown',
    });
    expect(notice.message).toContain('Failed during runtime converged');
    expect(notice.message).toContain('(runtime_composition_publication_verification_failed)');
    expect(notice.message).toContain('effects are unknown');
    expect(notice.message).toContain('Applied state was not confirmed (apply.a)');
    expect(notice.message).toContain('The runtime remains locked');
    expect(notice.message).toContain(
      'Stop the workbench and keep evidence: ./scripts/compose-dev.sh stop --keep-evidence',
    );
    expect(notice.message).toMatch(
      /Reload: \.\/scripts\/compose-dev\.sh stop --keep-evidence && \.\/scripts\/compose-dev\.sh start --project 'scenarios\/operator'"'"'s test cell\.scenario'$/,
    );
    expect(notice.message).toContain('run from the repository root');
    expect(notice.message).toContain('unsaved changes will be lost');
    expect(notice.message).toContain(
      'Evidence is written under .manyforge-dev/evidence/ and its exact path is printed in the terminal.',
    );
    expect(notice.message).toContain(
      'page disconnects when the workbench stops and may need a refresh after Reload',
    );
    expect(notice.message).toContain('invalid Saved Scenario leaves the runtime closed');
    expect(notice.message).toContain('typed_cause_code');
    expect(notice.message).toContain('component_manager');
    expect(notice.message).toContain('Execution never resumes automatically');
  });

  it('does not prescribe recovery for a known pre-effect refusal, and dismisses it locally', () => {
    const run = vi.fn();
    const dismiss = vi.fn();
    const notice = shownNotice('activation.b:1', 'apply.b', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
        causeCode: 'typed_cause_code',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'unknown',
      },
    }, null, true, { busy: false, error: null, outcomeUnknown: false, run, dismiss });
    // A failed Apply is an error (item 21): red, never a warning.
    expect(notice).toMatchObject({ title: 'Activation failed', severity: 'error' });
    expect(notice.acknowledgeable).toBeUndefined();
    expect(notice.message).toContain(
      'Candidate preflight was refused. Failed during accepted (manager_apply_preflight_refused); its effects are known.',
    );
    expect(notice.message).toContain(
      'Manager recorded a known pre-effect refusal.',
    );
    expect(notice.message).toContain('current owner confirms authoring admission is open');
    expect(notice.message).toContain('cold restart is not required');
    expect(notice.message).toContain('Correct the reported cause before trying again');
    expect(notice.message).not.toContain('stop --keep-evidence');
    expect(notice.message).not.toContain('start --project');
    // Refused before the fence: nothing is held, so its one Acknowledge hides it here.
    expect(notice.actions).toEqual([expect.objectContaining({
      id: 'acknowledge-apply-failure', label: 'Acknowledge',
      title: expect.stringContaining('refused before its fence'),
    })]);
    notice.actions?.[0]?.run();
    expect(dismiss).toHaveBeenCalledTimes(1);
    expect(run).not.toHaveBeenCalled();
  });

  it('sends the durable Acknowledge while the Manager offers it', () => {
    const run = vi.fn();
    const failure = {
      code: 'manager_apply_preflight_refused',
      message: 'Candidate preflight was refused.',
      // The Manager offers the durable Acknowledge only after the fence.
      phase: 'fence_accepted' as const,
      effectDisposition: 'known' as const,
      causeCode: 'typed_cause_code',
      causeDetails: {},
      producer: 'component_manager',
      effectBoundary: 'before_effect' as const,
    };
    const available = shownNotice('activation.ack:1', 'apply.ack', {
      state: 'failed', failure, failureAcknowledgement: 'available',
    }, null, true, { busy: true, error: null, outcomeUnknown: false, run, dismiss: vi.fn() });

    expect(available.title).toBe('Activation failed');
    expect(available.severity).toBe('error');
    expect(available.actions).toEqual([expect.objectContaining({
      label: 'Acknowledging…', disabled: true,
      title: expect.stringContaining('Release this failed Apply'),
    })]);
    expect(available.message).toContain('Candidate preflight was refused.');
    available.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);

    const unknown = shownNotice('activation.ack:2', 'apply.unknown', {
      state: 'unknown_effect',
      failureAcknowledgement: 'unavailable',
      failure: {
        ...failure,
        effectDisposition: 'unknown' as const,
        effectBoundary: 'unknown' as const,
      },
    }, null, true, { busy: false, error: null, outcomeUnknown: false, run, dismiss: vi.fn() });
    // After effect the Manager never offers acknowledgement: cold recovery, no button.
    expect(unknown.actions).toBeUndefined();
  });

  it('keeps the notice and sends the durable Acknowledge again after a proven no-effect refusal while still offered', () => {
    const run = vi.fn();
    const notice = shownNotice('activation.ack:3', 'apply.ack', {
      state: 'failed',
      // The refusal had `effect: none` and the Manager still offers acknowledgement.
      failureAcknowledgement: 'available',
      failure: {
        code: 'apply_admission_failed',
        message: 'The Apply was refused before effects began.',
        phase: 'fence_accepted',
        effectDisposition: 'known',
        causeCode: 'stale_generation',
        causeDetails: {},
        producer: 'kernel',
        effectBoundary: 'before_effect',
      },
    }, null, false, {
      busy: false,
      error: 'Acknowledgement was refused (The predecessor changed.).',
      outcomeUnknown: false,
      run,
      dismiss: vi.fn(),
    });

    expect(notice.severity).toBe('error');
    // The last refusal's reason is shown inline; the notice stays.
    expect(notice.message).toContain('Acknowledgement: Acknowledgement was refused (The predecessor changed.).');
    expect(notice.actions).toEqual([expect.objectContaining({
      label: 'Acknowledge', disabled: false, title: expect.stringContaining('Release this failed Apply'),
    })]);
    notice.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);
  });

  it('after an unknown outcome says so, names the cold restart, and only re-reads', () => {
    const notice = shownNotice('activation.ack:4', 'apply.ack', {
      state: 'failed',
      // Even if a read says available, a sent attempt with an unknown outcome is never resent.
      failureAcknowledgement: 'available',
      failure: {
        code: 'apply_admission_failed',
        message: 'The Apply was refused before effects began.',
        phase: 'fence_accepted',
        effectDisposition: 'known',
        causeCode: 'stale_generation',
        causeDetails: {},
        producer: 'kernel',
        effectBoundary: 'before_effect',
      },
    }, null, false, { busy: false, error: 'Acknowledgement was not confirmed.', outcomeUnknown: true, run: vi.fn(), dismiss: vi.fn() });

    expect(notice.severity).toBe('error');
    expect(notice.message).toContain('The acknowledgement outcome is unknown.');
    expect(notice.message).toContain('permits no second attempt, so nothing is resent');
    expect(notice.message).toContain('short of a cold restart');
    expect(notice.message).toContain('./scripts/compose-dev.sh stop --keep-evidence');
    expect(notice.actions).toEqual([expect.objectContaining({
      label: 'Acknowledge', title: expect.stringContaining('Re-read this Apply'),
    })]);
  });

  it('does not treat unavailable as released after the fence: it stays and says so', () => {
    const notice = shownNotice('activation.ack:5', 'apply.ack', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'apply_admission_failed',
        message: 'The Apply was refused before effects began.',
        phase: 'fence_accepted',
        effectDisposition: 'known',
        causeCode: 'stale_generation',
        causeDetails: {},
        producer: 'kernel',
        effectBoundary: 'before_effect',
      },
    }, null, false, { busy: false, error: null, outcomeUnknown: false, run: vi.fn(), dismiss: vi.fn() });

    expect(notice.severity).toBe('error');
    expect(notice.message).toContain(
      'Manager reports acknowledgement unavailable, which does not prove the Apply hold is released',
    );
    expect(notice.actions).toEqual([expect.objectContaining({
      title: expect.stringContaining('Re-read this Apply'),
    })]);
  });

  it.each([
    ['accepted', 'known', 'unknown', 'unavailable'],
    ['accepted', 'known', 'before_effect', 'unavailable'],
    ['accepted', 'known', 'before_effect', 'available'],
    ['fence_accepted', 'known', 'before_effect', 'unavailable'],
    ['fence_accepted', 'known', 'before_effect', 'available'],
  ] as const)('is red with exactly one Acknowledge (phase %s, %s, %s, offer %s)', (
    phase, effectDisposition, effectBoundary, failureAcknowledgement,
  ) => {
    for (const error of [null, 'refused']) {
      for (const [busy, outcomeUnknown] of [[false, false], [true, false], [false, true]] as const) {
        const run = vi.fn();
        const dismiss = vi.fn();
        const notice = shownNotice('activation.m:1', 'apply.m', {
          state: 'failed',
          failureAcknowledgement,
          failure: {
            code: 'apply_admission_failed',
            message: 'Refused.',
            phase,
            effectDisposition,
            causeCode: 'stale_generation',
            causeDetails: {},
            producer: 'kernel',
            effectBoundary,
          },
        }, null, null, { busy, error, outcomeUnknown, run, dismiss });
        expect(notice.severity).toBe('error');
        expect(notice.acknowledgeable).toBeUndefined();
        expect(notice.actions?.map((action) => action.id)).toEqual(['acknowledge-apply-failure']);
        // Only a refusal before the fence (nothing held, never offered) hides it
        // here; everything else goes through the hook (send or re-read).
        const durable = failureAcknowledgement === 'available' && !outcomeUnknown;
        const preFence = phase === 'accepted' && !durable && !outcomeUnknown;
        notice.actions?.[0]?.run();
        expect(dismiss).toHaveBeenCalledTimes(preFence ? 1 : 0);
        expect(run).toHaveBeenCalledTimes(preFence ? 0 : 1);
      }
    }
  });

  it('removes an authoritatively acknowledged failure from active attention', () => {
    const notice = activationFailureNotice('activation.acknowledged:1', 'apply.acknowledged', {
      state: 'failed',
      failureAcknowledgement: 'acknowledged',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
        causeCode: 'typed_cause_code',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'before_effect',
      },
    }, null, false);

    expect(notice).toBeNull();
  });

  it('shows a Scenario composition refusal with its cause and the restart before the next Load', () => {
    const notice = shownNotice('activation.r:1', 'apply.r', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'runtime_composition_preparation_failed',
        message: 'Scenario refused: RoboPlan addressed planner construction is inconsistent',
        phase: 'runtime_converged',
        effectDisposition: 'known',
        causeCode: 'scenario_composition_refused',
        causeDetails: {
          cause: 'RoboPlan addressed planner construction is inconsistent',
          component_id: 'arm.1',
        },
        producer: 'execution_host',
        effectBoundary: 'unknown',
      },
    }, '/workspace/scenarios/cell.scenario', false);

    expect(notice).toMatchObject({ severity: 'error', domain: 'activation', title: 'Scenario refused' });
    expect(notice.message).toBe(
      'Scenario refused: RoboPlan addressed planner construction is inconsistent (component arm.1). '
        + 'The Scenario was not applied and nothing changed: composing its configuration found an '
        + 'inconsistency in its data or setup. Fix the cause named in the details, then restart the '
        + 'workbench before loading again. Stop the workbench and keep evidence: '
        + './scripts/compose-dev.sh stop --keep-evidence, then start the workbench again. (apply.r)',
    );
    expect(notice.message).not.toContain('runtime remains locked');
    expect(notice.message).not.toContain('Load or Apply again');
    expect(notice.actions).toBeUndefined();
  });

  it('keeps cold recovery for a known failure after the pre-effect phase', () => {
    const notice = shownNotice('activation.c:1', 'apply.c', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'runtime_convergence_failed',
        message: 'Runtime convergence failed.',
        phase: 'runtime_converged',
        effectDisposition: 'known',
        causeCode: 'typed_cause_code',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'unknown',
      },
    }, '/tmp/external.scenario', false);

    expect(notice.message).toContain('The runtime remains locked');
    expect(notice.message).toContain(
      'The Saved Scenario is outside the mounted repository, so a new workbench cannot reload it; only stopping the workbench is available.',
    );
    expect(notice.message).toContain('./scripts/compose-dev.sh stop --keep-evidence');
    expect(notice.message).not.toContain('start --project');
    expect(notice.message).toMatch(
      /Stop the workbench and keep evidence: \.\/scripts\/compose-dev\.sh stop --keep-evidence$/,
    );
  });

  it.each([
    '/workspace/scenarios/cell.scenario.yaml',
    '/workspace/scenarios/cell.scenario.yml',
  ])('offers only stopping the workbench for single-file Saved Scenario %s', (scenarioPath) => {
    const notice = shownNotice('activation.d:1', 'apply.d', {
      state: 'unknown_effect',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'scene_publication_unknown',
        message: 'Scene publication could not be confirmed.',
        phase: 'scene_activated',
        effectDisposition: 'unknown',
        causeCode: 'typed_cause_code',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'unknown',
      },
    }, scenarioPath, false);

    expect(notice.message).toContain(
      'The Saved Scenario is a single-file artifact, while this workbench start command accepts Scenario project directories; only stopping the workbench is available.',
    );
    expect(notice.message).toContain('./scripts/compose-dev.sh stop --keep-evidence');
    expect(notice.message).not.toContain('start --project');
  });

  it('fails closed when a terminal failure has no detail', () => {
    const notice = shownNotice('activation.e:1', 'apply.e', {
      state: 'failed', failure: null, failureAcknowledgement: 'unavailable',
    }, null, false);

    expect(notice.message).toContain(
      'Manager reported a terminal failure without detail. Applied state was not confirmed (apply.e).',
    );
    expect(notice.message).toContain('The runtime remains locked');
    expect(notice.message).toContain(
      'No Saved Scenario path is available, so this workbench can only be closed.',
    );
  });

  it('does not claim a known pre-effect refusal reopened admission when it remains closed', () => {
    const notice = shownNotice('activation.f:1', 'apply.f', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
        causeCode: 'typed_cause_code',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'before_effect',
      },
    }, '/workspace/scenarios/cell.scenario', false);

    expect(notice.message).toContain('current owner still reports authoring admission closed');
    expect(notice.message).toContain('Do not retry while it remains closed');
    expect(notice.message).toContain('Apply record alone does not justify a cold restart');
    expect(notice.message).not.toContain('stop --keep-evidence');
  });

  it('does not infer settlement when authoring admission is unavailable', () => {
    const notice = shownNotice('activation.g:1', 'apply.g', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
        causeCode: 'typed_cause_code',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'before_effect',
      },
    }, '/workspace/scenarios/cell.scenario', null);

    expect(notice.message).toContain('Current authoring admission is unavailable');
    expect(notice.message).toContain('settlement cannot be confirmed');
    expect(notice.message).not.toContain('stop --keep-evidence');
  });

  it('uses before-effect guidance and the acknowledgement action after fence acceptance', () => {
    const run = vi.fn();
    const notice = shownNotice('activation.h:1', 'apply.h', {
      state: 'failed',
      failureAcknowledgement: 'available',
      failure: {
        code: 'apply_admission_failed',
        message: 'The Apply was refused before effects began.',
        phase: 'fence_accepted',
        effectDisposition: 'known',
        causeCode: 'stale_generation',
        causeDetails: {},
        producer: 'kernel',
        effectBoundary: 'before_effect',
      },
    }, '/workspace/scenarios/cell.scenario', false, {
      busy: false, error: null, outcomeUnknown: false, run, dismiss: vi.fn(),
    });

    expect(notice.message).toContain('Failed during fence accepted');
    expect(notice.message).toContain(
      'no configuration, runtime, component, or Scene change.',
    );
    expect(notice.message).toContain('authoring admission closed');
    expect(notice.message).toContain('An admission claim and Kernel fence may remain');
    expect(notice.message).toContain('readiness may remain closed');
    expect(notice.message).not.toContain('The runtime remains locked');
    expect(notice.message).not.toContain('stop --keep-evidence');
    expect(notice.actions).toEqual([expect.objectContaining({ label: 'Acknowledge' })]);
    notice.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);
  });

  it('reports actual open admission when acknowledgement is unavailable without claiming success', () => {
    const run = vi.fn();
    const notice = shownNotice('activation.i:1', 'apply.i', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'apply_admission_failed',
        message: 'The Apply was refused before effects began.',
        phase: 'fence_accepted',
        effectDisposition: 'known',
        causeCode: 'stale_generation',
        causeDetails: {},
        producer: 'kernel',
        effectBoundary: 'before_effect',
      },
    }, null, true, { busy: false, error: null, outcomeUnknown: false, run, dismiss: vi.fn() });

    expect(notice.message).toContain('authoring admission is open');
    expect(notice.message).toContain('The failed Apply remains recorded');
    expect(notice.message).toContain('does not confirm acknowledgement');
    expect(notice.message).not.toContain('confirmed acknowledgement');
    // Not offered: the one Acknowledge re-reads; only positive evidence hides it.
    expect(notice.actions).toHaveLength(1);
    notice.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);
    expect(notice.message).not.toContain('The runtime remains locked');
  });

  it('keeps the locked recovery warning when the typed effect boundary is unknown', () => {
    const notice = shownNotice('activation.j:1', 'apply.j', {
      state: 'failed',
      failureAcknowledgement: 'unavailable',
      failure: {
        code: 'runtime_convergence_failed',
        message: 'Runtime convergence failed.',
        phase: 'fence_accepted',
        effectDisposition: 'known',
        causeCode: 'runtime_convergence_failed',
        causeDetails: {},
        producer: 'component_manager',
        effectBoundary: 'unknown',
      },
    }, '/workspace/scenarios/cell.scenario', true);

    expect(notice.message).toContain('The runtime remains locked');
    expect(notice.message).toContain('./scripts/compose-dev.sh stop --keep-evidence');
    expect(notice.message).not.toContain('no configuration, runtime, component, or Scene change');
    expect(notice.message).not.toContain('authoring admission is open');
  });
});
