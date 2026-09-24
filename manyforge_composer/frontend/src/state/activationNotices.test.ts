import { describe, expect, it } from 'vitest';

import { activationFailureNotice } from './activationNotices';

describe('activation failure notice', () => {
  it('keeps the owner failure phase, code and effect disposition', () => {
    const notice = activationFailureNotice('activation.a:1', 'apply.a', {
      state: 'unknown_effect',
      failure: {
        code: 'runtime_composition_publication_verification_failed',
        message: 'native CCLC candidate routes are not ready.',
        phase: 'runtime_converged',
        effectDisposition: 'unknown',
      },
    });
    expect(notice).toMatchObject({
      severity: 'error', domain: 'activation', title: 'Activation outcome unknown',
    });
    expect(notice.message).toContain('Failed during runtime converged');
    expect(notice.message).toContain('(runtime_composition_publication_verification_failed)');
    expect(notice.message).toContain('effects are unknown');
    expect(notice.message).toContain('Applied state was not confirmed (apply.a)');
  });

  it('states a known failure without inventing detail', () => {
    expect(activationFailureNotice('activation.b:1', 'apply.b', {
      state: 'failed', failure: null,
    })).toMatchObject({
      title: 'Activation failed',
      message: 'Manager reported a terminal failure without detail. Applied state was not confirmed (apply.b).',
    });
  });
});
