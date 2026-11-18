import { describe, expect, it, vi } from 'vitest';

import { ERROR_CATALOG } from '../api/errorCatalog.generated';
import type { ManagerApplyFailure } from '../types/api';
import { activationFailureNotice, failedApplyHold } from './activationNotices';

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
      '3f9a/apply1',
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
      false,
    );
    expect(notice).toMatchObject({
      severity: 'error', domain: 'activation', title: 'Activation outcome unknown',
    });
    expect(notice.message).toContain('Failed during runtime converged');
    expect(notice.message).toContain('(runtime_composition_publication_verification_failed)');
    expect(notice.message).toContain('effects are unknown');
    expect(notice.message).toContain('Applied state was not confirmed (3f9a/apply1)');
    expect(notice.message).toContain('Use New to clear the failed Apply and empty the cell, then Load again.');
    expect(notice.message).toContain('typed_cause_code');
    expect(notice.message).toContain('component_manager');
    expect(notice.actions).toBeUndefined();
  });

  it.each(['apply_progression_failed'] as const)(
    'shows the catalogue text of a Manager-ended Apply (%s)',
    (code) => {
      const notice = shownNotice(
        'activation.a:1',
        '3f9a/apply1',
        {
          state: 'unknown_effect',
          failureAcknowledgement: 'unavailable',
          failure: {
            code,
            message: 'the Apply ended.',
            phase: 'runtime_converged',
            effectDisposition: 'unknown',
            causeCode: code,
            causeDetails: {},
            producer: 'component_manager',
            effectBoundary: 'unknown',
          },
        },
        false,
      );
      expect(notice.message).toContain(`(${code})`);
      expect(notice.message).toContain(ERROR_CATALOG[code][0]);
      expect(notice.message).toContain(ERROR_CATALOG[code][1]);
    },
  );

  it('does not prescribe recovery for a known pre-effect refusal, and dismisses it locally', () => {
    const run = vi.fn();
    const dismiss = vi.fn();
    const notice = shownNotice('activation.b:1', '3f9a/apply2', {
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
    }, true, { busy: false, error: null, outcomeUnknown: false, run, dismiss });
    // A failed Apply is an error (item 21): red, never a warning.
    expect(notice).toMatchObject({ title: 'Activation failed', severity: 'error' });
    expect(notice.acknowledgeable).toBeUndefined();
    expect(notice.message).toContain(
      'Candidate preflight was refused. Failed during accepted (manager_apply_preflight_refused); its effects are known.',
    );
    expect(notice.message).toContain(
      'Manager recorded a known pre-effect refusal.',
    );
    expect(notice.message).toContain('Authoring is available again; nothing is held.');
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
    const available = shownNotice('activation.ack:1', '3f9a/apply3', {
      state: 'failed', failure, failureAcknowledgement: 'available',
    }, true, { busy: true, error: null, outcomeUnknown: false, run, dismiss: vi.fn() });

    expect(available.title).toBe('Activation failed');
    expect(available.severity).toBe('error');
    expect(available.actions).toEqual([expect.objectContaining({
      label: 'Acknowledging…', disabled: true,
      title: expect.stringContaining('Release this failed Apply'),
    })]);
    expect(available.message).toContain('Candidate preflight was refused.');
    available.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);

    const unknown = shownNotice('activation.ack:2', '3f9a/apply4', {
      state: 'unknown_effect',
      failureAcknowledgement: 'unavailable',
      failure: {
        ...failure,
        effectDisposition: 'unknown' as const,
        effectBoundary: 'unknown' as const,
      },
    }, true, { busy: false, error: null, outcomeUnknown: false, run, dismiss: vi.fn() });
    // After effect the Manager never offers acknowledgement: no button.
    expect(unknown.actions).toBeUndefined();
  });

  it('keeps the notice and sends the durable Acknowledge again after a proven no-effect refusal while still offered', () => {
    const run = vi.fn();
    const notice = shownNotice('activation.ack:3', '3f9a/apply3', {
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
    }, false, {
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

  it('after an unknown outcome says so, names New, and only re-reads', () => {
    const notice = shownNotice('activation.ack:4', '3f9a/apply3', {
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
    }, false, { busy: false, error: 'Acknowledgement was not confirmed.', outcomeUnknown: true, run: vi.fn(), dismiss: vi.fn() });

    expect(notice.severity).toBe('error');
    expect(notice.message).toContain(
      ' The acknowledgement outcome is unknown; Acknowledge re-reads the status.',
    );
    expect(notice.message).toContain(
      'The failed Apply still holds the cell, so authoring is unavailable. Acknowledge it; '
        + 'if authoring stays unavailable, use New: it clears the failed Apply and empties the cell.',
    );
    expect(notice.message).not.toContain('restart');
    expect(notice.message).not.toContain('compose-dev.sh');
    expect(notice.actions).toEqual([expect.objectContaining({
      label: 'Acknowledge', title: expect.stringContaining('Re-read this Apply'),
    })]);
  });

  it('does not treat unavailable as released after the fence: it stays and says so', () => {
    const notice = shownNotice('activation.ack:5', '3f9a/apply3', {
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
    }, false, { busy: false, error: null, outcomeUnknown: false, run: vi.fn(), dismiss: vi.fn() });

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
        const notice = shownNotice('activation.m:1', '3f9a/apply5', {
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
        }, null, { busy, error, outcomeUnknown, run, dismiss });
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
    const notice = activationFailureNotice('activation.acknowledged:1', '3f9a/apply6', {
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
    }, false);

    expect(notice).toBeNull();
  });

  it('shows a Scenario composition refusal with its cause and New before the next Load', () => {
    const notice = shownNotice('activation.r:1', '3f9a/apply10', {
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
    }, false);

    expect(notice).toMatchObject({ severity: 'error', domain: 'activation', title: 'Scenario refused' });
    expect(notice.message).toBe(
      'Scenario refused: RoboPlan addressed planner construction is inconsistent (component arm.1). '
        + 'The Scenario was not applied and nothing changed: composing its configuration found an '
        + 'inconsistency in its data or setup. Fix the cause named in the details before loading '
        + 'again. Use New to clear the failed Apply, then correct the Scenario and Load again. (3f9a/apply10)',
    );
    expect(notice.message).not.toContain('Load or Apply again');
    expect(notice.actions).toBeUndefined();
  });

  it('names New for a known failure after the pre-effect phase', () => {
    const notice = shownNotice('activation.c:1', '3f9a/apply11', {
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
    }, false);

    expect(notice).toMatchObject({ severity: 'error', title: 'Activation failed' });
    expect(notice.message).toMatch(
      /^Runtime convergence failed\. Failed during runtime converged \(runtime_convergence_failed\); its effects are known\. Cause: /,
    );
    expect(notice.message).toMatch(
      / Applied state was not confirmed \(3f9a\/apply11\)\. What the Apply changed in the cell is not confirmed\. Use New to clear the failed Apply and empty the cell, then Load again\.$/,
    );
  });

  it('fails closed when a terminal failure has no detail', () => {
    const notice = shownNotice('activation.e:1', '3f9a/apply13', {
      state: 'failed', failure: null, failureAcknowledgement: 'unavailable',
    }, false);

    expect(notice.message).toBe(
      'Manager reported a terminal failure without detail. Applied state was not confirmed (3f9a/apply13). '
        + 'What the Apply changed in the cell is not confirmed. '
        + 'Use New to clear the failed Apply and empty the cell, then Load again.',
    );
  });

  it('does not claim a known pre-effect refusal reopened admission when it remains closed', () => {
    const notice = shownNotice('activation.f:1', '3f9a/apply14', {
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
    }, false);

    expect(notice.message).toContain(
      'The failed Apply still holds the cell, so authoring is unavailable. Acknowledge it; '
        + 'if authoring stays unavailable, use New: it clears the failed Apply and empties the cell. '
        + 'Correct the reported cause before trying again.',
    );
    expect(notice.message).not.toContain('stop --keep-evidence');
  });

  it('does not claim the hold is released when authoring admission is unavailable', () => {
    const notice = shownNotice('activation.g:1', '3f9a/apply15', {
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
    }, null);

    expect(notice.message).toContain(
      'Whether the failed Apply still holds the cell is not known yet. Acknowledge re-reads it; '
        + 'if authoring stays unavailable, use New: it clears the failed Apply and empties the cell.',
    );
    expect(notice.message).not.toContain('stop --keep-evidence');
  });

  it('uses before-effect guidance and the acknowledgement action after fence acceptance', () => {
    const run = vi.fn();
    const notice = shownNotice('activation.h:1', '3f9a/apply16', {
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
    }, false, {
      busy: false, error: null, outcomeUnknown: false, run, dismiss: vi.fn(),
    });

    expect(notice.message).toContain('Failed during fence accepted');
    expect(notice.message).toContain(
      'no configuration, runtime, component, or Scene change.',
    );
    expect(notice.message).toContain('The failed Apply still holds the cell, so authoring is unavailable.');
    expect(notice.message).not.toContain('The runtime remains locked');
    expect(notice.message).not.toContain('stop --keep-evidence');
    expect(notice.actions).toEqual([expect.objectContaining({ label: 'Acknowledge' })]);
    notice.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);
  });

  it('reports actual open admission when acknowledgement is unavailable without claiming success', () => {
    const run = vi.fn();
    const notice = shownNotice('activation.i:1', '3f9a/apply17', {
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
    }, true, { busy: false, error: null, outcomeUnknown: false, run, dismiss: vi.fn() });

    expect(notice.message).toContain('Authoring is available again; nothing is held.');
    // Not offered: the one Acknowledge re-reads; only positive evidence hides it.
    expect(notice.actions).toHaveLength(1);
    notice.actions?.[0]?.run();
    expect(run).toHaveBeenCalledTimes(1);
    expect(notice.message).not.toContain('The runtime remains locked');
  });

  it('says nothing is held when the typed effect boundary is unknown and admission is open', () => {
    const notice = shownNotice('activation.j:1', '3f9a/apply18', {
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
    }, true, undefined, failedApplyHold(true));

    expect(notice.title).toBe('Activation failed');
    expect(notice.message).toMatch(/ Applied state was not confirmed \(3f9a\/apply18\)\. Nothing is held; Load again\.$/);
    expect(notice.message).not.toContain('no configuration, runtime, component, or Scene change');
    expect(notice.message).not.toContain('Authoring is available again');
  });
});

// The way out follows the facts that enable New, by the classes of an ended Apply.
describe('the way out of an ended Apply', () => {
  const RESTART = 'compose-dev.sh';
  const NEW_CLEARS = 'What the Apply changed in the cell is not confirmed. '
    + 'Use New to clear the failed Apply and empty the cell, then Load again.';
  const failure = (
    phase: ManagerApplyFailure['phase'],
    effectDisposition: 'known' | 'unknown',
    effectBoundary: 'before_effect' | 'unknown',
    code = 'typed_failure',
    causeCode = 'typed_cause_code',
  ) => ({
    code, message: 'The Apply ended.', phase, effectDisposition, causeCode,
    causeDetails: {}, producer: 'component_manager', effectBoundary,
  }) satisfies ManagerApplyFailure;
  const ack = (outcomeUnknown = false) => ({
    busy: false, error: null, outcomeUnknown, run: vi.fn(), dismiss: vi.fn(),
  });

  it('maps the authoring admission to the way out', () => {
    expect(failedApplyHold(true)).toBe('nothing_held');
    expect(failedApplyHold(false)).toBe('new_clears');
    expect(failedApplyHold(null)).toBe('new_clears');
  });

  it('class 1: refused before any effect, nothing held: no change, no restart', () => {
    const notice = shownNotice('activation.c1:1', '3f9a/apply1', {
      state: 'failed', failureAcknowledgement: 'unavailable',
      failure: failure('accepted', 'known', 'before_effect'),
    }, true, ack(), failedApplyHold(true));
    expect(notice.message).toContain('Manager recorded a known pre-effect refusal.');
    expect(notice.message).toContain('Authoring is available again; nothing is held.');
    expect(notice.message).not.toContain(RESTART);
    expect(notice.message).not.toContain('use New');
  });

  it('class 2: refused before any effect with the admission still closed', () => {
    const notice = shownNotice('activation.c2:1', '3f9a/apply2', {
      state: 'failed', failureAcknowledgement: 'unavailable',
      failure: failure('accepted', 'known', 'before_effect'),
    }, false, ack(), failedApplyHold(false));
    expect(notice.message).toContain(
      'The failed Apply still holds the cell, so authoring is unavailable. Acknowledge it; '
        + 'if authoring stays unavailable, use New: it clears the failed Apply and empties the cell.',
    );
    expect(notice.message).not.toContain(RESTART);
  });

  it('class 3: a refused Scenario names New while the Apply holds the cell, nothing once released', () => {
    const refused = {
      state: 'failed' as const, failureAcknowledgement: 'available' as const,
      failure: {
        ...failure('runtime_converged', 'known', 'unknown', 'runtime_composition_preparation_failed',
          'scenario_composition_refused'),
        causeDetails: { component_id: 'arm.1' },
      },
    };
    const guidance = ERROR_CATALOG.scenario_composition_refused[1];
    expect(shownNotice('activation.c3:1', '3f9a/apply3', refused, false, ack(),
      failedApplyHold(false)).message).toBe(
      `The Apply ended. (component arm.1). ${guidance} `
        + 'Use New to clear the failed Apply, then correct the Scenario and Load again. (3f9a/apply3)',
    );
    expect(shownNotice('activation.c3:1', '3f9a/apply3', refused, true, ack(),
      failedApplyHold(true)).message).toBe(
      `The Apply ended. (component arm.1). ${guidance} `
        + 'Nothing was changed; correct the Scenario and Load again. (3f9a/apply3)',
    );
    // Before effect after the fence: Acknowledge, no restart.
    const beforeEffect = shownNotice('activation.c3:2', '3f9a/apply3', {
      state: 'failed', failureAcknowledgement: 'available',
      failure: failure('fence_accepted', 'known', 'before_effect'),
    }, false, ack(), failedApplyHold(false));
    expect(beforeEffect.message).toContain('made no configuration, runtime, component, or Scene change');
    expect(beforeEffect.message).not.toContain(RESTART);
    expect(beforeEffect.actions?.[0]?.title).toContain('Release this failed Apply');
  });

  it.each(['runtime_convergence_failed', 'apply_progression_failed'])(
    'class 4: unknown effect after the fence (%s): New clears it, no lock or restart',
    (code) => {
      const notice = shownNotice('activation.c4:1', '3f9a/apply4', {
        state: 'unknown_effect', failureAcknowledgement: 'unavailable',
        failure: failure('runtime_converged', 'unknown', 'unknown', code, code),
      }, false, ack(), failedApplyHold(false));
      expect(notice.title).toBe('Activation outcome unknown');
      expect(notice.message).toContain('its effects are unknown. Cause:');
      expect(notice.message).toContain(`Applied state was not confirmed (3f9a/apply4). ${NEW_CLEARS}`);
      expect(notice.message).not.toContain(RESTART);
    },
  );

  it.each<[string, ManagerApplyFailure['phase']]>([
    ['class 5: fence attempt never accepted', 'fence_acquired'],
    ['class 7: a failure after publication', 'published'],
  ])('%s: New clears it', (_name, phase) => {
    const notice = shownNotice('activation.c5:1', '3f9a/apply5', {
      state: 'unknown_effect', failureAcknowledgement: 'unavailable',
      failure: failure(phase, 'unknown', 'unknown'),
    }, false, ack(), failedApplyHold(false));
    expect(notice.message).toMatch(/its effects are unknown\. Cause: /);
    expect(notice.message).toContain(`Applied state was not confirmed (3f9a/apply5). ${NEW_CLEARS}`);
  });

  it('class 6: after a release whose outcome is unknown, New is still named', () => {
    const notice = shownNotice('activation.c6:1', '3f9a/apply6', {
      state: 'unknown_effect', failureAcknowledgement: 'unavailable',
      failure: failure('runtime_converged', 'unknown', 'unknown'),
    }, false, ack(true), failedApplyHold(false));
    expect(notice.message).toContain(`Applied state was not confirmed (3f9a/apply6). ${NEW_CLEARS}`);
  });

  it('nothing held: a failed Apply after the fence names Load only', () => {
    const notice = shownNotice('activation.n:1', '3f9a/apply7', {
      state: 'failed', failureAcknowledgement: 'unavailable',
      failure: failure('runtime_converged', 'unknown', 'unknown'),
    }, true, ack(true), failedApplyHold(true));
    expect(notice.title).toBe('Activation failed');
    expect(notice.message).toMatch(/ Applied state was not confirmed \(3f9a\/apply7\)\. Nothing is held; Load again\.$/);
  });

  it('no notice of an ended Apply names a restart', () => {
    const states = ['failed', 'unknown_effect'] as const;
    const offers = ['available', 'unavailable'] as const;
    const failures = [
      null,
      failure('accepted', 'known', 'before_effect'),
      failure('accepted', 'known', 'unknown'),
      failure('fence_accepted', 'known', 'before_effect'),
      failure('fence_acquired', 'unknown', 'unknown'),
      failure('runtime_converged', 'unknown', 'unknown'),
      failure('published', 'known', 'unknown'),
      failure('runtime_converged', 'known', 'unknown', 'runtime_composition_preparation_failed',
        'scenario_composition_refused'),
    ];
    for (const state of states) for (const failureAcknowledgement of offers)
      for (const shown of failures) for (const admission of [true, false, null])
        for (const outcomeUnknown of [true, false]) {
          const notice = shownNotice('activation.x:1', '3f9a/apply8', {
            state, failureAcknowledgement, failure: shown,
          }, admission, ack(outcomeUnknown), failedApplyHold(admission));
          expect(notice.message).not.toMatch(/restart/i);
          expect(notice.message).not.toContain('Stop ManyForge');
          expect(notice.message).not.toContain(RESTART);
        }
  });
});
